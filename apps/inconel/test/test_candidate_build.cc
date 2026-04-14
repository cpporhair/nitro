//
// Phase 6 (step 026) candidate build tests.
//
// Exercises:
//   1. merge_and_build_leaf() — sorted merge, tombstone compact, overflow
//   2. process_candidate_groups() — two-pass (cache hit + temp buffer),
//      bounded reads, throttle budget
//   3. build_candidates_for_partition() — PUMP pipeline instantiation
//      (forces template instantiation, catches type errors)
//

#include "apps/inconel/test/check.hh"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "apps/inconel/core/clock_cache.hh"
#include "apps/inconel/core/leaf_order.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/slru_cache.hh"
#include "apps/inconel/core/tree_geometry.hh"
#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/format/tree_page.hh"
#include "apps/inconel/format/types.hh"
#include "apps/inconel/memory/frame.hh"
#include "apps/inconel/tree/candidate_build.hh"
#include "apps/inconel/tree/flush_types.hh"
#include "apps/inconel/tree/lookup_scheduler.hh"
#include "apps/inconel/tree/page_builder.hh"
#include "apps/inconel/tree/page_reader.hh"
#include "apps/inconel/tree/worker_scheduler.hh"

using namespace apps::inconel;
using namespace apps::inconel::tree;
using namespace apps::inconel::format;

namespace {

// ── helpers ──

// Parse a leaf page and return its record count. Replaces the old
// flush_leaf_candidate::record_count field after 026A turned the
// worker output into a manifest overlay (changed_nodes map of raw
// page bytes).
static uint16_t
parse_leaf_record_count(const std::vector<char>& page) {
    leaf_page_reader reader;
    if (!reader.parse(page.data(), static_cast<uint32_t>(page.size())))
        return 0;
    return reader.record_count();
}


static std::string
key32(int n) {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%032d", n);
    return std::string(buf, 32);
}

static value_ref
make_vr(uint64_t lba, uint16_t off = 0, uint32_t len = 8) {
    return value_ref{paddr{0, lba}, off, len, 0};
}

static paddr
make_paddr(uint64_t lba) {
    return paddr{0, lba};
}

constexpr uint32_t PAGE_SIZE = 4096;
constexpr uint32_t LBA_SIZE  = 4096;

static core::tree_geometry test_geom{
    .lba_size             = LBA_SIZE,
    .tree_page_size       = PAGE_SIZE,
    .shadow_slots_per_range = 4,
};

// Build a leaf page with the given records.
struct test_record {
    std::string key;
    uint64_t    data_ver;
    bool        is_tombstone;
    value_ref   vr;
};

static std::vector<char>
build_leaf_page(std::span<const test_record> records, uint32_t page_size = PAGE_SIZE) {
    std::vector<char> page(page_size);
    leaf_page_builder b;
    b.init(page.data(), page_size);
    for (auto& r : records) {
        if (r.is_tombstone)
            b.add_tombstone(r.key, r.data_ver);
        else
            b.add_value(r.key, r.data_ver, r.vr);
    }
    b.finalize();
    return page;
}

// Build an internal page with a single rightmost child.
static std::vector<char>
build_internal_page_single_child(paddr child_range_base) {
    std::vector<char> page(PAGE_SIZE);
    internal_page_builder b;
    b.init(page.data(), PAGE_SIZE);
    b.set_rightmost_child(child_range_base);
    b.finalize();
    return page;
}

// Build an internal page with one separator + two children.
static std::vector<char>
build_internal_page_two_children(std::string_view sep_key,
                                  paddr left_child,
                                  paddr right_child) {
    std::vector<char> page(PAGE_SIZE);
    internal_page_builder b;
    b.init(page.data(), PAGE_SIZE);
    b.add_child(sep_key, left_child);
    b.set_rightmost_child(right_child);
    b.finalize();
    return page;
}

// Place an arbitrary page into a cache under its slot paddr's
// frame_id. Returns the page_frame for the caller to free.
static memory::page_frame*
put_page_in_cache(core::clock_cache& cache,
                  paddr slot,
                  const std::vector<char>& page)
{
    auto* pf = new memory::page_frame{
        .id        = make_tree_frame_id(slot, 1),
        .st        = memory::frame_state::clean_readonly,
        .buf       = new char[PAGE_SIZE],
        .byte_len  = PAGE_SIZE,
        .pin_count = 0,
        .crc_valid = true,
    };
    std::memcpy(pf->buf, page.data(), PAGE_SIZE);
    cache.put(pf);
    return pf;
}

// Free a page_frame produced by put_page_in_cache.
static void
free_cached_frame(memory::page_frame* pf) {
    delete[] pf->buf;
    delete pf;
}

// ────────────────────────────────────────────────────────────────────
// test 1: merge — old 3 records + 2 new winners, one overlap
// ────────────────────────────────────────────────────────────────────

static void
test_merge_basic() {
    // Old leaf: keys 10, 30, 50 (all values)
    test_record old_recs[] = {
        { key32(10), 100, false, make_vr(10) },
        { key32(30), 200, false, make_vr(30) },
        { key32(50), 300, false, make_vr(50) },
    };
    auto old_page = build_leaf_page(old_recs);

    // New winners: key 20 (insert), key 30 (overwrite)
    // Keys must outlive the flush_key_group (string_view).
    auto k20 = key32(20);
    auto k30 = key32(30);
    flush_key_group new_keys[] = {
        { k20, 400, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(20), {} }, 0 },
        { k30, 500, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(31), {} }, 0 },
    };

    flush_leaf_candidate out{
        .leaf_range_base = make_paddr(1000),
        .old_slot_paddr  = make_paddr(1000),
        .st              = flush_stage_status::ok,
    };

    auto st = merge_and_build_leaf(
        old_page.data(), PAGE_SIZE,
        std::span<const flush_key_group>(new_keys, 2),
        0,  // recovery_safe_lsn = 0, no tombstone gc
        out);

    CHECK(st == flush_stage_status::ok);
    CHECK(out.record_count == 4);  // 10, 20, 30(new), 50

    // Old key 30's value_ref should be retired.
    CHECK(out.retired_old_values.size() == 1);
    CHECK(out.retired_old_values[0].vr.base.lba == 30);
    CHECK(out.retired_old_values[0].data_ver == 200);

    // Verify the candidate page is valid.
    leaf_page_reader r;
    CHECK(r.parse(out.candidate_page.data(), PAGE_SIZE));
    CHECK(r.record_count() == 4);

    // Check key order.
    CHECK(r.get(0).key == key32(10));
    CHECK(r.get(1).key == key32(20));
    CHECK(r.get(2).key == key32(30));
    CHECK(r.get(2).data_ver == 500);  // new winner
    CHECK(r.get(3).key == key32(50));

    std::printf("  merge basic (3 old + 2 new, 1 overlap): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// test 2: tombstone compact — new tombstone with data_ver <= recovery_safe_lsn
// ────────────────────────────────────────────────────────────────────

static void
test_tombstone_compact_new() {
    test_record old_recs[] = {
        { key32(10), 100, false, make_vr(10) },
    };
    auto old_page = build_leaf_page(old_recs);

    // New winner: tombstone for key 10 at data_ver=100
    auto k10 = key32(10);
    flush_key_group new_keys[] = {
        { k10, 100, core::memtable_entry::kind::tombstone,
          core::value_handle{}, 0 },
    };

    flush_leaf_candidate out{
        .leaf_range_base = make_paddr(1000),
        .old_slot_paddr  = make_paddr(1000),
        .st              = flush_stage_status::ok,
    };

    // recovery_safe_lsn = 100 → tombstone at data_ver=100 should be compacted
    auto st = merge_and_build_leaf(
        old_page.data(), PAGE_SIZE,
        std::span<const flush_key_group>(new_keys, 1),
        100,
        out);

    CHECK(st == flush_stage_status::ok);
    CHECK(out.record_count == 0);  // both old value and new tombstone gone
    CHECK(out.retired_old_values.size() == 1);  // old value retired

    std::printf("  tombstone compact (new tombstone, data_ver <= recovery_safe_lsn): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// test 3: tombstone compact — old tombstone in tree
// ────────────────────────────────────────────────────────────────────

static void
test_tombstone_compact_old() {
    // Old leaf: key 10 is a tombstone at data_ver=50
    test_record old_recs[] = {
        { key32(10), 50, true, {} },
        { key32(20), 100, false, make_vr(20) },
    };
    auto old_page = build_leaf_page(old_recs);

    // No new winners for this leaf.
    // But the leaf is rewritten because another key on this page changed.
    // During rewrite, old tombstone should be compacted.
    auto k20 = key32(20);
    flush_key_group new_keys[] = {
        { k20, 200, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(21), {} }, 0 },
    };

    flush_leaf_candidate out{
        .leaf_range_base = make_paddr(1000),
        .old_slot_paddr  = make_paddr(1000),
        .st              = flush_stage_status::ok,
    };

    // recovery_safe_lsn = 100 → old tombstone at data_ver=50 should be compacted
    auto st = merge_and_build_leaf(
        old_page.data(), PAGE_SIZE,
        std::span<const flush_key_group>(new_keys, 1),
        100,
        out);

    CHECK(st == flush_stage_status::ok);
    CHECK(out.record_count == 1);  // only key 20 remains

    leaf_page_reader r;
    CHECK(r.parse(out.candidate_page.data(), PAGE_SIZE));
    CHECK(r.get(0).key == key32(20));
    CHECK(r.get(0).data_ver == 200);

    // Old key 20 at data_ver=100 → retired (superseded by new winner)
    CHECK(out.retired_old_values.size() == 1);
    CHECK(out.retired_old_values[0].data_ver == 100);

    std::printf("  tombstone compact (old tombstone in tree, data_ver <= recovery_safe_lsn): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// test 4: tombstone NOT compacted when data_ver > recovery_safe_lsn
// ────────────────────────────────────────────────────────────────────

static void
test_tombstone_preserved() {
    test_record old_recs[] = {
        { key32(10), 150, true, {} },
    };
    auto old_page = build_leaf_page(old_recs);

    flush_leaf_candidate out{
        .leaf_range_base = make_paddr(1000),
        .old_slot_paddr  = make_paddr(1000),
        .st              = flush_stage_status::ok,
    };

    // recovery_safe_lsn = 100 < 150 → tombstone preserved
    auto st = merge_and_build_leaf(
        old_page.data(), PAGE_SIZE,
        std::span<const flush_key_group>{},
        100,
        out);

    CHECK(st == flush_stage_status::ok);
    CHECK(out.record_count == 1);

    leaf_page_reader r;
    CHECK(r.parse(out.candidate_page.data(), PAGE_SIZE));
    CHECK(r.get(0).kind == record_kind::tombstone);
    CHECK(r.get(0).data_ver == 150);

    std::printf("  tombstone preserved (data_ver > recovery_safe_lsn): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// test 5: process_candidate_groups — temp buffer path (no cache)
// ────────────────────────────────────────────────────────────────────

static void
test_process_two_pass() {
    // Build an old leaf page.
    test_record old_recs[] = {
        { key32(10), 100, false, make_vr(10) },
        { key32(20), 200, false, make_vr(20) },
    };
    auto old_page = build_leaf_page(old_recs);

    // Set up manifest with one leaf.
    auto manifest = std::make_shared<core::tree_manifest>();
    manifest->geom = &test_geom;
    manifest->root_slot = make_paddr(1000);
    manifest->slot_map[make_paddr(1000)] = 0;

    // Set up leaf groups.
    auto k15 = key32(15);
    flush_key_group keys[] = {
        { k15, 300, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(15), {} }, 0 },
    };
    flush_leaf_group groups[] = {
        { make_paddr(1000), make_paddr(1000), 0u,
          std::span<const flush_key_group>(keys, 1) },
    };

    // Create state.
    auto state = make_candidate_build_state(
        std::span<const flush_leaf_group>(groups, 1),
        manifest.get(), 0, PAGE_SIZE, 1,
        flush_round_id{1}, 0);

    // Create a minimal clock_cache (capacity 4).
    core::clock_cache cache(4);

    // Round 1: pass 1 finds nothing (no cache, no bufs).
    // pass 2 should allocate buffer and return need_read.
    auto d1 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_need_read>(d1));
    auto& nr = std::get<candidate_need_read>(d1);
    CHECK(nr.read_descs.size() == 1);
    CHECK(nr.read_descs[0].lba == 1000);

    // Simulate NVMe read: copy old_page into the buffer.
    std::memcpy(nr.read_descs[0].buf, old_page.data(), PAGE_SIZE);

    // Round 2: pass 1 should process from page_bufs.
    auto d2 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_done>(d2));
    CHECK(state.all_done);
    CHECK(state.result.changed_nodes.size() == 1);
    {
        auto it = state.result.changed_nodes.find(make_paddr(1000));
        CHECK(it != state.result.changed_nodes.end());
        CHECK(it->second.level == 0);
        CHECK(!it->second.needs_new_range);
        CHECK(parse_leaf_record_count(it->second.page_content) == 3);  // 10, 15, 20
    }

    std::printf("  process_candidate_groups (two-pass, temp buffer): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// test 6: process_candidate_groups — cache hit path
// ────────────────────────────────────────────────────────────────────

static void
test_process_cache_hit() {
    test_record old_recs[] = {
        { key32(10), 100, false, make_vr(10) },
    };
    auto old_page = build_leaf_page(old_recs);

    auto manifest = std::make_shared<core::tree_manifest>();
    manifest->geom = &test_geom;
    manifest->root_slot = make_paddr(2000);
    manifest->slot_map[make_paddr(2000)] = 0;

    auto k20 = key32(20);
    flush_key_group keys[] = {
        { k20, 200, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(20), {} }, 0 },
    };
    flush_leaf_group groups[] = {
        { make_paddr(2000), make_paddr(2000), 0u,
          std::span<const flush_key_group>(keys, 1) },
    };

    auto state = make_candidate_build_state(
        std::span<const flush_leaf_group>(groups, 1),
        manifest.get(), 0, PAGE_SIZE, 1,
        flush_round_id{1}, 0);

    // Set up cache with the page pre-loaded.
    core::clock_cache cache(4);
    auto* pf = new memory::page_frame{
        .id       = make_tree_frame_id(make_paddr(2000), 1),
        .st       = memory::frame_state::clean_readonly,
        .buf      = new char[PAGE_SIZE],
        .byte_len = PAGE_SIZE,
        .pin_count = 0,
        .crc_valid = true,
    };
    std::memcpy(pf->buf, old_page.data(), PAGE_SIZE);
    cache.put(pf);  // page is now in cache

    // Round 1: pass 1 should hit cache and complete immediately.
    auto d = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_done>(d));
    CHECK(state.all_done);
    CHECK(state.result.changed_nodes.size() == 1);
    {
        auto it = state.result.changed_nodes.find(make_paddr(2000));
        CHECK(it != state.result.changed_nodes.end());
        CHECK(parse_leaf_record_count(it->second.page_content) == 2);  // 10, 20
    }

    // Cleanup.
    delete[] pf->buf;
    delete pf;

    std::printf("  process_candidate_groups (cache hit, one round): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// test 7: bounded reads — max_reads_per_round = 2, 5 groups
// ────────────────────────────────────────────────────────────────────

static void
test_bounded_reads() {
    // Build 5 identical old leaf pages.
    test_record old_recs[] = {
        { key32(10), 100, false, make_vr(10) },
    };
    auto old_page = build_leaf_page(old_recs);

    auto manifest = std::make_shared<core::tree_manifest>();
    manifest->geom = &test_geom;
    for (int i = 0; i < 5; ++i) {
        auto base = make_paddr(3000 + i * 100);
        manifest->slot_map[base] = 0;
    }
    manifest->root_slot = make_paddr(3000);

    // 5 leaf groups, each with one new key.
    // Keys must outlive the flush_key_group spans.
    std::vector<std::string> key_storage;
    for (int i = 0; i < 5; ++i)
        key_storage.push_back(key32(i * 10 + 5));

    std::vector<flush_key_group> all_keys;
    std::vector<flush_leaf_group> groups;
    for (int i = 0; i < 5; ++i) {
        all_keys.push_back({
            key_storage[i], 200u + static_cast<uint64_t>(i),
            core::memtable_entry::kind::value,
            core::value_handle{ make_vr(100 + i), {} }, 0,
        });
    }
    for (int i = 0; i < 5; ++i) {
        auto base = make_paddr(3000 + i * 100);
        groups.push_back({
            base, base, static_cast<uint32_t>(i),
            std::span<const flush_key_group>(&all_keys[i], 1),
        });
    }

    auto state = make_candidate_build_state(
        std::span<const flush_leaf_group>(groups),
        manifest.get(), 0, PAGE_SIZE, 1,
        flush_round_id{1}, 0,
        2);  // max_reads_per_round = 2

    core::clock_cache cache(4);

    // Round 1: should collect 2 reads (not 5).
    auto d1 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_need_read>(d1));
    CHECK(std::get<candidate_need_read>(d1).read_descs.size() == 2);

    // Simulate reads.
    for (auto& rd : std::get<candidate_need_read>(d1).read_descs)
        std::memcpy(rd.buf, old_page.data(), PAGE_SIZE);

    // Round 2: process 2, collect 2 more.
    auto d2 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_need_read>(d2));
    auto& nr2 = std::get<candidate_need_read>(d2);
    CHECK(nr2.read_descs.size() == 2);

    for (auto& rd : nr2.read_descs)
        std::memcpy(rd.buf, old_page.data(), PAGE_SIZE);

    // Round 3: process 2, collect 1 more.
    auto d3 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_need_read>(d3));
    CHECK(std::get<candidate_need_read>(d3).read_descs.size() == 1);

    for (auto& rd : std::get<candidate_need_read>(d3).read_descs)
        std::memcpy(rd.buf, old_page.data(), PAGE_SIZE);

    // Round 4: process last 1, done.
    auto d4 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_done>(d4));
    CHECK(state.all_done);
    CHECK(state.result.changed_nodes.size() == 5);

    std::printf("  bounded reads (max=2, 5 groups → 4 rounds): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// test 8: flush_read_budget
// ────────────────────────────────────────────────────────────────────

static void
test_flush_read_budget() {
    // idle → full speed
    CHECK(flush_read_budget(0, 256) == 256);

    // at threshold → minimum 1
    CHECK(flush_read_budget(32, 256) == 1);

    // above threshold → minimum 1
    CHECK(flush_read_budget(100, 256) == 1);

    // half load → ~half speed
    auto half = flush_read_budget(16, 256);
    CHECK(half == 128);

    // never returns 0
    for (uint32_t p = 0; p <= 100; ++p) {
        auto b = flush_read_budget(p, 256);
        CHECK(b >= 1);
        CHECK(b <= 256);
    }

    std::printf("  flush_read_budget (idle/half/full/never-zero): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// test 9: empty groups → immediate done
// ────────────────────────────────────────────────────────────────────

static void
test_empty_groups() {
    auto manifest = std::make_shared<core::tree_manifest>();
    manifest->geom = &test_geom;

    auto state = make_candidate_build_state(
        std::span<const flush_leaf_group>{},
        manifest.get(), 0, PAGE_SIZE, 1,
        flush_round_id{1}, 0);

    CHECK(state.all_done);  // set by make_candidate_build_state

    core::clock_cache cache(4);
    auto d = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_done>(d));

    std::printf("  empty groups → immediate done: OK\n");
}

// ════════════════════════════════════════════════════════════════════
// Cascade tests (step 026A)
// ════════════════════════════════════════════════════════════════════

// ────────────────────────────────────────────────────────────────────
// cascade 1: basic cache hit — 2-level tree, leaf exhausted, root in
// cache, cascade completes in one round. Verifies:
//   - leaf added with needs_new_range=true, level=0
//   - root added with needs_new_range=false, level=1 (parent)
// ────────────────────────────────────────────────────────────────────

static void
test_cascade_basic_cache_hit() {
    // Tree layout: root (slot 0, not exhausted) → leaf (slot 3, exhausted)
    auto leaf_range_base = make_paddr(1000);
    auto leaf_slot       = make_paddr(1003);
    auto root_range_base = make_paddr(2000);
    auto root_slot       = make_paddr(2000);

    auto manifest = std::make_shared<core::tree_manifest>();
    manifest->geom            = &test_geom;
    manifest->root_slot       = root_slot;
    manifest->root_range_base = root_range_base;
    manifest->slot_map[root_range_base] = 0;  // not exhausted
    manifest->slot_map[leaf_range_base] = 3;  // exhausted
    // reverse topology: root is sole internal, leaf 0 → root (idx 0)
    manifest->reverse_topology.internal_nodes.push_back(
        {root_range_base, core::kInvalidInternalIdx});
    manifest->reverse_topology.leaf_parent_idx.push_back(0);

    test_record old_recs[] = {
        { key32(10), 100, false, make_vr(10) },
    };
    auto leaf_page = build_leaf_page(old_recs);
    auto root_page = build_internal_page_single_child(leaf_range_base);

    core::clock_cache cache(8);
    auto* pf_leaf = put_page_in_cache(cache, leaf_slot, leaf_page);
    auto* pf_root = put_page_in_cache(cache, root_slot, root_page);

    auto k20 = key32(20);
    flush_key_group keys[] = {
        { k20, 200, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(20), {} }, 0 },
    };
    flush_leaf_group groups[] = {
        { leaf_range_base, leaf_slot, 0u,
          std::span<const flush_key_group>(keys, 1) },
    };

    auto state = make_candidate_build_state(
        std::span<const flush_leaf_group>(groups, 1),
        manifest.get(), 0, PAGE_SIZE, 1,
        flush_round_id{1}, 0);

    auto d = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_done>(d));
    CHECK(state.all_done);
    CHECK(state.result.changed_nodes.size() == 2);
    {
        auto it = state.result.changed_nodes.find(leaf_range_base);
        CHECK(it != state.result.changed_nodes.end());
        CHECK(it->second.level == 0);
        CHECK(it->second.needs_new_range);
    }
    {
        auto it = state.result.changed_nodes.find(root_range_base);
        CHECK(it != state.result.changed_nodes.end());
        CHECK(it->second.level == 1);
        CHECK(!it->second.needs_new_range);
        CHECK(it->second.page_content.size() == PAGE_SIZE);
    }

    free_cached_frame(pf_leaf);
    free_cached_frame(pf_root);
    std::printf("  cascade basic (cache hit, one round): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// cascade 2: async root read — 2-level tree, leaf in cache, root not
// in cache. Expected 2 rounds:
//   Round 1: merge leaf + trace finds root missing → candidate_need_read
//   Round 2: trace completes after root buffer filled → done
// ────────────────────────────────────────────────────────────────────

static void
test_cascade_async_root_read() {
    auto leaf_range_base = make_paddr(1000);
    auto leaf_slot       = make_paddr(1003);
    auto root_range_base = make_paddr(2000);
    auto root_slot       = make_paddr(2000);

    auto manifest = std::make_shared<core::tree_manifest>();
    manifest->geom            = &test_geom;
    manifest->root_slot       = root_slot;
    manifest->root_range_base = root_range_base;
    manifest->slot_map[root_range_base] = 0;
    manifest->slot_map[leaf_range_base] = 3;  // exhausted
    manifest->reverse_topology.internal_nodes.push_back(
        {root_range_base, core::kInvalidInternalIdx});
    manifest->reverse_topology.leaf_parent_idx.push_back(0);

    test_record old_recs[] = { { key32(10), 100, false, make_vr(10) } };
    auto leaf_page = build_leaf_page(old_recs);
    auto root_page = build_internal_page_single_child(leaf_range_base);

    core::clock_cache cache(8);
    auto* pf_leaf = put_page_in_cache(cache, leaf_slot, leaf_page);
    // root is NOT in cache.

    auto k20 = key32(20);
    flush_key_group keys[] = {
        { k20, 200, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(20), {} }, 0 },
    };
    flush_leaf_group groups[] = {
        { leaf_range_base, leaf_slot, 0u,
          std::span<const flush_key_group>(keys, 1) },
    };

    auto state = make_candidate_build_state(
        std::span<const flush_leaf_group>(groups, 1),
        manifest.get(), 0, PAGE_SIZE, 1,
        flush_round_id{1}, 0);

    // Round 1: leaf merged (cache hit), trace finds root missing.
    auto d1 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_need_read>(d1));
    auto& nr1 = std::get<candidate_need_read>(d1);
    CHECK(nr1.read_descs.size() == 1);
    CHECK(nr1.read_descs[0].lba == root_slot.lba);
    // Leaf already in changed_nodes after pass 2.
    CHECK(state.result.changed_nodes.count(leaf_range_base) == 1);
    CHECK(state.result.changed_nodes.count(root_range_base) == 0);

    // Simulate NVMe read completion: fill the root buffer.
    std::memcpy(nr1.read_descs[0].buf, root_page.data(), PAGE_SIZE);

    // Round 2: trace re-runs, root now available via internal_page_bufs.
    auto d2 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_done>(d2));
    CHECK(state.all_done);
    CHECK(state.result.changed_nodes.size() == 2);
    CHECK(state.result.changed_nodes.count(root_range_base) == 1);

    free_cached_frame(pf_leaf);
    std::printf("  cascade async (root cache miss, 2 rounds): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// cascade 3: multi-level — 3-level tree. leaf (slot 3) exhausted,
// middle (slot 3) exhausted, root (slot 0) not exhausted. Expected:
// changed_nodes has leaf + middle (needs_new_range=true) + root
// (needs_new_range=false), levels 0/1/2.
// ────────────────────────────────────────────────────────────────────

static void
test_cascade_multi_level() {
    auto leaf_range_base = make_paddr(1000);
    auto leaf_slot       = make_paddr(1003);
    auto mid_range_base  = make_paddr(2000);
    auto mid_slot        = make_paddr(2003);
    auto root_range_base = make_paddr(3000);
    auto root_slot       = make_paddr(3000);

    auto manifest = std::make_shared<core::tree_manifest>();
    manifest->geom            = &test_geom;
    manifest->root_slot       = root_slot;
    manifest->root_range_base = root_range_base;
    manifest->slot_map[root_range_base] = 0;  // not exhausted
    manifest->slot_map[mid_range_base]  = 3;  // exhausted
    manifest->slot_map[leaf_range_base] = 3;  // exhausted
    // reverse topology: idx 0 = root, idx 1 = mid (parent=root).
    manifest->reverse_topology.internal_nodes.push_back(
        {root_range_base, core::kInvalidInternalIdx});
    manifest->reverse_topology.internal_nodes.push_back(
        {mid_range_base, 0});
    manifest->reverse_topology.leaf_parent_idx.push_back(1);  // leaf → mid

    test_record old_recs[] = { { key32(10), 100, false, make_vr(10) } };
    auto leaf_page = build_leaf_page(old_recs);
    auto mid_page  = build_internal_page_single_child(leaf_range_base);
    auto root_page = build_internal_page_single_child(mid_range_base);

    core::clock_cache cache(8);
    auto* pf_leaf = put_page_in_cache(cache, leaf_slot, leaf_page);
    auto* pf_mid  = put_page_in_cache(cache, mid_slot, mid_page);
    auto* pf_root = put_page_in_cache(cache, root_slot, root_page);

    auto k20 = key32(20);
    flush_key_group keys[] = {
        { k20, 200, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(20), {} }, 0 },
    };
    flush_leaf_group groups[] = {
        { leaf_range_base, leaf_slot, 0u,
          std::span<const flush_key_group>(keys, 1) },
    };

    auto state = make_candidate_build_state(
        std::span<const flush_leaf_group>(groups, 1),
        manifest.get(), 0, PAGE_SIZE, 1,
        flush_round_id{1}, 0);

    auto d = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_done>(d));
    CHECK(state.all_done);
    CHECK(state.result.changed_nodes.size() == 3);
    {
        auto it = state.result.changed_nodes.find(leaf_range_base);
        CHECK(it != state.result.changed_nodes.end());
        CHECK(it->second.level == 0);
        CHECK(it->second.needs_new_range);
    }
    {
        auto it = state.result.changed_nodes.find(mid_range_base);
        CHECK(it != state.result.changed_nodes.end());
        CHECK(it->second.level == 1);
        CHECK(it->second.needs_new_range);
    }
    {
        auto it = state.result.changed_nodes.find(root_range_base);
        CHECK(it != state.result.changed_nodes.end());
        CHECK(it->second.level == 2);
        CHECK(!it->second.needs_new_range);
    }

    free_cached_frame(pf_leaf);
    free_cached_frame(pf_mid);
    free_cached_frame(pf_root);
    std::printf("  cascade multi-level (leaf + mid exhausted → root stops): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// cascade 4: mid-path miss — 3-level tree with middle level NOT in
// cache. Round 1 traces: reads root (cache hit), descends to middle
// (miss) → cascade_waiting. Round 2 fills middle, trace completes.
// ────────────────────────────────────────────────────────────────────

static void
test_cascade_mid_path_miss() {
    auto leaf_range_base = make_paddr(1000);
    auto leaf_slot       = make_paddr(1003);
    auto mid_range_base  = make_paddr(2000);
    auto mid_slot        = make_paddr(2000);   // slot 0, not exhausted
    auto root_range_base = make_paddr(3000);
    auto root_slot       = make_paddr(3000);

    auto manifest = std::make_shared<core::tree_manifest>();
    manifest->geom            = &test_geom;
    manifest->root_slot       = root_slot;
    manifest->root_range_base = root_range_base;
    manifest->slot_map[root_range_base] = 0;
    manifest->slot_map[mid_range_base]  = 0;  // not exhausted
    manifest->slot_map[leaf_range_base] = 3;  // exhausted
    // reverse topology: idx 0 = root, idx 1 = mid.
    manifest->reverse_topology.internal_nodes.push_back(
        {root_range_base, core::kInvalidInternalIdx});
    manifest->reverse_topology.internal_nodes.push_back(
        {mid_range_base, 0});
    manifest->reverse_topology.leaf_parent_idx.push_back(1);

    test_record old_recs[] = { { key32(10), 100, false, make_vr(10) } };
    auto leaf_page = build_leaf_page(old_recs);
    auto mid_page  = build_internal_page_single_child(leaf_range_base);
    auto root_page = build_internal_page_single_child(mid_range_base);

    core::clock_cache cache(8);
    auto* pf_leaf = put_page_in_cache(cache, leaf_slot, leaf_page);
    auto* pf_root = put_page_in_cache(cache, root_slot, root_page);
    // mid is NOT in cache.

    auto k20 = key32(20);
    flush_key_group keys[] = {
        { k20, 200, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(20), {} }, 0 },
    };
    flush_leaf_group groups[] = {
        { leaf_range_base, leaf_slot, 0u,
          std::span<const flush_key_group>(keys, 1) },
    };

    auto state = make_candidate_build_state(
        std::span<const flush_leaf_group>(groups, 1),
        manifest.get(), 0, PAGE_SIZE, 1,
        flush_round_id{1}, 0);

    // Round 1: trace descends from root, misses at mid_slot.
    auto d1 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_need_read>(d1));
    auto& nr1 = std::get<candidate_need_read>(d1);
    CHECK(nr1.read_descs.size() == 1);
    CHECK(nr1.read_descs[0].lba == mid_slot.lba);

    std::memcpy(nr1.read_descs[0].buf, mid_page.data(), PAGE_SIZE);

    // Round 2: trace re-runs, mid now available via internal_page_bufs.
    auto d2 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_done>(d2));
    CHECK(state.all_done);
    CHECK(state.result.changed_nodes.size() == 2);
    // leaf (level 0, needs_new_range) + mid (level 1, not exhausted → cascade
    // stops at mid, root NOT added).
    CHECK(state.result.changed_nodes.count(leaf_range_base) == 1);
    CHECK(state.result.changed_nodes.count(mid_range_base) == 1);
    CHECK(state.result.changed_nodes.count(root_range_base) == 0);
    {
        auto& mid = state.result.changed_nodes.at(mid_range_base);
        CHECK(mid.level == 1);
        CHECK(!mid.needs_new_range);
    }

    free_cached_frame(pf_leaf);
    free_cached_frame(pf_root);
    std::printf("  cascade mid-path miss (mid read then done): OK\n");
}

// ────────────────────────────────────────────────────────────────────
// cascade 5: shared parent — 2 leaves under same root, both exhausted.
// Root not in cache. Both cascades wait on same root_slot → ONE read.
// Parent added only once to changed_nodes.
// ────────────────────────────────────────────────────────────────────

static void
test_cascade_shared_parent() {
    auto leaf_a_range = make_paddr(1000);
    auto leaf_a_slot  = make_paddr(1003);
    auto leaf_b_range = make_paddr(2000);
    auto leaf_b_slot  = make_paddr(2003);
    auto root_range   = make_paddr(3000);
    auto root_slot    = make_paddr(3000);

    auto manifest = std::make_shared<core::tree_manifest>();
    manifest->geom            = &test_geom;
    manifest->root_slot       = root_slot;
    manifest->root_range_base = root_range;
    manifest->slot_map[root_range]   = 0;
    manifest->slot_map[leaf_a_range] = 3;  // exhausted
    manifest->slot_map[leaf_b_range] = 3;  // exhausted
    // reverse topology: root is idx 0, both leaves parent it.
    manifest->reverse_topology.internal_nodes.push_back(
        {root_range, core::kInvalidInternalIdx});
    manifest->reverse_topology.leaf_parent_idx.push_back(0);  // leaf A
    manifest->reverse_topology.leaf_parent_idx.push_back(0);  // leaf B

    test_record a_recs[] = { { key32(1), 100, false, make_vr(1) } };
    test_record b_recs[] = { { key32(50), 100, false, make_vr(50) } };
    auto leaf_a_page = build_leaf_page(a_recs);
    auto leaf_b_page = build_leaf_page(b_recs);
    // Root: separator "20"; keys < "20" → leaf A, keys ≥ "20" → leaf B.
    auto sep = key32(20);
    auto root_page = build_internal_page_two_children(
        sep, leaf_a_range, leaf_b_range);

    core::clock_cache cache(16);
    auto* pf_a = put_page_in_cache(cache, leaf_a_slot, leaf_a_page);
    auto* pf_b = put_page_in_cache(cache, leaf_b_slot, leaf_b_page);
    // root NOT in cache.

    auto ka = key32(5);
    auto kb = key32(60);
    flush_key_group keys_a[] = {
        { ka, 200, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(5), {} }, 0 },
    };
    flush_key_group keys_b[] = {
        { kb, 200, core::memtable_entry::kind::value,
          core::value_handle{ make_vr(60), {} }, 0 },
    };
    flush_leaf_group groups[] = {
        { leaf_a_range, leaf_a_slot, 0u,
          std::span<const flush_key_group>(keys_a, 1) },
        { leaf_b_range, leaf_b_slot, 1u,
          std::span<const flush_key_group>(keys_b, 1) },
    };

    auto state = make_candidate_build_state(
        std::span<const flush_leaf_group>(groups, 2),
        manifest.get(), 0, PAGE_SIZE, 1,
        flush_round_id{1}, 0);

    // Round 1: both leaves merged, both cascade-trace → both wait on root_slot.
    // Only ONE read should be issued (coalesced).
    auto d1 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_need_read>(d1));
    auto& nr1 = std::get<candidate_need_read>(d1);
    CHECK(nr1.read_descs.size() == 1);
    CHECK(nr1.read_descs[0].lba == root_slot.lba);
    CHECK(state.cascade_waiting.size() == 1);
    CHECK(state.cascade_waiting.at(root_slot).size() == 2);

    std::memcpy(nr1.read_descs[0].buf, root_page.data(), PAGE_SIZE);

    // Round 2: both leaves cascade to root, root added only once.
    auto d2 = process_candidate_groups(state, nullptr, &cache);
    CHECK(std::holds_alternative<candidate_done>(d2));
    CHECK(state.all_done);
    CHECK(state.result.changed_nodes.size() == 3);
    CHECK(state.result.changed_nodes.count(leaf_a_range) == 1);
    CHECK(state.result.changed_nodes.count(leaf_b_range) == 1);
    CHECK(state.result.changed_nodes.count(root_range) == 1);
    {
        auto& root = state.result.changed_nodes.at(root_range);
        CHECK(root.level == 1);
        CHECK(!root.needs_new_range);
    }

    free_cached_frame(pf_a);
    free_cached_frame(pf_b);
    std::printf("  cascade shared parent (2 leaves, 1 root read): OK\n");
}

}  // anonymous namespace

int main() {
    std::printf("inconel candidate build (Phase 6 / step 026) test:\n");

    test_merge_basic();
    test_tombstone_compact_new();
    test_tombstone_compact_old();
    test_tombstone_preserved();
    test_process_two_pass();
    test_process_cache_hit();
    test_bounded_reads();
    test_flush_read_budget();
    test_empty_groups();

    test_cascade_basic_cache_hit();
    test_cascade_async_root_read();
    test_cascade_multi_level();
    test_cascade_mid_path_miss();
    test_cascade_shared_parent();

    std::printf("all passed\n");
    return 0;
}
