#ifndef APPS_INCONEL_CORE_CHECKPOINT_GUARD_HH
#define APPS_INCONEL_CORE_CHECKPOINT_GUARD_HH

// ── checkpoint_guard.hh ── minimal reader guard (step 023 §3, D9/D27) ──
//
// OV §5.2 / RSM §4.6 give `checkpoint_guard` two responsibilities:
//
//   1. Pin the immutable `tree_manifest` snapshot used by readers
//      that captured this guard.
//   2. Carry the `retired_objects` bag whose destructor posts a
//      `reclaim_task` to `tree_sched.reclaim_q`.
//
// Phase 3 (step 023) only needs (1). The round-owner state machine
// requires `tree_flush_request.base_guard` to outlive the entire
// round and a future `tree_flush_result.new_manifest` to be
// wrappable into a fresh guard by the frontier_switch step — both
// only depend on pinning a manifest. The `retired` member, the
// destructor that posts a `reclaim_task`, and the rest of the
// reclaim pipeline all belong to the frontier_switch step that
// follows Phase 3; splitting them out now would force this step to
// also land `reclaim_task`, `data_area_heads`, and the TRIM
// dispatch — all of which are expressly outside 023's scope.
//
// When the frontier_switch step lands, it MUST extend this struct
// **in place** rather than introducing a parallel
// `checkpoint_guard_with_retired` type. The single shared type is
// what OV §5.2 mandates.
//
// Lifetime:
//   - `manifest` is a `shared_ptr<const tree_manifest>`, so the
//     guard owns a strong reference to the snapshot.
//   - Readers hold the whole guard via
//     `std::shared_ptr<const checkpoint_guard>`; copying a reader's
//     PRS/read_handle bumps the guard's refcount, which transitively
//     keeps the manifest alive.

#include <memory>

#include "./tree_manifest.hh"

namespace apps::inconel::core {

    struct checkpoint_guard {
        // Pinned manifest snapshot. Non-null is a caller
        // responsibility at construction time; Phase 3 does not add
        // factory helpers, so each production construction site
        // validates its own pointer before wrapping it in a guard.
        std::shared_ptr<const tree_manifest> manifest;
    };

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_CHECKPOINT_GUARD_HH
