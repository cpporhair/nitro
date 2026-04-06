#ifndef APPS_INCONEL_MOCK_NVME_DEVICE_HH
#define APPS_INCONEL_MOCK_NVME_DEVICE_HH

#include <cstdint>
#include <cstring>
#include <vector>

namespace apps::inconel::mock_nvme {

    struct mock_device {
        char* storage;
        uint64_t namespace_size;
        uint32_t lba_size;
        uint64_t total_lbas;
        std::vector<bool> trimmed;

        mock_device(uint64_t ns_size, uint32_t lba_sz)
            : namespace_size(ns_size)
            , lba_size(lba_sz)
            , total_lbas(ns_size / lba_sz)
            , trimmed(total_lbas, false) {
            storage = new char[ns_size]();
        }

        ~mock_device() {
            delete[] storage;
        }

        mock_device(const mock_device&) = delete;
        mock_device& operator=(const mock_device&) = delete;

        // ── I/O operations (called by scheduler::advance) ──

        bool
        do_write(uint64_t lba, const void* data, uint32_t num_lbas) {
            if (lba + num_lbas > total_lbas) return false;
            std::memcpy(storage + lba * lba_size, data, static_cast<uint64_t>(num_lbas) * lba_size);
            for (uint64_t i = lba; i < lba + num_lbas; ++i)
                trimmed[i] = false;
            return true;
        }

        bool
        do_read(uint64_t lba, void* buf, uint32_t num_lbas) const {
            if (lba + num_lbas > total_lbas) return false;
            std::memcpy(buf, storage + lba * lba_size, static_cast<uint64_t>(num_lbas) * lba_size);
            return true;
        }

        bool
        do_flush() const {
            return true;
        }

        bool
        do_trim(uint64_t lba, uint32_t num_lbas) {
            if (lba + num_lbas > total_lbas) return false;
            std::memset(storage + lba * lba_size, 0, static_cast<uint64_t>(num_lbas) * lba_size);
            for (uint64_t i = lba; i < lba + num_lbas; ++i)
                trimmed[i] = true;
            return true;
        }

        // ── Test helpers (direct access, bypass scheduler) ──

        const void*
        test_read_raw(uint64_t lba, uint32_t num_lbas = 1) const {
            if (lba + num_lbas > total_lbas) return nullptr;
            return storage + lba * lba_size;
        }

        void*
        test_write_raw(uint64_t lba, uint32_t num_lbas = 1) {
            if (lba + num_lbas > total_lbas) return nullptr;
            for (uint64_t i = lba; i < lba + num_lbas; ++i)
                trimmed[i] = false;
            return storage + lba * lba_size;
        }

        bool
        test_is_trimmed(uint64_t lba) const {
            if (lba >= total_lbas) return false;
            return trimmed[lba];
        }

        uint64_t get_total_lbas() const { return total_lbas; }
        uint32_t get_lba_size() const { return lba_size; }
        uint64_t get_namespace_size() const { return namespace_size; }
    };

}

#endif //APPS_INCONEL_MOCK_NVME_DEVICE_HH
