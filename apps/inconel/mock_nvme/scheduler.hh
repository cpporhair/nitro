#ifndef APPS_INCONEL_MOCK_NVME_SCHEDULER_HH
#define APPS_INCONEL_MOCK_NVME_SCHEDULER_HH

#include <functional>

#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "./device.hh"

namespace apps::inconel::mock_nvme {

    constexpr uint32_t IO_FLAGS_FUA = 0x01;

    struct scheduler;

    enum class op_type : uint8_t { write, read, flush, trim };

    // ── 1. req ──

    struct req {
        op_type type;
        uint64_t lba;
        void* buf;
        uint32_t num_lbas;
        uint32_t flags;
        std::move_only_function<void(bool)> cb;
    };

    namespace _mock_nvme {

        // ── 2. op ──

        struct op {
            constexpr static bool mock_nvme_op = true;
            scheduler* sched;
            op_type type;
            uint64_t lba;
            void* buf;
            uint32_t num_lbas;
            uint32_t flags;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        // ── 3. sender ──

        struct sender {
            scheduler* sched;
            op_type type;
            uint64_t lba;
            void* buf;
            uint32_t num_lbas;
            uint32_t flags;

            auto
            make_op() {
                return op{
                    .sched = sched, .type = type, .lba = lba,
                    .buf = buf, .num_lbas = num_lbas, .flags = flags
                };
            }

            template<typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    // ── 4. scheduler ──

    struct scheduler {
        mock_device* device;
        pump::core::per_core::queue<req*> req_queue;

        explicit
        scheduler(mock_device* dev, size_t queue_depth = 2048)
            : device(dev)
            , req_queue(queue_depth) {
        }

        void
        schedule(req* r) {
            req_queue.try_enqueue(r);
        }

        bool
        advance() {
            return req_queue.drain([this](req* r) {
                bool ok = false;
                switch (r->type) {
                    case op_type::write:
                        ok = device->do_write(r->lba, r->buf, r->num_lbas);
                        break;
                    case op_type::read:
                        ok = device->do_read(r->lba, r->buf, r->num_lbas);
                        break;
                    case op_type::flush:
                        ok = device->do_flush();
                        break;
                    case op_type::trim:
                        ok = device->do_trim(r->lba, r->num_lbas);
                        break;
                }
                r->cb(ok);
                delete r;
            });
        }

        template<typename runtime_t>
        bool
        advance(runtime_t&) {
            return advance();
        }

        // ── Sender factories ──

        auto
        write(uint64_t lba, const void* data, uint32_t num_lbas, uint32_t flags = 0) {
            return _mock_nvme::sender{
                this, op_type::write, lba,
                const_cast<void*>(data), num_lbas, flags
            };
        }

        auto
        read(uint64_t lba, void* buf, uint32_t num_lbas) {
            return _mock_nvme::sender{
                this, op_type::read, lba,
                buf, num_lbas, 0
            };
        }

        auto
        flush() {
            return _mock_nvme::sender{
                this, op_type::flush, 0,
                nullptr, 0, 0
            };
        }

        auto
        trim(uint64_t lba, uint32_t num_lbas) {
            return _mock_nvme::sender{
                this, op_type::trim, lba,
                nullptr, num_lbas, 0
            };
        }

        mock_device* get_device() { return device; }
        const mock_device* get_device() const { return device; }
    };

    // ── op::start deferred impl (needs scheduler complete type) ──

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _mock_nvme::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule(new req{
            type, lba, buf, num_lbas, flags,
            [ctx = ctx, scope = scope](bool ok) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope, ok);
            }
        });
    }

}

// ── 5. op_pusher specialization ──

namespace pump::core {
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::mock_nvme_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static
        void
        push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    // ── 6. compute_sender_type specialization ──

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::mock_nvme::_mock_nvme::sender> {
        consteval static uint32_t
        count_value() { return 1; }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<bool>{};
        }
    };
}

#endif //APPS_INCONEL_MOCK_NVME_SCHEDULER_HH
