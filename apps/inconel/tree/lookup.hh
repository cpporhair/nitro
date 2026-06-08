#ifndef APPS_INCONEL_TREE_LOOKUP_HH
#define APPS_INCONEL_TREE_LOOKUP_HH

#include <cstdint>
#include <variant>
#include "../format/types.hh"

namespace apps::inconel::memory { struct segmented_page_frame; }

namespace apps::inconel::tree {

    struct lookup_value {
        uint64_t data_ver;
        format::value_ref vr;
    };

    struct lookup_tombstone {
        uint64_t data_ver;
    };

    struct lookup_absent {};

    using lookup_result = std::variant<lookup_value, lookup_tombstone, lookup_absent>;

    // ── Scheduler decision per round ──

    struct decision_done {};

    struct decision_need_read {
        std::vector<memory::segmented_page_frame*> frames;
    };

    using batch_decision = std::variant<decision_done, decision_need_read>;

}

#endif //APPS_INCONEL_TREE_LOOKUP_HH
