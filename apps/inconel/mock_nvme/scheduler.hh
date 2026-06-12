#ifndef APPS_INCONEL_MOCK_NVME_SCHEDULER_HH
#define APPS_INCONEL_MOCK_NVME_SCHEDULER_HH

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "spdk/env.h"
#include "spdk/nvme.h"

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/ring_queue.hh"

#include "../memory/dma_page_pool.hh"
#include "../memory/frame.hh"
#include "../nvme/lba_page.hh"
#include "./device.hh"

namespace apps::inconel::mock_nvme {

    class mock_scheduler;

    namespace _mock_io {

        enum class kind : uint8_t {
            read_frame,
            write_frame,
            read_contiguous,
            write_contiguous,
            flush,
            trim,
        };

        struct req {
            kind op = kind::flush;
            memory::segmented_page_frame* frame = nullptr;
            uint64_t lba = 0;
            void* mutable_buf = nullptr;
            const void* const_buf = nullptr;
            uint32_t num_lbas = 0;
            uint32_t flags = 0;
            std::move_only_function<void(bool)> cb;
        };

        template <typename scheduler_t>
        struct op {
            constexpr static bool mock_nvme_io_op = true;

            scheduler_t* scheduler = nullptr;
            kind request_kind = kind::flush;
            memory::segmented_page_frame* frame = nullptr;
            uint64_t lba = 0;
            void* mutable_buf = nullptr;
            const void* const_buf = nullptr;
            uint32_t num_lbas = 0;
            uint32_t flags = 0;

            template <uint32_t pos, typename context_t, typename scope_t>
            void
            start(context_t& context, scope_t& scope) {
                scheduler->schedule(new req{
                    .op = request_kind,
                    .frame = frame,
                    .lba = lba,
                    .mutable_buf = mutable_buf,
                    .const_buf = const_buf,
                    .num_lbas = num_lbas,
                    .flags = flags,
                    .cb = [context = context, scope = scope](bool ok) mutable {
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(
                            context, scope, ok);
                    },
                });
            }
        };

        template <typename scheduler_t>
        struct sender {
            scheduler_t* scheduler = nullptr;
            kind request_kind = kind::flush;
            memory::segmented_page_frame* frame = nullptr;
            uint64_t lba = 0;
            void* mutable_buf = nullptr;
            const void* const_buf = nullptr;
            uint32_t num_lbas = 0;
            uint32_t flags = 0;

            [[nodiscard]] auto
            make_op() {
                return op<scheduler_t>{
                    .scheduler = scheduler,
                    .request_kind = request_kind,
                    .frame = frame,
                    .lba = lba,
                    .mutable_buf = mutable_buf,
                    .const_buf = const_buf,
                    .num_lbas = num_lbas,
                    .flags = flags,
                };
            }

            template <typename context_t>
            [[nodiscard]] auto
            connect() {
                return pump::core::builder::op_list_builder<0>().push_back(
                    make_op());
            }
        };

    }  // namespace _mock_io

    class mock_scheduler {
    public:
        using qpair_t = mock_device::core_handle;

        mock_scheduler(qpair_t* qpair,
                       uint32_t lba_size,
                       memory::dma_page_allocator allocator =
                           memory::make_heap_dma_page_allocator(),
                       size_t queue_depth = 2048,
                       size_t local_depth = 128,
                       uint32_t alignment = 4096,
                       int numa_id = SPDK_ENV_NUMA_ID_ANY,
                       uint16_t device_id = 0)
            : qpair_(qpair)
            , device_(qpair ? qpair->owner : nullptr)
            , req_q_(checked_queue_depth(queue_depth), qpair ? qpair->core : 0)
            , local_q_(checked_local_depth(local_depth))
            , pool_(lba_size, alignment, numa_id, allocator)
            , device_id_(device_id) {
            validate_qpair(qpair_);
        }

        mock_scheduler(qpair_t* qpair,
                       uint32_t lba_size,
                       uint64_t pool_pages,
                       size_t queue_depth = 2048,
                       size_t local_depth = 128,
                       uint32_t alignment = 4096,
                       int numa_id = SPDK_ENV_NUMA_ID_ANY,
                       uint16_t device_id = 0)
            : mock_scheduler(qpair,
                             lba_size,
                             memory::make_heap_dma_page_allocator(),
                             queue_depth,
                             local_depth,
                             alignment,
                             numa_id,
                             device_id) {
            (void)pool_pages;
        }

        [[nodiscard]] uint32_t
        lba_size() const noexcept {
            return pool_.lba_size();
        }

        bool
        advance() {
            bool worked = false;

            while (local_q_.size() < local_q_.capacity()) {
                auto item = req_q_.try_dequeue();
                if (!item.has_value()) break;
                local_q_.enqueue(*item);
                worked = true;
            }

            _mock_io::req* raw = nullptr;
            while (local_q_.dequeue(raw)) {
                std::unique_ptr<_mock_io::req> req(raw);
                const bool ok = handle(*req);
                auto cb = std::move(req->cb);
                req.reset();
                if (cb) cb(ok);
                worked = true;
            }

            return worked;
        }

        template <typename Runtime>
        bool
        advance(Runtime&) {
            return advance();
        }

        [[nodiscard]] auto
        read_frame(memory::segmented_page_frame* frame, uint32_t flags = 0) {
            return make_frame_sender(_mock_io::kind::read_frame, frame, flags);
        }

        [[nodiscard]] auto
        write_frame(memory::segmented_page_frame* frame, uint32_t flags = 0) {
            return make_frame_sender(_mock_io::kind::write_frame, frame, flags);
        }

        [[nodiscard]] auto
        read(uint64_t lba, void* buf, uint32_t num_lbas) {
            validate_io_span(num_lbas, "read");
            if (buf == nullptr) {
                throw std::invalid_argument(
                    "inconel::mock_nvme::mock_scheduler::read: buf is null");
            }
            return _mock_io::sender<mock_scheduler>{
                .scheduler = this,
                .request_kind = _mock_io::kind::read_contiguous,
                .lba = lba,
                .mutable_buf = buf,
                .num_lbas = num_lbas,
            };
        }

        [[nodiscard]] auto
        write(uint64_t lba,
              const void* data,
              uint32_t num_lbas,
              uint32_t flags = 0) {
            validate_io_span(num_lbas, "write");
            if (data == nullptr) {
                throw std::invalid_argument(
                    "inconel::mock_nvme::mock_scheduler::write: data is null");
            }
            return _mock_io::sender<mock_scheduler>{
                .scheduler = this,
                .request_kind = _mock_io::kind::write_contiguous,
                .lba = lba,
                .const_buf = data,
                .num_lbas = num_lbas,
                .flags = flags,
            };
        }

        [[nodiscard]] auto
        flush() {
            return _mock_io::sender<mock_scheduler>{
                .scheduler = this,
                .request_kind = _mock_io::kind::flush,
            };
        }

        [[nodiscard]] auto
        trim(uint64_t lba, uint32_t num_lbas) {
            return _mock_io::sender<mock_scheduler>{
                .scheduler = this,
                .request_kind = _mock_io::kind::trim,
                .lba = lba,
                .num_lbas = num_lbas,
            };
        }

        [[nodiscard]] auto
        trim_ns_lba(uint64_t lba, uint32_t num_lbas) {
            return trim(lba, num_lbas);
        }

        void
        schedule(_mock_io::req* r) {
            if (!req_q_.try_enqueue(r)) {
                delete r;
                throw std::runtime_error(
                    "inconel::mock_nvme::mock_scheduler: request queue full");
            }
        }

    private:
        [[nodiscard]] static bool
        is_power_of_two(size_t n) noexcept {
            return n != 0 && (n & (n - 1)) == 0;
        }

        [[nodiscard]] static size_t
        checked_queue_depth(size_t depth) {
            if (depth < 2 || !is_power_of_two(depth)) {
                throw std::invalid_argument(
                    "inconel::mock_nvme::mock_scheduler: queue_depth must be a power of two and >= 2");
            }
            return depth;
        }

        [[nodiscard]] static size_t
        checked_local_depth(size_t depth) {
            if (depth == 0) {
                throw std::invalid_argument(
                    "inconel::mock_nvme::mock_scheduler: local_depth is 0");
            }
            return depth;
        }

        static void
        validate_qpair(qpair_t* qpair) {
            if (qpair == nullptr || qpair->owner == nullptr) {
                throw std::invalid_argument(
                    "inconel::mock_nvme::mock_scheduler: qpair is null");
            }
        }

        static void
        validate_io_span(uint32_t num_lbas, const char* op) {
            if (num_lbas == 0) {
                throw std::invalid_argument(
                    std::string("inconel::mock_nvme::mock_scheduler::") + op +
                    ": num_lbas is 0");
            }
            if (num_lbas > std::numeric_limits<uint16_t>::max()) {
                throw std::invalid_argument(
                    std::string("inconel::mock_nvme::mock_scheduler::") + op +
                    ": num_lbas exceeds frame_id::span_lbas capacity");
            }
        }

        [[nodiscard]] _mock_io::sender<mock_scheduler>
        make_frame_sender(_mock_io::kind op,
                          memory::segmented_page_frame* frame,
                          uint32_t flags) {
            return _mock_io::sender<mock_scheduler>{
                .scheduler = this,
                .request_kind = op,
                .frame = frame,
                .flags = flags,
            };
        }

        [[nodiscard]] bool
        handle(const _mock_io::req& r) {
            switch (r.op) {
            case _mock_io::kind::read_frame:
                return handle_read_frame(r.frame);
            case _mock_io::kind::write_frame:
                return handle_write_frame(r.frame, r.flags);
            case _mock_io::kind::read_contiguous:
                return device_->read(r.lba, r.mutable_buf, r.num_lbas);
            case _mock_io::kind::write_contiguous:
                return device_->write(
                    r.lba, r.const_buf, r.num_lbas, r.flags);
            case _mock_io::kind::flush:
                return device_->flush();
            case _mock_io::kind::trim:
                return device_->trim(r.lba, r.num_lbas);
            }
            return false;
        }

        [[nodiscard]] bool
        handle_read_frame(memory::segmented_page_frame* frame) {
            if (!valid_frame(frame)) return false;
            for (uint16_t i = 0; i < frame->pages.size(); ++i) {
                auto* page = frame->pages[i];
                if (!device_->read(frame->id.base.lba + i, page->buf, 1)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool
        handle_write_frame(memory::segmented_page_frame* frame,
                           uint32_t flags) {
            if (!valid_frame(frame)) return false;
            for (uint16_t i = 0; i < frame->pages.size(); ++i) {
                auto* page = frame->pages[i];
                if (!device_->write(
                        frame->id.base.lba + i, page->buf, 1, flags)) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool
        valid_frame(const memory::segmented_page_frame* frame) const noexcept {
            if (frame == nullptr || !frame->complete()) return false;
            for (auto* page : frame->pages) {
                if (page == nullptr || page->buf == nullptr ||
                    page->byte_len != pool_.lba_size()) {
                    return false;
                }
            }
            return true;
        }

        qpair_t* qpair_ = nullptr;
        mock_device* device_ = nullptr;
        pump::core::per_core::queue<_mock_io::req*> req_q_;
        pump::core::ring_queue<_mock_io::req*> local_q_;
        memory::lba_dma_page_pool pool_;
        uint16_t device_id_ = 0;
    };

}  // namespace apps::inconel::mock_nvme

namespace pump::core {

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<
                        typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::mock_nvme_io_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename context_t>
        static void
        push_value(context_t& context, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(
                context, scope);
        }
    };

    template <typename context_t, typename scheduler_t>
    struct compute_sender_type<
        context_t,
        apps::inconel::mock_nvme::_mock_io::sender<scheduler_t>> {
        consteval static uint32_t
        count_value() {
            return 1;
        }

        consteval static auto
        get_value_type_identity() {
            return std::type_identity<bool>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_MOCK_NVME_SCHEDULER_HH
