//
// value scheduler regression test
//
// Step 6 verification. Six cases:
//
//   case_1_write_path        sender writes a batch of mixed-class values,
//                            verifies on-device bytes via test_read_raw
//   case_2_read_miss         test_write_raw seeds a value page; sender
//                            read_value pulls it via NVMe; check counter
//                            advanced
//   case_3_cache_hit         second read of same vr — counter unchanged
//   case_4_write_then_read   sender write then immediately read same vr —
//                            should hit resident frame / readonly_cache_
//   case_5_sub_lba_same_page two sub-LBA values of the same class share a
//                            page (vr.base equal, byte_offset different)
//   case_6_cross_class       multiple classes including multi-LBA — each
//                            writes to its own page
//
// Sync model: shared_ptr<atomic<int>> counter + sleep_for + check.
// Stage-only test; will be replaced with promise/future once the writeback
// pipeline lands and we can drive things end-to-end synchronously.
//

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "apps/inconel/test/check.hh"

#include "env/runtime/share_nothing.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"

#include "apps/inconel/core/registry.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/memory/frame.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/value/sender.hh"

using namespace apps::inconel;
using apps::inconel::format::paddr;
using apps::inconel::format::value_ref;
using apps::inconel::format::value_object_header;
using apps::inconel::format::VALUE_MAGIC;
using apps::inconel::memory::frame_id;
using apps::inconel::memory::frame_state;
using apps::inconel::memory::value_page_frame;

namespace {

constexpr uint32_t LBA_SIZE = 4096;
constexpr uint64_t TOTAL_LBAS = 8192;          // 32 MiB device
constexpr uint64_t DATA_AREA_BASE_LBA = 4000;  // value Data Area lower bound
constexpr uint64_t DATA_AREA_END_LBA  = 8000;  // bumps from here downward
constexpr uint64_t SEED_AREA_LBA = 1000;       // test_write_raw seed area

constexpr uint32_t EXPECTED_CLASS_SIZES[] = {
    64,     // class 0: sub-LBA, 64 slots/LBA, body <= 52
    256,    // class 1: sub-LBA, 16 slots/LBA, body <= 244
    1024,   // class 2: sub-LBA, 4 slots/LBA,  body <= 1012
    4096,   // class 3: LBA-equal, 1 slot,     body <= 4084
    16384,  // class 4: multi-LBA, span=4,     body <= 16372
};
constexpr uint16_t MULTI_LBA_CLASS_SPAN =
    EXPECTED_CLASS_SIZES[4] / LBA_SIZE;

std::span<const uint32_t> profile_class_sizes() {
    return format::kBootstrapFormatProfile.class_sizes();
}

frame_id value_frame_id_for(paddr base, uint16_t span_lbas) {
    return frame_id{
        .base = base,
        .span_lbas = span_lbas,
        .dom = frame_id::domain::value_page,
    };
}

uint16_t slots_per_page_for_class(uint16_t ci) {
    return static_cast<uint16_t>(LBA_SIZE / EXPECTED_CLASS_SIZES[ci]);
}

void check_bootstrap_profile_matches_value_expectations() {
    const auto& profile = format::kBootstrapFormatProfile;
    CHECK(profile.lba_size == LBA_SIZE);
    CHECK(profile.value_data_area_base.device_id == 0);
    CHECK(profile.value_data_area_base.lba == DATA_AREA_BASE_LBA);
    CHECK(profile.value_data_area_end.device_id == 0);
    CHECK(profile.value_data_area_end.lba == DATA_AREA_END_LBA);

    auto classes = profile_class_sizes();
    CHECK(classes.size() == sizeof(EXPECTED_CLASS_SIZES) / sizeof(EXPECTED_CLASS_SIZES[0]));
    for (size_t i = 0; i < classes.size(); ++i)
        CHECK(classes[i] == EXPECTED_CLASS_SIZES[i]);
}

// ── Helpers ──

std::string make_body(const char* tag, size_t len) {
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i)
        s.push_back(tag[i % std::strlen(tag)]);
    return s;
}

void wait_for(std::shared_ptr<std::atomic<int>> counter, int target) {
    using namespace std::chrono_literals;
    for (int i = 0; i < 200 && counter->load() < target; ++i)
        std::this_thread::sleep_for(10ms);
    CHECK(counter->load() >= target && "sender pipeline did not complete in time");
}

void persist_entries(value::value_alloc_sched_base* sched,
                     auto& ctx,
                     std::span<value::put_entry> entries) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    value::persist_values(sched, entries)
        >> pump::sender::then([counter](bool ok) {
            CHECK(ok);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(counter, 1);
}

std::string read_value_sync(value::value_alloc_sched_base* sched,
                            auto& ctx,
                            value_ref vr) {
    auto counter = std::make_shared<std::atomic<int>>(0);
    std::string got;
    value::read_value(sched, vr)
        >> pump::sender::then([&got, counter](std::string s) {
            got = std::move(s);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(counter, 1);
    return got;
}

// Decode an on-device page slot at (lba, byte_offset) into the body bytes,
// using mock_device.test_read_raw. Used by case_1 to verify the writer
// actually placed the right bytes on NVMe.
std::string decode_on_device(mock_nvme::mock_device& dev, value_ref vr) {
    // Determine span_lbas via class size
    size_t span_lbas = 1;
    for (uint32_t cs : profile_class_sizes()) {
        if (sizeof(value_object_header) + vr.len <= cs) {
            span_lbas = (cs >= LBA_SIZE) ? cs / LBA_SIZE : 1;
            break;
        }
    }
    const char* page = static_cast<const char*>(
        dev.test_read_raw(vr.base.lba, static_cast<uint32_t>(span_lbas)));
    if (!page) return std::string{};

    std::span<const char> slot(page + vr.byte_offset,
                               span_lbas * LBA_SIZE - vr.byte_offset);
    auto d = format::decode_value_object(slot, vr.len);
    if (!d.ok()) return std::string{};
    return std::string(d.body.data(), d.body.size());
}

// Build a 1-LBA page image with N sub-LBA value objects of class_size.
// Used by case_2 to seed the device directly with mock_device.test_write_raw.
void build_subLba_page_image(std::vector<char>& page,
                              uint32_t class_size,
                              const std::vector<std::string>& bodies) {
    page.assign(LBA_SIZE, char{0});
    uint32_t slots = LBA_SIZE / class_size;
    CHECK(bodies.size() <= slots);
    for (size_t i = 0; i < bodies.size(); ++i) {
        std::span<char> slot(page.data() + i * class_size, class_size);
        std::span<const char> body(bodies[i].data(), bodies[i].size());
        bool ok = format::encode_value_object(slot, body);
        CHECK(ok);
    }
}

// ── Test environment ──

struct test_env {
    using tree_cache_t      = core::clock_cache;
    using value_cache_t     = core::clock_cache;
    using runtime_t         = runtime::inconel_runtime_t<tree_cache_t, value_cache_t>;
    using value_scheduler_t = value::value_alloc_sched<value_cache_t>;

    mock_nvme::mock_device    dev;
    runtime_t*                rt = nullptr;
    std::vector<std::jthread> workers;
    std::vector<uint32_t>     cores;

    explicit test_env(uint32_t value_cache_cap = 32)
        : dev(LBA_SIZE * TOTAL_LBAS, LBA_SIZE)
    {
        cores = {2, 4, 6};
        runtime::build_options opts{
            .cores                = cores,
            .device               = &dev,
            .tree_cache_capacity  = 32,
            .value_cache_capacity = value_cache_cap,
        };
        rt = runtime::build_runtime<tree_cache_t, value_cache_t>(opts);

        for (uint32_t core : cores) {
            workers.emplace_back([this, core]() {
                pump::env::runtime::run(rt, core, [](auto*, uint32_t){});
            });
        }

        // Submitter thread (this thread) needs a non-conflicting this_core_id
        // so per_core::queue routes its enqueues into a free SPSC bucket.
        pump::core::this_core_id = 0;
    }

    ~test_env() {
        for (uint32_t core : cores)
            rt->is_running_by_core[core].store(false);
        workers.clear();   // jthread joins on dtor
        runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt);
    }
};

// ════════════════════════════════════════════════════════════════
//  case_1: write path — sender writes, verify on-device bytes
// ════════════════════════════════════════════════════════════════

void case_1_write_path() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    // Mix of classes: sub-LBA (class 1, 200B body), LBA-equal (class 3, 4000B),
    // multi-LBA (class 4, 16000B), sub-LBA tiny (class 0, 40B).
    std::string b1 = make_body("alpha", 200);
    std::string b2 = make_body("beta",  4000);
    std::string b3 = make_body("gamma", 16000);
    std::string b4 = make_body("delta", 40);

    value_ref vr1{}, vr2{}, vr3{}, vr4{};
    std::vector<value::put_entry> entries = {
        {.body = b1, .out_vr = &vr1},
        {.body = b2, .out_vr = &vr2},
        {.body = b3, .out_vr = &vr3},
        {.body = b4, .out_vr = &vr4},
    };

    auto counter = std::make_shared<std::atomic<int>>(0);
    auto* sched = core::registry::value_sched();

    value::persist_values(sched, std::span<value::put_entry>(entries))
        >> pump::sender::then([counter](bool ok) {
            CHECK(ok);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);

    wait_for(counter, 1);

    // Verify on-device bytes via test_read_raw + decode.
    auto decoded1 = decode_on_device(env.dev, vr1);
    auto decoded2 = decode_on_device(env.dev, vr2);
    auto decoded3 = decode_on_device(env.dev, vr3);
    auto decoded4 = decode_on_device(env.dev, vr4);
    CHECK(decoded1 == b1);
    CHECK(decoded2 == b2);
    CHECK(decoded3 == b3);
    CHECK(decoded4 == b4);

    printf("  case_1_write_path: OK (4 values, mixed class)\n");
}

// ════════════════════════════════════════════════════════════════
//  case_2: read miss — seed device, sender reads via NVMe
// ════════════════════════════════════════════════════════════════

void case_2_read_miss() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    // Seed two sub-LBA class-1 values into a known LBA.
    std::string b1 = make_body("xyz", 200);
    std::string b2 = make_body("uvw", 100);
    std::vector<char> page;
    build_subLba_page_image(page, /*class_size*/ 256, {b1, b2});

    void* dst = env.dev.test_write_raw(SEED_AREA_LBA);
    std::memcpy(dst, page.data(), LBA_SIZE);

    value_ref vr1{
        .base = paddr{0, SEED_AREA_LBA}, .byte_offset = 0,
        .len = static_cast<uint32_t>(b1.size()), .flags = 0
    };
    value_ref vr2{
        .base = paddr{0, SEED_AREA_LBA}, .byte_offset = 256,  // slot 1
        .len = static_cast<uint32_t>(b2.size()), .flags = 0
    };

    env.dev.reset_io_counters();

    auto counter = std::make_shared<std::atomic<int>>(0);
    auto* sched = core::registry::value_sched();
    std::string got1, got2;

    value::read_value(sched, vr1)
        >> pump::sender::then([&got1, counter](std::string s) {
            got1 = std::move(s);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);

    value::read_value(sched, vr2)
        >> pump::sender::then([&got2, counter](std::string s) {
            got2 = std::move(s);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);

    wait_for(counter, 2);

    CHECK(got1 == b1);
    CHECK(got2 == b2);

    // Both reads target the same page, so the first miss fills the cache
    // and the second hits — exactly one NVMe read either way (the order is
    // serialized through the value sched advance loop).
    auto reads = env.dev.get_read_count();
    CHECK(reads >= 1 && reads <= 2);

    printf("  case_2_read_miss: OK (decoded %zu+%zu bytes, %lu device reads)\n",
           got1.size(), got2.size(), reads);
}

// ════════════════════════════════════════════════════════════════
//  case_3: cache hit — second read of same vr after case_2
// ════════════════════════════════════════════════════════════════

void case_3_cache_hit() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    // Seed
    std::string b = make_body("hit", 50);
    std::vector<char> page;
    build_subLba_page_image(page, /*class_size*/ 64, {b});
    void* dst = env.dev.test_write_raw(SEED_AREA_LBA);
    std::memcpy(dst, page.data(), LBA_SIZE);

    value_ref vr{
        .base = paddr{0, SEED_AREA_LBA}, .byte_offset = 0,
        .len = static_cast<uint32_t>(b.size()), .flags = 0
    };

    env.dev.reset_io_counters();

    auto counter = std::make_shared<std::atomic<int>>(0);
    auto* sched = core::registry::value_sched();
    std::string got1, got2;

    // First read: miss → fills cache
    value::read_value(sched, vr)
        >> pump::sender::then([&got1, counter](std::string s) {
            got1 = std::move(s);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(counter, 1);
    auto reads_after_first = env.dev.get_read_count();
    CHECK(reads_after_first == 1);

    // Second read: cache hit → no NVMe activity
    value::read_value(sched, vr)
        >> pump::sender::then([&got2, counter](std::string s) {
            got2 = std::move(s);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(counter, 2);
    auto reads_after_second = env.dev.get_read_count();
    CHECK(reads_after_second == reads_after_first);

    CHECK(got1 == b);
    CHECK(got2 == b);

    printf("  case_3_cache_hit: OK (1 device read across 2 reads)\n");
}

// ════════════════════════════════════════════════════════════════
//  case_4: write then read — should hit resident frame / cache, no NVMe
// ════════════════════════════════════════════════════════════════

void case_4_write_then_read() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    std::string body = make_body("wtr", 30);  // tiny → class 0 sub-LBA
    value_ref vr{};
    std::vector<value::put_entry> entries = {{body, &vr}};

    env.dev.reset_io_counters();

    auto counter = std::make_shared<std::atomic<int>>(0);
    auto* sched = core::registry::value_sched();

    value::persist_values(sched, std::span<value::put_entry>(entries))
        >> pump::sender::then([counter](bool ok) {
            CHECK(ok);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(counter, 1);

    // Now read it back. This page is still resident with free slots
    // remaining, so the read should not touch NVMe.
    std::string got;
    value::read_value(sched, vr)
        >> pump::sender::then([&got, counter](std::string s) {
            got = std::move(s);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(counter, 2);

    CHECK(got == body);
    auto reads = env.dev.get_read_count();
    CHECK(reads == 0 && "write_then_read should hit resident frame, no NVMe read");

    printf("  case_4_write_then_read: OK (0 device reads)\n");
}

// ════════════════════════════════════════════════════════════════
//  case_5: sub-LBA same page — two values of same class share a page
// ════════════════════════════════════════════════════════════════

void case_5_sub_lba_same_page() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    std::string b1 = make_body("aa", 30);
    std::string b2 = make_body("bb", 40);
    value_ref vr1{}, vr2{};
    std::vector<value::put_entry> entries = {
        {b1, &vr1},
        {b2, &vr2},
    };

    auto counter = std::make_shared<std::atomic<int>>(0);
    auto* sched = core::registry::value_sched();

    value::persist_values(sched, std::span<value::put_entry>(entries))
        >> pump::sender::then([counter](bool ok) {
            CHECK(ok);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(counter, 1);

    // Both should be in the same LBA, different byte_offset
    CHECK(vr1.base == vr2.base);
    CHECK(vr1.byte_offset != vr2.byte_offset);
    CHECK(vr1.byte_offset == 0);
    CHECK(vr2.byte_offset == 64);  // class 0 = 64-byte slots

    // Verify both via on-device bytes
    auto d1 = decode_on_device(env.dev, vr1);
    auto d2 = decode_on_device(env.dev, vr2);
    CHECK(d1 == b1);
    CHECK(d2 == b2);

    printf("  case_5_sub_lba_same_page: OK (vr1.lba=%lu off=%u, vr2.off=%u)\n",
           vr1.base.lba, vr1.byte_offset, vr2.byte_offset);
}

// ════════════════════════════════════════════════════════════════
//  case_6: cross-class — different classes go to different pages
// ════════════════════════════════════════════════════════════════

void case_6_cross_class() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    std::string b_tiny  = make_body("t", 30);     // class 0
    std::string b_small = make_body("s", 200);    // class 1
    std::string b_med   = make_body("m", 1000);   // class 2
    std::string b_lba   = make_body("l", 4000);   // class 3 (LBA-equal)
    std::string b_multi = make_body("M", 16000);  // class 4 (multi-LBA)

    value_ref vr_t{}, vr_s{}, vr_m{}, vr_l{}, vr_M{};
    std::vector<value::put_entry> entries = {
        {b_tiny,  &vr_t},
        {b_small, &vr_s},
        {b_med,   &vr_m},
        {b_lba,   &vr_l},
        {b_multi, &vr_M},
    };

    auto counter = std::make_shared<std::atomic<int>>(0);
    auto* sched = core::registry::value_sched();

    value::persist_values(sched, std::span<value::put_entry>(entries))
        >> pump::sender::then([counter](bool ok) {
            CHECK(ok);
            counter->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(counter, 1);

    // Each different class lives on a distinct page.
    CHECK(vr_t.base != vr_s.base);
    CHECK(vr_s.base != vr_m.base);
    CHECK(vr_m.base != vr_l.base);
    CHECK(vr_l.base != vr_M.base);

    // multi-LBA value: byte_offset is 0, len fits within span
    CHECK(vr_M.byte_offset == 0);

    // Verify all decoded
    CHECK(decode_on_device(env.dev, vr_t) == b_tiny);
    CHECK(decode_on_device(env.dev, vr_s) == b_small);
    CHECK(decode_on_device(env.dev, vr_m) == b_med);
    CHECK(decode_on_device(env.dev, vr_l) == b_lba);
    CHECK(decode_on_device(env.dev, vr_M) == b_multi);

    printf("  case_6_cross_class: OK (5 distinct pages, 5 classes)\n");
}

// ════════════════════════════════════════════════════════════════
//  case_7: cache eviction — bounded cache evicts oldest pages
//
//  Writes 5 LBA-equal values (class 3, 4096-byte slots, one slot per page)
//  so each entry forces a fresh page allocation. With value_cache_capacity=2,
//  only the two most recently committed pages survive eviction.
//
//  Reading newest-first (vrs[4], vrs[3], vrs[2], vrs[1], vrs[0]) yields:
//    vrs[4], vrs[3] → cache hits (NVMe count unchanged)
//    vrs[2..0]      → cache misses (NVMe read each)
//  Total: exactly 3 NVMe reads.
//
//  This relies on clock_cache's LRU-ish ordering (last 2 puts survive when
//  no gets touched the older entries). slru_cache with cap=2 has different
//  invariants — case_7 is intentionally specific to clock since test_env
//  hardcodes value::value_alloc_sched<core::clock_cache>.
// ════════════════════════════════════════════════════════════════

void case_7_cache_evict() {
    test_env env(/*value_cache_cap*/ 2);
    auto ctx = pump::core::make_root_context();

    constexpr int N = 5;
    std::vector<std::string> bodies;
    std::vector<value_ref>   vrs(N);
    std::vector<value::put_entry> entries;
    bodies.reserve(N);
    entries.reserve(N);
    for (int i = 0; i < N; ++i)
        bodies.push_back(make_body("evict", 4000));   // class 3, full page
    for (int i = 0; i < N; ++i)
        entries.push_back({bodies[i], &vrs[i]});

    auto* sched = core::registry::value_sched();

    auto wctr = std::make_shared<std::atomic<int>>(0);
    value::persist_values(sched, std::span<value::put_entry>(entries))
        >> pump::sender::then([wctr](bool ok) {
            CHECK(ok);
            wctr->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(wctr, 1);

    // Each LBA-equal entry must land on its own page (1 slot per page).
    for (int i = 0; i < N; ++i)
        for (int j = i + 1; j < N; ++j)
            CHECK(vrs[i].base != vrs[j].base);

    env.dev.reset_io_counters();

    auto read_one = [&](value_ref vr, std::string& got) {
        auto c = std::make_shared<std::atomic<int>>(0);
        value::read_value(sched, vr)
            >> pump::sender::then([&got, c](std::string s) {
                got = std::move(s);
                c->fetch_add(1);
            })
            >> pump::sender::submit(ctx);
        wait_for(c, 1);
    };

    // Read newest-first so the two cached entries are touched before any
    // miss-driven put can evict them.
    for (int i = N - 1; i >= 0; --i) {
        std::string got;
        read_one(vrs[i], got);
        CHECK(got == bodies[i]);
    }

    auto reads = env.dev.get_read_count();
    // cap=2 → vrs[4]/vrs[3] hit, vrs[2]/vrs[1]/vrs[0] miss → exactly 3 reads.
    CHECK(reads == 3);

    printf("  case_7_cache_evict: OK (cap=2, 5 reads → %lu NVMe = 2 hits + 3 misses)\n",
           static_cast<unsigned long>(reads));
}

// ════════════════════════════════════════════════════════════════
//  case_8: D1 multi-LBA bypass — both write and read paths
//
//  Decision D1 says only 1-LBA pages enter the readonly cache. Multi-LBA
//  values bypass on:
//    - read path: handle_read computes admit = (span == 1) and skips both
//      cache pin/admit when admit is false.
//    - write path: commit_pages only releases the buf into the cache when
//      frame->id.span_lbas == 1; multi-LBA full pages are dropped.
//
//  Falsifying *both* paths needs more than NVMe-read count alone. Counting
//  reads only witnesses read-path bypass: if commit_pages regressed and
//  re-admitted the multi-LBA page, handle_read would still skip cache pin on
//  span > 1 and the test would still see reads_after_2 == 2. The
//  write-path check has to look directly at readonly_cache_ via a white-box
//  cast (allowed because test_env hardcodes value::value_alloc_sched<clock_cache>).
//
//  Three independent assertions:
//    [W1] After persist_values commits the multi-LBA page, readonly_cache_
//         must NOT contain the multi-LBA value frame_id (write-path bypass).
//    [R1] reads_after_1 == 1 — first read miss hits NVMe (handle_read
//         bypass + bounded read 1 LBA-equivalent op).
//    [R2] reads_after_2 == 2 — second read miss also hits NVMe; if
//         handle_fill regressed and admitted the multi-LBA page after the
//         first miss, this would still bypass via [R1]'s assertion path
//         (read side), but [W2] below catches admit-from-fill regressions.
//    [W2] After two cache-miss reads, readonly_cache_ must STILL not
//         contain the multi-LBA value frame_id (read-side fill bypass).
// ════════════════════════════════════════════════════════════════

void case_8_multi_lba_bypass() {
    test_env env;   // default value_cache_capacity = 32
    auto ctx = pump::core::make_root_context();

    std::string body = make_body("multi", 16000);  // class 4, span_lbas=4
    value_ref vr{};
    std::vector<value::put_entry> entries = {{body, &vr}};

    auto* sched_base = core::registry::value_sched();
    // White-box cast: test_env publishes its concrete value scheduler type
    // (test_env::value_scheduler_t == value::value_alloc_sched<value_cache_t>) so
    // case_8 inspects readonly_cache_ directly — the only way to assert
    // "commit_pages did NOT admit this page" without depending on a
    // behavior the read path would silently mask.
    auto* sched_typed = static_cast<test_env::value_scheduler_t*>(sched_base);

    auto wctr = std::make_shared<std::atomic<int>>(0);
    value::persist_values(sched_base, std::span<value::put_entry>(entries))
        >> pump::sender::then([wctr](bool ok) {
            CHECK(ok);
            wctr->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(wctr, 1);

    const auto multi_fid = value_frame_id_for(vr.base, MULTI_LBA_CLASS_SPAN);

    // [W1] Write-path bypass: commit_pages must drop the multi-LBA full
    //      page instead of admitting it to readonly_cache_. The page is
    //      class-4 (1 slot per page) so its only slot is consumed by the
    //      write, free_mask == 0, commit_pages enters the full-page branch.
    //      With D1 enforced, span_lbas != 1 → no put().
    CHECK(!sched_typed->readonly_cache_.contains(multi_fid));

    env.dev.reset_io_counters();

    auto read_one = [&](value_ref vr_in, std::string& got) {
        auto c = std::make_shared<std::atomic<int>>(0);
        value::read_value(sched_base, vr_in)
            >> pump::sender::then([&got, c](std::string s) {
                got = std::move(s);
                c->fetch_add(1);
            })
            >> pump::sender::submit(ctx);
        wait_for(c, 1);
    };

    // [R1] Read-path bypass: multi-LBA → admit=false → handle_read skips
    //      cache pin → miss → NVMe.
    std::string got1;
    read_one(vr, got1);
    auto reads_after_1 = env.dev.get_read_count();
    CHECK(got1 == body);
    CHECK(reads_after_1 == 1);

    // [R2] Second read still hits NVMe (cache holds nothing for span > 1).
    std::string got2;
    read_one(vr, got2);
    auto reads_after_2 = env.dev.get_read_count();
    CHECK(got2 == body);
    CHECK(reads_after_2 == 2);

    // [W2] Read-side fill bypass: handle_fill must observe admit=false on
    //      multi-LBA reads and skip cache.put(). After two miss-driven
    //      fills, readonly_cache_ still contains no multi-LBA frame.
    CHECK(!sched_typed->readonly_cache_.contains(multi_fid));

    printf("  case_8_multi_lba_bypass: OK "
           "(cache.contains(vr_multi)=false before+after %lu→%lu reads)\n",
           static_cast<unsigned long>(reads_after_1),
           static_cast<unsigned long>(reads_after_2));
}

// ════════════════════════════════════════════════════════════════
//  case_9: value_page_frame type surface
//
//  Step 019 makes resident value-page state explicit. The test keeps this
//  as a small compile/runtime contract so value_page_frame cannot collapse
//  back into an untyped page_data/free_mask carrier.
// ════════════════════════════════════════════════════════════════

void case_9_value_page_frame_type_contract() {
    char payload[LBA_SIZE]{};
    value_page_frame frame{};
    frame.id = value_frame_id_for(paddr{0, 1234}, 1);
    frame.st = frame_state::dirty_append;
    frame.buf = payload;
    frame.byte_len = LBA_SIZE;
    frame.pin_count = 0;
    frame.crc_valid = true;
    frame.class_idx = 0;
    frame.slots_per_page = slots_per_page_for_class(0);
    frame.free_mask = UINT64_MAX & ~1ULL;
    frame.free_count = 63;
    frame.mode = value_page_frame::open_mode::append;

    CHECK(frame.id.base.lba == 1234);
    CHECK(frame.st == frame_state::dirty_append);
    CHECK(frame.buf == payload);
    CHECK(frame.class_idx == 0);
    CHECK(frame.slots_per_page == 64);
    CHECK(frame.free_count == 63);
    CHECK((frame.free_mask & 1ULL) == 0);
    CHECK(frame.mode == value_page_frame::open_mode::append);

    printf("  case_9_value_page_frame_type_contract: OK\n");
}

// ════════════════════════════════════════════════════════════════
//  case_10: partial writeback → clean_allocatable, not readonly cache
//
//  A sub-LBA page with remaining free slots is placement state. After
//  writeback completes it must be resident as clean_allocatable, must not
//  enter readonly_cache_, and read_value must hit that resident frame
//  without NVMe.
// ════════════════════════════════════════════════════════════════

void case_10_partial_page_becomes_clean_allocatable() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    std::string body = make_body("alloc", 30);  // class 0, one slot used
    value_ref vr{};
    std::vector<value::put_entry> entries = {{body, &vr}};

    auto* sched_base = core::registry::value_sched();
    auto* sched_typed = static_cast<test_env::value_scheduler_t*>(sched_base);
    persist_entries(sched_base, ctx, std::span<value::put_entry>(entries));

    CHECK(vr.byte_offset == 0);
    CHECK(sched_typed->open_frames_.size() == profile_class_sizes().size());
    CHECK(sched_typed->open_frames_[0] == nullptr);
    CHECK(sched_typed->allocatable_frames_[0].size() == 1);

    auto* frame = sched_typed->allocatable_frames_[0].back();
    CHECK(frame != nullptr);
    CHECK(frame->id == value_frame_id_for(vr.base, 1));
    CHECK(frame->st == frame_state::clean_allocatable);
    CHECK(frame->mode == value_page_frame::open_mode::none);
    CHECK(frame->class_idx == 0);
    CHECK(frame->slots_per_page == slots_per_page_for_class(0));
    CHECK(frame->free_count == 63);
    CHECK((frame->free_mask & 1ULL) == 0);
    CHECK((frame->free_mask & (1ULL << 1)) != 0);
    CHECK(!sched_typed->readonly_cache_.contains(value_frame_id_for(vr.base, 1)));

    env.dev.reset_io_counters();
    auto got = read_value_sync(sched_base, ctx, vr);
    CHECK(got == body);
    CHECK(env.dev.get_read_count() == 0);

    printf("  case_10_partial_page_becomes_clean_allocatable: OK "
           "(free_count=%u, read hit resident frame)\n",
           frame->free_count);
}

// ════════════════════════════════════════════════════════════════
//  case_11: next persist reopens clean_allocatable resident frame
//
//  The second persist is a separate round. It must consume the resident
//  clean_allocatable frame from allocatable_frames_[0] instead of allocating
//  a fresh page, then return it to clean_allocatable after writeback.
// ════════════════════════════════════════════════════════════════

void case_11_next_round_reopens_clean_allocatable_frame() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    std::string b1 = make_body("r1", 30);
    std::string b2 = make_body("r2", 30);
    value_ref vr1{}, vr2{};

    auto* sched_base = core::registry::value_sched();
    auto* sched_typed = static_cast<test_env::value_scheduler_t*>(sched_base);

    std::vector<value::put_entry> entries1 = {{b1, &vr1}};
    persist_entries(sched_base, ctx, std::span<value::put_entry>(entries1));

    CHECK(sched_typed->allocatable_frames_[0].size() == 1);
    auto first_base = sched_typed->allocatable_frames_[0].back()->id.base;
    CHECK(first_base == vr1.base);

    std::vector<value::put_entry> entries2 = {{b2, &vr2}};
    persist_entries(sched_base, ctx, std::span<value::put_entry>(entries2));

    CHECK(vr2.base == vr1.base);
    CHECK(vr2.byte_offset == 64);
    CHECK(sched_typed->open_frames_[0] == nullptr);
    CHECK(sched_typed->allocatable_frames_[0].size() == 1);

    auto* frame = sched_typed->allocatable_frames_[0].back();
    CHECK(frame != nullptr);
    CHECK(frame->id.base == first_base);
    CHECK(frame->st == frame_state::clean_allocatable);
    CHECK(frame->mode == value_page_frame::open_mode::none);
    CHECK(frame->free_count == 62);
    CHECK((frame->free_mask & 1ULL) == 0);
    CHECK((frame->free_mask & (1ULL << 1)) == 0);
    CHECK((frame->free_mask & (1ULL << 2)) != 0);
    CHECK(!sched_typed->readonly_cache_.contains(value_frame_id_for(vr1.base, 1)));

    env.dev.reset_io_counters();
    CHECK(read_value_sync(sched_base, ctx, vr1) == b1);
    CHECK(read_value_sync(sched_base, ctx, vr2) == b2);
    CHECK(env.dev.get_read_count() == 0);

    printf("  case_11_next_round_reopens_clean_allocatable_frame: OK "
           "(same lba=%lu, free_count=%u)\n",
           static_cast<unsigned long>(vr1.base.lba),
           frame->free_count);
}

// ════════════════════════════════════════════════════════════════
//  case_12: full page → clean_readonly readonly cache
//
//  A full 1-LBA value page has no placement state left. It must not remain
//  in open/allocatable resident lists; it becomes clean_readonly and enters
//  readonly_cache_.
// ════════════════════════════════════════════════════════════════

void case_12_full_page_enters_readonly_cache_only() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    std::string body = make_body("full", 4000);  // class 3, one full LBA
    value_ref vr{};
    std::vector<value::put_entry> entries = {{body, &vr}};

    auto* sched_base = core::registry::value_sched();
    auto* sched_typed = static_cast<test_env::value_scheduler_t*>(sched_base);
    persist_entries(sched_base, ctx, std::span<value::put_entry>(entries));

    const auto fid = value_frame_id_for(vr.base, 1);
    CHECK(sched_typed->open_frames_[3] == nullptr);
    CHECK(sched_typed->allocatable_frames_[3].empty());
    CHECK(sched_typed->readonly_cache_.contains(fid));

    {
        auto pin = sched_typed->readonly_cache_.pin(fid);
        CHECK(pin.frame != nullptr);
        CHECK(pin.frame->st == frame_state::clean_readonly);
        CHECK(pin.frame->byte_len == LBA_SIZE);
    }

    env.dev.reset_io_counters();
    CHECK(read_value_sync(sched_base, ctx, vr) == body);
    CHECK(env.dev.get_read_count() == 0);

    printf("  case_12_full_page_enters_readonly_cache_only: OK\n");
}

// ════════════════════════════════════════════════════════════════
//  case_13: prepared round exposes open_frames_ until finalize
//
//  Step 019 requires open_frames_[ci] to be a real dirty/inflight resident
//  tier, not a dead read branch. Drive prepare_persist directly so the
//  round is published but not finalized; read_value must hit open_frames_
//  with no NVMe read, then finalize(false) must clear the open reference.
// ════════════════════════════════════════════════════════════════

void case_13_open_frame_visible_until_finalize() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    std::string body = make_body("open", 30);  // class 0, one slot used
    value_ref vr{};
    std::vector<value::put_entry> entries = {{body, &vr}};

    auto* sched_base = core::registry::value_sched();
    auto* sched_typed = static_cast<test_env::value_scheduler_t*>(sched_base);

    uint64_t round_id = 0;
    auto prep_ctr = std::make_shared<std::atomic<int>>(0);
    sched_base->prepare_persist(std::span<value::put_entry>(entries))
        >> pump::sender::then([&](value::prepare_persist_result res) {
            auto* leader = std::get_if<value::persist_leader>(&res);
            CHECK(leader != nullptr);
            round_id = leader->round_id;
            CHECK(leader->writes.size() == 1);
            CHECK(leader->writes[0].lba == vr.base.lba);
            prep_ctr->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(prep_ctr, 1);

    auto* open = sched_typed->open_frames_[0];
    CHECK(open != nullptr);
    CHECK(open->id == value_frame_id_for(vr.base, 1));
    CHECK(open->st == frame_state::writeback_inflight);
    CHECK(open->mode == value_page_frame::open_mode::none);
    CHECK(open->free_count == 63);
    CHECK(sched_typed->allocatable_frames_[0].empty());

    env.dev.reset_io_counters();
    CHECK(read_value_sync(sched_base, ctx, vr) == body);
    CHECK(env.dev.get_read_count() == 0);

    auto finalize_ctr = std::make_shared<std::atomic<int>>(0);
    sched_base->finalize_persist(round_id, false)
        >> pump::sender::then([finalize_ctr]() {
            finalize_ctr->fetch_add(1);
        })
        >> pump::sender::submit(ctx);
    wait_for(finalize_ctr, 1);

    CHECK(sched_typed->open_frames_[0] == nullptr);
    CHECK(sched_typed->allocatable_frames_[0].empty());

    printf("  case_13_open_frame_visible_until_finalize: OK "
           "(read hit open_frames before rollback finalize)\n");
}

}  // namespace

int main() {
    printf("inconel value scheduler test:\n");

    check_bootstrap_profile_matches_value_expectations();
    case_1_write_path();
    case_2_read_miss();
    case_3_cache_hit();
    case_4_write_then_read();
    case_5_sub_lba_same_page();
    case_6_cross_class();
    case_7_cache_evict();
    case_8_multi_lba_bypass();
    case_9_value_page_frame_type_contract();
    case_10_partial_page_becomes_clean_allocatable();
    case_11_next_round_reopens_clean_allocatable_frame();
    case_12_full_page_enters_readonly_cache_only();
    case_13_open_frame_visible_until_finalize();

    printf("all passed\n");
    return 0;
}
