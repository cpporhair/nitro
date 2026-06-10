#ifndef APPS_INCONEL_COORD_SCHEDULER_HH
#define APPS_INCONEL_COORD_SCHEDULER_HH

// Coord scheduler M03: LSN assignment, terminal publish/release prefix
// tracking, publish-gate buffering, and CAT/PRS read-handle acquisition.
//
// This file intentionally does not implement WAL append, front owner/value
// persistence, write-batch pipeline orchestration, tree lookup, runtime API,
// seal/frontier switching, or recovery.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

#include "../core/batch_carrier.hh"
#include "../core/read_catalog.hh"

namespace apps::inconel::coord {

    class ready_window {
      public:
        ready_window(uint64_t base_lsn, std::size_t window_size)
            : base_lsn_(base_lsn)
            , window_size_(window_size)
            , bits_((window_size + kBitsPerWord - 1) / kBitsPerWord, 0) {
            if (window_size == 0) {
                throw std::invalid_argument(
                    "coord::ready_window: window_size must be nonzero");
            }
        }

        [[nodiscard]] uint64_t
        next_unresolved_lsn() const noexcept {
            return base_lsn_;
        }

        [[nodiscard]] std::size_t
        window_size() const noexcept {
            return window_size_;
        }

        [[nodiscard]] bool
        has_assign_capacity(uint64_t next_lsn) const {
            if (next_lsn < base_lsn_) {
                throw std::logic_error(
                    "coord::ready_window: next_lsn is behind unresolved base");
            }
            return next_lsn - base_lsn_ < window_size_;
        }

        void
        mark_resolved(uint64_t lsn, uint64_t assigned_next_lsn) {
            if (lsn < base_lsn_) {
                throw std::logic_error(
                    "coord::ready_window: terminal LSN was already consumed");
            }
            if (lsn >= assigned_next_lsn) {
                throw std::logic_error(
                    "coord::ready_window: terminal LSN was never assigned");
            }
            if (lsn - base_lsn_ >= window_size_) {
                throw std::logic_error(
                    "coord::ready_window: terminal LSN is outside ready window");
            }
            if (test_bit(lsn)) {
                throw std::logic_error(
                    "coord::ready_window: duplicate terminal LSN");
            }
            set_bit(lsn);
        }

        [[nodiscard]] uint64_t
        advance_contiguous_prefix(uint64_t scan_from_lsn) {
            uint64_t resolved = scan_from_lsn;
            uint64_t lsn = scan_from_lsn + 1;

            while (lsn >= base_lsn_ &&
                   lsn - base_lsn_ < window_size_ &&
                   test_bit(lsn)) {
                clear_bit(lsn);
                if (lsn == base_lsn_) {
                    ++base_lsn_;
                }
                resolved = lsn;
                ++lsn;
            }

            return resolved;
        }

      private:
        static constexpr std::size_t kBitsPerWord = 64;

        [[nodiscard]] std::size_t
        bit_index(uint64_t lsn) const noexcept {
            return static_cast<std::size_t>(lsn % window_size_);
        }

        [[nodiscard]] bool
        test_bit(uint64_t lsn) const noexcept {
            const std::size_t idx = bit_index(lsn);
            return (bits_[idx / kBitsPerWord] &
                    (uint64_t{1} << (idx % kBitsPerWord))) != 0;
        }

        void
        set_bit(uint64_t lsn) noexcept {
            const std::size_t idx = bit_index(lsn);
            bits_[idx / kBitsPerWord] |=
                (uint64_t{1} << (idx % kBitsPerWord));
        }

        void
        clear_bit(uint64_t lsn) noexcept {
            const std::size_t idx = bit_index(lsn);
            bits_[idx / kBitsPerWord] &=
                ~(uint64_t{1} << (idx % kBitsPerWord));
        }

        uint64_t              base_lsn_;
        std::size_t           window_size_;
        std::vector<uint64_t> bits_;
    };

    class publish_gate {
      public:
        [[nodiscard]] bool
        is_open() const noexcept {
            return open_;
        }

        [[nodiscard]] uint64_t
        pending_prefix() const noexcept {
            return pending_prefix_;
        }

        void
        close() noexcept {
            if (open_) {
                pending_prefix_ = 0;
            }
            open_ = false;
        }

        [[nodiscard]] uint64_t
        open_and_take_pending() noexcept {
            open_ = true;
            const uint64_t pending = pending_prefix_;
            pending_prefix_ = 0;
            return pending;
        }

        void
        note_pending(uint64_t prefix) noexcept {
            if (prefix > pending_prefix_) {
                pending_prefix_ = prefix;
            }
        }

      private:
        bool     open_ = true;
        uint64_t pending_prefix_ = 0;
    };

    struct coord_sched;

    namespace _coord_assign  { struct req; struct sender; }
    namespace _coord_publish { struct req; struct sender; }
    namespace _coord_release { struct req; struct sender; }
    namespace _coord_read    { struct req; struct sender; }

    struct coord_sched {
        explicit
        coord_sched(std::shared_ptr<const core::publish_catalog> initial_cat,
                    uint32_t front_count,
                    uint64_t next_lsn = 1,
                    std::size_t ready_window_size = 65536,
                    std::size_t queue_depth = 1024)
            : assign_q_(queue_depth)
            , publish_q_(queue_depth)
            , release_q_(queue_depth)
            , read_q_(queue_depth)
            , cats_(validate_initial_cat(initial_cat,
                                         front_count,
                                         next_lsn,
                                         ready_window_size))
            , ready_(initial_cat->durable_lsn.load(std::memory_order_acquire) + 1,
                     ready_window_size)
            , next_lsn_(next_lsn)
            , front_count_(front_count)
            , cat_epoch_(initial_cat->epoch) {}

        ~coord_sched();

        coord_sched(const coord_sched&) = delete;
        coord_sched&
        operator=(const coord_sched&) = delete;
        coord_sched(coord_sched&&) = delete;
        coord_sched&
        operator=(coord_sched&&) = delete;

        [[nodiscard]] _coord_assign::sender
        assign_batch_lsn(core::client_batch_buffer&& input);

        [[nodiscard]] _coord_publish::sender
        publish_batch(uint64_t batch_lsn);

        // release_batch is only valid before the caller enters the memtable
        // phase. Coord deliberately tracks only terminal LSN resolution and
        // cannot infer that phase boundary from this value-only M03 surface.
        [[nodiscard]] _coord_release::sender
        release_batch(uint64_t batch_lsn);

        [[nodiscard]] _coord_read::sender
        acquire_read_handle();

        [[nodiscard]] core::batch_ctx
        assign_batch_lsn_for_testing(core::client_batch_buffer&& input) {
            validate_assign_input(input);
            if (!ready_.has_assign_capacity(next_lsn_)) {
                throw std::runtime_error(
                    "coord::coord_sched: assign window is full");
            }
            return assign_validated_now(std::move(input));
        }

        void
        publish_batch_for_testing(uint64_t batch_lsn) {
            resolve_terminal_lsn(batch_lsn);
        }

        void
        release_batch_for_testing(uint64_t batch_lsn) {
            resolve_terminal_lsn(batch_lsn);
        }

        [[nodiscard]] core::read_handle
        acquire_read_handle_for_testing() const {
            return cats_.acquire_read_handle();
        }

        void
        close_gate_for_testing() noexcept {
            gate_.close();
        }

        void
        open_gate_for_testing() {
            apply_pending_gate_prefix(gate_.open_and_take_pending());
            drain_pending_assigns();
        }

        void
        install_cat_for_testing(
            std::shared_ptr<const core::publish_catalog> cat) {
            validate_replacement_cat(cat);
            cat_epoch_ = cat->epoch;
            cats_.install_cat(std::move(cat));
        }

        void
        enqueue_assign_batch_lsn_for_testing(
            core::client_batch_buffer&& input,
            std::move_only_function<void(core::batch_ctx&&)> cb,
            std::move_only_function<void(std::exception_ptr)> fail = {});

        [[nodiscard]] uint64_t
        next_lsn_for_testing() const noexcept {
            return next_lsn_;
        }

        [[nodiscard]] uint64_t
        ready_base_for_testing() const noexcept {
            return ready_.next_unresolved_lsn();
        }

        [[nodiscard]] bool
        gate_open_for_testing() const noexcept {
            return gate_.is_open();
        }

        [[nodiscard]] uint64_t
        gate_pending_for_testing() const noexcept {
            return gate_.pending_prefix();
        }

        [[nodiscard]] uint32_t
        front_count() const noexcept {
            return front_count_;
        }

        [[nodiscard]] uint64_t
        cat_epoch() const noexcept {
            return cat_epoch_;
        }

        void schedule_assign(_coord_assign::req* r);
        void schedule_publish(_coord_publish::req* r);
        void schedule_release(_coord_release::req* r);
        void schedule_read(_coord_read::req* r);

        bool advance();

        template<typename runtime_t>
        bool advance(runtime_t&) { return advance(); }

      private:
        static constexpr uint32_t kMaxAssignPerAdvance = 64;
        static constexpr uint32_t kMaxPublishPerAdvance = 256;
        static constexpr uint32_t kMaxReleasePerAdvance = 256;
        static constexpr uint32_t kMaxReadPerAdvance = 128;

        static std::shared_ptr<const core::publish_catalog>
        validate_initial_cat(
            const std::shared_ptr<const core::publish_catalog>& cat,
            uint32_t front_count,
            uint64_t next_lsn,
            std::size_t ready_window_size) {
            if (!cat) {
                throw std::invalid_argument(
                    "coord::coord_sched: initial CAT must not be null");
            }
            if (front_count == 0) {
                throw std::invalid_argument(
                    "coord::coord_sched: front_count must be nonzero");
            }
            if (ready_window_size == 0) {
                throw std::invalid_argument(
                    "coord::coord_sched: ready_window_size must be nonzero");
            }
            if (!cat->prs || !cat->prs->fronts) {
                throw std::invalid_argument(
                    "coord::coord_sched: CAT must carry PRS/front snapshot");
            }
            if (cat->prs->fronts->size() != front_count) {
                throw std::invalid_argument(
                    "coord::coord_sched: front_count does not match PRS fronts");
            }
            const uint64_t durable =
                cat->durable_lsn.load(std::memory_order_acquire);
            if (durable == std::numeric_limits<uint64_t>::max() ||
                next_lsn <= durable) {
                throw std::invalid_argument(
                    "coord::coord_sched: next_lsn must be after durable_lsn");
            }
            return cat;
        }

        void
        validate_replacement_cat(
            const std::shared_ptr<const core::publish_catalog>& cat) const {
            if (!cat) {
                throw std::invalid_argument(
                    "coord::coord_sched: replacement CAT must not be null");
            }
            if (!cat->prs || !cat->prs->fronts ||
                cat->prs->fronts->size() != front_count_) {
                throw std::invalid_argument(
                    "coord::coord_sched: replacement CAT front_count mismatch");
            }
            const auto current = cats_.current_cat();
            const uint64_t visible =
                current->durable_lsn.load(std::memory_order_acquire);
            const uint64_t replacement_visible =
                cat->durable_lsn.load(std::memory_order_acquire);
            if (replacement_visible != visible) {
                throw std::logic_error(
                    "coord::coord_sched: replacement CAT visible durable "
                    "must match current CAT");
            }
        }

        static void
        validate_assign_input(const core::client_batch_buffer& input) {
            const core::client_batch_view view = input.view();
            if (view.op_count() == 0) {
                throw std::invalid_argument(
                    "coord::coord_sched: empty write batch");
            }
        }

        [[nodiscard]] core::batch_ctx
        assign_validated_now(core::client_batch_buffer&& input) {
            const uint64_t lsn = next_lsn_;
            core::batch_ctx ctx =
                core::build_batch_ctx(std::move(input), lsn, front_count_);
            if (ctx.entry_count == 0) {
                throw std::invalid_argument(
                    "coord::coord_sched: empty canonical write batch");
            }
            ++next_lsn_;
            return ctx;
        }

        bool
        try_complete_assign(_coord_assign::req* r);

        void
        handle_assign(_coord_assign::req* r);

        void
        handle_publish(_coord_publish::req* r);

        void
        handle_release(_coord_release::req* r);

        void
        handle_read(_coord_read::req* r);

        void
        resolve_terminal_lsn(uint64_t lsn) {
            ready_.mark_resolved(lsn, next_lsn_);

            const auto cat = cats_.current_cat();
            const uint64_t visible =
                cat->durable_lsn.load(std::memory_order_acquire);
            const uint64_t scan_from =
                gate_.is_open()
                    ? visible
                    : std::max(visible, gate_.pending_prefix());

            const uint64_t new_prefix =
                ready_.advance_contiguous_prefix(scan_from);
            if (new_prefix <= scan_from) {
                return;
            }

            if (gate_.is_open()) {
                if (new_prefix > visible) {
                    cat->durable_lsn.store(new_prefix,
                                           std::memory_order_release);
                }
            } else {
                gate_.note_pending(new_prefix);
            }

            drain_pending_assigns();
        }

        void
        apply_pending_gate_prefix(uint64_t pending_prefix) {
            if (pending_prefix == 0) return;

            const auto cat = cats_.current_cat();
            const uint64_t visible =
                cat->durable_lsn.load(std::memory_order_acquire);
            if (pending_prefix > visible) {
                cat->durable_lsn.store(pending_prefix,
                                       std::memory_order_release);
            }
        }

        void
        drain_pending_assigns() {
            while (!pending_assigns_.empty() &&
                   ready_.has_assign_capacity(next_lsn_)) {
                auto* r = pending_assigns_.front();
                pending_assigns_.pop_front();
                (void)try_complete_assign(r);
            }
        }

        pump::core::per_core::queue<_coord_assign::req*>  assign_q_;
        pump::core::per_core::queue<_coord_publish::req*> publish_q_;
        pump::core::per_core::queue<_coord_release::req*> release_q_;
        pump::core::per_core::queue<_coord_read::req*>    read_q_;

        core::catalog_store cats_;
        ready_window        ready_;
        publish_gate        gate_;
        std::deque<_coord_assign::req*> pending_assigns_;
        uint64_t            next_lsn_;
        uint32_t            front_count_;
        uint64_t            cat_epoch_;
    };

    namespace _coord_assign {

        struct req {
            core::client_batch_buffer                         input;
            std::move_only_function<void(core::batch_ctx&&)>  cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool coord_assign_op = true;
            coord_sched*              sched;
            core::client_batch_buffer input;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            coord_sched*              sched;
            core::client_batch_buffer input;

            sender(coord_sched* s, core::client_batch_buffer&& in)
                : sched(s), input(std::move(in)) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{.sched = sched, .input = std::move(input)};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }

    namespace _coord_publish {

        struct req {
            uint64_t batch_lsn;
            std::move_only_function<void()>                   cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool coord_publish_op = true;
            coord_sched* sched;
            uint64_t     batch_lsn;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            coord_sched* sched;
            uint64_t     batch_lsn;

            auto make_op() {
                return op{.sched = sched, .batch_lsn = batch_lsn};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }

    namespace _coord_release {

        struct req {
            uint64_t batch_lsn;
            std::move_only_function<void()>                   cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool coord_release_op = true;
            coord_sched* sched;
            uint64_t     batch_lsn;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            coord_sched* sched;
            uint64_t     batch_lsn;

            auto make_op() {
                return op{.sched = sched, .batch_lsn = batch_lsn};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }

    namespace _coord_read {

        struct req {
            std::move_only_function<void(core::read_handle&&)> cb;
            std::move_only_function<void(std::exception_ptr)>  fail;
        };

        struct op {
            constexpr static bool coord_read_op = true;
            coord_sched* sched;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            coord_sched* sched;

            auto make_op() {
                return op{.sched = sched};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }

    inline coord_sched::~coord_sched() {
        while (auto item = assign_q_.try_dequeue()) delete *item;
        while (auto item = publish_q_.try_dequeue()) delete *item;
        while (auto item = release_q_.try_dequeue()) delete *item;
        while (auto item = read_q_.try_dequeue()) delete *item;
        for (auto* r : pending_assigns_) delete r;
    }

    inline _coord_assign::sender
    coord_sched::assign_batch_lsn(core::client_batch_buffer&& input) {
        return _coord_assign::sender{this, std::move(input)};
    }

    inline _coord_publish::sender
    coord_sched::publish_batch(uint64_t batch_lsn) {
        return _coord_publish::sender{.sched = this, .batch_lsn = batch_lsn};
    }

    inline _coord_release::sender
    coord_sched::release_batch(uint64_t batch_lsn) {
        return _coord_release::sender{.sched = this, .batch_lsn = batch_lsn};
    }

    inline _coord_read::sender
    coord_sched::acquire_read_handle() {
        return _coord_read::sender{.sched = this};
    }

    inline void
    coord_sched::enqueue_assign_batch_lsn_for_testing(
        core::client_batch_buffer&& input,
        std::move_only_function<void(core::batch_ctx&&)> cb,
        std::move_only_function<void(std::exception_ptr)> fail) {
        schedule_assign(new _coord_assign::req{
            .input = std::move(input),
            .cb    = std::move(cb),
            .fail  = std::move(fail),
        });
    }

    inline void
    coord_sched::schedule_assign(_coord_assign::req* r) {
        if (!assign_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("coord::coord_sched: assign queue full");
        }
    }

    inline void
    coord_sched::schedule_publish(_coord_publish::req* r) {
        if (!publish_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("coord::coord_sched: publish queue full");
        }
    }

    inline void
    coord_sched::schedule_release(_coord_release::req* r) {
        if (!release_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("coord::coord_sched: release queue full");
        }
    }

    inline void
    coord_sched::schedule_read(_coord_read::req* r) {
        if (!read_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("coord::coord_sched: read queue full");
        }
    }

    inline bool
    coord_sched::try_complete_assign(_coord_assign::req* r) {
        if (!ready_.has_assign_capacity(next_lsn_)) {
            return false;
        }

        std::unique_ptr<_coord_assign::req> req(r);
        core::batch_ctx ctx;
        try {
            ctx = assign_validated_now(std::move(req->input));
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return true;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(std::move(ctx));
        }
        return true;
    }

    inline void
    coord_sched::handle_assign(_coord_assign::req* r) {
        std::unique_ptr<_coord_assign::req> req(r);
        try {
            validate_assign_input(req->input);
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        if (!ready_.has_assign_capacity(next_lsn_)) {
            pending_assigns_.push_back(req.release());
            return;
        }

        (void)try_complete_assign(req.release());
    }

    inline void
    coord_sched::handle_publish(_coord_publish::req* r) {
        std::unique_ptr<_coord_publish::req> req(r);
        try {
            resolve_terminal_lsn(req->batch_lsn);
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb();
        }
    }

    inline void
    coord_sched::handle_release(_coord_release::req* r) {
        std::unique_ptr<_coord_release::req> req(r);
        try {
            resolve_terminal_lsn(req->batch_lsn);
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb();
        }
    }

    inline void
    coord_sched::handle_read(_coord_read::req* r) {
        std::unique_ptr<_coord_read::req> req(r);
        core::read_handle handle;
        try {
            handle = cats_.acquire_read_handle();
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(std::move(handle));
        }
    }

    inline bool
    coord_sched::advance() {
        bool progress = false;

        for (uint32_t i = 0; i < kMaxAssignPerAdvance; ++i) {
            auto item = assign_q_.try_dequeue();
            if (!item) break;
            handle_assign(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxPublishPerAdvance; ++i) {
            auto item = publish_q_.try_dequeue();
            if (!item) break;
            handle_publish(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxReleasePerAdvance; ++i) {
            auto item = release_q_.try_dequeue();
            if (!item) break;
            handle_release(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxReadPerAdvance; ++i) {
            auto item = read_q_.try_dequeue();
            if (!item) break;
            handle_read(*item);
            progress = true;
        }

        return progress;
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_assign::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_assign(new req{
            .input = std::move(input),
            .cb = [ctx = ctx, scope = scope](core::batch_ctx&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_publish::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_publish(new req{
            .batch_lsn = batch_lsn,
            .cb = [ctx = ctx, scope = scope]() mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_release::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_release(new req{
            .batch_lsn = batch_lsn,
            .cb = [ctx = ctx, scope = scope]() mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_read::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_read(new req{
            .cb = [ctx = ctx, scope = scope](core::read_handle&& h) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(h));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

}  // namespace apps::inconel::coord

namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::coord_assign_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::coord::_coord_assign::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<apps::inconel::core::batch_ctx>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::coord_publish_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::coord::_coord_publish::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::coord_release_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::coord::_coord_release::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::coord_read_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::coord::_coord_read::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<apps::inconel::core::read_handle>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_COORD_SCHEDULER_HH
