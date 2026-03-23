#pragma once

#include <bits/move_only_function.h>

#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/op_tuple_builder.hh"

#include "env/scheduler/net/tcp/tcp.hh"
#include "env/scheduler/net/tcp/io_uring/scheduler.hh"
#include "env/scheduler/net/common/session_tags.hh"
#include "env/scheduler/net/common/session_lifecycle.hh"
#include "env/scheduler/net/common/errors.hh"

#include "resp/unpacker.hh"
#include "resp/batch.hh"

namespace sider::server {

    namespace tcp = pump::scheduler::tcp;

    // ── batch_recv_req ──

    struct batch_recv_req {
        std::move_only_function<void(resp::cmd_batch&&)> cb;
        std::move_only_function<void(std::exception_ptr)> cb_err;
    };

    // Thread-local pool to avoid per-recv heap allocation.
    struct batch_recv_pool {
        static constexpr uint32_t CAP = 16;
        batch_recv_req* pool_[CAP];
        uint32_t top_ = 0;

        batch_recv_req* get() {
            if (top_ > 0) return pool_[--top_];
            return new batch_recv_req{};
        }
        void put(batch_recv_req* p) {
            p->cb = {};
            p->cb_err = {};
            if (top_ < CAP) pool_[top_++] = p;
            else delete p;
        }
    };
    static inline thread_local batch_recv_pool recv_req_pool;

    // ── batch_receiver: queue-based async bridge for cmd_batch ──

    struct batch_receiver {
        pump::core::local::queue<batch_recv_req*> recv_q;
        pump::core::local::queue<resp::cmd_batch*> ready_q;
        bool closed = false;

        template<typename owner_t>
        void invoke(const struct pump::scheduler::net::on_frame&, owner_t&,
                    resp::cmd_batch&& batch) {
            if (auto opt = recv_q.try_dequeue()) {
                opt.value()->cb(std::move(batch));
                recv_req_pool.put(opt.value());
            } else {
                ready_q.try_enqueue(new resp::cmd_batch(std::move(batch)));
            }
        }

        template<typename owner_t>
        void invoke(const struct pump::scheduler::net::do_recv&, owner_t&,
                    batch_recv_req* req) {
            if (closed) {
                req->cb_err(std::make_exception_ptr(
                    pump::scheduler::net::session_closed_error()));
                recv_req_pool.put(req);
                return;
            }
            if (auto opt = ready_q.try_dequeue()) {
                req->cb(std::move(*opt.value()));
                delete opt.value();
                recv_req_pool.put(req);
            } else {
                if (!recv_q.try_enqueue(req)) {
                    req->cb_err(std::make_exception_ptr(
                        pump::scheduler::net::enqueue_failed_error()));
                    recv_req_pool.put(req);
                }
            }
        }

        template<typename owner_t>
        void invoke(const struct pump::scheduler::net::on_error&, owner_t&,
                    std::exception_ptr ex) {
            closed = true;
            while (auto opt = recv_q.try_dequeue()) {
                opt.value()->cb_err(ex);
                recv_req_pool.put(opt.value());
            }
            while (auto opt = ready_q.try_dequeue())
                delete opt.value();
        }
    };

    // ── Session factory ──

    struct sider_factory {
        template<typename sched_t>
        using session_type = pump::scheduler::net::session_t<
            tcp::common::tcp_bind<sched_t>,
            tcp::common::tcp_ring_buffer<resp::resp2_batch_unpacker>,
            batch_receiver,
            pump::scheduler::net::session_lifecycle
        >;

        template<typename sched_t>
        static auto* create(int fd, sched_t* sche) {
            return new session_type<sched_t>(
                tcp::common::tcp_bind<sched_t>(fd, sche),
                tcp::common::tcp_ring_buffer<resp::resp2_batch_unpacker>(2 * 1024 * 1024),
                batch_receiver(),
                pump::scheduler::net::session_lifecycle()
            );
        }
    };

    using accept_sched_t = tcp::io_uring::accept_scheduler<tcp::senders::conn::op>;
    using session_sched_t = tcp::io_uring::session_scheduler<sider_factory>;
    using session_t = session_sched_t::session_t;

    // ── recv_batch sender ──

    namespace _recv_batch {

        template<typename session_t>
        struct op {
            constexpr static bool batch_recv_op = true;
            session_t* session;

            op(session_t* s) : session(s) {}
            op(op&& rhs) noexcept : session(rhs.session) {}

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope) {
                auto* req = recv_req_pool.get();
                req->cb = [ctx, scope](resp::cmd_batch&& batch) mutable {
                    pump::core::op_pusher<pos + 1, scope_t>::push_value(
                        ctx, scope, std::move(batch));
                };
                req->cb_err = [ctx, scope](std::exception_ptr ex) mutable {
                    pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                        ctx, scope, ex);
                };
                session->invoke(pump::scheduler::net::do_recv, req);
            }
        };

        template<typename session_t>
        struct sender {
            session_t* session;
            sender(session_t* s) : session(s) {}
            sender(sender&& rhs) noexcept : session(rhs.session) {}

            auto make_op() { return op<session_t>(session); }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };

    } // namespace _recv_batch

    template<typename session_t>
    auto recv_batch(session_t* session) {
        return _recv_batch::sender<session_t>{session};
    }

} // namespace sider::server

// ── pump::core specializations ──

namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::batch_recv_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static inline void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t, typename session_t>
    struct compute_sender_type<ctx_t,
            sider::server::_recv_batch::sender<session_t>> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<sider::resp::cmd_batch>{};
        }
    };

} // namespace pump::core
