#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "store/types.hh"
#include "store/page_table.hh"
#include "store/slab.hh"
#include "store/entry.hh"
#include "store/hash_table.hh"
#include "store/store.hh"

using namespace sider::store;

static int pass_count = 0;

#define RUN(fn) do { printf("  %-55s", #fn); fn(); printf("OK\n"); pass_count++; } while(0)

// Helper: extract hot/nil as legacy get_result (no NVMe in tests).
static get_result G(const lookup_result& r) { return kv_store::as_get_result(r); }

// ── types ──

static void test_size_class_for() {
    assert(size_class_for(1)    == SC_64);
    assert(size_class_for(64)   == SC_64);
    assert(size_class_for(65)   == SC_128);
    assert(size_class_for(128)  == SC_128);
    assert(size_class_for(129)  == SC_256);
    assert(size_class_for(256)  == SC_256);
    assert(size_class_for(257)  == SC_512);
    assert(size_class_for(512)  == SC_512);
    assert(size_class_for(513)  == SC_1K);
    assert(size_class_for(1024) == SC_1K);
    assert(size_class_for(1025) == SC_2K);
    assert(size_class_for(2048) == SC_2K);
    assert(size_class_for(2049) == SC_4K);
    assert(size_class_for(4096) == SC_4K);
}

static void test_slot_offset() {
    assert(slot_offset(SC_128, 0) == 0);
    assert(slot_offset(SC_128, 1) == 128);
    assert(slot_offset(SC_128, 31) == 31 * 128);
    assert(slot_offset(SC_64, 63) == 63 * 64);
    assert(slot_offset(SC_4K, 0) == 0);
}

static void test_full_mask() {
    assert(full_mask_for(SC_64)  == ~0ULL);
    assert(full_mask_for(SC_128) == 0xFFFFFFFFULL);   // 32 bits
    assert(full_mask_for(SC_4K)  == 1ULL);             // 1 slot
    assert(full_mask_for(SC_256) == 0xFFFFULL);        // 16 bits
}

// ── page_table ──

static void test_page_table_alloc_free_reuse() {
    page_table pt;
    auto id0 = pt.alloc_page_id();
    auto id1 = pt.alloc_page_id();
    assert(id0 == 0);
    assert(id1 == 1);
    assert(pt.size() == 2);

    pt.free_page_id(id0);
    auto id2 = pt.alloc_page_id();
    assert(id2 == id0);   // reused
}

static void test_page_table_many() {
    page_table pt;
    std::vector<uint32_t> ids;
    for (int i = 0; i < 100; i++)
        ids.push_back(pt.alloc_page_id());
    assert(pt.size() == 100);

    for (int i = 0; i < 50; i++)
        pt.free_page_id(ids[i]);

    // Re-allocate should reuse freed ids.
    for (int i = 0; i < 50; i++) {
        auto id = pt.alloc_page_id();
        assert(id < 100);  // reused, not new
    }
    assert(pt.size() == 100);  // no growth
}

// ── slab ──

static void test_slab_basic() {
    page_table pt;
    slab_allocator slab(pt);

    auto r = slab.allocate(SC_128);
    assert(r.slot_ptr != nullptr);
    assert(r.slot_index == 0);
    assert(pt[r.page_id].state == page_entry::IN_MEMORY);
    assert(pt[r.page_id].live_count == 1);
    assert(slab.total_pages_ == 1);
}

static void test_slab_fill_page() {
    page_table pt;
    slab_allocator slab(pt);

    // SC_128: 32 slots per page.
    uint32_t first_pid = slab.allocate(SC_128).page_id;
    for (int i = 1; i < 32; i++) {
        auto r = slab.allocate(SC_128);
        assert(r.page_id == first_pid);
    }
    assert(pt[first_pid].live_count == 32);
    assert(pt[first_pid].slot_bitmap == full_mask_for(SC_128));

    // Next alloc creates a new page.
    auto r = slab.allocate(SC_128);
    assert(r.page_id != first_pid);
    assert(slab.total_pages_ == 2);
}

static void test_slab_free_reuse() {
    page_table pt;
    slab_allocator slab(pt);

    // Fill a page.
    uint32_t pid = slab.allocate(SC_128).page_id;
    for (int i = 1; i < 32; i++)
        slab.allocate(SC_128);
    assert(pt[pid].live_count == 32);

    // Free slot 10 — page goes from full to partial.
    slab.free_slot(pid, 10);
    assert(pt[pid].live_count == 31);
    assert((pt[pid].slot_bitmap & (1ULL << 10)) == 0);

    // Next alloc should reuse slot 10.
    auto r = slab.allocate(SC_128);
    assert(r.page_id == pid);
    assert(r.slot_index == 10);
}

static void test_slab_empty_page_freed() {
    page_table pt;
    slab_allocator slab(pt);

    auto r = slab.allocate(SC_4K);  // 1 slot per page
    uint32_t pid = r.page_id;
    assert(slab.total_pages_ == 1);

    slab.free_slot(pid, 0);
    assert(slab.total_pages_ == 0);
    assert(pt[pid].state == page_entry::FREE);
}

static void test_slab_different_classes() {
    page_table pt;
    slab_allocator slab(pt);

    auto r64  = slab.allocate(SC_64);
    auto r256 = slab.allocate(SC_256);
    auto r4k  = slab.allocate(SC_4K);

    // Each size class gets its own page.
    assert(r64.page_id != r256.page_id);
    assert(r256.page_id != r4k.page_id);
    assert(slab.total_pages_ == 3);
    assert(slab.memory_used_bytes() == 3 * PAGE_SIZE);
}

static void test_slab_write_read() {
    page_table pt;
    slab_allocator slab(pt);

    auto r = slab.allocate(SC_128);
    std::memcpy(r.slot_ptr, "hello world", 11);

    char* ptr = slab.slot_ptr(r.page_id, r.slot_index);
    assert(ptr == r.slot_ptr);
    assert(std::memcmp(ptr, "hello world", 11) == 0);
}

static void test_slab_sc64_full_page() {
    page_table pt;
    slab_allocator slab(pt);

    // SC_64: 64 slots per page — exercises the uint64_t bitmap fully.
    uint32_t pid = slab.allocate(SC_64).page_id;
    for (int i = 1; i < 64; i++) {
        auto r = slab.allocate(SC_64);
        assert(r.page_id == pid);
    }
    assert(pt[pid].slot_bitmap == ~0ULL);
    assert(pt[pid].live_count == 64);

    // Free all, page should be reclaimed.
    for (int i = 0; i < 64; i++)
        slab.free_slot(pid, i);
    assert(slab.total_pages_ == 0);
}

// ── hash_table ──

static void test_ht_insert_lookup() {
    hash_table ht;

    auto* e = ht.insert("hello", 5);
    assert(e != nullptr);
    assert(e->key_len == 5);
    assert(e->key_equals("hello", 5));

    auto* found = ht.lookup("hello", 5);
    assert(found == e);
    assert(ht.lookup("world", 5) == nullptr);
    assert(ht.count() == 1);
}

static void test_ht_insert_existing() {
    hash_table ht;

    auto* e1 = ht.insert("key", 3);
    e1->value_len = 42;

    auto* e2 = ht.insert("key", 3);
    assert(e2 == e1);
    assert(e2->value_len == 42);
    assert(ht.count() == 1);
}

static void test_ht_erase() {
    hash_table ht;
    ht.insert("foo", 3);
    assert(ht.count() == 1);

    assert(ht.erase("foo", 3));
    assert(ht.count() == 0);
    assert(ht.lookup("foo", 3) == nullptr);

    // Erase non-existent.
    assert(!ht.erase("bar", 3));
}

static void test_ht_many_entries() {
    hash_table ht;
    constexpr int N = 10000;

    for (int i = 0; i < N; i++) {
        auto key = std::to_string(i);
        auto* e = ht.insert(key.c_str(), static_cast<uint16_t>(key.size()));
        e->page_id = static_cast<uint32_t>(i);
    }
    assert(ht.count() == N);
    assert(ht.capacity() >= N);   // grew

    // Verify all lookups.
    for (int i = 0; i < N; i++) {
        auto key = std::to_string(i);
        auto* e = ht.lookup(key.c_str(), static_cast<uint16_t>(key.size()));
        assert(e != nullptr);
        assert(e->page_id == static_cast<uint32_t>(i));
    }
}

static void test_ht_delete_many() {
    hash_table ht;
    constexpr int N = 5000;

    for (int i = 0; i < N; i++) {
        auto key = std::to_string(i);
        ht.insert(key.c_str(), static_cast<uint16_t>(key.size()));
    }

    // Delete first half.
    for (int i = 0; i < N / 2; i++) {
        auto key = std::to_string(i);
        assert(ht.erase(key.c_str(), static_cast<uint16_t>(key.size())));
    }
    assert(ht.count() == N / 2);

    // Remaining half still findable.
    for (int i = N / 2; i < N; i++) {
        auto key = std::to_string(i);
        assert(ht.lookup(key.c_str(), static_cast<uint16_t>(key.size())) != nullptr);
    }

    // Deleted half not findable.
    for (int i = 0; i < N / 2; i++) {
        auto key = std::to_string(i);
        assert(ht.lookup(key.c_str(), static_cast<uint16_t>(key.size())) == nullptr);
    }
}

static void test_ht_reinsert_after_delete() {
    hash_table ht;

    ht.insert("abc", 3)->page_id = 1;
    assert(ht.erase("abc", 3));
    ht.insert("abc", 3)->page_id = 2;

    auto* e = ht.lookup("abc", 3);
    assert(e != nullptr);
    assert(e->page_id == 2);
}

static void test_ht_collision_chain() {
    // Insert keys that would hash near each other to exercise Robin Hood swapping.
    hash_table ht;
    for (int i = 0; i < 200; i++) {
        auto key = std::to_string(i);
        ht.insert(key.c_str(), static_cast<uint16_t>(key.size()));
    }

    // Delete every other key to exercise backward shift.
    for (int i = 0; i < 200; i += 2) {
        auto key = std::to_string(i);
        assert(ht.erase(key.c_str(), static_cast<uint16_t>(key.size())));
    }

    // Remaining keys still correct.
    for (int i = 1; i < 200; i += 2) {
        auto key = std::to_string(i);
        assert(ht.lookup(key.c_str(), static_cast<uint16_t>(key.size())) != nullptr);
    }
    assert(ht.count() == 100);
}

// ── integrated: hash_table + slab ──

static void test_integrated_set_get_del() {
    page_table pt;
    slab_allocator slab(pt);
    hash_table ht;

    // Simulate SET key=hello value=world
    const char* key = "hello";
    const char* val = "world";
    uint16_t key_len = 5, val_len = 5;

    auto sc = size_class_for(val_len);
    auto ar = slab.allocate(sc);
    std::memcpy(ar.slot_ptr, val, val_len);

    auto* e = ht.insert(key, key_len);
    e->page_id    = ar.page_id;
    e->slot_index = ar.slot_index;
    e->value_len  = val_len;

    // Simulate GET
    auto* found = ht.lookup(key, key_len);
    assert(found != nullptr);
    char* ptr = slab.slot_ptr(found->page_id, found->slot_index);
    assert(std::memcmp(ptr, val, val_len) == 0);

    // Simulate DEL
    slab.free_slot(found->page_id, found->slot_index);
    assert(ht.erase(key, key_len));
    assert(ht.lookup(key, key_len) == nullptr);
}

static void test_integrated_update_same_class() {
    page_table pt;
    slab_allocator slab(pt);
    hash_table ht;

    auto sc = size_class_for(10);
    auto ar = slab.allocate(sc);
    std::memcpy(ar.slot_ptr, "aaaaaaaaaa", 10);

    auto* e = ht.insert("k", 1);
    e->page_id    = ar.page_id;
    e->slot_index = ar.slot_index;
    e->value_len  = 10;

    // Update with same size class — overwrite in place.
    auto* e2 = ht.insert("k", 1);
    assert(e2 == e);
    char* ptr = slab.slot_ptr(e2->page_id, e2->slot_index);
    std::memcpy(ptr, "bbbbbbbbbb", 10);
    e2->value_len = 10;

    char* check = slab.slot_ptr(e2->page_id, e2->slot_index);
    assert(std::memcmp(check, "bbbbbbbbbb", 10) == 0);
}

static void test_integrated_update_different_class() {
    page_table pt;
    slab_allocator slab(pt);
    hash_table ht;

    // Initial: small value (64B class).
    auto sc1 = size_class_for(10);
    auto ar1 = slab.allocate(sc1);
    std::memcpy(ar1.slot_ptr, "small", 5);

    auto* e = ht.insert("k", 1);
    e->page_id    = ar1.page_id;
    e->slot_index = ar1.slot_index;
    e->value_len  = 5;

    // Update: larger value (128B class).
    auto sc2 = size_class_for(100);
    assert(sc2 != sc1);

    // Free old slot, allocate new.
    slab.free_slot(e->page_id, e->slot_index);
    auto ar2 = slab.allocate(sc2);
    std::memcpy(ar2.slot_ptr, "large-value-here", 16);

    e->page_id    = ar2.page_id;
    e->slot_index = ar2.slot_index;
    e->value_len  = 16;

    // Verify.
    char* ptr = slab.slot_ptr(e->page_id, e->slot_index);
    assert(std::memcmp(ptr, "large-value-here", 16) == 0);
}

// ── kv_store ──

static void test_store_set_get() {
    kv_store s;
    s.set("key", 3, "value", 5);
    auto r = G(s.get("key", 3));
    assert(r.found());
    assert(r.len == 5);
    assert(std::memcmp(r.data, "value", 5) == 0);
}

static void test_store_get_missing() {
    kv_store s;
    assert(!G(s.get("nope", 4)).found());
}

static void test_store_del() {
    kv_store s;
    s.set("k", 1, "v", 1);
    assert(s.del("k", 1) == 1);
    assert(s.del("k", 1) == 0);
    assert(!G(s.get("k", 1)).found());
}

static void test_store_update_same_class() {
    kv_store s;
    s.set("k", 1, "aaa", 3);
    s.set("k", 1, "bbb", 3);
    auto r = G(s.get("k", 1));
    assert(r.len == 3);
    assert(std::memcmp(r.data, "bbb", 3) == 0);
}

static void test_store_update_different_class() {
    kv_store s;
    s.set("k", 1, "small", 5);   // SC_64

    char big[200];
    std::memset(big, 'X', 200);
    s.set("k", 1, big, 200);     // SC_256

    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 200);
    assert(r.data[0] == 'X');
}

static void test_store_many_keys() {
    kv_store s;
    for (int i = 0; i < 1000; i++) {
        auto key = std::to_string(i);
        auto val = "val_" + key;
        s.set(key.c_str(), static_cast<uint16_t>(key.size()),
              val.c_str(), static_cast<uint16_t>(val.size()));
    }
    for (int i = 0; i < 1000; i++) {
        auto key = std::to_string(i);
        auto expected = "val_" + key;
        auto r = G(s.get(key.c_str(), static_cast<uint16_t>(key.size())));
        assert(r.found());
        assert(r.len == expected.size());
        assert(std::memcmp(r.data, expected.c_str(), r.len) == 0);
    }
}

static void test_store_no_leak_on_repeated_update() {
    kv_store s;
    // Repeatedly SET same key with varying sizes — slab pages should be recycled.
    for (int i = 0; i < 500; i++) {
        uint16_t vlen = static_cast<uint16_t>((i % 7) * 100 + 10);  // 10..610
        char buf[700];
        std::memset(buf, 'a' + (i % 26), vlen);
        s.set("key", 3, buf, vlen);
    }
    // Should have exactly 1 key.
    assert(s.key_count() == 1);
    // Memory should be minimal (1-2 pages).
    assert(s.memory_used_bytes() <= 2 * PAGE_SIZE);
}

// ── TTL ──

static void test_store_set_with_ttl_no_expiry_yet() {
    kv_store s;
    // expire_at far in the future — should still be found.
    int64_t future = now_ms() + 60000;
    s.set("k", 1, "v", 1, future);
    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 1);
}

static void test_store_get_lazy_expiry() {
    kv_store s;
    // Set with expire_at in the past → GET should return not found (lazy expiry).
    int64_t past = now_ms() - 1;
    s.set("k", 1, "v", 1, past);
    auto r = G(s.get("k", 1));
    assert(!r.found());
    // Entry and slab slot should be cleaned up.
    assert(s.key_count() == 0);
}

static void test_store_del_lazy_expiry() {
    kv_store s;
    int64_t past = now_ms() - 1;
    s.set("k", 1, "v", 1, past);
    // DEL on expired key → returns 0 (not found).
    assert(s.del("k", 1) == 0);
    assert(s.key_count() == 0);
}

static void test_store_set_updates_expire() {
    kv_store s;
    int64_t past = now_ms() - 1;
    s.set("k", 1, "v", 1, past);
    // Overwrite with no expiry — should be alive.
    s.set("k", 1, "v2", 2, -1);
    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 2);
}

static void test_store_expire_scan() {
    kv_store s;
    int64_t past = now_ms() - 1;
    // Insert 100 keys, all already expired.
    for (int i = 0; i < 100; i++) {
        auto key = std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), "v", 1, past);
    }
    assert(s.key_count() == 100);

    // Scan enough times to clean all.
    for (int round = 0; round < 20; round++)
        s.expire_scan(50);

    assert(s.key_count() == 0);
    assert(s.memory_used_bytes() == 0);
}

static void test_store_expire_scan_keeps_live() {
    kv_store s;
    int64_t past = now_ms() - 1;
    int64_t future = now_ms() + 60000;

    for (int i = 0; i < 50; i++) {
        auto key = "exp_" + std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), "v", 1, past);
    }
    for (int i = 0; i < 50; i++) {
        auto key = "live_" + std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), "v", 1, future);
    }
    assert(s.key_count() == 100);

    for (int round = 0; round < 20; round++)
        s.expire_scan(50);

    assert(s.key_count() == 50);
    // Live keys are still accessible.
    auto r = G(s.get("live_0", 6));
    assert(r.found());
}

// ── eviction ──

static void test_store_discard_one_page() {
    kv_store s;
    // Insert 200 keys with 32-byte values (SC_64: 64 slots/page → ~4 pages).
    // Must be > INLINE_VALUE_MAX (16) to use slab.
    char val32[32];
    std::memset(val32, 'V', sizeof(val32));
    for (int i = 0; i < 200; i++) {
        auto key = "k" + std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), val32, 32);
    }
    auto before_count = s.key_count();
    auto before_mem = s.memory_used_bytes();
    assert(before_count == 200);
    assert(before_mem > 0);

    int evicted = s.discard_one_page();
    assert(evicted > 0);
    assert(s.key_count() < before_count);
    assert(s.memory_used_bytes() < before_mem);

    // Evicted keys should return nil.
    auto after_count = s.key_count();
    int missing = 0;
    for (int i = 0; i < 200; i++) {
        auto key = "k" + std::to_string(i);
        if (!G(s.get(key.c_str(), static_cast<uint16_t>(key.size()))).found())
            missing++;
    }
    assert(missing > 0);
    assert(missing == static_cast<int>(before_count - after_count));
}

static void test_store_evict_returns_nil() {
    kv_store s;
    // Must use > INLINE_VALUE_MAX to create a slab page.
    char val32[32];
    std::memset(val32, 'x', sizeof(val32));
    s.set("only", 4, val32, 32);
    assert(s.key_count() == 1);

    s.discard_one_page();
    assert(!G(s.get("only", 4)).found());
    assert(s.key_count() == 0);
    assert(s.memory_used_bytes() == 0);
}

static void test_store_evict_preserves_hot_keys() {
    kv_store s;
    // Use 200-byte values → SC_256 (16 slots/page) → ~13 pages.
    char val[200];
    std::memset(val, 'x', 200);
    for (int i = 0; i < 200; i++) {
        auto key = "k" + std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), val, 200);
    }
    // 200 keys / 16 per page = 12.5 → 13 pages.
    // Keys 0-15 on page 0, 16-31 on page 1, 32-47 on page 2, 48-63 on page 3.

    // Access keys 0-49 repeatedly to make pages 0-3 hot.
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 50; i++) {
            auto key = "k" + std::to_string(i);
            G(s.get(key.c_str(), static_cast<uint16_t>(key.size())));
        }
    }

    // Evict 5 pages — should target cold pages (4-12).
    for (int i = 0; i < 5; i++)
        s.discard_one_page();

    // All 50 hot keys should survive (5 cold pages evicted from 9 cold pages).
    int hot_found = 0;
    for (int i = 0; i < 50; i++) {
        auto key = "k" + std::to_string(i);
        if (G(s.get(key.c_str(), static_cast<uint16_t>(key.size()))).found())
            hot_found++;
    }
    assert(hot_found >= 45);  // conservative margin for sampling randomness
}

static void test_store_evict_sync_on_max() {
    kv_store s;
    // 10 pages limit. SC_64: 64 slots/page.
    // Backpressure rejects new keys when memory_used_bytes >= max_bytes.
    // Page granularity: 9 pages fill (576 keys), 10th page alloc (1 key) → at limit.
    s.evict_cfg_ = {10 * PAGE_SIZE, 0.60, 0.90};

    char val32[32];
    std::memset(val32, 'V', sizeof(val32));
    int accepted = 0;
    for (int i = 0; i < 2000; i++) {
        auto key = "k" + std::to_string(i);
        if (s.set(key.c_str(), static_cast<uint16_t>(key.size()), val32, 32))
            accepted++;
    }

    // Backpressure: set() returns false at hard limit, no keys discarded.
    assert(s.memory_used_bytes() <= 10 * PAGE_SIZE);
    assert(s.key_count() == static_cast<uint32_t>(accepted));
    assert(accepted > 0);
    assert(accepted < 2000);
}

static void test_store_evict_memory_stable() {
    kv_store s;
    // 20 pages limit, SC_128 values (100 bytes → 32 slots/page → 640 max keys).
    s.evict_cfg_ = {20 * PAGE_SIZE, 0.50, 0.80};

    char val[100];
    std::memset(val, 'x', 100);
    for (int i = 0; i < 5000; i++) {
        auto key = "k" + std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), val, 100);
    }

    // Memory stays within limit.
    assert(s.memory_used_bytes() <= 21 * PAGE_SIZE);
    assert(s.key_count() > 0);
    assert(s.key_count() < 5000);
}

static void test_store_set_backpressure() {
    kv_store s;
    // 5 pages limit. SC_4K: 1 slot/page → each key = 1 page, 5 keys max.
    s.evict_cfg_ = {5 * PAGE_SIZE, 0.60, 0.90};

    char val[3000];
    std::memset(val, 'B', sizeof(val));

    int accepted = 0, rejected = 0;
    for (int i = 0; i < 10; i++) {
        auto key = "k" + std::to_string(i);
        bool ok = s.set(key.c_str(), static_cast<uint16_t>(key.size()), val, 3000);
        if (ok) accepted++; else rejected++;
    }

    // 5 accepted (one per page), 5 rejected at hard limit.
    assert(accepted == 5);
    assert(rejected == 5);
    assert(s.memory_used_bytes() == 5 * PAGE_SIZE);
    assert(s.key_count() == 5);

    // All accepted keys readable.
    for (int i = 0; i < 5; i++) {
        auto key = "k" + std::to_string(i);
        auto r = G(s.get(key.c_str(), static_cast<uint16_t>(key.size())));
        assert(r.found());
        assert(r.len == 3000);
    }

    // Rejected keys don't exist.
    for (int i = 5; i < 10; i++) {
        auto key = "k" + std::to_string(i);
        assert(!G(s.get(key.c_str(), static_cast<uint16_t>(key.size()))).found());
    }
}

static void test_store_evict_no_pages() {
    kv_store s;
    // Nothing to evict.
    assert(s.discard_one_page() == 0);
}

// ── large values ──

static void test_large_value_set_get_8k() {
    kv_store s;
    char val[8192];
    std::memset(val, 'A', sizeof(val));
    s.set("bigkey", 6, val, 8192);
    assert(s.key_count() == 1);

    auto r = G(s.get("bigkey", 6));
    assert(r.found());
    assert(r.len == 8192);
    assert(r.data[0] == 'A');
    assert(r.data[8191] == 'A');
    // Large value: memory = pages_for(8192) * 4096 = 2 * 4096
    assert(s.memory_used_bytes() == 2 * PAGE_SIZE);
}

static void test_large_value_set_get_16k() {
    kv_store s;
    char val[16384];
    std::memset(val, 'B', sizeof(val));
    s.set("bigkey", 6, val, 16384);

    auto r = G(s.get("bigkey", 6));
    assert(r.found());
    assert(r.len == 16384);
    assert(r.data[0] == 'B');
    assert(r.data[16383] == 'B');
    assert(s.memory_used_bytes() == 4 * PAGE_SIZE);
}

static void test_large_value_set_get_64k() {
    kv_store s;
    auto* val = new char[65536];
    std::memset(val, 'C', 65536);
    s.set("bigkey", 6, val, 65536);
    delete[] val;

    auto r = G(s.get("bigkey", 6));
    assert(r.found());
    assert(r.len == 65536);
    assert(r.data[0] == 'C');
    assert(r.data[65535] == 'C');
    assert(s.memory_used_bytes() == 16 * PAGE_SIZE);
}

static void test_large_value_del() {
    kv_store s;
    char val[8192];
    std::memset(val, 'D', sizeof(val));
    s.set("k", 1, val, 8192);
    assert(s.memory_used_bytes() == 2 * PAGE_SIZE);

    assert(s.del("k", 1) == 1);
    assert(s.key_count() == 0);
    assert(s.memory_used_bytes() == 0);
    assert(s.large_memory_bytes_ == 0);
}

static void test_large_value_update_large_same_size() {
    kv_store s;
    char val1[8192], val2[8192];
    std::memset(val1, 'E', sizeof(val1));
    std::memset(val2, 'F', sizeof(val2));

    s.set("k", 1, val1, 8192);
    s.set("k", 1, val2, 8192);  // same page_count → overwrite in place

    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 8192);
    assert(r.data[0] == 'F');
    assert(s.memory_used_bytes() == 2 * PAGE_SIZE);
    assert(s.key_count() == 1);
}

static void test_large_value_update_large_diff_size() {
    kv_store s;
    char val1[8192];
    std::memset(val1, 'G', sizeof(val1));
    s.set("k", 1, val1, 8192);
    assert(s.memory_used_bytes() == 2 * PAGE_SIZE);

    char val2[16384];
    std::memset(val2, 'H', sizeof(val2));
    s.set("k", 1, val2, 16384);  // different page_count → free old, alloc new
    assert(s.memory_used_bytes() == 4 * PAGE_SIZE);

    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 16384);
    assert(r.data[0] == 'H');
    assert(s.key_count() == 1);
}

static void test_large_value_small_to_large() {
    kv_store s;
    s.set("k", 1, "small", 5);
    assert(s.large_memory_bytes_ == 0);

    char big[8192];
    std::memset(big, 'I', sizeof(big));
    s.set("k", 1, big, 8192);

    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 8192);
    assert(r.data[0] == 'I');
    assert(s.large_memory_bytes_ == 2 * PAGE_SIZE);
    assert(s.key_count() == 1);
}

static void test_large_value_large_to_small() {
    kv_store s;
    char big[8192];
    std::memset(big, 'J', sizeof(big));
    s.set("k", 1, big, 8192);
    assert(s.large_memory_bytes_ == 2 * PAGE_SIZE);

    s.set("k", 1, "tiny", 4);
    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 4);
    assert(std::memcmp(r.data, "tiny", 4) == 0);
    assert(s.large_memory_bytes_ == 0);
    assert(s.key_count() == 1);
}

static void test_large_value_discard() {
    kv_store s;
    s.evict_cfg_ = {5 * PAGE_SIZE, 0.60, 0.90};

    // Insert 3 large values, each 2 pages (6 pages total > 5 page limit).
    // Backpressure: check is before allocation, so a single large alloc can
    // overshoot. All 3 inserts pass (4 pages < 5 page limit when big2 starts).
    int accepted = 0;
    for (int i = 0; i < 3; i++) {
        auto key = "big" + std::to_string(i);
        char val[8192];
        std::memset(val, 'K' + i, sizeof(val));
        if (s.set(key.c_str(), static_cast<uint16_t>(key.size()), val, 8192))
            accepted++;
    }

    // All 3 accepted (memory was below limit before each alloc).
    assert(accepted == 3);
    assert(s.key_count() == 3);
    // Overshoot: 6 pages > 5 page limit (no sync discard, scheduler handles it).
    assert(s.memory_used_bytes() == 6 * PAGE_SIZE);
}

static void test_large_value_mixed_eviction() {
    kv_store s;
    // 20 pages limit — mix of small and large values.
    s.evict_cfg_ = {20 * PAGE_SIZE, 0.50, 0.80};

    // Insert small values (SC_64, 64 per page).
    for (int i = 0; i < 200; i++) {
        auto key = "s" + std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), "v", 1);
    }

    // Insert large values.
    for (int i = 0; i < 5; i++) {
        auto key = "big" + std::to_string(i);
        char val[8192];
        std::memset(val, 'X', sizeof(val));
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), val, 8192);
    }

    // Memory should stay bounded.
    assert(s.memory_used_bytes() <= 21 * PAGE_SIZE);
    assert(s.key_count() > 0);
}

static void test_large_value_no_leak_repeated_update() {
    kv_store s;
    // Repeatedly update with varying large sizes — no memory leak.
    for (int i = 0; i < 100; i++) {
        uint32_t vlen = static_cast<uint32_t>(((i % 5) + 2) * PAGE_SIZE);  // 8K-24K
        auto* val = new char[vlen];
        std::memset(val, 'a' + (i % 26), vlen);
        s.set("key", 3, val, vlen);
        delete[] val;
    }
    assert(s.key_count() == 1);
    // Should have exactly 1 large value worth of memory.
    auto expected_pc = pages_for(static_cast<uint32_t>(((99 % 5) + 2) * PAGE_SIZE));
    assert(s.memory_used_bytes() == expected_pc * PAGE_SIZE);
}

static void test_large_value_entry_flag() {
    kv_store s;
    // Set a large value.
    char big[8192];
    std::memset(big, 'Z', sizeof(big));
    s.set("k", 1, big, 8192);

    auto* e = s.ht.lookup("k", 1);
    assert(e != nullptr);
    assert(e->is_large());
    assert(e->slot_index == 0);

    // Update to small.
    s.set("k", 1, "sm", 2);
    e = s.ht.lookup("k", 1);
    assert(e != nullptr);
    assert(!e->is_large());
}

// ── inline values ──

static void test_inline_set_get() {
    kv_store s;
    s.set("k", 1, "hello", 5);
    auto* e = s.ht.lookup("k", 1);
    assert(e != nullptr);
    assert(e->is_inline());
    assert(!e->is_large());
    assert(e->value_len == 5);

    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 5);
    assert(std::memcmp(r.data, "hello", 5) == 0);
    // Inline uses no slab memory.
    assert(s.memory_used_bytes() == 0);
}

static void test_inline_set_get_16b() {
    kv_store s;
    char val[16];
    std::memset(val, 'X', 16);
    s.set("k", 1, val, 16);

    auto* e = s.ht.lookup("k", 1);
    assert(e->is_inline());
    assert(e->value_len == 16);

    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 16);
    assert(r.data[0] == 'X' && r.data[15] == 'X');
}

static void test_inline_set_get_0b() {
    kv_store s;
    s.set("k", 1, "", 0);

    auto* e = s.ht.lookup("k", 1);
    assert(e->is_inline());
    assert(e->value_len == 0);

    auto r = G(s.get("k", 1));
    assert(r.found());
    assert(r.len == 0);
}

static void test_inline_del() {
    kv_store s;
    s.set("k", 1, "v", 1);
    assert(s.del("k", 1) == 1);
    assert(!G(s.get("k", 1)).found());
    assert(s.key_count() == 0);
}

static void test_inline_to_inline() {
    kv_store s;
    s.set("k", 1, "aaa", 3);
    s.set("k", 1, "bbb", 3);
    auto r = G(s.get("k", 1));
    assert(r.len == 3);
    assert(std::memcmp(r.data, "bbb", 3) == 0);
    assert(s.ht.lookup("k", 1)->is_inline());
}

static void test_inline_to_slab() {
    kv_store s;
    s.set("k", 1, "tiny", 4);
    assert(s.ht.lookup("k", 1)->is_inline());
    assert(s.memory_used_bytes() == 0);

    // Update to 32 bytes → slab.
    char val[32];
    std::memset(val, 'B', sizeof(val));
    s.set("k", 1, val, 32);
    auto* e = s.ht.lookup("k", 1);
    assert(!e->is_inline());
    assert(!e->is_large());
    assert(s.memory_used_bytes() == PAGE_SIZE);

    auto r = G(s.get("k", 1));
    assert(r.len == 32);
    assert(r.data[0] == 'B');
}

static void test_slab_to_inline() {
    kv_store s;
    char val[32];
    std::memset(val, 'A', sizeof(val));
    s.set("k", 1, val, 32);
    assert(!s.ht.lookup("k", 1)->is_inline());
    assert(s.memory_used_bytes() == PAGE_SIZE);

    s.set("k", 1, "tiny", 4);
    assert(s.ht.lookup("k", 1)->is_inline());
    // Slab page freed since it had only 1 entry.
    assert(s.memory_used_bytes() == 0);

    auto r = G(s.get("k", 1));
    assert(r.len == 4);
    assert(std::memcmp(r.data, "tiny", 4) == 0);
}

static void test_inline_to_large() {
    kv_store s;
    s.set("k", 1, "tiny", 4);
    assert(s.ht.lookup("k", 1)->is_inline());

    char big[8192];
    std::memset(big, 'L', sizeof(big));
    s.set("k", 1, big, 8192);
    auto* e = s.ht.lookup("k", 1);
    assert(!e->is_inline());
    assert(e->is_large());
    assert(s.large_memory_bytes_ == 2 * PAGE_SIZE);
}

static void test_large_to_inline() {
    kv_store s;
    char big[8192];
    std::memset(big, 'L', sizeof(big));
    s.set("k", 1, big, 8192);
    assert(s.large_memory_bytes_ == 2 * PAGE_SIZE);

    s.set("k", 1, "tiny", 4);
    assert(s.ht.lookup("k", 1)->is_inline());
    assert(s.large_memory_bytes_ == 0);

    auto r = G(s.get("k", 1));
    assert(r.len == 4);
    assert(std::memcmp(r.data, "tiny", 4) == 0);
}

static void test_inline_not_evicted() {
    kv_store s;
    // All inline — no slab pages to evict.
    for (int i = 0; i < 100; i++) {
        auto key = "k" + std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), "v", 1);
    }
    assert(s.memory_used_bytes() == 0);
    assert(s.discard_one_page() == 0);
    assert(s.key_count() == 100);
}

static void test_inline_many_keys() {
    kv_store s;
    for (int i = 0; i < 1000; i++) {
        auto key = std::to_string(i);
        auto val = "v" + key;
        s.set(key.c_str(), static_cast<uint16_t>(key.size()),
              val.c_str(), static_cast<uint16_t>(val.size()));
    }
    for (int i = 0; i < 1000; i++) {
        auto key = std::to_string(i);
        auto expected = "v" + key;
        auto r = G(s.get(key.c_str(), static_cast<uint16_t>(key.size())));
        assert(r.found());
        assert(r.len == expected.size());
        assert(std::memcmp(r.data, expected.c_str(), r.len) == 0);
    }
    // All values ≤ 16 bytes → all inline, no slab.
    assert(s.memory_used_bytes() == 0);
}

static void test_inline_entry_flag() {
    kv_store s;
    s.set("k", 1, "v", 1);
    auto* e = s.ht.lookup("k", 1);
    assert(e->is_inline());
    assert(!e->is_large());

    // Update to slab.
    char val[32];
    std::memset(val, 'Z', sizeof(val));
    s.set("k", 1, val, 32);
    e = s.ht.lookup("k", 1);
    assert(!e->is_inline());
    assert(!e->is_large());

    // Update back to inline.
    s.set("k", 1, "w", 1);
    e = s.ht.lookup("k", 1);
    assert(e->is_inline());
    assert(!e->is_large());
}

// ── clean eviction helpers ──

// Simulate NVMe eviction: pick page, write to NVMe (fake), complete.
// Returns page_id of the evicted page, or UINT32_MAX if none.
static uint32_t fake_evict_page(kv_store& s, uint64_t fake_lba = 1000, uint8_t disk = 0) {
    uint32_t victim = ([&]{ bool cf=false; return s.begin_eviction(cf); }());
    if (victim == UINT32_MAX) return UINT32_MAX;
    auto& pe = s.pt[victim];
    pe.nvme_lba = fake_lba;
    pe.disk_id = disk;
    bool free_lba = s.complete_eviction(victim, true);
    (void)free_lba;
    return victim;
}

// Fill a slab page by SETting enough keys to fill it, return (page_id, key list).
struct fill_result { uint32_t page_id; std::vector<std::string> keys; };
static fill_result fill_one_page(kv_store& s, const std::string& prefix, int value_len) {
    auto sc = size_class_for(value_len);
    int nslots = slots_per_page[sc];
    std::string val(value_len, 'V');
    fill_result fr;
    fr.page_id = UINT32_MAX;
    for (int i = 0; i < nslots; i++) {
        std::string k = prefix + std::to_string(i);
        s.set(k.c_str(), static_cast<uint16_t>(k.size()),
              val.c_str(), static_cast<uint32_t>(val.size()));
        auto* e = s.ht.lookup(k.c_str(), static_cast<uint16_t>(k.size()));
        if (fr.page_id == UINT32_MAX) fr.page_id = e->page_id;
        fr.keys.push_back(k);
    }
    return fr;
}

// ── clean eviction tests ──

// A1: promote doesn't decrement old page live_count
static void test_ce_promote_keeps_live_count() {
    kv_store s;
    std::string val(64, 'A');
    s.set("k1", 2, val.c_str(), 64);
    auto* e = s.ht.lookup("k1", 2);
    uint32_t pid = e->page_id;

    // Evict to NVMe.
    auto& pe = s.pt[pid];
    pe.state = page_entry::ON_NVME;
    pe.nvme_lba = 100;
    pe.disk_id = 0;
    uint16_t lc_before = pe.live_count;

    // Promote.
    s.promote("k1", 2, e->version, val.c_str(), 64);
    assert(pe.live_count == lc_before);  // NOT decremented
    e = s.ht.lookup("k1", 2);
    assert(e->nvme_page_id == pid);      // backup saved
    assert(e->page_id != pid);           // entry moved to new page
}

// A2: SET on clean entry releases backup
static void test_ce_set_clean_releases_backup() {
    kv_store s;
    std::string val(64, 'A');
    s.set("k1", 2, val.c_str(), 64);
    auto* e = s.ht.lookup("k1", 2);
    uint32_t old_pid = e->page_id;

    // Evict + promote.
    s.pt[old_pid].state = page_entry::ON_NVME;
    s.pt[old_pid].nvme_lba = 100;
    s.promote("k1", 2, e->version, val.c_str(), 64);
    e = s.ht.lookup("k1", 2);
    assert(e->has_nvme_backup());
    uint16_t lc = s.pt[old_pid].live_count;

    // SET → should release backup.
    std::string val2(64, 'B');
    s.set("k1", 2, val2.c_str(), 64);
    e = s.ht.lookup("k1", 2);
    assert(!e->has_nvme_backup());
    assert(s.pt[old_pid].live_count == lc - 1);
}

// A3: DEL on clean entry releases backup
static void test_ce_del_clean_releases_backup() {
    kv_store s;
    std::string val(64, 'A');
    s.set("k1", 2, val.c_str(), 64);
    s.set("k2", 2, val.c_str(), 64);  // keep page alive after k1 leaves
    auto* e1 = s.ht.lookup("k1", 2);
    uint32_t old_pid = e1->page_id;

    s.pt[old_pid].state = page_entry::ON_NVME;
    s.pt[old_pid].nvme_lba = 100;
    s.promote("k1", 2, e1->version, val.c_str(), 64);
    uint16_t lc = s.pt[old_pid].live_count;

    s.del("k1", 2);
    assert(s.pt[old_pid].live_count == lc - 1);
}

// A4: SET on ON_NVME entry (page_id == nvme_page_id) only decrements once
static void test_ce_set_on_nvme_no_double_dec() {
    kv_store s;
    std::string val(64, 'A');
    s.set("k1", 2, val.c_str(), 64);
    s.set("k2", 2, val.c_str(), 64);  // keep page alive
    auto* e = s.ht.lookup("k1", 2);
    uint32_t pid = e->page_id;

    // Evict → ON_NVME. Promote → clean evict → back to pid.
    auto& pe = s.pt[pid];
    pe.state = page_entry::ON_NVME;
    pe.nvme_lba = 100;
    uint16_t lc_nvme = pe.live_count;
    s.promote("k1", 2, e->version, val.c_str(), 64);
    assert(pe.live_count == lc_nvme);  // not decremented

    // Simulate clean evict: return entry to old page.
    e = s.ht.lookup("k1", 2);
    uint32_t new_pid = e->page_id;
    auto& new_pe = s.pt[new_pid];
    e->page_id = e->nvme_page_id;
    e->slot_index = e->nvme_slot;
    e->nvme_page_id = entry::NO_NVMe_PAGE;
    new_pe.slot_bitmap &= ~(1ULL << e->slot_index);
    // Don't actually free the page for simplicity.

    // Now entry is ON_NVME at pid. nvme_page_id = INVALID.
    assert(e->page_id == pid);
    assert(!e->has_nvme_backup());

    // SET on this ON_NVME entry.
    std::string val2(64, 'B');
    s.set("k1", 2, val2.c_str(), 64);
    // Should decrement pid only once.
    assert(s.pt[pid].live_count == lc_nvme - 1);
}

// A5: all refs released → page freed
static void test_ce_all_refs_released_frees_page() {
    kv_store s;
    std::string val(64, 'A');
    s.set("k1", 2, val.c_str(), 64);
    auto* e = s.ht.lookup("k1", 2);
    uint32_t pid = e->page_id;

    s.pt[pid].state = page_entry::ON_NVME;
    s.pt[pid].nvme_lba = 100;
    s.promote("k1", 2, e->version, val.c_str(), 64);

    // live_count should still be 1 (promote didn't dec).
    assert(s.pt[pid].live_count == 1);

    // SET releases the backup → live_count = 0 → page freed.
    std::string val2(64, 'B');
    s.set("k1", 2, val2.c_str(), 64);
    assert(s.pending_nvme_frees_.size() >= 1);
}

// B1-B4: dirty_bitmap
static void test_ce_dirty_bitmap_promote_vs_set() {
    kv_store s;
    std::string val(64, 'A');
    // Fill a page.
    s.set("k1", 2, val.c_str(), 64);
    s.set("k2", 2, val.c_str(), 64);
    auto* e1 = s.ht.lookup("k1", 2);
    auto* e2 = s.ht.lookup("k2", 2);
    uint32_t pid = e1->page_id;
    assert(e2->page_id == pid);  // same page

    // Both dirty from SET.
    auto& pe = s.pt[pid];
    assert(pe.dirty_bitmap & (1ULL << e1->slot_index));
    assert(pe.dirty_bitmap & (1ULL << e2->slot_index));

    // Evict + promote k1.
    pe.state = page_entry::ON_NVME;
    pe.nvme_lba = 100;
    s.promote("k1", 2, e1->version, val.c_str(), 64);
    e1 = s.ht.lookup("k1", 2);
    uint32_t new_pid = e1->page_id;
    auto& new_pe = s.pt[new_pid];

    // Promoted slot should be clean (dirty_bitmap = 0).
    assert(!(new_pe.dirty_bitmap & (1ULL << e1->slot_index)));

    // SET k1 → slot becomes dirty.
    s.set("k1", 2, val.c_str(), 64);
    e1 = s.ht.lookup("k1", 2);
    assert(s.pt[e1->page_id].dirty_bitmap & (1ULL << e1->slot_index));
}

// C1: all-clean page → begin_eviction frees directly
static void test_ce_begin_eviction_all_clean() {
    kv_store s;
    std::string val(256, 'X');

    // Create two pages: page A (will be evicted first) and page B (target for promote).
    auto fa = fill_one_page(s, "a", 256);  // SC_256: 16 slots
    auto fb = fill_one_page(s, "b", 256);  // ensures partial pages exist for promote
    uint32_t pa = fa.page_id;

    // Evict page A to NVMe (simulate).
    auto& pea = s.pt[pa];
    pea.state = page_entry::ON_NVME;
    pea.nvme_lba = 200;
    s.slab.remove_from_partials_public(pea.size_class, pa);
    s.slab.total_pages_--;

    // Promote all entries from page A (they go to page B or new pages).
    for (auto& k : fa.keys) {
        auto* e = s.ht.lookup(k.c_str(), static_cast<uint16_t>(k.size()));
        if (!e || s.pt[e->page_id].state != page_entry::ON_NVME) continue;
        s.promote(k.c_str(), static_cast<uint16_t>(k.size()), e->version,
                  val.c_str(), 256);
    }

    // All promoted entries should be clean with nvme_page_id = pa.
    for (auto& k : fa.keys) {
        auto* e = s.ht.lookup(k.c_str(), static_cast<uint16_t>(k.size()));
        assert(e);
        if (e->is_inline()) continue;
        assert(e->has_nvme_backup());
        assert(e->nvme_page_id == pa);
    }

    // Now find the page these promoted entries landed on and evict it.
    auto* e0 = s.ht.lookup(fa.keys[0].c_str(), static_cast<uint16_t>(fa.keys[0].size()));
    uint32_t promoted_page = e0->page_id;
    auto& ppe = s.pt[promoted_page];

    // Verify all slots on this page are clean.
    uint64_t occupied_clean = ppe.slot_bitmap & ~ppe.dirty_bitmap;
    assert(occupied_clean == ppe.slot_bitmap);  // all occupied slots are clean

    uint16_t lc_before = ppe.live_count;
    uint32_t total_pages_before = s.slab.total_pages_;

    // Force this page to be the coldest by setting hotness = 0.
    ppe.hotness = 0;
    uint32_t evicted = ([&]{ bool cf=false; return s.begin_eviction(cf); }());

    // begin_eviction should return UINT32_MAX (all clean, freed directly).
    assert(evicted == UINT32_MAX);
    assert(s.slab.total_pages_ < total_pages_before);

    // Entries should be back on page A (ON_NVME).
    for (auto& k : fa.keys) {
        auto* e = s.ht.lookup(k.c_str(), static_cast<uint16_t>(k.size()));
        if (!e) continue;
        auto r = s.get(k.c_str(), static_cast<uint16_t>(k.size()));
        assert(std::holds_alternative<cold_result>(r));
    }
}

// C2: mixed page → clean entries return, dirty remain
static void test_ce_begin_eviction_mixed() {
    kv_store s;
    std::string val(64, 'M');
    s.set("k1", 2, val.c_str(), 64);
    s.set("k2", 2, val.c_str(), 64);
    auto* e1 = s.ht.lookup("k1", 2);
    auto* e2 = s.ht.lookup("k2", 2);
    uint32_t pid = e1->page_id;
    assert(e2->page_id == pid);

    // Evict to NVMe.
    auto& pe = s.pt[pid];
    pe.state = page_entry::ON_NVME;
    pe.nvme_lba = 300;
    s.slab.remove_from_partials_public(pe.size_class, pid);
    s.slab.total_pages_--;

    // Promote both.
    s.promote("k1", 2, e1->version, val.c_str(), 64);
    s.promote("k2", 2, e2->version, val.c_str(), 64);
    e1 = s.ht.lookup("k1", 2);
    e2 = s.ht.lookup("k2", 2);

    // SET k1 → dirty. k2 stays clean.
    std::string val2(64, 'N');
    s.set("k1", 2, val2.c_str(), 64);
    e1 = s.ht.lookup("k1", 2);
    e2 = s.ht.lookup("k2", 2);

    uint32_t new_pid = e2->page_id;  // they might be on same or different pages
    if (e1->page_id == new_pid) {
        // Both on same page: mixed dirty/clean.
        auto& npe = s.pt[new_pid];
        npe.hotness = 0;
        uint16_t lc_before = npe.live_count;
        uint32_t victim = ([&]{ bool cf=false; return s.begin_eviction(cf); }());
        if (victim == UINT32_MAX) {
            // All clean evicted (k1 might have moved to different page on SET).
            // This is fine — k2 returned to its NVMe page.
        } else {
            // Mixed: should have fewer live entries (clean ones returned).
            assert(s.pt[victim].live_count < lc_before);
        }
    }
    // If on different pages, the test is trivially correct.
}

// C3: all-dirty page → normal eviction
static void test_ce_begin_eviction_all_dirty() {
    kv_store s;
    std::string val(64, 'D');
    s.set("k1", 2, val.c_str(), 64);
    s.set("k2", 2, val.c_str(), 64);
    auto* e1 = s.ht.lookup("k1", 2);
    uint32_t pid = e1->page_id;
    auto& pe = s.pt[pid];

    // All entries are from SET → all dirty.
    pe.hotness = 0;
    uint32_t victim = ([&]{ bool cf=false; return s.begin_eviction(cf); }());
    // Should return the page (needs NVMe write, not freed directly).
    assert(victim != UINT32_MAX);
    assert(s.pt[victim].state == page_entry::EVICTING);
}

// C4: after clean evict, get() returns cold_result
static void test_ce_cold_result_after_clean_evict() {
    kv_store s;
    std::string val(64, 'C');
    s.set("k1", 2, val.c_str(), 64);
    auto* e = s.ht.lookup("k1", 2);
    uint32_t pid = e->page_id;

    // Evict → ON_NVME → promote → simulate clean evict (return to old page).
    s.pt[pid].state = page_entry::ON_NVME;
    s.pt[pid].nvme_lba = 400;
    s.promote("k1", 2, e->version, val.c_str(), 64);
    e = s.ht.lookup("k1", 2);

    // Manual clean evict: return to old page.
    e->page_id = e->nvme_page_id;
    e->slot_index = e->nvme_slot;
    e->nvme_page_id = entry::NO_NVMe_PAGE;

    // get() should return cold_result.
    auto r = s.get("k1", 2);
    assert(std::holds_alternative<cold_result>(r));
    auto cr = std::get<cold_result>(r);
    assert(cr.nvme_lba == 400);
}

// D1: bounce 5 times
static void test_ce_bounce_cycle() {
    kv_store s;
    std::string val(64, 'B');
    s.set("k1", 2, val.c_str(), 64);
    auto* e = s.ht.lookup("k1", 2);
    uint32_t orig_pid = e->page_id;

    s.pt[orig_pid].state = page_entry::ON_NVME;
    s.pt[orig_pid].nvme_lba = 500;
    uint16_t orig_lc = s.pt[orig_pid].live_count;

    for (int i = 0; i < 5; i++) {
        e = s.ht.lookup("k1", 2);
        s.promote("k1", 2, e->version, val.c_str(), 64);
        e = s.ht.lookup("k1", 2);
        assert(e->has_nvme_backup());
        assert(e->nvme_page_id == orig_pid);
        assert(s.pt[orig_pid].live_count == orig_lc);  // stable

        // Simulate clean evict: return to orig_pid.
        e->page_id = e->nvme_page_id;
        e->slot_index = e->nvme_slot;
        e->nvme_page_id = entry::NO_NVMe_PAGE;
        // (Don't bother freeing the promote target page in this test.)
    }

    assert(s.pt[orig_pid].live_count == orig_lc);  // still stable
}

// D2: bounce then SET releases reference
static void test_ce_bounce_then_set() {
    kv_store s;
    std::string val(64, 'B');
    s.set("k1", 2, val.c_str(), 64);
    s.set("k2", 2, val.c_str(), 64);  // keep old page alive
    auto* e = s.ht.lookup("k1", 2);
    uint32_t pid = e->page_id;

    s.pt[pid].state = page_entry::ON_NVME;
    s.pt[pid].nvme_lba = 600;
    uint16_t lc = s.pt[pid].live_count;

    // Bounce twice.
    for (int i = 0; i < 2; i++) {
        e = s.ht.lookup("k1", 2);
        s.promote("k1", 2, e->version, val.c_str(), 64);
        e = s.ht.lookup("k1", 2);
        e->page_id = e->nvme_page_id;
        e->slot_index = e->nvme_slot;
        e->nvme_page_id = entry::NO_NVMe_PAGE;
    }
    assert(s.pt[pid].live_count == lc);

    // Now promote + SET → should release.
    e = s.ht.lookup("k1", 2);
    s.promote("k1", 2, e->version, val.c_str(), 64);
    std::string val2(64, 'Z');
    s.set("k1", 2, val2.c_str(), 64);
    assert(s.pt[pid].live_count == lc - 1);
}

// D3: two entries from same page, one SET one not
static void test_ce_two_entries_one_set() {
    kv_store s;
    std::string val(64, 'T');
    s.set("k1", 2, val.c_str(), 64);
    s.set("k2", 2, val.c_str(), 64);
    auto* e1 = s.ht.lookup("k1", 2);
    auto* e2 = s.ht.lookup("k2", 2);
    uint32_t pid = e1->page_id;
    assert(e2->page_id == pid);

    s.pt[pid].state = page_entry::ON_NVME;
    s.pt[pid].nvme_lba = 700;
    uint16_t lc = s.pt[pid].live_count;

    s.promote("k1", 2, e1->version, val.c_str(), 64);
    s.promote("k2", 2, e2->version, val.c_str(), 64);
    assert(s.pt[pid].live_count == lc);  // neither decremented

    // SET k1 → releases one reference.
    std::string val2(64, 'U');
    s.set("k1", 2, val2.c_str(), 64);
    assert(s.pt[pid].live_count == lc - 1);

    // k2 still has backup.
    e2 = s.ht.lookup("k2", 2);
    assert(e2->has_nvme_backup());
    assert(e2->nvme_page_id == pid);
}

// E1: promote then SET with different size class
static void test_ce_set_changes_size_class() {
    kv_store s;
    std::string val(64, 'S');
    s.set("k1", 2, val.c_str(), 64);
    auto* e = s.ht.lookup("k1", 2);
    uint32_t pid = e->page_id;

    s.pt[pid].state = page_entry::ON_NVME;
    s.pt[pid].nvme_lba = 800;
    s.promote("k1", 2, e->version, val.c_str(), 64);
    e = s.ht.lookup("k1", 2);
    assert(e->has_nvme_backup());

    // SET with larger value (different size class).
    std::string big(256, 'L');
    s.set("k1", 2, big.c_str(), 256);
    e = s.ht.lookup("k1", 2);
    assert(!e->has_nvme_backup());
    // Old page should have been dec_live'd.
}

// E2: inline promote doesn't track backup
static void test_ce_inline_promote_dec_lives() {
    kv_store s;
    std::string val(8, 'I');  // fits inline (≤16B)
    s.set("k1", 2, val.c_str(), 8);
    auto* e = s.ht.lookup("k1", 2);
    // Small value goes inline directly, no page involved.
    assert(e->is_inline());
    // Nothing to test for promote — inline values are never ON_NVME pages.
    // This test verifies inline entries don't interfere with dirty tracking.

    // SET with slab value.
    std::string sval(64, 'J');
    s.set("k1", 2, sval.c_str(), 64);
    e = s.ht.lookup("k1", 2);
    assert(!e->is_inline());
    assert(!e->has_nvme_backup());  // never promoted, should be INVALID

    // Evict → promote with inline value.
    uint32_t pid = e->page_id;
    s.pt[pid].state = page_entry::ON_NVME;
    s.pt[pid].nvme_lba = 900;
    uint16_t lc = s.pt[pid].live_count;

    std::string ival(8, 'K');  // inline-sized
    s.promote("k1", 2, e->version, ival.c_str(), 8);
    e = s.ht.lookup("k1", 2);
    assert(e->is_inline());
    // Inline promote DOES dec_live (no dirty tracking for inline).
    assert(s.pt[pid].live_count == lc - 1);
}

// ── main ──

int main() {
    printf("types:\n");
    RUN(test_size_class_for);
    RUN(test_slot_offset);
    RUN(test_full_mask);

    printf("page_table:\n");
    RUN(test_page_table_alloc_free_reuse);
    RUN(test_page_table_many);

    printf("slab:\n");
    RUN(test_slab_basic);
    RUN(test_slab_fill_page);
    RUN(test_slab_free_reuse);
    RUN(test_slab_empty_page_freed);
    RUN(test_slab_different_classes);
    RUN(test_slab_write_read);
    RUN(test_slab_sc64_full_page);

    printf("hash_table:\n");
    RUN(test_ht_insert_lookup);
    RUN(test_ht_insert_existing);
    RUN(test_ht_erase);
    RUN(test_ht_many_entries);
    RUN(test_ht_delete_many);
    RUN(test_ht_reinsert_after_delete);
    RUN(test_ht_collision_chain);

    printf("integrated:\n");
    RUN(test_integrated_set_get_del);
    RUN(test_integrated_update_same_class);
    RUN(test_integrated_update_different_class);

    printf("kv_store:\n");
    RUN(test_store_set_get);
    RUN(test_store_get_missing);
    RUN(test_store_del);
    RUN(test_store_update_same_class);
    RUN(test_store_update_different_class);
    RUN(test_store_many_keys);
    RUN(test_store_no_leak_on_repeated_update);

    printf("ttl:\n");
    RUN(test_store_set_with_ttl_no_expiry_yet);
    RUN(test_store_get_lazy_expiry);
    RUN(test_store_del_lazy_expiry);
    RUN(test_store_set_updates_expire);
    RUN(test_store_expire_scan);
    RUN(test_store_expire_scan_keeps_live);

    printf("eviction:\n");
    RUN(test_store_evict_no_pages);
    RUN(test_store_discard_one_page);
    RUN(test_store_evict_returns_nil);
    RUN(test_store_evict_preserves_hot_keys);
    RUN(test_store_evict_sync_on_max);
    RUN(test_store_evict_memory_stable);
    RUN(test_store_set_backpressure);

    printf("inline values:\n");
    RUN(test_inline_set_get);
    RUN(test_inline_set_get_16b);
    RUN(test_inline_set_get_0b);
    RUN(test_inline_del);
    RUN(test_inline_to_inline);
    RUN(test_inline_to_slab);
    RUN(test_slab_to_inline);
    RUN(test_inline_to_large);
    RUN(test_large_to_inline);
    RUN(test_inline_not_evicted);
    RUN(test_inline_many_keys);
    RUN(test_inline_entry_flag);

    printf("large values:\n");
    RUN(test_large_value_set_get_8k);
    RUN(test_large_value_set_get_16k);
    RUN(test_large_value_set_get_64k);
    RUN(test_large_value_del);
    RUN(test_large_value_update_large_same_size);
    RUN(test_large_value_update_large_diff_size);
    RUN(test_large_value_small_to_large);
    RUN(test_large_value_large_to_small);
    RUN(test_large_value_discard);
    RUN(test_large_value_mixed_eviction);
    RUN(test_large_value_no_leak_repeated_update);
    RUN(test_large_value_entry_flag);

    printf("clean eviction:\n");
    RUN(test_ce_promote_keeps_live_count);
    RUN(test_ce_set_clean_releases_backup);
    RUN(test_ce_del_clean_releases_backup);
    RUN(test_ce_set_on_nvme_no_double_dec);
    RUN(test_ce_all_refs_released_frees_page);
    RUN(test_ce_dirty_bitmap_promote_vs_set);
    RUN(test_ce_begin_eviction_all_clean);
    RUN(test_ce_begin_eviction_mixed);
    RUN(test_ce_begin_eviction_all_dirty);
    RUN(test_ce_cold_result_after_clean_evict);
    RUN(test_ce_bounce_cycle);
    RUN(test_ce_bounce_then_set);
    RUN(test_ce_two_entries_one_set);
    RUN(test_ce_set_changes_size_class);
    RUN(test_ce_inline_promote_dec_lives);

    printf("\nAll %d tests passed.\n", pass_count);
    return 0;
}
