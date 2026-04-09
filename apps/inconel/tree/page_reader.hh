#ifndef APPS_INCONEL_TREE_PAGE_READER_HH
#define APPS_INCONEL_TREE_PAGE_READER_HH

#include <cstring>
#include <optional>
#include <string_view>
#include "../format/tree_page.hh"

namespace apps::inconel::tree {

    using namespace format;

    // Slot directory readers ── shared layout note ──
    //
    // ODF §4.1: every tree page is laid out as
    //
    //     [ tree_slot_header ]                          ← 19 bytes
    //     [ uint16_t offsets[record_count] ]            ← directory
    //     [ records payload ... ]
    //
    // The directory holds in-page byte offsets in logical key order, so
    // both leaf and internal readers index slot `i` to land directly on a
    // record. find / lower_bound / find_child collapse to standard
    // binary search over the directory rather than the previous O(N)
    // walk through every preceding record.
    //
    // Reads of `offsets[i]` go through `load_tree_slot_offset(hdr, i)`
    // (memcpy) because the directory starts at the unaligned offset 19;
    // see the alignment note in format/tree_page.hh.

    // ── Leaf Page Reader ──

    struct leaf_record {
        std::string_view key;
        uint64_t data_ver;
        record_kind kind;
        value_ref vr;       // valid only when kind == value
    };

    struct leaf_page_reader {
        const char* buf;
        uint32_t page_size;
        const tree_slot_header* hdr;

        bool
        parse(const void* b, uint32_t ps) {
            buf = static_cast<const char*>(b);
            page_size = ps;
            hdr = static_cast<const tree_slot_header*>(b);
            return tree_page_validate(b, ps) && hdr->type == node_type::leaf;
        }

        uint16_t
        record_count() const { return hdr->record_count; }

        leaf_record
        get(uint16_t index) const {
            return read_leaf_record(buf + load_tree_slot_offset(hdr, index));
        }

        std::optional<leaf_record>
        find(std::string_view key) const {
            uint16_t idx = lower_bound(key);
            if (idx >= hdr->record_count) return std::nullopt;
            auto rec = get(idx);
            if (rec.key == key) return rec;
            return std::nullopt;
        }

        // Returns index of first record with key >= target.
        uint16_t
        lower_bound(std::string_view target) const {
            uint16_t lo = 0;
            uint16_t hi = hdr->record_count;
            while (lo < hi) {
                uint16_t mid = lo + static_cast<uint16_t>((hi - lo) / 2);
                if (read_leaf_key(buf + load_tree_slot_offset(hdr, mid)) < target) {
                    lo = static_cast<uint16_t>(mid + 1);
                } else {
                    hi = mid;
                }
            }
            return lo;
        }

    private:
        static leaf_record
        read_leaf_record(const char* p) {
            leaf_record_header rh;
            std::memcpy(&rh, p, sizeof(rh));
            p += sizeof(rh);

            leaf_record rec;
            rec.key = std::string_view(p, rh.key_len);
            rec.data_ver = rh.data_ver;
            rec.kind = rh.kind;
            rec.vr = {};
            p += rh.key_len;

            if (rh.kind == record_kind::value)
                std::memcpy(&rec.vr, p, sizeof(value_ref));

            return rec;
        }

        // Light-weight peek used by binary search: avoid copying the
        // value_ref / kind when only the key bytes matter for comparison.
        static std::string_view
        read_leaf_key(const char* p) {
            leaf_record_header rh;
            std::memcpy(&rh, p, sizeof(rh));
            return std::string_view(p + sizeof(rh), rh.key_len);
        }
    };

    // ── Internal Page Reader ──

    struct internal_entry {
        std::string_view separator_key;
        paddr child_base;
    };

    struct internal_page_reader {
        const char* buf;
        uint32_t page_size;
        const tree_slot_header* hdr;

        bool
        parse(const void* b, uint32_t ps) {
            buf = static_cast<const char*>(b);
            page_size = ps;
            hdr = static_cast<const tree_slot_header*>(b);
            return tree_page_validate(b, ps) && hdr->type == node_type::internal;
        }

        uint16_t
        record_count() const { return hdr->record_count; }

        internal_entry
        get(uint16_t index) const {
            return read_internal_record(buf + load_tree_slot_offset(hdr, index));
        }

        // The rightmost child base sits at the tail of the payload, just
        // before `free_space_offset`. ODF §4.1 keeps it out of the slot
        // directory because it is not addressed by a separator key — so
        // there is exactly one fixed location to look at, O(1).
        paddr
        rightmost_child() const {
            paddr result;
            std::memcpy(&result, buf + hdr->free_space_offset - sizeof(paddr), sizeof(result));
            return result;
        }

        // Find child base for lookup_key:
        // first separator > lookup_key → that child; otherwise rightmost.
        // Binary search the directory for the first index whose separator
        // key is strictly greater than `lookup_key` (upper_bound semantics).
        paddr
        find_child(std::string_view lookup_key) const {
            uint16_t lo = 0;
            uint16_t hi = hdr->record_count;
            while (lo < hi) {
                uint16_t mid = lo + static_cast<uint16_t>((hi - lo) / 2);
                if (read_separator_key(buf + load_tree_slot_offset(hdr, mid)) > lookup_key) {
                    hi = mid;
                } else {
                    lo = static_cast<uint16_t>(mid + 1);
                }
            }
            if (lo < hdr->record_count) {
                return read_internal_record(buf + load_tree_slot_offset(hdr, lo)).child_base;
            }
            return rightmost_child();
        }

    private:
        static internal_entry
        read_internal_record(const char* p) {
            auto* rec = reinterpret_cast<const internal_record*>(p);
            internal_entry e;
            e.separator_key = std::string_view(internal_record_key(rec), rec->key_len);
            std::memcpy(&e.child_base, internal_record_child_base(rec), sizeof(paddr));
            return e;
        }

        // Light-weight peek for binary search comparisons — skip the
        // child_base copy until we know which separator we want.
        static std::string_view
        read_separator_key(const char* p) {
            auto* rec = reinterpret_cast<const internal_record*>(p);
            return std::string_view(internal_record_key(rec), rec->key_len);
        }
    };

}

#endif //APPS_INCONEL_TREE_PAGE_READER_HH
