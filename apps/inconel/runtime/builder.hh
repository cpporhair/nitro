#ifndef APPS_INCONEL_RUNTIME_BUILDER_HH
#define APPS_INCONEL_RUNTIME_BUILDER_HH

#include <cstdint>
#include <span>
#include <thread>

#include "env/runtime/share_nothing.hh"
#include "pump/core/lock_free_queue.hh"

#include "../core/page_cache.hh"
#include "../core/registry.hh"
#include "../mock_nvme/scheduler.hh"
#include "../tree/scheduler.hh"

namespace apps::inconel::runtime {

    // ── Inconel runtime type ──
    //
    // PUMP global_runtime_t parameterized on the cache policy. The full
    // scheduler type list lives here; future schedulers (coord, front, wal,
    // value, real_nvme) get added to this template parameter list.
    //
    // This template is the only place the Cache parameter appears at the
    // runtime level — application code interacts with schedulers via the
    // non-templated core::runtime registry.

    template <core::cache_concept Cache>
    using inconel_runtime_t = pump::env::runtime::global_runtime_t<
        mock_nvme::scheduler,
        tree::lookup_scheduler<Cache>
    >;

    // ── Build context ──
    //
    // Hardcoded for now: every listed core hosts both nvme + tree_lookup.
    // Per-core placement (different scheduler sets per core) is supported
    // by PUMP runtime via nullptr slots, but the configuration plumbing
    // for it will land when more schedulers exist.

    struct build_options {
        std::span<const uint32_t> cores;     // which cores to populate
        mock_nvme::mock_device*   device;    // nvme backing device
        uint32_t                  cache_capacity = 32;
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

    template <core::cache_concept Cache>
    inline inconel_runtime_t<Cache>*
    build_runtime(const build_options& opts) {
        const uint32_t max_cores = std::thread::hardware_concurrency();
        core::registry::clear();
        core::registry::init_capacity(max_cores);

        auto* rt = new inconel_runtime_t<Cache>();

        for (uint32_t core : opts.cores) {
            // PUMP per_core::queue routes by this_core_id, so it must be set
            // before constructing schedulers that hold such queues.
            pump::core::this_core_id = core;

            auto* nvme = new mock_nvme::scheduler(opts.device);
            auto* tlookup = new tree::lookup_scheduler<Cache>(Cache(opts.cache_capacity));

            // PUMP runtime (typed)
            rt->add_core_schedulers(core, nvme, tlookup);

            // Application registry (non-templated)
            core::registry::nvme_scheds.list.push_back(nvme);
            core::registry::nvme_scheds.by_core[core] = nvme;
            core::registry::tree_lookup_scheds.list.push_back(tlookup);
            core::registry::tree_lookup_scheds.by_core[core] = tlookup;
        }

        return rt;
    }

    // ── tear_down: free schedulers + clear registry ──

    template <core::cache_concept Cache>
    inline void
    destroy_runtime(inconel_runtime_t<Cache>* rt) {
        for (auto* s : core::registry::nvme_scheds.list)         delete s;
        for (auto* s : core::registry::tree_lookup_scheds.list) {
            delete static_cast<tree::lookup_scheduler<Cache>*>(s);
        }
        core::registry::clear();
        delete rt;
    }

}

#endif //APPS_INCONEL_RUNTIME_BUILDER_HH
