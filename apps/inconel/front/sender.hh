#ifndef APPS_INCONEL_FRONT_SENDER_HH
#define APPS_INCONEL_FRONT_SENDER_HH

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "./scheduler.hh"

namespace apps::inconel::front {

    [[nodiscard]] inline auto
    insert_memtable_entries(
        front_sched& sched,
        const core::front_fragment& fragment,
        std::span<const core::canonical_entry> canonical_entries) {
        return sched.insert_memtable_entries(fragment, canonical_entries);
    }

    [[nodiscard]] inline auto
    lookup_memtable(front_sched& sched,
                    std::string_view key,
                    uint64_t read_lsn,
                    core::front_read_set frs) {
        return sched.lookup_memtable(key, read_lsn, std::move(frs));
    }

    [[nodiscard]] inline auto
    batch_lookup(front_sched& sched,
                 std::span<const std::string_view> keys,
                 uint64_t read_lsn,
                 core::front_read_set frs) {
        return sched.batch_lookup(keys, read_lsn, std::move(frs));
    }

    [[nodiscard]] inline auto
    scan_memtable(front_sched& sched,
                  std::string_view begin,
                  std::string_view end,
                  uint64_t read_lsn,
                  core::front_read_set frs) {
        return sched.scan_memtable(begin, end, read_lsn, std::move(frs));
    }

    [[nodiscard]] inline auto
    seal_active(front_sched& sched) {
        return sched.seal_active();
    }

    [[nodiscard]] inline auto
    collect_eligible_gens(front_sched& sched, uint64_t durable_lsn) {
        return sched.collect_eligible_gens(durable_lsn);
    }

    [[nodiscard]] inline auto
    release_gens(front_sched& sched, std::vector<uint64_t> gen_ids) {
        return sched.release_gens(std::move(gen_ids));
    }

    [[nodiscard]] inline auto
    prepare_wal_fragment(
        front_sched& sched,
        const core::front_fragment& fragment,
        std::span<const core::canonical_entry> canonical_entries,
        wal::wal_fragment_cursor cursor) {
        return sched.prepare_wal_fragment(
            fragment, canonical_entries, cursor);
    }

    [[nodiscard]] inline auto
    install_wal_segment(front_sched& sched, wal::segment_runtime* segment) {
        return sched.install_wal_segment(segment);
    }

    [[nodiscard]] inline auto
    commit_wal_plan(front_sched& sched,
                    uint64_t plan_id,
                    std::vector<wal::wal_frame_write> writes = {}) {
        return sched.commit_wal_plan(plan_id, std::move(writes));
    }

    [[nodiscard]] inline auto
    abort_wal_plan(front_sched& sched,
                   uint64_t plan_id,
                   std::vector<wal::wal_frame_write> writes = {}) {
        return sched.abort_wal_plan(plan_id, std::move(writes));
    }

}  // namespace apps::inconel::front

#endif  // APPS_INCONEL_FRONT_SENDER_HH
