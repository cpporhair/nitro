#ifndef APPS_INCONEL_TREE_FLUSH_TYPES_HH
#define APPS_INCONEL_TREE_FLUSH_TYPES_HH

// ── Flush shared shell types ──────────────────────────────────────
//
// Cross-scheduler type shells for the tree-local flush pipeline.
//
// Phase 7 (step 027) replaced the worker's per-paddr overlay output
// (`flush_changed_node` / `flush_worker_result`) with an in-memory
// hybrid tree (`mem_tree_node` + `child_ref` + `worker_tree_proposal`).
// The Phase 5 leaf-mapping carriers (`flush_mapping_req` /
// `flush_leaf_group_result` / `flush_leaf_group`) are gone too — the
// worker now does mapping inline against `base_manifest->leaf_order`.
//
// Carrier ownership rules (Phase 3 §6, still authoritative):
//
//   - Per-key fold output is owned by `flush_round_state.workset`.
//     `flush_key_partition.groups` is a borrowed span over that
//     vector and must not outlive the round_state.
//   - `flush_worker_req.key_groups` borrows from the same workset
//     (a sub-span belonging to one partition).
//   - `worker_tree_proposal` is the worker's output: a self-contained
//     mem_tree (paddr refs into the base_manifest's old subtree) +
//     touched old internal pages (for Phase 9 owner-side merge of
//     shared ancestors) + retired old tree value_refs (Gap 1B).
//
// Fail-fast convention (022 D11): every flush result struct carries a
// `flush_stage_status`. Unimplemented sender surfaces return the
// result with `st = unsupported_unimplemented` via the value path; we
// do not throw for unimplemented phases.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>

#include "../core/checkpoint_guard.hh"
#include "../core/memtable.hh"
#include "../core/retired_objects.hh"
#include "../core/tree_manifest.hh"
#include "../format/types.hh"
#include "../format/tree_page.hh"

namespace apps::inconel::tree {

    using format::paddr;

    // ── round identity ───────────────────────────────────────────

    struct flush_round_id {
        uint64_t v = 0;
    };

    enum class flush_stage_status : uint8_t {
        ok,
        unsupported_unimplemented,
        unsupported_shape_change,
    };

    // ── fold output ──────────────────────────────────────────────
    //
    // Per-logical-key fold result produced by the Phase 4 fold stage.
    //
    // Nothing here is owning — all lifetime is borrowed from the
    // enclosing `flush_round_state`:
    //
    //   - `key` is a `std::string_view` into the winner gen's
    //     kv_arena (RSM §3.2). `round_state.pinned_gens` keeps
    //     every gen's arena alive for the full flush round.
    //   - `winner_value` is a `value_handle` (POD). Same pin chain.
    //   - `winner_pinned_gen_index` is an index into
    //     `round_state.pinned_gens[]`, not an owning shared_ptr.
    //
    // Memtable losers for the same key are pushed directly into
    // each gen's `loser_durable_refs` by the fold step (Phase 4).

    struct flush_key_group {
        std::string_view            key;
        uint64_t                    winner_data_ver;
        core::memtable_entry::kind  winner_kind;
        core::value_handle          winner_value;  // valid iff winner_kind == kind::value
        uint32_t                    winner_pinned_gen_index;
    };

    // ── partition plan ──────────────────────────────────────────
    //
    // Phase 7 (step 027 §4.3) leaf-aligns partitions: every key in
    // a single partition shares a leaf in `base_manifest.leaf_order`
    // with no other partition. The worker can therefore process its
    // partition without coordinating with siblings about leaf
    // boundaries.

    struct flush_key_partition {
        uint32_t                          read_domain_index;
        std::span<const flush_key_group>  groups;  // borrows from flush_round_state.workset
    };

    // ── fold result (owner _flush_fold output) ──────────────────

    struct flush_fold_result {
        flush_round_id                     round_id;
        flush_stage_status                 st;
        std::vector<flush_key_partition>   partitions;
        const core::tree_manifest*         base_manifest;
    };

    // ── worker request (Phase 7) ────────────────────────────────
    //
    // `key_groups` borrows from `flush_round_state.workset`. The
    // round_state outlives every fanout arm by construction (the
    // owner unparks the round only after `to_vector` collects all
    // worker proposals — see flush_module_guide.md §2.4).

    struct flush_worker_req {
        flush_round_id                    round_id;
        uint32_t                          read_domain_index;
        const core::tree_manifest*        base_manifest;
        uint64_t                          recovery_safe_lsn;
        std::span<const flush_key_group>  key_groups;
    };

    // ── mem_tree_node + child_ref (Phase 7 / step 027 §2.1) ─────
    //
    // Worker output is a self-contained "in-memory hybrid tree":
    // every node is either a worker-built `mem_tree_node` (in-memory
    // page bytes — no paddr, no CRC) or a `paddr` reference into the
    // base_manifest's unchanged subtree. The hybrid is rooted at
    // `worker_tree_proposal.root`.
    //
    // Why hybrid (not full new tree, not pure paddr deltas):
    //
    //   - A worker only modifies a slice of the tree. The unmodified
    //     siblings stay where they are; we do not copy them.
    //   - The owner-side merge (Phase 9) needs a tree shape, not a
    //     paddr-keyed dictionary, because shape-changing operations
    //     (split / consolidation / root growth) introduce nodes that
    //     have no `old_paddr` to key on.
    //
    // Forward declaration: `child_ref` and `mem_tree_node` recurse
    // through `unique_ptr<mem_tree_node>`, which is a complete type
    // even when `mem_tree_node` is incomplete.

    struct mem_tree_node;

    struct child_ref {
        std::variant<paddr, std::unique_ptr<mem_tree_node>> target;
    };

    struct mem_tree_node {
        // Page type (leaf / internal). Mirrors the on-disk
        // tree_slot_header.type.
        format::node_type type;

        // Worker-formatted page bytes (ODF §4). The CRC field is
        // intentionally NOT filled — Phase 9 owner side recomputes
        // it after assigning paddrs and patching child_base entries.
        std::vector<char> content;

        // Old paddr(s) this node "replaces" in the base_manifest's
        // tree. Convention (027 §2.1):
        //
        //   - 1 element: this is a rewrite / consolidation of one
        //     old page (also covers the "shadow slot exhausted"
        //     case — owner side decides whether to allocate a new
        //     range; worker does not).
        //   - 2+ elements: a leaf merge (027 §3.3) replaces several
        //     old leaves with one new node. All old paddrs are
        //     listed so owner side knows which old slots / ranges
        //     to retire.
        //   - 0 elements (empty): a brand new page (split sibling
        //     or a new layer above the old root). Owner side
        //     allocates a fresh slot for this node.
        absl::InlinedVector<paddr, 2> replaces_old_paddrs;

        // Internal nodes only: child references (length N).
        // Each child is either a paddr into the unchanged base_manifest
        // tree or a unique_ptr to another worker-built mem_tree_node.
        std::vector<child_ref> children;

        // Internal nodes only: separator keys (length N-1).
        // Owned strings — the original separators came from the
        // worker-read old internal page bytes which are about to be
        // dropped, so we copy them once.
        std::vector<std::string> separators;
    };

    // ── worker_tree_proposal (Phase 7 / step 027 §2.2) ──────────

    struct worker_tree_proposal {
        flush_round_id     round_id;
        uint32_t           read_domain_index;
        flush_stage_status st;

        // The worker's local view of the tree's new root after this
        // worker's keys are applied.
        //
        // - root != nullptr when the worker had keys in scope and
        //   walked the cascade up to root.
        // - root == nullptr when the worker received an empty
        //   key_groups span (the partition system should not produce
        //   that, but the field is nullable for cleanliness).
        //
        // If the worker did not change the level the base_manifest
        // root sits at, `root` is still a worker-built mem_tree_node
        // — but most of its `children` are paddr refs. Owner side
        // (Phase 9) merges the hybrid trees from every worker.
        std::unique_ptr<mem_tree_node> root;

        // Old internal page bytes that the worker actually read while
        // walking the cascade. Phase 9 reuses these when merging
        // shared ancestors across workers (so owner side does not
        // re-read the same internal). Keyed by old paddr.
        absl::flat_hash_map<paddr, std::vector<char>> touched_old_pages;

        // Old leaf value_refs that this worker's `merge_and_build_leaf`
        // identified as "tree-visible winner that the memtable
        // winner now supersedes" (Gap 1B). Phase 9 aggregates these
        // across workers into `tree_flush_result.retired.old_tree_values`.
        absl::InlinedVector<core::retired_value_ref, 64> retired_old_values;
    };

    // ── per-round worker decision (Phase 7) ─────────────────────
    //
    // `submit_flush_work_round` is a per-round handle on the worker
    // scheduler that processes as much as it can with available
    // pages and returns one of:
    //
    //   - `flush_round_done`: the proposal is fully built; the
    //     wrapper sender extracts it from the worker_state.
    //   - `flush_round_need_read`: the worker needs old leaf or
    //     internal pages to make progress; the wrapper sender
    //     dispatches NVMe reads and re-enters the worker handle.
    //
    // This mirrors the Phase 6 `candidate_decision` shape so the
    // multi-round driver sender remains structurally identical.

    struct flush_round_done {};

    struct flush_round_need_read {
        std::vector<format::read_desc> read_descs;
    };

    using flush_round_decision = std::variant<flush_round_done, flush_round_need_read>;

    // ── owner merge request (Phase 7 → Phase 9) ─────────────────
    //
    // The single fanout (`worker.submit_flush_work`) emits one
    // `worker_tree_proposal` per partition. The `to_vector` collector
    // hands the whole vector to `tree_sched.submit_flush_merge`.
    // Phase 9 will implement the merge; Phase 7 only validates the
    // input and returns `unsupported_unimplemented`.

    struct flush_merge_request {
        flush_round_id                    round_id;
        std::vector<worker_tree_proposal> worker_proposals;
    };

    // ── owner-level flush request / result ──────────────────────
    //
    // `tree_flush_request` is the envelope coord_sched eventually
    // hands to `tree_sched`. Field layout frozen in Phase 3 (023
    // Δ-1 / Δ-2). The Phase 3 stub returned `unsupported_unimplemented`;
    // Phase 4-7 progressively replaced that with real content
    // without changing the field layout.

    struct tree_flush_request {
        std::shared_ptr<const core::checkpoint_guard>                base_guard;
        absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>  sealed_gens;
        uint64_t                                                     recovery_safe_lsn;
    };

    // Gap 1A decision (flush_development_plan §2.1.1): memtable-only
    // losers live solely on `core::memtable_gen.loser_durable_refs`;
    // they are never carried on `tree_flush_result`. The prior
    // `memtable_losers` field is deleted to prevent future consumers
    // from re-wiring the wrong lifetime.
    struct tree_flush_result {
        flush_stage_status                          st = flush_stage_status::ok;
        std::shared_ptr<const core::tree_manifest>  new_manifest;
        core::retired_objects                       retired;
        absl::flat_hash_map<
            uint32_t,
            absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>>
                                                    flushed_gens_by_front;
        uint64_t                                    flushed_max_lsn = 0;
    };

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_FLUSH_TYPES_HH
