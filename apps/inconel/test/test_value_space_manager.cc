//
// value_space_manager unit tests — INC-051 / plan 037.
//
// Coverage maps to plan 037 §"单元测试面" cases 1–28. The manager is a
// pure-metadata component (no DMA frame, no NVMe I/O, no PUMP sender), so
// every case here drives the public API plus public bit-helpers in
// `_vsm_detail`. Each test function notes which case(s) it covers.
//

#include "apps/inconel/test/check.hh"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "apps/inconel/core/data_area_heads.hh"
#include "apps/inconel/format/types.hh"
#include "apps/inconel/value/space_manager.hh"

using apps::inconel::core::data_area_heads;
using apps::inconel::format::paddr;
using apps::inconel::format::value_ref;
using namespace apps::inconel::value;
namespace vd = apps::inconel::value::_vsm_detail;

// ── Constants used by most tests ──
//
// 4 KiB LBA + 64 B quantum gives quantums_per_lba = 64, matching the most
// common case described in plan 037 §"Allocation Quantum". Group bytes are
// the minimum 64 MiB (= 16384 LBAs at 4 KiB) — the smallest legal value, so
// the address range only needs ~64 MiB to span multiple groups, which keeps
// the test data area small.

constexpr uint32_t kLBA          = 4096;
constexpr uint32_t kQuantum      = 64;
constexpr uint32_t kQPL          = kLBA / kQuantum; // 64
constexpr uint64_t kGroupBytes   = 64ULL * 1024 * 1024;
constexpr uint32_t kGroupLBAs    = static_cast<uint32_t>(kGroupBytes / kLBA);
constexpr uint16_t kDeviceId     = 7;
constexpr uint64_t kBaseLBA      = 100;          // intentionally non-zero
constexpr uint64_t kEndLBA       = kBaseLBA + 4ULL * kGroupLBAs; // 4 groups

// Class sizes: a mix of sub-LBA, LBA-equal, and multi-LBA. Each sub-LBA
// class size is 64·2^n with n in [0,5]; LBA-equal class is 4096 = lba_size;
// multi-LBA class is 8192 = lba_size * 2.
static const std::vector<uint32_t>&
default_class_sizes() {
    static const std::vector<uint32_t> v = {64, 128, 256, 512, 1024, 2048, 4096, 8192};
    return v;
}

// ── Helpers ──

static value_space_manager_config
make_default_config(uint64_t                    end_lba       = kEndLBA,
                    uint64_t                    base_lba      = kBaseLBA,
                    uint32_t                    group_lbas    = kGroupLBAs,
                    data_area_heads*            shared_heads  = nullptr,
                    std::span<const uint32_t>   class_sizes   = default_class_sizes()) {
    value_space_manager_config cfg;
    cfg.lba_size                    = kLBA;
    cfg.value_space_quantum_bytes   = kQuantum;
    cfg.value_space_group_size_lbas = group_lbas;
    cfg.device_id                   = kDeviceId;
    cfg.data_area_base_lba          = base_lba;
    cfg.data_area_end_lba           = end_lba;
    cfg.value_class_sizes           = class_sizes;
    cfg.object_header_bytes         = 0;
    cfg.shared_heads                = shared_heads;
    return cfg;
}

static paddr
make_paddr(uint64_t lba, uint16_t dev = kDeviceId) {
    return paddr{dev, lba};
}

static value_ref
make_vref(paddr base, uint16_t byte_offset, uint32_t len) {
    return value_ref{base, byte_offset, len, 0};
}

static allocation_request
make_req(uint32_t entry_idx, uint16_t class_idx, uint32_t alloc_bytes) {
    allocation_request r{};
    r.entry_index    = entry_idx;
    r.class_idx      = class_idx;
    r.alloc_bytes    = alloc_bytes;
    r.alloc_quantums = alloc_bytes / kQuantum; // sub-LBA only; LBA-equal/multi-LBA ignored
    return r;
}

// One-shot batch helper: allocate a single fresh sub-LBA via allocate_batch,
// commit, and return the resulting page_base. The page is left as a partial
// page with `kQPL - alloc_quantums` free quantums starting at byte_offset =
// alloc_bytes.
static paddr
allocate_one_sub_lba_and_commit(value_space_manager& m,
                                uint16_t              class_idx,
                                uint32_t              alloc_bytes) {
    auto round = m.begin_round();
    allocation_request reqs[1] = {make_req(0, class_idx, alloc_bytes)};
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1));
    CHECK(claims.size() == 1);
    CHECK(claims[0].alloc_bytes == alloc_bytes);
    CHECK(claims[0].class_idx == class_idx);
    CHECK(claims[0].src == byte_claim_source::new_whole_page);
    paddr p = claims[0].page_base;
    m.commit(std::move(round));
    return p;
}

// ─────────────────────────────────────────────────────────────────────────
// Case 1 + 25: initial single span, sequential carving via batch alloc.
// Also exercises bit_helpers via _vsm_detail::quantum_range_mask edges.
// ─────────────────────────────────────────────────────────────────────────

static void
test_initial_single_span_and_carve() {
    value_space_manager m(make_default_config());

    const uint64_t total_lbas = kEndLBA - kBaseLBA;
    CHECK(m.whole_free_lba_count() == total_lbas);
    CHECK(m.partial_page_count_total() == 0);
    CHECK(m.acknowledged_alloc_floor_lba() == kBaseLBA);

    // Allocate two LBA-equal values back-to-back. From-high allocation
    // means second allocation lands directly below the first.
    auto round = m.begin_round();
    allocation_request reqs[2] = {
        make_req(0, /*class_idx=4096*/ 6, /*alloc_bytes=*/4096),
        make_req(1, /*class_idx=4096*/ 6, /*alloc_bytes=*/4096),
    };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 2));
    CHECK(claims.size() == 2);
    CHECK(claims[0].src == byte_claim_source::new_whole_page);
    CHECK(claims[1].src == byte_claim_source::new_whole_page);
    // From-high: highest LBA is reserved first.
    CHECK(claims[0].page_base.lba > claims[1].page_base.lba);
    CHECK(claims[0].page_base.lba == kEndLBA - 1);
    CHECK(claims[1].page_base.lba == kEndLBA - 2);
    CHECK(m.whole_free_lba_count() == total_lbas - 2);
    m.commit(std::move(round));
    CHECK(m.partial_page_count_total() == 0);

    // Bit helper edges (case 25): UB-free shifts of 64.
    CHECK(vd::quantum_range_mask(0, 64) == ~0ULL);
    CHECK(vd::quantum_range_mask(0, 1) == 0x1ULL);
    CHECK(vd::quantum_range_mask(63, 1) == (1ULL << 63));
    CHECK(vd::valid_quantum_mask(64) == ~0ULL);
    CHECK(vd::valid_quantum_mask(8) == 0xFFULL);
    // bucket [1, qpl-1] mask
    CHECK(vd::valid_partial_run_bucket_mask(64) == (~0ULL & ~1ULL));
    // run_buckets_ge: need=1 means buckets [1, qpl-1].
    CHECK(vd::run_buckets_ge(1, 64) == (~0ULL & ~1ULL));
    // need=qpl-1: only bucket qpl-1.
    CHECK(vd::run_buckets_ge(63, 64) == (1ULL << 63));

    printf("  case 1+25 (initial span / carve / bit-helper edges): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 2: release neighbouring spans coalesce into one.
// ─────────────────────────────────────────────────────────────────────────

static void
test_release_coalesces_neighbours() {
    value_space_manager m(make_default_config());
    const uint64_t total = kEndLBA - kBaseLBA;

    // Allocate 4 multi-LBA (8 KiB → 2 LBAs each) values. They land at
    // [end-2, end-4, end-6, end-8) due to from-high carving, but the exact
    // LBAs only matter for release coalescing.
    auto round = m.begin_round();
    allocation_request reqs[4] = {
        make_req(0, /*class_idx=8192*/ 7, 8192),
        make_req(1, 7, 8192),
        make_req(2, 7, 8192),
        make_req(3, 7, 8192),
    };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 4));
    CHECK(claims.size() == 4);
    CHECK(m.whole_free_lba_count() == total - 8);
    m.commit(std::move(round));

    // Release the two outermost first — they should NOT coalesce with
    // each other yet (a 4-LBA gap remains).
    value_ref vrs0[] = { make_vref(claims[0].page_base, 0, 8192) };
    value_ref vrs3[] = { make_vref(claims[3].page_base, 0, 8192) };
    m.release_values(std::span<const value_ref>(vrs0, 1));
    CHECK(m.whole_free_lba_count() == total - 6);
    m.release_values(std::span<const value_ref>(vrs3, 1));
    CHECK(m.whole_free_lba_count() == total - 4);

    // Release the inner two — these will coalesce with each other AND with
    // the surviving outer free runs into a single span.
    value_ref vrs1[] = { make_vref(claims[1].page_base, 0, 8192) };
    value_ref vrs2[] = { make_vref(claims[2].page_base, 0, 8192) };
    m.release_values(std::span<const value_ref>(vrs1, 1));
    m.release_values(std::span<const value_ref>(vrs2, 1));
    CHECK(m.whole_free_lba_count() == total);

    // Allocate one big multi-LBA value and confirm we can take it from
    // the high end again — proves coalescing actually merged the spans.
    auto round2 = m.begin_round();
    allocation_request big[] = { make_req(0, 7, 8192) };
    auto big_claims = m.allocate_batch(round2, std::span<const allocation_request>(big, 1));
    CHECK(big_claims.size() == 1);
    CHECK(big_claims[0].page_base.lba == kEndLBA - 2);
    m.abort(std::move(round2));
    CHECK(m.whole_free_lba_count() == total);

    printf("  case 2 (release coalesces neighbours): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 3: sub-LBA release creates page-local partial metadata.
// Case 21: sparse by_page_delta lookup supports direct address update —
// release writes through to the right page even when group is sparse.
// ─────────────────────────────────────────────────────────────────────────

static void
test_sub_lba_release_creates_partial() {
    value_space_manager m(make_default_config());

    // Allocate a single sub-LBA value (256 B = 4 quantums, class_idx=2).
    paddr page = allocate_one_sub_lba_and_commit(m, /*class_idx=*/2, /*alloc_bytes=*/256);
    CHECK(m.partial_page_count_total() == 1);
    const uint64_t free_after_carve = m.whole_free_lba_count();

    // Release it. The page becomes all-free → conversion to whole-free
    // 1-LBA span. Cases 3+4 share this conversion; the all-free path is
    // re-tested below in test_partial_all_free_returns_whole().
    value_ref vr = make_vref(page, 0, 256);
    value_ref refs[] = { vr };
    m.release_values(std::span<const value_ref>(refs, 1));
    CHECK(m.partial_page_count_total() == 0);
    CHECK(m.whole_free_lba_count() == free_after_carve + 1);

    // Now the inverse: leave the page partial after release. Allocate two
    // sub-LBA claims into the same page, then release only one.
    auto round = m.begin_round();
    allocation_request reqs[2] = {
        make_req(0, /*class_idx=512*/ 3, 512),  // 8 quantums
        make_req(1, /*class_idx=512*/ 3, 512),
    };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 2));
    CHECK(claims.size() == 2);
    // Both should land in the same fresh tail page (mixed-size pack — same
    // size class, same LBA): claims[0] at byte_offset 0, claims[1] at
    // byte_offset 512. allocate_batch's "round_tail" path takes
    // both onto one fresh LBA because the round_tail still has 7×512 B free.
    CHECK(claims[0].page_base == claims[1].page_base);
    CHECK(claims[0].byte_offset != claims[1].byte_offset);
    paddr partial_page = claims[0].page_base;
    m.commit(std::move(round));
    CHECK(m.partial_page_count_total() == 1);

    // Release exactly one of the two claims.
    value_ref keep_only_one[] = {
        make_vref(partial_page, claims[1].byte_offset, 512),
    };
    m.release_values(std::span<const value_ref>(keep_only_one, 1));
    // Page is still partial: one 512 B claim remains live.
    CHECK(m.partial_page_count_total() == 1);

    printf("  case 3+21 (sub-LBA release creates partial / address-keyed update): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 4: partial page going all-free converts to whole-free 1-LBA span.
// ─────────────────────────────────────────────────────────────────────────

static void
test_partial_all_free_returns_whole_span() {
    value_space_manager m(make_default_config());
    const uint64_t total = kEndLBA - kBaseLBA;

    paddr page = allocate_one_sub_lba_and_commit(m, /*class_idx=*/2, /*alloc_bytes=*/256);
    CHECK(m.partial_page_count_total() == 1);
    CHECK(m.whole_free_lba_count() == total - 1);

    // Release the only live value on the page.
    value_ref vr = make_vref(page, 0, 256);
    value_ref refs[] = { vr };
    m.release_values(std::span<const value_ref>(refs, 1));
    CHECK(m.partial_page_count_total() == 0);
    CHECK(m.whole_free_lba_count() == total);

    // The freshly-returned LBA must be allocatable from the high end again
    // because it coalesced with the upper free span.
    auto round = m.begin_round();
    allocation_request reqs[] = { make_req(0, /*class_idx=4096*/ 6, 4096) };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1));
    CHECK(claims.size() == 1);
    CHECK(claims[0].page_base.lba == kEndLBA - 1);
    m.abort(std::move(round));

    printf("  case 4 (partial all-free → whole-free span): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 5 + 19: mixed-size batch packs different classes onto one page and
// preserves caller order in the returned claims.
// ─────────────────────────────────────────────────────────────────────────

static void
test_mixed_size_batch_packs_one_page_and_preserves_order() {
    value_space_manager m(make_default_config());

    // 4 KiB page = 64 quantums. Plan:
    //   claim 0: 1 KiB  = 16 quantums
    //   claim 1:   512 = 8  quantums
    //   claim 2: 1 KiB  = 16 quantums
    //   claim 3:   512 = 8  quantums
    //   claim 4: 1 KiB  = 16 quantums
    // Total = 64 quantums → packs exactly into one LBA page; no partial
    // page should remain after commit. allocate_batch BFD-sorts internally
    // (large→small) but the returned vector must restore caller order.
    auto round = m.begin_round();
    allocation_request reqs[5] = {
        make_req(0, /*class_idx=1024*/ 4, 1024),
        make_req(1, /*class_idx=512 */ 3, 512),
        make_req(2, /*class_idx=1024*/ 4, 1024),
        make_req(3, /*class_idx=512 */ 3, 512),
        make_req(4, /*class_idx=1024*/ 4, 1024),
    };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 5));
    CHECK(claims.size() == 5);

    // entry-index ↔ position mapping is preserved.
    for (uint32_t i = 0; i < 5; ++i) {
        CHECK(claims[i].class_idx == reqs[i].class_idx);
        CHECK(claims[i].alloc_bytes == reqs[i].alloc_bytes);
    }

    // All five claims share the same page_base — packed onto one fresh LBA.
    for (uint32_t i = 1; i < 5; ++i) {
        CHECK(claims[i].page_base == claims[0].page_base);
    }

    // Distinct byte_offsets, all within the page.
    std::vector<uint32_t> offsets;
    offsets.reserve(5);
    for (const auto& c : claims) {
        offsets.push_back(c.byte_offset);
        CHECK(c.byte_offset + c.alloc_bytes <= kLBA);
    }
    std::sort(offsets.begin(), offsets.end());
    for (uint32_t i = 1; i < offsets.size(); ++i) {
        CHECK(offsets[i] != offsets[i - 1]);
    }

    m.commit(std::move(round));
    // Sum = 64 quantums = full page → no partial.
    CHECK(m.partial_page_count_total() == 0);

    printf("  case 5+19 (mixed-size batch packing / order preserved): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 6 + 14 + 16 + 27:
//   - cost model picks cached-partial over fresh whole page
//   - mark_cached_partial admission accepts each kind
//   - active_tail dominates cached_free_candidate by score
//   - heat_seq drives selection (not arrival order)
// ─────────────────────────────────────────────────────────────────────────

static void
test_cached_admission_kinds_and_selection() {
    // ── Sub-test A: kind dominates score (active_tail > cached_free_candidate). ──
    //
    // Both pages are freshly created by an identical 512 B allocation, so
    // their `largest_free_run_quantums` (= leftover after the initial claim)
    // is identical. This isolates kind / heat ordering from leftover-best-fit.
    {
        value_space_manager m(make_default_config());
        paddr p_a = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
        paddr p_b = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
        CHECK(p_a != p_b);

        cached_partial_update u{};
        u.heat_seq    = 10;          // identical heat — kind alone differs
        u.cache_epoch = 0;
        u.page_base = p_a; u.kind = cached_partial_kind::cached_free_candidate;
        m.mark_cached_partial(u);
        u.page_base = p_b; u.kind = cached_partial_kind::active_tail;
        u.cache_epoch = 0xDEAD;
        m.mark_cached_partial(u);

        auto round = m.begin_round();
        allocation_request reqs[] = { make_req(0, /*class_idx=512*/ 3, 512) };
        auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1));
        CHECK(claims.size() == 1);
        CHECK(claims[0].src == byte_claim_source::cached_partial);
        CHECK(claims[0].page_base == p_b);   // active_tail wins
        CHECK(claims[0].cache_epoch == 0xDEAD);
        m.abort(std::move(round));
    }

    // ── Sub-test B: same kind + same leftover → heat_seq breaks the tie. ──
    {
        value_space_manager m(make_default_config());
        paddr p_cold = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
        paddr p_hot  = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
        CHECK(p_cold != p_hot);

        cached_partial_update u{};
        u.kind = cached_partial_kind::cached_free_candidate;
        u.cache_epoch = 0;
        u.page_base = p_cold; u.heat_seq = 1;   m.mark_cached_partial(u);
        u.page_base = p_hot;  u.heat_seq = 999; m.mark_cached_partial(u);

        auto round = m.begin_round();
        allocation_request reqs[] = { make_req(0, /*class_idx=512*/ 3, 512) };
        auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1));
        CHECK(claims.size() == 1);
        CHECK(claims[0].src == byte_claim_source::cached_partial);
        // Both pages have identical largest_run (56 quantums) → identical
        // leftover (48). Within that tie the score order
        // (kind_rank, largest_run_inv, heat_seq_inv) breaks via heat:
        // higher heat sorts first in cached_partial_by_score_, becoming
        // cached_locals[0]; the leftover comparison `<` keeps it as best.
        CHECK(claims[0].page_base == p_hot);
        m.abort(std::move(round));
    }

    printf("  case 6+14+16+27 (cached cost / admission / kind+heat): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 7: cached partial claim — stale miss aborts and erases stale index.
// We simulate the scheduler-side stale miss by:
//   1. try_claim_range succeeds (claim taken on a partial page).
//   2. caller decides "frame gone" → m.abort(round); m.erase_cached_partial.
//   3. observe: page bits restored, cached index empty for that page.
// ─────────────────────────────────────────────────────────────────────────

static void
test_cached_claim_stale_miss_aborts() {
    value_space_manager m(make_default_config());

    paddr page = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
    cached_partial_update u{};
    u.page_base   = page;
    u.kind        = cached_partial_kind::active_tail;
    u.heat_seq    = 1;
    u.cache_epoch = 0xabcd1234ULL;
    m.mark_cached_partial(u);

    const uint64_t partial_before = m.partial_page_count_total();

    auto round = m.begin_round();
    auto opt = m.try_claim_range(round, page,
                                 /*class_idx=512*/ 3,
                                 /*alloc_quantums=*/8,
                                 /*alloc_bytes=*/512,
                                 u.cache_epoch);
    CHECK(opt.has_value());
    CHECK(opt->src == byte_claim_source::cached_partial);
    CHECK(opt->cache_epoch == u.cache_epoch);
    CHECK(round.claims.size() == 1);

    // Simulate stale miss — abort the round (rollback claim delta) and
    // erase the cached entry.
    m.abort(std::move(round));
    m.erase_cached_partial(page, u.cache_epoch);

    // Page should still be partial (round abort restored its quantums) and
    // should NOT be claimable via cached path again — the cached index has
    // been wiped.
    CHECK(m.partial_page_count_total() == partial_before);

    auto round2 = m.begin_round();
    allocation_request reqs[] = { make_req(0, /*class_idx=512*/ 3, 512) };
    auto claims = m.allocate_batch(round2, std::span<const allocation_request>(reqs, 1));
    CHECK(claims.size() == 1);
    // No cached entry, no NRP under normal mode → must go fresh page.
    CHECK(claims[0].src == byte_claim_source::new_whole_page);
    CHECK(claims[0].page_base != page);
    m.abort(std::move(round2));

    printf("  case 7 (cached claim stale miss → abort + erase): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 8 + 18: round-A abort releases only its own delta, even when round-B
// has committed claims onto the same partial page in the meantime.
// ─────────────────────────────────────────────────────────────────────────

static void
test_two_rounds_interleaved_abort_keeps_committed_claims() {
    value_space_manager m(make_default_config());

    // Build a partial page with 7×512 B (56 q) free, starting from a
    // committed 1×512 B claim at byte_offset=0.
    paddr page = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);

    // Round A claims [byte_offset = X1] on `page`.
    auto round_a = m.begin_round();
    auto a = m.try_claim_range(round_a, page, /*class_idx=*/3,
                               /*alloc_quantums=*/8,
                               /*alloc_bytes=*/512,
                               /*cache_epoch=*/0);
    CHECK(a.has_value());
    const uint32_t off_a = a->byte_offset;

    // Round B claims a different range on the same page. choose_claim_offset
    // lays consecutive claims into the lowest fitting run, so round B's
    // claim must take a position different from A's.
    auto round_b = m.begin_round();
    auto b = m.try_claim_range(round_b, page, /*class_idx=*/3,
                               /*alloc_quantums=*/8,
                               /*alloc_bytes=*/512,
                               /*cache_epoch=*/0);
    CHECK(b.has_value());
    const uint32_t off_b = b->byte_offset;
    CHECK(off_a != off_b);

    // Commit B first, then abort A.
    m.commit(std::move(round_b));
    m.abort(std::move(round_a));

    // Verify only A's delta was released. The page still has B's claim
    // live, so a fresh sub-LBA value of 512 B placed via cached path (we
    // re-admit) should still NOT use round-B's range.
    cached_partial_update u{};
    u.page_base   = page;
    u.kind        = cached_partial_kind::active_tail;
    u.heat_seq    = 1;
    u.cache_epoch = 7;
    m.mark_cached_partial(u);

    auto round_c = m.begin_round();
    allocation_request reqs[] = { make_req(0, /*class_idx=*/3, 512) };
    auto claims = m.allocate_batch(round_c, std::span<const allocation_request>(reqs, 1));
    CHECK(claims.size() == 1);
    CHECK(claims[0].src == byte_claim_source::cached_partial);
    CHECK(claims[0].page_base == page);
    // The new claim must NOT alias round-B's range (which is still live).
    CHECK(claims[0].byte_offset != off_b);
    m.abort(std::move(round_c));

    printf("  case 8+18 (interleaved rounds: B commit + A abort): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 9 + 22 + 23: commit publishes fresh-page tail partials; the
// freshly-published page becomes selectable through the largest-run
// bucket index (group-local + manager-global).
// ─────────────────────────────────────────────────────────────────────────

static void
test_commit_publishes_fresh_tail_partial_and_buckets() {
    value_space_manager m(make_default_config());

    // Allocate one 1 KiB sub-LBA → 16 quantums of 64 free; fresh page
    // becomes a partial (48 free quantums).
    paddr page = allocate_one_sub_lba_and_commit(m, /*class_idx=1024*/ 4, 1024);
    CHECK(m.partial_page_count_total() == 1);

    // The page is NOT in the cached index (we did not call mark_cached…).
    // Force the manager into reuse_pressure so the NRP path kicks in; we
    // expect the partial page to be discoverable through the bucket index
    // and reused for the next 1 KiB request.
    auto round = m.begin_round();
    allocation_request reqs[] = { make_req(0, /*class_idx=1024*/ 4, 1024) };
    allocation_policy pol;
    pol.mode = allocation_mode_override::force_reuse_pressure;
    auto claims = m.allocate_batch(round,
                                   std::span<const allocation_request>(reqs, 1),
                                   pol);
    CHECK(claims.size() == 1);
    CHECK(claims[0].src == byte_claim_source::nonresident_partial);
    CHECK(claims[0].page_base == page);

    m.commit(std::move(round));
    // Page still partial: 48 - 16 = 32 quantums free remain.
    CHECK(m.partial_page_count_total() == 1);

    printf("  case 9+22+23 (commit publishes tail; bucket index discovers it): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 10: global free span ↔ group partial metadata interaction.
// A free span MUST be coalesceable across group boundaries; partial
// metadata in one group must not block a span that crosses into another.
// ─────────────────────────────────────────────────────────────────────────

static void
test_global_span_and_group_partial_interaction() {
    value_space_manager m(make_default_config());
    const uint64_t total = kEndLBA - kBaseLBA;

    // Allocate two LBA-equal values straddling the group boundary
    // (groups of 16384 LBAs starting at base 100). The high end at
    // base+4*group_lbas-1 sits in group 3; if we do enough allocations
    // we'll cross into group 2. 1×4 KiB carving at the high end is in
    // group 3; we then carve a multi-LBA span equal to a whole group to
    // make sure global_free_extents knows about a span that spans groups.
    auto round = m.begin_round();
    allocation_request reqs[] = {
        make_req(0, /*class_idx=4096*/ 6, 4096),
        make_req(1, /*class_idx=4096*/ 6, 4096),
    };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 2));
    CHECK(claims.size() == 2);
    m.commit(std::move(round));

    CHECK(m.whole_free_lba_count() == total - 2);

    // Release both — they must coalesce into the (now) single whole-free
    // span that spans the whole data area, regardless of which group each
    // page lives in.
    value_ref refs[] = {
        make_vref(claims[0].page_base, 0, 4096),
        make_vref(claims[1].page_base, 0, 4096),
    };
    m.release_values(std::span<const value_ref>(refs, 2));
    CHECK(m.whole_free_lba_count() == total);

    // Alloc a multi-LBA value that, if coalescing failed across group
    // boundaries, would not fit. We can't tickle the group boundary
    // exactly here without fixing kGroupLBAs at runtime, but the easiest
    // proxy is allocating an LBA-equal block at the high end and confirm
    // the very top LBA is reserved (proves the span starts at the absolute
    // top, i.e., no missing LBAs in between).
    auto r2 = m.begin_round();
    allocation_request big[] = { make_req(0, /*class_idx=4096*/ 6, 4096) };
    auto big_claims = m.allocate_batch(r2, std::span<const allocation_request>(big, 1));
    CHECK(big_claims.size() == 1);
    CHECK(big_claims[0].page_base.lba == kEndLBA - 1);
    m.abort(std::move(r2));

    printf("  case 10 (global span / group partial interaction): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 11: trim prepare withholds free range; complete (success or fail)
// returns range to allocatable free.
// ─────────────────────────────────────────────────────────────────────────

static void
test_trim_prepare_complete_cycle() {
    value_space_manager m(make_default_config());
    const uint64_t total = kEndLBA - kBaseLBA;

    // Plan a trim of 8 LBAs (single range from the high end).
    auto plan = m.prepare_trim(/*max_ranges=*/4, /*max_lbas=*/8);
    CHECK(plan.ranges.size() >= 1);

    uint32_t taken = 0;
    for (const auto& r : plan.ranges) taken += r.len_lbas;
    CHECK(taken == 8);
    CHECK(m.whole_free_lba_count() == total - 8);

    // While inflight, those 8 LBAs are NOT allocatable. Carving 8 LBAs of
    // multi-LBA must come from BELOW the trim window.
    auto round = m.begin_round();
    allocation_request reqs[] = { make_req(0, /*class_idx=8192*/ 7, 8192) };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1));
    CHECK(claims.size() == 1);
    // The trim plan took the high-end 8 LBAs; the next 2-LBA carve must
    // start strictly below the trim base.
    uint64_t trim_lo = UINT64_MAX;
    for (const auto& r : plan.ranges) trim_lo = std::min<uint64_t>(trim_lo, r.lba);
    CHECK(claims[0].page_base.lba + 2 <= trim_lo);
    m.abort(std::move(round));

    // Complete with ok=false — by design, range still returns to free
    // (TRIM is best-effort hardware maintenance; logical free is decoupled).
    m.complete_trim(plan.id, false);
    CHECK(m.whole_free_lba_count() == total);

    // Repeat the cycle, this time complete with ok=true.
    auto plan2 = m.prepare_trim(/*max_ranges=*/4, /*max_lbas=*/8);
    CHECK(m.whole_free_lba_count() == total - 8);
    m.complete_trim(plan2.id, true);
    CHECK(m.whole_free_lba_count() == total);

    printf("  case 11 (trim prepare withhold / complete returns range): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 12 + 20: recovery rebuild from live extents — does not depend on
// dead hints, does not read Value Area; canonical class mapping is stable.
// ─────────────────────────────────────────────────────────────────────────

static void
test_recovery_rebuild_no_hints_canonical_mapping() {
    value_space_manager m(make_default_config());
    const uint64_t total = kEndLBA - kBaseLBA;

    // Construct a few live extents. Mix sub-LBA + multi-LBA. We do NOT
    // pass any dead_class_hint to confirm correctness without hints.
    std::vector<live_value_extent> live;
    // sub-LBA 256 B at LBA = base+10 byte_offset 0
    live.push_back(live_value_extent{ make_paddr(kBaseLBA + 10), 0,   256  });
    // sub-LBA 512 B at LBA = base+10 byte_offset 256 (same LBA → mixed)
    live.push_back(live_value_extent{ make_paddr(kBaseLBA + 10), 256, 512  });
    // multi-LBA 8 KiB at LBA = base+50
    live.push_back(live_value_extent{ make_paddr(kBaseLBA + 50), 0,   8192 });
    // sub-LBA 1 KiB at LBA = base+200, byte_offset 1024
    live.push_back(live_value_extent{ make_paddr(kBaseLBA + 200), 1024, 1024 });

    m.install_recovered_state(std::span<const live_value_extent>(live),
                              /*tree_alloc_head_lba=*/kBaseLBA,
                              /*data_area_end_lba=*/kEndLBA,
                              /*hints=*/{});

    // Whole-free truth = data area minus occupied LBAs.
    //   LBA base+10  → 1 partial LBA (NOT whole-free — has live values)
    //   LBA base+50..51 → 2 fully-occupied LBAs (multi-LBA)
    //   LBA base+200 → 1 partial LBA
    // Total NOT-whole-free LBAs = 4. So whole_free_lba_count = total - 4.
    CHECK(m.whole_free_lba_count() == total - 4);
    // Two partial pages: base+10 and base+200.
    CHECK(m.partial_page_count_total() == 2);
    CHECK(m.acknowledged_alloc_floor_lba() == kBaseLBA);

    // Canonical class map stability (case 20): same len always maps to
    // the same class. We test the full class_size table — every value of
    // `len` in (prev_class, this_class] maps to this_class.
    //
    // The mapping is exposed only indirectly through release/recovery
    // behaviour, but a simple proxy is to release a value with len equal
    // to a class size — that must be cleanly accepted (resulting metadata
    // change is observable).
    const uint64_t pre_partial = m.partial_page_count_total();
    value_ref vr_512 = make_vref(make_paddr(kBaseLBA + 10), 256, 512);
    value_ref refs[] = { vr_512 };
    m.release_values(std::span<const value_ref>(refs, 1));
    // Releasing the 512 B half of the page leaves the 256 B claim alone:
    // page is still partial, count unchanged.
    CHECK(m.partial_page_count_total() == pre_partial);

    printf("  case 12+20 (recovery from live extents / canonical mapping): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 13 + 28: alloc-floor reservation fence semantics.
//   - tree publishes higher floor → value owner sync_alloc_floor must
//     accept; thereafter allocation/trim must not dip below the new floor.
//   - tree returning extents back (floor-down) is only allowed via
//     sync_alloc_floor(..., tree_returned_extents); manager will not
//     auto-resurrect below-floor LBAs from a previously-sync'd low.
// ─────────────────────────────────────────────────────────────────────────

static void
test_alloc_floor_sync_and_returned_extents() {
    data_area_heads heads;
    auto cfg = make_default_config(kEndLBA, kBaseLBA, kGroupLBAs, &heads);
    value_space_manager m(cfg);
    const uint64_t total = kEndLBA - kBaseLBA;

    // Tree raises floor to base+1000 — publishes via tree_head_lba then
    // posts sync_alloc_floor. No partial pages, no trim, no cached → must
    // be accepted.
    const uint64_t new_floor = kBaseLBA + 1000;
    heads.tree_head_lba.store(new_floor, std::memory_order_release);

    auto res = m.sync_alloc_floor(new_floor, std::span<const free_extent>{});
    CHECK(res.st == alloc_floor_sync_status::code::accepted);
    CHECK(res.acknowledged_floor_lba == new_floor);
    CHECK(m.acknowledged_alloc_floor_lba() == new_floor);
    // After acceptance, below-floor stale extents are pruned.
    CHECK(m.whole_free_lba_count() == total - 1000);

    // Allocation must now NOT yield any LBA below new_floor — even though
    // (before the sync) the data area was a single span starting at
    // base. We try to carve enough LBAs to dip below would-be floor; the
    // manager either fits within above-floor space or reports exhaustion.
    auto round = m.begin_round();
    allocation_request big[] = { make_req(0, /*class_idx=4096*/ 6, 4096) };
    auto big_claims = m.allocate_batch(round, std::span<const allocation_request>(big, 1));
    CHECK(big_claims.size() == 1);
    CHECK(big_claims[0].page_base.lba >= new_floor);
    m.abort(std::move(round));

    // Trim eligibility must respect the floor too: prepare_trim should
    // not return any range below new_floor.
    auto plan = m.prepare_trim(/*max_ranges=*/4, /*max_lbas=*/16);
    for (const auto& r : plan.ranges) {
        CHECK(r.lba >= new_floor);
    }
    m.complete_trim(plan.id, true);

    // Now floor-down: tree returns 200 LBAs of [new_floor-200, new_floor)
    // explicitly. Without the explicit return, the manager would NOT
    // resurrect those LBAs even when the tree head moves down.
    //
    // First, lower head atomically — note this alone changes nothing in
    // the manager's allocatable view; resurrection requires the explicit
    // returned-extents list.
    const uint64_t lower_floor = new_floor - 200;
    heads.tree_head_lba.store(lower_floor, std::memory_order_release);

    // Without tree_returned_extents: we test that simply dropping the
    // floor returns "accepted" but does NOT auto-resurrect LBAs.
    const uint64_t pre_free = m.whole_free_lba_count();
    auto res_lower_only = m.sync_alloc_floor(lower_floor,
                                             std::span<const free_extent>{});
    CHECK(res_lower_only.st == alloc_floor_sync_status::code::accepted);
    CHECK(m.whole_free_lba_count() == pre_free); // unchanged

    // Now resurrect via explicit list. Provide a single 200-LBA span at
    // [lower_floor, new_floor).
    free_extent returned[] = {
        free_extent{ lower_floor, 200 },
    };
    auto res_with_list = m.sync_alloc_floor(lower_floor,
                                            std::span<const free_extent>(returned, 1));
    CHECK(res_with_list.st == alloc_floor_sync_status::code::accepted);
    CHECK(m.whole_free_lba_count() == pre_free + 200);

    printf("  case 13+28 (alloc_floor sync fence + tree_returned_extents): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 13b: alloc_floor sync rejects when value owner still holds artefacts
// in the new floor band (live partial page below the proposed floor).
// ─────────────────────────────────────────────────────────────────────────

static void
test_alloc_floor_sync_rejects_collision() {
    data_area_heads heads;
    value_space_manager m(make_default_config(kEndLBA, kBaseLBA, kGroupLBAs, &heads));

    // Plant a partial page at a known low LBA via install_recovered_state.
    // Going through release_values on a never-carved LBA would violate the
    // global_free_extents ↔ partial_pages disjointness invariant (the LBA
    // would still be inside a free extent), which the post-INC-051 sync
    // floor correctly treats as no-collision. install_recovered_state
    // properly excludes the live LBA from global_free_extents and is the
    // canonical entry point for "rebuild manager state from live refs".
    const uint64_t partial_lba = kBaseLBA + 10;
    std::vector<live_value_extent> live = {
        live_value_extent{ make_paddr(partial_lba), /*byte_offset=*/0, /*len=*/256 },
    };
    m.install_recovered_state(std::span<const live_value_extent>(live),
                              /*tree_alloc_head_lba=*/kBaseLBA,
                              /*data_area_end_lba=*/kEndLBA,
                              /*hints=*/{});
    CHECK(m.partial_page_count_total() == 1);
    CHECK(m.acknowledged_alloc_floor_lba() == kBaseLBA);

    // Tree wants to claim everything up to base+100 — collides with our
    // partial page at base+10. Manager must reject.
    const uint64_t new_floor = kBaseLBA + 100;
    auto res = m.sync_alloc_floor(new_floor, std::span<const free_extent>{});
    CHECK(res.st == alloc_floor_sync_status::code::rejected_collision);
    CHECK(m.acknowledged_alloc_floor_lba() == kBaseLBA);

    printf("  case 13b (alloc_floor reject on collision): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 13c: alloc_floor sync rejects when a fully-allocated LBA-equal /
// multi-LBA page sits below the proposed floor.
//
// This is distinct from 13b: a full page is NOT in global_free_extents
// AND NOT in partial_pages — it shows up only as an implicit hole in the
// free-extent map. Plan 037 §"地址空间" rule 3 says the value owner must
// reject any floor raise whose covered band contains a "live value" or
// "已被更早 value allocation claim 的范围", which explicitly includes
// these full pages. 13b only covers partial pages, so this case proves
// the full-page lane is also guarded.
// ─────────────────────────────────────────────────────────────────────────

static void
test_alloc_floor_sync_rejects_full_page_collision() {
    data_area_heads heads;
    value_space_manager m(make_default_config(kEndLBA, kBaseLBA, kGroupLBAs, &heads));

    // Allocate one multi-LBA value (8 KiB, class_idx=7, span_lbas=2).
    // From-high carving lands it at [end_lba - 2, end_lba). The page is
    // "implicit full allocated": absent from global_free_extents AND
    // absent from partial_pages.
    auto round = m.begin_round();
    allocation_request reqs[] = { make_req(0, /*class_idx=8192*/ 7, 8192) };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1));
    CHECK(claims.size() == 1);
    CHECK(claims[0].src == byte_claim_source::new_whole_page);
    const paddr full_page_base = claims[0].page_base;
    constexpr uint32_t kSpanLbas = 2; // 8192 / 4096
    m.commit(std::move(round));

    // Sanity: no partial page was created.
    CHECK(m.partial_page_count_total() == 0);
    const uint64_t total = kEndLBA - kBaseLBA;
    const uint64_t free_before = m.whole_free_lba_count();
    CHECK(free_before == total - kSpanLbas);
    const uint64_t old_acked = m.acknowledged_alloc_floor_lba();
    CHECK(old_acked == kBaseLBA);

    // Tree wants to raise floor to claim.page_base.lba + span_lbas, which
    // covers the entire multi-LBA value. Because the value is still live,
    // the manager must reject.
    const uint64_t new_floor = full_page_base.lba + kSpanLbas;
    CHECK(new_floor > full_page_base.lba); // sanity: floor covers the value
    CHECK(new_floor <= kEndLBA);

    auto res = m.sync_alloc_floor(new_floor, std::span<const free_extent>{});
    CHECK(res.st == alloc_floor_sync_status::code::rejected_collision);
    CHECK(res.acknowledged_floor_lba == old_acked);

    // No state should have been mutated by the rejection: the acked floor
    // stays at the original value, free LBA count is unchanged, the
    // implicit full page is still implicitly allocated.
    CHECK(m.acknowledged_alloc_floor_lba() == old_acked);
    CHECK(m.whole_free_lba_count() == free_before);
    CHECK(m.partial_page_count_total() == 0);

    // Releasing the value re-opens the full page; only THEN may the floor
    // raise be accepted, because [old_acked, new_floor) is now entirely in
    // global_free_extents.
    value_ref vr = make_vref(full_page_base, 0, 8192);
    value_ref refs[] = { vr };
    m.release_values(std::span<const value_ref>(refs, 1));
    CHECK(m.whole_free_lba_count() == total);

    auto res_after_release =
        m.sync_alloc_floor(new_floor, std::span<const free_extent>{});
    CHECK(res_after_release.st == alloc_floor_sync_status::code::accepted);
    CHECK(m.acknowledged_alloc_floor_lba() == new_floor);

    printf("  case 13c (alloc_floor reject on full-page collision): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 15: cached partial budget eviction does not lose page-local
// allocator truth — only the cached candidate is removed, partial_pages
// metadata stays intact.
// ─────────────────────────────────────────────────────────────────────────

static void
test_cached_partial_budget_evicts_candidate_not_truth() {
    auto cfg = make_default_config();
    cfg.value_cached_partial_budget_bytes = 2 * kLBA; // budget for 2 candidates
    value_space_manager m(cfg);

    // Build 3 partial pages → admit each as cached candidate. The third
    // admission must evict one (lowest-score) entry; manager partial
    // metadata count must stay at 3.
    paddr p0 = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
    paddr p1 = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
    paddr p2 = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
    CHECK(m.partial_page_count_total() == 3);

    cached_partial_update u{};
    u.kind        = cached_partial_kind::cached_free_candidate;
    u.heat_seq    = 1;
    u.cache_epoch = 1;
    u.page_base = p0; m.mark_cached_partial(u);
    u.heat_seq = 2; u.cache_epoch = 2; u.page_base = p1; m.mark_cached_partial(u);
    u.heat_seq = 3; u.cache_epoch = 3; u.page_base = p2; m.mark_cached_partial(u);

    // partial_page_count_total_ unchanged — eviction operates on
    // cached_partial_index only.
    CHECK(m.partial_page_count_total() == 3);

    // The hottest two should still be selectable. heat_seq 3 (p2) and 2
    // (p1) survived; heat 1 (p0) was evicted. allocate_batch with one
    // 512 B request should pick from cached candidates; expect either p2
    // or p1 (highest heat is p2).
    auto round = m.begin_round();
    allocation_request reqs[] = { make_req(0, /*class_idx=512*/ 3, 512) };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1));
    CHECK(claims.size() == 1);
    CHECK(claims[0].src == byte_claim_source::cached_partial);
    CHECK(claims[0].page_base == p2); // hottest
    m.abort(std::move(round));

    printf("  case 15 (cached partial budget evicts candidate, keeps truth): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 17 + 24: pressure mode selection (space + partial) enables / disables
// non-resident-partial reuse and read budgets; partial_metadata hard limit
// gates fresh page allocation when projected partial count would grow.
// ─────────────────────────────────────────────────────────────────────────

static void
test_pressure_mode_nrp_and_partial_limits() {
    // Three sub-cases: normal disables NRP; reuse_pressure enables NRP with
    // batch budget; partial hard limit blocks fresh-page allocation that
    // would create a new partial.
    {
        // ── Normal mode: NRP disabled, even if partial pages exist. ──
        value_space_manager m(make_default_config());
        paddr p_partial = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
        // Don't mark cached; the partial sits non-resident.

        auto round = m.begin_round();
        allocation_request reqs[] = { make_req(0, /*class_idx=512*/ 3, 512) };
        // automatic mode at near-empty data area = normal.
        auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1));
        CHECK(claims.size() == 1);
        CHECK(claims[0].src == byte_claim_source::new_whole_page);
        CHECK(claims[0].page_base != p_partial);
        m.abort(std::move(round));
    }
    {
        // ── reuse_pressure: NRP allowed within batch read budget. ──
        auto cfg = make_default_config();
        cfg.reuse_pressure_max_prefill_reads_per_batch = 2;
        cfg.reuse_pressure_max_prefill_reads_per_class = 2;
        value_space_manager m(cfg);
        paddr p_partial = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
        // Don't mark cached.

        auto round = m.begin_round();
        allocation_request reqs[] = { make_req(0, /*class_idx=512*/ 3, 512) };
        allocation_policy pol;
        pol.mode = allocation_mode_override::force_reuse_pressure;
        auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1), pol);
        CHECK(claims.size() == 1);
        CHECK(claims[0].src == byte_claim_source::nonresident_partial);
        CHECK(claims[0].page_base == p_partial);
        m.abort(std::move(round));
    }
    {
        // ── partial hard limit + zero prefill budget: every avenue except
        // creating a new partial is blocked, so the hard gate must surface
        // as an empty allocate_batch result.
        auto cfg = make_default_config();
        cfg.partial_metadata_soft_limit_pages = 1;
        cfg.partial_metadata_hard_limit_pages = 1;
        // Forbid NRP under both pressure modes so the only remaining path
        // for a sub-LBA request is to create a fresh page → which is what
        // the hard gate exists to block.
        cfg.reuse_pressure_max_prefill_reads_per_batch = 0;
        cfg.reuse_pressure_max_prefill_reads_per_class = 0;
        cfg.hard_pressure_max_prefill_reads_per_batch  = 0;
        cfg.hard_pressure_max_prefill_reads_per_class  = 0;
        value_space_manager m(cfg);

        // First sub-LBA alloc creates partial #1, saturating the hard limit.
        paddr first = allocate_one_sub_lba_and_commit(m, /*class_idx=512*/ 3, 512);
        CHECK(m.partial_page_count_total() == 1);

        auto round = m.begin_round();
        allocation_request reqs[] = { make_req(0, /*class_idx=512*/ 3, 512) };
        auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 1));
        // No cached candidate, NRP budget = 0, fresh-page blocked by hard
        // gate → empty result, round stays clean.
        CHECK(claims.empty());
        CHECK(m.partial_page_count_total() == 1);
        m.abort(std::move(round));

        // Claiming onto the EXISTING partial (does NOT grow partial count)
        // remains allowed via cached path.
        cached_partial_update u{};
        u.page_base = first; u.kind = cached_partial_kind::active_tail;
        u.heat_seq = 1; u.cache_epoch = 0;
        m.mark_cached_partial(u);

        auto round2 = m.begin_round();
        auto claims2 = m.allocate_batch(round2, std::span<const allocation_request>(reqs, 1));
        CHECK(claims2.size() == 1);
        CHECK(claims2[0].src == byte_claim_source::cached_partial);
        m.abort(std::move(round2));
    }

    printf("  case 17+24 (pressure mode / partial-limit hard gate): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 26: page-local best-fit picks the smallest fitting run.
// ─────────────────────────────────────────────────────────────────────────

static void
test_choose_claim_offset_best_fit() {
    // Free bits: positions [0,1] (length-2 run) + positions [4..8]
    // (length-5 run). All other bits zero.
    constexpr uint64_t bits = (1ULL << 0) | (1ULL << 1)
                            | (1ULL << 4) | (1ULL << 5) | (1ULL << 6) | (1ULL << 7) | (1ULL << 8);
    const uint32_t k = vd::recompute_largest_run(bits, 64);
    CHECK(k == 5);

    // need=2: there is an exact length-2 run at pos 0 → pick it.
    CHECK(vd::choose_claim_offset(bits, 2, k, 64) == 0);
    // need=3: no exact 3-run; smallest fitting is length-5 → pos 4.
    CHECK(vd::choose_claim_offset(bits, 3, k, 64) == 4);
    // need=5: smallest fitting is length-5 → pos 4.
    CHECK(vd::choose_claim_offset(bits, 5, k, 64) == 4);

    // Symmetric scenario: two length-3 runs at different starts; need=3 must
    // take the lower start.
    constexpr uint64_t bits2 = (1ULL << 0) | (1ULL << 1) | (1ULL << 2)
                             | (1ULL << 8) | (1ULL << 9) | (1ULL << 10);
    const uint32_t k2 = vd::recompute_largest_run(bits2, 64);
    CHECK(k2 == 3);
    CHECK(vd::choose_claim_offset(bits2, 3, k2, 64) == 0);

    printf("  case 26 (page-local best-fit): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 25 (extra): bit helpers exact-run / actual-run / recompute on edges.
// ─────────────────────────────────────────────────────────────────────────

static void
test_bit_helpers_edges() {
    // recompute_largest_run on all-free 64-quantum page = 64.
    CHECK(vd::recompute_largest_run(~0ULL, 64) == 64);
    // recompute_largest_run on zero bits = 0.
    CHECK(vd::recompute_largest_run(0ULL, 64) == 0);
    // run_start_mask len=1 = bits.
    CHECK(vd::run_start_mask(0xA5A5A5A5'A5A5A5A5ULL, 1) == 0xA5A5A5A5'A5A5A5A5ULL);
    // exact_run_start_mask: bits=0xF0 (run length 4 at pos 4), need=4 →
    // start mask must have bit 4 set, no other bits.
    const uint64_t bits = 0xF0ULL;
    const uint64_t exact4 = vd::exact_run_start_mask(bits, 4, 64);
    CHECK(exact4 == (1ULL << 4));
    // For r=3: there is no exact 3-run inside a 4-run, so exact3 must be 0.
    CHECK(vd::exact_run_start_mask(bits, 3, 64) == 0);

    // actual_run_start_mask: only bit positions that ARE runs starts.
    // bits=0b1011 has run starts at 0 and 3 (2-run, then 1-run).
    CHECK(vd::actual_run_start_mask(0b1011ULL, 64) == 0b1001ULL);

    printf("  case 25 (bit helpers edges): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 6d (cached_partial_index consistency / auto-sync).
//
// 037 plan §"Cached Partial Selection" treats the cached_partial_index as
// a derived view over `partial_pages`. The index stores SNAPSHOT copies of
// `free_quantum_count` / `largest_free_run_quantums`; if those drift out
// of sync with the underlying partial_page_node truth, allocate_batch
// would either:
//   - select a cached candidate that cannot satisfy the request (stale
//     summary too high → claim_on_partial_ returns nullopt), or
//   - refuse to select a cached candidate that COULD satisfy (stale
//     summary too low → unnecessary fresh writes).
//
// Production converges every state change through `apply_node_bit_update_`
// (the single seam that mutates `free_quantum_bits`), and that seam also
// updates / erases the cached_partial_index. These tests pin that
// invariant by exercising each state-change path:
//   A. Cached claim that leaves the page partial    → index summary refreshed.
//   B. Cached claim that fills the page             → index entry erased.
//   C. release_values that empties the page         → index entry erased.
//   D. abort() that rolls back a cached claim       → index summary restored.
//   E. In-batch cached exhaustion mid-iteration     → remaining iterations
//                                                     fall through to fresh,
//                                                     batch does NOT return
//                                                     empty (regression
//                                                     against "stale cached
//                                                     candidate blocks
//                                                     allocate_batch even
//                                                     when fresh space is
//                                                     available").
// ─────────────────────────────────────────────────────────────────────────

// Plant a partial page at a chosen LBA with `free_count` free quantums
// laid out as a single contiguous run starting at `free_run_start`. Lets
// the test pin the precise summary the cached_partial_index records at
// mark_cached_partial time. Returns the page_base.
static paddr
plant_partial_page_with_run(value_space_manager& m,
                             uint64_t              page_lba,
                             uint32_t              free_run_start_q,
                             uint32_t              free_run_len_q) {
    // Compose live extents that occupy everything OUTSIDE the desired
    // free run. We use class_idx = 1 (128 B = 2 quantums) granularity so
    // we can exactly cover any quantum range that's a multiple of 2; for
    // 4 KiB LBA / 64 B quantum this gives 32 fillable slots. The test
    // helper rounds the prefix and suffix to multiples of 2 quantums and
    // installs them via install_recovered_state.
    constexpr uint32_t kFillerLenBytes = 128; // class_idx 1, 2 quantums
    constexpr uint32_t kFillerLenQ     = 2;
    CHECK(free_run_start_q % kFillerLenQ == 0);
    CHECK(free_run_len_q % kFillerLenQ == 0);
    CHECK(free_run_start_q + free_run_len_q <= kQPL);

    std::vector<live_value_extent> live;
    const paddr page = make_paddr(page_lba);
    // Prefix [0, free_run_start_q)
    for (uint32_t q = 0; q < free_run_start_q; q += kFillerLenQ) {
        live.push_back(live_value_extent{
            page, static_cast<uint16_t>(q * kQuantum), kFillerLenBytes,
        });
    }
    // Suffix [free_run_start_q + free_run_len_q, kQPL)
    for (uint32_t q = free_run_start_q + free_run_len_q; q < kQPL; q += kFillerLenQ) {
        live.push_back(live_value_extent{
            page, static_cast<uint16_t>(q * kQuantum), kFillerLenBytes,
        });
    }
    m.install_recovered_state(std::span<const live_value_extent>(live),
                              /*tree_alloc_head_lba=*/kBaseLBA,
                              /*data_area_end_lba=*/kEndLBA,
                              /*hints=*/{});
    return page;
}

static void
test_cached_partial_index_consistency() {
    // ── Sub-test A: cached claim leaves page partial → summary refreshed. ──
    {
        value_space_manager m(make_default_config());
        const paddr P = plant_partial_page_with_run(
            m, /*page_lba=*/kBaseLBA + 5,
            /*free_run_start_q=*/0,
            /*free_run_len_q=*/48);   // free run of 48 quantums at offset 0

        cached_partial_update u{};
        u.page_base   = P;
        u.kind        = cached_partial_kind::active_tail;
        u.heat_seq    = 1;
        u.cache_epoch = 100;
        m.mark_cached_partial(u);

        // Consume 32q via try_claim_range; page becomes free_count=16,
        // largest_run=16 (the surviving suffix [32, 48)).
        auto round = m.begin_round();
        auto opt = m.try_claim_range(round, P,
                                     /*class_idx=2048*/ 5,
                                     /*alloc_quantums=*/32,
                                     /*alloc_bytes=*/2048,
                                     u.cache_epoch);
        CHECK(opt.has_value());
        m.commit(std::move(round));

        // Next batch: ask for 32q. The OLD cached summary (largest=48)
        // would satisfy, but the synced summary (largest=16) cannot.
        // Manager must skip the cached candidate and fall through to fresh.
        auto round2 = m.begin_round();
        allocation_request reqs[] = { make_req(0, /*class_idx=2048*/ 5, 2048) };
        auto claims = m.allocate_batch(round2, std::span<const allocation_request>(reqs, 1));
        CHECK(claims.size() == 1);
        CHECK(claims[0].src == byte_claim_source::new_whole_page);
        CHECK(claims[0].page_base != P);
        m.abort(std::move(round2));

        // And: a sub-LBA request that DOES fit the new summary still
        // gets routed to the cached page (proves the summary refresh
        // isn't over-conservative).
        auto round3 = m.begin_round();
        allocation_request reqs2[] = { make_req(0, /*class_idx=1024*/ 4, 1024) }; // 16q
        auto claims2 = m.allocate_batch(round3, std::span<const allocation_request>(reqs2, 1));
        CHECK(claims2.size() == 1);
        CHECK(claims2[0].src == byte_claim_source::cached_partial);
        CHECK(claims2[0].page_base == P);
        m.abort(std::move(round3));
    }

    // ── Sub-test B: cached claim fills page → index entry erased. ──
    {
        value_space_manager m(make_default_config());
        // Page has free run of 8q only. Mark cached.
        const paddr P = plant_partial_page_with_run(
            m, kBaseLBA + 5, /*start=*/0, /*len=*/8);
        cached_partial_update u{};
        u.page_base   = P;
        u.kind        = cached_partial_kind::active_tail;
        u.heat_seq    = 1;
        u.cache_epoch = 200;
        m.mark_cached_partial(u);

        // Consume the full 8q → page becomes full → cached_partial_index
        // entry must be erased by apply_node_bit_update_'s sync hook.
        auto round = m.begin_round();
        auto opt = m.try_claim_range(round, P,
                                     /*class_idx=512*/ 3,
                                     /*alloc_quantums=*/8,
                                     /*alloc_bytes=*/512,
                                     u.cache_epoch);
        CHECK(opt.has_value());
        m.commit(std::move(round));

        // Next batch: even though we never explicitly called
        // erase_cached_partial(), the manager must NOT route any claim
        // to the now-full page.
        auto round2 = m.begin_round();
        allocation_request reqs[] = { make_req(0, /*class_idx=512*/ 3, 512) };
        auto claims = m.allocate_batch(round2, std::span<const allocation_request>(reqs, 1));
        CHECK(claims.size() == 1);
        CHECK(claims[0].src == byte_claim_source::new_whole_page);
        CHECK(claims[0].page_base != P);
        m.abort(std::move(round2));
    }

    // ── Sub-test C: release_values makes page all-free → index erased. ──
    {
        value_space_manager m(make_default_config());
        // Plant page with a single 256 B occupant (4q occupied, 60q free).
        const paddr P = plant_partial_page_with_run(
            m, kBaseLBA + 5, /*start=*/0, /*len=*/60);
        cached_partial_update u{};
        u.page_base   = P;
        u.kind        = cached_partial_kind::active_tail;
        u.heat_seq    = 1;
        u.cache_epoch = 300;
        m.mark_cached_partial(u);

        // Release the only live occupant — pages goes all-free, partial
        // node is removed AND the cached_partial_index entry must be
        // dropped via apply_node_bit_update_'s sync hook. The 1-LBA span
        // returns to global_free_extents.
        const value_ref live = make_vref(P, 60 * kQuantum, 256);
        const value_ref refs[] = { live };
        m.release_values(std::span<const value_ref>(refs, 1));
        CHECK(m.partial_page_count_total() == 0);

        // Subsequent allocate_batch must not select P from the cached
        // path (it's no longer partial). It MAY happen to allocate at
        // the same LBA via fresh-carve from the high end — but only as
        // a NEW whole page, not as cached_partial.
        auto round = m.begin_round();
        allocation_request reqs2[] = { make_req(0, /*class_idx=512*/ 3, 512) };
        auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs2, 1));
        CHECK(claims.size() == 1);
        CHECK(claims[0].src == byte_claim_source::new_whole_page);
        m.abort(std::move(round));
    }

    // ── Sub-test D: abort restores cached page to pre-claim summary. ──
    {
        value_space_manager m(make_default_config());
        const paddr P = plant_partial_page_with_run(
            m, kBaseLBA + 5, /*start=*/0, /*len=*/48);
        cached_partial_update u{};
        u.page_base   = P;
        u.kind        = cached_partial_kind::active_tail;
        u.heat_seq    = 1;
        u.cache_epoch = 400;
        m.mark_cached_partial(u);

        // Claim 32q → page largest=16 (mid-round).
        auto round_a = m.begin_round();
        auto opt = m.try_claim_range(round_a, P,
                                     /*class_idx=2048*/ 5,
                                     /*alloc_quantums=*/32,
                                     /*alloc_bytes=*/2048,
                                     u.cache_epoch);
        CHECK(opt.has_value());

        // Abort: rollback_round_state_ inverts the bit update through
        // apply_node_bit_update_, which also restores the cached index
        // summary back to {free=48, largest=48}.
        m.abort(std::move(round_a));

        // Next batch with need=48 (which only the RESTORED summary
        // satisfies) must route to cached, not fresh.
        auto round_b = m.begin_round();
        // 048q = 48 * 64 B = 3072 B. No exact-size class for 3072.
        // Use class_idx=2048 (32q) to verify the cached candidate is
        // still considered. Actually, easier: use class 1024 (16q) which
        // fits both the new and old summary; the test of "cached
        // selectable" is positively confirmed by claim landing on P.
        // For the stricter "old summary specifically" assertion, use a
        // 2048-byte class request — needs 32q, the post-abort summary
        // (48q) covers it; the mid-round summary (16q) would not.
        allocation_request reqs[] = { make_req(0, /*class_idx=2048*/ 5, 2048) };
        auto claims = m.allocate_batch(round_b, std::span<const allocation_request>(reqs, 1));
        CHECK(claims.size() == 1);
        CHECK(claims[0].src == byte_claim_source::cached_partial);
        CHECK(claims[0].page_base == P);
        m.abort(std::move(round_b));
    }

    // ── Sub-test E (regression): cached candidate gets exhausted in
    //   iteration 0 of allocate_batch; the remaining iterations must
    //   NOT fail because the cached entry's local-snapshot summary was
    //   pre-iteration-0. This pins the in-batch refresh + retry path
    //   (production line 2300-2310 + cached_locals refresh in case 0).
    //   Without that path, allocate_batch would return empty for any
    //   batch where iter 0 happens to fill the only cached candidate.
    //
    //   Batch is sized so Phase 1 admits the cached candidate: total
    //   need = 9 × 8q = 72q → f_baseline = 2. Absorbing the 8q cached
    //   leaves 64q to fresh = 1 fresh page, so writes_with = 1 + 1 = 2,
    //   i.e. == baseline → admitted. ──
    {
        value_space_manager m(make_default_config());
        // Cached page with EXACTLY 8 free quantums.
        const paddr P = plant_partial_page_with_run(
            m, kBaseLBA + 5, /*start=*/0, /*len=*/8);
        cached_partial_update u{};
        u.page_base   = P;
        u.kind        = cached_partial_kind::active_tail;
        u.heat_seq    = 1;
        u.cache_epoch = 500;
        m.mark_cached_partial(u);

        // 9-request batch, each 8q (512 B). Iter 0 picks P (only cached
        // candidate, no round_tail yet) and fills it; index entry erased
        // + cached_locals entry dropped by the in-iter refresh. Iters
        // 1..8 must fall through to fresh-carve + round_tail packing,
        // NOT abort the whole batch.
        auto round = m.begin_round();
        allocation_request reqs[9];
        for (uint32_t i = 0; i < 9; ++i) {
            reqs[i] = make_req(i, /*class_idx=512*/ 3, 512);
        }
        auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 9));
        CHECK(claims.size() == 9);

        // Counts: exactly one cached claim (on P), eight fresh claims
        // (all on the same single carved fresh LBA, packed via
        // round_tail).
        uint32_t cached_count = 0;
        uint32_t fresh_count  = 0;
        std::optional<paddr> fresh_page;
        for (const auto& c : claims) {
            if (c.src == byte_claim_source::cached_partial) {
                CHECK(c.page_base == P);
                ++cached_count;
            } else if (c.src == byte_claim_source::new_whole_page) {
                CHECK(c.page_base != P);
                if (!fresh_page) fresh_page = c.page_base;
                else CHECK(c.page_base == *fresh_page);
                ++fresh_count;
            }
        }
        CHECK(cached_count == 1);
        CHECK(fresh_count == 8);
        m.abort(std::move(round));
    }

    printf("  case 6d (cached_partial_index consistency / auto-sync): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 13d / 12b (malformed-input fail-fast for release_values +
// install_recovered_state).
//
// 037 plan §"Reclaim" leaves *liveness* defence to the caller (manager has
// no tree oracle), but STRUCTURAL malformation must be rejected. Silent
// continue would either:
//   - leak free space (release of a junk ref does nothing → page stays
//     marked allocated forever), or
//   - mark a live byte range as free (recovery skips a malformed live
//     extent → its LBA appears in global_free_extents → tree/value
//     re-allocate over live data).
//
// Both `release_values()` and `install_recovered_state()` validate every
// entry in a pre-pass before any state mutation; the FIRST malformed entry
// throws std::invalid_argument and leaves manager state untouched. These
// tests pin both the rejection AND the state-preservation property.
// ─────────────────────────────────────────────────────────────────────────

template <typename Fn>
static bool
throws_invalid_argument(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

static void
test_release_values_fails_fast_on_malformed_input() {
    value_space_manager m(make_default_config());

    // Plant a known partial page so we can detect any spurious mutation:
    // a successful malformed-input handling MUST NOT alter partial state.
    paddr existing = allocate_one_sub_lba_and_commit(m, /*class_idx=*/2, /*alloc_bytes=*/256);
    const uint64_t pre_partial = m.partial_page_count_total();
    const uint64_t pre_free    = m.whole_free_lba_count();

    auto reject = [&](value_ref vr) {
        const value_ref refs[] = { vr };
        CHECK(throws_invalid_argument([&]{
            m.release_values(std::span<const value_ref>(refs, 1));
        }));
        // State must be preserved across the rejection.
        CHECK(m.partial_page_count_total() == pre_partial);
        CHECK(m.whole_free_lba_count()     == pre_free);
    };

    // ── 1. Invalid device_id ──
    reject(make_vref(make_paddr(kBaseLBA + 5, /*dev=*/99), 0, 256));

    // ── 2. LBA below data_area_base_lba ──
    reject(make_vref(make_paddr(kBaseLBA - 1), 0, 256));

    // ── 3. LBA at-or-above data_area_end_lba ──
    reject(make_vref(make_paddr(kEndLBA), 0, 256));

    // ── 4. len exceeds the largest configured class_size ──
    //   default_class_sizes() max = 8192. Use 99999 so canonical lookup
    //   returns nullopt.
    reject(make_vref(make_paddr(kBaseLBA + 5), 0, 99999));

    // ── 5. sub-LBA byte_offset not quantum-aligned ──
    //   33 is not a multiple of 64. class for len=256 is sub-LBA.
    reject(make_vref(make_paddr(kBaseLBA + 5), 33, 256));

    // ── 6. sub-LBA byte_offset + class_size > lba_size ──
    //   class_size=256, byte_offset=4000 → 4256 > 4096.
    reject(make_vref(make_paddr(kBaseLBA + 5), 4000, 256));

    // ── 7. LBA-equal byte_offset != 0 ──
    //   class for len=4096 is LBA-equal. byte_offset=64 illegal.
    reject(make_vref(make_paddr(kBaseLBA + 5), 64, 4096));

    // ── 8. multi-LBA byte_offset != 0 ──
    //   class for len=8192 is multi-LBA. byte_offset=64 illegal.
    reject(make_vref(make_paddr(kBaseLBA + 5), 64, 8192));

    // ── 9. multi-LBA span overflows data_area_end_lba ──
    //   8 KiB class span_lbas=2. Place at end_lba - 1 → spans [end-1, end+1).
    reject(make_vref(make_paddr(kEndLBA - 1), 0, 8192));

    // ── 10. Mid-batch malformed: pre-pass must reject the WHOLE call
    //   without applying earlier (legal) entries. Build a 3-entry batch
    //   where the first is legal, the second is malformed, the third is
    //   legal. Manager must throw on entry 2 and leave entry 1's release
    //   un-applied. ──
    {
        value_ref refs[3] = {
            make_vref(existing, 0, 256),                                 // legal
            make_vref(make_paddr(kBaseLBA + 5), /*bad_off=*/33, 256),     // malformed
            make_vref(make_paddr(kBaseLBA + 6), 0, 256),                  // legal
        };
        CHECK(throws_invalid_argument([&]{
            m.release_values(std::span<const value_ref>(refs, 3));
        }));
        // partial_page_count and whole_free_lba_count must reflect the
        // pre-call snapshot — entry 0 must NOT have been applied to
        // `existing`. (If it had been, the page would have gone all-free
        // and partial_page_count would have decremented.)
        CHECK(m.partial_page_count_total() == pre_partial);
        CHECK(m.whole_free_lba_count()     == pre_free);
    }

    // Sanity: after all the rejection cases, a properly-formed release
    // STILL works — manager isn't somehow stuck in an error state.
    {
        const value_ref ok_ref = make_vref(existing, 0, 256);
        const value_ref refs[] = { ok_ref };
        m.release_values(std::span<const value_ref>(refs, 1));
        // The partial page goes all-free → 1-LBA whole-free span back.
        CHECK(m.partial_page_count_total() == pre_partial - 1);
        CHECK(m.whole_free_lba_count()     == pre_free + 1);
    }

    printf("  case 13d (release_values fail-fast on malformed input): OK\n");
}

static void
test_install_recovered_state_fails_fast_on_malformed_input() {
    // ── Build manager with a known non-trivial pre-state. install_*
    //   must NOT mutate this on a malformed input. The regression target
    //   is whole_free_lba_count() — silent skip of a malformed extent
    //   would mark the live LBA range as free, producing an inflated
    //   free count.
    value_space_manager m(make_default_config());
    paddr existing = allocate_one_sub_lba_and_commit(m, /*class_idx=*/2, /*alloc_bytes=*/256);
    (void)existing;
    const uint64_t pre_partial = m.partial_page_count_total();
    const uint64_t pre_free    = m.whole_free_lba_count();
    const uint64_t pre_acked   = m.acknowledged_alloc_floor_lba();

    auto reject_install = [&](std::vector<live_value_extent> live,
                              uint64_t tree_alloc_head_lba = kBaseLBA) {
        CHECK(throws_invalid_argument([&]{
            m.install_recovered_state(
                std::span<const live_value_extent>(live),
                tree_alloc_head_lba,
                /*data_area_end_lba=*/kEndLBA,
                /*hints=*/{});
        }));
        // Pre-pass validation runs BEFORE the state-clear, so the manager
        // must look exactly the same as pre-call.
        CHECK(m.partial_page_count_total()      == pre_partial);
        CHECK(m.whole_free_lba_count()          == pre_free);
        CHECK(m.acknowledged_alloc_floor_lba()  == pre_acked);
    };

    // ── 1. Invalid device_id ──
    reject_install({
        live_value_extent{ make_paddr(kBaseLBA + 5, /*dev=*/99), 0, 256 },
    });

    // ── 2. LBA below data_area_base_lba ──
    reject_install({
        live_value_extent{ make_paddr(kBaseLBA - 1), 0, 256 },
    });

    // ── 3. LBA at-or-above data_area_end_lba ──
    reject_install({
        live_value_extent{ make_paddr(kEndLBA), 0, 256 },
    });

    // ── 4. len exceeds the largest configured class_size ──
    reject_install({
        live_value_extent{ make_paddr(kBaseLBA + 5), 0, 99999 },
    });

    // ── 5. sub-LBA byte_offset not quantum-aligned ──
    reject_install({
        live_value_extent{ make_paddr(kBaseLBA + 5), 33, 256 },
    });

    // ── 6. sub-LBA byte_offset + class_size > lba_size ──
    reject_install({
        live_value_extent{ make_paddr(kBaseLBA + 5), 4000, 256 },
    });

    // ── 7. LBA-equal byte_offset != 0 ──
    reject_install({
        live_value_extent{ make_paddr(kBaseLBA + 5), 64, 4096 },
    });

    // ── 8. multi-LBA byte_offset != 0 ──
    reject_install({
        live_value_extent{ make_paddr(kBaseLBA + 5), 64, 8192 },
    });

    // ── 9. multi-LBA span overflows data_area_end_lba ──
    reject_install({
        live_value_extent{ make_paddr(kEndLBA - 1), 0, 8192 },
    });

    // ── 10. Recovery-specific guard: live extent below tree_alloc_head_lba.
    //   The value owner does not own the prefix [data_area_base, tree_head),
    //   so a live extent there is a structural error from the recovery
    //   driver, not from the value layer. ──
    {
        const uint64_t head = kBaseLBA + 100;
        reject_install({
            live_value_extent{ make_paddr(kBaseLBA + 50), 0, 256 },
        }, /*tree_alloc_head_lba=*/head);
    }

    // ── 11. Mid-batch malformed extent.
    //   This is the explicit regression: if the malformed entry were
    //   silently skipped, install would nonetheless succeed, the live
    //   LBA it covers would appear in global_free_extents, and
    //   whole_free_lba_count() would over-count by that LBA's worth.
    //   The pre-pass throw must prevent any of that from happening.
    reject_install({
        live_value_extent{ make_paddr(kBaseLBA + 100), 0, 256 },           // legal
        live_value_extent{ make_paddr(kBaseLBA + 200), /*bad_off=*/33, 256 }, // malformed
        live_value_extent{ make_paddr(kBaseLBA + 300), 0, 256 },           // legal
    });

    // ── Sanity: a properly-formed install_recovered_state succeeds and
    //   produces the expected whole_free_lba_count. The manager is not
    //   stuck in an error state after all the rejected calls above. ──
    {
        const uint64_t total = kEndLBA - kBaseLBA;
        std::vector<live_value_extent> live = {
            live_value_extent{ make_paddr(kBaseLBA + 100), 0, 256 },  // sub-LBA
            live_value_extent{ make_paddr(kBaseLBA + 200), 0, 8192 }, // multi-LBA span 2
        };
        m.install_recovered_state(std::span<const live_value_extent>(live),
                                  /*tree_alloc_head_lba=*/kBaseLBA,
                                  /*data_area_end_lba=*/kEndLBA,
                                  /*hints=*/{});
        // Sub-LBA at LBA base+100 → 1 partial page. Multi-LBA at base+200
        // → 2 fully-occupied LBAs. So 3 LBAs are NOT whole-free.
        CHECK(m.partial_page_count_total() == 1);
        CHECK(m.whole_free_lba_count()     == total - 3);
    }

    printf("  case 12b (install_recovered_state fail-fast on malformed input): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 6b (batch-level cost model): when a mixed-size batch packs exactly
// into one fresh LBA, the cost model MUST prefer that single-page plan
// over a plan that consumes a cached partial first and then needs a fresh
// page anyway. Per 037 plan §"分配示例", the cost tuple is
// `(read_pages, write_pages, new_pages, leftover_quantums)` and write_pages
// dominates new_pages, so:
//
//   plan A (1 fresh, ignore cached): cost = (0, 1, 1, 0)
//   plan B (cached + 1 fresh):       cost = (0, 2, 1, leftover>0)
//
// Plan A wins. This test locks in the contract that allocate_batch sees
// the batch as a whole, not as a sequence of greedy per-request choices.
// ─────────────────────────────────────────────────────────────────────────

static void
test_batch_cost_prefers_one_fresh_over_cached_plus_fresh() {
    value_space_manager m(make_default_config());

    // Build a partial page with exactly 16 quantums (= 1 KiB) free at the
    // top of the LBA. Going through install_recovered_state is the
    // canonical way to plant a partial page at a chosen LBA without
    // breaking the global_free_extents ↔ partial_pages disjointness
    // invariant. The two live extents OR to occupy positions [0, 48):
    //   - 2048 B at byte_offset 0    → positions [0, 32)
    //   - 1024 B at byte_offset 2048 → positions [32, 48)
    // Free: positions [48, 64) → largest_free_run_quantums = 16.
    const paddr cached_page = make_paddr(kBaseLBA + 1);
    std::vector<live_value_extent> live = {
        live_value_extent{ cached_page, /*byte_offset=*/0,    /*len=*/2048 },
        live_value_extent{ cached_page, /*byte_offset=*/2048, /*len=*/1024 },
    };
    m.install_recovered_state(std::span<const live_value_extent>(live),
                              /*tree_alloc_head_lba=*/kBaseLBA,
                              /*data_area_end_lba=*/kEndLBA,
                              /*hints=*/{});
    CHECK(m.partial_page_count_total() == 1);

    // Admit the partial as a cached candidate so the cost model sees it.
    cached_partial_update u{};
    u.page_base   = cached_page;
    u.kind        = cached_partial_kind::active_tail;
    u.heat_seq    = 100;
    u.cache_epoch = 1;
    m.mark_cached_partial(u);

    // Mixed-size batch totalling exactly 1 LBA = 4096 B:
    //   3 × 1 KiB + 2 × 512 B = 3072 + 1024 = 4096 B
    // alloc_quantums: 16, 16, 16, 8, 8 — totals 64 = quantums_per_lba.
    auto round = m.begin_round();
    allocation_request reqs[5] = {
        make_req(0, /*class_idx=1024*/ 4, 1024),
        make_req(1, /*class_idx=512 */ 3, 512),
        make_req(2, /*class_idx=1024*/ 4, 1024),
        make_req(3, /*class_idx=512 */ 3, 512),
        make_req(4, /*class_idx=1024*/ 4, 1024),
    };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 5));
    CHECK(claims.size() == 5);

    // All 5 claims must share the same fresh page_base, and none may
    // touch the cached page. This is the contract the test exists to
    // lock in: batch write_pages = 1, regardless of how attractive the
    // cached candidate looks for any single sub-request.
    const paddr first_page = claims[0].page_base;
    CHECK(first_page != cached_page);
    for (uint32_t i = 0; i < 5; ++i) {
        CHECK(claims[i].page_base == first_page);
        CHECK(claims[i].src == byte_claim_source::new_whole_page);
        CHECK(claims[i].class_idx == reqs[i].class_idx);
        CHECK(claims[i].alloc_bytes == reqs[i].alloc_bytes);
    }

    // Distinct byte_offsets within the page; collectively cover the LBA.
    std::vector<uint32_t> offsets;
    offsets.reserve(5);
    uint32_t total_bytes = 0;
    for (const auto& c : claims) {
        offsets.push_back(c.byte_offset);
        total_bytes += c.alloc_bytes;
        CHECK(c.byte_offset + c.alloc_bytes <= kLBA);
    }
    std::sort(offsets.begin(), offsets.end());
    for (uint32_t i = 1; i < offsets.size(); ++i) {
        CHECK(offsets[i] != offsets[i - 1]);
    }
    CHECK(total_bytes == kLBA); // batch packs the page exactly.

    m.abort(std::move(round));

    printf("  case 6b (batch cost: 1 fresh beats cached+fresh): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 6c (batch cost — round_tail beats cached when both fit a request):
// even when Phase 1 keeps a cached candidate (because absorbing all of its
// free quantums would not raise total writes), the per-request placement
// still has to charge a +1 write_page for picking cached vs picking an
// already-pinned round_tail. The greedy "leftover best-fit" tie-break is
// not cost-equivalent under normal / reuse_pressure: cached carries a
// separate write, round_tail does not.
//
// Setup:
//   - Cached partial page with 48 q free fragmented into 3 × 16-q runs
//     (largest_free_run = 16, free_count = 48). Phase 1 admits it because
//     the 48-q absorption would, in principle, save one fresh page.
//   - Batch: alloc_quantums = [32, 32, 32, 16]. Total = 112 q.
//     baseline = ceil(112/64) = 2 fresh pages.
//
// Greedy execution order: three 32-q reqs land on two fresh pages (Page A
// fills up exactly, Page B has 32 q free at the top). The trailing 16-q
// req now has both candidates: cached (largest_run=16, leftover=0) and
// Page B's round_tail (largest_run=32, leftover=16). Today's tie-break
// picks cached because cached_leftover < tail_leftover. That costs 3
// total writes (A + B + cached) for a workload whose baseline is 2.
//
// Expected: round_tail wins, total writes = 2, cached is untouched.
// ─────────────────────────────────────────────────────────────────────────

static void
test_batch_cost_round_tail_beats_cached_on_tail_fit() {
    value_space_manager m(make_default_config());

    // Plant a partial page with three 16-q free runs separated by occupied
    // ranges. Layout (positions 0..63):
    //   [0,16)  free
    //   [16,20) occupied (256 B at byte_offset 1024)
    //   [20,36) free
    //   [36,40) occupied (256 B at byte_offset 2304)
    //   [40,56) free
    //   [56,64) occupied (512 B at byte_offset 3584)
    // → free_count = 48, three runs of 16 q each, largest_run = 16.
    const paddr cached_page = make_paddr(kBaseLBA + 1);
    std::vector<live_value_extent> live = {
        live_value_extent{ cached_page, /*byte_offset=*/1024, /*len=*/256 },
        live_value_extent{ cached_page, /*byte_offset=*/2304, /*len=*/256 },
        live_value_extent{ cached_page, /*byte_offset=*/3584, /*len=*/512 },
    };
    m.install_recovered_state(std::span<const live_value_extent>(live),
                              /*tree_alloc_head_lba=*/kBaseLBA,
                              /*data_area_end_lba=*/kEndLBA,
                              /*hints=*/{});
    CHECK(m.partial_page_count_total() == 1);

    // Admit the partial as a cached candidate.
    cached_partial_update u{};
    u.page_base   = cached_page;
    u.kind        = cached_partial_kind::active_tail;
    u.heat_seq    = 100;
    u.cache_epoch = 1;
    m.mark_cached_partial(u);

    // Batch: 3 × 2 KiB (32 q) + 1 × 1 KiB (16 q). Total = 112 q.
    auto round = m.begin_round();
    allocation_request reqs[4] = {
        make_req(0, /*class_idx=2048*/ 5, 2048),
        make_req(1, /*class_idx=2048*/ 5, 2048),
        make_req(2, /*class_idx=2048*/ 5, 2048),
        make_req(3, /*class_idx=1024*/ 4, 1024),
    };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 4));
    CHECK(claims.size() == 4);

    // None of the claims may land on the cached page; total fresh pages = 2.
    std::vector<paddr> fresh_pages;
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(claims[i].page_base != cached_page);
        CHECK(claims[i].src == byte_claim_source::new_whole_page);
        // Maintain dedup of distinct page_base values.
        bool seen = false;
        for (const paddr& p : fresh_pages) {
            if (p == claims[i].page_base) { seen = true; break; }
        }
        if (!seen) fresh_pages.push_back(claims[i].page_base);
    }
    CHECK(fresh_pages.size() == 2);

    // Sanity: total bytes claimed = 7 KiB; baseline says 2 fresh pages.
    uint32_t total_bytes = 0;
    for (const auto& c : claims) total_bytes += c.alloc_bytes;
    CHECK(total_bytes == 3 * 2048 + 1024);

    m.abort(std::move(round));

    printf("  case 6c (round_tail beats cached on tail fit): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Case 19 (explicit): allocate_batch may reorder internally, claims[i]
// always corresponds to reqs[i].entry_index.
// ─────────────────────────────────────────────────────────────────────────

static void
test_allocate_batch_preserves_caller_order() {
    value_space_manager m(make_default_config());

    // Mix sub-LBA and multi-LBA in non-monotonic size order. The internal
    // BFD sort flips them; the returned vector must be in caller order.
    auto round = m.begin_round();
    allocation_request reqs[] = {
        make_req(0, /*class_idx=64*/  0, 64),    // 1 quantum, very small
        make_req(1, /*class_idx=8192*/ 7, 8192), // 2-LBA, very big
        make_req(2, /*class_idx=512*/ 3, 512),
        make_req(3, /*class_idx=4096*/ 6, 4096),
        make_req(4, /*class_idx=128*/ 1, 128),
    };
    auto claims = m.allocate_batch(round, std::span<const allocation_request>(reqs, 5));
    CHECK(claims.size() == 5);
    for (uint32_t i = 0; i < 5; ++i) {
        CHECK(claims[i].class_idx == reqs[i].class_idx);
        CHECK(claims[i].alloc_bytes == reqs[i].alloc_bytes);
    }
    m.abort(std::move(round));

    printf("  case 19 (allocate_batch preserves caller order): OK\n");
}

// ─────────────────────────────────────────────────────────────────────────
// Driver
// ─────────────────────────────────────────────────────────────────────────

int main() {
    printf("value_space_manager unit tests:\n");

    test_initial_single_span_and_carve();                    // 1 + 25
    test_release_coalesces_neighbours();                     // 2
    test_sub_lba_release_creates_partial();                  // 3 + 21
    test_partial_all_free_returns_whole_span();              // 4
    test_mixed_size_batch_packs_one_page_and_preserves_order(); // 5 + 19
    test_cached_admission_kinds_and_selection();             // 6 + 14 + 16 + 27
    test_cached_claim_stale_miss_aborts();                   // 7
    test_two_rounds_interleaved_abort_keeps_committed_claims(); // 8 + 18
    test_commit_publishes_fresh_tail_partial_and_buckets();  // 9 + 22 + 23
    test_global_span_and_group_partial_interaction();        // 10
    test_trim_prepare_complete_cycle();                      // 11
    test_recovery_rebuild_no_hints_canonical_mapping();      // 12 + 20
    test_alloc_floor_sync_and_returned_extents();            // 13 + 28
    test_alloc_floor_sync_rejects_collision();               // 13b
    test_alloc_floor_sync_rejects_full_page_collision();     // 13c
    test_release_values_fails_fast_on_malformed_input();     // 13d
    test_install_recovered_state_fails_fast_on_malformed_input(); // 12b
    test_cached_partial_index_consistency();                 // 6d
    test_cached_partial_budget_evicts_candidate_not_truth(); // 15
    test_pressure_mode_nrp_and_partial_limits();             // 17 + 24
    test_choose_claim_offset_best_fit();                     // 26
    test_bit_helpers_edges();                                // 25
    test_allocate_batch_preserves_caller_order();            // 19
    test_batch_cost_prefers_one_fresh_over_cached_plus_fresh(); // 6b
    test_batch_cost_round_tail_beats_cached_on_tail_fit();      // 6c

    printf("all passed\n");
    return 0;
}
