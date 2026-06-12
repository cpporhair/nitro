#ifndef APPS_INCONEL_COORD_SENDER_HH
#define APPS_INCONEL_COORD_SENDER_HH

#include <cstdint>
#include <memory>
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

    [[nodiscard]] inline auto
    close_gate(coord_sched& sched) {
        return sched.close_gate();
    }

    [[nodiscard]] inline auto
    install_cat(coord_sched& sched,
                std::shared_ptr<const core::publish_catalog> cat) {
        return sched.install_cat(std::move(cat));
    }

    [[nodiscard]] inline auto
    open_gate(coord_sched& sched) {
        return sched.open_gate();
    }

    [[nodiscard]] inline auto
    enter_memtable_phase(coord_sched& sched, uint64_t batch_lsn) {
        return sched.enter_memtable_phase(batch_lsn);
    }

}  // namespace apps::inconel::coord

#endif  // APPS_INCONEL_COORD_SENDER_HH
