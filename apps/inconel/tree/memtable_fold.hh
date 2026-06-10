#ifndef APPS_INCONEL_TREE_MEMTABLE_FOLD_HH
#define APPS_INCONEL_TREE_MEMTABLE_FOLD_HH

// ── memtable_fold.hh ── fold + shard-partition partition free functions ──
//
// Two inline free functions for the fold + partition stage:
//
//   fold_pinned_gens(flush_round_state& rs)
//     K-way merge across all pinned sealed gens; produces sorted
//     workset + losers pushed directly into each gen's
//     loser_durable_refs.
//
//   build_key_partitions(flush_round_state& rs,
//                        const core::tree_manifest* base_manifest)
//     Step 030 §2.5: every key is routed to a read_domain via the
//     globally installed `shard_partition_map`
//     (`core::registry::current_shard_partitions()->route(key)`).
//     Both the read path (`tree::lookup`) and the flush path (this
//     function) route through the same map, so a given key always
//     lands on the same `tree_read_domain` — this is the
//     precondition for the "one tree_node cache shard per leaf
//     range" invariant (RSM §4.7). The map's `+∞` sentinel
//     guarantees every key lands on some shard; no coverage-gap
//     panic is needed.
//
// `build_key_partitions` depends on `core::registry::current_shard_partitions()`;
// otherwise the fold path stays free of PUMP runtime types.
//
// Marked `inline` for ODR safety.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "../core/memtable.hh"
#include "../core/panic.hh"
#include "../core/shard_partition.hh"
#include "../core/tree_manifest.hh"
#include "./flush_round_state.hh"
#include "./flush_types.hh"

// Forward-declare `current_shard_partitions()` instead of #including
// `core/registry.hh` — that header also includes `tree/scheduler.hh`,
// which re-enters this file through `owner_scheduler.hh` and would
// hit a dependency cycle where the registry namespace is not yet
// defined at the point we try to call into it.
namespace apps::inconel::core::registry {
    std::shared_ptr<const apps::inconel::core::shard_partition_map>
    current_shard_partitions();
}

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

                if (n == 0) {
                    core::panic_inconsistency(
                        "tree::fold_pinned_gens",
                        "empty version vector for key");
                }

                const core::memtable_entry* gen_winner = nullptr;
                std::size_t                 gen_winner_index = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    const auto& e = entries_vec[i];
                    if (gen_winner == nullptr ||
                        e.data_ver > gen_winner->data_ver) {
                        gen_winner = &e;
                        gen_winner_index = i;
                    }
                }

                for (std::size_t i = 0; i < n; ++i) {
                    if (i == gen_winner_index) continue;
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
                    .entry     = gen_winner,
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

    // ── build_key_partitions (step 030 §2.5) ─────────────────────
    //
    // Routes every key in the workset through the globally installed
    // `shard_partition_map`, which is the SAME decision surface the
    // read path uses. The folded workset is already sorted by key, so
    // this function emits a partition for each consecutive same-shard
    // run and does not rewrite/copy the workset. If a future routing
    // table maps multiple discontiguous fence ranges to the same
    // shard_idx, that read_domain simply receives multiple partition
    // requests.
    //
    // Empty base_manifest is a legitimate bootstrap case — the
    // first flush on a freshly formatted disk. Routing still works
    // through the installed shard_partition_map (bootstrap
    // placeholder at boot, rebuilt by tree_sched after each
    // successful flush); every key lands on some read_domain. The
    // worker (`candidate_build.hh::initialize_worker`) and the
    // owner merge coroutine (`owner_scheduler.hh::run_merge`) each
    // carry a matching bootstrap branch that produces fresh leaves
    // and a fresh root instead of merging against the (nonexistent)
    // old tree. `base_manifest` is therefore not consulted here —
    // we keep the parameter so the call site (`handle_fold_req`)
    // stays stable and future per-manifest validation has an
    // obvious attachment point.
    //
    // Key coverage: the map's `+∞` sentinel guarantees every key
    // falls into some shard; no "outside tree coverage" panic is
    // needed (compare `shard_partition_map::route` vs the old
    // `leaf_order.find_leaf_for_key` path).
    //
    // Returns:
    //   - flush_stage_status::ok — all keys mapped to a shard
    //
    // Precondition: workset is fully populated (fold has run) and
    // is not mutated after this call returns a partition span.

    inline flush_stage_status
    build_key_partitions(flush_round_state& rs,
                         const core::tree_manifest* base_manifest)
    {
        (void)base_manifest;  // routing source is shard_partition_map, not the manifest

        const auto N = static_cast<uint32_t>(rs.workset.size());
        if (N == 0) return flush_stage_status::ok;

        auto partitions = core::registry::current_shard_partitions();
        if (!partitions || partitions->empty()) {
            core::panic_inconsistency(
                "tree::build_key_partitions",
                "shard_partition_map not installed — builder must "
                "call install_shard_partitions at startup");
        }

        const uint32_t K = partitions->shard_count();

        rs.partitions.clear();
        rs.partitions.reserve(std::min<uint32_t>(N, K));

        uint32_t run_start = 0;
        uint32_t run_shard = partitions->route(rs.workset[0].key);
        if (run_shard >= K) {
            core::panic_inconsistency(
                "tree::build_key_partitions",
                "route() returned shard=%u >= shard_count=%u",
                static_cast<unsigned>(run_shard),
                static_cast<unsigned>(K));
        }

        auto emit_run = [&](uint32_t begin, uint32_t end, uint32_t shard) {
            if (end == begin) return;
            rs.partitions.push_back(flush_key_partition{
                .read_domain_index = shard,
                .groups = std::span<const flush_key_group>(
                    rs.workset.data() + begin, end - begin),
            });
        };

        for (uint32_t i = 1; i < N; ++i) {
            const uint32_t shard = partitions->route(rs.workset[i].key);
            if (shard >= K) {
                // `shard_count()` is the highest `shard_idx + 1`,
                // so `route()` never returns a larger value. Trip
                // a structural assertion anyway — a mismatch is an
                // invariant violation worth panicking on, not a
                // silent bucket overflow.
                core::panic_inconsistency(
                    "tree::build_key_partitions",
                    "route() returned shard=%u >= shard_count=%u",
                    static_cast<unsigned>(shard),
                    static_cast<unsigned>(K));
            }
            if (shard != run_shard) {
                emit_run(run_start, i, run_shard);
                run_start = i;
                run_shard = shard;
            }
        }
        emit_run(run_start, N, run_shard);

        return flush_stage_status::ok;
    }

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_MEMTABLE_FOLD_HH
