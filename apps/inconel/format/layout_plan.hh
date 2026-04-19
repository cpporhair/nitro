#ifndef APPS_INCONEL_FORMAT_LAYOUT_PLAN_HH
#define APPS_INCONEL_FORMAT_LAYOUT_PLAN_HH

#include <cstdint>
#include <stdexcept>
#include <string>

#include "./format_options.hh"
#include "./format_profile.hh"   // kMaxValueClassCount
#include "./superblock.hh"       // sizeof(superblock) sanity against lba_size
#include "./types.hh"            // paddr
#include "./wal.hh"              // WAL_SEGMENT_HEADER_SIZE, WAL_SEALED_TRAILER_SIZE, WAL_PUT_FIXED_BYTES

// ─────────────────────────────────────────────────────────────────────────────
// layout_plan — derived disk geometry fed to `build_superblock` and
// `make_formatted_storage`.
//
// `compute_layout` performs the arithmetic (with a bare minimum of
// pre-condition checks to avoid UB from /0); `validate_layout` is the single
// gate all post-derivation invariants flow through. Both throw
// std::invalid_argument with a readable reason on violation.
//
// The split lets callers recover structured diagnostics without re-running
// the arithmetic, and it keeps the arithmetic side branch-free for the
// happy path.
// ─────────────────────────────────────────────────────────────────────────────

namespace apps::inconel::format {

    struct layout_plan {
        // ── namespace shape ──
        uint32_t lba_size;
        uint64_t namespace_size;
        uint64_t total_lbas;

        // ── WAL area ──
        paddr    wal_base_paddr;        // {0, 2 + kReservedMetadataPagesV1}
        uint32_t wal_segment_size;
        uint32_t wal_segment_count;
        uint32_t wal_segment_lbas;      // wal_segment_size / lba_size

        // ── data area ──
        paddr    data_area_base_paddr;  // == wal_end_lba
        paddr    data_area_end_paddr;   // == total_lbas

        // ── tree params (from options, passed through) ──
        uint32_t tree_page_size;
        uint32_t shadow_slots_per_range;

        // ── value-class table (from options, passed through) ──
        uint8_t  value_class_count;
        uint32_t value_class_sizes[kMaxValueClassCount];
    };

    // ── reserved trailer footprint ─────────────────────────────────────────
    //
    // ODF §3.4: the sealed trailer is written in the last page(s) of a
    // segment. The number of LBA pages consumed rounds the raw trailer
    // struct size up to the next multiple of `lba_size`.

    constexpr uint64_t
    round_up_to_lba(uint32_t bytes, uint32_t lba_size) noexcept {
        return (static_cast<uint64_t>(bytes) + lba_size - 1) / lba_size * lba_size;
    }

    // ── compute_layout ─────────────────────────────────────────────────────
    //
    // Derives every field of `layout_plan` from `opts` + `namespace_size`.
    // Throws std::invalid_argument only on pre-conditions that would make
    // the arithmetic undefined (lba_size == 0, namespace_size == 0). Every
    // other invariant — including any derived value — is the responsibility
    // of `validate_layout`.

    inline layout_plan
    compute_layout(const format_options& opts, uint64_t namespace_size) {
        if (opts.lba_size == 0) {
            throw std::invalid_argument(
                "compute_layout: opts.lba_size is 0");
        }
        if (namespace_size == 0) {
            throw std::invalid_argument(
                "compute_layout: namespace_size is 0");
        }
        if (namespace_size % opts.lba_size != 0) {
            throw std::invalid_argument(
                "compute_layout: namespace_size is not a multiple of lba_size");
        }

        layout_plan L{};
        L.lba_size               = opts.lba_size;
        L.namespace_size         = namespace_size;
        L.total_lbas             = namespace_size / opts.lba_size;

        L.wal_base_paddr         = paddr{/*device_id=*/0,
                                         /*lba=*/ 2 + kReservedMetadataPagesV1};
        L.wal_segment_size       = opts.wal_segment_size;
        L.wal_segment_count      = opts.wal_segment_count;

        // wal_segment_lbas only meaningful when wal_segment_size is aligned;
        // validate_layout enforces that. Compute via integer division here
        // so the derived plan is self-consistent even if misaligned — the
        // caller sees it surface as a validation failure, not a silent
        // truncation.
        L.wal_segment_lbas       = opts.wal_segment_size / opts.lba_size;

        const uint64_t wal_end_lba =
            L.wal_base_paddr.lba +
            static_cast<uint64_t>(L.wal_segment_count) *
            static_cast<uint64_t>(L.wal_segment_lbas);

        L.data_area_base_paddr   = paddr{/*device_id=*/0, /*lba=*/wal_end_lba};
        L.data_area_end_paddr    = paddr{/*device_id=*/0, /*lba=*/L.total_lbas};

        L.tree_page_size         = opts.tree_page_size;
        L.shadow_slots_per_range = opts.shadow_slots_per_range;

        L.value_class_count      = opts.value_class_count;
        for (uint8_t i = 0; i < kMaxValueClassCount; ++i) {
            L.value_class_sizes[i] = opts.value_class_sizes[i];
        }

        return L;
    }

    // ── validate_layout ────────────────────────────────────────────────────
    //
    // Single source of truth for "is this plan legal under ODF §2 / §4.1 /
    // §6". Throws std::invalid_argument with a per-rule message on the
    // first violation. Exhaustive (no short-circuit beyond the first
    // failure), because for a test-only helper the order in which multiple
    // violations surface is irrelevant — callers either pass valid options
    // or fix them one at a time.

    inline void
    validate_layout(const layout_plan& L) {
        // ── namespace / lba_size ──
        if (L.lba_size == 0) {
            throw std::invalid_argument("validate_layout: lba_size is 0");
        }
        if (L.namespace_size == 0) {
            throw std::invalid_argument("validate_layout: namespace_size is 0");
        }
        if (L.namespace_size % L.lba_size != 0) {
            throw std::invalid_argument(
                "validate_layout: namespace_size is not a multiple of lba_size");
        }
        // One superblock must fit entirely within a single LBA (ODF §2.1:
        // SB A at LBA 0, SB B at LBA 1). A pathologically small `lba_size`
        // would split the struct across LBAs and break reads.
        if (L.lba_size < sizeof(superblock)) {
            throw std::invalid_argument(
                "validate_layout: lba_size smaller than sizeof(superblock) — "
                "a superblock slot must occupy exactly one LBA");
        }

        // ── WAL segment size ──
        if (L.wal_segment_size == 0) {
            throw std::invalid_argument(
                "validate_layout: wal_segment_size is 0");
        }
        if (L.wal_segment_size % L.lba_size != 0) {
            throw std::invalid_argument(
                "validate_layout: wal_segment_size is not a multiple of lba_size");
        }

        // A segment must hold at least: header + a trailer-sized reservation
        // at the tail + room for one minimum-sized PUT entry (zero-byte
        // key). ODF §3.6 phrases this around a policy MAX_KEY_LEN; the
        // format layer does not enforce the policy cap (see wal.hh §275
        // comment), so we ground the check on "at least one entry fits"
        // which is the true structural floor.
        const uint64_t trailer_reserved =
            round_up_to_lba(WAL_SEALED_TRAILER_SIZE, L.lba_size);
        const uint64_t entry_area =
            (L.wal_segment_size > WAL_SEGMENT_HEADER_SIZE + trailer_reserved)
                ? L.wal_segment_size - WAL_SEGMENT_HEADER_SIZE - trailer_reserved
                : 0;
        if (entry_area < WAL_PUT_FIXED_BYTES) {
            throw std::invalid_argument(
                "validate_layout: wal_segment_size leaves no room for even a "
                "zero-key PUT entry (header + trailer page already fill it)");
        }

        // ── WAL segment count ──
        if (L.wal_segment_count < kMinWalSegmentCount) {
            throw std::invalid_argument(
                "validate_layout: wal_segment_count below kMinWalSegmentCount");
        }

        // ── region boundaries ──
        // wal_base + wal_total_lbas must leave room for a non-empty data
        // area. wal_end_lba (encoded in data_area_base) past total_lbas
        // means WAL alone overflows the namespace.
        if (L.data_area_base_paddr.lba > L.total_lbas) {
            throw std::invalid_argument(
                "validate_layout: WAL area extends past the namespace");
        }
        if (L.data_area_base_paddr.lba >= L.data_area_end_paddr.lba) {
            throw std::invalid_argument(
                "validate_layout: data area is empty (WAL consumes everything "
                "after the metadata prefix)");
        }

        // ── tree params (ODF §4.1 / §10) ──
        if (L.tree_page_size == 0) {
            throw std::invalid_argument("validate_layout: tree_page_size is 0");
        }
        if (L.tree_page_size % L.lba_size != 0) {
            throw std::invalid_argument(
                "validate_layout: tree_page_size is not a multiple of lba_size");
        }
        if (L.shadow_slots_per_range == 0) {
            throw std::invalid_argument(
                "validate_layout: shadow_slots_per_range is 0");
        }

        // ── value-class table (ODF §5.2 / §6) ──
        if (L.value_class_count == 0) {
            throw std::invalid_argument(
                "validate_layout: value_class_count is 0");
        }
        if (L.value_class_count > kMaxValueClassCount) {
            throw std::invalid_argument(
                "validate_layout: value_class_count exceeds ODF §2.2 cap of 16");
        }
        uint32_t prev = 0;
        for (uint8_t i = 0; i < L.value_class_count; ++i) {
            const uint32_t cs = L.value_class_sizes[i];
            if (cs == 0) {
                throw std::invalid_argument(
                    "validate_layout: value_class_sizes contains 0");
            }
            if (cs <= prev) {
                throw std::invalid_argument(
                    "validate_layout: value_class_sizes is not strictly "
                    "ascending");
            }
            prev = cs;

            // ODF §5.2 / §6: each class must satisfy one of
            //   sub-LBA   (cs <  lba_size): lba_size % cs == 0
            //   LBA-equal (cs == lba_size)
            //   multi-LBA (cs >  lba_size): cs % lba_size == 0
            if (cs < L.lba_size) {
                if (L.lba_size % cs != 0) {
                    throw std::invalid_argument(
                        "validate_layout: sub-LBA value class does not "
                        "divide lba_size");
                }
            } else if (cs > L.lba_size) {
                if (cs % L.lba_size != 0) {
                    throw std::invalid_argument(
                        "validate_layout: multi-LBA value class is not a "
                        "multiple of lba_size");
                }
            }
            // cs == lba_size is always valid.
        }
    }

}

#endif //APPS_INCONEL_FORMAT_LAYOUT_PLAN_HH
