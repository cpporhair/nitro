//
// page cache unit tests — clock + slru
//
// Verifies cache_concept compliance, basic put/get/contains, eviction
// behavior, hot-page protection, and SLRU scan resistance.
//

#include <cassert>
#include <cstdio>
#include <vector>

#include "apps/inconel/core/page_cache.hh"

using namespace apps::inconel::core;
using namespace apps::inconel::format;

// Compile-time check: both impls satisfy the concept.
static_assert(cache_concept<clock_cache>);
static_assert(cache_concept<slru_cache>);

// ── Helpers ──

static paddr P(uint64_t lba) { return paddr{0, lba}; }

// Buffer pool — each char buffer is just a 1-byte marker.
static std::vector<char> g_bufs(8192);
static char* B(uint64_t i) { return &g_bufs[i]; }

// ── Generic API tests (templated on cache type) ──

template <cache_concept Cache>
static void test_basic(const char* label) {
    Cache c(4);

    assert(!c.contains(P(1)));
    assert(c.get(P(1)) == nullptr);
    assert(c.size() == 0);

    auto e = c.put(P(1), B(1));
    assert(!e.has_value());
    assert(c.size() == 1);
    assert(c.contains(P(1)));
    assert(c.get(P(1)) == B(1));

    c.put(P(2), B(2));
    c.put(P(3), B(3));
    c.put(P(4), B(4));
    assert(c.size() == 4);
    assert(c.get(P(2)) == B(2));
    assert(c.get(P(3)) == B(3));
    assert(c.get(P(4)) == B(4));

    printf("  [%s] basic: OK\n", label);
}

template <cache_concept Cache>
static void test_capacity_eviction(const char* label) {
    Cache c(3);
    c.put(P(1), B(1));
    c.put(P(2), B(2));
    c.put(P(3), B(3));
    assert(c.size() == 3);

    auto e = c.put(P(4), B(4));
    assert(e.has_value());
    assert(c.size() == 3);
    assert(c.contains(P(4)));

    printf("  [%s] capacity eviction: OK\n", label);
}

template <cache_concept Cache>
static void test_update_existing(const char* label) {
    Cache c(3);
    c.put(P(1), B(1));
    auto e = c.put(P(1), B(11));   // update — no eviction
    assert(!e.has_value());
    assert(c.size() == 1);
    assert(c.get(P(1)) == B(11));
    printf("  [%s] update existing: OK\n", label);
}

template <cache_concept Cache>
static void test_eviction_returns_buf(const char* label) {
    Cache c(2);
    c.put(P(1), B(1));
    c.put(P(2), B(2));
    auto e = c.put(P(3), B(3));
    assert(e.has_value());
    assert(e->buf == B(1) || e->buf == B(2));
    printf("  [%s] eviction returns buf: OK\n", label);
}

// ── Clock-specific: hot page survives sweeps ──

static void test_clock_hot_page() {
    clock_cache c(3);
    c.put(P(1), B(1));   // will be the "hot" page
    c.put(P(2), B(2));
    c.put(P(3), B(3));

    for (int round = 0; round < 5; ++round) {
        c.get(P(1));   // refresh ref bit
        c.put(P(100 + round), B(100 + round));  // cold insert
    }

    assert(c.contains(P(1)));
    assert(c.get(P(1)) == B(1));
    printf("  [clock] hot page survives sweeps: OK\n");
}

// ── SLRU-specific: probation → protected promotion ──

static void test_slru_promotion() {
    slru_cache c(10);
    // P(1) inserted but never accessed → stays in probation
    c.put(P(1), B(1));
    // P(2) inserted and accessed → promoted to protected
    c.put(P(2), B(2));
    c.get(P(2));

    // Scan to evict probation entries
    for (int i = 0; i < 50; ++i) c.put(P(1000 + i), B(1000 + i));

    assert(!c.contains(P(1)));   // probation-only → evicted
    assert(c.contains(P(2)));    // promoted → still present

    printf("  [slru] promotion to protected: OK\n");
}

// ── SLRU-specific: scan resistance ──

static void test_slru_scan_resistance() {
    // capacity 10 → protected_cap=8, probation_cap=2
    slru_cache c(10);

    // Insert 8 hot pages and immediately access them → all promoted to protected.
    for (int i = 0; i < 8; ++i) {
        c.put(P(i), B(i));
        c.get(P(i));
    }

    // Scan: insert 100 cold pages.
    int evicted_hot = 0;
    for (int i = 0; i < 100; ++i) {
        auto e = c.put(P(1000 + i), B(1000 + i));
        if (e.has_value() && e->key.lba < 8) evicted_hot++;
    }

    assert(evicted_hot == 0);
    for (int i = 0; i < 8; ++i)
        assert(c.contains(P(i)));

    printf("  [slru] scan resistance (0 hot pages evicted by 100 scans): OK\n");
}

int main() {
    printf("page cache tests:\n");

    test_basic<clock_cache>("clock");
    test_basic<slru_cache>("slru ");
    test_capacity_eviction<clock_cache>("clock");
    test_capacity_eviction<slru_cache>("slru ");
    test_update_existing<clock_cache>("clock");
    test_update_existing<slru_cache>("slru ");
    test_eviction_returns_buf<clock_cache>("clock");
    test_eviction_returns_buf<slru_cache>("slru ");

    test_clock_hot_page();
    test_slru_promotion();
    test_slru_scan_resistance();

    printf("all passed\n");
    return 0;
}
