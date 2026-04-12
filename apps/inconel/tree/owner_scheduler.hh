#ifndef APPS_INCONEL_TREE_OWNER_SCHEDULER_HH
#define APPS_INCONEL_TREE_OWNER_SCHEDULER_HH

// ── tree_sched — tree-domain flush round owner ──
//
// Phase 3 (step 023) landed `tree_sched` as the singleton owner of
// the tree-local flush round state machine (RSM §1, OV §1.7).
//
// Phase 4 (step 024) implements the first real algorithm:
//
//   - `advance()` validates inputs, allocates `flush_round_state`,
//     parks it in `tree_state.active_rounds`, runs
//     `fold_pinned_gens()` + `build_key_partitions()`, then unparks.
//   - Empty sealed_gens → ok (D8); empty workset → ok with
//     `new_manifest = base_manifest` (D19); non-empty workset →
//     `unsupported_unimplemented` (Phase 5-7 downstream pending).
//   - Losers pushed directly into each gen's `loser_durable_refs`
//     during fold; `clear()` at fold start ensures idempotency.
//   - Fail-fast: null gen / non-sealed / dup gen_id / null guard /
//     null manifest / zero lookup count → `panic_inconsistency`.
//
// `reclaim_q` exists on `tree_state` for structural fidelity to RSM
// §4.1, but has no producer and no consumer yet: `advance()` does
// NOT drain it (023 D21).
//
// File split (022 D12, §9): op/sender/op_pusher/compute_sender_type
// specializations live here alongside the scheduler.
// `tree/scheduler.hh` aggregates sub-headers; `tree/sender.hh`
// exports the outward-facing facade.

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
// The full definition lives in core/registry.hh, which every
// translation unit that instantiates advance() also includes.
namespace apps::inconel::core::registry {
    inline uint32_t tree_lookup_count();
}

namespace apps::inconel::tree {

    // ── reclaim_task forward declaration (023 §7, 约束 4) ─────────
    //
    // Real `reclaim_task` definition lands with the frontier_switch
    // step. Phase 3 only needs the queue element type to be a
    // pointer, and a forward declaration satisfies that — the
    // queue implementation stores raw pointers and never dereferences
    // them, so the incomplete type is fine here.
    struct reclaim_task;

    // ── tree_allocator (Phase 3 placeholder, 023 D20) ─────────────
    //
    // RSM §4.1 / §4.4 freeze the allocator shape. Phase 3 only needs
    // the field name to exist on `tree_state` so the Phase 7 root-
    // stable writer can wire `allocate / recycle / data_area_heads`
    // in place without renaming or moving fields. None of the
    // members are usable before Phase 7: any call site that tries
    // to use them earlier is a phase-creep bug and should be caught
    // at code review. Intentionally has no methods at all.

    struct tree_allocator {
        format::paddr                       head{};
        std::vector<format::range_ref>      free_ranges{};
    };

    // ── tree_state (RSM §4.1) ─────────────────────────────────────
    //
    // Owner-local state for `tree_sched`. Field names mirror RSM §4.1
    // one-for-one (023 D19). Phase 3 constructs every field; only
    // future phases mutate them — the LSN cursors stay at 0, and
    // `reclaim_q` has neither a producer nor a consumer per 023 D21.
    //
    // Phase 4 will add two more fields here:
    //   - `absl::flat_hash_map<flush_round_id,
    //                          std::unique_ptr<flush_round_state>> active_rounds;`
    //   - `uint64_t next_round_id = 1;`
    // 023 D30 explicitly says NOT to pre-create them in Phase 3 —
    // the diff should show up in the fold step so the reviewer can
    // see where `active_rounds` starts being used.

    struct tree_state {
        tree_allocator alloc;
        uint64_t       flush_max_lsn       = 0;
        uint64_t       superblock_safe_lsn = 0;
        uint64_t       recovery_safe_lsn   = 0;

        // Structural placeholder. No producer, no consumer in
        // Phase 3 — see 023 D21 / §7. `advance()` deliberately does
        // NOT drain this queue.
        pump::core::per_core::queue<reclaim_task*> reclaim_q{256};

        // ── Phase 4: active flush rounds (D1/D2) ─────────────────
        //
        // Key is `flush_round_id.v` (uint64_t).
        // `flush_round_id` has no `AbslHashValue`; `.v` is used
        // directly as hash key.
        //
        // Phase 4 round lifecycle: park at fold start, unpark at fold
        // end (same advance tick). Phase 5+ will extend the window
        // across multiple advance ticks when async fanout is introduced.
        absl::flat_hash_map<uint64_t, std::unique_ptr<flush_round_state>>
            active_rounds;
        uint64_t next_round_id = 1;
    };

    // ── build_flushed_gens_by_front (D18) ──────────────────────────
    //
    // Groups pinned gens by their front_owner_index. All ok paths
    // must call this so outer flow can release_gens correctly.
    // front_owner_index == UINT32_MAX is the invalid sentinel (D17)
    // and triggers panic_inconsistency.

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

    // ── PUMP req / op / sender for tree_flush ────────────────────

    struct tree_sched;

    namespace _tree_flush {

        struct req {
            tree_flush_request args;
            std::move_only_function<void(tree_flush_result&&)> cb;
        };

        struct op {
            constexpr static bool tree_flush_op = true;

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

    }  // namespace _tree_flush

    // ── tree_sched ────────────────────────────────────────────────
    //
    // Singleton flush owner. Holds `tree_state` plus a single ingress
    // queue for `tree_flush_request`. In Phase 3 the only ingress
    // that ever drains is `flush_q`; `reclaim_q` is a structural
    // placeholder only.
    //
    // The per-advance drain cap is deliberately small (8) compared
    // with the worker's 64: flush requests are owner-level round
    // submissions, expected cadence on the order of one every few
    // tens of milliseconds, and each request eventually fans out
    // across the whole tree domain via the Phase 4-7 pipeline. A
    // tight cap on the singleton prevents any starvation of
    // co-resident schedulers on cores[0].

    struct tree_sched {
        static constexpr uint32_t kMaxFlushOpsPerAdvance = 8;

        tree_state                                       state;
        pump::core::per_core::queue<_tree_flush::req*>   flush_q;

        explicit
        tree_sched(std::size_t flush_q_depth = 256)
            : flush_q(flush_q_depth) {}

        void
        schedule_flush(_tree_flush::req* r) {
            flush_q.try_enqueue(r);
        }

        // Sender factory; callers reach this through
        // `tree::tree_flush(tree_sched*, tree_flush_request)`
        // declared in `tree/sender.hh`.
        auto
        submit_flush(tree_flush_request args) {
            return _tree_flush::sender{ this, std::move(args) };
        }

        // Phase 4 advance(): drain at most kMaxFlushOpsPerAdvance
        // flush requests per tick. reclaim_q is intentionally NOT
        // drained (023 D21, §7).
        //
        // Phase 4 flow: validate → park round_state → fold →
        // partition → unpark → cb. Full algorithm in plan §5.
        bool
        advance() {
            bool progress = false;
            for (uint32_t i = 0; i < kMaxFlushOpsPerAdvance; ++i) {
                auto item = flush_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                // ── Stage 1: input validation ──

                if (r->args.base_guard == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance",
                        "tree_flush_request.base_guard is null");
                }
                if (r->args.base_guard->manifest == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance",
                        "tree_flush_request.base_guard->manifest is null");
                }

                // D8: sealed_gens.empty() downgraded from Phase 3
                // panic to fast-path success. Empty sealed_gens means
                // no gen to release, flushed_gens_by_front is empty.
                // M-1: populate new_manifest = base manifest so all ok
                // paths uniformly guarantee new_manifest != nullptr.
                if (r->args.sealed_gens.empty()) {
                    tree_flush_result res{
                        .st                    = flush_stage_status::ok,
                        .new_manifest          = r->args.base_guard->manifest,
                        .flushed_gens_by_front = {},
                        .flushed_max_lsn       = 0,
                    };
                    r->cb(std::move(res));
                    delete r;
                    progress = true;
                    continue;
                }

                // D9/D10/D11: per-gen validation.
                {
                    absl::flat_hash_set<uint64_t> seen_ids;
                    for (const auto& g : r->args.sealed_gens) {
                        if (g == nullptr)
                            core::panic_inconsistency(
                                "tree::tree_sched::advance",
                                "sealed_gens contains null gen");
                        if (g->st != core::memtable_gen::state::sealed)
                            core::panic_inconsistency(
                                "tree::tree_sched::advance",
                                "sealed_gens contains non-sealed gen");
                        if (!seen_ids.insert(g->gen_id).second)
                            core::panic_inconsistency(
                                "tree::tree_sched::advance",
                                "sealed_gens contains duplicate gen_id");
                    }
                }

                // ── Stage 2: allocate round_state and park ──

                auto rs = std::make_unique<flush_round_state>();
                rs->round_id = flush_round_id{ state.next_round_id++ };
                rs->pinned_base_guard = std::move(r->args.base_guard);
                rs->pinned_gens = std::move(r->args.sealed_gens);
                rs->recovery_safe_lsn = r->args.recovery_safe_lsn;

                auto round_id_v = rs->round_id.v;
                state.active_rounds.emplace(round_id_v, std::move(rs));
                auto& round = *state.active_rounds[round_id_v];

                // ── Stage 3: fold (side effects round-local, D16) ──

                fold_pinned_gens(round);

                // ── Stage 4: empty workset fast path ──
                //
                // Non-empty sealed_gens but all gen tables are empty.
                // Empty gens are allowed to seal (RSM §479). ok path
                // must populate flushed_gens_by_front (D18) so outer
                // flow can release these empty gens.

                if (round.workset.empty()) {
                    auto gens_by_front =
                        build_flushed_gens_by_front(round.pinned_gens);
                    // D19: empty delta → new_manifest = base manifest.
                    auto base_manifest = round.pinned_base_guard->manifest;
                    state.active_rounds.erase(round_id_v);  // unpark

                    tree_flush_result res{
                        .st                    = flush_stage_status::ok,
                        .new_manifest          = std::move(base_manifest),
                        .flushed_gens_by_front = std::move(gens_by_front),
                        .flushed_max_lsn       = 0,
                    };
                    r->cb(std::move(res));
                    delete r;
                    progress = true;
                    continue;
                }

                // ── Stage 5: build partitions ──

                auto lookup_count = core::registry::tree_lookup_count();
                if (lookup_count == 0)
                    core::panic_inconsistency(
                        "tree::tree_sched::advance",
                        "registry::tree_lookup_count() is 0");
                build_key_partitions(round, lookup_count);

                // ── Stage 6: unpark, return unsupported_unimplemented ──
                //
                // Phase 5-7 downstream not implemented. Losers were
                // already pushed into each gen's loser_durable_refs
                // during fold, but gen is not released on the
                // unsupported path → losers are not drained → no harm.
                // No flushed_gens_by_front needed on unsupported path.

                state.active_rounds.erase(round_id_v);  // unpark

                tree_flush_result res{
                    .st              = flush_stage_status::unsupported_unimplemented,
                    .flushed_max_lsn = 0,
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

    // ── Deferred op::start definition (needs full tree_sched) ─────

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _tree_flush::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_flush(new _tree_flush::req{
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

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::tree_flush_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_tree_flush::sender> {
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
