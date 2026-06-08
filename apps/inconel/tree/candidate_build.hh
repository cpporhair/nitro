#ifndef APPS_INCONEL_TREE_CANDIDATE_BUILD_HH
#define APPS_INCONEL_TREE_CANDIDATE_BUILD_HH

// ── candidate_build.hh ── leaf-only worker builder ──────────────
//
// INC-046 shrinks the worker to leaf-only responsibilities:
//
//   1. map sorted flush winners to touched base leaves
//   2. read old leaf pages from the local read-domain cache / NVMe
//   3. merge + tombstone-compact + format final leaf page images
//   4. return a `worker_leaf_chain` with zero-extra-copy page bodies
//
// The worker does not read or build non-leaf pages. Owner side owns
// non-leaf cache, reverse, placement, manifest rebuild, and writes.
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
#include "../format/tree_page.hh"
#include "../format/types.hh"
#include "../memory/dma_page_pool.hh"
#include "../memory/frame.hh"
#include "./flush_types.hh"
#include "./lookup_scheduler.hh"
#include "./page_builder.hh"
#include "./page_reader.hh"

namespace apps::inconel::tree {

    // ── leaf_work / worker_state ─────────────────────────────────

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
            core::internal_idx  parent_idx;

            // Subspan of `worker_state.key_groups` for this leaf's
            // memtable winners.
            std::span<const flush_key_group> keys;

            // NVMe read landing frame; may stay empty if cache hit.
            memory::pooled_frame_ptr<memory::segmented_tree_frame> read_frame;
            bool page_loaded   = false;
            bool read_inflight = false;
            bool merged        = false;

            // Bootstrap path parks fresh pages here until
            // `process_flush_round()` converts them into the final
            // leaf carrier. Non-bootstrap rounds build the carrier
            // directly after each old leaf read/merge.
            absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1> built_leaves;
        };
        std::vector<leaf_work> leaves;

        // ── output ──
        worker_leaf_chain    leaf_result;
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

        s.leaf_result.round_id          = req.round_id;
        s.leaf_result.read_domain_index = req.read_domain_index;
        s.leaf_result.st                = flush_stage_status::ok;

        // Empty partition should not happen, but keep the worker
        // trivially done instead of building a degenerate carrier.
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
        // subsequent split siblings carry empty `replaces_old_paddrs`.
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
            absl::InlinedVector<core::retired_value_ref, 64>& retired_out)
        {
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

        // ── bootstrap leaf build (empty base_manifest) ──
        //
        // Chunks a contiguous sorted run of memtable winners into one
        // or more fresh leaf pages with `replaces_old_paddrs` empty
        // (so the owner allocates fresh ranges for every page). Used
        // by the bootstrap path in `initialize_worker` when
        // `base_manifest->leaf_order.empty()` — there is no old leaf
        // to merge against, so we emit pages directly from the new
        // keys.
        //
        // Tombstone GC is still applied (data_ver <= recovery_safe_lsn
        // → drop) so the first flush does not write dead tombstones.
        //
        // Returns at least one leaf page. Empty `keys` (after
        // tombstone GC) still yields a single empty leaf page so the
        // canonical owner path can materialize an explicit empty
        // tree without relying on a separate prune/collapse pass.
        inline absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1>
        build_leaves_from_sorted_keys_impl(
            uint32_t                                 page_size,
            std::span<const flush_key_group>         keys,
            uint64_t                                 recovery_safe_lsn)
        {
            struct merged_rec {
                std::string_view              key;
                uint64_t                      data_ver;
                core::memtable_entry::kind    kind;
                format::value_ref             vr;  // valid iff kind==value
            };

            std::vector<merged_rec> merged;
            merged.reserve(keys.size());
            for (const auto& kg : keys) {
                if (kg.winner_kind == core::memtable_entry::kind::tombstone
                    && kg.winner_data_ver <= recovery_safe_lsn) {
                    continue;
                }
                merged.push_back({
                    .key      = kg.key,
                    .data_ver = kg.winner_data_ver,
                    .kind     = kg.winner_kind,
                    .vr       = (kg.winner_kind == core::memtable_entry::kind::value)
                                    ? kg.winner_value.durable
                                    : format::value_ref{},
                });
            }

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
                            "build_leaves_from_sorted_keys",
                            "record did not fit despite chunk pre-sizing "
                            "(idx=%zu)",
                            i);
                    }
                }
                builder.finalize();
                return node;
            };

            auto rec_size = [](const merged_rec& m) -> uint32_t {
                uint16_t key_len = static_cast<uint16_t>(m.key.size());
                if (m.kind == core::memtable_entry::kind::value) {
                    return sizeof(format::leaf_record_header) + key_len
                         + sizeof(format::value_ref);
                }
                return sizeof(format::leaf_record_header) + key_len;
            };

            absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1> result;
            std::size_t chunk_start = 0;
            std::size_t i           = 0;
            uint32_t    used        = sizeof(format::tree_slot_header);
            uint16_t    chunk_count = 0;
            while (i < merged.size()) {
                uint32_t add = rec_size(merged[i]);
                uint32_t dir_after = sizeof(uint16_t)
                                   * static_cast<uint32_t>(chunk_count + 1u);
                if (used + add + dir_after > page_size) {
                    if (chunk_count == 0) {
                        core::panic_inconsistency(
                            "build_leaves_from_sorted_keys",
                            "single record larger than page "
                            "(rec_size=%u page_size=%u key_len=%zu)",
                            add, page_size, merged[i].key.size());
                    }
                    auto node = emit_chunk_to_node(chunk_start, i);
                    result.push_back(std::move(node));
                    chunk_start = i;
                    used        = sizeof(format::tree_slot_header);
                    chunk_count = 0;
                    continue;
                }
                used += add;
                chunk_count++;
                ++i;
            }
            if (chunk_count > 0) {
                auto node = emit_chunk_to_node(chunk_start, merged.size());
                result.push_back(std::move(node));
            }

            if (result.empty()) {
                // No live records (all tombstones dropped). Emit a
                // single empty leaf so the owner can keep an empty
                // bootstrap root if needed.
                auto node = std::make_unique<mem_tree_node>();
                node->type = format::node_type::leaf;
                node->content.resize(page_size);
                leaf_page_builder builder;
                builder.init(node->content.data(), page_size);
                builder.finalize();
                result.push_back(std::move(node));
            }

            return result;
        }

        // ── bootstrap init: build fresh leaves directly from keys ──
        //
        // Bootstrap has no old leaf to read, so the worker emits
        // fresh leaf pages directly from the sorted winner stream and
        // parks them on a single synthetic `leaf_work`.
        inline flush_stage_status
        initialize_worker_bootstrap(worker_state& s)
        {
            auto pages = _wb::build_leaves_from_sorted_keys_impl(
                s.page_size, s.key_groups, s.recovery_safe_lsn);

            worker_state::leaf_work lw;
            lw.leaf_idx          = 0;
            lw.leaf_range_base   = paddr{0, 0};
            lw.old_slot_paddr    = paddr{0, 0};
            lw.parent_idx        = core::kInvalidInternalIdx;
            lw.keys              = s.key_groups;
            lw.page_loaded       = true;
            lw.merged            = true;
            lw.built_leaves      = std::move(pages);
            s.leaves.push_back(std::move(lw));

            return flush_stage_status::ok;
        }

        // ── init: classify keys by touched leaf ──────────────────

        inline flush_stage_status
        initialize_worker(worker_state& s)
        {
            const auto& lo   = s.base_manifest->leaf_order;
            const auto& topo = s.base_manifest->reverse_topology;

            if (lo.empty()) {
                // Empty base_manifest — route to the bootstrap path.
                // The caller in `process_flush_round` selects between
                // `initialize_worker_bootstrap` and `initialize_worker`
                // based on `lo.empty()`, so reaching this branch would
                // indicate the dispatch forgot to special-case
                // bootstrap. Panic rather than silently returning the
                // old unsupported status (constraint A: shape-specific
                // implementations must declare their limitation in the
                // callsite, not in a fallback).
                core::panic_inconsistency(
                    "initialize_worker",
                    "empty base_manifest reached non-bootstrap init — "
                    "caller must dispatch to initialize_worker_bootstrap");
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

            return flush_stage_status::ok;
        }

    }  // namespace _wb

    // ── public re-exports for testing / external use ─────────────

    using _wb::merge_and_build_leaf_impl;

    inline std::span<const char>
    frame_bytes_for_page(const memory::segmented_page_frame& frame,
                         uint32_t page_size,
                         std::vector<char>& scratch) {
        if (page_size <= frame.lba_size()) {
            return frame.contiguous_bytes(0, page_size);
        }
        scratch.resize(page_size);
        frame.copy_to_contiguous(scratch.data(), page_size);
        return std::span<const char>(scratch.data(), scratch.size());
    }

    inline leaf_page_image
    make_leaf_page_image(std::unique_ptr<mem_tree_node> node,
                         uint32_t                       page_size)
    {
        if (node == nullptr) {
            core::panic_inconsistency(
                "make_leaf_page_image",
                "mem_tree_node is null");
        }
        if (node->type != format::node_type::leaf) {
            core::panic_inconsistency(
                "make_leaf_page_image",
                "expected leaf node, got type=%u",
                static_cast<unsigned>(node->type));
        }

        leaf_page_reader reader;
        if (!reader.parse(node->content.data(), page_size)) {
            core::panic_inconsistency(
                "make_leaf_page_image",
                "formatted leaf page failed validation");
        }

        uint16_t first_key_off = 0;
        uint16_t first_key_len = 0;
        if (reader.record_count() != 0) {
            const auto* hdr =
                reinterpret_cast<const format::tree_slot_header*>(
                    node->content.data());
            const auto rec_off = format::load_tree_slot_offset(hdr, 0);
            const auto rec = reader.get(0);
            first_key_off = static_cast<uint16_t>(
                rec_off + sizeof(format::leaf_record_header));
            first_key_len = static_cast<uint16_t>(rec.key.size());
        }

        leaf_page_image out;
        out.first_key_off = first_key_off;
        out.first_key_len = first_key_len;
        out.page = std::move(node->content);
        return out;
    }

    inline leaf_chain_item
    make_leaf_chain_item(uint32_t leaf_idx,
                         paddr old_range_base,
                         core::internal_idx parent_idx,
                         absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1>&& pages,
                         absl::InlinedVector<core::retired_value_ref, 64>&& retired_old_values,
                         uint32_t page_size)
    {
        leaf_chain_item item;
        item.old_leaf_idx    = leaf_idx;
        item.old_range_base  = old_range_base;
        item.parent_idx      = parent_idx;
        item.shape = (pages.size() <= 1)
            ? leaf_chain_shape::rewrite
            : leaf_chain_shape::split;
        item.retired_old_values = std::move(retired_old_values);
        item.new_pages.reserve(pages.size());
        for (auto& page : pages) {
            item.new_pages.push_back(
                make_leaf_page_image(std::move(page), page_size));
        }
        return item;
    }

    // ── per-round driver ──
    //
    // Worker advance() calls this once per round, supplying the
    // worker_state and the read-domain-local cache. Returns
    // `flush_round_done` once the leaf chain is complete, or
    // `flush_round_need_read` with a batch of old leaf frame reads to
    // dispatch. The returned frame descriptors point at frames owned by
    // `worker_state.leaves[*].read_frame`.

    template <core::cache_concept Cache>
    inline flush_round_decision
    process_flush_round(worker_state& s,
                        Cache* cache,
                        memory::lba_dma_page_pool* frame_pool)
    {
        if (s.all_done) {
            return flush_round_done{};
        }

        if (!s.initialized) {
            if (s.all_done) {
                return flush_round_done{};
            }
            auto st = s.base_manifest->leaf_order.empty()
                          ? _wb::initialize_worker_bootstrap(s)
                          : _wb::initialize_worker(s);
            if (st != flush_stage_status::ok) {
                s.leaf_result.st = st;
                s.all_done       = true;
                return flush_round_done{};
            }
            s.initialized = true;
        }

        if (s.base_manifest->leaf_order.empty()) {
            if (s.leaves.size() != 1) {
                core::panic_inconsistency(
                    "process_flush_round",
                    "bootstrap worker expected exactly one synthetic leaf_work "
                    "(count=%zu)",
                    s.leaves.size());
            }

            auto& lw = s.leaves.front();
            absl::InlinedVector<core::retired_value_ref, 64> retired_old_values;
            s.leaf_result.items.push_back(make_leaf_chain_item(
                UINT32_MAX,
                paddr{0, 0},
                core::kInvalidInternalIdx,
                std::move(lw.built_leaves),
                std::move(retired_old_values),
                s.page_size));
            s.all_done = true;
            return flush_round_done{};
        }

        for (auto& lw : s.leaves) {
            if (lw.read_inflight) {
                lw.page_loaded   = true;
                lw.read_inflight = false;
            }
        }

        for (auto& lw : s.leaves) {
            if (lw.page_loaded || !cache) continue;
            auto pin = cache->pin(make_tree_frame_id(
                lw.old_slot_paddr, s.page_lbas));
            if (pin.frame) lw.page_loaded = true;
        }

        for (auto& lw : s.leaves) {
            if (lw.merged || !lw.page_loaded) continue;

            typename Cache::pin_type pin{nullptr};
            const memory::segmented_page_frame* page_frame = nullptr;
            if (cache) {
                pin = cache->pin(make_tree_frame_id(
                    lw.old_slot_paddr, s.page_lbas));
                if (pin.frame) page_frame = pin.frame;
            }
            if (page_frame == nullptr && lw.read_frame) {
                page_frame = lw.read_frame.get();
            }
            if (page_frame == nullptr) {
                core::panic_inconsistency(
                    "process_flush_round",
                    "leaf page_loaded but no page bytes "
                    "(rdi=%u leaf_idx=%u)",
                    static_cast<unsigned>(s.read_domain_index),
                    static_cast<unsigned>(lw.leaf_idx));
            }
            std::vector<char> scratch;
            auto image = frame_bytes_for_page(
                *page_frame, s.page_size, scratch);

            absl::InlinedVector<core::retired_value_ref, 64> retired_old_values;
            auto built_pages = _wb::merge_and_build_leaf_impl(
                image.data(),
                s.page_size,
                lw.keys,
                s.recovery_safe_lsn,
                lw.leaf_range_base,
                retired_old_values);

            s.leaf_result.items.push_back(make_leaf_chain_item(
                lw.leaf_idx,
                lw.leaf_range_base,
                lw.parent_idx,
                std::move(built_pages),
                std::move(retired_old_values),
                s.page_size));

            lw.merged = true;
            lw.read_frame.reset();
        }

        bool all_leaves_merged = true;
        for (const auto& lw : s.leaves) {
            if (!lw.merged) {
                all_leaves_merged = false;
                break;
            }
        }

        if (!all_leaves_merged) {
            flush_round_need_read need;
            for (auto& lw : s.leaves) {
                if (lw.page_loaded || lw.read_inflight) continue;
                if (frame_pool == nullptr) {
                    core::panic_inconsistency(
                        "process_flush_round",
                        "frame_pool is null for NVMe leaf read");
                }
                auto frame = frame_pool
                    ->get_typed_frame<memory::segmented_tree_frame>(
                        make_tree_frame_id(lw.old_slot_paddr, s.page_lbas),
                        memory::frame_state::clean_readonly,
                        /*zero_fill=*/false);
                if (!frame) {
                    core::panic_inconsistency(
                        "process_flush_round",
                        "failed to allocate DMA frame for old leaf "
                        "(rdi=%u leaf_idx=%u)",
                        static_cast<unsigned>(s.read_domain_index),
                        static_cast<unsigned>(lw.leaf_idx));
                }
                lw.read_frame =
                    memory::pooled_frame_ptr<memory::segmented_tree_frame>(
                        frame_pool,
                        new memory::segmented_tree_frame(std::move(*frame)));
                need.reads.push_back(memory::frame_read_desc{
                    .frame = lw.read_frame.get(),
                });
                lw.read_inflight = true;
            }
            if (need.reads.empty()) {
                core::panic_inconsistency(
                    "process_flush_round",
                    "leaf worker made no progress and staged no reads "
                    "(rdi=%u)",
                    static_cast<unsigned>(s.read_domain_index));
            }
            return need;
        }

        std::sort(
            s.leaf_result.items.begin(),
            s.leaf_result.items.end(),
            [](const leaf_chain_item& a, const leaf_chain_item& b) {
                return a.old_leaf_idx < b.old_leaf_idx;
            });

        s.all_done = true;
        return flush_round_done{};
    }

}  // namespace apps::inconel::tree

#endif  // APPS_INCONEL_TREE_CANDIDATE_BUILD_HH
