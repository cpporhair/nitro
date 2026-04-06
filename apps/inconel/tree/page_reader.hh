#ifndef APPS_INCONEL_TREE_PAGE_READER_HH
#define APPS_INCONEL_TREE_PAGE_READER_HH

#include <cstring>
#include <optional>
#include <string_view>
#include "../format/tree_page.hh"

namespace apps::inconel::tree {

    using namespace format;

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
            const char* p = buf + sizeof(tree_slot_header);
            for (uint16_t i = 0; i < index; ++i)
                p = skip_leaf_record(p);
            return read_leaf_record(p);
        }

        std::optional<leaf_record>
        find(std::string_view key) const {
            const char* p = buf + sizeof(tree_slot_header);
            for (uint16_t i = 0; i < hdr->record_count; ++i) {
                auto rec = read_leaf_record(p);
                int cmp = rec.key.compare(key);
                if (cmp == 0) return rec;
                if (cmp > 0) return std::nullopt;
                p = skip_leaf_record(p);
            }
            return std::nullopt;
        }

        // Returns index of first record with key >= target.
        uint16_t
        lower_bound(std::string_view target) const {
            const char* p = buf + sizeof(tree_slot_header);
            for (uint16_t i = 0; i < hdr->record_count; ++i) {
                auto rec = read_leaf_record(p);
                if (rec.key >= target) return i;
                p = skip_leaf_record(p);
            }
            return hdr->record_count;
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

        static const char*
        skip_leaf_record(const char* p) {
            leaf_record_header rh;
            std::memcpy(&rh, p, sizeof(rh));
            p += sizeof(rh) + rh.key_len;
            if (rh.kind == record_kind::value)
                p += sizeof(value_ref);
            return p;
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
            const char* p = buf + sizeof(tree_slot_header);
            for (uint16_t i = 0; i < index; ++i)
                p = skip_internal_record(p);
            return read_internal_record(p);
        }

        paddr
        rightmost_child() const {
            const char* p = buf + sizeof(tree_slot_header);
            for (uint16_t i = 0; i < hdr->record_count; ++i)
                p = skip_internal_record(p);
            paddr result;
            std::memcpy(&result, p, sizeof(result));
            return result;
        }

        // Find child base for lookup_key:
        // first separator > lookup_key → that child; otherwise rightmost.
        paddr
        find_child(std::string_view lookup_key) const {
            const char* p = buf + sizeof(tree_slot_header);
            for (uint16_t i = 0; i < hdr->record_count; ++i) {
                auto entry = read_internal_record(p);
                if (entry.separator_key > lookup_key)
                    return entry.child_base;
                p = skip_internal_record(p);
            }
            paddr result;
            std::memcpy(&result, p, sizeof(result));
            return result;
        }

    private:
        static internal_entry
        read_internal_record(const char* p) {
            uint16_t key_len;
            std::memcpy(&key_len, p, sizeof(key_len));
            p += sizeof(key_len);

            internal_entry e;
            e.separator_key = std::string_view(p, key_len);
            p += key_len;
            std::memcpy(&e.child_base, p, sizeof(paddr));
            return e;
        }

        static const char*
        skip_internal_record(const char* p) {
            uint16_t key_len;
            std::memcpy(&key_len, p, sizeof(key_len));
            return p + sizeof(uint16_t) + key_len + sizeof(paddr);
        }
    };

}

#endif //APPS_INCONEL_TREE_PAGE_READER_HH
