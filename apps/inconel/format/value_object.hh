#ifndef APPS_INCONEL_FORMAT_VALUE_OBJECT_HH
#define APPS_INCONEL_FORMAT_VALUE_OBJECT_HH

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include <absl/crc/crc32c.h>
#include <absl/strings/string_view.h>

#include "./types.hh"

namespace apps::inconel::format {

    constexpr uint32_t VALUE_MAGIC = 0x56414C55;  // "VALU"

    struct __attribute__((packed)) value_object_header {
        uint32_t magic;
        uint32_t body_len;
        uint32_t body_crc;
    };
    static_assert(sizeof(value_object_header) == 12);

    enum class value_decode_status : uint8_t {
        ok = 0,
        truncated,
        bad_magic,
        bad_body_len,
        bad_crc,
    };

    struct value_decode_result {
        value_decode_status status = value_decode_status::truncated;
        std::string_view    body;

        bool ok() const noexcept { return status == value_decode_status::ok; }
    };

    // ── codec ──
    //
    // encode: write header + body into a slot span. The slot must be at least
    // sizeof(header) + body.size() bytes. Trailing bytes (padding) are NOT
    // touched here — caller is responsible for zeroing the slot if it cares.
    // (Persisting nondeterministic bytes after the body is fine because the
    // CRC only covers the body and decoders use the header's body_len.)

    inline bool
    encode_value_object(std::span<char> slot, std::span<const char> body) noexcept {
        if (slot.size() < sizeof(value_object_header) + body.size()) return false;

        value_object_header hdr{};
        hdr.magic    = VALUE_MAGIC;
        hdr.body_len = static_cast<uint32_t>(body.size());
        hdr.body_crc = static_cast<uint32_t>(absl::ComputeCrc32c(
            absl::string_view(body.data(), body.size())));

        std::memcpy(slot.data(), &hdr, sizeof(hdr));
        std::memcpy(slot.data() + sizeof(hdr), body.data(), body.size());
        return true;
    }

    inline value_decode_result
    decode_value_object(std::span<const char> slot, uint32_t expected_body_len) noexcept {
        value_decode_result r{};

        if (slot.size() < sizeof(value_object_header) + expected_body_len) {
            r.status = value_decode_status::truncated;
            return r;
        }

        value_object_header hdr{};
        std::memcpy(&hdr, slot.data(), sizeof(hdr));

        if (hdr.magic != VALUE_MAGIC) {
            r.status = value_decode_status::bad_magic;
            return r;
        }
        if (hdr.body_len != expected_body_len) {
            r.status = value_decode_status::bad_body_len;
            return r;
        }

        const char* body_ptr = slot.data() + sizeof(hdr);
        uint32_t computed = static_cast<uint32_t>(absl::ComputeCrc32c(
            absl::string_view(body_ptr, expected_body_len)));
        if (computed != hdr.body_crc) {
            r.status = value_decode_status::bad_crc;
            return r;
        }

        r.status = value_decode_status::ok;
        r.body   = std::string_view(body_ptr, expected_body_len);
        return r;
    }

    // ── bounded slot helpers ──
    //
    // These helpers keep the object codec aware of the allocator's slot
    // boundary. The span overloads serve the current contiguous page_frame
    // path. The templated overloads serve segmented_value_frame-style frames:
    // any frame type with complete(), span_lbas(), lba_size(), byte_len(), and
    // page_at(i)->{buf, byte_len} can be used without making format depend on
    // memory/frame.hh.

    namespace detail {

        inline bool
        range_within(uint64_t total, uint64_t offset, uint64_t bytes) noexcept {
            return offset <= total && bytes <= total - offset;
        }

        template <typename Frame>
        inline bool
        segmented_range_within(const Frame& frame,
                               uint64_t offset,
                               uint64_t bytes) noexcept {
            if (!frame.complete()) return false;
            if (frame.span_lbas() == 0) {
                return offset == 0 && bytes == 0;
            }
            return range_within(frame.byte_len(), offset, bytes);
        }

        template <typename Frame, typename Fn>
        inline bool
        visit_segmented_const_bytes(const Frame& frame,
                                    uint64_t byte_offset,
                                    uint64_t bytes,
                                    Fn&& fn) noexcept {
            if (!frame.complete()) return false;
            if (frame.span_lbas() == 0) {
                return byte_offset == 0 && bytes == 0;
            }
            const uint64_t lba_size = frame.lba_size();
            if (lba_size == 0 ||
                !range_within(frame.byte_len(), byte_offset, bytes)) {
                return false;
            }

            uint64_t pos = byte_offset;
            uint64_t remaining = bytes;
            while (remaining > 0) {
                const uint64_t page_idx = pos / lba_size;
                if (page_idx >= frame.span_lbas()) return false;

                const auto* page = frame.page_at(static_cast<uint16_t>(page_idx));
                if (page == nullptr || page->buf == nullptr ||
                    page->byte_len != lba_size) {
                    return false;
                }

                const uint64_t in_page = pos % lba_size;
                const uint64_t n = std::min<uint64_t>(
                    remaining, lba_size - in_page);
                fn(static_cast<const char*>(page->buf) + in_page, n);
                pos += n;
                remaining -= n;
            }
            return true;
        }

        template <typename Frame, typename Fn>
        inline bool
        visit_segmented_mutable_bytes(Frame& frame,
                                      uint64_t byte_offset,
                                      uint64_t bytes,
                                      Fn&& fn) noexcept {
            if (!frame.complete()) return false;
            if (frame.span_lbas() == 0) {
                return byte_offset == 0 && bytes == 0;
            }
            const uint64_t lba_size = frame.lba_size();
            if (lba_size == 0 ||
                !range_within(frame.byte_len(), byte_offset, bytes)) {
                return false;
            }

            uint64_t pos = byte_offset;
            uint64_t remaining = bytes;
            while (remaining > 0) {
                const uint64_t page_idx = pos / lba_size;
                if (page_idx >= frame.span_lbas()) return false;

                auto* page = frame.page_at(static_cast<uint16_t>(page_idx));
                if (page == nullptr || page->buf == nullptr ||
                    page->byte_len != lba_size) {
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

    } // namespace detail

    inline bool
    encode_value_object_slot(std::span<char> slot_image,
                             uint64_t slot_offset,
                             uint64_t slot_bytes,
                             std::span<const char> body) noexcept {
        if (slot_bytes > slot_image.size()) return false;
        if (!detail::range_within(slot_image.size(), slot_offset, slot_bytes)) {
            return false;
        }
        return encode_value_object(
            slot_image.subspan(static_cast<size_t>(slot_offset),
                               static_cast<size_t>(slot_bytes)),
            body);
    }

    inline value_decode_result
    decode_value_object_slot(std::span<const char> slot_image,
                             uint64_t slot_offset,
                             uint64_t slot_bytes,
                             uint32_t expected_body_len) noexcept {
        value_decode_result r{};
        if (slot_bytes > slot_image.size() ||
            !detail::range_within(slot_image.size(), slot_offset, slot_bytes)) {
            r.status = value_decode_status::truncated;
            return r;
        }
        return decode_value_object(
            slot_image.subspan(static_cast<size_t>(slot_offset),
                               static_cast<size_t>(slot_bytes)),
            expected_body_len);
    }

    template <typename Frame>
    inline bool
    copy_value_slot_from(Frame& frame,
                         uint64_t slot_offset,
                         uint64_t slot_bytes,
                         std::span<const char> src) noexcept {
        if (src.size() > slot_bytes) return false;
        if (!detail::segmented_range_within(frame, slot_offset, slot_bytes)) {
            return false;
        }

        size_t copied = 0;
        return detail::visit_segmented_mutable_bytes(
            frame, slot_offset, src.size(),
            [&](char* dst, uint64_t n) noexcept {
                std::memcpy(dst, src.data() + copied, static_cast<size_t>(n));
                copied += static_cast<size_t>(n);
            });
    }

    template <typename Frame>
    inline bool
    copy_value_slot_to(const Frame& frame,
                       uint64_t slot_offset,
                       uint64_t slot_bytes,
                       std::span<char> dst) noexcept {
        if (dst.size() > slot_bytes) return false;
        if (!detail::segmented_range_within(frame, slot_offset, slot_bytes)) {
            return false;
        }

        size_t copied = 0;
        return detail::visit_segmented_const_bytes(
            frame, slot_offset, dst.size(),
            [&](const char* src, uint64_t n) noexcept {
                std::memcpy(dst.data() + copied, src, static_cast<size_t>(n));
                copied += static_cast<size_t>(n);
            });
    }

    template <typename Frame>
    inline bool
    encode_value_object_slot(Frame& frame,
                             uint64_t slot_offset,
                             uint64_t slot_bytes,
                             std::span<const char> body) noexcept {
        if (slot_bytes < sizeof(value_object_header) + body.size()) {
            return false;
        }
        if (!detail::segmented_range_within(frame, slot_offset, slot_bytes)) {
            return false;
        }

        value_object_header hdr{};
        hdr.magic    = VALUE_MAGIC;
        hdr.body_len = static_cast<uint32_t>(body.size());
        hdr.body_crc = static_cast<uint32_t>(absl::ComputeCrc32c(
            absl::string_view(body.data(), body.size())));

        return copy_value_slot_from(
                   frame, slot_offset, slot_bytes,
                   std::span<const char>(
                       reinterpret_cast<const char*>(&hdr), sizeof(hdr))) &&
               copy_value_slot_from(
                   frame, slot_offset + sizeof(hdr),
                   slot_bytes - sizeof(hdr), body);
    }

    template <typename Frame>
    inline value_decode_status
    decode_value_object_slot_to(const Frame& frame,
                                uint64_t slot_offset,
                                uint64_t slot_bytes,
                                uint32_t expected_body_len,
                                std::span<char> body_out) noexcept {
        if (body_out.size() < expected_body_len ||
            slot_bytes < sizeof(value_object_header) + expected_body_len ||
            !detail::segmented_range_within(frame, slot_offset, slot_bytes)) {
            return value_decode_status::truncated;
        }

        value_object_header hdr{};
        if (!copy_value_slot_to(
                frame, slot_offset, slot_bytes,
                std::span<char>(reinterpret_cast<char*>(&hdr), sizeof(hdr)))) {
            return value_decode_status::truncated;
        }

        if (hdr.magic != VALUE_MAGIC) {
            return value_decode_status::bad_magic;
        }
        if (hdr.body_len != expected_body_len) {
            return value_decode_status::bad_body_len;
        }

        absl::crc32c_t crc{0};
        const uint64_t body_offset = slot_offset + sizeof(hdr);
        if (!detail::visit_segmented_const_bytes(
                frame, body_offset, expected_body_len,
                [&](const char* src, uint64_t n) noexcept {
                    crc = absl::ExtendCrc32c(
                        crc, absl::string_view(src, static_cast<size_t>(n)));
                })) {
            return value_decode_status::truncated;
        }
        if (static_cast<uint32_t>(crc) != hdr.body_crc) {
            return value_decode_status::bad_crc;
        }

        if (!copy_value_slot_to(
                frame, body_offset, slot_bytes - sizeof(hdr),
                body_out.first(expected_body_len))) {
            return value_decode_status::truncated;
        }
        return value_decode_status::ok;
    }

    // ── class / slot helpers ──
    //
    // Three class shapes are supported:
    //   sub-LBA      : class_size <  lba_size  (lba_size % class_size == 0)
    //   LBA-equal    : class_size == lba_size
    //   multi-LBA    : class_size >  lba_size  (class_size % lba_size == 0)
    //
    // Other shapes (non-divisible boundaries) are rejected by returning
    // nullopt; the builder asserts on construction so this never trips at
    // runtime.

    inline std::optional<uint32_t>
    value_slots_per_lba(uint32_t class_size, uint32_t lba_size) noexcept {
        if (class_size == 0 || lba_size == 0) return std::nullopt;
        if (class_size >= lba_size) return std::nullopt;
        if (lba_size % class_size != 0) return std::nullopt;
        return lba_size / class_size;
    }

    inline std::optional<uint32_t>
    value_span_lbas(uint32_t class_size, uint32_t lba_size) noexcept {
        if (class_size == 0 || lba_size == 0) return std::nullopt;
        if (class_size < lba_size) return 1;
        if (class_size % lba_size != 0) return std::nullopt;
        return class_size / lba_size;
    }

    inline std::optional<uint16_t>
    value_byte_offset_for_slot(uint32_t class_size, uint32_t lba_size, uint32_t slot_index) noexcept {
        if (class_size == 0 || lba_size == 0) return std::nullopt;

        if (class_size < lba_size) {
            // sub-LBA: slot_index in [0, slots_per_lba)
            if (lba_size % class_size != 0) return std::nullopt;
            uint32_t slots = lba_size / class_size;
            if (slot_index >= slots) return std::nullopt;
            return static_cast<uint16_t>(slot_index * class_size);
        }

        // LBA-equal or multi-LBA: only slot 0, byte_offset 0
        if (class_size % lba_size != 0) return std::nullopt;
        if (slot_index != 0) return std::nullopt;
        return uint16_t{0};
    }

    // ── min-class lookup ──
    //
    // Returns the index of the smallest class whose size can hold the given
    // total bytes (header + body), or nullopt if no class is large enough.
    // class_sizes must be sorted ascending.

    inline std::optional<uint16_t>
    find_min_class(uint32_t total_bytes, std::span<const uint32_t> class_sizes) noexcept {
        for (uint16_t i = 0; i < class_sizes.size(); ++i) {
            if (class_sizes[i] >= total_bytes) return i;
        }
        return std::nullopt;
    }

    inline uint32_t
    slots_per_page(uint32_t class_size, uint32_t lba_size) noexcept {
        if (class_size < lba_size) {
            return lba_size / class_size;  // sub-LBA: many slots per LBA
        }
        return 1;  // LBA-equal / multi-LBA: one slot per page
    }

}

#endif //APPS_INCONEL_FORMAT_VALUE_OBJECT_HH
