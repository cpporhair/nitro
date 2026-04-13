#ifndef APPS_INCONEL_TREE_CANDIDATE_BUILD_HH
#define APPS_INCONEL_TREE_CANDIDATE_BUILD_HH

// ── candidate_build.hh ── Phase 6 merge algorithm + state (step 026) ──
//
// Two concerns:
//
//   1. `candidate_build_state` — per-worker-arm multi-round state that
//      lives on the PUMP context stack. Tracks which leaf groups have
//      been processed, holds temporary page buffers for NVMe-read
//      pages (not cached — see 026 D5), and accumulates the output
//      `flush_candidate_batch`.
//
//   2. `merge_and_build_leaf()` — sorted merge of old leaf records
//      with memtable winners, tombstone compact, and candidate page
//      construction via `leaf_page_builder`. Pure function, no
//      scheduler / PUMP dependency.
//
// Marked `inline` for ODR safety — both worker_scheduler.hh and test
// translation units include this header.

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

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
    // per worker arm in the flush fanout. The multi-round protocol:
    //
    //   Round N:
    //     pass 1 — scan all groups: cache-hit or page_bufs[i] present
    //              → merge+build → processed[i] = true, page_bufs[i].reset()
    //     if all done → return candidate_done
    //     pass 2 — scan remaining, allocate buffers up to
    //              max_reads_per_round, return candidate_need_read
    //
    //   Pipeline does NVMe read into page_bufs → re-enter worker.
    //
    // At most ceil(miss_count / max_reads_per_round) + 1 rounds.

    struct candidate_build_state {
        // ── inputs (set at construction, immutable) ──
        std::span<const flush_leaf_group> leaf_groups;
        const core::tree_manifest*        base_manifest;
        uint64_t                          recovery_safe_lsn;
        uint32_t                          page_size;
        uint32_t                          page_lbas;
        uint32_t                          max_reads_per_round;

        // ── per-group state ──
        std::vector<bool>                          processed;
        std::vector<std::unique_ptr<char[]>>       page_bufs;
        bool                                       all_done = false;

        // ── output ──
        flush_candidate_batch                      result;
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
            .processed           = std::vector<bool>(n, false),
            .page_bufs           = {},
            .all_done            = (n == 0),
            .result              = {
                .round_id          = round_id,
                .read_domain_index = read_domain_index,
                .st                = flush_stage_status::ok,
                .leaves            = {},
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

    // ── process_candidate_groups ──────────────────────────────────
    //
    // Two-pass algorithm called by worker's advance().
    //
    // Pass 1: process all groups where page data is available
    //         (page_bufs[i] present from a previous round's NVMe read).
    // Pass 2: allocate temp buffers and collect read_descs for groups
    //         that still need their old leaf page, up to
    //         max_reads_per_round.
    //
    // Cache check is intentionally absent in Phase 6. The worker
    // cannot access the paired lookup sched's templated cache without
    // either templating the worker or introducing a type-erased cache
    // interface — both belong to the cache ownership migration step.
    // For now every old leaf is read via NVMe. The optimization
    // (skip NVMe for cache-hot pages) will be added when read_domain
    // unification lands.

    template <core::cache_concept Cache>
    inline candidate_decision
    process_candidate_groups(candidate_build_state& state,
                             tree_lookup_sched_base* paired_lookup,
                             Cache* cache)
    {
        const auto n = state.leaf_groups.size();

        // ── pass 1: process all groups with page data ready ──
        for (std::size_t i = 0; i < n; ++i) {
            if (state.processed[i]) continue;

            const auto& lg = state.leaf_groups[i];
            const char* page_data = nullptr;

            // Priority 1: shared read_domain cache. Same cache that
            // lookup uses — same call, same syntax: cache->pin(fid).
            if (cache) {
                auto fid = make_tree_frame_id(lg.old_slot_paddr, state.page_lbas);
                auto pin = cache->pin(fid);
                if (pin.frame)
                    page_data = pin.frame->buf;
            }

            // Priority 2: temp buffer from previous round's NVMe read.
            if (!page_data && state.page_bufs[i])
                page_data = state.page_bufs[i].get();

            if (!page_data) continue;  // miss — wait for pass 2

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

            state.result.leaves.push_back(std::move(candidate));
            state.processed[i] = true;
            state.page_bufs[i].reset();  // free temp buffer immediately
        }

        // ── check: all done? ──
        bool all = true;
        for (std::size_t i = 0; i < n; ++i) {
            if (!state.processed[i]) { all = false; break; }
        }
        if (all) {
            state.all_done = true;
            return candidate_done{};
        }

        // ── pass 2: collect pages to read (throttled + bounded) ──
        //
        // Query the paired lookup sched's pending count to compute
        // this round's read budget. Budget is at most
        // max_reads_per_round and may be 0 if lookup is very busy.
        uint32_t pending = paired_lookup
            ? paired_lookup->pending_lookups
            : 0;
        uint32_t budget = flush_read_budget(
            pending, state.max_reads_per_round);

        candidate_need_read need;
        uint32_t reads_this_round = 0;

        for (std::size_t i = 0; i < n; ++i) {
            if (state.processed[i]) continue;
            if (state.page_bufs[i]) continue;  // has buffer from a prior pass 2
            if (reads_this_round >= budget) break;

            state.page_bufs[i] = std::make_unique<char[]>(state.page_size);

            const auto& lg = state.leaf_groups[i];
            need.read_descs.push_back(format::read_desc{
                .lba      = lg.old_slot_paddr.lba,
                .buf      = state.page_bufs[i].get(),
                .num_lbas = state.page_lbas,
            });
            ++reads_this_round;
        }

        return need;
    }

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_CANDIDATE_BUILD_HH
