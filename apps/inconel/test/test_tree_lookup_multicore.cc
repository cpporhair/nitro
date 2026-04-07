//
// Multi-core tree lookup regression test
//
// Core 0: test driver, fires many independent lookup pipelines
// Core 2: mock_nvme + lookup_scheduler (handles half the keys)
// Core 4: mock_nvme + lookup_scheduler (handles other half)
// Both mock_nvme share one mock_device (with mutex)
//
// Concurrency: 400 individual pipelines in-flight simultaneously,
// interleaving on cores 2 and 4.
//

#include <cassert>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pump/core/lock_free_queue.hh"
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
    for (auto& [sep, child] : children) { [[maybe_unused]] bool ok = b.add_child(sep, child); assert(ok); }
    b.set_rightmost_child(rightmost);
    b.finalize();
    [[maybe_unused]] bool wrote = dev.do_write(lba, page, 1); assert(wrote);
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

        manifest.tree_page_size = PS;
        manifest.lba_size = LBS;
        manifest.root_slot = {0, 1000};
        for (uint64_t lba : {1000, 2000, 2004, 2008, 2012,
                              3000, 3004, 3012, 3016, 3020, 3024, 3028, 3032})
            manifest.slot_map[{0, lba}] = 0;
    }
};

struct core_schedulers {
    scheduler nvme_sched;
    lookup_scheduler<clock_cache> tree_sched;
    core_schedulers(mock_device* dev)
        : nvme_sched(dev), tree_sched(clock_cache(32)) {}
    void advance() { tree_sched.advance(); nvme_sched.advance(); }
};

int main() {
    printf("multi-core concurrent tree lookup (core 0,2,4):\n");

    tree_data td;
    const size_t N = td.all_records.size();  // 400
    printf("  built 3-level tree: %zu keys\n", N);

    core_schedulers core2(&td.dev);
    core_schedulers core4(&td.dev);

    // Register both core's schedulers — sender::lookup() uses
    // registry::local_nvme() which indexes by this_core_id.
    registry::clear();
    registry::init_capacity(8);
    registry::nvme_scheds.list.push_back(&core2.nvme_sched);
    registry::nvme_scheds.list.push_back(&core4.nvme_sched);
    registry::nvme_scheds.by_core[2] = &core2.nvme_sched;
    registry::nvme_scheds.by_core[4] = &core4.nvme_sched;
    registry::tree_lookup_scheds.list.push_back(&core2.tree_sched);
    registry::tree_lookup_scheds.list.push_back(&core4.tree_sched);
    registry::tree_lookup_scheds.by_core[2] = &core2.tree_sched;
    registry::tree_lookup_scheds.by_core[4] = &core4.tree_sched;

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
        // Route: even → core 2, odd → core 4
        auto* ts = (i % 2 == 0) ? &core2.tree_sched : &core4.tree_sched;

        std::vector<std::string_view> single_key = { keys_owned[i] };
        auto ctx = pump::core::make_root_context();

        pump::sender::just()
            >> lookup(ts, single_key, &td.manifest)
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
        assert(std::holds_alternative<lookup_value>(results[i]));
        auto& v = std::get<lookup_value>(results[i]);
        assert(v.data_ver == expected_ver);
        assert(v.vr.len == static_cast<uint32_t>(expected_ver));
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
            auto* ts = (i % 2 == 0) ? &core2.tree_sched : &core4.tree_sched;
            std::vector<std::string_view> k = { miss_keys_owned[i] };
            auto ctx = pump::core::make_root_context();
            pump::sender::just()
                >> lookup(ts, k, &td.manifest)
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
            assert(std::holds_alternative<lookup_absent>(miss_results[i]));
        printf("  verified: %d concurrent misses, all absent\n", M);
    }

    printf("all passed\n");
    return 0;
}
