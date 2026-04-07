#ifndef APPS_INCONEL_RUNTIME_START_HH
#define APPS_INCONEL_RUNTIME_START_HH

#include <span>
#include <stdexcept>
#include <string_view>

#include "env/runtime/share_nothing.hh"

#include "../core/page_cache.hh"
#include "./builder.hh"

namespace apps::inconel::runtime {

    // ── start_runtime: top-level entry point ──
    //
    // Parses the cache policy from a string, instantiates the corresponding
    // build_runtime / inconel_runtime_t specialization once, then hands off
    // to PUMP's share_nothing::start() which blocks the main core.
    //
    // The cache template parameter only crosses this single dispatch point
    // — every other module sees only base classes via core::runtime.
    //
    // The runtime can be stopped by setting
    //   rt->is_running_by_core[core].store(false)
    // for each core; PUMP's start() will unwind once all cores exit their
    // advance loops. Stop coordination is left to the application (signal
    // handler / RPC / external trigger).

    struct start_options {
        std::string_view          cache_policy;     // "clock" | "slru"
        uint32_t                  cache_capacity = 32;
        std::span<const uint32_t> cores;
        uint32_t                  main_core = 0;
        mock_nvme::mock_device*   device;
    };

    template <core::cache_concept Cache>
    inline void
    run_with(const start_options& opts) {
        build_options bopts{
            .cores          = opts.cores,
            .device         = opts.device,
            .cache_capacity = opts.cache_capacity,
        };
        auto* rt = build_runtime<Cache>(bopts);
        pump::env::runtime::start(rt, opts.cores, opts.main_core,
            [](auto*, uint32_t /*core*/) {
                // Per-core init hook. PUMP has already set this_core_id by
                // the time this runs. Future per-core init (e.g. NUMA-local
                // allocator warm-up) goes here.
            });
        // start() returns once every is_running_by_core[core] flag is false.
        destroy_runtime<Cache>(rt);
    }

    inline void
    start_runtime(const start_options& opts) {
        if (opts.cache_policy == "clock") {
            run_with<core::clock_cache>(opts);
        } else if (opts.cache_policy == "slru") {
            run_with<core::slru_cache>(opts);
        } else {
            throw std::invalid_argument("unknown cache_policy");
        }
    }

}

#endif //APPS_INCONEL_RUNTIME_START_HH
