#pragma once

#include <stdexcept>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"
#include "pump/core/context.hh"
#include "pump/coro/coro.hh"

#include "server/session.hh"
#include "resp/parser.hh"
#include "resp/response.hh"

namespace sider::server {

    namespace tcp = pump::scheduler::tcp;
    using namespace pump::sender;

    // Connection state: tracks whether the session is still alive.
    struct conn_state {
        session_t* session;
        bool closed = false;

        void close() { closed = true; }
        bool is_closed() const { return closed; }
    };

    // Coroutine that controls the recv loop: yields true while connection is open.
    static inline auto
    check_conn_state(conn_state& cs) -> pump::coro::return_yields<bool> {
        while (!cs.is_closed())
            co_yield true;
        co_return false;
    }

    // Build the RESP2 error response for an unknown command.
    static inline resp::resp_buffer
    unknown_command_error(const resp::parsed_command& cmd) {
        std::string msg = "unknown command '";
        msg += cmd.name();
        msg += "'";
        return resp::error(msg);
    }

    // Dispatch a parsed command to a response.
    static inline resp::resp_buffer
    dispatch(const resp::parsed_command& cmd) {
        if (cmd.argc == 0)
            return resp::error("empty command");

        if (resp::cmd_is(cmd, "PING"))
            return resp::pong();

        if (resp::cmd_is(cmd, "QUIT"))
            return resp::ok();  // caller handles connection close

        if (resp::cmd_is(cmd, "COMMAND"))
            return resp::empty_array();

        if (resp::cmd_is(cmd, "CLIENT"))
            return resp::ok();

        return unknown_command_error(cmd);
    }

    // Send a resp_buffer over TCP. Takes ownership of resp.
    template<typename session_ptr_t>
    static inline auto
    send_resp(session_ptr_t session, resp::resp_buffer&& resp) {
        auto len = resp.size();
        return tcp::send(session, resp.release(), len);
    }

    // Start the per-connection handler pipeline.
    static inline void
    handle_connection(session_sched_t* sched, session_t* session) {
        auto* cs = new conn_state{session};

        tcp::join(sched, session)
            >> flat_map([cs](auto&&...) {
                return just()
                    >> for_each(pump::coro::make_view_able(check_conn_state(*cs)))
                    >> flat_map([cs](auto&&...) {
                        return tcp::recv(cs->session);
                    })
                    >> flat_map([cs](pump::scheduler::net::net_frame&& frame) {
                        auto cmd = resp::parse_command(frame);
                        bool quit = resp::cmd_is(cmd, "QUIT");
                        auto resp = dispatch(cmd);
                        return send_resp(cs->session, std::move(resp))
                            >> then([quit](bool) -> bool {
                                if (quit) throw std::runtime_error("quit");
                                return true;
                            });
                    })
                    >> any_exception([cs](std::exception_ptr e) mutable {
                        cs->close();
                        return just_exception(e);
                    })
                    >> reduce();
            })
            >> any_exception([cs](std::exception_ptr) {
                delete cs->session;
                delete cs;
                return just();
            })
            >> submit(pump::core::make_root_context());
    }

} // namespace sider::server
