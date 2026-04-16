//
// Multi-core tree lookup regression test
//
// Core 0: test driver, fires many independent lookup pipelines
// Core 2: mock_nvme + tree_lookup_sched (handles half the keys)
// Core 4: mock_nvme + tree_lookup_sched (handles other half)
// Both mock_nvme share one mock_device (with mutex)
//
// Concurrency: 400 individual pipelines in-flight simultaneously,
// interleaving on cores 2 and 4.
//

#include "apps/inconel/test/check.hh"
#include <cstdio>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pump/core/lock_free_queue.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/shard_partition_builder.hh"
#include "apps/inconel/core/tree_read_domain.hh"
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

// ── Tree builder ──

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
    for (auto& [sep, child] : children) { [[maybe_unused]] bool ok = b.add_child(sep, child); CHECK(ok); }
    b.set_rightmost_child(rightmost);
    b.finalize();
    [[maybe_unused]] bool wrote = dev.do_write(lba, page, 1); CHECK(wrote);
}

// Leaf order for the shared 3-level/8-leaf fixture. Required by
// `tree::lookup`'s key-range router (INC-040): without the index every
// routed key would panic as "outside leaf_order coverage". Fence bytes
// match the internal-page separators exactly.
static leaf_order_index
build_leaf_order_8leaves() {
    leaf_order_index idx;
    idx.fence_pool = "cegjmps";
    static const uint64_t bases[]     = {3000, 3004, 3012, 3016,
                                          3020, 3024, 3028, 3032};
    static const uint32_t lower_off[] = {0, 0, 1, 2, 3, 4, 5, 6};
    static const uint16_t lower_len[] = {0, 1, 1, 1, 1, 1, 1, 1};
    static const uint32_t upper_off[] = {0, 1, 2, 3, 4, 5, 6, 0};
    static const uint16_t upper_len[] = {1, 1, 1, 1, 1, 1, 1, 0};
    for (int i = 0; i < 8; ++i) {
        idx.spans.push_back({
            .fence_lower_off = lower_off[i],
            .fence_upper_off = upper_off[i],
            .fence_lower_len = lower_len[i],
            .fence_upper_len = upper_len[i],
            .leaf_range_base = paddr{0, bases[i]},
        });
    }
    return idx;
}

struct tree_data {
    mock_device dev;
    std::mutex dev_mtx;
    tree_manifest manifest;
    std::vector<std::pair<std::string, uint64_t>> all_records;

    tree_data() : dev(PS * 8192, LBS) {
        dev.enable_thread_safety(&dev_mtx);
        build();
    }

private:
    void build() {
        struct leaf_spec { uint64_t lba; const char* prefix; uint64_t ver_base; };
        leaf_spec leaves[] = {
            {3000, "a", 100}, {3004, "c", 200}, {3012, "e", 300}, {3016, "g", 400},
            {3020, "j", 500}, {3024, "m", 600}, {3028, "p", 700}, {3032, "s", 800},
        };
        for (auto& [lba, prefix, ver_base] : leaves) {
            std::vector<std::pair<std::string, uint64_t>> records;
            for (int i = 1; i <= 50; ++i) {
                records.emplace_back(make_key(prefix, i), ver_base + i);
                all_records.emplace_back(make_key(prefix, i), ver_base + i);
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
        manifest.leaf_order = build_leaf_order_8leaves();
    }
};

struct core_schedulers {
    scheduler                     nvme_sched;
    tree_read_domain<clock_cache> read_domain;

    core_schedulers(mock_device*                               dev,
                    uint32_t                                   read_domain_index,
                    std::shared_ptr<const shard_partition_map> partitions)
        : nvme_sched(dev)
        , read_domain(read_domain_index,
                      std::move(partitions),
                      clock_cache(32),
                      &kTreeGeom) {}

    void advance() { read_domain.advance(); nvme_sched.advance(); }
};

int main() {
    printf("multi-core concurrent tree lookup (core 0,2,4):\n");

    tree_data td;
    const size_t N = td.all_records.size();  // 400
    printf("  built 3-level tree: %zu keys\n", N);

    // Build the shard_partition_map from the manifest's leaf_order.
    // With K = 2 read_domains, `build_initial_shard_partition_map`
    // produces a 2-shard map whose fence_upper splits the 8 leaves
    // into contiguous halves — shard 0 covers leaves 0..3 (a,c,e,g =
    // 200 keys), shard 1 covers leaves 4..7 (j,m,p,s = 200 keys).
    auto partitions = std::make_shared<const shard_partition_map>(
        build_initial_shard_partition_map(td.manifest.leaf_order, 2));

    core_schedulers core2(&td.dev, 0, partitions);
    core_schedulers core4(&td.dev, 1, partitions);

    // Register both core's schedulers — sender::lookup() uses
    // registry::local_nvme() which indexes by this_core_id.
    registry::clear();
    registry::init_capacity(8);
    registry::install_shard_partitions(partitions);
    registry::nvme_scheds.list.push_back(&core2.nvme_sched);
    registry::nvme_scheds.list.push_back(&core4.nvme_sched);
    registry::nvme_scheds.by_core[2] = &core2.nvme_sched;
    registry::nvme_scheds.by_core[4] = &core4.nvme_sched;
    registry::tree_read_domains.list.push_back(&core2.read_domain);
    registry::tree_read_domains.list.push_back(&core4.read_domain);
    registry::tree_read_domains.by_core[2] = &core2.read_domain;
    registry::tree_read_domains.by_core[4] = &core4.read_domain;

    std::atomic<bool> running{true};

    // ── Per-key result slots ──
    std::vector<lookup_result> results(N);
    std::atomic<int> completed{0};

    // ── Stable key storage (string_views point here) ──
    std::vector<std::string> keys_owned;
    keys_owned.reserve(N);
    for (auto& [k, v] : td.all_records)
        keys_owned.push_back(k);

    // ── Start worker threads ──
    std::thread t2([&] {
        pump::core::this_core_id = 2;
        while (running.load(std::memory_order_relaxed))
            core2.advance();
    });

    std::thread t4([&] {
        pump::core::this_core_id = 4;
        while (running.load(std::memory_order_relaxed))
            core4.advance();
    });

    // ── Core 0: fire 400 independent lookup pipelines ──
    pump::core::this_core_id = 0;

    for (size_t i = 0; i < N; ++i) {
        // Routing is now resolved internally by `tree::lookup` via
        // `current_shard_partitions()->route(key)` — the caller no
        // longer picks a shard pointer (INC-040 / INC-003 / step 030).
        // With 8 leaves range-partitioned into 2 shards the first
        // 4 leaves (a,c,e,g = 200 keys) land on shard 0 (core 2)
        // and the last 4 leaves (j,m,p,s = 200 keys) on shard 1
        // (core 4), so both worker cores carry balanced work and
        // `tree_node` cache hits stay partitioned by key range —
        // the invariant the read_domain design is built around.
        std::vector<std::string_view> single_key = { keys_owned[i] };
        auto ctx = pump::core::make_root_context();

        pump::sender::just()
            >> lookup(single_key, &td.manifest)
            >> pump::sender::then([&results, &completed, i](std::vector<lookup_result>&& r) {
                results[i] = std::move(r[0]);
                completed.fetch_add(1, std::memory_order_release);
            })
            >> pump::sender::submit(ctx);
    }

    printf("  fired %zu concurrent pipelines\n", N);

    // ── Wait for all ──
    while (completed.load(std::memory_order_acquire) < static_cast<int>(N))
        std::this_thread::yield();

    running.store(false, std::memory_order_relaxed);
    t2.join();
    t4.join();

    // ── Verify every result ──
    int hits = 0, misses = 0;
    for (size_t i = 0; i < N; ++i) {
        auto& [key, expected_ver] = td.all_records[i];
        CHECK(std::holds_alternative<lookup_value>(results[i]));
        auto& v = std::get<lookup_value>(results[i]);
        CHECK(v.data_ver == expected_ver);
        CHECK(v.vr.len == static_cast<uint32_t>(expected_ver));
        hits++;
    }
    printf("  verified: %d hits, all correct\n", hits);

    // ── Also fire some missing keys concurrently ──
    {
        constexpr int M = 100;
        std::vector<std::string> miss_keys_owned;
        for (int i = 0; i < M; ++i)
            miss_keys_owned.push_back("miss" + std::to_string(i));

        std::vector<lookup_result> miss_results(M);
        std::atomic<int> miss_completed{0};
        running.store(true);

        std::thread t2b([&] {
            pump::core::this_core_id = 2;
            while (running.load(std::memory_order_relaxed)) core2.advance();
        });
        std::thread t4b([&] {
            pump::core::this_core_id = 4;
            while (running.load(std::memory_order_relaxed)) core4.advance();
        });

        pump::core::this_core_id = 0;
        for (int i = 0; i < M; ++i) {
            std::vector<std::string_view> k = { miss_keys_owned[i] };
            auto ctx = pump::core::make_root_context();
            pump::sender::just()
                >> lookup(k, &td.manifest)
                >> pump::sender::then([&miss_results, &miss_completed, i](std::vector<lookup_result>&& r) {
                    miss_results[i] = std::move(r[0]);
                    miss_completed.fetch_add(1, std::memory_order_release);
                })
                >> pump::sender::submit(ctx);
        }

        while (miss_completed.load(std::memory_order_acquire) < M)
            std::this_thread::yield();

        running.store(false);
        t2b.join();
        t4b.join();

        for (int i = 0; i < M; ++i)
            CHECK(std::holds_alternative<lookup_absent>(miss_results[i]));
        printf("  verified: %d concurrent misses, all absent\n", M);
    }

    printf("all passed\n");
    return 0;
}
