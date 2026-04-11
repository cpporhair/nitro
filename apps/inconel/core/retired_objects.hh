#ifndef APPS_INCONEL_CORE_RETIRED_OBJECTS_HH
#define APPS_INCONEL_CORE_RETIRED_OBJECTS_HH

// ── retired_objects.hh ── flush-round retire bag (step 023 §4, D28) ──
//
// Aggregate of everything a single tree-local flush round retires
// (RSM §4.2, FF §5). Phase 3 only freezes the layout; producers
// (the Phase 7 writer) and consumers (the frontier_switch step's
// retire-list plumbing) both land later.
//
// Two use sites known up front:
//
//   - `tree_flush_result` (step 023 §5) holds a `retired_objects` by
//     value; the Phase 3 stub always sets it to `{}`, and Phase 7
//     writer replaces that with the real bag when `finish_flush_round`
//     lands.
//   - The frontier_switch step will move the bag onto the OLD
//     checkpoint_guard's retire list (FF §4.1 / §5.1 / §5.2).
//
// 023 D28 places this header under `core/` (not `tree/`) on purpose:
// `checkpoint_guard` lives in `core/` and will eventually hold a
// `retired_objects` member; `tree_flush_result` also lives outside
// the tree-local flush pipeline scheduler. Keeping the aggregate in
// `core/` avoids a cyclic include story when frontier_switch
// attaches it to checkpoint_guard.
//
// `retired_value_ref` is NOT re-defined here — 023 D29 requires
// reusing the Phase 1 definition from `core/memtable.hh`. That
// header already documents the {value_ref, data_ver} shape used by
// reclamation decisions against `recovery_safe_lsn`.

#include <cstdint>

#include <absl/container/inlined_vector.h>

#include "../format/types.hh"
#include "./memtable.hh"

namespace apps::inconel::core {

    struct retired_objects {
        // Old slot paddrs whose shadow-range home can be reused once
        // the OLD checkpoint_guard drops. Written by the Phase 7
        // writer as leaves rewrite or move between shadow slots.
        absl::InlinedVector<format::paddr, 32>     old_slots;

        // Old shadow ranges retired wholesale (e.g. when a range
        // consolidates away). Written by Phase 7 / Phase 8 writer.
        absl::InlinedVector<format::range_ref, 8>  old_ranges;

        // Value references (durable + data_ver) that the FOLD stage
        // or the writer determined to be losers / unreferenced tree
        // values for this round. Retire-time value reclamation in
        // the frontier_switch step will compare every entry's
        // `data_ver` against `recovery_safe_lsn` before actually
        // freeing the backing value page.
        absl::InlinedVector<retired_value_ref, 64> old_tree_values;
    };

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_RETIRED_OBJECTS_HH
