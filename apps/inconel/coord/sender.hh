#ifndef APPS_INCONEL_COORD_SENDER_HH
#define APPS_INCONEL_COORD_SENDER_HH

#include <cstdint>
#include <utility>

#include "./scheduler.hh"

namespace apps::inconel::coord {

    [[nodiscard]] inline auto
    assign_batch_lsn(coord_sched& sched, core::client_batch_buffer&& input) {
        return sched.assign_batch_lsn(std::move(input));
    }

    [[nodiscard]] inline auto
    publish_batch(coord_sched& sched, uint64_t batch_lsn) {
        return sched.publish_batch(batch_lsn);
    }

    [[nodiscard]] inline auto
    release_batch(coord_sched& sched, uint64_t batch_lsn) {
        return sched.release_batch(batch_lsn);
    }

    [[nodiscard]] inline auto
    acquire_read_handle(coord_sched& sched) {
        return sched.acquire_read_handle();
    }

}  // namespace apps::inconel::coord

#endif  // APPS_INCONEL_COORD_SENDER_HH
