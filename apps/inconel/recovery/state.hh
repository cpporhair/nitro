#ifndef APPS_INCONEL_RECOVERY_STATE_HH
#define APPS_INCONEL_RECOVERY_STATE_HH

#include <cstdint>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "../core/leaf_order.hh"
#include "../core/tree_reverse_topology.hh"
#include "../format/superblock.hh"
#include "../format/types.hh"
#include "../value/space_manager.hh"

namespace apps::inconel::recovery {

    struct recovered_tree_snapshot {
        format::paddr root_slot{};
        absl::flat_hash_map<format::paddr, uint32_t> slot_map;
        core::leaf_order_index leaf_order;
        format::paddr root_range_base{};
        core::tree_reverse_topology reverse_topology;

        [[nodiscard]] bool
        has_root() const noexcept {
            return root_slot.lba != 0 || root_slot.device_id != 0;
        }
    };

    struct recovered_runtime_state {
        recovered_tree_snapshot tree;
        std::vector<value::live_value_extent> live_value_extents;
        std::vector<format::range_ref> tree_free_ranges;
        uint64_t recovered_durable_lsn = 0;
        uint64_t next_lsn = 1;
        uint64_t tree_alloc_head_lba = 0;
        format::superblock_choice::source active_superblock_source =
            format::superblock_choice::source::none;
    };

}  // namespace apps::inconel::recovery

#endif  // APPS_INCONEL_RECOVERY_STATE_HH
