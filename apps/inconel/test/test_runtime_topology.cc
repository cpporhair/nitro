//
// Asymmetric runtime topology integration test
//
// Exercises the role-aware `runtime::build_runtime` path with a layout
// that mirrors the planned production wiring:
//
//   - core 0         : value scheduler + (future) business logic
//   - cores 2, 4, 6  : tree_read_domain (lookup + worker)
//   - core 8         : tree_sched (owner)
//   - every core     : mock_nvme::scheduler, sharing one mock_device
//
// Goals:
//   1. build_runtime wires each scheduler to the core its role names,
//      non-role slots carry nullptr in the PUMP per-core tuple.
//   2. PUMP `run()` loops advance on every participating core without
//      panic when there is no work queued.
//   3. Stop/teardown is clean — every jthread joins, destroy_runtime
//      releases every scheduler.
//

#include "apps/inconel/test/check.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "env/runtime/share_nothing.hh"

#include "apps/inconel/core/page_cache.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/tree_read_domain.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/mock_nvme/device.hh"
#include "apps/inconel/mock_nvme/scheduler.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/runtime/run.hh"
#include "apps/inconel/tree/owner_scheduler.hh"
#include "apps/inconel/value/scheduler.hh"

using namespace apps::inconel;
using Cache          = core::clock_cache;
using TreeReadDomain = core::tree_read_domain<Cache>;
using ValueSched     = value::value_alloc_sched<Cache>;

static uint64_t
bootstrap_namespace_size_bytes() {
    const auto& p = format::kBootstrapFormatProfile;
    return static_cast<uint64_t>(p.lba_size) * p.value_data_area_end.lba;
}

int
main() {
    printf("inconel runtime topology test:\n");

    // The topology names cores up to 8; machines with fewer hardware
    // cores cannot run this fixture end-to-end — PUMP's per-core tuple
    // is sized by `std::thread::hardware_concurrency()` and any core
    // id beyond that fails `add_core_schedulers`'s bounds assert.
    const uint32_t hw = std::thread::hardware_concurrency();
    if (hw < 9) {
        printf("  skipped (hardware_concurrency=%u, need >= 9)\n", hw);
        return 0;
    }

    mock_nvme::mock_device dev(bootstrap_namespace_size_bytes(),
                               format::kBootstrapFormatProfile.lba_size);
    std::mutex dev_mtx;
    dev.enable_thread_safety(&dev_mtx);

    const std::vector<uint32_t> cores    = {0, 2, 4, 6, 8};
    const std::vector<uint32_t> rd_cores = {2, 4, 6};
    constexpr int32_t           kValueCore = 0;
    constexpr int32_t           kOwnerCore = 8;

    runtime::build_options opts{
        .cores               = cores,
        .device              = &dev,
        .read_domain_cores   = rd_cores,
        .value_core          = kValueCore,
        .owner_core          = kOwnerCore,
        .tree_cache_capacity = 16,
    };
    auto* rt = runtime::build_runtime<Cache, Cache>(opts);

    // ── 1. Registry topology reflects the role map ──────────────────
    CHECK(core::registry::nvme_count() == cores.size());
    CHECK(core::registry::tree_read_domain_count() == rd_cores.size());
    for (uint32_t c : cores) {
        CHECK(core::registry::nvme_for_core(c) != nullptr);
    }
    for (uint32_t c : rd_cores) {
        auto* rd = core::registry::tree_read_domain_for_core(c);
        CHECK(rd != nullptr);
        CHECK(rd->lookup_sched != nullptr);
        CHECK(rd->worker_sched != nullptr);
    }
    CHECK(core::registry::tree_read_domain_for_core(kValueCore) == nullptr);
    CHECK(core::registry::tree_read_domain_for_core(kOwnerCore) == nullptr);
    CHECK(core::registry::value_alloc_sched != nullptr);
    CHECK(core::registry::tree_sched_singleton_ptr != nullptr);

    // Read-domain indices match the order they appear in `rd_cores`.
    for (size_t i = 0; i < rd_cores.size(); ++i) {
        auto* rd = core::registry::tree_read_domain_for_core(rd_cores[i]);
        CHECK(rd->read_domain_index == i);
    }

    // ── 2. PUMP per-core tuple slots ────────────────────────────────
    for (uint32_t c : cores) {
        CHECK(rt->template get_by_core<mock_nvme::scheduler>(c) != nullptr);
    }
    for (uint32_t c : rd_cores) {
        CHECK(rt->template get_by_core<TreeReadDomain>(c) != nullptr);
    }
    CHECK(rt->template get_by_core<TreeReadDomain>(kValueCore) == nullptr);
    CHECK(rt->template get_by_core<TreeReadDomain>(kOwnerCore) == nullptr);

    CHECK(rt->template get_by_core<ValueSched>(kValueCore) != nullptr);
    CHECK(rt->template get_by_core<ValueSched>(kOwnerCore) == nullptr);
    for (uint32_t c : rd_cores) {
        CHECK(rt->template get_by_core<ValueSched>(c) == nullptr);
    }

    CHECK(rt->template get_by_core<tree::tree_sched>(kOwnerCore) != nullptr);
    CHECK(rt->template get_by_core<tree::tree_sched>(kValueCore) == nullptr);
    for (uint32_t c : rd_cores) {
        CHECK(rt->template get_by_core<tree::tree_sched>(c) == nullptr);
    }

    // ── 3. All cores boot + advance + shut down cleanly ─────────────
    std::atomic<uint32_t> cores_started{0};
    std::vector<std::jthread> workers;
    workers.reserve(cores.size());
    for (uint32_t core : cores) {
        workers.emplace_back([rt, core, &cores_started]() {
            rt::run(rt, core,
                [&cores_started](auto*, uint32_t) {
                    cores_started.fetch_add(1, std::memory_order_release);
                });
        });
    }

    while (cores_started.load(std::memory_order_acquire) <
           static_cast<uint32_t>(cores.size())) {
        std::this_thread::yield();
    }

    // Dwell long enough that every core's advance loop cycles many
    // times. There is no queued work, so advance() returns false and
    // share_nothing::run() yields — the test fails only if a scheduler
    // panics, deadlocks, or the scheduler tuple access trips an assert.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    for (uint32_t core : cores) {
        rt->is_running_by_core[core].store(false);
    }
    workers.clear();   // jthread joins on destruction

    runtime::destroy_runtime<Cache, Cache>(rt);
    printf("  [clock] asymmetric topology: OK\n");
    printf("all passed\n");
    return 0;
}
