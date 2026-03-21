#pragma once

#include <cstdint>
#include "env/scheduler/nvme/nvme_page.hh"

namespace sider::nvme {

    // Satisfies pump::scheduler::nvme::page_concept.
    // payload points directly to DMA-safe memory — zero-copy.
    // size supports variable-length pages (4KB for slab, N*4KB for large values).
    struct sider_page {
        uint64_t lba;       // page position on NVMe (in pages, not bytes)
        char*    payload;   // DMA-safe memory
        const pump::scheduler::nvme::ssd<sider_page>* ssd_info;
        uint64_t size;      // actual size in bytes (multiple of 4096)

        uint64_t get_size() const { return size; }
        uint64_t get_pos() const { return lba; }
        void* get_payload() const { return payload; }
        const pump::scheduler::nvme::ssd<sider_page>* get_ssd_info() const { return ssd_info; }
    };

    // thread_local object pool for sider_page.
    // Avoids heap alloc/free on every NVMe read/write.
    struct sider_page_pool {
        static constexpr uint32_t CAP = 64;
        sider_page storage[CAP];
        sider_page* free_list[CAP];
        uint32_t count = CAP;

        sider_page_pool() {
            for (uint32_t i = 0; i < CAP; i++)
                free_list[i] = &storage[i];
        }

        sider_page* get() {
            return count > 0 ? free_list[--count] : new sider_page{};
        }

        void put(sider_page* p) {
            if (p >= storage && p < storage + CAP)
                free_list[count++] = p;
            else
                delete p;
        }
    };

    inline thread_local sider_page_pool page_obj_pool;

} // namespace sider::nvme
