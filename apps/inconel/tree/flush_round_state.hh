#ifndef APPS_INCONEL_TREE_FLUSH_ROUND_STATE_HH
#define APPS_INCONEL_TREE_FLUSH_ROUND_STATE_HH

// ── flush_round_state.hh ── round-owned flush carrier (step 023 §6) ──
//
// Owning side for everything a single tree-local flush round
// produces and consumes between stages. Every borrowed view inside
// the flush sub-stage carriers — `flush_mapping_req.groups`,
// `flush_worker_req.leaf_groups`, and any future intermediate span
// — points into this struct's owning vectors.
//
// Lifetime contract (Phase 3 only freezes the shape; Phase 4 starts
// using it):
//
//   1. `tree_sched` allocates `std::unique_ptr<flush_round_state>`
//      when a `tree_flush_request` enters its handle and the round
//      actually proceeds. Phase 3 no round proceeds — the handle
//      returns `unsupported_unimplemented` BEFORE allocating, see
//      023 D22 / §7 `handle_tree_flush` contract.
//   2. Phase 4 introduces `tree_state.active_rounds` (a
//      `flush_round_id → unique_ptr<flush_round_state>` map) so each
//      subsequent stage handle on `tree_sched` can find the live
//      round_state by `round_id` without re-routing through the
//      queue payload. Phase 3 does NOT pre-create that map.
//   3. Each `flush_mapping_req` / `flush_worker_req` carries a
//      `round_id` plus a borrowed span into this round_state. The
//      span is safe as long as `tree_sched` has not freed the
//      round_state, which it MUST NOT do until every fan-out has
//      fanned back in.
//   4. `finish_flush_round` (Phase 7) drops the round_state from
//      `active_rounds`, fires the request callback with a
//      `tree_flush_result` populated from the round_state, then
//      releases the unique_ptr.
//
// Ownership rules (023 §6 约束 1-4):
//
//   - Phase 3 does NOT instantiate this struct in any handle. The
//     `tree_sched::handle_tree_flush` value-path return path bypasses
//     allocation entirely; any attempt to "just allocate and free it
//     immediately" is explicitly out of scope per 023 D22.
//   - The round_state is the owning side — every intermediate array
//     uses an owning container (`std::vector` / `absl::InlinedVector`).
//     Views borrowed across per_core::queue hops must be
//     `std::span<const …>` over those vectors.
//   - No raw pointers; every strong reference (guard / gens) is a
//     `shared_ptr`. `tree_sched` holds the unique_ptr single-threaded,
//     so no internal lock is required.
//   - Field ordering below mirrors phase progression so reviewers
//     can see "which sub-block is whose responsibility".
//
// Phase 3 deliberately does NOT add per-stage methods (sorted
// workset builder, install_leaf_group_result, install_candidate_batch,
// etc.) — those belong to Phase 4/5/6/7 in their own step docs.

#include <cstdint>
#include <memory>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>

#include "../core/checkpoint_guard.hh"
#include "../core/memtable.hh"
#include "../core/retired_objects.hh"
#include "./flush_types.hh"

namespace apps::inconel::tree {

    struct flush_round_state {
        // ── identity + pinned inputs (populated at round allocation) ──

        // Round identity. Phase 4 will allocate these from
        // `tree_state.next_round_id`; Phase 3 leaves this default
        // because no round_state is ever created in Phase 3.
        flush_round_id round_id;

        // Strong reference to the base checkpoint_guard. This is
        // the ONLY pin that keeps the base tree_manifest alive for
        // the full round — the incoming `tree_flush_request.base_guard`
        // is moved into this field.
        std::shared_ptr<const core::checkpoint_guard> pinned_base_guard;

        // Strong references to every sealed gen included in this
        // round. kv_arena bytes for the `flush_key_group.key` views
        // and for every winner `value_handle.hot` are alive as long
        // as at least one shared_ptr here survives — see FF §3.3.
        absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>
            pinned_gens;

        // Snapshotted at round start so Phase 7 value reclaim can
        // compare tombstones / retired_value_ref.data_ver against
        // the stable recovery barrier (RSM §3.3 / FF §5.1).
        uint64_t recovery_safe_lsn = 0;

        // ── populated by Phase 4 (memtable fold / workset build) ──
        //
        // Sorted by `key` ascending. Backing storage for
        // `flush_mapping_req.groups`. Phase 3 leaves this empty.
        std::vector<flush_key_group> workset;

        // ── populated by Phase 4 (partition plan for Phase 5 lookup fanout) ──
        //
        // Stable as long as workset is not reallocated after
        // `build_key_partitions()` completes. Each partition's `groups`
        // span points into `workset` above.
        std::vector<flush_key_partition> partitions;

        // ── Phase 4 fold pushes losers directly into gen ──
        //
        // Losers are pushed into each gen's `loser_durable_refs`
        // during fold (not staged on round_state). fold_pinned_gens
        // calls `gen->loser_durable_refs.clear()` at the start for
        // idempotency, then pushes losers directly. If the round
        // fails, gen is not released → losers are not drained → no
        // harm. See review E-2 §7 for the full rationale.

        // ── populated by Phase 5 (lookup fanout merge) ──
        //
        // Sorted by `leaf_range_base` ascending after the per-shard
        // fan-in deduplication runs back on `tree_sched`. Backing
        // storage for `flush_worker_req.leaf_groups`. Phase 3 leaves
        // this empty.
        std::vector<flush_leaf_group> leaf_groups;

        // ── populated by Phase 6 (worker fanout) ──
        //
        // One entry per leaf group; concatenated back on `tree_sched`
        // after the worker fan-in reduces each candidate batch.
        // Phase 3 leaves this empty.
        std::vector<flush_leaf_candidate> candidates;

        // ── populated by Phase 7 (writer + finish_flush_round) ──
        //
        // These mirror every field in `tree_flush_result` so
        // `finish_flush_round` can hand them out without
        // restructuring — 023 review H-1 made this mirror promise
        // strict: if `tree_flush_result` gains a field, this
        // section must gain it too, otherwise Phase 7 is forced to
        // either re-carve the carrier or stash overflow state
        // somewhere off to the side.
        //
        // Phase 3 initializes everything to the same default the
        // Phase 3 stub result uses: `st = ok`, `new_manifest = nullptr`,
        // empty maps / vectors, `flushed_max_lsn = 0`. Phase 7 writer
        // and `finish_flush_round` will be the first code to mutate
        // them. The `st` default is intentionally `ok` rather than
        // `unsupported_unimplemented`: the round_state is an owning
        // live-round object, so the only legitimate reason for it to
        // exist at all is that the round is proceeding. The
        // Phase 3 stub never allocates a round_state (023 D22), so
        // the default is only observed by Phase 4+ construction sites
        // that then fold / map / candidate-build / write into place.
        flush_stage_status                         st = flush_stage_status::ok;
        std::shared_ptr<const core::tree_manifest> new_manifest;
        core::retired_objects                      retired;
        absl::flat_hash_map<
            uint32_t,
            absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>>
            flushed_gens_by_front;
        absl::InlinedVector<core::retired_value_ref, 64>
            memtable_losers;
        uint64_t flushed_max_lsn = 0;
    };

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_FLUSH_ROUND_STATE_HH
