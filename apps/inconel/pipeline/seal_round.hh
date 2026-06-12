#ifndef APPS_INCONEL_PIPELINE_SEAL_ROUND_HH
#define APPS_INCONEL_PIPELINE_SEAL_ROUND_HH

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"

#include "../coord/sender.hh"
#include "../core/read_catalog.hh"
#include "../front/sender.hh"

namespace apps::inconel::pipeline {

    using namespace pump::sender;

    struct seal_round_result {
        std::shared_ptr<const core::publish_catalog> cat1;
    };

    namespace detail {

        struct seal_round_state {
            std::shared_ptr<const core::publish_catalog> old_cat;
            std::vector<core::front_read_set> results;
            std::shared_ptr<const core::publish_catalog> cat1;
        };

        inline void
        validate_seal_topology(
            const seal_round_state& st,
            std::span<front::front_sched* const> fronts) {
            if (fronts.empty()) {
                throw std::invalid_argument(
                    "pipeline::seal_round: fronts must not be empty");
            }
            const auto& prs_fronts = *st.old_cat->prs->fronts;
            if (fronts.size() != prs_fronts.size()) {
                throw std::invalid_argument(
                    "pipeline::seal_round: front topology size mismatch");
            }
            for (std::size_t i = 0; i < fronts.size(); ++i) {
                if (fronts[i] == nullptr) {
                    throw std::invalid_argument(
                        "pipeline::seal_round: front scheduler is null");
                }
                if (fronts[i]->owner_id() != static_cast<uint32_t>(i)) {
                    throw std::invalid_argument(
                        "pipeline::seal_round: front owner order mismatch");
                }
            }
        }

        [[nodiscard]] inline std::shared_ptr<const core::publish_catalog>
        build_cat1(std::shared_ptr<const core::publish_catalog> old_cat,
                   std::vector<core::front_read_set>&& fronts) {
            if (!old_cat) {
                throw std::invalid_argument(
                    "pipeline::seal_round: old CAT must not be null");
            }

            const uint64_t epoch = old_cat->epoch + 1;
            auto prs_fronts =
                std::make_shared<const std::vector<core::front_read_set>>(
                    std::move(fronts));
            auto prs = std::make_shared<const core::published_read_set>(
                core::published_read_set{
                    .tree_guard = old_cat->prs->tree_guard,
                    .fronts = std::move(prs_fronts),
                    .epoch = epoch,
                });
            const uint64_t durable =
                old_cat->durable_lsn.load(std::memory_order_acquire);
            return std::make_shared<const core::publish_catalog>(
                std::move(prs), durable, epoch);
        }

    }  // namespace detail

    [[nodiscard]] inline auto
    seal_round(coord::coord_sched& coord_sched,
               std::span<front::front_sched* const> fronts) {
        return coord::close_gate(coord_sched)
            >> then([fronts](
                        std::shared_ptr<const core::publish_catalog> old_cat) {
                return detail::seal_round_state{
                    .old_cat = std::move(old_cat),
                    .results =
                        std::vector<core::front_read_set>(fronts.size()),
                };
            })
            >> push_result_to_context()
            >> get_context<detail::seal_round_state>()
            >> flat_map([fronts, &coord_sched](
                            detail::seal_round_state& st) {
                detail::validate_seal_topology(st, fronts);

                return just()
                    >> loop(fronts.size())
                    >> concurrent(fronts.size())
                    >> flat_map([fronts, &st](std::size_t i) {
                        return front::seal_active(*fronts[i])
                            >> then([&st, i](core::front_read_set&& frs) {
                                st.results[i] = std::move(frs);
                            });
                    })
                    >> reduce()
                    >> flat_map([&coord_sched, &st](bool) {
                        st.cat1 = detail::build_cat1(
                            st.old_cat, std::move(st.results));
                        return coord::install_cat(coord_sched, st.cat1);
                    })
                    >> flat_map([&coord_sched]() {
                        // Seal orchestration failures intentionally leave the
                        // gate closed for runtime intervention; there is no
                        // automatic rollback path in M12/051.
                        return coord::open_gate(coord_sched);
                    })
                    >> then([&st]() {
                        return seal_round_result{.cat1 = std::move(st.cat1)};
                    });
            })
            >> pop_context();
    }

}  // namespace apps::inconel::pipeline

#endif  // APPS_INCONEL_PIPELINE_SEAL_ROUND_HH
