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
    // Parses tree+value cache policies from strings, instantiates the
    // corresponding inconel_runtime_t<TreeCache, ValueCache> specialization
    // once, then hands off to PUMP's share_nothing::start() which blocks the
    // main core.
    //
    // The cache template parameters only cross this single dispatch point —
    // every other module sees only base classes via core::registry. Two
    // nested template dispatches expand into 4 instantiations
    // (clock×clock, clock×slru, slru×clock, slru×slru), which is more
    // readable than a flat 4-way if/else chain.
    //
    // The runtime can be stopped by setting
    //   rt->is_running_by_core[core].store(false)
    // for each core; PUMP's start() will unwind once all cores exit their
    // advance loops. Stop coordination is left to the application (signal
    // handler / RPC / external trigger).

    struct start_options {
        // Cache policies — each is "clock" or "slru".
        std::string_view tree_cache_policy;
        std::string_view value_cache_policy;

        // Capacities — minimum legal value depends on the chosen policy:
        // "clock" requires >= 1, "slru" requires >= 2. Out-of-range values
        // throw std::invalid_argument from the cache ctor (propagated from
        // build_runtime). Default 32 is safely above both thresholds.
        uint32_t tree_cache_capacity  = 32;
        uint32_t value_cache_capacity = 32;

        std::span<const uint32_t> cores;
        uint32_t                  main_core = 0;
        mock_nvme::mock_device*   device;

        // value scheduler configuration. Empty value_class_sizes disables
        // the value scheduler (the runtime still builds, just without it).
        std::span<const uint32_t> value_class_sizes;
        uint32_t                  lba_size = 4096;
        format::paddr             value_data_area_base = {0, 0};
        format::paddr             value_data_area_end  = {0, 0};
    };

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    inline void
    run_with(const start_options& opts) {
        build_options bopts{
            .cores                = opts.cores,
            .device               = opts.device,
            .tree_cache_capacity  = opts.tree_cache_capacity,
            .value_class_sizes    = opts.value_class_sizes,
            .lba_size             = opts.lba_size,
            .value_data_area_base = opts.value_data_area_base,
            .value_data_area_end  = opts.value_data_area_end,
            .value_cache_capacity = opts.value_cache_capacity,
        };
        auto* rt = build_runtime<TreeCache, ValueCache>(bopts);
        pump::env::runtime::start(rt, opts.cores, opts.main_core,
            [](auto*, uint32_t /*core*/) {
                // Per-core init hook. PUMP has already set this_core_id by
                // the time this runs. Future per-core init (e.g. NUMA-local
                // allocator warm-up) goes here.
            });
        // start() returns once every is_running_by_core[core] flag is false.
        destroy_runtime<TreeCache, ValueCache>(rt);
    }

    template <core::cache_concept TreeCache>
    inline void
    start_with_tree(const start_options& opts) {
        if (opts.value_cache_policy == "clock") {
            run_with<TreeCache, core::clock_cache>(opts);
        } else if (opts.value_cache_policy == "slru") {
            run_with<TreeCache, core::slru_cache>(opts);
        } else {
            throw std::invalid_argument("unknown value_cache_policy");
        }
    }

    inline void
    start_runtime(const start_options& opts) {
        if (opts.tree_cache_policy == "clock") {
            start_with_tree<core::clock_cache>(opts);
        } else if (opts.tree_cache_policy == "slru") {
            start_with_tree<core::slru_cache>(opts);
        } else {
            throw std::invalid_argument("unknown tree_cache_policy");
        }
    }

}

#endif //APPS_INCONEL_RUNTIME_START_HH
