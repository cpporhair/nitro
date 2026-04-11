#ifndef APPS_INCONEL_TREE_FLUSH_TYPES_HH
#define APPS_INCONEL_TREE_FLUSH_TYPES_HH

// ── Flush shared shell types (step 022 / Phase 2 G4) ──
//
// Minimal cross-scheduler type shells for the tree-local flush
// pipeline. Phase 2 only freezes the top-level identity and the
// stable payload that every later phase will certainly need; any
// field tied to round-owned-array ownership — including
// `flush_key_group_ref`, `round_index`, or similar indirection
// layers — is deliberately absent. Those belong to Phase 3 once the
// owning side of the round state has been fixed.
//
// Fail-fast convention (022 D11, §10): every flush result struct
// carries a `flush_stage_status`. Unimplemented sender surfaces
// return the result with `st = unsupported_unimplemented` via the
// normal value path; we do not throw for unimplemented phases.
//
// Consumers:
//   - tree/worker_scheduler.hh consumes `flush_worker_req` and
//     produces `flush_candidate_batch`.
//   - Phase 5 will add a `tree/lookup_scheduler.hh`-side sender
//     consuming `flush_lookup_req` and producing
//     `flush_leaf_group_result`. In Phase 2 those types exist but
//     no sender consumes them yet.

#include <cstdint>

#include <absl/container/inlined_vector.h>

#include "../core/tree_manifest.hh"
#include "../format/types.hh"

namespace apps::inconel::tree {

    using format::paddr;

    // ── round identity ───────────────────────────────────────────

    struct flush_round_id {
        uint64_t v = 0;
    };

    enum class flush_stage_status : uint8_t {
        ok,
        unsupported_unimplemented,
        unsupported_shape_change,
    };

    // ── leaf mapping stage (Phase 5 consumer) ────────────────────

    struct flush_lookup_req {
        flush_round_id             round_id;
        uint32_t                   read_domain_index;
        const core::tree_manifest* base_manifest;
    };

    struct flush_leaf_group {
        paddr leaf_range_base;
        paddr old_slot_paddr;
    };

    struct flush_leaf_group_result {
        flush_round_id                            round_id;
        uint32_t                                  read_domain_index;
        flush_stage_status                        st;
        absl::InlinedVector<flush_leaf_group, 8>  leaf_groups;
    };

    // ── candidate materialization stage (Phase 6 consumer; Phase 2
    //    already wires the worker skeleton so the sender exists but
    //    returns unsupported_unimplemented) ────────────────────────
    //
    // Phase 2 intentionally does NOT carry any leaf-group payload
    // inside `flush_worker_req`. The earlier draft held a
    // `std::span<const flush_leaf_group>` borrowed view here, but a
    // borrowed view crossing an async `per_core::queue` is a
    // lifetime trap: the sender copies the req into its own op,
    // enqueues it, and the worker consumes it on another scheduler
    // tick — the caller's backing storage may already be gone.
    // Round-owned-array ownership belongs to Phase 3 (see 022 D10,
    // review round 2 M-3). When that step lands, the real payload
    // will be added alongside an explicit owner contract; until
    // then Phase 2 only carries the fields whose ownership is
    // already unambiguous.

    struct flush_worker_req {
        flush_round_id             round_id;
        uint32_t                   read_domain_index;
        const core::tree_manifest* base_manifest;
        uint64_t                   recovery_safe_lsn;
    };

    struct flush_leaf_candidate {
        paddr              leaf_range_base;
        paddr              old_slot_paddr;
        flush_stage_status st;
    };

    struct flush_candidate_batch {
        flush_round_id                               round_id;
        uint32_t                                     read_domain_index;
        flush_stage_status                           st;
        absl::InlinedVector<flush_leaf_candidate, 8> leaves;
    };

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_FLUSH_TYPES_HH
