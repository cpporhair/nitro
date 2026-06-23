#ifndef APPS_INCONEL_RECOVERY_BOOT_HH
#define APPS_INCONEL_RECOVERY_BOOT_HH

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
        std::map<uint64_t, std::set<std::string>> keys_by_lsn;
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
        auto& keys = out.keys_by_lsn[decoded.lsn];
        auto [_, key_inserted] = keys.insert(std::string(decoded.key));
        if (!key_inserted) {
            throw std::runtime_error(
                "inconel recovery: WAL LSN contains duplicate key");
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
                continue;
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

                if (status != format::wal_entry_decode_status::ok) {
                    break;
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
        std::vector<format::range_ref> tree_free_ranges;
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
                .tree_free_ranges = {},
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
            .tree_free_ranges = {},
            .tree_alloc_head_lba = next_range_base.lba,
        };
    }

    struct leaf_merge_result {
        std::vector<recovered_tree_record> records;
        bool changed = false;
    };

    struct same_shape_leaf_replay_plan {
        format::paddr leaf_range_base{};
        uint32_t next_slot = 0;
        format::paddr next_slot_paddr{};
        dma_buffer page;
    };

    [[nodiscard]] inline recovered_tree_record
    recovered_record_from_replay(const replay_record& rec,
                                 format::paddr leaf_range_base) {
        recovered_tree_record out{
            .key = rec.key,
            .leaf_range_base = leaf_range_base,
            .data_ver = rec.data_ver,
            .kind = record_kind_for_replay(rec),
            .vr = {},
        };
        if (out.kind == format::record_kind::value) {
            out.vr = rec.vr;
        }
        return out;
    }

    [[nodiscard]] inline leaf_merge_result
    merge_leaf_records(std::span<const recovered_tree_record> tree_records,
                       std::span<const replay_record> wal_records,
                       format::paddr leaf_range_base) {
        std::map<std::string, recovered_tree_record> by_key;
        for (const auto& rec : tree_records) {
            auto [_, inserted] = by_key.emplace(rec.key, rec);
            if (!inserted) {
                throw std::runtime_error(
                    "inconel recovery: duplicate key in same-shape leaf scan");
            }
        }

        bool changed = false;
        for (const auto& wal_rec : wal_records) {
            auto incoming =
                recovered_record_from_replay(wal_rec, leaf_range_base);
            auto it = by_key.find(incoming.key);
            if (it == by_key.end()) {
                by_key.emplace(incoming.key, std::move(incoming));
                changed = true;
                continue;
            }
            if (it->second.data_ver < incoming.data_ver) {
                it->second = std::move(incoming);
                changed = true;
                continue;
            }
            if (it->second.data_ver == incoming.data_ver) {
                (void)tree_record_covers_replay_record(it->second, wal_rec);
            }
        }

        std::vector<recovered_tree_record> out;
        out.reserve(by_key.size());
        for (auto& [_, rec] : by_key) {
            out.push_back(std::move(rec));
        }
        return leaf_merge_result{
            .records = std::move(out),
            .changed = changed,
        };
    }

    [[nodiscard]] inline std::vector<value::live_value_extent>
    live_extents_from_records(
        const std::vector<recovered_tree_record>& records) {
        std::vector<value::live_value_extent> out;
        for (const auto& rec : records) {
            if (rec.kind != format::record_kind::value) {
                continue;
            }
            out.push_back(value::live_value_extent{
                .base = rec.vr.base,
                .byte_offset = rec.vr.byte_offset,
                .len = rec.vr.len,
            });
        }
        return out;
    }

    [[nodiscard]] inline uint64_t
    max_data_ver_from_records(
        const std::vector<recovered_tree_record>& records) noexcept {
        uint64_t out = 0;
        for (const auto& rec : records) {
            out = std::max(out, rec.data_ver);
        }
        return out;
    }

    [[nodiscard]] inline std::optional<dma_buffer>
    build_leaf_records_page(const format::format_profile& profile,
                            const core::tree_geometry& geom,
                            const std::vector<recovered_tree_record>& records) {
        auto page = make_zeroed_dma_buffer(
            geom.tree_page_size, profile.lba_size);
        tree::leaf_page_builder builder;
        builder.init(page.get(), geom.tree_page_size);

        for (const auto& rec : records) {
            validate_replay_key_size(rec.key);
            bool ok = false;
            if (rec.kind == format::record_kind::value) {
                ok = builder.add_value(rec.key, rec.data_ver, rec.vr);
            } else if (rec.kind == format::record_kind::tombstone) {
                ok = builder.add_tombstone(rec.key, rec.data_ver);
            } else {
                throw std::runtime_error(
                    "inconel recovery: unknown tree record kind");
            }
            if (!ok) {
                return std::nullopt;
            }
        }

        builder.finalize();
        std::optional<dma_buffer> out;
        out.emplace(std::move(page));
        return out;
    }

    [[nodiscard]] inline std::optional<recovered_tree_scan>
    replay_same_shape_wal_delta(nvme::real_device& device,
                                uint32_t core,
                                const format::format_profile& profile,
                                const core::tree_geometry& geom,
                                const recovered_tree_scan& tree_scan,
                                const wal_scan_result& wal_scan) {
        const auto& leaf_order = tree_scan.tree.leaf_order;
        if (leaf_order.empty()) {
            return std::nullopt;
        }

        absl::flat_hash_map<format::paddr, uint32_t> leaf_idx_by_range;
        leaf_idx_by_range.reserve(leaf_order.spans.size());
        for (uint32_t i = 0; i < leaf_order.spans.size(); ++i) {
            auto [_, inserted] =
                leaf_idx_by_range.emplace(
                    leaf_order.spans[i].leaf_range_base, i);
            if (!inserted) {
                throw std::runtime_error(
                    "inconel recovery: duplicate leaf range in leaf_order");
            }
        }

        std::vector<std::vector<recovered_tree_record>> tree_by_leaf(
            leaf_order.spans.size());
        for (const auto& rec : tree_scan.records) {
            auto it = leaf_idx_by_range.find(rec.leaf_range_base);
            if (it == leaf_idx_by_range.end()) {
                throw std::runtime_error(
                    "inconel recovery: scanned record points at unknown leaf");
            }
            tree_by_leaf[it->second].push_back(rec);
        }

        const auto wal_records = build_replay_records(wal_scan);
        std::vector<std::vector<replay_record>> wal_by_leaf(
            leaf_order.spans.size());
        for (const auto& wal_rec : wal_records) {
            const auto leaf_idx = leaf_order.find_leaf_for_key(wal_rec.key);
            if (leaf_idx >= leaf_order.spans.size()) {
                return std::nullopt;
            }
            wal_by_leaf[leaf_idx].push_back(wal_rec);
        }

        std::vector<same_shape_leaf_replay_plan> plans;
        plans.reserve(wal_records.size());
        std::vector<std::vector<recovered_tree_record>> merged_by_leaf(
            leaf_order.spans.size());

        for (uint32_t leaf_idx = 0; leaf_idx < wal_by_leaf.size(); ++leaf_idx) {
            if (wal_by_leaf[leaf_idx].empty()) {
                continue;
            }

            const auto leaf_range_base =
                leaf_order.spans[leaf_idx].leaf_range_base;
            auto merge = merge_leaf_records(
                std::span<const recovered_tree_record>(
                    tree_by_leaf[leaf_idx].data(),
                    tree_by_leaf[leaf_idx].size()),
                std::span<const replay_record>(
                    wal_by_leaf[leaf_idx].data(),
                    wal_by_leaf[leaf_idx].size()),
                leaf_range_base);
            if (!merge.changed) {
                continue;
            }
            if (merge.records.empty()) {
                return std::nullopt;
            }

            auto slot_it = tree_scan.tree.slot_map.find(leaf_range_base);
            if (slot_it == tree_scan.tree.slot_map.end()) {
                throw std::runtime_error(
                    "inconel recovery: affected leaf missing from slot map");
            }
            const uint32_t current_slot = slot_it->second;
            if (current_slot + 1 >= geom.shadow_slots_per_range) {
                return std::nullopt;
            }

            auto page = build_leaf_records_page(profile, geom, merge.records);
            if (!page) {
                return std::nullopt;
            }

            const uint32_t next_slot = current_slot + 1;
            const auto next_slot_paddr =
                geom.slot_paddr(leaf_range_base, next_slot);
            merged_by_leaf[leaf_idx] = merge.records;
            plans.push_back(same_shape_leaf_replay_plan{
                .leaf_range_base = leaf_range_base,
                .next_slot = next_slot,
                .next_slot_paddr = next_slot_paddr,
                .page = std::move(*page),
            });
        }

        if (plans.empty()) {
            return std::nullopt;
        }

        for (const auto& plan : plans) {
            write_replay_page(
                device,
                core,
                profile,
                geom,
                plan.next_slot_paddr,
                plan.page.get());
        }
        sync_flush(device, core);

        std::vector<recovered_tree_record> next_records;
        for (uint32_t leaf_idx = 0; leaf_idx < leaf_order.spans.size();
             ++leaf_idx) {
            const auto& records =
                merged_by_leaf[leaf_idx].empty()
                    ? tree_by_leaf[leaf_idx]
                    : merged_by_leaf[leaf_idx];
            next_records.insert(
                next_records.end(), records.begin(), records.end());
        }

        auto out = recovered_tree_scan{
            .tree = tree_scan.tree,
            .live_value_extents = live_extents_from_records(next_records),
            .tree_free_ranges = tree_scan.tree_free_ranges,
            .records = std::move(next_records),
            .max_data_ver = 0,
            .tree_alloc_head_lba = tree_scan.tree_alloc_head_lba,
        };
        out.max_data_ver = max_data_ver_from_records(out.records);
        for (const auto& plan : plans) {
            out.tree.slot_map[plan.leaf_range_base] = plan.next_slot;
            if (plan.leaf_range_base == out.tree.root_range_base) {
                out.tree.root_slot = plan.next_slot_paddr;
            }
        }
        return out;
    }

    struct cow_replay_node;

    struct cow_replay_child_ref {
        std::variant<format::paddr, std::unique_ptr<cow_replay_node>> target;
    };

    struct cow_replay_node {
        format::node_type type = format::node_type::leaf;
        std::vector<char> content;
        std::vector<cow_replay_child_ref> children;
        std::vector<std::string> separators;
        format::paddr new_range_base{};
        uint32_t new_slot_index = 0;
        format::paddr new_paddr{};
    };

    struct cow_child_delta {
        format::paddr old_child_range_base{};
        core::internal_idx parent_idx = core::kInvalidInternalIdx;
        std::vector<std::unique_ptr<cow_replay_node>> nodes;
        std::vector<std::string> sibling_seps;
    };

    struct cow_leaf_work_item {
        uint32_t old_leaf_idx = 0;
        format::paddr old_range_base{};
        core::internal_idx parent_idx = core::kInvalidInternalIdx;
        std::vector<std::unique_ptr<cow_replay_node>> nodes;
    };

    struct cow_replay_result {
        recovered_tree_scan tree_scan;
        bool root_range_changed = false;
    };

    [[nodiscard]] inline bool
    paddr_is_null(format::paddr p) noexcept {
        return p.device_id == 0 && p.lba == 0;
    }

    [[nodiscard]] inline format::paddr
    cow_child_range_base(const cow_replay_child_ref& child) {
        if (const auto* p = std::get_if<format::paddr>(&child.target)) {
            return *p;
        }
        const auto* node =
            std::get<std::unique_ptr<cow_replay_node>>(child.target).get();
        if (paddr_is_null(node->new_range_base)) {
            throw std::logic_error(
                "inconel recovery: unplaced CoW child reached internal "
                "page builder");
        }
        return node->new_range_base;
    }

    [[nodiscard]] inline std::string
    cow_first_key_of_leaf_node(const cow_replay_node* node,
                               uint32_t page_size) {
        if (node == nullptr || node->type != format::node_type::leaf) {
            throw std::logic_error(
                "inconel recovery: expected leaf node for first key");
        }
        tree::leaf_page_reader reader;
        if (!reader.parse(node->content.data(), page_size)) {
            throw std::runtime_error(
                "inconel recovery: built leaf page failed validation");
        }
        if (reader.record_count() == 0) {
            return {};
        }
        return std::string(reader.get(0).key);
    }

    [[nodiscard]] inline std::vector<std::unique_ptr<cow_replay_node>>
    build_cow_leaf_nodes_from_records(
        const format::format_profile& profile,
        const core::tree_geometry& geom,
        const std::vector<recovered_tree_record>& records) {
        if (records.empty()) {
            throw std::runtime_error(
                "inconel recovery: full CoW merge would create an empty "
                "leaf; recovery tombstones must remain frontier carriers");
        }

        std::vector<std::unique_ptr<cow_replay_node>> out;
        std::size_t pos = 0;
        while (pos < records.size()) {
            auto node = std::make_unique<cow_replay_node>();
            node->type = format::node_type::leaf;
            node->content.resize(geom.tree_page_size);

            tree::leaf_page_builder builder;
            builder.init(node->content.data(), geom.tree_page_size);

            bool wrote_any = false;
            while (pos < records.size()) {
                const auto& rec = records[pos];
                validate_replay_key_size(rec.key);
                bool ok = false;
                if (rec.kind == format::record_kind::value) {
                    ok = builder.add_value(rec.key, rec.data_ver, rec.vr);
                } else if (rec.kind == format::record_kind::tombstone) {
                    ok = builder.add_tombstone(rec.key, rec.data_ver);
                } else {
                    throw std::runtime_error(
                        "inconel recovery: unknown record kind in CoW leaf");
                }
                if (!ok) {
                    if (!wrote_any) {
                        throw std::runtime_error(
                            "inconel recovery: single CoW leaf record does "
                            "not fit a tree page");
                    }
                    break;
                }
                wrote_any = true;
                ++pos;
            }

            builder.finalize();
            (void)profile;
            out.push_back(std::move(node));
        }
        return out;
    }

    [[nodiscard]] inline std::vector<char>
    read_base_internal_page(nvme::real_device& device,
                            uint32_t core,
                            const format::format_profile& profile,
                            const core::tree_geometry& geom,
                            const recovered_tree_snapshot& tree,
                            format::paddr range_base) {
        auto slot_it = tree.slot_map.find(range_base);
        if (slot_it == tree.slot_map.end()) {
            throw std::runtime_error(
                "inconel recovery: internal parent missing from slot map");
        }
        const auto slot_paddr = geom.slot_paddr(range_base, slot_it->second);
        auto page = make_zeroed_dma_buffer(
            geom.tree_page_size, profile.lba_size);
        sync_read_logical_lbas(
            device,
            core,
            slot_paddr.lba,
            geom.page_lbas(),
            page.get(),
            profile.lba_size);

        std::vector<char> bytes(geom.tree_page_size);
        std::memcpy(bytes.data(), page.get(), bytes.size());
        format::tree_slot_header header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.type != format::node_type::internal) {
            throw std::runtime_error(
                "inconel recovery: expected internal parent page");
        }
        tree::internal_page_reader reader;
        if (!reader.parse(bytes.data(), geom.tree_page_size)) {
            throw std::runtime_error(
                "inconel recovery: failed to parse internal parent page");
        }
        return bytes;
    }

    [[nodiscard]] inline format::paddr
    allocate_cow_replay_range(const format::format_profile& profile,
                              const core::tree_geometry& geom,
                              format::paddr& next_range_base,
                              uint64_t allocation_limit_lba) {
        const uint64_t range_lbas = geom.range_lbas();
        if (range_lbas == 0 ||
            next_range_base.lba >
                std::numeric_limits<uint64_t>::max() - range_lbas ||
            next_range_base.lba + range_lbas > allocation_limit_lba ||
            next_range_base.lba + range_lbas >
                profile.value_data_area_end.lba) {
            throw std::runtime_error(
                "inconel recovery: full CoW merge exhausted tree space "
                "before live value area");
        }
        auto out = next_range_base;
        next_range_base.lba += range_lbas;
        return out;
    }

    inline void
    plan_fresh_cow_node(cow_replay_node* node,
                        const format::format_profile& profile,
                        const core::tree_geometry& geom,
                        format::paddr& next_range_base,
                        uint64_t allocation_limit_lba) {
        node->new_range_base = allocate_cow_replay_range(
            profile, geom, next_range_base, allocation_limit_lba);
        node->new_slot_index = 0;
        node->new_paddr = node->new_range_base;
    }

    template <typename NodeVec>
    inline void
    plan_cow_group_against_old_range(
        NodeVec& nodes,
        format::paddr old_range_base,
        const recovered_tree_snapshot& tree,
        const format::format_profile& profile,
        const core::tree_geometry& geom,
        format::paddr& next_range_base,
        uint64_t allocation_limit_lba) {
        if (nodes.empty()) {
            throw std::runtime_error(
                "inconel recovery: full CoW merge produced zero child "
                "pages for an existing range");
        }

        auto slot_it = tree.slot_map.find(old_range_base);
        if (slot_it == tree.slot_map.end()) {
            throw std::runtime_error(
                "inconel recovery: CoW target range missing from slot map");
        }
        const uint32_t cur_slot = slot_it->second;
        if (cur_slot >= geom.shadow_slots_per_range) {
            throw std::runtime_error(
                "inconel recovery: CoW target slot is out of range");
        }

        // A split group must not publish its first sibling through the old
        // range before the parent update is durable. Otherwise a crash can
        // make the old parent/root see only the first split fragment while
        // the WAL does not necessarily contain unchanged records that moved
        // to later siblings. Single-page replacements are shape-preserving
        // and may safely reuse the next shadow slot.
        if (nodes.size() == 1 &&
            cur_slot + 1 < geom.shadow_slots_per_range) {
            auto* first = nodes.front().get();
            first->new_range_base = old_range_base;
            first->new_slot_index = cur_slot + 1;
            first->new_paddr = geom.slot_paddr(old_range_base, cur_slot + 1);
        } else {
            plan_fresh_cow_node(
                nodes.front().get(),
                profile,
                geom,
                next_range_base,
                allocation_limit_lba);
        }

        for (std::size_t i = 1; i < nodes.size(); ++i) {
            plan_fresh_cow_node(
                nodes[i].get(),
                profile,
                geom,
                next_range_base,
                allocation_limit_lba);
        }
    }

    [[nodiscard]] inline std::vector<std::unique_ptr<cow_replay_node>>
    move_cow_node_vector(
        std::vector<std::unique_ptr<cow_replay_node>>&& nodes) {
        std::vector<std::unique_ptr<cow_replay_node>> out;
        out.reserve(nodes.size());
        for (auto& node : nodes) {
            out.push_back(std::move(node));
        }
        return out;
    }

    [[nodiscard]] inline std::vector<std::unique_ptr<cow_replay_node>>
    build_cow_internal_pages(std::vector<cow_replay_child_ref>&& children,
                             std::vector<std::string>&& separators,
                             format::paddr replaces_old_range,
                             bool is_new_layer,
                             uint32_t page_size,
                             std::vector<std::string>& sibling_seps_out) {
        sibling_seps_out.clear();
        if (children.empty()) {
            throw std::runtime_error(
                "inconel recovery: CoW internal page has no children");
        }
        if (separators.size() + 1 != children.size()) {
            throw std::runtime_error(
                "inconel recovery: CoW internal separator count mismatch");
        }

        std::vector<std::unique_ptr<cow_replay_node>> result;
        std::size_t next = 0;
        while (next < children.size()) {
            uint32_t used = sizeof(format::tree_slot_header);
            uint32_t count = 0;
            for (std::size_t j = next; j < children.size(); ++j) {
                uint32_t add = 0;
                if (count == 0) {
                    add = sizeof(format::paddr);
                } else {
                    add = format::internal_record_size(
                              static_cast<uint16_t>(
                                  separators[next + count - 1].size()))
                        + sizeof(uint16_t);
                }
                if (used + add > page_size) {
                    break;
                }
                used += add;
                ++count;
            }
            if (count == 0) {
                throw std::runtime_error(
                    "inconel recovery: CoW internal child does not fit page");
            }

            auto node = std::make_unique<cow_replay_node>();
            node->type = format::node_type::internal;
            node->content.resize(page_size);

            tree::internal_page_builder builder;
            builder.init(node->content.data(), page_size);
            for (uint32_t j = 0; j + 1 < count; ++j) {
                const auto child_range = cow_child_range_base(children[next + j]);
                if (!builder.add_child(separators[next + j], child_range)) {
                    throw std::logic_error(
                        "inconel recovery: CoW internal builder/probe drift");
                }
            }
            builder.set_rightmost_child(
                cow_child_range_base(children[next + count - 1]));
            builder.finalize();

            node->children.reserve(count);
            for (uint32_t j = 0; j < count; ++j) {
                node->children.push_back(std::move(children[next + j]));
            }
            node->separators.reserve(count > 0 ? count - 1 : 0);
            for (uint32_t j = 0; j + 1 < count; ++j) {
                node->separators.push_back(std::move(separators[next + j]));
            }
            if (!is_new_layer && result.empty()) {
                (void)replaces_old_range;
            }
            result.push_back(std::move(node));

            if (next + count < children.size()) {
                sibling_seps_out.push_back(
                    std::move(separators[next + count - 1]));
            }
            next += count;
        }
        return result;
    }

    [[nodiscard]] inline cow_replay_child_ref
    finalize_cow_root_group(
        std::vector<std::unique_ptr<cow_replay_node>>&& nodes,
        std::vector<std::string>&& sibling_seps,
        const format::format_profile& profile,
        const core::tree_geometry& geom,
        format::paddr& next_range_base,
        uint64_t allocation_limit_lba) {
        if (nodes.empty()) {
            throw std::runtime_error(
                "inconel recovery: full CoW merge produced an empty root");
        }
        if (nodes.size() == 1) {
            return cow_replay_child_ref{ .target = std::move(nodes.front()) };
        }

        std::vector<cow_replay_child_ref> children;
        children.reserve(nodes.size());
        for (auto& node : nodes) {
            children.push_back(cow_replay_child_ref{
                .target = std::move(node),
            });
        }
        std::vector<std::string> separators = std::move(sibling_seps);
        while (true) {
            std::vector<std::string> next_sibling_seps;
            auto layer = build_cow_internal_pages(
                std::move(children),
                std::move(separators),
                format::paddr{0, 0},
                /*is_new_layer=*/true,
                geom.tree_page_size,
                next_sibling_seps);
            for (auto& node : layer) {
                plan_fresh_cow_node(
                    node.get(),
                    profile,
                    geom,
                    next_range_base,
                    allocation_limit_lba);
            }
            if (layer.size() == 1) {
                return cow_replay_child_ref{
                    .target = std::move(layer.front()),
                };
            }

            children.clear();
            children.reserve(layer.size());
            for (auto& node : layer) {
                children.push_back(cow_replay_child_ref{
                    .target = std::move(node),
                });
            }
            separators = std::move(next_sibling_seps);
        }
    }

    inline void
    collect_cow_nodes_for_write(const cow_replay_child_ref& ref,
                                std::vector<const cow_replay_node*>& out) {
        const auto* node_up =
            std::get_if<std::unique_ptr<cow_replay_node>>(&ref.target);
        if (node_up == nullptr) {
            return;
        }
        const auto* node = node_up->get();
        for (const auto& child : node->children) {
            collect_cow_nodes_for_write(child, out);
        }
        out.push_back(node);
    }

    inline void
    write_cow_nodes(nvme::real_device& device,
                    uint32_t core,
                    const format::format_profile& profile,
                    const core::tree_geometry& geom,
                    const cow_replay_child_ref* root,
                    const std::vector<cow_replay_child_ref>& detached) {
        std::vector<const cow_replay_node*> nodes;
        if (root != nullptr) {
            collect_cow_nodes_for_write(*root, nodes);
        }
        for (const auto& subtree : detached) {
            collect_cow_nodes_for_write(subtree, nodes);
        }

        for (const auto* node : nodes) {
            if (paddr_is_null(node->new_paddr)) {
                throw std::logic_error(
                    "inconel recovery: unplaced CoW node reached writeback");
            }
            auto page = make_zeroed_dma_buffer(
                geom.tree_page_size, profile.lba_size);
            std::memcpy(page.get(), node->content.data(), node->content.size());
            write_replay_page(
                device, core, profile, geom, node->new_paddr, page.get());
        }
        if (!nodes.empty()) {
            sync_flush(device, core);
        }
    }

    [[nodiscard]] inline uint64_t
    allocation_limit_from_records(
        const format::format_profile& profile,
        const std::vector<std::vector<recovered_tree_record>>& records_by_leaf) {
        uint64_t limit = profile.value_data_area_end.lba;
        for (const auto& records : records_by_leaf) {
            for (const auto& rec : records) {
                if (rec.kind == format::record_kind::value) {
                    limit = std::min(limit, rec.vr.base.lba);
                }
            }
        }
        return limit;
    }

    [[nodiscard]] inline std::optional<cow_replay_result>
    replay_full_cow_wal_delta(nvme::real_device& device,
                              uint32_t core,
                              const format::format_profile& profile,
                              const core::tree_geometry& geom,
                              const recovered_tree_scan& tree_scan,
                              const wal_scan_result& wal_scan) {
        const auto& leaf_order = tree_scan.tree.leaf_order;
        if (leaf_order.empty()) {
            return std::nullopt;
        }

        absl::flat_hash_map<format::paddr, uint32_t> leaf_idx_by_range;
        leaf_idx_by_range.reserve(leaf_order.spans.size());
        for (uint32_t i = 0; i < leaf_order.spans.size(); ++i) {
            if (!leaf_idx_by_range
                     .emplace(leaf_order.spans[i].leaf_range_base, i)
                     .second) {
                throw std::runtime_error(
                    "inconel recovery: duplicate leaf range in CoW replay");
            }
        }

        std::vector<std::vector<recovered_tree_record>> tree_by_leaf(
            leaf_order.spans.size());
        std::map<std::string, const recovered_tree_record*> tree_by_key;
        for (const auto& rec : tree_scan.records) {
            auto leaf_it = leaf_idx_by_range.find(rec.leaf_range_base);
            if (leaf_it == leaf_idx_by_range.end()) {
                throw std::runtime_error(
                    "inconel recovery: scanned tree record points at "
                    "unknown CoW leaf");
            }
            tree_by_leaf[leaf_it->second].push_back(rec);
            if (!tree_by_key.emplace(rec.key, &rec).second) {
                throw std::runtime_error(
                    "inconel recovery: duplicate scanned tree key in CoW "
                    "index");
            }
        }

        const auto wal_records = build_replay_records(wal_scan);
        std::vector<std::vector<replay_record>> wal_by_leaf(
            leaf_order.spans.size());
        bool saw_delta = false;
        for (const auto& wal_rec : wal_records) {
            auto tree_it = tree_by_key.find(wal_rec.key);
            if (tree_it != tree_by_key.end() &&
                tree_record_covers_replay_record(
                    *tree_it->second, wal_rec)) {
                continue;
            }
            const auto leaf_idx = leaf_order.find_leaf_for_key(wal_rec.key);
            if (leaf_idx >= leaf_order.spans.size()) {
                throw std::runtime_error(
                    "inconel recovery: WAL key did not map to a CoW leaf");
            }
            wal_by_leaf[leaf_idx].push_back(wal_rec);
            saw_delta = true;
        }
        if (!saw_delta) {
            return std::nullopt;
        }

        std::vector<std::vector<recovered_tree_record>> final_by_leaf =
            tree_by_leaf;
        std::vector<cow_leaf_work_item> leaf_work;
        leaf_work.reserve(leaf_order.spans.size());
        for (uint32_t leaf_idx = 0; leaf_idx < wal_by_leaf.size(); ++leaf_idx) {
            if (wal_by_leaf[leaf_idx].empty()) {
                continue;
            }
            const auto old_range =
                leaf_order.spans[leaf_idx].leaf_range_base;
            auto merge = merge_leaf_records(
                std::span<const recovered_tree_record>(
                    tree_by_leaf[leaf_idx].data(),
                    tree_by_leaf[leaf_idx].size()),
                std::span<const replay_record>(
                    wal_by_leaf[leaf_idx].data(),
                    wal_by_leaf[leaf_idx].size()),
                old_range);
            if (!merge.changed) {
                continue;
            }
            auto nodes = build_cow_leaf_nodes_from_records(
                profile, geom, merge.records);
            final_by_leaf[leaf_idx] = std::move(merge.records);
            const auto parent_idx =
                tree_scan.tree.reverse_topology.leaf_parent_idx.at(leaf_idx);
            leaf_work.push_back(cow_leaf_work_item{
                .old_leaf_idx = leaf_idx,
                .old_range_base = old_range,
                .parent_idx = parent_idx,
                .nodes = std::move(nodes),
            });
        }
        if (leaf_work.empty()) {
            return std::nullopt;
        }

        auto next_range_base = format::paddr{
            profile.value_data_area_base.device_id,
            tree_scan.tree_alloc_head_lba,
        };
        const uint64_t allocation_limit_lba =
            allocation_limit_from_records(profile, final_by_leaf);
        if (next_range_base.lba > allocation_limit_lba) {
            throw std::runtime_error(
                "inconel recovery: scanned tree already overlaps live "
                "value space");
        }

        std::sort(
            leaf_work.begin(),
            leaf_work.end(),
            [](const cow_leaf_work_item& a, const cow_leaf_work_item& b) {
                return a.old_leaf_idx < b.old_leaf_idx;
            });

        std::vector<cow_child_delta> current_level;
        std::optional<cow_replay_child_ref> final_root;
        std::vector<cow_replay_child_ref> detached_subtrees;

        for (auto& item : leaf_work) {
            plan_cow_group_against_old_range(
                item.nodes,
                item.old_range_base,
                tree_scan.tree,
                profile,
                geom,
                next_range_base,
                allocation_limit_lba);

            if (item.parent_idx == core::kInvalidInternalIdx) {
                std::vector<std::string> sibling_seps;
                sibling_seps.reserve(
                    item.nodes.size() > 0 ? item.nodes.size() - 1 : 0);
                for (std::size_t i = 1; i < item.nodes.size(); ++i) {
                    auto sep = cow_first_key_of_leaf_node(
                        item.nodes[i].get(), geom.tree_page_size);
                    if (sep.empty()) {
                        throw std::runtime_error(
                            "inconel recovery: split root leaf has empty "
                            "separator");
                    }
                    sibling_seps.push_back(std::move(sep));
                }
                final_root.emplace(finalize_cow_root_group(
                    std::move(item.nodes),
                    std::move(sibling_seps),
                    profile,
                    geom,
                    next_range_base,
                    allocation_limit_lba));
                continue;
            }

            if (item.nodes.size() == 1 &&
                item.nodes.front()->new_range_base == item.old_range_base) {
                detached_subtrees.push_back(cow_replay_child_ref{
                    .target = std::move(item.nodes.front()),
                });
                continue;
            }

            cow_child_delta delta;
            delta.old_child_range_base = item.old_range_base;
            delta.parent_idx = item.parent_idx;
            delta.nodes = std::move(item.nodes);
            delta.sibling_seps.reserve(
                delta.nodes.size() > 0 ? delta.nodes.size() - 1 : 0);
            for (std::size_t i = 1; i < delta.nodes.size(); ++i) {
                auto sep = cow_first_key_of_leaf_node(
                    delta.nodes[i].get(), geom.tree_page_size);
                if (sep.empty()) {
                    throw std::runtime_error(
                        "inconel recovery: split leaf has empty separator");
                }
                delta.sibling_seps.push_back(std::move(sep));
            }
            current_level.push_back(std::move(delta));
        }

        const auto& topo = tree_scan.tree.reverse_topology;
        while (!current_level.empty()) {
            absl::flat_hash_map<
                core::internal_idx,
                std::vector<cow_child_delta>> grouped;
            for (auto& delta : current_level) {
                grouped[delta.parent_idx].push_back(std::move(delta));
            }
            current_level.clear();

            std::vector<cow_child_delta> next_level;
            for (auto& [parent_idx, deltas] : grouped) {
                if (parent_idx >= topo.internal_nodes.size()) {
                    throw std::runtime_error(
                        "inconel recovery: CoW parent index out of range");
                }
                const auto parent_rb =
                    topo.internal_nodes[parent_idx].range_base;
                auto parent_bytes = read_base_internal_page(
                    device,
                    core,
                    profile,
                    geom,
                    tree_scan.tree,
                    parent_rb);
                tree::internal_page_reader reader;
                if (!reader.parse(parent_bytes.data(), geom.tree_page_size)) {
                    throw std::runtime_error(
                        "inconel recovery: failed to parse CoW parent page");
                }

                absl::flat_hash_map<format::paddr, std::size_t> delta_by_child;
                delta_by_child.reserve(deltas.size());
                for (std::size_t i = 0; i < deltas.size(); ++i) {
                    if (!delta_by_child
                             .emplace(deltas[i].old_child_range_base, i)
                             .second) {
                        throw std::runtime_error(
                            "inconel recovery: multiple CoW deltas target "
                            "one parent child");
                    }
                }

                std::vector<cow_replay_child_ref> new_children;
                std::vector<std::string> new_separators;
                const uint16_t old_separator_count = reader.record_count();
                const uint32_t old_child_count =
                    static_cast<uint32_t>(old_separator_count) + 1;
                new_children.reserve(old_child_count + deltas.size());
                new_separators.reserve(old_separator_count + deltas.size());

                for (uint32_t i = 0; i < old_child_count; ++i) {
                    if (!new_children.empty()) {
                        const auto sep =
                            reader.get(static_cast<uint16_t>(i - 1));
                        new_separators.emplace_back(sep.separator_key);
                    }

                    const format::paddr old_child =
                        (i < old_separator_count)
                            ? reader.get(static_cast<uint16_t>(i)).child_base
                            : reader.rightmost_child();
                    auto delta_it = delta_by_child.find(old_child);
                    if (delta_it == delta_by_child.end()) {
                        new_children.push_back(cow_replay_child_ref{
                            .target = old_child,
                        });
                        continue;
                    }

                    auto& delta = deltas[delta_it->second];
                    for (std::size_t n = 0; n < delta.nodes.size(); ++n) {
                        if (!new_children.empty() && n > 0) {
                            new_separators.push_back(
                                std::move(delta.sibling_seps[n - 1]));
                        }
                        new_children.push_back(cow_replay_child_ref{
                            .target = std::move(delta.nodes[n]),
                        });
                    }
                }

                std::vector<std::string> sibling_seps;
                auto built = build_cow_internal_pages(
                    std::move(new_children),
                    std::move(new_separators),
                    parent_rb,
                    /*is_new_layer=*/false,
                    geom.tree_page_size,
                    sibling_seps);
                plan_cow_group_against_old_range(
                    built,
                    parent_rb,
                    tree_scan.tree,
                    profile,
                    geom,
                    next_range_base,
                    allocation_limit_lba);

                const auto parent_parent_idx =
                    topo.internal_nodes[parent_idx].parent_idx;
                if (parent_parent_idx == core::kInvalidInternalIdx) {
                    final_root.emplace(finalize_cow_root_group(
                        std::move(built),
                        std::move(sibling_seps),
                        profile,
                        geom,
                        next_range_base,
                        allocation_limit_lba));
                    continue;
                }

                if (built.size() == 1 &&
                    built.front()->new_range_base == parent_rb) {
                    detached_subtrees.push_back(cow_replay_child_ref{
                        .target = std::move(built.front()),
                    });
                    continue;
                }

                next_level.push_back(cow_child_delta{
                    .old_child_range_base = parent_rb,
                    .parent_idx = parent_parent_idx,
                    .nodes = move_cow_node_vector(std::move(built)),
                    .sibling_seps = std::move(sibling_seps),
                });
            }

            current_level = std::move(next_level);
        }

        const format::paddr old_root_range = tree_scan.tree.root_range_base;
        cow_replay_child_ref root_ref{
            .target = old_root_range,
        };
        cow_replay_child_ref* root_to_write = nullptr;
        if (final_root.has_value()) {
            root_ref = std::move(*final_root);
            root_to_write = &root_ref;
        }

        write_cow_nodes(
            device,
            core,
            profile,
            geom,
            root_to_write,
            detached_subtrees);

        const format::paddr final_root_range = cow_child_range_base(root_ref);
        auto rescanned = scan_existing_tree(
            device, core, profile, geom, final_root_range);
        return cow_replay_result{
            .tree_scan = std::move(rescanned),
            .root_range_changed = final_root_range != old_root_range,
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
            .tree_free_ranges = {},
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
            auto active_source = choice.which;
            auto tree_scan = scan_existing_tree(
                device,
                core,
                profile,
                tree_geometry,
                choice.chosen->root_base_paddr);
            if (scan.saw_nonzero) {
                if (existing_tree_wal_delta_is_empty(tree_scan, scan)) {
                    reset_wal_region(device, core, profile);
                } else if (auto replayed = replay_same_shape_wal_delta(
                               device,
                               core,
                               profile,
                               tree_geometry,
                               tree_scan,
                               scan)) {
                    tree_scan = std::move(*replayed);
                    reset_wal_region(device, core, profile);
                } else if (auto replayed = replay_full_cow_wal_delta(
                               device,
                               core,
                               profile,
                               tree_geometry,
                               tree_scan,
                               scan)) {
                    tree_scan = std::move(replayed->tree_scan);
                    if (replayed->root_range_changed) {
                        active_source = write_recovered_superblock_root(
                            device,
                            core,
                            *choice.chosen,
                            choice.which,
                            tree_scan.tree.root_range_base,
                            profile);
                    }
                    reset_wal_region(device, core, profile);
                } else {
                    throw std::runtime_error(
                        "inconel recovery: WAL delta reached no-op CoW "
                        "fallback after non-empty delta detection");
                }
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
                .tree_free_ranges =
                    std::move(tree_scan.tree_free_ranges),
                .recovered_durable_lsn = recovered_durable_lsn,
                .next_lsn = recovered_durable_lsn + 1,
                .tree_alloc_head_lba = tree_scan.tree_alloc_head_lba,
                .active_superblock_source = active_source,
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
                    .tree_free_ranges =
                        std::move(replay.tree_free_ranges),
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
