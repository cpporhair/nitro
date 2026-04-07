//
// 3-level tree lookup regression test
//
// Tree: root internal → 4 internal → 8 leaves (400 keys)
// Verifies: all hits, misses, mixed, single-key batch
//

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
        [[maybe_unused]] bool ok = b.add_value(key, ver, vr); assert(ok);
    }
    b.finalize();
    [[maybe_unused]] bool wrote = dev.do_write(lba, page, 1); assert(wrote);
}

static void write_internal(mock_device& dev, uint64_t lba,
                           const std::vector<std::pair<std::string, paddr>>& children,
                           paddr rightmost) {
    alignas(64) char page[PS];
    internal_page_builder b;
    b.init(page, PS);
    for (auto& [sep, child] : children) {
        [[maybe_unused]] bool added = b.add_child(sep, child); assert(added);
    }
    b.set_rightmost_child(rightmost);
    b.finalize();
    [[maybe_unused]] bool wrote = dev.do_write(lba, page, 1); assert(wrote);
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
    lookup_scheduler tree_sched;
    tree_manifest manifest;
    std::vector<std::pair<std::string, uint64_t>> all_records;

    test_env()
        : dev(PS * 8192, LBS)
        , nvme_sched(&dev) {
        build();
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

        manifest.tree_page_size = PS;
        manifest.lba_size = LBS;
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
        >> lookup(&env.tree_sched, &env.nvme_sched, keys, &env.manifest)
        >> pump::sender::then([&](std::vector<lookup_result>&& r) {
            out = std::move(r); done = true;
        })
        >> pump::sender::submit(ctx);
    for (int i = 0; i < 200 && !done; ++i) {
        env.tree_sched.advance();
        env.nvme_sched.advance();
    }
    assert(done);
    return out;
}

// ── Tests ──

static void test_all_existing_keys(test_env& env) {
    std::vector<std::string_view> keys;
    for (auto& [k, v] : env.all_records)
        keys.push_back(k);

    auto results = do_lookup(env, keys);
    assert(results.size() == env.all_records.size());

    for (size_t i = 0; i < results.size(); ++i) {
        auto& [key, expected_ver] = env.all_records[i];
        assert(std::holds_alternative<lookup_value>(results[i]));
        auto& val = std::get<lookup_value>(results[i]);
        assert(val.data_ver == expected_ver);
        assert(val.vr.len == static_cast<uint32_t>(expected_ver));
    }
    printf("  all %zu existing keys: OK\n", results.size());
}

static void test_missing_keys(test_env& env) {
    std::vector<std::string_view> keys = {
        "a000", "b001", "d001", "f001", "z999", "zzz", "000"
    };
    auto results = do_lookup(env, keys);
    for (size_t i = 0; i < results.size(); ++i)
        assert(std::holds_alternative<lookup_absent>(results[i]));
    printf("  %zu missing keys: OK\n", results.size());
}

static void test_mixed_hit_miss(test_env& env) {
    std::vector<std::string_view> keys = {
        "a001", "nope", "s050", "aaaa", "m025",
    };
    auto results = do_lookup(env, keys);
    assert(results.size() == 5);
    assert(std::holds_alternative<lookup_value>(results[0]));
    assert(std::holds_alternative<lookup_absent>(results[1]));
    assert(std::holds_alternative<lookup_value>(results[2]));
    assert(std::holds_alternative<lookup_absent>(results[3]));
    assert(std::holds_alternative<lookup_value>(results[4]));
    assert(std::get<lookup_value>(results[0]).data_ver == 101);
    assert(std::get<lookup_value>(results[2]).data_ver == 850);
    assert(std::get<lookup_value>(results[4]).data_ver == 625);
    printf("  mixed hit/miss: OK\n");
}

static void test_single_key(test_env& env) {
    auto results = do_lookup(env, {"g033"});
    assert(results.size() == 1);
    assert(std::holds_alternative<lookup_value>(results[0]));
    assert(std::get<lookup_value>(results[0]).data_ver == 433);
    printf("  single key: OK\n");
}

static void test_empty_tree() {
    mock_device dev(PS * 1024, LBS);
    scheduler ns(&dev);
    lookup_scheduler ts;
    auto m = tree_manifest::empty(PS, LBS);

    auto ctx = pump::core::make_root_context();
    std::vector<lookup_result> out;
    bool done = false;
    std::vector<std::string_view> keys = {"a", "b", "c"};
    pump::sender::just()
        >> lookup(&ts, &ns, keys, &m)
        >> pump::sender::then([&](std::vector<lookup_result>&& r) {
            out = std::move(r); done = true;
        })
        >> pump::sender::submit(ctx);
    for (int i = 0; i < 50 && !done; ++i) { ts.advance(); ns.advance(); }
    assert(done && out.size() == 3);
    for (auto& r : out) assert(std::holds_alternative<lookup_absent>(r));
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
    printf("all passed\n");
    return 0;
}
