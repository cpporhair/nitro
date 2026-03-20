#pragma once

#include <cstring>
#include <functional>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/lock_free_queue.hh"

#include "store/store.hh"

namespace sider::store {

    // Type-erased store request.
    struct store_req {
        std::move_only_function<void(kv_store&)> fn;
    };

    struct store_scheduler;

    namespace _store_ops {

        // ── lookup: key → get_result ──
        // Phase 1: key pointer borrowed from caller (frame in context keeps it alive).
        // Phase 2: will need owned copy for cross-core routing.

        template<typename sched_t>
        struct lookup_op {
            constexpr static bool store_op = true;
            sched_t* sched;
            const char* key;
            uint16_t key_len;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope) {
                sched->schedule(new store_req{
                    [k = key, kl = key_len, ctx, scope](kv_store& store) mutable {
                        auto result = store.get(k, kl);
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(
                            ctx, scope, std::move(result));
                    }
                });
            }
        };

        template<typename sched_t>
        struct lookup_sender {
            sched_t* sched;
            const char* key;
            uint16_t key_len;

            auto make_op() { return lookup_op<sched_t>{sched, key, key_len}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };

        // ── put: key + value + expire_at → (no value) ──

        template<typename sched_t>
        struct put_op {
            constexpr static bool store_op = true;
            sched_t* sched;
            const char* key;
            uint16_t key_len;
            const char* value;
            uint16_t value_len;
            int64_t expire_at;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope) {
                sched->schedule(new store_req{
                    [k = key, kl = key_len, v = value, vl = value_len,
                     ea = expire_at, ctx, scope](kv_store& store) mutable {
                        store.set(k, kl, v, vl, ea);
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
                    }
                });
            }
        };

        template<typename sched_t>
        struct put_sender {
            sched_t* sched;
            const char* key;
            uint16_t key_len;
            const char* value;
            uint16_t value_len;
            int64_t expire_at;

            auto make_op() { return put_op<sched_t>{sched, key, key_len, value, value_len, expire_at}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };

        // ── del: key → int (deleted count) ──

        template<typename sched_t>
        struct del_op {
            constexpr static bool store_op = true;
            sched_t* sched;
            const char* key;
            uint16_t key_len;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope) {
                sched->schedule(new store_req{
                    [k = key, kl = key_len, ctx, scope](kv_store& store) mutable {
                        int count = store.del(k, kl);
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(
                            ctx, scope, count);
                    }
                });
            }
        };

        template<typename sched_t>
        struct del_sender {
            sched_t* sched;
            const char* key;
            uint16_t key_len;

            auto make_op() { return del_op<sched_t>{sched, key, key_len}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };

    } // namespace _store_ops

    // ── store_scheduler ──
    //
    // Phase 1: local::queue (single-core, same thread).
    //   Key/value pointers borrow from caller (net_frame in context).
    // Phase 2: switch to per_core::queue, will need owned data transfer.

    struct store_scheduler {
        kv_store store;
        pump::core::local::queue<store_req*> req_q{4096};
        int advance_count_ = 0;

        void schedule(store_req* r) { req_q.try_enqueue(r); }

        bool advance() {
            bool progress = false;
            store_req* r;
            while (req_q.try_dequeue(r)) {
                r->fn(store);
                delete r;
                progress = true;
            }

            // Active expiry: sample every 64 advance() calls.
            if (++advance_count_ >= 64) {
                advance_count_ = 0;
                store.expire_scan(20);
            }

            return progress;
        }

        auto lookup(const char* key, uint16_t key_len) {
            return _store_ops::lookup_sender<store_scheduler>{this, key, key_len};
        }

        auto put(const char* key, uint16_t key_len,
                 const char* value, uint16_t value_len,
                 int64_t expire_at = -1) {
            return _store_ops::put_sender<store_scheduler>{
                this, key, key_len, value, value_len, expire_at};
        }

        auto del(const char* key, uint16_t key_len) {
            return _store_ops::del_sender<store_scheduler>{this, key, key_len};
        }

        uint64_t memory_used_bytes() const { return store.memory_used_bytes(); }
        uint32_t key_count()         const { return store.key_count(); }
    };

} // namespace sider::store

// ── pump::core specializations ──

namespace pump::core {

    // Single op_pusher for all store ops (shared store_op tag).
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::store_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static inline void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    // lookup → get_result
    template<typename ctx_t, typename sched_t>
    struct compute_sender_type<ctx_t,
            sider::store::_store_ops::lookup_sender<sched_t>> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<sider::store::get_result>{};
        }
    };

    // put → void (0 values)
    template<typename ctx_t, typename sched_t>
    struct compute_sender_type<ctx_t,
            sider::store::_store_ops::put_sender<sched_t>> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    // del → int
    template<typename ctx_t, typename sched_t>
    struct compute_sender_type<ctx_t,
            sider::store::_store_ops::del_sender<sched_t>> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<int>{};
        }
    };

} // namespace pump::core
