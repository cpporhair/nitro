#ifndef APPS_INCONEL_FORMAT_TREE_PAGE_HH
#define APPS_INCONEL_FORMAT_TREE_PAGE_HH

#include <cstdint>
#include <cstring>

#include <absl/crc/crc32c.h>
#include <absl/strings/string_view.h>

#include "./types.hh"

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

    // ── Internal node record (variable-length) ──
    //
    // ODF §4.2: each record is [ key_len(uint16_t) | key_bytes(key_len) |
    // child_base(paddr) ]. Only the fixed prefix has a POD; key bytes and
    // child_base are reached via the inline helpers below so the layout
    // arithmetic stays in one place rather than smeared across the tree
    // builder/reader.

    struct __attribute__((packed)) internal_record {
        uint16_t key_len;
    };
    static_assert(sizeof(internal_record) == 2);

    inline uint32_t
    internal_record_size(uint16_t key_len) {
        return sizeof(internal_record) + key_len + sizeof(paddr);
    }

    inline const char*
    internal_record_key(const internal_record* rec) {
        return reinterpret_cast<const char*>(rec) + sizeof(internal_record);
    }

    inline char*
    internal_record_key(internal_record* rec) {
        return reinterpret_cast<char*>(rec) + sizeof(internal_record);
    }

    inline const paddr*
    internal_record_child_base(const internal_record* rec) {
        return reinterpret_cast<const paddr*>(
            reinterpret_cast<const char*>(rec) + sizeof(internal_record) + rec->key_len);
    }

    inline paddr*
    internal_record_child_base(internal_record* rec) {
        return reinterpret_cast<paddr*>(
            reinterpret_cast<char*>(rec) + sizeof(internal_record) + rec->key_len);
    }

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
        auto crc = absl::ComputeCrc32c(
            absl::string_view(static_cast<const char*>(page), crc_field_offset));
        crc = absl::ExtendCrc32c(
            crc,
            absl::string_view(static_cast<const char*>(page) + crc_field_offset + crc_field_size,
                              page_size - crc_field_offset - crc_field_size));
        return static_cast<uint32_t>(crc);
    }

    // ── tree page status ──
    //
    // Reason-aware page validation. The scheduler needs to know *why* a
    // page failed (zero / bad magic / bad crc) so it can panic with a
    // diagnostic that points at the actual on-disk corruption rather than
    // collapse every failure into a single bool.

    enum class tree_page_status : uint8_t {
        ok = 0,
        zero_page,
        bad_magic,
        bad_crc,
    };

    inline tree_page_status
    inspect_tree_page(const void* page, uint32_t page_size) {
        // A freshly TRIM'd / never-written page reads back as zeros (or at
        // least its first four bytes do). Distinguish that from a stale
        // page whose magic happens to be wrong: zero_page means "no page
        // here" while bad_magic means "something corrupted the header".
        uint32_t first4 = 0;
        std::memcpy(&first4, page, sizeof(first4));
        if (first4 == 0) return tree_page_status::zero_page;

        auto* hdr = static_cast<const tree_slot_header*>(page);
        if (hdr->magic != TREE_PAGE_MAGIC) return tree_page_status::bad_magic;
        if (hdr->page_crc != tree_page_compute_crc(page, page_size))
            return tree_page_status::bad_crc;
        return tree_page_status::ok;
    }

    inline const char*
    tree_page_status_to_string(tree_page_status s) {
        switch (s) {
        case tree_page_status::ok:        return "ok";
        case tree_page_status::zero_page: return "zero_page";
        case tree_page_status::bad_magic: return "bad_magic";
        case tree_page_status::bad_crc:   return "bad_crc";
        }
        return "unknown";
    }

    inline bool
    tree_page_validate(const void* page, uint32_t page_size) {
        return inspect_tree_page(page, page_size) == tree_page_status::ok;
    }

}

#endif //APPS_INCONEL_FORMAT_TREE_PAGE_HH
