#pragma once

#include <cstring>

#include "store/types.hh"
#include "store/page_table.hh"
#include "store/slab.hh"
#include "store/hash_table.hh"

namespace sider::store {

    struct get_result {
        const char* data = nullptr;
        uint16_t len = 0;
        bool found() const { return data != nullptr; }
    };

    struct kv_store {
        page_table     pt;
        slab_allocator slab{pt};
        hash_table     ht;

        get_result get(const char* key, uint16_t key_len) {
            auto* e = ht.lookup(key, key_len);
            if (!e) return {};
            return {slab.slot_ptr(e->page_id, e->slot_index), e->value_len};
        }

        void set(const char* key, uint16_t key_len,
                 const char* value, uint16_t value_len) {
            auto new_sc = size_class_for(value_len);
            auto* e = ht.lookup(key, key_len);

            if (e) {
                auto old_sc = static_cast<size_class_t>(pt[e->page_id].size_class);

                if (old_sc == new_sc) {
                    // Same size class — overwrite in place.
                    std::memcpy(slab.slot_ptr(e->page_id, e->slot_index), value, value_len);
                } else {
                    // Different size class — free old, allocate new.
                    slab.free_slot(e->page_id, e->slot_index);
                    auto ar = slab.allocate(new_sc);
                    std::memcpy(ar.slot_ptr, value, value_len);
                    e->page_id    = ar.page_id;
                    e->slot_index = ar.slot_index;
                }
                e->value_len = value_len;
                e->version++;
            } else {
                // New key.
                auto ar = slab.allocate(new_sc);
                std::memcpy(ar.slot_ptr, value, value_len);

                e = ht.insert(key, key_len);
                e->page_id    = ar.page_id;
                e->slot_index = ar.slot_index;
                e->value_len  = value_len;
                e->version    = 0;
            }
        }

        int del(const char* key, uint16_t key_len) {
            auto* e = ht.lookup(key, key_len);
            if (!e) return 0;

            slab.free_slot(e->page_id, e->slot_index);
            ht.erase(key, key_len);
            return 1;
        }

        uint64_t memory_used_bytes() const { return slab.memory_used_bytes(); }
        uint32_t key_count()         const { return ht.count(); }
    };

} // namespace sider::store
