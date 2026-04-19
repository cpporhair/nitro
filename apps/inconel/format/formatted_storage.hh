#ifndef APPS_INCONEL_FORMAT_FORMATTED_STORAGE_HH
#define APPS_INCONEL_FORMAT_FORMATTED_STORAGE_HH

#include <cstdint>
#include <cstring>
#include <memory>

#include "./format_options.hh"
#include "./layout_plan.hh"
#include "./superblock.hh"
#include "./superblock_builder.hh"

// ─────────────────────────────────────────────────────────────────────────────
// make_formatted_storage — test-only helper that materialises a byte buffer
// shaped like a freshly-formatted inconel disk.
//
// Usage:
//   auto buf = format::make_formatted_storage(opts, namespace_size);
//   mock_nvme::mock_device dev(std::move(buf), namespace_size, opts.lba_size);
//
// The returned buffer satisfies the recovery-input assumptions encoded in
// `design_overview.md` §12.2 / `on_disk_formats.md` §7:
//   - LBA 0 carries a valid superblock A (generation = 1)
//   - LBA 1 carries a valid superblock B (generation = 0)
//   - All other bytes are zero (matches the post-TRIM state a production
//     format would leave behind)
//   - `root_base_paddr == {0, 0}` signalling an empty tree
//
// Not a production mkfs path: no I/O, no device handle, no scheduler. When
// the real-nvme format command is built, it will likely reuse
// `layout_plan` + `build_superblock` but add FUA writes + TRIM through a
// device owner.
//
// Throws std::invalid_argument on any option / layout invariant violation
// (see `validate_layout` for the full list). Allocation happens AFTER all
// validation, so a rejected input produces no side effect.
// ─────────────────────────────────────────────────────────────────────────────

namespace apps::inconel::format {

    inline std::unique_ptr<char[]>
    make_formatted_storage(const format_options& opts,
                           uint64_t              namespace_size) {
        const layout_plan L = compute_layout(opts, namespace_size);
        validate_layout(L);

        // std::make_unique<char[]>(N) value-initialises to zero — that is
        // the entire basis for "post-TRIM state" downstream of the two
        // superblock memcpys.
        auto buf = std::make_unique<char[]>(namespace_size);

        const superblock sb_a = build_superblock(L, /*generation=*/1);
        const superblock sb_b = build_superblock(L, /*generation=*/0);

        std::memcpy(buf.get() + 0 * static_cast<uint64_t>(L.lba_size),
                    &sb_a, sizeof(sb_a));
        std::memcpy(buf.get() + 1 * static_cast<uint64_t>(L.lba_size),
                    &sb_b, sizeof(sb_b));

        return buf;
    }

}

#endif //APPS_INCONEL_FORMAT_FORMATTED_STORAGE_HH
