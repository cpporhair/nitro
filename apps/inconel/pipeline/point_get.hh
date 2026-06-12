#ifndef APPS_INCONEL_PIPELINE_POINT_GET_HH
#define APPS_INCONEL_PIPELINE_POINT_GET_HH

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "pump/sender/flat.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"

#include "../coord/sender.hh"
#include "../core/batch_carrier.hh"
#include "../core/memtable_lookup.hh"
#include "../core/panic.hh"
#include "../core/read_catalog.hh"
#include "../front/sender.hh"
#include "../tree/lookup.hh"
#include "../tree/sender.hh"
#include "../value/sender.hh"

namespace apps::inconel::pipeline {

    using namespace pump::sender;

    struct point_get_result {
        bool        found = false;
        std::string value;
    };

    namespace detail {

        [[nodiscard]] inline point_get_result
        found_value(std::string body) {
            return point_get_result{
                .found = true,
                .value = std::move(body),
            };
        }

        [[nodiscard]] inline tree::lookup_result
        take_single_tree_result(std::vector<tree::lookup_result>&& results) {
            if (results.size() != 1) {
                core::panic_inconsistency(
                    "pipeline::point_get",
                    "single-key tree lookup returned %llu results",
                    static_cast<unsigned long long>(results.size()));
            }
            return std::move(results[0]);
        }

    }  // namespace detail

    template <typename NvmeProvider = value::local_nvme_provider>
    [[nodiscard]] inline auto
    point_get(coord::coord_sched& coord_sched,
              std::span<front::front_sched* const> fronts,
              std::string_view key,
              NvmeProvider value_nvme = {}) {
        return coord::acquire_read_handle(coord_sched)
            >> push_result_to_context()
            >> get_context<core::read_handle>()
            >> flat_map([fronts, key, value_nvme](core::read_handle& rh) mutable {
                if (fronts.empty()) {
                    throw std::invalid_argument(
                        "pipeline::point_get: fronts must not be empty");
                }
                const auto& prs_fronts = *rh.cat->prs->fronts;
                if (fronts.size() != prs_fronts.size()) {
                    throw std::invalid_argument(
                        "pipeline::point_get: front topology size mismatch");
                }

                const uint32_t owner =
                    static_cast<uint32_t>(core::key_hash(key) % fronts.size());
                if (fronts[owner] == nullptr) {
                    throw std::invalid_argument(
                        "pipeline::point_get: owner front scheduler is null");
                }

                core::read_handle* rhp = &rh;
                const core::front_read_set* frs = &prs_fronts[owner];

                return front::lookup_memtable(
                           *fronts[owner], key, rh.read_lsn, frs)
                    >> visit()
                    >> flat_map([key, value_nvme, rhp]<typename R>(
                                    R&& r) mutable {
                        using result_t = std::decay_t<R>;
                        if constexpr (std::is_same_v<
                                          result_t,
                                          core::memtable_value_hit>) {
                            return value::read_value(r.durable, value_nvme)
                                >> then([](std::string body) {
                                    return detail::found_value(std::move(body));
                                });
                        } else if constexpr (std::is_same_v<
                                                 result_t,
                                                 core::memtable_tombstone>) {
                            return just(point_get_result{});
                        } else {
                            std::string_view single_key[] = {key};
                            return just()
                                >> tree::lookup(
                                       std::span<const std::string_view>(
                                           single_key, 1),
                                       rhp->cat->prs->tree_guard
                                           ->manifest.get())
                                >> then([](
                                            std::vector<tree::lookup_result>
                                                results) {
                                    return detail::take_single_tree_result(
                                        std::move(results));
                                })
                                >> visit()
                                >> flat_map([value_nvme]<typename T>(
                                                T&& t) mutable {
                                    using tree_result_t = std::decay_t<T>;
                                    if constexpr (std::is_same_v<
                                                      tree_result_t,
                                                      tree::lookup_value>) {
                                        return value::read_value(
                                                   t.vr, value_nvme)
                                            >> then([](std::string body) {
                                                return detail::found_value(
                                                    std::move(body));
                                            });
                                    } else {
                                        return just(point_get_result{});
                                    }
                                });
                        }
                    });
            })
            >> pop_context();
    }

}  // namespace apps::inconel::pipeline

#endif  // APPS_INCONEL_PIPELINE_POINT_GET_HH
