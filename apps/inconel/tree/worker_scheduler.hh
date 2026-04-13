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
#include "./leaf_mapping.hh"

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

    // ── PUMP req / op / sender for leaf_mapping (Phase 5 D6-D8) ──

    namespace _leaf_mapping {

        struct req {
            flush_mapping_req args;
            std::move_only_function<void(flush_leaf_group_result&&)> cb;
        };

        struct op {
            constexpr static bool leaf_mapping_op = true;

            tree_worker_sched* sched;
            flush_mapping_req  args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_worker_sched* sched;
            flush_mapping_req  args;

            auto
            make_op() {
                return op{ .sched = sched, .args = std::move(args) };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _leaf_mapping

    // ── tree_worker_sched ──
    //
    // Single queue (`build_q`), single handle. Bounded drain per
    // advance() tick so no one scheduler starves the runtime loop.

    struct tree_worker_sched {
        static constexpr uint32_t kMaxBuildOpsPerAdvance       = 64;
        static constexpr uint32_t kMaxLeafMappingOpsPerAdvance = 64;  // D8

        uint32_t                                                      read_domain_index;
        pump::core::per_core::queue<_build_leaf_candidates::req*>     build_q;
        pump::core::per_core::queue<_leaf_mapping::req*>              leaf_mapping_q;  // Phase 5

        explicit
        tree_worker_sched(uint32_t rdi, std::size_t depth = 2048)
            : read_domain_index(rdi), build_q(depth), leaf_mapping_q(depth) {}

        void
        schedule_build(_build_leaf_candidates::req* r) {
            build_q.try_enqueue(r);
        }

        void
        schedule_leaf_mapping(_leaf_mapping::req* r) {
            leaf_mapping_q.try_enqueue(r);
        }

        // Sender factory (mirrors tree_lookup_sched_base::process).
        // Callers normally reach this through
        // `tree::build_leaf_candidates(worker, req)` in tree/sender.hh.
        auto
        submit_build(flush_worker_req args) {
            return _build_leaf_candidates::sender{ this, args };
        }

        // Phase 5 sender factory for leaf mapping.
        auto
        submit_leaf_mapping(flush_mapping_req args) {
            return _leaf_mapping::sender{ this, std::move(args) };
        }

        bool
        advance() {
            bool progress = false;

            // ── drain build_q (Phase 2) ──
            for (uint32_t i = 0; i < kMaxBuildOpsPerAdvance; ++i) {
                auto item = build_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

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

            // ── drain leaf_mapping_q (Phase 5) ──
            for (uint32_t i = 0; i < kMaxLeafMappingOpsPerAdvance; ++i) {
                auto item = leaf_mapping_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                // Fail-fast: routing validation (025 §fail-fast)
                if (r->args.read_domain_index != read_domain_index) {
                    core::panic_inconsistency(
                        "tree::tree_worker_sched::advance(leaf_mapping)",
                        "req routed to wrong worker: req.rdi=%u self.rdi=%u",
                        static_cast<unsigned>(r->args.read_domain_index),
                        static_cast<unsigned>(read_domain_index));
                }
                if (r->args.base_manifest == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_worker_sched::advance(leaf_mapping)",
                        "flush_mapping_req.base_manifest is null (rdi=%u)",
                        static_cast<unsigned>(read_domain_index));
                }

                // Run the mapping algorithm. keys_to_leaf_groups
                // sets result.st directly.
                flush_leaf_group_result res{
                    .round_id          = r->args.round_id,
                    .read_domain_index = r->args.read_domain_index,
                    .st                = flush_stage_status::ok,
                    .leaf_groups       = {},
                };
                keys_to_leaf_groups(r->args, res);

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

    // ── Deferred _leaf_mapping::op::start (needs full tree_worker_sched) ──

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _leaf_mapping::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_leaf_mapping(new _leaf_mapping::req{
            std::move(args),
            [ctx = ctx, scope = scope](flush_leaf_group_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

}  // namespace apps::inconel::tree

// ── PUMP specializations ──

namespace pump::core {

    // build_leaf_candidates op_pusher
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

    // leaf_mapping op_pusher (Phase 5)
    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::leaf_mapping_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_leaf_mapping::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::flush_leaf_group_result>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_TREE_WORKER_SCHEDULER_HH
