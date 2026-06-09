#ifndef APPS_INCONEL_TREE_PAGE_BUILDER_HH
#define APPS_INCONEL_TREE_PAGE_BUILDER_HH

#include <cstring>
#include <string_view>

#include <absl/container/inlined_vector.h>

#include "../core/panic.hh"
#include "../format/tree_page.hh"

namespace apps::inconel::tree {

    using namespace format;

    // Slot directory builders ── shared layout note ──
    //
    // ODF §4.1: every tree page is laid out as
    //
    //     [ tree_slot_header ]
    //     [ uint16_t offsets[record_count] ]            ← directory
    //     [ records payload ... ]
    //
    // Each builder still exposes "append records in sorted key order" as
    // its outer API, but internally it appends payload to a *no-directory*
    // layout (records start right after the header) and captures each
    // record's pre-shift offset in `record_offsets_`. `finalize()` then
    // memmoves the entire payload right by `dir_bytes` and writes the
    // captured offsets shifted by `dir_bytes` into the directory slot.
    //
    // Why finalize-time materialization (plan 014 D4):
    //   - This is the cold write side, not the lookup hot path.
    //   - One memmove per page is auditable; we don't have to thread
    //     "future directory size" arithmetic through every add_*() call.
    //   - We don't need a second payload buffer — only one extra owner-
    //     local offset scratch per builder.
    //
    // Free-space accounting still has to reserve room for the directory:
    // each `add_*` budgets `record_size + sizeof(uint16_t)` against the
    // remaining page bytes so the post-finalize page never overflows.

    // ── Leaf Page Builder ──
    // Records must be added in sorted key order.

    struct leaf_page_builder {
        void* buf;
        uint32_t page_size;
        uint32_t offset;  // next no-directory payload write position
        uint16_t count;
        absl::InlinedVector<uint16_t, 384> record_offsets_;

        void
        init(void* b, uint32_t ps) {
            buf = b;
            page_size = ps;
            offset = sizeof(tree_slot_header);
            count = 0;
            record_offsets_.clear();
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

    private:
        // Bytes still available for one more record + its directory entry.
        // Equals the post-finalize free space if `record_size` were added
        // next: `page_size - (header + dir_so_far + new_dir_entry + payload + record_size) >= 0`.
        bool
        can_fit(uint32_t record_size) const {
            const uint32_t dir_after = sizeof(uint16_t) * (count + 1u);
            // header sits at [0, sizeof(tree_slot_header)); payload starts
            // at `offset` (already past the header). Final layout will
            // need `dir_after` bytes inserted between header and payload,
            // plus this new record at the end.
            return offset + record_size + dir_after <= page_size;
        }

    public:
        template <typename CopyKey>
        bool
        add_value_with_key_copy(uint16_t key_len,
                                uint64_t data_ver,
                                const value_ref& vr,
                                CopyKey&& copy_key) {
            uint32_t needed = value_record_size(key_len);
            if (!can_fit(needed)) return false;

            record_offsets_.push_back(static_cast<uint16_t>(offset));

            auto* p = static_cast<char*>(buf) + offset;
            leaf_record_header hdr { .data_ver = data_ver, .kind = record_kind::value, .key_len = key_len };
            std::memcpy(p, &hdr, sizeof(hdr));
            p += sizeof(hdr);
            copy_key(p);
            p += key_len;
            std::memcpy(p, &vr, sizeof(vr));

            offset += needed;
            ++count;
            return true;
        }

        bool
        add_value(std::string_view key, uint64_t data_ver, const value_ref& vr) {
            auto key_len = static_cast<uint16_t>(key.size());
            return add_value_with_key_copy(
                key_len, data_ver, vr,
                [&](char* dst) {
                    std::memcpy(dst, key.data(), key_len);
                });
        }

        template <typename CopyKey>
        bool
        add_tombstone_with_key_copy(uint16_t key_len,
                                    uint64_t data_ver,
                                    CopyKey&& copy_key) {
            uint32_t needed = tombstone_record_size(key_len);
            if (!can_fit(needed)) return false;

            record_offsets_.push_back(static_cast<uint16_t>(offset));

            auto* p = static_cast<char*>(buf) + offset;
            leaf_record_header hdr { .data_ver = data_ver, .kind = record_kind::tombstone, .key_len = key_len };
            std::memcpy(p, &hdr, sizeof(hdr));
            p += sizeof(hdr);
            copy_key(p);

            offset += needed;
            ++count;
            return true;
        }

        bool
        add_tombstone(std::string_view key, uint64_t data_ver) {
            auto key_len = static_cast<uint16_t>(key.size());
            return add_tombstone_with_key_copy(
                key_len, data_ver,
                [&](char* dst) {
                    std::memcpy(dst, key.data(), key_len);
                });
        }

        void
        finalize(uint32_t format_version = 1) {
            const uint32_t header_end   = sizeof(tree_slot_header);
            const uint32_t dir_bytes    = tree_slot_directory_bytes(count);
            const uint32_t payload_used = offset - header_end;

            // Shift the no-directory payload right by `dir_bytes` to make
            // room for the slot directory. memmove handles the overlap.
            if (payload_used > 0 && dir_bytes > 0) {
                std::memmove(static_cast<char*>(buf) + header_end + dir_bytes,
                             static_cast<char*>(buf) + header_end,
                             payload_used);
            }

            auto* hdr = static_cast<tree_slot_header*>(buf);
            // Directory writes go through store_tree_slot_offset (memcpy)
            // because the directory starts at offset 19 (right after the
            // packed header), which is not 2-byte aligned — see the
            // alignment note in format/tree_page.hh.
            for (uint16_t i = 0; i < count; ++i) {
                store_tree_slot_offset(hdr, i, static_cast<uint16_t>(record_offsets_[i] + dir_bytes));
            }

            hdr->magic = TREE_PAGE_MAGIC;
            hdr->format_version = format_version;
            hdr->type = node_type::leaf;
            hdr->record_count = count;
            hdr->free_space_offset = offset + dir_bytes;
            hdr->page_crc = tree_page_compute_crc(buf, page_size);
        }
    };

    // ── Internal Page Builder ──
    // Records must be added in sorted separator key order.
    // After all separators, call set_rightmost_child().
    //
    // Note: rightmost_child_base sits at the tail of the payload area and
    // is intentionally *not* indexed by the slot directory (ODF §4.1).
    // Readers fetch it from `free_space_offset - sizeof(paddr)`.

    struct internal_page_builder {
        void* buf;
        uint32_t page_size;
        uint32_t offset;
        uint16_t count;
        absl::InlinedVector<uint16_t, 384> record_offsets_;
        bool rightmost_set_ = false;

        void
        init(void* b, uint32_t ps) {
            buf = b;
            page_size = ps;
            offset = sizeof(tree_slot_header);
            count = 0;
            record_offsets_.clear();
            rightmost_set_ = false;
            std::memset(buf, 0, page_size);
        }

        uint32_t
        record_size(uint16_t key_len) const {
            return internal_record_size(key_len);
        }

    private:
        // Reserve room for `record_size` separator + its directory entry,
        // plus the trailing rightmost_child_base (10 bytes), so that
        // set_rightmost_child() always succeeds after add_child() returns
        // true. The rightmost child does *not* get a directory entry.
        bool
        can_fit_separator(uint32_t separator_size) const {
            const uint32_t dir_after = sizeof(uint16_t) * (count + 1u);
            return offset + separator_size + dir_after + sizeof(paddr) <= page_size;
        }

    public:
        bool
        add_child(std::string_view separator_key, paddr child_base) {
            auto key_len = static_cast<uint16_t>(separator_key.size());
            uint32_t needed = record_size(key_len);
            if (!can_fit_separator(needed)) return false;

            record_offsets_.push_back(static_cast<uint16_t>(offset));

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
            rightmost_set_ = true;
        }

        void
        finalize(uint32_t format_version = 1) {
            // Internal pages must always carry a rightmost child base —
            // it is the fall-through target when no separator key is
            // strictly greater than the lookup key. Reader's O(1)
            // rightmost_child() reads `free_space_offset - sizeof(paddr)`
            // unconditionally, so a finalize without set_rightmost_child()
            // would yield a CRC-valid page that returns garbage bytes
            // (silent wrong child) instead of an explicit failure. We
            // fail fast here so the bug surfaces at build time, not as a
            // mis-routed lookup at read time.
            if (!rightmost_set_) {
                core::panic_inconsistency(
                    "tree::internal_page_builder::finalize",
                    "rightmost_child not set before finalize (count=%u)",
                    static_cast<unsigned>(count));
            }

            const uint32_t header_end   = sizeof(tree_slot_header);
            const uint32_t dir_bytes    = tree_slot_directory_bytes(count);
            const uint32_t payload_used = offset - header_end;

            // Shift the entire payload (separator records + rightmost
            // child base) right by `dir_bytes` so the directory slots in
            // immediately after the header.
            if (payload_used > 0 && dir_bytes > 0) {
                std::memmove(static_cast<char*>(buf) + header_end + dir_bytes,
                             static_cast<char*>(buf) + header_end,
                             payload_used);
            }

            auto* hdr = static_cast<tree_slot_header*>(buf);
            // Directory writes go through store_tree_slot_offset (memcpy)
            // because the directory starts at offset 19 (right after the
            // packed header), which is not 2-byte aligned — see the
            // alignment note in format/tree_page.hh.
            for (uint16_t i = 0; i < count; ++i) {
                store_tree_slot_offset(hdr, i, static_cast<uint16_t>(record_offsets_[i] + dir_bytes));
            }

            hdr->magic = TREE_PAGE_MAGIC;
            hdr->format_version = format_version;
            hdr->type = node_type::internal;
            hdr->record_count = count;
            hdr->free_space_offset = offset + dir_bytes;
            hdr->page_crc = tree_page_compute_crc(buf, page_size);
        }
    };

}

#endif //APPS_INCONEL_TREE_PAGE_BUILDER_HH
