#ifndef APPS_INCONEL_FORMAT_TREE_PAGE_HH
#define APPS_INCONEL_FORMAT_TREE_PAGE_HH

#include <cstdint>
#include "./types.hh"
#include "./crc.hh"

namespace apps::inconel::format {

    constexpr uint32_t TREE_PAGE_MAGIC = 0x54524545;  // "TREE"

    enum class node_type : uint8_t {
        internal = 1,
        leaf     = 2,
    };

    enum class record_kind : uint8_t {
        value     = 1,
        tombstone = 2,
    };

    struct __attribute__((packed)) tree_slot_header {
        uint32_t  magic;
        uint32_t  format_version;
        node_type type;
        uint16_t  record_count;
        uint32_t  free_space_offset;
        uint32_t  page_crc;
    };
    static_assert(sizeof(tree_slot_header) == 19);

    struct __attribute__((packed)) leaf_record_header {
        uint64_t    data_ver;
        record_kind kind;
        uint16_t    key_len;
    };
    static_assert(sizeof(leaf_record_header) == 11);

    // ── shadow range address helpers ──

    inline uint64_t
    range_size_lbas(uint32_t tree_page_size, uint32_t shadow_slots_per_range, uint32_t lba_size) {
        return static_cast<uint64_t>(tree_page_size / lba_size) * shadow_slots_per_range;
    }

    inline paddr
    slot_paddr(paddr range_base, uint32_t slot_index, uint32_t tree_page_size, uint32_t lba_size) {
        return { range_base.device_id,
                 range_base.lba + static_cast<uint64_t>(slot_index) * (tree_page_size / lba_size) };
    }

    // ── tree page CRC ──
    // covers entire page except the page_crc field itself (4 bytes at offset 15)

    inline uint32_t
    tree_page_compute_crc(const void* page, uint32_t page_size) {
        constexpr uint32_t crc_field_offset = offsetof(tree_slot_header, page_crc);
        constexpr uint32_t crc_field_size   = sizeof(uint32_t);
        auto crc = crc32c(page, crc_field_offset);
        crc = crc32c(static_cast<const char*>(page) + crc_field_offset + crc_field_size,
                      page_size - crc_field_offset - crc_field_size, crc);
        return crc;
    }

    inline bool
    tree_page_validate(const void* page, uint32_t page_size) {
        auto* hdr = static_cast<const tree_slot_header*>(page);
        if (hdr->magic != TREE_PAGE_MAGIC) return false;
        return hdr->page_crc == tree_page_compute_crc(page, page_size);
    }

}

#endif //APPS_INCONEL_FORMAT_TREE_PAGE_HH
