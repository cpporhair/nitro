#pragma once

#include <cstdint>
#include <cstring>

namespace sider::store {

    struct entry {
        uint32_t key_hash    = 0;
        uint16_t key_len     = 0;
        uint16_t value_len   = 0;
        uint32_t page_id     = 0;
        uint8_t  slot_index  = 0;
        uint8_t  type        = 0;       // 0 = STRING
        uint32_t last_access = 0;
        uint32_t version     = 0;
        int64_t  expire_at   = -1;      // -1 = no expiry
        char*    key_data    = nullptr;  // heap-allocated key bytes

        entry() = default;

        ~entry() { delete[] key_data; }

        entry(entry&& o) noexcept
            : key_hash(o.key_hash), key_len(o.key_len), value_len(o.value_len),
              page_id(o.page_id), slot_index(o.slot_index), type(o.type),
              last_access(o.last_access), version(o.version),
              expire_at(o.expire_at), key_data(o.key_data) {
            o.key_data = nullptr;
            o.key_len  = 0;
        }

        entry& operator=(entry&& o) noexcept {
            if (this != &o) {
                delete[] key_data;
                key_hash    = o.key_hash;
                key_len     = o.key_len;
                value_len   = o.value_len;
                page_id     = o.page_id;
                slot_index  = o.slot_index;
                type        = o.type;
                last_access = o.last_access;
                version     = o.version;
                expire_at   = o.expire_at;
                key_data    = o.key_data;
                o.key_data  = nullptr;
                o.key_len   = 0;
            }
            return *this;
        }

        entry(const entry&)            = delete;
        entry& operator=(const entry&) = delete;

        bool empty() const { return key_data == nullptr; }

        bool key_equals(const char* k, uint16_t klen) const {
            return key_len == klen && std::memcmp(key_data, k, klen) == 0;
        }

        void set_key(const char* k, uint16_t klen) {
            delete[] key_data;
            key_data = new char[klen];
            std::memcpy(key_data, k, klen);
            key_len = klen;
        }

        void free_key() {
            delete[] key_data;
            key_data = nullptr;
            key_len  = 0;
        }
    };

} // namespace sider::store
