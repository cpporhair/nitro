#ifndef APPS_INCONEL_CORE_TREE_GEOMETRY_HH
#define APPS_INCONEL_CORE_TREE_GEOMETRY_HH

// ── tree_geometry — runtime-visible single source for the ODF §4.1
//    tree parameters the flush / lookup / manifest paths actually
//    consume.
//
// Step 022 (Phase 2 G1-G2) fixes the bootstrap source at
// `format::kBootstrapFormatProfile` and introduces this header so every
// tree-side consumer reads the same three numbers from the same runtime
// carrier, instead of sprinkling naked `tree_page_size / lba_size /
// shadow_slots_per_range` triples across manifest / allocator / writer.
//
// The carrier is intentionally POD and by-value cheap. `tree_manifest`
// holds a `const tree_geometry*` (RSM §4.5 / FF §3 / step 022 §2);
// future `tree_sched` / `tree_worker_sched` / writer code copies / refers
// to the same `tree_geometry` instance owned by the runtime builder.
//
// Rules enforced elsewhere (`profile_is_self_consistent` in
// format/format_profile.hh and `runtime::validate_build_inputs` in
// runtime/builder.hh):
//
//   1. lba_size > 0
//   2. tree_page_size > 0 and an integral multiple of lba_size
//   3. shadow_slots_per_range >= 1
//
// This header does **not** bake in any specific numeric value for
// `tree_page_size` or `shadow_slots_per_range`. Any LBA-aligned page
// size and any non-zero shadow slot count must work end to end.

#include <cstdint>

#include "../format/types.hh"

namespace apps::inconel::core {

    using format::paddr;
    using format::range_ref;

    struct tree_geometry {
        uint32_t lba_size;
        uint32_t tree_page_size;
        uint32_t shadow_slots_per_range;

        // Value equality (review round 2 / H-1). Consumers that need
        // to check "same geometry" must compare by value — never by
        // pointer identity — so that test fixtures, recovery, and the
        // bootstrap builder can each own their own `tree_geometry`
        // instance as long as the three numeric fields agree.
        bool operator==(const tree_geometry&) const = default;

        // ── derived quantities ──
        //
        // All helpers are pure arithmetic on the three fields above.
        // They exist so callers never recompute `tree_page_size /
        // lba_size` or `page_lbas * shadow_slots_per_range` locally and
        // forget one of the invariants when the numbers change.

        constexpr uint32_t
        page_lbas() const noexcept {
            return tree_page_size / lba_size;
        }

        constexpr uint64_t
        range_lbas() const noexcept {
            return static_cast<uint64_t>(page_lbas())
                 * static_cast<uint64_t>(shadow_slots_per_range);
        }

        // ── addressing helpers ──
        //
        // `range_ref_from_base(base)` returns the metadata shell for the
        // whole shadow range anchored at `base` — i.e. base plus
        // `shadow_slots_per_range` slots. Name chosen over the earlier
        // `whole_range()` to stay unambiguous if a future path needs to
        // talk about partial ranges.
        //
        // `slot_paddr(range_base, slot_index)` resolves the i-th slot
        // within a shadow range to its absolute `paddr`. Callers must
        // already know `slot_index < shadow_slots_per_range`;
        // `tree_manifest::resolve()` is the canonical source of the
        // currently-live slot index for any range.

        constexpr range_ref
        range_ref_from_base(paddr base) const noexcept {
            return range_ref{ .base = base, .slot_count = shadow_slots_per_range };
        }

        constexpr paddr
        slot_paddr(paddr range_base, uint32_t slot_index) const noexcept {
            return paddr{
                range_base.device_id,
                range_base.lba + static_cast<uint64_t>(slot_index) * page_lbas(),
            };
        }
    };

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_TREE_GEOMETRY_HH
