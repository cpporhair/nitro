#ifndef APPS_INCONEL_TREE_LEAF_MAPPING_HH
#define APPS_INCONEL_TREE_LEAF_MAPPING_HH

// ── leaf_mapping.hh ── Phase 5 mapping algorithms (step 025 §2) ──
//
// Two inline free functions for the Phase 5 leaf mapping stage:
//
//   keys_to_leaf_groups(req, result)
//     Maps a contiguous sorted span of flush_key_groups to affected
//     leaves using the base manifest's leaf_order. Binary-search the
//     first key, then sequentially scan through leaves absorbing keys
//     that fall within each leaf's [fence_lower, fence_upper) range.
//     O(log L + N + H) where L = leaf count, N = key count, H = hit
//     leaf count.
//
//   merge_lookup_leaf_groups(mapping_results, leaf_groups_out)
//     Fan-in merge: sort mapping results by read_domain_index,
//     concatenate leaf groups, deduplicate adjacent entries hitting
//     the same leaf, and extend key spans.
//
// Neither function depends on tree_sched, registry, or any PUMP
// runtime type. They operate purely on flush carrier data structures
// and are independently testable.
//
// Marked `inline` for ODR safety — both worker_scheduler.hh and
// the test translation unit include this header.

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

#include "../core/leaf_order.hh"
#include "../core/panic.hh"
#include "../core/tree_manifest.hh"
#include "./flush_types.hh"

namespace apps::inconel::tree {

    // ── keys_to_leaf_groups ─────────────────────────────────────────
    //
    // Maps a sorted span of flush_key_groups to affected leaves using
    // the base manifest's leaf_order_index.
    //
    // Algorithm (D5):
    //   1. Binary-search leaf_order for the first key.
    //   2. Sequentially scan keys, accumulating them into the current
    //      leaf group as long as they fall within [lower, upper).
    //   3. When a key crosses the upper fence, advance to the next
    //      leaf (sequential, O(1) per leaf boundary).
    //   4. A gap between adjacent leaves (key < next leaf's lower)
    //      is a manifest corruption → panic_inconsistency.
    //
    // Fail-fast matrix (025 §fail-fast):
    //   - leaf_order empty && groups non-empty → unsupported_shape_change
    //   - key outside tree range → unsupported_shape_change
    //   - key falls in gap between leaves → panic_inconsistency

    inline void
    keys_to_leaf_groups(const flush_mapping_req& req,
                        flush_leaf_group_result& result)
    {
        const auto& lo = req.base_manifest->leaf_order;
        const auto& groups = req.groups;

        if (groups.empty()) {
            result.st = flush_stage_status::ok;
            return;
        }

        if (lo.empty()) {
            result.st = flush_stage_status::unsupported_shape_change;
            return;
        }

        // Binary-search for the leaf containing the first key.
        auto li = lo.find_leaf_for_key(groups[0].key);
        if (li >= lo.size()) {
            result.st = flush_stage_status::unsupported_shape_change;
            return;
        }

        std::size_t ki = 0;

        while (ki < groups.size()) {
            const auto& span = lo.spans[li];
            auto upper = lo.fence_upper(span);

            // Absorb all keys that fall within this leaf's range.
            std::size_t key_start = ki;
            while (ki < groups.size()) {
                // empty upper = +∞ → all remaining keys are in range
                if (!upper.empty() && groups[ki].key >= upper)
                    break;
                ++ki;
            }

            // Emit a leaf group if any keys hit this leaf.
            if (ki > key_start) {
                result.leaf_groups.push_back(flush_leaf_group{
                    .leaf_range_base = span.leaf_range_base,
                    .old_slot_paddr  = req.base_manifest->resolve(
                                           span.leaf_range_base),
                    .keys            = groups.subspan(key_start,
                                                      ki - key_start),
                });
            }

            if (ki >= groups.size()) break;

            // Advance to the next leaf.
            ++li;
            if (li >= lo.size()) {
                result.st = flush_stage_status::unsupported_shape_change;
                return;
            }

            // Verify contiguity: the next leaf's lower fence must
            // not leave a gap. A key that falls between two adjacent
            // leaves means the manifest is corrupt.
            auto next_lower = lo.fence_lower(lo.spans[li]);
            if (!next_lower.empty() && groups[ki].key < next_lower) {
                core::panic_inconsistency(
                    "keys_to_leaf_groups",
                    "key falls in gap between leaf %zu and %zu: "
                    "key=%.32s next_lower=%.32s",
                    li - 1, li,
                    std::string(groups[ki].key).c_str(),
                    std::string(next_lower).c_str());
            }
        }

        result.st = flush_stage_status::ok;
    }

    // ── merge_lookup_leaf_groups ─────────────────────────────────────
    //
    // Fan-in merge (D18): sort mapping results by read_domain_index
    // for deterministic ordering, propagate errors, concatenate leaf
    // groups, and deduplicate adjacent entries that hit the same leaf
    // (adjacent partitions may map keys to the same leaf because
    // partition boundaries are key-count-based, not leaf-aligned).
    //
    // Deduplication extends the first entry's `keys` span to cover
    // both groups' keys. This works because both spans borrow from
    // the same contiguous `flush_round_state.workset` vector and the
    // partitions are contiguous sub-spans — so the second group's
    // keys start exactly where the first group's keys end.

    inline flush_stage_status
    merge_lookup_leaf_groups(
        std::vector<flush_leaf_group_result>& mapping_results,
        std::vector<flush_leaf_group>& leaf_groups_out)
    {
        // D18: sort by read_domain_index for deterministic merge order.
        std::sort(mapping_results.begin(), mapping_results.end(),
                  [](const flush_leaf_group_result& a,
                     const flush_leaf_group_result& b) {
                      return a.read_domain_index < b.read_domain_index;
                  });

        for (auto& result : mapping_results) {
            if (result.st != flush_stage_status::ok)
                return result.st;

            for (auto& lg : result.leaf_groups) {
                // Adjacent same-leaf dedup: if the last emitted group
                // has the same leaf_range_base, extend its keys span
                // to cover this group's keys.
                if (!leaf_groups_out.empty() &&
                    leaf_groups_out.back().leaf_range_base == lg.leaf_range_base)
                {
                    auto& prev = leaf_groups_out.back();
                    // Verify contiguity: the new keys must start
                    // exactly where the previous keys end (both are
                    // sub-spans of the same workset vector).
                    auto prev_end = prev.keys.data() + prev.keys.size();
                    if (lg.keys.data() != prev_end) {
                        core::panic_inconsistency(
                            "merge_lookup_leaf_groups",
                            "non-contiguous spans for same leaf: "
                            "prev_end=%p new_start=%p",
                            static_cast<const void*>(prev_end),
                            static_cast<const void*>(lg.keys.data()));
                    }
                    // Extend the span.
                    prev.keys = std::span<const flush_key_group>(
                        prev.keys.data(),
                        prev.keys.size() + lg.keys.size());
                } else {
                    leaf_groups_out.push_back(std::move(lg));
                }
            }
        }

        return flush_stage_status::ok;
    }

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_LEAF_MAPPING_HH
