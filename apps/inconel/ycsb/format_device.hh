#ifndef APPS_INCONEL_YCSB_FORMAT_DEVICE_HH
#define APPS_INCONEL_YCSB_FORMAT_DEVICE_HH

#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "spdk/env.h"
#include "spdk/nvme.h"

#include "../format/format_profile.hh"
#include "../format/layout_plan.hh"
#include "../format/superblock_builder.hh"
#include "../nvme/real_device.hh"
#include "../nvme/real_scheduler.hh"

namespace apps::inconel::ycsb {

    namespace detail {

        struct dma_free_deleter {
            void
            operator()(void* p) const noexcept {
                if (p != nullptr) {
                    spdk_dma_free(p);
                }
            }
        };

        using dma_buffer = std::unique_ptr<void, dma_free_deleter>;

        [[nodiscard]] inline dma_buffer
        make_zeroed_dma_buffer(uint32_t bytes, uint32_t alignment) {
            void* p = spdk_dma_zmalloc(bytes, alignment, nullptr);
            if (p == nullptr) {
                throw std::runtime_error(
                    "inconel_ycsb: spdk_dma_zmalloc failed");
            }
            return dma_buffer(p);
        }

        struct sync_completion {
            bool done = false;
            bool ok = false;
        };

        inline void
        sync_completion_cb(void* arg, const spdk_nvme_cpl* cpl) {
            auto* done = static_cast<sync_completion*>(arg);
            done->ok = !spdk_nvme_cpl_is_error(cpl);
            done->done = true;
        }

        inline void
        wait_for_completion(nvme::real_device::qpair_t* qpair,
                            sync_completion& done,
                            const char* what) {
            while (!done.done) {
                const int rc =
                    spdk_nvme_qpair_process_completions(qpair->impl, 0);
                if (rc < 0) {
                    throw std::runtime_error(
                        std::string("inconel_ycsb: ") + what +
                        " completion polling failed");
                }
            }
            if (!done.ok) {
                throw std::runtime_error(
                    std::string("inconel_ycsb: ") + what +
                    " completed with NVMe error");
            }
        }

        inline void
        sync_write_logical_lba(nvme::real_device& device,
                               uint32_t core,
                               uint64_t logical_lba,
                               const void* data,
                               uint32_t logical_lba_size) {
            if (device.sector_size() == 0 ||
                logical_lba_size % device.sector_size() != 0) {
                throw std::invalid_argument(
                    "inconel_ycsb: logical LBA size is not sector aligned");
            }

            auto* qpair = device.qpair_for_core(core);
            const uint32_t sectors_per_lba =
                logical_lba_size / device.sector_size();
            const uint64_t ns_lba = logical_lba * sectors_per_lba;

            sync_completion done;
            const int rc = spdk_nvme_ns_cmd_write(
                qpair->owner->ns,
                qpair->impl,
                const_cast<void*>(data),
                ns_lba,
                sectors_per_lba,
                sync_completion_cb,
                &done,
                nvme::IO_FLAGS_FUA);
            if (rc != 0) {
                throw std::runtime_error(
                    "inconel_ycsb: spdk_nvme_ns_cmd_write failed");
            }
            wait_for_completion(qpair, done, "write");
        }

        inline void
        sync_flush(nvme::real_device& device, uint32_t core) {
            auto* qpair = device.qpair_for_core(core);
            sync_completion done;
            const int rc = spdk_nvme_ns_cmd_flush(
                qpair->owner->ns,
                qpair->impl,
                sync_completion_cb,
                &done);
            if (rc != 0) {
                throw std::runtime_error(
                    "inconel_ycsb: spdk_nvme_ns_cmd_flush failed");
            }
            wait_for_completion(qpair, done, "flush");
        }

    }  // namespace detail

    [[nodiscard]] inline format::layout_plan
    bootstrap_layout_for_device(const nvme::real_device& device) {
        const auto& p = format::kBootstrapFormatProfile;
        format::layout_plan L{};
        L.lba_size = p.lba_size;
        L.namespace_size = device.size_bytes();
        L.total_lbas = device.total_logical_lbas(p.lba_size);
        L.wal_base_paddr = p.wal_base_paddr;
        L.wal_segment_size = p.wal_segment_size;
        L.wal_segment_count = p.wal_segment_count;
        L.wal_segment_lbas = p.wal_segment_size / p.lba_size;
        L.data_area_base_paddr = p.value_data_area_base;
        L.data_area_end_paddr = p.value_data_area_end;
        L.tree_page_size = p.tree_page_size;
        L.shadow_slots_per_range = p.shadow_slots_per_range;
        L.value_class_count = p.value_class_count;
        for (uint8_t i = 0; i < format::kMaxValueClassCount; ++i) {
            L.value_class_sizes[i] = p.value_class_sizes[i];
        }
        L.value_space_quantum_bytes = p.value_space_quantum_bytes;
        L.value_space_group_size_lbas = p.value_space_group_size_lbas;

        if (L.total_lbas < p.value_data_area_end.lba) {
            throw std::runtime_error(
                "inconel_ycsb: namespace too small for bootstrap profile");
        }
        format::validate_layout(L);
        return L;
    }

    inline void
    force_format_device(nvme::real_device& device, uint32_t core) {
        const auto layout = bootstrap_layout_for_device(device);
        auto sb_a = format::build_superblock(layout, 1);
        auto sb_b = format::build_superblock(layout, 0);

        auto buf = detail::make_zeroed_dma_buffer(
            layout.lba_size, layout.lba_size);
        std::memcpy(buf.get(), &sb_a, sizeof(sb_a));
        detail::sync_write_logical_lba(
            device, core, 0, buf.get(), layout.lba_size);

        std::memset(buf.get(), 0, layout.lba_size);
        std::memcpy(buf.get(), &sb_b, sizeof(sb_b));
        detail::sync_write_logical_lba(
            device, core, 1, buf.get(), layout.lba_size);
        detail::sync_flush(device, core);
    }

}  // namespace apps::inconel::ycsb

#endif  // APPS_INCONEL_YCSB_FORMAT_DEVICE_HH
