#ifndef APPS_INCONEL_CORE_TREE_REVERSE_TOPOLOGY_HH
#define APPS_INCONEL_CORE_TREE_REVERSE_TOPOLOGY_HH

// ── tree_reverse_topology.hh ── per-manifest leaf→root index (step 026A) ──
//
// Worker cascade needs to walk leaf → parent → grandparent → root.
// The forward descent approach (root-down via find_child at each
// level) costs O(H) page reads of internal pages that are NOT part
// of the cascade output (transit reads). It must also restart from
// root whenever an async internal page read completes.
//
// This carrier makes the climb O(H) constant-time lookups: each leaf
// knows its parent's internal_idx (parallel to leaf_order.spans),
// and each internal node knows its own parent's internal_idx. Root
// is marked by kInvalidInternalIdx. The climb reads only the pages
// that actually land in changed_nodes (no transit reads).
//
// Layout mirrors leaf_order_index:
//   - immutable after construction (owning manifest-by-value)
//   - dense vectors, no per-node small allocations
//   - retires together with the owning tree_manifest snapshot
//
// Capacity (1B KV baseline):
//   16 KiB page  ~3.86M leaves × 4B + ~10.8K internal × 14B ≈ 16 MB
//    4 KiB page  ~15.6M leaves × 4B + ~247K internal × 14B ≈ 65 MB
// Both well below the leaf_order budget (~217 MB / ~875 MB).

#include <cstdint>
#include <vector>

#include "../format/types.hh"

namespace apps::inconel::core {

    using format::paddr;

    // Index into tree_reverse_topology::internal_nodes. UINT32_MAX
    // marks "no parent" (root). With uint32_t we can address up to
    // ~4B internal nodes — far beyond any realistic working set.
    using internal_idx = uint32_t;
    inline constexpr internal_idx kInvalidInternalIdx = UINT32_MAX;

    // One entry per reachable internal node in a manifest snapshot.
    // `range_base` identifies the node (same key as slot_map).
    // `parent_idx` points back into internal_nodes; root = sentinel.
    struct __attribute__((packed)) internal_node_entry {
        paddr         range_base;   // 10 bytes
        internal_idx  parent_idx;   // 4 bytes
    };

    static_assert(sizeof(internal_node_entry) == 14,
                  "internal_node_entry layout frozen at 14 bytes (026A)");

    struct tree_reverse_topology {
        // Parallel to leaf_order_index::spans. leaf_parent_idx[i] is
        // the index into internal_nodes[] of span[i]'s direct parent.
        // kInvalidInternalIdx would mean "leaf is root" (single-node
        // tree, no parent). Normal leaves always have a valid parent.
        std::vector<internal_idx>        leaf_parent_idx;

        // Reverse-topology entries for all internal nodes in the
        // manifest. Ordering is arbitrary; only the parent_idx links
        // matter.
        std::vector<internal_node_entry> internal_nodes;

        bool
        empty() const noexcept {
            return internal_nodes.empty() && leaf_parent_idx.empty();
        }
    };

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_TREE_REVERSE_TOPOLOGY_HH
