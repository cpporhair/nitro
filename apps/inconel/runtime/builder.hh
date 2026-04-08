#ifndef APPS_INCONEL_RUNTIME_BUILDER_HH
#define APPS_INCONEL_RUNTIME_BUILDER_HH

#include <cstdint>
#include <span>
#include <thread>

#include "env/runtime/share_nothing.hh"
#include "pump/core/lock_free_queue.hh"

#include "../core/page_cache.hh"
#include "../core/registry.hh"
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
        tree::lookup_scheduler<TreeCache>,
        value::scheduler<ValueCache>
    >;

    // ── Build context ──
    //
    // Hardcoded for now: every listed core hosts both nvme + tree_lookup.
    // Per-core placement (different scheduler sets per core) is supported
    // by PUMP runtime via nullptr slots, but the configuration plumbing
    // for it will land when more schedulers exist.

    struct build_options {
        std::span<const uint32_t> cores;             // which cores to populate
        mock_nvme::mock_device*   device;            // nvme backing device

        // tree cache capacity (entries). Applies to lookup_scheduler<TreeCache>.
        // Minimum legal value depends on the cache impl: clock_cache requires
        // >= 1, slru_cache requires >= 2 (so each segment gets at least one
        // slot under the default 0.8 protected ratio). Smaller values throw
        // std::invalid_argument from the cache ctor — there is no silent
        // capping. The default 32 is well above both thresholds.
        uint32_t                  tree_cache_capacity = 32;

        // value scheduler configuration. The class sizes must be sorted
        // ascending and each must satisfy the value_object alignment rules
        // (lba_size divisible by class_size for sub-LBA, or class_size
        // divisible by lba_size for LBA-aligned / multi-LBA). The value
        // scheduler is pinned to cores[0] and bumps from data_area_end
        // downward toward data_area_base.
        std::span<const uint32_t> value_class_sizes;
        uint32_t                  lba_size = 4096;
        format::paddr             value_data_area_base = {0, 0};
        format::paddr             value_data_area_end  = {0, 0};

        // value cache capacity (entries). Applies to value::scheduler<ValueCache>.
        // Same minimum-capacity rules as tree_cache_capacity above.
        uint32_t                  value_cache_capacity = 32;
    };

    // ── build_runtime: construct schedulers + double-register ──
    //
    // 1. Create one mock_nvme::scheduler and one lookup_scheduler<Cache> per core.
    // 2. Register each pair to PUMP runtime via add_core_schedulers (per-core tuple).
    // 3. Register the same pointers to core::registry::nvme_scheds and
    //    core::registry::tree_lookup_scheds (lookup_scheduler<Cache>* implicitly
    //    upcasts to lookup_scheduler_base*).
    //
    // After this returns, application code uses core::registry::local_nvme()
    // / local_tree_lookup() etc., never touching the runtime pointer directly.
    // The runtime pointer is only needed by pump::env::runtime::start/run.

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    inline inconel_runtime_t<TreeCache, ValueCache>*
    build_runtime(const build_options& opts) {
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
            auto* tlookup = new tree::lookup_scheduler<TreeCache>(
                TreeCache(opts.tree_cache_capacity));

            // value::scheduler is a singleton, pinned to cores[0]. Other
            // cores get a nullptr placeholder so the PUMP per-core tuple
            // shape stays uniform — share_nothing's advance loop skips
            // nullptr slots automatically.
            using value_sched_t = value::scheduler<ValueCache>;
            value_sched_t* value_sched = nullptr;
            if (first && !opts.value_class_sizes.empty()) {
                value_sched = new value_sched_t(
                    opts.value_class_sizes,
                    opts.lba_size,
                    opts.value_data_area_base,
                    opts.value_data_area_end,
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
            delete static_cast<tree::lookup_scheduler<TreeCache>*>(s);
        }
        if (core::registry::value_alloc_sched) {
            using value_sched_t = value::scheduler<ValueCache>;
            delete static_cast<value_sched_t*>(core::registry::value_alloc_sched);
        }
        core::registry::clear();
        delete rt;
    }

}

#endif //APPS_INCONEL_RUNTIME_BUILDER_HH
