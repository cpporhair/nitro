#ifndef APPS_INCONEL_CORE_SLRU_CACHE_HH
#define APPS_INCONEL_CORE_SLRU_CACHE_HH

#include <cassert>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "../format/types.hh"
#include "./clock_cache.hh"  // for evicted_entry

namespace apps::inconel::core {

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
    // Eviction: probation tail (single-access pages get evicted first).
    //
    // Scan resistance: a one-pass scan only fills probation, never threatens
    // protected segment where hot pages live.

    struct slru_cache {
        static constexpr uint32_t NIL = UINT32_MAX;

        struct node {
            paddr key;
            char* buf = nullptr;
            uint32_t prev = NIL;
            uint32_t next = NIL;
            bool in_protected = false;
            bool occupied = false;
        };

        std::vector<node> nodes_;
        std::unordered_map<paddr, uint32_t> index_;

        uint32_t prot_head_ = NIL;
        uint32_t prot_tail_ = NIL;
        uint32_t prob_head_ = NIL;
        uint32_t prob_tail_ = NIL;
        uint32_t prot_size_ = 0;
        uint32_t prob_size_ = 0;
        uint32_t prot_cap_;
        uint32_t prob_cap_;

        uint32_t free_head_ = NIL;  // free node list

        // capacity = total slots; protected segment is 80%, probation 20%.
        //
        // Both segments must have at least one slot, otherwise:
        //   - put() with prob_cap_=0 dereferences prob_tail_=NIL while
        //     evicting, crashing on the first call.
        //   - get()'s promote-to-protected path with prot_cap_=0 hits
        //     `prot_size_ >= prot_cap_` and demotes prot_tail_=NIL.
        // The smallest legal capacity that gives prot_cap >= 1 ∧ prob_cap >= 1
        // under the default 0.8 ratio is 2 (prot_cap=1, prob_cap=1). Higher
        // ratios shift the threshold; the prot_cap_/prob_cap_ check below
        // catches both cases uniformly.
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

        const char*
        get(paddr key) {
            auto it = index_.find(key);
            if (it == index_.end()) return nullptr;
            uint32_t idx = it->second;
            auto& n = nodes_[idx];

            if (n.in_protected) {
                // Already in protected — move to head.
                move_to_protected_head(idx);
            } else {
                // In probation — promote to protected.
                unlink_probation(idx);
                promote_to_protected(idx);
            }
            return n.buf;
        }

        bool
        contains(paddr key) const {
            return index_.find(key) != index_.end();
        }

        std::optional<evicted_entry>
        put(paddr key, char* buf) {
            // Update existing entry — return the displaced buf so the
            // caller can free it (see clock_cache::put for the rationale).
            if (auto it = index_.find(key); it != index_.end()) {
                uint32_t idx = it->second;
                evicted_entry old{ key, nodes_[idx].buf };
                nodes_[idx].buf = buf;
                // Treat as a hit so it gets promoted/refreshed.
                if (nodes_[idx].in_protected) {
                    move_to_protected_head(idx);
                } else {
                    unlink_probation(idx);
                    promote_to_protected(idx);
                }
                return old;
            }

            std::optional<evicted_entry> result;

            // Probation full → evict probation tail.
            if (prob_size_ >= prob_cap_) {
                uint32_t victim_idx = prob_tail_;
                auto& v = nodes_[victim_idx];
                result = evicted_entry{ v.key, v.buf };
                unlink_probation(victim_idx);
                index_.erase(v.key);
                free_node(victim_idx);
            }

            // Allocate node from free list and insert at probation head.
            uint32_t idx = alloc_node();
            auto& n = nodes_[idx];
            n.key = key;
            n.buf = buf;
            n.in_protected = false;
            n.occupied = true;
            link_probation_head(idx);
            index_[key] = idx;
            return result;
        }

        // Evict any one entry, returning its (key, buf) so the caller can
        // release the underlying buffer. Used at scheduler teardown to drain
        // the cache. Probation tail first (LRU within probation), then
        // protected tail. Calling this repeatedly until it returns nullopt
        // empties the cache.
        std::optional<evicted_entry>
        evict_one() {
            if (prob_tail_ != NIL) {
                uint32_t idx = prob_tail_;
                auto& n = nodes_[idx];
                evicted_entry victim{ n.key, n.buf };
                unlink_probation(idx);
                index_.erase(n.key);
                free_node(idx);
                return victim;
            }
            if (prot_tail_ != NIL) {
                uint32_t idx = prot_tail_;
                auto& n = nodes_[idx];
                evicted_entry victim{ n.key, n.buf };
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
        uint32_t
        alloc_node() {
            assert(free_head_ != NIL);
            uint32_t idx = free_head_;
            free_head_ = nodes_[idx].next;
            nodes_[idx].prev = NIL;
            nodes_[idx].next = NIL;
            return idx;
        }

        void
        free_node(uint32_t idx) {
            nodes_[idx].occupied = false;
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

        // Promote a probation entry to protected head.
        // If protected is full, demote protected tail back to probation head.
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
