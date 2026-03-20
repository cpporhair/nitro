#ifndef AISAQ_SEARCH_STATE_HH
#define AISAQ_SEARCH_STATE_HH

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace aisaq::search {

    struct
    neighbor {
        uint32_t id;
        float distance;
        bool expanded;
    };

    // Fixed-capacity sorted priority queue of neighbors (by distance ascending)
    // Maintains at most `capacity` closest neighbors
    struct
    candidate_queue {
        std::vector<neighbor> data;
        uint32_t capacity;
        uint32_t size_ = 0;

        explicit
        candidate_queue(uint32_t cap)
            : data(cap), capacity(cap) {}

        void
        clear() { size_ = 0; }

        [[nodiscard]]
        uint32_t
        size() const { return size_; }

        [[nodiscard]]
        bool
        empty() const { return size_ == 0; }

        [[nodiscard]]
        float
        worst_distance() const {
            return size_ < capacity ? std::numeric_limits<float>::max() : data[size_ - 1].distance;
        }

        // Update distance for an existing node (e.g., replacing PQ with exact L2)
        // Re-sorts the queue to maintain order
        void
        update_distance(uint32_t id, float new_dist) {
            for (uint32_t i = 0; i < size_; ++i) {
                if (data[i].id == id) {
                    // Remove from current position
                    bool was_expanded = data[i].expanded;
                    for (uint32_t j = i; j + 1 < size_; ++j)
                        data[j] = data[j + 1];
                    --size_;
                    // Re-insert with new distance (preserving expanded flag)
                    insert(id, new_dist);
                    // Restore expanded flag
                    for (uint32_t j = 0; j < size_; ++j)
                        if (data[j].id == id) { data[j].expanded = was_expanded; break; }
                    return;
                }
            }
        }

        // Insert a neighbor, maintaining sorted order
        // Returns true if inserted (distance < worst or not full)
        bool
        insert(uint32_t id, float dist) {
            if (size_ >= capacity && dist >= data[size_ - 1].distance)
                return false;

            // Binary search for insertion point
            uint32_t lo = 0, hi = size_;
            while (lo < hi) {
                uint32_t mid = (lo + hi) / 2;
                if (data[mid].distance < dist)
                    lo = mid + 1;
                else
                    hi = mid;
            }

            // Shift elements to make room
            uint32_t insert_at = lo;
            uint32_t new_size = std::min(size_ + 1, capacity);
            for (uint32_t i = new_size - 1; i > insert_at; --i)
                data[i] = data[i - 1];

            data[insert_at] = {id, dist, false};
            size_ = new_size;
            return true;
        }

        // Get the k-th closest neighbor
        [[nodiscard]]
        const neighbor&
        operator[](uint32_t k) const { return data[k]; }

        neighbor&
        operator[](uint32_t k) { return data[k]; }

        // Select up to beam_width closest unexpanded nodes
        // Returns count of selected nodes, fills ids[] with their positions in queue
        uint32_t
        select_beam(uint32_t beam_width, uint32_t* positions) {
            uint32_t count = 0;
            for (uint32_t i = 0; i < size_ && count < beam_width; ++i) {
                if (!data[i].expanded) {
                    positions[count++] = i;
                    data[i].expanded = true;
                }
            }
            return count;
        }

        // Check if there are unexpanded nodes
        [[nodiscard]]
        bool
        has_unexpanded() const {
            for (uint32_t i = 0; i < size_; ++i)
                if (!data[i].expanded)
                    return true;
            return false;
        }

        // Distance of the closest unexpanded node (queue is sorted by distance)
        [[nodiscard]]
        float
        closest_unexpanded_distance() const {
            for (uint32_t i = 0; i < size_; ++i)
                if (!data[i].expanded)
                    return data[i].distance;
            return std::numeric_limits<float>::max();
        }
    };

    // Simple bitset for visited node tracking
    struct
    visited_set {
        std::vector<uint64_t> bits;
        uint64_t npts;

        explicit
        visited_set(uint64_t n)
            : bits((n + 63) / 64, 0), npts(n) {}

        void
        clear() {
            std::memset(bits.data(), 0, bits.size() * sizeof(uint64_t));
        }

        bool
        test_and_set(uint64_t id) {
            uint64_t word = id / 64;
            uint64_t bit = id % 64;
            uint64_t mask = 1ULL << bit;
            if (bits[word] & mask)
                return true; // already visited
            bits[word] |= mask;
            return false;
        }

        [[nodiscard]]
        bool
        test(uint64_t id) const {
            return bits[id / 64] & (1ULL << (id % 64));
        }
    };

    // Complete search state for one query
    struct
    search_state {
        candidate_queue candidates;
        visited_set visited;
        uint32_t io_count = 0;

        search_state(uint32_t search_list_size, uint64_t npts)
            : candidates(search_list_size), visited(npts) {}

        void
        reset() {
            candidates.clear();
            visited.clear();
            io_count = 0;
        }
    };

}

#endif //AISAQ_SEARCH_STATE_HH
