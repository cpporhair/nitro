#pragma once

#include <stdexcept>
#include <variant>
#include <string>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/core/context.hh"
#include "pump/coro/coro.hh"

#include "server/session.hh"
#include "resp/parser.hh"
#include "resp/response.hh"
#include "store/scheduler.hh"

namespace sider::server {

    namespace tcp = pump::scheduler::tcp;
    using namespace pump::sender;

    // ── Command classification ──

    struct simple_resp {
        resp::resp_buffer buf;
        bool quit = false;
    };
    struct get_cmd { std::string key; };
    struct set_cmd { std::string key; std::string value; };
    struct del_cmd { std::string key; };

    using cmd_action = std::variant<simple_resp, get_cmd, set_cmd, del_cmd>;

    // Connection state: tracks whether the session is still alive. Move-only.
    template<typename session_sched_t>
    struct conn_state {
        session_sched_t* sched;
        session_t* session;
        bool closed = false;

        void close() { closed = true; }
        bool is_closed() const { return closed; }

        conn_state(session_sched_t* s, session_t* sess) : sched(s), session(sess) {}
        ~conn_state() { delete session; }

        conn_state(conn_state&& o) noexcept
            : sched(o.sched), session(o.session), closed(o.closed) {
            o.session = nullptr;
        }
        conn_state& operator=(conn_state&& o) noexcept {
            if (this != &o) {
                delete session;
                sched = o.sched; session = o.session; closed = o.closed;
                o.session = nullptr;
            }
            return *this;
        }
        conn_state(const conn_state&) = delete;
        conn_state& operator=(const conn_state&) = delete;
    };

    // Build the RESP2 error response for an unknown command.
    static inline resp::resp_buffer
    unknown_command_error(const resp::parsed_command& cmd) {
        std::string msg = "unknown command '";
        msg += cmd.name();
        msg += "'";
        return resp::error(msg);
    }

    // Classify a parsed command into a cmd_action variant.
    // Copies key/value into owned strings for lifetime safety across async.
    static inline cmd_action
    classify_command(const resp::parsed_command& cmd) {
        if (cmd.argc == 0)
            return simple_resp{resp::error("empty command")};

        if (resp::cmd_is(cmd, "PING"))
            return simple_resp{resp::pong()};

        if (resp::cmd_is(cmd, "QUIT"))
            return simple_resp{resp::ok(), true};

        if (resp::cmd_is(cmd, "COMMAND"))
            return simple_resp{resp::empty_array()};

        if (resp::cmd_is(cmd, "CLIENT"))
            return simple_resp{resp::ok()};

        if (resp::cmd_is(cmd, "GET")) {
            if (cmd.argc < 2)
                return simple_resp{resp::error("wrong number of arguments for 'get' command")};
            return get_cmd{std::string(cmd.arg(1))};
        }

        if (resp::cmd_is(cmd, "SET")) {
            if (cmd.argc < 3)
                return simple_resp{resp::error("wrong number of arguments for 'set' command")};
            return set_cmd{std::string(cmd.arg(1)), std::string(cmd.arg(2))};
        }

        if (resp::cmd_is(cmd, "DEL")) {
            if (cmd.argc < 2)
                return simple_resp{resp::error("wrong number of arguments for 'del' command")};
            return del_cmd{std::string(cmd.arg(1))};
        }

        return simple_resp{unknown_command_error(cmd)};
    }

    // Send a resp_buffer over TCP. Takes ownership of resp.
    template<typename session_ptr_t>
    static inline auto
    send_resp(session_ptr_t session, resp::resp_buffer&& resp) {
        auto len = resp.size();
        return tcp::send(session, resp.release(), len);
    }

    // ── Per-command execution ──

    template <typename session_sched_t>
    static inline auto
    exec_simple(simple_resp&& action, conn_state<session_sched_t>* cs) {
        bool quit = action.quit;
        return send_resp(cs->session, std::move(action.buf))
            >> then([quit](bool) -> bool {
                if (quit) throw std::runtime_error("quit");
                return true;
            });
    }

    template <typename session_sched_t>
    static inline auto
    exec_get(get_cmd&& action, conn_state<session_sched_t>* cs,
             store::store_scheduler* store_sched) {
        return store_sched->lookup(
                action.key.data(),
                static_cast<uint16_t>(action.key.size()))
            >> flat_map([cs](store::get_result r) {
                auto resp = r.found()
                    ? resp::bulk_string(r.data, r.len)
                    : resp::nil();
                return send_resp(cs->session, std::move(resp));
            });
    }

    template <typename session_sched_t>
    static inline auto
    exec_set(set_cmd&& action, conn_state<session_sched_t>* cs,
             store::store_scheduler* store_sched) {
        return store_sched->put(
                action.key.data(),
                static_cast<uint16_t>(action.key.size()),
                action.value.data(),
                static_cast<uint16_t>(action.value.size()))
            >> flat_map([cs]() {
                return send_resp(cs->session, resp::ok());
            });
    }

    template <typename session_sched_t>
    static inline auto
    exec_del(del_cmd&& action, conn_state<session_sched_t>* cs,
             store::store_scheduler* store_sched) {
        return store_sched->del(
                action.key.data(),
                static_cast<uint16_t>(action.key.size()))
            >> flat_map([cs](int count) {
                return send_resp(cs->session, resp::integer(count));
            });
    }

    // Dispatch a classified command action → returns sender producing bool.
    template<typename T0, typename T1>
    static inline auto
    execute_action(T0&& action, conn_state<T1>* cs,
                   store::store_scheduler* store_sched) {
        using A = std::decay_t<T0>;
        if constexpr (std::is_same_v<A, simple_resp>)
            return exec_simple(std::move(action), cs);
        else if constexpr (std::is_same_v<A, get_cmd>)
            return exec_get(std::move(action), cs, store_sched);
        else if constexpr (std::is_same_v<A, set_cmd>)
            return exec_set(std::move(action), cs, store_sched);
        else if constexpr (std::is_same_v<A, del_cmd>)
            return exec_del(std::move(action), cs, store_sched);
    }

    // Coroutine that controls the recv loop: yields true while connection is open.
    template <typename session_t>
    static inline auto
    pick_good_session(auto& cs) -> pump::coro::return_yields<session_t*> {
        while (!cs.is_closed())
            co_yield cs.session;
        co_return nullptr;
    }

    static inline void
    handle_connection(session_sched_t* sched, session_t* session,
                      store::store_scheduler* store_sched) {
        tcp::join(sched, session)
            >> get_context<conn_state<session_sched_t> >()
            >> then([](conn_state<session_sched_t> &state) {
                return pump::coro::make_view_able(pick_good_session<session_t>(state));
            })
            >> for_each()
            >> flat_map([](session_t *session) {
                return tcp::recv(session);
            })
            >> then([](pump::scheduler::net::net_frame &&frame) -> cmd_action {
                auto cmd = resp::parse_command(frame);
                return classify_command(cmd);
            })
            >> visit()
            >> get_context<conn_state<session_sched_t> >()
            >> flat_map([store_sched]<typename T>(auto &state, T &&action) {
                return execute_action(
                    std::forward<T>(action),
                    &state,
                    store_sched
                );
            })
            >> any_exception([](std::exception_ptr e) mutable {
                return just()
                    >> get_context<conn_state<session_sched_t> >()
                    >> then([e](auto &state) { state.close(); return e;});
            })
            >> reduce()
            >> submit(pump::core::make_root_context(conn_state{sched, session}));
    }

} // namespace sider::server
