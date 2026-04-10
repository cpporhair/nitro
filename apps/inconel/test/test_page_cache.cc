//
// read-only frame cache unit tests — clock + slru
//
// Reviewer-owned step 018 tests. These intentionally lock the frame-cache
// contract before production code is migrated away from raw char* entries.
//

#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "apps/inconel/core/page_cache.hh"
#include "apps/inconel/memory/frame.hh"

using namespace apps::inconel::core;
using namespace apps::inconel::format;
using namespace apps::inconel::memory;

static_assert(cache_concept<clock_cache>);
static_assert(cache_concept<slru_cache>);

namespace {

using domain = frame_id::domain;

static paddr P(uint64_t lba) {
    return paddr{0, lba};
}

static frame_id FID(uint64_t lba,
                    domain dom = domain::value_page,
                    uint16_t span_lbas = 1) {
    return frame_id{
        .base = P(lba),
        .span_lbas = span_lbas,
        .dom = dom,
    };
}

// Payload bytes are not owned by the cache in this phase; descriptors point
// into this stable test pool so eviction can be tested without allocation.
static std::array<std::array<char, 16>, 8192> g_bufs{};

static char* B(uint64_t i) {
    return g_bufs.at(i).data();
}

static page_frame Frame(uint64_t lba,
                        frame_state st = frame_state::clean_readonly,
                        domain dom = domain::value_page,
                        uint16_t span_lbas = 1,
                        uint64_t buf_idx = UINT64_MAX) {
    if (buf_idx == UINT64_MAX) buf_idx = lba;
    return page_frame{
        .id = FID(lba, dom, span_lbas),
        .st = st,
        .buf = B(buf_idx),
        .byte_len = static_cast<uint32_t>(4096U * span_lbas),
        .pin_count = 0,
        .crc_valid = true,
    };
}

static bool has_frame(const std::vector<page_frame*>& xs, page_frame* f) {
    return std::find(xs.begin(), xs.end(), f) != xs.end();
}

template <cache_concept Cache>
static void test_basic_pin(const char* label) {
    Cache c(20);
    auto f = Frame(1);

    CHECK(c.size() == 0);
    CHECK(!c.contains(f.id));
    {
        auto miss = c.pin(f.id);
        CHECK(miss.frame == nullptr);
    }
    CHECK(f.pin_count == 0);

    auto ev = c.put(&f);
    CHECK(!ev.has_value());
    CHECK(c.size() == 1);
    CHECK(c.contains(f.id));

    {
        auto pin = c.pin(f.id);
        CHECK(pin.frame == &f);
        CHECK(pin.frame->buf == f.buf);
        CHECK(f.pin_count == 1);
    }
    CHECK(f.pin_count == 0);

    printf("  [%s] basic pin: OK\n", label);
}

template <cache_concept Cache>
static void test_put_rejects_non_clean_readonly(const char* label) {
    Cache c(20);
    auto dirty = Frame(2, frame_state::dirty_append);

    bool threw = false;
    try {
        (void)c.put(&dirty);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    CHECK(threw);
    CHECK(c.size() == 0);
    CHECK(!c.contains(dirty.id));

    printf("  [%s] put rejects non-clean-readonly: OK\n", label);
}

template <cache_concept Cache>
static void test_update_existing_returns_old_frame(const char* label) {
    Cache c(20);
    auto old_frame = Frame(42, frame_state::clean_readonly,
                           domain::value_page, 1, 42);
    auto new_frame = Frame(42, frame_state::clean_readonly,
                           domain::value_page, 1, 1042);

    auto e1 = c.put(&old_frame);
    CHECK(!e1.has_value());

    auto e2 = c.put(&new_frame);
    CHECK(e2.has_value());
    CHECK(*e2 == &old_frame);
    CHECK(c.size() == 1);

    {
        auto pin = c.pin(old_frame.id);
        CHECK(pin.frame == &new_frame);
        CHECK(pin.frame->buf == B(1042));
    }

    printf("  [%s] update existing returns old frame: OK\n", label);
}

template <cache_concept Cache>
static void test_update_existing_rejects_when_old_frame_pinned(const char* label) {
    Cache c(20);
    auto old_frame = Frame(43, frame_state::clean_readonly,
                           domain::value_page, 1, 43);
    auto replacement = Frame(43, frame_state::clean_readonly,
                             domain::value_page, 1, 1043);

    CHECK(!c.put(&old_frame).has_value());

    {
        auto pin = c.pin(old_frame.id);
        CHECK(pin.frame == &old_frame);
        CHECK(old_frame.pin_count == 1);

        auto rejected = c.put(&replacement);
        CHECK(rejected.has_value());
        CHECK(*rejected == &replacement);
        CHECK(c.size() == 1);

        auto still_old = c.pin(old_frame.id);
        CHECK(still_old.frame == &old_frame);
        CHECK(old_frame.pin_count == 2);
        CHECK(replacement.pin_count == 0);
    }
    CHECK(old_frame.pin_count == 0);

    {
        auto pin = c.pin(old_frame.id);
        CHECK(pin.frame == &old_frame);
    }

    printf("  [%s] update existing rejects pinned old frame: OK\n", label);
}

template <cache_concept Cache>
static void test_frame_id_domain_and_span_are_key_parts(const char* label) {
    Cache c(20);

    auto tree = Frame(7, frame_state::clean_readonly,
                      domain::tree_node, 4, 100);
    auto value = Frame(7, frame_state::clean_readonly,
                       domain::value_page, 1, 101);
    auto value_span = Frame(7, frame_state::clean_readonly,
                            domain::value_page, 4, 102);

    CHECK(!c.put(&tree).has_value());
    CHECK(!c.put(&value).has_value());
    CHECK(!c.put(&value_span).has_value());
    CHECK(c.size() == 3);

    {
        auto pin_tree = c.pin(tree.id);
        auto pin_value = c.pin(value.id);
        auto pin_value_span = c.pin(value_span.id);
        CHECK(pin_tree.frame == &tree);
        CHECK(pin_value.frame == &value);
        CHECK(pin_value_span.frame == &value_span);
    }

    printf("  [%s] frame_id domain/span keys: OK\n", label);
}

static void test_clock_skips_pinned_on_runtime_eviction() {
    clock_cache c(2);
    auto f1 = Frame(10);
    auto f2 = Frame(11);
    auto f3 = Frame(12);

    CHECK(!c.put(&f1).has_value());
    CHECK(!c.put(&f2).has_value());

    {
        auto pin = c.pin(f1.id);
        CHECK(pin.frame == &f1);
        auto ev = c.put(&f3);
        CHECK(ev.has_value());
        CHECK(*ev == &f2);
        CHECK(c.contains(f1.id));
        CHECK(!c.contains(f2.id));
        CHECK(c.contains(f3.id));
        CHECK(f1.pin_count == 1);
    }
    CHECK(f1.pin_count == 0);

    printf("  [clock] runtime eviction skips pinned: OK\n");
}

static void test_slru_skips_pinned_on_runtime_eviction() {
    // capacity 10 -> probation capacity 2. Keep f1 in probation by setting
    // pin_count directly; a pin() hit would be free to update SLRU policy and
    // would not necessarily force an eviction on the next fresh insert.
    slru_cache c(10);
    auto f1 = Frame(20);
    auto f2 = Frame(21);
    auto f3 = Frame(22);

    CHECK(!c.put(&f1).has_value());
    CHECK(!c.put(&f2).has_value());
    f1.pin_count = 1;

    auto ev = c.put(&f3);
    CHECK(ev.has_value());
    CHECK(*ev == &f2);
    CHECK(c.contains(f1.id));
    CHECK(!c.contains(f2.id));
    CHECK(c.contains(f3.id));
    f1.pin_count = 0;

    printf("  [slru ] runtime eviction skips pinned: OK\n");
}

template <cache_concept Cache>
static void test_drain_one_teardown_only(const char* label) {
    Cache c(20);
    auto f1 = Frame(30);
    auto f2 = Frame(31);
    auto f3 = Frame(32);

    CHECK(!c.put(&f1).has_value());
    CHECK(!c.put(&f2).has_value());
    CHECK(!c.put(&f3).has_value());

    std::vector<page_frame*> drained;
    while (auto f = c.drain_one()) {
        drained.push_back(*f);
    }

    CHECK(c.size() == 0);
    CHECK(drained.size() == 3);
    CHECK(has_frame(drained, &f1));
    CHECK(has_frame(drained, &f2));
    CHECK(has_frame(drained, &f3));
    CHECK(!c.drain_one().has_value());

    // No assertion is made about drain order. The API is a teardown drain,
    // not "choose the policy victim now".
    printf("  [%s] drain_one teardown pop: OK\n", label);
}

static void test_clock_capacity_zero_throws() {
    bool threw = false;
    try {
        clock_cache c(0);
        (void)c;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    printf("  [clock] capacity=0 throws: OK\n");
}

static void test_slru_capacity_zero_throws() {
    bool threw = false;
    try {
        slru_cache c(0);
        (void)c;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    printf("  [slru ] capacity=0 throws: OK\n");
}

static void test_slru_capacity_one_throws() {
    bool threw = false;
    try {
        slru_cache c(1);
        (void)c;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    printf("  [slru ] capacity=1 throws: OK\n");
}

static void test_slru_extreme_ratio_throws() {
    bool threw_low = false;
    try {
        slru_cache c(8, 0.0f);
        (void)c;
    } catch (const std::invalid_argument&) {
        threw_low = true;
    }
    CHECK(threw_low);

    bool threw_high = false;
    try {
        slru_cache c(8, 1.0f);
        (void)c;
    } catch (const std::invalid_argument&) {
        threw_high = true;
    }
    CHECK(threw_high);

    printf("  [slru ] extreme prot_ratio throws: OK\n");
}

static void test_clock_min_capacity_works() {
    clock_cache c(1);
    auto f1 = Frame(40);
    auto f2 = Frame(41);

    CHECK(c.capacity() == 1);
    CHECK(!c.put(&f1).has_value());

    {
        auto pin = c.pin(f1.id);
        CHECK(pin.frame == &f1);
    }

    auto ev = c.put(&f2);
    CHECK(ev.has_value());
    CHECK(*ev == &f1);
    CHECK(!c.contains(f1.id));
    CHECK(c.contains(f2.id));

    auto drained = c.drain_one();
    CHECK(drained.has_value());
    CHECK(*drained == &f2);
    CHECK(!c.drain_one().has_value());

    printf("  [clock] capacity=1 works: OK\n");
}

static void test_slru_min_capacity_works() {
    slru_cache c(2);
    auto f1 = Frame(50);
    auto f2 = Frame(51);
    auto f3 = Frame(52);

    CHECK(c.capacity() == 2);
    CHECK(!c.put(&f1).has_value());
    {
        auto pin = c.pin(f1.id);
        CHECK(pin.frame == &f1);
    }

    CHECK(!c.put(&f2).has_value());
    auto ev = c.put(&f3);
    CHECK(ev.has_value());
    CHECK(*ev == &f2);
    CHECK(c.contains(f1.id));
    CHECK(!c.contains(f2.id));
    CHECK(c.contains(f3.id));

    printf("  [slru ] capacity=2 works: OK\n");
}

}  // namespace

int main() {
    printf("read-only frame cache tests:\n");

    test_basic_pin<clock_cache>("clock");
    test_basic_pin<slru_cache>("slru ");

    test_put_rejects_non_clean_readonly<clock_cache>("clock");
    test_put_rejects_non_clean_readonly<slru_cache>("slru ");

    test_update_existing_returns_old_frame<clock_cache>("clock");
    test_update_existing_returns_old_frame<slru_cache>("slru ");
    test_update_existing_rejects_when_old_frame_pinned<clock_cache>("clock");
    test_update_existing_rejects_when_old_frame_pinned<slru_cache>("slru ");

    test_frame_id_domain_and_span_are_key_parts<clock_cache>("clock");
    test_frame_id_domain_and_span_are_key_parts<slru_cache>("slru ");

    test_clock_skips_pinned_on_runtime_eviction();
    test_slru_skips_pinned_on_runtime_eviction();

    test_drain_one_teardown_only<clock_cache>("clock");
    test_drain_one_teardown_only<slru_cache>("slru ");

    test_clock_capacity_zero_throws();
    test_slru_capacity_zero_throws();
    test_slru_capacity_one_throws();
    test_slru_extreme_ratio_throws();
    test_clock_min_capacity_works();
    test_slru_min_capacity_works();

    printf("all passed\n");
    return 0;
}
