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
                    partials.pop_back();
                    continue;
                }

                uint8_t idx = static_cast<uint8_t>(std::countr_zero(free_bits));
                pe.slot_bitmap |= (1ULL << idx);
                pe.live_count++;

                // Remove from partials if now full.
                if ((pe.slot_bitmap & fmask) == fmask)
                    partials.pop_back();

                return {pid, idx, pe.mem_ptr + slot_offset(sc, idx)};
            }

            // Allocate a new page.
            uint32_t pid = pt_.alloc_page_id();
            auto& pe = pt_[pid];
            pe.state    = page_entry::IN_MEMORY;
            pe.size_class = sc;
            pe.live_count = 1;
            pe.hotness  = 0;
            pe.mem_ptr  = alloc_page();
            pe.slot_bitmap = 1ULL;   // slot 0 occupied
            total_pages_++;

            // Multi-slot page goes to partial list (still has free slots).
            if (slots_per_page[sc] > 1)
                partials.push_back(pid);

            return {pid, 0, pe.mem_ptr};
        }

        void free_slot(uint32_t page_id, uint8_t slot_index) {
            auto& pe = pt_[page_id];
            auto sc = static_cast<size_class_t>(pe.size_class);
            uint64_t fmask = full_mask_for(sc);

            bool was_full = (pe.slot_bitmap & fmask) == fmask;

            pe.slot_bitmap &= ~(1ULL << slot_index);
            pe.live_count--;

            if (pe.live_count == 0) {
                // Page empty — reclaim.
                free_page(pe.mem_ptr);
                total_pages_--;

                // Remove from partial list if it was there (wasn't full before).
                if (!was_full)
                    remove_from_partials(sc, page_id);

                pt_.free_page_id(page_id);
            } else if (was_full) {
                // Was full, now has space — add to partials.
                partial_pages_[sc].push_back(page_id);
            }
        }

        char* slot_ptr(uint32_t page_id, uint8_t slot_index) const {
            auto& pe = pt_[page_id];
            auto sc = static_cast<size_class_t>(pe.size_class);
            return pe.mem_ptr + slot_offset(sc, slot_index);
        }

        uint64_t memory_used_bytes() const {
            return static_cast<uint64_t>(total_pages_) * PAGE_SIZE;
        }

    private:
        void remove_from_partials(size_class_t sc, uint32_t page_id) {
            auto& v = partial_pages_[sc];
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == page_id) {
                    v[i] = v.back();
                    v.pop_back();
                    return;
                }
            }
        }
    };

} // namespace sider::store
