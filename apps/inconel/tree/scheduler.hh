#ifndef APPS_INCONEL_TREE_SCHEDULER_HH
#define APPS_INCONEL_TREE_SCHEDULER_HH

// ── Umbrella shim (step 022 §9, D12) ──
//
// Phase 2 split the former monolithic `tree/scheduler.hh` into:
//
//   - `tree/lookup_scheduler.hh` — `tree_lookup_sched_base` /
//     `tree_lookup_sched<Cache>` + their PUMP specializations
//   - `tree/worker_scheduler.hh` — `tree_worker_sched` +
//     `build_leaf_candidates` sender + its PUMP specializations
//
// This file exists only to aggregate both sub-headers so any existing
// include of `tree/scheduler.hh` keeps compiling. External modules
// should prefer `tree/sender.hh` as the single facade entry point —
// see `code_modules.md` §L2.tree "tree 对外只暴露 sender.hh".
//
// Do not add new content here. New tree-side types, ops, senders, or
// PUMP specializations belong in the specific sub-header that owns
// their runtime role.

#include "./lookup_scheduler.hh"
#include "./worker_scheduler.hh"

#endif //APPS_INCONEL_TREE_SCHEDULER_HH
