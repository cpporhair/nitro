#ifndef APPS_INCONEL_CORE_TREE_MANIFEST_HH
#define APPS_INCONEL_CORE_TREE_MANIFEST_HH

// ── Why this file lives in core/ and not tree/ ──
//
// `tree_manifest` is an immutable runtime snapshot of the on-disk B+tree
// layout: root slot, slot_map for shadow-range resolution, page geometry.
// It is NOT a tree-writer-local helper, NOT a page-format helper, and NOT
// internal state of the tree mutator. It is reader-visible and shared
// across modules — tree_lookup, the runtime registry, and (eventually)
// checkpoint/reclaim flows all hold and reason about manifest snapshots.
// Putting it under tree/ would imply ownership by the tree-writer side,
// which would be misleading. core/ is the right home for cross-module
// immutable runtime snapshots, so the file stays here.

#include <absl/container/flat_hash_map.h>

#include "../format/types.hh"
#include "./panic.hh"
#include "./tree_geometry.hh"

namespace apps::inconel::core {

    using format::paddr;

    // ── tree_manifest ─────────────────────────────────────────────
    //
    // Immutable runtime snapshot of the B+tree on-disk layout (RSM §4.5).
    // Step 022 (Phase 2 M-5 / §2) changed the geometry access from two
    // inline fields (`tree_page_size / lba_size`) to a single
    // `const tree_geometry*`, so a reader-side snapshot no longer has
    // to carry writer-only parameters like `shadow_slots_per_range` by
    // value. The pointee is owned by the runtime builder and outlives
    // every manifest snapshot it produces, so there is no lifetime
    // concern for readers.

    struct tree_manifest {
        paddr root_slot;
        absl::flat_hash_map<paddr, uint32_t> slot_map;
        const tree_geometry* geom;

        bool
        has_root() const {
            return root_slot.lba != 0;
        }

        paddr
        resolve(paddr range_base) const {
            auto it = slot_map.find(range_base);
            if (it == slot_map.end()) {
                // Internal-node child_base must always resolve via the
                // manifest — anything else means the on-disk tree is
                // pointing at a range we don't know about, which is
                // unrecoverable corruption. Continuing past this would
                // either dereference garbage (Release, no assert) or
                // silently mis-route the lookup, so we abort here with
                // the offending range so the operator can find it.
                panic_inconsistency("tree_manifest::resolve",
                    "missing range_base dev=%u lba=%lu",
                    static_cast<unsigned>(range_base.device_id),
                    static_cast<unsigned long>(range_base.lba));
            }
            uint32_t idx = it->second;
            // M-3 (review §1): an in-bounds `range_base` with an
            // out-of-range slot index is the same class of
            // unrecoverable manifest corruption as a missing
            // range_base — `slot_paddr()` would otherwise cheerfully
            // compute an address outside the shadow range and the
            // lookup would read whatever happens to be on disk there.
            if (idx >= geom->shadow_slots_per_range) {
                panic_inconsistency("tree_manifest::resolve",
                    "slot_index out of range dev=%u lba=%lu idx=%u shadow_slots=%u",
                    static_cast<unsigned>(range_base.device_id),
                    static_cast<unsigned long>(range_base.lba),
                    static_cast<unsigned>(idx),
                    static_cast<unsigned>(geom->shadow_slots_per_range));
            }
            return geom->slot_paddr(range_base, idx);
        }

        uint32_t
        page_lbas() const {
            return geom->page_lbas();
        }

        static tree_manifest
        empty(const tree_geometry* g) {
            // M-2 (review round 2): `tree_manifest::geom` is a
            // non-owning raw pointer, so the non-null contract has
            // to live in every factory. Aggregate init with
            // `.geom = nullptr` can still bypass this helper —
            // callers that hand-build manifests get checked at the
            // consumer side (e.g. `make_lookup_state`), but
            // factory-built ones are caught here before the corrupt
            // state ever reaches a consumer.
            if (g == nullptr) {
                panic_inconsistency("tree_manifest::empty",
                    "tree_geometry pointer must not be null");
            }
            return { .root_slot = {0, 0}, .slot_map = {}, .geom = g };
        }
    };

}

#endif //APPS_INCONEL_CORE_TREE_MANIFEST_HH
