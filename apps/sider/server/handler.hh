#pragma once

#include <stdexcept>
#include <variant>
#include <string>
#include <memory>
#include <vector>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/get_context.hh"
#include "pump/core/context.hh"
#include "pump/coro/coro.hh"

#include "server/session.hh"
#include "resp/parser.hh"
#include "resp/response.hh"
#include "resp/batch.hh"
#include "store/scheduler.hh"
#include "env/scheduler/nvme/get_page.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/push_context.hh"

namespace sider::server {

    namespace tcp = pump::scheduler::tcp;
    using namespace pump::sender;

    // ── Command classification ──

    struct simple_resp {
        resp::resp_slot slot;
        bool quit = false;
    };
    struct get_cmd { std::string_view key; };
    struct set_cmd { std::string_view key; std::string_view value; int64_t expire_ms = -1; };
    struct del_cmd { std::string_view key; };

    using cmd_action = std::variant<simple_resp, get_cmd, set_cmd, del_cmd>;

    // Connection state
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

    using rslot = resp::resp_slot;

    static inline cmd_action
    classify_command(const resp::parsed_command& cmd) {
        if (cmd.argc == 0)
            return simple_resp{{rslot::ERR, "empty command", 13}};
        if (resp::cmd_is(cmd, "PING"))
            return simple_resp{{rslot::PONG}};
        if (resp::cmd_is(cmd, "QUIT"))
            return simple_resp{{rslot::OK}, true};
        if (resp::cmd_is(cmd, "COMMAND"))
            return simple_resp{{rslot::EMPTY_ARRAY}};
        if (resp::cmd_is(cmd, "CLIENT"))
            return simple_resp{{rslot::OK}};
        if (resp::cmd_is(cmd, "GET")) {
            if (cmd.argc < 2)
                return simple_resp{{rslot::ERR, "wrong number of arguments for 'get' command", 44}};
            return get_cmd{cmd.arg(1)};
        }
        if (resp::cmd_is(cmd, "SET")) {
            if (cmd.argc < 3)
                return simple_resp{{rslot::ERR, "wrong number of arguments for 'set' command", 44}};
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
                return simple_resp{{rslot::ERR, "wrong number of arguments for 'del' command", 44}};
            return del_cmd{cmd.arg(1)};
        }
        // unknown command — message points to cmd name in batch data (alive during pipeline)
        return simple_resp{{rslot::ERR, "unknown command", 15}};
    }

    // ── Execute functions: fill resp_slot instead of sending ──

    static inline auto
    exec_get_slot(get_cmd&& action,
                  store::store_scheduler* store_sched,
                  resp::resp_slot* slot,
                  resp::cmd_batch* batch) {
        auto key_data = action.key.data();
        auto key_len  = static_cast<uint16_t>(action.key.size());

        return store_sched->lookup(key_data, key_len)
            >> visit()
            >> then([slot, batch, store_sched, key_data, key_len](auto&& result) {
                using T = std::decay_t<decltype(result)>;
                if constexpr (std::is_same_v<T, store::hot_result>) {
                    *slot = {resp::resp_slot::BULK, result.data, result.len};
                    return just();
                } else if constexpr (std::is_same_v<T, store::nil_result>) {
                    slot->type = resp::resp_slot::NIL;
                    return just();
                } else {
                    // cold_result: NVMe read → promote → point to slab
                    constexpr uint32_t pg_sz = 4096;
                    uint32_t buf_size = result.page_count * pg_sz;
                    bool is_large = (result.page_count > 1);
                    auto* read_buf = is_large
                        ? store::alloc_large(buf_size)
                        : store::alloc_page();
                    auto* page = nvme::page_obj_pool.get();
                    *page = nvme::sider_page{
                        result.nvme_lba, read_buf,
                        store_sched->ssd_info_for(result.disk_id),
                        static_cast<uint64_t>(buf_size)};

                    return store_sched->nvme_sched_for(result.disk_id)->get(page)
                        >> then([slot, batch, store_sched, key_data, key_len,
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
                                // Promote to hot slab, then point slot to slab memory.
                                store_sched->store.promote(
                                    key_data, key_len, result.version,
                                    val, result.value_len);
                                auto promoted = store_sched->store.get(key_data, key_len);
                                if (auto* hr = std::get_if<store::hot_result>(&promoted)) {
                                    // Promote succeeded — slot points to slab. Free DMA buf.
                                    *slot = {resp::resp_slot::BULK, hr->data, hr->len};
                                    if (is_large) store::free_large(read_buf);
                                    else store::free_page(read_buf);
                                } else {
                                    // Promote failed — keep DMA buf alive, batch owns it.
                                    *slot = {resp::resp_slot::BULK, val, result.value_len};
                                    batch->owned_bufs.push_back({read_buf,
                                        is_large ? store::free_large : store::free_page});
                                }
                            } else {
                                slot->type = resp::resp_slot::NIL;
                                if (is_large) store::free_large(read_buf);
                                else store::free_page(read_buf);
                            }
                            nvme::page_obj_pool.put(page);
                        });
                }
            })
            >> flat();
    }

    static inline auto
    exec_set_slot(set_cmd&& action,
                  store::store_scheduler* store_sched,
                  resp::resp_slot* slot) {
        int64_t expire_at = action.expire_ms >= 0
            ? store::now_ms() + action.expire_ms : -1;
        return store_sched->put(
                action.key.data(),
                static_cast<uint16_t>(action.key.size()),
                action.value.data(),
                static_cast<uint32_t>(action.value.size()),
                expire_at)
            >> then([slot]() {
                slot->type = resp::resp_slot::OK;
            });
    }

    static inline auto
    exec_del_slot(del_cmd&& action,
                  store::store_scheduler* store_sched,
                  resp::resp_slot* slot) {
        return store_sched->del(
                action.key.data(),
                static_cast<uint16_t>(action.key.size()))
            >> then([slot](int count) {
                *slot = {resp::resp_slot::INTEGER, nullptr, 0, count};
            });
    }

    // Generator: yields slot indices [0, count).
    static inline auto
    batch_gen(resp::cmd_batch& batch)
        -> pump::coro::return_yields<uint32_t> {
        for (uint32_t i = 0; i < batch.count; i++)
            co_yield i;
        co_return 0u;
    }

    // Build one send buffer from all resp_slots and send.
    static inline auto
    batch_send(session_t* sess, resp::cmd_batch& batch) {
        // Compute total wire size.
        uint32_t total = 0;
        for (auto& s : batch.responses)
            total += s.wire_size();

        // Build send buffer: one copy per value, directly from slab to buffer.
        auto* buf = new char[total];
        uint32_t pos = 0;
        for (auto& s : batch.responses)
            pos += s.write_to(buf + pos);

        return tcp::send(sess, buf, total);
    }

    template <typename session_t>
    static inline auto
    pick_good_session(auto& cs) -> pump::coro::return_yields<session_t*> {
        while (!cs.is_closed())
            co_yield cs.session;
        co_return nullptr;
    }

    // ── Main connection handler ──
    //
    // recv_batch → cmd_batch
    //   → for_each(slot index) >> concurrent()
    //   → parse + classify + visit + execute → fill slot
    //   → reduce()
    //   → batch send all slots

    /*
    static inline void
    handle_connection(session_sched_t* sched, session_t* session,
                      store::store_scheduler* store_sched) {
        tcp::join(sched, session)
            >> get_context<conn_state<session_sched_t> >()
            >> then([](conn_state<session_sched_t> &state) {
                return pump::coro::make_view_able(
                    pick_good_session<session_t>(state));
            })
            >> for_each()
            // Step 1: recv_batch → cmd_batch
            >> flat_map([](session_t *session) {
                return recv_batch(session);
            })
            // Step 2: set up batch context, inner for_each + concurrent execute
            >> get_context<conn_state<session_sched_t> >()
            >> flat_map([store_sched](auto &&state, resp::cmd_batch &&batch) {
                auto slots = std::make_shared<batch_slots>(batch.count);
                auto *sess = state.session;
                return just()
                    >> get_context<resp::cmd_batch>()
                    >> then([](resp::cmd_batch &b) {
                        return pump::coro::make_view_able(batch_gen(b));
                    })
                    >> for_each()
                    >> concurrent()
                    >> get_context<resp::cmd_batch>()
                    >> flat_map([store_sched](resp::cmd_batch &b, uint32_t slot) {
                        return just()
                            >> then([&b, slot]() -> cmd_action {
                                auto cmd = resp::parse_command(
                                    b.cmd_data(slot), b.cmd_len(slot));
                                return classify_command(cmd);
                            })
                            >> visit()
                            >> then([store_sched, slot](auto &&act) {
                                using A = std::decay_t<decltype(act)>;
                                if constexpr (std::is_same_v<A, simple_resp>) {
                                    slots->responses[slot] = std::move(act.buf);
                                    if (act.quit) slots->has_quit = true;
                                    return just();
                                } else if constexpr (std::is_same_v<A, get_cmd>) {
                                    return exec_get_slot(
                                        std::move(act), slot, store_sched, slots);
                                } else if constexpr (std::is_same_v<A, set_cmd>) {
                                    return exec_set_slot(
                                        std::move(act), slot, store_sched, slots);
                                } else {
                                    return exec_del_slot(
                                        std::move(act), slot, store_sched, slots);
                                }
                            })
                            >> flat();
                    })
                    >> reduce()
                    // Step 3: batch send
                    >> flat_map([slots, sess](bool) {
                        return batch_send(sess, slots);
                    })
                    >> then([slots](bool) {
                        if (slots->has_quit)
                            throw std::runtime_error("quit");
                    })
                    >> submit(pump::core::make_root_context(__fwd__(batch)));
            })
            >> any_exception([](std::exception_ptr e) mutable {
                return just()
                    >> get_context<conn_state<session_sched_t> >()
                    >> then([e](auto &state) {
                        state.close();
                        return e;
                    });
            })
            >> reduce()
            >> submit(pump::core::make_root_context(conn_state{sched, session}));
    }
    */

    struct
    cmd_slot {
        const char* cmd_data;
        uint32_t cmd_len;
        resp::resp_slot* resp;  // points to batch.responses[slot]
    };

    static inline void
    handle_connection(session_sched_t* sched, session_t* session,
                      store::store_scheduler* store_sched) {
        tcp::join(sched, session)
            >> get_context<conn_state<session_sched_t> >()
            >> then([](conn_state<session_sched_t> &state) {
                return pump::coro::make_view_able(
                    pick_good_session<session_t>(state));
            })
            >> for_each()
            // Step 1: recv_batch → cmd_batch
            >> flat_map([](session_t *session) {
                return recv_batch(session);
            })
            // Step 2: set up batch context, inner for_each + concurrent execute
            >> get_context<conn_state<session_sched_t> >()
            >> then([store_sched](auto &&state, resp::cmd_batch &&batch) {
                auto *sess = state.session;
                return just()
                    >> for_each(std::views::iota(0u, batch.count))
                    >> concurrent()
                    >> get_context<resp::cmd_batch>()
                    >> then([](resp::cmd_batch &b, uint32_t i) {
                        return cmd_slot{b.cmd_data(i), b.cmd_len(i), &b.responses[i]};
                    })
                    >> push_result_to_context()
                    >> get_context<cmd_slot>()
                    >> then([](cmd_slot &s) -> cmd_action {
                        return classify_command(resp::parse_command(s.cmd_data, s.cmd_len));
                    })
                    >> visit()
                    >> get_context<resp::cmd_batch, cmd_slot>()
                    >> flat_map([store_sched]<typename T0>(resp::cmd_batch &b, cmd_slot &s, T0 &&act) {
                        if constexpr (std::is_same_v<T0, simple_resp>) {
                            *s.resp = act.slot;
                            if (act.quit) b.has_quit = true;
                            return just();
                        } else if constexpr (std::is_same_v<T0, get_cmd>) {
                            return exec_get_slot(
                                std::forward<T0>(act), store_sched, s.resp, &b);
                        } else if constexpr (std::is_same_v<T0, set_cmd>) {
                            return exec_set_slot(
                                std::forward<T0>(act), store_sched, s.resp);
                        } else {
                            return exec_del_slot(
                                std::forward<T0>(act), store_sched, s.resp);
                        }
                    })
                    >> pop_context()
                    >> reduce()
                    >> get_context<resp::cmd_batch>()
                    >> flat_map([sess](resp::cmd_batch &b, ...) {
                        return batch_send(sess, b);
                    })
                    >> submit(pump::core::make_root_context(__fwd__(batch)));
            })
            >> any_exception([](std::exception_ptr e) mutable {
                return just()
                    >> get_context<conn_state<session_sched_t> >()
                    >> then([e](auto &state) {
                        state.close();
                        return e;
                    });
            })
            >> reduce()
            >> submit(pump::core::make_root_context(conn_state{sched, session}));
    }

} // namespace sider::server
