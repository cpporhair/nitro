#ifndef APPS_INCONEL_TREE_WORKER_SCHEDULER_HH
#define APPS_INCONEL_TREE_WORKER_SCHEDULER_HH

// ── tree_worker_sched — Phase 2 skeleton (step 022 §5, D7) ──
//
// Hosts the `build_leaf_candidates(flush_worker_req)` PUMP sender
// surface so that later phases (Phase 6 actual candidate build) can
// attach real logic without changing the sender shape, op layout, or
// runtime tuple position.
//
// Phase 2 constraints:
//
//   1. Worker is **non-templated**. Cache / frame pool / inflight are
//      still lookup-local in Phase 2 (step 022 §3, §4, §6) — the worker
//      does not need a Cache template parameter until the cache
//      ownership migration step before Phase 5/6.
//   2. Worker does not read old leaves, does not merge, does not
//      compact, does not touch NVMe. The handle simply returns a
//      `flush_candidate_batch` with
//      `st = flush_stage_status::unsupported_unimplemented` through
//      the normal value path (022 D11, §10) — no exception channel.
//   3. `read_domain_index` is stored here so `registry::by_core[core]`
//      can cheaply answer "which worker shares a domain with this
//      lookup" without materializing a named `tree_read_domain`
//      runtime object (022 D5, §3).
//
// File split (022 D12, §9): the op/sender/op_pusher/compute_sender_type
// specializations for this sender live in this header, next to their
// definitions. `tree/scheduler.hh` is an umbrella shim and
// `tree/sender.hh` is the outward-facing facade that #includes both
// sub-headers.

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
#include "./flush_types.hh"

namespace apps::inconel::tree {

    // ── PUMP req / op / sender for build_leaf_candidates ──

    struct tree_worker_sched;

    namespace _build_leaf_candidates {

        struct req {
            flush_worker_req args;
            std::move_only_function<void(flush_candidate_batch&&)> cb;
        };

        struct op {
            constexpr static bool build_leaf_candidates_op = true;

            tree_worker_sched* sched;
            flush_worker_req   args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_worker_sched* sched;
            flush_worker_req   args;

            auto
            make_op() {
                return op{ .sched = sched, .args = args };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _build_leaf_candidates

    // ── tree_worker_sched ──
    //
    // Single queue (`build_q`), single handle. Bounded drain per
    // advance() tick so no one scheduler starves the runtime loop.

    struct tree_worker_sched {
        static constexpr uint32_t kMaxBuildOpsPerAdvance = 64;

        uint32_t                                                      read_domain_index;
        pump::core::per_core::queue<_build_leaf_candidates::req*>     build_q;

        explicit
        tree_worker_sched(uint32_t rdi, std::size_t depth = 2048)
            : read_domain_index(rdi), build_q(depth) {}

        void
        schedule_build(_build_leaf_candidates::req* r) {
            build_q.try_enqueue(r);
        }

        // Sender factory (mirrors tree_lookup_sched_base::process).
        // Callers normally reach this through
        // `tree::build_leaf_candidates(worker, req)` in tree/sender.hh.
        auto
        submit_build(flush_worker_req args) {
            return _build_leaf_candidates::sender{ this, args };
        }

        bool
        advance() {
            bool progress = false;
            for (uint32_t i = 0; i < kMaxBuildOpsPerAdvance; ++i) {
                auto item = build_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                // M-4 (review §1): the Phase 2 pairing seam is
                // expressed via `read_domain_index` + same-core
                // install. A req whose `read_domain_index` does not
                // match this worker's own is a routing bug on the
                // caller side — not a tree-shape case — so it gets
                // the strongest fail-fast rather than being masked as
                // `unsupported_shape_change`. Likewise a null
                // `base_manifest` violates the carrier contract and
                // must panic here; we never let it leak into the
                // (currently stubbed) candidate build path.
                if (r->args.read_domain_index != read_domain_index) {
                    core::panic_inconsistency(
                        "tree::tree_worker_sched::advance",
                        "req routed to wrong worker: req.rdi=%u self.rdi=%u",
                        static_cast<unsigned>(r->args.read_domain_index),
                        static_cast<unsigned>(read_domain_index));
                }
                if (r->args.base_manifest == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_worker_sched::advance",
                        "flush_worker_req.base_manifest is null (rdi=%u)",
                        static_cast<unsigned>(read_domain_index));
                }

                // Phase 2: no old-leaf read, no merge, no compact.
                // Return the structured unsupported result via the
                // value path (022 D11, §10). The caller pipeline is
                // responsible for reacting to `st ==
                // unsupported_unimplemented` — we never silently emit
                // an empty `leaves` vector.
                flush_candidate_batch res{
                    .round_id          = r->args.round_id,
                    .read_domain_index = r->args.read_domain_index,
                    .st                = flush_stage_status::unsupported_unimplemented,
                    .leaves            = {},
                };
                r->cb(std::move(res));
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

    // ── Deferred op::start definition (needs full tree_worker_sched) ──

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _build_leaf_candidates::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_build(new _build_leaf_candidates::req{
            args,
            [ctx = ctx, scope = scope](flush_candidate_batch&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

}  // namespace apps::inconel::tree

// ── PUMP specializations ──

namespace pump::core {

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::build_leaf_candidates_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_build_leaf_candidates::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::flush_candidate_batch>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_TREE_WORKER_SCHEDULER_HH
