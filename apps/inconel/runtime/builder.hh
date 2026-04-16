#ifndef APPS_INCONEL_RUNTIME_BUILDER_HH
#define APPS_INCONEL_RUNTIME_BUILDER_HH

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>

#include "env/runtime/share_nothing.hh"
#include "pump/core/lock_free_queue.hh"

#include "../core/page_cache.hh"
#include "../core/registry.hh"
#include "../core/data_area_heads.hh"
#include "../core/leaf_order.hh"
#include "../core/shard_partition_builder.hh"
#include "../core/tree_geometry.hh"
#include "../core/tree_read_domain.hh"
#include "../format/format_profile.hh"
#include "../format/types.hh"
#include "../mock_nvme/scheduler.hh"
#include "../tree/scheduler.hh"
#include "../value/scheduler.hh"

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
        mock_nvme::scheduler,
        core::tree_read_domain<TreeCache>,
        value::value_alloc_sched<ValueCache>,
        tree::tree_sched
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

    // ── Build context ──
    //
    // Hardcoded for now: every listed core hosts both nvme + tree_lookup.
    // The value scheduler is a singleton pinned to cores[0]; the PUMP typed
    // tuple on every other core carries a nullptr slot so share_nothing's
    // advance loop skips the empty slot automatically.
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
        mock_nvme::mock_device*   device;            // nvme backing device

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
        if (opts.device->get_lba_size() != profile.lba_size) {
            throw std::invalid_argument(
                "runtime::build_runtime: device lba_size does not match "
                "profile.lba_size");
        }
        if (opts.device->get_total_lbas() < profile.value_data_area_end.lba) {
            throw std::runtime_error(
                "runtime::build_runtime: device namespace too small for "
                "profile value_data_area_end.lba");
        }
    }

    // ── build_runtime: construct schedulers + register ──
    //
    // Startup sequence (030 §2.8):
    //   1. Validate `opts` + `kBootstrapFormatProfile` (fail-fast).
    //   2. Install the bootstrap `shard_partition_map` — a single-shard
    //      placeholder covering (-∞, +∞) → shard 0 (030 §6.7 decision P).
    //      The tree is empty at boot (leaf_order = {}) and
    //      `manifest->has_root()` short-circuits reads before they ever
    //      route; the placeholder is present so the routing invariant
    //      ("map is always installed") holds during the bootstrap
    //      window between first tree_read_domain construction and the
    //      first flush that populates `leaf_order`.
    //   3. Per core: construct `tree_read_domain<TreeCache>` which in
    //      turn owns its `tree_lookup_sched<TreeCache>` and
    //      `tree_worker_sched<TreeCache>` via `unique_ptr`. The PUMP
    //      runtime tuple registers the read_domain (not the individual
    //      schedulers) — one scheduling unit per core.
    //   4. Construct the global singletons `value_alloc_sched` and
    //      `tree::tree_sched` on the first core.
    //   5. Register non-templated base pointers into
    //      `core::registry::tree_read_domains`. Application code reaches
    //      schedulers via `tree_read_domain_at(idx)->lookup_sched` /
    //      `->worker_sched`.
    //
    // INC-034 (unchanged by 030): the standard runtime always
    // constructs the value scheduler; tests that want a tree-only
    // harness must route through a dedicated test fixture, not through
    // build_runtime / start_runtime.

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

        // ── Step 2: bootstrap shard_partition_map ───────────────────
        // Empty leaf_order at boot; `build_initial_shard_partition_map`
        // returns a single-shard placeholder map (030 §2.2 decision P).
        // The placeholder routes every key to shard 0 — K - 1 read
        // domains sit idle until the first flush populates `leaf_order`
        // and (future) `tree_sched` triggers a rebuild. Bootstrap-time
        // reads are short-circuited by `manifest->has_root() == false`,
        // so the placeholder is not exercised on the read path.
        {
            core::leaf_order_index empty_leaf_order;
            auto map = core::build_initial_shard_partition_map(
                empty_leaf_order,
                static_cast<uint32_t>(opts.cores.size()));
            core::registry::install_shard_partitions(
                std::make_shared<const core::shard_partition_map>(
                    std::move(map)));
        }

        bool first = true;
        uint32_t next_read_domain_index = 0;
        for (uint32_t core : opts.cores) {
            // PUMP per_core::queue routes by this_core_id, so it must be set
            // before constructing schedulers that hold such queues.
            pump::core::this_core_id = core;

            const uint32_t read_domain_index = next_read_domain_index++;

            auto* nvme = new mock_nvme::scheduler(opts.device);

            // H-1 (review §1): every tree_lookup_sched is locked to a
            // single tree_geometry instance — namely the bootstrap one
            // owned by this translation unit. Any manifest fed to
            // `process()` later on is asserted to reference the same
            // value, so free-frame reuse cannot cross page sizes. The
            // read_domain transparently propagates that pointer into
            // its owned lookup scheduler.
            auto* read_domain = new core::tree_read_domain<TreeCache>(
                read_domain_index,
                core::registry::current_shard_partitions(),
                TreeCache(opts.tree_cache_capacity),
                &kBootstrapTreeGeometry);

            // Singleton: value_alloc_sched is pinned to cores[0]. Every other
            // core gets a nullptr placeholder so the PUMP per-core tuple shape
            // stays uniform.
            using value_sched_t = value::value_alloc_sched<ValueCache>;
            value_sched_t* value_sched = nullptr;
            if (first) {
                value_sched = new value_sched_t(
                    profile.class_sizes(),
                    profile.lba_size,
                    profile.value_data_area_base,
                    profile.value_data_area_end,
                    shared_heads.get(),
                    ValueCache(opts.value_cache_capacity));
                core::registry::value_alloc_sched = value_sched;
            }

            // Singleton: tree::tree_sched is pinned to cores[0] (step
            // 023 §9, D18/D25). Every other core carries a nullptr
            // placeholder in the per-core tuple so share_nothing's
            // `run()` loop skips the empty slot. The singleton is
            // constructed before the `add_core_schedulers` call below
            // so the first core's tuple gets the real pointer and
            // every subsequent core's tuple gets nullptr.
            tree::tree_sched* tsched = nullptr;
            if (first) {
                tsched = new tree::tree_sched(
                    &kBootstrapTreeGeometry,
                    profile.value_data_area_base,
                    shared_heads.get());
                core::registry::tree_sched_singleton_ptr = tsched;
                shared_heads->tree_head_lba.store(
                    tsched->state.alloc.head.lba, std::memory_order_relaxed);
                shared_heads->value_head_lba.store(
                    value_sched->alloc_.bump_head().lba,
                    std::memory_order_relaxed);
            }

            // PUMP runtime (typed). Tuple order matches `inconel_runtime_t`:
            // nvme, tree_read_domain, value_alloc, tree_sched.
            rt->add_core_schedulers(core, nvme, read_domain, value_sched, tsched);

            // Application registry (non-templated)
            core::registry::nvme_scheds.list.push_back(nvme);
            core::registry::nvme_scheds.by_core[core] = nvme;
            core::registry::tree_read_domains.list.push_back(read_domain);
            core::registry::tree_read_domains.by_core[core] = read_domain;

            first = false;
        }

        return rt;
    }

    // ── tear_down: free schedulers + clear registry ──
    //
    // Destroy order (030 §2.3 ownership graph): tree_sched →
    // tree_read_domain → value → nvme. `tree_sched` still goes first
    // because its destructor is trivial and we want the order stable
    // across later phases that add real reclaim_q plumbing. Each
    // `tree_read_domain<TreeCache>` owns its lookup and worker via
    // unique_ptr, so deleting the read_domain retires both arms —
    // there are no separate delete loops for the sub-schedulers (030
    // §2.3 ownership graph / §2.7 registry).

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    inline void
    destroy_runtime(inconel_runtime_t<TreeCache, ValueCache>* rt) {
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
