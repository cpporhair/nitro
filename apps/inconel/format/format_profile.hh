#ifndef APPS_INCONEL_FORMAT_FORMAT_PROFILE_HH
#define APPS_INCONEL_FORMAT_FORMAT_PROFILE_HH

#include <cstdint>
#include <span>

#include "./types.hh"

// ─────────────────────────────────────────────────────────────────────────────
// format_profile — bring-up-era single source of truth for the subset of
// ODF §4.1 parameters the current runtime actually consumes.
//
// Scope (step 017 / INC-034):
//   - Pins `lba_size`, `value_data_area_base`, `value_data_area_end`, and
//     `value_class_sizes` to one compile-time constant (`kBootstrapFormatProfile`)
//     so they stop leaking onto `runtime::build_options` / `runtime::start_options`
//     as per-run configuration.
//   - Not a runtime configuration surface. There is exactly one instance, and
//     the standard build path reads from it; tests that need a different shape
//     must go through a dedicated harness, not through public options.
//
// Out of scope for v1:
//   - Superblock / WAL / tree page parameters. They already live in their own
//     format headers (`format/superblock.hh`, `format/wal.hh`, `format/tree_page.hh`).
//   - Dynamic profile loading. Once INC-031 (superblock POD) and INC-035
//     (format/recovery) land, the live runtime will populate the same fields
//     from the superblock it just read, and this constant will degrade to
//     "default template used when formatting a fresh disk".
//
// Validation:
//   - `profile_is_self_consistent` encodes the shape invariants from ODF §2,
//     §4.1, and §6, and is used in a `static_assert` at the bottom of this
//     file so `kBootstrapFormatProfile` drift is caught at compile time.
//   - Runtime fail-fast with readable diagnostics lives in
//     `runtime::validate_build_inputs`; that layer also cross-checks the
//     profile against the live device. External profile / device validation
//     must never be reduced to `assert` (see the step 017 plan).
// ─────────────────────────────────────────────────────────────────────────────

namespace apps::inconel::format {

    // ODF §2.2 caps `value_size_classes` at 16 entries. Pinning the same limit
    // here keeps the profile storage bounded and lets it live entirely inline
    // (no heap, no dynamic allocation) so it can be a proper constexpr value.
    constexpr uint8_t kMaxValueClassCount = 16;

    struct format_profile {
        uint32_t lba_size;
        paddr    value_data_area_base;
        paddr    value_data_area_end;

        // value_class_count is the live prefix of value_class_sizes; entries
        // beyond `[0, value_class_count)` are unused padding. Keeping the
        // storage as a raw array (not std::array) matches the ODF §2 superblock
        // layout helper and avoids any brace-elision subtleties in constexpr
        // aggregate init.
        uint8_t  value_class_count;
        uint32_t value_class_sizes[kMaxValueClassCount];

        constexpr std::span<const uint32_t>
        class_sizes() const noexcept {
            return std::span<const uint32_t>(value_class_sizes, value_class_count);
        }
    };

    // ── profile_is_self_consistent ──────────────────────────────────────────
    //
    // Compile-time predicate used to pin `kBootstrapFormatProfile` via
    // `static_assert`. The rules mirror the ones runtime::validate_build_inputs
    // raises as throws — this version just collapses them into a single bool
    // because `static_assert` can't print per-rule diagnostics.
    //
    // Rules (ODF §2, §4.1, §6):
    //   1. lba_size > 0
    //   2. value_data_area_base.device_id == value_data_area_end.device_id
    //   3. value_data_area_base.lba  <  value_data_area_end.lba
    //   4. value_class_count in [1, kMaxValueClassCount]
    //   5. every active entry is > 0
    //   6. active entries are strictly ascending
    //   7. every active entry satisfies value_object alignment:
    //        - sub-LBA  (cs <  lba_size): lba_size % cs == 0
    //        - LBA-equal (cs == lba_size): always ok
    //        - multi-LBA (cs >  lba_size): cs % lba_size == 0

    constexpr bool
    profile_is_self_consistent(const format_profile& p) noexcept {
        if (p.lba_size == 0) return false;
        if (p.value_data_area_base.device_id != p.value_data_area_end.device_id)
            return false;
        if (!(p.value_data_area_base.lba < p.value_data_area_end.lba))
            return false;
        if (p.value_class_count == 0 || p.value_class_count > kMaxValueClassCount)
            return false;

        uint32_t prev = 0;
        for (uint8_t i = 0; i < p.value_class_count; ++i) {
            const uint32_t cs = p.value_class_sizes[i];
            if (cs == 0) return false;
            if (cs <= prev) return false;
            prev = cs;

            if (cs < p.lba_size) {
                if (p.lba_size % cs != 0) return false;
            } else if (cs > p.lba_size) {
                if (cs % p.lba_size != 0) return false;
            }
            // cs == p.lba_size is always a valid LBA-equal class.
        }
        return true;
    }

    // ── Bootstrap profile ───────────────────────────────────────────────────
    //
    // v1 dev / bring-up defaults. The constants are deliberately aligned with
    // the mock_nvme harness the current test suite uses so the standard build
    // path stays drop-in compatible.
    //
    // Any change here must also keep `profile_is_self_consistent` happy — the
    // `static_assert` below will fire on drift.

    inline constexpr format_profile kBootstrapFormatProfile = {
        .lba_size             = 4096,
        .value_data_area_base = paddr{0, 4000},
        .value_data_area_end  = paddr{0, 8000},
        .value_class_count    = 5,
        .value_class_sizes    = { 64, 256, 1024, 4096, 16384 },
    };

    static_assert(profile_is_self_consistent(kBootstrapFormatProfile),
                  "kBootstrapFormatProfile must satisfy ODF §2/§4.1/§6 rules");

}

#endif //APPS_INCONEL_FORMAT_FORMAT_PROFILE_HH
