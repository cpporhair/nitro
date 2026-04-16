#ifndef APPS_INCONEL_CORE_SHARD_PARTITION_BUILDER_HH
#define APPS_INCONEL_CORE_SHARD_PARTITION_BUILDER_HH

// ── shard_partition_builder.hh ── leaf_order → shard_partition_map ──
//
// The ONLY site that reads `leaf_order_index` to derive an initial
// `shard_partition_map` (step 030 §2.1 / §2.2 decision P).
//
// Separated from `shard_partition.hh` on purpose: the routing header
// must stay dependency-light so the read path (`tree::lookup`) and the
// flush fold path (`memtable_fold`) can include it without dragging in
// `leaf_order.hh`. Only call sites that *construct* a map (builder,
// future `tree_sched` rebuild, tests) include this header.
//
// A future heat-driven rebuild can swap in a new builder in this file
// (or an adjacent one) without touching the routing surface.

#include <cstddef>
#include <cstdint>

#include "./leaf_order.hh"
#include "./panic.hh"
#include "./shard_partition.hh"

namespace apps::inconel::core {

    // ── build_initial_shard_partition_map ──────────────────────────
    //
    // Bootstrap / recovery / flush-end builder. Consumes the current
    // manifest's `leaf_order` and produces a range-partitioned map
    // with `read_domain_count` logical shards.
    //
    // Policy:
    //   L = leaf_order.size()
    //   P = min(read_domain_count, L)
    //   shard i covers leaves [i * L / P, (i+1) * L / P)
    //   fence_upper of shard i = fence_upper of that shard's last leaf
    //   shard P-1 carries the +∞ sentinel (fence_upper_len == 0)
    //
    // Edge cases:
    //   - `read_domain_count == 0` → panic (runtime configuration bug)
    //   - `leaf_order.empty()` → single-shard placeholder map
    //        `shards = [{fence_upper_len=0, shard_idx=0}]` (030 §6.7
    //        decision P). Bootstrap installs this so that the routing
    //        invariants hold during the "have runtime but no tree
    //        yet" window. Empty-tree lookup still short-circuits
    //        via `manifest->has_root()` — the placeholder map is
    //        present for correctness, not exercised on the read path
    //        until the first flush populates `leaf_order`.
    //   - `L < read_domain_count` → P = L; the first L shards each
    //        cover exactly one leaf; shards in `[L, read_domain_count)`
    //        are not referenced by any partition (idle). The registry
    //        still holds `read_domain_count` read_domain instances;
    //        load imbalance is acceptable until a rebuild merges the
    //        empty shards.
    //
    // Leaf alignment is a construction decision — `shard_partition_map::route()`
    // never observes leaves. A future heat-driven rebuild can split at
    // any key without touching the routing code.

    inline shard_partition_map
    build_initial_shard_partition_map(const leaf_order_index& leaf_order,
                                      uint32_t                read_domain_count) {
        if (read_domain_count == 0) {
            panic_inconsistency("build_initial_shard_partition_map",
                "read_domain_count must be > 0");
        }

        shard_partition_map map;

        if (leaf_order.empty()) {
            // Placeholder map: single shard covering (-∞, +∞) → 0.
            // No bytes in fence_pool; the +∞ sentinel is encoded by
            // `fence_upper_len == 0`.
            map.shards.push_back(shard_partition{
                .fence_upper_off = 0,
                .fence_upper_len = 0,
                ._pad0           = 0,
                .shard_idx       = 0,
            });
            return map;
        }

        const std::size_t L = leaf_order.size();
        const std::size_t P =
            (static_cast<std::size_t>(read_domain_count) < L)
                ? static_cast<std::size_t>(read_domain_count)
                : L;

        map.shards.reserve(P);
        // Reserve an upper bound for the fence pool. Every shard
        // except the last one stores its upper bound in the pool;
        // the final shard's upper is +∞ (length 0). This is a hint,
        // not a contract — the real size depends on each copied
        // fence length, appended below.
        std::size_t reserve_bytes = 0;
        for (std::size_t i = 0; i + 1 < P; ++i) {
            const std::size_t last_leaf = (i + 1) * L / P - 1;
            reserve_bytes += leaf_order.fence_upper(
                leaf_order.spans[last_leaf]).size();
        }
        map.fence_pool.reserve(reserve_bytes);

        for (std::size_t i = 0; i < P; ++i) {
            shard_partition p{};
            p.shard_idx = static_cast<uint32_t>(i);

            if (i + 1 == P) {
                // Last shard carries the +∞ sentinel.
                p.fence_upper_off = 0;
                p.fence_upper_len = 0;
            } else {
                const std::size_t last_leaf = (i + 1) * L / P - 1;
                auto upper = leaf_order.fence_upper(
                    leaf_order.spans[last_leaf]);
                // A non-last leaf MUST have a finite fence_upper. An
                // empty upper on a non-last leaf is a leaf_order
                // construction bug and should panic rather than
                // silently collapse the partition boundary.
                if (upper.empty()) {
                    panic_inconsistency(
                        "build_initial_shard_partition_map",
                        "leaf_order.spans[%zu].fence_upper is empty "
                        "(+∞) but it is not the last leaf "
                        "(L=%zu P=%zu shard_idx=%zu)",
                        last_leaf, L, P, i);
                }
                p.fence_upper_off =
                    static_cast<uint32_t>(map.fence_pool.size());
                p.fence_upper_len =
                    static_cast<uint16_t>(upper.size());
                map.fence_pool.append(upper);
            }
            map.shards.push_back(p);
        }

        return map;
    }

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_SHARD_PARTITION_BUILDER_HH
