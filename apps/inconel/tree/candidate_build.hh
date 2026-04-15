#ifndef APPS_INCONEL_TREE_CANDIDATE_BUILD_HH
#define APPS_INCONEL_TREE_CANDIDATE_BUILD_HH

// ── candidate_build.hh ── Phase 7 worker tree builder (step 027) ──
//
// Step 027 replaces the Phase 6 "manifest overlay" model
// (`flush_changed_node` keyed by paddr) with an in-memory hybrid
// tree (`mem_tree_node` + paddr refs to unchanged subtrees). The
// worker now produces a complete tree from its local view; owner
// side (Phase 9) merges hybrid trees from all workers, assigns
// paddrs, patches placeholders, and writes pages.
//
// Three concerns:
//
//   1. `worker_state` — per-worker-arm multi-round state living on
//      the PUMP context stack. Tracks per-leaf and per-internal
//      progress (page reads, merge, internal cascade).
//
//   2. `merge_and_build_leaf()` — sorted merge of old leaf records
//      with memtable winners + tombstone compaction. Produces one
//      `mem_tree_node` for a clean rewrite, multiple for an
//      overflow-driven leaf split. Pure CPU; no scheduler / PUMP
//      dependency.
//
//   3. Internal cascade — after leaves are built and pairwise
//      merged, the worker walks each affected leaf's parent chain
//      (`tree_reverse_topology`) to root, reading each affected
//      old internal page and producing a new `mem_tree_node` for
//      it (substituting the changed children with our new nodes /
//      paddr refs to the rest of the original tree).
//
// All structural decisions (rewrite / leaf merge / leaf split /
// internal split / consolidation flag / root growth) are made
// here on worker side. Owner side only allocates paddrs, patches
// child_base placeholders, recomputes CRC, and writes pages.
//
// Phase 7 limits (declared per project rule constraint A):
//
//   - The worker does NOT touch `tree_allocator`. New mem_tree_nodes
//     have placeholder paddr {0,0} for their unique_ptr children;
//     owner side patches at Phase 9.
//   - Internal page CRC is left as whatever `internal_page_builder.finalize()`
//     computes from the placeholder bytes — owner side recomputes.
//   - Pairwise leaf merge is opportunistic and only triggers when
//     two adjacent (in tree) leaves under the same parent both have
//     a single mem_tree_node post-merge AND combined size <= 30%
//     page (027 §3.3).
//   - Cross-worker leaf merge is NOT attempted (027 §3.4).
//
// Marked `inline` for ODR safety — both worker_scheduler.hh and
// future test translation units may include this header.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>

#include "../core/memtable.hh"
#include "../core/page_cache.hh"
#include "../core/panic.hh"
#include "../core/tree_manifest.hh"
#include "../core/tree_reverse_topology.hh"
#include "../format/tree_page.hh"
#include "../format/types.hh"
#include "../memory/frame.hh"
#include "./flush_types.hh"
#include "./lookup_scheduler.hh"
#include "./page_builder.hh"
#include "./page_reader.hh"

namespace apps::inconel::tree {

    // ── leaf_work / internal_work / worker_state ─────────────────

    struct worker_state {
        // ── inputs (immutable after construction) ──
        flush_round_id                    round_id;
        uint32_t                          read_domain_index;
        const core::tree_manifest*        base_manifest;
        uint64_t                          recovery_safe_lsn;
        std::span<const flush_key_group>  key_groups;
        uint32_t                          page_size;
        uint32_t                          page_lbas;

        // ── one-shot init flag ──
        bool initialized = false;

        // ── per-affected-leaf state ──
        struct leaf_work {
            // base_manifest leaf identity.
            uint32_t            leaf_idx;        // index into leaf_order.spans
            paddr               leaf_range_base; // span.leaf_range_base
            paddr               old_slot_paddr;  // resolved
            core::internal_idx  parent_idx;      // for pairwise same-parent check

            // Subspan of `worker_state.key_groups` for this leaf's
            // memtable winners.
            std::span<const flush_key_group> keys;

            // NVMe read landing buffer; may stay null if cache hit.
            std::unique_ptr<char[]> read_buf;
            bool page_loaded   = false;
            bool read_inflight = false;
            bool merged        = false;

            // Post-merge result. Usually 1 mem_tree_node; 2+ if the
            // leaf had to split. After a pairwise leaf merge absorbed
            // a sibling, this leaf_work's `built_leaves[0]` carries
            // both old paddrs in its `replaces_old_paddrs`.
            absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1> built_leaves;

            // Sibling separators between consecutive `built_leaves`
            // (size = built_leaves.size() - 1). Only populated on
            // leaf split.
            absl::InlinedVector<std::string, 1> sibling_separators;

            // Set true when this leaf's mem_tree_node was absorbed by
            // a pairwise merge with the previous leaf in `s.leaves`.
            // Absorbed entries are skipped during cascade and have an
            // empty `built_leaves`.
            bool absorbed = false;
        };
        std::vector<leaf_work> leaves;

        // Map old leaf range_base → index into `leaves`. Both paddrs
        // of a pairwise-merged pair point at the same `leaves[k]`
        // (the one that absorbed the other).
        absl::flat_hash_map<paddr, uint32_t> leaf_range_base_to_idx;

        bool all_leaves_merged = false;
        bool pairwise_done     = false;

        // ── per-affected-internal state (cascade) ──
        struct internal_work {
            core::internal_idx  idx;            // index into reverse_topology.internal_nodes
            paddr               range_base;     // node's range_base
            paddr               old_slot_paddr; // resolved
            uint32_t            depth = 0;      // 1 = leaf's parent, 2 = grandparent, ..., max = root

            std::unique_ptr<char[]> read_buf;
            bool page_loaded   = false;
            bool read_inflight = false;
            bool built         = false;

            // Post-build result. Usually 1 mem_tree_node; 2+ if this
            // internal had to split.
            absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1> built_internals;

            // Sibling separators between consecutive `built_internals`
            // (size = built_internals.size() - 1). Only populated on
            // internal split.
            absl::InlinedVector<std::string, 1> sibling_separators;

            // Affected-internal children gating (Phase 7 P0 fix).
            //
            // Internal N can only be built after every descendant
            // internal in the worker's work set has been built —
            // `build_one_internal()` resolves a changed child by
            // looking it up in `s.internals` and dereferencing its
            // `built_internals`, which must already be populated.
            //
            // `process_flush_round` sorts by depth ascending, but
            // that alone does not gate "this internal's affected-child
            // internals are all ready": an ancestor page may be
            // cache-hit in the same round a descendant is still awaiting
            // an NVMe read. This vector is populated during
            // `initialize_cascade()` with the internal_idxs of every
            // affected-internal child of this node (leaves do not need
            // gating — they are always built before cascade starts).
            absl::InlinedVector<core::internal_idx, 8> affected_child_internals;
        };
        absl::flat_hash_map<core::internal_idx, internal_work> internals;

        bool cascade_initialized = false;
        bool all_internals_built = false;

        // The worker's view-local root after cascade. This is the
        // internal_idx whose mem_tree_node becomes proposal.root,
        // OR kInvalidInternalIdx if there were no internals (the
        // single-leaf-tree case where the leaf itself is root).
        core::internal_idx top_internal_idx = core::kInvalidInternalIdx;

        // ── output ──
        worker_tree_proposal result;
        bool                 all_done = false;
    };

    inline worker_state
    make_worker_state(const flush_worker_req& req)
    {
        if (req.base_manifest == nullptr) {
            core::panic_inconsistency("make_worker_state",
                "flush_worker_req.base_manifest is null");
        }
        if (req.base_manifest->geom == nullptr) {
            core::panic_inconsistency("make_worker_state",
                "flush_worker_req.base_manifest->geom is null");
        }

        worker_state s;
        s.round_id          = req.round_id;
        s.read_domain_index = req.read_domain_index;
        s.base_manifest     = req.base_manifest;
        s.recovery_safe_lsn = req.recovery_safe_lsn;
        s.key_groups        = req.key_groups;
        s.page_size         = req.base_manifest->geom->tree_page_size;
        s.page_lbas         = req.base_manifest->page_lbas();

        s.result.round_id          = req.round_id;
        s.result.read_domain_index = req.read_domain_index;
        s.result.st                = flush_stage_status::ok;

        // Empty partition (should not happen with non-empty workset
        // and non-degenerate partitioning). Mark the proposal as
        // trivially done; root stays null.
        if (req.key_groups.empty()) {
            s.all_done = true;
        }

        return s;
    }

    // ── leaf-builder helpers ─────────────────────────────────────

    namespace _wb {  // worker-build private helpers

        // Decode an old leaf page and merge with sorted memtable
        // winners; emit greedily into one or more new pages, splitting
        // at overflow. Tombstones with data_ver <= recovery_safe_lsn
        // are physically dropped (FF §3.4B).
        //
        // First emitted page carries `replaces_old_paddrs = [old_range_base]`;
        // subsequent split siblings carry empty `replaces_old_paddrs`
        // (027 §2.1 convention: split siblings are "new pages").
        //
        // Sibling separators (between split parts) are pushed into
        // `sibling_seps_out`; size = (returned).size() - 1.
        //
        // If the merge produces zero records (e.g. all old records
        // were dropped tombstones and no winners), a single empty leaf
        // page is emitted that still claims to replace the old
        // range_base — Phase 9 owner side decides whether to retire
        // the slot entirely or keep an empty page.
        inline absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1>
        merge_and_build_leaf_impl(
            const char*                              old_page_data,
            uint32_t                                 page_size,
            std::span<const flush_key_group>         keys,
            uint64_t                                 recovery_safe_lsn,
            paddr                                    old_leaf_range_base,
            absl::InlinedVector<std::string, 1>&     sibling_seps_out,
            absl::InlinedVector<core::retired_value_ref, 64>& retired_out)
        {
            sibling_seps_out.clear();

            leaf_page_reader reader;
            if (!reader.parse(old_page_data, page_size)) {
                core::panic_inconsistency(
                    "merge_and_build_leaf",
                    "old leaf page failed validation "
                    "(leaf_range_base dev=%u lba=%lu)",
                    static_cast<unsigned>(old_leaf_range_base.device_id),
                    static_cast<unsigned long>(old_leaf_range_base.lba));
            }

            // ── one-pass sorted merge → flat record list ──
            //
            // We collect a flat list of (key, kind, data_ver, vr) so
            // the page-chunking pass below can decide split points
            // independently of the merge logic. Tombstone GC happens
            // during merge; old values that lost to memtable winners
            // are pushed into `retired_out`.

            struct merged_rec {
                std::string_view              key;
                uint64_t                      data_ver;
                core::memtable_entry::kind    kind;
                format::value_ref             vr;  // valid iff kind==value
            };

            std::vector<merged_rec> merged;
            merged.reserve(static_cast<std::size_t>(reader.record_count())
                           + keys.size());

            const auto old_count = reader.record_count();
            uint16_t    oi = 0;
            std::size_t ni = 0;

            auto emit_old = [&](const leaf_record& r) {
                // Tombstone compact: drop if data_ver <= recovery
                // barrier (FF §3.4B). Applies to BOTH old records and
                // new winners; for old records we just skip the emit.
                if (r.kind == format::record_kind::tombstone
                    && r.data_ver <= recovery_safe_lsn) {
                    return;
                }
                merged_rec m{
                    .key      = r.key,
                    .data_ver = r.data_ver,
                    .kind     = (r.kind == format::record_kind::value)
                                    ? core::memtable_entry::kind::value
                                    : core::memtable_entry::kind::tombstone,
                    .vr       = (r.kind == format::record_kind::value)
                                    ? r.vr
                                    : format::value_ref{},
                };
                merged.push_back(m);
            };

            auto emit_winner = [&](const flush_key_group& kg) {
                if (kg.winner_kind == core::memtable_entry::kind::tombstone
                    && kg.winner_data_ver <= recovery_safe_lsn) {
                    return;
                }
                merged_rec m{
                    .key      = kg.key,
                    .data_ver = kg.winner_data_ver,
                    .kind     = kg.winner_kind,
                    .vr       = (kg.winner_kind == core::memtable_entry::kind::value)
                                    ? kg.winner_value.durable
                                    : format::value_ref{},
                };
                merged.push_back(m);
            };

            while (oi < old_count || ni < keys.size()) {
                const bool have_old = (oi < old_count);
                const bool have_new = (ni < keys.size());
                if (have_old && have_new) {
                    auto old_rec = reader.get(oi);
                    int cmp = old_rec.key.compare(keys[ni].key);
                    if (cmp < 0) {
                        emit_old(old_rec);
                        ++oi;
                    } else if (cmp > 0) {
                        emit_winner(keys[ni]);
                        ++ni;
                    } else {
                        // Same key — memtable winner supersedes old.
                        // Old value (if any) joins retired_old_values.
                        if (old_rec.kind == format::record_kind::value) {
                            retired_out.push_back(core::retired_value_ref{
                                .vr       = old_rec.vr,
                                .data_ver = old_rec.data_ver,
                            });
                        }
                        emit_winner(keys[ni]);
                        ++oi;
                        ++ni;
                    }
                } else if (have_old) {
                    emit_old(reader.get(oi));
                    ++oi;
                } else {
                    emit_winner(keys[ni]);
                    ++ni;
                }
            }

            // ── chunk into pages (greedy fill, split on overflow) ──

            absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1> result;

            auto emit_chunk_to_node = [&](std::size_t start, std::size_t end)
                -> std::unique_ptr<mem_tree_node>
            {
                auto node = std::make_unique<mem_tree_node>();
                node->type = format::node_type::leaf;
                node->content.resize(page_size);

                leaf_page_builder builder;
                builder.init(node->content.data(), page_size);
                for (std::size_t i = start; i < end; ++i) {
                    auto& m = merged[i];
                    bool ok = (m.kind == core::memtable_entry::kind::value)
                                  ? builder.add_value(m.key, m.data_ver, m.vr)
                                  : builder.add_tombstone(m.key, m.data_ver);
                    if (!ok) {
                        core::panic_inconsistency(
                            "merge_and_build_leaf",
                            "record did not fit despite chunk pre-sizing "
                            "(idx=%zu)",
                            i);
                    }
                }
                builder.finalize();
                return node;
            };

            // Greedy page-fill: simulate add_value/add_tombstone by
            // measuring expected record sizes against running page
            // budget. The leaf builder accounts for header + per-record
            // directory entry + record bytes.
            std::size_t chunk_start = 0;
            std::size_t i           = 0;
            uint32_t    used        = sizeof(format::tree_slot_header);
            uint16_t    chunk_count = 0;

            auto rec_size = [](const merged_rec& m) -> uint32_t {
                uint16_t key_len = static_cast<uint16_t>(m.key.size());
                if (m.kind == core::memtable_entry::kind::value) {
                    return sizeof(format::leaf_record_header) + key_len
                         + sizeof(format::value_ref);
                }
                return sizeof(format::leaf_record_header) + key_len;
            };

            while (i < merged.size()) {
                uint32_t add = rec_size(merged[i]);
                uint32_t dir_after = sizeof(uint16_t)
                                   * static_cast<uint32_t>(chunk_count + 1u);
                if (used + add + dir_after > page_size) {
                    // Cannot fit — close current chunk and start new.
                    if (chunk_count == 0) {
                        // Single record larger than a page — only
                        // realistic with absurd key sizes for the
                        // current geom. Constraint A: declare and
                        // panic instead of silent truncation.
                        core::panic_inconsistency(
                            "merge_and_build_leaf",
                            "single record larger than page "
                            "(rec_size=%u page_size=%u key_len=%zu)",
                            add, page_size, merged[i].key.size());
                    }
                    auto node = emit_chunk_to_node(chunk_start, i);
                    if (result.empty()) {
                        node->replaces_old_paddrs.push_back(old_leaf_range_base);
                    }
                    if (!result.empty()) {
                        // Sibling separator between previous and this
                        // chunk = first key of this chunk.
                        sibling_seps_out.push_back(
                            std::string(merged[chunk_start].key));
                    }
                    result.push_back(std::move(node));
                    chunk_start = i;
                    used        = sizeof(format::tree_slot_header);
                    chunk_count = 0;
                    continue;  // re-evaluate this record against fresh page
                }
                used += add;
                chunk_count++;
                ++i;
            }

            // Tail chunk.
            if (chunk_count > 0) {
                auto node = emit_chunk_to_node(chunk_start, merged.size());
                if (result.empty()) {
                    node->replaces_old_paddrs.push_back(old_leaf_range_base);
                }
                if (!result.empty()) {
                    sibling_seps_out.push_back(
                        std::string(merged[chunk_start].key));
                }
                result.push_back(std::move(node));
            }

            // Empty merge result — emit a single empty page so the
            // parent still has a child slot to reference.
            if (result.empty()) {
                auto node = std::make_unique<mem_tree_node>();
                node->type = format::node_type::leaf;
                node->content.resize(page_size);
                node->replaces_old_paddrs.push_back(old_leaf_range_base);
                leaf_page_builder builder;
                builder.init(node->content.data(), page_size);
                builder.finalize();
                result.push_back(std::move(node));
            }

            return result;
        }

        // ── pairwise leaf merge (027 §3.3) ──
        //
        // Combine two adjacent (in this worker's `leaves` vector) leaf
        // mem_tree_nodes into one when both have a single built_leaf,
        // share the same parent in the base_manifest, and combined
        // record bytes are <= 30% of a page. The right one is marked
        // absorbed; the left's built_leaves[0] becomes the merged
        // page.
        //
        // Pre-condition: `s.all_leaves_merged == true` (every leaf has
        // built_leaves populated).
        //
        // Post-condition: for each absorbed leaf_work, `absorbed = true`
        // and `built_leaves` is empty. The map
        // `s.leaf_range_base_to_idx` is updated so the absorbed
        // leaf's range_base maps to the absorbing leaf_work index.
        inline void
        do_pairwise_leaf_merge_impl(worker_state& s)
        {
            const uint32_t threshold = (s.page_size * 30) / 100;
            const uint32_t header_overlap = sizeof(format::tree_slot_header);

            for (std::size_t i = 0; i + 1 < s.leaves.size(); ++i) {
                auto& L = s.leaves[i];
                auto& R = s.leaves[i + 1];
                if (L.absorbed || R.absorbed) continue;
                if (L.built_leaves.size() != 1) continue;  // L was split
                if (R.built_leaves.size() != 1) continue;  // R was split
                if (L.parent_idx == core::kInvalidInternalIdx) continue;
                if (L.parent_idx != R.parent_idx) continue;

                // free_space_offset is the byte position right after
                // the last record (header + dir + records). Sum
                // approximates combined "useful" bytes; subtract one
                // header (we keep only one header in the merged page).
                leaf_page_reader lr, rr;
                if (!lr.parse(L.built_leaves[0]->content.data(), s.page_size)) {
                    core::panic_inconsistency(
                        "do_pairwise_leaf_merge",
                        "left leaf candidate failed reparse");
                }
                if (!rr.parse(R.built_leaves[0]->content.data(), s.page_size)) {
                    core::panic_inconsistency(
                        "do_pairwise_leaf_merge",
                        "right leaf candidate failed reparse");
                }

                uint32_t l_used = lr.hdr->free_space_offset;
                uint32_t r_used = rr.hdr->free_space_offset;
                if (l_used + r_used <= header_overlap) continue;
                uint32_t combined_estimate = l_used + r_used - header_overlap;
                if (combined_estimate > threshold) continue;

                // Build the merged page.
                auto merged = std::make_unique<mem_tree_node>();
                merged->type = format::node_type::leaf;
                merged->content.resize(s.page_size);
                // replaces_old_paddrs: union of both leaves' paddrs,
                // preserving order (left first).
                for (auto& p : L.built_leaves[0]->replaces_old_paddrs) {
                    merged->replaces_old_paddrs.push_back(p);
                }
                for (auto& p : R.built_leaves[0]->replaces_old_paddrs) {
                    merged->replaces_old_paddrs.push_back(p);
                }

                leaf_page_builder builder;
                builder.init(merged->content.data(), s.page_size);
                for (uint16_t j = 0; j < lr.record_count(); ++j) {
                    auto rec = lr.get(j);
                    bool ok = (rec.kind == format::record_kind::value)
                                  ? builder.add_value(rec.key, rec.data_ver, rec.vr)
                                  : builder.add_tombstone(rec.key, rec.data_ver);
                    if (!ok) {
                        core::panic_inconsistency(
                            "do_pairwise_leaf_merge",
                            "left record did not fit in combined page");
                    }
                }
                for (uint16_t j = 0; j < rr.record_count(); ++j) {
                    auto rec = rr.get(j);
                    bool ok = (rec.kind == format::record_kind::value)
                                  ? builder.add_value(rec.key, rec.data_ver, rec.vr)
                                  : builder.add_tombstone(rec.key, rec.data_ver);
                    if (!ok) {
                        core::panic_inconsistency(
                            "do_pairwise_leaf_merge",
                            "right record did not fit in combined page");
                    }
                }
                builder.finalize();

                L.built_leaves.clear();
                L.built_leaves.push_back(std::move(merged));
                R.built_leaves.clear();
                R.absorbed = true;

                // Map R's range_base to L now (so cascade still
                // resolves R's old paddr to the merged group).
                s.leaf_range_base_to_idx[R.leaf_range_base] =
                    static_cast<uint32_t>(i);
            }
        }

        // ── internal-page builder (handles overflow split) ──
        //
        // Given a list of new children + separators (in cover-key
        // order), produce one or more internal mem_tree_nodes. The
        // first emitted node carries `replaces_old_paddrs = [old_paddr]`
        // (when `is_new_layer == false`); subsequent split siblings
        // carry an empty `replaces_old_paddrs`.
        //
        // Sibling separators between consecutive emitted internals
        // are pushed into `sibling_seps_out`; the value is the
        // original separator that fell BETWEEN the split chunks
        // (027 §3.4 / §3.5: the lost separator becomes the parent's
        // new boundary).
        inline absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1>
        build_internal_pages(
            std::vector<child_ref>&&             new_children,
            std::vector<std::string>&&           new_separators,
            paddr                                replaces_old_paddr,
            bool                                 is_new_layer,
            uint32_t                             page_size,
            absl::InlinedVector<std::string, 1>& sibling_seps_out)
        {
            sibling_seps_out.clear();

            absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1> result;

            if (new_children.empty()) {
                core::panic_inconsistency(
                    "build_internal_pages",
                    "internal node has zero children "
                    "(replaces dev=%u lba=%lu, is_new_layer=%d)",
                    static_cast<unsigned>(replaces_old_paddr.device_id),
                    static_cast<unsigned long>(replaces_old_paddr.lba),
                    is_new_layer ? 1 : 0);
            }

            auto child_to_paddr = [](const child_ref& c) -> paddr {
                if (auto* p = std::get_if<paddr>(&c.target)) return *p;
                return paddr{0, 0};  // placeholder for unique_ptr child
            };

            std::size_t next = 0;
            while (next < new_children.size()) {
                // Find K = max children that fit, growing greedily.
                // First child tentatively counts as rightmost
                // (sizeof(paddr) cost). Subsequent children promote
                // the previous rightmost into a record (cost shift =
                // internal_record_size(sep) + 2 directory bytes), and
                // claim a new rightmost slot — net +
                // internal_record_size(sep) + 2 per added child.
                uint32_t used = sizeof(format::tree_slot_header);
                uint32_t K    = 0;
                for (std::size_t j = next; j < new_children.size(); ++j) {
                    uint32_t additional;
                    if (K == 0) {
                        additional = sizeof(format::paddr);
                    } else {
                        const auto& sep = new_separators[next + K - 1];
                        additional = format::internal_record_size(
                                         static_cast<uint16_t>(sep.size()))
                                   + sizeof(uint16_t);
                    }
                    if (used + additional > page_size) break;
                    used += additional;
                    ++K;
                }

                if (K == 0) {
                    core::panic_inconsistency(
                        "build_internal_pages",
                        "single child cannot fit in page (page_size=%u)",
                        page_size);
                }

                auto node = std::make_unique<mem_tree_node>();
                node->type = format::node_type::internal;
                node->content.resize(page_size);
                if (result.empty() && !is_new_layer) {
                    node->replaces_old_paddrs.push_back(replaces_old_paddr);
                }

                // Format with placeholders for unique_ptr children.
                internal_page_builder builder;
                builder.init(node->content.data(), page_size);
                for (uint32_t j = 0; j + 1 < K; ++j) {
                    const auto& sep = new_separators[next + j];
                    if (!builder.add_child(sep,
                                            child_to_paddr(new_children[next + j])))
                    {
                        core::panic_inconsistency(
                            "build_internal_pages",
                            "add_child failed despite size precomputation "
                            "(j=%u K=%u)", j, K);
                    }
                }
                builder.set_rightmost_child(
                    child_to_paddr(new_children[next + K - 1]));
                builder.finalize();

                // Move structured children/separators into mem_tree_node
                // so Phase 9 owner can walk the structure without
                // re-decoding the page.
                node->children.reserve(K);
                node->separators.reserve(K - 1);
                for (uint32_t j = 0; j < K; ++j) {
                    node->children.push_back(std::move(new_children[next + j]));
                }
                for (uint32_t j = 0; j + 1 < K; ++j) {
                    node->separators.push_back(std::move(new_separators[next + j]));
                }
                result.push_back(std::move(node));

                // If there's a next chunk, the separator at index
                // `next + K - 1` is the boundary between this chunk and
                // the next. Claim it as a sibling separator.
                if (next + K < new_children.size()) {
                    sibling_seps_out.push_back(
                        std::move(new_separators[next + K - 1]));
                }
                next += K;
            }

            return result;
        }

        // For a child paddr in some old internal page, look up our
        // worker's contribution (if any). Returns:
        //   - nullopt: child unchanged (use paddr ref in new internal)
        //   - {leaf_work*, ...}: leaf-level group at that child slot
        //   - {internal_work*, ...}: internal-level group at that slot
        struct child_lookup_result {
            absl::InlinedVector<mem_tree_node*, 1>     nodes;        // borrowed
            absl::InlinedVector<std::string_view, 1>   sib_seps;     // borrowed
            absl::InlinedVector<paddr, 2>              covered_paddrs; // for dedupe
            bool                                       is_leaf = false;
        };

        // Pull the build group that represents `child_base` into our
        // new-internal layout. Returns false when the child is
        // unchanged (caller should emit a paddr child_ref instead).
        // When true, the caller must NOT process subsequent old
        // children whose paddrs are in `out.covered_paddrs` (they
        // were absorbed into the same group).
        inline bool
        lookup_child_group(worker_state&            s,
                           paddr                    child_base,
                           child_lookup_result&     out)
        {
            out.nodes.clear();
            out.sib_seps.clear();
            out.covered_paddrs.clear();

            // Try leaf-side first.
            auto lit = s.leaf_range_base_to_idx.find(child_base);
            if (lit != s.leaf_range_base_to_idx.end()) {
                auto& lw = s.leaves[lit->second];
                if (lw.built_leaves.empty()) {
                    core::panic_inconsistency(
                        "lookup_child_group",
                        "leaf_work %u resolved with no built_leaves "
                        "(absorbed=%d)",
                        static_cast<unsigned>(lit->second),
                        lw.absorbed ? 1 : 0);
                }
                for (auto& n : lw.built_leaves) {
                    out.nodes.push_back(n.get());
                    for (auto& p : n->replaces_old_paddrs) {
                        out.covered_paddrs.push_back(p);
                    }
                }
                for (auto& s : lw.sibling_separators) {
                    out.sib_seps.push_back(s);
                }
                out.is_leaf = true;
                return true;
            }

            // Try internal-side.
            for (auto& [idx, iw] : s.internals) {
                if (iw.range_base != child_base) continue;
                if (iw.built_internals.empty()) {
                    // Internal not built yet — the depth-ordered
                    // build pass guarantees we don't reach this
                    // case under normal control flow.
                    core::panic_inconsistency(
                        "lookup_child_group",
                        "internal idx=%u not built when looked up "
                        "(depth=%u)",
                        static_cast<unsigned>(idx), iw.depth);
                }
                for (auto& n : iw.built_internals) {
                    out.nodes.push_back(n.get());
                    for (auto& p : n->replaces_old_paddrs) {
                        out.covered_paddrs.push_back(p);
                    }
                }
                for (auto& sep : iw.sibling_separators) {
                    out.sib_seps.push_back(sep);
                }
                out.is_leaf = false;
                return true;
            }

            return false;  // unchanged
        }

        // Build the internal mem_tree_node(s) for `iw` from its old
        // page. Walks (sep, child_base) entries + rightmost; for each
        // old child, either keeps as paddr ref or substitutes from
        // our work groups.
        inline void
        build_one_internal(worker_state& s,
                           worker_state::internal_work& iw,
                           const char* old_page_data)
        {
            internal_page_reader reader;
            if (!reader.parse(old_page_data, s.page_size)) {
                core::panic_inconsistency(
                    "build_one_internal",
                    "old internal page failed validation "
                    "(range_base dev=%u lba=%lu)",
                    static_cast<unsigned>(iw.range_base.device_id),
                    static_cast<unsigned long>(iw.range_base.lba));
            }

            const auto old_count = reader.record_count();

            // Reconstruct the old children + separators arrays from
            // the page so we can walk them in cover-key order.
            // children = old_count + 1 (rightmost), separators = old_count.
            std::vector<paddr>            old_children;
            std::vector<std::string_view> old_separators;
            old_children.reserve(static_cast<std::size_t>(old_count) + 1);
            old_separators.reserve(old_count);
            for (uint16_t j = 0; j < old_count; ++j) {
                auto e = reader.get(j);
                old_separators.push_back(e.separator_key);
                old_children.push_back(e.child_base);
            }
            old_children.push_back(reader.rightmost_child());

            // Walk old children, building new_children + new_separators.
            std::vector<child_ref>   new_children;
            std::vector<std::string> new_separators;
            new_children.reserve(old_children.size());
            new_separators.reserve(old_separators.size());

            child_lookup_result lookup;
            std::size_t i = 0;
            bool have_emitted = false;
            while (i < old_children.size()) {
                if (lookup_child_group(s, old_children[i], lookup)) {
                    // Changed — emit the group. First, separator before
                    // the group (if not the first emitted entry).
                    if (have_emitted) {
                        new_separators.push_back(
                            std::string(old_separators[i - 1]));
                    }
                    // Emit group's nodes interspersed with sibling
                    // separators for splits.
                    for (std::size_t j = 0; j < lookup.nodes.size(); ++j) {
                        auto* n = lookup.nodes[j];
                        if (j > 0) {
                            new_separators.push_back(
                                std::string(lookup.sib_seps[j - 1]));
                        }
                        // We need to extract a unique_ptr ownership
                        // for the child_ref. Since `lookup` holds raw
                        // borrowed pointers (the unique_ptrs live in
                        // leaf_work/internal_work), we move them out
                        // here. But since the source is in worker_state,
                        // we do the move directly via re-lookup at
                        // construction time.
                        //
                        // Simpler approach: rebuild the child_ref by
                        // moving the unique_ptr out of its source. We
                        // do it inline below to keep ownership flow
                        // visible.
                        new_children.push_back(child_ref{
                            .target = std::unique_ptr<mem_tree_node>{},
                        });
                        // Will fix up the unique_ptr move just after.
                    }
                    // Now move-out the unique_ptrs from source. We
                    // deferred this so the loop bound was clean.
                    if (lookup.is_leaf) {
                        auto lit = s.leaf_range_base_to_idx.find(old_children[i]);
                        auto& lw = s.leaves[lit->second];
                        std::size_t base = new_children.size() - lookup.nodes.size();
                        for (std::size_t j = 0; j < lookup.nodes.size(); ++j) {
                            new_children[base + j].target =
                                std::move(lw.built_leaves[j]);
                        }
                        // Drop the now-empty unique_ptrs in source.
                        lw.built_leaves.clear();
                    } else {
                        // Find the iw_src by linear scan (small N).
                        worker_state::internal_work* iw_src = nullptr;
                        for (auto& [_, candidate] : s.internals) {
                            if (candidate.range_base == old_children[i]) {
                                iw_src = &candidate;
                                break;
                            }
                        }
                        if (!iw_src) {
                            core::panic_inconsistency(
                                "build_one_internal",
                                "lost internal_work for child_base "
                                "dev=%u lba=%lu",
                                static_cast<unsigned>(old_children[i].device_id),
                                static_cast<unsigned long>(old_children[i].lba));
                        }
                        std::size_t base = new_children.size() - lookup.nodes.size();
                        for (std::size_t j = 0; j < lookup.nodes.size(); ++j) {
                            new_children[base + j].target =
                                std::move(iw_src->built_internals[j]);
                        }
                        iw_src->built_internals.clear();
                    }
                    have_emitted = true;
                    // Skip subsequent old children covered by this group
                    // (pairwise leaf merge case: 2 covered_paddrs).
                    std::size_t skip = 1;
                    if (lookup.covered_paddrs.size() > 1) {
                        // The group covers consecutive old children.
                        // We've already accounted for old_children[i];
                        // skip the rest.
                        skip = lookup.covered_paddrs.size();
                    }
                    i += skip;
                } else {
                    // Unchanged — paddr ref.
                    if (have_emitted) {
                        new_separators.push_back(
                            std::string(old_separators[i - 1]));
                    }
                    new_children.push_back(child_ref{
                        .target = old_children[i],
                    });
                    have_emitted = true;
                    ++i;
                }
            }

            iw.built_internals = build_internal_pages(
                std::move(new_children),
                std::move(new_separators),
                iw.range_base,
                /*is_new_layer=*/false,
                s.page_size,
                iw.sibling_separators);
        }

        // ── cascade init: classify keys + populate leaves & internals ──

        inline flush_stage_status
        initialize_worker(worker_state& s)
        {
            const auto& lo   = s.base_manifest->leaf_order;
            const auto& topo = s.base_manifest->reverse_topology;

            if (lo.empty()) {
                // Empty tree (no root). Phase 7 narrowing — bootstrap
                // is owner-side Phase 9 territory.
                return flush_stage_status::unsupported_shape_change;
            }

            // Group keys by leaf_idx (sorted input → contiguous runs).
            std::size_t i = 0;
            const auto N = s.key_groups.size();
            const auto leaf_count = static_cast<uint32_t>(lo.size());

            while (i < N) {
                auto leaf_idx = lo.find_leaf_for_key(s.key_groups[i].key);
                if (leaf_idx >= leaf_count) {
                    return flush_stage_status::unsupported_shape_change;
                }
                std::size_t start = i;
                ++i;
                while (i < N) {
                    auto next_idx = lo.find_leaf_for_key(s.key_groups[i].key);
                    if (next_idx != leaf_idx) break;
                    ++i;
                }

                worker_state::leaf_work lw;
                lw.leaf_idx        = static_cast<uint32_t>(leaf_idx);
                lw.leaf_range_base = lo.spans[leaf_idx].leaf_range_base;
                lw.old_slot_paddr  = s.base_manifest->resolve(lw.leaf_range_base);

                // Fetch parent_idx; kInvalidInternalIdx means leaf is
                // root (single-leaf tree case).
                if (lw.leaf_idx >= topo.leaf_parent_idx.size()) {
                    core::panic_inconsistency(
                        "initialize_worker",
                        "leaf_idx %u out of range for reverse_topology "
                        "(leaf_parent_idx size=%zu)",
                        static_cast<unsigned>(lw.leaf_idx),
                        topo.leaf_parent_idx.size());
                }
                lw.parent_idx = topo.leaf_parent_idx[lw.leaf_idx];

                lw.keys = s.key_groups.subspan(start, i - start);
                s.leaves.push_back(std::move(lw));
            }

            // Build leaf_range_base → idx map.
            s.leaf_range_base_to_idx.reserve(s.leaves.size());
            for (uint32_t k = 0; k < s.leaves.size(); ++k) {
                s.leaf_range_base_to_idx[s.leaves[k].leaf_range_base] = k;
            }

            return flush_stage_status::ok;
        }

        // ── populate s.internals based on affected leaves' parent chains ──

        inline void
        initialize_cascade(worker_state& s)
        {
            const auto& topo = s.base_manifest->reverse_topology;

            // Walk each non-absorbed leaf's parent chain to root.
            // Use a queue so we don't re-traverse subpaths.
            for (auto& lw : s.leaves) {
                if (lw.absorbed) continue;
                core::internal_idx pidx = lw.parent_idx;
                uint32_t depth = 1;
                while (pidx != core::kInvalidInternalIdx) {
                    if (pidx >= topo.internal_nodes.size()) {
                        core::panic_inconsistency(
                            "initialize_cascade",
                            "internal_idx %u out of range "
                            "(internal_nodes size=%zu)",
                            static_cast<unsigned>(pidx),
                            topo.internal_nodes.size());
                    }
                    auto it = s.internals.find(pidx);
                    if (it == s.internals.end()) {
                        worker_state::internal_work iw;
                        iw.idx            = pidx;
                        iw.range_base     = topo.internal_nodes[pidx].range_base;
                        iw.old_slot_paddr = s.base_manifest->resolve(iw.range_base);
                        iw.depth          = depth;
                        s.internals.emplace(pidx, std::move(iw));
                    } else {
                        // Update depth to max — same node might be
                        // reachable from multiple leaves at different
                        // climb points (only happens with bugs but
                        // be defensive).
                        if (it->second.depth < depth) it->second.depth = depth;
                    }
                    pidx = topo.internal_nodes[pidx].parent_idx;
                    ++depth;
                }
            }

            // The "top" internal of the worker's view = the one with
            // max depth (root of the reachable-from-affected-leaves
            // sub-forest). With reverse_topology being a single
            // rooted tree, there's exactly one such node — the actual
            // root — when at least one leaf is affected.
            uint32_t max_depth = 0;
            for (auto& [idx, iw] : s.internals) {
                if (iw.depth > max_depth) {
                    max_depth = iw.depth;
                    s.top_internal_idx = idx;
                }
            }

            // Populate affected_child_internals for build-time gating
            // (P0 fix). For each affected internal, add itself to its
            // parent's child list — but only if that parent is also
            // affected (i.e. present in s.internals).
            for (auto& [idx, iw] : s.internals) {
                auto parent_idx = topo.internal_nodes[idx].parent_idx;
                if (parent_idx == core::kInvalidInternalIdx) continue;
                auto pit = s.internals.find(parent_idx);
                if (pit == s.internals.end()) continue;
                pit->second.affected_child_internals.push_back(idx);
            }
        }

        // ── finalize: pluck the top mem_tree_node into result.root ──
        //
        // Worker view is local — Phase 9 owner side decides whether
        // a global root split / new layer is needed; worker only
        // expresses "from my view, root is this mem_tree_node, which
        // may itself be a split-at-root sequence". When worker's top
        // internal split into multiple, we synthesize a single-layer
        // wrapper internal whose children are the split parts (a
        // "local root split"), per 027 §3.2 step 5.
        //
        // Single-leaf-tree (no internals): the leaf itself is root.
        inline void
        finalize_root(worker_state& s)
        {
            if (s.internals.empty()) {
                // No cascade: single leaf is root. Find the (only)
                // non-absorbed leaf_work (single-leaf tree implies
                // exactly one affected leaf, since worker can only
                // see leaves that exist in the tree).
                worker_state::leaf_work* lw = nullptr;
                for (auto& candidate : s.leaves) {
                    if (candidate.absorbed) continue;
                    if (lw != nullptr) {
                        core::panic_inconsistency(
                            "finalize_root",
                            "multiple non-absorbed leaves but no internals "
                            "(leaves=%zu)", s.leaves.size());
                    }
                    lw = &candidate;
                }
                if (lw == nullptr) {
                    // No affected leaves — proposal stays empty.
                    return;
                }
                if (lw->built_leaves.size() == 1) {
                    s.result.root = std::move(lw->built_leaves[0]);
                    return;
                }
                // Single-leaf root that overflowed into multiple leaves
                // → we created a new internal layer (the worker's
                // local root split). Synthesize the wrapper internal.
                std::vector<child_ref> new_children;
                std::vector<std::string> new_separators;
                new_children.reserve(lw->built_leaves.size());
                for (auto& n : lw->built_leaves) {
                    new_children.push_back(child_ref{ .target = std::move(n) });
                }
                lw->built_leaves.clear();
                for (auto& sep : lw->sibling_separators) {
                    new_separators.push_back(std::move(sep));
                }
                lw->sibling_separators.clear();

                absl::InlinedVector<std::string, 1> wrap_sib_seps;
                auto wrap_pages = build_internal_pages(
                    std::move(new_children),
                    std::move(new_separators),
                    paddr{0, 0},
                    /*is_new_layer=*/true,
                    s.page_size,
                    wrap_sib_seps);
                if (wrap_pages.size() == 1) {
                    s.result.root = std::move(wrap_pages[0]);
                } else {
                    // Wrapping itself overflowed (extremely large key
                    // count for one originally-leaf-rooted tree). Add
                    // another layer.
                    std::vector<child_ref> outer_children;
                    std::vector<std::string> outer_seps;
                    for (auto& n : wrap_pages) {
                        outer_children.push_back(
                            child_ref{ .target = std::move(n) });
                    }
                    for (auto& sep : wrap_sib_seps) {
                        outer_seps.push_back(std::move(sep));
                    }
                    absl::InlinedVector<std::string, 1> outer_sib_seps;
                    auto outer_pages = build_internal_pages(
                        std::move(outer_children),
                        std::move(outer_seps),
                        paddr{0, 0},
                        /*is_new_layer=*/true,
                        s.page_size,
                        outer_sib_seps);
                    if (outer_pages.size() != 1) {
                        core::panic_inconsistency(
                            "finalize_root",
                            "two layers of wrap-internal still overflow "
                            "(outer_pages=%zu)", outer_pages.size());
                    }
                    s.result.root = std::move(outer_pages[0]);
                }
                return;
            }

            // Cascade case: top internal becomes root. If top split
            // into multiple, wrap them in a new layer (local root
            // split, 027 §3.2 step 5).
            auto it = s.internals.find(s.top_internal_idx);
            if (it == s.internals.end()) {
                core::panic_inconsistency(
                    "finalize_root",
                    "top_internal_idx %u not in s.internals",
                    static_cast<unsigned>(s.top_internal_idx));
            }
            auto& top = it->second;
            if (top.built_internals.size() == 1) {
                s.result.root = std::move(top.built_internals[0]);
                return;
            }
            // Split at root → new layer.
            std::vector<child_ref> new_children;
            std::vector<std::string> new_separators;
            new_children.reserve(top.built_internals.size());
            for (auto& n : top.built_internals) {
                new_children.push_back(child_ref{ .target = std::move(n) });
            }
            top.built_internals.clear();
            for (auto& sep : top.sibling_separators) {
                new_separators.push_back(std::move(sep));
            }
            top.sibling_separators.clear();

            absl::InlinedVector<std::string, 1> wrap_sib_seps;
            auto wrap_pages = build_internal_pages(
                std::move(new_children),
                std::move(new_separators),
                paddr{0, 0},
                /*is_new_layer=*/true,
                s.page_size,
                wrap_sib_seps);
            if (wrap_pages.size() != 1) {
                core::panic_inconsistency(
                    "finalize_root",
                    "wrap layer overflowed for root split "
                    "(wrap_pages=%zu)", wrap_pages.size());
            }
            s.result.root = std::move(wrap_pages[0]);
        }

    }  // namespace _wb

    // ── public re-exports for testing / external use ─────────────

    using _wb::merge_and_build_leaf_impl;
    using _wb::do_pairwise_leaf_merge_impl;

    // ── per-round driver ──
    //
    // Worker advance() calls this once per round, supplying the
    // worker_state and the read-domain-local cache. Returns
    // `flush_round_done` when the worker_tree_proposal is fully
    // built (extracted from `s.result`), or `flush_round_need_read`
    // with a list of NVMe reads to dispatch.
    //
    // The returned `flush_round_need_read.read_descs` references
    // buffers stored in `s.leaves[*].read_buf` / `s.internals[*].read_buf`
    // — those buffers are owned by the worker_state and remain
    // valid until the next round (when promotion clears them).

    template <core::cache_concept Cache>
    inline flush_round_decision
    process_flush_round(worker_state& s, Cache* cache)
    {
        // ── 1. one-shot init ──
        if (!s.initialized) {
            if (s.all_done) {
                // Empty key_groups path: nothing to do.
                return flush_round_done{};
            }
            auto st = _wb::initialize_worker(s);
            if (st != flush_stage_status::ok) {
                s.result.st = st;
                s.all_done  = true;
                return flush_round_done{};
            }
            s.initialized = true;
        }

        // ── 2. leaf reads + merge ──
        if (!s.all_leaves_merged) {
            // Promote inflight reads into "loaded".
            for (auto& lw : s.leaves) {
                if (lw.read_inflight) {
                    lw.page_loaded   = true;
                    lw.read_inflight = false;
                }
            }

            // Try cache hits for not-yet-loaded leaves (first round
            // only, but harmless to retry — pin returns null on miss).
            for (auto& lw : s.leaves) {
                if (lw.page_loaded) continue;
                if (!cache) continue;
                auto pin = cache->pin(make_tree_frame_id(
                    lw.old_slot_paddr, s.page_lbas));
                if (pin.frame) lw.page_loaded = true;
            }

            // Merge any leaves whose page is now available.
            for (auto& lw : s.leaves) {
                if (!lw.page_loaded || lw.merged) continue;

                memory::frame_pin pin{nullptr};
                const char* page_data = nullptr;
                if (cache) {
                    pin = cache->pin(make_tree_frame_id(
                        lw.old_slot_paddr, s.page_lbas));
                    if (pin.frame) page_data = pin.frame->buf;
                }
                if (!page_data && lw.read_buf) {
                    page_data = lw.read_buf.get();
                }
                if (!page_data) {
                    core::panic_inconsistency(
                        "process_flush_round",
                        "leaf page_loaded but no data (rdi=%u "
                        "leaf_idx=%u)",
                        static_cast<unsigned>(s.read_domain_index),
                        static_cast<unsigned>(lw.leaf_idx));
                }

                // Save touched_old_pages BEFORE moving page_data out
                // of cache scope. Leaves are not strictly required by
                // 027 §2.2 (only internals are), but Phase 9 may
                // benefit; we include them for diagnostic uniformity.
                s.result.touched_old_pages[lw.leaf_range_base] =
                    std::vector<char>(page_data, page_data + s.page_size);

                lw.built_leaves = _wb::merge_and_build_leaf_impl(
                    page_data, s.page_size, lw.keys,
                    s.recovery_safe_lsn, lw.leaf_range_base,
                    lw.sibling_separators,
                    s.result.retired_old_values);
                lw.merged = true;
                lw.read_buf.reset();
            }

            // Done?
            s.all_leaves_merged = true;
            for (const auto& lw : s.leaves) {
                if (!lw.merged) { s.all_leaves_merged = false; break; }
            }

            if (!s.all_leaves_merged) {
                flush_round_need_read need;
                for (auto& lw : s.leaves) {
                    if (lw.page_loaded || lw.read_inflight) continue;
                    lw.read_buf = std::make_unique<char[]>(s.page_size);
                    need.read_descs.push_back(format::read_desc{
                        .lba      = lw.old_slot_paddr.lba,
                        .buf      = lw.read_buf.get(),
                        .num_lbas = s.page_lbas,
                    });
                    lw.read_inflight = true;
                }
                if (need.read_descs.empty()) {
                    core::panic_inconsistency(
                        "process_flush_round",
                        "stuck: no merged leaves, no in-flight reads, "
                        "no leaves to dispatch (rdi=%u)",
                        static_cast<unsigned>(s.read_domain_index));
                }
                return need;
            }
        }

        // ── 3. pairwise leaf merge (one-shot) ──
        if (!s.pairwise_done) {
            _wb::do_pairwise_leaf_merge_impl(s);
            s.pairwise_done = true;
        }

        // ── 4. cascade init ──
        if (!s.cascade_initialized) {
            _wb::initialize_cascade(s);
            s.cascade_initialized = true;
        }

        // ── 5. internal reads + build (depth-ascending) ──
        if (!s.all_internals_built) {
            // Promote inflight reads.
            for (auto& [_, iw] : s.internals) {
                if (iw.read_inflight) {
                    iw.page_loaded   = true;
                    iw.read_inflight = false;
                }
            }

            // Cache probe for unloaded internals.
            for (auto& [_, iw] : s.internals) {
                if (iw.page_loaded) continue;
                if (!cache) continue;
                auto pin = cache->pin(make_tree_frame_id(
                    iw.old_slot_paddr, s.page_lbas));
                if (pin.frame) iw.page_loaded = true;
            }

            // Sort by depth ascending, build any whose page is loaded
            // AND whose affected-child internals are already built
            // (P0 gating fix). Ascending depth alone is not enough: an
            // ancestor's page may be cache-hit in the same round its
            // descendant is still awaiting an NVMe read, so the
            // ancestor must not attempt to resolve a not-yet-built
            // child in `build_one_internal()` (which would panic).
            std::vector<core::internal_idx> sorted;
            sorted.reserve(s.internals.size());
            for (auto& [idx, _] : s.internals) sorted.push_back(idx);
            std::sort(sorted.begin(), sorted.end(),
                      [&s](core::internal_idx a, core::internal_idx b) {
                          return s.internals[a].depth < s.internals[b].depth;
                      });

            auto all_children_built = [&s](const worker_state::internal_work& iw) {
                for (auto cidx : iw.affected_child_internals) {
                    auto cit = s.internals.find(cidx);
                    if (cit == s.internals.end()) {
                        core::panic_inconsistency(
                            "process_flush_round",
                            "affected_child_internals references missing "
                            "internal_idx %u",
                            static_cast<unsigned>(cidx));
                    }
                    if (!cit->second.built) return false;
                }
                return true;
            };

            for (auto idx : sorted) {
                auto& iw = s.internals[idx];
                if (iw.built || !iw.page_loaded) continue;
                if (!all_children_built(iw)) continue;

                memory::frame_pin pin{nullptr};
                const char* page_data = nullptr;
                if (cache) {
                    pin = cache->pin(make_tree_frame_id(
                        iw.old_slot_paddr, s.page_lbas));
                    if (pin.frame) page_data = pin.frame->buf;
                }
                if (!page_data && iw.read_buf) page_data = iw.read_buf.get();
                if (!page_data) {
                    core::panic_inconsistency(
                        "process_flush_round",
                        "internal page_loaded but no data (idx=%u)",
                        static_cast<unsigned>(idx));
                }
                s.result.touched_old_pages[iw.range_base] =
                    std::vector<char>(page_data, page_data + s.page_size);

                _wb::build_one_internal(s, iw, page_data);
                iw.built = true;
                iw.read_buf.reset();
            }

            // Done?
            s.all_internals_built = true;
            for (const auto& [_, iw] : s.internals) {
                if (!iw.built) { s.all_internals_built = false; break; }
            }

            if (!s.all_internals_built) {
                flush_round_need_read need;
                for (auto& [_, iw] : s.internals) {
                    if (iw.page_loaded || iw.read_inflight) continue;
                    iw.read_buf = std::make_unique<char[]>(s.page_size);
                    need.read_descs.push_back(format::read_desc{
                        .lba      = iw.old_slot_paddr.lba,
                        .buf      = iw.read_buf.get(),
                        .num_lbas = s.page_lbas,
                    });
                    iw.read_inflight = true;
                }
                if (need.read_descs.empty()) {
                    core::panic_inconsistency(
                        "process_flush_round",
                        "stuck: no built internals, no in-flight reads "
                        "(rdi=%u)",
                        static_cast<unsigned>(s.read_domain_index));
                }
                return need;
            }
        }

        // ── 6. extract root ──
        _wb::finalize_root(s);
        s.all_done = true;
        return flush_round_done{};
    }

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_CANDIDATE_BUILD_HH
