#ifndef APPS_INCONEL_CORE_TREE_MANIFEST_HH
#define APPS_INCONEL_CORE_TREE_MANIFEST_HH

#include <cassert>
#include <unordered_map>
#include "../format/types.hh"

namespace apps::inconel::core {

    using format::paddr;

    struct tree_manifest {
        paddr root_slot;
        std::unordered_map<paddr, uint32_t> slot_map;
        uint32_t tree_page_size;
        uint32_t lba_size;

        bool
        has_root() const {
            return root_slot.lba != 0;
        }

        paddr
        resolve(paddr range_base) const {
            auto it = slot_map.find(range_base);
            assert(it != slot_map.end());
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
