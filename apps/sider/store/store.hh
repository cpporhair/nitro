#pragma once

#include <chrono>
#include <cstring>
#include <vector>

#include "store/types.hh"
#include "store/page_table.hh"
#include "store/slab.hh"
#include "store/hash_table.hh"

namespace sider::store {

    static inline int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    struct eviction_config {
        uint64_t memory_limit = 0;    // 0 = no eviction
        double begin_pct  = 0.60;
        double urgent_pct = 0.90;

        uint64_t begin_bytes()  const { return static_cast<uint64_t>(memory_limit * begin_pct); }
        uint64_t urgent_bytes() const { return static_cast<uint64_t>(memory_limit * urgent_pct); }
        uint64_t max_bytes()    const { return memory_limit; }
    };

    struct get_result {
        const char* data = nullptr;
        uint16_t len = 0;
        bool found() const { return data != nullptr; }
    };

    struct kv_store {
        page_table     pt;
        slab_allocator slab{pt};
        hash_table     ht;
        uint32_t       scan_cursor_ = 0;
        uint32_t       access_clock_ = 0;
        uint32_t       rng_state_ = 12345;
        eviction_config evict_cfg_;

        get_result get(const char* key, uint16_t key_len) {
            auto* e = ht.lookup(key, key_len);
            if (!e) return {};

            // Lazy expiry.
            if (e->expire_at >= 0 && now_ms() >= e->expire_at) {
                slab.free_slot(e->page_id, e->slot_index);
                ht.erase(key, key_len);
                return {};
            }

            pt[e->page_id].hotness = ++access_clock_;
            return {slab.slot_ptr(e->page_id, e->slot_index), e->value_len};
        }

        void set(const char* key, uint16_t key_len,
                 const char* value, uint16_t value_len,
                 int64_t expire_at = -1) {
            // Sync eviction at hard limit.
            if (evict_cfg_.memory_limit > 0 &&
                memory_used_bytes() >= evict_cfg_.max_bytes()) {
                while (memory_used_bytes() >= evict_cfg_.urgent_bytes()) {
                    if (evict_one_page() == 0) break;
                }
            }

            auto new_sc = size_class_for(value_len);
            auto* e = ht.lookup(key, key_len);

            if (e) {
                auto old_sc = static_cast<size_class_t>(pt[e->page_id].size_class);

                if (old_sc == new_sc) {
                    std::memcpy(slab.slot_ptr(e->page_id, e->slot_index), value, value_len);
                } else {
                    slab.free_slot(e->page_id, e->slot_index);
                    auto ar = slab.allocate(new_sc);
                    std::memcpy(ar.slot_ptr, value, value_len);
                    e->page_id    = ar.page_id;
                    e->slot_index = ar.slot_index;
                }
                e->value_len  = value_len;
                e->expire_at  = expire_at;
                e->version++;
            } else {
                auto ar = slab.allocate(new_sc);
                std::memcpy(ar.slot_ptr, value, value_len);

                e = ht.insert(key, key_len);
                e->page_id    = ar.page_id;
                e->slot_index = ar.slot_index;
                e->value_len  = value_len;
                e->expire_at  = expire_at;
                e->version    = 0;
            }

            pt[e->page_id].hotness = ++access_clock_;
        }

        int del(const char* key, uint16_t key_len) {
            auto* e = ht.lookup(key, key_len);
            if (!e) return 0;

            // Lazy expiry on del: expired key counts as not found.
            if (e->expire_at >= 0 && now_ms() >= e->expire_at) {
                slab.free_slot(e->page_id, e->slot_index);
                ht.erase(key, key_len);
                return 0;
            }

            slab.free_slot(e->page_id, e->slot_index);
            ht.erase(key, key_len);
            return 1;
        }

        // Active expiry: sample up to `samples` slots, delete expired entries.
        int expire_scan(int samples = 20) {
            if (ht.count() == 0) return 0;
            auto cap = ht.capacity();
            auto ts = now_ms();
            int expired = 0;

            for (int i = 0; i < samples; i++) {
                if (scan_cursor_ >= cap) scan_cursor_ = 0;
                auto* e = ht.entry_at(scan_cursor_);
                scan_cursor_++;
                if (!e) continue;
                if (e->expire_at >= 0 && ts >= e->expire_at) {
                    slab.free_slot(e->page_id, e->slot_index);
                    ht.erase(e->key_data, e->key_len);
                    expired++;
                }
            }
            return expired;
        }

        // ── Eviction ──

        uint32_t fast_rand() {
            rng_state_ ^= rng_state_ << 13;
            rng_state_ ^= rng_state_ >> 17;
            rng_state_ ^= rng_state_ << 5;
            return rng_state_;
        }

        // Sample random IN_MEMORY pages, return the coldest page_id.
        // Returns UINT32_MAX if no pages to evict.
        uint32_t sample_coldest_page(int samples = 5) {
            auto page_count = pt.size();
            if (page_count == 0) return UINT32_MAX;

            uint32_t best_id = UINT32_MAX;
            uint32_t best_hotness = UINT32_MAX;
            int found = 0;

            for (int attempt = 0; attempt < samples * 4 && found < samples; attempt++) {
                uint32_t idx = fast_rand() % page_count;
                auto& pe = pt[idx];
                if (pe.state != page_entry::IN_MEMORY) continue;
                found++;
                if (pe.hotness < best_hotness) {
                    best_hotness = pe.hotness;
                    best_id = idx;
                }
            }
            return best_id;
        }

        // Evict the coldest sampled page: remove all its entries, free page.
        // Returns number of entries evicted, or 0 if nothing to evict.
        int evict_one_page() {
            uint32_t victim = sample_coldest_page();
            if (victim == UINT32_MAX) return 0;

            // Collect keys on this page (copies, since erase invalidates pointers).
            struct key_copy { char* data; uint16_t len; };
            std::vector<key_copy> keys;

            auto cap = ht.capacity();
            for (uint32_t i = 0; i < cap; i++) {
                auto* e = ht.entry_at(i);
                if (!e || e->page_id != victim) continue;
                auto* kd = new char[e->key_len];
                std::memcpy(kd, e->key_data, e->key_len);
                keys.push_back({kd, e->key_len});
            }

            // Erase all entries from hash table.
            for (auto& k : keys) {
                ht.erase(k.data, k.len);
                delete[] k.data;
            }

            // Free the entire page.
            slab.evict_page(victim);

            return static_cast<int>(keys.size());
        }

        uint64_t memory_used_bytes() const { return slab.memory_used_bytes(); }
        uint32_t key_count()         const { return ht.count(); }
    };

} // namespace sider::store
