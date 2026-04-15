#ifndef APPS_INCONEL_TREE_WORKER_SCHEDULER_HH
#define APPS_INCONEL_TREE_WORKER_SCHEDULER_HH

// ── tree_worker_sched — Phase 7 cache-aware worker (step 027) ──
//
// Split into non-templated base + templated derived, mirroring
// tree_lookup_sched_base / tree_lookup_sched<Cache>:
//
//   tree_worker_sched_base  — single per-round queue + schedule /
//                             submit methods. Registry stores base
//                             pointers.
//   tree_worker_sched<Cache> — holds Cache* (shared read_domain
//                              cache); advance() invokes
//                              process_flush_round<Cache>().
//
// Step 027 collapses the prior two-handle worker surface
// (`_leaf_mapping` + `_process_candidates`) into a single handle
// `_flush_round` that drives one round of the worker's
// merge / cascade / build state machine. The wrapper
// `submit_flush_work` (this file, free helper) loops the per-round
// handle with NVMe-read dispatch until the proposal is complete.
//
// The shared Cache instance is owned by whoever constructs the
// read_domain (currently tree_lookup_sched<Cache> owns it). The
// worker holds a non-owning pointer set at construction time.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

#include "../core/panic.hh"
#include "./candidate_build.hh"
#include "./flush_types.hh"
#include "./lookup_scheduler.hh"

namespace apps::inconel::tree {

    // ── PUMP req / op / sender for flush_round (step 027) ──
    //
    // One handle per worker arm per round. Takes a pointer to the
    // worker_state on the PUMP context stack and returns a
    // flush_round_decision (done | need_read).

    struct tree_worker_sched_base;

    namespace _flush_round {

        struct req {
            worker_state*                                       state;
            std::move_only_function<void(flush_round_decision&&)> cb;
        };

        struct op {
            constexpr static bool flush_round_op = true;

            tree_worker_sched_base* sched;
            worker_state*           state;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_worker_sched_base* sched;
            worker_state*           state;

            auto
            make_op() {
                return op{ .sched = sched, .state = state };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _flush_round

    // ── tree_worker_sched_base (non-templated) ───────────────────

    struct tree_worker_sched_base {
        static constexpr uint32_t kMaxFlushRoundOpsPerAdvance = 16;

        uint32_t                read_domain_index;
        tree_lookup_sched_base* paired_lookup = nullptr;

        pump::core::per_core::queue<_flush_round::req*> flush_round_q;

        explicit
        tree_worker_sched_base(uint32_t                rdi,
                               tree_lookup_sched_base* lookup = nullptr,
                               std::size_t             depth  = 2048)
            : read_domain_index(rdi)
            , paired_lookup(lookup)
            , flush_round_q(depth) {}

        void
        schedule_flush_round(_flush_round::req* r) {
            flush_round_q.try_enqueue(r);
        }

        // Per-round handle (step 027 §3): submit the worker_state for
        // one round of merge/cascade/build. The wrapper sender
        // submit_flush_work() drives the loop.
        auto
        submit_flush_round(worker_state* state) {
            return _flush_round::sender{ this, state };
        }
    };

    // ── tree_worker_sched<Cache> (templated) ─────────────────────
    //
    // Holds a non-owning Cache* — the same read_domain cache that
    // tree_lookup_sched<Cache> uses. advance() invokes the cache-
    // aware per-round driver from candidate_build.hh.

    template <core::cache_concept Cache>
    struct tree_worker_sched : tree_worker_sched_base {
        Cache* cache_;

        explicit
        tree_worker_sched(uint32_t                rdi,
                          Cache*                  cache,
                          tree_lookup_sched_base* lookup = nullptr,
                          std::size_t             depth  = 2048)
            : tree_worker_sched_base(rdi, lookup, depth)
            , cache_(cache) {}

        bool
        advance() {
            bool progress = false;

            for (uint32_t i = 0; i < kMaxFlushRoundOpsPerAdvance; ++i) {
                auto item = flush_round_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                if (r->state == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_worker_sched::advance",
                        "_flush_round::req with null worker_state "
                        "(rdi=%u)",
                        static_cast<unsigned>(read_domain_index));
                }
                if (r->state->read_domain_index != read_domain_index) {
                    core::panic_inconsistency(
                        "tree::tree_worker_sched::advance",
                        "worker_state routed to wrong worker: "
                        "state.rdi=%u self.rdi=%u",
                        static_cast<unsigned>(r->state->read_domain_index),
                        static_cast<unsigned>(read_domain_index));
                }

                auto decision = process_flush_round(*r->state, cache_);
                r->cb(std::move(decision));
                delete r;
                progress = true;
            }

            return progress;
        }

        template <typename runtime_t>
        bool
        advance(runtime_t&) {
            return advance();
        }
    };

    // ── Deferred op::start definition ──

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _flush_round::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_flush_round(new _flush_round::req{
            state,
            [ctx = ctx, scope = scope](flush_round_decision&& d) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(d));
            },
        });
    }

}  // namespace apps::inconel::tree

// ── PUMP specializations ──

namespace pump::core {

    // flush_round
    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::flush_round_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename ctx_t>
        static void
        push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template <typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::tree::_flush_round::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::flush_round_decision>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_TREE_WORKER_SCHEDULER_HH
