#ifndef APPS_INCONEL_MEMORY_DMA_PAGE_POOL_HH
#define APPS_INCONEL_MEMORY_DMA_PAGE_POOL_HH

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/container/inlined_vector.h"

#include "./frame.hh"

namespace apps::inconel::memory {

    struct dma_page_allocator {
        using alloc_fn = char* (*)(void* ctx, uint32_t bytes, uint32_t align,
                                  int numa_id);
        using free_fn = void (*)(void* ctx, char* ptr, uint32_t bytes,
                                 int numa_id);

        void*    ctx = nullptr;
        alloc_fn alloc = nullptr;
        free_fn  free = nullptr;
    };

    namespace detail {
        inline char*
        heap_dma_alloc(void*, uint32_t bytes, uint32_t, int) {
            return new (std::nothrow) char[bytes];
        }

        inline void
        heap_dma_free(void*, char* ptr, uint32_t, int) {
            delete[] ptr;
        }
    }  // namespace detail

    inline dma_page_allocator
    make_heap_dma_page_allocator() noexcept {
        return dma_page_allocator{
            .ctx   = nullptr,
            .alloc = &detail::heap_dma_alloc,
            .free  = &detail::heap_dma_free,
        };
    }

    // Owner-local pool of LBA-sized DMA pages. The pool manages descriptor
    // lifetime and returns raw lba_dma_page* so frame/cache code can keep the
    // same cache-owning + pin-token model as page_frame.
    class lba_dma_page_pool {
    public:
        lba_dma_page_pool(uint32_t lba_size,
                          uint32_t alignment,
                          int numa_id,
                          dma_page_allocator allocator)
            : lba_size_(lba_size)
            , alignment_(alignment)
            , numa_id_(numa_id)
            , allocator_(allocator) {
            if (lba_size_ == 0) {
                throw std::invalid_argument(
                    "lba_dma_page_pool: lba_size is 0");
            }
            if (alignment_ == 0) {
                throw std::invalid_argument(
                    "lba_dma_page_pool: alignment is 0");
            }
            if (allocator_.alloc == nullptr || allocator_.free == nullptr) {
                throw std::invalid_argument(
                    "lba_dma_page_pool: allocator callbacks are null");
            }
        }

        ~lba_dma_page_pool() {
            for (auto& page : pages_) {
                if (page && page->buf) {
                    allocator_.free(allocator_.ctx, page->buf, page->byte_len,
                                    numa_id_);
                    page->buf = nullptr;
                }
            }
        }

        lba_dma_page_pool(const lba_dma_page_pool&) = delete;
        lba_dma_page_pool& operator=(const lba_dma_page_pool&) = delete;
        lba_dma_page_pool(lba_dma_page_pool&&) = delete;
        lba_dma_page_pool& operator=(lba_dma_page_pool&&) = delete;

        [[nodiscard]] uint32_t lba_size() const noexcept { return lba_size_; }
        [[nodiscard]] uint32_t alignment() const noexcept { return alignment_; }
        [[nodiscard]] int numa_id() const noexcept { return numa_id_; }
        [[nodiscard]] std::size_t free_page_count() const noexcept {
            return free_pages_.size();
        }

        [[nodiscard]] lba_dma_page*
        get_page(bool zero_fill = false) {
            lba_dma_page* page = nullptr;
            if (!free_pages_.empty()) {
                page = free_pages_.back();
                free_pages_.pop_back();
            } else {
                char* buf = allocator_.alloc(allocator_.ctx, lba_size_,
                                             alignment_, numa_id_);
                if (buf == nullptr) return nullptr;
                auto owned = std::make_unique<lba_dma_page>();
                owned->buf = buf;
                owned->byte_len = lba_size_;
                page = owned.get();
                pages_.push_back(std::move(owned));
            }

            if (zero_fill) {
                std::fill_n(page->buf, page->byte_len, char{0});
            }
            return page;
        }

        void
        put_page(lba_dma_page* page) {
            if (page == nullptr) return;
            if (page->byte_len != lba_size_) {
                throw std::invalid_argument(
                    "lba_dma_page_pool::put_page: page size mismatch");
            }
            free_pages_.push_back(page);
        }

        void
        put_pages(std::span<lba_dma_page* const> pages) {
            for (auto* page : pages) put_page(page);
        }

        [[nodiscard]] std::optional<segmented_page_frame>
        get_frame(frame_id id,
                  frame_state st,
                  bool zero_fill = false) {
            if (id.span_lbas == 0) {
                throw std::invalid_argument(
                    "lba_dma_page_pool::get_frame: span_lbas is 0");
            }

            segmented_page_frame frame{
                .id = id,
                .st = st,
            };
            frame.pages.reserve(id.span_lbas);

            for (uint16_t i = 0; i < id.span_lbas; ++i) {
                auto* page = get_page(zero_fill);
                if (page == nullptr) {
                    put_pages(frame.page_span());
                    return std::nullopt;
                }
                frame.pages.push_back(page);
            }
            return frame;
        }

        void
        put_frame(segmented_page_frame&& frame) {
            put_pages(frame.page_span());
            frame.pages.clear();
            frame.id.span_lbas = 0;
        }

        template <typename FrameT = segmented_page_frame>
        [[nodiscard]] std::optional<FrameT>
        get_typed_frame(frame_id id,
                        frame_state st,
                        bool zero_fill = false) {
            static_assert(std::is_base_of_v<segmented_page_frame, FrameT>);
            auto frame = get_frame(id, st, zero_fill);
            if (!frame) return std::nullopt;
            return FrameT(std::move(*frame));
        }

    private:
        uint32_t lba_size_;
        uint32_t alignment_;
        int numa_id_;
        dma_page_allocator allocator_;
        std::vector<std::unique_ptr<lba_dma_page>> pages_;
        absl::InlinedVector<lba_dma_page*, 64> free_pages_;
    };

    template <typename FrameT = segmented_page_frame>
    class pooled_frame_ptr {
    public:
        pooled_frame_ptr() = default;

        pooled_frame_ptr(lba_dma_page_pool* pool, FrameT* frame) noexcept
            : pool_(pool), frame_(frame) {}

        ~pooled_frame_ptr() {
            reset();
        }

        pooled_frame_ptr(pooled_frame_ptr&& rhs) noexcept
            : pool_(rhs.pool_), frame_(rhs.frame_) {
            rhs.pool_ = nullptr;
            rhs.frame_ = nullptr;
        }

        pooled_frame_ptr&
        operator=(pooled_frame_ptr&& rhs) noexcept {
            if (this != &rhs) {
                reset();
                pool_ = rhs.pool_;
                frame_ = rhs.frame_;
                rhs.pool_ = nullptr;
                rhs.frame_ = nullptr;
            }
            return *this;
        }

        pooled_frame_ptr(const pooled_frame_ptr&) = delete;
        pooled_frame_ptr& operator=(const pooled_frame_ptr&) = delete;

        [[nodiscard]] FrameT* get() const noexcept { return frame_; }
        [[nodiscard]] FrameT& operator*() const noexcept { return *frame_; }
        [[nodiscard]] FrameT* operator->() const noexcept { return frame_; }
        explicit operator bool() const noexcept { return frame_ != nullptr; }

        [[nodiscard]] FrameT*
        release() noexcept {
            FrameT* out = frame_;
            frame_ = nullptr;
            pool_ = nullptr;
            return out;
        }

        void
        reset() noexcept {
            if (frame_ != nullptr) {
                if (pool_ != nullptr) {
                    try {
                        pool_->put_frame(std::move(*frame_));
                    } catch (...) {
                    }
                }
                delete frame_;
            }
            pool_ = nullptr;
            frame_ = nullptr;
        }

    private:
        lba_dma_page_pool* pool_ = nullptr;
        FrameT* frame_ = nullptr;
    };

}  // namespace apps::inconel::memory

#endif  // APPS_INCONEL_MEMORY_DMA_PAGE_POOL_HH
