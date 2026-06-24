#ifndef APPS_INCONEL_NVME_REAL_DEVICE_HH
#define APPS_INCONEL_NVME_REAL_DEVICE_HH

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "spdk/env.h"
#include "spdk/nvme.h"

#include "./lba_page.hh"

namespace apps::inconel::nvme {

    struct real_device_options {
        const char*               pci_addr = nullptr;
        std::span<const uint32_t> cores;
        const char*               spdk_core_mask = nullptr;
        const char*               spdk_name = "inconel";
        bool                      init_spdk_env = true;
        uint32_t                  qpair_depth = 128;
        uint16_t                  device_id = 0;
    };

    namespace _real_device {
        inline bool g_spdk_env_initialized = false;

        inline std::string
        make_core_mask(std::span<const uint32_t> cores) {
            if (cores.empty()) {
                throw std::invalid_argument(
                    "inconel::nvme::real_device: cores is empty");
            }
            uint64_t mask = 0;
            for (uint32_t core : cores) {
                if (core >= 64) {
                    throw std::invalid_argument(
                        "inconel::nvme::real_device: core id >= 64 cannot "
                        "be represented in the SPDK core mask helper");
                }
                mask |= (uint64_t{1} << core);
            }

            std::ostringstream out;
            out << "0x" << std::hex << mask;
            return out.str();
        }

        inline void
        init_spdk_env_once(std::span<const uint32_t> cores,
                           const char* explicit_core_mask,
                           const char* name) {
            if (g_spdk_env_initialized) return;

            const std::string generated_mask =
                explicit_core_mask == nullptr ? make_core_mask(cores)
                                              : std::string();

            spdk_env_opts opts;
            opts.opts_size = sizeof(opts);
            spdk_env_opts_init(&opts);
            opts.name = name == nullptr ? "inconel" : name;
            opts.core_mask = explicit_core_mask == nullptr
                ? generated_mask.c_str()
                : explicit_core_mask;

            if (spdk_env_init(&opts) < 0) {
                throw std::runtime_error(
                    "inconel::nvme::real_device: spdk_env_init failed");
            }
            g_spdk_env_initialized = true;
        }

        struct probe_ctx {
            const char*  pci_addr = nullptr;
            pump_lba_ssd* ssd = nullptr;
            uint32_t     qpair_count = 0;
            uint32_t     qpair_depth = 0;
            bool         found = false;
        };

        inline bool
        probe_cb(void* cb_ctx,
                 const spdk_nvme_transport_id* trid,
                 spdk_nvme_ctrlr_opts* opts) {
            if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
                return false;
            }
            auto* ctx = static_cast<probe_ctx*>(cb_ctx);
            if (std::strcmp(ctx->pci_addr, trid->traddr) != 0) {
                return false;
            }
            opts->num_io_queues = ctx->qpair_count;
            opts->io_queue_size = std::max<uint32_t>(
                ctx->qpair_depth, opts->io_queue_size);
            return true;
        }

        inline void
        attach_cb(void* cb_ctx,
                  const spdk_nvme_transport_id* trid,
                  spdk_nvme_ctrlr* ctrlr,
                  const spdk_nvme_ctrlr_opts* /*opts*/) {
            auto* ctx = static_cast<probe_ctx*>(cb_ctx);
            if (std::strcmp(ctx->pci_addr, trid->traddr) != 0) {
                return;
            }
            if (spdk_nvme_ctrlr_reset(ctrlr) < 0) {
                throw std::runtime_error(
                    "inconel::nvme::real_device: spdk_nvme_ctrlr_reset failed");
            }

            auto* ssd = ctx->ssd;
            ssd->ctrlr = ctrlr;
            ssd->ns = spdk_nvme_ctrlr_get_ns(
                ctrlr, spdk_nvme_ctrlr_get_first_active_ns(ctrlr));
            if (ssd->ns == nullptr) {
                throw std::runtime_error(
                    "inconel::nvme::real_device: controller has no active namespace");
            }
            ssd->sector_size = spdk_nvme_ns_get_sector_size(ssd->ns);
            ssd->max_sector = spdk_nvme_ns_get_num_sectors(ssd->ns);
            ssd->cfg_qpair_count = ctx->qpair_count;
            ssd->cfg_qpair_depth = ctx->qpair_depth;
            ctx->found = true;
        }
    }  // namespace _real_device

    class real_device {
    public:
        using ssd_t = pump_lba_ssd;
        using qpair_t = pump::scheduler::nvme::qpair<lba_page_adapter>;

        explicit
        real_device(const real_device_options& opts)
            : pci_addr_(opts.pci_addr == nullptr ? "" : opts.pci_addr)
            , device_id_(opts.device_id) {
            if (pci_addr_.empty()) {
                throw std::invalid_argument(
                    "inconel::nvme::real_device: pci_addr is null/empty");
            }
            if (opts.cores.empty()) {
                throw std::invalid_argument(
                    "inconel::nvme::real_device: cores is empty");
            }
            if (opts.qpair_depth == 0) {
                throw std::invalid_argument(
                    "inconel::nvme::real_device: qpair_depth is 0");
            }

            if (opts.init_spdk_env) {
                _real_device::init_spdk_env_once(
                    opts.cores, opts.spdk_core_mask, opts.spdk_name);
            }

            ssd_ = std::make_unique<ssd_t>();
            ssd_->sn = pci_addr_.c_str();

            _real_device::probe_ctx pctx{
                .pci_addr = pci_addr_.c_str(),
                .ssd = ssd_.get(),
                .qpair_count = static_cast<uint32_t>(opts.cores.size()),
                .qpair_depth = opts.qpair_depth,
            };

            spdk_nvme_transport_id trid{};
            spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
            if (spdk_nvme_probe(
                    &trid, &pctx, _real_device::probe_cb,
                    _real_device::attach_cb, nullptr) < 0) {
                throw std::runtime_error(
                    "inconel::nvme::real_device: spdk_nvme_probe failed");
            }
            if (!pctx.found || ssd_->ctrlr == nullptr || ssd_->ns == nullptr) {
                throw std::runtime_error(
                    "inconel::nvme::real_device: target NVMe device not found");
            }

            try {
                allocate_qpairs(opts.cores, opts.qpair_depth);
            } catch (...) {
                cleanup();
                throw;
            }
        }

        ~real_device() {
            cleanup();
        }

        real_device(const real_device&) = delete;
        real_device& operator=(const real_device&) = delete;
        real_device(real_device&&) = delete;
        real_device& operator=(real_device&&) = delete;

        [[nodiscard]] qpair_t*
        qpair_for_core(uint32_t core) const {
            if (core >= qpairs_by_core_.size() ||
                qpairs_by_core_[core] == nullptr) {
                throw std::runtime_error(
                    "inconel::nvme::real_device: no qpair for requested core");
            }
            return qpairs_by_core_[core];
        }

        [[nodiscard]] const ssd_t*
        ssd() const noexcept {
            return ssd_.get();
        }

        [[nodiscard]] uint16_t
        device_id() const noexcept {
            return device_id_;
        }

        [[nodiscard]] uint32_t
        sector_size() const noexcept {
            return ssd_ ? ssd_->sector_size : 0;
        }

        [[nodiscard]] uint64_t
        size_bytes() const noexcept {
            if (!ssd_) return 0;
            return static_cast<uint64_t>(ssd_->sector_size) * ssd_->max_sector;
        }

        [[nodiscard]] uint64_t
        total_logical_lbas(uint32_t logical_lba_size) const noexcept {
            if (logical_lba_size == 0) return 0;
            return size_bytes() / logical_lba_size;
        }

        [[nodiscard]] bool
        namespace_supports_deallocate() const noexcept {
            return ssd_ != nullptr && ssd_->ns != nullptr &&
                   (spdk_nvme_ns_get_flags(ssd_->ns) &
                    SPDK_NVME_NS_DEALLOCATE_SUPPORTED) != 0;
        }

        [[nodiscard]] bool
        deallocate_reads_zero() const noexcept {
            return ssd_ != nullptr && ssd_->ns != nullptr &&
                   spdk_nvme_ns_get_dealloc_logical_block_read_value(
                       ssd_->ns) == SPDK_NVME_DEALLOC_READ_00;
        }

    private:
        void
        allocate_qpairs(std::span<const uint32_t> cores, uint32_t depth) {
            auto max_core = *std::max_element(cores.begin(), cores.end());
            qpairs_by_core_.assign(static_cast<size_t>(max_core) + 1, nullptr);

            spdk_nvme_io_qpair_opts qopts{};
            spdk_nvme_ctrlr_get_default_io_qpair_opts(
                ssd_->ctrlr, &qopts, sizeof(qopts));
            qopts.delay_cmd_submit = false;
            qopts.create_only = false;

            for (uint32_t core : cores) {
                auto* raw_qp = spdk_nvme_ctrlr_alloc_io_qpair(
                    ssd_->ctrlr, &qopts, sizeof(qopts));
                if (raw_qp == nullptr) {
                    throw std::runtime_error(
                        "inconel::nvme::real_device: "
                        "spdk_nvme_ctrlr_alloc_io_qpair failed");
                }

                auto* qp = new qpair_t{
                    .impl = raw_qp,
                    .core = core,
                    .depth = depth,
                    .used = 0,
                    .owner = ssd_.get(),
                };
                qpairs_.push_back(qp);
                qpairs_by_core_[core] = qp;
            }
        }

        void
        cleanup() noexcept {
            for (auto* qp : qpairs_) {
                if (qp == nullptr) continue;
                if (qp->impl != nullptr) {
                    spdk_nvme_ctrlr_free_io_qpair(qp->impl);
                    qp->impl = nullptr;
                }
                delete qp;
            }
            qpairs_.clear();
            qpairs_by_core_.clear();

            if (ssd_ && ssd_->ctrlr != nullptr) {
                spdk_nvme_detach(ssd_->ctrlr);
                ssd_->ctrlr = nullptr;
            }
        }

        std::string pci_addr_;
        uint16_t device_id_ = 0;
        std::unique_ptr<ssd_t> ssd_;
        std::vector<qpair_t*> qpairs_;
        std::vector<qpair_t*> qpairs_by_core_;
    };

}  // namespace apps::inconel::nvme

#endif  // APPS_INCONEL_NVME_REAL_DEVICE_HH
