#ifndef APPS_INCONEL_CORE_SHARD_PARTITION_HH
#define APPS_INCONEL_CORE_SHARD_PARTITION_HH

// ── shard_partition.hh ── key-range to read_domain routing (step 030) ──
//
// `shard_partition_map` is the single decision carrier that maps a key
// to a `read_domain` shard. Both the read path (`tree::lookup`) and
// the flush path (`memtable_fold`) route through the same map so that
// a given key always lands on the same `tree_read_domain` — this is
// the precondition for the "one tree_node cache shard per leaf range"
// invariant (RSM §4.7 / INC-040 step 030).
//
// Separation of concerns (step 030 §2.1 / §2.2):
//   - `shard_partition_map` is self-contained: it only knows its own
//     sorted fence upper bounds and does not import `leaf_order` or
//     `tree_manifest`. Routing decisions never look at leaves.
//   - `shard_partition_builder.hh` is the ONLY site that reads
//     `leaf_order` to derive an initial partition. A future heat-driven
//     rebuild will replace the builder without touching this routing
//     header — the split is physical, not just a naming convention.
//
// The header is deliberately kept small and dependency-light so it can
// be included from both the front-side memtable fold path and the
// tree-side lookup path without dragging in any scheduler headers or
// `leaf_order.hh`. Call sites that need to *construct* a map include
// `shard_partition_builder.hh` instead.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "./panic.hh"

namespace apps::inconel::core {

    // ── shard_partition ────────────────────────────────────────────
    //
    // One partition = one [prev_upper, fence_upper) key range that
    // routes to `shard_idx`. The upper bound is stored as a
    // {offset, length} slice into the enclosing map's `fence_pool`.
    //
    // Sentinel rule: the last partition in the map carries
    // `fence_upper_len == 0`, which means +∞. Any builder that does
    // not set the sentinel breaks the `route()` invariant and the map
    // panics rather than silently routing off the end of `shards`.
    //
    // `shard_idx` is not required to equal the partition's index in
    // `shards` — the type allows a future heat-driven rebuild to map
    // several discontiguous ranges onto the same read_domain. The
    // bootstrap builder in `shard_partition_builder.hh` assigns
    // `shard_idx == i` so the initial map has no duplicates.

    struct shard_partition {
        uint32_t fence_upper_off;
        uint16_t fence_upper_len;
        uint16_t _pad0;
        uint32_t shard_idx;
    };

    static_assert(sizeof(shard_partition) == 12,
                  "shard_partition layout frozen at 12 bytes (030 §2.1)");
    static_assert(std::is_trivially_copyable_v<shard_partition>,
                  "shard_partition must be a POD so vectors of partitions "
                  "copy/move without per-element construction cost");

    // ── shard_partition_map ────────────────────────────────────────
    //
    // Immutable map from `std::string_view` key → `shard_idx`.
    // Construction is the builder's job; once built, the map is
    // treated as read-only and is shared across read_domains via
    // `shared_ptr<const shard_partition_map>` (see `core/registry.hh`
    // install/current API).
    //
    // `route()` does a single binary search on the fence upper bounds
    // — cost is `log2(shards.size())` comparisons, independent of
    // leaf count. The map does not know what a leaf is.
    //
    // Builder contract (enforced by panic on violation):
    //   - `shards` is non-empty
    //   - shards are sorted by fence_upper ascending (empty = +∞)
    //   - the last shard's fence_upper is empty (+∞ sentinel)
    //
    // Empty map (`shards.size() == 0`) is never produced by the
    // bootstrap builder (see `shard_partition_builder.hh`), and
    // `route()` panics if it is ever invoked on one. Callers are
    // expected to short-circuit the empty-tree case via
    // `manifest->has_root()` before reaching the routing layer.

    struct shard_partition_map {
        std::string                  fence_pool;
        std::vector<shard_partition> shards;

        bool
        empty() const noexcept {
            return shards.empty();
        }

        // Highest `shard_idx` value + 1. The builder assigns
        // `shard_idx == position` on the initial map, so this is
        // effectively `shards.size()`. A future rebuild may reuse a
        // `shard_idx` across multiple partitions; `shard_count()` is
        // the conservative upper bound on the number of distinct
        // read_domains the map can route to.
        uint32_t
        shard_count() const noexcept {
            uint32_t hi = 0;
            for (const auto& p : shards)
                if (p.shard_idx + 1 > hi) hi = p.shard_idx + 1;
            return hi;
        }

        uint32_t
        route(std::string_view key) const {
            if (shards.empty()) {
                panic_inconsistency("shard_partition_map::route",
                    "route() invoked on empty shard_partition_map — "
                    "caller must short-circuit empty-tree via "
                    "manifest->has_root() before routing");
            }

            // Binary search for the first partition whose fence_upper
            // is strictly greater than `key`. Empty upper = +∞, so it
            // satisfies the predicate for every key.
            std::size_t lo = 0, hi = shards.size();
            while (lo < hi) {
                std::size_t mid = lo + (hi - lo) / 2;
                auto upper = fence_upper_view(shards[mid]);
                if (upper.empty() || key < upper) hi = mid;
                else                              lo = mid + 1;
            }

            if (lo >= shards.size()) {
                // Only possible when the last partition's fence_upper
                // is not the +∞ sentinel — a builder contract
                // violation. Panic rather than routing off the end.
                panic_inconsistency("shard_partition_map::route",
                    "shard_partition_map has no +∞ sentinel "
                    "(shards=%zu)",
                    shards.size());
            }
            return shards[lo].shard_idx;
        }

      private:
        std::string_view
        fence_upper_view(const shard_partition& s) const {
            const std::size_t pool_size = fence_pool.size();
            const std::size_t off = static_cast<std::size_t>(s.fence_upper_off);
            const std::size_t len = static_cast<std::size_t>(s.fence_upper_len);
            if (off > pool_size || len > pool_size - off) {
                panic_inconsistency(
                    "shard_partition_map::fence_upper_view",
                    "out-of-bounds fence slice: off=%u len=%u pool_size=%zu",
                    static_cast<unsigned>(s.fence_upper_off),
                    static_cast<unsigned>(s.fence_upper_len),
                    pool_size);
            }
            return std::string_view(fence_pool.data() + off, len);
        }
    };

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_SHARD_PARTITION_HH
