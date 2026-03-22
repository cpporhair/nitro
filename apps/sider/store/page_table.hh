#pragma once

#include <cstdint>
#include <vector>

namespace sider::store {

    struct page_entry {
        enum state_t : uint8_t { FREE = 0, IN_MEMORY, EVICTING, ON_NVME };
        static constexpr uint32_t NO_PARTIAL = UINT32_MAX;

        state_t   state = FREE;
        uint8_t   size_class = 0;
        uint16_t  live_count = 0;
        uint32_t  page_count = 1;    // number of 4KB pages (>1 for large values)
        uint64_t  hotness = 0;       // LRU clock (uint64 to avoid wraparound)
        char*     mem_ptr = nullptr;
        uint64_t  slot_bitmap = 0;   // 1 = occupied, 0 = free
        uint64_t  nvme_lba = 0;      // valid when ON_NVME (page position on disk)
        uint8_t   disk_id = 0;       // which NVMe disk (valid when ON_NVME)
        uint32_t  partial_index = NO_PARTIAL;  // index in slab partial_pages_ vector
        uint32_t* slot_key_hashes = nullptr;   // reverse index: key_hash per slot for O(1) discard
    };

    struct page_table {
        std::vector<page_entry> pages_;
        std::vector<uint32_t>   free_ids_;

        uint32_t alloc_page_id() {
            if (!free_ids_.empty()) {
                auto id = free_ids_.back();
                free_ids_.pop_back();
                return id;
            }
            uint32_t id = static_cast<uint32_t>(pages_.size());
            pages_.emplace_back();
            return id;
        }

        void free_page_id(uint32_t id) {
            delete[] pages_[id].slot_key_hashes;
            pages_[id] = page_entry{};
            free_ids_.push_back(id);
        }

        page_entry& operator[](uint32_t id) { return pages_[id]; }
        const page_entry& operator[](uint32_t id) const { return pages_[id]; }

        uint32_t size() const { return static_cast<uint32_t>(pages_.size()); }
    };

} // namespace sider::store
