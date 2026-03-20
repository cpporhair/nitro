#ifndef AISAQ_NVME_SECTOR_PAGE_HH
#define AISAQ_NVME_SECTOR_PAGE_HH

#include <cstdint>
#include <span>
#include "spdk/env.h"
#include "env/scheduler/nvme/nvme_page.hh"
#include "env/scheduler/nvme/ssd.hh"
#include "env/scheduler/nvme/dma.hh"

namespace aisaq::nvme {

    // Sector page for NVMe reads — satisfies pump::scheduler::nvme::page_concept
    //
    // Fixed 4K pages. A node may span multiple 4K sectors — use get_pages()
    // to read N pages concurrently and assemble the result.
    //
    //   sector 0..spn-1 = metadata (spn sectors, padded)
    //   sector spn..2*spn-1 = node 0 (spn sectors)
    //   sector 2*spn..3*spn-1 = node 1
    //   ...
    //
    // NVMe LBA calculation (in scheduler.hh):
    //   start_LBA = get_pos() * get_size() / hw_sector_size
    //             = sector_id * 4096 / 512 = sector_id * 8
    //   num_LBAs  = get_size() / hw_sector_size = 4096 / 512 = 8

    struct sector_page {
        uint64_t sector_id;
        void*    payload;
        const pump::scheduler::nvme::ssd<sector_page>* ssd_info;

        constexpr uint64_t get_size() const { return 4096; }
        uint64_t get_pos() const { return sector_id; }
        void* get_payload() const { return payload; }

        const pump::scheduler::nvme::ssd<sector_page>*
        get_ssd_info() const { return ssd_info; }
    };

    using ssd_t = pump::scheduler::nvme::ssd<sector_page>;
    using nvme_scheduler_t = pump::scheduler::nvme::scheduler<sector_page>;

    inline sector_page*
    alloc_sector_page(uint64_t sector_id, const ssd_t* ssd) {
        return new sector_page{
            sector_id,
            pump::scheduler::nvme::malloc_page(4096),
            ssd
        };
    }

    inline void
    free_sector_page(sector_page* page) {
        if (page) {
            pump::scheduler::nvme::free_page(page->payload);
            delete page;
        }
    }

    // DMA pool for node reads.
    // spn=1: shares global 4K pool. spn>1: dedicated SPDK mempool.
    inline spdk_mempool* g_node_pool = nullptr;
    inline uint32_t g_node_spn = 1;

    inline void init_node_pool(uint32_t spn, uint32_t pool_size = 4096) {
        g_node_spn = spn;
        if (spn > 1) {
            uint32_t buf_size = spn * 4096;
            g_node_pool = spdk_mempool_create("node_read", pool_size, buf_size, 0, SPDK_ENV_SOCKET_ID_ANY);
            // Pre-fill with DMA-safe memory
            for (uint32_t i = 0; i < 1024 && i < pool_size; ++i)
                spdk_mempool_put(g_node_pool, spdk_dma_malloc(buf_size, 4096, nullptr));
        }
    }

    inline void* node_dma_alloc() {
        if (g_node_spn == 1)
            return pump::scheduler::nvme::malloc_page(4096);
        auto* p = spdk_mempool_get(g_node_pool);
        if (!p) p = spdk_dma_malloc(g_node_spn * 4096, 4096, nullptr);
        return p;
    }

    inline void node_dma_free(void* p) {
        if (g_node_spn == 1)
            pump::scheduler::nvme::free_page(p);
        else
            spdk_mempool_put(g_node_pool, p);
    }

    // Contiguous DMA buffer for multi-sector node reads.
    // One DMA allocation from pool, N sector_page structs pointing into offsets.
    // After concurrent 4K reads, data is contiguous — zero memcpy.
    struct node_read_buf {
        void* dma_buf = nullptr;
        sector_page pages[4];
        uint32_t spn = 0;

        node_read_buf() = default;
        node_read_buf(node_read_buf&& o) noexcept
            : dma_buf(o.dma_buf), spn(o.spn) {
            std::copy(o.pages, o.pages + 4, pages);
            o.dma_buf = nullptr;
        }
        node_read_buf& operator=(node_read_buf&& o) noexcept {
            if (this != &o) {
                release();
                dma_buf = o.dma_buf;
                spn = o.spn;
                std::copy(o.pages, o.pages + 4, pages);
                o.dma_buf = nullptr;
            }
            return *this;
        }
        node_read_buf(const node_read_buf&) = delete;
        node_read_buf& operator=(const node_read_buf&) = delete;

        void acquire() {
            spn = g_node_spn;
            dma_buf = node_dma_alloc();
        }

        void release() {
            if (dma_buf) {
                node_dma_free(dma_buf);
                dma_buf = nullptr;
            }
        }

        sector_page* ptrs[4];    // pointer array, updated in setup()

        // Per-read: set sector IDs, payload pointers, and ptr array (zero allocation)
        void setup(uint64_t start_sector_id, const ssd_t* ssd) {
            for (uint32_t i = 0; i < spn; ++i) {
                pages[i] = {
                    start_sector_id + i,
                    static_cast<uint8_t*>(dma_buf) + i * 4096,
                    ssd
                };
                ptrs[i] = &pages[i];
            }
        }

        auto page_range() { return std::span<sector_page*>(ptrs, spn); }
    };

}

#endif //AISAQ_NVME_SECTOR_PAGE_HH
