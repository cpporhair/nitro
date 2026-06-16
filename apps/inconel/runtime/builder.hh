#ifndef APPS_INCONEL_RUNTIME_BUILDER_HH
#define APPS_INCONEL_RUNTIME_BUILDER_HH

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "env/runtime/share_nothing.hh"
#include "pump/core/lock_free_queue.hh"

#include "../core/page_cache.hh"
#include "../core/registry.hh"
#include "../core/data_area_heads.hh"
#include "../core/leaf_order.hh"
#include "../core/read_catalog.hh"
#include "../core/shard_partition_builder.hh"
#include "../core/tree_geometry.hh"
#include "../core/tree_read_domain.hh"
#include "../coord/scheduler.hh"
#include "../format/format_profile.hh"
#include "../format/types.hh"
#include "../front/scheduler.hh"
#include "../memory/spdk_dma_page_allocator.hh"
#include "../nvme/runtime_scheduler.hh"
#include "../tree/scheduler.hh"
#include "../value/scheduler.hh"
#include "../wal/scheduler.hh"
#include "./facade.hh"

namespace apps::inconel::runtime {

    // ── Inconel runtime type ──
    //
    // PUMP global_runtime_t parameterized on tree + value cache policies
    // independently. tree and value have different working sets and access
    // patterns, so a real deployment may want clock for one and slru for the
    // other (or different capacities). This template is the only place the
    // cache parameters appear at the runtime level — application code
    // interacts with schedulers via the non-templated core::runtime registry.
    //
    // Step 030 (§6.5 decision G1) collapses the former pair of tuple
    // entries `tree_lookup_sched<TreeCache>` + `tree_worker_sched<TreeCache>`
    // into a single `core::tree_read_domain<TreeCache>` entry. The
    // read_domain owns both schedulers via unique_ptr and exposes
    // `advance()` that drives both arms in one round — the PUMP runtime
    // now schedules one unit per core instead of two (030 §2.3 / §2.8
    // step 6). The ownership chain flattens to:
    //
    //   PUMP tuple
    //     └── tree_read_domain<TreeCache>
    //           ├── tree_lookup_sched<TreeCache>
    //           └── tree_worker_sched<TreeCache>
    //
    // which lines up with the "one cache shard per read_domain"
    // invariant (RSM §4.7) and matches the singleton-at-tail pattern
    // already in place for `tree::tree_sched`.

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    using inconel_runtime_t = pump::env::runtime::global_runtime_t<
        nvme::runtime_scheduler,
        core::tree_read_domain<TreeCache>,
        value::value_alloc_sched<ValueCache>,
        tree::tree_sched,
        coord::coord_sched,
        front::front_sched,
        wal::wal_space_sched
    >;

    // ── Bootstrap tree geometry ──
    //
    // Step 022 D3 / §1 约束 3: the tree geometry runtime carrier is
    // constructed by the builder, and the bootstrap values are copied
    // straight out of `format::kBootstrapFormatProfile` — the profile
    // remains the single source of truth, this constant is just a
    // runtime-shaped view of the same numbers. No independent
    // "constexpr tree_geometry k{...}" with hand-written values is
    // allowed here.
    //
    // Once INC-031 / INC-035 land, recovery will replace this
    // compile-time constant with a runtime `tree_geometry` populated
    // from the superblock it just read.

    inline constexpr core::tree_geometry kBootstrapTreeGeometry = {
        .lba_size               = format::kBootstrapFormatProfile.lba_size,
        .tree_page_size         = format::kBootstrapFormatProfile.tree_page_size,
        .shadow_slots_per_range = format::kBootstrapFormatProfile.shadow_slots_per_range,
    };

    [[nodiscard]] inline wal::segment_geometry
    wal_geometry_from_profile(const format::format_profile& profile) {
        return wal::segment_geometry{
            .wal_base_paddr    = profile.wal_base_paddr,
            .wal_segment_size  = profile.wal_segment_size,
            .lba_size          = profile.lba_size,
            .wal_segment_count = profile.wal_segment_count,
        };
    }

    struct front_topology_options {
        std::span<const uint32_t> cores;
        std::span<const uint32_t> front_cores;
        int32_t coord_core = -1;
        int32_t wal_space_core = -1;
        wal::segment_geometry wal_geometry;
        core::wal_reclaim_frontier* wal_reclaim_frontier = nullptr;
        const core::tree_geometry* tree_geometry = nullptr;
        std::size_t front_queue_depth = 1024;
        std::size_t coord_queue_depth = 1024;
        std::size_t coord_ready_window = 65536;
        wal::wal_append_config front_wal_config =
            { .pending_prepare_capacity = 64 };
    };

    struct front_topology {
        std::vector<uint32_t> front_cores;
        std::vector<front::front_sched*> fronts;
        coord::coord_sched* coord = nullptr;
        wal::wal_space_sched* wal_space = nullptr;
        uint32_t coord_core = 0;
        uint32_t wal_space_core = 0;
    };

    [[nodiscard]] inline bool
    span_contains(std::span<const uint32_t> xs, uint32_t value) noexcept {
        return std::find(xs.begin(), xs.end(), value) != xs.end();
    }

    inline void
    validate_runtime_queue_depth(const char* name, std::size_t depth) {
        if (depth < 2 || !std::has_single_bit(depth)) {
            throw std::invalid_argument(name);
        }
    }

    [[nodiscard]] inline memory::dma_page_allocator
    make_runtime_dma_page_allocator() noexcept {
#ifdef INCONEL_NVME_MOCK_BACKEND
        return memory::make_heap_dma_page_allocator();
#else
        return memory::make_spdk_dma_page_allocator();
#endif
    }

    [[nodiscard]] inline uint32_t
    resolve_optional_core(int32_t configured,
                          uint32_t fallback,
                          const char* what) {
        if (configured == -1) {
            return fallback;
        }
        if (configured < -1) {
            throw std::invalid_argument(what);
        }
        return static_cast<uint32_t>(configured);
    }

    [[nodiscard]] inline std::vector<uint32_t>
    resolve_front_cores(const front_topology_options& opts) {
        if (opts.cores.empty()) {
            throw std::invalid_argument(
                "runtime::build_front_topology: cores is empty");
        }

        std::vector<uint32_t> out;
        if (opts.front_cores.empty()) {
            out.assign(opts.cores.begin(), opts.cores.end());
        } else {
            out.assign(opts.front_cores.begin(), opts.front_cores.end());
        }
        if (out.empty()) {
            throw std::invalid_argument(
                "runtime::build_front_topology: front_cores is empty");
        }
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (!span_contains(opts.cores, out[i])) {
                throw std::invalid_argument(
                    "runtime::build_front_topology: front_cores contains "
                    "a core not in cores");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (out[j] == out[i]) {
                    throw std::invalid_argument(
                        "runtime::build_front_topology: front_cores contains "
                        "a duplicate");
                }
            }
        }
        return out;
    }

    inline void
    validate_tree_geometry_for_front_topology(const core::tree_geometry* geom) {
        if (geom == nullptr) {
            throw std::invalid_argument(
                "runtime::build_front_topology: tree_geometry is null");
        }
        if (geom->lba_size == 0) {
            throw std::invalid_argument(
                "runtime::build_front_topology: tree_geometry.lba_size is 0");
        }
        if (geom->tree_page_size == 0 ||
            geom->tree_page_size % geom->lba_size != 0) {
            throw std::invalid_argument(
                "runtime::build_front_topology: invalid tree_page_size");
        }
        if (geom->shadow_slots_per_range == 0) {
            throw std::invalid_argument(
                "runtime::build_front_topology: shadow_slots_per_range is 0");
        }
    }

    [[nodiscard]] inline front_topology
    build_front_topology(const front_topology_options& opts) {
        auto front_cores = resolve_front_cores(opts);
        const uint32_t coord_core =
            resolve_optional_core(
                opts.coord_core,
                opts.cores.front(),
                "runtime::build_front_topology: coord_core is below -1");
        const uint32_t wal_space_core =
            resolve_optional_core(
                opts.wal_space_core,
                coord_core,
                "runtime::build_front_topology: wal_space_core is below -1");

        if (!span_contains(opts.cores, coord_core)) {
            throw std::invalid_argument(
                "runtime::build_front_topology: coord_core is not in cores");
        }
        if (!span_contains(opts.cores, wal_space_core)) {
            throw std::invalid_argument(
                "runtime::build_front_topology: wal_space_core is not in cores");
        }
        validate_runtime_queue_depth(
            "runtime::build_front_topology: front_queue_depth must be a "
            "power of two and >= 2",
            opts.front_queue_depth);
        validate_runtime_queue_depth(
            "runtime::build_front_topology: coord_queue_depth must be a "
            "power of two and >= 2",
            opts.coord_queue_depth);
        wal::validate_segment_geometry(opts.wal_geometry);
        if (opts.wal_reclaim_frontier == nullptr) {
            throw std::invalid_argument(
                "runtime::build_front_topology: wal_reclaim_frontier is null");
        }
        validate_tree_geometry_for_front_topology(opts.tree_geometry);
        wal::validate_wal_append_config(opts.front_wal_config);

        if (!core::registry::front_scheds.list.empty() ||
            core::registry::coord_sched_singleton_ptr != nullptr ||
            core::registry::wal_space_sched_singleton_ptr != nullptr) {
            throw std::logic_error(
                "runtime::build_front_topology: registry already has a "
                "front/coord/wal topology");
        }

        const uint32_t max_core =
            *std::max_element(opts.cores.begin(), opts.cores.end());
        if (core::registry::front_scheds.by_core.size() <= max_core) {
            core::registry::front_scheds.by_core.resize(
                static_cast<std::size_t>(max_core) + 1, nullptr);
        }

        const uint32_t front_count =
            static_cast<uint32_t>(front_cores.size());
        std::vector<std::shared_ptr<core::memtable_gen>> active_gens;
        active_gens.reserve(front_count);
        for (uint32_t owner = 0; owner < front_count; ++owner) {
            active_gens.push_back(front::make_front_memtable_gen(
                owner, front_count, 0, core::memtable_gen::state::active));
        }

        auto manifest = std::make_shared<const core::tree_manifest>(
            core::tree_manifest::empty(opts.tree_geometry));
        auto guard = std::make_shared<core::checkpoint_guard>();
        guard->manifest = std::move(manifest);

        std::vector<core::front_read_set> front_sets;
        front_sets.reserve(front_count);
        for (const auto& gen : active_gens) {
            front_sets.push_back(core::front_read_set{
                .active = gen,
                .imms = {},
            });
        }
        auto prs_fronts =
            std::make_shared<const std::vector<core::front_read_set>>(
                std::move(front_sets));
        auto prs = std::make_shared<const core::published_read_set>(
            core::published_read_set{
                .tree_guard = std::move(guard),
                .fronts = std::move(prs_fronts),
                .epoch = 1,
            });
        auto cat = std::make_shared<const core::publish_catalog>(
            prs, 0, 1);

        front_topology topology;
        topology.front_cores = std::move(front_cores);
        topology.fronts.resize(front_count, nullptr);
        topology.coord_core = coord_core;
        topology.wal_space_core = wal_space_core;

        pump::core::this_core_id = wal_space_core;
        topology.wal_space = new wal::wal_space_sched(
            opts.wal_geometry,
            opts.wal_reclaim_frontier,
            front_count);
        core::registry::wal_space_sched_singleton_ptr = topology.wal_space;

        core::registry::front_scheds.list.assign(front_count, nullptr);
        for (uint32_t owner = 0; owner < front_count; ++owner) {
            const uint32_t home_core = topology.front_cores[owner];
            pump::core::this_core_id = home_core;
            auto* fs = new front::front_sched(
                owner,
                front_count,
                active_gens[owner],
                1,
                opts.wal_geometry,
                opts.front_wal_config,
                opts.front_queue_depth);
            topology.fronts[owner] = fs;
            core::registry::front_scheds.list[owner] = fs;
            core::registry::front_scheds.by_core[home_core] = fs;
        }

        pump::core::this_core_id = coord_core;
        topology.coord = new coord::coord_sched(
            std::move(cat),
            front_count,
            1,
            opts.coord_ready_window,
            opts.coord_queue_depth);
        core::registry::coord_sched_singleton_ptr = topology.coord;

        return topology;
    }

    // ── Build context ──
    //
    // `cores` lists every core that participates in the runtime. Each such
    // core unconditionally hosts an `nvme::runtime_scheduler` so the per-core
    // advance loop always has an I/O seam available.
    //
    // The three per-role fields describe where the remaining schedulers
    // live. When all of them are left at their sentinel defaults the
    // builder falls back to the historical symmetric topology — every
    // listed core hosts a `tree_read_domain`, the value singleton and the
    // owner singleton both pin to `cores[0]`. Tests that want an
    // asymmetric layout (e.g. `core 0 = value`, `core 2/4/6 = read_domain`,
    // `core 8 = owner`) populate the role fields explicitly; the core ids
    // named there must also appear in `cores`. Every role-core that is
    // not listed as a read_domain host carries a nullptr slot in the PUMP
    // per-core tuple so `share_nothing::run()` skips it.
    //
    // Step 017 / INC-034 removed the disk-format fields that used to live
    // here (`value_class_sizes`, `lba_size`, `value_data_area_base`,
    // `value_data_area_end`). Those parameters are now sourced from
    // `format::kBootstrapFormatProfile` in a single place so the runtime
    // config surface only keeps genuine per-run knobs — right now that is
    // topology (cores, device) and in-memory cache capacities. Superblock /
    // recovery landing in a later step will replace the code-owned constant
    // with an on-disk profile load.

    struct build_options {
        std::span<const uint32_t> cores;             // which cores to populate
        nvme::runtime_device*     device;            // nvme backing device

        // Optional per-role topology. Leave default for the symmetric
        // layout (read_domain on every core, value + owner on cores[0]).
        // When set, each role-core must be a member of `cores`.
        //   read_domain_cores — empty means "all cores".
        //   value_core        — < 0 means cores[0].
        //   owner_core        — < 0 means cores[0].
        std::span<const uint32_t> read_domain_cores = {};
        int32_t                   value_core        = -1;
        int32_t                   owner_core        = -1;
        std::span<const uint32_t> front_cores       = {};
        int32_t                   coord_core        = -1;
        int32_t                   wal_space_core    = -1;
        std::size_t               front_queue_depth = 1024;
        std::size_t               coord_queue_depth = 1024;
        std::size_t               coord_ready_window = 65536;
        wal::wal_append_config    front_wal_config =
            { .pending_prepare_capacity = 64 };

        // tree cache capacity (entries). Applies to tree_lookup_sched<TreeCache>.
        // Minimum legal value depends on the cache impl: clock_cache requires
        // >= 1, slru_cache requires >= 2 (so each segment gets at least one
        // slot under the default 0.8 protected ratio). Smaller values throw
        // std::invalid_argument from the cache ctor — there is no silent
        // capping. The default 32 is well above both thresholds.
        uint32_t                  tree_cache_capacity = 32;

        // value cache capacity (entries). Applies to value::value_alloc_sched<ValueCache>.
        // Same minimum-capacity rules as tree_cache_capacity above.
        uint32_t                  value_cache_capacity = 32;

        // Per-role scheduler queue depths. Existing callers that do not set
        // them keep the previous constructor defaults.
        size_t                    tree_queue_depth = 2048;
        size_t                    value_queue_depth = 2048;
        value::value_io_policy    value_io_policy = {};

        // Real NVMe scheduler queue knobs. The device/qpair lifecycle is owned
        // by nvme::real_device; these tune the per-core PUMP scheduler and
        // the request-scope LBA DMA page pool.
        size_t                    nvme_queue_depth = 2048;
        size_t                    nvme_local_depth = 128;
        uint64_t                  nvme_dma_pool_pages_per_core = 4096;
        uint32_t                  nvme_dma_alignment = 4096;
        int                       nvme_numa_id = SPDK_ENV_NUMA_ID_ANY;
    };

    // ── validate_build_inputs ──
    //
    // Upfront fail-fast check for every invariant build_runtime relies on.
    // Raises std::invalid_argument / std::runtime_error so run_with()'s
    // top-level catch funnels any failure through panic_inconsistency with a
    // readable site + reason. External profile / device validation must
    // never be reduced to `assert` — assertions compile out in Release and
    // would turn a bad config into undefined runtime state.
    //
    // Checks are grouped into three tiers:
    //   1. build_options preconditions (non-null device, non-empty cores).
    //   2. profile self-consistency — ODF §2, §4.1 and §6 shape rules.
    //      kBootstrapFormatProfile is already `static_assert`-ed consistent
    //      at compile time, but the runtime leg is retained so any future
    //      dynamic profile (populated from a superblock after INC-031 /
    //      INC-035) goes through the same gate with detailed messages.
    //   3. profile ↔ device agreement (lba_size match, device large enough
    //      to fit the profile's value_data_area_end).

    inline void
    validate_build_inputs(const build_options& opts,
                          const format::format_profile& profile) {
        // ── tier 1: build_options ──
        if (opts.device == nullptr) {
            throw std::invalid_argument(
                "runtime::build_runtime: opts.device is null");
        }
        if (opts.cores.empty()) {
            throw std::invalid_argument(
                "runtime::build_runtime: opts.cores is empty");
        }

        // ── tier 2: profile self-consistency ──
        if (profile.lba_size == 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.lba_size is 0");
        }
        if (profile.value_data_area_base.device_id !=
            profile.value_data_area_end.device_id) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile value_data_area base/end "
                "device_id mismatch");
        }
        if (!(profile.value_data_area_base.lba <
              profile.value_data_area_end.lba)) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile value_data_area_base.lba "
                ">= value_data_area_end.lba");
        }

        const auto cls = profile.class_sizes();
        if (cls.empty()) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile value_class_sizes is empty");
        }
        if (cls.size() > format::kMaxValueClassCount) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile value_class_sizes exceeds "
                "the ODF §2 superblock cap of 16 entries");
        }
        uint32_t prev = 0;
        for (uint32_t cs : cls) {
            if (cs == 0) {
                throw std::invalid_argument(
                    "runtime::build_runtime: profile value_class_sizes "
                    "contains 0");
            }
            if (cs <= prev) {
                throw std::invalid_argument(
                    "runtime::build_runtime: profile value_class_sizes is "
                    "not strictly ascending");
            }
            prev = cs;

            if (cs < profile.lba_size) {
                if (profile.lba_size % cs != 0) {
                    throw std::invalid_argument(
                        "runtime::build_runtime: sub-LBA class_size does "
                        "not divide profile.lba_size");
                }
            } else if (cs > profile.lba_size) {
                if (cs % profile.lba_size != 0) {
                    throw std::invalid_argument(
                        "runtime::build_runtime: multi-LBA class_size is "
                        "not a multiple of profile.lba_size");
                }
            }
            // cs == profile.lba_size is always a valid LBA-equal class.
        }

        // Tree parameters (step 022 G1). `profile_is_self_consistent`
        // already encodes the same invariants at compile time for
        // `kBootstrapFormatProfile`, but the runtime leg is retained
        // with a readable error message so a future dynamic profile
        // (populated from a superblock after INC-031 / INC-035) goes
        // through the same gate.
        if (profile.tree_page_size == 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.tree_page_size is 0");
        }
        if (profile.tree_page_size % profile.lba_size != 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.tree_page_size is not "
                "an integral multiple of profile.lba_size");
        }
        if (profile.shadow_slots_per_range == 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.shadow_slots_per_range is 0");
        }

        // WAL area parameters (M11 / ODF §2.2, §3.6, §6, §7). These
        // are disk-format fields sourced from format_profile; runtime
        // options must not grow an independent WAL layout surface.
        if (profile.wal_segment_size == 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.wal_segment_size is 0");
        }
        if (profile.wal_segment_size % profile.lba_size != 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.wal_segment_size is not "
                "an integral multiple of profile.lba_size");
        }
        if (profile.wal_segment_count == 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.wal_segment_count is 0");
        }
        if (profile.wal_base_paddr.device_id !=
            profile.value_data_area_base.device_id) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile WAL base device_id does "
                "not match value_data_area_base device_id");
        }
        if (profile.wal_base_paddr.lba < 2) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.wal_base_paddr.lba is "
                "inside the superblock area");
        }
        {
            const uint64_t wal_segment_lbas =
                static_cast<uint64_t>(profile.wal_segment_size)
                / profile.lba_size;
            if (profile.wal_segment_count >
                (std::numeric_limits<uint64_t>::max()
                 - profile.wal_base_paddr.lba) / wal_segment_lbas) {
                throw std::invalid_argument(
                    "runtime::build_runtime: profile WAL area overflows LBA "
                    "address space");
            }
            const uint64_t wal_end_lba =
                profile.wal_base_paddr.lba
                + static_cast<uint64_t>(profile.wal_segment_count)
                    * wal_segment_lbas;
            if (wal_end_lba > profile.value_data_area_base.lba) {
                throw std::invalid_argument(
                    "runtime::build_runtime: profile WAL area overlaps the "
                    "data area");
            }
        }
        if (!format::profile_wal_segment_can_fit_v1_max_entry(
                profile.lba_size, profile.wal_segment_size)) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile WAL segment cannot fit the "
                "v1 maximum WAL entry");
        }

        // INC-051 / 037 plan §"Allocation Quantum" + §"Group 分片". Both
        // fields are disk-format truth (recovery rebuilds value_space_manager
        // partial metadata from them); reject any drift before the runtime
        // ever touches a value page.
        if (profile.value_space_quantum_bytes != 64) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.value_space_quantum_bytes "
                "must be 64 (only supported value)");
        }
        if (profile.lba_size % profile.value_space_quantum_bytes != 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.lba_size is not a multiple "
                "of profile.value_space_quantum_bytes");
        }
        if (profile.lba_size / profile.value_space_quantum_bytes > 64) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.lba_size / "
                "profile.value_space_quantum_bytes > 64 "
                "(value_space_manager free_quantum_bits is uint64_t)");
        }
        if (profile.value_space_group_size_lbas == 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.value_space_group_size_lbas is 0");
        }
        {
            const uint64_t group_bytes =
                static_cast<uint64_t>(profile.value_space_group_size_lbas)
                * profile.lba_size;
            if (group_bytes < (64ULL * 1024 * 1024)) {
                throw std::invalid_argument(
                    "runtime::build_runtime: profile group bytes "
                    "(value_space_group_size_lbas * lba_size) below 64 MiB");
            }
            if (group_bytes > (1024ULL * 1024 * 1024)) {
                throw std::invalid_argument(
                    "runtime::build_runtime: profile group bytes above 1 GiB");
            }
            if ((group_bytes & (group_bytes - 1)) != 0) {
                throw std::invalid_argument(
                    "runtime::build_runtime: profile group bytes not a power "
                    "of two");
            }
        }
        // M-2 (review §1): the cache key `memory::frame_id::span_lbas`
        // is a uint16_t, and `make_tree_frame_id` feeds it
        // `tree_page_size / lba_size` via a static_cast that would
        // silently truncate. Reject any profile whose page_lbas
        // overflows that width at build time so the truncation path
        // can never be reached at runtime.
        {
            const uint64_t page_lbas =
                static_cast<uint64_t>(profile.tree_page_size) / profile.lba_size;
            if (page_lbas > std::numeric_limits<uint16_t>::max()) {
                throw std::invalid_argument(
                    "runtime::build_runtime: profile.tree_page_size / "
                    "profile.lba_size exceeds uint16_t capacity of "
                    "memory::frame_id::span_lbas");
            }
        }

        // ── tier 3: profile ↔ device agreement ──
        if (opts.device->sector_size() == 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: real NVMe sector_size is 0");
        }
        if (profile.lba_size % opts.device->sector_size() != 0) {
            throw std::invalid_argument(
                "runtime::build_runtime: profile.lba_size is not an "
                "integral multiple of real NVMe sector_size");
        }
        if (opts.device->total_logical_lbas(profile.lba_size) <
            profile.value_data_area_end.lba) {
            throw std::runtime_error(
                "runtime::build_runtime: real NVMe namespace too small for "
                "profile value_data_area_end.lba");
        }
        for (uint32_t core : opts.cores) {
            (void)opts.device->qpair_for_core(core);
        }

        // ── tier 4: role-core membership ──
        //
        // Each role core must appear in `cores` and fit the registry's
        // `by_core` arrays (indexed by hardware core id). Duplicates in
        // `read_domain_cores` would produce two `tree_read_domain` slots
        // on the same core and break the "one read_domain per core"
        // invariant — reject them at build time rather than letting
        // `add_core_schedulers` trip its assert at run time.
        auto core_in_set = [&](uint32_t c) {
            for (uint32_t x : opts.cores) if (x == c) return true;
            return false;
        };
        if (opts.value_core >= 0 &&
            !core_in_set(static_cast<uint32_t>(opts.value_core))) {
            throw std::invalid_argument(
                "runtime::build_runtime: opts.value_core is not a member "
                "of opts.cores");
        }
        if (opts.owner_core >= 0 &&
            !core_in_set(static_cast<uint32_t>(opts.owner_core))) {
            throw std::invalid_argument(
                "runtime::build_runtime: opts.owner_core is not a member "
                "of opts.cores");
        }
        if (opts.coord_core >= 0 &&
            !core_in_set(static_cast<uint32_t>(opts.coord_core))) {
            throw std::invalid_argument(
                "runtime::build_runtime: opts.coord_core is not a member "
                "of opts.cores");
        }
        if (opts.wal_space_core >= 0 &&
            !core_in_set(static_cast<uint32_t>(opts.wal_space_core))) {
            throw std::invalid_argument(
                "runtime::build_runtime: opts.wal_space_core is not a member "
                "of opts.cores");
        }
        for (size_t i = 0; i < opts.read_domain_cores.size(); ++i) {
            uint32_t c = opts.read_domain_cores[i];
            if (!core_in_set(c)) {
                throw std::invalid_argument(
                    "runtime::build_runtime: opts.read_domain_cores "
                    "contains a core not in opts.cores");
            }
            for (size_t j = 0; j < i; ++j) {
                if (opts.read_domain_cores[j] == c) {
                    throw std::invalid_argument(
                        "runtime::build_runtime: opts.read_domain_cores "
                        "contains a duplicate");
                }
            }
        }
        for (size_t i = 0; i < opts.front_cores.size(); ++i) {
            uint32_t c = opts.front_cores[i];
            if (!core_in_set(c)) {
                throw std::invalid_argument(
                    "runtime::build_runtime: opts.front_cores contains a "
                    "core not in opts.cores");
            }
            for (size_t j = 0; j < i; ++j) {
                if (opts.front_cores[j] == c) {
                    throw std::invalid_argument(
                        "runtime::build_runtime: opts.front_cores contains "
                        "a duplicate");
                }
            }
        }
    }

    // ── build_runtime: construct schedulers + register ──
    //
    // Startup sequence (030 §2.8, extended to support asymmetric
    // topologies):
    //   1. Validate `opts` + `kBootstrapFormatProfile` (fail-fast).
    //   2. Resolve the role → core map. If `opts.read_domain_cores` is
    //      empty the symmetric layout is used (every member of
    //      `opts.cores` hosts a `tree_read_domain`). `value_core` and
    //      `owner_core` default to `cores[0]` when left at -1.
    //   3. Install the bootstrap `shard_partition_map` — a single-shard
    //      placeholder covering (-∞, +∞) → shard 0 (030 §6.7 decision P).
    //      The shard count tracks the number of read_domain hosts, not
    //      the total core count.
    //   4. Per core in `opts.cores`:
    //        - unconditionally build `nvme::runtime_scheduler`;
    //        - if this core is a read_domain host, build
    //          `tree_read_domain<TreeCache>` (which owns
    //          `tree_lookup_sched` / `tree_worker_sched` via unique_ptr);
    //        - if this core is the value host, build
    //          `value::value_alloc_sched<ValueCache>`;
    //        - if this core is the owner host, build `tree::tree_sched`.
    //      Non-role cores receive a nullptr slot in the PUMP per-core
    //      tuple and `share_nothing::run()` skips the empty slot.
    //   5. Register non-templated base pointers into
    //      `core::registry::tree_read_domains`. Application code reaches
    //      schedulers via `tree_read_domain_at(idx)->lookup_sched` /
    //      `->worker_sched`.
    //
    // INC-034: the standard runtime always constructs the value
    // scheduler; tests that want a tree-only harness must route through
    // a dedicated test fixture, not through build_runtime / start_runtime.

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    inline inconel_runtime_t<TreeCache, ValueCache>*
    build_runtime(const build_options& opts) {
        const auto& profile = format::kBootstrapFormatProfile;
        validate_build_inputs(opts, profile);

        const uint32_t max_cores = std::thread::hardware_concurrency();
        core::registry::clear();
        core::registry::init_capacity(max_cores);

        auto* rt = new inconel_runtime_t<TreeCache, ValueCache>();
        auto shared_heads = std::make_shared<core::data_area_heads>();
        core::registry::data_area_heads_ptr = shared_heads;
        auto wal_reclaim_frontier =
            std::make_shared<core::wal_reclaim_frontier>();
        core::registry::wal_reclaim_frontier_ptr = wal_reclaim_frontier;

        // ── Step 2: resolve role → core map ─────────────────────────
        const uint32_t value_core = opts.value_core < 0
            ? opts.cores.front()
            : static_cast<uint32_t>(opts.value_core);
        const uint32_t owner_core = opts.owner_core < 0
            ? opts.cores.front()
            : static_cast<uint32_t>(opts.owner_core);
        std::vector<uint32_t> read_domain_cores_buf;
        std::span<const uint32_t> read_domain_cores = opts.read_domain_cores;
        if (read_domain_cores.empty()) {
            read_domain_cores_buf.assign(opts.cores.begin(), opts.cores.end());
            read_domain_cores = read_domain_cores_buf;
        }

        auto rd_slot_of = [&](uint32_t core) -> int32_t {
            for (size_t i = 0; i < read_domain_cores.size(); ++i) {
                if (read_domain_cores[i] == core) {
                    return static_cast<int32_t>(i);
                }
            }
            return -1;
        };

        // ── Step 3: bootstrap shard_partition_map ───────────────────
        // Empty leaf_order at boot; `build_initial_shard_partition_map`
        // returns a single-shard placeholder map (030 §2.2 decision P).
        // Shard count tracks the number of read_domain hosts — not
        // every core is a read_domain in asymmetric topologies.
        //
        // install_shard_partitions here (not publish) because the
        // read_domains do not exist yet: their ctors pull the
        // shared_ptr via `current_shard_partitions()`. The publish
        // call at the end of the builder re-syncs the same map into
        // every read_domain for parity with the rebuild path.
        std::shared_ptr<const core::shard_partition_map> bootstrap_map;
        {
            core::leaf_order_index empty_leaf_order;
            auto map = core::build_initial_shard_partition_map(
                empty_leaf_order,
                static_cast<uint32_t>(read_domain_cores.size()));
            bootstrap_map = std::make_shared<const core::shard_partition_map>(
                std::move(map));
            core::registry::install_shard_partitions(bootstrap_map);
        }

        // ── Step 3b: front / coord / WAL topology ───────────────────
        //
        // PUMP add_core_schedulers() is a one-shot tuple install per core,
        // so the front stack must be fully constructed before the per-core
        // loop below. The loop then passes all seven scheduler classes in a
        // single call for each core.
        auto front_topo = build_front_topology(front_topology_options{
            .cores = opts.cores,
            .front_cores = opts.front_cores,
            .coord_core = opts.coord_core,
            .wal_space_core = opts.wal_space_core,
            .wal_geometry = wal_geometry_from_profile(profile),
            .wal_reclaim_frontier = wal_reclaim_frontier.get(),
            .tree_geometry = &kBootstrapTreeGeometry,
            .front_queue_depth = opts.front_queue_depth,
            .coord_queue_depth = opts.coord_queue_depth,
            .coord_ready_window = opts.coord_ready_window,
            .front_wal_config = opts.front_wal_config,
        });

        // ── Step 4: per-core scheduler construction ─────────────────
        using value_sched_t = value::value_alloc_sched<ValueCache>;
        value_sched_t* value_sched_singleton = nullptr;
        tree::tree_sched* tsched_singleton = nullptr;

        for (uint32_t core : opts.cores) {
            // PUMP per_core::queue routes by this_core_id, so it must be set
            // before constructing schedulers that hold such queues.
            pump::core::this_core_id = core;

            auto* nvme_sched = new nvme::runtime_scheduler(
                opts.device->qpair_for_core(core),
                profile.lba_size,
                opts.nvme_dma_pool_pages_per_core,
                opts.nvme_queue_depth,
                opts.nvme_local_depth,
                opts.nvme_dma_alignment,
                opts.nvme_numa_id,
                opts.device->device_id());

            // H-1 (review §1): every tree_lookup_sched is locked to a
            // single tree_geometry instance — namely the bootstrap one
            // owned by this translation unit. Any manifest fed to
            // `process()` later on is asserted to reference the same
            // value, so free-frame reuse cannot cross page sizes. The
            // read_domain transparently propagates that pointer into
            // its owned lookup scheduler.
            core::tree_read_domain<TreeCache>* read_domain = nullptr;
            const int32_t rd_index = rd_slot_of(core);
            if (rd_index >= 0) {
                read_domain = new core::tree_read_domain<TreeCache>(
                    static_cast<uint32_t>(rd_index),
                    core::registry::current_shard_partitions(),
                    TreeCache(opts.tree_cache_capacity),
                    &kBootstrapTreeGeometry,
                    opts.tree_queue_depth,
                    make_runtime_dma_page_allocator(),
                    opts.nvme_dma_alignment,
                    opts.nvme_numa_id);
            }

            value_sched_t* value_sched = nullptr;
            if (core == value_core) {
                value_sched = new value_sched_t(
                    profile.class_sizes(),
                    profile.lba_size,
                    profile.value_data_area_base,
                    profile.value_data_area_end,
                    shared_heads.get(),
                    ValueCache(opts.value_cache_capacity),
                    profile.value_space_quantum_bytes,
                    profile.value_space_group_size_lbas,
                    opts.value_queue_depth,
                    make_runtime_dma_page_allocator(),
                    opts.nvme_dma_alignment,
                    opts.nvme_numa_id,
                    opts.value_io_policy);
                core::registry::value_alloc_sched = value_sched;
                value_sched_singleton = value_sched;
                // value_head_lba is published by value_alloc_sched's ctor
                // (it constructs the value_space_manager and pins the
                // initial low watermark to data_area_end). No explicit
                // publish needed here.
            }

            tree::tree_sched* tsched = nullptr;
            if (core == owner_core) {
                tsched = new tree::tree_sched(
                    &kBootstrapTreeGeometry,
                    profile.value_data_area_base,
                    shared_heads.get(),
                    wal_reclaim_frontier.get(),
                    opts.tree_queue_depth,
                    make_runtime_dma_page_allocator(),
                    opts.nvme_dma_alignment,
                    opts.nvme_numa_id);
                core::registry::tree_sched_singleton_ptr = tsched;
                core::set_reclaim_sink(tsched);
                tsched_singleton = tsched;
                shared_heads->tree_head_lba.store(
                    tsched->state.alloc.head.lba, std::memory_order_relaxed);
            }

            coord::coord_sched* coord_sched =
                core == front_topo.coord_core ? front_topo.coord : nullptr;
            front::front_sched* front_sched =
                core < core::registry::front_scheds.by_core.size()
                    ? core::registry::front_scheds.by_core[core]
                    : nullptr;
            wal::wal_space_sched* wal_space_sched =
                core == front_topo.wal_space_core
                    ? front_topo.wal_space
                    : nullptr;

            // PUMP runtime (typed). Tuple order matches `inconel_runtime_t`:
            // nvme, tree_read_domain, value_alloc, tree_sched, coord,
            // front, wal_space.
            rt->add_core_schedulers(
                core,
                nvme_sched,
                read_domain,
                value_sched,
                tsched,
                coord_sched,
                front_sched,
                wal_space_sched);

            // Application registry (non-templated)
            core::registry::nvme_scheds.list.push_back(nvme_sched);
            core::registry::nvme_scheds.by_core[core] = nvme_sched;
            if (read_domain != nullptr) {
                core::registry::tree_read_domains.list.push_back(read_domain);
                core::registry::tree_read_domains.by_core[core] = read_domain;
            }
        }

        core::registry::nvme_by_front_owner.assign(
            front_topo.fronts.size(), nullptr);
        for (std::size_t owner = 0; owner < front_topo.fronts.size(); ++owner) {
            const uint32_t front_core = front_topo.front_cores[owner];
            auto* nvme_for_owner = core::registry::nvme_scheds.by_core[front_core];
            if (nvme_for_owner == nullptr) {
                throw std::logic_error(
                    "runtime::build_runtime: front owner has no home-core "
                    "nvme scheduler");
            }
            core::registry::nvme_by_front_owner[owner] = nvme_for_owner;
        }

        // Suppress unused-variable warnings when the helper singletons
        // are not consulted after construction — they are retained for
        // readability and future assertions.
        (void)value_sched_singleton;
        (void)tsched_singleton;

        // Go through `publish_shard_partitions` even during bootstrap
        // so every install path (bootstrap + future tree_sched
        // rebuild) converges on the same 2-step contract: install the
        // global pointer + refresh every read_domain's snapshot. The
        // refresh is a no-op in the common case (the read_domains
        // just had the same `shared_ptr` handed to their ctors) but
        // keeps the rebuild site from ever being the first code to
        // exercise the propagation loop.
        rt::publish_shard_partitions(bootstrap_map);

        return rt;
    }

    // ── tear_down: free schedulers + clear registry ──
    //
    // Destroy order: coord → fronts → wal_space → tree_sched →
    // tree_read_domain → value → nvme. The front stack has no destructor
    // dependency on tree/value/nvme; deleting coord first releases the CAT
    // pin chain before front-owned active gens go away. Each
    // `tree_read_domain<TreeCache>` owns its lookup and worker via
    // unique_ptr, so deleting the read_domain retires both arms.

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    inline void
    destroy_runtime(inconel_runtime_t<TreeCache, ValueCache>* rt) {
        core::set_reclaim_sink(nullptr);
        if (core::registry::coord_sched_singleton_ptr) {
            delete core::registry::coord_sched_singleton_ptr;
        }
        for (auto* fs : core::registry::front_scheds.list) {
            delete fs;
        }
        if (core::registry::wal_space_sched_singleton_ptr) {
            delete core::registry::wal_space_sched_singleton_ptr;
        }
        if (core::registry::tree_sched_singleton_ptr) {
            delete core::registry::tree_sched_singleton_ptr;
        }
        for (auto* rd : core::registry::tree_read_domains.list) {
            delete static_cast<core::tree_read_domain<TreeCache>*>(rd);
        }
        if (core::registry::value_alloc_sched) {
            using value_sched_t = value::value_alloc_sched<ValueCache>;
            delete static_cast<value_sched_t*>(core::registry::value_alloc_sched);
        }
        for (auto* s : core::registry::nvme_scheds.list) delete s;
        core::registry::clear();
        delete rt;
    }

}

#endif //APPS_INCONEL_RUNTIME_BUILDER_HH
