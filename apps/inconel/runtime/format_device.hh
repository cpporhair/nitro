#ifndef APPS_INCONEL_RUNTIME_FORMAT_DEVICE_HH
#define APPS_INCONEL_RUNTIME_FORMAT_DEVICE_HH

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../format/format_options.hh"
#include "../format/format_profile.hh"
#include "../format/layout_plan.hh"
#include "../format/superblock_builder.hh"
#include "../nvme/real_device.hh"
#include "../recovery/boot.hh"
#include "../recovery/sync_io.hh"

namespace apps::inconel::runtime {

    [[nodiscard]] inline format::format_options
    default_format_options_for_runtime() {
        const auto& p = format::kBootstrapFormatProfile;
        format::format_options opts{};
        opts.lba_size = p.lba_size;
        opts.tree_page_size = p.tree_page_size;
        opts.shadow_slots_per_range = p.shadow_slots_per_range;
        opts.value_class_count = p.value_class_count;
        for (uint8_t i = 0; i < format::kMaxValueClassCount; ++i) {
            opts.value_class_sizes[i] = p.value_class_sizes[i];
        }
        opts.wal_segment_size = p.wal_segment_size;
        opts.wal_segment_count = p.wal_segment_count;
        opts.value_space_quantum_bytes = p.value_space_quantum_bytes;
        opts.value_space_group_size_lbas = p.value_space_group_size_lbas;
        return opts;
    }

    struct format_disk_policy {
        bool require_deallocate = true;
        bool require_deallocate_read_zero = true;
        bool allow_zero_write_fallback = false;
        bool verify_zero_after_deallocate = true;
        uint32_t zero_verify_sample_lbas = 8;
        uint64_t trim_chunk_lbas = 0;
    };

    struct format_disk_request {
        format::format_options options = default_format_options_for_runtime();
        format_disk_policy policy = {};
    };

    enum class format_disk_clear_method : uint8_t {
        deallocate,
        zero_write_fallback,
    };

    struct formatted_disk_state {
        format::layout_plan layout{};
        format::format_profile profile{};
        recovery::recovered_runtime_state clean_runtime{};
        format::superblock_choice::source active_superblock_source =
            format::superblock_choice::source::none;
        uint64_t superblock_generation = 0;
        format_disk_clear_method clear_method =
            format_disk_clear_method::deallocate;
    };

    [[nodiscard]] inline const char*
    format_disk_clear_method_name(format_disk_clear_method method) noexcept {
        switch (method) {
        case format_disk_clear_method::deallocate: return "deallocate";
        case format_disk_clear_method::zero_write_fallback:
            return "zero-write-fallback";
        }
        return "unknown";
    }

    [[nodiscard]] inline format::format_profile
    profile_from_layout(const format::layout_plan& layout) {
        format::format_profile profile{};
        profile.lba_size = layout.lba_size;
        profile.value_data_area_base = layout.data_area_base_paddr;
        profile.value_data_area_end = layout.data_area_end_paddr;
        profile.value_class_count = layout.value_class_count;
        for (uint8_t i = 0; i < format::kMaxValueClassCount; ++i) {
            profile.value_class_sizes[i] = layout.value_class_sizes[i];
        }
        profile.tree_page_size = layout.tree_page_size;
        profile.shadow_slots_per_range = layout.shadow_slots_per_range;
        profile.wal_base_paddr = layout.wal_base_paddr;
        profile.wal_segment_size = layout.wal_segment_size;
        profile.wal_segment_count = layout.wal_segment_count;
        profile.value_space_quantum_bytes =
            layout.value_space_quantum_bytes;
        profile.value_space_group_size_lbas =
            layout.value_space_group_size_lbas;
        if (!format::profile_is_self_consistent(profile)) {
            throw std::invalid_argument(
                "inconel runtime: formatted profile is not self-consistent");
        }
        return profile;
    }

    inline void
    validate_format_device_geometry(const nvme::real_device& device,
                                    const format::layout_plan& layout) {
        if (layout.lba_size != recovery::kBootSuperblockReadBytes) {
            throw std::invalid_argument(
                "inconel runtime: force-format currently requires a "
                "4096-byte boot LBA");
        }
        if (device.sector_size() == 0 ||
            layout.lba_size % device.sector_size() != 0) {
            throw std::invalid_argument(
                "inconel runtime: format LBA size is not sector aligned");
        }
        if (device.total_logical_lbas(layout.lba_size) !=
            layout.total_lbas) {
            throw std::invalid_argument(
                "inconel runtime: computed layout total_lbas does not match "
                "the device geometry");
        }
    }

    [[nodiscard]] inline format_disk_clear_method
    clear_namespace_for_format(nvme::real_device& device,
                               uint32_t core,
                               const format::layout_plan& layout,
                               const format_disk_policy& policy) {
        const bool supports_deallocate =
            device.namespace_supports_deallocate();
        const bool deallocate_reads_zero = device.deallocate_reads_zero();
        const bool can_deallocate =
            supports_deallocate &&
            (!policy.require_deallocate_read_zero || deallocate_reads_zero);

        if (can_deallocate) {
            if (policy.trim_chunk_lbas == 0) {
                recovery::sync_trim_logical_lbas(
                    device, core, 0, layout.total_lbas, layout.lba_size);
            } else {
                uint64_t cursor = 0;
                uint64_t remaining = layout.total_lbas;
                while (remaining != 0) {
                    const uint64_t now =
                        std::min(remaining, policy.trim_chunk_lbas);
                    recovery::sync_trim_logical_lbas(
                        device, core, cursor, now, layout.lba_size);
                    cursor += now;
                    remaining -= now;
                }
            }
            recovery::sync_flush(device, core);
            return format_disk_clear_method::deallocate;
        }

        if (policy.allow_zero_write_fallback) {
            recovery::sync_zero_logical_lbas(
                device,
                core,
                0,
                layout.total_lbas,
                layout.lba_size,
                layout.lba_size);
            recovery::sync_flush(device, core);
            return format_disk_clear_method::zero_write_fallback;
        }

        if (policy.require_deallocate && !supports_deallocate) {
            throw std::runtime_error(
                "inconel runtime: force-format requires NVMe DEALLOCATE, "
                "but the namespace does not advertise support");
        }
        if (policy.require_deallocate_read_zero && !deallocate_reads_zero) {
            throw std::runtime_error(
                "inconel runtime: force-format requires deallocated LBAs to "
                "read as zero");
        }
        throw std::runtime_error(
            "inconel runtime: no permitted full-device clear method");
    }

    inline void
    add_zero_verify_lba(std::vector<uint64_t>& lbas,
                        uint64_t total_lbas,
                        uint64_t lba) {
        if (lba < total_lbas) {
            lbas.push_back(lba);
        }
    }

    inline void
    verify_selected_lbas_are_zero(nvme::real_device& device,
                                  uint32_t core,
                                  const format::layout_plan& layout,
                                  uint32_t sample_count) {
        std::vector<uint64_t> lbas;
        lbas.reserve(static_cast<std::size_t>(sample_count) + 8);
        add_zero_verify_lba(lbas, layout.total_lbas, 0);
        add_zero_verify_lba(lbas, layout.total_lbas, 1);
        add_zero_verify_lba(
            lbas, layout.total_lbas, layout.wal_base_paddr.lba);
        const uint64_t wal_lbas =
            static_cast<uint64_t>(layout.wal_segment_count) *
            layout.wal_segment_lbas;
        if (wal_lbas != 0) {
            add_zero_verify_lba(
                lbas,
                layout.total_lbas,
                layout.wal_base_paddr.lba + wal_lbas - 1);
        }
        add_zero_verify_lba(
            lbas, layout.total_lbas, layout.data_area_base_paddr.lba);
        if (layout.data_area_end_paddr.lba != 0) {
            add_zero_verify_lba(
                lbas, layout.total_lbas, layout.data_area_end_paddr.lba - 1);
        }
        if (sample_count != 0 && layout.total_lbas != 0) {
            const uint64_t stride = std::max<uint64_t>(
                1,
                layout.total_lbas /
                    (static_cast<uint64_t>(sample_count) + 1));
            for (uint32_t i = 1; i <= sample_count; ++i) {
                add_zero_verify_lba(
                    lbas,
                    layout.total_lbas,
                    std::min(layout.total_lbas - 1,
                             stride * static_cast<uint64_t>(i)));
            }
        }

        std::sort(lbas.begin(), lbas.end());
        lbas.erase(std::unique(lbas.begin(), lbas.end()), lbas.end());

        auto buf =
            recovery::make_zeroed_dma_buffer(layout.lba_size, layout.lba_size);
        for (uint64_t lba : lbas) {
            recovery::sync_read_logical_lbas(
                device, core, lba, 1, buf.get(), layout.lba_size);
            if (!recovery::buffer_is_zero(buf.get(), layout.lba_size)) {
                throw std::runtime_error(
                    "inconel runtime: force-format zero verification failed "
                    "at logical LBA " +
                    std::to_string(lba));
            }
        }
    }

    inline void
    write_format_superblocks(nvme::real_device& device,
                             uint32_t core,
                             const format::layout_plan& layout) {
        auto sb_a = format::build_superblock(layout, 1);
        auto sb_b = format::build_superblock(layout, 0);

        auto buf = recovery::make_zeroed_dma_buffer(
            layout.lba_size, layout.lba_size);
        std::memcpy(buf.get(), &sb_a, sizeof(sb_a));
        recovery::sync_write_logical_lbas(
            device, core, 0, 1, buf.get(), layout.lba_size);

        std::memset(buf.get(), 0, layout.lba_size);
        std::memcpy(buf.get(), &sb_b, sizeof(sb_b));
        recovery::sync_write_logical_lbas(
            device, core, 1, 1, buf.get(), layout.lba_size);
        recovery::sync_flush(device, core);
    }

    inline void
    verify_format_superblocks(nvme::real_device& device, uint32_t core) {
        const auto pair = recovery::read_superblock_pair_from_device(
            device, core);
        const auto choice =
            format::choose_newer_superblock(pair.a, pair.b);
        if (choice.chosen == nullptr ||
            choice.which != format::superblock_choice::source::a ||
            choice.chosen->generation != 1) {
            throw std::runtime_error(
                "inconel runtime: force-format superblock readback did not "
                "select fresh slot A");
        }
    }

    [[nodiscard]] inline formatted_disk_state
    format_disk(nvme::real_device& device,
                uint32_t core,
                const format_disk_request& req = {}) {
        auto layout = format::compute_layout(req.options, device.size_bytes());
        format::validate_layout(layout);
        validate_format_device_geometry(device, layout);

        const auto clear_method =
            clear_namespace_for_format(device, core, layout, req.policy);
        if (req.policy.verify_zero_after_deallocate) {
            verify_selected_lbas_are_zero(
                device, core, layout, req.policy.zero_verify_sample_lbas);
        }

        write_format_superblocks(device, core, layout);
        verify_format_superblocks(device, core);

        auto profile = profile_from_layout(layout);
        auto clean_runtime = recovery::make_empty_runtime_state(
            profile, format::superblock_choice::source::a);
        return formatted_disk_state{
            .layout = layout,
            .profile = profile,
            .clean_runtime = std::move(clean_runtime),
            .active_superblock_source = format::superblock_choice::source::a,
            .superblock_generation = 1,
            .clear_method = clear_method,
        };
    }

    [[nodiscard]] inline formatted_disk_state
    force_format_device(nvme::real_device& device, uint32_t core) {
        return format_disk(device, core, format_disk_request{});
    }

}  // namespace apps::inconel::runtime

#endif  // APPS_INCONEL_RUNTIME_FORMAT_DEVICE_HH
