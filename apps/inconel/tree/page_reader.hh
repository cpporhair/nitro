#ifndef APPS_INCONEL_TREE_PAGE_READER_HH
#define APPS_INCONEL_TREE_PAGE_READER_HH

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "../format/tree_page.hh"
#include "../memory/frame.hh"

namespace apps::inconel::tree {

    using namespace format;

    namespace detail {

        inline bool
        segmented_range_within(const memory::segmented_page_frame& frame,
                               uint64_t offset,
                               uint64_t bytes) noexcept {
            if (!frame.complete()) return false;
            if (frame.span_lbas() == 0) return offset == 0 && bytes == 0;
            if (offset > frame.byte_len()) return false;
            return bytes <= frame.byte_len() - offset;
        }

        template <typename Fn>
        inline bool
        visit_segmented_const_bytes(const memory::segmented_page_frame& frame,
                                    uint64_t offset,
                                    uint64_t bytes,
                                    Fn&& fn) noexcept {
            if (!segmented_range_within(frame, offset, bytes)) return false;

            const uint64_t lba_size = frame.lba_size();
            uint64_t pos = offset;
            uint64_t remaining = bytes;
            while (remaining > 0) {
                const uint64_t page_idx = pos / lba_size;
                if (page_idx >= frame.span_lbas()) return false;
                const auto* page = frame.page_at(static_cast<uint16_t>(page_idx));
                if (page == nullptr || page->buf == nullptr
                    || page->byte_len != lba_size) {
                    return false;
                }

                const uint64_t in_page = pos % lba_size;
                const uint64_t n = std::min<uint64_t>(
                    remaining, lba_size - in_page);
                fn(page->buf + in_page, n);
                pos += n;
                remaining -= n;
            }
            return true;
        }

        inline bool
        copy_from_segmented(const memory::segmented_page_frame& frame,
                            uint64_t offset,
                            void* dst,
                            uint64_t bytes) noexcept {
            char* out = static_cast<char*>(dst);
            uint64_t copied = 0;
            return visit_segmented_const_bytes(
                frame, offset, bytes,
                [&](const char* src, uint64_t n) noexcept {
                    std::memcpy(out + copied, src, static_cast<std::size_t>(n));
                    copied += n;
                });
        }

        template <typename T>
        inline bool
        load_pod(const memory::segmented_page_frame& frame,
                 uint64_t offset,
                 T& out) noexcept {
            return copy_from_segmented(frame, offset, &out, sizeof(T));
        }

        inline bool
        load_slot_offset(const memory::segmented_page_frame& frame,
                         uint16_t index,
                         uint16_t& out) noexcept {
            const uint64_t offset = sizeof(tree_slot_header)
                + static_cast<uint64_t>(index) * sizeof(uint16_t);
            return load_pod(frame, offset, out);
        }

        inline bool
        extend_crc(const memory::segmented_page_frame& frame,
                   uint64_t offset,
                   uint64_t bytes,
                   absl::crc32c_t& crc) noexcept {
            return visit_segmented_const_bytes(
                frame, offset, bytes,
                [&](const char* src, uint64_t n) noexcept {
                    crc = absl::ExtendCrc32c(
                        crc,
                        absl::string_view(src, static_cast<std::size_t>(n)));
                });
        }

        inline int
        compare_segmented_bytes(const memory::segmented_page_frame& frame,
                                uint64_t offset,
                                uint64_t bytes,
                                std::string_view rhs) noexcept {
            const uint64_t common = std::min<uint64_t>(bytes, rhs.size());
            uint64_t compared = 0;
            int result = 0;
            const bool ok = visit_segmented_const_bytes(
                frame, offset, common,
                [&](const char* src, uint64_t n) noexcept {
                    if (result != 0) {
                        compared += n;
                        return;
                    }
                    const int c = std::memcmp(
                        src, rhs.data() + compared,
                        static_cast<std::size_t>(n));
                    if (c < 0) result = -1;
                    else if (c > 0) result = 1;
                    compared += n;
                });
            if (!ok) return 1;
            if (result != 0) return result;
            if (bytes < rhs.size()) return -1;
            if (bytes > rhs.size()) return 1;
            return 0;
        }

        inline std::string
        read_segmented_string(const memory::segmented_page_frame& frame,
                              uint64_t offset,
                              uint64_t bytes) {
            std::string out;
            out.resize(static_cast<std::size_t>(bytes));
            if (bytes == 0) return out;
            if (!copy_from_segmented(frame, offset, out.data(), bytes)) {
                out.clear();
            }
            return out;
        }

    }  // namespace detail

    inline tree_page_status
    inspect_segmented_tree_page(const memory::segmented_page_frame& frame,
                                uint32_t page_size) noexcept {
        if (!frame.complete() || frame.span_lbas() == 0
            || page_size < sizeof(tree_slot_header)
            || page_size > frame.byte_len()) {
            return tree_page_status::bad_magic;
        }

        uint32_t first4 = 0;
        if (!detail::load_pod(frame, 0, first4)) {
            return tree_page_status::bad_magic;
        }
        if (first4 == 0) return tree_page_status::zero_page;

        tree_slot_header hdr{};
        if (!detail::load_pod(frame, 0, hdr)) {
            return tree_page_status::bad_magic;
        }
        if (hdr.magic != TREE_PAGE_MAGIC) return tree_page_status::bad_magic;

        constexpr uint32_t crc_field_offset = offsetof(tree_slot_header, page_crc);
        constexpr uint32_t crc_field_size = sizeof(uint32_t);
        absl::crc32c_t crc{0};
        if (!detail::extend_crc(frame, 0, crc_field_offset, crc)) {
            return tree_page_status::bad_crc;
        }
        if (!detail::extend_crc(
                frame,
                crc_field_offset + crc_field_size,
                page_size - crc_field_offset - crc_field_size,
                crc)) {
            return tree_page_status::bad_crc;
        }
        if (hdr.page_crc != static_cast<uint32_t>(crc)) {
            return tree_page_status::bad_crc;
        }
        return tree_page_status::ok;
    }

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

    struct segmented_leaf_record {
        std::string key;
        uint64_t data_ver;
        record_kind kind;
        value_ref vr;       // valid only when kind == value
    };

    struct segmented_leaf_value {
        uint64_t data_ver;
        record_kind kind;
        value_ref vr;       // valid only when kind == value
    };

    struct segmented_leaf_record_ref {
        uint16_t index = 0;
        uint64_t key_off = 0;
        uint16_t key_len = 0;
        uint64_t data_ver = 0;
        record_kind kind;
        value_ref vr;       // valid only when kind == value
    };

    struct segmented_leaf_page_reader {
        const memory::segmented_page_frame* frame = nullptr;
        uint32_t page_size = 0;
        tree_slot_header hdr{};

        bool
        parse(const memory::segmented_page_frame& f, uint32_t ps) {
            if (inspect_segmented_tree_page(f, ps) != tree_page_status::ok) {
                return false;
            }
            return parse_validated(f, ps);
        }

        bool
        parse_validated(const memory::segmented_page_frame& f, uint32_t ps) {
            frame = &f;
            page_size = ps;
            if (!detail::load_pod(f, 0, hdr)) return false;
            return hdr.type == node_type::leaf;
        }

        uint16_t
        record_count() const { return hdr.record_count; }

        segmented_leaf_record
        get(uint16_t index) const {
            uint16_t rec_off = 0;
            leaf_record_header rh{};
            if (!detail::load_slot_offset(*frame, index, rec_off)
                || !detail::load_pod(*frame, rec_off, rh)) {
                return {};
            }

            const uint64_t key_off = rec_off + sizeof(rh);
            segmented_leaf_record rec;
            rec.key = detail::read_segmented_string(*frame, key_off, rh.key_len);
            rec.data_ver = rh.data_ver;
            rec.kind = rh.kind;
            rec.vr = {};
            if (rh.kind == record_kind::value) {
                (void)detail::load_pod(
                    *frame, key_off + rh.key_len, rec.vr);
            }
            return rec;
        }

        bool
        get_ref(uint16_t index, segmented_leaf_record_ref& out) const {
            uint16_t rec_off = 0;
            leaf_record_header rh{};
            if (!detail::load_slot_offset(*frame, index, rec_off)
                || !detail::load_pod(*frame, rec_off, rh)) {
                return false;
            }

            out.index = index;
            out.key_off = rec_off + sizeof(rh);
            out.key_len = rh.key_len;
            out.data_ver = rh.data_ver;
            out.kind = rh.kind;
            out.vr = {};
            if (rh.kind == record_kind::value) {
                return detail::load_pod(
                    *frame, out.key_off + out.key_len, out.vr);
            }
            return true;
        }

        int
        compare_key_at(uint16_t index, std::string_view target) const {
            return compare_key(index, target);
        }

        template <typename Fn>
        bool
        visit_key(const segmented_leaf_record_ref& rec, Fn&& fn) const {
            return detail::visit_segmented_const_bytes(
                *frame, rec.key_off, rec.key_len, std::forward<Fn>(fn));
        }

        std::optional<segmented_leaf_value>
        find(std::string_view key) const {
            uint16_t idx = lower_bound(key);
            if (idx >= hdr.record_count) return std::nullopt;
            if (compare_key(idx, key) != 0) return std::nullopt;
            return read_value_at(idx);
        }

        uint16_t
        lower_bound(std::string_view target) const {
            uint16_t lo = 0;
            uint16_t hi = hdr.record_count;
            while (lo < hi) {
                uint16_t mid = lo + static_cast<uint16_t>((hi - lo) / 2);
                if (compare_key(mid, target) < 0) {
                    lo = static_cast<uint16_t>(mid + 1);
                } else {
                    hi = mid;
                }
            }
            return lo;
        }

    private:
        int
        compare_key(uint16_t index, std::string_view target) const {
            uint16_t rec_off = 0;
            leaf_record_header rh{};
            if (!detail::load_slot_offset(*frame, index, rec_off)
                || !detail::load_pod(*frame, rec_off, rh)) {
                return 1;
            }
            return detail::compare_segmented_bytes(
                *frame, rec_off + sizeof(rh), rh.key_len, target);
        }

        segmented_leaf_value
        read_value_at(uint16_t index) const {
            uint16_t rec_off = 0;
            leaf_record_header rh{};
            segmented_leaf_value out{};
            if (!detail::load_slot_offset(*frame, index, rec_off)
                || !detail::load_pod(*frame, rec_off, rh)) {
                return out;
            }
            out.data_ver = rh.data_ver;
            out.kind = rh.kind;
            out.vr = {};
            if (rh.kind == record_kind::value) {
                (void)detail::load_pod(
                    *frame,
                    rec_off + sizeof(rh) + rh.key_len,
                    out.vr);
            }
            return out;
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

    struct segmented_internal_entry {
        std::string separator_key;
        paddr child_base;
    };

    struct segmented_internal_page_reader {
        const memory::segmented_page_frame* frame = nullptr;
        uint32_t page_size = 0;
        tree_slot_header hdr{};

        bool
        parse(const memory::segmented_page_frame& f, uint32_t ps) {
            if (inspect_segmented_tree_page(f, ps) != tree_page_status::ok) {
                return false;
            }
            return parse_validated(f, ps);
        }

        bool
        parse_validated(const memory::segmented_page_frame& f, uint32_t ps) {
            frame = &f;
            page_size = ps;
            if (!detail::load_pod(f, 0, hdr)) return false;
            return hdr.type == node_type::internal;
        }

        uint16_t
        record_count() const { return hdr.record_count; }

        segmented_internal_entry
        get(uint16_t index) const {
            uint16_t rec_off = 0;
            uint16_t key_len = 0;
            segmented_internal_entry e;
            if (!detail::load_slot_offset(*frame, index, rec_off)
                || !detail::load_pod(*frame, rec_off, key_len)) {
                return e;
            }
            const uint64_t key_off = rec_off + sizeof(internal_record);
            e.separator_key = detail::read_segmented_string(
                *frame, key_off, key_len);
            (void)detail::load_pod(
                *frame, key_off + key_len, e.child_base);
            return e;
        }

        paddr
        rightmost_child() const {
            paddr result{};
            (void)detail::load_pod(
                *frame, hdr.free_space_offset - sizeof(paddr), result);
            return result;
        }

        paddr
        child_base_at(uint16_t index) const {
            uint16_t rec_off = 0;
            uint16_t key_len = 0;
            paddr child{};
            if (!detail::load_slot_offset(*frame, index, rec_off)
                || !detail::load_pod(*frame, rec_off, key_len)) {
                return child;
            }
            (void)detail::load_pod(
                *frame,
                rec_off + sizeof(internal_record) + key_len,
                child);
            return child;
        }

        paddr
        find_child(std::string_view lookup_key) const {
            uint16_t lo = 0;
            uint16_t hi = hdr.record_count;
            while (lo < hi) {
                uint16_t mid = lo + static_cast<uint16_t>((hi - lo) / 2);
                if (compare_separator(mid, lookup_key) > 0) {
                    hi = mid;
                } else {
                    lo = static_cast<uint16_t>(mid + 1);
                }
            }
            if (lo < hdr.record_count) return child_base_at(lo);
            return rightmost_child();
        }

    private:
        int
        compare_separator(uint16_t index, std::string_view target) const {
            uint16_t rec_off = 0;
            uint16_t key_len = 0;
            if (!detail::load_slot_offset(*frame, index, rec_off)
                || !detail::load_pod(*frame, rec_off, key_len)) {
                return 1;
            }
            return detail::compare_segmented_bytes(
                *frame, rec_off + sizeof(internal_record), key_len, target);
        }
    };

}

#endif //APPS_INCONEL_TREE_PAGE_READER_HH
