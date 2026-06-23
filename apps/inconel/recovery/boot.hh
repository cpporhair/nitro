#ifndef APPS_INCONEL_RECOVERY_BOOT_HH
#define APPS_INCONEL_RECOVERY_BOOT_HH

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/tree_geometry.hh"
#include "../core/wal_stream.hh"
#include "../format/format_profile.hh"
#include "../format/superblock.hh"
#include "../format/tree_page.hh"
#include "../format/wal.hh"
#include "../nvme/real_device.hh"
#include "../tree/page_builder.hh"
#include "./state.hh"
#include "./sync_io.hh"
#include "./tree_scanner.hh"

namespace apps::inconel::recovery {

    constexpr uint32_t kBootSuperblockReadBytes = 4096;

    struct recovered_boot_state {
        format::format_profile profile{};
        core::tree_geometry tree_geometry{};
        format::superblock_choice::source superblock_source =
            format::superblock_choice::source::none;
        uint64_t superblock_generation = 0;
        recovered_runtime_state runtime_state{};
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

    struct scanned_wal_entry {
        std::string key;
        uint64_t lsn = 0;
        format::wal_op_type op_type = format::wal_op_type::put;
        std::optional<format::value_ref> vr;
    };

    struct wal_lsn_scan_state {
        uint32_t expected = 0;
        uint32_t observed = 0;
    };

    struct wal_scan_result {
        std::vector<scanned_wal_entry> entries;
        std::map<uint64_t, wal_lsn_scan_state> lsn_states;
        uint64_t max_complete_lsn = 0;
        bool saw_nonzero = false;
    };

    [[nodiscard]] inline wal::segment_geometry
    wal_geometry_from_profile(const format::format_profile& profile) {
        return wal::segment_geometry{
            .wal_base_paddr = profile.wal_base_paddr,
            .wal_segment_size = profile.wal_segment_size,
            .lba_size = profile.lba_size,
            .wal_segment_count = profile.wal_segment_count,
        };
    }

    [[nodiscard]] inline bool
    is_zero_tail(const void* buf, std::size_t off, std::size_t end) noexcept {
        if (off >= end) {
            return true;
        }
        return buffer_is_zero(
            static_cast<const char*>(buf) + off,
            end - off);
    }

    inline void
    record_wal_entry(wal_scan_result& out,
                     const format::decoded_wal_entry& decoded) {
        if (decoded.entry_count == 0) {
            throw std::runtime_error(
                "inconel recovery: WAL entry has zero entry_count");
        }
        if (decoded.lsn == 0) {
            throw std::runtime_error(
                "inconel recovery: WAL entry has reserved LSN 0");
        }

        auto& st = out.lsn_states[decoded.lsn];
        if (st.expected == 0) {
            st.expected = decoded.entry_count;
        } else if (st.expected != decoded.entry_count) {
            throw std::runtime_error(
                "inconel recovery: WAL entries disagree on entry_count "
                "for one LSN");
        }
        ++st.observed;
        if (st.observed > st.expected) {
            throw std::runtime_error(
                "inconel recovery: WAL LSN has more entries than "
                "entry_count");
        }

        out.entries.push_back(scanned_wal_entry{
            .key = std::string(decoded.key),
            .lsn = decoded.lsn,
            .op_type = decoded.op_type,
            .vr = decoded.vr,
        });
    }

    [[nodiscard]] inline wal_scan_result
    scan_wal(nvme::real_device& device,
             uint32_t core,
             const format::format_profile& profile) {
        const auto geometry = wal_geometry_from_profile(profile);
        wal::validate_segment_geometry(geometry);
        const uint32_t usable_end = wal::segment_usable_end_offset(geometry);
        const uint64_t segment_lbas =
            profile.wal_segment_size / profile.lba_size;

        auto segment_buf =
            make_zeroed_dma_buffer(profile.wal_segment_size, profile.lba_size);
        wal_scan_result out;

        for (uint32_t idx = 0; idx < profile.wal_segment_count; ++idx) {
            const uint64_t segment_lba =
                profile.wal_base_paddr.lba
                + static_cast<uint64_t>(idx) * segment_lbas;
            sync_read_logical_lbas(
                device,
                core,
                segment_lba,
                static_cast<uint32_t>(segment_lbas),
                segment_buf.get(),
                profile.lba_size);

            if (buffer_is_zero(segment_buf.get(), profile.wal_segment_size)) {
                continue;
            }
            out.saw_nonzero = true;

            format::wal_segment_header header{};
            std::memcpy(&header, segment_buf.get(), sizeof(header));
            const auto header_status =
                format::inspect_wal_segment_header(
                    header, format::SUPERBLOCK_FORMAT_VERSION_V1);
            if (header_status != format::wal_segment_status::ok) {
                throw std::runtime_error(
                    std::string("inconel recovery: bad WAL segment header: ")
                    + format::wal_segment_status_to_string(header_status));
            }
            if (header.segment_index != idx ||
                header.device_id != profile.wal_base_paddr.device_id) {
                throw std::runtime_error(
                    "inconel recovery: WAL segment header address mismatch");
            }

            uint32_t cursor = format::WAL_SEGMENT_HEADER_SIZE;
            while (cursor < usable_end) {
                if (is_zero_tail(segment_buf.get(), cursor, usable_end)) {
                    break;
                }

                format::decoded_wal_entry decoded{};
                uint32_t total_len = 0;
                const auto status = format::decode_wal_entry(
                    std::span<const char>{
                        static_cast<const char*>(segment_buf.get()) + cursor,
                        static_cast<std::size_t>(usable_end - cursor)},
                    header.segment_gen,
                    &decoded,
                    &total_len);

                if (status == format::wal_entry_decode_status::truncated) {
                    break;
                }
                if (status != format::wal_entry_decode_status::ok) {
                    throw std::runtime_error(
                        std::string("inconel recovery: bad WAL entry: ")
                        + format::wal_entry_decode_status_to_string(status));
                }
                record_wal_entry(out, decoded);
                cursor += total_len;
            }
        }

        for (const auto& [lsn, st] : out.lsn_states) {
            if (st.expected != 0 && st.observed == st.expected) {
                out.max_complete_lsn = std::max(out.max_complete_lsn, lsn);
            }
        }

        return out;
    }

    [[nodiscard]] inline bool
    wal_lsn_is_complete(const wal_scan_result& scan, uint64_t lsn) {
        auto it = scan.lsn_states.find(lsn);
        return it != scan.lsn_states.end() &&
               it->second.expected != 0 &&
               it->second.observed == it->second.expected;
    }

    struct replay_record {
        std::string key;
        uint64_t data_ver = 0;
        format::wal_op_type op_type = format::wal_op_type::put;
        format::value_ref vr{};
    };

    [[nodiscard]] inline std::vector<replay_record>
    build_replay_records(const wal_scan_result& scan) {
        std::map<std::string, replay_record> winners;
        for (const auto& entry : scan.entries) {
            if (!wal_lsn_is_complete(scan, entry.lsn)) {
                continue;
            }
            auto it = winners.find(entry.key);
            if (it != winners.end() && it->second.data_ver > entry.lsn) {
                continue;
            }
            if (entry.op_type == format::wal_op_type::put && !entry.vr) {
                throw std::runtime_error(
                    "inconel recovery: PUT WAL entry missing value_ref");
            }
            replay_record rec{
                .key = entry.key,
                .data_ver = entry.lsn,
                .op_type = entry.op_type,
                .vr = entry.vr.value_or(format::value_ref{}),
            };
            winners[entry.key] = std::move(rec);
        }

        std::vector<replay_record> out;
        out.reserve(winners.size());
        for (auto& [_, rec] : winners) {
            out.push_back(std::move(rec));
        }
        return out;
    }

    [[nodiscard]] inline format::record_kind
    record_kind_for_replay(const replay_record& rec) {
        switch (rec.op_type) {
        case format::wal_op_type::put:
            return format::record_kind::value;
        case format::wal_op_type::del:
            return format::record_kind::tombstone;
        }
        throw std::runtime_error("inconel recovery: unknown WAL op type");
    }

    [[nodiscard]] inline bool
    value_refs_equal(const format::value_ref& lhs,
                     const format::value_ref& rhs) noexcept {
        return std::memcmp(&lhs, &rhs, sizeof(format::value_ref)) == 0;
    }

    [[nodiscard]] inline bool
    tree_record_covers_replay_record(const recovered_tree_record& tree_rec,
                                     const replay_record& wal_rec) {
        if (tree_rec.data_ver > wal_rec.data_ver) {
            return true;
        }
        if (tree_rec.data_ver < wal_rec.data_ver) {
            return false;
        }

        const auto wal_kind = record_kind_for_replay(wal_rec);
        if (tree_rec.kind != wal_kind) {
            throw std::runtime_error(
                "inconel recovery: tree/WAL disagree for same key and LSN");
        }
        if (wal_kind == format::record_kind::value &&
            !value_refs_equal(tree_rec.vr, wal_rec.vr)) {
            throw std::runtime_error(
                "inconel recovery: tree/WAL value_ref mismatch for same "
                "key and LSN");
        }
        return true;
    }

    [[nodiscard]] inline bool
    existing_tree_wal_delta_is_empty(const recovered_tree_scan& tree_scan,
                                     const wal_scan_result& wal_scan) {
        const auto wal_records = build_replay_records(wal_scan);
        if (wal_records.empty()) {
            return true;
        }

        std::map<std::string, const recovered_tree_record*> tree_by_key;
        for (const auto& rec : tree_scan.records) {
            auto [_, inserted] = tree_by_key.emplace(rec.key, &rec);
            if (!inserted) {
                throw std::runtime_error(
                    "inconel recovery: duplicate key in scanned tree record "
                    "index");
            }
        }

        for (const auto& wal_rec : wal_records) {
            auto it = tree_by_key.find(wal_rec.key);
            if (it == tree_by_key.end()) {
                return false;
            }
            if (!tree_record_covers_replay_record(*it->second, wal_rec)) {
                return false;
            }
        }
        return true;
    }

    struct replay_tree_node {
        format::node_type type = format::node_type::leaf;
        format::paddr range_base{};
        format::paddr slot_paddr{};
        std::string first_key;
        replay_tree_node* parent = nullptr;
        std::vector<replay_tree_node*> children;
    };

    struct replay_tree_build {
        recovered_tree_snapshot tree;
        std::vector<value::live_value_extent> live_value_extents;
        uint64_t tree_alloc_head_lba = 0;
    };

    [[nodiscard]] inline format::paddr
    allocate_replay_range(const format::format_profile& profile,
                          const core::tree_geometry& geom,
                          format::paddr& next_range_base) {
        const uint64_t range_lbas = geom.range_lbas();
        if (range_lbas == 0 ||
            next_range_base.lba >
                std::numeric_limits<uint64_t>::max() - range_lbas ||
            next_range_base.lba + range_lbas > profile.value_data_area_end.lba) {
            throw std::runtime_error(
                "inconel recovery: tree replay exhausted data area");
        }
        auto out = next_range_base;
        next_range_base.lba += range_lbas;
        return out;
    }

    inline void
    write_replay_page(nvme::real_device& device,
                      uint32_t core,
                      const format::format_profile& profile,
                      const core::tree_geometry& geom,
                      format::paddr slot_paddr,
                      const void* page) {
        sync_write_logical_lbas(
            device,
            core,
            slot_paddr.lba,
            geom.page_lbas(),
            page,
            profile.lba_size);
    }

    [[nodiscard]] inline std::unique_ptr<replay_tree_node>
    make_replay_node(const format::format_profile& profile,
                     const core::tree_geometry& geom,
                     format::paddr& next_range_base,
                     format::node_type type,
                     std::string first_key) {
        auto node = std::make_unique<replay_tree_node>();
        node->type = type;
        node->range_base =
            allocate_replay_range(profile, geom, next_range_base);
        node->slot_paddr = geom.slot_paddr(node->range_base, 0);
        node->first_key = std::move(first_key);
        return node;
    }

    inline void
    validate_replay_key_size(std::string_view key) {
        if (key.size() > std::numeric_limits<uint16_t>::max()) {
            throw std::runtime_error(
                "inconel recovery: replay key exceeds tree page key limit");
        }
    }

    [[nodiscard]] inline std::vector<replay_tree_node*>
    build_replay_leaves(nvme::real_device& device,
                        uint32_t core,
                        const format::format_profile& profile,
                        const core::tree_geometry& geom,
                        const std::vector<replay_record>& records,
                        format::paddr& next_range_base,
                        std::vector<std::unique_ptr<replay_tree_node>>& owned,
                        std::vector<value::live_value_extent>& live_extents) {
        std::vector<replay_tree_node*> leaves;
        if (records.empty()) {
            return leaves;
        }

        std::size_t pos = 0;
        while (pos < records.size()) {
            validate_replay_key_size(records[pos].key);
            auto node = make_replay_node(
                profile,
                geom,
                next_range_base,
                format::node_type::leaf,
                records[pos].key);
            auto page = make_zeroed_dma_buffer(
                geom.tree_page_size, profile.lba_size);
            tree::leaf_page_builder builder;
            builder.init(page.get(), geom.tree_page_size);

            bool wrote_any = false;
            while (pos < records.size()) {
                const auto& rec = records[pos];
                validate_replay_key_size(rec.key);
                bool ok = false;
                if (rec.op_type == format::wal_op_type::put) {
                    ok = builder.add_value(rec.key, rec.data_ver, rec.vr);
                } else {
                    ok = builder.add_tombstone(rec.key, rec.data_ver);
                }
                if (!ok) {
                    if (!wrote_any) {
                        throw std::runtime_error(
                            "inconel recovery: WAL replay record does not "
                            "fit an empty leaf page");
                    }
                    break;
                }
                if (rec.op_type == format::wal_op_type::put) {
                    live_extents.push_back(value::live_value_extent{
                        .base = rec.vr.base,
                        .byte_offset = rec.vr.byte_offset,
                        .len = rec.vr.len,
                    });
                }
                wrote_any = true;
                ++pos;
            }

            builder.finalize();
            write_replay_page(
                device, core, profile, geom, node->slot_paddr, page.get());

            leaves.push_back(node.get());
            owned.push_back(std::move(node));
        }

        return leaves;
    }

    [[nodiscard]] inline std::vector<replay_tree_node*>
    build_replay_internal_layer(
        nvme::real_device& device,
        uint32_t core,
        const format::format_profile& profile,
        const core::tree_geometry& geom,
        const std::vector<replay_tree_node*>& children,
        format::paddr& next_range_base,
        std::vector<std::unique_ptr<replay_tree_node>>& owned) {
        std::vector<replay_tree_node*> parents;
        std::size_t pos = 0;
        while (pos < children.size()) {
            auto page =
                make_zeroed_dma_buffer(geom.tree_page_size, profile.lba_size);
            tree::internal_page_builder probe;
            probe.init(page.get(), geom.tree_page_size);

            std::size_t end = pos + 1;
            while (end < children.size()) {
                validate_replay_key_size(children[end]->first_key);
                if (!probe.add_child(
                        children[end]->first_key,
                        children[end - 1]->slot_paddr)) {
                    break;
                }
                ++end;
            }
            if (end == pos + 1 && end < children.size()) {
                throw std::runtime_error(
                    "inconel recovery: internal separator does not fit an "
                    "empty internal page");
            }
            if (children.size() - end == 1 && end - pos > 2) {
                --end;
            }

            auto node = make_replay_node(
                profile,
                geom,
                next_range_base,
                format::node_type::internal,
                children[pos]->first_key);
            node->children.reserve(end - pos);
            auto write_page =
                make_zeroed_dma_buffer(geom.tree_page_size, profile.lba_size);
            tree::internal_page_builder builder;
            builder.init(write_page.get(), geom.tree_page_size);
            for (std::size_t i = pos + 1; i < end; ++i) {
                const bool ok = builder.add_child(
                    children[i]->first_key,
                    children[i - 1]->slot_paddr);
                if (!ok) {
                    throw std::logic_error(
                        "inconel recovery: internal page probe/build drift");
                }
            }
            builder.set_rightmost_child(children[end - 1]->slot_paddr);
            builder.finalize();
            write_replay_page(
                device, core, profile, geom, node->slot_paddr, write_page.get());

            for (std::size_t i = pos; i < end; ++i) {
                children[i]->parent = node.get();
                node->children.push_back(children[i]);
            }
            parents.push_back(node.get());
            owned.push_back(std::move(node));
            pos = end;
        }
        return parents;
    }

    [[nodiscard]] inline core::leaf_order_index
    build_replay_leaf_order(const std::vector<replay_tree_node*>& leaves) {
        core::leaf_order_index out;
        if (leaves.empty()) {
            return out;
        }
        out.spans.reserve(leaves.size());

        auto append_fence = [&](std::string_view fence) {
            const auto off = static_cast<uint32_t>(out.fence_pool.size());
            out.fence_pool.append(fence.data(), fence.size());
            return std::pair<uint32_t, uint16_t>{
                off,
                static_cast<uint16_t>(fence.size()),
            };
        };

        std::pair<uint32_t, uint16_t> next_lower = append_fence({});
        for (std::size_t i = 0; i < leaves.size(); ++i) {
            auto lower = next_lower;
            std::pair<uint32_t, uint16_t> upper;
            if (i + 1 < leaves.size()) {
                if (leaves[i + 1]->first_key.empty()) {
                    throw std::runtime_error(
                        "inconel recovery: non-first replay leaf has empty "
                        "first key");
                }
                upper = append_fence(leaves[i + 1]->first_key);
                next_lower = upper;
            } else {
                upper = {
                    static_cast<uint32_t>(out.fence_pool.size()),
                    0,
                };
            }
            out.spans.push_back(core::leaf_span{
                .fence_lower_off = lower.first,
                .fence_upper_off = upper.first,
                .fence_lower_len = lower.second,
                .fence_upper_len = upper.second,
                .leaf_range_base = leaves[i]->range_base,
            });
        }

        return out;
    }

    [[nodiscard]] inline core::tree_reverse_topology
    build_replay_reverse_topology(
        const std::vector<replay_tree_node*>& leaves,
        const std::vector<std::unique_ptr<replay_tree_node>>& owned) {
        std::vector<replay_tree_node*> internals;
        for (const auto& node : owned) {
            if (node->type == format::node_type::internal) {
                internals.push_back(node.get());
            }
        }

        absl::flat_hash_map<format::paddr, core::internal_idx> idx_by_range;
        idx_by_range.reserve(internals.size());
        core::tree_reverse_topology out;
        out.internal_nodes.reserve(internals.size());

        for (auto* node : internals) {
            idx_by_range.emplace(
                node->range_base,
                static_cast<core::internal_idx>(out.internal_nodes.size()));
            out.internal_nodes.push_back(core::internal_node_entry{
                .range_base = node->range_base,
                .parent_idx = core::kInvalidInternalIdx,
            });
        }

        for (std::size_t i = 0; i < internals.size(); ++i) {
            auto* parent = internals[i]->parent;
            if (parent != nullptr) {
                out.internal_nodes[i].parent_idx =
                    idx_by_range.at(parent->range_base);
            }
        }

        out.leaf_parent_idx.reserve(leaves.size());
        for (auto* leaf : leaves) {
            if (leaf->parent == nullptr) {
                out.leaf_parent_idx.push_back(core::kInvalidInternalIdx);
            } else {
                out.leaf_parent_idx.push_back(
                    idx_by_range.at(leaf->parent->range_base));
            }
        }

        return out;
    }

    [[nodiscard]] inline replay_tree_build
    build_replay_tree(nvme::real_device& device,
                      uint32_t core,
                      const format::format_profile& profile,
                      const core::tree_geometry& geom,
                      const std::vector<replay_record>& records) {
        std::vector<std::unique_ptr<replay_tree_node>> owned;
        std::vector<value::live_value_extent> live_extents;
        format::paddr next_range_base = profile.value_data_area_base;

        auto leaves = build_replay_leaves(
            device,
            core,
            profile,
            geom,
            records,
            next_range_base,
            owned,
            live_extents);
        if (leaves.empty()) {
            return replay_tree_build{
                .tree = {},
                .live_value_extents = std::move(live_extents),
                .tree_alloc_head_lba = next_range_base.lba,
            };
        }

        std::vector<replay_tree_node*> layer = leaves;
        while (layer.size() > 1) {
            layer = build_replay_internal_layer(
                device,
                core,
                profile,
                geom,
                layer,
                next_range_base,
                owned);
        }
        auto* root = layer.front();

        recovered_tree_snapshot tree{
            .root_slot = root->slot_paddr,
            .slot_map = {},
            .leaf_order = build_replay_leaf_order(leaves),
            .root_range_base = root->range_base,
            .reverse_topology = build_replay_reverse_topology(leaves, owned),
        };
        tree.slot_map.reserve(owned.size());
        for (const auto& node : owned) {
            tree.slot_map[node->range_base] = 0;
        }

        sync_flush(device, core);

        return replay_tree_build{
            .tree = std::move(tree),
            .live_value_extents = std::move(live_extents),
            .tree_alloc_head_lba = next_range_base.lba,
        };
    }

    [[nodiscard]] inline format::superblock_choice::source
    write_recovered_superblock_root(
        nvme::real_device& device,
        uint32_t core,
        const format::superblock& chosen,
        format::superblock_choice::source source,
        format::paddr root_range_base,
        const format::format_profile& profile) {
        if (source == format::superblock_choice::source::none) {
            throw std::logic_error(
                "inconel recovery: cannot update unknown superblock slot");
        }
        auto next = chosen;
        next.root_base_paddr = root_range_base;
        if (next.generation == std::numeric_limits<uint64_t>::max()) {
            throw std::runtime_error(
                "inconel recovery: superblock generation overflow");
        }
        ++next.generation;
        next.crc = format::superblock_compute_crc(next);

        const uint64_t inactive_lba =
            source == format::superblock_choice::source::a ? 1 : 0;
        const auto committed =
            source == format::superblock_choice::source::a
                ? format::superblock_choice::source::b
                : format::superblock_choice::source::a;

        auto buf = make_zeroed_dma_buffer(profile.lba_size, profile.lba_size);
        std::memcpy(buf.get(), &next, sizeof(next));
        sync_write_logical_lbas(
            device, core, inactive_lba, 1, buf.get(), profile.lba_size);
        sync_flush(device, core);
        return committed;
    }

    inline void
    reset_wal_region(nvme::real_device& device,
                     uint32_t core,
                     const format::format_profile& profile) {
        const uint64_t segment_lbas =
            profile.wal_segment_size / profile.lba_size;
        const uint64_t total_lbas =
            segment_lbas * static_cast<uint64_t>(profile.wal_segment_count);
        sync_zero_logical_lbas(
            device,
            core,
            profile.wal_base_paddr.lba,
            total_lbas,
            profile.lba_size,
            profile.lba_size);
        sync_flush(device, core);
    }

    [[nodiscard]] inline recovered_runtime_state
    make_empty_runtime_state(const format::format_profile& profile,
                             format::superblock_choice::source source) {
        return recovered_runtime_state{
            .tree = {},
            .live_value_extents = {},
            .recovered_durable_lsn = 0,
            .next_lsn = 1,
            .tree_alloc_head_lba = profile.value_data_area_base.lba,
            .active_superblock_source = source,
        };
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

        auto profile = profile_from_superblock(*choice.chosen);
        const auto tree_geometry = tree_geometry_from_profile(profile);

        auto runtime_state = make_empty_runtime_state(profile, choice.which);
        const auto scan = scan_wal(device, core, profile);

        if (choice.chosen->root_base_paddr.lba != 0 ||
            choice.chosen->root_base_paddr.device_id != 0) {
            auto tree_scan = scan_existing_tree(
                device,
                core,
                profile,
                tree_geometry,
                choice.chosen->root_base_paddr);
            if (scan.saw_nonzero) {
                if (!existing_tree_wal_delta_is_empty(tree_scan, scan)) {
                    throw std::runtime_error(
                        "inconel recovery: WAL delta on existing tree "
                        "requires boot CoW merge (064D full merge is not "
                        "implemented yet)");
                }
                reset_wal_region(device, core, profile);
            }
            const uint64_t recovered_durable_lsn =
                std::max(tree_scan.max_data_ver, scan.max_complete_lsn);
            if (recovered_durable_lsn ==
                std::numeric_limits<uint64_t>::max()) {
                throw std::runtime_error(
                    "inconel recovery: recovered tree LSN overflow");
            }
            runtime_state = recovered_runtime_state{
                .tree = std::move(tree_scan.tree),
                .live_value_extents =
                    std::move(tree_scan.live_value_extents),
                .recovered_durable_lsn = recovered_durable_lsn,
                .next_lsn = recovered_durable_lsn + 1,
                .tree_alloc_head_lba = tree_scan.tree_alloc_head_lba,
                .active_superblock_source = choice.which,
            };
        } else if (scan.saw_nonzero) {
            const auto records = build_replay_records(scan);
            if (!records.empty()) {
                if (scan.max_complete_lsn ==
                    std::numeric_limits<uint64_t>::max()) {
                    throw std::runtime_error(
                        "inconel recovery: recovered LSN overflow");
                }
                auto replay = build_replay_tree(
                    device,
                    core,
                    profile,
                    tree_geometry,
                    records);
                const auto committed_source =
                    write_recovered_superblock_root(
                        device,
                        core,
                        *choice.chosen,
                        choice.which,
                        replay.tree.root_range_base,
                        profile);
                reset_wal_region(device, core, profile);
                runtime_state = recovered_runtime_state{
                    .tree = std::move(replay.tree),
                    .live_value_extents =
                        std::move(replay.live_value_extents),
                    .recovered_durable_lsn = scan.max_complete_lsn,
                    .next_lsn = scan.max_complete_lsn + 1,
                    .tree_alloc_head_lba = replay.tree_alloc_head_lba,
                    .active_superblock_source = committed_source,
                };
            } else {
                reset_wal_region(device, core, profile);
            }
        }

        const uint64_t active_generation =
            runtime_state.active_superblock_source == choice.which
                ? choice.chosen->generation
                : choice.chosen->generation + 1;

        return recovered_boot_state{
            .profile = profile,
            .tree_geometry = tree_geometry,
            .superblock_source = runtime_state.active_superblock_source,
            .superblock_generation = active_generation,
            .runtime_state = std::move(runtime_state),
        };
    }

}  // namespace apps::inconel::recovery

#endif  // APPS_INCONEL_RECOVERY_BOOT_HH
