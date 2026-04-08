//
// page cache unit tests — clock + slru
//
// Verifies cache_concept compliance, basic put/get/contains, eviction
// behavior, hot-page protection, and SLRU scan resistance.
//

#include "apps/inconel/test/check.hh"
#include <algorithm>
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
    auto e1 = c.put(P(1), B(1));
    CHECK(!e1.has_value());                  // first insert: nothing displaced

    // Same-key put MUST return the old buf as an "evicted_entry" so the
    // caller can free it. Earlier the cache silently overwrote and leaked
    // every superseded buf — this came up in inconel value::scheduler when
    // two concurrent reads on the same paddr both reach handle_fill in one
    // advance round, calling put(P, buf1) then put(P, buf2).
    auto e2 = c.put(P(1), B(11));
    CHECK(e2.has_value());
    CHECK(e2->key == P(1));
    CHECK(e2->buf == B(1));                  // old buf returned to caller
    CHECK(c.size() == 1);
    CHECK(c.get(P(1)) == B(11));             // cache now holds the new buf

    printf("  [%s] update existing returns old: OK\n", label);
}

// Reject obviously broken capacities at construction. The cache impls
// previously took any uint32_t and crashed on the first put()/get() for
// capacity 0 (clock_cache, slru_cache) and capacity 1 (slru_cache),
// which made it possible for a runtime config typo to bring the whole
// scheduler down. Both impls now throw std::invalid_argument from the
// ctor — verify each rejected boundary explicitly.

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
    // capacity=1 → prot_cap = floor(1 * 0.8) = 0, prob_cap = 1.
    // Pre-fix the first get() would try to demote prot_tail_=NIL.
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
    // prot_ratio=0 → prot_cap=0 → reject.
    bool threw_low = false;
    try {
        slru_cache c(8, 0.0f);
        (void)c;
    } catch (const std::invalid_argument&) {
        threw_low = true;
    }
    CHECK(threw_low);

    // prot_ratio=1.0 → prob_cap=0 → reject.
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

// Smallest legal capacity for each impl must still drive a full
// put/get/evict cycle without hitting the fixed boundaries.

static void test_clock_min_capacity_works() {
    clock_cache c(1);                         // smallest legal
    CHECK(c.capacity() == 1);

    auto e1 = c.put(P(1), B(1));
    CHECK(!e1.has_value());
    CHECK(c.size() == 1);
    CHECK(c.get(P(1)) == B(1));

    auto e2 = c.put(P(2), B(2));              // evicts the only slot
    CHECK(e2.has_value());
    CHECK(e2->buf == B(1));
    CHECK(c.get(P(2)) == B(2));
    CHECK(c.get(P(1)) == nullptr);

    auto e3 = c.evict_one();                  // drain
    CHECK(e3.has_value());
    CHECK(e3->buf == B(2));
    CHECK(c.size() == 0);

    printf("  [clock] capacity=1 works: OK\n");
}

static void test_slru_min_capacity_works() {
    slru_cache c(2);                          // prot_cap=1, prob_cap=1
    CHECK(c.capacity() == 2);

    auto e1 = c.put(P(1), B(1));
    CHECK(!e1.has_value());
    CHECK(c.get(P(1)) == B(1));               // promotes P1 to protected

    auto e2 = c.put(P(2), B(2));
    CHECK(!e2.has_value());                   // P2 enters probation
    CHECK(c.size() == 2);

    auto e3 = c.put(P(3), B(3));              // prob full → evicts P2
    CHECK(e3.has_value());
    CHECK(e3->buf == B(2));
    CHECK(c.contains(P(1)));                  // P1 (protected) survives
    CHECK(c.contains(P(3)));
    CHECK(!c.contains(P(2)));

    printf("  [slru ] capacity=2 works: OK\n");
}

// Reproducer for the leak the put-existing fix addresses: do N back-to-back
// puts on the same key, then drain via evict_one(). Each put should hand
// back the previous buf (N-1 displaced + 1 still resident), so iterating
// every returned buf accounts for all N allocations exactly once. Pre-fix
// the cache silently overwrote and the displaced bufs were leaked.
template <cache_concept Cache>
static void test_put_same_key_drain_no_leak(const char* label) {
    Cache c(8);
    constexpr int N = 4;
    std::vector<char*> seen;

    for (int i = 0; i < N; ++i) {
        auto e = c.put(P(42), B(100 + i));
        if (i == 0) {
            CHECK(!e.has_value());           // first insert displaces nothing
        } else {
            CHECK(e.has_value());            // every subsequent put displaces
            CHECK(e->key == P(42));
            CHECK(e->buf == B(100 + i - 1)); // and returns the previous buf
            seen.push_back(e->buf);
        }
    }

    // Drain the resident entry via evict_one.
    CHECK(c.size() == 1);
    auto last = c.evict_one();
    CHECK(last.has_value());
    CHECK(last->buf == B(100 + N - 1));
    seen.push_back(last->buf);
    CHECK(!c.evict_one().has_value());
    CHECK(c.size() == 0);

    // Every B(100), B(101), B(102), B(103) accounted for exactly once.
    CHECK(seen.size() == static_cast<size_t>(N));
    std::sort(seen.begin(), seen.end());
    CHECK(std::unique(seen.begin(), seen.end()) == seen.end());

    printf("  [%s] put same key drain no leak: OK\n", label);
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

// ── evict_one: drain cache at teardown ──
//
// Used by value::scheduler::~scheduler to release buffers the cache still
// owns. Both impls return one entry per call, exactly N times for N puts,
// then nullopt with size()==0.

template <cache_concept Cache>
static void test_evict_one(const char* label) {
    // N kept small enough to fit SLRU probation (prob_cap = capacity * 0.2 = 4
    // for capacity 20). Without any get() the entries never get promoted, so
    // they all live in probation; the test would fail at N>4 with a premature
    // eviction. clock_cache has no such constraint, but using the same N for
    // both keeps the test single-template.
    Cache c(20);
    constexpr int N = 3;
    for (int i = 1; i <= N; ++i) c.put(P(i), B(i));
    CHECK(c.size() == N);

    std::vector<char*> seen;
    while (auto e = c.evict_one()) {
        seen.push_back(e->buf);
    }
    CHECK(seen.size() == static_cast<size_t>(N));
    CHECK(c.size() == 0);
    CHECK(!c.evict_one().has_value());

    // Each buffer evicted exactly once.
    std::sort(seen.begin(), seen.end());
    CHECK(std::unique(seen.begin(), seen.end()) == seen.end());

    // After full drain the cache must be reusable.
    c.put(P(42), B(42));
    CHECK(c.size() == 1);
    CHECK(c.contains(P(42)));
    CHECK(c.get(P(42)) == B(42));

    printf("  [%s] evict_one: OK\n", label);
}

template <cache_concept Cache>
static void test_evict_one_empty(const char* label) {
    Cache c(8);
    CHECK(!c.evict_one().has_value());
    CHECK(c.size() == 0);
    printf("  [%s] evict_one (empty): OK\n", label);
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

    // ── evict_one (drain at teardown) ──
    test_evict_one<clock_cache>("clock");
    test_evict_one<slru_cache>("slru ");
    test_evict_one_empty<clock_cache>("clock");
    test_evict_one_empty<slru_cache>("slru ");

    // ── put-existing must return old buf (regression: silent overwrite) ──
    test_put_same_key_drain_no_leak<clock_cache>("clock");
    test_put_same_key_drain_no_leak<slru_cache>("slru ");

    // ── capacity-boundary rejection (regression: ctor crashed at use) ──
    test_clock_capacity_zero_throws();
    test_slru_capacity_zero_throws();
    test_slru_capacity_one_throws();
    test_slru_extreme_ratio_throws();

    // ── smallest legal capacity must still work end-to-end ──
    test_clock_min_capacity_works();
    test_slru_min_capacity_works();

    printf("all passed\n");
    return 0;
}
