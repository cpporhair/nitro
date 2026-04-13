#ifndef APPS_INCONEL_TREE_FLUSH_TYPES_HH
#define APPS_INCONEL_TREE_FLUSH_TYPES_HH

// ── Flush shared shell types ──────────────────────────────────────
//
// Cross-scheduler type shells for the tree-local flush pipeline.
//
// Phase 2 (step 022 G4) froze the top-level identity and the stable
// payload every later phase would certainly need (flush_round_id,
// flush_stage_status, the lookup / worker request identity fields,
// the worker candidate result).
//
// Phase 3 (step 023 G2/G3/G6) extends the header with:
//   - `tree_flush_request` / `tree_flush_result` — the full owner
//     payload handed to `tree_sched::handle_tree_flush`. Shapes mirror
//     RSM §4.2 one-for-one except for two deliberate deviations
//     (Δ-1 / Δ-2) logged in 023 §与 RSM §4.1/§4.2 的偏差 and tracked
//     for a follow-up doc-sync step.
//   - `flush_key_group` — per-logical-key fold output. Phase 3 only
//     freezes the shape so the Phase 4 fold step does not have to
//     touch this header a second time.
//   - `flush_mapping_req` / `flush_worker_req` re-gain the borrowed
//     payload (`std::span<const flush_key_group>` /
//     `std::span<const flush_leaf_group>`). 022 temporarily stripped
//     those fields because no owning side existed; Phase 3 establishes
//     the owning side in `tree/flush_round_state.hh` and the borrow
//     is now safe. The span's backing storage is the round-owned
//     vectors inside `flush_round_state` — see 023 §6.
//
// Fail-fast convention (022 D11, §10; reused in Phase 3): every flush
// result struct carries a `flush_stage_status`. Unimplemented sender
// surfaces return the result with `st = unsupported_unimplemented` via
// the normal value path; we do not throw for unimplemented phases.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>

#include "../core/checkpoint_guard.hh"
#include "../core/memtable.hh"
#include "../core/retired_objects.hh"
#include "../core/tree_manifest.hh"
#include "../format/types.hh"

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
    // `memtable_winner` / the raw `winner_*` triple below are the
    // references the rest of the flush pipeline carries through to
    // leaf candidate build / writer.
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
    //     This avoids O(unique_keys) refcount bumps and keeps the
    //     workset a pure borrowed-view carrier.
    //
    // Memtable losers for the same key are pushed directly into
    // each gen's `loser_durable_refs` by the fold step (Phase 4);
    // they are not carried on this struct.

    struct flush_key_group {
        std::string_view            key;
        uint64_t                    winner_data_ver;
        core::memtable_entry::kind  winner_kind;
        core::value_handle          winner_value;  // valid iff winner_kind == kind::value
        uint32_t                    winner_pinned_gen_index;  // index into round_state.pinned_gens
    };

    // ── partition plan for Phase 5 lookup dispatch ──────────────
    //
    // Phase 4 `build_key_partitions()` produces one partition per
    // lookup shard. Each partition's `groups` span borrows
    // contiguous elements from `flush_round_state.workset`. The
    // span is valid as long as the round_state exists AND the
    // workset vector is not reallocated after partitioning.
    //
    // Phase 5 `dispatch_key_partitions_to_lookup()` fans out these
    // partitions, converting each into a `flush_mapping_req` with
    // `groups` pointing at the same span.

    struct flush_key_partition {
        uint32_t                          read_domain_index;
        std::span<const flush_key_group>  groups;  // borrows from flush_round_state.workset
    };

    // ── leaf mapping stage (Phase 5 consumer) ────────────────────

    // Phase 5 (D17): renamed from `flush_mapping_req` — the target
    // scheduler changed from `tree_lookup_sched` to
    // `tree_worker_sched`, so the name follows.
    //
    // `groups` points at the round-owned `flush_round_state.workset`
    // vector and is valid for the lifetime of the enclosing round
    // (see 023 §6). `tree_sched` MUST NOT let a `flush_mapping_req`
    // outlive the round_state it borrows from.
    struct flush_mapping_req {
        flush_round_id                    round_id;
        uint32_t                          read_domain_index;
        const core::tree_manifest*        base_manifest;
        std::span<const flush_key_group>  groups;   // borrows from flush_round_state
    };

    struct flush_leaf_group {
        paddr leaf_range_base;
        paddr old_slot_paddr;
        std::span<const flush_key_group> keys;  // borrows from flush_round_state.workset
    };

    struct flush_leaf_group_result {
        flush_round_id                            round_id;
        uint32_t                                  read_domain_index;
        flush_stage_status                        st;
        absl::InlinedVector<flush_leaf_group, 8>  leaf_groups;
    };

    // ── Phase 5 fold/merge stage carriers (D10, D11) ────────────

    struct flush_fold_result {
        flush_round_id                     round_id;
        flush_stage_status                 st;
        std::vector<flush_key_partition>   partitions;
        const core::tree_manifest*         base_manifest;
    };

    struct flush_merge_request {
        flush_round_id                            round_id;
        std::vector<flush_leaf_group_result>      mapping_results;
    };

    // ── candidate materialization stage (Phase 6 consumer) ───────
    //
    // Phase 3 re-attaches the borrowed `leaf_groups` payload that
    // 022 review M-3 had to strip for the same ownership reason as
    // above. The span points at the round-owned
    // `flush_round_state.leaf_groups` vector; Phase 3's Phase-2-era
    // worker handle still does NOT deref it (the handle keeps
    // returning `unsupported_unimplemented`), but the field must be
    // present so Phase 6 does not have to refloat this carrier.

    struct flush_worker_req {
        flush_round_id                    round_id;
        uint32_t                          read_domain_index;
        const core::tree_manifest*        base_manifest;
        uint64_t                          recovery_safe_lsn;
        std::span<const flush_leaf_group> leaf_groups;  // borrows from flush_round_state
    };

    struct flush_leaf_candidate {
        paddr              leaf_range_base;
        paddr              old_slot_paddr;
        flush_stage_status st;
    };

    struct flush_candidate_batch {
        flush_round_id                               round_id;
        uint32_t                                     read_domain_index;
        flush_stage_status                           st;
        absl::InlinedVector<flush_leaf_candidate, 8> leaves;
    };

    // ── owner-level flush request / result ──────────────────────
    //
    // `tree_flush_request` is the envelope the coord_sched eventually
    // hands to `tree_sched` at seal time. Phase 3 only freezes field
    // layout; the Phase 3 `handle_tree_flush` inspects `base_guard`
    // and `sealed_gens` for fail-fast validation, then returns an
    // `unsupported_unimplemented` result via the value path (D22).
    //
    // Deviation Δ-1 (023 §与 RSM §4.1/§4.2 的偏差): RSM §4.2 declares
    // `sealed_gens` as a `std::span<std::shared_ptr<memtable_gen>>`.
    // Phase 3 promotes it to an owning
    // `absl::InlinedVector<..., 8>`. A borrowed span crossing the
    // `tree_sched` ingress queue would repeat the M-3 bug from 022
    // review §1: the op copies args into a heap req, the caller's
    // stack frame returns, and `tree_sched::advance()` consumes the
    // req in a different scheduler tick — by which time the
    // caller-side backing storage is gone. Owning InlinedVector at
    // inline capacity 8 still zero-allocates for the common Phase 4
    // round size. The `shared_ptr` copies bump refcount on the
    // caller's core (correct ownership transfer); `tree_sched` only
    // ever sees fully pinned gens. A doc-sync step after Phase 3
    // will propagate this back to RSM / FF / cross_doc.

    struct tree_flush_request {
        std::shared_ptr<const core::checkpoint_guard>                base_guard;
        absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>  sealed_gens;
        uint64_t                                                     recovery_safe_lsn;
    };

    // Deviation Δ-2 (023 §与 RSM §4.1/§4.2 的偏差): RSM §4.2 shows
    // `tree_flush_result` with a `bool ok` plus a separate
    // `flush_error error` field. Phase 3 unifies the status into a
    // single `flush_stage_status st`, matching the existing carrier
    // convention already used by `flush_leaf_group_result` and
    // `flush_candidate_batch` (022 D11). Having two status fields on
    // one result struct invites subtle bugs where callers inspect
    // `ok` but forget to consult `error`. A doc-sync step after
    // Phase 3 will propagate this back to RSM / FF / cross_doc.
    //
    // Phase 3 never emits a successful `tree_flush_result`: the only
    // value `tree_sched::advance()` ever returns is
    //   { st                    = unsupported_unimplemented,
    //     new_manifest          = nullptr,
    //     retired               = {},
    //     flushed_gens_by_front = {},
    //     memtable_losers       = {},
    //     flushed_max_lsn       = 0 }.
    // Phases 4-7 progressively replace that with real content
    // without changing the field layout.

    struct tree_flush_result {
        flush_stage_status                          st = flush_stage_status::ok;
        std::shared_ptr<const core::tree_manifest>  new_manifest;
        core::retired_objects                       retired;
        absl::flat_hash_map<
            uint32_t,
            absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>>
                                                    flushed_gens_by_front;
        absl::InlinedVector<core::retired_value_ref, 64>
                                                    memtable_losers;
        uint64_t                                    flushed_max_lsn = 0;
    };

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_FLUSH_TYPES_HH
