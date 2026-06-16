#ifndef APPS_INCONEL_CORE_FLUSH_ROUND_HH
#define APPS_INCONEL_CORE_FLUSH_ROUND_HH

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>

#include "./checkpoint_guard.hh"
#include "./memtable.hh"

namespace apps::inconel::tree {

    using flushed_gens_by_front_map = absl::flat_hash_map<
        uint32_t,
        absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>>;

}  // namespace apps::inconel::tree

namespace apps::inconel::core {

    struct flush_frontier {
        uint64_t durable_lsn = 0;
        std::shared_ptr<checkpoint_guard> old_guard;
    };

    struct flush_release_plan {
        std::vector<std::vector<uint64_t>> gen_ids_by_front;

        [[nodiscard]] std::span<const uint64_t>
        gen_ids_for(std::size_t i) const noexcept {
            if (i >= gen_ids_by_front.size()) {
                return {};
            }
            return std::span<const uint64_t>(gen_ids_by_front[i]);
        }
    };

    struct flush_noop {};

    [[nodiscard]] inline flush_release_plan
    extract_release_plan(
        const tree::flushed_gens_by_front_map& flushed_gens_by_front) {
        flush_release_plan plan;
        for (const auto& [front_idx, gens] : flushed_gens_by_front) {
            if (front_idx >= plan.gen_ids_by_front.size()) {
                plan.gen_ids_by_front.resize(
                    static_cast<std::size_t>(front_idx) + 1);
            }

            auto& ids = plan.gen_ids_by_front[front_idx];
            ids.reserve(ids.size() + gens.size());
            for (const auto& gen : gens) {
                if (!gen) {
                    throw std::invalid_argument(
                        "core::extract_release_plan: flushed gen is null");
                }
                ids.push_back(gen->gen_id);
            }
        }
        return plan;
    }

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_FLUSH_ROUND_HH
