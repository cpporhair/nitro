#ifndef APPS_INCONEL_RECOVERY_SYNC_IO_HH
#define APPS_INCONEL_RECOVERY_SYNC_IO_HH

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "spdk/env.h"
#include "spdk/nvme.h"

#include "../nvme/real_device.hh"
#include "../nvme/real_scheduler.hh"

namespace apps::inconel::recovery {

    inline constexpr auto kSyncIoTimeout = std::chrono::seconds(30);

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
    make_zeroed_dma_buffer(std::size_t bytes, std::size_t alignment) {
        void* p = spdk_dma_zmalloc(bytes, alignment, nullptr);
        if (p == nullptr) {
            throw std::runtime_error(
                "inconel recovery: spdk_dma_zmalloc failed");
        }
        return dma_buffer(p);
    }

    struct sync_io_buffer_deleter {
        bool spdk_allocated = false;

        void
        operator()(void* p) const noexcept {
            if (p == nullptr) return;
            if (spdk_allocated) {
                spdk_dma_free(p);
            } else {
                std::free(p);
            }
        }
    };

    using sync_io_buffer =
        std::unique_ptr<void, sync_io_buffer_deleter>;

    [[nodiscard]] inline sync_io_buffer
    make_zeroed_sync_io_buffer(nvme::real_device&,
                               std::size_t bytes,
                               std::size_t alignment) {
        void* p = spdk_dma_zmalloc(bytes, alignment, nullptr);
        if (p == nullptr) {
            throw std::runtime_error(
                "inconel recovery: spdk_dma_zmalloc failed");
        }
        return sync_io_buffer(
            p, sync_io_buffer_deleter{.spdk_allocated = true});
    }

    template <typename Device>
    requires(!std::same_as<std::remove_cvref_t<Device>, nvme::real_device>)
    [[nodiscard]] inline sync_io_buffer
    make_zeroed_sync_io_buffer(Device&,
                               std::size_t bytes,
                               std::size_t alignment) {
        if (alignment == 0) {
            throw std::invalid_argument(
                "inconel recovery: sync buffer alignment is 0");
        }
        void* p = nullptr;
        if (posix_memalign(&p, alignment, bytes) != 0 || p == nullptr) {
            throw std::runtime_error(
                "inconel recovery: posix_memalign failed");
        }
        std::memset(p, 0, bytes);
        return sync_io_buffer(
            p, sync_io_buffer_deleter{.spdk_allocated = false});
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
        const auto deadline =
            std::chrono::steady_clock::now() + kSyncIoTimeout;
        while (!done.done) {
            const int rc =
                spdk_nvme_qpair_process_completions(qpair->impl, 0);
            if (rc < 0) {
                throw std::runtime_error(
                    std::string("inconel recovery: ") + what +
                    " completion polling failed");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error(
                    std::string("inconel recovery: ") + what +
                    " timed out");
            }
        }
        if (!done.ok) {
            throw std::runtime_error(
                std::string("inconel recovery: ") + what +
                " completed with NVMe error");
        }
    }

    inline void
    validate_logical_io_shape(const nvme::real_device& device,
                              uint32_t logical_lba_size,
                              uint32_t logical_lba_count) {
        if (device.sector_size() == 0 ||
            logical_lba_size % device.sector_size() != 0) {
            throw std::invalid_argument(
                "inconel recovery: logical LBA size is not sector aligned");
        }
        if (logical_lba_count == 0) {
            throw std::invalid_argument(
                "inconel recovery: logical LBA count is 0");
        }
        const uint64_t sectors_per_lba =
            static_cast<uint64_t>(logical_lba_size) / device.sector_size();
        if (sectors_per_lba == 0 ||
            sectors_per_lba >
                std::numeric_limits<uint32_t>::max() / logical_lba_count) {
            throw std::invalid_argument(
                "inconel recovery: logical IO sector count overflows");
        }
    }

    inline void
    sync_read_logical_lbas(nvme::real_device& device,
                           uint32_t core,
                           uint64_t logical_lba,
                           uint32_t logical_lba_count,
                           void* data,
                           uint32_t logical_lba_size) {
        validate_logical_io_shape(device, logical_lba_size, logical_lba_count);

        auto* qpair = device.qpair_for_core(core);
        const uint32_t sectors_per_lba =
            logical_lba_size / device.sector_size();
        const uint64_t ns_lba =
            logical_lba * static_cast<uint64_t>(sectors_per_lba);
        const uint32_t sector_count =
            logical_lba_count * sectors_per_lba;

        sync_completion done;
        const int rc = spdk_nvme_ns_cmd_read(
            qpair->owner->ns,
            qpair->impl,
            data,
            ns_lba,
            sector_count,
            sync_completion_cb,
            &done,
            0);
        if (rc != 0) {
            throw std::runtime_error(
                "inconel recovery: spdk_nvme_ns_cmd_read failed");
        }
        wait_for_completion(qpair, done, "read");
    }

    inline void
    sync_write_logical_lbas(nvme::real_device& device,
                            uint32_t core,
                            uint64_t logical_lba,
                            uint32_t logical_lba_count,
                            const void* data,
                            uint32_t logical_lba_size,
                            uint32_t io_flags = nvme::IO_FLAGS_FUA) {
        validate_logical_io_shape(device, logical_lba_size, logical_lba_count);

        auto* qpair = device.qpair_for_core(core);
        const uint32_t sectors_per_lba =
            logical_lba_size / device.sector_size();
        const uint64_t ns_lba =
            logical_lba * static_cast<uint64_t>(sectors_per_lba);
        const uint32_t sector_count =
            logical_lba_count * sectors_per_lba;

        sync_completion done;
        const int rc = spdk_nvme_ns_cmd_write(
            qpair->owner->ns,
            qpair->impl,
            const_cast<void*>(data),
            ns_lba,
            sector_count,
            sync_completion_cb,
            &done,
            io_flags);
        if (rc != 0) {
            throw std::runtime_error(
                "inconel recovery: spdk_nvme_ns_cmd_write failed");
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
                "inconel recovery: spdk_nvme_ns_cmd_flush failed");
        }
        wait_for_completion(qpair, done, "flush");
    }

    inline void
    sync_zero_logical_lbas(nvme::real_device& device,
                           uint32_t core,
                           uint64_t logical_lba,
                           uint64_t logical_lba_count,
                           uint32_t logical_lba_size,
                           uint32_t alignment) {
        if (logical_lba_count == 0) {
            return;
        }
        if (logical_lba_size == 0) {
            throw std::invalid_argument(
                "inconel recovery: zero logical LBA size is 0");
        }

        constexpr uint64_t kMaxChunkBytes = 1024ull * 1024ull;
        uint64_t chunk_lbas = kMaxChunkBytes / logical_lba_size;
        if (chunk_lbas == 0) {
            chunk_lbas = 1;
        }
        if (chunk_lbas > std::numeric_limits<uint32_t>::max()) {
            chunk_lbas = std::numeric_limits<uint32_t>::max();
        }

        auto buf = make_zeroed_dma_buffer(
            static_cast<std::size_t>(chunk_lbas) * logical_lba_size,
            alignment);
        uint64_t remaining = logical_lba_count;
        uint64_t cursor = logical_lba;
        while (remaining != 0) {
            const uint32_t now = static_cast<uint32_t>(
                remaining < chunk_lbas ? remaining : chunk_lbas);
            sync_write_logical_lbas(
                device, core, cursor, now, buf.get(), logical_lba_size);
            cursor += now;
            remaining -= now;
        }
    }

    template <typename Device>
    requires(!std::same_as<std::remove_cvref_t<Device>, nvme::real_device>)
    inline void
    validate_logical_io_shape(const Device& device,
                              uint32_t logical_lba_size,
                              uint32_t logical_lba_count) {
        if (device.sector_size() == 0 ||
            logical_lba_size % device.sector_size() != 0) {
            throw std::invalid_argument(
                "inconel recovery: logical LBA size is not sector aligned");
        }
        if (logical_lba_count == 0) {
            throw std::invalid_argument(
                "inconel recovery: logical LBA count is 0");
        }
        const uint64_t sectors_per_lba =
            static_cast<uint64_t>(logical_lba_size) / device.sector_size();
        if (sectors_per_lba == 0 ||
            sectors_per_lba >
                std::numeric_limits<uint32_t>::max() / logical_lba_count) {
            throw std::invalid_argument(
                "inconel recovery: logical IO sector count overflows");
        }
    }

    template <typename Device>
    requires(!std::same_as<std::remove_cvref_t<Device>, nvme::real_device>)
    inline void
    sync_read_logical_lbas(Device& device,
                           uint32_t core,
                           uint64_t logical_lba,
                           uint32_t logical_lba_count,
                           void* data,
                           uint32_t logical_lba_size) {
        validate_logical_io_shape(device, logical_lba_size, logical_lba_count);
        device.sync_read_logical_lbas(
            core, logical_lba, logical_lba_count, data, logical_lba_size);
    }

    template <typename Device>
    requires(!std::same_as<std::remove_cvref_t<Device>, nvme::real_device>)
    inline void
    sync_write_logical_lbas(Device& device,
                            uint32_t core,
                            uint64_t logical_lba,
                            uint32_t logical_lba_count,
                            const void* data,
                            uint32_t logical_lba_size,
                            uint32_t io_flags = nvme::IO_FLAGS_FUA) {
        validate_logical_io_shape(device, logical_lba_size, logical_lba_count);
        device.sync_write_logical_lbas(
            core,
            logical_lba,
            logical_lba_count,
            data,
            logical_lba_size,
            io_flags);
    }

    template <typename Device>
    requires(!std::same_as<std::remove_cvref_t<Device>, nvme::real_device>)
    inline void
    sync_flush(Device& device, uint32_t core) {
        device.sync_flush(core);
    }

    template <typename Device>
    requires(!std::same_as<std::remove_cvref_t<Device>, nvme::real_device>)
    inline void
    sync_zero_logical_lbas(Device& device,
                           uint32_t core,
                           uint64_t logical_lba,
                           uint64_t logical_lba_count,
                           uint32_t logical_lba_size,
                           uint32_t alignment) {
        if (logical_lba_count == 0) {
            return;
        }
        if (logical_lba_size == 0) {
            throw std::invalid_argument(
                "inconel recovery: zero logical LBA size is 0");
        }

        constexpr uint64_t kMaxChunkBytes = 1024ull * 1024ull;
        uint64_t chunk_lbas = kMaxChunkBytes / logical_lba_size;
        if (chunk_lbas == 0) {
            chunk_lbas = 1;
        }
        if (chunk_lbas > std::numeric_limits<uint32_t>::max()) {
            chunk_lbas = std::numeric_limits<uint32_t>::max();
        }

        auto buf =
            make_zeroed_sync_io_buffer(device,
                                       static_cast<std::size_t>(chunk_lbas)
                                           * logical_lba_size,
                                       alignment);
        uint64_t remaining = logical_lba_count;
        uint64_t cursor = logical_lba;
        while (remaining != 0) {
            const uint32_t now = static_cast<uint32_t>(
                remaining < chunk_lbas ? remaining : chunk_lbas);
            sync_write_logical_lbas(
                device, core, cursor, now, buf.get(), logical_lba_size);
            cursor += now;
            remaining -= now;
        }
    }

    [[nodiscard]] inline bool
    buffer_is_zero(const void* data, std::size_t bytes) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < bytes; ++i) {
            if (p[i] != 0) {
                return false;
            }
        }
        return true;
    }

}  // namespace apps::inconel::recovery

#endif  // APPS_INCONEL_RECOVERY_SYNC_IO_HH
