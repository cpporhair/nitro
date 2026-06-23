#ifndef APPS_INCONEL_RECOVERY_TREE_SCANNER_HH
#define APPS_INCONEL_RECOVERY_TREE_SCANNER_HH

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "../core/leaf_order.hh"
#include "../core/tree_geometry.hh"
#include "../core/tree_reverse_topology.hh"
#include "../format/format_profile.hh"
#include "../format/tree_page.hh"
#include "../format/types.hh"
#include "../tree/page_reader.hh"
#include "./state.hh"
#include "./sync_io.hh"

namespace apps::inconel::recovery {

    struct recovered_tree_record {
        std::string key;
        format::paddr leaf_range_base{};
        uint64_t data_ver = 0;
        format::record_kind kind = format::record_kind::tombstone;
        format::value_ref vr{};
    };

    struct recovered_tree_scan {
        recovered_tree_snapshot tree;
        std::vector<value::live_value_extent> live_value_extents;
        std::vector<format::range_ref> tree_free_ranges;
        std::vector<recovered_tree_record> records;
        uint64_t max_data_ver = 0;
        uint64_t tree_alloc_head_lba = 0;
    };

    namespace tree_scan_detail {

        struct scanned_page {
            format::paddr range_base{};
            uint32_t slot_index = 0;
            format::paddr slot_paddr{};
            format::node_type type = format::node_type::leaf;
            std::vector<char> bytes;
        };

        struct leaf_item {
            format::paddr range_base{};
            std::string first_key;
            std::string last_key;
            format::paddr parent_range{};
        };

        struct internal_item {
            format::paddr range_base{};
            format::paddr parent_range{};
        };

        template <typename Device>
        struct scan_context {
            Device& device;
            uint32_t core = 0;
            const format::format_profile& profile;
            const core::tree_geometry& geom;

            absl::flat_hash_map<format::paddr, uint32_t> slot_map;
            std::vector<leaf_item> leaves;
            std::vector<internal_item> internals;
            std::vector<value::live_value_extent> live_extents;
            std::vector<recovered_tree_record> records;
            absl::flat_hash_set<std::string> seen_keys;
            uint64_t max_data_ver = 0;
        };

        [[nodiscard]] inline bool
        paddr_is_null(format::paddr p) noexcept {
            return p.device_id == 0 && p.lba == 0;
        }

        template <typename Device>
        inline void
        validate_range_base(const scan_context<Device>& ctx,
                            format::paddr range_base) {
            if (range_base.device_id !=
                ctx.profile.value_data_area_base.device_id) {
                throw std::runtime_error(
                    "inconel recovery: tree child range device mismatch");
            }
            const uint64_t range_lbas = ctx.geom.range_lbas();
            if (range_lbas == 0 ||
                range_base.lba < ctx.profile.value_data_area_base.lba ||
                range_base.lba >
                    std::numeric_limits<uint64_t>::max() - range_lbas ||
                range_base.lba > ctx.profile.value_data_area_end.lba ||
                range_base.lba + range_lbas >
                    ctx.profile.value_data_area_end.lba) {
                throw std::runtime_error(
                    "inconel recovery: tree child range outside data area");
            }
            const uint64_t delta =
                range_base.lba - ctx.profile.value_data_area_base.lba;
            if (delta % range_lbas != 0) {
                throw std::runtime_error(
                    "inconel recovery: tree child range is not range-aligned");
            }
        }

        template <typename Device>
        [[nodiscard]] inline scanned_page
        read_current_page(scan_context<Device>& ctx,
                          format::paddr range_base) {
            validate_range_base(ctx, range_base);
            auto page =
                make_zeroed_sync_io_buffer(
                    ctx.device, ctx.geom.tree_page_size, ctx.profile.lba_size);

            for (uint32_t rev = 0; rev < ctx.geom.shadow_slots_per_range; ++rev) {
                const uint32_t slot =
                    ctx.geom.shadow_slots_per_range - 1u - rev;
                const format::paddr slot_paddr =
                    ctx.geom.slot_paddr(range_base, slot);
                sync_read_logical_lbas(
                    ctx.device,
                    ctx.core,
                    slot_paddr.lba,
                    ctx.geom.page_lbas(),
                    page.get(),
                    ctx.profile.lba_size);

                const auto status =
                    format::inspect_tree_page(page.get(), ctx.geom.tree_page_size);
                if (status != format::tree_page_status::ok) {
                    continue;
                }

                format::tree_slot_header header{};
                std::memcpy(&header, page.get(), sizeof(header));
                std::vector<char> bytes(ctx.geom.tree_page_size);
                std::memcpy(bytes.data(), page.get(), bytes.size());
                return scanned_page{
                    .range_base = range_base,
                    .slot_index = slot,
                    .slot_paddr = slot_paddr,
                    .type = header.type,
                    .bytes = std::move(bytes),
                };
            }

            throw std::runtime_error(
                "inconel recovery: selected tree range has no valid slot");
        }

        template <typename Device>
        inline void
        scan_range(scan_context<Device>& ctx,
                   format::paddr range_base,
                   format::paddr parent_range);

        template <typename Device>
        inline void
        scan_leaf(scan_context<Device>& ctx,
                  const scanned_page& page,
                  format::paddr parent_range) {
            tree::leaf_page_reader reader;
            if (!reader.parse(page.bytes.data(), ctx.geom.tree_page_size)) {
                throw std::runtime_error(
                    "inconel recovery: failed to parse selected leaf page");
            }

            std::string first_key;
            std::string last_key;
            std::optional<std::string> prev_key;
            for (uint16_t i = 0; i < reader.record_count(); ++i) {
                const auto rec = reader.get(i);
                if (prev_key && rec.key <= std::string_view(*prev_key)) {
                    throw std::runtime_error(
                        "inconel recovery: leaf keys are not strictly sorted");
                }
                if (i == 0) {
                    first_key = std::string(rec.key);
                }
                auto [_, inserted] =
                    ctx.seen_keys.insert(std::string(rec.key));
                if (!inserted) {
                    throw std::runtime_error(
                        "inconel recovery: duplicate key across tree leaves");
                }
                ctx.max_data_ver = std::max(ctx.max_data_ver, rec.data_ver);
                ctx.records.push_back(recovered_tree_record{
                    .key = std::string(rec.key),
                    .leaf_range_base = page.range_base,
                    .data_ver = rec.data_ver,
                    .kind = rec.kind,
                    .vr = rec.vr,
                });
                if (rec.kind == format::record_kind::value) {
                    ctx.live_extents.push_back(value::live_value_extent{
                        .base = rec.vr.base,
                        .byte_offset = rec.vr.byte_offset,
                        .len = rec.vr.len,
                    });
                } else if (rec.kind != format::record_kind::tombstone) {
                    throw std::runtime_error(
                        "inconel recovery: unknown leaf record kind");
                }
                prev_key = std::string(rec.key);
                last_key = *prev_key;
            }

            ctx.leaves.push_back(leaf_item{
                .range_base = page.range_base,
                .first_key = std::move(first_key),
                .last_key = std::move(last_key),
                .parent_range = parent_range,
            });
        }

        template <typename Device>
        inline void
        scan_internal(scan_context<Device>& ctx,
                      const scanned_page& page,
                      format::paddr parent_range) {
            tree::internal_page_reader reader;
            if (!reader.parse(page.bytes.data(), ctx.geom.tree_page_size)) {
                throw std::runtime_error(
                    "inconel recovery: failed to parse selected internal page");
            }

            ctx.internals.push_back(internal_item{
                .range_base = page.range_base,
                .parent_range = parent_range,
            });

            std::optional<std::string> prev_separator;
            for (uint16_t i = 0; i < reader.record_count(); ++i) {
                const auto entry = reader.get(i);
                if (prev_separator &&
                    entry.separator_key <=
                        std::string_view(*prev_separator)) {
                    throw std::runtime_error(
                        "inconel recovery: internal separators are not "
                        "strictly sorted");
                }
                if (entry.child_base == page.range_base) {
                    throw std::runtime_error(
                        "inconel recovery: internal page points to itself");
                }
                scan_range(ctx, entry.child_base, page.range_base);
                prev_separator = std::string(entry.separator_key);
            }

            const auto rightmost = reader.rightmost_child();
            if (rightmost == page.range_base) {
                throw std::runtime_error(
                    "inconel recovery: internal rightmost child points to "
                    "itself");
            }
            scan_range(ctx, rightmost, page.range_base);
        }

        template <typename Device>
        inline void
        scan_range(scan_context<Device>& ctx,
                   format::paddr range_base,
                   format::paddr parent_range) {
            if (ctx.slot_map.contains(range_base)) {
                throw std::runtime_error(
                    "inconel recovery: selected tree range is reachable "
                    "through multiple paths");
            }

            auto page = read_current_page(ctx, range_base);
            ctx.slot_map.emplace(range_base, page.slot_index);
            if (page.type == format::node_type::leaf) {
                scan_leaf(ctx, page, parent_range);
            } else if (page.type == format::node_type::internal) {
                scan_internal(ctx, page, parent_range);
            } else {
                throw std::runtime_error(
                    "inconel recovery: tree page has unknown node type");
            }
        }

        [[nodiscard]] inline core::leaf_order_index
        build_leaf_order(const std::vector<leaf_item>& leaves) {
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
                if (i > 0) {
                    if (leaves[i].first_key.empty()) {
                        throw std::runtime_error(
                            "inconel recovery: non-first leaf is empty");
                    }
                    if (!leaves[i - 1].last_key.empty() &&
                        leaves[i - 1].last_key >= leaves[i].first_key) {
                        throw std::runtime_error(
                            "inconel recovery: leaf ranges are not globally "
                            "ordered");
                    }
                }
                auto lower = next_lower;
                std::pair<uint32_t, uint16_t> upper;
                if (i + 1 < leaves.size()) {
                    if (leaves[i + 1].first_key.empty()) {
                        throw std::runtime_error(
                            "inconel recovery: cannot rebuild leaf_order "
                            "for a non-first empty leaf");
                    }
                    upper = append_fence(leaves[i + 1].first_key);
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
                    .leaf_range_base = leaves[i].range_base,
                });
            }
            return out;
        }

        [[nodiscard]] inline core::tree_reverse_topology
        build_reverse_topology(const std::vector<leaf_item>& leaves,
                               const std::vector<internal_item>& internals) {
            core::tree_reverse_topology out;
            out.internal_nodes.reserve(internals.size());
            absl::flat_hash_map<format::paddr, core::internal_idx> idx_by_range;
            idx_by_range.reserve(internals.size());

            for (const auto& item : internals) {
                idx_by_range.emplace(
                    item.range_base,
                    static_cast<core::internal_idx>(out.internal_nodes.size()));
                out.internal_nodes.push_back(core::internal_node_entry{
                    .range_base = item.range_base,
                    .parent_idx = core::kInvalidInternalIdx,
                });
            }

            for (std::size_t i = 0; i < internals.size(); ++i) {
                const auto parent = internals[i].parent_range;
                if (!paddr_is_null(parent)) {
                    out.internal_nodes[i].parent_idx =
                        idx_by_range.at(parent);
                }
            }

            out.leaf_parent_idx.reserve(leaves.size());
            for (const auto& leaf : leaves) {
                if (paddr_is_null(leaf.parent_range)) {
                    out.leaf_parent_idx.push_back(core::kInvalidInternalIdx);
                } else {
                    out.leaf_parent_idx.push_back(
                        idx_by_range.at(leaf.parent_range));
                }
            }

            return out;
        }

        [[nodiscard]] inline uint64_t
        compute_tree_alloc_head_lba(
            const format::format_profile& profile,
            const core::tree_geometry& geom,
            const absl::flat_hash_map<format::paddr, uint32_t>& slot_map) {
            uint64_t head = profile.value_data_area_base.lba;
            const uint64_t range_lbas = geom.range_lbas();
            for (const auto& [range_base, _] : slot_map) {
                head = std::max(head, range_base.lba + range_lbas);
            }
            return head;
        }

        [[nodiscard]] inline std::vector<format::range_ref>
        compute_tree_free_ranges(
            const format::format_profile& profile,
            const core::tree_geometry& geom,
            const absl::flat_hash_map<format::paddr, uint32_t>& slot_map,
            uint64_t tree_alloc_head_lba) {
            std::vector<format::range_ref> out;
            const uint64_t range_lbas = geom.range_lbas();
            if (range_lbas == 0) {
                throw std::runtime_error(
                    "inconel recovery: tree range_lbas is 0");
            }
            for (uint64_t lba = profile.value_data_area_base.lba;
                 lba < tree_alloc_head_lba;
                 lba += range_lbas) {
                if (tree_alloc_head_lba - lba < range_lbas) {
                    throw std::runtime_error(
                        "inconel recovery: tree_alloc_head is not range-aligned");
                }
                format::paddr range_base{
                    profile.value_data_area_base.device_id,
                    lba,
                };
                if (!slot_map.contains(range_base)) {
                    out.push_back(geom.range_ref_from_base(range_base));
                }
            }
            return out;
        }

    }  // namespace tree_scan_detail

    [[nodiscard]] inline recovered_tree_scan
    scan_existing_tree(auto& device,
                       uint32_t core,
                       const format::format_profile& profile,
                       const core::tree_geometry& geom,
                       format::paddr root_range_base) {
        if (root_range_base.lba == 0 && root_range_base.device_id == 0) {
            return recovered_tree_scan{
                .tree = {},
                .live_value_extents = {},
                .tree_free_ranges = {},
                .records = {},
                .max_data_ver = 0,
                .tree_alloc_head_lba = profile.value_data_area_base.lba,
            };
        }

        tree_scan_detail::scan_context<std::remove_reference_t<decltype(device)>> ctx{
            .device = device,
            .core = core,
            .profile = profile,
            .geom = geom,
        };
        tree_scan_detail::scan_range(
            ctx, root_range_base, format::paddr{0, 0});

        if (ctx.leaves.empty()) {
            throw std::runtime_error(
                "inconel recovery: selected tree has no reachable leaf");
        }
        if (ctx.max_data_ver == 0) {
            throw std::runtime_error(
                "inconel recovery: selected tree has no durable LSN "
                "frontier carrier");
        }

        auto leaf_order = tree_scan_detail::build_leaf_order(ctx.leaves);
        auto reverse_topology =
            tree_scan_detail::build_reverse_topology(
                ctx.leaves, ctx.internals);
        const uint32_t root_slot_index = ctx.slot_map.at(root_range_base);
        recovered_tree_snapshot tree{
            .root_slot = geom.slot_paddr(root_range_base, root_slot_index),
            .slot_map = std::move(ctx.slot_map),
            .leaf_order = std::move(leaf_order),
            .root_range_base = root_range_base,
            .reverse_topology = std::move(reverse_topology),
        };
        const uint64_t tree_alloc_head =
            tree_scan_detail::compute_tree_alloc_head_lba(
                profile, geom, tree.slot_map);
        auto tree_free_ranges =
            tree_scan_detail::compute_tree_free_ranges(
                profile, geom, tree.slot_map, tree_alloc_head);

        return recovered_tree_scan{
            .tree = std::move(tree),
            .live_value_extents = std::move(ctx.live_extents),
            .tree_free_ranges = std::move(tree_free_ranges),
            .records = std::move(ctx.records),
            .max_data_ver = ctx.max_data_ver,
            .tree_alloc_head_lba = tree_alloc_head,
        };
    }

}  // namespace apps::inconel::recovery

#endif  // APPS_INCONEL_RECOVERY_TREE_SCANNER_HH
