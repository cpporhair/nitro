//
// Phase 3 (step 023) flush carrier boundary tests.
//
// Verifies that the carrier-only deliverables of step 023 actually behave
// the way the design doc demands. The implementation step is "carrier
// only" — no fold / leaf mapping / candidate build / writer — so this
// suite focuses on:
//
//   1. leaf_order layout, dedupe view semantics, and zero-length sentinel
//      handling (023 §1, D2-D5)
//   2. tree_manifest empty-bootstrap leaf_order integration (023 §2, D5)
//   3. checkpoint_guard pin lifetime (023 §3, D27)
//   4. retired_objects layout (023 §4, D28-D29)
//   5. tree_flush_request owning sealed_gens — Δ-1 (023 D8)
//   6. tree_flush_result single-status field — Δ-2 (023 D10-D12)
//   7. flush_round_state default empty shape (023 §6, D13-D15)
//   8. tree_state field set + defaults aligned with RSM §4.1 (023 D19, D20)
//   9. tree_sched D22 fail-fast: panic on null base_guard / empty
//      sealed_gens — exercised in forked children
//  10. tree_sched advance() drain cap kMaxFlushOpsPerAdvance (023 §7)
//  11. handle pin release through the value-path unsupported_unimplemented
//      reply (023 D22)
//  12. runtime singleton lifecycle: build → tree_sched_singleton non-null;
//      destroy → singleton pointer cleared (023 §9, D24-D26)
//
// Compile-time invariants (Δ-1, Δ-2, D27, D30, borrowed view types,
// leaf_span layout) are enforced via static_assert at the top of the
// translation unit so any future drift fails the build, not the test.
//
// Panic tests use fork() because `core::panic_inconsistency` calls
// abort() and would terminate the entire test process. The fork happens
// while the parent is still single-threaded; subsequent multi-threaded
// runtime tests run AFTER all forks have been reaped.
//

#include "apps/inconel/test/check.hh"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pump/core/lock_free_queue.hh"

#include "apps/inconel/core/checkpoint_guard.hh"
#include "apps/inconel/core/clock_cache.hh"
#include "apps/inconel/core/leaf_order.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/retired_objects.hh"
#include "apps/inconel/core/slru_cache.hh"
#include "apps/inconel/core/tree_geometry.hh"
#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/types.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/tree/flush_round_state.hh"
#include "apps/inconel/tree/flush_types.hh"
#include "apps/inconel/tree/owner_scheduler.hh"

using namespace apps::inconel;

namespace {

// ────────────────────────────────────────────────────────────────────────
// Compile-time invariants (drift here fails the build, not the test)
// ────────────────────────────────────────────────────────────────────────

// 023 §1 / D4: leaf_span layout is frozen at 24 bytes and POD.
static_assert(sizeof(core::leaf_span) == 24,
              "leaf_span sizeof drift — 023 §1 D4");
static_assert(std::is_trivially_copyable_v<core::leaf_span>,
              "leaf_span must remain a POD");

// 023 §2 / D7: leaf_order_index is by-value inside tree_manifest, not a
// pointer. Asserting the static type (not just the address) keeps a
// future "let me put it behind a unique_ptr" refactor visible.
static_assert(std::is_same_v<
    decltype(std::declval<core::tree_manifest>().leaf_order),
    core::leaf_order_index>,
    "tree_manifest.leaf_order must be by-value (023 §2 / D7)");

// Δ-1 (023 §与 RSM §4.1/§4.2 的偏差): tree_flush_request.sealed_gens is
// the owning InlinedVector, not the std::span the original RSM §4.2
// declared. The borrowed-span shape would repeat the 022 review M-3
// dangling-payload bug across the tree_sched ingress queue.
static_assert(std::is_same_v<
    decltype(std::declval<tree::tree_flush_request>().sealed_gens),
    absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>>,
    "Δ-1: sealed_gens must be owning InlinedVector");

// Δ-2 (023 §与 RSM §4.1/§4.2 的偏差): tree_flush_result must use the
// single `flush_stage_status st` field, not the (bool ok + flush_error
// error) pair from the original RSM §4.2 text.
template <typename T>
concept has_field_ok = requires(T& t) { t.ok; };
template <typename T>
concept has_field_error = requires(T& t) { t.error; };
template <typename T>
concept has_field_st = requires(T& t) { t.st; };
static_assert(!has_field_ok<tree::tree_flush_result>,
              "Δ-2: tree_flush_result must NOT carry `bool ok`");
static_assert(!has_field_error<tree::tree_flush_result>,
              "Δ-2: tree_flush_result must NOT carry `flush_error error`");
static_assert(has_field_st<tree::tree_flush_result>,
              "Δ-2: tree_flush_result must carry `flush_stage_status st`");

// 023 D27: checkpoint_guard is the minimal Phase 3 form — manifest
// only, no `retired` field. The full form lands in the frontier_switch
// step.
template <typename T>
concept has_field_retired = requires(T& t) { t.retired; };
static_assert(!has_field_retired<core::checkpoint_guard>,
              "D27: checkpoint_guard.retired is frontier_switch step");

// 023 D30: tree_state must NOT pre-create the Phase 4 fields. Leaving
// them out means the diff that adds them in Phase 4 is the visible
// signal to reviewers that round-state plumbing has begun.
template <typename T>
concept has_active_rounds = requires(T& t) { t.active_rounds; };
template <typename T>
concept has_next_round_id = requires(T& t) { t.next_round_id; };
static_assert(!has_active_rounds<tree::tree_state>,
              "D30: tree_state.active_rounds is Phase 4");
static_assert(!has_next_round_id<tree::tree_state>,
              "D30: tree_state.next_round_id is Phase 4");

// 023 D20: tree_allocator is a Phase 3 placeholder with no methods.
// `allocate()` / `recycle(...)` belong to Phase 7. SFINAE-detect that
// neither exists, so any premature implementation surfaces here.
template <typename T>
concept has_method_allocate = requires(T& t) { t.allocate(); };
template <typename T>
concept has_method_recycle = requires(T& t) { t.recycle(format::range_ref{}); };
static_assert(!has_method_allocate<tree::tree_allocator>,
              "D20: tree_allocator.allocate() is Phase 7");
static_assert(!has_method_recycle<tree::tree_allocator>,
              "D20: tree_allocator.recycle() is Phase 7");

// 023 §最少验证范围 #7 / G6: borrowed views inside the lookup / worker
// requests are spans of const elements (read-only borrows that point
// into the round-owned vectors of flush_round_state).
static_assert(std::is_same_v<
    decltype(std::declval<tree::flush_lookup_req>().groups),
    std::span<const tree::flush_key_group>>,
    "G6: flush_lookup_req.groups must be std::span<const flush_key_group>");
static_assert(std::is_same_v<
    decltype(std::declval<tree::flush_worker_req>().leaf_groups),
    std::span<const tree::flush_leaf_group>>,
    "G6: flush_worker_req.leaf_groups must be std::span<const flush_leaf_group>");

// 023 review H-1: flush_round_state must mirror EVERY tree_flush_result
// field, including `st` and `new_manifest`. The earlier draft only
// mirrored 4 of 6 fields and would have forced Phase 7 to either
// re-carve the carrier or stash overflow state somewhere off to the
// side — both signs that the carrier freeze was incomplete. SFINAE
// detect both fields by name AND by type so a future "let me drop the
// shared_ptr because the writer can park it elsewhere" refactor fails
// here, not at the Phase 7 stage where it would be expensive to roll
// back.
template <typename T>
concept has_round_state_st = requires(T& t) { t.st; };
template <typename T>
concept has_round_state_new_manifest = requires(T& t) { t.new_manifest; };
static_assert(has_round_state_st<tree::flush_round_state>,
              "H-1: flush_round_state must mirror tree_flush_result.st");
static_assert(has_round_state_new_manifest<tree::flush_round_state>,
              "H-1: flush_round_state must mirror tree_flush_result.new_manifest");
static_assert(std::is_same_v<
    decltype(std::declval<tree::flush_round_state>().st),
    tree::flush_stage_status>,
    "H-1: flush_round_state.st must match tree_flush_result.st type");
static_assert(std::is_same_v<
    decltype(std::declval<tree::flush_round_state>().new_manifest),
    std::shared_ptr<const core::tree_manifest>>,
    "H-1: flush_round_state.new_manifest must match tree_flush_result.new_manifest type");

// ────────────────────────────────────────────────────────────────────────
// Bootstrap geometry + helpers
// ────────────────────────────────────────────────────────────────────────

constexpr core::tree_geometry kTestGeom{
    .lba_size               = 4096,
    .tree_page_size         = 4096,
    .shadow_slots_per_range = 1,
};

static std::shared_ptr<const core::checkpoint_guard>
make_empty_guard() {
    auto manifest = std::make_shared<const core::tree_manifest>(
        core::tree_manifest::empty(&kTestGeom));
    return std::make_shared<const core::checkpoint_guard>(
        core::checkpoint_guard{ .manifest = manifest });
}

static std::shared_ptr<core::memtable_gen>
make_dummy_sealed_gen(uint64_t gen_id) {
    auto g = std::make_shared<core::memtable_gen>();
    g->gen_id  = gen_id;
    g->st      = core::memtable_gen::state::sealed;
    g->min_lsn = gen_id;
    g->max_lsn = gen_id;
    return g;
}

static uint64_t
bootstrap_namespace_size_bytes() {
    const auto& profile = format::kBootstrapFormatProfile;
    return static_cast<uint64_t>(profile.lba_size) *
           profile.value_data_area_end.lba;
}

// ────────────────────────────────────────────────────────────────────────
// Test 1: leaf_span layout exercised at runtime as well
// ────────────────────────────────────────────────────────────────────────

static void
test_leaf_span_layout() {
    core::leaf_span s{};
    CHECK(sizeof(s) == 24);
    s.fence_lower_off = 1;
    s.fence_upper_off = 2;
    s.fence_lower_len = 3;
    s.fence_upper_len = 4;
    s.leaf_range_base = format::paddr{ .device_id = 7, .lba = 100 };
    auto t = s;  // POD copy
    CHECK(t.fence_lower_off == 1);
    CHECK(t.fence_upper_off == 2);
    CHECK(t.fence_lower_len == 3);
    CHECK(t.fence_upper_len == 4);
    CHECK(t.leaf_range_base.device_id == 7);
    CHECK(t.leaf_range_base.lba == 100);
    printf("  leaf_span runtime layout: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 2: leaf_order_index default-constructed empty
// ────────────────────────────────────────────────────────────────────────

static void
test_leaf_order_index_default_empty() {
    core::leaf_order_index idx;
    CHECK(idx.empty());
    CHECK(idx.size() == 0);
    CHECK(idx.fence_pool.empty());
    CHECK(idx.spans.empty());
    printf("  leaf_order_index default empty: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 3: leaf_order_index 2-leaf shared-fence dedupe view
//
// Manually populated index — Phase 3 has no builder. fence_pool is the
// 3-byte string "akz" so leaf[0]=[a,k) and leaf[1]=[k,z). The dedupe
// invariant (D3) requires that the byte address of leaf[0].upper EQUAL
// the byte address of leaf[1].lower; both views must be backed by the
// SAME single byte at fence_pool[1], not by two copies.
// ────────────────────────────────────────────────────────────────────────

static void
test_leaf_order_index_shared_fence_views() {
    core::leaf_order_index idx;
    idx.fence_pool = "akz";
    idx.spans = {
        { .fence_lower_off = 0, .fence_upper_off = 1,
          .fence_lower_len = 1, .fence_upper_len = 1,
          .leaf_range_base = { 0, 100 } },
        { .fence_lower_off = 1, .fence_upper_off = 2,
          .fence_lower_len = 1, .fence_upper_len = 1,
          .leaf_range_base = { 0, 200 } },
    };

    auto upper0 = idx.fence_upper(idx.spans[0]);
    auto lower1 = idx.fence_lower(idx.spans[1]);
    CHECK(upper0.size() == 1);
    CHECK(lower1.size() == 1);
    CHECK(upper0[0] == 'k');
    CHECK(lower1[0] == 'k');
    // The defining property of dedupe: same offset → same data pointer.
    // If a future refactor copies fence bytes per leaf, this fails.
    CHECK(upper0.data() == lower1.data());

    auto lower0 = idx.fence_lower(idx.spans[0]);
    auto upper1 = idx.fence_upper(idx.spans[1]);
    CHECK(lower0 == "a");
    CHECK(upper1 == "z");
    CHECK(idx.size() == 2);
    CHECK(!idx.empty());

    // Adjacent leaves share a single physical byte; the pool itself
    // therefore stays at exactly 3 bytes for two leaves, NOT at 4.
    CHECK(idx.fence_pool.size() == 3);
    printf("  leaf_order_index dedupe view: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 4: leaf_order_index zero-length sentinel fence
//
// The last leaf in a tree may use a +inf upper-bound sentinel, expressed
// as a zero-length fence. The accessor must return an empty string_view
// without UB.
// ────────────────────────────────────────────────────────────────────────

static void
test_leaf_order_index_sentinel_fence() {
    core::leaf_order_index idx;
    idx.fence_pool = "z";
    idx.spans = {
        { .fence_lower_off = 0, .fence_upper_off = 0,
          .fence_lower_len = 1, .fence_upper_len = 0,  // +inf upper
          .leaf_range_base = { 0, 999 } },
    };
    auto lo = idx.fence_lower(idx.spans[0]);
    auto hi = idx.fence_upper(idx.spans[0]);
    CHECK(lo == "z");
    CHECK(hi.empty());
    CHECK(hi.size() == 0);
    printf("  leaf_order_index sentinel fence: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 5: tree_manifest::empty has empty leaf_order
// ────────────────────────────────────────────────────────────────────────

static void
test_tree_manifest_empty_leaf_order() {
    auto m = core::tree_manifest::empty(&kTestGeom);
    CHECK(!m.has_root());
    CHECK(m.leaf_order.empty());
    CHECK(m.leaf_order.size() == 0);
    CHECK(m.leaf_order.fence_pool.empty());
    CHECK(m.geom == &kTestGeom);
    CHECK(m.slot_map.empty());
    printf("  tree_manifest::empty has empty leaf_order: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 6: checkpoint_guard pin lifetime
// ────────────────────────────────────────────────────────────────────────

static void
test_checkpoint_guard_pin() {
    auto manifest = std::make_shared<const core::tree_manifest>(
        core::tree_manifest::empty(&kTestGeom));
    CHECK(manifest.use_count() == 1);
    {
        core::checkpoint_guard g{ .manifest = manifest };
        CHECK(manifest.use_count() == 2);
    }
    CHECK(manifest.use_count() == 1);
    printf("  checkpoint_guard pin lifetime: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 7: retired_objects defaults + push semantics
// ────────────────────────────────────────────────────────────────────────

static void
test_retired_objects_default_and_push() {
    core::retired_objects r;
    CHECK(r.old_slots.empty());
    CHECK(r.old_ranges.empty());
    CHECK(r.old_tree_values.empty());

    r.old_slots.push_back(format::paddr{ 0, 100 });
    r.old_ranges.push_back(format::range_ref{ .base = { 0, 200 },
                                              .slot_count = 4 });
    r.old_tree_values.push_back(core::retired_value_ref{
        .vr       = format::value_ref{ .base = { 0, 300 },
                                       .byte_offset = 0,
                                       .len = 16,
                                       .flags = 0 },
        .data_ver = 42 });

    CHECK(r.old_slots.size() == 1);
    CHECK(r.old_ranges.size() == 1);
    CHECK(r.old_tree_values.size() == 1);
    CHECK(r.old_slots[0].lba == 100);
    CHECK(r.old_ranges[0].slot_count == 4);
    CHECK(r.old_tree_values[0].data_ver == 42);
    printf("  retired_objects default + push: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 8: tree_flush_request owning sealed_gens (Δ-1)
//
// Verifies the owning InlinedVector pins each shared_ptr through the
// request's lifetime, and that destroying the request releases the pins.
// This is the runtime evidence behind the deviation from RSM §4.2 — if
// a future refactor reverts sealed_gens to std::span, this test fails
// because the spans wouldn't bump refcount.
// ────────────────────────────────────────────────────────────────────────

static void
test_tree_flush_request_pin_semantics() {
    auto guard = make_empty_guard();
    auto gen0  = make_dummy_sealed_gen(10);
    auto gen1  = make_dummy_sealed_gen(11);

    CHECK(guard.use_count() == 1);
    CHECK(gen0.use_count()  == 1);
    CHECK(gen1.use_count()  == 1);

    {
        tree::tree_flush_request req{
            .base_guard        = guard,             // copy → +1
            .sealed_gens       = { gen0, gen1 },    // copies → +1 each
            .recovery_safe_lsn = 5,
        };
        CHECK(guard.use_count() == 2);
        CHECK(gen0.use_count()  == 2);
        CHECK(gen1.use_count()  == 2);
        CHECK(req.sealed_gens.size() == 2);
        CHECK(req.recovery_safe_lsn  == 5);
    }
    // After req drops, all pins released back to test scope only.
    CHECK(guard.use_count() == 1);
    CHECK(gen0.use_count()  == 1);
    CHECK(gen1.use_count()  == 1);
    printf("  tree_flush_request pin semantics (Δ-1): OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 9: tree_flush_result default state (Δ-2)
// ────────────────────────────────────────────────────────────────────────

static void
test_tree_flush_result_defaults() {
    tree::tree_flush_result r;
    CHECK(r.st == tree::flush_stage_status::ok);
    CHECK(!r.new_manifest);
    CHECK(r.retired.old_slots.empty());
    CHECK(r.retired.old_ranges.empty());
    CHECK(r.retired.old_tree_values.empty());
    CHECK(r.flushed_gens_by_front.empty());
    CHECK(r.memtable_losers.empty());
    CHECK(r.flushed_max_lsn == 0);
    printf("  tree_flush_result defaults (Δ-2): OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 10: flush_round_state default state
// ────────────────────────────────────────────────────────────────────────

static void
test_flush_round_state_defaults() {
    tree::flush_round_state s;
    CHECK(s.round_id.v == 0);
    CHECK(!s.pinned_base_guard);
    CHECK(s.pinned_gens.empty());
    CHECK(s.recovery_safe_lsn == 0);
    CHECK(s.workset.empty());
    CHECK(s.leaf_groups.empty());
    CHECK(s.candidates.empty());
    // 023 review H-1: round_state must mirror tree_flush_result.
    // Verify the two newly added mirror fields exist with the same
    // defaults the tree_flush_result default ctor uses.
    CHECK(s.st == tree::flush_stage_status::ok);
    CHECK(!s.new_manifest);
    CHECK(s.retired.old_slots.empty());
    CHECK(s.retired.old_ranges.empty());
    CHECK(s.retired.old_tree_values.empty());
    CHECK(s.flushed_gens_by_front.empty());
    CHECK(s.memtable_losers.empty());
    CHECK(s.flushed_max_lsn == 0);

    // Same field-by-field as tree_flush_result default — if any field
    // ever drifts between the two structs, this comparison breaks.
    tree::tree_flush_result r;
    CHECK(s.st == r.st);
    CHECK(s.new_manifest.get() == r.new_manifest.get());
    CHECK(s.retired.old_slots.size() == r.retired.old_slots.size());
    CHECK(s.flushed_gens_by_front.size() == r.flushed_gens_by_front.size());
    CHECK(s.memtable_losers.size() == r.memtable_losers.size());
    CHECK(s.flushed_max_lsn == r.flushed_max_lsn);
    printf("  flush_round_state defaults + H-1 mirror parity: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 11: tree_state default field values, RSM §4.1 alignment
// ────────────────────────────────────────────────────────────────────────

static void
test_tree_state_defaults() {
    pump::core::this_core_id = 0;  // reclaim_q construction needs it
    tree::tree_state s;
    CHECK(s.flush_max_lsn       == 0);
    CHECK(s.superblock_safe_lsn == 0);
    CHECK(s.recovery_safe_lsn   == 0);
    CHECK(s.alloc.head.lba       == 0);
    CHECK(s.alloc.head.device_id == 0);
    CHECK(s.alloc.free_ranges.empty());
    printf("  tree_state defaults aligned to RSM §4.1: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 12 + 13: D22 panic paths (forked children)
//
// `core::panic_inconsistency` calls abort(), so a runtime panic test
// has to run in a child process. The fork happens while the parent is
// still single-threaded — multi-threaded runtime tests are sequenced
// AFTER the panic block.
// ────────────────────────────────────────────────────────────────────────

static void
in_child_provoke_null_base_guard() {
    pump::core::this_core_id = 0;
    tree::tree_sched ts;
    auto* r = new tree::_tree_flush::req{
        .args = tree::tree_flush_request{
            .base_guard        = nullptr,
            .sealed_gens       = { make_dummy_sealed_gen(1) },
            .recovery_safe_lsn = 0,
        },
        .cb = [](tree::tree_flush_result&&) {
            std::fprintf(stderr,
                "child(null base_guard): cb fired but should have panicked\n");
            std::_Exit(2);
        },
    };
    ts.schedule_flush(r);
    ts.advance();  // expected: panic_inconsistency → abort()
    std::fprintf(stderr,
        "child(null base_guard): advance() returned but should have panicked\n");
    std::_Exit(3);
}

static void
in_child_provoke_empty_sealed_gens() {
    pump::core::this_core_id = 0;
    tree::tree_sched ts;
    auto* r = new tree::_tree_flush::req{
        .args = tree::tree_flush_request{
            .base_guard        = make_empty_guard(),
            .sealed_gens       = {},
            .recovery_safe_lsn = 0,
        },
        .cb = [](tree::tree_flush_result&&) {
            std::fprintf(stderr,
                "child(empty sealed_gens): cb fired but should have panicked\n");
            std::_Exit(2);
        },
    };
    ts.schedule_flush(r);
    ts.advance();  // expected: panic_inconsistency → abort()
    std::fprintf(stderr,
        "child(empty sealed_gens): advance() returned but should have panicked\n");
    std::_Exit(3);
}

// 023 review M-1: a non-null guard wrapping a null manifest must be
// rejected at the carrier-contract boundary, not silently masked as
// `unsupported_unimplemented`. Constructing the guard inline so the
// outer shared_ptr is non-null but the inner `manifest` field is the
// default-null shared_ptr.
static void
in_child_provoke_null_inner_manifest() {
    pump::core::this_core_id = 0;
    tree::tree_sched ts;
    auto bad_guard = std::make_shared<const core::checkpoint_guard>(
        core::checkpoint_guard{ .manifest = nullptr });
    auto* r = new tree::_tree_flush::req{
        .args = tree::tree_flush_request{
            .base_guard        = bad_guard,
            .sealed_gens       = { make_dummy_sealed_gen(1) },
            .recovery_safe_lsn = 0,
        },
        .cb = [](tree::tree_flush_result&&) {
            std::fprintf(stderr,
                "child(null inner manifest): cb fired but should have panicked\n");
            std::_Exit(2);
        },
    };
    ts.schedule_flush(r);
    ts.advance();  // expected: panic_inconsistency → abort()
    std::fprintf(stderr,
        "child(null inner manifest): advance() returned but should have panicked\n");
    std::_Exit(3);
}

// 023 review M-2: leaf_order accessors must panic with Inconel context
// on any out-of-bounds {offset, length}. Constructs an index whose pool
// is 2 bytes ("ab") and a span pointing at offset 10 — far outside the
// pool. The accessor must panic, not terminate via stdlib exception
// and not silently truncate.
static void
in_child_provoke_leaf_order_out_of_bounds() {
    core::leaf_order_index idx;
    idx.fence_pool = "ab";
    idx.spans = {
        { .fence_lower_off = 10, .fence_upper_off = 0,
          .fence_lower_len = 1,  .fence_upper_len = 1,
          .leaf_range_base = { 0, 0 } },
    };
    auto v = idx.fence_lower(idx.spans[0]);  // expected panic
    std::fprintf(stderr,
        "child(leaf_order OOB): fence_lower returned %.*s, should have panicked\n",
        static_cast<int>(v.size()), v.data());
    std::_Exit(3);
}

static void
expect_child_aborts(void (*fn)(), const char* label) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        fn();
        std::_Exit(0);  // unreachable; child must have panicked
    }
    int status = 0;
    pid_t r = waitpid(pid, &status, 0);
    CHECK(r == pid);
    // Either WIFSIGNALED with SIGABRT (raise()) or a clean abort exit;
    // panic_inconsistency uses std::abort(), which on Linux delivers
    // SIGABRT and the child terminates via the signal path.
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
    printf("  panic: %s → SIGABRT: OK\n", label);
}

static void
test_panics_d22() {
    expect_child_aborts(&in_child_provoke_null_base_guard,
                        "null base_guard");
    expect_child_aborts(&in_child_provoke_empty_sealed_gens,
                        "empty sealed_gens");
    // 023 review M-1
    expect_child_aborts(&in_child_provoke_null_inner_manifest,
                        "null inner manifest (M-1)");
    // 023 review M-2
    expect_child_aborts(&in_child_provoke_leaf_order_out_of_bounds,
                        "leaf_order out-of-bounds (M-2)");
}

// ────────────────────────────────────────────────────────────────────────
// Test 14: tree_sched advance() drain cap
//
// Submits kMaxFlushOpsPerAdvance + 1 valid requests, runs advance()
// once, verifies exactly kMaxFlushOpsPerAdvance fired. A second
// advance() drains the remainder. A third advance() reports no
// progress. Bypasses the PUMP runtime entirely — the cb is a plain
// lambda that doesn't talk to op_pusher.
// ────────────────────────────────────────────────────────────────────────

static void
test_advance_drain_cap() {
    pump::core::this_core_id = 0;
    tree::tree_sched ts;

    constexpr uint32_t cap   = tree::tree_sched::kMaxFlushOpsPerAdvance;
    constexpr uint32_t total = cap + 1;

    auto guard = make_empty_guard();
    auto gen   = make_dummy_sealed_gen(99);

    std::atomic<uint32_t> fired{0};
    for (uint32_t i = 0; i < total; ++i) {
        auto* r = new tree::_tree_flush::req{
            .args = tree::tree_flush_request{
                .base_guard        = guard,
                .sealed_gens       = { gen },
                .recovery_safe_lsn = i,
            },
            .cb = [&fired](tree::tree_flush_result&& res) {
                CHECK(res.st == tree::flush_stage_status::unsupported_unimplemented);
                CHECK(!res.new_manifest);
                CHECK(res.flushed_max_lsn == 0);
                CHECK(res.retired.old_slots.empty());
                CHECK(res.flushed_gens_by_front.empty());
                CHECK(res.memtable_losers.empty());
                fired.fetch_add(1, std::memory_order_release);
            },
        };
        ts.schedule_flush(r);
    }

    bool prog1 = ts.advance();
    CHECK(prog1);
    CHECK(fired.load() == cap);

    bool prog2 = ts.advance();
    CHECK(prog2);
    CHECK(fired.load() == total);

    bool prog3 = ts.advance();
    CHECK(!prog3);  // queue empty

    // After every cb fired and every req was deleted, the only pins
    // left on guard / gen are the test scope's originals.
    CHECK(guard.use_count() == 1);
    CHECK(gen.use_count()   == 1);
    printf("  advance() drain cap (cap=%u, total=%u): OK\n", cap, total);
}

// ────────────────────────────────────────────────────────────────────────
// Test 15: handle pin release through value-path cb
//
// A focused pin lifetime test: build one valid request, observe the
// shared_ptr counts before/after enqueue, after advance, and after the
// request struct has been deleted by tree_sched.
// ────────────────────────────────────────────────────────────────────────

static void
test_handle_pin_release_value_path() {
    pump::core::this_core_id = 0;
    tree::tree_sched ts;

    auto guard = make_empty_guard();
    auto gen   = make_dummy_sealed_gen(7);
    CHECK(guard.use_count() == 1);
    CHECK(gen.use_count()   == 1);

    bool fired = false;
    auto* r = new tree::_tree_flush::req{
        .args = tree::tree_flush_request{
            .base_guard        = guard,
            .sealed_gens       = { gen },
            .recovery_safe_lsn = 0,
        },
        .cb = [&fired](tree::tree_flush_result&& res) {
            CHECK(res.st == tree::flush_stage_status::unsupported_unimplemented);
            fired = true;
        },
    };
    // The req now holds a copy of guard and a copy of gen.
    CHECK(guard.use_count() == 2);
    CHECK(gen.use_count()   == 2);

    ts.schedule_flush(r);
    bool prog = ts.advance();
    CHECK(prog);
    CHECK(fired);
    // tree_sched::advance() deletes r after firing the cb, so both
    // pins drop back to the test scope only.
    CHECK(guard.use_count() == 1);
    CHECK(gen.use_count()   == 1);
    printf("  handle pin release after value-path cb: OK\n");
}

// ────────────────────────────────────────────────────────────────────────
// Test 16: runtime singleton lifecycle
//
// build_runtime → tree_sched_singleton() returns the same instance the
// PUMP per-core tuple stores on cores[0]. destroy_runtime → singleton
// pointer cleared. Repeated build/destroy cycles must be clean.
// ────────────────────────────────────────────────────────────────────────

template <core::cache_concept Cache>
static void
test_runtime_singleton_lifecycle(const char* label) {
    constexpr uint32_t LBS = 4096;
    CHECK(format::kBootstrapFormatProfile.lba_size == LBS);
    mock_nvme::mock_device dev(bootstrap_namespace_size_bytes(), LBS);

    std::vector<uint32_t> cores = {0};
    runtime::build_options opts{
        .cores               = cores,
        .device              = &dev,
        .tree_cache_capacity = 16,
    };

    auto* rt = runtime::build_runtime<Cache, Cache>(opts);

    auto* singleton = core::registry::tree_sched_singleton();
    CHECK(singleton != nullptr);
    CHECK(core::registry::tree_sched_singleton_ptr == singleton);

    auto* by_core_first =
        rt->template get_by_core<tree::tree_sched>(cores.front());
    CHECK(by_core_first == singleton);

    runtime::destroy_runtime<Cache, Cache>(rt);
    CHECK(core::registry::tree_sched_singleton_ptr == nullptr);

    // Second build/destroy cycle to make sure clear() left no residue.
    auto* rt2 = runtime::build_runtime<Cache, Cache>(opts);
    CHECK(core::registry::tree_sched_singleton() != nullptr);
    runtime::destroy_runtime<Cache, Cache>(rt2);
    CHECK(core::registry::tree_sched_singleton_ptr == nullptr);

    printf("  [%s] runtime tree_sched singleton lifecycle: OK\n", label);
}

}  // namespace

int
main() {
    printf("inconel flush carrier (Phase 3 / step 023) test:\n");

    // Pure type/value tests — no scheduler, no runtime, no fork.
    test_leaf_span_layout();
    test_leaf_order_index_default_empty();
    test_leaf_order_index_shared_fence_views();
    test_leaf_order_index_sentinel_fence();
    test_tree_manifest_empty_leaf_order();
    test_checkpoint_guard_pin();
    test_retired_objects_default_and_push();
    test_tree_flush_request_pin_semantics();
    test_tree_flush_result_defaults();
    test_flush_round_state_defaults();
    test_tree_state_defaults();

    // Panic tests must run while the parent is still single-threaded.
    test_panics_d22();

    // Direct tree_sched smoke (no PUMP runtime, single-threaded).
    test_advance_drain_cap();
    test_handle_pin_release_value_path();

    // Full runtime build / destroy lifecycle.
    test_runtime_singleton_lifecycle<core::clock_cache>("clock");
    test_runtime_singleton_lifecycle<core::slru_cache>("slru ");

    printf("all passed\n");
    return 0;
}
