#pragma once

#include <atomic>
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
        ~conn_state() {
            if (session)
                session->broadcast(::pump::scheduler::net::pipeline_end);
        }

        conn_state(conn_state&& o) noexcept
            : sched(o.sched), session(o.session), closed(o.closed) {
            o.session = nullptr;
        }
        conn_state& operator=(conn_state&& o) noexcept {
            if (this != &o) {
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
                  resp::resp_slot* slot) {
        auto key_data = action.key.data();
        auto key_len  = static_cast<uint16_t>(action.key.size());

        return store_sched->lookup(key_data, key_len)
            >> visit()
            >> then([slot, store_sched, key_data, key_len](auto&& result) {
                using T = std::decay_t<decltype(result)>;
                if constexpr (std::is_same_v<T, store::hot_result>) {
                    *slot = {resp::resp_slot::BULK, result.data, result.len};
                    return just();
                } else if constexpr (std::is_same_v<T, store::inline_result>) {
                    slot->set_inline(result.data, result.len);
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
                        >> then([slot, store_sched, key_data, key_len,
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
                                    // Promote failed — keep DMA buf alive, slot owns it.
                                    *slot = {resp::resp_slot::BULK, val, result.value_len};
                                    slot->set_owned(read_buf,
                                        is_large ? store::free_large : store::free_page);
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
            >> then([slot](bool ok) {
                if (ok) slot->type = resp::resp_slot::OK;
                else *slot = {resp::resp_slot::ERR,
                    "OOM memory limit exceeded", 25};
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

    // Route key to target store: hash(key) % num_stores.
    static inline store::store_scheduler*
    route_store(const char* key, uint16_t key_len,
                store::store_scheduler** stores, uint32_t num_stores) {
        return stores[store::hash_key(key, key_len) % num_stores];
    }

    // ── Batch route: classify + group + inline local + batched remote ──
    //
    // Replaces for_each >> concurrent >> per_cmd_store_sender >> reduce.
    //   - Classify all commands in the batch
    //   - Group by target store core (hash % N)
    //   - Execute local commands inline (zero overhead, no alloc)
    //   - Send ONE store_req per remote core (batched)
    //   - When all remote cores complete, continue pipeline
    //
    // Reduces cross-core overhead from O(batch_size) to O(num_cores).
    // Cold GET returns NIL in batch path (use enough memory for hot benchmark).

    namespace _batch_route {

        // Fill resp_slot from GET result. Returns true if cold (needs NVMe read).
        static inline bool fill_get_resp(store::lookup_result& result,
                                         resp::resp_slot* slot) {
            if (auto* hr = std::get_if<store::hot_result>(&result)) {
                *slot = {resp::resp_slot::BULK, hr->data, hr->len};
            } else if (auto* ir = std::get_if<store::inline_result>(&result)) {
                slot->set_inline(ir->data, ir->len);
            } else if (std::get_if<store::nil_result>(&result)) {
                slot->type = resp::resp_slot::NIL;
            } else {
                return true;  // cold_result — needs NVMe read
            }
            return false;
        }

        // Lightweight command descriptor for remote execution.
        struct remote_cmd {
            resp::resp_slot* resp;
            uint8_t op;        // 0=get, 1=set, 2=del
            const char* key;
            uint16_t key_len;
            const char* value;      // SET only
            uint32_t value_len;     // SET only
            int64_t expire_at;      // SET only (absolute)
        };

        // Async completion state — tracks remote batches + cold reads.
        struct pending_state {
            std::atomic<uint32_t> remaining;
            std::move_only_function<void()> continue_fn;

            void complete_one() {
                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    auto fn = std::move(continue_fn);
                    delete this;
                    fn();
                }
            }
        };

        // Start NVMe cold read sub-pipeline, calls state->complete_one() when done.
        static inline void
        launch_cold_read(store::cold_result cr, store::store_scheduler* sched,
                         resp::resp_slot* slot, const char* key_data,
                         uint16_t key_len, pending_state* state) {
            using namespace pump::sender;
            constexpr uint32_t pg_sz = 4096;
            uint32_t buf_size = cr.page_count * pg_sz;
            bool is_large = (cr.page_count > 1);
            auto* read_buf = is_large
                ? store::alloc_large(buf_size) : store::alloc_page();
            auto* page = nvme::page_obj_pool.get();
            *page = nvme::sider_page{
                cr.nvme_lba, read_buf,
                sched->ssd_info_for(cr.disk_id),
                static_cast<uint64_t>(buf_size)};

            sched->nvme_sched_for(cr.disk_id)->get(page)
                >> then([slot, sched, key_data, key_len,
                         cr, page, read_buf, is_large, state]
                        (pump::scheduler::nvme::get::res<nvme::sider_page>&& res) {
                    if (res.is_success()) {
                        char* val = is_large ? read_buf
                            : read_buf + store::slot_offset(
                                static_cast<store::size_class_t>(cr.size_class),
                                cr.slot_index);
                        sched->store.promote(key_data, key_len, cr.version,
                                             val, cr.value_len);
                        auto promoted = sched->store.get(key_data, key_len);
                        if (auto* hr = std::get_if<store::hot_result>(&promoted)) {
                            *slot = {resp::resp_slot::BULK, hr->data, hr->len};
                            if (is_large) store::free_large(read_buf);
                            else store::free_page(read_buf);
                        } else {
                            *slot = {resp::resp_slot::BULK, val, cr.value_len};
                            slot->set_owned(read_buf,
                                is_large ? store::free_large : store::free_page);
                        }
                    } else {
                        slot->type = resp::resp_slot::NIL;
                        if (is_large) store::free_large(read_buf);
                        else store::free_page(read_buf);
                    }
                    nvme::page_obj_pool.put(page);
                    state->complete_one();
                })
                >> submit(pump::core::make_root_context());
        }

        struct op {
            constexpr static bool batch_route_op = true;
            store::store_scheduler** stores;
            uint32_t num_stores;

            template<uint32_t pos, typename ctx_t, typename scope_t, typename ...Extra>
            void start(ctx_t& ctx, scope_t& scope, resp::cmd_batch& batch, Extra&&...) {
                uint32_t local_core = pump::core::this_core_id;

                // Find which store index is local to this core.
                uint32_t local_idx = 0;
                for (uint32_t ci = 0; ci < num_stores; ci++) {
                    if (stores[ci]->owner_core_ == local_core) {
                        local_idx = ci;
                        break;
                    }
                }

                // Group remote commands by target core.
                // Track local cold reads.
                auto groups = std::make_unique<std::vector<remote_cmd>[]>(num_stores);

                struct local_cold {
                    store::cold_result cr;
                    resp::resp_slot* resp;
                    const char* key;
                    uint16_t key_len;
                };
                std::vector<local_cold> local_colds;

                for (uint32_t i = 0; i < batch.count; i++) {
                    auto cmd = resp::parse_command(batch.cmd_data(i), batch.cmd_len(i));
                    auto action = classify_command(cmd);
                    auto* resp = &batch.responses[i];

                    std::visit([&](auto&& act) {
                        using A = std::decay_t<decltype(act)>;
                        if constexpr (std::is_same_v<A, simple_resp>) {
                            *resp = act.slot;
                            if (act.quit) batch.has_quit = true;
                        } else {
                            auto key = act.key.data();
                            auto key_len = static_cast<uint16_t>(act.key.size());
                            uint32_t target_idx = store::hash_key(key, key_len) % num_stores;

                            if (target_idx == local_idx) {
                                // Execute inline on local store.
                                if constexpr (std::is_same_v<A, get_cmd>) {
                                    auto r = stores[local_idx]->store.get(key, key_len);
                                    if (fill_get_resp(r, resp)) {
                                        // Cold — need NVMe read
                                        local_colds.push_back(
                                            {std::get<store::cold_result>(r),
                                             resp, key, key_len});
                                    }
                                } else if constexpr (std::is_same_v<A, set_cmd>) {
                                    int64_t ea = act.expire_ms >= 0
                                        ? store::now_ms() + act.expire_ms : -1;
                                    bool ok = stores[local_idx]->store.set(
                                        key, key_len, act.value.data(),
                                        static_cast<uint32_t>(act.value.size()), ea);
                                    if (ok) resp->type = resp::resp_slot::OK;
                                    else *resp = {resp::resp_slot::ERR,
                                        "OOM memory limit exceeded", 25};
                                } else {
                                    int cnt = stores[local_idx]->store.del(key, key_len);
                                    *resp = {resp::resp_slot::INTEGER, nullptr, 0, cnt};
                                }
                            } else {
                                // Add to remote group.
                                uint8_t op_type;
                                const char* val = nullptr;
                                uint32_t val_len = 0;
                                int64_t ea = 0;
                                if constexpr (std::is_same_v<A, get_cmd>) {
                                    op_type = 0;
                                } else if constexpr (std::is_same_v<A, set_cmd>) {
                                    op_type = 1;
                                    val = act.value.data();
                                    val_len = static_cast<uint32_t>(act.value.size());
                                    ea = act.expire_ms >= 0
                                        ? store::now_ms() + act.expire_ms : -1;
                                } else {
                                    op_type = 2;
                                }
                                groups[target_idx].push_back(
                                    {resp, op_type, key, key_len, val, val_len, ea});
                            }
                        }
                    }, action);
                }

                // Count remote cores with pending work.
                uint32_t remote_cores = 0;
                for (uint32_t ci = 0; ci < num_stores; ci++)
                    if (ci != local_idx && !groups[ci].empty())
                        remote_cores++;

                uint32_t local_cold_count = static_cast<uint32_t>(local_colds.size());
                uint32_t total_pending = remote_cores + local_cold_count;

                if (total_pending == 0) {
                    // All local, no cold reads — continue immediately.
                    pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
                    return;
                }

                auto* state = new pending_state{
                    {total_pending},
                    [ctx = ctx, scope = scope]() mutable {
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
                    }
                };

                // Launch local cold reads.
                for (auto& lc : local_colds)
                    launch_cold_read(lc.cr, stores[local_idx], lc.resp,
                                     lc.key, lc.key_len, state);

                // Schedule ONE store_req per remote core.
                for (uint32_t ci = 0; ci < num_stores; ci++) {
                    if (ci == local_idx || groups[ci].empty()) continue;

                    stores[ci]->schedule(new store::store_req{
                        [cmds = std::move(groups[ci]), state,
                         sched = stores[ci]](store::kv_store& store) {
                            uint32_t cold_count = 0;
                            for (auto& c : cmds) {
                                switch (c.op) {
                                    case 0: { // GET
                                        auto r = store.get(c.key, c.key_len);
                                        if (fill_get_resp(r, c.resp)) {
                                            cold_count++;
                                            launch_cold_read(
                                                std::get<store::cold_result>(r),
                                                sched, c.resp, c.key, c.key_len,
                                                state);
                                        }
                                        break;
                                    }
                                    case 1: { // SET
                                        bool ok = store.set(c.key, c.key_len,
                                                  c.value, c.value_len, c.expire_at);
                                        if (ok) c.resp->type = resp::resp_slot::OK;
                                        else *c.resp = {resp::resp_slot::ERR,
                                            "OOM memory limit exceeded", 25};
                                        break;
                                    }
                                    case 2: { // DEL
                                        int cnt = store.del(c.key, c.key_len);
                                        *c.resp = {resp::resp_slot::INTEGER, nullptr, 0, cnt};
                                        break;
                                    }
                                }
                            }
                            // Add cold reads to counter before signaling batch done.
                            if (cold_count > 0)
                                state->remaining.fetch_add(cold_count,
                                    std::memory_order_relaxed);
                            state->complete_one();
                        }
                    });
                }
            }
        };

        template<typename prev_t>
        struct sender {
            using prev_type = prev_t;
            prev_t prev;
            store::store_scheduler** stores;
            uint32_t num_stores;

            sender(prev_t&& p, store::store_scheduler** s, uint32_t n)
                : prev(__fwd__(p)), stores(s), num_stores(n) {}

            sender(sender&& o) noexcept
                : prev(__fwd__(o.prev)), stores(o.stores), num_stores(o.num_stores) {}

            auto make_op() { return op{stores, num_stores}; }

            template<typename ctx_t>
            auto connect() {
                return prev.template connect<ctx_t>().push_back(make_op());
            }
        };

        struct cpo {
            template<typename sender_t>
            auto operator()(sender_t&& s,
                            store::store_scheduler** stores,
                            uint32_t num_stores) const {
                return sender<sender_t>{__fwd__(s), stores, num_stores};
            }
        };

    } // namespace _batch_route

    static inline auto batch_route(store::store_scheduler** stores, uint32_t num_stores) {
        return pump::core::bind_back<_batch_route::cpo,
                                     store::store_scheduler**,
                                     uint32_t>(
            _batch_route::cpo{}, stores, num_stores);
    }

    // ── Main connection handler ──
    //
    // recv_batch → cmd_batch
    //   → batch_route: classify + group + inline local + remote batch
    //   → batch send all slots

    static inline void
    handle_connection(session_sched_t* sched, session_t* session,
                      store::store_scheduler** stores, uint32_t num_stores) {
        tcp::join(sched, session)
            >> get_context<conn_state<session_sched_t> >()
            >> then([](conn_state<session_sched_t> &state) {
                return pump::coro::make_view_able(
                    pick_good_session<session_t>(state));
            })
            >> for_each()
            >> flat_map([](session_t *session) {
                return recv_batch(session);
            })
            >> get_context<conn_state<session_sched_t> >()
            >> then([stores, num_stores](auto &&state, resp::cmd_batch &&batch) {
                auto *sess = state.session;
                return just(0)
                    >> get_context<resp::cmd_batch>()
                    >> batch_route(stores, num_stores)
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

// ── pump::core specializations for batch_route ──

namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::batch_route_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t, typename ...V>
        static inline void push_value(ctx_t& ctx, scope_t& scope, V&&... v) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(
                ctx, scope, std::forward<V>(v)...);
        }
    };

    template<typename ctx_t, typename prev_t>
    struct compute_sender_type<ctx_t, sider::server::_batch_route::sender<prev_t>> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

} // namespace pump::core
