#ifndef APPS_INCONEL_RUNTIME_MAINTENANCE_SCHEDULER_HH
#define APPS_INCONEL_RUNTIME_MAINTENANCE_SCHEDULER_HH

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/context.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/submit.hh"

#include "../core/panic.hh"
#include "./operations.hh"

namespace apps::inconel::runtime {

    struct maintenance_options {
        bool enabled = true;
        int32_t core = -1;

        uint32_t active_gap_ticks = 1;
        uint32_t idle_initial_backoff_ticks = 256;
        uint32_t idle_max_backoff_ticks = 1u << 20;
        uint32_t completion_queue_depth = 4;
    };

    struct maintenance_stats_snapshot {
        bool enabled = false;
        bool stopping = false;
        bool inflight = false;
        uint32_t cooldown_ticks = 0;
        uint32_t idle_backoff_ticks = 0;
        uint64_t launched_rounds = 0;
        uint64_t completed_rounds = 0;
        uint64_t failed_rounds = 0;
        uint64_t work_rounds = 0;
        uint64_t noop_rounds = 0;
    };

    namespace _maintenance_finish { struct req; }
    namespace _maintenance_fail { struct req; }

    [[nodiscard]] inline uint32_t
    checked_maintenance_completion_queue_depth(uint32_t depth) {
        if (depth < 2 || (depth & (depth - 1)) != 0) {
            core::panic_inconsistency(
                "runtime::maintenance_sched::ctor",
                "completion_queue_depth must be a power of two and >= 2");
        }
        return depth;
    }

    struct maintenance_sched {
        enum class mode : uint8_t {
            running,
            stopping,
            disabled,
        };

        std::atomic<mode> st{mode::disabled};
        std::atomic<bool> inflight{false};
        std::atomic<bool> disable_requested{false};

        uint32_t active_gap_ticks = 1;
        uint32_t idle_initial_backoff_ticks = 256;
        uint32_t idle_max_backoff_ticks = 1u << 20;
        std::atomic<uint32_t> cooldown_ticks{0};
        std::atomic<uint32_t> idle_backoff_ticks{256};

        std::atomic<uint64_t> launched_rounds{0};
        std::atomic<uint64_t> completed_rounds{0};
        std::atomic<uint64_t> failed_rounds{0};
        std::atomic<uint64_t> work_rounds{0};
        std::atomic<uint64_t> noop_rounds{0};

        pump::core::per_core::queue<_maintenance_finish::req*, false> finish_q;
        pump::core::per_core::queue<_maintenance_fail::req*, false> fail_q;

        explicit maintenance_sched(maintenance_options opts = {});

        bool advance();

        template <typename Runtime>
        bool advance(Runtime&) { return advance(); }

        void request_disable() noexcept {
            disable_requested.store(true, std::memory_order_release);
        }

        [[nodiscard]] bool quiesced() const noexcept {
            return !inflight.load(std::memory_order_acquire);
        }

        [[nodiscard]] maintenance_stats_snapshot snapshot() const noexcept {
            const auto state = st.load(std::memory_order_acquire);
            return maintenance_stats_snapshot{
                .enabled = state != mode::disabled,
                .stopping = state == mode::stopping,
                .inflight = inflight.load(std::memory_order_acquire),
                .cooldown_ticks =
                    cooldown_ticks.load(std::memory_order_acquire),
                .idle_backoff_ticks =
                    idle_backoff_ticks.load(std::memory_order_acquire),
                .launched_rounds =
                    launched_rounds.load(std::memory_order_acquire),
                .completed_rounds =
                    completed_rounds.load(std::memory_order_acquire),
                .failed_rounds =
                    failed_rounds.load(std::memory_order_acquire),
                .work_rounds = work_rounds.load(std::memory_order_acquire),
                .noop_rounds = noop_rounds.load(std::memory_order_acquire),
            };
        }

        auto submit_finish_round(maintenance_round_result result);
        auto submit_fail_round(std::exception_ptr error);
        bool schedule_finish(_maintenance_finish::req* r) noexcept;
        bool schedule_fail(_maintenance_fail::req* r) noexcept;

    private:
        void launch_round();
        void finish_round(maintenance_round_result result);
        void fail_round(std::exception_ptr error);
    };

    namespace _maintenance_finish {

        struct req {
            maintenance_round_result result;
            std::move_only_function<void()> cb;
        };

        struct op {
            constexpr static bool maintenance_finish_op = true;
            maintenance_sched* sched;
            maintenance_round_result result;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            maintenance_sched* sched;
            maintenance_round_result result;

            auto make_op() {
                return op{.sched = sched, .result = std::move(result)};
            }

            template <typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _maintenance_finish

    namespace _maintenance_fail {

        struct req {
            std::exception_ptr error;
        };

        struct op {
            constexpr static bool maintenance_fail_op = true;
            maintenance_sched* sched;
            std::exception_ptr error;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            maintenance_sched* sched;
            std::exception_ptr error;

            auto make_op() {
                return op{.sched = sched, .error = std::move(error)};
            }

            template <typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _maintenance_fail

    [[nodiscard]] inline std::string
    describe_exception(std::exception_ptr error) {
        if (!error) {
            return "unknown exception";
        }
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            return e.what();
        } catch (...) {
            return "unknown exception";
        }
    }

    inline maintenance_sched::maintenance_sched(maintenance_options opts)
        : st(opts.enabled ? mode::running : mode::disabled)
        , active_gap_ticks(opts.active_gap_ticks)
        , idle_initial_backoff_ticks(opts.idle_initial_backoff_ticks)
        , idle_max_backoff_ticks(opts.idle_max_backoff_ticks)
        , cooldown_ticks(0)
        , idle_backoff_ticks(opts.idle_initial_backoff_ticks)
        , finish_q(checked_maintenance_completion_queue_depth(
              opts.completion_queue_depth))
        , fail_q(checked_maintenance_completion_queue_depth(
              opts.completion_queue_depth)) {
        if (opts.idle_initial_backoff_ticks == 0 ||
            opts.idle_max_backoff_ticks == 0 ||
            opts.idle_initial_backoff_ticks > opts.idle_max_backoff_ticks) {
            core::panic_inconsistency(
                "runtime::maintenance_sched::ctor",
                "invalid maintenance options");
        }
    }

    inline bool
    maintenance_sched::schedule_finish(_maintenance_finish::req* r) noexcept {
        return finish_q.try_enqueue(r);
    }

    inline bool
    maintenance_sched::schedule_fail(_maintenance_fail::req* r) noexcept {
        return fail_q.try_enqueue(r);
    }

    inline auto
    maintenance_sched::submit_finish_round(maintenance_round_result result) {
        return _maintenance_finish::sender{
            .sched = this,
            .result = std::move(result),
        };
    }

    inline auto
    maintenance_sched::submit_fail_round(std::exception_ptr error) {
        return _maintenance_fail::sender{
            .sched = this,
            .error = std::move(error),
        };
    }

    inline void
    maintenance_sched::launch_round() {
        launched_rounds.fetch_add(1, std::memory_order_acq_rel);
        inflight.store(true, std::memory_order_release);

        try {
            auto ctx = pump::core::make_root_context();
            rt::maintenance_once()
                >> pump::sender::flat_map(
                    [this](maintenance_round_result result) mutable {
                        return submit_finish_round(std::move(result));
                    })
                >> pump::sender::any_exception(
                    [this](std::exception_ptr error) mutable {
                        return submit_fail_round(std::move(error));
                    })
                >> pump::sender::submit(ctx);
        } catch (...) {
            fail_round(std::current_exception());
        }
    }

    inline void
    maintenance_sched::finish_round(maintenance_round_result result) {
        completed_rounds.fetch_add(1, std::memory_order_acq_rel);

        if (result.did_work()) {
            work_rounds.fetch_add(1, std::memory_order_acq_rel);
            idle_backoff_ticks.store(
                idle_initial_backoff_ticks, std::memory_order_release);
            cooldown_ticks.store(active_gap_ticks, std::memory_order_release);
        } else {
            noop_rounds.fetch_add(1, std::memory_order_acq_rel);
            const auto backoff =
                idle_backoff_ticks.load(std::memory_order_acquire);
            cooldown_ticks.store(backoff, std::memory_order_release);
            const auto next_backoff = std::min(
                idle_max_backoff_ticks,
                backoff > idle_max_backoff_ticks / 2
                    ? idle_max_backoff_ticks
                    : backoff * 2);
            idle_backoff_ticks.store(
                next_backoff, std::memory_order_release);
        }
        inflight.store(false, std::memory_order_release);
    }

    inline void
    maintenance_sched::fail_round(std::exception_ptr error) {
        failed_rounds.fetch_add(1, std::memory_order_acq_rel);
        inflight.store(false, std::memory_order_release);
        const auto message = describe_exception(error);
        core::panic_inconsistency(
            "runtime::maintenance_sched",
            "maintenance round failed: %s",
            message.c_str());
    }

    inline bool
    maintenance_sched::advance() {
        bool progress = false;

        if (auto item = fail_q.try_dequeue()) {
            auto* r = *item;
            auto error = std::move(r->error);
            delete r;
            fail_round(std::move(error));
            progress = true;
        }

        if (auto item = finish_q.try_dequeue()) {
            auto* r = *item;
            auto result = std::move(r->result);
            auto cb = std::move(r->cb);
            delete r;
            finish_round(std::move(result));
            cb();
            progress = true;
        }

        auto state = st.load(std::memory_order_acquire);
        if (disable_requested.load(std::memory_order_acquire) &&
            state == mode::running) {
            st.store(mode::stopping, std::memory_order_release);
            state = mode::stopping;
        }
        if (state != mode::running) {
            if (!inflight.load(std::memory_order_acquire)) {
                st.store(mode::disabled, std::memory_order_release);
            }
            return progress;
        }
        if (inflight.load(std::memory_order_acquire)) {
            return progress;
        }
        const auto cooldown =
            cooldown_ticks.load(std::memory_order_acquire);
        if (cooldown > 0) {
            cooldown_ticks.store(cooldown - 1, std::memory_order_release);
            return progress;
        }

        launch_round();
        return true;
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _maintenance_finish::op::start(ctx_t& ctx, scope_t& scope) {
        auto* r = new req{
            .result = std::move(result),
            .cb = [ctx = ctx, scope = scope]() mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope);
            },
        };
        if (!sched->schedule_finish(r)) {
            delete r;
            core::panic_inconsistency(
                "runtime::_maintenance_finish::op::start",
                "finish queue full");
        }
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _maintenance_fail::op::start(ctx_t& ctx, scope_t& scope) {
        auto* r = new req{.error = std::move(error)};
        if (!sched->schedule_fail(r)) {
            auto message = describe_exception(r->error);
            delete r;
            core::panic_inconsistency(
                "runtime::_maintenance_fail::op::start",
                "fail queue full after maintenance failure: %s",
                message.c_str());
        }
        pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
    }

    template <typename Runtime>
    inline void
    request_maintenance_disable(Runtime* rt) {
        for (auto* sched : rt->template get_schedulers<maintenance_sched>()) {
            if (sched != nullptr) {
                sched->request_disable();
            }
        }
    }

    template <typename Runtime>
    [[nodiscard]] inline bool
    all_maintenance_quiesced(Runtime* rt) {
        for (auto* sched : rt->template get_schedulers<maintenance_sched>()) {
            if (sched != nullptr && !sched->quiesced()) {
                return false;
            }
        }
        return true;
    }

    template <typename Runtime>
    inline void
    disable_maintenance_and_wait(Runtime* rt) {
        request_maintenance_disable(rt);

        while (!all_maintenance_quiesced(rt)) {
            std::this_thread::yield();
        }
    }

}  // namespace apps::inconel::runtime

namespace pump::core {

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<
                    typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::maintenance_finish_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(
                ctx, scope);
        }
    };

    template <typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::runtime::_maintenance_finish::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<
                    typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::maintenance_fail_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(
                ctx, scope);
        }
    };

    template <typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::runtime::_maintenance_fail::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_RUNTIME_MAINTENANCE_SCHEDULER_HH
