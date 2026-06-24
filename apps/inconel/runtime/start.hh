#ifndef APPS_INCONEL_RUNTIME_START_HH
#define APPS_INCONEL_RUNTIME_START_HH

#include <exception>
#include <span>
#include <stdexcept>
#include <string_view>

#include "env/runtime/share_nothing.hh"

#include "../core/page_cache.hh"
#include "../core/panic.hh"
#include "./builder.hh"
#include "./run.hh"

namespace apps::inconel::runtime {

    // ── start_runtime: compatibility low-level entry point ──
    //
    // New application-facing code should use `runtime/start_db.hh`.
    // `start_runtime()` is retained for low-level scheduler bring-up callers
    // that already own a live device and do not need boot recovery,
    // app-root submit, or lifecycle stats. It is no longer the canonical DB
    // entry point.
    //
    // Parses tree+value cache policies from strings, instantiates the
    // corresponding inconel_runtime_t<TreeCache, ValueCache> specialization
    // once, then hands off to Inconel's pre-captured per-core run loop which
    // blocks the main core.
    //
    // The cache template parameters only cross this single dispatch point —
    // production runtime policies instantiate the segmented cache aliases
    // used by LBA DMA frames. Every other module sees only base classes via
    // core::registry. Two
    // nested template dispatches expand into 4 instantiations
    // (clock×clock, clock×slru, slru×clock, slru×slru), which is more
    // readable than a flat 4-way if/else chain.
    //
    // Shutdown uses the same run-flag signal as PUMP, but Inconel's run loop
    // treats a cleared flag as "stop accepting new maintenance and keep
    // draining until the maintenance root pipeline quiesces". Callers using
    // the lower-level `build_runtime()` API may call
    // `disable_maintenance_and_wait(rt)` before clearing run flags when they
    // want an explicit two-phase stop.

    // Step 017 / INC-034: the disk-format fields that used to live here
    // (value_class_sizes / lba_size / value_data_area_{base,end}) are now
    // owned by `format::kBootstrapFormatProfile` and read internally by
    // build_runtime. `start_options` only keeps genuine per-run parameters:
    // cache policy + capacity, runtime topology, and the live device handle.
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
        nvme::runtime_device*     device;
        maintenance_options       maintenance = {};
    };

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    inline void
    run_with(const start_options& opts) {
        // Init failures (build_runtime constructor faults, share_nothing
        // start exceptions) are process-fatal: at this point neither the
        // PUMP runtime nor the Inconel registry has reached a state we can
        // partially unwind, so leaking via abort and letting the OS reclaim
        // is strictly cleaner than half-initialized cleanup paths. Both
        // std::exception and unknown throws funnel through panic so the
        // operator gets a diagnostic before SIGABRT.
        try {
            build_options bopts{
                .cores                = opts.cores,
                .device               = opts.device,
                .tree_cache_capacity  = opts.tree_cache_capacity,
                .value_cache_capacity = opts.value_cache_capacity,
                .maintenance          = opts.maintenance,
            };
            auto* rt = build_runtime<TreeCache, ValueCache>(bopts);
            ::apps::inconel::rt::start(rt, opts.cores, opts.main_core,
                [](auto*, uint32_t /*core*/) {
                    // Per-core init hook. Inconel's run loop has already set
                    // this_core_id by the time this runs. Future per-core
                    // init (e.g. NUMA-local allocator warm-up) goes here.
                });
            // start() returns after the main-core loop observes stop, asks
            // every worker loop to stop, and joins the worker threads.
            destroy_runtime<TreeCache, ValueCache>(rt);
        } catch (const std::exception& e) {
            core::panic_inconsistency("runtime::run_with",
                "init failed: %s", e.what());
        } catch (...) {
            core::panic_inconsistency("runtime::run_with",
                "init failed: unknown exception");
        }
    }

    template <core::cache_concept TreeCache>
    inline void
    start_with_tree(const start_options& opts) {
        if (opts.value_cache_policy == "clock") {
            run_with<TreeCache, core::segmented_clock_cache>(opts);
        } else if (opts.value_cache_policy == "slru") {
            run_with<TreeCache, core::segmented_slru_cache>(opts);
        } else {
            throw std::invalid_argument("unknown value_cache_policy");
        }
    }

    inline void
    start_runtime(const start_options& opts) {
        if (opts.tree_cache_policy == "clock") {
            start_with_tree<core::segmented_clock_cache>(opts);
        } else if (opts.tree_cache_policy == "slru") {
            start_with_tree<core::segmented_slru_cache>(opts);
        } else {
            throw std::invalid_argument("unknown tree_cache_policy");
        }
    }

}

#endif //APPS_INCONEL_RUNTIME_START_HH
