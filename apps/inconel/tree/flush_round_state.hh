#ifndef APPS_INCONEL_TREE_FLUSH_ROUND_STATE_HH
#define APPS_INCONEL_TREE_FLUSH_ROUND_STATE_HH

// ── flush_round_state.hh ── round-owned flush carrier (step 023 §6) ──
//
// Owning side for everything a single tree-local flush round
// produces and consumes between stages. Every borrowed view inside
// the flush sub-stage carriers — `flush_worker_req.key_groups` and
// `flush_key_partition.groups` — points into this struct's owning
// vectors.
//
// Lifetime contract:
//
//   1. `tree_sched` allocates `std::unique_ptr<flush_round_state>`
//      when a `tree_flush_request` enters its handle and the round
//      actually proceeds.
//   2. `tree_state.active_rounds` keeps the round_state alive
//      across the async worker fanout (`_flush_fold` parks it,
//      `_flush_merge` unparks it).
//   3. `flush_worker_req` carries a borrowed span into this
//      round_state. The span is safe as long as `tree_sched` has
//      not freed the round_state, which it MUST NOT do until every
//      fan-out has fanned back in.
//   4. `finish_flush_round` (Phase 9) drops the round_state from
//      `active_rounds`, fires the request callback with a
//      `tree_flush_result`, then releases the unique_ptr.
//
// Worker output (`worker_leaf_chain`) flows through the sender fan-in
// directly into the owner merge loop payload; round_state only owns
// the folded workset / partition plan plus the committed result.

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

        flush_round_id round_id;

        // Strong reference to the base checkpoint_guard. This is
        // the ONLY pin that keeps the base tree_manifest alive for
        // the full round.
        std::shared_ptr<const core::checkpoint_guard> pinned_base_guard;

        // Strong references to every sealed gen included in this
        // round. kv_arena bytes for the `flush_key_group.key` views
        // are alive as long as at least one shared_ptr here survives.
        // Value body residency belongs to value_alloc_sched; the
        // legacy value_handle.hot carrier is not part of the target
        // contract (INC-055).
        absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>
            pinned_gens;

        // Snapshotted at round start so worker tombstone GC and
        // Phase 9 value reclaim can compare against the stable
        // recovery barrier (RSM §3.3 / FF §5.1).
        uint64_t recovery_safe_lsn = 0;

        // ── populated by Phase 4 (memtable fold / workset build) ──
        //
        // Folded winners in key order. Partitioning no longer rewrites
        // this vector; `partitions` below points at consecutive same-
        // read_domain runs inside this sorted workset.
        std::vector<flush_key_group> workset;

        // ── populated by Phase 4 (partition plan for worker fanout) ──
        //
        // Stable as long as workset is not reallocated after
        // `build_key_partitions()` completes. Each partition's
        // `groups` span points into a consecutive run in `workset`
        // above. The same read_domain may appear in multiple runs.
        std::vector<flush_key_partition> partitions;

        // ── Phase 4 fold pushes losers directly into gen ──
        //
        // Memtable-only losers (Gap 1A) are pushed into each gen's
        // `loser_durable_refs` during fold — never staged on
        // round_state. fold_pinned_gens calls
        // `gen->loser_durable_refs.clear()` at the start for
        // idempotency.

        // ── populated by Phase 9 (writer + finish_flush_round) ──
        //
        // These mirror every field in `tree_flush_result` so
        // `finish_flush_round` can hand them out without
        // restructuring. The `st` default is intentionally `ok`:
        // round_state is an owning live-round object, so the only
        // legitimate reason for it to exist is that the round is
        // proceeding.
        //
        // Gap 1A (flush_development_plan §2.1.1): memtable-only
        // losers live solely on `core::memtable_gen.loser_durable_refs`;
        // they are never staged on round_state. `flush_round_state`
        // intentionally has no `memtable_losers` field to prevent
        // future code from re-wiring the wrong lifetime.
        //
        // Gap 2 (flush_development_plan §2.2): `flushed_max_lsn` is
        // `max(pinned_gens[*].max_lsn)`, computed once at round
        // allocation in `_flush_fold`. Every consumer (merge empty
        // path, merge success path, future coord update) reads a
        // single authoritative value.
        flush_stage_status                         st = flush_stage_status::ok;
        std::shared_ptr<const core::tree_manifest> new_manifest;
        core::retired_objects                      retired;
        absl::flat_hash_map<
            uint32_t,
            absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>>
            flushed_gens_by_front;
        uint64_t flushed_max_lsn = 0;
    };

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_FLUSH_ROUND_STATE_HH
