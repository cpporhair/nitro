#ifndef APPS_INCONEL_FORMAT_WAL_HH
#define APPS_INCONEL_FORMAT_WAL_HH

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include <absl/crc/crc32c.h>
#include <absl/strings/string_view.h>

#include "./types.hh"

// ─────────────────────────────────────────────────────────────────────────────
// WAL on-disk format (ODF §3).
//
// This header is the *single* source of truth for the byte layout of WAL
// segment headers, sealed trailers and per-entry records, plus the encode /
// decode helpers used by the front owner (write side) and recovery (read
// side). Future front_sched / wal_space_sched / recovery code must reach
// the WAL bytes through these helpers — never via hand-rolled offset
// arithmetic — so the layout stays in one place.
//
// Out of scope for this header (intentionally):
//   - segment allocator / append cursor
//   - per-stream state, FUA scheduling, rotation policy
//   - any owner / sender / scheduler glue
//
// All multi-byte fields are little-endian (ODF §1.1). All packed structs
// preserve their declared field order; `static_assert(sizeof(...))`s
// downstream of each definition catch any accidental drift.
// ─────────────────────────────────────────────────────────────────────────────

namespace apps::inconel::format {

    // ── Magics ─────────────────────────────────────────────────────────────
    //
    // Both magics are stored as little-endian uint32_t. The four-character
    // gloss in the ODF (`"WALS"`, `"SEAL"`) is purely for human eyeballing
    // hex dumps; the on-disk bytes are whatever the integer literal
    // serialises to.

    constexpr uint32_t WAL_SEGMENT_MAGIC = 0x57414C53;  // "WALS"
    constexpr uint32_t WAL_SEAL_MAGIC    = 0x5345414C;  // "SEAL"

    // Sealed trailer flag value. ODF §3.4 only defines `1 = sealed`; any
    // other value at decode time is treated as "not sealed".
    constexpr uint8_t  WAL_SEALED_FLAG_SEALED = 1;

    // ── Op type ────────────────────────────────────────────────────────────
    //
    // ODF §3.3 reserves opcode bytes 0x03..0xFF for future large-value /
    // multi-extent ops. Decoders must reject any byte outside the values
    // declared here, so AI implementations of newer extensions cannot quietly
    // alias an unknown op into PUT/DELETE.

    enum class wal_op_type : uint8_t {
        put = 0x01,
        del = 0x02,
    };

    // ── PODs ───────────────────────────────────────────────────────────────

    // ODF §3.2: 26-byte header at the start of every WAL segment.
    struct __attribute__((packed)) wal_segment_header {
        uint32_t magic;            // WAL_SEGMENT_MAGIC
        uint32_t format_version;   // matches superblock.format_version
        uint32_t segment_index;    // index within the WAL area
        uint16_t device_id;        // v1 = 0
        uint32_t stream_id;        // owning front_sched id
        uint32_t segment_gen;      // bumped each time the segment is recycled
        uint32_t crc;              // CRC-32C over magic..segment_gen (= 22 bytes)
    };
    static_assert(sizeof(wal_segment_header) == 26);

    // ODF §3.3: 25-byte fixed prefix per WAL entry. Followed in memory by
    //   PUT:    [ value_ref(18) | key_bytes(key_len) | crc32(4) ]
    //   DELETE: [                  key_bytes(key_len) | crc32(4) ]
    // The trailing CRC covers everything from the start of this header
    // through the last key byte (i.e. `total_len - 4` bytes).
    struct __attribute__((packed)) wal_entry_header {
        uint32_t total_len;        // entire entry: header + payload + 4-byte trailer crc
        uint32_t segment_gen;      // must equal owning segment header's gen
        uint64_t lsn;              // batch_lsn
        uint32_t entry_count;      // canonical record count of the owning batch
        uint8_t  op_type;          // wal_op_type as raw byte
        uint32_t key_len;          // bytes of the trailing key
    };
    static_assert(sizeof(wal_entry_header) == 25);

    // ODF §3.4: 33-byte trailer written when a segment is sealed. The
    // trailer is a hint, not a correctness anchor — recovery must still be
    // able to scan entries even if the trailer is missing or corrupt.
    struct __attribute__((packed)) wal_sealed_trailer {
        uint32_t magic;            // WAL_SEAL_MAGIC
        uint32_t segment_gen;      // matches segment header's gen
        uint32_t write_end;        // byte offset of the byte after the last entry
        uint64_t min_lsn;
        uint64_t max_lsn;
        uint8_t  sealed;           // == WAL_SEALED_FLAG_SEALED when committed
        uint32_t crc;              // CRC-32C over magic..sealed (= 29 bytes)
    };
    static_assert(sizeof(wal_sealed_trailer) == 33);

    // Sizes exposed for callers that want to do arithmetic without
    // re-stating sizeof at the call site. Trailing CRC is the same 4-byte
    // CRC-32C used by the headers.
    constexpr uint32_t WAL_SEGMENT_HEADER_SIZE     = sizeof(wal_segment_header);  // 26
    constexpr uint32_t WAL_SEALED_TRAILER_SIZE     = sizeof(wal_sealed_trailer);  // 33
    constexpr uint32_t WAL_ENTRY_TRAILER_CRC_BYTES = sizeof(uint32_t);            // 4

    // ── Header / trailer CRC ───────────────────────────────────────────────
    //
    // CRC-32C is computed over every byte preceding the trailing `crc`
    // field, which is exactly `offsetof(..., crc)` bytes thanks to the
    // packed layout. Helpers do not touch the `crc` field of the input;
    // callers fill it themselves so the same helper covers both
    // encode-time ("compute and store") and decode-time ("recompute and
    // compare") use sites.
    //
    // `offsetof` on a packed struct is conditionally supported by the
    // standard, but GCC/Clang (which is what Inconel targets) do support
    // it, and `format/tree_page.hh` already relies on the same idiom.

    inline uint32_t
    wal_segment_header_crc(const wal_segment_header& h) noexcept {
        constexpr uint32_t covered = offsetof(wal_segment_header, crc);
        return static_cast<uint32_t>(absl::ComputeCrc32c(
            absl::string_view(reinterpret_cast<const char*>(&h), covered)));
    }

    inline uint32_t
    wal_sealed_trailer_crc(const wal_sealed_trailer& t) noexcept {
        constexpr uint32_t covered = offsetof(wal_sealed_trailer, crc);
        return static_cast<uint32_t>(absl::ComputeCrc32c(
            absl::string_view(reinterpret_cast<const char*>(&t), covered)));
    }

    // ── Reason-aware status enums ──────────────────────────────────────────
    //
    // The recovery and front paths need to know *why* a structural check
    // failed so they can panic / log with a precise reason rather than
    // collapse every failure into a single bool. Each status enum is
    // intentionally tight: only the values an inspector / encoder /
    // decoder can actually return are listed.

    enum class wal_segment_status : uint8_t {
        ok = 0,
        bad_magic,
        bad_crc,
        bad_version,
    };

    enum class wal_trailer_status : uint8_t {
        ok = 0,
        bad_magic,
        bad_crc,
        bad_sealed_flag,
    };

    enum class wal_entry_encode_status : uint8_t {
        ok = 0,
        dst_too_small,
        key_too_large,
    };

    enum class wal_entry_decode_status : uint8_t {
        ok = 0,
        truncated,
        bad_total_len,
        bad_segment_gen,
        bad_op_type,
        bad_crc,
    };

    inline const char*
    wal_segment_status_to_string(wal_segment_status s) {
        switch (s) {
        case wal_segment_status::ok:          return "ok";
        case wal_segment_status::bad_magic:   return "bad_magic";
        case wal_segment_status::bad_crc:     return "bad_crc";
        case wal_segment_status::bad_version: return "bad_version";
        }
        return "unknown";
    }

    inline const char*
    wal_trailer_status_to_string(wal_trailer_status s) {
        switch (s) {
        case wal_trailer_status::ok:              return "ok";
        case wal_trailer_status::bad_magic:       return "bad_magic";
        case wal_trailer_status::bad_crc:         return "bad_crc";
        case wal_trailer_status::bad_sealed_flag: return "bad_sealed_flag";
        }
        return "unknown";
    }

    inline const char*
    wal_entry_encode_status_to_string(wal_entry_encode_status s) {
        switch (s) {
        case wal_entry_encode_status::ok:            return "ok";
        case wal_entry_encode_status::dst_too_small: return "dst_too_small";
        case wal_entry_encode_status::key_too_large: return "key_too_large";
        }
        return "unknown";
    }

    inline const char*
    wal_entry_decode_status_to_string(wal_entry_decode_status s) {
        switch (s) {
        case wal_entry_decode_status::ok:              return "ok";
        case wal_entry_decode_status::truncated:       return "truncated";
        case wal_entry_decode_status::bad_total_len:   return "bad_total_len";
        case wal_entry_decode_status::bad_segment_gen: return "bad_segment_gen";
        case wal_entry_decode_status::bad_op_type:     return "bad_op_type";
        case wal_entry_decode_status::bad_crc:         return "bad_crc";
        }
        return "unknown";
    }

    // ── Header / trailer inspect helpers ───────────────────────────────────
    //
    // Each inspector validates the structural invariants documented in
    // ODF §3.2 / §3.4 and returns a reason-aware status. Cross-field
    // checks that depend on caller knowledge (e.g. the segment_index that
    // wal_space_sched expects, or matching the trailer's segment_gen
    // against the header's gen) are intentionally left to the caller —
    // the format layer cannot know the right "expected" values.

    inline wal_segment_status
    inspect_wal_segment_header(const wal_segment_header& h,
                               uint32_t expected_format_version) noexcept {
        if (h.magic != WAL_SEGMENT_MAGIC) return wal_segment_status::bad_magic;
        if (h.format_version != expected_format_version)
            return wal_segment_status::bad_version;
        if (h.crc != wal_segment_header_crc(h))
            return wal_segment_status::bad_crc;
        return wal_segment_status::ok;
    }

    inline wal_trailer_status
    inspect_wal_sealed_trailer(const wal_sealed_trailer& t) noexcept {
        if (t.magic != WAL_SEAL_MAGIC) return wal_trailer_status::bad_magic;
        if (t.crc != wal_sealed_trailer_crc(t)) return wal_trailer_status::bad_crc;
        if (t.sealed != WAL_SEALED_FLAG_SEALED)
            return wal_trailer_status::bad_sealed_flag;
        return wal_trailer_status::ok;
    }

    // ── Entry size helpers ─────────────────────────────────────────────────
    //
    //   PUT    total_len = header + value_ref + key_len + 4-byte CRC
    //   DELETE total_len = header              + key_len + 4-byte CRC
    //
    // The "fixed" constants below are the parts of `total_len` that do not
    // depend on `key_len`; they exist so the encoders can compute the
    // overflow-safe maximum key length once at compile time.

    constexpr uint32_t WAL_PUT_FIXED_BYTES =
        WAL_ENTRY_TRAILER_CRC_BYTES
        + static_cast<uint32_t>(sizeof(wal_entry_header))
        + static_cast<uint32_t>(sizeof(value_ref));

    constexpr uint32_t WAL_DELETE_FIXED_BYTES =
        WAL_ENTRY_TRAILER_CRC_BYTES
        + static_cast<uint32_t>(sizeof(wal_entry_header));

    // The largest `key_len` for which `wal_put_entry_size` (resp. delete)
    // does not overflow `uint32_t`. Both encoders enforce this; the format
    // layer does *not* try to enforce a smaller "policy" cap (e.g. the
    // 1024-byte hint in ODF §3.6) — that belongs in the front owner.
    constexpr uint32_t WAL_PUT_MAX_KEY_LEN =
        std::numeric_limits<uint32_t>::max() - WAL_PUT_FIXED_BYTES;
    constexpr uint32_t WAL_DELETE_MAX_KEY_LEN =
        std::numeric_limits<uint32_t>::max() - WAL_DELETE_FIXED_BYTES;

    constexpr uint32_t
    wal_put_entry_size(uint32_t key_len) noexcept {
        return WAL_PUT_FIXED_BYTES + key_len;
    }

    constexpr uint32_t
    wal_delete_entry_size(uint32_t key_len) noexcept {
        return WAL_DELETE_FIXED_BYTES + key_len;
    }

    // ── Decoded entry view ─────────────────────────────────────────────────
    //
    // `key` is a non-owning view into the source span passed to
    // `decode_wal_entry`; the caller is responsible for keeping that span
    // alive (or copying out of it) before reusing the buffer. PUT entries
    // populate `vr`; DELETE entries leave it `nullopt`.

    struct decoded_wal_entry {
        wal_op_type              op_type     = wal_op_type::put;
        uint64_t                 lsn         = 0;
        uint32_t                 entry_count = 0;
        std::string_view         key;
        std::optional<value_ref> vr;
    };

    // ── Encode helpers ─────────────────────────────────────────────────────
    //
    // Both encoders write a complete entry (header + payload + trailing
    // CRC) into `dst`, leave `dst` unmodified on failure, and return the
    // emitted byte count via `*out_total_len` on success. `dst` must point
    // at the start of where the entry should land in the segment — the
    // encoders do not advance any cursor.

    inline wal_entry_encode_status
    encode_wal_put_entry(std::span<char>     dst,
                         uint32_t            segment_gen,
                         uint64_t            lsn,
                         uint32_t            entry_count,
                         std::string_view    key,
                         const value_ref&    vr,
                         uint32_t*           out_total_len) noexcept {
        if (key.size() > WAL_PUT_MAX_KEY_LEN)
            return wal_entry_encode_status::key_too_large;
        const uint32_t key_len   = static_cast<uint32_t>(key.size());
        const uint32_t total_len = wal_put_entry_size(key_len);

        if (dst.size() < total_len)
            return wal_entry_encode_status::dst_too_small;

        wal_entry_header hdr{};
        hdr.total_len   = total_len;
        hdr.segment_gen = segment_gen;
        hdr.lsn         = lsn;
        hdr.entry_count = entry_count;
        hdr.op_type     = static_cast<uint8_t>(wal_op_type::put);
        hdr.key_len     = key_len;

        char* p = dst.data();
        std::memcpy(p, &hdr, sizeof(hdr));
        p += sizeof(hdr);
        std::memcpy(p, &vr, sizeof(value_ref));
        p += sizeof(value_ref);
        if (key_len > 0) std::memcpy(p, key.data(), key_len);
        p += key_len;

        const uint32_t covered = total_len - WAL_ENTRY_TRAILER_CRC_BYTES;
        const uint32_t crc = static_cast<uint32_t>(absl::ComputeCrc32c(
            absl::string_view(dst.data(), covered)));
        std::memcpy(p, &crc, sizeof(crc));

        if (out_total_len) *out_total_len = total_len;
        return wal_entry_encode_status::ok;
    }

    inline wal_entry_encode_status
    encode_wal_delete_entry(std::span<char>     dst,
                            uint32_t            segment_gen,
                            uint64_t            lsn,
                            uint32_t            entry_count,
                            std::string_view    key,
                            uint32_t*           out_total_len) noexcept {
        if (key.size() > WAL_DELETE_MAX_KEY_LEN)
            return wal_entry_encode_status::key_too_large;
        const uint32_t key_len   = static_cast<uint32_t>(key.size());
        const uint32_t total_len = wal_delete_entry_size(key_len);

        if (dst.size() < total_len)
            return wal_entry_encode_status::dst_too_small;

        wal_entry_header hdr{};
        hdr.total_len   = total_len;
        hdr.segment_gen = segment_gen;
        hdr.lsn         = lsn;
        hdr.entry_count = entry_count;
        hdr.op_type     = static_cast<uint8_t>(wal_op_type::del);
        hdr.key_len     = key_len;

        char* p = dst.data();
        std::memcpy(p, &hdr, sizeof(hdr));
        p += sizeof(hdr);
        if (key_len > 0) std::memcpy(p, key.data(), key_len);
        p += key_len;

        const uint32_t covered = total_len - WAL_ENTRY_TRAILER_CRC_BYTES;
        const uint32_t crc = static_cast<uint32_t>(absl::ComputeCrc32c(
            absl::string_view(dst.data(), covered)));
        std::memcpy(p, &crc, sizeof(crc));

        if (out_total_len) *out_total_len = total_len;
        return wal_entry_encode_status::ok;
    }

    // ── Decode helper ──────────────────────────────────────────────────────
    //
    // `src` is a prefix view of the segment's remaining bytes. The decoder
    // first reads the fixed header to learn `total_len`, then validates
    // structurally before touching CRC. Validation order:
    //
    //   1. src has at least the fixed header → otherwise truncated
    //   2. total_len ≥ header + crc          → otherwise bad_total_len
    //   3. src has at least total_len bytes  → otherwise truncated
    //   4. segment_gen matches expected      → otherwise bad_segment_gen
    //   5. op_type is recognised             → otherwise bad_op_type
    //   6. key_len agrees with total_len     → otherwise bad_total_len
    //   7. trailing CRC matches              → otherwise bad_crc
    //
    // The output struct is touched only after every check passes; on any
    // failure `*out` and `*out_total_len` are left unchanged. This makes
    // the helper safe to use inside a recovery loop that wants to detect
    // truncation without copying state on every iteration.

    inline wal_entry_decode_status
    decode_wal_entry(std::span<const char> src,
                     uint32_t              expected_segment_gen,
                     decoded_wal_entry*    out,
                     uint32_t*             out_total_len) noexcept {
        constexpr uint32_t header_bytes = sizeof(wal_entry_header);
        constexpr uint32_t vr_bytes     = sizeof(value_ref);
        constexpr uint32_t crc_bytes    = WAL_ENTRY_TRAILER_CRC_BYTES;

        if (src.size() < header_bytes)
            return wal_entry_decode_status::truncated;

        wal_entry_header hdr{};
        std::memcpy(&hdr, src.data(), header_bytes);

        if (hdr.total_len < header_bytes + crc_bytes)
            return wal_entry_decode_status::bad_total_len;
        if (src.size() < hdr.total_len)
            return wal_entry_decode_status::truncated;

        if (hdr.segment_gen != expected_segment_gen)
            return wal_entry_decode_status::bad_segment_gen;

        wal_op_type op;
        if (hdr.op_type == static_cast<uint8_t>(wal_op_type::put))
            op = wal_op_type::put;
        else if (hdr.op_type == static_cast<uint8_t>(wal_op_type::del))
            op = wal_op_type::del;
        else
            return wal_entry_decode_status::bad_op_type;

        // Derive payload length from total_len, then check key_len agrees
        // with it. Both directions ("key_len corrupted" and "total_len
        // corrupted") collapse into bad_total_len without ever invoking
        // the size helpers (which could overflow uint32_t on attacker
        // input).
        const uint32_t payload_len = hdr.total_len - header_bytes - crc_bytes;
        uint32_t       key_offset_in_src = header_bytes;
        if (op == wal_op_type::put) {
            if (payload_len < vr_bytes)
                return wal_entry_decode_status::bad_total_len;
            if (hdr.key_len != payload_len - vr_bytes)
                return wal_entry_decode_status::bad_total_len;
            key_offset_in_src += vr_bytes;
        } else {
            if (hdr.key_len != payload_len)
                return wal_entry_decode_status::bad_total_len;
        }

        const uint32_t covered = hdr.total_len - crc_bytes;
        const uint32_t computed = static_cast<uint32_t>(absl::ComputeCrc32c(
            absl::string_view(src.data(), covered)));
        uint32_t stored = 0;
        std::memcpy(&stored, src.data() + covered, crc_bytes);
        if (stored != computed)
            return wal_entry_decode_status::bad_crc;

        if (out) {
            out->op_type     = op;
            out->lsn         = hdr.lsn;
            out->entry_count = hdr.entry_count;
            if (op == wal_op_type::put) {
                value_ref vr{};
                std::memcpy(&vr, src.data() + header_bytes, vr_bytes);
                out->vr = vr;
            } else {
                out->vr.reset();
            }
            out->key = std::string_view(src.data() + key_offset_in_src, hdr.key_len);
        }

        if (out_total_len) *out_total_len = hdr.total_len;
        return wal_entry_decode_status::ok;
    }

}

#endif //APPS_INCONEL_FORMAT_WAL_HH
