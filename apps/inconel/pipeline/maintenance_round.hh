#ifndef APPS_INCONEL_PIPELINE_MAINTENANCE_ROUND_HH
#define APPS_INCONEL_PIPELINE_MAINTENANCE_ROUND_HH

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "pump/sender/flat.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"

#include "./flush_round.hh"
#include "./seal_round.hh"
#include "../tree/sender.hh"
#include "../value/sender.hh"
#include "../wal/scheduler.hh"

namespace apps::inconel::runtime {

    struct maintenance_policy {
        bool auto_seal_flush = true;

        uint64_t seal_active_memtable_bytes = 256ull * 1024ull * 1024ull;
        uint64_t total_memtable_limit_bytes = 1024ull * 1024ull * 1024ull;
        float    wal_seal_used_ratio = 0.70f;
        uint32_t max_sealed_gens_per_front = 4;
    };

    inline void
    validate_maintenance_policy(const maintenance_policy& policy) {
        if (policy.wal_seal_used_ratio < 0.0f ||
            policy.wal_seal_used_ratio > 1.0f) {
            throw std::invalid_argument(
                "runtime::maintenance_policy: wal_seal_used_ratio must be "
                "between 0 and 1");
        }
        if (policy.max_sealed_gens_per_front == 0) {
            throw std::invalid_argument(
                "runtime::maintenance_policy: max_sealed_gens_per_front "
                "must be > 0");
        }
    }

    struct maintenance_pressure_snapshot {
        uint64_t active_memtable_bytes = 0;
        uint64_t sealed_memtable_bytes = 0;
        uint64_t total_memtable_bytes = 0;
        uint32_t max_sealed_gens_per_front = 0;
        std::size_t wal_used_segments = 0;
        std::size_t wal_total_segments = 0;
        bool active_has_data = false;
    };

    struct maintenance_seal_flush_result {
        bool auto_enabled = false;
        bool seal_requested = false;
        bool seal_ran = false;
        bool flush_requested = false;
        bool flush_ran = false;
        pipeline::flush_round_result flush{.noop = true};
        maintenance_pressure_snapshot pressure{};

        [[nodiscard]] bool did_work() const noexcept {
            return seal_ran || !flush.noop;
        }
    };

    struct maintenance_reclaim_trim_result {
        tree::reclaim_round_result     reclaim;
        value::value_trim_round_result trim;
    };

    struct maintenance_round_result {
        maintenance_seal_flush_result  seal_flush;
        tree::reclaim_round_result     reclaim;
        value::value_trim_round_result trim;

        [[nodiscard]] bool did_work() const noexcept {
            return seal_flush.did_work() || !reclaim.noop || !trim.noop;
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

        struct maintenance_seal_flush_state {
            runtime::maintenance_policy policy;
            runtime::maintenance_seal_flush_result result;
        };

        struct maintenance_skip_seal {
            maintenance_seal_flush_state st;
        };

        struct maintenance_run_seal {
            maintenance_seal_flush_state st;
        };

        using maintenance_seal_branch =
            std::variant<maintenance_skip_seal, maintenance_run_seal>;

        struct maintenance_skip_flush {
            maintenance_seal_flush_state st;
        };

        struct maintenance_run_flush {
            maintenance_seal_flush_state st;
        };

        using maintenance_flush_branch =
            std::variant<maintenance_skip_flush, maintenance_run_flush>;

        template <typename FrontSpan>
        [[nodiscard]] runtime::maintenance_pressure_snapshot
        capture_pressure(FrontSpan fronts, const wal::wal_space_sched& wal) {
            runtime::maintenance_pressure_snapshot out{};
            for (front::front_sched* sched : fronts) {
                if (sched == nullptr) {
                    throw std::invalid_argument(
                        "pipeline::maintenance_round_once: front scheduler "
                        "is null");
                }
                const uint64_t active = sched->active_memtable_bytes();
                const uint64_t sealed = sched->sealed_memtable_bytes();
                out.active_memtable_bytes += active;
                out.sealed_memtable_bytes += sealed;
                out.total_memtable_bytes += active + sealed;
                out.max_sealed_gens_per_front =
                    std::max(out.max_sealed_gens_per_front,
                             sched->sealed_gen_count());
                out.active_has_data =
                    out.active_has_data || sched->active_memtable_has_entries();
            }
            out.wal_used_segments = wal.used_segment_count();
            out.wal_total_segments = wal.segment_count();
            return out;
        }

        [[nodiscard]] inline bool
        wal_pressure_exceeded(const runtime::maintenance_policy& policy,
                              const runtime::maintenance_pressure_snapshot& p) {
            if (p.wal_total_segments == 0) return false;
            const double used =
                static_cast<double>(p.wal_used_segments) /
                static_cast<double>(p.wal_total_segments);
            return used > static_cast<double>(policy.wal_seal_used_ratio);
        }

        [[nodiscard]] inline bool
        should_seal(const runtime::maintenance_policy& policy,
                    const runtime::maintenance_pressure_snapshot& p) {
            if (!policy.auto_seal_flush || !p.active_has_data) {
                return false;
            }
            return p.active_memtable_bytes >
                       policy.seal_active_memtable_bytes ||
                   p.total_memtable_bytes >
                       policy.total_memtable_limit_bytes ||
                   wal_pressure_exceeded(policy, p) ||
                   p.max_sealed_gens_per_front >=
                       policy.max_sealed_gens_per_front;
        }

        [[nodiscard]] inline bool
        should_flush(const runtime::maintenance_policy& policy,
                     const runtime::maintenance_pressure_snapshot& p,
                     bool seal_requested) {
            if (!policy.auto_seal_flush) {
                return false;
            }
            return seal_requested ||
                   p.max_sealed_gens_per_front > 0 ||
                   p.total_memtable_bytes >
                       policy.total_memtable_limit_bytes ||
                   wal_pressure_exceeded(policy, p);
        }

        template <typename FrontSpan>
        [[nodiscard]] maintenance_seal_flush_state
        make_seal_flush_state(FrontSpan fronts,
                              const wal::wal_space_sched& wal,
                              runtime::maintenance_policy policy) {
            runtime::validate_maintenance_policy(policy);
            auto pressure = capture_pressure(fronts, wal);
            const bool seal = should_seal(policy, pressure);
            const bool flush = should_flush(policy, pressure, seal);
            return maintenance_seal_flush_state{
                .policy = policy,
                .result = runtime::maintenance_seal_flush_result{
                    .auto_enabled = policy.auto_seal_flush,
                    .seal_requested = seal,
                    .seal_ran = false,
                    .flush_requested = flush,
                    .flush_ran = false,
                    .flush = flush_round_result{.noop = true},
                    .pressure = pressure,
                },
            };
        }

        [[nodiscard]] inline maintenance_seal_branch
        make_seal_branch(maintenance_seal_flush_state st) {
            if (!st.result.seal_requested) {
                return maintenance_seal_branch{
                    maintenance_skip_seal{.st = std::move(st)}};
            }
            return maintenance_seal_branch{
                maintenance_run_seal{.st = std::move(st)}};
        }

        [[nodiscard]] inline maintenance_flush_branch
        make_flush_branch(maintenance_seal_flush_state st) {
            if (!st.result.flush_requested) {
                return maintenance_flush_branch{
                    maintenance_skip_flush{.st = std::move(st)}};
            }
            return maintenance_flush_branch{
                maintenance_run_flush{.st = std::move(st)}};
        }

        [[nodiscard]] inline bool
        is_catalog_update_busy(std::exception_ptr ep) noexcept {
            if (!ep) return false;
            try {
                std::rethrow_exception(ep);
            } catch (const std::logic_error& e) {
                return std::string_view(e.what()) ==
                       "catalog_update_in_progress";
            } catch (...) {
                return false;
            }
        }

    }  // namespace detail

    template <typename NvmeProvider = value::local_nvme_provider>
    [[nodiscard]] inline auto
    reclaim_trim_round_once(tree::tree_sched& owner,
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
                    return just(runtime::maintenance_reclaim_trim_result{
                        .reclaim = std::move(branch.reclaim),
                        .trim = value::value_trim_round_result{.noop = true},
                    });
                } else {
                    return value::drain_trim_once(value_nvme)
                        >> then([reclaim = std::move(branch.reclaim)](
                                    value::value_trim_round_result trim) mutable {
                            return runtime::maintenance_reclaim_trim_result{
                                .reclaim = std::move(reclaim),
                                .trim = trim,
                            };
                        });
                }
            });
    }

    template <typename NvmeProvider = value::local_nvme_provider>
    [[nodiscard]] inline auto
    maintenance_round_once(tree::tree_sched& owner,
                           NvmeProvider value_nvme = {}) {
        return reclaim_trim_round_once(owner, value_nvme)
            >> then([](runtime::maintenance_reclaim_trim_result rt) {
                return runtime::maintenance_round_result{
                    .seal_flush = runtime::maintenance_seal_flush_result{},
                    .reclaim = std::move(rt.reclaim),
                    .trim = std::move(rt.trim),
                };
            });
    }

    template <typename NvmeProvider = value::local_nvme_provider>
    [[nodiscard]] inline auto
    maintenance_round_once(coord::coord_sched& coord,
                           std::span<front::front_sched* const> fronts,
                           wal::wal_space_sched& wal_space,
                           tree::tree_sched& tree,
                           runtime::maintenance_policy policy = {},
                           NvmeProvider value_nvme = {}) {
        return just()
            >> then([fronts, &wal_space, policy]() {
                return detail::make_seal_flush_state(
                    fronts, wal_space, policy);
            })
            >> then([](detail::maintenance_seal_flush_state st) {
                return detail::make_seal_branch(std::move(st));
            })
            >> visit()
            >> flat_map([&coord, fronts](auto&& branch) mutable {
                using branch_t = std::decay_t<decltype(branch)>;
                if constexpr (std::is_same_v<branch_t,
                                              detail::maintenance_skip_seal>) {
                    return just(std::move(branch.st));
                } else {
                    return seal_round(coord, fronts)
                        >> then([st = branch.st](
                                    seal_round_result) mutable {
                            st.result.seal_ran = true;
                            return std::move(st);
                        })
                        >> any_exception([st = branch.st](
                                             std::exception_ptr ep) mutable {
                            return just(std::move(st))
                                >> then([ep](detail::maintenance_seal_flush_state
                                                 st) mutable {
                                    if (!detail::is_catalog_update_busy(ep)) {
                                        std::rethrow_exception(ep);
                                    }
                                    st.result.seal_requested = false;
                                    st.result.flush_requested = false;
                                    return st;
                                });
                        });
                }
            })
            >> then([](detail::maintenance_seal_flush_state st) {
                return detail::make_flush_branch(std::move(st));
            })
            >> visit()
            >> flat_map([&coord, fronts, &tree](auto&& branch) mutable {
                using branch_t = std::decay_t<decltype(branch)>;
                if constexpr (std::is_same_v<branch_t,
                                              detail::maintenance_skip_flush>) {
                    return just(std::move(branch.st));
                } else {
                    return flush_round_once(coord, fronts, tree)
                        >> then([st = branch.st](
                                    flush_round_result flush) mutable {
                            st.result.flush_ran = true;
                            st.result.flush = flush;
                            return std::move(st);
                        })
                        >> any_exception([st = branch.st](
                                             std::exception_ptr ep) mutable {
                            return just(std::move(st))
                                >> then([ep](detail::maintenance_seal_flush_state
                                                 st) mutable {
                                    if (!detail::is_catalog_update_busy(ep)) {
                                        std::rethrow_exception(ep);
                                    }
                                    st.result.flush_requested = false;
                                    return st;
                                });
                        });
                }
            })
            >> flat_map([&tree, value_nvme](
                            detail::maintenance_seal_flush_state st) mutable {
                return reclaim_trim_round_once(tree, value_nvme)
                    >> then([seal_flush = std::move(st.result)](
                                runtime::maintenance_reclaim_trim_result rt)
                                mutable {
                        return runtime::maintenance_round_result{
                            .seal_flush = std::move(seal_flush),
                            .reclaim = std::move(rt.reclaim),
                            .trim = std::move(rt.trim),
                        };
                    });
            });
    }

}  // namespace apps::inconel::pipeline

#endif  // APPS_INCONEL_PIPELINE_MAINTENANCE_ROUND_HH
