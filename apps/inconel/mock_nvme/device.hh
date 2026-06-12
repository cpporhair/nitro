#ifndef APPS_INCONEL_MOCK_NVME_DEVICE_HH
#define APPS_INCONEL_MOCK_NVME_DEVICE_HH

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "spdk/nvme.h"

namespace apps::inconel::mock_nvme {

    class mock_device {
    public:
        struct core_handle {
            mock_device* owner = nullptr;
            uint32_t core = 0;
        };

        struct io_counts {
            uint64_t reads = 0;
            uint64_t writes = 0;
            uint64_t trims = 0;
            uint64_t flushes = 0;
            uint64_t fua_writes = 0;
        };

        explicit
        mock_device(uint32_t lba_size,
                    uint64_t namespace_lbas,
                    uint16_t device_id = 0)
            : lba_size_(lba_size)
            , namespace_lbas_(namespace_lbas)
            , device_id_(device_id) {
            if (lba_size_ == 0) {
                throw std::invalid_argument(
                    "inconel::mock_nvme::mock_device: lba_size is 0");
            }
            if (namespace_lbas_ == 0) {
                throw std::invalid_argument(
                    "inconel::mock_nvme::mock_device: namespace_lbas is 0");
            }
            if (namespace_lbas_ >
                std::numeric_limits<uint64_t>::max() / lba_size_) {
                throw std::invalid_argument(
                    "inconel::mock_nvme::mock_device: namespace byte size overflows uint64_t");
            }

            const uint64_t bytes = namespace_lbas_ * lba_size_;
            if (bytes > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                    "inconel::mock_nvme::mock_device: namespace byte size exceeds size_t");
            }
            bytes_.assign(static_cast<std::size_t>(bytes), char{0});
        }

        mock_device(const mock_device&) = delete;
        mock_device& operator=(const mock_device&) = delete;
        mock_device(mock_device&&) = delete;
        mock_device& operator=(mock_device&&) = delete;

        [[nodiscard]] core_handle*
        qpair_for_core(uint32_t core) {
            std::lock_guard<std::mutex> g(mu_);
            if (handles_by_core_.size() <= core) {
                handles_by_core_.resize(static_cast<std::size_t>(core) + 1);
            }
            auto& handle = handles_by_core_[core];
            if (!handle) {
                handle = std::make_unique<core_handle>(
                    core_handle{.owner = this, .core = core});
            }
            return handle.get();
        }

        [[nodiscard]] uint32_t
        sector_size() const noexcept {
            return lba_size_;
        }

        [[nodiscard]] uint64_t
        total_logical_lbas(uint32_t logical_lba_size) const noexcept {
            if (logical_lba_size == 0) return 0;
            return (namespace_lbas_ * lba_size_) / logical_lba_size;
        }

        [[nodiscard]] uint16_t
        device_id() const noexcept {
            return device_id_;
        }

        bool
        read(uint64_t lba, void* dst, uint32_t num_lbas) {
            if (dst == nullptr || num_lbas == 0) return false;
            const uint64_t bytes = static_cast<uint64_t>(num_lbas) * lba_size_;
            std::lock_guard<std::mutex> g(mu_);
            const auto offset = byte_offset_locked(lba, bytes);
            if (!offset.has_value()) return false;
            std::memcpy(dst, bytes_.data() + *offset,
                        static_cast<std::size_t>(bytes));
            ++counts_.reads;
            return true;
        }

        bool
        write(uint64_t lba,
              const void* src,
              uint32_t num_lbas,
              uint32_t flags = 0) {
            if (src == nullptr || num_lbas == 0) return false;
            const uint64_t bytes = static_cast<uint64_t>(num_lbas) * lba_size_;
            std::lock_guard<std::mutex> g(mu_);
            const auto offset = byte_offset_locked(lba, bytes);
            if (!offset.has_value()) return false;
            std::memcpy(bytes_.data() + *offset, src,
                        static_cast<std::size_t>(bytes));
            ++counts_.writes;
            if ((flags & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0) {
                ++counts_.fua_writes;
            }
            return true;
        }

        bool
        trim(uint64_t lba, uint32_t num_lbas) {
            if (num_lbas == 0) return false;
            const uint64_t bytes = static_cast<uint64_t>(num_lbas) * lba_size_;
            std::lock_guard<std::mutex> g(mu_);
            const auto offset = byte_offset_locked(lba, bytes);
            if (!offset.has_value()) return false;
            std::fill_n(bytes_.data() + *offset,
                        static_cast<std::size_t>(bytes),
                        char{0});
            ++counts_.trims;
            return true;
        }

        bool
        flush() {
            std::lock_guard<std::mutex> g(mu_);
            ++counts_.flushes;
            return true;
        }

        bool
        read_bytes(uint64_t lba, std::span<char> out) {
            return read_bytes_raw(lba, out.data(), out.size());
        }

        bool
        read_bytes(uint64_t lba, std::span<std::byte> out) {
            return read_bytes_raw(lba, out.data(), out.size());
        }

        bool
        write_bytes(uint64_t lba, std::span<const char> in) {
            return write_bytes_raw(lba, in.data(), in.size(), 0);
        }

        bool
        write_bytes(uint64_t lba, std::span<const std::byte> in) {
            return write_bytes_raw(lba, in.data(), in.size(), 0);
        }

        [[nodiscard]] io_counts
        counts() const {
            std::lock_guard<std::mutex> g(mu_);
            return counts_;
        }

        [[nodiscard]] uint64_t reads() const {
            return counts().reads;
        }

        [[nodiscard]] uint64_t writes() const {
            return counts().writes;
        }

        [[nodiscard]] uint64_t trims() const {
            return counts().trims;
        }

        [[nodiscard]] uint64_t flushes() const {
            return counts().flushes;
        }

        [[nodiscard]] uint64_t fua_writes() const {
            return counts().fua_writes;
        }

    private:
        [[nodiscard]] std::optional<std::size_t>
        byte_offset_locked(uint64_t lba, uint64_t bytes) const {
            if (lba > namespace_lbas_) return std::nullopt;
            const uint64_t offset = lba * lba_size_;
            const uint64_t total = namespace_lbas_ * lba_size_;
            if (bytes > total - offset) return std::nullopt;
            return static_cast<std::size_t>(offset);
        }

        bool
        read_bytes_raw(uint64_t lba, void* dst, std::size_t bytes) {
            if (dst == nullptr && bytes != 0) return false;
            std::lock_guard<std::mutex> g(mu_);
            const auto offset = byte_offset_locked(lba, bytes);
            if (!offset.has_value()) return false;
            if (bytes != 0) {
                std::memcpy(dst, bytes_.data() + *offset, bytes);
            }
            ++counts_.reads;
            return true;
        }

        bool
        write_bytes_raw(uint64_t lba,
                        const void* src,
                        std::size_t bytes,
                        uint32_t flags) {
            if (src == nullptr && bytes != 0) return false;
            std::lock_guard<std::mutex> g(mu_);
            const auto offset = byte_offset_locked(lba, bytes);
            if (!offset.has_value()) return false;
            if (bytes != 0) {
                std::memcpy(bytes_.data() + *offset, src, bytes);
            }
            ++counts_.writes;
            if ((flags & SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS) != 0) {
                ++counts_.fua_writes;
            }
            return true;
        }

        uint32_t lba_size_ = 0;
        uint64_t namespace_lbas_ = 0;
        uint16_t device_id_ = 0;
        mutable std::mutex mu_;
        std::vector<char> bytes_;
        std::vector<std::unique_ptr<core_handle>> handles_by_core_;
        io_counts counts_;
    };

}  // namespace apps::inconel::mock_nvme

#endif  // APPS_INCONEL_MOCK_NVME_DEVICE_HH
