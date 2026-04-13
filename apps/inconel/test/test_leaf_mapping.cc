//
// Phase 5 (step 025) leaf mapping algorithm tests.
//
// Tests the three free functions that Phase 5 introduces:
//
//   1. leaf_order_index::find_leaf_for_key()  — binary search
//   2. keys_to_leaf_groups()                  — sorted merge mapping
//   3. merge_lookup_leaf_groups()             — fan-in dedupe
//
// All tests are pure algorithm — no PUMP runtime, no scheduler, no NVMe.
// Panic tests use fork() because panic_inconsistency calls abort().
//

#include "apps/inconel/test/check.hh"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "apps/inconel/core/leaf_order.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/tree_geometry.hh"
#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/tree/flush_types.hh"
#include "apps/inconel/tree/leaf_mapping.hh"

using namespace apps::inconel;

namespace {

// ────────────────────────────────────────────────────────────────────────
// Compile-time invariants
// ────────────────────────────────────────────────────────────────────────

// Phase 5 D1: flush_leaf_group must carry a keys span.
static_assert(std::is_same_v<
    decltype(std::declval<tree::flush_leaf_group>().keys),
    std::span<const tree::flush_key_group>>,
    "Phase 5 D1: flush_leaf_group.keys must be span<const flush_key_group>");

// Phase 5 D17: flush_lookup_req renamed to flush_mapping_req.
template <typename T>
concept has_flush_mapping_req_fields = requires(T& t) {
    t.round_id;
    t.read_domain_index;
    t.base_manifest;
    t.groups;
};
static_assert(has_flush_mapping_req_fields<tree::flush_mapping_req>,
    "Phase 5 D17: flush_mapping_req must exist with expected fields");

// Phase 5 D10: flush_fold_result exists.
template <typename T>
concept has_flush_fold_result_fields = requires(T& t) {
    t.round_id;
    t.st;
    t.partitions;
    t.base_manifest;
};
static_assert(has_flush_fold_result_fields<tree::flush_fold_result>,
    "Phase 5 D10: flush_fold_result must exist with expected fields");

// Phase 5 D11: flush_merge_request exists.
template <typename T>
concept has_flush_merge_request_fields = requires(T& t) {
    t.round_id;
    t.mapping_results;
};
static_assert(has_flush_merge_request_fields<tree::flush_merge_request>,
    "Phase 5 D11: flush_merge_request must exist with expected fields");

// ────────────────────────────────────────────────────────────────────────
// Bootstrap geometry + helpers
// ────────────────────────────────────────────────────────────────────────

constexpr core::tree_geometry kTestGeom{
    .lba_size               = 4096,
    .tree_page_size         = 4096,
    .shadow_slots_per_range = 4,
};

// Build a leaf_order_index with N leaves.
//
// 1 leaf:  [-, -)          covers all keys
// 3 leaf:  [-, "k"), ["k", "r"), ["r", -)
// Range bases: {0, 1000}, {0, 2000}, {0, 3000}, ...
static core::leaf_order_index
make_index_1leaf() {
    core::leaf_order_index idx;
    idx.fence_pool = "";
    idx.spans.push_back({
        .fence_lower_off = 0, .fence_upper_off = 0,
        .fence_lower_len = 0, .fence_upper_len = 0,
        .leaf_range_base = {0, 1000},
    });
    return idx;
}

static core::leaf_order_index
make_index_3leaf() {
    core::leaf_order_index idx;
    idx.fence_pool = "kr";
    // leaf 0: [-, "k")
    idx.spans.push_back({
        .fence_lower_off = 0, .fence_upper_off = 0,
        .fence_lower_len = 0, .fence_upper_len = 1,
        .leaf_range_base = {0, 1000},
    });
    // leaf 1: ["k", "r")
    idx.spans.push_back({
        .fence_lower_off = 0, .fence_upper_off = 1,
        .fence_lower_len = 1, .fence_upper_len = 1,
        .leaf_range_base = {0, 2000},
    });
    // leaf 2: ["r", -)
    idx.spans.push_back({
        .fence_lower_off = 1, .fence_upper_off = 0,
        .fence_lower_len = 1, .fence_upper_len = 0,
        .leaf_range_base = {0, 3000},
    });
    return idx;
}

static core::tree_manifest
make_manifest(const core::tree_geometry* geom, core::leaf_order_index idx) {
    core::tree_manifest m;
    m.root_slot = {0, 500};
    m.geom = geom;
    for (auto& s : idx.spans)
        m.slot_map[s.leaf_range_base] = 0;
    m.leaf_order = std::move(idx);
    return m;
}

static tree::flush_key_group
make_key_group(std::string_view key, uint64_t data_ver) {
    return tree::flush_key_group{
        .key                     = key,
        .winner_data_ver         = data_ver,
        .winner_kind             = core::memtable_entry::kind::value,
        .winner_value            = {},
        .winner_pinned_gen_index = 0,
    };
}

// Run `fn` in a forked child; expect it to abort (panic).
// Returns true if child was killed by SIGABRT.
static bool
expect_abort(void (*fn)()) {
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(0);  // should not reach
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

// ────────────────────────────────────────────────────────────────────────
// Tests: find_leaf_for_key
// ────────────────────────────────────────────────────────────────────────

static void
test_find_empty_index() {
    core::leaf_order_index idx;
    CHECK(idx.find_leaf_for_key("anything") == idx.size());
    printf("  find_leaf_for_key: empty index → size(): OK\n");
}

static void
test_find_single_leaf() {
    auto idx = make_index_1leaf();
    // Single leaf covers everything.
    CHECK(idx.find_leaf_for_key("") == 0);
    CHECK(idx.find_leaf_for_key("a") == 0);
    CHECK(idx.find_leaf_for_key("zzz") == 0);
    printf("  find_leaf_for_key: single leaf: OK\n");
}

static void
test_find_3leaf_boundaries() {
    auto idx = make_index_3leaf();  // [-, k), [k, r), [r, -)

    // Before "k" → leaf 0
    CHECK(idx.find_leaf_for_key("") == 0);
    CHECK(idx.find_leaf_for_key("a") == 0);
    CHECK(idx.find_leaf_for_key("j") == 0);

    // Exactly "k" → leaf 1 (lower inclusive)
    CHECK(idx.find_leaf_for_key("k") == 1);
    CHECK(idx.find_leaf_for_key("m") == 1);
    CHECK(idx.find_leaf_for_key("q") == 1);

    // Exactly "r" → leaf 2
    CHECK(idx.find_leaf_for_key("r") == 2);
    CHECK(idx.find_leaf_for_key("z") == 2);
    CHECK(idx.find_leaf_for_key("zzz") == 2);

    printf("  find_leaf_for_key: 3 leaf boundaries: OK\n");
}

static void
test_find_all() {
    test_find_empty_index();
    test_find_single_leaf();
    test_find_3leaf_boundaries();
}

// ────────────────────────────────────────────────────────────────────────
// Tests: keys_to_leaf_groups
// ────────────────────────────────────────────────────────────────────────

static void
test_mapping_empty_groups() {
    auto manifest = make_manifest(&kTestGeom, make_index_3leaf());
    std::vector<tree::flush_key_group> workset;  // empty

    tree::flush_mapping_req req{
        .round_id          = {1},
        .read_domain_index = 0,
        .base_manifest     = &manifest,
        .groups            = std::span(workset),
    };
    tree::flush_leaf_group_result result;
    tree::keys_to_leaf_groups(req, result);

    CHECK(result.st == tree::flush_stage_status::ok);
    CHECK(result.leaf_groups.empty());
    printf("  keys_to_leaf_groups: empty groups → ok: OK\n");
}

static void
test_mapping_empty_tree() {
    auto manifest = core::tree_manifest::empty(&kTestGeom);
    std::vector<tree::flush_key_group> workset;
    workset.push_back(make_key_group("abc", 1));

    tree::flush_mapping_req req{
        .round_id          = {1},
        .read_domain_index = 0,
        .base_manifest     = &manifest,
        .groups            = std::span(workset),
    };
    tree::flush_leaf_group_result result;
    tree::keys_to_leaf_groups(req, result);

    CHECK(result.st == tree::flush_stage_status::unsupported_shape_change);
    printf("  keys_to_leaf_groups: empty tree → unsupported_shape_change: OK\n");
}

static void
test_mapping_happy_path_3leaf() {
    auto manifest = make_manifest(&kTestGeom, make_index_3leaf());
    // [-, k), [k, r), [r, -)

    std::vector<tree::flush_key_group> workset;
    workset.push_back(make_key_group("aaa", 1));  // leaf 0
    workset.push_back(make_key_group("bbb", 2));  // leaf 0
    workset.push_back(make_key_group("kkk", 3));  // leaf 1
    workset.push_back(make_key_group("mmm", 4));  // leaf 1
    workset.push_back(make_key_group("zzz", 5));  // leaf 2

    tree::flush_mapping_req req{
        .round_id          = {1},
        .read_domain_index = 0,
        .base_manifest     = &manifest,
        .groups            = std::span(workset),
    };
    tree::flush_leaf_group_result result;
    tree::keys_to_leaf_groups(req, result);

    CHECK(result.st == tree::flush_stage_status::ok);
    CHECK(result.leaf_groups.size() == 3);

    // leaf 0: "aaa", "bbb"
    CHECK(result.leaf_groups[0].leaf_range_base.lba == 1000);
    CHECK(result.leaf_groups[0].keys.size() == 2);
    CHECK(result.leaf_groups[0].keys[0].key == "aaa");
    CHECK(result.leaf_groups[0].keys[1].key == "bbb");

    // leaf 1: "kkk", "mmm"
    CHECK(result.leaf_groups[1].leaf_range_base.lba == 2000);
    CHECK(result.leaf_groups[1].keys.size() == 2);
    CHECK(result.leaf_groups[1].keys[0].key == "kkk");
    CHECK(result.leaf_groups[1].keys[1].key == "mmm");

    // leaf 2: "zzz"
    CHECK(result.leaf_groups[2].leaf_range_base.lba == 3000);
    CHECK(result.leaf_groups[2].keys.size() == 1);
    CHECK(result.leaf_groups[2].keys[0].key == "zzz");

    // keys span addresses must point into the workset vector
    CHECK(result.leaf_groups[0].keys.data() == &workset[0]);
    CHECK(result.leaf_groups[1].keys.data() == &workset[2]);
    CHECK(result.leaf_groups[2].keys.data() == &workset[4]);

    printf("  keys_to_leaf_groups: 3-leaf happy path: OK\n");
}

static void
test_mapping_single_leaf_all_keys() {
    auto manifest = make_manifest(&kTestGeom, make_index_1leaf());

    std::vector<tree::flush_key_group> workset;
    workset.push_back(make_key_group("aaa", 1));
    workset.push_back(make_key_group("mmm", 2));
    workset.push_back(make_key_group("zzz", 3));

    tree::flush_mapping_req req{
        .round_id = {1}, .read_domain_index = 0,
        .base_manifest = &manifest, .groups = std::span(workset),
    };
    tree::flush_leaf_group_result result;
    tree::keys_to_leaf_groups(req, result);

    CHECK(result.st == tree::flush_stage_status::ok);
    CHECK(result.leaf_groups.size() == 1);
    CHECK(result.leaf_groups[0].keys.size() == 3);
    CHECK(result.leaf_groups[0].keys.data() == &workset[0]);

    printf("  keys_to_leaf_groups: single leaf, all keys: OK\n");
}

static void
test_mapping_sparse_keys_skip_leaf() {
    auto manifest = make_manifest(&kTestGeom, make_index_3leaf());
    // Only keys in leaf 0 and leaf 2, skip leaf 1.

    std::vector<tree::flush_key_group> workset;
    workset.push_back(make_key_group("aaa", 1));  // leaf 0
    workset.push_back(make_key_group("zzz", 2));  // leaf 2

    tree::flush_mapping_req req{
        .round_id = {1}, .read_domain_index = 0,
        .base_manifest = &manifest, .groups = std::span(workset),
    };
    tree::flush_leaf_group_result result;
    tree::keys_to_leaf_groups(req, result);

    CHECK(result.st == tree::flush_stage_status::ok);
    CHECK(result.leaf_groups.size() == 2);
    CHECK(result.leaf_groups[0].leaf_range_base.lba == 1000);  // leaf 0
    CHECK(result.leaf_groups[0].keys.size() == 1);
    CHECK(result.leaf_groups[1].leaf_range_base.lba == 3000);  // leaf 2
    CHECK(result.leaf_groups[1].keys.size() == 1);

    printf("  keys_to_leaf_groups: sparse keys, skip leaf: OK\n");
}

static void
test_mapping_at_fence_boundary() {
    auto manifest = make_manifest(&kTestGeom, make_index_3leaf());
    // Key exactly at fence "k" → leaf 1 (lower inclusive)
    // Key exactly at fence "r" → leaf 2

    std::vector<tree::flush_key_group> workset;
    workset.push_back(make_key_group("k", 1));   // leaf 1
    workset.push_back(make_key_group("r", 2));   // leaf 2

    tree::flush_mapping_req req{
        .round_id = {1}, .read_domain_index = 0,
        .base_manifest = &manifest, .groups = std::span(workset),
    };
    tree::flush_leaf_group_result result;
    tree::keys_to_leaf_groups(req, result);

    CHECK(result.st == tree::flush_stage_status::ok);
    CHECK(result.leaf_groups.size() == 2);
    CHECK(result.leaf_groups[0].leaf_range_base.lba == 2000);  // "k" → leaf 1
    CHECK(result.leaf_groups[1].leaf_range_base.lba == 3000);  // "r" → leaf 2

    printf("  keys_to_leaf_groups: fence boundary keys: OK\n");
}

// Panic: gap between leaves.
static void
test_mapping_gap_panics_impl() {
    // Construct a broken index with a gap: [-, "k"), ["m", -)
    // Key "l" falls in the gap between "k" and "m".
    core::leaf_order_index idx;
    idx.fence_pool = "km";
    idx.spans.push_back({
        .fence_lower_off = 0, .fence_upper_off = 0,
        .fence_lower_len = 0, .fence_upper_len = 1,  // upper = "k"
        .leaf_range_base = {0, 1000},
    });
    idx.spans.push_back({
        .fence_lower_off = 1, .fence_upper_off = 0,
        .fence_lower_len = 1, .fence_upper_len = 0,  // lower = "m", upper = +inf
        .leaf_range_base = {0, 2000},
    });
    auto manifest = make_manifest(&kTestGeom, std::move(idx));

    std::vector<tree::flush_key_group> workset;
    workset.push_back(make_key_group("aaa", 1));  // leaf 0 — ok
    workset.push_back(make_key_group("l", 2));    // gap — should panic

    tree::flush_mapping_req req{
        .round_id = {1}, .read_domain_index = 0,
        .base_manifest = &manifest, .groups = std::span(workset),
    };
    tree::flush_leaf_group_result result;
    tree::keys_to_leaf_groups(req, result);  // should abort
}

static void
test_mapping_gap_panics() {
    CHECK(expect_abort(test_mapping_gap_panics_impl));
    printf("  keys_to_leaf_groups: gap → panic: OK\n");
}

static void
test_mapping_all() {
    test_mapping_empty_groups();
    test_mapping_empty_tree();
    test_mapping_happy_path_3leaf();
    test_mapping_single_leaf_all_keys();
    test_mapping_sparse_keys_skip_leaf();
    test_mapping_at_fence_boundary();
    test_mapping_gap_panics();
}

// ────────────────────────────────────────────────────────────────────────
// Tests: merge_lookup_leaf_groups
// ────────────────────────────────────────────────────────────────────────

static void
test_merge_single_partition() {
    // One partition, 2 affected leaves.
    auto manifest = make_manifest(&kTestGeom, make_index_3leaf());

    std::vector<tree::flush_key_group> workset;
    workset.push_back(make_key_group("aaa", 1));
    workset.push_back(make_key_group("zzz", 2));

    tree::flush_leaf_group_result r0;
    r0.round_id = {1};
    r0.read_domain_index = 0;
    r0.st = tree::flush_stage_status::ok;
    r0.leaf_groups.push_back({
        .leaf_range_base = {0, 1000},
        .old_slot_paddr  = {0, 1000},
        .keys = std::span(&workset[0], 1),
    });
    r0.leaf_groups.push_back({
        .leaf_range_base = {0, 3000},
        .old_slot_paddr  = {0, 3000},
        .keys = std::span(&workset[1], 1),
    });

    std::vector<tree::flush_leaf_group_result> results;
    results.push_back(std::move(r0));

    std::vector<tree::flush_leaf_group> merged;
    auto st = tree::merge_lookup_leaf_groups(results, merged);

    CHECK(st == tree::flush_stage_status::ok);
    CHECK(merged.size() == 2);
    CHECK(merged[0].leaf_range_base.lba == 1000);
    CHECK(merged[1].leaf_range_base.lba == 3000);

    printf("  merge: single partition: OK\n");
}

static void
test_merge_cross_boundary_dedupe() {
    // Two partitions, both hit leaf 1 (lba=2000).
    // Partition 0: workset[0..4], hits leaf 0 and leaf 1.
    // Partition 1: workset[5..9], hits leaf 1 and leaf 2.
    // After merge, leaf 1 should have a merged contiguous span.

    std::vector<tree::flush_key_group> workset;
    // leaf 0 keys
    workset.push_back(make_key_group("aaa", 1));
    workset.push_back(make_key_group("bbb", 2));
    workset.push_back(make_key_group("ccc", 3));
    // leaf 1 keys from partition 0
    workset.push_back(make_key_group("kkk", 4));
    workset.push_back(make_key_group("lll", 5));
    // leaf 1 keys from partition 1 (contiguous in workset)
    workset.push_back(make_key_group("mmm", 6));
    workset.push_back(make_key_group("nnn", 7));
    // leaf 2 keys
    workset.push_back(make_key_group("rrr", 8));
    workset.push_back(make_key_group("zzz", 9));

    // Partition 0 result (rdi=0)
    tree::flush_leaf_group_result r0;
    r0.round_id = {1};
    r0.read_domain_index = 0;
    r0.st = tree::flush_stage_status::ok;
    r0.leaf_groups.push_back({
        .leaf_range_base = {0, 1000},
        .old_slot_paddr  = {0, 1000},
        .keys = std::span(&workset[0], 3),  // aaa, bbb, ccc
    });
    r0.leaf_groups.push_back({
        .leaf_range_base = {0, 2000},
        .old_slot_paddr  = {0, 2000},
        .keys = std::span(&workset[3], 2),  // kkk, lll
    });

    // Partition 1 result (rdi=1)
    tree::flush_leaf_group_result r1;
    r1.round_id = {1};
    r1.read_domain_index = 1;
    r1.st = tree::flush_stage_status::ok;
    r1.leaf_groups.push_back({
        .leaf_range_base = {0, 2000},
        .old_slot_paddr  = {0, 2000},
        .keys = std::span(&workset[5], 2),  // mmm, nnn
    });
    r1.leaf_groups.push_back({
        .leaf_range_base = {0, 3000},
        .old_slot_paddr  = {0, 3000},
        .keys = std::span(&workset[7], 2),  // rrr, zzz
    });

    std::vector<tree::flush_leaf_group_result> results;
    results.push_back(std::move(r0));
    results.push_back(std::move(r1));

    std::vector<tree::flush_leaf_group> merged;
    auto st = tree::merge_lookup_leaf_groups(results, merged);

    CHECK(st == tree::flush_stage_status::ok);
    CHECK(merged.size() == 3);  // 3 unique leaves

    // leaf 0
    CHECK(merged[0].leaf_range_base.lba == 1000);
    CHECK(merged[0].keys.size() == 3);

    // leaf 1: merged span from two partitions
    CHECK(merged[1].leaf_range_base.lba == 2000);
    CHECK(merged[1].keys.size() == 4);  // kkk, lll, mmm, nnn
    CHECK(merged[1].keys[0].key == "kkk");
    CHECK(merged[1].keys[3].key == "nnn");
    // Verify contiguity: merged span starts at workset[3]
    CHECK(merged[1].keys.data() == &workset[3]);

    // leaf 2
    CHECK(merged[2].leaf_range_base.lba == 3000);
    CHECK(merged[2].keys.size() == 2);

    printf("  merge: cross-boundary dedupe: OK\n");
}

static void
test_merge_error_propagation() {
    // One shard returns unsupported_shape_change.
    tree::flush_leaf_group_result r0;
    r0.round_id = {1};
    r0.read_domain_index = 0;
    r0.st = tree::flush_stage_status::ok;

    tree::flush_leaf_group_result r1;
    r1.round_id = {1};
    r1.read_domain_index = 1;
    r1.st = tree::flush_stage_status::unsupported_shape_change;

    std::vector<tree::flush_leaf_group_result> results;
    results.push_back(std::move(r0));
    results.push_back(std::move(r1));

    std::vector<tree::flush_leaf_group> merged;
    auto st = tree::merge_lookup_leaf_groups(results, merged);

    CHECK(st == tree::flush_stage_status::unsupported_shape_change);

    printf("  merge: error propagation: OK\n");
}

static void
test_merge_no_overlap() {
    // Two partitions, no shared leaf.
    std::vector<tree::flush_key_group> workset;
    workset.push_back(make_key_group("aaa", 1));
    workset.push_back(make_key_group("zzz", 2));

    tree::flush_leaf_group_result r0;
    r0.round_id = {1};
    r0.read_domain_index = 0;
    r0.st = tree::flush_stage_status::ok;
    r0.leaf_groups.push_back({
        .leaf_range_base = {0, 1000},
        .old_slot_paddr  = {0, 1000},
        .keys = std::span(&workset[0], 1),
    });

    tree::flush_leaf_group_result r1;
    r1.round_id = {1};
    r1.read_domain_index = 1;
    r1.st = tree::flush_stage_status::ok;
    r1.leaf_groups.push_back({
        .leaf_range_base = {0, 3000},
        .old_slot_paddr  = {0, 3000},
        .keys = std::span(&workset[1], 1),
    });

    std::vector<tree::flush_leaf_group_result> results;
    results.push_back(std::move(r0));
    results.push_back(std::move(r1));

    std::vector<tree::flush_leaf_group> merged;
    auto st = tree::merge_lookup_leaf_groups(results, merged);

    CHECK(st == tree::flush_stage_status::ok);
    CHECK(merged.size() == 2);
    CHECK(merged[0].leaf_range_base.lba == 1000);
    CHECK(merged[1].leaf_range_base.lba == 3000);

    printf("  merge: no overlap: OK\n");
}

static void
test_merge_empty_results() {
    std::vector<tree::flush_leaf_group_result> results;
    std::vector<tree::flush_leaf_group> merged;
    auto st = tree::merge_lookup_leaf_groups(results, merged);

    CHECK(st == tree::flush_stage_status::ok);
    CHECK(merged.empty());

    printf("  merge: empty results: OK\n");
}

static void
test_merge_all() {
    test_merge_single_partition();
    test_merge_cross_boundary_dedupe();
    test_merge_error_propagation();
    test_merge_no_overlap();
    test_merge_empty_results();
}

}  // namespace

// ────────────────────────────────────────────────────────────────────────

int
main() {
    printf("inconel leaf mapping (Phase 5 / step 025) test:\n");

    test_find_all();
    test_mapping_all();
    test_merge_all();

    printf("all passed\n");
    return 0;
}
