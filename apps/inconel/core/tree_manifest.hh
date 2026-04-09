#ifndef APPS_INCONEL_CORE_TREE_MANIFEST_HH
#define APPS_INCONEL_CORE_TREE_MANIFEST_HH

#include <absl/container/flat_hash_map.h>

#include "../format/types.hh"
#include "./panic.hh"

namespace apps::inconel::core {

    using format::paddr;

    struct tree_manifest {
        paddr root_slot;
        absl::flat_hash_map<paddr, uint32_t> slot_map;
        uint32_t tree_page_size;
        uint32_t lba_size;

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
            return { range_base.device_id,
                     range_base.lba + static_cast<uint64_t>(idx) * (tree_page_size / lba_size) };
        }

        uint32_t
        page_lbas() const {
            return tree_page_size / lba_size;
        }

        static tree_manifest
        empty(uint32_t tps, uint32_t lbs) {
            return { .root_slot = {0, 0}, .slot_map = {}, .tree_page_size = tps, .lba_size = lbs };
        }
    };

}

#endif //APPS_INCONEL_CORE_TREE_MANIFEST_HH
