#ifndef APPS_INCONEL_CORE_CLOCK_CACHE_HH
#define APPS_INCONEL_CORE_CLOCK_CACHE_HH

#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "../memory/frame.hh"
#include "./panic.hh"

namespace apps::inconel::core {

    using memory::frame_id;
    using memory::frame_state;
    using memory::frame_pin;
    using memory::page_frame;
    using memory::segmented_frame_pin;
    using memory::segmented_page_frame;

    // ── Clock (second-chance) cache ──
    //
    // Fixed-capacity slot array + ref bit per slot + circular hand.
    // pin(): find + set ref=1 + return RAII pin_type (pin_count++)
    // put() on full cache: advance hand, clear ref=1 to ref=0, skip
    //   pinned entries, evict first ref=0 + pin_count=0
    //
    // Hot pages (root, upper internal) are touched every lookup, so their
    // ref bit is constantly refreshed and they are never evicted.
    //
    // Ownership: the cache stores frame_type* and never frees them. The
    // caller is sole owner of backing buffers and frame descriptors; the
    // cache is a non-owning index with pin-awareness.

    template <typename FrameT = page_frame,
              typename PinT = frame_pin>
    struct basic_clock_cache {
        using frame_type = FrameT;
        using pin_type = PinT;

        struct slot {
            frame_id key{};
            frame_type* frame = nullptr;
            bool occupied = false;
            bool ref = false;
        };

        std::vector<slot> slots_;
        absl::flat_hash_map<frame_id, uint32_t> index_;
        uint32_t hand_ = 0;
        uint32_t capacity_;
        uint32_t size_ = 0;

        explicit
        basic_clock_cache(uint32_t capacity)
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
        pin_type
        pin(frame_id id) {
            auto it = index_.find(id);
            if (it == index_.end()) return pin_type{nullptr};
            auto& s = slots_[it->second];
            s.ref = true;
            return pin_type{s.frame};
        }

        std::optional<frame_type*>
        take(frame_id id) {
            auto it = index_.find(id);
            if (it == index_.end()) return std::nullopt;
            auto& s = slots_[it->second];
            if (s.frame && s.frame->pin_count > 0) {
                panic_inconsistency(
                    "clock_cache::take",
                    "attempted to take pinned frame dev=%u lba=%lu span=%u dom=%u pin_count=%u",
                    static_cast<unsigned>(id.base.device_id),
                    static_cast<unsigned long>(id.base.lba),
                    static_cast<unsigned>(id.span_lbas),
                    static_cast<unsigned>(id.dom),
                    static_cast<unsigned>(s.frame->pin_count));
            }
            frame_type* out = s.frame;
            index_.erase(it);
            s.frame = nullptr;
            s.occupied = false;
            s.ref = false;
            --size_;
            return out;
        }

        bool
        contains(frame_id id) const {
            return index_.find(id) != index_.end();
        }

        // put: insert a clean_readonly frame into the cache. Returns the
        // displaced frame_type* in three cases:
        //   - key already present → old frame for that key
        //   - cache full, evictable victim found → victim frame
        //   - cache full, all entries pinned → the input frame f (rejected)
        // Returns nullopt when inserted into a free slot.
        std::optional<frame_type*>
        put(frame_type* f) {
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
                frame_type* old = s.frame;
                s.frame = f;
                s.ref = true;
                return old;
            }

            // Free slot available — fill it.
            if (size_ < capacity_) {
                uint32_t idx = capacity_;
                for (uint32_t i = 0; i < capacity_; ++i) {
                    if (!slots_[i].occupied) {
                        idx = i;
                        break;
                    }
                }
                assert(idx < capacity_ && "clock_cache: size_/occupied mismatch");
                ++size_;
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
                    frame_type* victim = s.frame;
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
        // of ref/pin state and returns its frame_type* so the caller can
        // free the backing buffer and descriptor. Returns nullopt when
        // empty. Calling repeatedly until nullopt empties the cache.
        std::optional<frame_type*>
        drain_one() {
            if (index_.empty()) return std::nullopt;
            auto it = index_.begin();
            uint32_t idx = it->second;
            auto& s = slots_[idx];
            frame_type* victim = s.frame;
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

    using clock_cache = basic_clock_cache<>;
    using segmented_clock_cache =
        basic_clock_cache<segmented_page_frame, segmented_frame_pin>;

}

#endif //APPS_INCONEL_CORE_CLOCK_CACHE_HH
