#ifndef APPS_INCONEL_TREE_OWNER_SCHEDULER_HH
#define APPS_INCONEL_TREE_OWNER_SCHEDULER_HH

// ── tree_sched — tree-domain flush round owner ──
//
// Phase 5 split the prior monolithic `_tree_flush` handle into the
// fold/merge stage pair. Phase 7 (step 027) keeps the same two-handle
// shape but updates the contract:
//
//   `_flush_fold`  — validate + park round + fold + leaf-aligned
//                    partition → cb(flush_fold_result).
//                    The partition step now consults
//                    `base_manifest->leaf_order` (027 §4.3 corrected
//                    to range-based assignment); empty trees and
//                    out-of-range keys propagate as
//                    unsupported_shape_change.
//   `_flush_merge` — receive the fanout's collected
//                    `vector<worker_tree_proposal>`, unpark round,
//                    return tree_flush_result. Phase 9 will replace
//                    the body with the actual hybrid-tree merge,
//                    paddr allocation, write dispatch, and new
//                    manifest construction. Phase 7 only validates
//                    the input shape and returns
//                    `unsupported_unimplemented`.
//
// The PUMP pipeline in sender.hh connects the two via the single
// worker fanout (`worker.submit_flush_work`).
//
// `reclaim_q` exists on `tree_state` for structural fidelity to RSM
// §4.1, but has no producer and no consumer yet (Phase 9 territory).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "../core/panic.hh"
#include "../format/types.hh"
#include "./flush_round_state.hh"
#include "./flush_types.hh"
#include "./memtable_fold.hh"

// Forward declaration to break include cycle:
// registry.hh → tree/scheduler.hh → owner_scheduler.hh.
namespace apps::inconel::core::registry {
    inline uint32_t tree_worker_count();
}

namespace apps::inconel::tree {

    // ── reclaim_task forward declaration (Phase 9 producer) ──────
    struct reclaim_task;

    // ── tree_allocator (Phase 9 placeholder) ─────────────────────
    struct tree_allocator {
        format::paddr                       head{};
        std::vector<format::range_ref>      free_ranges{};
    };

    // ── tree_state (RSM §4.1) ─────────────────────────────────────
    struct tree_state {
        tree_allocator alloc;
        uint64_t       flush_max_lsn       = 0;
        uint64_t       superblock_safe_lsn = 0;
        uint64_t       recovery_safe_lsn   = 0;

        pump::core::per_core::queue<reclaim_task*> reclaim_q{256};

        absl::flat_hash_map<uint64_t, std::unique_ptr<flush_round_state>>
            active_rounds;
        uint64_t next_round_id = 1;
    };

    // ── build_flushed_gens_by_front ──────────────────────────────
    static inline auto
    build_flushed_gens_by_front(
        const absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>& gens)
    {
        absl::flat_hash_map<uint32_t,
                            absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>>
            result;
        for (const auto& g : gens) {
            if (g->front_owner_index == UINT32_MAX)
                core::panic_inconsistency(
                    "build_flushed_gens_by_front",
                    "memtable_gen.front_owner_index not initialized");
            result[g->front_owner_index].push_back(g);
        }
        return result;
    }

    // ── PUMP req / op / sender for flush_fold ────────────────────

    struct tree_sched;

    namespace _flush_fold {

        struct req {
            tree_flush_request                                args;
            std::move_only_function<void(flush_fold_result&&)> cb;
        };

        struct op {
            constexpr static bool flush_fold_op = true;

            tree_sched*        sched;
            tree_flush_request args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*        sched;
            tree_flush_request args;

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

    }  // namespace _flush_fold

    // ── PUMP req / op / sender for flush_merge ───────────────────

    namespace _flush_merge {

        struct req {
            flush_merge_request                                 args;
            std::move_only_function<void(tree_flush_result&&)>  cb;
        };

        struct op {
            constexpr static bool flush_merge_op = true;

            tree_sched*         sched;
            flush_merge_request args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*         sched;
            flush_merge_request args;

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

    }  // namespace _flush_merge

    // ── tree_sched ────────────────────────────────────────────────

    struct tree_sched {
        static constexpr uint32_t kMaxFoldOpsPerAdvance  = 8;
        static constexpr uint32_t kMaxMergeOpsPerAdvance = 8;

        tree_state                                        state;
        pump::core::per_core::queue<_flush_fold::req*>    fold_q;
        pump::core::per_core::queue<_flush_merge::req*>   merge_q;

        explicit
        tree_sched(std::size_t depth = 256)
            : fold_q(depth), merge_q(depth) {}

        void
        schedule_fold(_flush_fold::req* r) {
            fold_q.try_enqueue(r);
        }

        void
        schedule_merge(_flush_merge::req* r) {
            merge_q.try_enqueue(r);
        }

        auto
        submit_flush_fold(tree_flush_request args) {
            return _flush_fold::sender{ this, std::move(args) };
        }

        auto
        submit_flush_merge(flush_merge_request args) {
            return _flush_merge::sender{ this, std::move(args) };
        }

        bool
        advance() {
            bool progress = false;

            // ── drain fold_q ──
            for (uint32_t i = 0; i < kMaxFoldOpsPerAdvance; ++i) {
                auto item = fold_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                // ── Stage 1: input validation ──
                if (r->args.base_guard == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance(fold)",
                        "tree_flush_request.base_guard is null");
                }
                if (r->args.base_guard->manifest == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance(fold)",
                        "tree_flush_request.base_guard->manifest is null");
                }

                // Empty sealed_gens → fast-path success.
                if (r->args.sealed_gens.empty()) {
                    flush_fold_result res{
                        .round_id      = flush_round_id{0},
                        .st            = flush_stage_status::ok,
                        .partitions    = {},
                        .base_manifest = r->args.base_guard->manifest.get(),
                    };
                    r->cb(std::move(res));
                    delete r;
                    progress = true;
                    continue;
                }

                // Per-gen validation.
                {
                    absl::flat_hash_set<uint64_t> seen_ids;
                    for (const auto& g : r->args.sealed_gens) {
                        if (g == nullptr)
                            core::panic_inconsistency(
                                "tree::tree_sched::advance(fold)",
                                "sealed_gens contains null gen");
                        if (g->st != core::memtable_gen::state::sealed)
                            core::panic_inconsistency(
                                "tree::tree_sched::advance(fold)",
                                "sealed_gens contains non-sealed gen");
                        if (!seen_ids.insert(g->gen_id).second)
                            core::panic_inconsistency(
                                "tree::tree_sched::advance(fold)",
                                "sealed_gens contains duplicate gen_id");
                    }
                }

                // ── Stage 2: allocate round_state and park ──
                auto rs = std::make_unique<flush_round_state>();
                rs->round_id          = flush_round_id{ state.next_round_id++ };
                rs->pinned_base_guard = std::move(r->args.base_guard);
                rs->pinned_gens       = std::move(r->args.sealed_gens);
                rs->recovery_safe_lsn = r->args.recovery_safe_lsn;

                // Gap 2 decision (flush_development_plan §2.2):
                // flushed_max_lsn = max(pinned_gens[*].max_lsn).
                // Computed once at round allocation so every consumer
                // (merge empty-path, merge success-path, future
                // coord-side update_flush_max_lsn) reads a single
                // authoritative value. For empty pinned_gens the max
                // is 0 (case 1); for fresh sealed gens the max is
                // their greatest max_lsn.
                rs->flushed_max_lsn = 0;
                for (auto& g : rs->pinned_gens) {
                    if (g->max_lsn > rs->flushed_max_lsn) {
                        rs->flushed_max_lsn = g->max_lsn;
                    }
                }

                auto round_id_v = rs->round_id.v;
                state.active_rounds.emplace(round_id_v, std::move(rs));
                auto& round = *state.active_rounds[round_id_v];

                // ── Stage 3: fold ──
                fold_pinned_gens(round);

                // ── Stage 4: empty workset fast path ──
                //
                // Round stays parked — merge handle will unpark it.
                if (round.workset.empty()) {
                    flush_fold_result res{
                        .round_id      = flush_round_id{round_id_v},
                        .st            = flush_stage_status::ok,
                        .partitions    = {},
                        .base_manifest = round.pinned_base_guard->manifest.get(),
                    };
                    r->cb(std::move(res));
                    delete r;
                    progress = true;
                    continue;
                }

                // ── Stage 5: build leaf-aligned partitions ──
                auto worker_count = core::registry::tree_worker_count();
                if (worker_count == 0)
                    core::panic_inconsistency(
                        "tree::tree_sched::advance(fold)",
                        "registry::tree_worker_count() is 0");

                auto partition_st = build_key_partitions(
                    round,
                    round.pinned_base_guard->manifest.get(),
                    worker_count);

                if (partition_st != flush_stage_status::ok) {
                    // Empty tree / out-of-range key — propagate as a
                    // fold result with empty partitions and the
                    // unsupported status. Round stays parked; merge
                    // handle will unpark and return the same status
                    // up to the caller.
                    round.st = partition_st;
                    flush_fold_result res{
                        .round_id      = flush_round_id{round_id_v},
                        .st            = partition_st,
                        .partitions    = {},
                        .base_manifest = round.pinned_base_guard->manifest.get(),
                    };
                    r->cb(std::move(res));
                    delete r;
                    progress = true;
                    continue;
                }

                // ── Stage 6: return fold result, round stays parked ──
                flush_fold_result res{
                    .round_id      = flush_round_id{round_id_v},
                    .st            = flush_stage_status::ok,
                    .partitions    = std::move(round.partitions),
                    .base_manifest = round.pinned_base_guard->manifest.get(),
                };
                r->cb(std::move(res));
                delete r;
                progress = true;
            }

            // ── drain merge_q ──
            for (uint32_t i = 0; i < kMaxMergeOpsPerAdvance; ++i) {
                auto item = merge_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                auto round_id_v = r->args.round_id.v;

                // Empty-gens fast path: no round was created.
                // Gap 2 case 1: flushed_max_lsn = max(∅) = 0.
                if (round_id_v == 0) {
                    tree_flush_result res{
                        .st              = flush_stage_status::ok,
                        .flushed_max_lsn = 0,
                    };
                    r->cb(std::move(res));
                    delete r;
                    progress = true;
                    continue;
                }

                auto it = state.active_rounds.find(round_id_v);
                if (it == state.active_rounds.end()) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance(merge)",
                        "round_id %lu not in active_rounds",
                        static_cast<unsigned long>(round_id_v));
                }
                auto& round = *it->second;

                // Defensive: each proposal carries its own round_id;
                // mismatch is a plumbing bug.
                for (auto& wp : r->args.worker_proposals) {
                    if (wp.round_id.v != round_id_v) {
                        core::panic_inconsistency(
                            "tree::tree_sched::advance(merge)",
                            "worker_proposal round_id %lu != merge "
                            "round_id %lu",
                            static_cast<unsigned long>(wp.round_id.v),
                            static_cast<unsigned long>(round_id_v));
                    }
                }

                // Determine result status (Gap 2 case dispatch):
                //
                //   - fold failed (round.st != ok): propagate the
                //     fold error (unsupported_shape_change for empty
                //     tree / out-of-range key).
                //   - case 2 (sealed gens non-empty but tables all
                //     empty → round.workset empty): legitimate no-op
                //     flush. Return ok. flushed_max_lsn = max(gens)
                //     which is 0 for fresh empty gens.
                //   - case 3 (non-empty workset): Phase 9 is what
                //     lands the real merge; Phase 7 returns
                //     unsupported_unimplemented. flushed_max_lsn is
                //     still the authoritative round value.
                flush_stage_status result_st;
                if (round.st != flush_stage_status::ok) {
                    result_st = round.st;
                } else if (round.workset.empty()) {
                    result_st = flush_stage_status::ok;
                } else {
                    result_st = flush_stage_status::unsupported_unimplemented;
                }

                auto gens_by_front =
                    build_flushed_gens_by_front(round.pinned_gens);
                auto flushed_max_lsn_v = round.flushed_max_lsn;
                state.active_rounds.erase(round_id_v);  // unpark

                tree_flush_result res{
                    .st                    = result_st,
                    .flushed_gens_by_front = std::move(gens_by_front),
                    .flushed_max_lsn       = flushed_max_lsn_v,
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

    // ── Deferred op::start definitions ───────────────────────────

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _flush_fold::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_fold(new _flush_fold::req{
            std::move(args),
            [ctx = ctx, scope = scope](flush_fold_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _flush_merge::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_merge(new _flush_merge::req{
            std::move(args),
            [ctx = ctx, scope = scope](tree_flush_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

}  // namespace apps::inconel::tree

// ── PUMP specializations ──────────────────────────────────────────

namespace pump::core {

    // flush_fold op_pusher
    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::flush_fold_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_flush_fold::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::flush_fold_result>{};
        }
    };

    // flush_merge op_pusher
    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::flush_merge_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_flush_merge::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::tree_flush_result>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_TREE_OWNER_SCHEDULER_HH
