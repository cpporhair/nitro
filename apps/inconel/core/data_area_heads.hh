#ifndef APPS_INCONEL_CORE_DATA_AREA_HEADS_HH
#define APPS_INCONEL_CORE_DATA_AREA_HEADS_HH

#include <atomic>
#include <cstdint>

namespace apps::inconel::core {

    struct data_area_heads {
        std::atomic<uint64_t> tree_head_lba{0};
        std::atomic<uint64_t> value_head_lba{0};
    };

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_DATA_AREA_HEADS_HH
