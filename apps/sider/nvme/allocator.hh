#pragma once

#include <cstdint>
#include <vector>
#include <bit>

namespace sider::nvme {

    // Two-level bitmap NVMe page allocator.
    //
    // L1: one bit per page (1 = free, 0 = used).
    // L0: one bit per 64 pages (1 = at least one free page in that group).
    //
    // allocate: scan L0 for non-zero word → ctzll → scan L1 word → ctzll. O(1) amortized.
    // free: set L1 bit + set L0 bit. O(1).
    //
    // Memory: 1TB disk = 244M pages → L1 = 30MB, L0 = 480KB. Total ~30.5MB.
    // (vs 1.9GB for the free-page stack)
    //
    // Not persistent: on restart all pages are free.
    // Single-threaded per core (share-nothing).

    struct nvme_allocator {
        static constexpr uint64_t INVALID_LBA = UINT64_MAX;

        std::vector<uint64_t> l1_;       // one bit per page
        std::vector<uint64_t> l0_;       // one bit per 64-page group (summary)
        uint64_t total_pages_ = 0;
        uint64_t used_pages_ = 0;
        uint64_t l0_hint_ = 0;          // last L0 word with a free bit (scan hint)

        nvme_allocator() = default;

        // Initialize with [0, page_count) all free.
        explicit nvme_allocator(uint64_t page_count)
            : total_pages_(page_count) {
            uint64_t l1_words = (page_count + 63) / 64;
            uint64_t l0_words = (l1_words + 63) / 64;

            l1_.resize(l1_words, ~0ULL);
            l0_.resize(l0_words, ~0ULL);

            // Mask off trailing bits beyond page_count in the last L1 word.
            uint64_t tail = page_count % 64;
            if (tail != 0)
                l1_.back() = (1ULL << tail) - 1;

            // Mask off trailing bits beyond l1_words in the last L0 word.
            uint64_t l0_tail = l1_words % 64;
            if (l0_tail != 0)
                l0_.back() = (1ULL << l0_tail) - 1;
        }

        uint64_t allocate() {
            // Scan L0 from hint for a word with a free group.
            uint64_t l0_size = l0_.size();
            for (uint64_t i = 0; i < l0_size; i++) {
                uint64_t idx = (l0_hint_ + i) % l0_size;
                if (l0_[idx] == 0) continue;

                // Found a group with free pages.
                uint32_t group = std::countr_zero(l0_[idx]);
                uint64_t l1_idx = idx * 64 + group;

                if (l1_idx >= l1_.size()) continue;
                if (l1_[l1_idx] == 0) {
                    // L0 stale — clear it and continue.
                    l0_[idx] &= ~(1ULL << group);
                    continue;
                }

                // Found a free page.
                uint32_t bit = std::countr_zero(l1_[l1_idx]);
                uint64_t lba = l1_idx * 64 + bit;
                if (lba >= total_pages_) return INVALID_LBA;

                // Mark used.
                l1_[l1_idx] &= ~(1ULL << bit);
                if (l1_[l1_idx] == 0)
                    l0_[idx] &= ~(1ULL << group);

                used_pages_++;
                l0_hint_ = idx;
                return lba;
            }
            return INVALID_LBA;
        }

        void free(uint64_t lba) {
            uint64_t l1_idx = lba / 64;
            uint32_t bit = lba % 64;
            l1_[l1_idx] |= (1ULL << bit);

            uint64_t l0_idx = l1_idx / 64;
            uint32_t l0_bit = l1_idx % 64;
            l0_[l0_idx] |= (1ULL << l0_bit);

            used_pages_--;
        }

        // Allocate n contiguous free pages. Returns starting LBA or INVALID_LBA.
        uint64_t allocate_contiguous(uint32_t n) {
            if (n == 0) return INVALID_LBA;
            if (n == 1) return allocate();
            if (used_pages_ + n > total_pages_) return INVALID_LBA;

            // Scan L1 for a run of n consecutive free bits.
            uint64_t run_start = 0;
            uint32_t run_len = 0;

            for (uint64_t lba = 0; lba < total_pages_; lba++) {
                uint64_t l1_idx = lba / 64;
                uint32_t bit = lba % 64;

                if (l1_[l1_idx] & (1ULL << bit)) {
                    // Free bit.
                    if (run_len == 0) run_start = lba;
                    run_len++;
                    if (run_len >= n) {
                        // Found a run. Mark all as used.
                        for (uint64_t i = run_start; i < run_start + n; i++) {
                            uint64_t wi = i / 64;
                            uint32_t bi = i % 64;
                            l1_[wi] &= ~(1ULL << bi);
                            if (l1_[wi] == 0) {
                                uint64_t l0i = wi / 64;
                                uint32_t l0b = wi % 64;
                                l0_[l0i] &= ~(1ULL << l0b);
                            }
                        }
                        used_pages_ += n;
                        return run_start;
                    }
                } else {
                    run_len = 0;
                }
            }
            return INVALID_LBA;
        }

        // Free n contiguous pages starting at lba.
        void free_contiguous(uint64_t lba, uint32_t n) {
            for (uint32_t i = 0; i < n; i++)
                free(lba + i);
        }

        bool full() const { return used_pages_ >= total_pages_; }
        uint64_t used() const { return used_pages_; }
        uint64_t total() const { return total_pages_; }
    };

} // namespace sider::nvme
