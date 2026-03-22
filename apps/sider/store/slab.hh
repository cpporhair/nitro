#pragma once

#include <cstdint>
#include <bit>
#include <vector>

#include "store/types.hh"
#include "store/page_table.hh"

namespace sider::store {

    struct alloc_result {
        uint32_t page_id;
        uint8_t  slot_index;
        char*    slot_ptr;    // direct pointer to write value into
    };

    struct slab_allocator {
        page_table& pt_;
        std::vector<uint32_t> partial_pages_[SC_COUNT];   // pages with free slots
        uint32_t total_pages_ = 0;

        explicit slab_allocator(page_table& pt) : pt_(pt) {}

        alloc_result allocate(size_class_t sc) {
            auto& partials = partial_pages_[sc];
            uint64_t fmask = full_mask_for(sc);

            // Try existing partial page.
            while (!partials.empty()) {
                uint32_t pid = partials.back();
                auto& pe = pt_[pid];

                uint64_t free_bits = ~pe.slot_bitmap & fmask;
                if (free_bits == 0) {
                    pe.partial_index = page_entry::NO_PARTIAL;
                    partials.pop_back();
                    continue;
                }

                uint8_t idx = static_cast<uint8_t>(std::countr_zero(free_bits));
                pe.slot_bitmap |= (1ULL << idx);
                pe.live_count++;

                // Remove from partials if now full.
                if ((pe.slot_bitmap & fmask) == fmask) {
                    pe.partial_index = page_entry::NO_PARTIAL;
                    partials.pop_back();
                }

                return {pid, idx, pe.mem_ptr + slot_offset(sc, idx)};
            }

            // Allocate a new page.
            auto* mem = alloc_page();
            if (!mem) [[unlikely]]
                return {UINT32_MAX, 0, nullptr};  // DMA alloc failed

            uint32_t pid = pt_.alloc_page_id();
            auto& pe = pt_[pid];
            pe.state    = page_entry::IN_MEMORY;
            pe.size_class = sc;
            pe.live_count = 1;
            pe.hotness  = 0;
            pe.mem_ptr  = mem;
            pe.slot_bitmap = 1ULL;   // slot 0 occupied
            pe.slot_key_hashes = new uint32_t[slots_per_page[sc]]{};
            total_pages_++;

            // Multi-slot page goes to partial list (still has free slots).
            if (slots_per_page[sc] > 1) {
                pe.partial_index = static_cast<uint32_t>(partials.size());
                partials.push_back(pid);
            }

            return {pid, 0, mem};
        }

        void free_slot(uint32_t page_id, uint8_t slot_index) {
            auto& pe = pt_[page_id];
            auto sc = static_cast<size_class_t>(pe.size_class);
            uint64_t fmask = full_mask_for(sc);

            bool was_full = (pe.slot_bitmap & fmask) == fmask;

            pe.slot_bitmap &= ~(1ULL << slot_index);
            pe.live_count--;

            if (pe.live_count == 0) {
                if (pe.state == page_entry::EVICTING) {
                    // DMA in progress — can't free memory yet.
                    if (!was_full)
                        remove_from_partials(sc, page_id);
                    return;
                }

                // Page empty — reclaim.
                free_page(pe.mem_ptr);
                total_pages_--;

                if (!was_full)
                    remove_from_partials(sc, page_id);

                pt_.free_page_id(page_id);
            } else if (was_full) {
                // Was full, now has space — add to partials.
                // But NOT for EVICTING pages (no new allocations on them).
                if (pe.state != page_entry::EVICTING) {
                    pe.partial_index = static_cast<uint32_t>(partial_pages_[sc].size());
                    partial_pages_[sc].push_back(page_id);
                }
            }
        }

        char* slot_ptr(uint32_t page_id, uint8_t slot_index) const {
            auto& pe = pt_[page_id];
            auto sc = static_cast<size_class_t>(pe.size_class);
            return pe.mem_ptr + slot_offset(sc, slot_index);
        }

        // Force-free an entire page during eviction.
        // All hash table entries on this page must already be erased.
        void evict_page(uint32_t page_id) {
            auto& pe = pt_[page_id];
            auto sc = static_cast<size_class_t>(pe.size_class);
            if (pe.partial_index != page_entry::NO_PARTIAL)
                remove_from_partials(sc, page_id);
            free_page(pe.mem_ptr);
            total_pages_--;
            pt_.free_page_id(page_id);
        }

        void remove_from_partials_public(uint8_t sc, uint32_t page_id) {
            remove_from_partials(static_cast<size_class_t>(sc), page_id);
        }

        uint64_t memory_used_bytes() const {
            return static_cast<uint64_t>(total_pages_) * PAGE_SIZE;
        }

        // Free an entire page's memory without touching bitmap/live_count.
        // Used by eviction completion callback.
        void release_page_memory(uint32_t page_id) {
            auto& pe = pt_[page_id];
            auto sc = static_cast<size_class_t>(pe.size_class);
            remove_from_partials(sc, page_id);
            if (pe.mem_ptr) {
                free_page(pe.mem_ptr);
                pe.mem_ptr = nullptr;
            }
            total_pages_--;
        }

    private:
        // O(1) removal via partial_index tracking.
        void remove_from_partials(size_class_t sc, uint32_t page_id) {
            auto& pe = pt_[page_id];
            if (pe.partial_index == page_entry::NO_PARTIAL) return;

            auto& v = partial_pages_[sc];
            uint32_t idx = pe.partial_index;
            if (idx < v.size()) {
                uint32_t back_pid = v.back();
                v[idx] = back_pid;
                pt_[back_pid].partial_index = idx;
                v.pop_back();
            }
            pe.partial_index = page_entry::NO_PARTIAL;
        }
    };

} // namespace sider::store
