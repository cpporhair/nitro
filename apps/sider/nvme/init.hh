#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "spdk/env.h"
#include "spdk/nvme.h"
#include "rte_mempool.h"

#include "env/scheduler/nvme/dma.hh"
#include "env/scheduler/nvme/ssd.hh"
#include "env/scheduler/nvme/scheduler.hh"

#include "nvme/page.hh"
#include "store/types.hh"

namespace sider::nvme {

    // ── Types ──

    using nvme_ssd_t = pump::scheduler::nvme::ssd<sider_page>;
    using nvme_qpair_t = pump::scheduler::nvme::qpair<sider_page>;
    using nvme_scheduler_t = pump::scheduler::nvme::scheduler<sider_page>;

    struct nvme_device {
        nvme_ssd_t* ssd = nullptr;
        uint64_t disk_pages = 0;

        struct per_core_nvme {
            nvme_qpair_t* qp = nullptr;
            nvme_scheduler_t* scheduler = nullptr;
        };
        std::vector<per_core_nvme> per_core;  // one per store core
    };

    // ── Global shared state ──

    inline spdk_mempool* page_pool = nullptr;

    // ── DMA pool functions ──
    // All allocated memory is DMA-safe (hugepage-backed).
    // Never falls back to aligned_alloc — returns nullptr on failure.

    inline char* dma_alloc_page() {
        auto* p = spdk_mempool_get(page_pool);
        if (p) [[likely]] return static_cast<char*>(p);
        // Pool exhausted — try hugepage heap as fallback (still DMA-safe).
        p = spdk_dma_zmalloc(4096, 4096, nullptr);
        if (p) return static_cast<char*>(p);
        return nullptr;  // caller handles: discard key
    }

    inline void dma_free_page(char* ptr) {
        if (rte_mempool_from_obj(ptr) != nullptr)
            spdk_mempool_put(page_pool, ptr);
        else
            spdk_dma_free(ptr);
    }

    inline char* dma_alloc_large(uint32_t size) {
        auto* p = spdk_dma_zmalloc(size, 4096, nullptr);
        if (p) return static_cast<char*>(p);
        return nullptr;  // caller handles: discard key
    }

    inline void dma_free_large(char* ptr) {
        spdk_dma_free(ptr);
    }

    // ── SPDK probe/attach callbacks ──

    struct probe_ctx {
        const char* target_addr;
        nvme_ssd_t* dev;
        uint32_t num_cores;
        bool found;
    };

    inline bool probe_cb(void* cb_ctx, const spdk_nvme_transport_id* trid,
                         spdk_nvme_ctrlr_opts* opts) {
        if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE)
            return false;
        auto* ctx = static_cast<probe_ctx*>(cb_ctx);
        if (strcmp(ctx->target_addr, trid->traddr) == 0) {
            opts->num_io_queues = ctx->num_cores;
            opts->io_queue_size = UINT16_MAX;
            return true;
        }
        return false;
    }

    inline void attach_cb(void* cb_ctx, const spdk_nvme_transport_id* trid,
                          spdk_nvme_ctrlr* ctrlr,
                          const spdk_nvme_ctrlr_opts* opts) {
        auto* ctx = static_cast<probe_ctx*>(cb_ctx);
        if (strcmp(ctx->target_addr, trid->traddr) != 0)
            return;

        if (spdk_nvme_ctrlr_reset(ctrlr) < 0)
            throw std::runtime_error("spdk_nvme_ctrlr_reset failed");

        auto* dev = ctx->dev;
        dev->sn = trid->traddr;
        dev->ctrlr = ctrlr;
        dev->ns = spdk_nvme_ctrlr_get_ns(ctrlr,
                      spdk_nvme_ctrlr_get_first_active_ns(ctrlr));
        dev->sector_size = spdk_nvme_ns_get_sector_size(dev->ns);
        dev->max_sector = spdk_nvme_ns_get_num_sectors(dev->ns);
        dev->cfg_qpair_count = 1;
        dev->cfg_qpair_depth = 128;
        ctx->found = true;
    }

    // ── Init SPDK environment + DMA pool (call once) ──
    // pool_pages: auto-calculated from memory_limit, or explicit override.

    inline void init_env(const char* core_mask, uint64_t pool_pages) {
        spdk_env_opts opts;
        spdk_env_opts_init(&opts);
        opts.name = "sider";
        opts.core_mask = core_mask;

        if (spdk_env_init(&opts) < 0)
            throw std::runtime_error("spdk_env_init failed");

        page_pool = spdk_mempool_create("sider_pages",
            pool_pages, 4096,
            SPDK_MEMPOOL_DEFAULT_CACHE_SIZE,
            SPDK_ENV_NUMA_ID_ANY);
        if (!page_pool)
            throw std::runtime_error("spdk_mempool_create failed — not enough hugepages?");

        // Wire up alloc/free — all paths produce DMA-safe memory, never aligned_alloc.
        store::_page_alloc::alloc_fn = dma_alloc_page;
        store::_page_alloc::free_fn = dma_free_page;
        store::_large_alloc::alloc_fn = dma_alloc_large;
        store::_large_alloc::free_fn = dma_free_large;

        printf("DMA pool: %lu pages (%luMB)\n",
               pool_pages, pool_pages * 4096 / (1024*1024));
    }

    // ── Init one NVMe device (creates num_cores qpairs + schedulers) ──

    inline nvme_device init_device(const char* pci_addr, uint32_t num_cores) {
        auto* ssd = new nvme_ssd_t{};

        probe_ctx pctx{pci_addr, ssd, num_cores, false};

        spdk_nvme_transport_id trid{};
        spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);

        if (spdk_nvme_probe(&trid, &pctx, probe_cb, attach_cb, nullptr) < 0)
            throw std::runtime_error("spdk_nvme_probe failed");

        if (!pctx.found) {
            fprintf(stderr, "NVMe device %s not found\n", pci_addr);
            throw std::runtime_error("NVMe device not found");
        }

        uint64_t disk_bytes = static_cast<uint64_t>(ssd->max_sector) * ssd->sector_size;
        uint64_t disk_pages = disk_bytes / 4096;
        printf("NVMe %s: %lu sectors × %u bytes = %luMB (%lu pages)\n",
               pci_addr, ssd->max_sector, ssd->sector_size,
               disk_bytes / (1024*1024), disk_pages);

        // Create one qpair + scheduler per core.
        spdk_nvme_io_qpair_opts qp_opts{};
        spdk_nvme_ctrlr_get_default_io_qpair_opts(
            ssd->ctrlr, &qp_opts, sizeof(qp_opts));
        qp_opts.delay_cmd_submit = false;
        qp_opts.create_only = false;

        nvme_device dev;
        dev.ssd = ssd;
        dev.disk_pages = disk_pages;
        dev.per_core.resize(num_cores);

        for (uint32_t i = 0; i < num_cores; i++) {
            auto* raw_qp = spdk_nvme_ctrlr_alloc_io_qpair(
                ssd->ctrlr, &qp_opts, sizeof(qp_opts));
            if (!raw_qp)
                throw std::runtime_error("spdk_nvme_ctrlr_alloc_io_qpair failed");

            auto* qp = new nvme_qpair_t{raw_qp, i, 128, 0, ssd};
            auto* scheduler = new nvme_scheduler_t(qp, 65536, 16384);
            dev.per_core[i] = {qp, scheduler};
        }

        // TRIM — tells SSD all blocks are free, reduces write amplification.
        // Use first core's qpair for the TRIM command.
        {
            struct trim_ctx { bool done = false; int status = 0; };
            trim_ctx tc;

            auto* trim_qp = dev.per_core[0].qp->impl;
            uint64_t total_sectors = ssd->max_sector;
            spdk_nvme_dsm_range range{};
            range.starting_lba = 0;
            range.length = static_cast<uint32_t>(
                std::min(total_sectors, static_cast<uint64_t>(UINT32_MAX)));

            spdk_nvme_ns_cmd_dataset_management(
                ssd->ns, trim_qp,
                SPDK_NVME_DSM_ATTR_DEALLOCATE,
                &range, 1,
                [](void* arg, const spdk_nvme_cpl* cpl) {
                    auto* c = static_cast<trim_ctx*>(arg);
                    c->status = (cpl && !spdk_nvme_cpl_is_error(cpl)) ? 0 : 1;
                    c->done = true;
                },
                &tc);

            while (!tc.done)
                spdk_nvme_qpair_process_completions(trim_qp, 0);

            if (tc.status == 0)
                printf("NVMe %s TRIM: deallocated %lu sectors\n",
                       pci_addr, static_cast<unsigned long>(range.length));
            else
                printf("NVMe %s TRIM: failed (non-fatal)\n", pci_addr);
        }

        return dev;
    }

} // namespace sider::nvme
