#ifndef APPS_INCONEL_NVME_REAL_SCHEDULER_HH
#define APPS_INCONEL_NVME_REAL_SCHEDULER_HH

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "spdk/nvme.h"

#include "pump/sender/flat.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/then.hh"

#include "env/scheduler/nvme/scheduler.hh"

#include "../format/types.hh"
#include "../memory/dma_page_pool.hh"
#include "../memory/spdk_dma_page_allocator.hh"
#include "./lba_page.hh"

namespace apps::inconel::nvme {

    constexpr uint32_t IO_FLAGS_FUA =
        SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS;

    class real_scheduler {
        struct frame_deleter {
            real_scheduler* owner = nullptr;

            void operator()(memory::segmented_page_frame* frame) const noexcept {
                if (frame == nullptr) return;
                if (owner != nullptr) {
                    try {
                        owner->pool_.put_frame(std::move(*frame));
                    } catch (...) {
                    }
                }
                delete frame;
            }
        };

        using scoped_frame_ptr =
            std::unique_ptr<memory::segmented_page_frame, frame_deleter>;

        static scoped_frame_ptr
        make_scoped_frame_ptr(real_scheduler* owner) noexcept {
            return scoped_frame_ptr(nullptr, frame_deleter{owner});
        }

    public:
        using qpair_t = pump::scheduler::nvme::qpair<lba_page_adapter>;

        real_scheduler(qpair_t* qpair,
                       uint32_t lba_size,
                       memory::dma_page_allocator allocator =
                           memory::make_spdk_dma_page_allocator(),
                       size_t queue_depth = 2048,
                       size_t local_depth = 128,
                       uint32_t alignment = 4096,
                       int numa_id = SPDK_ENV_NUMA_ID_ANY,
                       uint16_t device_id = 0)
            : ssd_(qpair ? qpair->owner : nullptr)
            , scheduler_(std::make_unique<pump_lba_scheduler>(
                  qpair, queue_depth, local_depth))
            , pool_(lba_size, alignment, numa_id, allocator)
            , device_id_(device_id) {
            if (qpair == nullptr) {
                throw std::invalid_argument(
                    "inconel::nvme::real_scheduler: qpair is null");
            }
        }

        real_scheduler(qpair_t* qpair,
                       uint32_t lba_size,
                       uint64_t pool_pages,
                       size_t queue_depth = 2048,
                       size_t local_depth = 128,
                       uint32_t alignment = 4096,
                       int numa_id = SPDK_ENV_NUMA_ID_ANY,
                       uint16_t device_id = 0)
            : ssd_(qpair ? qpair->owner : nullptr)
            , scheduler_(std::make_unique<pump_lba_scheduler>(
                  qpair, queue_depth, local_depth))
            , owned_mempool_(make_mempool(
                  qpair, lba_size, pool_pages, numa_id))
            , pool_(lba_size, alignment, numa_id, owned_mempool_->allocator())
            , device_id_(device_id) {
            if (qpair == nullptr) {
                throw std::invalid_argument(
                    "inconel::nvme::real_scheduler: qpair is null");
            }
        }

        [[nodiscard]] const pump_lba_ssd*
        get_ssd() const noexcept {
            return ssd_;
        }

        [[nodiscard]] uint32_t
        lba_size() const noexcept {
            return pool_.lba_size();
        }

        bool
        advance() {
            return scheduler_->advance();
        }

        template <typename Runtime>
        bool
        advance(Runtime&) {
            return advance();
        }

        auto
        read(uint64_t lba, void* buf, uint32_t num_lbas) {
            validate_io_span(num_lbas, "read");
            struct read_ctx {
                scoped_frame_ptr frame;
            };

            return pump::sender::just()
                >> pump::sender::with_context(
                    read_ctx{make_scoped_frame_ptr(this)})
                ([this, lba, buf, num_lbas]() {
                    return pump::sender::get_context<read_ctx>()
                        >> pump::sender::then([this, lba, num_lbas](read_ctx& c) {
                            auto frame = pool_.get_frame(
                                memory::frame_id{
                                    .base = format::paddr{
                                        .device_id = device_id_, .lba = lba},
                                    .span_lbas = static_cast<uint16_t>(num_lbas),
                                    .dom = memory::frame_id::domain::nvme_scratch,
                                },
                                memory::frame_state::clean_readonly,
                                false);
                            if (!frame) {
                                throw std::runtime_error(
                                    "inconel::nvme::real_scheduler::read: DMA page allocation failed");
                            }
                            c.frame.reset(new memory::segmented_page_frame(
                                std::move(*frame)));
                            return true;
                        })
                        >> pump::sender::get_context<read_ctx>()
                        >> pump::sender::flat_map([this](read_ctx& c, bool) {
                            return get_frame(c.frame.get(), scheduler_.get(), ssd_);
                        })
                        >> pump::sender::get_context<read_ctx>()
                        >> pump::sender::then([this, buf](read_ctx& c, bool ok) {
                            if (ok) {
                                c.frame->copy_to_contiguous(
                                    buf,
                                    static_cast<uint64_t>(c.frame->span_lbas()) *
                                        pool_.lba_size());
                            }
                            c.frame.reset();
                            return ok;
                        });
                });
        }

        auto
        write(uint64_t lba, const void* data, uint32_t num_lbas,
              uint32_t flags = 0) {
            validate_io_span(num_lbas, "write");
            struct write_ctx {
                scoped_frame_ptr frame;
            };

            return pump::sender::just()
                >> pump::sender::with_context(
                    write_ctx{make_scoped_frame_ptr(this)})
                ([this, lba, data, num_lbas, flags]() {
                    return pump::sender::get_context<write_ctx>()
                        >> pump::sender::then([this, lba, data, num_lbas](write_ctx& c) {
                            auto frame = pool_.get_frame(
                                memory::frame_id{
                                    .base = format::paddr{
                                        .device_id = device_id_, .lba = lba},
                                    .span_lbas = static_cast<uint16_t>(num_lbas),
                                    .dom = memory::frame_id::domain::nvme_scratch,
                                },
                                memory::frame_state::dirty_append,
                                true);
                            if (!frame) {
                                throw std::runtime_error(
                                    "inconel::nvme::real_scheduler::write: DMA page allocation failed");
                            }
                            c.frame.reset(new memory::segmented_page_frame(
                                std::move(*frame)));
                            c.frame->copy_from_contiguous(
                                data,
                                static_cast<uint64_t>(num_lbas) *
                                    pool_.lba_size());
                            return true;
                        })
                        >> pump::sender::get_context<write_ctx>()
                        >> pump::sender::flat_map([this, flags](write_ctx& c, bool) {
                            return put_frame(c.frame.get(), scheduler_.get(), ssd_, flags);
                        })
                        >> pump::sender::get_context<write_ctx>()
                        >> pump::sender::then([this](write_ctx& c, bool ok) {
                            c.frame.reset();
                            return ok;
                        });
                });
        }

        auto
        read_frame(memory::segmented_page_frame* frame, uint32_t flags = 0) {
            return get_frame(frame, scheduler_.get(), ssd_, flags);
        }

        auto
        write_frame(memory::segmented_page_frame* frame, uint32_t flags = 0) {
            return put_frame(frame, scheduler_.get(), ssd_, flags);
        }

        auto
        flush() {
            return nvme::flush(scheduler_.get());
        }

        auto
        trim(uint64_t lba, uint32_t num_lbas) {
            return nvme::trim(scheduler_.get(), lba, num_lbas);
        }

        auto
        trim_ns_lba(uint64_t lba, uint32_t num_lbas) {
            return trim(lba, num_lbas);
        }

    private:
        static std::unique_ptr<memory::spdk_dma_page_mempool>
        make_mempool(qpair_t* qpair,
                     uint32_t lba_size,
                     uint64_t pool_pages,
                     int numa_id) {
            if (qpair == nullptr) {
                throw std::invalid_argument(
                    "inconel::nvme::real_scheduler: qpair is null");
            }
            std::ostringstream name;
            name << "inconel_lba_pages_c" << qpair->core;
            return std::make_unique<memory::spdk_dma_page_mempool>(
                name.str(), pool_pages, lba_size,
                SPDK_MEMPOOL_DEFAULT_CACHE_SIZE, numa_id);
        }

        static void
        validate_io_span(uint32_t num_lbas, const char* op) {
            if (num_lbas == 0) {
                throw std::invalid_argument(
                    std::string("inconel::nvme::real_scheduler::") + op +
                    ": num_lbas is 0");
            }
            if (num_lbas > std::numeric_limits<uint16_t>::max()) {
                throw std::invalid_argument(
                    std::string("inconel::nvme::real_scheduler::") + op +
                    ": num_lbas exceeds frame_id::span_lbas capacity");
            }
        }

        const pump_lba_ssd* ssd_ = nullptr;
        std::unique_ptr<pump_lba_scheduler> scheduler_;
        std::unique_ptr<memory::spdk_dma_page_mempool> owned_mempool_;
        memory::lba_dma_page_pool pool_;
        uint16_t device_id_ = 0;
    };

}  // namespace apps::inconel::nvme

#endif  // APPS_INCONEL_NVME_REAL_SCHEDULER_HH
