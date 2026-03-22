#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>

#include "pump/core/lock_free_queue.hh"

namespace sider::nvme {

    // ── lba_pool: central pool shared across all cores ──
    //
    // Manages bitmap words (each covering 64 NVMe pages).
    // Cores grab/return words via mpmc queue.
    // Initialized with all words free; cores take words on demand.

    struct lba_pool {
        static constexpr uint32_t INVALID_WORD = UINT32_MAX;

        pump::core::mpmc::queue<uint32_t> free_words;
        uint32_t total_words = 0;
        uint64_t total_pages = 0;
        uint64_t tail_mask = ~0ULL;  // mask for the last word (partial)

        lba_pool() = default;

        explicit lba_pool(uint64_t disk_pages)
            : free_words(next_pow2((disk_pages + 63) / 64 + 1))
            , total_words(static_cast<uint32_t>((disk_pages + 63) / 64))
            , total_pages(disk_pages) {

            // Compute tail mask: last word may have fewer than 64 valid pages.
            uint64_t tail = disk_pages % 64;
            tail_mask = (tail != 0) ? ((1ULL << tail) - 1) : ~0ULL;

            // Push all word indices into the pool.
            for (uint32_t i = 0; i < total_words; i++)
                free_words.try_enqueue(uint32_t(i));
        }

        // Bitmap value for a word freshly taken from the pool (all pages free).
        uint64_t fresh_bitmap(uint32_t word_idx) const {
            return (word_idx == total_words - 1) ? tail_mask : ~0ULL;
        }

    private:
        static size_t next_pow2(size_t n) {
            size_t p = 1;
            while (p < n) p <<= 1;
            return p;
        }
    };

    // ── nvme_allocator: per-core allocator with local word cache ──
    //
    // Each "word" is a uint64_t bitmap covering 64 NVMe pages.
    // Words are kept sorted by word_idx for O(log N) binary search on free().
    // Words are grabbed from lba_pool on demand and returned when fully free
    // and the local cache exceeds the high watermark.

    struct nvme_allocator {
        static constexpr uint64_t INVALID_LBA = UINT64_MAX;

        struct word_entry {
            uint32_t word_idx;    // global word index (LBA = word_idx * 64 + bit)
            uint64_t bitmap;      // 1 = free, 0 = used
        };

        lba_pool* pool_ = nullptr;
        std::vector<word_entry> words_;   // sorted by word_idx
        uint32_t active_ = 0;            // hint: last word with free bits
        uint64_t used_pages_ = 0;
        uint32_t low_mark_ = 0;          // don't shrink below this many words
        uint32_t high_mark_ = 0;         // return fully-free words above this

        nvme_allocator() = default;

        // For multicore: each core gets its own allocator sharing one pool.
        // low_mark: initial word count to pre-take (≈ per_core_memory / 4KB / 64).
        // high_mark: return threshold (≈ low_mark * 2).
        nvme_allocator(lba_pool* pool, uint32_t low_mark, uint32_t high_mark)
            : pool_(pool), low_mark_(low_mark), high_mark_(high_mark) {
            words_.reserve(high_mark);
            prefill(low_mark);
        }

        // Legacy single-core constructor: one allocator owns the entire disk.
        // Creates its own pool internally.
        nvme_allocator(uint64_t base_lba, uint64_t page_count)
            : pool_(new lba_pool(page_count))
            , low_mark_(static_cast<uint32_t>((page_count + 63) / 64))
            , high_mark_(low_mark_ + 1) {
            words_.reserve(low_mark_);
            prefill(low_mark_);
        }

        uint64_t allocate() {
            // Try active word first.
            if (active_ < words_.size() && words_[active_].bitmap != 0)
                return alloc_from(active_);

            // Scan for any word with free bits.
            for (uint32_t i = 0; i < words_.size(); i++) {
                if (words_[i].bitmap != 0) {
                    active_ = i;
                    return alloc_from(i);
                }
            }

            // All local words full — grab from pool.
            if (!grab_word()) return INVALID_LBA;
            active_ = static_cast<uint32_t>(words_.size() - 1);
            return alloc_from(active_);
        }

        void free(uint64_t lba) {
            uint32_t word_idx = static_cast<uint32_t>(lba / 64);
            uint32_t bit = static_cast<uint32_t>(lba % 64);

            // Binary search for word_idx in sorted words_.
            auto it = std::lower_bound(words_.begin(), words_.end(), word_idx,
                [](const word_entry& e, uint32_t idx) { return e.word_idx < idx; });

            it->bitmap |= (1ULL << bit);
            used_pages_--;

            // If word fully free and above high watermark → return to pool.
            if (it->bitmap == pool_->fresh_bitmap(word_idx) &&
                words_.size() > high_mark_) {
                pool_->free_words.try_enqueue(word_idx);
                uint32_t pos = static_cast<uint32_t>(it - words_.begin());
                words_.erase(it);
                if (active_ >= words_.size()) active_ = 0;
                else if (active_ > pos) active_--;
            }
        }

        // Allocate n contiguous pages. Fresh from pool's cursor only.
        uint64_t allocate_contiguous(uint32_t n) {
            if (n == 0) return INVALID_LBA;
            if (n == 1) return allocate();

            // Contiguous pages must come from consecutive word positions.
            // Try to grab enough consecutive words from pool.
            // Simple approach: grab n individual pages from fresh words,
            // checking if they happen to be contiguous.
            // For large values this is rare, so a simpler fallback:
            // scan existing words for a contiguous run.

            // First: try finding a run in already-held words.
            uint64_t result = find_contiguous_run(n);
            if (result != INVALID_LBA) return result;

            // Grab more words and retry.
            for (uint32_t i = 0; i < (n + 63) / 64 + 1; i++)
                if (!grab_word()) break;

            return find_contiguous_run(n);
        }

        void free_contiguous(uint64_t lba, uint32_t n) {
            for (uint32_t i = 0; i < n; i++)
                free(lba + i);
        }

        bool full() const {
            return pool_->free_words.empty() && all_local_full();
        }

        uint64_t used() const { return used_pages_; }

    private:
        uint64_t alloc_from(uint32_t idx) {
            auto& w = words_[idx];
            uint32_t bit = std::countr_zero(w.bitmap);
            w.bitmap &= ~(1ULL << bit);
            used_pages_++;
            return static_cast<uint64_t>(w.word_idx) * 64 + bit;
        }

        bool grab_word() {
            uint32_t word_idx;
            if (!pool_->free_words.try_dequeue(word_idx)) return false;

            word_entry entry{word_idx, pool_->fresh_bitmap(word_idx)};

            // Insert sorted.
            auto pos = std::lower_bound(words_.begin(), words_.end(), word_idx,
                [](const word_entry& e, uint32_t idx) { return e.word_idx < idx; });
            auto inserted = words_.insert(pos, entry);
            // Update active_ if insertion shifted it.
            uint32_t ins_pos = static_cast<uint32_t>(inserted - words_.begin());
            if (active_ >= ins_pos && active_ < words_.size() - 1) active_++;
            return true;
        }

        void prefill(uint32_t count) {
            for (uint32_t i = 0; i < count; i++)
                if (!grab_word()) break;
        }

        bool all_local_full() const {
            for (auto& w : words_)
                if (w.bitmap != 0) return false;
            return true;
        }

        // Scan local words for n contiguous free pages.
        uint64_t find_contiguous_run(uint32_t n) {
            uint64_t run_start = 0;
            uint32_t run_len = 0;

            for (auto& w : words_) {
                uint64_t bm = w.bitmap;
                uint64_t base = static_cast<uint64_t>(w.word_idx) * 64;

                for (uint32_t bit = 0; bit < 64 && bm; bit++) {
                    if (bm & (1ULL << bit)) {
                        uint64_t lba = base + bit;
                        if (run_len == 0 || lba != run_start + run_len) {
                            run_start = lba;
                            run_len = 1;
                        } else {
                            run_len++;
                        }
                        if (run_len >= n) {
                            // Mark all as used.
                            for (uint32_t i = 0; i < n; i++)
                                mark_used(run_start + i);
                            return run_start;
                        }
                    }
                }
            }
            return INVALID_LBA;
        }

        void mark_used(uint64_t lba) {
            uint32_t word_idx = static_cast<uint32_t>(lba / 64);
            uint32_t bit = static_cast<uint32_t>(lba % 64);
            auto it = std::lower_bound(words_.begin(), words_.end(), word_idx,
                [](const word_entry& e, uint32_t idx) { return e.word_idx < idx; });
            it->bitmap &= ~(1ULL << bit);
            used_pages_++;
        }
    };

} // namespace sider::nvme
