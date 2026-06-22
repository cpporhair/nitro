#ifndef APPS_INCONEL_RUNTIME_RUN_HH
#define APPS_INCONEL_RUNTIME_RUN_HH

// ── runtime/run.hh ── inconel per-core advance loop ──
//
// Inconel replaces `pump::env::runtime::run()` with its own loop for
// one reason: the PUMP version reads the per-core scheduler tuple on
// every iteration and does a null check per slot. Every scheduler
// pointer is write-once during `build_runtime()` — once the advance
// loop starts, the set of non-null slots never changes again. That
// means the null checks (and the tuple memory loads) can move out of
// the loop.
//
// `rt::run()` does exactly that: before entering the hot loop it
// walks the tuple once, captures each non-null slot's pointer plus a
// type-erased advance thunk into a fixed-size stack array, and then
// iterates that array on every tick. A core whose tuple has K
// non-null slots out of N does N trips through the fold once, and
// then K indirect calls per tick instead of N conditional loads.
//
// The thunks are plus-lambdas (no capture → function pointer), so
// the indirect call resolves to a direct `T::advance()` site the
// first time and stays in the branch target buffer.
//
// The loop still uses PUMP's `is_running_by_core[core]` so
// application-level stop signaling (clearing that atomic) keeps
// working the same way as before.
//
// Constraint: every scheduler in the tuple must expose a no-arg
// `advance()` returning `bool`. All Inconel schedulers satisfy this
// by construction; preemptive schedulers (which take a runtime
// reference) are not registered in Inconel today. If one is added,
// this loop must be extended to pass the runtime pointer through —
// most cleanly by widening the thunk signature to `bool(void*,
// void*)` and storing the runtime alongside the step.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "env/runtime/share_nothing.hh"
#include "pump/core/lock_free_queue.hh"

#include "./maintenance_scheduler.hh"

namespace apps::inconel::rt {

    template <typename... S, typename OnInit>
    inline void
    run(pump::env::runtime::global_runtime_t<S...>* runtime,
        uint32_t core,
        OnInit&& on_init) {
        pump::core::this_core_id = core;
        runtime->is_running_by_core[core].store(true);

        std::forward<OnInit>(on_init)(runtime, core);

        // ── Capture the per-core step list once ──
        struct step {
            void* obj;
            bool (*fn)(void*);
        };
        constexpr std::size_t kMaxSteps = sizeof...(S);
        std::array<step, kMaxSteps> steps{};
        std::size_t n = 0;

        std::apply(
            [&](auto*... sche) {
                auto add_one = [&](auto* p) {
                    if (!p) return;
                    using T = std::remove_pointer_t<decltype(p)>;
                    steps[n++] = step{
                        p,
                        +[](void* o) {
                            return static_cast<T*>(o)->advance();
                        }};
                };
                (add_one(sche), ...);
            },
            runtime->schedulers_by_core[core]);

        // ── Hot loop ──
        //
        // Once stop is requested, keep every core draining until the runtime
        // maintenance driver reports no inflight root pipeline. This preserves
        // the old "clear run flags" stop signal while honoring 061's
        // maintenance shutdown contract.
        while (true) {
            if (!runtime->is_running_by_core[core].load(
                    std::memory_order_acquire)) [[unlikely]] {
                ::apps::inconel::runtime::request_maintenance_disable(runtime);
                if (::apps::inconel::runtime::all_maintenance_quiesced(
                        runtime)) {
                    break;
                }
            }

            bool any = false;
            for (std::size_t i = 0; i < n; ++i) {
                any |= steps[i].fn(steps[i].obj);
            }
            if (!any) [[unlikely]] std::this_thread::yield();
        }
    }

    template <typename... S>
    inline void
    run(pump::env::runtime::global_runtime_t<S...>* runtime, uint32_t core) {
        // Fully qualify the three-arg overload to suppress ADL — the
        // first argument's namespace is `pump::env::runtime`, which
        // has its own `run(runtime, core, on_init)` that would
        // otherwise make this call ambiguous.
        ::apps::inconel::rt::run(runtime, core,
                                 [](auto*, uint32_t) {});
    }

    template <typename... S, typename OnInit>
    inline void
    start(pump::env::runtime::global_runtime_t<S...>* runtime,
          std::span<const uint32_t> cores,
          uint32_t main_core,
          OnInit&& on_init) {
        std::vector<std::jthread> workers;
        workers.reserve(cores.size());
        for (auto core : cores) {
            if (core == main_core) {
                continue;
            }
            workers.emplace_back([runtime, core, on_init]() mutable {
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET(core, &cpuset);
                pthread_setaffinity_np(
                    pthread_self(), sizeof(cpuset), &cpuset);
                ::apps::inconel::rt::run(runtime, core, on_init);
            });
        }
        auto stop_all = [&] {
            for (auto core : cores) {
                runtime->is_running_by_core[core].store(
                    false, std::memory_order_release);
            }
        };

        try {
            ::apps::inconel::rt::run(
                runtime, main_core, std::forward<OnInit>(on_init));
        } catch (...) {
            stop_all();
            throw;
        }
        stop_all();
    }

    template <typename... S>
    inline void
    start(pump::env::runtime::global_runtime_t<S...>* runtime,
          std::span<const uint32_t> cores,
          uint32_t main_core) {
        ::apps::inconel::rt::start(
            runtime, cores, main_core, [](auto*, uint32_t) {});
    }

}  // namespace apps::inconel::rt

#endif  // APPS_INCONEL_RUNTIME_RUN_HH
