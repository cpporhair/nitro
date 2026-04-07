//
// page cache unit tests — clock + slru
//
// Verifies cache_concept compliance, basic put/get/contains, eviction
// behavior, hot-page protection, and SLRU scan resistance.
//

#include "apps/inconel/test/check.hh"
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
    // Capacity 20: leaves SLRU prob_cap = 20*0.2 = 4, enough to hold 4 fresh
    // entries before any of them is promoted to protected. Both clock_cache
    // and slru_cache use the same single-arg constructor, so the test stays
    // template-uniform without needing per-cache helpers.
    Cache c(20);

    CHECK(!c.contains(P(1)));
    CHECK(c.get(P(1)) == nullptr);
    CHECK(c.size() == 0);

    auto e = c.put(P(1), B(1));
    CHECK(!e.has_value());
    CHECK(c.size() == 1);
    CHECK(c.contains(P(1)));
    CHECK(c.get(P(1)) == B(1));

    // For SLRU, the get() above promotes P1 to protected. P2..P4 then go to
    // probation; with prob_cap=4 there is exactly enough room for the three
    // fresh entries, so total size = 1 (protected) + 3 (probation) = 4.
    c.put(P(2), B(2));
    c.put(P(3), B(3));
    c.put(P(4), B(4));
    CHECK(c.size() == 4);
    CHECK(c.get(P(2)) == B(2));
    CHECK(c.get(P(3)) == B(3));
    CHECK(c.get(P(4)) == B(4));

    printf("  [%s] basic: OK\n", label);
}

// Capacity eviction tests are split per cache type because LRU (clock) and
// SLRU have fundamentally different "what counts as full" semantics:
//
//   clock(N): fills all N slots equally; (N+1)-th fresh put evicts the
//             clock victim.
//   slru(N) :  fresh entries only enter probation, whose capacity is roughly
//             20% of N. The (prob_cap + 1)-th fresh put evicts probation tail.
//
// A single template can't capture both correctly without per-cache constants.

static void test_clock_capacity_eviction() {
    clock_cache c(3);
    c.put(P(1), B(1));
    c.put(P(2), B(2));
    c.put(P(3), B(3));
    CHECK(c.size() == 3);

    auto e = c.put(P(4), B(4));
    CHECK(e.has_value());
    CHECK(c.size() == 3);
    CHECK(c.contains(P(4)));

    printf("  [clock] capacity eviction: OK\n");
}

static void test_slru_capacity_eviction() {
    // capacity 10 → prot_cap=8, prob_cap=2.
    // Two fresh puts fill probation; the third evicts probation tail.
    slru_cache c(10);
    c.put(P(1), B(1));
    c.put(P(2), B(2));
    CHECK(c.size() == 2);

    auto e = c.put(P(3), B(3));
    CHECK(e.has_value());
    CHECK(c.size() == 2);            // prob still saturated
    CHECK(c.contains(P(3)));         // newest entry stays
    CHECK(!c.contains(P(1)));        // oldest probation entry evicted

    printf("  [slru ] capacity eviction: OK\n");
}

template <cache_concept Cache>
static void test_update_existing(const char* label) {
    Cache c(3);
    c.put(P(1), B(1));
    auto e = c.put(P(1), B(11));   // update — no eviction
    CHECK(!e.has_value());
    CHECK(c.size() == 1);
    CHECK(c.get(P(1)) == B(11));
    printf("  [%s] update existing: OK\n", label);
}

static void test_clock_eviction_returns_buf() {
    clock_cache c(2);
    c.put(P(1), B(1));
    c.put(P(2), B(2));
    auto e = c.put(P(3), B(3));
    CHECK(e.has_value());
    CHECK(e->buf == B(1) || e->buf == B(2));
    printf("  [clock] eviction returns buf: OK\n");
}

static void test_slru_eviction_returns_buf() {
    // capacity 10 → prot_cap=8, prob_cap=2.
    // Probation evicts in LRU order, so the third fresh put returns P1's buf.
    slru_cache c(10);
    c.put(P(1), B(1));
    c.put(P(2), B(2));
    auto e = c.put(P(3), B(3));
    CHECK(e.has_value());
    CHECK(e->buf == B(1));
    printf("  [slru ] eviction returns buf: OK\n");
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

    CHECK(c.contains(P(1)));
    CHECK(c.get(P(1)) == B(1));
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

    CHECK(!c.contains(P(1)));   // probation-only → evicted
    CHECK(c.contains(P(2)));    // promoted → still present

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

    CHECK(evicted_hot == 0);
    for (int i = 0; i < 8; ++i)
        CHECK(c.contains(P(i)));

    printf("  [slru] scan resistance (0 hot pages evicted by 100 scans): OK\n");
}

int main() {
    printf("page cache tests:\n");

    // ── Generic interface tests (both cache types) ──
    test_basic<clock_cache>("clock");
    test_basic<slru_cache>("slru ");
    test_update_existing<clock_cache>("clock");
    test_update_existing<slru_cache>("slru ");

    // ── Capacity-eviction tests (per-cache because LRU/SLRU differ) ──
    test_clock_capacity_eviction();
    test_slru_capacity_eviction();
    test_clock_eviction_returns_buf();
    test_slru_eviction_returns_buf();

    // ── Algorithm-specific behavior tests ──
    test_clock_hot_page();
    test_slru_promotion();
    test_slru_scan_resistance();

    printf("all passed\n");
    return 0;
}
