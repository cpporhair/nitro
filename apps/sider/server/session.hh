#pragma once

#include "env/scheduler/net/tcp/tcp.hh"
#include "env/scheduler/net/tcp/io_uring/scheduler.hh"
#include "resp/unpacker.hh"

namespace sider::server {

    namespace tcp = pump::scheduler::tcp;

    struct sider_factory {
        template<typename sched_t>
        using session_type = pump::scheduler::net::session_t<
            tcp::common::tcp_bind<sched_t>,
            tcp::common::tcp_ring_buffer<resp::resp2_unpacker>,
            pump::scheduler::net::frame_receiver
        >;

        template<typename sched_t>
        static auto* create(int fd, sched_t* sche) {
            return new session_type<sched_t>(
                tcp::common::tcp_bind<sched_t>(fd, sche),
                tcp::common::tcp_ring_buffer<resp::resp2_unpacker>(65536),
                pump::scheduler::net::frame_receiver()
            );
        }
    };

    using accept_sched_t = tcp::io_uring::accept_scheduler<tcp::senders::conn::op>;
    using session_sched_t = tcp::io_uring::session_scheduler<sider_factory>;
    using session_t = session_sched_t::session_t;

} // namespace sider::server
