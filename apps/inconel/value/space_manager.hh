#ifndef APPS_INCONEL_VALUE_SPACE_MANAGER_HH
#define APPS_INCONEL_VALUE_SPACE_MANAGER_HH

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <absl/container/btree_map.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>

#include "../core/data_area_heads.hh"
#include "../format/types.hh"

// ─────────────────────────────────────────────────────────────────────────────
// value_space_manager — INC-051 standalone metadata component.
//
// 037 ai_context plan §"独立类边界": this is the single source of truth for
// value-area free-space (LBA + sub-LBA), partial-page allocator metadata, and
// cached-partial admission/selection. It does NOT own DMA frames, does NOT
// submit NVMe I/O, does NOT persist its own metadata, and is NOT a PUMP
// sender — it is a synchronous metadata API consumed by the value owner
// scheduler (`apps/inconel/value/scheduler.hh::value_alloc_sched`).
//
// Ownership / threading:
//   - The manager runs single-thread inside the value owner. Round operations,
//     claim / release, cached_partial mutations, allocate_batch, recovery
//     install — all owner-local.
//   - The only cross-core surface is the alloc-floor reservation fence:
//     `published_alloc_floor_lba` per the plan doc §"地址空间". The carrier
//     of that atomic is `core::data_area_heads.tree_head_lba`, shared with
//     the tree allocator; the manager only reads it through
//     `effective_alloc_floor_for_owner_()`.
//
// Hard invariants (must be preserved by every code path here):
//   - quantums_per_lba ≤ 64 (free_quantum_bits is uint64_t per 037 §"Index
//     Layout"; ctor fail-fasts if violated).
//   - global_free_extents is the *only* whole-free truth; group_state never
//     duplicates that information.
//   - sparse_partial_repr maintains both an address-keyed truth (by_page_delta
//     → node_id) AND a derived selection index (pages_by_largest_run buckets);
//     they must stay in sync after every claim/release/admission event.
// ─────────────────────────────────────────────────────────────────────────────

namespace apps::inconel::value {

    using format::paddr;
    using format::value_ref;

    // ── Public POD types ────────────────────────────────────────────────────
    //
    // These cross the manager's public surface. Layout is plain C++ (no packed
    // attribute) because none of them is persisted or transmitted on the wire.

    struct space_class {
        uint16_t class_idx;
        uint32_t class_size;     // bytes per slot (header + body if applicable)
        uint32_t span_lbas;      // 1 for sub-LBA, ≥1 for LBA-equal / multi-LBA
        uint16_t alloc_quantums; // class_size / value_space_quantum_bytes (sub-LBA only)
    };

    struct allocation_request {
        uint32_t entry_index;    // caller's batch entry index, preserved for ordering
        uint16_t class_idx;
        uint32_t alloc_bytes;
        uint32_t alloc_quantums; // = alloc_bytes / value_space_quantum_bytes
    };

    enum class cached_partial_kind : uint8_t {
        active_tail,
        cached_free_candidate,
    };

    struct cached_partial_update {
        paddr               page_base;
        cached_partial_kind kind;
        uint64_t            heat_seq;     // larger means newer / hotter
        uint64_t            cache_epoch;  // residency token from cache layer
    };

    enum class byte_claim_source : uint8_t {
        cached_partial,
        new_whole_page,
        nonresident_partial,
    };

    struct byte_claim {
        paddr             page_base;
        uint32_t          byte_offset;
        uint32_t          alloc_bytes;
        uint16_t          class_idx;
        uint64_t          cache_epoch; // valid only when src == cached_partial
        byte_claim_source src;
    };

    struct trim_range {
        uint64_t lba;
        uint32_t len_lbas;
    };

    struct trim_plan_id {
        // Real plan ids are issued from `next_trim_plan_seq_` which starts
        // at 1, so raw == 0 is reserved as the "idle / not registered"
        // sentinel: prepare_trim returns it when no eligible extent exists
        // and the caller must NOT pass it to complete_trim. complete_trim
        // is defensive (silently no-ops on unknown id), but treating 0 as
        // a valid handle would mask future bookkeeping bugs.
        uint64_t raw;

        bool operator==(const trim_plan_id&) const noexcept = default;
    };

    struct trim_plan {
        trim_plan_id            id;     // raw == 0 ↔ ranges empty (idle)
        std::vector<trim_range> ranges;
    };

    struct free_extent {
        uint64_t base_lba;
        uint32_t len_lbas;
    };

    struct alloc_floor_sync_status {
        enum class code : uint8_t {
            accepted,
            rejected_collision,
        } st;
    };

    struct alloc_floor_sync_result {
        alloc_floor_sync_status::code st;
        uint64_t                      acknowledged_floor_lba;
    };

    // ── Recovery inputs ─────────────────────────────────────────────────────
    //
    // 037 plan §"Recovery": tree/WAL produces live `value_ref { base,
    // byte_offset, len }`. The manager rebuilds free space as the complement
    // of those occupied byte ranges; it never reads Value Area payload pages.

    struct live_value_extent {
        paddr    base;
        uint16_t byte_offset;
        uint32_t len;
    };

    // dead_class_hint is used only as a future-friendly hint surface (TRIM
    // ordering / class bucketing). 037 plan §"Recovery" rule 2 explicitly
    // states that losing hints must NOT cause free-space leaks, so this struct
    // can stay minimal; the recovery rebuild never depends on it for
    // correctness.

    struct dead_class_hint {
        paddr    page_base;
        uint16_t class_idx;
    };

    // ── Allocation policy ───────────────────────────────────────────────────
    //
    // 037 plan §"Allocation Mode Selection" computes effective_mode from
    // (space_mode, partial_mode). `allocation_policy` is the caller's
    // per-batch knob: defaults to automatic so the manager picks the mode
    // itself; force_* lets maintenance / recovery callers pin a specific
    // strategy.

    enum class allocation_mode_override : uint8_t {
        automatic,
        force_normal,
        force_reuse_pressure,
        force_hard_pressure,
    };

    struct allocation_policy {
        allocation_mode_override mode = allocation_mode_override::automatic;
    };

    // ── Manager configuration ───────────────────────────────────────────────
    //
    // Plain inputs — once superblock / format_profile gain the quantum and
    // group-size fields (later step), the runtime builder copies them into
    // this struct before constructing the manager.

    struct value_space_manager_config {
        uint32_t lba_size                     = 0;
        uint32_t value_space_quantum_bytes    = 0;
        uint32_t value_space_group_size_lbas  = 0;

        // The value-area sits on a single device; manager echoes this id
        // into the byte_claim page_base it returns.
        uint16_t device_id          = 0;
        uint64_t data_area_base_lba = 0;
        uint64_t data_area_end_lba  = 0;

        // Class table — strictly ascending, deduped, every entry satisfies the
        // 64B·2^n / lba_size·2^m shape rules. Borrowed view; the underlying
        // storage must outlive the manager.
        std::span<const uint32_t> value_class_sizes;

        // Object header bytes added to `value_ref.len` when computing the
        // canonical class. Set to `sizeof(value_object_header)` when the value
        // payload uses the existing format::value_object encoding; 0 otherwise.
        uint32_t object_header_bytes = 0;

        // Cross-core reservation fence carrier. May be nullptr in unit
        // contexts where no tree allocator runs alongside; in that case the
        // effective floor degrades to acknowledged_alloc_floor_lba_.
        core::data_area_heads* shared_heads = nullptr;

        // Initial owner-local acknowledged floor. Default 0 means "use
        // data_area_base_lba"; explicit non-zero values let recovery /
        // bring-up start with a higher floor when tree has already claimed
        // some prefix.
        uint64_t initial_acknowledged_alloc_floor_lba = 0;

        // ── Pressure mode watermarks (basis points: 100 bp = 1 %) ──
        // Defaults from 037 plan §"Allocation Mode Selection".
        uint16_t normal_low_watermark_bp  = 1500; // 15 %
        uint16_t normal_high_watermark_bp = 2000; // 20 %
        uint16_t hard_low_watermark_bp    =  300; //  3 %
        uint16_t hard_high_watermark_bp   =  500; //  5 %

        // ── Partial metadata budgets (037 §"Partial Metadata Budget") ──
        uint64_t partial_metadata_soft_limit_pages = 10'000'000ULL;
        uint64_t partial_metadata_hard_limit_pages = 100'000'000ULL;

        // ── Read budgets / candidate caps (037 §"Allocation Mode Selection",
        // §"Cached Partial Selection") ──
        uint32_t reuse_pressure_max_prefill_reads_per_batch = 16;
        uint32_t reuse_pressure_max_prefill_reads_per_class = 4;
        uint32_t hard_pressure_max_prefill_reads_per_batch  = 128;
        uint32_t hard_pressure_max_prefill_reads_per_class  = 32;
        uint32_t max_candidate_pages                        = 64;
        uint32_t active_tail_pages_cap                      = 8;

        // ── Cached partial DMA frame budget (037 §"Cached Partial Budget") ──
        uint64_t value_cached_partial_budget_bytes = 256ULL * 1024 * 1024;
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Bit helpers (037 plan §"Bit Helpers").
    //
    // Operate on a 64-bit free-quantum bitmap where bit i == 1 iff quantum i
    // is free. quantums_per_lba is provided explicitly so the helpers degrade
    // to the correct masks for sub-64 quantum counts (which the format also
    // permits when lba_size / quantum_bytes < 64). All of them avoid UB
    // shifts of 64; callers must respect the documented preconditions.
    // ─────────────────────────────────────────────────────────────────────────

    namespace _vsm_detail {

        // low_bits_mask(n) returns a uint64_t with bits [0, n) set. Defined
        // for n in [0, 64].
        constexpr uint64_t
        low_bits_mask(uint32_t n) noexcept {
            if (n == 0) return 0;
            if (n >= 64) return UINT64_MAX;
            return (1ULL << n) - 1ULL;
        }

        // valid_quantum_mask: bits [0, qpl) set. The free-quantum bitmap
        // never sets bits ≥ qpl; the mask is used to clip the result of
        // shift-and steps.
        constexpr uint64_t
        valid_quantum_mask(uint32_t qpl) noexcept {
            return low_bits_mask(qpl);
        }

        // valid_partial_run_bucket_mask: bits [1, qpl-1] set. Bucket 0 means
        // "no free quantum" (page must not stay in partial_pages); bucket qpl
        // means "all-free" (must convert to whole-free 1-LBA span and leave
        // partial_pages). Only intermediate buckets are admissible.
        constexpr uint64_t
        valid_partial_run_bucket_mask(uint32_t qpl) noexcept {
            return low_bits_mask(qpl) & ~1ULL;
        }

        // quantum_range_mask(pos, len): bits [pos, pos+len) set. Precondition
        // pos + len ≤ 64; len == 64 requires pos == 0.
        constexpr uint64_t
        quantum_range_mask(uint32_t pos, uint32_t len) noexcept {
            assert(len > 0);
            assert(pos + len <= 64);
            if (len == 64) {
                assert(pos == 0);
                return UINT64_MAX;
            }
            return low_bits_mask(len) << pos;
        }

        // run_buckets_ge(need, qpl): mask of bucket indices in [need, qpl-1].
        // Precondition 1 ≤ need ≤ qpl. Used by the global / group bucket
        // selection: AND with nonempty-bucket-mask to find smallest k ≥ need
        // with non-empty bucket.
        constexpr uint64_t
        run_buckets_ge(uint32_t need, uint32_t qpl) noexcept {
            assert(need >= 1);
            assert(need <= qpl);
            return ~low_bits_mask(need) & valid_partial_run_bucket_mask(qpl);
        }

        // run_start_mask(bits, len): for each bit position p, set iff
        // bits[p..p+len) are all 1, AND p is a valid quantum (≤ qpl-len).
        // For len == 1 this equals `bits & valid_quantum_mask(qpl)`. The
        // shift-and tower runs at most ⌈log2(len)⌉ rounds.
        //
        // Note: this function does NOT clip to valid_quantum_mask itself; the
        // caller must AND with `valid_quantum_mask(qpl)` if the input bitmap
        // could have set bits ≥ qpl (it should not, by invariant).
        constexpr uint64_t
        run_start_mask(uint64_t bits, uint32_t len) noexcept {
            assert(len >= 1);
            uint64_t m = bits;
            uint32_t span = 1;
            while (span < len) {
                uint32_t step = std::min(span, len - span);
                m &= (m >> step);
                span += step;
            }
            return m;
        }

        // actual_run_start_mask(bits, qpl): bit positions that are the START
        // of an actual free run (i.e., bit set, AND either at position 0 or
        // preceded by 0 bit). Clipped to valid quantums.
        constexpr uint64_t
        actual_run_start_mask(uint64_t bits, uint32_t qpl) noexcept {
            return bits & ~(bits << 1) & valid_quantum_mask(qpl);
        }

        // exact_run_start_mask(bits, r, qpl): starts of runs whose length is
        // EXACTLY r (≥ r AND NOT ≥ r+1). For r == qpl, ≥ qpl+1 is impossible,
        // so we just return ≥ r (i.e., starts of full-len runs).
        constexpr uint64_t
        exact_run_start_mask(uint64_t bits, uint32_t r, uint32_t qpl) noexcept {
            assert(r >= 1);
            assert(r <= qpl);
            const uint64_t starts = actual_run_start_mask(bits, qpl);
            const uint64_t ge_r = starts & run_start_mask(bits, r);
            if (r == qpl) {
                return ge_r;
            }
            const uint64_t ge_r_plus_1 = run_start_mask(bits, r + 1);
            return ge_r & ~ge_r_plus_1;
        }

        // recompute_largest_run(bits, qpl): largest k in [0, qpl] such that
        // there exists a run of length ≥ k. Uses bounded binary search; for
        // qpl == 64 this is at most 6 probes per the plan doc §"Bit Helpers".
        constexpr uint32_t
        recompute_largest_run(uint64_t bits, uint32_t qpl) noexcept {
            const uint64_t valid_bits = bits & valid_quantum_mask(qpl);
            if (valid_bits == 0) return 0;
            uint32_t lo = 1;
            uint32_t hi = qpl;
            uint32_t best = 0;
            while (lo <= hi) {
                const uint32_t mid = lo + (hi - lo) / 2;
                if (run_start_mask(valid_bits, mid) != 0) {
                    best = mid;
                    lo = mid + 1;
                } else {
                    hi = mid - 1;
                }
            }
            return best;
        }

        // choose_claim_offset(bits, need_quantums, k, qpl): page-local
        // best-fit start position. k is the page's current
        // `largest_free_run_quantums`, used as the upper bound for the
        // best-fit search. Returns the lowest start of the shortest run whose
        // length is ≥ need_quantums.
        constexpr uint32_t
        choose_claim_offset(uint64_t  bits,
                            uint32_t  need_quantums,
                            uint32_t  k,
                            uint32_t  qpl) noexcept {
            assert(need_quantums >= 1);
            assert(need_quantums <= k);
            assert(k <= qpl);
            for (uint32_t r = need_quantums; r <= k; ++r) {
                const uint64_t exact = exact_run_start_mask(bits, r, qpl);
                if (exact != 0) {
                    return static_cast<uint32_t>(std::countr_zero(exact));
                }
            }
            // Unreachable when caller guarantees a run of length ≥ need_quantums
            // exists in the page; the bucket selection upstream guarantees it.
            assert(false && "choose_claim_offset: no fitting run despite k≥need");
            return 0;
        }

        // Sentinel handle for the intrusive doubly-linked lists / arena
        // free chain. UINT32_MAX is reserved across all node_id and group_id
        // bucket links.
        inline constexpr uint32_t kInvalidNodeId  = UINT32_MAX;
        inline constexpr uint32_t kInvalidGroupId = UINT32_MAX;

        // ── partial_page_node ──────────────────────────────────────────────
        //
        // 037 plan §"Index Layout": the per-page truth + intrusive bucket
        // link stored compactly. `free_quantum_bits` is uint64_t, which pins
        // quantums_per_lba ≤ 64 — enforced at manager construction.
        //
        // Field layout (24 bytes excluding any tail padding):
        //   page_delta              4 B
        //   free_quantum_count      2 B
        //   largest_free_run_quantums 2 B
        //   free_quantum_bits       8 B
        //   bucket_prev             4 B
        //   bucket_next             4 B
        //
        // bucket_prev / bucket_next form a doubly-linked list within ONE
        // pages_by_largest_run[k] bucket. They also double as the free-chain
        // link when the node is on the arena's free list (see `node_arena`):
        // node_arena guarantees this overload is never observable outside the
        // arena boundary.

        struct partial_page_node {
            uint32_t page_delta = 0;
            uint16_t free_quantum_count = 0;
            uint16_t largest_free_run_quantums = 0;
            uint64_t free_quantum_bits = 0;
            uint32_t bucket_prev = kInvalidNodeId;
            uint32_t bucket_next = kInvalidNodeId;
        };
        static_assert(sizeof(partial_page_node) == 24,
                      "partial_page_node must stay 24 B per 037 plan budget");

        // ── node_arena ─────────────────────────────────────────────────────
        //
        // Compact arena with O(1) alloc / free that NEVER calls operator new
        // per node. Storage grows monotonically via `nodes_.push_back`; freed
        // slots are recycled via a single-link free chain whose `next` field
        // is overlaid on the released node's `bucket_next`. The overload is
        // permitted because:
        //   1. The free chain is only traversed by the arena itself.
        //   2. External callers never deference a freed node — they only hold
        //      `node_id` handles, and once `free(id)` is called the handle is
        //      invalidated.
        //   3. Debug-mode invariants verify (a) freed nodes have
        //      `bucket_prev == kInvalidNodeId` (caller must scrub before free)
        //      and (b) at(id) is not called after free.
        //
        // Decision rationale: see scope discussion §4 — at 100 M partial
        // pages the in-place free chain saves up to ~400 MiB peak vs a
        // separate free_slot_ids vector, while introducing zero extra
        // pointer-chasing per alloc/free (the chain prefetches the same
        // cache line the caller subsequently writes).

        class node_arena {
        public:
            uint32_t
            alloc() {
                if (free_head_ == kInvalidNodeId) {
                    nodes_.emplace_back();
                    return static_cast<uint32_t>(nodes_.size() - 1);
                }
                const uint32_t id = free_head_;
                // Move free_head_ to the next link. Read the overlaid
                // bucket_next field; this is the ONLY place outside the arena
                // boundary where bucket_next of a freed node is observed.
                free_head_ = nodes_[id].bucket_next;
                // Caller is expected to overwrite all fields before publishing
                // the node; we still reset bucket_prev / bucket_next so that
                // forgetting to do so manifests as a clean "no neighbor" state.
                nodes_[id] = partial_page_node{};
                return id;
            }

            void
            free(uint32_t id) {
                assert(id < nodes_.size());
                // Scrub bucket_prev so the debug invariant in alloc() can
                // catch a free → alloc → forgot-to-init class of bug.
                nodes_[id].bucket_prev = kInvalidNodeId;
                nodes_[id].bucket_next = free_head_;
                free_head_ = id;
            }

            partial_page_node&
            at(uint32_t id) noexcept {
                assert(id < nodes_.size());
                return nodes_[id];
            }

            const partial_page_node&
            at(uint32_t id) const noexcept {
                assert(id < nodes_.size());
                return nodes_[id];
            }

            std::size_t
            capacity() const noexcept {
                return nodes_.size();
            }

        private:
            std::vector<partial_page_node> nodes_;
            uint32_t                       free_head_ = kInvalidNodeId;
        };

        // ── sparse_partial_repr ───────────────────────────────────────────
        //
        // 037 plan §"Sparse-Only Partial Metadata" + §"Continuous Slot
        // Lookup Index". Maintains:
        //   - by_page_delta: address-keyed truth, page_delta → node_id
        //   - nodes:         arena storing partial_page_node
        //   - pages_by_largest_run[k]: head node_id of bucket k (k =
        //     largest_free_run_quantums); bucket-internal links are stored on
        //     the node itself (bucket_prev / bucket_next), so head-only is
        //     sufficient — pop_front via head, mid-list remove via the node's
        //     own prev/next.
        //   - nonempty_run_bucket_mask: bit k iff pages_by_largest_run[k]
        //     non-empty. Lets the global selection AND with run_buckets_ge
        //     to find the smallest non-empty bucket ≥ need.

        struct sparse_partial_repr {
            absl::flat_hash_map<uint32_t, uint32_t> by_page_delta;
            node_arena                              nodes;

            uint32_t  pages_by_largest_run[64]{}; // head node_id; init to 0
            uint64_t  nonempty_run_bucket_mask = 0;

            sparse_partial_repr() noexcept {
                for (uint32_t& head : pages_by_largest_run) {
                    head = kInvalidNodeId;
                }
            }
        };

        // ── group_state ──────────────────────────────────────────────────
        //
        // 037 plan §"Group 分片". Lazily materialized; group directory only
        // creates a group_state when the first partial node lands in it (or
        // when recovery requires it).
        //
        // global_run_links: a single group can simultaneously appear in
        // multiple top-level groups_by_largest_run[k] buckets (one per
        // distinct largest_run value across its partial pages), so we need
        // ONE prev/next pair per bucket. For qpl == 64 this is 64 × 8 B = 512 B
        // per group; at 4096 groups (1 TiB / 256 MiB) that is ~2 MiB total —
        // well within the budget.

        struct group_link {
            uint32_t prev = kInvalidGroupId;
            uint32_t next = kInvalidGroupId;
        };

        struct group_state {
            uint64_t base_lba  = 0;
            uint32_t lba_count = 0;

            sparse_partial_repr partial_pages;
            uint32_t            partial_page_count = 0;

            // Per-bucket prev/next links into the manager-level
            // groups_by_largest_run[k] lists. A group is linked in global
            // bucket k iff `partial_pages.nonempty_run_bucket_mask` bit k is
            // set (i.e. the group has at least one partial page with
            // largest_free_run_quantums == k). We keep one link pair per
            // bucket because a group can be in multiple global buckets
            // simultaneously.
            group_link global_run_links[64]{};
        };

    } // namespace _vsm_detail

    // ── Round-scoped state ──────────────────────────────────────────────────
    //
    // 037 plan §"Reservation / Rollback". Every value-write round acquires a
    // value_space_round handle. Claims against EXISTING partial pages mutate
    // metadata immediately so concurrent later rounds see the reservation;
    // fresh whole extents are pulled out of global_free_extents wholesale and
    // published as partial pages only on commit. abort releases just this
    // round's deltas — never restores a touched page to its pre-round
    // snapshot, since later rounds may have claimed against the same page
    // already.

    struct reserved_extent {
        uint64_t base_lba;
        uint32_t len_lbas;
    };

    struct pending_tail_page {
        paddr    page_base;
        uint64_t free_quantum_bits;
        uint16_t free_quantum_count;
        uint16_t largest_free_run_quantums;
    };

    class value_space_round {
    public:
        std::vector<byte_claim>        claims;
        std::vector<reserved_extent>   fresh_extents;
        std::vector<pending_tail_page> unpublished_tails;
    };

    // ─────────────────────────────────────────────────────────────────────────
    // value_space_manager
    // ─────────────────────────────────────────────────────────────────────────

    class value_space_manager {
    public:
        explicit value_space_manager(value_space_manager_config cfg);

        // Owner-local reservation reconcile: tree owner publishes a new floor
        // request via store-release on data_area_heads.tree_head_lba, then
        // posts sync_alloc_floor() to the value owner. Manager checks that
        // [old_acked_floor, new_alloc_floor_lba) does not contain anything
        // we still hold (live values, partial pages, dirty/open frames,
        // trim_inflight, fresh_extents already claimed by an inflight round)
        // and either accepts (advancing acknowledged_alloc_floor_lba_) or
        // rejects with rejected_collision.
        //
        // tree_returned_extents: when tree head moves DOWN and hands whole
        // extents back to value, the caller must list them here. Each must
        // satisfy `base_lba >= new_alloc_floor_lba`. They are coalesced into
        // global_free_extents only on accept.
        alloc_floor_sync_result
        sync_alloc_floor(uint64_t                       new_alloc_floor_lba,
                         std::span<const free_extent>  tree_returned_extents);

        value_space_round
        begin_round();

        // Caller-targeted claim against a known partial page (used by the
        // cached-partial fast path and by stale-miss retry). Returns a
        // byte_claim with src == cached_partial; failure cases (page not
        // currently partial, no fitting run) are signalled by an empty
        // optional so the caller can re-plan.
        std::optional<byte_claim>
        try_claim_range(value_space_round& round,
                        paddr              page_base,
                        uint16_t           class_idx,
                        uint32_t           alloc_quantums,
                        uint32_t           alloc_bytes,
                        uint64_t           cache_epoch);

        void
        mark_cached_partial(const cached_partial_update& update);

        void
        erase_cached_partial(paddr page_base, uint64_t cache_epoch);

        // Returns a vector of byte_claims whose order matches `reqs`
        // (allocate_batch may internally reorder by size for BFD packing,
        // but the returned vector restores caller order — claims[i] ↔
        // reqs[i].entry_index).
        //
        // On space exhaustion the returned vector is empty and the round
        // stays clean (no fresh extents, no unpublished tails); caller can
        // back-pressure / retry.
        std::vector<byte_claim>
        allocate_batch(value_space_round&                  round,
                       std::span<const allocation_request> reqs,
                       allocation_policy                   policy = {});

        void
        commit(value_space_round&& round);

        void
        abort(value_space_round&& round);

        // Caller asserts every value_ref is dead w.r.t. tree + outstanding
        // read handles. Manager does not perform liveness defence (037 plan
        // §"Reclaim"). LBA-equal / multi-LBA refs return whole spans to
        // global_free_extents; sub-LBA refs invert the byte range on the
        // host partial page.
        void
        release_values(std::span<const value_ref> refs);

        trim_plan
        prepare_trim(uint32_t max_ranges, uint32_t max_lbas);

        void
        complete_trim(trim_plan_id id, bool ok);

        // Recovery rebuild from live extents. Manager rebuilds
        // global_free_extents = [tree_alloc_head_lba, data_area_end_lba) -
        // occupied(live_extents). Sub-LBA partial metadata is rebuilt from
        // each live extent's (byte_offset, len) under the canonical class
        // mapping. Hints are advisory only; losing them never causes free
        // space leaks. Cached_partial_index is NOT rebuilt — it starts empty
        // post-recovery and refills as the cache layer admits frames.
        void
        install_recovered_state(std::span<const live_value_extent> live_extents,
                                uint64_t                           tree_alloc_head_lba,
                                uint64_t                           data_area_end_lba,
                                std::span<const dead_class_hint>   hints);

        // ── Inspectors (testable / observable, no owner mutation) ──────────

        uint64_t
        whole_free_lba_count() const noexcept {
            return whole_free_lba_count_;
        }

        uint64_t
        partial_page_count_total() const noexcept {
            return partial_page_count_total_;
        }

        uint64_t
        acknowledged_alloc_floor_lba() const noexcept {
            return acknowledged_alloc_floor_lba_;
        }

        const value_space_manager_config&
        config() const noexcept {
            return cfg_;
        }

        // True iff `page_base.lba` currently has a partial_page_node, i.e.
        // the page is partially allocated (some quantums occupied, some
        // free). Returns false for full pages (no node), all-free pages
        // (returned to global_free_extents), and out-of-range LBAs. The
        // value scheduler uses this after commit / release to decide
        // whether the DMA frame should be admitted to resident_partial_
        // (partial → yes) or readonly_cache_ / freed (full → cache, all-
        // free → drop).
        bool
        page_is_partial(paddr page_base) const noexcept {
            if (page_base.device_id != cfg_.device_id) return false;
            if (page_base.lba < cfg_.data_area_base_lba ||
                page_base.lba >= cfg_.data_area_end_lba) {
                return false;
            }
            const uint32_t group_id = group_id_for_lba_(page_base.lba);
            auto git = group_view_.find(group_id);
            if (git == group_view_.end()) return false;
            const group_state* g = git->second;
            const uint32_t page_delta =
                static_cast<uint32_t>(page_base.lba - g->base_lba);
            return g->partial_pages.by_page_delta.contains(page_delta);
        }

        // Returns the cached_partial_index entry's free_quantum_count for
        // page_base, or 0 if the page is not in the index. Used by the
        // value scheduler to capture the pre-claim baseline when a round
        // pins a cached partial frame, so commit can refresh the index
        // entry with the correct post-claim leftover.
        uint16_t
        cached_partial_free_quantum_count(paddr page_base) const noexcept {
            auto it = cached_partial_index_by_page_.find(page_base);
            if (it == cached_partial_index_by_page_.end()) return 0;
            return it->second.free_quantum_count;
        }

    private:
        // ── Type aliases for internal structures ─────────────────────────
        using partial_page_node = _vsm_detail::partial_page_node;
        using sparse_partial_repr = _vsm_detail::sparse_partial_repr;
        using group_state = _vsm_detail::group_state;
        using group_link = _vsm_detail::group_link;

        // ── Configuration (immutable post-construction) ──────────────────
        value_space_manager_config cfg_;
        uint32_t                   quantums_per_lba_ = 0;
        uint64_t                   value_usable_lba_count_ = 0;

        // Class-table-derived metadata. Indexed by class_idx; populated in
        // ctor; never mutated.
        absl::InlinedVector<space_class, 16> classes_;

        // ── Free-space truth ─────────────────────────────────────────────
        // global_free_extents: key = base_lba, value = len_lbas. Coalesced.
        absl::btree_map<uint64_t, uint32_t> global_free_extents_;
        uint64_t                            whole_free_lba_count_ = 0;

        // Group directory: lazy materialization. group_id =
        // (lba - data_area_base_lba) / value_space_group_size_lbas. We use a
        // hash map keyed by group_id; each value owns its group_state.
        absl::flat_hash_map<uint32_t, std::unique_ptr<group_state>>
            group_directory_;

        // Manager-level group buckets: groups_by_largest_run[k] holds head
        // group_id of groups whose partial_pages.nonempty_run_bucket_mask bit
        // k is set. Bit k of nonempty_group_run_bucket_mask iff that head is
        // valid. head-only — group-internal global_run_links[k] provides
        // O(1) splice and the manager itself never needs a tail iterator.
        uint32_t groups_by_largest_run_[64]{};
        uint64_t nonempty_group_run_bucket_mask_ = 0;
        uint64_t partial_page_count_total_ = 0;

        // ── Ownership of group_state by id, for O(1) link resolution ─────
        // Cached pointer view into group_directory_; rebuilt lazily as
        // groups are materialized. The keys are the same group_id values.
        absl::flat_hash_map<uint32_t, group_state*> group_view_;

        // ── alloc-floor reservation state (037 plan §"地址空间") ────────
        // published_alloc_floor_lba lives in cfg_.shared_heads->tree_head_lba
        // (decision §5: reuse). acknowledged_alloc_floor_lba_ is owner-local
        // single-thread state.
        uint64_t acknowledged_alloc_floor_lba_ = 0;

        // ── Pressure mode hysteresis state ───────────────────────────────
        enum class space_mode_t : uint8_t { normal, reuse_pressure, hard_pressure };
        space_mode_t current_space_mode_ = space_mode_t::normal;

        // ── TRIM bookkeeping (in-flight extents are withheld from
        //     global_free_extents until complete_trim returns) ─────────
        struct trim_inflight_entry {
            std::vector<trim_range> ranges;
        };
        absl::flat_hash_map<uint64_t, trim_inflight_entry> trim_inflight_;
        uint64_t                                           next_trim_plan_seq_ = 1;

        // ── Cached-partial index (037 plan §"Cached Partial Selection") ──
        struct cached_partial_entry {
            cached_partial_kind kind;
            uint16_t            free_quantum_count;
            uint16_t            largest_free_run_quantums;
            uint64_t            heat_seq;
            uint64_t            cache_epoch;
        };

        struct cached_partial_score {
            // Smaller key sorts first. Active tail dominates over generic
            // candidates; within a kind, larger largest_run sorts first
            // (encoded via complement); within that, hotter heat_seq sorts
            // first (encoded via complement).
            uint8_t  kind_rank;
            uint16_t largest_run_inv;
            uint64_t heat_seq_inv;

            bool operator<(const cached_partial_score& o) const noexcept {
                if (kind_rank != o.kind_rank) return kind_rank < o.kind_rank;
                if (largest_run_inv != o.largest_run_inv) return largest_run_inv < o.largest_run_inv;
                return heat_seq_inv < o.heat_seq_inv;
            }
        };

        absl::btree_map<paddr, cached_partial_entry>      cached_partial_index_by_page_;
        absl::btree_multimap<cached_partial_score, paddr> cached_partial_by_score_;
        uint64_t                                          cached_partial_estimated_bytes_ = 0;

        // ── Internal primitive helpers ───────────────────────────────────
        // (group / partial / extent manipulation)

        uint32_t
        group_id_for_lba_(uint64_t lba) const noexcept {
            assert(lba >= cfg_.data_area_base_lba);
            assert(lba <  cfg_.data_area_end_lba);
            return static_cast<uint32_t>(
                (lba - cfg_.data_area_base_lba) / cfg_.value_space_group_size_lbas);
        }

        uint64_t
        group_base_lba_(uint32_t group_id) const noexcept {
            return cfg_.data_area_base_lba
                 + static_cast<uint64_t>(group_id) * cfg_.value_space_group_size_lbas;
        }

        uint32_t
        group_lba_count_(uint32_t group_id) const noexcept {
            const uint64_t base = group_base_lba_(group_id);
            const uint64_t naive_end = base + cfg_.value_space_group_size_lbas;
            const uint64_t end = std::min(naive_end, cfg_.data_area_end_lba);
            return static_cast<uint32_t>(end - base);
        }

        // Returns nullptr if the group has not yet been materialized.
        group_state*
        find_group_(uint32_t group_id) noexcept {
            auto it = group_view_.find(group_id);
            return it == group_view_.end() ? nullptr : it->second;
        }

        // Materialize lazily. The group's bucket links start empty.
        group_state*
        ensure_group_(uint32_t group_id) {
            auto it = group_view_.find(group_id);
            if (it != group_view_.end()) return it->second;
            auto owned = std::make_unique<group_state>();
            owned->base_lba  = group_base_lba_(group_id);
            owned->lba_count = group_lba_count_(group_id);
            group_state* raw = owned.get();
            group_directory_.emplace(group_id, std::move(owned));
            group_view_.emplace(group_id, raw);
            return raw;
        }

        void
        try_release_group_(uint32_t group_id) noexcept {
            auto it = group_view_.find(group_id);
            if (it == group_view_.end()) return;
            group_state* g = it->second;
            // Only release when the group has no partial pages AND is not
            // linked into any global bucket. We allow keeping empty groups
            // in the directory to avoid churn, so this helper is conservative
            // and can be called freely after every release path.
            if (g->partial_page_count != 0) return;
            if (g->partial_pages.nonempty_run_bucket_mask != 0) return;
            group_view_.erase(it);
            group_directory_.erase(group_id);
        }

        // ── Bucket linkage primitives ────────────────────────────────────
        //
        // Manage the intrusive doubly-linked lists (head-only at the array
        // level; per-node prev/next inside the node).

        void
        node_link_into_bucket_(sparse_partial_repr& sp, uint32_t node_id, uint32_t k) noexcept {
            assert(k >= 1 && k < quantums_per_lba_);
            partial_page_node& n = sp.nodes.at(node_id);
            const uint32_t old_head = sp.pages_by_largest_run[k];
            n.bucket_prev = _vsm_detail::kInvalidNodeId;
            n.bucket_next = old_head;
            if (old_head != _vsm_detail::kInvalidNodeId) {
                sp.nodes.at(old_head).bucket_prev = node_id;
            }
            sp.pages_by_largest_run[k] = node_id;
            sp.nonempty_run_bucket_mask |= (1ULL << k);
        }

        void
        node_unlink_from_bucket_(sparse_partial_repr& sp, uint32_t node_id, uint32_t k) noexcept {
            assert(k >= 1 && k < quantums_per_lba_);
            partial_page_node& n = sp.nodes.at(node_id);
            const uint32_t prev = n.bucket_prev;
            const uint32_t next = n.bucket_next;
            if (prev != _vsm_detail::kInvalidNodeId) {
                sp.nodes.at(prev).bucket_next = next;
            } else {
                assert(sp.pages_by_largest_run[k] == node_id);
                sp.pages_by_largest_run[k] = next;
            }
            if (next != _vsm_detail::kInvalidNodeId) {
                sp.nodes.at(next).bucket_prev = prev;
            }
            n.bucket_prev = _vsm_detail::kInvalidNodeId;
            n.bucket_next = _vsm_detail::kInvalidNodeId;
            if (sp.pages_by_largest_run[k] == _vsm_detail::kInvalidNodeId) {
                sp.nonempty_run_bucket_mask &= ~(1ULL << k);
            }
        }

        void
        group_link_into_global_bucket_(uint32_t group_id, uint32_t k) noexcept {
            assert(k >= 1 && k < quantums_per_lba_);
            group_state* g = group_view_.at(group_id);
            const uint32_t old_head = groups_by_largest_run_[k];
            g->global_run_links[k].prev = _vsm_detail::kInvalidGroupId;
            g->global_run_links[k].next = old_head;
            if (old_head != _vsm_detail::kInvalidGroupId) {
                group_view_.at(old_head)->global_run_links[k].prev = group_id;
            }
            groups_by_largest_run_[k] = group_id;
            nonempty_group_run_bucket_mask_ |= (1ULL << k);
        }

        void
        group_unlink_from_global_bucket_(uint32_t group_id, uint32_t k) noexcept {
            assert(k >= 1 && k < quantums_per_lba_);
            group_state* g = group_view_.at(group_id);
            const uint32_t prev = g->global_run_links[k].prev;
            const uint32_t next = g->global_run_links[k].next;
            if (prev != _vsm_detail::kInvalidGroupId) {
                group_view_.at(prev)->global_run_links[k].next = next;
            } else {
                assert(groups_by_largest_run_[k] == group_id);
                groups_by_largest_run_[k] = next;
            }
            if (next != _vsm_detail::kInvalidGroupId) {
                group_view_.at(next)->global_run_links[k].prev = prev;
            }
            g->global_run_links[k].prev = _vsm_detail::kInvalidGroupId;
            g->global_run_links[k].next = _vsm_detail::kInvalidGroupId;
            if (groups_by_largest_run_[k] == _vsm_detail::kInvalidGroupId) {
                nonempty_group_run_bucket_mask_ &= ~(1ULL << k);
            }
        }

        // After mutating a group's partial_pages bucket-mask (via node
        // insert / remove / migrate), reconcile the group's membership in
        // the manager-level global buckets. delta_added_bits and
        // delta_removed_bits encode which group-level buckets transitioned
        // empty→nonempty (added) or nonempty→empty (removed).
        void
        reconcile_group_global_buckets_(uint32_t group_id,
                                         uint64_t delta_added_bits,
                                         uint64_t delta_removed_bits) noexcept {
            // Removals first so an old_k=new_k same-bucket no-op is cheap.
            uint64_t rm = delta_removed_bits;
            while (rm != 0) {
                const uint32_t k = static_cast<uint32_t>(std::countr_zero(rm));
                rm &= rm - 1;
                group_unlink_from_global_bucket_(group_id, k);
            }
            uint64_t add = delta_added_bits;
            while (add != 0) {
                const uint32_t k = static_cast<uint32_t>(std::countr_zero(add));
                add &= add - 1;
                group_link_into_global_bucket_(group_id, k);
            }
        }

        // Validate that a (base, byte_offset, len) triple describes a
        // structurally legal value within the manager's value area. Throws
        // std::invalid_argument with a context-tagged message on any rule
        // violation. Used by release_values() and install_recovered_state()
        // as a pre-pass — passing this guarantees `byte_offset` is a clean
        // quantum index and the placement does not straddle data_area_end.
        //
        // Out of scope: this never checks tree-liveness; the caller is
        // responsible for asserting the ref is dead w.r.t. tree + read
        // handles (037 plan §"Reclaim" trust boundary).
        void
        validate_value_ref_in_value_area_(const paddr&  base,
                                           uint16_t     byte_offset,
                                           uint32_t     len,
                                           const char*  context) const;

        // Apply a free-quantum-bits update to a partial node: scrub bit
        // ranges, recompute summary, migrate buckets, optionally remove the
        // node entirely (full or all-free). delta_count is the signed change
        // to free_quantum_count (positive = freeing more quantums).
        //
        // page_lba is needed because all-free pages convert to a 1-LBA
        // whole-free span returned to global_free_extents.
        //
        // Returns true if the node was removed (caller must not access it
        // afterwards); returns false if it survives.
        bool
        apply_node_bit_update_(group_state*           g,
                                uint32_t              group_id,
                                uint32_t              node_id,
                                uint64_t              new_free_bits,
                                int32_t               delta_count,
                                uint64_t              page_lba);

        // Look up or create a partial node for `page_lba`. If a node already
        // exists, the caller must add to it via apply_node_bit_update_.
        // Returns (node_id, group_id, group_state*).
        struct ensure_partial_node_result {
            uint32_t      node_id;
            uint32_t      group_id;
            group_state*  group;
            bool          created_new;
        };

        ensure_partial_node_result
        ensure_partial_node_(uint64_t page_lba);

        // ── global_free_extents helpers ─────────────────────────────────

        // Return [base_lba, base_lba + len_lbas) into global_free_extents,
        // coalescing with neighboring spans. Updates whole_free_lba_count_.
        void
        return_extent_to_global_(uint64_t base_lba, uint32_t len_lbas);

        // Carve a contiguous range of `need_lbas` LBAs out of
        // global_free_extents, respecting effective_alloc_floor_for_owner_().
        // Allocation policy is from-high-end. Returns nullopt on space
        // exhaustion.
        std::optional<reserved_extent>
        carve_high_extent_(uint32_t need_lbas);

        // Internal undo primitive shared by abort() and the failure path
        // inside allocate_batch (037 plan §"Reservation / Rollback").
        // Releases this round's claim deltas + returns reserved extents to
        // global_free_extents WITHOUT touching state owned by any other
        // inflight round. Leaves `round` empty so the caller can reuse it.
        void
        rollback_round_state_(value_space_round& round);

        // Cached-partial admission helper (eviction by worst score when over
        // budget). The manager controls only the candidate role; the cache
        // layer owns frame eviction (037 plan §"Cached Partial Budget" #8).
        void
        evict_one_cached_partial_();

        // Apply a sub-LBA claim against an EXISTING partial page (cached or
        // non-resident). On success, returns the claim and mutates the
        // partial node via apply_node_bit_update_. On any failure (no
        // group materialized, no node, no fitting run) returns nullopt and
        // mutates nothing. Caller is responsible for appending the claim
        // to the round.
        std::optional<byte_claim>
        claim_on_partial_(paddr             page_base,
                          uint16_t          class_idx,
                          uint32_t          alloc_quantums,
                          uint32_t          alloc_bytes,
                          uint64_t          cache_epoch,
                          byte_claim_source src);


        // Reservation-floor read with full defensive max.
        uint64_t
        effective_alloc_floor_for_owner_() const noexcept {
            uint64_t published = 0;
            if (cfg_.shared_heads != nullptr) {
                published = cfg_.shared_heads->tree_head_lba.load(
                    std::memory_order_acquire);
            }
            return std::max<uint64_t>(published, acknowledged_alloc_floor_lba_);
        }

        // ── Class-table helpers ─────────────────────────────────────────

        // Canonical mapping from value_ref.len → class_idx. Adds
        // cfg_.object_header_bytes to len before binary search.
        std::optional<uint16_t>
        canonical_class_idx_for_len_(uint32_t body_len) const noexcept {
            const uint64_t total = static_cast<uint64_t>(body_len)
                                 + cfg_.object_header_bytes;
            for (uint16_t i = 0; i < classes_.size(); ++i) {
                if (classes_[i].class_size >= total) return i;
            }
            return std::nullopt;
        }

        bool
        is_sub_lba_class_(uint16_t class_idx) const noexcept {
            return classes_[class_idx].class_size < cfg_.lba_size;
        }

        // ── Scoring / mode helpers (see allocate_batch impl) ────────────

        space_mode_t
        compute_space_mode_(space_mode_t prev_mode) const noexcept;

        space_mode_t
        compute_partial_mode_() const noexcept;

        space_mode_t
        compute_effective_mode_(allocation_policy policy);

        // ── Cached-partial index helpers ────────────────────────────────

        cached_partial_score
        score_for_(const cached_partial_entry& e) const noexcept {
            cached_partial_score s{};
            s.kind_rank       = (e.kind == cached_partial_kind::active_tail) ? 0u : 1u;
            s.largest_run_inv = static_cast<uint16_t>(quantums_per_lba_)
                              - e.largest_free_run_quantums;
            s.heat_seq_inv    = UINT64_MAX - e.heat_seq;
            return s;
        }

        // ── Disabled — manager owns arena-style state, copying makes no sense. ──
        value_space_manager(const value_space_manager&) = delete;
        value_space_manager& operator=(const value_space_manager&) = delete;
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Implementation
    // ─────────────────────────────────────────────────────────────────────────

    namespace _vsm_detail {

        // Lightweight power-of-two checks used in ctor validation. Returning
        // bool keeps the call sites readable; wrap_in_throw is shouldered by
        // the caller.
        inline bool
        is_power_of_two_u64(uint64_t v) noexcept {
            return v != 0 && (v & (v - 1)) == 0;
        }

        // class_size shape: either 64B·2^n (sub-LBA, < lba_size) or
        // lba_size·2^m (LBA-equal / multi-LBA). Returns true if class_size
        // satisfies one of the two shapes given quantum and lba_size.
        inline bool
        class_size_shape_ok(uint32_t class_size,
                            uint32_t lba_size,
                            uint32_t quantum_bytes) noexcept {
            if (class_size == 0) return false;
            if (class_size < lba_size) {
                if (class_size % quantum_bytes != 0) return false;
                if (class_size < quantum_bytes) return false;
                const uint64_t multiples = class_size / quantum_bytes;
                if (!is_power_of_two_u64(multiples)) return false;
                return true;
            }
            if (class_size == lba_size) return true;
            // multi-LBA
            if (class_size % lba_size != 0) return false;
            const uint64_t lba_mult = class_size / lba_size;
            return is_power_of_two_u64(lba_mult);
        }

    } // namespace _vsm_detail

    inline
    value_space_manager::value_space_manager(value_space_manager_config cfg)
        : cfg_(std::move(cfg))
    {
        // ── Pin format / start invariants. fail-fast on any drift. ──
        if (cfg_.lba_size == 0) {
            throw std::invalid_argument("value_space_manager: lba_size must be > 0");
        }
        if (cfg_.value_space_quantum_bytes != 64) {
            throw std::invalid_argument(
                "value_space_manager: value_space_quantum_bytes must be 64 "
                "(037 plan §\"Allocation Quantum\")");
        }
        if (cfg_.lba_size % cfg_.value_space_quantum_bytes != 0) {
            throw std::invalid_argument(
                "value_space_manager: lba_size must be a multiple of "
                "value_space_quantum_bytes");
        }

        quantums_per_lba_ = cfg_.lba_size / cfg_.value_space_quantum_bytes;
        if (quantums_per_lba_ == 0 || quantums_per_lba_ > 64) {
            throw std::invalid_argument(
                "value_space_manager: quantums_per_lba must be in [1, 64] "
                "(uint64_t free_quantum_bits is hard-pinned)");
        }

        // group_size_lbas: integer LBA, integer quantums; group bytes
        // power-of-two; in [64 MiB, 1 GiB].
        if (cfg_.value_space_group_size_lbas == 0) {
            throw std::invalid_argument(
                "value_space_manager: value_space_group_size_lbas must be > 0");
        }
        const uint64_t group_bytes =
            static_cast<uint64_t>(cfg_.value_space_group_size_lbas) * cfg_.lba_size;
        if (!_vsm_detail::is_power_of_two_u64(group_bytes)) {
            throw std::invalid_argument(
                "value_space_manager: group bytes (group_size_lbas * lba_size) "
                "must be a power of two");
        }
        if (group_bytes < 64ULL * 1024 * 1024 || group_bytes > 1024ULL * 1024 * 1024) {
            throw std::invalid_argument(
                "value_space_manager: group bytes must be in [64 MiB, 1 GiB]");
        }
        if (group_bytes % cfg_.value_space_quantum_bytes != 0) {
            // Already implied by integer LBA + lba_size%quantum==0, but keep
            // the explicit check so future relaxations don't silently break.
            throw std::invalid_argument(
                "value_space_manager: group bytes must cover an integer number "
                "of quantum_bytes");
        }

        if (!(cfg_.data_area_base_lba < cfg_.data_area_end_lba)) {
            throw std::invalid_argument(
                "value_space_manager: data_area_base_lba must be < data_area_end_lba");
        }

        // ── Class table validation + materialization. ──
        if (cfg_.value_class_sizes.empty() || cfg_.value_class_sizes.size() > 16) {
            throw std::invalid_argument(
                "value_space_manager: value_class_sizes must have size in [1, 16]");
        }
        uint32_t prev = 0;
        for (uint16_t i = 0; i < cfg_.value_class_sizes.size(); ++i) {
            const uint32_t cs = cfg_.value_class_sizes[i];
            if (cs <= prev) {
                throw std::invalid_argument(
                    "value_space_manager: value_class_sizes must be strictly ascending");
            }
            if (!_vsm_detail::class_size_shape_ok(
                    cs, cfg_.lba_size, cfg_.value_space_quantum_bytes)) {
                throw std::invalid_argument(
                    "value_space_manager: class_size must satisfy "
                    "64B·2^n (sub-LBA) or lba_size·2^m (LBA-equal / multi-LBA)");
            }
            prev = cs;

            space_class sc{};
            sc.class_idx  = i;
            sc.class_size = cs;
            if (cs < cfg_.lba_size) {
                sc.span_lbas      = 1;
                sc.alloc_quantums = static_cast<uint16_t>(cs / cfg_.value_space_quantum_bytes);
            } else {
                sc.span_lbas      = cs / cfg_.lba_size;
                sc.alloc_quantums = 0; // not meaningful for LBA-equal/multi-LBA
            }
            classes_.push_back(sc);
        }

        // ── Acked floor / usable LBA count. ──
        acknowledged_alloc_floor_lba_ =
            cfg_.initial_acknowledged_alloc_floor_lba == 0
                ? cfg_.data_area_base_lba
                : cfg_.initial_acknowledged_alloc_floor_lba;
        if (acknowledged_alloc_floor_lba_ < cfg_.data_area_base_lba ||
            acknowledged_alloc_floor_lba_ > cfg_.data_area_end_lba) {
            throw std::invalid_argument(
                "value_space_manager: initial_acknowledged_alloc_floor_lba "
                "out of [data_area_base_lba, data_area_end_lba]");
        }
        value_usable_lba_count_ = cfg_.data_area_end_lba - cfg_.data_area_base_lba;

        // ── Watermark sanity. Hysteresis order: hard_low ≤ hard_high
        //    ≤ normal_low ≤ normal_high. ──
        if (!(cfg_.hard_low_watermark_bp  <= cfg_.hard_high_watermark_bp  &&
              cfg_.hard_high_watermark_bp <= cfg_.normal_low_watermark_bp &&
              cfg_.normal_low_watermark_bp <= cfg_.normal_high_watermark_bp)) {
            throw std::invalid_argument(
                "value_space_manager: watermark hysteresis order violated");
        }

        // ── Partial-budget sanity. ──
        if (cfg_.partial_metadata_soft_limit_pages > cfg_.partial_metadata_hard_limit_pages) {
            throw std::invalid_argument(
                "value_space_manager: partial_metadata_soft_limit_pages must be "
                "<= partial_metadata_hard_limit_pages");
        }

        // ── Initial free-space truth: one big extent spanning the whole
        //    value-allocatable region. The acked floor only constrains
        //    consumption sites; we don't pre-clip the extent here so that
        //    future floor-down (tree returns extents) is symmetric. ──
        global_free_extents_.emplace(
            cfg_.data_area_base_lba,
            static_cast<uint32_t>(value_usable_lba_count_));
        whole_free_lba_count_ = value_usable_lba_count_;

        // group / cached / trim heads start as initialized (kInvalid* / 0).
        for (uint32_t& h : groups_by_largest_run_) {
            h = _vsm_detail::kInvalidGroupId;
        }
    }

    // ── ensure_partial_node_ ───────────────────────────────────────────────
    //
    // Look up `page_lba` in its group's by_page_delta table; if absent,
    // allocate a fresh node with empty free_quantum_bits. The node is NOT
    // linked into any bucket — caller must call apply_node_bit_update_ to
    // populate bits and migrate buckets.

    inline value_space_manager::ensure_partial_node_result
    value_space_manager::ensure_partial_node_(uint64_t page_lba) {
        const uint32_t group_id   = group_id_for_lba_(page_lba);
        group_state*   g          = ensure_group_(group_id);
        const uint32_t page_delta = static_cast<uint32_t>(page_lba - g->base_lba);
        auto& by_delta = g->partial_pages.by_page_delta;
        auto it = by_delta.find(page_delta);
        if (it != by_delta.end()) {
            return ensure_partial_node_result{
                .node_id     = it->second,
                .group_id    = group_id,
                .group       = g,
                .created_new = false,
            };
        }
        const uint32_t node_id = g->partial_pages.nodes.alloc();
        partial_page_node& n   = g->partial_pages.nodes.at(node_id);
        n.page_delta                  = page_delta;
        n.free_quantum_count          = 0;
        n.largest_free_run_quantums   = 0;
        n.free_quantum_bits           = 0;
        n.bucket_prev                 = _vsm_detail::kInvalidNodeId;
        n.bucket_next                 = _vsm_detail::kInvalidNodeId;
        by_delta.emplace(page_delta, node_id);
        return ensure_partial_node_result{
            .node_id     = node_id,
            .group_id    = group_id,
            .group       = g,
            .created_new = true,
        };
    }

    // ── apply_node_bit_update_ ─────────────────────────────────────────────
    //
    // Adopt a new free_quantum_bits image on the node, recompute summary
    // fields, and reconcile bucket membership at both group-local
    // (pages_by_largest_run) and manager-global (groups_by_largest_run)
    // levels.
    //
    // Three exit shapes:
    //   1. new_k == 0 → page is full → unlink + free arena slot, return true.
    //   2. new_k == quantums_per_lba_ → page is all-free → unlink + free
    //      arena slot, return [page_lba, page_lba+1) into global_free_extents,
    //      return true.
    //   3. otherwise → migrate (or stay) inside [1, qpl-1] buckets, return
    //      false.
    //
    // partial_page_count and partial_page_count_total_ are maintained here.

    inline bool
    value_space_manager::apply_node_bit_update_(group_state* g,
                                                 uint32_t      group_id,
                                                 uint32_t      node_id,
                                                 uint64_t      new_free_bits,
                                                 int32_t       delta_count,
                                                 uint64_t      page_lba) {
        partial_page_node& n = g->partial_pages.nodes.at(node_id);
        const uint32_t old_k = n.largest_free_run_quantums;
        // pre-update group-level mask snapshot for global bucket reconcile.
        const uint64_t old_group_mask = g->partial_pages.nonempty_run_bucket_mask;
        const bool node_was_in_partial = (n.free_quantum_count != 0);

        n.free_quantum_bits  = new_free_bits;
        if (delta_count >= 0) {
            n.free_quantum_count = static_cast<uint16_t>(
                n.free_quantum_count + static_cast<uint16_t>(delta_count));
        } else {
            n.free_quantum_count = static_cast<uint16_t>(
                n.free_quantum_count - static_cast<uint16_t>(-delta_count));
        }
        // Invariant guard (debug-only): popcount must match. Hot path keeps
        // the running tally; this catches bookkeeping drift.
        assert(static_cast<uint32_t>(std::popcount(new_free_bits))
               == n.free_quantum_count);

        const uint32_t new_k = _vsm_detail::recompute_largest_run(
            new_free_bits, quantums_per_lba_);
        n.largest_free_run_quantums = static_cast<uint16_t>(new_k);

        // Unlink from old bucket (if it was in one).
        if (node_was_in_partial && old_k >= 1 && old_k < quantums_per_lba_) {
            node_unlink_from_bucket_(g->partial_pages, node_id, old_k);
        }

        bool removed = false;
        if (n.free_quantum_count == 0) {
            // Page is now full.
            if (node_was_in_partial) {
                g->partial_page_count -= 1;
                partial_page_count_total_ -= 1;
            }
            g->partial_pages.by_page_delta.erase(n.page_delta);
            // Scrub bucket_prev so node_arena's free-chain invariant holds.
            n.bucket_prev = _vsm_detail::kInvalidNodeId;
            n.bucket_next = _vsm_detail::kInvalidNodeId;
            g->partial_pages.nodes.free(node_id);
            removed = true;
        } else if (new_k == quantums_per_lba_) {
            // Page is now all-free → return a 1-LBA whole-free span.
            if (node_was_in_partial) {
                g->partial_page_count -= 1;
                partial_page_count_total_ -= 1;
            }
            g->partial_pages.by_page_delta.erase(n.page_delta);
            n.bucket_prev = _vsm_detail::kInvalidNodeId;
            n.bucket_next = _vsm_detail::kInvalidNodeId;
            g->partial_pages.nodes.free(node_id);
            return_extent_to_global_(page_lba, 1u);
            removed = true;
        } else {
            // Page stays partial.
            if (!node_was_in_partial) {
                g->partial_page_count += 1;
                partial_page_count_total_ += 1;
            }
            node_link_into_bucket_(g->partial_pages, node_id, new_k);
        }

        // Reconcile group-level membership in the manager's global buckets.
        const uint64_t new_group_mask = g->partial_pages.nonempty_run_bucket_mask;
        const uint64_t added_bits     = new_group_mask & ~old_group_mask;
        const uint64_t removed_bits   = old_group_mask & ~new_group_mask;
        reconcile_group_global_buckets_(group_id, added_bits, removed_bits);

        // ── cached_partial_index coherence (037 plan §"Cached Partial
        //    Admission" + §"Cached Partial Selection") ──
        //
        // The index stores SNAPSHOT copies of free_quantum_count /
        // largest_free_run_quantums. apply_node_bit_update_ is the single
        // seam through which partial-page truth changes; converging the
        // repair here keeps every claim/release/abort/recovery path in
        // sync without spreading conditional patch-ups across callers.
        //
        // - removed (page → full or → all-free): drop the index entry.
        //   The page is no longer partial; any cache-layer notification
        //   that re-introduces it must come through mark_cached_partial.
        // - still partial: preserve kind / heat_seq / cache_epoch (the
        //   cache-layer-owned dimensions) but refresh the summary fields
        //   and rebuild the score multimap entry.
        const paddr cached_paddr{cfg_.device_id, page_lba};
        auto cit = cached_partial_index_by_page_.find(cached_paddr);
        if (cit != cached_partial_index_by_page_.end()) {
            const cached_partial_score old_score = score_for_(cit->second);
            auto [lo, hi] = cached_partial_by_score_.equal_range(old_score);
            for (auto sit = lo; sit != hi; ++sit) {
                if (sit->second == cached_paddr) {
                    cached_partial_by_score_.erase(sit);
                    break;
                }
            }
            if (removed) {
                cached_partial_index_by_page_.erase(cit);
                if (cached_partial_estimated_bytes_ >= cfg_.lba_size) {
                    cached_partial_estimated_bytes_ -= cfg_.lba_size;
                } else {
                    cached_partial_estimated_bytes_ = 0;
                }
            } else {
                // n is still valid in this branch (only the partial branch
                // reaches here without freeing the slot). Refresh the
                // summary copy in the index.
                cit->second.free_quantum_count        = n.free_quantum_count;
                cit->second.largest_free_run_quantums = n.largest_free_run_quantums;
                cached_partial_by_score_.emplace(score_for_(cit->second), cached_paddr);
            }
        }

        if (g->partial_page_count == 0) {
            try_release_group_(group_id);
        }

        return removed;
    }

    // ── return_extent_to_global_ ──────────────────────────────────────────
    //
    // Insert [base_lba, base_lba + len_lbas) into global_free_extents,
    // coalescing with the immediate predecessor / successor span. Updates
    // whole_free_lba_count_.

    inline void
    value_space_manager::return_extent_to_global_(uint64_t base_lba, uint32_t len_lbas) {
        assert(len_lbas > 0);
        assert(base_lba >= cfg_.data_area_base_lba);
        assert(base_lba + len_lbas <= cfg_.data_area_end_lba);

        uint64_t new_base = base_lba;
        uint32_t new_len  = len_lbas;

        // ── Coalesce with predecessor (highest extent whose base ≤ new_base). ──
        auto it_prev = global_free_extents_.upper_bound(new_base);
        if (it_prev != global_free_extents_.begin()) {
            --it_prev;
            const uint64_t prev_base = it_prev->first;
            const uint32_t prev_len  = it_prev->second;
            if (prev_base + prev_len == new_base) {
                new_base = prev_base;
                new_len  = static_cast<uint32_t>(prev_len + new_len);
                global_free_extents_.erase(it_prev);
            } else if (prev_base + prev_len > new_base) {
                // Overlap with predecessor — caller error.
                assert(false && "return_extent_to_global_: overlap with predecessor");
            }
        }

        // ── Coalesce with successor. ──
        auto it_next = global_free_extents_.lower_bound(new_base + new_len);
        if (it_next != global_free_extents_.end()) {
            const uint64_t next_base = it_next->first;
            if (next_base == new_base + new_len) {
                new_len = static_cast<uint32_t>(new_len + it_next->second);
                global_free_extents_.erase(it_next);
            } else if (next_base < new_base + new_len) {
                assert(false && "return_extent_to_global_: overlap with successor");
            }
        }

        global_free_extents_.emplace(new_base, new_len);
        whole_free_lba_count_ += len_lbas;
    }

    // ── carve_high_extent_ ────────────────────────────────────────────────
    //
    // Carve `need_lbas` contiguous LBAs from the high end of
    // global_free_extents, respecting the effective alloc floor. Returns
    // the carved [base_lba, base_lba+need_lbas) on success.
    //
    // Iteration: walk extents from highest base downward, skipping any whose
    // tail is below the floor + need, until we find a fit. Linear in the
    // extent count for each call; production workloads keep the extent count
    // small (most allocations either consume entire extents or trim from
    // the high side).

    inline std::optional<reserved_extent>
    value_space_manager::carve_high_extent_(uint32_t need_lbas) {
        if (need_lbas == 0) return std::nullopt;
        if (global_free_extents_.empty()) return std::nullopt;
        const uint64_t floor = effective_alloc_floor_for_owner_();

        // Walk from largest base downward (rbegin → end), find first extent
        // with len_lbas >= need AND whose top portion lies above floor.
        for (auto it = global_free_extents_.rbegin();
             it != global_free_extents_.rend();
             ++it) {
            const uint64_t base = it->first;
            const uint32_t len  = it->second;
            // Below-floor stale extents are skipped here. They will get
            // pruned by the next sync_alloc_floor accept; until then they
            // are simply invisible to allocation.
            if (base + len <= floor) continue;
            // Effective top-end carving: take the highest `need_lbas` LBAs
            // within this extent that still lie above floor.
            const uint64_t hi_end_excl = base + len;
            const uint64_t lo_for_take = hi_end_excl - need_lbas;
            if (lo_for_take < base) continue;       // extent too short
            if (lo_for_take < floor) continue;      // would dip below floor

            // Carve. Erase via forward iterator (reverse_iterator base is one past).
            const uint64_t orig_base = base;
            const uint32_t orig_len  = len;
            // Compute the forward iterator before invalidating.
            auto fwd = std::next(it).base();         // forward iter at this entry
            global_free_extents_.erase(fwd);

            const uint32_t taken_len    = need_lbas;
            const uint64_t carved_base  = lo_for_take;
            const uint32_t leftover_len = static_cast<uint32_t>(orig_len - taken_len);
            if (leftover_len > 0) {
                global_free_extents_.emplace(orig_base, leftover_len);
            }
            whole_free_lba_count_ -= taken_len;
            return reserved_extent{ .base_lba = carved_base, .len_lbas = taken_len };
        }
        return std::nullopt;
    }

    // ── begin_round / try_claim_range ─────────────────────────────────────

    inline value_space_round
    value_space_manager::begin_round() {
        return value_space_round{};
    }

    inline std::optional<byte_claim>
    value_space_manager::try_claim_range(value_space_round& round,
                                          paddr              page_base,
                                          uint16_t           class_idx,
                                          uint32_t           alloc_quantums,
                                          uint32_t           alloc_bytes,
                                          uint64_t           cache_epoch) {
        // Bounds: this entry point exists only for sub-LBA, page-local
        // claims. LBA-equal / multi-LBA never lives in partial_pages and
        // must be allocated via allocate_batch only.
        if (alloc_quantums == 0 || alloc_quantums >= quantums_per_lba_) {
            return std::nullopt;
        }
        if (page_base.lba < cfg_.data_area_base_lba ||
            page_base.lba >= cfg_.data_area_end_lba) {
            return std::nullopt;
        }
        const uint32_t group_id = group_id_for_lba_(page_base.lba);
        group_state*   g        = find_group_(group_id);
        if (g == nullptr) return std::nullopt;
        const uint32_t page_delta =
            static_cast<uint32_t>(page_base.lba - g->base_lba);
        auto it = g->partial_pages.by_page_delta.find(page_delta);
        if (it == g->partial_pages.by_page_delta.end()) return std::nullopt;
        const uint32_t node_id = it->second;
        partial_page_node& n   = g->partial_pages.nodes.at(node_id);
        if (n.largest_free_run_quantums < alloc_quantums) return std::nullopt;

        const uint32_t pos = _vsm_detail::choose_claim_offset(
            n.free_quantum_bits,
            alloc_quantums,
            n.largest_free_run_quantums,
            quantums_per_lba_);
        const uint64_t mask = _vsm_detail::quantum_range_mask(pos, alloc_quantums);
        const uint64_t new_bits = n.free_quantum_bits & ~mask;
        const int32_t  delta    = -static_cast<int32_t>(alloc_quantums);
        apply_node_bit_update_(g, group_id, node_id, new_bits, delta, page_base.lba);

        byte_claim claim{
            .page_base   = page_base,
            .byte_offset = pos * cfg_.value_space_quantum_bytes,
            .alloc_bytes = alloc_bytes,
            .class_idx   = class_idx,
            .cache_epoch = cache_epoch,
            .src         = byte_claim_source::cached_partial,
        };
        round.claims.push_back(claim);
        return claim;
    }

    // ── mark_cached_partial / erase_cached_partial / evict ────────────────

    inline void
    value_space_manager::mark_cached_partial(const cached_partial_update& update) {
        // Only admit if the page is actually partial in our metadata; cache
        // layer notifications for fully-allocated or all-free pages are
        // safely ignored (037 plan §"Cached Partial Admission" — admission
        // requires "partial in manager metadata").
        if (update.page_base.lba < cfg_.data_area_base_lba ||
            update.page_base.lba >= cfg_.data_area_end_lba) {
            return;
        }
        const uint32_t group_id = group_id_for_lba_(update.page_base.lba);
        group_state*   g        = find_group_(group_id);
        if (g == nullptr) return;
        const uint32_t page_delta =
            static_cast<uint32_t>(update.page_base.lba - g->base_lba);
        auto it = g->partial_pages.by_page_delta.find(page_delta);
        if (it == g->partial_pages.by_page_delta.end()) return;
        const partial_page_node& n = g->partial_pages.nodes.at(it->second);

        cached_partial_entry e{};
        e.kind                      = update.kind;
        e.free_quantum_count        = n.free_quantum_count;
        e.largest_free_run_quantums = n.largest_free_run_quantums;
        e.heat_seq                  = update.heat_seq;
        e.cache_epoch               = update.cache_epoch;

        auto idx_it = cached_partial_index_by_page_.find(update.page_base);
        if (idx_it != cached_partial_index_by_page_.end()) {
            // Erase old score entry → reinsert with new score.
            const cached_partial_score old_score = score_for_(idx_it->second);
            auto [lo, hi] = cached_partial_by_score_.equal_range(old_score);
            for (auto sit = lo; sit != hi; ++sit) {
                if (sit->second == update.page_base) {
                    cached_partial_by_score_.erase(sit);
                    break;
                }
            }
            idx_it->second = e;
        } else {
            // New entry; budget check first.
            const uint64_t add_bytes = cfg_.lba_size;
            while (cached_partial_estimated_bytes_ + add_bytes
                       > cfg_.value_cached_partial_budget_bytes &&
                   !cached_partial_by_score_.empty()) {
                evict_one_cached_partial_();
            }
            cached_partial_index_by_page_.emplace(update.page_base, e);
            cached_partial_estimated_bytes_ += add_bytes;
        }
        cached_partial_by_score_.emplace(score_for_(e), update.page_base);
    }

    inline void
    value_space_manager::erase_cached_partial(paddr page_base, uint64_t cache_epoch) {
        auto it = cached_partial_index_by_page_.find(page_base);
        if (it == cached_partial_index_by_page_.end()) return;
        if (it->second.cache_epoch != cache_epoch) return; // stale notification
        const cached_partial_score s = score_for_(it->second);
        auto [lo, hi] = cached_partial_by_score_.equal_range(s);
        for (auto sit = lo; sit != hi; ++sit) {
            if (sit->second == page_base) {
                cached_partial_by_score_.erase(sit);
                break;
            }
        }
        cached_partial_index_by_page_.erase(it);
        if (cached_partial_estimated_bytes_ >= cfg_.lba_size) {
            cached_partial_estimated_bytes_ -= cfg_.lba_size;
        } else {
            cached_partial_estimated_bytes_ = 0;
        }
    }

    inline void
    value_space_manager::evict_one_cached_partial_() {
        if (cached_partial_by_score_.empty()) return;
        // Worst score = highest key (kind_rank dominant: cached_free_candidate
        // > active_tail; within kind, smaller largest_run dominates eviction;
        // colder heat_seq dominates eviction).
        auto worst = std::prev(cached_partial_by_score_.end());
        const paddr page_base = worst->second;
        cached_partial_by_score_.erase(worst);
        cached_partial_index_by_page_.erase(page_base);
        if (cached_partial_estimated_bytes_ >= cfg_.lba_size) {
            cached_partial_estimated_bytes_ -= cfg_.lba_size;
        } else {
            cached_partial_estimated_bytes_ = 0;
        }
    }

    // ── claim_on_partial_ ──────────────────────────────────────────────────

    inline std::optional<byte_claim>
    value_space_manager::claim_on_partial_(paddr             page_base,
                                            uint16_t          class_idx,
                                            uint32_t          alloc_quantums,
                                            uint32_t          alloc_bytes,
                                            uint64_t          cache_epoch,
                                            byte_claim_source src) {
        if (alloc_quantums == 0 || alloc_quantums >= quantums_per_lba_) {
            return std::nullopt;
        }
        const uint32_t group_id = group_id_for_lba_(page_base.lba);
        group_state*   g        = find_group_(group_id);
        if (g == nullptr) return std::nullopt;
        const uint32_t page_delta =
            static_cast<uint32_t>(page_base.lba - g->base_lba);
        auto it = g->partial_pages.by_page_delta.find(page_delta);
        if (it == g->partial_pages.by_page_delta.end()) return std::nullopt;
        const uint32_t node_id = it->second;
        partial_page_node& n   = g->partial_pages.nodes.at(node_id);
        if (n.largest_free_run_quantums < alloc_quantums) return std::nullopt;

        const uint32_t pos = _vsm_detail::choose_claim_offset(
            n.free_quantum_bits,
            alloc_quantums,
            n.largest_free_run_quantums,
            quantums_per_lba_);
        const uint64_t mask = _vsm_detail::quantum_range_mask(pos, alloc_quantums);
        const uint64_t new_bits = n.free_quantum_bits & ~mask;
        const int32_t  delta    = -static_cast<int32_t>(alloc_quantums);
        apply_node_bit_update_(g, group_id, node_id, new_bits, delta, page_base.lba);

        return byte_claim{
            .page_base   = page_base,
            .byte_offset = pos * cfg_.value_space_quantum_bytes,
            .alloc_bytes = alloc_bytes,
            .class_idx   = class_idx,
            .cache_epoch = cache_epoch,
            .src         = src,
        };
    }

    // ── commit / abort ─────────────────────────────────────────────────────

    inline void
    value_space_manager::commit(value_space_round&& round) {
        // Publish freshly-carved tail pages as partial_pages entries. Their
        // hosting fresh_extents stay consumed (LBAs were carved at allocate
        // time). Tail pages have, by construction, no overlap with any
        // pre-existing partial node, so ensure_partial_node_ creates fresh.
        for (const pending_tail_page& tail : round.unpublished_tails) {
            ensure_partial_node_result enr = ensure_partial_node_(tail.page_base.lba);
            assert(enr.created_new
                   && "pending_tail_page expects a freshly-carved LBA");
            const int32_t delta = static_cast<int32_t>(tail.free_quantum_count);
            apply_node_bit_update_(enr.group,
                                    enr.group_id,
                                    enr.node_id,
                                    tail.free_quantum_bits,
                                    delta,
                                    tail.page_base.lba);
        }
        // Round state is consumed; nothing else to do.
    }

    inline void
    value_space_manager::abort(value_space_round&& round) {
        rollback_round_state_(round);
    }

    // ── compute_*_mode_ ────────────────────────────────────────────────────
    //
    // 037 plan §"Allocation Mode Selection" hysteresis tables. space_mode
    // moves on whole_free_lba_count_ ratio; partial_mode moves on
    // partial_page_count_total_; effective_mode = max.

    inline value_space_manager::space_mode_t
    value_space_manager::compute_space_mode_(space_mode_t prev_mode) const noexcept {
        if (value_usable_lba_count_ == 0) return prev_mode;
        // ratio in basis points (per 10000)
        const uint64_t ratio_bp =
            (static_cast<uint64_t>(whole_free_lba_count_) * 10000)
            / value_usable_lba_count_;
        switch (prev_mode) {
        case space_mode_t::normal:
            if (ratio_bp < cfg_.normal_low_watermark_bp) {
                return space_mode_t::reuse_pressure;
            }
            return space_mode_t::normal;
        case space_mode_t::reuse_pressure:
            if (ratio_bp < cfg_.hard_low_watermark_bp) {
                return space_mode_t::hard_pressure;
            }
            if (ratio_bp > cfg_.normal_high_watermark_bp) {
                return space_mode_t::normal;
            }
            return space_mode_t::reuse_pressure;
        case space_mode_t::hard_pressure:
            if (ratio_bp > cfg_.hard_high_watermark_bp) {
                return space_mode_t::reuse_pressure;
            }
            return space_mode_t::hard_pressure;
        }
        return prev_mode;
    }

    inline value_space_manager::space_mode_t
    value_space_manager::compute_partial_mode_() const noexcept {
        if (partial_page_count_total_ >= cfg_.partial_metadata_hard_limit_pages) {
            return space_mode_t::hard_pressure;
        }
        if (partial_page_count_total_ >= cfg_.partial_metadata_soft_limit_pages) {
            return space_mode_t::reuse_pressure;
        }
        return space_mode_t::normal;
    }

    inline value_space_manager::space_mode_t
    value_space_manager::compute_effective_mode_(allocation_policy policy) {
        switch (policy.mode) {
        case allocation_mode_override::force_normal:
            current_space_mode_ = space_mode_t::normal;
            return space_mode_t::normal;
        case allocation_mode_override::force_reuse_pressure:
            return space_mode_t::reuse_pressure;
        case allocation_mode_override::force_hard_pressure:
            return space_mode_t::hard_pressure;
        case allocation_mode_override::automatic:
        default:
            break;
        }
        const space_mode_t space = compute_space_mode_(current_space_mode_);
        const space_mode_t partial = compute_partial_mode_();
        // current_space_mode_ tracks only the space-driven hysteresis state.
        current_space_mode_ = space;
        // max(space, partial)
        return static_cast<uint8_t>(space) >= static_cast<uint8_t>(partial)
                 ? space
                 : partial;
    }

    // ── allocate_batch ────────────────────────────────────────────────────
    //
    // Best-fit-decreasing planner. Internally reorders entries by allocation
    // size; outputs are restored to caller order. Mutates partial-page state
    // as it places (not "plan then commit") so concurrent rounds see the
    // claimed quantum ranges immediately. On failure, rolls back the round
    // and returns an empty vector so the caller can back-pressure.

    inline std::vector<byte_claim>
    value_space_manager::allocate_batch(value_space_round& round,
                                         std::span<const allocation_request> reqs,
                                         allocation_policy policy) {
        if (reqs.empty()) return {};

        const space_mode_t mode = compute_effective_mode_(policy);

        const bool hard_gate_active =
            (partial_page_count_total_ >= cfg_.partial_metadata_hard_limit_pages);

        // Per-class read budget for NRP usage (per-batch budget is
        // enforced at admission time by truncating nrp_locals).
        uint32_t prefill_per_class_max = 0;
        if (mode == space_mode_t::reuse_pressure) {
            prefill_per_class_max = cfg_.reuse_pressure_max_prefill_reads_per_class;
        } else if (mode == space_mode_t::hard_pressure) {
            prefill_per_class_max = cfg_.hard_pressure_max_prefill_reads_per_class;
        }
        absl::flat_hash_map<uint16_t, uint32_t> prefill_per_class_used;

        // BFD sort indices.
        std::vector<uint32_t> sorted_idx(reqs.size());
        for (uint32_t i = 0; i < reqs.size(); ++i) sorted_idx[i] = i;
        const auto entry_size_for_sort = [&](uint32_t i) -> uint64_t {
            const auto& r = reqs[i];
            const space_class& cls = classes_[r.class_idx];
            if (cls.class_size < cfg_.lba_size) {
                return r.alloc_quantums;
            }
            // LBA-equal / multi-LBA — express as quantum-equivalent units so
            // the sort order is monotone in absolute byte size.
            return static_cast<uint64_t>(cls.span_lbas) * quantums_per_lba_;
        };
        std::sort(sorted_idx.begin(), sorted_idx.end(),
                  [&](uint32_t a, uint32_t b) {
                      return entry_size_for_sort(a) > entry_size_for_sort(b);
                  });

        // Snapshot up to max_candidate_pages cached candidates, ordered by
        // score (active_tail kind first, then largest_run desc, then heat
        // desc). Kind / heat are preserved so phase-2 best-fit can apply
        // 037 §"Cached Partial Selection" priority (active_tail first,
        // then smallest leftover, then recency).
        struct cached_local {
            paddr               page_base;
            uint16_t            free_quantum_count;
            uint16_t            largest_free_run_quantums;
            uint64_t            cache_epoch;
            cached_partial_kind kind;
            uint64_t            heat_seq;
        };
        std::vector<cached_local> cached_locals;
        {
            const uint32_t cap = std::min<uint32_t>(
                cfg_.max_candidate_pages,
                static_cast<uint32_t>(cached_partial_by_score_.size()));
            cached_locals.reserve(cap);
            uint32_t taken = 0;
            for (auto it = cached_partial_by_score_.begin();
                 it != cached_partial_by_score_.end() && taken < cap;
                 ++it, ++taken) {
                const paddr p = it->second;
                auto eit = cached_partial_index_by_page_.find(p);
                if (eit == cached_partial_index_by_page_.end()) continue;
                const auto& e = eit->second;
                cached_locals.push_back(cached_local{
                    .page_base                  = p,
                    .free_quantum_count         = e.free_quantum_count,
                    .largest_free_run_quantums  = e.largest_free_run_quantums,
                    .cache_epoch                = e.cache_epoch,
                    .kind                       = e.kind,
                    .heat_seq                   = e.heat_seq,
                });
            }
        }

        // ── Phase 1: batch-first candidate admission (037 plan §"Allocation
        //    Mode Selection" cost-tuple semantics).
        //
        // The cost contribution per page used:
        //   cached partial:        (0r, +1w, 0n)       — write only
        //   non-resident partial:  (+1r, +1w, 0n)      — read + write
        //   new whole page:        (0r, +1w, +1n)      — write + new
        //   round-local tail:      (0,  0,   0)        — already engaged
        //
        // Per-entry greedy on cached/NRP violates the batch cost tuple
        // (typical counter-example: a small cached candidate adds 1 write
        // without saving any fresh page, but greedy consumes it anyway).
        // Phase 1 admits candidates ONLY if they help the BATCH cost tuple
        // under the current mode; phase 2 sees just the admitted set.
        //
        // Decision rule per mode:
        //   NORMAL          (read, write, new, leftover):
        //     - NRP disabled (read is primary).
        //     - cached: greedy savings >= count. Each admitted page must,
        //       cumulatively, save at least one fresh page so total writes
        //       stay ≤ F_baseline.
        //   REUSE_PRESSURE  (write, new, read, leftover):
        //     - Same write-monotone rule as NORMAL for cached AND NRP.
        //       037 rule 1 explicitly forbids trading write for new.
        //   HARD_PRESSURE   (new, write, read, leftover):
        //     - Lookahead admission: admit cached/NRP until projected F
        //       reaches F_optimal (the lowest F achievable with all
        //       candidates). HARD allows trading +write for −new, so
        //       multi-page admission is required even when no individual
        //       page reduces F by itself (037 example B: 2 NRPs of 4
        //       quantums together absorb a full 8-quantum LBA, saving
        //       1 fresh page).

        // Helper: total sub-LBA need + write baselines.
        const uint64_t qpl = quantums_per_lba_;
        uint64_t total_sub_lba_need = 0;
        for (const allocation_request& r : reqs) {
            if (r.class_idx >= classes_.size()) continue;
            if (classes_[r.class_idx].class_size < cfg_.lba_size) {
                total_sub_lba_need += r.alloc_quantums;
            }
        }
        const uint64_t f_baseline =
            (total_sub_lba_need + qpl - 1) / qpl;
        const auto fresh_for_remaining = [&](uint64_t k_used) -> uint64_t {
            if (k_used >= total_sub_lba_need) return 0;
            return (total_sub_lba_need - k_used + qpl - 1) / qpl;
        };

        // ── Step 1.a: discover NRP candidates upfront (pressure modes
        //    only). HARD_PRESSURE admission needs the full candidate set
        //    visible together with cached so we can compute the joint
        //    f_optimal; REUSE_PRESSURE also benefits from the upfront set
        //    so phase-2 placement picks deterministic NRPs. ──
        struct nrp_local {
            paddr    page_base;
            uint16_t free_quantum_count;
            uint16_t largest_free_run_quantums;
        };
        std::vector<nrp_local> nrp_locals;
        if (mode != space_mode_t::normal) {
            const uint32_t cap = cfg_.max_candidate_pages;
            nrp_locals.reserve(cap);
            uint64_t bucket_cursor = nonempty_group_run_bucket_mask_;
            while (bucket_cursor != 0 && nrp_locals.size() < cap) {
                const uint32_t k = 63u - static_cast<uint32_t>(
                    std::countl_zero(bucket_cursor));
                bucket_cursor &= ~(1ULL << k);
                if (k == 0 || k >= quantums_per_lba_) continue;
                uint32_t group_id = groups_by_largest_run_[k];
                while (group_id != _vsm_detail::kInvalidGroupId
                       && nrp_locals.size() < cap) {
                    auto git = group_view_.find(group_id);
                    if (git == group_view_.end()) break;
                    const group_state* g = git->second;
                    uint32_t node_id = g->partial_pages.pages_by_largest_run[k];
                    while (node_id != _vsm_detail::kInvalidNodeId
                           && nrp_locals.size() < cap) {
                        const partial_page_node& nn =
                            g->partial_pages.nodes.at(node_id);
                        const uint64_t page_lba =
                            g->base_lba + static_cast<uint64_t>(nn.page_delta);
                        const paddr p{ cfg_.device_id, page_lba };
                        if (cached_partial_index_by_page_.find(p)
                                == cached_partial_index_by_page_.end()) {
                            nrp_locals.push_back(nrp_local{
                                .page_base                  = p,
                                .free_quantum_count         = nn.free_quantum_count,
                                .largest_free_run_quantums  = nn.largest_free_run_quantums,
                            });
                        }
                        node_id = nn.bucket_next;
                    }
                    group_id = g->global_run_links[k].next;
                }
            }
        }

        // Sort cached and NRP by free_quantum_count DESC so admission picks
        // highest-absorption pages first.
        std::sort(cached_locals.begin(), cached_locals.end(),
                  [](const cached_local& a, const cached_local& b) {
                      return a.free_quantum_count > b.free_quantum_count;
                  });
        std::sort(nrp_locals.begin(), nrp_locals.end(),
                  [](const nrp_local& a, const nrp_local& b) {
                      return a.free_quantum_count > b.free_quantum_count;
                  });

        // Read budget caps for NRP usage.
        const uint32_t nrp_read_budget_batch =
            (mode == space_mode_t::reuse_pressure)
              ? cfg_.reuse_pressure_max_prefill_reads_per_batch
              : (mode == space_mode_t::hard_pressure)
                  ? cfg_.hard_pressure_max_prefill_reads_per_batch
                  : 0;

        // ── Step 1.b: mode-specific admission. ──
        if (mode == space_mode_t::hard_pressure) {
            // Lookahead across cached + NRP combined: HARD's primary cost
            // dimension is new_pages, so we want to drive F to the joint
            // f_optimal. Within the joint set, prefer cached over NRP
            // because cached costs only +1w while NRP costs +1w +1r.
            uint64_t k_max_combined = 0;
            for (const auto& c : cached_locals) {
                k_max_combined += c.free_quantum_count;
                if (k_max_combined >= total_sub_lba_need) {
                    k_max_combined = total_sub_lba_need;
                    break;
                }
            }
            if (k_max_combined < total_sub_lba_need) {
                const std::size_t nrp_cap = std::min<std::size_t>(
                    nrp_locals.size(), nrp_read_budget_batch);
                for (std::size_t i = 0; i < nrp_cap; ++i) {
                    k_max_combined += nrp_locals[i].free_quantum_count;
                    if (k_max_combined >= total_sub_lba_need) {
                        k_max_combined = total_sub_lba_need;
                        break;
                    }
                }
            }
            const uint64_t f_optimal = fresh_for_remaining(k_max_combined);
            if (f_optimal >= f_baseline) {
                cached_locals.clear();
                nrp_locals.clear();
            } else {
                const uint64_t k_target =
                    (f_optimal == 0)
                        ? total_sub_lba_need
                        : (total_sub_lba_need - f_optimal * qpl);
                std::size_t cached_kept = 0;
                uint64_t    k_running   = 0;
                for (std::size_t i = 0; i < cached_locals.size(); ++i) {
                    if (k_running >= k_target) break;
                    if (i != cached_kept) cached_locals[cached_kept] = cached_locals[i];
                    k_running += cached_locals[cached_kept].free_quantum_count;
                    ++cached_kept;
                }
                cached_locals.resize(cached_kept);

                std::size_t nrp_kept = 0;
                const std::size_t nrp_cap = std::min<std::size_t>(
                    nrp_locals.size(), nrp_read_budget_batch);
                for (std::size_t i = 0; i < nrp_cap; ++i) {
                    if (k_running >= k_target) break;
                    if (i != nrp_kept) nrp_locals[nrp_kept] = nrp_locals[i];
                    k_running += nrp_locals[nrp_kept].free_quantum_count;
                    ++nrp_kept;
                }
                nrp_locals.resize(nrp_kept);
            }
        } else {
            // NORMAL / REUSE_PRESSURE: write-monotone admission. Each
            // admitted page must, cumulatively, save at least one fresh
            // page (savings >= count). NORMAL has no NRP at all.
            uint64_t    k_running   = 0;
            std::size_t cached_kept = 0;
            for (std::size_t i = 0; i < cached_locals.size(); ++i) {
                if (k_running >= total_sub_lba_need) break;
                const uint64_t k_with =
                    k_running + cached_locals[i].free_quantum_count;
                const uint64_t f_with     = fresh_for_remaining(k_with);
                const uint64_t writes_with = (cached_kept + 1) + f_with;
                if (writes_with > f_baseline) break;
                if (i != cached_kept) cached_locals[cached_kept] = cached_locals[i];
                ++cached_kept;
                k_running = k_with;
            }
            cached_locals.resize(cached_kept);

            if (mode == space_mode_t::reuse_pressure) {
                std::size_t nrp_kept = 0;
                const std::size_t nrp_cap = std::min<std::size_t>(
                    nrp_locals.size(), nrp_read_budget_batch);
                std::size_t total_pages_so_far = cached_kept;
                for (std::size_t i = 0; i < nrp_cap; ++i) {
                    if (k_running >= total_sub_lba_need) break;
                    const uint64_t k_with =
                        k_running + nrp_locals[i].free_quantum_count;
                    const uint64_t f_with = fresh_for_remaining(k_with);
                    const uint64_t writes_with =
                        (total_pages_so_far + 1) + f_with;
                    if (writes_with > f_baseline) break;
                    if (i != nrp_kept) nrp_locals[nrp_kept] = nrp_locals[i];
                    ++nrp_kept;
                    ++total_pages_so_far;
                    k_running = k_with;
                }
                nrp_locals.resize(nrp_kept);
            } else {
                nrp_locals.clear();
            }
        }

        struct round_tail_local {
            paddr    page_base;
            uint64_t free_quantum_bits;
            uint16_t free_quantum_count;
            uint16_t largest_free_run_quantums;
        };
        std::vector<round_tail_local> round_tails;

        std::vector<byte_claim> sorted_claims(reqs.size());
        bool ok = true;

        for (uint32_t sp = 0; ok && sp < sorted_idx.size(); ++sp) {
            const uint32_t orig_idx = sorted_idx[sp];
            const allocation_request& req = reqs[orig_idx];
            if (req.class_idx >= classes_.size()) { ok = false; break; }
            const space_class& cls = classes_[req.class_idx];

            // ── LBA-equal / multi-LBA path ──
            if (cls.class_size >= cfg_.lba_size) {
                const uint32_t span = cls.span_lbas;
                auto ext = carve_high_extent_(span);
                if (!ext) { ok = false; break; }
                round.fresh_extents.push_back(*ext);
                sorted_claims[sp] = byte_claim{
                    .page_base   = paddr{ cfg_.device_id, ext->base_lba },
                    .byte_offset = 0,
                    .alloc_bytes = req.alloc_bytes,
                    .class_idx   = req.class_idx,
                    .cache_epoch = 0,
                    .src         = byte_claim_source::new_whole_page,
                };
                continue;
            }

            // ── sub-LBA path ──
            const uint32_t need = req.alloc_quantums;
            if (need == 0 || need >= quantums_per_lba_) { ok = false; break; }

            // Retry loop: cached / NRP claim failures are treated as STALE
            // hints (the manager's index entry is out of sync with truth, or
            // the page transitioned full / all-free since admission).
            // Drop the stale entry from the local list and re-evaluate
            // placement options for THIS entry. Only fresh-carve exhaustion
            // is real space pressure that aborts the batch.
            std::optional<byte_claim> claim_opt;
            bool placed = false;
            while (ok && !placed) {
                // 1) cached candidates: 037 §"Cached Partial Selection"
                //    priority: active_tail kind > cached_free_candidate;
                //    then smallest leftover; then highest heat_seq.
                int      best_cached_i        = -1;
                uint8_t  best_cached_kind     = 2;
                uint16_t best_cached_leftover = UINT16_MAX;
                uint64_t best_cached_heat     = 0;
                for (uint32_t i = 0; i < cached_locals.size(); ++i) {
                    const auto& c = cached_locals[i];
                    if (c.largest_free_run_quantums < need) continue;
                    const uint8_t  k_rank   =
                        (c.kind == cached_partial_kind::active_tail) ? 0u : 1u;
                    const uint16_t leftover =
                        c.largest_free_run_quantums - need;
                    if (k_rank < best_cached_kind ||
                        (k_rank == best_cached_kind && leftover < best_cached_leftover) ||
                        (k_rank == best_cached_kind && leftover == best_cached_leftover
                            && c.heat_seq > best_cached_heat)) {
                        best_cached_kind     = k_rank;
                        best_cached_leftover = leftover;
                        best_cached_heat     = c.heat_seq;
                        best_cached_i        = static_cast<int>(i);
                    }
                }

                // 2) round-local tails best-fit by leftover.
                int      best_tail_i        = -1;
                uint16_t best_tail_leftover = UINT16_MAX;
                for (uint32_t i = 0; i < round_tails.size(); ++i) {
                    const auto& t = round_tails[i];
                    if (t.largest_free_run_quantums < need) continue;
                    const uint16_t leftover =
                        t.largest_free_run_quantums - need;
                    if (leftover < best_tail_leftover) {
                        best_tail_leftover = leftover;
                        best_tail_i        = static_cast<int>(i);
                    }
                }

                // round_tail strictly preferred over cached (037 §"Allocation
                // Mode Selection" — round_tail is already write-engaged).
                int placement_kind = -1; // 0=cached, 1=round_tail, 2=nrp, 3=fresh
                if (best_tail_i >= 0) {
                    placement_kind = 1;
                } else if (best_cached_i >= 0) {
                    placement_kind = 0;
                }

                // 3) NRP under pressure modes — only from admitted set.
                int      best_nrp_i        = -1;
                uint16_t best_nrp_leftover = UINT16_MAX;
                if (placement_kind == -1
                    && mode != space_mode_t::normal
                    && prefill_per_class_used[req.class_idx] < prefill_per_class_max) {
                    for (uint32_t i = 0; i < nrp_locals.size(); ++i) {
                        const auto& n = nrp_locals[i];
                        if (n.largest_free_run_quantums < need) continue;
                        const uint16_t leftover =
                            n.largest_free_run_quantums - need;
                        if (leftover < best_nrp_leftover) {
                            best_nrp_leftover = leftover;
                            best_nrp_i        = static_cast<int>(i);
                        }
                    }
                    if (best_nrp_i >= 0) placement_kind = 2;
                }

                // 4) Fresh page. Sub-LBA fresh always becomes partial →
                //    blocked under hard gate.
                if (placement_kind == -1) {
                    if (hard_gate_active) { ok = false; break; }
                    placement_kind = 3;
                }

                switch (placement_kind) {
                case 0: { // cached
                    auto& c = cached_locals[best_cached_i];
                    auto cm = claim_on_partial_(c.page_base,
                                                req.class_idx,
                                                need,
                                                req.alloc_bytes,
                                                c.cache_epoch,
                                                byte_claim_source::cached_partial);
                    if (!cm) {
                        // Stale hint: manager's index entry at this paddr
                        // (under this epoch) does not reflect the partial
                        // node truth. Erase from manager's index AND drop
                        // from this batch's local snapshot, then retry.
                        const paddr   stale_page  = c.page_base;
                        const uint64_t stale_epoch = c.cache_epoch;
                        cached_locals[best_cached_i] = cached_locals.back();
                        cached_locals.pop_back();
                        erase_cached_partial(stale_page, stale_epoch);
                        continue; // retry inner while
                    }
                    // claim_on_partial_ already updated the partial node and
                    // (via apply_node_bit_update_) the cached_partial_index.
                    // Refresh the LOCAL snapshot so the next iteration sees
                    // the up-to-date free_count / largest_run, or drops it
                    // if the page is no longer partial.
                    const uint32_t group_id = group_id_for_lba_(c.page_base.lba);
                    bool drop_local = true;
                    if (group_state* g = find_group_(group_id)) {
                        const uint32_t pd =
                            static_cast<uint32_t>(c.page_base.lba - g->base_lba);
                        auto pit = g->partial_pages.by_page_delta.find(pd);
                        if (pit != g->partial_pages.by_page_delta.end()) {
                            const partial_page_node& nn =
                                g->partial_pages.nodes.at(pit->second);
                            c.free_quantum_count        = nn.free_quantum_count;
                            c.largest_free_run_quantums = nn.largest_free_run_quantums;
                            drop_local = false;
                        }
                    }
                    if (drop_local) {
                        cached_locals[best_cached_i] = cached_locals.back();
                        cached_locals.pop_back();
                    }
                    claim_opt = cm;
                    placed    = true;
                    break;
                }
                case 1: { // round_tail
                    auto& t = round_tails[best_tail_i];
                    const uint32_t pos = _vsm_detail::choose_claim_offset(
                        t.free_quantum_bits, need,
                        t.largest_free_run_quantums, quantums_per_lba_);
                    const uint64_t mask =
                        _vsm_detail::quantum_range_mask(pos, need);
                    t.free_quantum_bits &= ~mask;
                    t.free_quantum_count =
                        static_cast<uint16_t>(t.free_quantum_count - need);
                    t.largest_free_run_quantums = static_cast<uint16_t>(
                        _vsm_detail::recompute_largest_run(
                            t.free_quantum_bits, quantums_per_lba_));
                    claim_opt = byte_claim{
                        .page_base   = t.page_base,
                        .byte_offset = pos * cfg_.value_space_quantum_bytes,
                        .alloc_bytes = req.alloc_bytes,
                        .class_idx   = req.class_idx,
                        .cache_epoch = 0,
                        .src         = byte_claim_source::new_whole_page,
                    };
                    if (t.free_quantum_count == 0) {
                        t = round_tails.back();
                        round_tails.pop_back();
                    }
                    placed = true;
                    break;
                }
                case 2: { // nrp from admitted set
                    auto& n = nrp_locals[best_nrp_i];
                    auto cm = claim_on_partial_(n.page_base,
                                                req.class_idx,
                                                need,
                                                req.alloc_bytes,
                                                0,
                                                byte_claim_source::nonresident_partial);
                    if (!cm) {
                        // NRP candidate is stale (page no longer partial or
                        // no fitting run). NRP has no manager-side index to
                        // erase — just drop from the local set and retry.
                        nrp_locals[best_nrp_i] = nrp_locals.back();
                        nrp_locals.pop_back();
                        continue; // retry inner while
                    }
                    prefill_per_class_used[req.class_idx] += 1;
                    // Refresh local NRP snapshot.
                    const uint32_t group_id = group_id_for_lba_(n.page_base.lba);
                    bool drop_local = true;
                    if (group_state* g = find_group_(group_id)) {
                        const uint32_t pd =
                            static_cast<uint32_t>(n.page_base.lba - g->base_lba);
                        auto pit = g->partial_pages.by_page_delta.find(pd);
                        if (pit != g->partial_pages.by_page_delta.end()) {
                            const partial_page_node& nn =
                                g->partial_pages.nodes.at(pit->second);
                            n.free_quantum_count        = nn.free_quantum_count;
                            n.largest_free_run_quantums = nn.largest_free_run_quantums;
                            drop_local = false;
                        }
                    }
                    if (drop_local) {
                        nrp_locals[best_nrp_i] = nrp_locals.back();
                        nrp_locals.pop_back();
                    }
                    claim_opt = cm;
                    placed    = true;
                    break;
                }
                case 3: { // fresh — only failure here is real space exhaustion
                    auto ext = carve_high_extent_(1);
                    if (!ext) { ok = false; break; }
                    round.fresh_extents.push_back(*ext);
                    const paddr  page_base{ cfg_.device_id, ext->base_lba };
                    const uint64_t init_bits =
                        _vsm_detail::valid_quantum_mask(quantums_per_lba_);
                    const uint32_t pos = 0;
                    const uint64_t mask =
                        _vsm_detail::quantum_range_mask(pos, need);
                    const uint64_t new_bits = init_bits & ~mask;
                    const uint16_t new_count =
                        static_cast<uint16_t>(quantums_per_lba_ - need);
                    const uint16_t new_largest = static_cast<uint16_t>(
                        _vsm_detail::recompute_largest_run(new_bits, quantums_per_lba_));
                    if (new_count > 0) {
                        round_tails.push_back(round_tail_local{
                            .page_base                  = page_base,
                            .free_quantum_bits          = new_bits,
                            .free_quantum_count         = new_count,
                            .largest_free_run_quantums  = new_largest,
                        });
                    }
                    claim_opt = byte_claim{
                        .page_base   = page_base,
                        .byte_offset = pos * cfg_.value_space_quantum_bytes,
                        .alloc_bytes = req.alloc_bytes,
                        .class_idx   = req.class_idx,
                        .cache_epoch = 0,
                        .src         = byte_claim_source::new_whole_page,
                    };
                    placed = true;
                    break;
                }
                } // switch
            } // while retry

            if (!ok) break;
            sorted_claims[sp] = *claim_opt;
        }

        if (!ok) {
            // Roll back partial state; round becomes reusable but empty.
            rollback_round_state_(round);
            return {};
        }

        // Publish round-local tails as pending_tail_pages on the round.
        for (const auto& t : round_tails) {
            round.unpublished_tails.push_back(pending_tail_page{
                .page_base                  = t.page_base,
                .free_quantum_bits          = t.free_quantum_bits,
                .free_quantum_count         = t.free_quantum_count,
                .largest_free_run_quantums  = t.largest_free_run_quantums,
            });
        }

        // Reorder claims back to original input order.
        std::vector<byte_claim> out(reqs.size());
        for (uint32_t sp = 0; sp < sorted_idx.size(); ++sp) {
            out[sorted_idx[sp]] = sorted_claims[sp];
        }

        // Append claims to the round in original order.
        round.claims.reserve(round.claims.size() + out.size());
        for (const byte_claim& c : out) {
            round.claims.push_back(c);
        }
        return out;
    }

    // ── rollback_round_state_ ─────────────────────────────────────────────

    inline void
    value_space_manager::rollback_round_state_(value_space_round& round) {
        // 1. Invert claims that landed on existing partial pages. Fresh
        //    claims (src == new_whole_page) are recovered wholesale via
        //    fresh_extents return below — their byte ranges have never been
        //    visible to other rounds, so we don't replay them here.
        for (const byte_claim& claim : round.claims) {
            if (claim.src == byte_claim_source::new_whole_page) continue;
            const uint32_t alloc_quantums =
                claim.alloc_bytes / cfg_.value_space_quantum_bytes;
            const uint32_t pos =
                claim.byte_offset / cfg_.value_space_quantum_bytes;
            assert(alloc_quantums >= 1);
            assert(alloc_quantums < quantums_per_lba_);
            const uint64_t mask =
                _vsm_detail::quantum_range_mask(pos, alloc_quantums);

            // Recreate the partial node if the claim had filled the page
            // (which freed the node). ensure_partial_node_ starts with empty
            // bits; the OR below installs the rolled-back range and migrates
            // to the appropriate bucket.
            ensure_partial_node_result enr =
                ensure_partial_node_(claim.page_base.lba);
            partial_page_node& n =
                enr.group->partial_pages.nodes.at(enr.node_id);
            const uint64_t new_bits = n.free_quantum_bits | mask;
            apply_node_bit_update_(enr.group,
                                    enr.group_id,
                                    enr.node_id,
                                    new_bits,
                                    +static_cast<int32_t>(alloc_quantums),
                                    claim.page_base.lba);
        }

        // 2. Return fresh_extents to global_free_extents wholesale. Any
        //    pending_tail_pages are inside these extents and are implicitly
        //    discarded.
        for (const reserved_extent& ext : round.fresh_extents) {
            return_extent_to_global_(ext.base_lba, ext.len_lbas);
        }

        round.claims.clear();
        round.fresh_extents.clear();
        round.unpublished_tails.clear();
    }

    // ── validate_value_ref_in_value_area_ ─────────────────────────────────

    inline void
    value_space_manager::validate_value_ref_in_value_area_(
        const paddr& base,
        uint16_t     byte_offset,
        uint32_t     len,
        const char*  context) const {
        if (base.device_id != cfg_.device_id) {
            throw std::invalid_argument(
                std::string(context) + ": base.device_id does not match "
                "manager device_id");
        }
        if (base.lba < cfg_.data_area_base_lba ||
            base.lba >= cfg_.data_area_end_lba) {
            throw std::invalid_argument(
                std::string(context) + ": base.lba outside "
                "[data_area_base_lba, data_area_end_lba)");
        }

        // canonical class lookup must succeed; an absent canonical class
        // means the byte range is not describable under the format-time
        // class table. Silently skipping it would leak free space (release)
        // or mark a live region as free (recovery).
        const auto class_idx_opt = canonical_class_idx_for_len_(len);
        if (!class_idx_opt) {
            throw std::invalid_argument(
                std::string(context) + ": no canonical class for value_ref.len "
                "(len exceeds the largest configured class_size)");
        }
        const space_class& cls = classes_[*class_idx_opt];

        if (cls.class_size < cfg_.lba_size) {
            // sub-LBA: byte_offset must be quantum-aligned and the slot must
            // not straddle the LBA boundary.
            if (byte_offset % cfg_.value_space_quantum_bytes != 0) {
                throw std::invalid_argument(
                    std::string(context) + ": sub-LBA byte_offset is not "
                    "aligned to value_space_quantum_bytes");
            }
            if (static_cast<uint32_t>(byte_offset) + cls.class_size
                    > cfg_.lba_size) {
                throw std::invalid_argument(
                    std::string(context) + ": sub-LBA "
                    "byte_offset + canonical_class_size exceeds lba_size");
            }
        } else {
            // LBA-equal / multi-LBA: byte_offset must be 0 and the span
            // must fit within the data area.
            if (byte_offset != 0) {
                throw std::invalid_argument(
                    std::string(context) + ": LBA-equal/multi-LBA "
                    "byte_offset must be 0");
            }
            // Avoid overflow: base.lba is already < data_area_end_lba, so
            // the sum is bounded.
            if (base.lba + cls.span_lbas > cfg_.data_area_end_lba) {
                throw std::invalid_argument(
                    std::string(context) + ": LBA-equal/multi-LBA span "
                    "exceeds data_area_end_lba");
            }
        }
    }

    // ── release_values ────────────────────────────────────────────────────
    //
    // 037 plan §"Reclaim" trust boundary: caller asserts each ref is dead
    // w.r.t. tree + outstanding read_handles. Manager does not perform
    // liveness defence (would be O(N) tree scan and unable to catch
    // coordination errors). It DOES however reject structurally malformed
    // refs — silently absorbing junk would leak free space and corrupt
    // partial-page metadata. Validation is two-pass so that a malformed
    // entry mid-batch does not leave the manager in a half-mutated state.

    inline void
    value_space_manager::release_values(std::span<const value_ref> refs) {
        // Pre-pass: validate every entry. fail-fast on first malformed.
        for (const value_ref& vr : refs) {
            validate_value_ref_in_value_area_(
                vr.base, vr.byte_offset, vr.len, "release_values");
        }
        // Process: every entry is structurally legal.
        for (const value_ref& vr : refs) {
            const auto class_idx_opt = canonical_class_idx_for_len_(vr.len);
            // Validation guarantees this lookup succeeds.
            const space_class& cls = classes_[*class_idx_opt];

            if (cls.class_size >= cfg_.lba_size) {
                return_extent_to_global_(vr.base.lba, cls.span_lbas);
                continue;
            }

            const uint32_t pos =
                vr.byte_offset / cfg_.value_space_quantum_bytes;
            const uint32_t alloc_quantums = cls.alloc_quantums;
            const uint64_t mask =
                _vsm_detail::quantum_range_mask(pos, alloc_quantums);

            ensure_partial_node_result enr = ensure_partial_node_(vr.base.lba);
            partial_page_node& n =
                enr.group->partial_pages.nodes.at(enr.node_id);
            const uint64_t new_bits = n.free_quantum_bits | mask;
            apply_node_bit_update_(enr.group,
                                    enr.group_id,
                                    enr.node_id,
                                    new_bits,
                                    +static_cast<int32_t>(alloc_quantums),
                                    vr.base.lba);
        }
    }

    // ── sync_alloc_floor ──────────────────────────────────────────────────

    inline alloc_floor_sync_result
    value_space_manager::sync_alloc_floor(
        uint64_t                       new_alloc_floor_lba,
        std::span<const free_extent>  tree_returned_extents) {
        if (new_alloc_floor_lba > cfg_.data_area_end_lba) {
            return alloc_floor_sync_result{
                alloc_floor_sync_status::code::rejected_collision,
                acknowledged_alloc_floor_lba_,
            };
        }

        // Validate tree_returned_extents fall above new_floor (037 plan
        // §"地址空间" rule 8). All-or-nothing: a single bad extent rejects
        // the whole sync.
        for (const free_extent& fe : tree_returned_extents) {
            if (fe.base_lba < new_alloc_floor_lba) {
                return alloc_floor_sync_result{
                    alloc_floor_sync_status::code::rejected_collision,
                    acknowledged_alloc_floor_lba_,
                };
            }
            if (fe.len_lbas == 0) {
                return alloc_floor_sync_result{
                    alloc_floor_sync_status::code::rejected_collision,
                    acknowledged_alloc_floor_lba_,
                };
            }
        }

        if (new_alloc_floor_lba > acknowledged_alloc_floor_lba_) {
            // Raise: tree wants to claim (acked_floor, new_alloc_floor_lba].
            // 037 plan §"地址空间" rule 3 enumerates what value still owns:
            // live values (full pages — implicit), partial pages, dirty/open
            // frames, trim_inflight, and ranges already claimed by an earlier
            // (still inflight) round. Each of those is an LBA that is NOT in
            // global_free_extents:
            //   * partial pages          → carved out + tracked in partial_pages
            //   * trim_inflight          → carved out + tracked in trim_inflight_
            //   * inflight round fresh   → carved out + held by the round
            //   * full allocated pages   → carved out + IMPLICIT (no map at all)
            // Rather than enumerate each carrier separately (and miss the
            // implicit one), the safe check is a coverage equation: the
            // entire (old_floor, new_floor] interval must be 100% covered by
            // free_extents. Any gap means we still own something there.
            const uint64_t old_floor   = acknowledged_alloc_floor_lba_;
            const uint64_t target_lbas = new_alloc_floor_lba - old_floor;
            uint64_t free_lbas_in_range = 0;

            auto it = global_free_extents_.lower_bound(old_floor);
            if (it != global_free_extents_.begin()) {
                auto prev = std::prev(it);
                const uint64_t pend = prev->first + prev->second;
                if (pend > old_floor) {
                    const uint64_t lo = std::max<uint64_t>(prev->first, old_floor);
                    const uint64_t hi = std::min<uint64_t>(pend, new_alloc_floor_lba);
                    if (hi > lo) free_lbas_in_range += hi - lo;
                }
            }
            for (; it != global_free_extents_.end(); ++it) {
                if (it->first >= new_alloc_floor_lba) break;
                const uint64_t lo = std::max<uint64_t>(it->first, old_floor);
                const uint64_t hi = std::min<uint64_t>(
                    it->first + it->second, new_alloc_floor_lba);
                if (hi > lo) free_lbas_in_range += hi - lo;
            }

            if (free_lbas_in_range != target_lbas) {
                return alloc_floor_sync_result{
                    alloc_floor_sync_status::code::rejected_collision,
                    acknowledged_alloc_floor_lba_,
                };
            }

            // Accept. Free extents in (-inf, new_floor) are now tree's;
            // erase / split out the slice strictly below new_floor. This
            // also handles any stale below-old_floor extent that the doc
            // explicitly tolerates during a pending request window.
            while (!global_free_extents_.empty()) {
                auto fit = global_free_extents_.begin();
                const uint64_t base = fit->first;
                const uint64_t end_excl = base + fit->second;
                if (end_excl <= new_alloc_floor_lba) {
                    whole_free_lba_count_ -= fit->second;
                    global_free_extents_.erase(fit);
                    continue;
                }
                if (base < new_alloc_floor_lba) {
                    const uint32_t cleared =
                        static_cast<uint32_t>(new_alloc_floor_lba - base);
                    const uint32_t survivor_len =
                        static_cast<uint32_t>(end_excl - new_alloc_floor_lba);
                    global_free_extents_.erase(fit);
                    global_free_extents_.emplace(new_alloc_floor_lba, survivor_len);
                    whole_free_lba_count_ -= cleared;
                }
                break;
            }
        }

        // Merge tree_returned_extents.
        for (const free_extent& fe : tree_returned_extents) {
            return_extent_to_global_(fe.base_lba, fe.len_lbas);
        }

        acknowledged_alloc_floor_lba_ = new_alloc_floor_lba;
        return alloc_floor_sync_result{
            alloc_floor_sync_status::code::accepted,
            acknowledged_alloc_floor_lba_,
        };
    }

    // ── prepare_trim / complete_trim ──────────────────────────────────────

    inline trim_plan
    value_space_manager::prepare_trim(uint32_t max_ranges, uint32_t max_lbas) {
        // Defer plan_id assignment until we know there is something to
        // track. Idle prepare_trim calls (zero caps, no eligible extents)
        // must NOT burn a sequence number and must NOT register an entry
        // in trim_inflight_ — caller signals idle by skipping
        // complete_trim, so leaving an empty entry behind would leak
        // forever.
        trim_plan plan;
        plan.id = trim_plan_id{ 0 };
        if (max_ranges == 0 || max_lbas == 0) return plan;

        const uint64_t floor = effective_alloc_floor_for_owner_();

        uint32_t taken_lbas = 0;
        for (auto it = global_free_extents_.begin();
             it != global_free_extents_.end() && plan.ranges.size() < max_ranges
             && taken_lbas < max_lbas; ) {
            const uint64_t base = it->first;
            const uint32_t len  = it->second;

            // Skip below-floor stale extents: they cannot be trimmed (037
            // §"TRIM" rule 1: trim eligibility requires base_lba ≥ acked
            // floor).
            if (base < floor) {
                ++it;
                continue;
            }

            const uint32_t want = std::min(len, max_lbas - taken_lbas);
            if (want == len) {
                plan.ranges.push_back(trim_range{ base, len });
                taken_lbas += len;
                it = global_free_extents_.erase(it);
            } else {
                // Carve high-end suffix of `want` LBAs out of this extent.
                const uint64_t carved_base = base + (len - want);
                const uint32_t leftover    = static_cast<uint32_t>(len - want);
                it->second = leftover;
                plan.ranges.push_back(trim_range{ carved_base, want });
                taken_lbas += want;
                ++it;
            }
        }

        if (plan.ranges.empty()) {
            // Nothing eligible — keep plan_id == 0 so the caller can tell
            // this is an idle plan and skip complete_trim. No state
            // mutation is necessary; whole_free_lba_count_ unchanged.
            return plan;
        }

        whole_free_lba_count_ -= taken_lbas;

        plan.id = trim_plan_id{ next_trim_plan_seq_++ };
        trim_inflight_entry entry;
        entry.ranges = plan.ranges;
        trim_inflight_.emplace(plan.id.raw, std::move(entry));
        return plan;
    }

    inline void
    value_space_manager::complete_trim(trim_plan_id id, bool /* ok */) {
        // ok is purely informational; whether the underlying NVMe TRIM
        // succeeded or failed, the LBA range remains logically free and
        // returns to global_free_extents for re-allocation.
        auto it = trim_inflight_.find(id.raw);
        if (it == trim_inflight_.end()) return;
        for (const trim_range& tr : it->second.ranges) {
            return_extent_to_global_(tr.lba, tr.len_lbas);
        }
        trim_inflight_.erase(it);
    }

    // ── install_recovered_state ───────────────────────────────────────────
    //
    // 037 plan §"Recovery": rebuild free-space + partial metadata as the
    // complement of `live_extents`. Never reads Value Area payload pages.
    // Hints are advisory and ignored here — losing them must not cause free
    // space leaks, so the algorithm derives everything from live extents
    // alone.

    inline void
    value_space_manager::install_recovered_state(
        std::span<const live_value_extent> live_extents,
        uint64_t                           tree_alloc_head_lba,
        uint64_t                           data_area_end_lba,
        std::span<const dead_class_hint>   /* hints */) {
        if (data_area_end_lba != cfg_.data_area_end_lba) {
            throw std::invalid_argument(
                "value_space_manager::install_recovered_state: "
                "data_area_end_lba mismatch with config");
        }
        if (tree_alloc_head_lba < cfg_.data_area_base_lba ||
            tree_alloc_head_lba > cfg_.data_area_end_lba) {
            throw std::invalid_argument(
                "value_space_manager::install_recovered_state: "
                "tree_alloc_head_lba out of data area");
        }

        // Pre-pass: validate every live extent BEFORE clearing state. A
        // silently-skipped malformed live extent would mark a live byte
        // range as free, producing tree/value collision after recovery.
        // Throwing here leaves the previous state intact so the caller
        // can correct the recovery input and retry.
        for (const live_value_extent& lve : live_extents) {
            validate_value_ref_in_value_area_(
                lve.base, lve.byte_offset, lve.len,
                "install_recovered_state");
            // Additional recovery-specific guard: live extents must lie
            // entirely above tree_alloc_head_lba (they belong to the value
            // area, not tree's claimed region).
            if (lve.base.lba < tree_alloc_head_lba) {
                throw std::invalid_argument(
                    "install_recovered_state: live_value_extent.base.lba "
                    "below tree_alloc_head_lba");
            }
        }

        // ── Reset all owner state to a clean slate. ──
        global_free_extents_.clear();
        whole_free_lba_count_           = 0;
        group_directory_.clear();
        group_view_.clear();
        for (uint32_t& h : groups_by_largest_run_) {
            h = _vsm_detail::kInvalidGroupId;
        }
        nonempty_group_run_bucket_mask_ = 0;
        partial_page_count_total_       = 0;
        cached_partial_index_by_page_.clear();
        cached_partial_by_score_.clear();
        cached_partial_estimated_bytes_ = 0;
        trim_inflight_.clear();
        next_trim_plan_seq_             = 1;
        current_space_mode_             = space_mode_t::normal;
        acknowledged_alloc_floor_lba_   = tree_alloc_head_lba;

        // ── Bucket live extents into (a) wholly-occupied LBA ranges and
        //    (b) sub-LBA occupied bitmaps per LBA. ──
        std::vector<std::pair<uint64_t, uint64_t>> non_free_intervals;
        std::vector<std::pair<uint64_t, uint64_t>> sub_lba_occupied_per_lba;
        non_free_intervals.reserve(live_extents.size());
        sub_lba_occupied_per_lba.reserve(live_extents.size());

        for (const live_value_extent& lve : live_extents) {
            // Validation pre-pass guarantees canonical class lookup, byte
            // alignment, and span fitness — process unconditionally.
            const space_class& cls =
                classes_[*canonical_class_idx_for_len_(lve.len)];
            if (cls.class_size < cfg_.lba_size) {
                const uint32_t pos =
                    lve.byte_offset / cfg_.value_space_quantum_bytes;
                const uint32_t q = cls.alloc_quantums;
                const uint64_t mask =
                    _vsm_detail::quantum_range_mask(pos, q);
                non_free_intervals.emplace_back(lve.base.lba, lve.base.lba + 1);
                sub_lba_occupied_per_lba.emplace_back(lve.base.lba, mask);
            } else {
                const uint64_t base = lve.base.lba;
                const uint64_t end_excl = base + cls.span_lbas;
                non_free_intervals.emplace_back(base, end_excl);
            }
        }

        // ── Coalesce non_free_intervals so the gap-walk runs in O(N). ──
        std::sort(non_free_intervals.begin(), non_free_intervals.end());
        std::vector<std::pair<uint64_t, uint64_t>> merged;
        merged.reserve(non_free_intervals.size());
        for (const auto& iv : non_free_intervals) {
            if (!merged.empty() && iv.first <= merged.back().second) {
                merged.back().second = std::max(merged.back().second, iv.second);
            } else {
                merged.push_back(iv);
            }
        }

        // ── Walk gaps in [tree_alloc_head_lba, data_area_end_lba). ──
        uint64_t cursor = tree_alloc_head_lba;
        for (const auto& iv : merged) {
            const uint64_t lo = std::max(iv.first, cursor);
            const uint64_t hi = std::min(iv.second, data_area_end_lba);
            if (hi <= cursor) continue;
            if (lo > cursor) {
                const uint32_t len = static_cast<uint32_t>(lo - cursor);
                global_free_extents_.emplace(cursor, len);
                whole_free_lba_count_ += len;
            }
            if (hi > cursor) cursor = hi;
            if (cursor >= data_area_end_lba) break;
        }
        if (cursor < data_area_end_lba) {
            const uint32_t len = static_cast<uint32_t>(data_area_end_lba - cursor);
            global_free_extents_.emplace(cursor, len);
            whole_free_lba_count_ += len;
        }

        // ── Aggregate sub-LBA occupancy per LBA, then materialize partial
        //    nodes for LBAs that still have any free quantums. ──
        std::sort(sub_lba_occupied_per_lba.begin(),
                  sub_lba_occupied_per_lba.end());
        std::size_t i = 0;
        while (i < sub_lba_occupied_per_lba.size()) {
            const uint64_t lba = sub_lba_occupied_per_lba[i].first;
            uint64_t occupied = 0;
            while (i < sub_lba_occupied_per_lba.size()
                   && sub_lba_occupied_per_lba[i].first == lba) {
                occupied |= sub_lba_occupied_per_lba[i].second;
                ++i;
            }
            const uint64_t valid =
                _vsm_detail::valid_quantum_mask(quantums_per_lba_);
            const uint64_t free_bits = (~occupied) & valid;
            const uint32_t free_count = std::popcount(free_bits);
            if (free_count == 0) continue;
            ensure_partial_node_result enr = ensure_partial_node_(lba);
            apply_node_bit_update_(enr.group,
                                    enr.group_id,
                                    enr.node_id,
                                    free_bits,
                                    +static_cast<int32_t>(free_count),
                                    lba);
        }
    }

} // namespace apps::inconel::value

#endif // APPS_INCONEL_VALUE_SPACE_MANAGER_HH
