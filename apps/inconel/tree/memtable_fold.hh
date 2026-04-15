#ifndef APPS_INCONEL_TREE_MEMTABLE_FOLD_HH
#define APPS_INCONEL_TREE_MEMTABLE_FOLD_HH

// ── memtable_fold.hh ── fold + leaf-aligned partition free functions ──
//
// Two inline free functions for the fold + partition stage:
//
//   fold_pinned_gens(flush_round_state& rs)
//     K-way merge across all pinned sealed gens; produces sorted
//     workset + losers pushed directly into each gen's
//     loser_durable_refs.
//
//   build_key_partitions(flush_round_state& rs,
//                        const core::tree_manifest* base_manifest,
//                        uint32_t worker_count)
//     Step 027 §4.3: leaf-align partitions. Every key is mapped to
//     its base_manifest leaf via `leaf_order.find_leaf_for_key()`,
//     then assigned to partition `leaf_idx % worker_count`. The
//     workset is rewritten so each partition occupies a contiguous
//     slice; `flush_key_partition.groups` is a span over that
//     slice. Within a partition, keys remain in sorted key order.
//
// Neither function depends on tree_sched, registry, or any PUMP
// runtime type. They operate purely on flush carrier data structures.
//
// Marked `inline` for ODR safety.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "../core/memtable.hh"
#include "../core/tree_manifest.hh"
#include "./flush_round_state.hh"
#include "./flush_types.hh"

namespace apps::inconel::tree {

    // ── fold_pinned_gens ─────────────────────────────────────────
    //
    // K-way merge fold (FF §3.3). For each unique key across all
    // pinned gens, selects the winner entry (max data_ver) and
    // appends a flush_key_group to rs.workset. Non-winner value
    // entries are pushed directly into each gen's
    // `loser_durable_refs`.
    //
    // Idempotency: fold clears every gen's loser_durable_refs at
    // the start. If the round fails and the same gens are re-folded,
    // the clear ensures no double-push.
    //
    // Exploits InlinedVector<memtable_entry, 1> data_ver strict
    // ascending order: back() = gen-local winner (O(1)),
    // [0..n-2] = intra-gen losers pushed directly. Cross-gen
    // comparison only among K gen-local winners (O(K)).
    //
    // Complexity: O(N * K) where N = unique keys, K = gen count.

    inline void
    fold_pinned_gens(flush_round_state& rs) {
        const auto K = static_cast<uint32_t>(rs.pinned_gens.size());
        if (K == 0) return;

        {
            std::size_t total_entries = 0;
            for (const auto& g : rs.pinned_gens)
                total_entries += g->table.size();
            rs.workset.reserve(total_entries);
        }

        for (const auto& g : rs.pinned_gens)
            g->loser_durable_refs.clear();

        using table_t = decltype(core::memtable_gen::table);
        using iter_t  = table_t::const_iterator;

        struct gen_cursor {
            iter_t  it;
            iter_t  end;
        };

        absl::InlinedVector<gen_cursor, 8> cursors;
        cursors.reserve(K);
        for (const auto& g : rs.pinned_gens) {
            cursors.push_back({
                .it  = g->table.cbegin(),
                .end = g->table.cend(),
            });
        }

        struct gen_candidate {
            uint32_t                    gen_index;
            const core::memtable_entry* entry;
            std::string_view            key;
        };
        absl::InlinedVector<gen_candidate, 8> candidates;

        for (;;) {
            std::string_view min_key;
            bool found = false;
            for (uint32_t gi = 0; gi < K; ++gi) {
                if (cursors[gi].it == cursors[gi].end) continue;
                const auto& candidate = cursors[gi].it->first;
                if (!found || candidate < min_key) {
                    min_key = candidate;
                    found   = true;
                }
            }
            if (!found) break;

            candidates.clear();

            for (uint32_t gi = 0; gi < K; ++gi) {
                if (cursors[gi].it == cursors[gi].end) continue;
                if (cursors[gi].it->first != min_key) continue;

                const auto& gen_key    = cursors[gi].it->first;
                const auto& entries_vec = cursors[gi].it->second;
                const auto  n = entries_vec.size();
                auto&       gen = *rs.pinned_gens[gi];

                for (std::size_t i = 0; i + 1 < n; ++i) {
                    const auto& e = entries_vec[i];
                    if (e.k == core::memtable_entry::kind::value) {
                        gen.loser_durable_refs.push(
                            core::retired_value_ref{
                                .vr       = e.vh.durable,
                                .data_ver = e.data_ver,
                            });
                    }
                }

                candidates.push_back({
                    .gen_index = gi,
                    .entry     = &entries_vec.back(),
                    .key       = gen_key,
                });

                ++cursors[gi].it;
            }

            gen_candidate* global_winner = &candidates[0];
            for (std::size_t ci = 1; ci < candidates.size(); ++ci) {
                if (candidates[ci].entry->data_ver >
                    global_winner->entry->data_ver) {
                    global_winner = &candidates[ci];
                }
            }

            for (auto& c : candidates) {
                if (&c == global_winner) continue;
                if (c.entry->k == core::memtable_entry::kind::value) {
                    rs.pinned_gens[c.gen_index]->loser_durable_refs.push(
                        core::retired_value_ref{
                            .vr       = c.entry->vh.durable,
                            .data_ver = c.entry->data_ver,
                        });
                }
            }

            rs.workset.push_back(flush_key_group{
                .key                     = global_winner->key,
                .winner_data_ver         = global_winner->entry->data_ver,
                .winner_kind             = global_winner->entry->k,
                .winner_value            = global_winner->entry->vh,
                .winner_pinned_gen_index = global_winner->gen_index,
            });
        }
    }

    // ── build_key_partitions (Phase 7 / step 027 §4.3) ──────────
    //
    // Leaf-aligned key-range partitioning: every key is mapped to
    // its base_manifest leaf, then assigned to partition
    // `(leaf_idx * P) / leaf_count`. This gives worker N a
    // contiguous range of leaves `[N*L/P, (N+1)*L/P)`, which is
    // what step 027 §3.3's pairwise leaf-merge pass requires —
    // adjacent leaves in a worker's slice are also tree-adjacent,
    // so a pairwise-merged page covers a contiguous key range
    // without colliding with other workers' coverage.
    //
    // (Step 027 §4.3 originally wrote `leaf_idx % worker_count`
    // round-robin. Range partitioning is the only assignment that
    // makes §3.3 semantically sound; that doc text is a typo and
    // is being corrected when 027 is folded into design_doc/.)
    //
    // Workset is rewritten so each partition occupies a contiguous
    // slice; `flush_key_partition.groups` is a span over that
    // slice. Within a partition keys remain in sorted order
    // because grouping preserves relative order from a sorted
    // source.
    //
    // Returns:
    //   - flush_stage_status::ok                       — all keys mapped
    //   - flush_stage_status::unsupported_shape_change — any key falls
    //     outside the tree's covered range, OR the tree has no root
    //     yet (bootstrap is Phase 9 territory)
    //
    // Precondition: workset is fully populated (fold has run) and
    // is not mutated after this call returns a partition span.

    inline flush_stage_status
    build_key_partitions(flush_round_state& rs,
                         const core::tree_manifest* base_manifest,
                         uint32_t worker_count)
    {
        const auto N = static_cast<uint32_t>(rs.workset.size());
        if (N == 0) return flush_stage_status::ok;

        if (worker_count == 0) {
            return flush_stage_status::unsupported_shape_change;
        }

        // Empty tree (no root yet) — bootstrap is not in Phase 7
        // scope. Constraint A: declare narrowing explicitly via the
        // unsupported_shape_change return; the worker fanout is
        // skipped for this round and the merge step propagates the
        // status upward.
        const auto& lo = base_manifest->leaf_order;
        if (lo.empty()) {
            return flush_stage_status::unsupported_shape_change;
        }

        const auto leaf_count = static_cast<uint32_t>(lo.size());
        const uint32_t P = std::min(worker_count, leaf_count);

        std::vector<std::vector<flush_key_group>> buckets(P);

        for (const auto& k : rs.workset) {
            auto leaf_idx = lo.find_leaf_for_key(k.key);
            if (leaf_idx >= leaf_count) {
                // Key beyond tree's covered key range. v1 spec
                // requires the writer to grow the tree to absorb
                // such keys; that is shape-changing on the boot /
                // bound side and lives in Phase 9.
                return flush_stage_status::unsupported_shape_change;
            }
            // Range assignment: partition_idx = leaf_idx * P / leaf_count.
            // The 64-bit promotion avoids overflow even at 10 billion
            // keys / 4 KB-page (~15.6 M leaves) × P up to a few hundred.
            const uint32_t p = static_cast<uint32_t>(
                (static_cast<uint64_t>(leaf_idx) * P) / leaf_count);
            buckets[p].push_back(k);
        }

        // Rewrite workset as the concatenation of buckets so each
        // partition's slice is contiguous.
        rs.workset.clear();
        rs.workset.reserve(N);

        std::vector<uint32_t> bucket_offsets(P);
        for (uint32_t p = 0; p < P; ++p) {
            bucket_offsets[p] = static_cast<uint32_t>(rs.workset.size());
            for (auto& k : buckets[p]) {
                rs.workset.push_back(k);
            }
        }

        rs.partitions.reserve(P);
        for (uint32_t p = 0; p < P; ++p) {
            const auto offset = bucket_offsets[p];
            const auto count  = static_cast<uint32_t>(buckets[p].size());
            if (count == 0) continue;
            rs.partitions.push_back(flush_key_partition{
                .read_domain_index = p,
                .groups = std::span<const flush_key_group>(
                    rs.workset.data() + offset, count),
            });
        }

        return flush_stage_status::ok;
    }

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_MEMTABLE_FOLD_HH
