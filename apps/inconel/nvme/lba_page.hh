#ifndef APPS_INCONEL_NVME_LBA_PAGE_HH
#define APPS_INCONEL_NVME_LBA_PAGE_HH

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

#include "absl/container/inlined_vector.h"

#include "pump/sender/flat.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/then.hh"
#include "env/scheduler/nvme/sender.hh"
#include "env/scheduler/nvme/ssd.hh"

#include "../format/types.hh"
#include "../memory/frame.hh"

namespace apps::inconel::nvme {

    // PUMP page_concept adapter for one logical Inconel LBA. A multi-LBA
    // frame creates one lba_page_adapter per LBA and submits the vector via
    // PUMP get_pages/put_pages. That is intentionally command-per-LBA in v1:
    // no contiguous DMA span requirement and no SGL contract.
    struct lba_page_adapter {
        using ssd_type = pump::scheduler::nvme::ssd<lba_page_adapter>;

        const ssd_type* ssd = nullptr;
        format::paddr addr{};
        memory::lba_dma_page* page = nullptr;

        [[nodiscard]] uint64_t
        get_size() const noexcept {
            assert(page != nullptr);
            return page->byte_len;
        }

        [[nodiscard]] uint64_t
        get_pos() const noexcept {
            return addr.lba;
        }

        [[nodiscard]] void*
        get_payload() const noexcept {
            assert(page != nullptr);
            return page->buf;
        }

        [[nodiscard]] const ssd_type*
        get_ssd_info() const noexcept {
            return ssd;
        }
    };

    static_assert(pump::scheduler::nvme::page_concept<lba_page_adapter>);

    using pump_lba_ssd = pump::scheduler::nvme::ssd<lba_page_adapter>;
    using pump_lba_scheduler =
        pump::scheduler::nvme::scheduler<lba_page_adapter>;

    struct lba_page_batch {
        absl::InlinedVector<lba_page_adapter, 4> pages;
        std::vector<lba_page_adapter*> page_ptrs;

        explicit
        lba_page_batch(memory::segmented_page_frame* frame,
                       const pump_lba_ssd* ssd) {
            assert(frame != nullptr);
            assert(frame->complete());
            pages.reserve(frame->pages.size());
            page_ptrs.reserve(frame->pages.size());
            for (uint16_t i = 0; i < frame->pages.size(); ++i) {
                pages.push_back(lba_page_adapter{
                    .ssd = ssd,
                    .addr = format::paddr{
                        .device_id = frame->id.base.device_id,
                        .lba = frame->id.base.lba + i,
                    },
                    .page = frame->pages[i],
                });
            }
            for (auto& page : pages) {
                page_ptrs.push_back(&page);
            }
        }

        lba_page_batch(lba_page_batch&& rhs) noexcept
            : pages(std::move(rhs.pages)) {
            page_ptrs.reserve(pages.size());
            for (auto& page : pages) {
                page_ptrs.push_back(&page);
            }
        }

        lba_page_batch& operator=(lba_page_batch&& rhs) noexcept {
            if (this != &rhs) {
                pages = std::move(rhs.pages);
                page_ptrs.clear();
                page_ptrs.reserve(pages.size());
                for (auto& page : pages) {
                    page_ptrs.push_back(&page);
                }
            }
            return *this;
        }

        lba_page_batch(const lba_page_batch&) = delete;
        lba_page_batch& operator=(const lba_page_batch&) = delete;
    };

    inline lba_page_batch
    make_lba_page_batch(memory::segmented_page_frame* frame,
                        const pump_lba_ssd* ssd) {
        return lba_page_batch(frame, ssd);
    }

    template <typename scheduler_t>
    inline auto
    get_frame(memory::segmented_page_frame* frame,
              scheduler_t* sched,
              const pump_lba_ssd* ssd,
              uint32_t flags = 0) {
        using pump::sender::flat_map;
        using pump::sender::get_context;
        using pump::sender::just;
        using pump::sender::pop_context;

        return just()
            >> pump::sender::push_context_with_id<__COUNTER__>(
                make_lba_page_batch(frame, ssd))
            >> get_context<lba_page_batch>()
            >> flat_map([sched, flags](lba_page_batch& batch) {
                return just()
                    >> pump::scheduler::nvme::get_pages(
                        batch.page_ptrs, sched, flags);
            })
            >> pop_context();
    }

    template <typename scheduler_t>
    inline auto
    put_frame(memory::segmented_page_frame* frame,
              scheduler_t* sched,
              const pump_lba_ssd* ssd,
              uint32_t flags = 0) {
        using pump::sender::flat_map;
        using pump::sender::get_context;
        using pump::sender::just;
        using pump::sender::pop_context;

        return just()
            >> pump::sender::push_context_with_id<__COUNTER__>(
                make_lba_page_batch(frame, ssd))
            >> get_context<lba_page_batch>()
            >> flat_map([sched, flags](lba_page_batch& batch) {
                return just()
                    >> pump::scheduler::nvme::put_pages(
                        batch.page_ptrs, sched, flags);
            })
            >> pop_context();
    }

    template <typename scheduler_t>
    inline auto
    flush(scheduler_t* sched) {
        return sched->flush()
            >> pump::sender::then([](auto&& res) {
                return res.is_success();
            });
    }

    template <typename scheduler_t>
    inline auto
    trim(scheduler_t* sched, uint64_t ns_lba, uint32_t ns_lba_count) {
        return sched->trim_ns_lba(ns_lba, ns_lba_count)
            >> pump::sender::then([](auto&& res) {
                return res.is_success();
            });
    }

}  // namespace apps::inconel::nvme

#endif  // APPS_INCONEL_NVME_LBA_PAGE_HH
