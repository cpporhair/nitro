#ifndef APPS_INCONEL_PIPELINE_MAINTENANCE_ROUND_HH
#define APPS_INCONEL_PIPELINE_MAINTENANCE_ROUND_HH

#include <type_traits>
#include <utility>
#include <variant>

#include "pump/sender/flat.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"

#include "../tree/sender.hh"
#include "../value/sender.hh"

namespace apps::inconel::runtime {

    struct maintenance_round_result {
        tree::reclaim_round_result      reclaim;
        value::value_trim_round_result  trim;

        [[nodiscard]] bool did_work() const noexcept {
            return !reclaim.noop || !trim.noop;
        }
    };

}  // namespace apps::inconel::runtime

namespace apps::inconel::pipeline {

    using namespace pump::sender;

    namespace detail {

        struct maintenance_skip_trim {
            tree::reclaim_round_result reclaim;
        };

        struct maintenance_run_trim {
            tree::reclaim_round_result reclaim;
        };

        using maintenance_trim_branch =
            std::variant<maintenance_skip_trim, maintenance_run_trim>;

    }  // namespace detail

    template <typename NvmeProvider = value::local_nvme_provider>
    [[nodiscard]] inline auto
    maintenance_round_once(tree::tree_sched& owner,
                           NvmeProvider value_nvme = {}) {
        return tree::reclaim_once(owner)
            >> then([](tree::reclaim_round_result reclaim) {
                if (reclaim.noop) {
                    return detail::maintenance_trim_branch{
                        detail::maintenance_skip_trim{
                            .reclaim = std::move(reclaim),
                        }};
                }
                return detail::maintenance_trim_branch{
                    detail::maintenance_run_trim{
                        .reclaim = std::move(reclaim),
                    }};
            })
            >> visit()
            >> flat_map([value_nvme](auto&& branch) mutable {
                using branch_t = std::decay_t<decltype(branch)>;
                if constexpr (std::is_same_v<branch_t,
                                              detail::maintenance_skip_trim>) {
                    return just(runtime::maintenance_round_result{
                        .reclaim = std::move(branch.reclaim),
                        .trim    = value::value_trim_round_result{.noop = true},
                    });
                } else {
                    return value::drain_trim_once(value_nvme)
                        >> then([reclaim = std::move(branch.reclaim)](
                                    value::value_trim_round_result trim) mutable {
                            return runtime::maintenance_round_result{
                                .reclaim = std::move(reclaim),
                                .trim    = trim,
                            };
                        });
                }
            });
    }

}  // namespace apps::inconel::pipeline

#endif  // APPS_INCONEL_PIPELINE_MAINTENANCE_ROUND_HH
