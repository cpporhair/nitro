#ifndef APPS_INCONEL_CORE_WAL_RECLAIM_FRONTIER_HH
#define APPS_INCONEL_CORE_WAL_RECLAIM_FRONTIER_HH

#include <atomic>
#include <cstdint>
#include <limits>

namespace apps::inconel::core {

struct wal_reclaim_frontier {
    static constexpr uint64_t no_unreclaimed_lsn =
        std::numeric_limits<uint64_t>::max();

    std::atomic<uint64_t> global_min_unreclaimed_lsn{no_unreclaimed_lsn};

    void
    publish_exact_min(uint64_t min_unreclaimed_lsn) noexcept {
        global_min_unreclaimed_lsn.store(
            min_unreclaimed_lsn, std::memory_order_release);
    }

    void
    observe_active_min(uint64_t min_unreclaimed_lsn) noexcept {
        if (min_unreclaimed_lsn == no_unreclaimed_lsn) {
            return;
        }

        auto cur = global_min_unreclaimed_lsn.load(std::memory_order_acquire);
        while (min_unreclaimed_lsn < cur &&
               !global_min_unreclaimed_lsn.compare_exchange_weak(
                   cur,
                   min_unreclaimed_lsn,
                   std::memory_order_release,
                   std::memory_order_acquire)) {
        }
    }
};

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_WAL_RECLAIM_FRONTIER_HH
