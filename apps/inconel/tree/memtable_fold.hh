#ifndef APPS_INCONEL_TREE_MEMTABLE_FOLD_HH
#define APPS_INCONEL_TREE_MEMTABLE_FOLD_HH

// ── memtable_fold.hh ── Phase 4 fold + partition free functions ──
//
// Two inline free functions for the Phase 4 memtable fold stage:
//
//   fold_pinned_gens(flush_round_state& rs)
//     K-way merge across all pinned sealed gens; produces sorted
//     workset + staged loser entries on the round_state.
//
//   build_key_partitions(flush_round_state& rs, uint32_t lookup_count)
//     Equal-count contiguous partitioning of the workset into P
//     partitions for Phase 5 lookup fanout.
//
// Neither function depends on tree_sched, registry, or any PUMP
// runtime type. They operate purely on flush_round_state +
// memtable_gen data structures and are independently testable.
//
// Marked `inline` for ODR safety — both owner_scheduler.hh and
// the test translation unit include this header.

#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "./flush_round_state.hh"
#include "./flush_types.hh"
#include "../core/memtable.hh"

namespace apps::inconel::tree {

    // ── fold_pinned_gens ─────────────────────────────────────────
    //
    // K-way merge fold (plan §4.1, design_overview §9 FF §3.3).
    //
    // For each unique key across all pinned gens, selects the winner
    // entry (max data_ver) and appends a flush_key_group to
    // rs.workset. Non-winner value entries are pushed directly into
    // each gen's `loser_durable_refs` (no staging on round_state).
    //
    // Idempotency: fold clears every gen's loser_durable_refs at the
    // start. If the round fails and the same gens are re-folded, the
    // clear ensures no double-push. In practice flush failure =
    // engine restart (NVMe I/O failure is fatal), so re-fold of the
    // same gen never happens in production. See review E-2 §7.
    //
    // Exploits InlinedVector<memtable_entry, 1> data_ver strict
    // ascending order: back() = gen-local winner (O(1)),
    // [0..n-2] = intra-gen losers pushed directly (zero comparison).
    // Cross-gen comparison only among K gen-local winners (O(K)).
    //
    // Complexity: O(N * K) where N = unique keys, K = gen count.
    // K is typically <= 8; this runs on the background flush path.

    inline void
    fold_pinned_gens(flush_round_state& rs) {
        const auto K = static_cast<uint32_t>(rs.pinned_gens.size());
        if (K == 0) return;

        // Reserve workset. total_entries is the upper bound on
        // unique keys (exact when no overlap across gens).
        // Eliminates ~23 realloc+copy rounds for 10M-key flushes.
        {
            std::size_t total_entries = 0;
            for (const auto& g : rs.pinned_gens)
                total_entries += g->table.size();
            rs.workset.reserve(total_entries);
        }

        // Clear every gen's loser_durable_refs for idempotency.
        for (const auto& g : rs.pinned_gens)
            g->loser_durable_refs.clear();

        // Per-gen iterator state.
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

        // Per-key candidate: the gen-local winner extracted from
        // back(). Allocated once on the stack (K <= 8).
        struct gen_candidate {
            uint32_t                    gen_index;
            const core::memtable_entry* entry;
            std::string_view            key;  // from this gen's btree_map
        };
        absl::InlinedVector<gen_candidate, 8> candidates;

        // Main merge loop: each iteration produces one
        // flush_key_group for the lexicographically smallest key.
        for (;;) {
            // (a) Find the minimum key among all non-exhausted cursors.
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
            if (!found) break;  // all cursors exhausted

            candidates.clear();

            // (b) For each gen holding min_key: push intra-gen
            //     losers directly into gen, extract gen-local
            //     winner (back()) as candidate.
            for (uint32_t gi = 0; gi < K; ++gi) {
                if (cursors[gi].it == cursors[gi].end) continue;
                if (cursors[gi].it->first != min_key) continue;

                const auto& gen_key    = cursors[gi].it->first;
                const auto& entries_vec = cursors[gi].it->second;
                const auto  n = entries_vec.size();
                auto&       gen = *rs.pinned_gens[gi];

                // Intra-gen losers: entries [0..n-2], all have
                // data_ver < back().data_ver. Push value losers
                // directly into gen — zero comparison needed.
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

                // Gen-local winner: back().
                candidates.push_back({
                    .gen_index = gi,
                    .entry     = &entries_vec.back(),
                    .key       = gen_key,
                });

                ++cursors[gi].it;
            }

            // (c) Cross-gen comparison: pick global winner among
            //     at most K gen-local winners (O(K)).
            gen_candidate* global_winner = &candidates[0];
            for (std::size_t ci = 1; ci < candidates.size(); ++ci) {
                if (candidates[ci].entry->data_ver >
                    global_winner->entry->data_ver) {
                    global_winner = &candidates[ci];
                }
            }

            // (d) Inter-gen losers: gen-local winners that lost
            //     the cross-gen comparison. Push directly into gen.
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

            // (e) Produce the flush_key_group for the global winner.
            rs.workset.push_back(flush_key_group{
                .key                     = global_winner->key,
                .winner_data_ver         = global_winner->entry->data_ver,
                .winner_kind             = global_winner->entry->k,
                .winner_value            = global_winner->entry->vh,
                .winner_pinned_gen_index = global_winner->gen_index,
            });
        }
    }

    // ── build_key_partitions ─────────────────────────────────────
    //
    // Equal-count contiguous partitioning (plan §4.2, D7).
    //
    // Splits rs.workset into P = min(lookup_count, N) partitions
    // where each partition's `groups` span borrows contiguous
    // elements from the workset vector. Only non-empty partitions
    // are emitted.
    //
    // Precondition: workset is fully populated and must NOT be
    // mutated (push_back / resize) after this call, otherwise the
    // spans would be invalidated by a potential reallocation.

    inline void
    build_key_partitions(flush_round_state& rs, uint32_t lookup_count) {
        const auto N = static_cast<uint32_t>(rs.workset.size());
        if (N == 0) return;

        const uint32_t P = std::min(lookup_count, N);
        rs.partitions.reserve(P);

        for (uint32_t i = 0; i < P; ++i) {
            const uint32_t start = static_cast<uint32_t>(
                static_cast<uint64_t>(i) * N / P);
            const uint32_t end = static_cast<uint32_t>(
                static_cast<uint64_t>(i + 1) * N / P);
            if (start < end) {
                rs.partitions.push_back(flush_key_partition{
                    .read_domain_index = i,
                    .groups = std::span<const flush_key_group>(
                        rs.workset.data() + start, end - start),
                });
            }
        }
    }

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_MEMTABLE_FOLD_HH
