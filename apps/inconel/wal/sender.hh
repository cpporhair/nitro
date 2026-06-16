#ifndef APPS_INCONEL_WAL_SENDER_HH
#define APPS_INCONEL_WAL_SENDER_HH

#include <cstdint>
#include <optional>
#include <utility>

#include "./scheduler.hh"

namespace apps::inconel::wal {

[[nodiscard]] inline auto
alloc_segment(wal_space_sched &sched, uint32_t stream_id,
              std::optional<sealed_segment_info> sealed = std::nullopt) {
  return sched.alloc_segment(stream_id, std::move(sealed));
}

[[nodiscard]] inline auto reclaim_check(wal_space_sched &sched,
                                        uint64_t flush_durable_frontier) {
  return sched.reclaim_check(flush_durable_frontier);
}

} // namespace apps::inconel::wal

#endif // APPS_INCONEL_WAL_SENDER_HH
