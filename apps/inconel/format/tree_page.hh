#ifndef APPS_INCONEL_FORMAT_TREE_PAGE_HH
#define APPS_INCONEL_FORMAT_TREE_PAGE_HH

#include <cstdint>
#include <cstring>

#include <absl/crc/crc32c.h>
#include <absl/strings/string_view.h>

#include "./crc32c.hh"
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

    // ── Slot directory helpers ──
    //
    // ODF §4.1: each tree page has the layout
    //
    //     [ tree_slot_header ]                          ← 19 bytes
    //     [ uint16_t offsets[record_count] ]            ← slot directory
    //     [ records payload ... ]
    //
    // The directory is a full per-record offset table sitting immediately
    // after the header. `offsets[i]` is the in-page byte offset of the i-th
    // record in logical key order — readers index slot `i` to land directly
    // on a record without scanning every preceding one.
    //
    // The directory size is uniquely determined by `record_count`; it is
    // not stored as a separate header field. These helpers concentrate the
    // layout arithmetic so builders / readers never reach into the page by
    // hand-rolled byte math.
    //
    // ── Why memcpy and not typed `uint16_t*` ──
    //
    // `tree_slot_header` is 19 bytes packed, so the directory starts at a
    // page-internal offset that is **not** 2-byte aligned. Casting that
    // address to `uint16_t*` and dereferencing it would be UB even though
    // x86 happens to tolerate the misaligned access. We therefore expose
    // only `load_tree_slot_offset` / `store_tree_slot_offset`, which copy
    // the two bytes via `std::memcpy` (the standard sanctioned escape
    // hatch for unaligned reads/writes). Modern compilers fold the
    // 2-byte memcpy into a single mov, so there is no perf cost.
    //
    // For internal pages, `rightmost_child_base` (a trailing `paddr`) sits
    // after the last separator record in the payload area and is *not*
    // covered by the directory — it is reached via
    // `tree_slot_header::free_space_offset - sizeof(paddr)`.

    inline uint32_t
    tree_slot_directory_bytes(uint16_t record_count) {
        return static_cast<uint32_t>(sizeof(uint16_t)) * record_count;
    }

    inline uint16_t
    load_tree_slot_offset(const tree_slot_header* hdr, uint16_t index) {
        uint16_t value;
        std::memcpy(&value,
                    reinterpret_cast<const char*>(hdr) + sizeof(tree_slot_header)
                        + static_cast<size_t>(index) * sizeof(uint16_t),
                    sizeof(uint16_t));
        return value;
    }

    inline void
    store_tree_slot_offset(tree_slot_header* hdr, uint16_t index, uint16_t value) {
        std::memcpy(reinterpret_cast<char*>(hdr) + sizeof(tree_slot_header)
                        + static_cast<size_t>(index) * sizeof(uint16_t),
                    &value,
                    sizeof(uint16_t));
    }

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
        return crc32c_skip(page, page_size, crc_field_offset, crc_field_size);
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
