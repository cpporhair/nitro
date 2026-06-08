#ifndef APPS_INCONEL_TREE_FLUSH_TYPES_HH
#define APPS_INCONEL_TREE_FLUSH_TYPES_HH

// ── Flush shared shell types ──────────────────────────────────────
//
// Cross-scheduler type shells for the tree-local flush pipeline.
//
// The worker now maps and rewrites only touched leaves. Owner side
// builds the non-leaf working tree (`mem_tree_node` + `child_ref`),
// assigns slots / ranges, and writes pages.
//
// Carrier ownership rules (Phase 3 §6, still authoritative):
//
//   - Per-key fold output is owned by `flush_round_state.workset`.
//     `flush_key_partition.groups` is a borrowed span over that
//     vector and must not outlive the round_state.
//   - `flush_worker_req.key_groups` borrows from the same workset
//     (a sub-span belonging to one partition).
//   - `worker_leaf_chain` is the worker's output: a zero-extra-copy
//     leaf carrier plus retired old tree value_refs.
//
// Fail-fast convention (022 D11): every flush result struct carries a
// `flush_stage_status`. Unimplemented sender surfaces return the
// result with `st = unsupported_unimplemented` via the value path; we
// do not throw for unimplemented phases.

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
#include "../memory/frame.hh"

namespace apps::inconel::tree {

    using format::paddr;

    // ── round identity ───────────────────────────────────────────

    struct flush_round_id {
        uint64_t v = 0;
    };

    enum class superblock_slot : uint8_t { A, B };

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
        uint64_t                           recovery_safe_lsn;
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

    // ── owner-side working tree node ─────────────────────────────
    //
    // The canonical write side is owner-built:
    //
    //   - leaf pages arrive from workers as `leaf_page_image`
    //   - owner reverse materializes internal/root pages as
    //     `mem_tree_node`
    //   - unchanged subtrees remain `paddr` references into the base
    //     manifest until a changed ancestor rewrites them
    //
    // `child_ref` and `mem_tree_node` recurse through
    // `unique_ptr<mem_tree_node>`, which is complete even when
    // `mem_tree_node` itself is forward-declared.

    struct mem_tree_node;

    struct child_ref {
        std::variant<paddr, std::unique_ptr<mem_tree_node>> target;
    };

    struct mem_tree_node {
        // Page type (leaf / internal). Mirrors the on-disk
        // tree_slot_header.type.
        format::node_type type;

        // Page bytes ready for writeback. Internal content may be
        // reformatted after child placement to rewrite child range
        // bases; CRC is filled by the normal page builder/finalize
        // path before writeback.
        std::vector<char> content;

        // Old range(s) this node logically replaces.
        //
        //   - 1 element: rewrite / same-range next-slot / consolidation
        //   - 0 elements: brand-new page (split sibling / new layer)
        //
        // The owner uses this only for placement/retire planning.
        absl::InlinedVector<paddr, 2> replaces_old_paddrs;

        // Internal nodes only: child references (length N).
        // Each child is either an unchanged base-manifest range_base
        // or another owner-built working-tree node.
        std::vector<child_ref> children;

        // Internal nodes only: separator keys (length N-1).
        // Owned strings — the original separators came from the
        // worker-read old internal page bytes which are about to be
        // dropped, so we copy them once.
        std::vector<std::string> separators;

        // Owner-side placement result.
        paddr    new_range_base{};
        uint32_t new_slot_index = 0;
        paddr    new_paddr{};
    };

    // ── leaf-only worker carrier (INC-046) ──────────────────────
    //
    // The canonical worker/owner contract is now leaf-only:
    //
    //   - worker reads / merges / formats only touched leaves
    //   - owner owns every non-leaf read / cache / reverse / write
    //
    // `leaf_page_image.page` is the final on-disk leaf page body.
    // Worker formats the page once; owner copies it into a segmented
    // writeback frame before issuing NVMe I/O.

    struct leaf_page_image {
        std::vector<char> page;
        uint16_t          first_key_off = 0;
        uint16_t          first_key_len = 0;

        std::string_view
        first_key() const noexcept {
            if (first_key_len == 0 || first_key_off >= page.size()) {
                return {};
            }
            return std::string_view(
                page.data() + first_key_off,
                first_key_len);
        }
    };

    enum class leaf_chain_shape : uint8_t {
        rewrite,
        split,
    };

    struct leaf_chain_item {
        uint32_t            old_leaf_idx = UINT32_MAX;
        paddr               old_range_base{};
        core::internal_idx  parent_idx = core::kInvalidInternalIdx;
        leaf_chain_shape    shape = leaf_chain_shape::rewrite;
        absl::InlinedVector<leaf_page_image, 2> new_pages;
        absl::InlinedVector<core::retired_value_ref, 64>
            retired_old_values;
    };

    struct worker_leaf_chain {
        flush_round_id     round_id;
        uint32_t           read_domain_index;
        flush_stage_status st;
        std::vector<leaf_chain_item> items;
    };

    // ── per-round worker decision (INC-046 leaf worker) ─────────
    //
    // `submit_flush_work_round` is a per-round handle on the worker
    // scheduler that processes as much as it can with available
    // pages and returns one of:
    //
    //   - `flush_round_done`: the leaf chain is fully built; the
    //     wrapper sender extracts it from the worker_state.
    //   - `flush_round_need_read`: the worker needs old leaf pages to
    //     make progress; the wrapper sender dispatches NVMe reads and
    //     re-enters the worker handle.
    //
    // This mirrors the Phase 6 `candidate_decision` shape so the
    // multi-round driver sender remains structurally identical.

    struct flush_round_done {};

    struct flush_round_need_read {
        std::vector<memory::frame_read_desc> reads;
    };

    using flush_round_decision = std::variant<flush_round_done, flush_round_need_read>;

    // ── owner merge request (leaf fan-in → owner reverse) ───────

    struct flush_merge_request {
        flush_round_id                    round_id;
        std::vector<worker_leaf_chain>    worker_leaf_chains;
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

    struct update_superblock_request {
        flush_round_id  round_id;
        paddr           new_root_base_paddr{};
    };

    struct update_superblock_result {
        flush_round_id   round_id;
        bool             ok = false;
        superblock_slot  committed_slot = superblock_slot::A;
    };

    struct finalize_flush_request {
        flush_round_id                      round_id;
        bool                                ok = false;
        std::optional<superblock_slot>      committed_slot;
    };

    struct flush_merge_done {
        tree_flush_result  result;
    };

    struct flush_merge_root_stable {
        finalize_flush_request  finalize_req;
    };

    struct flush_merge_root_change {
        update_superblock_request  update_req;
    };

    // ── merge loop state (lives in the outer pipeline context) ──
    //
    // Replaces the earlier `merge_step_driver` variant: instead of
    // carrying the round_id / worker leaf chains in a variant arm that
    // the coroutine yields on its first iteration, we keep them in a
    // pipeline-context struct and hand a `merge_loop_state*` to
    // `submit_merge_step` every iteration. The scheduler reads the
    // pointer and, on the first call (when `active_merge` is still
    // empty), moves `worker_leaf_chains` out to seed the coroutine;
    // subsequent calls ignore the payload fields.
    //
    // `cpu_done` is the loop termination flag:
    //   - flipped to true by the outer iter handler when the
    //     scheduler returns `merge_step_done`
    //   - read by the `drive_merge` coroutine each iteration to
    //     decide whether to keep yielding
    // `concurrent(kMergeIterConcurrency)` means multiple iter
    // handlers can write the flag concurrently from different cores
    // after the coroutine finishes, so `atomic<bool>` is required.
    struct merge_loop_state {
        flush_round_id                 round_id;
        std::vector<worker_leaf_chain> worker_leaf_chains;
        std::atomic<bool>              cpu_done{false};

        merge_loop_state() = default;
        merge_loop_state(flush_round_id r, std::vector<worker_leaf_chain>&& p)
            : round_id(r), worker_leaf_chains(std::move(p)) {}

        // Custom move ctor because `std::atomic<bool>` is neither
        // copyable nor movable. A snapshot-then-store is safe here —
        // `merge_loop_state` only moves at construction time (into
        // the PUMP context), never in a racy window.
        merge_loop_state(merge_loop_state&& o) noexcept
            : round_id(o.round_id),
              worker_leaf_chains(std::move(o.worker_leaf_chains)),
              cpu_done(o.cpu_done.load(std::memory_order_relaxed)) {}
        merge_loop_state& operator=(merge_loop_state&&)      = delete;
        merge_loop_state(const merge_loop_state&)            = delete;
        merge_loop_state& operator=(const merge_loop_state&) = delete;
    };

    // ── merge step decision (scheduler → outer pipeline) ─────────
    //
    // `merge_io_desc` is the unified NVMe op carrier the coroutine
    // stages in `merge_round_state::pending_ios`. Read and write
    // ops live in the same `std::vector<merge_io_desc>` so the
    // outer pipeline can dispatch them with a single
    // `as_stream >> concurrent(N) >> visit() >> flat_map(...)`
    // chain — no artificial serialization between the two.
    // Pointer lifetime:
    //   - frame_read_desc.frame  → owner merge read frame for an old
    //                              internal page
    //   - frame_write_desc.frame → owner merge writeback frame copied from
    //                              owner-built tree page bytes
    // Both descriptors point at tree_sched-owned frames that live until the
    // merge round is finalized.
    using merge_io_desc = std::variant<
        memory::frame_read_desc,
        memory::frame_write_desc>;

    // `merge_step_need_io` carries one yield's worth of IOs moved
    // out of `merge_round_state::pending_ios`. `has_reads` is
    // precomputed by the seam handler so the outer pipeline doesn't
    // need to re-scan the vector; it's true iff the coroutine
    // cannot safely be resumed until every io in this batch
    // completes (i.e. any frame_read_desc present whose frame the
    // next resume will inspect). The outer pipeline ACKs
    // back via `submit_merge_reads_done` when `has_reads` is true.
    //
    // `merge_step_done` is returned once the coroutine reaches
    // `co_return` — all writes are already emitted and in flight
    // (or completed); the outer pipeline then issues a device
    // FLUSH and calls `submit_finalize_merge`.
    struct merge_step_need_io {
        std::vector<merge_io_desc> ios;
        bool                       has_reads = false;
    };

    struct merge_step_done {};

    using merge_step_decision = std::variant<
        merge_step_need_io,
        merge_step_done>;

    // ── finalize after flush ─────────────────────────────────────
    //
    // Called once per flush round after the outer pipeline's
    // `merge_step` loop has drained (all emitted writes complete)
    // AND a device FLUSH has been issued. `flush_ok` carries the
    // FLUSH outcome. The scheduler inspects `active_merge` to
    // produce the commit variant:
    //   - done        : no writes happened (or anything failed)
    //   - root_stable : CAT-only finalize
    //   - root_change : needs superblock rewrite
    // `active_merge` is released here; `active_rounds[round_id]`
    // is handed to `_finalize_flush_round` via the commit variant.
    struct merge_finalize_request {
        flush_round_id  round_id;
        bool            flush_ok = false;
    };

    using merge_finalize_result = std::variant<
        flush_merge_done,
        flush_merge_root_stable,
        flush_merge_root_change>;

    // Step 1 of the root-change superblock update: `tree_sched`
    // atomically picks the active/inactive slot pair and latches
    // the inflight serialization flag. The outer pipeline is the
    // one that performs the NVMe read, mutates the superblock
    // bytes, and issues the inactive-slot FUA write.
    struct begin_update_superblock_result {
        flush_round_id   round_id;
        paddr            new_root_base_paddr{};
        uint64_t         active_lba       = 0;
        uint64_t         inactive_lba     = 0;
        superblock_slot  inactive_slot    = superblock_slot::A;
        uint32_t         lba_size         = 0;
    };

    // Step 2 of the root-change superblock update: notifies
    // `tree_sched` that the inactive-slot write has completed so it
    // can clear the inflight flag and surface an `update_superblock_result`.
    struct finish_update_superblock_request {
        flush_round_id   round_id;
        superblock_slot  inactive_slot = superblock_slot::A;
        bool             write_ok      = false;
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

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_FLUSH_TYPES_HH
