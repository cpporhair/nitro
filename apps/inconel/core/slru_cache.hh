#ifndef APPS_INCONEL_CORE_SLRU_CACHE_HH
#define APPS_INCONEL_CORE_SLRU_CACHE_HH

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
    using memory::page_frame;
    using memory::frame_pin;

    // ── Segmented LRU cache ──
    //
    // Two intrusive doubly-linked lists (probation + protected) backed by a
    // contiguous node array — prev/next are uint32_t indices, not pointers,
    // so cache locality on list operations is much better than pointer-based
    // linked lists.
    //
    // Insertion: new entries enter probation at head.
    // First-time hit (in probation): promote to protected head; if protected
    // is full, demote protected tail back to probation head.
    // Repeat hit (in protected): move to protected head.
    // Eviction: probation tail (single-access pages get evicted first),
    //   skipping entries with pin_count > 0.
    //
    // Scan resistance: a one-pass scan only fills probation, never threatens
    // protected segment where hot pages live.

    struct slru_cache {
        static constexpr uint32_t NIL = UINT32_MAX;

        struct node {
            frame_id key{};
            page_frame* frame = nullptr;
            uint32_t prev = NIL;
            uint32_t next = NIL;
            bool in_protected = false;
            bool occupied = false;
        };

        std::vector<node> nodes_;
        absl::flat_hash_map<frame_id, uint32_t> index_;

        uint32_t prot_head_ = NIL;
        uint32_t prot_tail_ = NIL;
        uint32_t prob_head_ = NIL;
        uint32_t prob_tail_ = NIL;
        uint32_t prot_size_ = 0;
        uint32_t prob_size_ = 0;
        uint32_t prot_cap_;
        uint32_t prob_cap_;

        uint32_t free_head_ = NIL;  // free node list

        explicit
        slru_cache(uint32_t capacity, float prot_ratio = 0.8f)
            : nodes_(capacity)
            , prot_cap_(static_cast<uint32_t>(capacity * prot_ratio))
            , prob_cap_(capacity - static_cast<uint32_t>(capacity * prot_ratio)) {
            if (capacity < 2) {
                throw std::invalid_argument(
                    "slru_cache: capacity must be >= 2");
            }
            if (prot_cap_ == 0 || prob_cap_ == 0) {
                throw std::invalid_argument(
                    "slru_cache: prot_ratio must yield "
                    "prot_cap >= 1 and prob_cap >= 1");
            }
            index_.reserve(capacity);
            // Build free list.
            for (uint32_t i = 0; i < capacity; ++i) {
                nodes_[i].next = (i + 1 < capacity) ? (i + 1) : NIL;
            }
            free_head_ = 0;
        }

        // pin: look up a frame by id, promote it (probation→protected or
        // protected→head), return an RAII pin that increments pin_count.
        // Returns an empty pin (frame == nullptr) on miss.
        frame_pin
        pin(frame_id id) {
            auto it = index_.find(id);
            if (it == index_.end()) return frame_pin{nullptr};
            uint32_t idx = it->second;
            auto& n = nodes_[idx];

            if (n.in_protected) {
                move_to_protected_head(idx);
            } else {
                unlink_probation(idx);
                promote_to_protected(idx);
            }
            return frame_pin{n.frame};
        }

        std::optional<page_frame*>
        take(frame_id id) {
            auto it = index_.find(id);
            if (it == index_.end()) return std::nullopt;
            uint32_t idx = it->second;
            auto& n = nodes_[idx];
            if (n.frame && n.frame->pin_count > 0) {
                panic_inconsistency(
                    "slru_cache::take",
                    "attempted to take pinned frame dev=%u lba=%lu span=%u dom=%u pin_count=%u",
                    static_cast<unsigned>(id.base.device_id),
                    static_cast<unsigned long>(id.base.lba),
                    static_cast<unsigned>(id.span_lbas),
                    static_cast<unsigned>(id.dom),
                    static_cast<unsigned>(n.frame->pin_count));
            }
            page_frame* out = n.frame;
            if (n.in_protected) unlink_protected(idx);
            else unlink_probation(idx);
            index_.erase(it);
            free_node(idx);
            return out;
        }

        bool
        contains(frame_id id) const {
            return index_.find(id) != index_.end();
        }

        // put: insert a clean_readonly frame. Returns the displaced
        // page_frame* on replacement or eviction; returns f itself if
        // probation is full and all entries are pinned (rejected).
        // Returns nullopt when inserted into a free slot.
        std::optional<page_frame*>
        put(page_frame* f) {
            if (!f || f->st != frame_state::clean_readonly) {
                throw std::invalid_argument(
                    "slru_cache::put: frame must be non-null and clean_readonly");
            }

            // Update existing entry — return the displaced frame. If the
            // old frame is still pinned, reject the replacement: cache
            // keeps the old frame (no promote/move), caller gets f back.
            if (auto it = index_.find(f->id); it != index_.end()) {
                uint32_t idx = it->second;
                page_frame* old = nodes_[idx].frame;
                if (old && old->pin_count > 0) return f;
                nodes_[idx].frame = f;
                if (nodes_[idx].in_protected) {
                    move_to_protected_head(idx);
                } else {
                    unlink_probation(idx);
                    promote_to_protected(idx);
                }
                return old;
            }

            std::optional<page_frame*> result;

            // Probation full → evict the LRU non-pinned probation entry.
            if (prob_size_ >= prob_cap_) {
                uint32_t victim_idx = find_evictable_probation();
                if (victim_idx != NIL) {
                    auto& v = nodes_[victim_idx];
                    result = v.frame;
                    unlink_probation(victim_idx);
                    index_.erase(v.key);
                    free_node(victim_idx);
                } else {
                    // All probation entries pinned — reject insertion.
                    return f;
                }
            }

            // Allocate node from free list and insert at probation head.
            uint32_t idx = alloc_node();
            auto& n = nodes_[idx];
            n.key = f->id;
            n.frame = f;
            n.in_protected = false;
            n.occupied = true;
            link_probation_head(idx);
            index_[f->id] = idx;
            return result;
        }

        // drain_one: teardown-only drain. Pulls one entry regardless of
        // pin state and returns its page_frame*. Probation tail first, then
        // protected tail. Returns nullopt when empty.
        std::optional<page_frame*>
        drain_one() {
            if (prob_tail_ != NIL) {
                uint32_t idx = prob_tail_;
                auto& n = nodes_[idx];
                page_frame* victim = n.frame;
                unlink_probation(idx);
                index_.erase(n.key);
                free_node(idx);
                return victim;
            }
            if (prot_tail_ != NIL) {
                uint32_t idx = prot_tail_;
                auto& n = nodes_[idx];
                page_frame* victim = n.frame;
                unlink_protected(idx);
                index_.erase(n.key);
                free_node(idx);
                return victim;
            }
            return std::nullopt;
        }

        uint32_t size() const { return prot_size_ + prob_size_; }
        uint32_t capacity() const { return prot_cap_ + prob_cap_; }

    private:
        // Find the closest-to-LRU non-pinned probation entry. Walks from
        // tail (LRU) toward head (MRU). Returns NIL if all are pinned.
        uint32_t
        find_evictable_probation() const {
            uint32_t idx = prob_tail_;
            while (idx != NIL) {
                if (!nodes_[idx].frame || nodes_[idx].frame->pin_count == 0) {
                    return idx;
                }
                idx = nodes_[idx].prev;
            }
            return NIL;
        }

        uint32_t
        alloc_node() {
            assert(free_head_ != NIL);
            uint32_t idx = free_head_;
            free_head_ = nodes_[idx].next;
            nodes_[idx].prev = NIL;
            nodes_[idx].next = NIL;
            assert(!nodes_[idx].in_protected);
            return idx;
        }

        void
        free_node(uint32_t idx) {
            nodes_[idx].occupied = false;
            nodes_[idx].frame = nullptr;
            // INC-037: clear in_protected when retiring a node so the next
            // alloc_node() that picks this slot starts in a known state.
            nodes_[idx].in_protected = false;
            nodes_[idx].next = free_head_;
            free_head_ = idx;
        }

        // ── Probation list ops ──

        void
        link_probation_head(uint32_t idx) {
            auto& n = nodes_[idx];
            n.prev = NIL;
            n.next = prob_head_;
            if (prob_head_ != NIL) nodes_[prob_head_].prev = idx;
            prob_head_ = idx;
            if (prob_tail_ == NIL) prob_tail_ = idx;
            ++prob_size_;
        }

        void
        unlink_probation(uint32_t idx) {
            auto& n = nodes_[idx];
            if (n.prev != NIL) nodes_[n.prev].next = n.next;
            else prob_head_ = n.next;
            if (n.next != NIL) nodes_[n.next].prev = n.prev;
            else prob_tail_ = n.prev;
            n.prev = NIL;
            n.next = NIL;
            --prob_size_;
        }

        // ── Protected list ops ──

        void
        link_protected_head(uint32_t idx) {
            auto& n = nodes_[idx];
            n.prev = NIL;
            n.next = prot_head_;
            if (prot_head_ != NIL) nodes_[prot_head_].prev = idx;
            prot_head_ = idx;
            if (prot_tail_ == NIL) prot_tail_ = idx;
            n.in_protected = true;
            ++prot_size_;
        }

        void
        unlink_protected(uint32_t idx) {
            auto& n = nodes_[idx];
            if (n.prev != NIL) nodes_[n.prev].next = n.next;
            else prot_head_ = n.next;
            if (n.next != NIL) nodes_[n.next].prev = n.prev;
            else prot_tail_ = n.prev;
            n.prev = NIL;
            n.next = NIL;
            --prot_size_;
        }

        void
        move_to_protected_head(uint32_t idx) {
            if (prot_head_ == idx) return;
            unlink_protected(idx);
            link_protected_head(idx);
        }

        void
        promote_to_protected(uint32_t idx) {
            if (prot_size_ >= prot_cap_) {
                uint32_t demote_idx = prot_tail_;
                unlink_protected(demote_idx);
                nodes_[demote_idx].in_protected = false;
                link_probation_head(demote_idx);
            }
            link_protected_head(idx);
        }
    };

}

#endif //APPS_INCONEL_CORE_SLRU_CACHE_HH
