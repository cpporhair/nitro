#ifndef APPS_INCONEL_TREE_PAGE_BUILDER_HH
#define APPS_INCONEL_TREE_PAGE_BUILDER_HH

#include <cstring>
#include <string_view>
#include "../format/tree_page.hh"

namespace apps::inconel::tree {

    using namespace format;

    // ── Leaf Page Builder ──
    // Records must be added in sorted key order.

    struct leaf_page_builder {
        void* buf;
        uint32_t page_size;
        uint32_t offset;
        uint16_t count;

        void
        init(void* b, uint32_t ps) {
            buf = b;
            page_size = ps;
            offset = sizeof(tree_slot_header);
            count = 0;
            std::memset(buf, 0, page_size);
        }

        uint32_t
        value_record_size(uint16_t key_len) const {
            return sizeof(leaf_record_header) + key_len + sizeof(value_ref);
        }

        uint32_t
        tombstone_record_size(uint16_t key_len) const {
            return sizeof(leaf_record_header) + key_len;
        }

        uint32_t
        free_space() const {
            return page_size - offset;
        }

        bool
        add_value(std::string_view key, uint64_t data_ver, const value_ref& vr) {
            auto key_len = static_cast<uint16_t>(key.size());
            uint32_t needed = value_record_size(key_len);
            if (needed > free_space()) return false;

            auto* p = static_cast<char*>(buf) + offset;
            leaf_record_header hdr { .data_ver = data_ver, .kind = record_kind::value, .key_len = key_len };
            std::memcpy(p, &hdr, sizeof(hdr));
            p += sizeof(hdr);
            std::memcpy(p, key.data(), key_len);
            p += key_len;
            std::memcpy(p, &vr, sizeof(vr));

            offset += needed;
            ++count;
            return true;
        }

        bool
        add_tombstone(std::string_view key, uint64_t data_ver) {
            auto key_len = static_cast<uint16_t>(key.size());
            uint32_t needed = tombstone_record_size(key_len);
            if (needed > free_space()) return false;

            auto* p = static_cast<char*>(buf) + offset;
            leaf_record_header hdr { .data_ver = data_ver, .kind = record_kind::tombstone, .key_len = key_len };
            std::memcpy(p, &hdr, sizeof(hdr));
            p += sizeof(hdr);
            std::memcpy(p, key.data(), key_len);

            offset += needed;
            ++count;
            return true;
        }

        void
        finalize(uint32_t format_version = 1) {
            auto* hdr = static_cast<tree_slot_header*>(buf);
            hdr->magic = TREE_PAGE_MAGIC;
            hdr->format_version = format_version;
            hdr->type = node_type::leaf;
            hdr->record_count = count;
            hdr->free_space_offset = offset;
            hdr->page_crc = tree_page_compute_crc(buf, page_size);
        }
    };

    // ── Internal Page Builder ──
    // Records must be added in sorted separator key order.
    // After all separators, call set_rightmost_child().

    struct internal_page_builder {
        void* buf;
        uint32_t page_size;
        uint32_t offset;
        uint16_t count;

        void
        init(void* b, uint32_t ps) {
            buf = b;
            page_size = ps;
            offset = sizeof(tree_slot_header);
            count = 0;
            std::memset(buf, 0, page_size);
        }

        uint32_t
        record_size(uint16_t key_len) const {
            return internal_record_size(key_len);
        }

        uint32_t
        free_space() const {
            return page_size - offset;
        }

        bool
        add_child(std::string_view separator_key, paddr child_base) {
            auto key_len = static_cast<uint16_t>(separator_key.size());
            uint32_t needed = record_size(key_len);
            if (needed + sizeof(paddr) > free_space()) return false;  // reserve room for rightmost

            auto* rec = reinterpret_cast<internal_record*>(static_cast<char*>(buf) + offset);
            rec->key_len = key_len;
            std::memcpy(internal_record_key(rec), separator_key.data(), key_len);
            std::memcpy(internal_record_child_base(rec), &child_base, sizeof(child_base));

            offset += needed;
            ++count;
            return true;
        }

        void
        set_rightmost_child(paddr child_base) {
            auto* p = static_cast<char*>(buf) + offset;
            std::memcpy(p, &child_base, sizeof(child_base));
            offset += sizeof(paddr);
        }

        void
        finalize(uint32_t format_version = 1) {
            auto* hdr = static_cast<tree_slot_header*>(buf);
            hdr->magic = TREE_PAGE_MAGIC;
            hdr->format_version = format_version;
            hdr->type = node_type::internal;
            hdr->record_count = count;
            hdr->free_space_offset = offset;
            hdr->page_crc = tree_page_compute_crc(buf, page_size);
        }
    };

}

#endif //APPS_INCONEL_TREE_PAGE_BUILDER_HH
