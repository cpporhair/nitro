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

    // Owned copies of key/value data for lifetime safety across async boundaries.
    struct owned_kv {
        char* key = nullptr;
        uint16_t key_len = 0;
        char* value = nullptr;
        uint16_t value_len = 0;

        owned_kv() = default;
        ~owned_kv() { delete[] key; delete[] value; }

        owned_kv(owned_kv&& o) noexcept
            : key(o.key), key_len(o.key_len), value(o.value), value_len(o.value_len) {
            o.key = nullptr; o.value = nullptr;
        }
        owned_kv& operator=(owned_kv&& o) noexcept {
            if (this != &o) {
                delete[] key; delete[] value;
                key = o.key; key_len = o.key_len;
                value = o.value; value_len = o.value_len;
                o.key = nullptr; o.value = nullptr;
            }
            return *this;
        }
        owned_kv(const owned_kv&) = delete;
        owned_kv& operator=(const owned_kv&) = delete;

        static owned_kv make_key(const char* k, uint16_t kl) {
            owned_kv r;
            r.key = new char[kl]; std::memcpy(r.key, k, kl); r.key_len = kl;
            return r;
        }
        static owned_kv make_kv(const char* k, uint16_t kl, const char* v, uint16_t vl) {
            auto r = make_key(k, kl);
            r.value = new char[vl]; std::memcpy(r.value, v, vl); r.value_len = vl;
            return r;
        }
    };

    struct store_scheduler;

    namespace _store_ops {

        // ── lookup: key → get_result ──

        template<typename sched_t>
        struct lookup_op {
            constexpr static bool store_op = true;
            sched_t* sched;
            owned_kv data;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope) {
                char* k = data.key; data.key = nullptr;
                uint16_t kl = data.key_len;
                sched->schedule(new store_req{
                    [k, kl, ctx, scope](kv_store& store) mutable {
                        auto result = store.get(k, kl);
                        delete[] k;
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(
                            ctx, scope, std::move(result));
                    }
                });
            }
        };

        template<typename sched_t>
        struct lookup_sender {
            sched_t* sched;
            owned_kv data;

            auto make_op() { return lookup_op<sched_t>{sched, std::move(data)}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };

        // ── put: key + value → (no value) ──

        template<typename sched_t>
        struct put_op {
            constexpr static bool store_op = true;
            sched_t* sched;
            owned_kv data;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope) {
                char* k = data.key; data.key = nullptr;
                uint16_t kl = data.key_len;
                char* v = data.value; data.value = nullptr;
                uint16_t vl = data.value_len;
                sched->schedule(new store_req{
                    [k, kl, v, vl, ctx, scope](kv_store& store) mutable {
                        store.set(k, kl, v, vl);
                        delete[] k; delete[] v;
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
                    }
                });
            }
        };

        template<typename sched_t>
        struct put_sender {
            sched_t* sched;
            owned_kv data;

            auto make_op() { return put_op<sched_t>{sched, std::move(data)}; }

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
            owned_kv data;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope) {
                char* k = data.key; data.key = nullptr;
                uint16_t kl = data.key_len;
                sched->schedule(new store_req{
                    [k, kl, ctx, scope](kv_store& store) mutable {
                        int count = store.del(k, kl);
                        delete[] k;
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(
                            ctx, scope, count);
                    }
                });
            }
        };

        template<typename sched_t>
        struct del_sender {
            sched_t* sched;
            owned_kv data;

            auto make_op() { return del_op<sched_t>{sched, std::move(data)}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };

    } // namespace _store_ops

    // ── store_scheduler ──
    //
    // Phase 1.3: local::queue (single-core, same thread).
    // Phase 2.x: switch to per_core::queue for cross-core routing.

    struct store_scheduler {
        kv_store store;
        pump::core::local::queue<store_req*> req_q{4096};

        void schedule(store_req* r) { req_q.try_enqueue(r); }

        bool advance() {
            bool progress = false;
            store_req* r;
            while (req_q.try_dequeue(r)) {
                r->fn(store);
                delete r;
                progress = true;
            }
            return progress;
        }

        auto lookup(const char* key, uint16_t key_len) {
            return _store_ops::lookup_sender<store_scheduler>{
                this, owned_kv::make_key(key, key_len)};
        }

        auto put(const char* key, uint16_t key_len,
                 const char* value, uint16_t value_len) {
            return _store_ops::put_sender<store_scheduler>{
                this, owned_kv::make_kv(key, key_len, value, value_len)};
        }

        auto del(const char* key, uint16_t key_len) {
            return _store_ops::del_sender<store_scheduler>{
                this, owned_kv::make_key(key, key_len)};
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
