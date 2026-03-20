#ifndef AISAQ_NVME_INIT_HH
#define AISAQ_NVME_INIT_HH

#include <cstring>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

#include "spdk/env.h"
#include "spdk/nvme.h"

#include "env/scheduler/nvme/scheduler.hh"
#include "env/scheduler/nvme/dma.hh"
#include "./sector_page.hh"

namespace aisaq::nvme {

    static constexpr uint32_t MAX_DEVICES = 4;

    // Multi-device state
    inline ssd_t*    g_ssds[MAX_DEVICES] = {};
    inline uint32_t  g_num_devices = 0;

    // Probe/attach context: which device we're currently probing
    inline std::string g_probe_pcie;
    inline uint32_t    g_probe_num_qpairs = 1;
    inline ssd_t*      g_probe_result = nullptr;

    // ── Probe/Attach callbacks ──

    inline bool
    probe_cb(void* cb_ctx, const spdk_nvme_transport_id* trid, spdk_nvme_ctrlr_opts* opts) {
        if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE)
            return false;
        if (strcmp(trid->traddr, g_probe_pcie.c_str()) != 0)
            return false;
        opts->num_io_queues = g_probe_num_qpairs;
        opts->io_queue_size = UINT16_MAX;
        return true;
    }

    inline void
    attach_cb(void* cb_ctx, const spdk_nvme_transport_id* trid,
              spdk_nvme_ctrlr* ctrlr, const spdk_nvme_ctrlr_opts* opts) {
        if (spdk_nvme_ctrlr_reset(ctrlr) < 0)
            throw std::runtime_error("spdk_nvme_ctrlr_reset failed");

        auto* ssd = new ssd_t();
        auto* cdata = spdk_nvme_ctrlr_get_data(ctrlr);
        ssd->sn = reinterpret_cast<const char*>(cdata->sn);
        ssd->ctrlr = ctrlr;
        ssd->ns = spdk_nvme_ctrlr_get_ns(ctrlr, spdk_nvme_ctrlr_get_first_active_ns(ctrlr));
        ssd->sector_size = spdk_nvme_ns_get_sector_size(ssd->ns);
        ssd->max_sector = spdk_nvme_ns_get_num_sectors(ssd->ns);
        ssd->cfg_qpair_count = 1;
        ssd->cfg_qpair_depth = 1024;

        g_probe_result = ssd;
    }

    // ── Initialize SPDK environment + probe multiple NVMe devices ──

    struct device_init_config {
        std::string pcie;
        uint32_t qpair_depth = 1024;
    };

    inline void
    init_spdk_env(const std::vector<device_init_config>& devices,
                  uint64_t core_mask, uint32_t num_qpairs = 1) {
        if (devices.empty())
            throw std::runtime_error("no NVMe devices configured");
        if (devices.size() > MAX_DEVICES)
            throw std::runtime_error("too many NVMe devices (max " + std::to_string(MAX_DEVICES) + ")");

        std::stringstream ss;
        ss << "0x" << std::hex << core_mask;
        std::string mask_str = ss.str();

        spdk_env_opts opts;
        spdk_env_opts_init(&opts);
        opts.name = "aisaq";
        opts.core_mask = mask_str.c_str();

        if (spdk_env_init(&opts) < 0)
            throw std::runtime_error("spdk_env_init failed");

        // DMA buffer pool (4096-byte sectors, 4096 pool entries)
        pump::scheduler::nvme::global_page_dma.init(4096, 4096);

        // Probe each device
        spdk_nvme_transport_id trid{};
        spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);

        g_num_devices = 0;
        for (auto& dev : devices) {
            g_probe_pcie = dev.pcie;
            g_probe_num_qpairs = num_qpairs;
            g_probe_result = nullptr;

            if (spdk_nvme_probe(&trid, nullptr, probe_cb, attach_cb, nullptr) < 0)
                throw std::runtime_error("spdk_nvme_probe failed for " + dev.pcie);

            if (!g_probe_result || !g_probe_result->ctrlr)
                throw std::runtime_error("NVMe device not found: " + dev.pcie);

            g_ssds[g_num_devices] = g_probe_result;
            printf("NVMe[%u] initialized: %s (sector_size=%u, sectors=%lu)\n",
                   g_num_devices, dev.pcie.c_str(),
                   g_probe_result->sector_size, g_probe_result->max_sector);
            g_num_devices++;
        }
    }

    // Backward-compatible single-device init
    inline void
    init_spdk_env(const std::string& pcie_addr, uint64_t core_mask, uint32_t num_qpairs = 1) {
        std::vector<device_init_config> devs;
        devs.push_back({pcie_addr});
        init_spdk_env(devs, core_mask, num_qpairs);
    }

    // ── Create per-core qpair + scheduler for a specific device ──

    inline nvme_scheduler_t*
    create_nvme_scheduler(uint32_t device_idx, uint32_t core_id,
                          uint32_t qpair_depth, size_t queue_depth = 8192) {
        auto* ssd = g_ssds[device_idx];
        if (!ssd)
            throw std::runtime_error("device " + std::to_string(device_idx) + " not initialized");

        spdk_nvme_io_qpair_opts qopts{};
        spdk_nvme_ctrlr_get_default_io_qpair_opts(ssd->ctrlr, &qopts, sizeof(qopts));
        qopts.delay_cmd_submit = false;
        qopts.create_only = false;

        auto* raw_qp = spdk_nvme_ctrlr_alloc_io_qpair(ssd->ctrlr, &qopts, sizeof(qopts));
        if (!raw_qp)
            throw std::runtime_error("failed to allocate NVMe qpair for device " +
                std::to_string(device_idx) + " core " + std::to_string(core_id));

        auto* qp = new pump::scheduler::nvme::qpair<sector_page>{
            raw_qp, core_id, qpair_depth, 0, ssd
        };

        return new nvme_scheduler_t(qp, queue_depth);
    }

    // Backward-compatible: create scheduler for device 0
    inline nvme_scheduler_t*
    create_nvme_scheduler(uint32_t core_id, uint32_t qpair_depth, size_t queue_depth = 8192) {
        return create_nvme_scheduler(0, core_id, qpair_depth, queue_depth);
    }

}

#endif //AISAQ_NVME_INIT_HH
