#pragma once

#include <chrono>
#include <cstring>

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

        get_result get(const char* key, uint16_t key_len) {
            auto* e = ht.lookup(key, key_len);
            if (!e) return {};

            // Lazy expiry.
            if (e->expire_at >= 0 && now_ms() >= e->expire_at) {
                slab.free_slot(e->page_id, e->slot_index);
                ht.erase(key, key_len);
                return {};
            }

            return {slab.slot_ptr(e->page_id, e->slot_index), e->value_len};
        }

        void set(const char* key, uint16_t key_len,
                 const char* value, uint16_t value_len,
                 int64_t expire_at = -1) {
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
        // Returns the number of expired entries deleted.
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

        uint64_t memory_used_bytes() const { return slab.memory_used_bytes(); }
        uint32_t key_count()         const { return ht.count(); }
    };

} // namespace sider::store
