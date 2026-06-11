#ifndef APPS_INCONEL_WRITE_PATH_SENDER_HH
#define APPS_INCONEL_WRITE_PATH_SENDER_HH

#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include "pump/coro/coro.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"

#include "../front/sender.hh"
#include "../nvme/frame_io.hh"
#include "../wal/sender.hh"

namespace apps::inconel::write_path {

    using namespace pump::sender;

    struct wal_plan_issue_result {
        uint64_t plan_id = 0;
        wal::wal_fragment_cursor cursor_after{};
        bool fragment_done = false;
        bool fua_ok = false;
        std::exception_ptr exception{};
    };

    struct wal_plan_completion {
        wal::wal_fragment_cursor cursor_after{};
        bool fragment_done = false;
        std::optional<wal::sealed_segment_info> sealed;
    };

    struct wal_commit_after_issue {
        front::front_sched* sched = nullptr;
        uint64_t plan_id = 0;
        wal::wal_fragment_cursor cursor_after{};
        bool fragment_done = false;
    };

    struct wal_abort_after_issue {
        front::front_sched* sched = nullptr;
        uint64_t plan_id = 0;
        bool had_exception = false;
    };

    using wal_finish_after_issue =
        std::variant<wal_commit_after_issue, wal_abort_after_issue>;

    template <typename nvme_sched_t>
    inline auto
    issue_wal_fragment_plan_bounded(nvme_sched_t* nvme_sched,
                                    wal::wal_append_plan plan) {
        wal::validate_wal_append_config(plan.config);
        const uint64_t plan_id = plan.plan_id;
        const auto cursor_after = plan.cursor_after;
        const bool fragment_done = plan.fragment_done;
        const auto config = plan.config;
        const auto write_count = plan.writes.size();

        return just()
            >> push_context(std::move(plan))
            >> get_context<wal::wal_append_plan>()
            >> flat_map([nvme_sched,
                          write_count,
                          config](wal::wal_append_plan& plan) {
                return nvme::write_frame_range_bounded_fua(
                    nvme_sched,
                    std::span<wal::wal_frame_write>(
                        plan.writes.data(), write_count),
                    config.max_fua_inflight,
                    [](wal::wal_frame_write& write) {
                        return write.desc;
                    });
            })
            >> then([plan_id, cursor_after, fragment_done](bool fua_ok) {
                return wal_plan_issue_result{
                    .plan_id = plan_id,
                    .cursor_after = cursor_after,
                    .fragment_done = fragment_done,
                    .fua_ok = fua_ok,
                };
            })
            >> any_exception([plan_id,
                              cursor_after,
                              fragment_done](std::exception_ptr ep) {
                return just(wal_plan_issue_result{
                    .plan_id = plan_id,
                    .cursor_after = cursor_after,
                    .fragment_done = fragment_done,
                    .fua_ok = false,
                    .exception = std::move(ep),
                });
            })
            >> pop_context();
    }

    inline auto
    finish_wal_plan_after_issue(front::front_sched* sched,
                                wal_plan_issue_result issue) {
        return just(std::move(issue))
            >> then([sched](wal_plan_issue_result issue)
                        -> wal_finish_after_issue {
                if (issue.fua_ok && issue.exception == nullptr) {
                    return wal_commit_after_issue{
                        .sched = sched,
                        .plan_id = issue.plan_id,
                        .cursor_after = issue.cursor_after,
                        .fragment_done = issue.fragment_done,
                    };
                }
                return wal_abort_after_issue{
                    .sched = sched,
                    .plan_id = issue.plan_id,
                    .had_exception = issue.exception != nullptr,
                };
            })
            >> visit()
            >> flat_map([]<typename T>(T&& alt) {
                using alt_t = std::decay_t<T>;
                if constexpr (std::is_same_v<alt_t, wal_commit_after_issue>) {
                    return front::commit_wal_plan(*alt.sched, alt.plan_id)
                        >> then([alt](std::optional<wal::sealed_segment_info>
                                          sealed) {
                            return wal_plan_completion{
                                .cursor_after = alt.cursor_after,
                                .fragment_done = alt.fragment_done,
                                .sealed = std::move(sealed),
                            };
                        });
                } else {
                    return front::abort_wal_plan(*alt.sched, alt.plan_id)
                        >> then([alt]() -> wal_plan_completion {
                            if (alt.had_exception) {
                                throw wal::wal_append_error(
                                    wal::wal_append_error_reason::device_failure,
                                    "write_path::write_wal_fragment: WAL FUA write raised exception");
                            }
                            throw wal::wal_append_error(
                                wal::wal_append_error_reason::device_failure,
                                "write_path::write_wal_fragment: WAL FUA write failed");
                        });
                }
            });
    }

    template <typename nvme_sched_t>
    inline auto
    issue_and_finish_wal_plan(front::front_sched* front_sched,
                              nvme_sched_t* nvme_sched,
                              wal::wal_append_plan plan) {
        return issue_wal_fragment_plan_bounded(nvme_sched, std::move(plan))
            >> flat_map([front_sched](wal_plan_issue_result issue) {
                return finish_wal_plan_after_issue(
                    front_sched, std::move(issue));
            });
    }

    struct wal_fragment_write_state {
        front::front_sched* front_sched = nullptr;
        wal::wal_space_sched* wal_sched = nullptr;
        core::front_fragment fragment;
        std::span<const core::canonical_entry> canonical_entries;
        wal::wal_fragment_cursor cursor{};
        std::optional<wal::sealed_segment_info> sealed_for_alloc;
        std::exception_ptr error{};
        bool done = false;
    };

    inline pump::coro::return_yields<bool>
    wal_fragment_not_done(const wal_fragment_write_state& state) {
        while (!state.done) co_yield true;
        co_return false;
    }

    inline auto
    allocate_and_install_wal_segment(wal_fragment_write_state& state,
                                     wal::wal_prepare_needs_segment&& needs) {
        auto sealed = std::move(state.sealed_for_alloc);
        state.sealed_for_alloc.reset();
        if (!sealed && needs.sealed.has_value()) {
            sealed = std::move(needs.sealed);
        }

        return wal::alloc_segment(
                *state.wal_sched, needs.stream_id, std::move(sealed))
            >> flat_map([&state](wal::segment_runtime* segment) {
                return front::install_wal_segment(
                    *state.front_sched, segment);
            })
            >> then([]() { return true; });
    }

    template <typename nvme_sched_t>
    inline auto
    write_wal_fragment_step(wal_fragment_write_state& state,
                            nvme_sched_t* nvme_sched) {
        return just(bool{state.done})
            >> then([](bool done)
                        -> std::variant<std::true_type, std::false_type> {
                if (done) return std::true_type{};
                return std::false_type{};
            })
            >> visit()
            >> flat_map([&state, nvme_sched]<typename Flag>(Flag&&) {
                using flag_t = std::decay_t<Flag>;
                if constexpr (std::is_same_v<flag_t, std::true_type>) {
                    return just(true);
                } else {
                    return front::prepare_wal_fragment(
                            *state.front_sched,
                            state.fragment,
                            state.canonical_entries,
                            state.cursor)
                        >> visit()
                        >> flat_map([&state, nvme_sched]<typename T>(
                                T&& alt) {
                            using alt_t = std::decay_t<T>;
                            if constexpr (std::is_same_v<
                                              alt_t,
                                              wal::wal_prepare_needs_segment>) {
                                return allocate_and_install_wal_segment(
                                    state, std::move(alt));
                            } else {
                                static_assert(std::is_same_v<
                                    alt_t, wal::wal_prepare_ready>);
                                return issue_and_finish_wal_plan(
                                        state.front_sched,
                                        nvme_sched,
                                        std::move(alt.plan))
                                    >> then([&state](
                                            wal_plan_completion&& completion) {
                                        state.cursor = completion.cursor_after;
                                        state.done = completion.fragment_done;
                                        if (completion.sealed.has_value()) {
                                            state.sealed_for_alloc =
                                                std::move(completion.sealed);
                                        }
                                        return true;
                                    });
                            }
                        });
                }
            });
    }

    template <typename nvme_sched_t = nvme::runtime_scheduler>
    inline auto
    write_wal_fragment(front::front_sched& front_sched,
                       wal::wal_space_sched& wal_sched,
                       nvme_sched_t* nvme_sched,
                       core::front_fragment fragment,
                       std::span<const core::canonical_entry>
                           canonical_entries) {
        const bool done = fragment.entry_indices.empty();
        return just()
            >> push_context(wal_fragment_write_state{
                .front_sched = &front_sched,
                .wal_sched = &wal_sched,
                .fragment = std::move(fragment),
                .canonical_entries = canonical_entries,
                .done = done,
            })
            >> get_context<wal_fragment_write_state>()
            >> flat_map([nvme_sched](wal_fragment_write_state& state) {
                return just()
                    >> for_each(pump::coro::make_view_able(
                        wal_fragment_not_done(state)))
                    >> flat_map([&state, nvme_sched](bool) {
                        return write_wal_fragment_step(state, nvme_sched)
                            >> any_exception([&state](std::exception_ptr ep) {
                                state.error = std::move(ep);
                                state.done = true;
                                return just(false);
                            });
                    })
                    >> all()
                    >> then([&state](bool) {
                        if (state.error) {
                            std::rethrow_exception(state.error);
                        }
                        return true;
                    });
            })
            >> pop_context();
    }

}  // namespace apps::inconel::write_path

#endif  // APPS_INCONEL_WRITE_PATH_SENDER_HH
