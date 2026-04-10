#ifndef APPS_INCONEL_CORE_CLOCK_CACHE_HH
#define APPS_INCONEL_CORE_CLOCK_CACHE_HH

#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "../memory/frame.hh"

namespace apps::inconel::core {

    using memory::frame_id;
    using memory::frame_state;
    using memory::page_frame;
    using memory::frame_pin;

    // ── Clock (second-chance) cache ──
    //
    // Fixed-capacity slot array + ref bit per slot + circular hand.
    // pin(): find + set ref=1 + return RAII frame_pin (pin_count++)
    // put() on full cache: advance hand, clear ref=1 to ref=0, skip
    //   pinned entries, evict first ref=0 + pin_count=0
    //
    // Hot pages (root, upper internal) are touched every lookup, so their
    // ref bit is constantly refreshed and they are never evicted.
    //
    // Ownership: the cache stores page_frame* and never frees them. The
    // caller is sole owner of backing buffers and frame descriptors; the
    // cache is a non-owning index with pin-awareness.

    struct clock_cache {
        struct slot {
            frame_id key{};
            page_frame* frame = nullptr;
            bool occupied = false;
            bool ref = false;
        };

        std::vector<slot> slots_;
        absl::flat_hash_map<frame_id, uint32_t> index_;
        uint32_t hand_ = 0;
        uint32_t capacity_;
        uint32_t size_ = 0;

        explicit
        clock_cache(uint32_t capacity)
            : slots_(capacity), capacity_(capacity) {
            if (capacity == 0) {
                throw std::invalid_argument(
                    "clock_cache: capacity must be >= 1");
            }
            index_.reserve(capacity);
        }

        // pin: look up a frame by id, mark as recently used, return an
        // RAII pin that increments pin_count. Returns an empty pin (frame
        // == nullptr) on miss.
        frame_pin
        pin(frame_id id) {
            auto it = index_.find(id);
            if (it == index_.end()) return frame_pin{nullptr};
            auto& s = slots_[it->second];
            s.ref = true;
            return frame_pin{s.frame};
        }

        bool
        contains(frame_id id) const {
            return index_.find(id) != index_.end();
        }

        // put: insert a clean_readonly frame into the cache. Returns the
        // displaced page_frame* in three cases:
        //   - key already present → old frame for that key
        //   - cache full, evictable victim found → victim frame
        //   - cache full, all entries pinned → the input frame f (rejected)
        // Returns nullopt when inserted into a free slot.
        std::optional<page_frame*>
        put(page_frame* f) {
            if (!f || f->st != frame_state::clean_readonly) {
                throw std::invalid_argument(
                    "clock_cache::put: frame must be non-null and clean_readonly");
            }

            // Update existing entry — return the displaced frame so the
            // caller can free it. If the old frame is still pinned, reject
            // the replacement: cache keeps the old frame, caller gets f
            // back (same semantics as "cannot evict, insertion rejected").
            if (auto it = index_.find(f->id); it != index_.end()) {
                auto& s = slots_[it->second];
                if (s.frame && s.frame->pin_count > 0) return f;
                page_frame* old = s.frame;
                s.frame = f;
                s.ref = true;
                return old;
            }

            // Free slot available — fill it.
            if (size_ < capacity_) {
                uint32_t idx = size_++;
                auto& s = slots_[idx];
                s.key = f->id;
                s.frame = f;
                s.occupied = true;
                s.ref = false;
                index_[f->id] = idx;
                return std::nullopt;
            }

            // Cache full — clock sweep to find victim.
            // Skip entries with pin_count > 0. Limit to 2 full sweeps
            // (first clears ref bits, second finds a ref=0 victim) to
            // guarantee termination when all entries are pinned.
            for (uint32_t sweep = 0; sweep < 2 * capacity_; ++sweep) {
                auto& s = slots_[hand_];
                hand_ = (hand_ + 1) % capacity_;

                // Pinned frames must not be evicted.
                if (s.frame && s.frame->pin_count > 0) continue;

                if (!s.ref) {
                    // Evict this slot.
                    page_frame* victim = s.frame;
                    index_.erase(s.key);

                    s.key = f->id;
                    s.frame = f;
                    s.ref = false;
                    index_[f->id] = (hand_ + capacity_ - 1) % capacity_;

                    return victim;
                }
                s.ref = false;
            }

            // All entries pinned — cannot evict; reject insertion.
            return f;
        }

        // drain_one: teardown-only drain. Pulls one entry out regardless
        // of ref/pin state and returns its page_frame* so the caller can
        // free the backing buffer and descriptor. Returns nullopt when
        // empty. Calling repeatedly until nullopt empties the cache.
        std::optional<page_frame*>
        drain_one() {
            if (index_.empty()) return std::nullopt;
            auto it = index_.begin();
            uint32_t idx = it->second;
            auto& s = slots_[idx];
            page_frame* victim = s.frame;
            index_.erase(it);
            s.occupied = false;
            s.frame = nullptr;
            s.ref = false;
            --size_;
            return victim;
        }

        uint32_t size() const { return size_; }
        uint32_t capacity() const { return capacity_; }
    };

}

#endif //APPS_INCONEL_CORE_CLOCK_CACHE_HH
