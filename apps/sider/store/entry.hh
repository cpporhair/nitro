#pragma once

#include <cstdint>
#include <cstring>

#include "store/types.hh"

namespace sider::store {

    // Layout optimized: 48 bytes with 16-byte inline value union at no extra cost.
    //
    // offset  field              size
    // 0       key_data           8
    // 8       expire_at          8
    // 16      key_hash           4
    // 20      value_len          4
    // 24      version            4
    // 28      key_len            2
    // 30      type               1   (bit 7: large, bit 6: inline)
    // 31      _reserved          1
    // 32      union              16  ({page_id,slot_index,last_access} or inline_value[16])
    // Total: 48 bytes, alignment: 8

    struct entry {
        char*    key_data    = nullptr;  // heap-allocated key bytes
        int64_t  expire_at   = -1;       // -1 = no expiry
        uint32_t key_hash    = 0;
        uint32_t value_len   = 0;        // supports large values (> 4KB)
        uint32_t version     = 0;
        uint16_t key_len     = 0;
        uint8_t  type        = 0;        // bit 0-6: type (0=STRING), bit 6: inline, bit 7: large
        uint8_t  _reserved   = 0;

        union {
            struct {
                uint32_t page_id;        // slab/large value page id
                uint8_t  slot_index;     // 0 for large values
                uint8_t  _pad1[3];
                uint32_t last_access;    // per-key LRU for large values
                uint32_t _pad2;
            };
            char inline_value[16];       // for values <= INLINE_VALUE_MAX
        };

        bool is_large()  const { return type & 0x80; }
        void set_large(bool v) { if (v) type |= 0x80; else type &= ~0x80; }

        bool is_inline()  const { return type & 0x40; }
        void set_inline(bool v) { if (v) type |= 0x40; else type &= ~0x40; }

        entry() { std::memset(inline_value, 0, sizeof(inline_value)); }

        ~entry() { delete[] key_data; }

        entry(entry&& o) noexcept
            : key_data(o.key_data), expire_at(o.expire_at),
              key_hash(o.key_hash), value_len(o.value_len),
              version(o.version), key_len(o.key_len), type(o.type) {
            std::memcpy(inline_value, o.inline_value, sizeof(inline_value));
            o.key_data = nullptr;
            o.key_len  = 0;
        }

        entry& operator=(entry&& o) noexcept {
            if (this != &o) {
                delete[] key_data;
                key_data  = o.key_data;
                expire_at = o.expire_at;
                key_hash  = o.key_hash;
                value_len = o.value_len;
                version   = o.version;
                key_len   = o.key_len;
                type      = o.type;
                std::memcpy(inline_value, o.inline_value, sizeof(inline_value));
                o.key_data = nullptr;
                o.key_len  = 0;
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
