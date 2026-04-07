#ifndef APPS_INCONEL_FORMAT_VALUE_OBJECT_HH
#define APPS_INCONEL_FORMAT_VALUE_OBJECT_HH

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include "./crc.hh"
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
        hdr.body_crc = crc32c(body.data(), body.size());

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
        if (crc32c(body_ptr, expected_body_len) != hdr.body_crc) {
            r.status = value_decode_status::bad_crc;
            return r;
        }

        r.status = value_decode_status::ok;
        r.body   = std::string_view(body_ptr, expected_body_len);
        return r;
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
