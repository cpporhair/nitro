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
#include "env/scheduler/nvme/get_page.hh"

namespace sider::server {

    namespace tcp = pump::scheduler::tcp;
    using namespace pump::sender;

    // ── Command classification ──

    struct simple_resp {
        resp::resp_buffer buf;
        bool quit = false;
    };
    struct get_cmd { std::string_view key; };
    struct set_cmd { std::string_view key; std::string_view value; int64_t expire_ms = -1; };
    struct del_cmd { std::string_view key; };

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
            return get_cmd{cmd.arg(1)};
        }

        if (resp::cmd_is(cmd, "SET")) {
            if (cmd.argc < 3)
                return simple_resp{resp::error("wrong number of arguments for 'set' command")};
            int64_t expire_ms = -1;
            for (uint8_t i = 3; i + 1 < cmd.argc; i++) {
                if (resp::cmd_is_arg(cmd, i, "EX")) {
                    expire_ms = std::strtoll(cmd.arg(i + 1).data(), nullptr, 10) * 1000;
                    break;
                }
                if (resp::cmd_is_arg(cmd, i, "PX")) {
                    expire_ms = std::strtoll(cmd.arg(i + 1).data(), nullptr, 10);
                    break;
                }
            }
            return set_cmd{cmd.arg(1), cmd.arg(2), expire_ms};
        }

        if (resp::cmd_is(cmd, "DEL")) {
            if (cmd.argc < 2)
                return simple_resp{resp::error("wrong number of arguments for 'del' command")};
            return del_cmd{cmd.arg(1)};
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

    // ── Zero-copy bulk string send ──
    // Sends "$<len>\r\n" + value_data + "\r\n" via 3-iovec scatter-gather.
    // Header is heap-allocated and freed on completion. Value data is borrowed.

    namespace _bulk_send {

        static constexpr char crlf[2] = {'\r', '\n'};

        template<typename session_t>
        struct op {
            constexpr static bool bulk_send_op = true;
            session_t* session;
            const char* value;
            uint32_t value_len;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope) {
                char tmp[16];
                int hdr_len;
                if (value) {
                    hdr_len = snprintf(tmp, sizeof(tmp), "$%u\r\n", value_len);
                } else {
                    hdr_len = 5;
                    std::memcpy(tmp, "$-1\r\n", 5);
                }
                auto* hdr = new char[hdr_len];
                std::memcpy(hdr, tmp, hdr_len);

                auto* req = new pump::scheduler::net::frame_send_req{
                    hdr, static_cast<uint32_t>(hdr_len),
                    [ctx, scope](bool ok) mutable {
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope, ok);
                    }
                };
                if (value) {
                    req->send_vec[0] = {hdr, static_cast<size_t>(hdr_len)};
                    req->send_vec[1] = {const_cast<char*>(value), value_len};
                    req->send_vec[2] = {const_cast<char*>(crlf), 2};
                    req->send_cnt = 3;
                } else {
                    req->send_vec[0] = {hdr, static_cast<size_t>(hdr_len)};
                    req->send_cnt = 1;
                }
                session->invoke(pump::scheduler::net::do_send, req);
            }
        };

        template<typename session_t>
        struct sender {
            session_t* session;
            const char* value;
            uint32_t value_len;

            auto make_op() { return op<session_t>{session, value, value_len}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };

    } // namespace _bulk_send

    template<typename session_ptr_t>
    static inline auto
    send_bulk_string_zero_copy(session_ptr_t session,
                               const char* value, uint32_t value_len) {
        return _bulk_send::sender<std::remove_pointer_t<session_ptr_t>>{
            session, value, value_len};
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
        auto key_data = action.key.data();
        auto key_len  = static_cast<uint16_t>(action.key.size());

        return store_sched->lookup(key_data, key_len)
            >> visit()
            >> then([cs, store_sched, key_data, key_len](auto&& result) {
                using T = std::decay_t<decltype(result)>;
                if constexpr (std::is_same_v<T, store::hot_result>) {
                    return send_bulk_string_zero_copy(cs->session, result.data, result.len);
                } else if constexpr (std::is_same_v<T, store::nil_result>) {
                    return send_bulk_string_zero_copy(cs->session, nullptr, 0);
                } else {
                    // cold_result: NVMe read → extract value → promote → send.
                    constexpr uint32_t pg_sz = 4096;
                    uint32_t buf_size = result.page_count * pg_sz;
                    bool is_large = (result.page_count > 1);
                    auto* read_buf = is_large
                        ? store::alloc_large(buf_size)
                        : store::alloc_page();
                    auto* page = new nvme::sider_page{
                        result.nvme_lba, read_buf,
                        store_sched->ssd_info_for(result.disk_id),
                        static_cast<uint64_t>(buf_size)};

                    return store_sched->nvme_sched_for(result.disk_id)->get(page)
                        >> flat_map([cs, store_sched, key_data, key_len,
                                     result, page, read_buf, is_large]
                                    (pump::scheduler::nvme::get::res<nvme::sider_page>&& res) {
                            if (res.is_success()) {
                                char* val;
                                if (is_large) {
                                    val = read_buf;
                                } else {
                                    auto sc = static_cast<store::size_class_t>(result.size_class);
                                    val = read_buf + store::slot_offset(sc, result.slot_index);
                                }
                                auto resp = resp::bulk_string(val, result.value_len);

                                // Best-effort promote: copy to hot slot.
                                store_sched->store.promote(
                                    key_data, key_len, result.version,
                                    val, result.value_len);

                                if (is_large) store::free_large(read_buf);
                                else store::free_page(read_buf);
                                delete page;
                                return send_resp(cs->session, std::move(resp));
                            }
                            if (is_large) store::free_large(read_buf);
                            else store::free_page(read_buf);
                            delete page;
                            return send_resp(cs->session, resp::nil());
                        });
                }
            })
            >> flat();
    }

    template <typename session_sched_t>
    static inline auto
    exec_set(set_cmd&& action, conn_state<session_sched_t>* cs,
             store::store_scheduler* store_sched) {
        int64_t expire_at = action.expire_ms >= 0
            ? store::now_ms() + action.expire_ms : -1;
        return store_sched->put(
                action.key.data(),
                static_cast<uint16_t>(action.key.size()),
                action.value.data(),
                static_cast<uint32_t>(action.value.size()),
                expire_at)
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
            >> push_result_to_context()
            >> get_context<pump::scheduler::net::net_frame>()
            >> then([](pump::scheduler::net::net_frame &frame) -> cmd_action {
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
            >> pop_context()
            >> any_exception([](std::exception_ptr e) mutable {
                return just()
                    >> get_context<conn_state<session_sched_t> >()
                    >> then([e](auto &state) { state.close(); return e;});
            })
            >> reduce()
            >> submit(pump::core::make_root_context(conn_state{sched, session}));
    }

} // namespace sider::server

// ── pump::core specializations for bulk_send ──
namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::bulk_send_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static inline void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t, typename session_t>
    struct compute_sender_type<ctx_t,
            sider::server::_bulk_send::sender<session_t>> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<bool>{};
        }
    };

} // namespace pump::core
