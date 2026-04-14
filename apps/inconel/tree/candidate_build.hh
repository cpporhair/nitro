#ifndef APPS_INCONEL_TREE_CANDIDATE_BUILD_HH
#define APPS_INCONEL_TREE_CANDIDATE_BUILD_HH

// ── candidate_build.hh ── Phase 6 merge + cascade (step 026 / 026A) ──
//
// Three concerns:
//
//   1. `candidate_build_state` — per-worker-arm multi-round state that
//      lives on the PUMP context stack. Tracks leaf merge progress,
//      cascade progress, holds temporary page buffers for NVMe-read
//      pages, and accumulates the output `flush_worker_result`.
//
//   2. `merge_and_build_leaf()` — sorted merge of old leaf records
//      with memtable winners, tombstone compact, and candidate page
//      construction via `leaf_page_builder`. Pure function, no
//      scheduler / PUMP dependency.
//
//   3. Consolidation cascade — after leaf merge, check if shadow slot
//      is exhausted. If yes, climb the manifest's tree_reverse_topology
//      (leaf → parent → grandparent → ...), copy each ancestor's old
//      internal page into changed_nodes, and stop at the first
//      non-exhausted level or root. All I/O through the same
//      multi-round async protocol (candidate_need_read).
//
// Marked `inline` for ODR safety — both worker_scheduler.hh and test
// translation units include this header.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/inlined_vector.h>

#include "../core/memtable.hh"
#include "../core/panic.hh"
#include "../core/tree_manifest.hh"
#include "../format/tree_page.hh"
#include "../format/types.hh"
#include "./flush_types.hh"
#include "./lookup_scheduler.hh"
#include "./page_builder.hh"
#include "./page_reader.hh"

namespace apps::inconel::tree {

    // ── candidate_build_state ──────────────────────────────────────
    //
    // Lives on the PUMP context stack (`with_context`). One instance
    // per worker arm in the flush fanout. Worklist state machine:
    //
    //   Leaf queues:
    //     leaf_ready     — page data available, ready to merge
    //     leaf_need_read — cache miss, buffer not yet allocated
    //     leaf_inflight  — buffer allocated, NVMe read in flight
    //
    //   Cascade queues:
    //     cascade_ready   — ready to climb reverse_topology
    //     cascade_waiting — waiting on internal page; key = slot_paddr
    //
    //   Each round:
    //     1. Promote completions: leaf_inflight → leaf_ready;
    //        cascade_waiting entries whose page is loaded → cascade_ready
    //     2. Drain leaf_ready (merge → changed_nodes → maybe cascade_ready)
    //     3. Drain cascade_ready (climb → materialize ancestors / add to cascade_waiting)
    //     4. If no pending work → candidate_done
    //     5. Allocate buffers for leaf_need_read / cascade_waiting,
    //        bounded by budget → candidate_need_read
    //
    // No full scans of leaf_groups — work flows through queues.

    struct candidate_build_state {
        // ── inputs (set at construction, immutable) ──
        std::span<const flush_leaf_group> leaf_groups;
        const core::tree_manifest*        base_manifest;
        uint64_t                          recovery_safe_lsn;
        uint32_t                          page_size;
        uint32_t                          page_lbas;
        uint32_t                          max_reads_per_round;

        // ── first-entry classification flag ──
        bool initialized = false;

        // ── leaf worklists ──
        std::vector<uint32_t> leaf_ready;
        std::vector<uint32_t> leaf_need_read;
        std::vector<uint32_t> leaf_inflight;

        // Per-leaf temp buffer (sparse; indexed by leaf_idx).
        std::vector<std::unique_ptr<char[]>> page_bufs;

        // ── cascade worklists ──
        std::vector<uint32_t> cascade_ready;

        // slot_paddr → leaves waiting on this internal page.
        // The read is "in flight" iff internal_page_bufs contains the
        // same paddr. If not in internal_page_bufs, still need to
        // allocate a buffer in pass-5.
        absl::flat_hash_map<paddr, std::vector<uint32_t>>
            cascade_waiting;

        // Internal page buffers; persists once allocated. Existence of
        // an entry at round-entry implies the NVMe read has completed.
        absl::flat_hash_map<paddr, std::unique_ptr<char[]>>
            internal_page_bufs;

        bool all_done = false;

        // ── output (026A: manifest overlay) ──
        flush_worker_result result;
    };

    inline candidate_build_state
    make_candidate_build_state(
        std::span<const flush_leaf_group> leaf_groups,
        const core::tree_manifest*        base_manifest,
        uint64_t                          recovery_safe_lsn,
        uint32_t                          page_size,
        uint32_t                          page_lbas,
        flush_round_id                    round_id,
        uint32_t                          read_domain_index,
        uint32_t                          max_reads_per_round = 256)
    {
        auto n = leaf_groups.size();
        candidate_build_state s{
            .leaf_groups         = leaf_groups,
            .base_manifest       = base_manifest,
            .recovery_safe_lsn   = recovery_safe_lsn,
            .page_size           = page_size,
            .page_lbas           = page_lbas,
            .max_reads_per_round = max_reads_per_round,
            .initialized         = false,
            .leaf_ready          = {},
            .leaf_need_read      = {},
            .leaf_inflight       = {},
            .page_bufs           = {},
            .cascade_ready       = {},
            .cascade_waiting     = {},
            .internal_page_bufs  = {},
            .all_done            = (n == 0),
            .result              = {
                .round_id          = round_id,
                .read_domain_index = read_domain_index,
                .st                = flush_stage_status::ok,
                .base              = base_manifest,
            },
        };
        s.page_bufs.resize(n);
        return s;
    }

    // ── merge_and_build_leaf ───────────────────────────────────────
    //
    // Sorted merge of old leaf records + memtable winners into a new
    // candidate page image.
    //
    // Parameters:
    //   page_data     — old leaf page bytes (from cache or temp buffer)
    //   page_size     — tree page size in bytes
    //   keys          — memtable winners for this leaf (sorted by key)
    //   recovery_safe_lsn — tombstone gc threshold
    //   out           — output candidate (page image + retired values)
    //
    // Returns flush_stage_status:
    //   ok                        — success
    //   unsupported_shape_change  — candidate page overflows (needs split)

    inline flush_stage_status
    merge_and_build_leaf(
        const char*                              page_data,
        uint32_t                                 page_size,
        std::span<const flush_key_group>         keys,
        uint64_t                                 recovery_safe_lsn,
        flush_leaf_candidate&                    out)
    {
        // Decode old leaf.
        leaf_page_reader reader;
        if (!reader.parse(page_data, page_size)) {
            core::panic_inconsistency(
                "merge_and_build_leaf",
                "old leaf page failed validation "
                "(leaf_range_base dev=%u lba=%lu)",
                static_cast<unsigned>(out.leaf_range_base.device_id),
                static_cast<unsigned long>(out.leaf_range_base.lba));
        }

        const auto old_count = reader.record_count();

        // Allocate candidate page buffer.
        out.candidate_page.resize(page_size);
        leaf_page_builder builder;
        builder.init(out.candidate_page.data(), page_size);

        // ── sorted merge: dual-pointer ──
        //
        // Both old records and new keys are sorted by key ascending.
        // Same key → memtable winner supersedes old record (old's
        // value_ref → retired). Tombstone compact: skip winners
        // where kind == tombstone && data_ver <= recovery_safe_lsn.

        uint16_t oi = 0;
        std::size_t ni = 0;

        auto emit_old = [&](const leaf_record& rec) -> bool {
            // Tombstone compact applies to ALL records in the
            // candidate image, not just memtable winners (FF §3.4B).
            if (rec.kind == format::record_kind::tombstone
                && rec.data_ver <= recovery_safe_lsn)
                return true;  // physically delete old tombstone

            if (rec.kind == format::record_kind::value)
                return builder.add_value(rec.key, rec.data_ver, rec.vr);
            else
                return builder.add_tombstone(rec.key, rec.data_ver);
        };

        auto emit_winner = [&](const flush_key_group& kg) -> bool {
            // Tombstone compact (FF §3.4B / 026 D8).
            if (kg.winner_kind == core::memtable_entry::kind::tombstone
                && kg.winner_data_ver <= recovery_safe_lsn)
                return true;  // physically deleted — don't emit

            if (kg.winner_kind == core::memtable_entry::kind::value)
                return builder.add_value(kg.key, kg.winner_data_ver,
                                         kg.winner_value.durable);
            else
                return builder.add_tombstone(kg.key, kg.winner_data_ver);
        };

        while (oi < old_count || ni < keys.size()) {
            const bool have_old = (oi < old_count);
            const bool have_new = (ni < keys.size());

            if (have_old && have_new) {
                auto old_rec = reader.get(oi);
                int cmp = old_rec.key.compare(keys[ni].key);

                if (cmp < 0) {
                    // old-only key — preserve
                    if (!emit_old(old_rec))
                        return flush_stage_status::unsupported_shape_change;
                    ++oi;
                } else if (cmp > 0) {
                    // new-only key — insert
                    if (!emit_winner(keys[ni]))
                        return flush_stage_status::unsupported_shape_change;
                    ++ni;
                } else {
                    // same key — memtable winner supersedes old
                    if (old_rec.kind == format::record_kind::value) {
                        out.retired_old_values.push_back(
                            core::retired_value_ref{
                                .vr       = old_rec.vr,
                                .data_ver = old_rec.data_ver,
                            });
                    }
                    if (!emit_winner(keys[ni]))
                        return flush_stage_status::unsupported_shape_change;
                    ++oi;
                    ++ni;
                }
            } else if (have_old) {
                if (!emit_old(reader.get(oi)))
                    return flush_stage_status::unsupported_shape_change;
                ++oi;
            } else {
                if (!emit_winner(keys[ni]))
                    return flush_stage_status::unsupported_shape_change;
                ++ni;
            }
        }

        builder.finalize();
        out.record_count = builder.count;
        out.st = flush_stage_status::ok;
        return flush_stage_status::ok;
    }

    // ── flush_read_budget ─────────────────────────────────────────
    //
    // Computes the read budget for this round based on the paired
    // lookup sched's pending count. Called at the start of pass 2
    // in process_candidate_groups. The result is clamped to
    // [0, max_reads].
    //
    // Policy: linear back-off. When lookup is idle, flush reads at
    // full speed. When lookup queue depth exceeds the throttle
    // threshold, flush pauses reads entirely for this round.
    //
    // The threshold and the linear ramp are tuning knobs — the
    // important invariant is that budget never exceeds max_reads.

    inline uint32_t
    flush_read_budget(uint32_t pending_lookups,
                      uint32_t max_reads,
                      uint32_t throttle_threshold = 32)
    {
        if (pending_lookups == 0) return max_reads;
        if (pending_lookups >= throttle_threshold) return 1;
        uint32_t scaled = max_reads * (throttle_threshold - pending_lookups)
                        / throttle_threshold;
        return (scaled > 0) ? scaled : 1;
    }

    // ── cascade helpers (026A) ─────────────────────────────────────
    //
    // Cascade walks leaf → parent → grandparent → ... using the
    // per-manifest tree_reverse_topology. No root-down descent.
    //
    // Each climb step is O(1) table lookups for the parent's
    // range_base, plus one page fetch (cache hit / internal_page_buf
    // hit / async read). Only nodes that become part of changed_nodes
    // get their pages read — no transit reads.

    enum class cascade_step_result : uint8_t {
        complete,       // leaf's climb fully done
        need_read,      // an ancestor page is missing → waiting
    };

    struct cascade_step_outcome {
        cascade_step_result status;
        paddr missing_slot_paddr = {0, 0};  // valid iff status==need_read
    };

    template <core::cache_concept Cache>
    inline cascade_step_outcome
    cascade_climb_one_leaf(uint32_t leaf_idx,
                           candidate_build_state& state,
                           Cache* cache)
    {
        const auto* manifest = state.base_manifest;
        const auto& topo = manifest->reverse_topology;
        const auto& lg = state.leaf_groups[leaf_idx];

        if (lg.leaf_span_idx >= topo.leaf_parent_idx.size()) {
            core::panic_inconsistency(
                "cascade_climb_one_leaf",
                "leaf_span_idx %u out of range (reverse_topology "
                "leaf_parent_idx size=%zu)",
                static_cast<unsigned>(lg.leaf_span_idx),
                topo.leaf_parent_idx.size());
        }

        core::internal_idx pidx = topo.leaf_parent_idx[lg.leaf_span_idx];
        uint32_t level = 1;  // leaf's direct parent is level 1

        while (pidx != core::kInvalidInternalIdx) {
            if (pidx >= topo.internal_nodes.size()) {
                core::panic_inconsistency(
                    "cascade_climb_one_leaf",
                    "internal_idx %u out of range (internal_nodes "
                    "size=%zu)",
                    static_cast<unsigned>(pidx),
                    topo.internal_nodes.size());
            }
            const auto& node = topo.internal_nodes[pidx];

            auto existing = state.result.changed_nodes.find(node.range_base);
            if (existing != state.result.changed_nodes.end()) {
                // Another leaf's cascade (or this leaf's prior climb)
                // already materialized this level. Use its stored
                // needs_new_range to decide whether cascade continues.
                if (!existing->second.needs_new_range)
                    return {cascade_step_result::complete, {}};
                pidx = node.parent_idx;
                ++level;
                continue;
            }

            // Need to materialize this level. Fetch the page.
            paddr parent_slot = manifest->resolve(node.range_base);
            const char* page_data = nullptr;

            if (cache) {
                auto fid = make_tree_frame_id(parent_slot, state.page_lbas);
                auto pin = cache->pin(fid);
                if (pin.frame)
                    page_data = pin.frame->buf;
            }
            if (!page_data) {
                auto it = state.internal_page_bufs.find(parent_slot);
                if (it != state.internal_page_bufs.end())
                    page_data = it->second.get();
            }

            if (!page_data)
                return {cascade_step_result::need_read, parent_slot};

            bool exhausted = manifest->slot_exhausted(node.range_base);
            state.result.changed_nodes[node.range_base] = flush_changed_node{
                .range_base      = node.range_base,
                .level           = level,
                .needs_new_range = exhausted,
                .page_content    = std::vector<char>(
                    page_data, page_data + state.page_size),
            };

            if (!exhausted)
                return {cascade_step_result::complete, {}};

            pidx = node.parent_idx;
            ++level;
        }

        // Reached the sentinel (past root) — cascade complete.
        return {cascade_step_result::complete, {}};
    }

    // ── process_candidate_groups ──────────────────────────────────
    //
    // Worklist state machine called by worker's advance().
    //
    //   Phase 1: first-entry classification OR completion promotion
    //   Phase 2: drain leaf_ready (merge → maybe cascade_ready)
    //   Phase 3: drain cascade_ready (climb → materialize / add to waiting)
    //   Phase 4: check done
    //   Phase 5: allocate reads (bounded) for need_read / waiting

    template <core::cache_concept Cache>
    inline candidate_decision
    process_candidate_groups(candidate_build_state& state,
                             tree_lookup_sched_base* paired_lookup,
                             Cache* cache)
    {
        const auto n = state.leaf_groups.size();

        // ── Phase 1: classify (first entry) or promote completions ──
        if (!state.initialized) {
            for (uint32_t i = 0; i < n; ++i) {
                const auto& lg = state.leaf_groups[i];
                bool cache_hit = false;
                if (cache) {
                    auto fid = make_tree_frame_id(
                        lg.old_slot_paddr, state.page_lbas);
                    auto pin = cache->pin(fid);
                    if (pin.frame) cache_hit = true;
                }
                if (cache_hit)
                    state.leaf_ready.push_back(i);
                else
                    state.leaf_need_read.push_back(i);
            }
            state.initialized = true;
        } else {
            // Inflight leaf reads have completed.
            for (auto i : state.leaf_inflight)
                state.leaf_ready.push_back(i);
            state.leaf_inflight.clear();

            // cascade_waiting entries whose page is now loaded →
            // cascade_ready. A paddr present in internal_page_bufs at
            // entry time implies its read has completed.
            for (auto it = state.cascade_waiting.begin();
                 it != state.cascade_waiting.end();) {
                if (state.internal_page_bufs.contains(it->first)) {
                    for (auto leaf_idx : it->second)
                        state.cascade_ready.push_back(leaf_idx);
                    state.cascade_waiting.erase(it++);
                } else {
                    ++it;
                }
            }
        }

        // ── Phase 2: drain leaf_ready (merge) ──
        for (auto i : state.leaf_ready) {
            const auto& lg = state.leaf_groups[i];
            const char* page_data = nullptr;

            if (cache) {
                auto fid = make_tree_frame_id(
                    lg.old_slot_paddr, state.page_lbas);
                auto pin = cache->pin(fid);
                if (pin.frame) page_data = pin.frame->buf;
            }
            if (!page_data && state.page_bufs[i])
                page_data = state.page_bufs[i].get();

            if (!page_data) {
                // Invariant: a leaf in leaf_ready must have page data
                // available (from cache or a completed read).
                core::panic_inconsistency(
                    "process_candidate_groups",
                    "leaf_ready entry without page data (idx=%u)",
                    static_cast<unsigned>(i));
            }

            flush_leaf_candidate candidate{
                .leaf_range_base = lg.leaf_range_base,
                .old_slot_paddr  = lg.old_slot_paddr,
                .st              = flush_stage_status::ok,
            };

            auto merge_st = merge_and_build_leaf(
                page_data, state.page_size,
                lg.keys, state.recovery_safe_lsn,
                candidate);

            if (merge_st != flush_stage_status::ok) {
                state.result.st = merge_st;
                state.all_done = true;
                return candidate_done{};
            }

            bool exhausted = state.base_manifest->slot_exhausted(
                lg.leaf_range_base);

            state.result.changed_nodes[lg.leaf_range_base] = flush_changed_node{
                .range_base      = lg.leaf_range_base,
                .level           = 0,
                .needs_new_range = exhausted,
                .page_content    = std::move(candidate.candidate_page),
            };
            for (auto& rv : candidate.retired_old_values)
                state.result.retired_old_values.push_back(rv);

            state.page_bufs[i].reset();

            if (exhausted)
                state.cascade_ready.push_back(i);
        }
        state.leaf_ready.clear();

        // ── Phase 3: drain cascade_ready (climb reverse topology) ──
        for (auto i : state.cascade_ready) {
            auto outcome = cascade_climb_one_leaf(i, state, cache);
            if (outcome.status == cascade_step_result::need_read) {
                state.cascade_waiting[outcome.missing_slot_paddr]
                    .push_back(i);
            }
        }
        state.cascade_ready.clear();

        // ── Phase 4: done? ──
        if (state.leaf_need_read.empty()
            && state.leaf_inflight.empty()
            && state.cascade_waiting.empty()) {
            state.all_done = true;
            return candidate_done{};
        }

        // ── Phase 5: allocate reads (bounded) ──
        uint32_t pending = paired_lookup
            ? paired_lookup->pending_lookups
            : 0;
        uint32_t budget = flush_read_budget(
            pending, state.max_reads_per_round);

        candidate_need_read need;
        uint32_t reads = 0;

        // Leaf reads: pull from leaf_need_read → leaf_inflight.
        while (reads < budget && !state.leaf_need_read.empty()) {
            auto i = state.leaf_need_read.back();
            state.leaf_need_read.pop_back();

            state.page_bufs[i] = std::make_unique<char[]>(state.page_size);
            const auto& lg = state.leaf_groups[i];
            need.read_descs.push_back(format::read_desc{
                .lba      = lg.old_slot_paddr.lba,
                .buf      = state.page_bufs[i].get(),
                .num_lbas = state.page_lbas,
            });
            state.leaf_inflight.push_back(i);
            ++reads;
        }

        // Internal page reads: for each cascade_waiting paddr without
        // a buffer yet, allocate and issue.
        for (auto& kv : state.cascade_waiting) {
            if (reads >= budget) break;
            if (state.internal_page_bufs.contains(kv.first)) continue;

            auto buf = std::make_unique<char[]>(state.page_size);
            need.read_descs.push_back(format::read_desc{
                .lba      = kv.first.lba,
                .buf      = buf.get(),
                .num_lbas = state.page_lbas,
            });
            state.internal_page_bufs[kv.first] = std::move(buf);
            ++reads;
        }

        return need;
    }

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_CANDIDATE_BUILD_HH
