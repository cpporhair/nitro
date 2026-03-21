#pragma once

#include <cstring>
#include <functional>

#include "pump/core/op_pusher.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"

#include "store/store.hh"
#include "nvme/page.hh"
#include "nvme/allocator.hh"
#include "env/scheduler/nvme/scheduler.hh"

namespace sider::store {

    // Type-erased store request.
    struct store_req {
        std::move_only_function<void(kv_store&)> fn;
    };

    struct store_scheduler;

    namespace _store_ops {

        // ── lookup: key → get_result ──

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

    struct store_scheduler {
        kv_store store;
        pump::core::local::queue<store_req*> req_q{4096};
        int advance_count_ = 0;

        // NVMe eviction support (nullptr = no NVMe, discard-only).
        using nvme_sched_t = pump::scheduler::nvme::scheduler<nvme::sider_page>;
        nvme_sched_t*      nvme_sched_ = nullptr;
        nvme::nvme_allocator* nvme_alloc_ = nullptr;
        const pump::scheduler::nvme::ssd<nvme::sider_page>* ssd_info_ = nullptr;
        uint32_t           in_flight_evictions_ = 0;

        void schedule(store_req* r) { req_q.try_enqueue(r); }

        void set_eviction_config(uint64_t memory_limit,
                                 double begin_pct, double urgent_pct) {
            store.evict_cfg_ = {memory_limit, begin_pct, urgent_pct};
        }

        void set_nvme(nvme_sched_t* sched, nvme::nvme_allocator* alloc,
                      const pump::scheduler::nvme::ssd<nvme::sider_page>* ssd) {
            nvme_sched_ = sched;
            nvme_alloc_ = alloc;
            ssd_info_ = ssd;
        }

        bool advance();            // defined in scheduler_impl.hh
        bool try_start_eviction(); // defined in scheduler_impl.hh

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

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::store_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static inline void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    // lookup → lookup_result (variant<hot_result, cold_result, nil_result>)
    template<typename ctx_t, typename sched_t>
    struct compute_sender_type<ctx_t,
            sider::store::_store_ops::lookup_sender<sched_t>> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<sider::store::lookup_result>{};
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
