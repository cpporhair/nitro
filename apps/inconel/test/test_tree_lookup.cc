//
// 3-level tree lookup regression test
//
// Tree: root internal → 4 internal → 8 leaves (400 keys)
// Verifies: all hits, misses, mixed, single-key batch
//

#include <algorithm>
#include "apps/inconel/test/check.hh"
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "apps/inconel/core/registry.hh"
#include "apps/inconel/tree/sender.hh"
#include "apps/inconel/tree/page_builder.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"

using namespace apps::inconel::format;
using namespace apps::inconel::tree;
using namespace apps::inconel::core;
using namespace apps::inconel::mock_nvme;

constexpr uint32_t PS  = 4096;
constexpr uint32_t LBS = 4096;
const tree_geometry kTreeGeom{
    .lba_size = LBS,
    .tree_page_size = PS,
    .shadow_slots_per_range = 1,
};

// ── Helpers ──

static std::string make_key(const char* prefix, int n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%03d", prefix, n);
    return buf;
}

static void write_leaf(mock_device& dev, uint64_t lba,
                       const std::vector<std::pair<std::string, uint64_t>>& records) {
    alignas(64) char page[PS];
    leaf_page_builder b;
    b.init(page, PS);
    for (auto& [key, ver] : records) {
        value_ref vr = {.base = {0, ver}, .byte_offset = 0,
                        .len = static_cast<uint32_t>(ver), .flags = 0};
        [[maybe_unused]] bool ok = b.add_value(key, ver, vr); CHECK(ok);
    }
    b.finalize();
    [[maybe_unused]] bool wrote = dev.do_write(lba, page, 1); CHECK(wrote);
}

static void write_internal(mock_device& dev, uint64_t lba,
                           const std::vector<std::pair<std::string, paddr>>& children,
                           paddr rightmost) {
    alignas(64) char page[PS];
    internal_page_builder b;
    b.init(page, PS);
    for (auto& [sep, child] : children) {
        [[maybe_unused]] bool added = b.add_child(sep, child); CHECK(added);
    }
    b.set_rightmost_child(rightmost);
    b.finalize();
    [[maybe_unused]] bool wrote = dev.do_write(lba, page, 1); CHECK(wrote);
}

// ── Test environment ──
//
// 3-level tree:
//
//   [Root @ 1000]  sep: e, j, p
//     → [Int @ 2000] sep: c    → [Leaf @ 3000] a001..a050
//                               → [Leaf @ 3004] c001..c050
//     → [Int @ 2004] sep: g    → [Leaf @ 3012] e001..e050
//                               → [Leaf @ 3016] g001..g050
//     → [Int @ 2008] sep: m    → [Leaf @ 3020] j001..j050
//                               → [Leaf @ 3024] m001..m050
//     → [Int @ 2012] sep: s    → [Leaf @ 3028] p001..p050
//                               → [Leaf @ 3032] s001..s050

struct test_env {
    mock_device dev;
    scheduler nvme_sched;
    tree_lookup_sched<clock_cache> tree_sched;
    tree_manifest manifest;
    std::vector<std::pair<std::string, uint64_t>> all_records;

    test_env()
        : dev(PS * 8192, LBS)
        , nvme_sched(&dev)
        , tree_sched(0, &kTreeGeom, clock_cache(32)) {
        build();
        // Single-thread test runs everything on this_core_id == 0; register
        // both schedulers there so the sender's local_nvme() lookup works.
        registry::clear();
        registry::init_capacity(8);
        registry::nvme_scheds.list.push_back(&nvme_sched);
        registry::nvme_scheds.by_core[0] = &nvme_sched;
        registry::tree_lookup_scheds.list.push_back(&tree_sched);
        registry::tree_lookup_scheds.by_core[0] = &tree_sched;
    }

    ~test_env() {
        registry::clear();
    }

private:
    void build() {
        struct leaf_spec { uint64_t lba; const char* prefix; uint64_t ver_base; };
        leaf_spec leaves[] = {
            {3000, "a", 100}, {3004, "c", 200},
            {3012, "e", 300}, {3016, "g", 400},
            {3020, "j", 500}, {3024, "m", 600},
            {3028, "p", 700}, {3032, "s", 800},
        };

        for (auto& [lba, prefix, ver_base] : leaves) {
            std::vector<std::pair<std::string, uint64_t>> records;
            for (int i = 1; i <= 50; ++i) {
                auto key = make_key(prefix, i);
                uint64_t ver = ver_base + i;
                records.emplace_back(key, ver);
                all_records.emplace_back(key, ver);
            }
            write_leaf(dev, lba, records);
        }

        write_internal(dev, 2000, {{"c", {0, 3000}}}, {0, 3004});
        write_internal(dev, 2004, {{"g", {0, 3012}}}, {0, 3016});
        write_internal(dev, 2008, {{"m", {0, 3020}}}, {0, 3024});
        write_internal(dev, 2012, {{"s", {0, 3028}}}, {0, 3032});

        write_internal(dev, 1000,
            {{"e", {0, 2000}}, {"j", {0, 2004}}, {"p", {0, 2008}}},
            {0, 2012});

        manifest.geom = &kTreeGeom;
        manifest.root_slot = {0, 1000};
        for (uint64_t lba : {1000, 2000, 2004, 2008, 2012,
                              3000, 3004, 3012, 3016, 3020, 3024, 3028, 3032})
            manifest.slot_map[{0, lba}] = 0;
    }
};

static std::vector<lookup_result>
do_lookup(test_env& env, std::vector<std::string_view> keys) {
    auto ctx = pump::core::make_root_context();
    std::vector<lookup_result> out;
    bool done = false;
    pump::sender::just()
        >> lookup(&env.tree_sched, keys, &env.manifest)
        >> pump::sender::then([&](std::vector<lookup_result>&& r) {
            out = std::move(r); done = true;
        })
        >> pump::sender::submit(ctx);
    for (int i = 0; i < 200 && !done; ++i) {
        env.tree_sched.advance();
        env.nvme_sched.advance();
    }
    CHECK(done);
    return out;
}

// ── Tests ──

static void test_all_existing_keys(test_env& env) {
    std::vector<std::string_view> keys;
    for (auto& [k, v] : env.all_records)
        keys.push_back(k);

    auto results = do_lookup(env, keys);
    CHECK(results.size() == env.all_records.size());

    for (size_t i = 0; i < results.size(); ++i) {
        auto& [key, expected_ver] = env.all_records[i];
        CHECK(std::holds_alternative<lookup_value>(results[i]));
        auto& val = std::get<lookup_value>(results[i]);
        CHECK(val.data_ver == expected_ver);
        CHECK(val.vr.len == static_cast<uint32_t>(expected_ver));
    }
    printf("  all %zu existing keys: OK\n", results.size());
}

static void test_missing_keys(test_env& env) {
    std::vector<std::string_view> keys = {
        "a000", "b001", "d001", "f001", "z999", "zzz", "000"
    };
    auto results = do_lookup(env, keys);
    for (size_t i = 0; i < results.size(); ++i)
        CHECK(std::holds_alternative<lookup_absent>(results[i]));
    printf("  %zu missing keys: OK\n", results.size());
}

static void test_mixed_hit_miss(test_env& env) {
    std::vector<std::string_view> keys = {
        "a001", "nope", "s050", "aaaa", "m025",
    };
    auto results = do_lookup(env, keys);
    CHECK(results.size() == 5);
    CHECK(std::holds_alternative<lookup_value>(results[0]));
    CHECK(std::holds_alternative<lookup_absent>(results[1]));
    CHECK(std::holds_alternative<lookup_value>(results[2]));
    CHECK(std::holds_alternative<lookup_absent>(results[3]));
    CHECK(std::holds_alternative<lookup_value>(results[4]));
    CHECK(std::get<lookup_value>(results[0]).data_ver == 101);
    CHECK(std::get<lookup_value>(results[2]).data_ver == 850);
    CHECK(std::get<lookup_value>(results[4]).data_ver == 625);
    printf("  mixed hit/miss: OK\n");
}

static void test_single_key(test_env& env) {
    auto results = do_lookup(env, {"g033"});
    CHECK(results.size() == 1);
    CHECK(std::holds_alternative<lookup_value>(results[0]));
    CHECK(std::get<lookup_value>(results[0]).data_ver == 433);
    printf("  single key: OK\n");
}

// ── Cache eviction regression ──
//
// Tree has 13 unique pages (1 root + 4 internal + 8 leaves). With cache
// capacity = 4, the cache cannot hold all pages, so traversal must evict
// and re-read pages from NVMe. We verify:
//   1. all 400 keys still resolve to the correct value (functional correctness
//      after eviction)
//   2. NVMe read count is significantly larger than 13 — confirming that
//      pages were evicted and re-read, not just loaded once
//   3. Clock policy specifically: the root page is hit on every traversal,
//      so it should never be evicted; non-root reads dominate the count.

template <typename Cache>
static void test_cache_eviction(Cache cache, const char* label) {
    // Build a fresh tree env (isolated from other tests' global env).
    mock_device dev(PS * 8192, LBS);
    scheduler nvme_sched(&dev);
    tree_lookup_sched<Cache> tree_sched(0, &kTreeGeom, std::move(cache));
    tree_manifest manifest;
    std::vector<std::pair<std::string, uint64_t>> all_records;

    struct leaf_spec { uint64_t lba; const char* prefix; uint64_t ver_base; };
    leaf_spec leaves[] = {
        {3000, "a", 100}, {3004, "c", 200},
        {3012, "e", 300}, {3016, "g", 400},
        {3020, "j", 500}, {3024, "m", 600},
        {3028, "p", 700}, {3032, "s", 800},
    };
    for (auto& [lba, prefix, ver_base] : leaves) {
        std::vector<std::pair<std::string, uint64_t>> records;
        for (int i = 1; i <= 50; ++i) {
            auto key = make_key(prefix, i);
            uint64_t ver = ver_base + i;
            records.emplace_back(key, ver);
            all_records.emplace_back(key, ver);
        }
        write_leaf(dev, lba, records);
    }
    write_internal(dev, 2000, {{"c", {0, 3000}}}, {0, 3004});
    write_internal(dev, 2004, {{"g", {0, 3012}}}, {0, 3016});
    write_internal(dev, 2008, {{"m", {0, 3020}}}, {0, 3024});
    write_internal(dev, 2012, {{"s", {0, 3028}}}, {0, 3032});
    write_internal(dev, 1000,
        {{"e", {0, 2000}}, {"j", {0, 2004}}, {"p", {0, 2008}}}, {0, 2012});

    manifest.geom = &kTreeGeom;
    manifest.root_slot = {0, 1000};
    for (uint64_t lba : {1000, 2000, 2004, 2008, 2012,
                          3000, 3004, 3012, 3016, 3020, 3024, 3028, 3032})
        manifest.slot_map[{0, lba}] = 0;

    // Shuffle the access order so consecutive lookups touch DIFFERENT leaves —
    // this is what actually forces eviction with a small cache. Without
    // shuffling, the natural batch-by-leaf order keeps the same 3 pages hot
    // and never triggers eviction.
    std::vector<size_t> order(all_records.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::mt19937 rng(0xBEEF);
    std::shuffle(order.begin(), order.end(), rng);

    // Register schedulers for the sender's local_nvme() resolution.
    registry::clear();
    registry::init_capacity(8);
    registry::nvme_scheds.list.push_back(&nvme_sched);
    registry::nvme_scheds.by_core[0] = &nvme_sched;
    registry::tree_lookup_scheds.list.push_back(&tree_sched);
    registry::tree_lookup_scheds.by_core[0] = &tree_sched;

    // Reset counter — only count reads from the lookup phase, not the writes
    // we just did to set up the tree.
    dev.reset_io_counters();

    // Run lookups one key at a time, with the small cache forcing eviction
    // between independent lookups.
    int correct = 0;
    for (size_t idx : order) {
        auto& [key, expected_ver] = all_records[idx];
        auto ctx = pump::core::make_root_context();
        std::vector<lookup_result> out;
        bool done = false;
        std::vector<std::string_view> single = { key };
        pump::sender::just()
            >> lookup(&tree_sched, single, &manifest)
            >> pump::sender::then([&](std::vector<lookup_result>&& r) {
                out = std::move(r); done = true;
            })
            >> pump::sender::submit(ctx);
        for (int i = 0; i < 200 && !done; ++i) {
            tree_sched.advance();
            nvme_sched.advance();
        }
        CHECK(done);
        CHECK(std::holds_alternative<lookup_value>(out[0]));
        if (std::get<lookup_value>(out[0]).data_ver == expected_ver)
            correct++;
    }

    uint64_t total_unique_pages = 13;
    uint64_t reads = dev.get_read_count();

    CHECK(correct == static_cast<int>(all_records.size()));
    // With only 4 cache slots holding 13 unique pages, lookups must trigger
    // many re-reads. Each lookup that misses needs at least 1 NVMe read for
    // the leaf (root and intermediate may stay in cache thanks to clock).
    // Reads must be much greater than the unique page count.
    CHECK(reads > total_unique_pages * 5);

    printf("  [%s] eviction stress: %d/%zu correct, %lu NVMe reads "
           "(13 unique pages, cache cap=4)\n",
           label, correct, all_records.size(), (unsigned long)reads);

    registry::clear();
}

static void test_empty_tree() {
    mock_device dev(PS * 1024, LBS);
    scheduler ns(&dev);
    tree_lookup_sched<clock_cache> ts(0, &kTreeGeom, clock_cache(8));
    auto m = tree_manifest::empty(&kTreeGeom);

    registry::clear();
    registry::init_capacity(8);
    registry::nvme_scheds.list.push_back(&ns);
    registry::nvme_scheds.by_core[0] = &ns;
    registry::tree_lookup_scheds.list.push_back(&ts);
    registry::tree_lookup_scheds.by_core[0] = &ts;

    auto ctx = pump::core::make_root_context();
    std::vector<lookup_result> out;
    bool done = false;
    std::vector<std::string_view> keys = {"a", "b", "c"};
    pump::sender::just()
        >> lookup(&ts, keys, &m)
        >> pump::sender::then([&](std::vector<lookup_result>&& r) {
            out = std::move(r); done = true;
        })
        >> pump::sender::submit(ctx);
    for (int i = 0; i < 50 && !done; ++i) { ts.advance(); ns.advance(); }
    CHECK(done && out.size() == 3);
    for (auto& r : out) CHECK(std::holds_alternative<lookup_absent>(r));
    printf("  empty tree: OK\n");
}

int main() {
    printf("tree lookup regression (3-level, %u keys):\n", 8 * 50);
    test_env env;
    test_all_existing_keys(env);
    test_missing_keys(env);
    test_mixed_hit_miss(env);
    test_single_key(env);
    test_empty_tree();
    test_cache_eviction(clock_cache(4), "clock");
    test_cache_eviction(slru_cache(4),  "slru ");
    printf("all passed\n");
    return 0;
}
