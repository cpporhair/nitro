#pragma once

#include <cstdint>
#include <cstring>
#include <utility>

#include "store/entry.hh"

namespace sider::store {

    // FNV-1a hash.
    static inline uint32_t hash_key(const char* data, uint16_t len) {
        uint32_t h = 2166136261u;
        for (uint16_t i = 0; i < len; i++) {
            h ^= static_cast<uint8_t>(data[i]);
            h *= 16777619u;
        }
        return h;
    }

    // Open-addressing Robin Hood hash table.
    // Single-threaded, per-core. No locking.
    struct hash_table {

        struct slot {
            entry   e;
            uint8_t psl = 0;   // 0 = empty, 1 = at ideal pos, 2 = 1 away, ...
        };

        slot*    slots_    = nullptr;
        uint32_t capacity_ = 0;
        uint32_t count_    = 0;
        uint32_t mask_     = 0;

        static constexpr uint32_t INITIAL_CAPACITY = 16;

        hash_table()  { alloc_slots(INITIAL_CAPACITY); }
        ~hash_table() { delete[] slots_; }

        hash_table(const hash_table&)            = delete;
        hash_table& operator=(const hash_table&) = delete;

        uint32_t count()    const { return count_; }
        uint32_t capacity() const { return capacity_; }

        // Access entry at slot position (for scanning). Returns nullptr if empty.
        entry* entry_at(uint32_t pos) {
            if (pos >= capacity_ || slots_[pos].psl == 0) return nullptr;
            return &slots_[pos].e;
        }

        // Lookup by key. Returns nullptr if not found.
        entry* lookup(const char* key, uint16_t key_len) {
            if (count_ == 0) return nullptr;

            uint32_t h   = hash_key(key, key_len);
            uint32_t pos = h & mask_;
            uint8_t  d   = 1;

            for (;;) {
                auto& s = slots_[pos];
                if (s.psl == 0)  return nullptr;   // empty
                if (s.psl < d)   return nullptr;   // Robin Hood invariant
                if (s.e.key_hash == h && s.e.key_equals(key, key_len))
                    return &s.e;
                pos = (pos + 1) & mask_;
                d++;
            }
        }

        // Insert a new key or return the existing entry.
        // Caller fills value-related fields (page_id, slot_index, value_len, ...).
        // WARNING: returned pointer is invalidated by a subsequent insert that triggers resize.
        entry* insert(const char* key, uint16_t key_len) {
            uint32_t h = hash_key(key, key_len);

            // Lookup with pre-computed hash.
            if (count_ > 0) {
                uint32_t pos = h & mask_;
                uint8_t  d   = 1;
                for (;;) {
                    auto& s = slots_[pos];
                    if (s.psl == 0)  break;
                    if (s.psl < d)   break;
                    if (s.e.key_hash == h && s.e.key_equals(key, key_len))
                        return &s.e;
                    pos = (pos + 1) & mask_;
                    d++;
                }
            }

            if (count_ * 4 >= capacity_ * 3)   // load > 0.75
                grow();

            return robin_insert(h, key, key_len);
        }

        // Lookup by (key_hash, page_id, slot_index) — O(1) amortized.
        // Used by begin_eviction to return clean entries to old NVMe pages.
        entry* lookup_by_page(uint32_t key_hash, uint32_t page_id, uint8_t slot_index) {
            if (count_ == 0) return nullptr;

            uint32_t pos = key_hash & mask_;
            uint8_t  d   = 1;

            for (;;) {
                auto& s = slots_[pos];
                if (s.psl == 0)  return nullptr;
                if (s.psl < d)   return nullptr;
                if (s.e.key_hash == key_hash && !s.e.is_inline() &&
                    s.e.page_id == page_id && s.e.slot_index == slot_index)
                    return &s.e;
                pos = (pos + 1) & mask_;
                d++;
            }
        }

        // Erase by (key_hash, page_id, slot_index) — O(1) amortized, no full key needed.
        // Used by discard_page_entries for fast page eviction.
        bool erase_by_page(uint32_t key_hash, uint32_t page_id, uint8_t slot_index) {
            if (count_ == 0) return false;

            uint32_t pos = key_hash & mask_;
            uint8_t  d   = 1;

            for (;;) {
                auto& s = slots_[pos];
                if (s.psl == 0)  return false;
                if (s.psl < d)   return false;
                if (s.e.key_hash == key_hash && !s.e.is_inline() &&
                    s.e.page_id == page_id && s.e.slot_index == slot_index) {
                    s.e.free_key();
                    s.psl = 0;
                    backward_shift(pos);
                    count_--;
                    return true;
                }
                pos = (pos + 1) & mask_;
                d++;
            }
        }

        // Erase by key. Returns true if found and removed.
        bool erase(const char* key, uint16_t key_len) {
            if (count_ == 0) return false;

            uint32_t h   = hash_key(key, key_len);
            uint32_t pos = h & mask_;
            uint8_t  d   = 1;

            for (;;) {
                auto& s = slots_[pos];
                if (s.psl == 0)  return false;
                if (s.psl < d)   return false;
                if (s.e.key_hash == h && s.e.key_equals(key, key_len)) {
                    // Found — remove and backward-shift.
                    s.e.free_key();
                    s.psl = 0;
                    backward_shift(pos);
                    count_--;
                    return true;
                }
                pos = (pos + 1) & mask_;
                d++;
            }
        }

    private:
        void alloc_slots(uint32_t cap) {
            capacity_ = cap;
            mask_     = cap - 1;
            count_    = 0;
            slots_    = new slot[cap]{};
        }

        entry* robin_insert(uint32_t h, const char* key, uint16_t key_len) {
            slot ns{};
            ns.e.key_hash = h;
            ns.e.set_key(key, key_len);
            ns.psl = 1;

            entry* result = nullptr;
            uint32_t pos = h & mask_;
            for (;;) {
                auto& s = slots_[pos];
                if (s.psl == 0) {
                    s = std::move(ns);
                    count_++;
                    return result ? result : &s.e;
                }
                if (ns.psl > s.psl) {
                    if (!result) result = &s.e;  // our entry lands here after swap
                    std::swap(ns, s);
                }
                pos = (pos + 1) & mask_;
                ns.psl++;
            }
        }

        void backward_shift(uint32_t pos) {
            uint32_t prev = pos;
            uint32_t cur  = (pos + 1) & mask_;
            while (slots_[cur].psl > 1) {
                slots_[prev] = std::move(slots_[cur]);
                slots_[prev].psl--;
                slots_[cur].psl = 0;
                prev = cur;
                cur  = (cur + 1) & mask_;
            }
        }

        void grow() {
            auto* old = slots_;
            auto  old_cap = capacity_;

            capacity_ *= 2;
            mask_      = capacity_ - 1;
            slots_     = new slot[capacity_]{};
            count_     = 0;

            for (uint32_t i = 0; i < old_cap; i++) {
                if (old[i].psl == 0) continue;
                auto& oe = old[i].e;
                uint32_t h   = oe.key_hash;
                uint32_t pos = h & mask_;

                slot ns{};
                ns.e   = std::move(oe);
                ns.psl = 1;

                for (;;) {
                    auto& s = slots_[pos];
                    if (s.psl == 0) {
                        s = std::move(ns);
                        count_++;
                        break;
                    }
                    if (ns.psl > s.psl)
                        std::swap(ns, s);
                    pos = (pos + 1) & mask_;
                    ns.psl++;
                }
            }

            delete[] old;
        }
    };

} // namespace sider::store
