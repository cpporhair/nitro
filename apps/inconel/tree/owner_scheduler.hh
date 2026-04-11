#ifndef APPS_INCONEL_TREE_OWNER_SCHEDULER_HH
#define APPS_INCONEL_TREE_OWNER_SCHEDULER_HH

// ── tree_sched — tree-domain flush round owner (step 023 §7) ──
//
// Phase 3 (step 023 G4/G5) lands `tree_sched` as the singleton owner
// of the tree-local flush round state machine (RSM §1, OV §1.7). The
// scheduler holds the `tree_state` aggregate defined by RSM §4.1 and
// a single ingress queue — `flush_q` — for `tree_flush_request`
// deliveries.
//
// Phase 3 intentionally does NOT implement any flush algorithm. The
// `handle_tree_flush` path is stubbed per 023 D22:
//
//   1. `base_guard` must be non-null.
//   2. `sealed_gens` must be non-empty. FF §2.3 reserves the empty-
//      round fast path for Phase 4; Phase 3 keeps it strict so call
//      sites cannot silently rely on the stub returning `ok`.
//   3. If either check fails, `tree_sched::advance()` calls
//      `core::panic_inconsistency` (matching worker_scheduler.hh
//      M-4 fail-fast convention).
//   4. Otherwise, the handle fires the callback with a fully
//      populated `tree_flush_result { st = unsupported_unimplemented,
//      new_manifest = nullptr, ... }` via the value path. No
//      `flush_round_state` is allocated, no `tree_state` field is
//      mutated, no LSN cursor advances. This is deliberate: a
//      prototype that allocates+frees a round_state without Phase 4-7
//      following it up would exercise the allocation path but not
//      validate any downstream stage, muddling the Phase 3/4 seam.
//
// `reclaim_q` exists on `tree_state` for structural fidelity to RSM
// §4.1, but Phase 3 has no producer and no consumer: `advance()`
// deliberately does NOT drain it (023 D21). The real producer is
// `~checkpoint_guard()` posting a reclaim_task during
// frontier_switch; the real consumer is the Phase 8+ TRIM dispatch.
// Phase 3 uses `struct reclaim_task;` as a forward declaration — the
// queue only holds `reclaim_task*`, so the full type is not needed
// here.
//
// File split (022 D12, §9; carried through Phase 3 023 D23): the
// op/sender/op_pusher/compute_sender_type specializations live in
// this header alongside the scheduler definition. `tree/scheduler.hh`
// aggregates the tree-domain sub-headers; `tree/sender.hh` exports
// the outward-facing `tree::tree_flush(tree_sched*, tree_flush_request)`
// facade.

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

#include "../core/panic.hh"
#include "../format/types.hh"
#include "./flush_types.hh"

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
    };

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

        // Phase 3 advance(): drain at most kMaxFlushOpsPerAdvance
        // flush requests per tick. reclaim_q is intentionally NOT
        // drained (023 D21, §7).
        bool
        advance() {
            bool progress = false;
            for (uint32_t i = 0; i < kMaxFlushOpsPerAdvance; ++i) {
                auto item = flush_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                // Carrier-contract validation. All three checks match
                // the fail-fast convention the worker already uses
                // (worker_scheduler.hh M-4 in step 022): a malformed
                // request is a caller-side bug and must surface near
                // the producer, not be masked as
                // `unsupported_unimplemented`. Phase 4 will relax
                // these to structured statuses once real round
                // handling exists and empty-round is a legitimate
                // fast path; for Phase 3 we keep it strict.
                //
                // 023 review M-1: the middle check — "outer guard
                // non-null but inner manifest is null" — is not a
                // "Phase 4 problem", it is a Phase 3 carrier-contract
                // violation right now. Letting it slip through the
                // `unsupported_unimplemented` stub would disguise a
                // real caller bug as "just not implemented yet", so
                // we panic alongside the other two checks.
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
                if (r->args.sealed_gens.empty()) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance",
                        "tree_flush_request.sealed_gens is empty");
                }

                // Phase 3 stub: no `flush_round_state` allocation,
                // no `tree_state` mutation, no LSN cursor movement.
                // The value-path return shape mirrors the carrier
                // contract Phase 4-7 will progressively populate;
                // for now every field is its default.
                tree_flush_result res{
                    .st                    = flush_stage_status::unsupported_unimplemented,
                    .new_manifest          = nullptr,
                    .retired               = {},
                    .flushed_gens_by_front = {},
                    .memtable_losers       = {},
                    .flushed_max_lsn       = 0,
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
