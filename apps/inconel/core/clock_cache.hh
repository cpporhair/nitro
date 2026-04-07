#ifndef APPS_INCONEL_CORE_CLOCK_CACHE_HH
#define APPS_INCONEL_CORE_CLOCK_CACHE_HH

#include <cassert>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "../format/types.hh"

namespace apps::inconel::core {

    using format::paddr;

    struct evicted_entry {
        paddr key;
        char* buf;
    };

    // ── Clock (second-chance) cache ──
    //
    // Fixed-capacity slot array + ref bit per slot + circular hand.
    // get(): set ref=1 (single byte write, 1 cache line touch)
    // put() on full cache: advance hand, clear ref=1 to ref=0, evict first ref=0
    //
    // Hot pages (root, upper internal) are touched every lookup, so their
    // ref bit is constantly refreshed and they are never evicted.

    struct clock_cache {
        struct slot {
            paddr key;
            char* buf = nullptr;
            bool occupied = false;
            bool ref = false;
        };

        std::vector<slot> slots_;
        std::unordered_map<paddr, uint32_t> index_;
        uint32_t hand_ = 0;
        uint32_t capacity_;
        uint32_t size_ = 0;

        explicit
        clock_cache(uint32_t capacity)
            : slots_(capacity), capacity_(capacity) {
            index_.reserve(capacity);
        }

        const char*
        get(paddr key) {
            auto it = index_.find(key);
            if (it == index_.end()) return nullptr;
            slots_[it->second].ref = true;
            return slots_[it->second].buf;
        }

        bool
        contains(paddr key) const {
            return index_.find(key) != index_.end();
        }

        std::optional<evicted_entry>
        put(paddr key, char* buf) {
            // Update existing entry (rare path).
            if (auto it = index_.find(key); it != index_.end()) {
                auto& s = slots_[it->second];
                s.buf = buf;
                s.ref = true;
                return std::nullopt;
            }

            // Free slot available — fill it.
            if (size_ < capacity_) {
                uint32_t idx = size_++;
                auto& s = slots_[idx];
                s.key = key;
                s.buf = buf;
                s.occupied = true;
                s.ref = false;
                index_[key] = idx;
                return std::nullopt;
            }

            // Cache full — clock sweep to find victim.
            for (;;) {
                auto& s = slots_[hand_];
                if (!s.ref) {
                    // Evict this slot.
                    evicted_entry victim{ s.key, s.buf };
                    index_.erase(s.key);

                    s.key = key;
                    s.buf = buf;
                    s.ref = false;
                    index_[key] = hand_;

                    hand_ = (hand_ + 1) % capacity_;
                    return victim;
                }
                s.ref = false;
                hand_ = (hand_ + 1) % capacity_;
            }
        }

        uint32_t size() const { return size_; }
        uint32_t capacity() const { return capacity_; }
    };

}

#endif //APPS_INCONEL_CORE_CLOCK_CACHE_HH
