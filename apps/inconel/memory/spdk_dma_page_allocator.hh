#ifndef APPS_INCONEL_MEMORY_SPDK_DMA_PAGE_ALLOCATOR_HH
#define APPS_INCONEL_MEMORY_SPDK_DMA_PAGE_ALLOCATOR_HH

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "spdk/env.h"

#include "./dma_page_pool.hh"

namespace apps::inconel::memory {

    inline char*
    spdk_dma_page_alloc(void* /*ctx*/, uint32_t bytes, uint32_t align,
                        int numa_id) {
        return static_cast<char*>(
            spdk_dma_zmalloc_socket(bytes, align, nullptr, numa_id));
    }

    inline void
    spdk_dma_page_free(void* /*ctx*/, char* ptr, uint32_t /*bytes*/,
                       int /*numa_id*/) {
        spdk_dma_free(ptr);
    }

    inline dma_page_allocator
    make_spdk_dma_page_allocator() noexcept {
        return dma_page_allocator{
            .ctx = nullptr,
            .alloc = spdk_dma_page_alloc,
            .free = spdk_dma_page_free,
        };
    }

    class spdk_dma_page_mempool {
    public:
        spdk_dma_page_mempool(std::string name,
                              uint64_t page_count,
                              uint32_t page_size,
                              size_t cache_size =
                                  SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
                              int numa_id = SPDK_ENV_NUMA_ID_ANY)
            : name_(std::move(name))
            , page_size_(page_size) {
            if (name_.empty()) {
                throw std::invalid_argument(
                    "spdk_dma_page_mempool: name is empty");
            }
            if (page_count == 0) {
                throw std::invalid_argument(
                    "spdk_dma_page_mempool: page_count is 0");
            }
            if (page_size_ == 0) {
                throw std::invalid_argument(
                    "spdk_dma_page_mempool: page_size is 0");
            }

            pool_ = spdk_mempool_create(
                name_.c_str(), page_count, page_size_, cache_size, numa_id);
            if (pool_ == nullptr) {
                throw std::runtime_error(
                    "spdk_dma_page_mempool: spdk_mempool_create failed");
            }
        }

        ~spdk_dma_page_mempool() {
            if (pool_ != nullptr) {
                spdk_mempool_free(pool_);
                pool_ = nullptr;
            }
        }

        spdk_dma_page_mempool(const spdk_dma_page_mempool&) = delete;
        spdk_dma_page_mempool& operator=(const spdk_dma_page_mempool&) = delete;
        spdk_dma_page_mempool(spdk_dma_page_mempool&&) = delete;
        spdk_dma_page_mempool& operator=(spdk_dma_page_mempool&&) = delete;

        [[nodiscard]] dma_page_allocator
        allocator() noexcept {
            return dma_page_allocator{
                .ctx = this,
                .alloc = mempool_alloc,
                .free = mempool_free,
            };
        }

    private:
        static char*
        mempool_alloc(void* ctx, uint32_t bytes, uint32_t /*align*/,
                      int /*numa_id*/) {
            auto* self = static_cast<spdk_dma_page_mempool*>(ctx);
            if (self == nullptr || self->pool_ == nullptr ||
                bytes != self->page_size_) {
                return nullptr;
            }
            return static_cast<char*>(spdk_mempool_get(self->pool_));
        }

        static void
        mempool_free(void* ctx, char* ptr, uint32_t /*bytes*/,
                     int /*numa_id*/) {
            if (ptr == nullptr) return;
            auto* self = static_cast<spdk_dma_page_mempool*>(ctx);
            if (self == nullptr || self->pool_ == nullptr) {
                spdk_dma_free(ptr);
                return;
            }
            spdk_mempool_put(self->pool_, ptr);
        }

        std::string name_;
        uint32_t page_size_ = 0;
        spdk_mempool* pool_ = nullptr;
    };

}  // namespace apps::inconel::memory

#endif  // APPS_INCONEL_MEMORY_SPDK_DMA_PAGE_ALLOCATOR_HH
