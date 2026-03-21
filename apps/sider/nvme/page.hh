#pragma once

#include <cstdint>
#include "env/scheduler/nvme/nvme_page.hh"

namespace sider::nvme {

    // Satisfies pump::scheduler::nvme::page_concept.
    // payload points directly to the slab memory page — zero-copy DMA.
    struct sider_page {
        uint64_t lba;       // page position on NVMe (in pages, not bytes)
        char*    payload;   // DMA-safe memory (slab page)
        const pump::scheduler::nvme::ssd<sider_page>* ssd_info;

        uint64_t get_size() const { return 4096; }
        uint64_t get_pos() const { return lba; }
        void* get_payload() const { return payload; }
        const pump::scheduler::nvme::ssd<sider_page>* get_ssd_info() const { return ssd_info; }
    };

} // namespace sider::nvme
