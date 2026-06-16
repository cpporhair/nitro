#ifndef APPS_INCONEL_PIPELINE_FLUSH_ROUND_HH
#define APPS_INCONEL_PIPELINE_FLUSH_ROUND_HH

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "pump/sender/any_exception.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"

#include "../coord/sender.hh"
#include "../core/flush_round.hh"
#include "../core/panic.hh"
#include "../front/sender.hh"
#include "../tree/sender.hh"

namespace apps::inconel::pipeline {

    using namespace pump::sender;

    struct flush_round_result {
        bool noop = false;
    };

    class flush_round_error : public std::runtime_error {
      public:
        explicit flush_round_error(tree::flush_stage_status st)
            : std::runtime_error("pipeline::flush_round_once: tree flush failed")
            , status(st) {}

        tree::flush_stage_status status;
    };

    namespace detail {

        struct flush_round_state {
            core::flush_frontier frontier;
            std::vector<std::vector<std::shared_ptr<core::memtable_gen>>>
                eligible_by_front;
            core::flush_release_plan release_plan;
        };

        inline void
        validate_flush_topology(
            const flush_round_state& st,
            std::span<front::front_sched* const> fronts,
            tree::tree_sched& tree_sched) {
            if (fronts.empty()) {
                throw std::invalid_argument(
                    "pipeline::flush_round_once: fronts must not be empty");
            }
            if (!st.frontier.old_guard ||
                !st.frontier.old_guard->manifest) {
                throw std::invalid_argument(
                    "pipeline::flush_round_once: frontier guard is invalid");
            }
            const auto* registered_tree = rt::owner();
            if (registered_tree != &tree_sched) {
                throw std::invalid_argument(
                    "pipeline::flush_round_once: tree scheduler is not the "
                    "registered owner");
            }
            for (std::size_t i = 0; i < fronts.size(); ++i) {
                if (fronts[i] == nullptr) {
                    throw std::invalid_argument(
                        "pipeline::flush_round_once: front scheduler is null");
                }
                if (fronts[i]->owner_id() != static_cast<uint32_t>(i)) {
                    throw std::invalid_argument(
                        "pipeline::flush_round_once: front owner order mismatch");
                }
            }
        }

        [[nodiscard]] inline tree::tree_flush_request
        build_tree_flush_request(flush_round_state& st) {
            tree::tree_flush_request req{
                .base_guard = st.frontier.old_guard,
                .sealed_gens = {},
                .recovery_safe_lsn = 0,
            };
            for (auto& front_gens : st.eligible_by_front) {
                req.sealed_gens.reserve(
                    req.sealed_gens.size() + front_gens.size());
                for (auto& gen : front_gens) {
                    if (!gen) {
                        throw std::invalid_argument(
                            "pipeline::flush_round_once: eligible gen is null");
                    }
                    req.sealed_gens.push_back(gen);
                }
            }
            if (req.sealed_gens.empty()) {
                throw std::logic_error(
                    "pipeline::flush_round_once: empty tree flush request");
            }
            return req;
        }

        [[nodiscard]] inline std::variant<core::flush_noop,
                                          tree::tree_flush_request>
        build_flush_branch(flush_round_state& st) {
            std::size_t total = 0;
            for (const auto& front_gens : st.eligible_by_front) {
                total += front_gens.size();
            }
            if (total == 0) {
                return core::flush_noop{};
            }
            return build_tree_flush_request(st);
        }

        [[nodiscard]] inline tree::tree_flush_result
        require_successful_tree_flush(tree::tree_flush_result&& result) {
            if (result.st != tree::flush_stage_status::ok) {
                throw flush_round_error(result.st);
            }
            if (result.new_manifest == nullptr) {
                throw std::runtime_error(
                    "pipeline::flush_round_once: tree flush returned null "
                    "manifest");
            }
            return std::move(result);
        }

        [[nodiscard]] inline auto
        end_round_then_rethrow(coord::coord_sched& coord_sched,
                               std::exception_ptr ep) {
            return coord::end_flush_round(coord_sched)
                >> then([ep = std::move(ep)]() -> flush_round_result {
                    std::rethrow_exception(ep);
                });
        }

        [[nodiscard]] inline auto
        release_flushed_gens(std::span<front::front_sched* const> fronts,
                             core::flush_release_plan& release_plan) {
            return just()
                >> loop(fronts.size())
                >> concurrent(fronts.size())
                >> flat_map([fronts, &release_plan](std::size_t i) {
                    std::vector<uint64_t> ids;
                    if (i < release_plan.gen_ids_by_front.size()) {
                        ids = std::move(release_plan.gen_ids_by_front[i]);
                    }
                    return front::release_gens(
                            *fronts[i],
                            std::move(ids))
                        >> then([]() {
                            return std::exception_ptr{};
                        })
                        >> any_exception([](std::exception_ptr ep) {
                            return just(std::move(ep));
                        });
                })
                >> reduce(
                    std::exception_ptr{},
                    [](std::exception_ptr& first, std::exception_ptr ep) {
                        if (!first && ep) {
                            first = std::move(ep);
                        }
                    })
                >> then([](std::exception_ptr first) {
                    if (first) {
                        core::panic_inconsistency(
                            "pipeline::flush_round_once",
                            "release_gens failed after CAT2 install");
                    }
                });
        }

    }  // namespace detail

    [[nodiscard]] inline auto
    flush_round_once(coord::coord_sched& coord_sched,
                     std::span<front::front_sched* const> fronts,
                     tree::tree_sched& tree_sched) {
        return coord::capture_flush_frontier(coord_sched)
            >> then([fronts](core::flush_frontier&& frontier) {
                return detail::flush_round_state{
                    .frontier = std::move(frontier),
                    .eligible_by_front =
                        std::vector<std::vector<
                            std::shared_ptr<core::memtable_gen>>>(
                            fronts.size()),
                    .release_plan = {},
                };
            })
            >> push_result_to_context()
            >> get_context<detail::flush_round_state>()
            >> flat_map([fronts, &coord_sched, &tree_sched](
                            detail::flush_round_state& st) {
                return just()
                    >> then([fronts, &tree_sched, &st]() {
                        detail::validate_flush_topology(st, fronts, tree_sched);
                    })
                    >> flat_map([fronts, &st]() {
                        return just()
                            >> loop(fronts.size())
                            >> concurrent(fronts.size())
                            >> flat_map([fronts, &st](std::size_t i) {
                                return front::collect_eligible_gens(
                                        *fronts[i],
                                        st.frontier.durable_lsn)
                                    >> then([&st, i](
                                                std::vector<
                                                    std::shared_ptr<
                                                        core::memtable_gen>>&&
                                                    gens) {
                                        st.eligible_by_front[i] =
                                            std::move(gens);
                                    });
                            })
                            >> reduce();
                    })
                    >> then([&st](bool) {
                        return detail::build_flush_branch(st);
                    })
                    >> visit()
                    >> flat_map([fronts, &coord_sched, &st]<typename Branch>(
                                    Branch&& branch) {
                        using branch_t = std::decay_t<Branch>;
                        if constexpr (std::is_same_v<branch_t,
                                                      core::flush_noop>) {
                            (void)branch;
                            return coord::end_flush_round(coord_sched)
                                >> then([]() {
                                    return flush_round_result{.noop = true};
                                });
                        } else {
                            return tree::tree_local_flush(std::move(branch))
                                >> then([](tree::tree_flush_result&& result) {
                                    return detail::require_successful_tree_flush(
                                        std::move(result));
                                })
                                >> flat_map([&coord_sched, &st](
                                                tree::tree_flush_result&&
                                                    result) {
                                    st.release_plan =
                                        core::extract_release_plan(
                                            result.flushed_gens_by_front);
                                    return coord::frontier_switch(
                                        coord_sched,
                                        st.frontier.old_guard,
                                        std::move(result.new_manifest),
                                        std::move(result.retired),
                                        st.release_plan);
                                })
                                >> flat_map([fronts, &st]() {
                                    return detail::release_flushed_gens(
                                        fronts, st.release_plan);
                                })
                                >> flat_map([&coord_sched]() {
                                    return coord::end_flush_round(coord_sched);
                                })
                                >> then([]() {
                                    return flush_round_result{.noop = false};
                                });
                        }
                    })
                    >> any_exception([&coord_sched](std::exception_ptr ep) {
                        return detail::end_round_then_rethrow(
                            coord_sched, std::move(ep));
                    });
            })
            >> pop_context();
    }

}  // namespace apps::inconel::pipeline

#endif  // APPS_INCONEL_PIPELINE_FLUSH_ROUND_HH
