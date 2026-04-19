#ifndef APPS_INCONEL_FORMAT_FORMAT_OPTIONS_HH
#define APPS_INCONEL_FORMAT_FORMAT_OPTIONS_HH

#include <cstdint>

#include "./format_profile.hh"   // kMaxValueClassCount (ODF §2.2 cap of 16)

// ─────────────────────────────────────────────────────────────────────────────
// format_options — complete format-time input for make_formatted_storage().
//
// Scope (032 test-only helper):
//   - Carries every parameter that must land in a superblock A/B pair.
//   - Deliberately does NOT accept a `format_profile&`: profile carries
//     `value_data_area_base/end` which are runtime placeholders rather than
//     format-time boundaries. Making callers spell out the format-time
//     fields by hand prevents "forgot to overwrite the runtime placeholder"
//     drift.
//   - No `default_format_options(...)` factory. Test fixtures vary WAL
//     parameters widely (small / large / deliberately mis-sized for stress
//     cases); a shared default would mask fixture-specific intent. Aggregate
//     init is concise enough.
//
// Out of scope:
//   - `namespace_size` is a fixture decision ("how big is this mock SSD"),
//     passed separately to make_formatted_storage().
//   - No I/O, no mock_nvme dependency, no runtime dependency.
// ─────────────────────────────────────────────────────────────────────────────

namespace apps::inconel::format {

    // v1 hardcodes reserved-metadata-pages to 0: LBA 2 sits directly against
    // the WAL area. The concept (ODF §2.4: space reserved for future multi-disk
    // redundancy / format extensions / diagnostic pages) is not exercised in
    // the current test scope. When a real-nvme format command needs this
    // tunable, promote it to a field on `format_options` then.
    constexpr uint32_t kReservedMetadataPagesV1 = 0;

    // Minimum accepted wal_segment_count. Only guards against 0 (which would
    // produce a zero-byte WAL area and immediately break downstream layout
    // math). A "sensible runtime floor" (e.g. ≥ front_sched_count × 2 for
    // rotation) is not a format-layer concern — stress fixtures that want a
    // single-segment WAL must be allowed through.
    constexpr uint32_t kMinWalSegmentCount = 1;

    struct format_options {
        // ── superblock format-version-pinned fields ──
        uint32_t lba_size;
        uint32_t tree_page_size;
        uint32_t shadow_slots_per_range;

        // ── value-class table (ODF §2.2 caps at 16) ──
        uint8_t  value_class_count;
        uint32_t value_class_sizes[kMaxValueClassCount];

        // ── WAL area ──
        uint32_t wal_segment_size;
        uint32_t wal_segment_count;   // >= kMinWalSegmentCount
    };

}

#endif //APPS_INCONEL_FORMAT_FORMAT_OPTIONS_HH
