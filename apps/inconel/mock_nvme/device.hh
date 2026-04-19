#ifndef APPS_INCONEL_MOCK_NVME_DEVICE_HH
#define APPS_INCONEL_MOCK_NVME_DEVICE_HH

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace apps::inconel::mock_nvme {

    struct mock_device {
        // `storage` owns the backing memory. Switched from `char*` so that
        // an externally-initialised buffer (e.g. the one produced by
        // `format::make_formatted_storage`) can be adopted via the second
        // ctor below without changing ownership semantics at the call site.
        std::unique_ptr<char[]> storage;
        uint64_t namespace_size;
        uint32_t lba_size;
        uint64_t total_lbas;
        std::vector<bool> trimmed;
        std::mutex* shared_mtx = nullptr;  // optional, for multi-threaded testing
        std::atomic<uint64_t> read_count_{0};
        std::atomic<uint64_t> write_count_{0};

        // Blank-buffer ctor: allocates a zero-initialised `ns_size` block.
        // `trimmed(total_lbas, false)` reflects "no LBA has been explicitly
        // TRIM'd yet" — the zero bytes coming from `make_unique<char[]>`
        // are the fresh-allocation state, not a post-TRIM state.
        mock_device(uint64_t ns_size, uint32_t lba_sz)
            : storage(std::make_unique<char[]>(ns_size))
            , namespace_size(ns_size)
            , lba_size(lba_sz)
            , total_lbas(ns_size / lba_sz)
            , trimmed(total_lbas, false) {
        }

        // Adopting ctor: takes ownership of a pre-initialised buffer. The
        // buffer must be exactly `ns_size` bytes; the caller is responsible
        // for producing that (typically via `format::make_formatted_storage`).
        // `trimmed` is initialised to all-false for the same reason as
        // above: the adopted bytes have not flowed through `do_trim`.
        mock_device(std::unique_ptr<char[]> adopted_bytes,
                    uint64_t                ns_size,
                    uint32_t                lba_sz)
            : storage(std::move(adopted_bytes))
            , namespace_size(ns_size)
            , lba_size(lba_sz)
            , total_lbas(ns_size / lba_sz)
            , trimmed(total_lbas, false) {
        }

        // ~mock_device: relies on the implicit destructor — `unique_ptr`
        // releases `storage`, `vector`/`atomic` members clean up
        // themselves.

        mock_device(const mock_device&) = delete;
        mock_device& operator=(const mock_device&) = delete;

        void enable_thread_safety(std::mutex* mtx) { shared_mtx = mtx; }

        // ── I/O operations (called by scheduler::advance) ──

        bool
        do_write(uint64_t lba, const void* data, uint32_t num_lbas) {
            if (shared_mtx) { std::lock_guard lk(*shared_mtx); return do_write_impl(lba, data, num_lbas); }
            return do_write_impl(lba, data, num_lbas);
        }

        bool
        do_read(uint64_t lba, void* buf, uint32_t num_lbas) {
            if (shared_mtx) { std::lock_guard lk(*shared_mtx); return do_read_impl(lba, buf, num_lbas); }
            return do_read_impl(lba, buf, num_lbas);
        }

        bool
        do_flush() { return true; }

        bool
        do_trim(uint64_t lba, uint32_t num_lbas) {
            if (shared_mtx) { std::lock_guard lk(*shared_mtx); return do_trim_impl(lba, num_lbas); }
            return do_trim_impl(lba, num_lbas);
        }

    private:
        bool do_write_impl(uint64_t lba, const void* data, uint32_t num_lbas) {
            if (lba + num_lbas > total_lbas) return false;
            std::memcpy(storage.get() + lba * lba_size, data, static_cast<uint64_t>(num_lbas) * lba_size);
            for (uint64_t i = lba; i < lba + num_lbas; ++i) trimmed[i] = false;
            write_count_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        bool do_read_impl(uint64_t lba, void* buf, uint32_t num_lbas) {
            if (lba + num_lbas > total_lbas) return false;
            std::memcpy(buf, storage.get() + lba * lba_size, static_cast<uint64_t>(num_lbas) * lba_size);
            read_count_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        bool do_trim_impl(uint64_t lba, uint32_t num_lbas) {
            if (lba + num_lbas > total_lbas) return false;
            std::memset(storage.get() + lba * lba_size, 0, static_cast<uint64_t>(num_lbas) * lba_size);
            for (uint64_t i = lba; i < lba + num_lbas; ++i) trimmed[i] = true;
            return true;
        }
    public:

        // ── Test helpers (direct access, bypass scheduler) ──

        const void*
        test_read_raw(uint64_t lba, uint32_t num_lbas = 1) const {
            if (lba + num_lbas > total_lbas) return nullptr;
            return storage.get() + lba * lba_size;
        }

        void*
        test_write_raw(uint64_t lba, uint32_t num_lbas = 1) {
            if (lba + num_lbas > total_lbas) return nullptr;
            for (uint64_t i = lba; i < lba + num_lbas; ++i)
                trimmed[i] = false;
            return storage.get() + lba * lba_size;
        }

        bool
        test_is_trimmed(uint64_t lba) const {
            if (lba >= total_lbas) return false;
            return trimmed[lba];
        }

        uint64_t get_total_lbas() const { return total_lbas; }
        uint32_t get_lba_size() const { return lba_size; }
        uint64_t get_namespace_size() const { return namespace_size; }

        uint64_t get_read_count() const  { return read_count_.load(std::memory_order_relaxed); }
        uint64_t get_write_count() const { return write_count_.load(std::memory_order_relaxed); }
        void reset_io_counters() {
            read_count_.store(0, std::memory_order_relaxed);
            write_count_.store(0, std::memory_order_relaxed);
        }
    };

}

#endif //APPS_INCONEL_MOCK_NVME_DEVICE_HH
