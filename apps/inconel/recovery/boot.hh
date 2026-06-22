#ifndef APPS_INCONEL_RECOVERY_BOOT_HH
#define APPS_INCONEL_RECOVERY_BOOT_HH

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "../core/tree_geometry.hh"
#include "../format/format_profile.hh"
#include "../format/superblock.hh"
#include "../nvme/real_device.hh"
#include "./sync_io.hh"

namespace apps::inconel::recovery {

    constexpr uint32_t kBootSuperblockReadBytes = 4096;

    struct recovered_boot_state {
        format::format_profile profile{};
        core::tree_geometry tree_geometry{};
        format::superblock_choice::source superblock_source =
            format::superblock_choice::source::none;
        uint64_t superblock_generation = 0;
    };

    [[nodiscard]] inline core::tree_geometry
    tree_geometry_from_profile(const format::format_profile& profile) noexcept {
        return core::tree_geometry{
            .lba_size = profile.lba_size,
            .tree_page_size = profile.tree_page_size,
            .shadow_slots_per_range = profile.shadow_slots_per_range,
        };
    }

    [[nodiscard]] inline format::format_profile
    profile_from_superblock(const format::superblock& sb) {
        format::format_profile profile{};
        profile.lba_size = sb.lba_size;
        profile.value_data_area_base = sb.data_area_base_paddr;
        profile.value_data_area_end = sb.data_area_end_paddr;
        profile.value_class_count = sb.value_size_class_count;
        for (uint8_t i = 0; i < format::kMaxValueClassCount; ++i) {
            profile.value_class_sizes[i] = sb.value_size_classes[i];
        }
        profile.tree_page_size = sb.tree_page_size;
        profile.shadow_slots_per_range = sb.shadow_slots_per_range;
        profile.wal_base_paddr = sb.wal_base_paddr;
        profile.wal_segment_size = sb.wal_segment_size;
        profile.wal_segment_count = sb.wal_segment_count;
        profile.value_space_quantum_bytes = sb.value_space_quantum_bytes;
        profile.value_space_group_size_lbas =
            sb.value_space_group_size_lbas;

        if (!format::profile_is_self_consistent(profile)) {
            throw std::invalid_argument(
                "inconel recovery: superblock profile is not supported");
        }
        if (profile.lba_size != kBootSuperblockReadBytes) {
            throw std::invalid_argument(
                "inconel recovery: only 4096-byte boot LBA is supported "
                "in the initial recovery boot path");
        }
        return profile;
    }

    struct read_superblock_pair {
        format::superblock a{};
        format::superblock b{};
    };

    [[nodiscard]] inline read_superblock_pair
    read_superblock_pair_from_device(nvme::real_device& device,
                                     uint32_t core) {
        auto buf = make_zeroed_dma_buffer(
            kBootSuperblockReadBytes, kBootSuperblockReadBytes);
        read_superblock_pair out{};

        sync_read_logical_lbas(
            device, core, 0, 1, buf.get(), kBootSuperblockReadBytes);
        std::memcpy(&out.a, buf.get(), sizeof(out.a));

        sync_read_logical_lbas(
            device, core, 1, 1, buf.get(), kBootSuperblockReadBytes);
        std::memcpy(&out.b, buf.get(), sizeof(out.b));

        return out;
    }

    [[nodiscard]] inline const char*
    superblock_choice_source_to_string(
        format::superblock_choice::source source) noexcept {
        switch (source) {
        case format::superblock_choice::source::a: return "A";
        case format::superblock_choice::source::b: return "B";
        case format::superblock_choice::source::none: return "none";
        }
        return "unknown";
    }

    [[nodiscard]] inline bool
    wal_region_is_zero(nvme::real_device& device,
                       uint32_t core,
                       const format::format_profile& profile) {
        const uint64_t segment_lbas =
            profile.wal_segment_size / profile.lba_size;
        const uint64_t total_lbas =
            segment_lbas * static_cast<uint64_t>(profile.wal_segment_count);
        if (total_lbas == 0) {
            return true;
        }

        constexpr uint64_t kMaxChunkBytes = 1024ull * 1024ull;
        uint64_t chunk_lbas = kMaxChunkBytes / profile.lba_size;
        if (chunk_lbas == 0) {
            chunk_lbas = 1;
        }
        auto buf = make_zeroed_dma_buffer(
            static_cast<std::size_t>(chunk_lbas) * profile.lba_size,
            profile.lba_size);

        uint64_t cursor = profile.wal_base_paddr.lba;
        uint64_t remaining = total_lbas;
        while (remaining != 0) {
            const uint32_t now = static_cast<uint32_t>(
                remaining < chunk_lbas ? remaining : chunk_lbas);
            sync_read_logical_lbas(
                device, core, cursor, now, buf.get(), profile.lba_size);
            const std::size_t bytes =
                static_cast<std::size_t>(now) * profile.lba_size;
            if (!buffer_is_zero(buf.get(), bytes)) {
                return false;
            }
            cursor += now;
            remaining -= now;
        }
        return true;
    }

    [[nodiscard]] inline recovered_boot_state
    recover_empty_clean_boot(nvme::real_device& device, uint32_t core) {
        const auto pair = read_superblock_pair_from_device(device, core);
        const auto choice =
            format::choose_newer_superblock(pair.a, pair.b);
        if (choice.chosen == nullptr) {
            const auto a_status = format::inspect_superblock(pair.a);
            const auto b_status = format::inspect_superblock(pair.b);
            throw std::runtime_error(
                std::string("inconel recovery: no valid superblock "
                            "(A=") +
                format::superblock_status_to_string(a_status) +
                ", B=" +
                format::superblock_status_to_string(b_status) +
                "); rerun with --force-format only if the device may be "
                "destroyed");
        }

        if (choice.chosen->root_base_paddr.lba != 0) {
            throw std::runtime_error(
                "inconel recovery: non-empty tree recovery is not "
                "implemented in 064A");
        }

        auto profile = profile_from_superblock(*choice.chosen);
        if (!wal_region_is_zero(device, core, profile)) {
            throw std::runtime_error(
                "inconel recovery: non-empty WAL recovery is not "
                "implemented in 064A");
        }

        return recovered_boot_state{
            .profile = profile,
            .tree_geometry = tree_geometry_from_profile(profile),
            .superblock_source = choice.which,
            .superblock_generation = choice.chosen->generation,
        };
    }

}  // namespace apps::inconel::recovery

#endif  // APPS_INCONEL_RECOVERY_BOOT_HH
