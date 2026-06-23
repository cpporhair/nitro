#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/superblock.hh"
#include "apps/inconel/format/tree_page.hh"
#include "apps/inconel/format/types.hh"
#include "apps/inconel/format/wal.hh"
#include "apps/inconel/recovery/boot.hh"
#include "apps/inconel/tree/page_builder.hh"

using namespace apps::inconel;

namespace {

constexpr uint32_t kCore = 0;

class ram_recovery_device {
public:
    ram_recovery_device(uint64_t logical_lbas, uint32_t lba_size)
        : lba_size_(lba_size)
        , bytes_(logical_lbas * static_cast<uint64_t>(lba_size), char{0}) {}

    [[nodiscard]] uint32_t
    sector_size() const noexcept {
        return lba_size_;
    }

    [[nodiscard]] uint64_t
    size_bytes() const noexcept {
        return bytes_.size();
    }

    void
    sync_read_logical_lbas(uint32_t /*core*/,
                           uint64_t logical_lba,
                           uint32_t logical_lba_count,
                           void* data,
                           uint32_t logical_lba_size) {
        copy_out(logical_lba, logical_lba_count, data, logical_lba_size);
    }

    void
    sync_write_logical_lbas(uint32_t /*core*/,
                            uint64_t logical_lba,
                            uint32_t logical_lba_count,
                            const void* data,
                            uint32_t logical_lba_size,
                            uint32_t /*io_flags*/) {
        copy_in(logical_lba, logical_lba_count, data, logical_lba_size);
    }

    void
    sync_flush(uint32_t /*core*/) noexcept {}

private:
    uint32_t lba_size_ = 0;
    std::vector<char> bytes_;

    [[nodiscard]] uint64_t
    byte_offset(uint64_t logical_lba, uint32_t logical_lba_size) const {
        if (logical_lba_size != lba_size_) {
            throw std::runtime_error("ram device: unexpected logical LBA size");
        }
        const uint64_t off = logical_lba * static_cast<uint64_t>(lba_size_);
        if (off > bytes_.size()) {
            throw std::runtime_error("ram device: LBA offset out of range");
        }
        return off;
    }

    [[nodiscard]] uint64_t
    byte_count(uint32_t logical_lba_count,
               uint32_t logical_lba_size) const {
        if (logical_lba_size != lba_size_) {
            throw std::runtime_error("ram device: unexpected logical LBA size");
        }
        return static_cast<uint64_t>(logical_lba_count) * lba_size_;
    }

    void
    copy_out(uint64_t logical_lba,
             uint32_t logical_lba_count,
             void* data,
             uint32_t logical_lba_size) const {
        const uint64_t off = byte_offset(logical_lba, logical_lba_size);
        const uint64_t len = byte_count(logical_lba_count, logical_lba_size);
        if (len > bytes_.size() - off) {
            throw std::runtime_error("ram device: read out of range");
        }
        std::memcpy(data, bytes_.data() + off, len);
    }

    void
    copy_in(uint64_t logical_lba,
            uint32_t logical_lba_count,
            const void* data,
            uint32_t logical_lba_size) {
        const uint64_t off = byte_offset(logical_lba, logical_lba_size);
        const uint64_t len = byte_count(logical_lba_count, logical_lba_size);
        if (len > bytes_.size() - off) {
            throw std::runtime_error("ram device: write out of range");
        }
        std::memcpy(bytes_.data() + off, data, len);
    }
};

[[nodiscard]] format::format_profile
test_profile(uint32_t shadow_slots_per_range = 1) {
    format::format_profile p{};
    p.lba_size = 4096;
    p.value_data_area_base = format::paddr{0, 64};
    p.value_data_area_end = format::paddr{0, 4096};
    p.value_class_count = 3;
    p.value_class_sizes[0] = 64;
    p.value_class_sizes[1] = 256;
    p.value_class_sizes[2] = 4096;
    p.tree_page_size = 4096;
    p.shadow_slots_per_range = shadow_slots_per_range;
    p.wal_base_paddr = format::paddr{0, 8};
    p.wal_segment_size = 64u * 1024u;
    p.wal_segment_count = 2;
    p.value_space_quantum_bytes = 64;
    p.value_space_group_size_lbas = (64u * 1024u * 1024u) / 4096u;
    CHECK(format::profile_is_self_consistent(p));
    return p;
}

[[nodiscard]] format::value_ref
vr(uint64_t lba, uint32_t len = 32) {
    return format::value_ref{
        .base = format::paddr{0, lba},
        .byte_offset = 0,
        .len = len,
        .flags = 0,
    };
}

[[nodiscard]] bool
same_vr(const format::value_ref& lhs,
        const format::value_ref& rhs) noexcept {
    return std::memcmp(&lhs, &rhs, sizeof(format::value_ref)) == 0;
}

[[nodiscard]] format::superblock
make_superblock(const format::format_profile& profile,
                uint64_t generation,
                format::paddr root,
                uint64_t namespace_size) {
    format::superblock sb{};
    sb.magic = format::SUPERBLOCK_MAGIC;
    sb.format_version = format::SUPERBLOCK_FORMAT_VERSION_V1;
    sb.namespace_size = namespace_size;
    sb.lba_size = profile.lba_size;
    sb.tree_page_size = profile.tree_page_size;
    sb.shadow_slots_per_range = profile.shadow_slots_per_range;
    sb.wal_base_paddr = profile.wal_base_paddr;
    sb.wal_segment_size = profile.wal_segment_size;
    sb.wal_segment_count = profile.wal_segment_count;
    sb.data_area_base_paddr = profile.value_data_area_base;
    sb.data_area_end_paddr = profile.value_data_area_end;
    sb.value_size_class_count = profile.value_class_count;
    for (uint8_t i = 0; i < profile.value_class_count; ++i) {
        sb.value_size_classes[i] = profile.value_class_sizes[i];
    }
    sb.value_space_quantum_bytes = profile.value_space_quantum_bytes;
    sb.value_space_group_size_lbas = profile.value_space_group_size_lbas;
    sb.root_base_paddr = root;
    sb.generation = generation;
    sb.crc = format::superblock_compute_crc(sb);
    return sb;
}

void
write_lbas(ram_recovery_device& device,
           uint64_t lba,
           std::span<const char> bytes,
           uint32_t lba_size) {
    CHECK(bytes.size() % lba_size == 0);
    device.sync_write_logical_lbas(
        kCore,
        lba,
        static_cast<uint32_t>(bytes.size() / lba_size),
        bytes.data(),
        lba_size,
        0);
}

void
read_lbas(ram_recovery_device& device,
          uint64_t lba,
          std::span<char> bytes,
          uint32_t lba_size) {
    CHECK(bytes.size() % lba_size == 0);
    device.sync_read_logical_lbas(
        kCore,
        lba,
        static_cast<uint32_t>(bytes.size() / lba_size),
        bytes.data(),
        lba_size);
}

[[nodiscard]] bool
range_is_zero(ram_recovery_device& device,
              uint64_t lba,
              uint32_t lba_count,
              uint32_t lba_size) {
    std::vector<char> bytes(
        static_cast<std::size_t>(lba_count) * lba_size);
    read_lbas(device, lba, bytes, lba_size);
    return std::all_of(
        bytes.begin(), bytes.end(), [](char c) { return c == 0; });
}

void
write_superblock_pair(ram_recovery_device& device,
                      const format::format_profile& profile,
                      format::paddr root) {
    const auto sb_a =
        make_superblock(profile, 1, root, device.size_bytes());
    const auto sb_b =
        make_superblock(profile, 0, format::paddr{0, 0}, device.size_bytes());

    std::vector<char> page(profile.lba_size, char{0});
    std::memcpy(page.data(), &sb_a, sizeof(sb_a));
    write_lbas(device, 0, page, profile.lba_size);

    std::fill(page.begin(), page.end(), char{0});
    std::memcpy(page.data(), &sb_b, sizeof(sb_b));
    write_lbas(device, 1, page, profile.lba_size);
}

struct seed_record {
    std::string key;
    uint64_t data_ver = 0;
    format::record_kind kind = format::record_kind::value;
    format::value_ref value{};
};

void
write_leaf(ram_recovery_device& device,
           const format::format_profile& profile,
           format::paddr leaf_range,
           std::span<const seed_record> records) {
    std::vector<char> page(profile.tree_page_size, char{0});
    tree::leaf_page_builder builder;
    builder.init(page.data(), profile.tree_page_size);
    for (const auto& rec : records) {
        bool ok = false;
        if (rec.kind == format::record_kind::value) {
            ok = builder.add_value(rec.key, rec.data_ver, rec.value);
        } else {
            ok = builder.add_tombstone(rec.key, rec.data_ver);
        }
        CHECK(ok);
    }
    builder.finalize();
    write_lbas(device, leaf_range.lba, page, profile.lba_size);
}

void
write_leaf_slot(ram_recovery_device& device,
                const format::format_profile& profile,
                const core::tree_geometry& geom,
                format::paddr leaf_range,
                uint32_t slot_index,
                std::span<const seed_record> records) {
    std::vector<char> page(profile.tree_page_size, char{0});
    tree::leaf_page_builder builder;
    builder.init(page.data(), profile.tree_page_size);
    for (const auto& rec : records) {
        bool ok = false;
        if (rec.kind == format::record_kind::value) {
            ok = builder.add_value(rec.key, rec.data_ver, rec.value);
        } else {
            ok = builder.add_tombstone(rec.key, rec.data_ver);
        }
        CHECK(ok);
    }
    builder.finalize();
    const auto slot = geom.slot_paddr(leaf_range, slot_index);
    write_lbas(device, slot.lba, page, profile.lba_size);
}

void
write_root_internal(ram_recovery_device& device,
                    const format::format_profile& profile,
                    format::paddr root_range,
                    std::string_view separator,
                    format::paddr left_child,
                    format::paddr right_child) {
    std::vector<char> page(profile.tree_page_size, char{0});
    tree::internal_page_builder builder;
    builder.init(page.data(), profile.tree_page_size);
    CHECK(builder.add_child(separator, left_child));
    builder.set_rightmost_child(right_child);
    builder.finalize();
    write_lbas(device, root_range.lba, page, profile.lba_size);
}

void
append_wal_put(std::vector<char>& segment,
               uint32_t& cursor,
               uint32_t segment_gen,
               uint64_t lsn,
               uint32_t entry_count,
               std::string_view key,
               const format::value_ref& value) {
    uint32_t total_len = 0;
    const auto status = format::encode_wal_put_entry(
        std::span<char>{segment}.subspan(cursor),
        segment_gen,
        lsn,
        entry_count,
        key,
        value,
        &total_len);
    CHECK(status == format::wal_entry_encode_status::ok);
    cursor += total_len;
}

void
append_wal_delete(std::vector<char>& segment,
                  uint32_t& cursor,
                  uint32_t segment_gen,
                  uint64_t lsn,
                  uint32_t entry_count,
                  std::string_view key) {
    uint32_t total_len = 0;
    const auto status = format::encode_wal_delete_entry(
        std::span<char>{segment}.subspan(cursor),
        segment_gen,
        lsn,
        entry_count,
        key,
        &total_len);
    CHECK(status == format::wal_entry_encode_status::ok);
    cursor += total_len;
}

void
write_wal_delta(ram_recovery_device& device,
                const format::format_profile& profile,
                const format::value_ref& updated_value) {
    constexpr uint32_t kSegmentGen = 1;
    std::vector<char> segment(profile.wal_segment_size, char{0});
    auto header =
        format::make_wal_segment_header(0, 0, 0, kSegmentGen);
    std::memcpy(segment.data(), &header, sizeof(header));

    uint32_t cursor = format::WAL_SEGMENT_HEADER_SIZE;
    append_wal_put(
        segment, cursor, kSegmentGen, 20, 2, "key-b", updated_value);
    append_wal_delete(segment, cursor, kSegmentGen, 20, 2, "key-c");
    append_wal_delete(segment, cursor, kSegmentGen, 30, 1, "key-e");

    write_lbas(
        device,
        profile.wal_base_paddr.lba,
        segment,
        profile.lba_size);
}

[[nodiscard]] std::map<std::string, recovery::recovered_tree_record>
records_by_key(const recovery::recovered_tree_scan& scan) {
    std::map<std::string, recovery::recovered_tree_record> out;
    for (const auto& rec : scan.records) {
        CHECK(out.emplace(rec.key, rec).second);
    }
    return out;
}

void
expect_value(const std::map<std::string, recovery::recovered_tree_record>& m,
             std::string_view key,
             uint64_t data_ver,
             const format::value_ref& value) {
    auto it = m.find(std::string(key));
    CHECK(it != m.end());
    CHECK(it->second.kind == format::record_kind::value);
    CHECK(it->second.data_ver == data_ver);
    CHECK(same_vr(it->second.vr, value));
}

void
expect_tombstone(
    const std::map<std::string, recovery::recovered_tree_record>& m,
    std::string_view key,
    uint64_t data_ver) {
    auto it = m.find(std::string(key));
    CHECK(it != m.end());
    CHECK(it->second.kind == format::record_kind::tombstone);
    CHECK(it->second.data_ver == data_ver);
}

bool
live_extents_contain(const std::vector<value::live_value_extent>& extents,
                     const format::value_ref& value) {
    return std::any_of(
        extents.begin(),
        extents.end(),
        [&](const value::live_value_extent& e) {
            return e.base == value.base &&
                   e.byte_offset == value.byte_offset &&
                   e.len == value.len;
        });
}

void
run_existing_tree_wal_update_delete_recovery() {
    const auto profile = test_profile();
    const auto geom = recovery::tree_geometry_from_profile(profile);
    ram_recovery_device device(
        profile.value_data_area_end.lba, profile.lba_size);

    const format::paddr root{0, 64};
    const format::paddr leaf_a{0, 65};
    const format::paddr leaf_b{0, 66};
    const auto old_a = vr(4000);
    const auto old_b = vr(4001);
    const auto old_c = vr(4002);
    const auto old_d = vr(4003);
    const auto new_b = vr(4004);

    const std::vector<seed_record> left_records{
        seed_record{.key = "key-a", .data_ver = 10, .value = old_a},
        seed_record{.key = "key-b", .data_ver = 10, .value = old_b},
    };
    const std::vector<seed_record> right_records{
        seed_record{.key = "key-c", .data_ver = 10, .value = old_c},
        seed_record{.key = "key-d", .data_ver = 10, .value = old_d},
    };
    write_leaf(device, profile, leaf_a, left_records);
    write_leaf(device, profile, leaf_b, right_records);
    write_root_internal(device, profile, root, "key-c", leaf_a, leaf_b);
    write_superblock_pair(device, profile, root);
    write_wal_delta(device, profile, new_b);

    const auto recovered = recovery::recover_empty_clean_boot(device, kCore);
    CHECK(recovered.runtime_state.next_lsn == 31);
    CHECK(recovered.runtime_state.recovered_durable_lsn == 30);
    CHECK(recovered.runtime_state.tree.root_range_base != root);
    CHECK(recovered.superblock_generation == 2);
    CHECK(recovery::wal_region_is_zero(device, kCore, profile));

    auto scan = recovery::scan_existing_tree(
        device,
        kCore,
        recovered.profile,
        recovered.tree_geometry,
        recovered.runtime_state.tree.root_range_base);
    CHECK(scan.max_data_ver == 30);
    auto records = records_by_key(scan);
    CHECK(records.size() == 5);
    expect_value(records, "key-a", 10, old_a);
    expect_value(records, "key-b", 20, new_b);
    expect_tombstone(records, "key-c", 20);
    expect_value(records, "key-d", 10, old_d);
    expect_tombstone(records, "key-e", 30);
    CHECK(live_extents_contain(scan.live_value_extents, old_a));
    CHECK(live_extents_contain(scan.live_value_extents, new_b));
    CHECK(live_extents_contain(scan.live_value_extents, old_d));
    CHECK(!live_extents_contain(scan.live_value_extents, old_b));
    CHECK(!live_extents_contain(scan.live_value_extents, old_c));

    const auto clean = recovery::recover_empty_clean_boot(device, kCore);
    CHECK(clean.runtime_state.next_lsn == 31);
    CHECK(clean.runtime_state.recovered_durable_lsn == 30);
    CHECK(clean.runtime_state.tree.root_range_base ==
          recovered.runtime_state.tree.root_range_base);
    CHECK(recovery::wal_region_is_zero(device, kCore, profile));
}

void
run_existing_tree_boot_scrubs_free_shadow_ranges() {
    const auto profile = test_profile(/*shadow_slots_per_range=*/2);
    const auto geom = recovery::tree_geometry_from_profile(profile);
    ram_recovery_device device(
        profile.value_data_area_end.lba, profile.lba_size);

    const format::paddr stale_free_range{0, 66};
    const format::paddr root_leaf{0, 68};
    const auto live_value = vr(4000);
    const auto stale_value = vr(4001);

    const std::vector<seed_record> root_records{
        seed_record{.key = "key-live", .data_ver = 10, .value = live_value},
    };
    const std::vector<seed_record> stale_records{
        seed_record{.key = "key-stale", .data_ver = 99, .value = stale_value},
    };

    write_leaf(device, profile, root_leaf, root_records);
    write_leaf_slot(
        device,
        profile,
        geom,
        stale_free_range,
        /*slot_index=*/1,
        stale_records);
    CHECK(!range_is_zero(
        device, geom.slot_paddr(stale_free_range, 1).lba, 1, profile.lba_size));
    write_superblock_pair(device, profile, root_leaf);

    const auto recovered = recovery::recover_empty_clean_boot(device, kCore);
    CHECK(recovered.runtime_state.tree.root_range_base == root_leaf);
    CHECK(recovered.runtime_state.recovered_durable_lsn == 10);

    bool saw_stale_free_range = false;
    for (const auto& range : recovered.runtime_state.tree_free_ranges) {
        if (range.base == stale_free_range) {
            saw_stale_free_range = true;
            break;
        }
    }
    CHECK(saw_stale_free_range);
    CHECK(range_is_zero(
        device,
        stale_free_range.lba,
        geom.range_lbas(),
        profile.lba_size));
}

}  // namespace

int
main() {
    run_existing_tree_wal_update_delete_recovery();
    run_existing_tree_boot_scrubs_free_shadow_ranges();
    return 0;
}
