#ifndef APPS_INCONEL_RUNTIME_BUILDER_HH
#define APPS_INCONEL_RUNTIME_BUILDER_HH

#include <cstdint>
#include <span>
#include <stdexcept>
#include <thread>

#include "env/runtime/share_nothing.hh"
#include "pump/core/lock_free_queue.hh"

#include "../core/page_cache.hh"
#include "../core/registry.hh"
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

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    using inconel_runtime_t = pump::env::runtime::global_runtime_t<
        mock_nvme::scheduler,
        tree::tree_lookup_sched<TreeCache>,
        value::value_alloc_sched<ValueCache>
    >;

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

    // ── build_runtime: construct schedulers + double-register ──
    //
    // 1. Validate opts + kBootstrapFormatProfile upfront (fail-fast).
    // 2. Create one mock_nvme::scheduler and one tree_lookup_sched<Cache>
    //    per core.
    // 3. Create the singleton value_alloc_sched<Cache> on cores[0] using the
    //    bootstrap profile; every other core carries a nullptr slot in the
    //    PUMP typed tuple.
    // 4. Register each pair to PUMP runtime via add_core_schedulers (per-core
    //    tuple).
    // 5. Register the same pointers to core::registry::nvme_scheds,
    //    core::registry::tree_lookup_scheds, and
    //    core::registry::value_alloc_sched (upcast to the non-templated base).
    //
    // After this returns, application code uses core::registry::local_nvme()
    // / local_tree_lookup() / value_sched() etc., never touching the runtime
    // pointer directly. The runtime pointer is only needed by
    // pump::env::runtime::start/run.
    //
    // INC-034: the "empty value_class_sizes → silent disable" half-runtime
    // that used to live in this function was removed. The standard runtime
    // now always constructs the value scheduler; tests that want a tree-only
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

        bool first = true;
        for (uint32_t core : opts.cores) {
            // PUMP per_core::queue routes by this_core_id, so it must be set
            // before constructing schedulers that hold such queues.
            pump::core::this_core_id = core;

            auto* nvme = new mock_nvme::scheduler(opts.device);
            auto* tlookup = new tree::tree_lookup_sched<TreeCache>(
                TreeCache(opts.tree_cache_capacity));

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
                    ValueCache(opts.value_cache_capacity));
                core::registry::value_alloc_sched = value_sched;
            }

            // PUMP runtime (typed)
            rt->add_core_schedulers(core, nvme, tlookup, value_sched);

            // Application registry (non-templated)
            core::registry::nvme_scheds.list.push_back(nvme);
            core::registry::nvme_scheds.by_core[core] = nvme;
            core::registry::tree_lookup_scheds.list.push_back(tlookup);
            core::registry::tree_lookup_scheds.by_core[core] = tlookup;

            first = false;
        }

        return rt;
    }

    // ── tear_down: free schedulers + clear registry ──

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    inline void
    destroy_runtime(inconel_runtime_t<TreeCache, ValueCache>* rt) {
        for (auto* s : core::registry::nvme_scheds.list)         delete s;
        for (auto* s : core::registry::tree_lookup_scheds.list) {
            delete static_cast<tree::tree_lookup_sched<TreeCache>*>(s);
        }
        if (core::registry::value_alloc_sched) {
            using value_sched_t = value::value_alloc_sched<ValueCache>;
            delete static_cast<value_sched_t*>(core::registry::value_alloc_sched);
        }
        core::registry::clear();
        delete rt;
    }

}

#endif //APPS_INCONEL_RUNTIME_BUILDER_HH
