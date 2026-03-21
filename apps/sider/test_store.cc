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
    auto r = s.get("key", 3);
    assert(r.found());
    assert(r.len == 5);
    assert(std::memcmp(r.data, "value", 5) == 0);
}

static void test_store_get_missing() {
    kv_store s;
    assert(!s.get("nope", 4).found());
}

static void test_store_del() {
    kv_store s;
    s.set("k", 1, "v", 1);
    assert(s.del("k", 1) == 1);
    assert(s.del("k", 1) == 0);
    assert(!s.get("k", 1).found());
}

static void test_store_update_same_class() {
    kv_store s;
    s.set("k", 1, "aaa", 3);
    s.set("k", 1, "bbb", 3);
    auto r = s.get("k", 1);
    assert(r.len == 3);
    assert(std::memcmp(r.data, "bbb", 3) == 0);
}

static void test_store_update_different_class() {
    kv_store s;
    s.set("k", 1, "small", 5);   // SC_64

    char big[200];
    std::memset(big, 'X', 200);
    s.set("k", 1, big, 200);     // SC_256

    auto r = s.get("k", 1);
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
        auto r = s.get(key.c_str(), static_cast<uint16_t>(key.size()));
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
    auto r = s.get("k", 1);
    assert(r.found());
    assert(r.len == 1);
}

static void test_store_get_lazy_expiry() {
    kv_store s;
    // Set with expire_at in the past → GET should return not found (lazy expiry).
    int64_t past = now_ms() - 1;
    s.set("k", 1, "v", 1, past);
    auto r = s.get("k", 1);
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
    auto r = s.get("k", 1);
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
    auto r = s.get("live_0", 6);
    assert(r.found());
}

// ── eviction ──

static void test_store_discard_one_page() {
    kv_store s;
    // Insert 200 keys (SC_64: 64 slots/page → ~4 pages).
    for (int i = 0; i < 200; i++) {
        auto key = "k" + std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), "value", 5);
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
    int missing = 0;
    for (int i = 0; i < 200; i++) {
        auto key = "k" + std::to_string(i);
        if (!s.get(key.c_str(), static_cast<uint16_t>(key.size())).found())
            missing++;
    }
    assert(missing == evicted);
}

static void test_store_evict_returns_nil() {
    kv_store s;
    s.set("only", 4, "v", 1);
    assert(s.key_count() == 1);

    s.discard_one_page();
    assert(!s.get("only", 4).found());
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
            s.get(key.c_str(), static_cast<uint16_t>(key.size()));
        }
    }

    // Evict 5 pages — should target cold pages (4-12).
    for (int i = 0; i < 5; i++)
        s.discard_one_page();

    // All 50 hot keys should survive (5 cold pages evicted from 9 cold pages).
    int hot_found = 0;
    for (int i = 0; i < 50; i++) {
        auto key = "k" + std::to_string(i);
        if (s.get(key.c_str(), static_cast<uint16_t>(key.size())).found())
            hot_found++;
    }
    assert(hot_found >= 45);  // conservative margin for sampling randomness
}

static void test_store_evict_sync_on_max() {
    kv_store s;
    // 10 pages limit. SC_64: 64 slots/page → 640 keys at capacity.
    s.evict_cfg_ = {10 * PAGE_SIZE, 0.60, 0.90};

    // Insert 2000 keys — sync eviction in set() keeps memory bounded.
    for (int i = 0; i < 2000; i++) {
        auto key = "k" + std::to_string(i);
        s.set(key.c_str(), static_cast<uint16_t>(key.size()), "value", 5);
    }

    // Memory should not exceed limit + 1 page (overshoot from final alloc).
    assert(s.memory_used_bytes() <= 11 * PAGE_SIZE);
    // Some keys were evicted.
    assert(s.key_count() < 2000);
    assert(s.key_count() > 0);
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

static void test_store_evict_no_pages() {
    kv_store s;
    // Nothing to evict.
    assert(s.discard_one_page() == 0);
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

    printf("\nAll %d tests passed.\n", pass_count);
    return 0;
}
