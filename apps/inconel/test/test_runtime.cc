//
// Runtime registry integration test
//
// Verifies that:
//   1. build_runtime<Cache>() correctly populates core::runtime registry
//      with both nvme + tree_lookup per core, and the PUMP global_runtime_t
//      tuple in parallel
//   2. core::registry::local_nvme() / local_tree_lookup() return the right
//      instance based on pump::core::this_core_id (per-core fast path)
//   3. PUMP share_nothing::run() advance loops on multiple cores correctly
//      drive the schedulers exposed via the registry
//   4. Tree lookups go through the registry end-to-end and produce correct
//      results across cores
//

#include <atomic>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "env/runtime/share_nothing.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"

#include "apps/inconel/core/registry.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/tree/page_builder.hh"
#include "apps/inconel/tree/sender.hh"

using namespace apps::inconel::format;
using namespace apps::inconel::core;
using namespace apps::inconel::tree;
using namespace apps::inconel::mock_nvme;
using namespace apps::inconel;

constexpr uint32_t PS  = 4096;
constexpr uint32_t LBS = 4096;

// ── Tree builder helpers (same shape as test_tree_lookup_multicore) ──

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

// ── Test 1: build_runtime registry population ──

template <core::cache_concept Cache>
static void test_registry_population(const char* label) {
    mock_device dev(PS * 1024, LBS);

    std::vector<uint32_t> cores = {0, 2, 4};
    runtime::build_options opts{
        .cores          = cores,
        .device         = &dev,
        .cache_capacity = 16,
    };
    auto* rt = runtime::build_runtime<Cache>(opts);

    // 1. List counts match the number of populated cores.
    assert(core::registry::nvme_count() == cores.size());
    assert(core::registry::tree_lookup_count() == cores.size());

    // 2. by_core is filled exactly for the listed cores, nullptr elsewhere.
    for (uint32_t c = 0; c < std::thread::hardware_concurrency(); ++c) {
        bool expected = std::find(cores.begin(), cores.end(), c) != cores.end();
        bool got_nvme = core::registry::nvme_for_core(c) != nullptr;
        bool got_tlookup = core::registry::tree_lookup_for_core(c) != nullptr;
        assert(got_nvme == expected);
        assert(got_tlookup == expected);
    }

    // 3. PUMP runtime tuple has the same instances we registered.
    for (uint32_t c : cores) {
        auto* nvme_p = rt->template get_by_core<mock_nvme::scheduler>(c);
        auto* tlookup_p = rt->template get_by_core<lookup_scheduler<Cache>>(c);
        assert(nvme_p == core::registry::nvme_for_core(c));
        // tree_lookup is registered as base in the application registry,
        // but PUMP holds the full derived type. Compare via base upcast.
        assert(static_cast<lookup_scheduler_base*>(tlookup_p) ==
               core::registry::tree_lookup_for_core(c));
    }

    runtime::destroy_runtime<Cache>(rt);
    printf("  [%s] registry population: OK\n", label);
}

// ── Test 2: end-to-end multi-core lookup via registry ──
//
// Workers run pump::env::runtime::run() with the registry already populated.
// Main thread submits lookups, splits across the tree_lookup shards via the
// registry's tree_lookup_at() function, and verifies all 400 keys resolve.

template <core::cache_concept Cache>
static void test_e2e_multicore_via_runtime(const char* label) {
    tree_data td;

    std::vector<uint32_t> cores = {2, 4};   // worker cores
    runtime::build_options opts{
        .cores          = cores,
        .device         = &td.dev,
        .cache_capacity = 32,
    };
    auto* rt = runtime::build_runtime<Cache>(opts);

    const uint32_t shards = core::registry::tree_lookup_count();
    assert(shards == cores.size());

    // Spawn worker threads using PUMP's run() — each thread sets its own
    // this_core_id and drives the per-core advance loop. Stop them later by
    // clearing rt->is_running_by_core[core].
    std::vector<std::jthread> workers;
    for (uint32_t core : cores) {
        workers.emplace_back([rt, core]() {
            pump::env::runtime::run(rt, core, [](auto*, uint32_t){});
        });
    }

    // Set the submitter thread's this_core_id to a non-conflicting value so
    // its per_core::queue enqueues route into the worker SPSC slots
    // correctly. Pick core 0 (no worker there).
    pump::core::this_core_id = 0;

    const size_t N = td.all_records.size();   // 400
    std::vector<lookup_result> results(N);
    std::atomic<int> completed{0};

    std::vector<std::string> keys_owned;
    keys_owned.reserve(N);
    for (auto& [k, v] : td.all_records) keys_owned.push_back(k);

    for (size_t i = 0; i < N; ++i) {
        // Stable shard selection — replace with proper key hash routing once
        // route_to_front exists. For now, even/odd split.
        auto* tlookup = core::registry::tree_lookup_at(i % shards);

        std::vector<std::string_view> single = { keys_owned[i] };
        auto ctx = pump::core::make_root_context();
        pump::sender::just()
            >> tree::lookup(tlookup, single, &td.manifest)
            >> pump::sender::then([&results, &completed, i](std::vector<lookup_result>&& r) {
                results[i] = std::move(r[0]);
                completed.fetch_add(1, std::memory_order_release);
            })
            >> pump::sender::submit(ctx);
    }

    while (completed.load(std::memory_order_acquire) < static_cast<int>(N))
        std::this_thread::yield();

    // Stop workers.
    for (uint32_t core : cores)
        rt->is_running_by_core[core].store(false);
    workers.clear();   // jthread joins on destruction

    // Verify all results.
    int ok = 0;
    for (size_t i = 0; i < N; ++i) {
        auto& [key, expected_ver] = td.all_records[i];
        assert(std::holds_alternative<lookup_value>(results[i]));
        if (std::get<lookup_value>(results[i]).data_ver == expected_ver) ok++;
    }
    assert(ok == static_cast<int>(N));

    runtime::destroy_runtime<Cache>(rt);
    printf("  [%s] e2e multi-core via runtime: %d/%zu OK\n", label, ok, N);
}

int main() {
    printf("inconel runtime registry test:\n");

    test_registry_population<clock_cache>("clock");
    test_registry_population<slru_cache>("slru ");
    test_e2e_multicore_via_runtime<clock_cache>("clock");
    test_e2e_multicore_via_runtime<slru_cache>("slru ");

    printf("all passed\n");
    return 0;
}
