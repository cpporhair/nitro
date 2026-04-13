#ifndef APPS_INCONEL_TREE_WORKER_SCHEDULER_HH
#define APPS_INCONEL_TREE_WORKER_SCHEDULER_HH

// ── tree_worker_sched — Phase 6 cache-aware worker (step 026) ──
//
// Split into non-templated base + templated derived, mirroring
// tree_lookup_sched_base / tree_lookup_sched<Cache>:
//
//   tree_worker_sched_base  — queues, schedule methods, PUMP ops.
//                             Registry stores base pointers.
//   tree_worker_sched<Cache> — holds Cache* (shared read_domain cache),
//                              advance() calls cache->pin() directly.
//
// The shared Cache instance is owned by whoever constructs the
// read_domain (currently tree_lookup_sched<Cache> owns it; a future
// step may extract it into a dedicated read_domain object). The
// worker holds a non-owning pointer set at construction time.
//
// Both lookup and worker call cache->pin(fid) with the same syntax
// — no type erasure, no function pointers.

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
#include "./leaf_mapping.hh"

namespace apps::inconel::tree {

    // ── PUMP req / op / sender for build_leaf_candidates ──

    struct tree_worker_sched_base;

    namespace _build_leaf_candidates {

        struct req {
            flush_worker_req args;
            std::move_only_function<void(flush_candidate_batch&&)> cb;
        };

        struct op {
            constexpr static bool build_leaf_candidates_op = true;

            tree_worker_sched_base* sched;
            flush_worker_req        args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_worker_sched_base* sched;
            flush_worker_req        args;

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

    // ── PUMP req / op / sender for leaf_mapping (Phase 5) ──

    namespace _leaf_mapping {

        struct req {
            flush_mapping_req args;
            std::move_only_function<void(flush_leaf_group_result&&)> cb;
        };

        struct op {
            constexpr static bool leaf_mapping_op = true;

            tree_worker_sched_base* sched;
            flush_mapping_req       args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_worker_sched_base* sched;
            flush_mapping_req       args;

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

    // ── PUMP req / op / sender for process_candidates (Phase 6) ──

    namespace _process_candidates {

        struct req {
            candidate_build_state* state;
            std::move_only_function<void(candidate_decision&&)> cb;
        };

        struct op {
            constexpr static bool process_candidates_op = true;

            tree_worker_sched_base* sched;
            candidate_build_state*  state;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_worker_sched_base* sched;
            candidate_build_state*  state;

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

    }  // namespace _process_candidates

    // ── tree_worker_sched_base (non-templated) ───────────────────
    //
    // Holds queues and schedule/submit methods. PUMP ops reference
    // this type. Registry stores pointers to this type.

    struct tree_worker_sched_base {
        static constexpr uint32_t kMaxBuildOpsPerAdvance            = 64;
        static constexpr uint32_t kMaxLeafMappingOpsPerAdvance      = 64;
        static constexpr uint32_t kMaxProcessCandidatesOpsPerAdvance = 8;

        uint32_t                                                      read_domain_index;
        tree_lookup_sched_base*                                       paired_lookup = nullptr;

        pump::core::per_core::queue<_build_leaf_candidates::req*>     build_q;
        pump::core::per_core::queue<_leaf_mapping::req*>              leaf_mapping_q;
        pump::core::per_core::queue<_process_candidates::req*>        candidates_q;

        explicit
        tree_worker_sched_base(uint32_t rdi,
                               tree_lookup_sched_base* lookup = nullptr,
                               std::size_t depth = 2048)
            : read_domain_index(rdi)
            , paired_lookup(lookup)
            , build_q(depth)
            , leaf_mapping_q(depth)
            , candidates_q(depth) {}

        void
        schedule_build(_build_leaf_candidates::req* r) {
            build_q.try_enqueue(r);
        }

        void
        schedule_leaf_mapping(_leaf_mapping::req* r) {
            leaf_mapping_q.try_enqueue(r);
        }

        void
        schedule_process_candidates(_process_candidates::req* r) {
            candidates_q.try_enqueue(r);
        }

        auto
        submit_build(flush_worker_req args) {
            return _build_leaf_candidates::sender{ this, args };
        }

        auto
        submit_leaf_mapping(flush_mapping_req args) {
            return _leaf_mapping::sender{ this, std::move(args) };
        }

        auto
        submit_process_candidates(candidate_build_state* state) {
            return _process_candidates::sender{ this, state };
        }
    };

    // ── tree_worker_sched<Cache> (templated) ─────────────────────
    //
    // Holds a non-owning Cache* — the same read_domain cache that
    // tree_lookup_sched<Cache> uses. advance() calls cache->pin()
    // directly — same syntax as lookup, no type erasure.

    template <core::cache_concept Cache>
    struct tree_worker_sched : tree_worker_sched_base {
        Cache* cache_;

        explicit
        tree_worker_sched(uint32_t rdi,
                          Cache* cache,
                          tree_lookup_sched_base* lookup = nullptr,
                          std::size_t depth = 2048)
            : tree_worker_sched_base(rdi, lookup, depth)
            , cache_(cache) {}

        bool
        advance() {
            bool progress = false;

            // ── drain build_q (Phase 2 stub) ──
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

            // ── drain candidates_q (Phase 6) ──
            for (uint32_t i = 0; i < kMaxProcessCandidatesOpsPerAdvance; ++i) {
                auto item = candidates_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                auto decision = process_candidate_groups(
                    *r->state, paired_lookup, cache_);
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

    // ── Deferred op::start definitions ──────────────────────────

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

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _process_candidates::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_process_candidates(new _process_candidates::req{
            state,
            [ctx = ctx, scope = scope](candidate_decision&& d) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(d));
            },
        });
    }

}  // namespace apps::inconel::tree

// ── PUMP specializations ──

namespace pump::core {

    // build_leaf_candidates
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

    // leaf_mapping
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

    // process_candidates
    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::process_candidates_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_process_candidates::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::candidate_decision>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_TREE_WORKER_SCHEDULER_HH
