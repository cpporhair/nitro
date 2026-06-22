#ifndef APPS_INCONEL_YCSB_FORMAT_DEVICE_HH
#define APPS_INCONEL_YCSB_FORMAT_DEVICE_HH

#include <cstring>
#include <stdexcept>

#include "../format/format_profile.hh"
#include "../format/layout_plan.hh"
#include "../format/superblock_builder.hh"
#include "../nvme/real_device.hh"
#include "../recovery/sync_io.hh"

namespace apps::inconel::ycsb {

    [[nodiscard]] inline format::layout_plan
    bootstrap_layout_for_device(const nvme::real_device& device) {
        const auto& p = format::kBootstrapFormatProfile;
        format::layout_plan L{};
        L.lba_size = p.lba_size;
        L.namespace_size = device.size_bytes();
        L.total_lbas = device.total_logical_lbas(p.lba_size);
        L.wal_base_paddr = p.wal_base_paddr;
        L.wal_segment_size = p.wal_segment_size;
        L.wal_segment_count = p.wal_segment_count;
        L.wal_segment_lbas = p.wal_segment_size / p.lba_size;
        L.data_area_base_paddr = p.value_data_area_base;
        L.data_area_end_paddr = p.value_data_area_end;
        L.tree_page_size = p.tree_page_size;
        L.shadow_slots_per_range = p.shadow_slots_per_range;
        L.value_class_count = p.value_class_count;
        for (uint8_t i = 0; i < format::kMaxValueClassCount; ++i) {
            L.value_class_sizes[i] = p.value_class_sizes[i];
        }
        L.value_space_quantum_bytes = p.value_space_quantum_bytes;
        L.value_space_group_size_lbas = p.value_space_group_size_lbas;

        if (L.total_lbas < p.value_data_area_end.lba) {
            throw std::runtime_error(
                "inconel_ycsb: namespace too small for bootstrap profile");
        }
        format::validate_layout(L);
        return L;
    }

    inline void
    force_format_device(nvme::real_device& device, uint32_t core) {
        const auto layout = bootstrap_layout_for_device(device);
        recovery::sync_zero_logical_lbas(
            device,
            core,
            layout.wal_base_paddr.lba,
            static_cast<uint64_t>(layout.wal_segment_count) *
                layout.wal_segment_lbas,
            layout.lba_size,
            layout.lba_size);

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

}  // namespace apps::inconel::ycsb

#endif  // APPS_INCONEL_YCSB_FORMAT_DEVICE_HH
