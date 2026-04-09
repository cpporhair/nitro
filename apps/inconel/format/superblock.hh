#ifndef APPS_INCONEL_FORMAT_SUPERBLOCK_HH
#define APPS_INCONEL_FORMAT_SUPERBLOCK_HH

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include <absl/crc/crc32c.h>
#include <absl/strings/string_view.h>

#include "./types.hh"

// ─────────────────────────────────────────────────────────────────────────────
// Superblock on-disk format (ODF §2).
//
// This header is the *single* source of truth for the byte layout of the
// inconel superblock and the helpers used to validate, CRC, and choose
// between the A/B copies. Any future format / recovery / superblock-update
// code must reach those bytes through these helpers — never via hand-rolled
// CRC math or hand-rolled A/B selection — so the layout and the rules in
// ODF §2.3 / recovery_and_wal_reclaim §2 stay in one place.
//
// Out of scope for this header (intentionally):
//   - format_disk() / region planning
//   - boot recovery / runtime install
//   - runtime build_options / profile hardening
//   - any I/O (NVMe read/write, FUA ordering)
//
// All multi-byte fields are little-endian (ODF §1.1). The packed layout is
// pinned by `static_assert(std::is_trivially_copyable_v<superblock>)` plus
// `static_assert(sizeof(superblock) <= 4096)` so a single LBA always fits.
// ─────────────────────────────────────────────────────────────────────────────

namespace apps::inconel::format {

    constexpr uint64_t SUPERBLOCK_MAGIC = 0x494E434F4E454C31ULL;  // "INCONEL1"

    // v1 is the only format version `inspect_superblock` will accept. When
    // a future incompatible bump lands, the writers will set this constant
    // to the new value and recovery / format will refuse the old one.
    constexpr uint32_t SUPERBLOCK_FORMAT_VERSION_V1 = 1;

    // Field order MUST match ODF §2.2. The packed attribute removes any
    // implicit padding, and the static_assert below pins trivial-copy
    // semantics so the struct can be memcpy'd into / out of a DMA buffer
    // without invoking any user-provided ctor/dtor.
    struct __attribute__((packed)) superblock {
        // ── magic / version ──
        uint64_t magic;
        uint32_t format_version;

        // ── device parameters ──
        uint64_t namespace_size;
        uint32_t lba_size;

        // ── tree parameters ──
        uint32_t tree_page_size;
        uint32_t shadow_slots_per_range;

        // ── WAL parameters ──
        paddr    wal_base_paddr;
        uint32_t wal_segment_size;
        uint32_t wal_segment_count;

        // ── data area parameters ──
        paddr    data_area_base_paddr;
        paddr    data_area_end_paddr;

        // ── value parameters ──
        uint8_t  value_size_class_count;
        uint32_t value_size_classes[16];

        // ── current state ──
        paddr    root_base_paddr;
        uint64_t generation;

        // ── checksum ──
        uint32_t crc;
    };

    static_assert(std::is_trivially_copyable_v<superblock>);
    static_assert(sizeof(superblock) <= 4096);

    // ── CRC ─────────────────────────────────────────────────────────────────
    //
    // CRC-32C covers every byte preceding `crc`, which is exactly
    // `offsetof(superblock, crc)` bytes thanks to the packed layout. This
    // matches the wal_segment_header / wal_sealed_trailer pattern. The
    // helper does not touch the `crc` field of the input — callers fill
    // it themselves so the same function serves both encode-time
    // ("compute and store") and decode-time ("recompute and compare").

    inline uint32_t
    superblock_compute_crc(const superblock& s) noexcept {
        constexpr uint32_t covered = offsetof(superblock, crc);
        return static_cast<uint32_t>(absl::ComputeCrc32c(
            absl::string_view(reinterpret_cast<const char*>(&s), covered)));
    }

    // ── Reason-aware status ─────────────────────────────────────────────────

    enum class superblock_status : uint8_t {
        ok = 0,
        bad_magic,
        bad_crc,
        bad_format_version,
    };

    inline const char*
    superblock_status_to_string(superblock_status s) {
        switch (s) {
        case superblock_status::ok:                 return "ok";
        case superblock_status::bad_magic:          return "bad_magic";
        case superblock_status::bad_crc:            return "bad_crc";
        case superblock_status::bad_format_version: return "bad_format_version";
        }
        return "unknown";
    }

    // ── inspect ────────────────────────────────────────────────────────────
    //
    // Validation order: magic → format_version → CRC. Magic is the
    // cheapest and most discriminating check (a TRIM'd LBA reads back as
    // zeros, which trips bad_magic immediately); CRC is the most
    // expensive and runs last.

    inline superblock_status
    inspect_superblock(const superblock& s) noexcept {
        if (s.magic != SUPERBLOCK_MAGIC)
            return superblock_status::bad_magic;
        if (s.format_version != SUPERBLOCK_FORMAT_VERSION_V1)
            return superblock_status::bad_format_version;
        if (s.crc != superblock_compute_crc(s))
            return superblock_status::bad_crc;
        return superblock_status::ok;
    }

    // ── A/B selection ──────────────────────────────────────────────────────
    //
    // The format layer is the *only* place that decides which of the two
    // on-disk superblocks to trust. Recovery / format / superblock-update
    // call this helper; they MUST NOT roll their own generation
    // comparison or fall-through "pick A on tie" logic.
    //
    // Rules (ODF §2.3 + recovery_and_wal_reclaim §2):
    //   1. Both valid, generations differ → pick the higher generation.
    //   2. Exactly one valid              → pick the valid one.
    //   3. Both invalid                   → return none.
    //   4. Both valid, generations equal but contents differ → return
    //      none. ODF §2.3 rule 5 says this should never happen — the
    //      write protocol guarantees it. If it does happen we treat the
    //      slot as corrupted rather than silently tie-breaking.
    //
    // Same-generation + bit-for-bit-identical contents (e.g. a freshly
    // formatted disk where neither slot has been updated yet) are not
    // considered a "tie": they are bytewise the same value, so returning
    // slot A is information-preserving and not a silent decision.

    struct superblock_choice {
        enum class source : uint8_t { a, b, none };

        const superblock* chosen;
        source            which;
    };

    inline superblock_choice
    choose_newer_superblock(const superblock& a, const superblock& b) noexcept {
        const bool a_ok = (inspect_superblock(a) == superblock_status::ok);
        const bool b_ok = (inspect_superblock(b) == superblock_status::ok);

        if (a_ok && b_ok) {
            if (a.generation > b.generation)
                return { &a, superblock_choice::source::a };
            if (b.generation > a.generation)
                return { &b, superblock_choice::source::b };
            // Generations equal — only acceptable if the bytes are
            // bitwise identical. Anything else is a write-protocol
            // violation per ODF §2.3 rule 5: bail rather than pick a
            // side.
            if (std::memcmp(&a, &b, sizeof(superblock)) == 0)
                return { &a, superblock_choice::source::a };
            return { nullptr, superblock_choice::source::none };
        }

        if (a_ok) return { &a, superblock_choice::source::a };
        if (b_ok) return { &b, superblock_choice::source::b };
        return { nullptr, superblock_choice::source::none };
    }

}

#endif //APPS_INCONEL_FORMAT_SUPERBLOCK_HH
