#ifndef APPS_INCONEL_CORE_CLOCK_CACHE_HH
#define APPS_INCONEL_CORE_CLOCK_CACHE_HH

#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <absl/container/flat_hash_map.h>

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
        absl::flat_hash_map<paddr, uint32_t> index_;
        uint32_t hand_ = 0;
        uint32_t capacity_;
        uint32_t size_ = 0;

        explicit
        clock_cache(uint32_t capacity)
            : slots_(capacity), capacity_(capacity) {
            // capacity == 0 would make put() fall straight into the
            // "cache full" branch and indexing into an empty slots_ vector,
            // crashing on the first call. Reject it at construction time
            // (fail-fast) — runtime callers that propagate user-supplied
            // capacities through build_options must clamp / validate before
            // reaching here.
            if (capacity == 0) {
                throw std::invalid_argument(
                    "clock_cache: capacity must be >= 1");
            }
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
            // Update existing entry — return the displaced buf so the
            // caller can free it. The cache only stores raw char*, so
            // overwriting silently would leak the previous owner's
            // allocation. Two concurrent misses on the same paddr (e.g.
            // value::scheduler's read_q_ → fill_q_ pipeline draining a
            // batch of cold reads in one advance round) hit this path.
            if (auto it = index_.find(key); it != index_.end()) {
                auto& s = slots_[it->second];
                evicted_entry old{ key, s.buf };
                s.buf = buf;
                s.ref = true;
                return old;
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

        // Evict any one entry, returning its (key, buf) so the caller can
        // release the underlying buffer. Used at scheduler teardown to drain
        // the cache. O(1) amortized: pick from index_ rather than scanning.
        // Calling this repeatedly until it returns nullopt empties the cache.
        std::optional<evicted_entry>
        evict_one() {
            if (index_.empty()) return std::nullopt;
            auto it = index_.begin();
            uint32_t idx = it->second;
            auto& s = slots_[idx];
            evicted_entry victim{ s.key, s.buf };
            index_.erase(it);
            s.occupied = false;
            s.buf = nullptr;
            s.ref = false;
            --size_;
            return victim;
        }

        uint32_t size() const { return size_; }
        uint32_t capacity() const { return capacity_; }
    };

}

#endif //APPS_INCONEL_CORE_CLOCK_CACHE_HH
