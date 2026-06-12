#ifndef APPS_INCONEL_WRITE_PATH_SENDER_HH
#define APPS_INCONEL_WRITE_PATH_SENDER_HH

#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

#include "../core/batch_carrier.hh"
#include "../front/sender.hh"
#include "../nvme/frame_io.hh"
#include "../runtime/facade.hh"
#include "../value/sender.hh"
#include "../wal/sender.hh"

namespace apps::inconel::write_path {

    using namespace pump::sender;

    template <typename NvmeProvider = value::local_nvme_provider>
    inline auto
    persist_put_values(core::batch_ctx& ctx, NvmeProvider nvme = {}) {
        using skip_variant = std::variant<std::true_type, std::false_type>;
        return just(ctx.put_entry_indices.empty()
                    ? skip_variant{std::true_type{}}
                    : skip_variant{std::false_type{}})
            >> visit()
            >> flat_map([&ctx, nvme]<typename Flag>(Flag&&) mutable {
                using flag_t = std::decay_t<Flag>;
                if constexpr (std::is_same_v<flag_t, std::true_type>) {
                    return just(true);
                } else {
                    const uint32_t max_len = rt::value()->max_body_len();
                    for (uint32_t idx : ctx.put_entry_indices) {
                        if (ctx.canonical_entries[idx].value.size() > max_len) {
                            throw value::value_persist_error(
                                value::value_persist_error_reason::oversized_value,
                                "write_path::persist_put_values: value body exceeds max_body_len");
                        }
                    }

                    std::vector<value::put_entry> puts;
                    puts.reserve(ctx.put_entry_indices.size());
                    for (uint32_t idx : ctx.put_entry_indices) {
                        auto& entry = ctx.canonical_entries[idx];
                        puts.push_back(value::put_entry{
                            .body   = entry.value,
                            .out_vr = &entry.allocated_vr,
                        });
                    }

                    return just()
                        >> push_context(std::move(puts))
                        >> get_context<std::vector<value::put_entry>>()
                        >> flat_map([nvme](
                                std::vector<value::put_entry>& puts) mutable {
                            return value::persist_put_values(
                                std::span<value::put_entry>(
                                    puts.data(), puts.size()),
                                nvme);
                        })
                        >> then([](bool ok) {
                            if (!ok) {
                                throw value::value_persist_error(
                                    value::value_persist_error_reason::round_failed,
                                    "write_path::persist_put_values: value persist round failed");
                            }
                            return true;
                        })
                        >> pop_context();
                }
            });
    }

    struct wal_plan_issue_result {
        uint64_t plan_id = 0;
        wal::wal_fragment_cursor cursor_after{};
        bool fragment_done = false;
        bool fua_ok = false;
        std::exception_ptr exception{};
        std::vector<wal::wal_frame_write> writes;
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
        std::vector<wal::wal_frame_write> writes;
    };

    struct wal_abort_after_issue {
        front::front_sched* sched = nullptr;
        uint64_t plan_id = 0;
        bool had_exception = false;
        std::vector<wal::wal_frame_write> writes;
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
                          plan_id,
                          cursor_after,
                          fragment_done,
                          write_count,
                          config](wal::wal_append_plan& plan) {
                return nvme::write_frame_range_bounded_fua(
                    nvme_sched,
                    std::span<wal::wal_frame_write>(
                        plan.writes.data(), write_count),
                    config.max_fua_inflight,
                    [](wal::wal_frame_write& write) {
                        return write.desc;
                    })
                    >> then([&plan,
                             plan_id,
                             cursor_after,
                             fragment_done](bool fua_ok) {
                        return wal_plan_issue_result{
                            .plan_id = plan_id,
                            .cursor_after = cursor_after,
                            .fragment_done = fragment_done,
                            .fua_ok = fua_ok,
                            .writes = std::move(plan.writes),
                        };
                    })
                    >> any_exception([&plan,
                                      plan_id,
                                      cursor_after,
                                      fragment_done](std::exception_ptr ep) {
                        return just(wal_plan_issue_result{
                            .plan_id = plan_id,
                            .cursor_after = cursor_after,
                            .fragment_done = fragment_done,
                            .fua_ok = false,
                            .exception = std::move(ep),
                            .writes = std::move(plan.writes),
                        });
                    });
            })
            >> pop_context();
    }

    inline auto
    finish_wal_plan_after_issue(front::front_sched* sched,
                                wal_plan_issue_result issue) {
        return just(std::move(issue))
            >> then([sched](auto&& issue)
                        -> wal_finish_after_issue {
                if (issue.fua_ok && issue.exception == nullptr) {
                    return wal_commit_after_issue{
                        .sched = sched,
                        .plan_id = issue.plan_id,
                        .cursor_after = issue.cursor_after,
                        .fragment_done = issue.fragment_done,
                        .writes = std::move(issue.writes),
                    };
                }
                return wal_abort_after_issue{
                    .sched = sched,
                    .plan_id = issue.plan_id,
                    .had_exception = issue.exception != nullptr,
                    .writes = std::move(issue.writes),
                };
            })
            >> visit()
            >> flat_map([]<typename T>(T&& alt) {
                using alt_t = std::decay_t<T>;
                if constexpr (std::is_same_v<alt_t, wal_commit_after_issue>) {
                    auto cursor_after = alt.cursor_after;
                    const bool fragment_done = alt.fragment_done;
                    return front::commit_wal_plan(
                            *alt.sched, alt.plan_id, std::move(alt.writes))
                        >> then([cursor_after, fragment_done](
                                      std::optional<wal::sealed_segment_info>
                                          sealed) {
                            return wal_plan_completion{
                                .cursor_after = cursor_after,
                                .fragment_done = fragment_done,
                                .sealed = std::move(sealed),
                            };
                        });
                } else {
                    const bool had_exception = alt.had_exception;
                    return front::abort_wal_plan(
                            *alt.sched, alt.plan_id, std::move(alt.writes))
                        >> then([had_exception]() -> wal_plan_completion {
                            if (had_exception) {
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
            >> flat_map([front_sched](auto&& issue) {
                return finish_wal_plan_after_issue(
                    front_sched, std::move(issue));
            });
    }

    struct wal_fragment_write_state {
        front::front_sched* front_sched = nullptr;
        wal::wal_space_sched* wal_sched = nullptr;
        const core::front_fragment* fragment = nullptr;
        std::span<const core::canonical_entry> canonical_entries;
        wal::wal_fragment_cursor cursor{};
        std::optional<wal::sealed_segment_info> sealed_for_alloc;
        std::exception_ptr error{};
        bool done = false;
    };

    inline pump::coro::return_yields<bool>
    wal_fragment_not_done(const wal_fragment_write_state& state) {
        while (!state.done && state.fragment != nullptr &&
               state.cursor.next_fragment_entry <
                   state.fragment->entry_indices.size()) {
            co_yield true;
        }
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
        using gate_t = std::variant<std::true_type, std::false_type>;
        const bool skip =
            state.done || state.fragment == nullptr ||
            state.cursor.next_fragment_entry >=
                state.fragment->entry_indices.size();
        return just(skip ? gate_t{std::true_type{}}
                         : gate_t{std::false_type{}})
            >> visit()
            >> flat_map([&state, nvme_sched]<typename Gate>(Gate&&) {
                using gate_t = std::decay_t<Gate>;
                if constexpr (std::is_same_v<gate_t, std::true_type>) {
                    state.done = true;
                    return just(true);
                } else {
                    return front::prepare_wal_fragment(
                            *state.front_sched,
                            *state.fragment,
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
                       const core::front_fragment& fragment,
                       std::span<const core::canonical_entry>
                           canonical_entries) {
        using done_variant = std::variant<std::true_type, std::false_type>;
        return just(fragment.entry_indices.empty()
                    ? done_variant{std::true_type{}}
                    : done_variant{std::false_type{}})
            >> visit()
            >> flat_map([&front_sched,
                          &wal_sched,
                          nvme_sched,
                          &fragment,
                          canonical_entries]<typename Flag>(Flag&&) {
                using flag_t = std::decay_t<Flag>;
                if constexpr (std::is_same_v<flag_t, std::true_type>) {
                    return just(true);
                } else {
                    return just()
                        >> push_context(wal_fragment_write_state{
                            .front_sched = &front_sched,
                            .wal_sched = &wal_sched,
                            .fragment = &fragment,
                            .canonical_entries = canonical_entries,
                        })
                        >> get_context<wal_fragment_write_state>()
                        >> flat_map([nvme_sched](
                                wal_fragment_write_state& state) {
                            return just()
                                >> for_each(pump::coro::make_view_able(
                                    wal_fragment_not_done(state)))
                                >> flat_map([&state, nvme_sched](bool) {
                                    return write_wal_fragment_step(
                                            state, nvme_sched)
                                        >> any_exception([&state](
                                                std::exception_ptr ep) {
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
            });
    }

}  // namespace apps::inconel::write_path

#endif  // APPS_INCONEL_WRITE_PATH_SENDER_HH
