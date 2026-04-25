#ifndef APPS_INCONEL_FORMAT_SUPERBLOCK_BUILDER_HH
#define APPS_INCONEL_FORMAT_SUPERBLOCK_BUILDER_HH

#include <cstdint>
#include <cstring>

#include "./format_profile.hh"   // kMaxValueClassCount
#include "./layout_plan.hh"
#include "./superblock.hh"
#include "./types.hh"

// ─────────────────────────────────────────────────────────────────────────────
// build_superblock — single entry point for constructing a superblock from
// a `layout_plan` + generation number.
//
// All ODF §2.2 fields are populated here, CRC is computed via the
// `superblock_compute_crc` helper already exposed by `superblock.hh`, and
// `root_base_paddr` is pinned to `{0, 0}` (the null-root sentinel: no tree
// has been written, recovery treats the leaf-record pool as empty).
//
// This builder is the only production path for materialising a superblock.
// Future root-change flush will reuse it by cloning a layout_plan from the
// in-memory catalog, bumping the generation, and overriding the
// root_base_paddr before calling. Keeping that single path means CRC,
// magic, and version management never fork.
// ─────────────────────────────────────────────────────────────────────────────

namespace apps::inconel::format {

    inline superblock
    build_superblock(const layout_plan& L, uint64_t generation) {
        superblock sb{};

        sb.magic                   = SUPERBLOCK_MAGIC;
        sb.format_version          = SUPERBLOCK_FORMAT_VERSION_V1;

        sb.namespace_size          = L.namespace_size;
        sb.lba_size                = L.lba_size;

        sb.tree_page_size          = L.tree_page_size;
        sb.shadow_slots_per_range  = L.shadow_slots_per_range;

        sb.wal_base_paddr          = L.wal_base_paddr;
        sb.wal_segment_size        = L.wal_segment_size;
        sb.wal_segment_count       = L.wal_segment_count;

        sb.data_area_base_paddr    = L.data_area_base_paddr;
        sb.data_area_end_paddr     = L.data_area_end_paddr;

        sb.value_size_class_count  = L.value_class_count;
        std::memset(sb.value_size_classes, 0, sizeof(sb.value_size_classes));
        for (uint8_t i = 0; i < L.value_class_count; ++i) {
            sb.value_size_classes[i] = L.value_class_sizes[i];
        }

        sb.value_space_quantum_bytes   = L.value_space_quantum_bytes;
        sb.value_space_group_size_lbas = L.value_space_group_size_lbas;

        // Null-root sentinel: format-time there is no tree to point at.
        // Recovery reads `root_base_paddr.lba == 0` and treats the leaf
        // record pool as empty (design_overview.md §2.4 / §12.2 step 2).
        sb.root_base_paddr         = paddr{/*device_id=*/0, /*lba=*/0};

        sb.generation              = generation;
        sb.crc                     = superblock_compute_crc(sb);

        return sb;
    }

}

#endif //APPS_INCONEL_FORMAT_SUPERBLOCK_BUILDER_HH
