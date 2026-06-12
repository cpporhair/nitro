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
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

#include "../core/batch_carrier.hh"
#include "../core/owner_callback.hh"
#include "../core/read_catalog.hh"

namespace apps::inconel::coord {

    class ready_window {
      public:
        ready_window(uint64_t base_lsn, std::size_t window_size)
            : base_lsn_(base_lsn)
            , window_size_(window_size)
            , mask_(window_size == 0 ? 0 : window_size - 1)
            , bits_((window_size + kBitsPerWord - 1) / kBitsPerWord, 0) {
            if (window_size == 0) {
                throw std::invalid_argument(
                    "coord::ready_window: window_size must be nonzero");
            }
            if (!std::has_single_bit(window_size)) {
                throw std::invalid_argument(
                    "coord::ready_window: window_size must be a power of two");
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
            if (lsn < base_lsn_) {
                return resolved;
            }

            while (lsn - base_lsn_ < window_size_) {
                const std::size_t idx = bit_index(lsn);
                const std::size_t word = idx / kBitsPerWord;
                const std::size_t bit = idx % kBitsPerWord;
                const uint64_t suffix = bits_[word] >> bit;
                const uint64_t run = std::countr_one(suffix);
                const uint64_t span =
                    std::min<uint64_t>(kBitsPerWord - bit,
                                       window_size_ - idx);
                const uint64_t take =
                    std::min<uint64_t>(run, span);
                if (take == 0) {
                    break;
                }

                const uint64_t mask =
                    (take == kBitsPerWord)
                        ? ~uint64_t{0}
                        : ((uint64_t{1} << take) - 1) << bit;
                bits_[word] &= ~mask;
                lsn += take;
                base_lsn_ = lsn;
                resolved = lsn - 1;
                if (take < span) {
                    break;
                }
            }

            return resolved;
        }

      private:
        static constexpr std::size_t kBitsPerWord = 64;

        [[nodiscard]] std::size_t
        bit_index(uint64_t lsn) const noexcept {
            return static_cast<std::size_t>(lsn & mask_);
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
        std::size_t           mask_;
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
    namespace _coord_close_gate { struct req; struct sender; }
    namespace _coord_install_cat { struct req; struct sender; }
    namespace _coord_open_gate { struct req; struct sender; }
    namespace _coord_enter_memtable { struct req; struct sender; }

    namespace _coord_event {
        using request = std::variant<
            _coord_publish::req*,
            _coord_release::req*,
            _coord_read::req*,
            _coord_close_gate::req*,
            _coord_install_cat::req*,
            _coord_open_gate::req*,
            _coord_enter_memtable::req*>;
    }

    struct coord_sched {
        explicit
        coord_sched(std::shared_ptr<const core::publish_catalog> initial_cat,
                    uint32_t front_count,
                    uint64_t next_lsn = 1,
                    std::size_t ready_window_size = 65536,
                    std::size_t queue_depth = 1024,
                    std::size_t pending_assign_capacity = 0)
            : assign_q_(queue_depth)
            , event_q_(queue_depth)
            , cats_(validate_initial_cat(initial_cat,
                                         front_count,
                                         next_lsn,
                                         ready_window_size))
            , ready_(initial_cat->durable_lsn.load(std::memory_order_acquire) + 1,
                     ready_window_size)
            , pending_assign_capacity_(
                  pending_assign_capacity == 0
                      ? ready_window_size
                      : pending_assign_capacity)
            , next_lsn_(next_lsn)
            , front_count_(front_count)
            , cat_epoch_(initial_cat->epoch) {
            if (pending_assign_capacity_ == 0) {
                throw std::invalid_argument(
                    "coord::coord_sched: pending_assign_capacity must be nonzero");
            }
        }

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

        [[nodiscard]] _coord_close_gate::sender
        close_gate();

        [[nodiscard]] _coord_install_cat::sender
        install_cat(std::shared_ptr<const core::publish_catalog> cat);

        [[nodiscard]] _coord_open_gate::sender
        open_gate();

        [[nodiscard]] _coord_enter_memtable::sender
        enter_memtable_phase(uint64_t batch_lsn);

        [[nodiscard]] core::batch_ctx
        assign_batch_lsn_for_testing(core::client_batch_buffer&& input) {
            core::client_batch_view parsed = parse_assign_input(input);
            if (!ready_.has_assign_capacity(next_lsn_)) {
                throw std::runtime_error(
                    "coord::coord_sched: assign window is full");
            }
            return assign_validated_now(std::move(input), std::move(parsed));
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
        void schedule_close_gate(_coord_close_gate::req* r);
        void schedule_install_cat(_coord_install_cat::req* r);
        void schedule_open_gate(_coord_open_gate::req* r);
        void schedule_enter_memtable(_coord_enter_memtable::req* r);

        bool advance();

        template<typename runtime_t>
        bool advance(runtime_t&) { return advance(); }

      private:
        static constexpr uint32_t kMaxAssignPerAdvance = 64;
        static constexpr uint32_t kMaxEventPerAdvance = 512;

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
            if (cat->epoch <= cat_epoch_) {
                throw std::logic_error(
                    "coord::coord_sched: replacement CAT epoch must advance");
            }
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

        [[nodiscard]] static core::client_batch_view
        parse_assign_input(const core::client_batch_buffer& input) {
            core::client_batch_view view = input.view();
            if (view.op_count() == 0) {
                throw std::invalid_argument(
                    "coord::coord_sched: empty write batch");
            }
            return view;
        }

        [[nodiscard]] core::batch_ctx
        assign_validated_now(core::client_batch_buffer&& input,
                             core::client_batch_view&& parsed) {
            const uint64_t lsn = next_lsn_;
            core::batch_ctx ctx =
                core::build_batch_ctx(
                    std::move(input), std::move(parsed), lsn, front_count_);
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
        handle_close_gate(_coord_close_gate::req* r);

        void
        handle_install_cat(_coord_install_cat::req* r);

        void
        handle_open_gate(_coord_open_gate::req* r);

        void
        handle_enter_memtable(_coord_enter_memtable::req* r);

        void
        handle_event(_coord_event::request event);

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

        pump::core::per_core::queue<_coord_assign::req*> assign_q_;
        pump::core::per_core::queue<_coord_event::request> event_q_;

        core::catalog_store cats_;
        ready_window        ready_;
        publish_gate        gate_;
        std::deque<_coord_assign::req*> pending_assigns_;
        std::size_t         pending_assign_capacity_;
        uint64_t            next_lsn_;
        uint32_t            front_count_;
        uint64_t            cat_epoch_;
    };

    namespace _coord_assign {

        struct req {
            core::client_batch_buffer                         input;
            std::optional<core::client_batch_view>            parsed;
            std::move_only_function<void(
                core::owner_outcome<core::batch_ctx>&&)>      cb;
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
            std::move_only_function<void(
                core::owner_outcome<void>&&)> cb;
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
            std::move_only_function<void(
                core::owner_outcome<void>&&)> cb;
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
            std::move_only_function<void(
                core::owner_outcome<core::read_handle>&&)> cb;
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

    namespace _coord_close_gate {

        struct req {
            std::move_only_function<void(
                core::owner_outcome<
                    std::shared_ptr<const core::publish_catalog>>&&)> cb;
        };

        struct op {
            constexpr static bool coord_close_gate_op = true;
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

    namespace _coord_install_cat {

        struct req {
            std::shared_ptr<const core::publish_catalog> cat;
            std::move_only_function<void(core::owner_outcome<void>&&)> cb;
        };

        struct op {
            constexpr static bool coord_install_cat_op = true;
            coord_sched* sched;
            std::shared_ptr<const core::publish_catalog> cat;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            coord_sched* sched;
            std::shared_ptr<const core::publish_catalog> cat;

            sender(coord_sched* s,
                   std::shared_ptr<const core::publish_catalog> c)
                : sched(s), cat(std::move(c)) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{.sched = sched, .cat = std::move(cat)};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }

    namespace _coord_open_gate {

        struct req {
            std::move_only_function<void(core::owner_outcome<void>&&)> cb;
        };

        struct op {
            constexpr static bool coord_open_gate_op = true;
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

    namespace _coord_enter_memtable {

        struct req {
            uint64_t batch_lsn;
            std::move_only_function<void(core::owner_outcome<void>&&)> cb;
        };

        struct op {
            constexpr static bool coord_enter_memtable_op = true;
            coord_sched* sched;
            uint64_t batch_lsn;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            coord_sched* sched;
            uint64_t batch_lsn;

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

    inline coord_sched::~coord_sched() {
        while (auto item = assign_q_.try_dequeue()) delete *item;
        while (auto item = event_q_.try_dequeue()) {
            std::visit([](auto* r) { delete r; }, *item);
        }
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

    inline _coord_close_gate::sender
    coord_sched::close_gate() {
        return _coord_close_gate::sender{.sched = this};
    }

    inline _coord_install_cat::sender
    coord_sched::install_cat(
        std::shared_ptr<const core::publish_catalog> cat) {
        return _coord_install_cat::sender{this, std::move(cat)};
    }

    inline _coord_open_gate::sender
    coord_sched::open_gate() {
        return _coord_open_gate::sender{.sched = this};
    }

    inline _coord_enter_memtable::sender
    coord_sched::enter_memtable_phase(uint64_t batch_lsn) {
        return _coord_enter_memtable::sender{
            .sched = this,
            .batch_lsn = batch_lsn,
        };
    }

    inline void
    coord_sched::enqueue_assign_batch_lsn_for_testing(
        core::client_batch_buffer&& input,
        std::move_only_function<void(core::batch_ctx&&)> cb,
        std::move_only_function<void(std::exception_ptr)> fail) {
        schedule_assign(new _coord_assign::req{
            .input = std::move(input),
            .cb = [cb = std::move(cb),
                   fail = std::move(fail)](
                      core::owner_outcome<core::batch_ctx>&& r) mutable {
                if (r.has_value()) {
                    if (cb) cb(std::move(*r));
                } else if (fail) {
                    fail(std::move(r.error()));
                }
            },
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
        if (!event_q_.try_enqueue(_coord_event::request{r})) {
            delete r;
            throw std::runtime_error("coord::coord_sched: publish queue full");
        }
    }

    inline void
    coord_sched::schedule_release(_coord_release::req* r) {
        if (!event_q_.try_enqueue(_coord_event::request{r})) {
            delete r;
            throw std::runtime_error("coord::coord_sched: release queue full");
        }
    }

    inline void
    coord_sched::schedule_read(_coord_read::req* r) {
        if (!event_q_.try_enqueue(_coord_event::request{r})) {
            delete r;
            throw std::runtime_error("coord::coord_sched: read queue full");
        }
    }

    inline void
    coord_sched::schedule_close_gate(_coord_close_gate::req* r) {
        if (!event_q_.try_enqueue(_coord_event::request{r})) {
            delete r;
            throw std::runtime_error(
                "coord::coord_sched: close_gate queue full");
        }
    }

    inline void
    coord_sched::schedule_install_cat(_coord_install_cat::req* r) {
        if (!event_q_.try_enqueue(_coord_event::request{r})) {
            delete r;
            throw std::runtime_error(
                "coord::coord_sched: install_cat queue full");
        }
    }

    inline void
    coord_sched::schedule_open_gate(_coord_open_gate::req* r) {
        if (!event_q_.try_enqueue(_coord_event::request{r})) {
            delete r;
            throw std::runtime_error(
                "coord::coord_sched: open_gate queue full");
        }
    }

    inline void
    coord_sched::schedule_enter_memtable(_coord_enter_memtable::req* r) {
        if (!event_q_.try_enqueue(_coord_event::request{r})) {
            delete r;
            throw std::runtime_error(
                "coord::coord_sched: enter_memtable queue full");
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
            if (!req->parsed.has_value()) {
                throw std::logic_error(
                    "coord::coord_sched: assign request was not parsed");
            }
            ctx = assign_validated_now(
                std::move(req->input), std::move(*req->parsed));
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return true;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<core::batch_ctx>{std::move(ctx)});
        }
        return true;
    }

    inline void
    coord_sched::handle_assign(_coord_assign::req* r) {
        std::unique_ptr<_coord_assign::req> req(r);
        try {
            req->parsed.emplace(parse_assign_input(req->input));
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        if (!ready_.has_assign_capacity(next_lsn_)) {
            if (pending_assigns_.size() >= pending_assign_capacity_) {
                auto cb = std::move(req->cb);
                req.reset();
                if (cb) {
                    cb(std::unexpected(std::make_exception_ptr(
                        std::runtime_error(
                            "coord assign backpressure overflow"))));
                }
                return;
            }
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
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<void>{});
        }
    }

    inline void
    coord_sched::handle_release(_coord_release::req* r) {
        std::unique_ptr<_coord_release::req> req(r);
        try {
            resolve_terminal_lsn(req->batch_lsn);
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<void>{});
        }
    }

    inline void
    coord_sched::handle_read(_coord_read::req* r) {
        std::unique_ptr<_coord_read::req> req(r);
        core::read_handle handle;
        try {
            handle = cats_.acquire_read_handle();
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<core::read_handle>{std::move(handle)});
        }
    }

    inline void
    coord_sched::handle_close_gate(_coord_close_gate::req* r) {
        std::unique_ptr<_coord_close_gate::req> req(r);
        std::shared_ptr<const core::publish_catalog> cat;
        try {
            if (!gate_.is_open()) {
                throw std::logic_error(
                    "coord::coord_sched: close_gate on a closed gate");
            }
            gate_.close();
            cat = cats_.current_cat();
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<
               std::shared_ptr<const core::publish_catalog>>{std::move(cat)});
        }
    }

    inline void
    coord_sched::handle_install_cat(_coord_install_cat::req* r) {
        std::unique_ptr<_coord_install_cat::req> req(r);
        try {
            validate_replacement_cat(req->cat);
            cat_epoch_ = req->cat->epoch;
            cats_.install_cat(std::move(req->cat));
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<void>{});
        }
    }

    inline void
    coord_sched::handle_open_gate(_coord_open_gate::req* r) {
        std::unique_ptr<_coord_open_gate::req> req(r);
        try {
            if (gate_.is_open()) {
                throw std::logic_error(
                    "coord::coord_sched: open_gate on an open gate");
            }
            apply_pending_gate_prefix(gate_.open_and_take_pending());
            drain_pending_assigns();
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<void>{});
        }
    }

    inline void
    coord_sched::handle_enter_memtable(_coord_enter_memtable::req* r) {
        std::unique_ptr<_coord_enter_memtable::req> req(r);
        // This handle intentionally performs no state mutation. Its value is
        // the execution-domain boundary: the caller's continuation dispatches
        // every memtable fragment while this coord event is still being
        // advanced, so it is ordered against close_gate's seal fan-out.
        (void)req->batch_lsn;
        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<void>{});
        }
    }

    inline void
    coord_sched::handle_event(_coord_event::request event) {
        std::visit(
            [this](auto* r) {
                using req_t = std::remove_pointer_t<decltype(r)>;
                if constexpr (std::is_same_v<req_t, _coord_publish::req>) {
                    handle_publish(r);
                } else if constexpr (
                    std::is_same_v<req_t, _coord_release::req>) {
                    handle_release(r);
                } else if constexpr (
                    std::is_same_v<req_t, _coord_read::req>) {
                    handle_read(r);
                } else if constexpr (
                    std::is_same_v<req_t, _coord_close_gate::req>) {
                    handle_close_gate(r);
                } else if constexpr (
                    std::is_same_v<req_t, _coord_install_cat::req>) {
                    handle_install_cat(r);
                } else if constexpr (
                    std::is_same_v<req_t, _coord_open_gate::req>) {
                    handle_open_gate(r);
                } else {
                    static_assert(
                        std::is_same_v<req_t, _coord_enter_memtable::req>);
                    handle_enter_memtable(r);
                }
            },
            event);
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

        for (uint32_t i = 0; i < kMaxEventPerAdvance; ++i) {
            auto item = event_q_.try_dequeue();
            if (!item) break;
            handle_event(*item);
            progress = true;
        }

        return progress;
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_assign::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_assign(new req{
            .input = std::move(input),
            .cb = core::make_owner_pusher<
                pos, scope_t, core::batch_ctx>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_publish::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_publish(new req{
            .batch_lsn = batch_lsn,
            .cb = core::make_owner_pusher<pos, scope_t, void>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_release::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_release(new req{
            .batch_lsn = batch_lsn,
            .cb = core::make_owner_pusher<pos, scope_t, void>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_read::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_read(new req{
            .cb = core::make_owner_pusher<
                pos, scope_t, core::read_handle>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_close_gate::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_close_gate(new req{
            .cb = core::make_owner_pusher<
                pos,
                scope_t,
                std::shared_ptr<const core::publish_catalog>>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_install_cat::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_install_cat(new req{
            .cat = std::move(cat),
            .cb = core::make_owner_pusher<pos, scope_t, void>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_open_gate::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_open_gate(new req{
            .cb = core::make_owner_pusher<pos, scope_t, void>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _coord_enter_memtable::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_enter_memtable(new req{
            .batch_lsn = batch_lsn,
            .cb = core::make_owner_pusher<pos, scope_t, void>(ctx, scope),
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

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::coord_close_gate_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::coord::_coord_close_gate::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<
                std::shared_ptr<
                    const apps::inconel::core::publish_catalog>>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::coord_install_cat_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::coord::_coord_install_cat::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::coord_open_gate_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::coord::_coord_open_gate::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::coord_enter_memtable_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::coord::_coord_enter_memtable::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_COORD_SCHEDULER_HH
