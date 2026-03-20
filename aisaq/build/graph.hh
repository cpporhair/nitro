#ifndef AISAQ_BUILD_GRAPH_HH
#define AISAQ_BUILD_GRAPH_HH

#include <atomic>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <vector>

namespace aisaq::build {

    // Graph for Vamana construction.
    // Pre-allocated flat neighbor array + per-node spinlock for inter_insert re-pruning.
    struct
    build_graph {
        uint32_t* neighbors = nullptr;                  // [npts * max_slots]
        std::atomic<uint32_t>* degrees = nullptr;      // [npts]
        std::atomic_flag* locks = nullptr;              // [npts] per-node spinlock
        uint32_t npts = 0;
        uint32_t max_slots = 0;

        build_graph() = default;

        build_graph(uint32_t n, uint32_t slots)
            : npts(n), max_slots(slots)
        {
            neighbors = new uint32_t[static_cast<size_t>(n) * slots];
            degrees = new std::atomic<uint32_t>[n];
            locks = new std::atomic_flag[n];
            for (uint32_t i = 0; i < n; i++) {
                degrees[i].store(0, std::memory_order_relaxed);
                locks[i].clear();
            }
        }

        ~build_graph() {
            delete[] neighbors;
            delete[] degrees;
            delete[] locks;
        }

        build_graph(const build_graph&) = delete;
        build_graph& operator=(const build_graph&) = delete;

        build_graph& operator=(build_graph&& o) noexcept {
            if (this != &o) {
                delete[] neighbors; delete[] degrees; delete[] locks;
                neighbors = o.neighbors; degrees = o.degrees; locks = o.locks;
                npts = o.npts; max_slots = o.max_slots;
                o.neighbors = nullptr; o.degrees = nullptr; o.locks = nullptr;
            }
            return *this;
        }

        build_graph(build_graph&& o) noexcept
            : neighbors(o.neighbors), degrees(o.degrees), locks(o.locks)
            , npts(o.npts), max_slots(o.max_slots) {
            o.neighbors = nullptr;
            o.degrees = nullptr;
            o.locks = nullptr;
        }

        // ── Read (lock-free, safe from any thread) ──

        [[nodiscard]]
        uint32_t
        degree(uint32_t node) const {
            return std::min(degrees[node].load(std::memory_order_relaxed), max_slots);
        }

        [[nodiscard]]
        std::span<const uint32_t>
        get_neighbors(uint32_t node) const {
            return {neighbors + static_cast<size_t>(node) * max_slots, degree(node)};
        }

        // ── Write: set entire neighbor list (single writer per node) ──

        void
        set_neighbors(uint32_t node, std::span<const uint32_t> nbrs) {
            auto* dst = neighbors + static_cast<size_t>(node) * max_slots;
            std::memcpy(dst, nbrs.data(), nbrs.size() * sizeof(uint32_t));
            degrees[node].store(static_cast<uint32_t>(nbrs.size()),
                                std::memory_order_release);
        }

        // ── Spinlock per node (for inter_insert read-modify-write) ──

        void lock(uint32_t node) {
            while (locks[node].test_and_set(std::memory_order_acquire)) {}
        }

        void unlock(uint32_t node) {
            locks[node].clear(std::memory_order_release);
        }

        // Under lock: check if src is already a neighbor of target
        [[nodiscard]]
        bool
        has_neighbor_locked(uint32_t target, uint32_t src) const {
            uint32_t deg = degrees[target].load(std::memory_order_relaxed);
            if (deg > max_slots) deg = max_slots;
            const auto* base = neighbors + static_cast<size_t>(target) * max_slots;
            for (uint32_t i = 0; i < deg; i++)
                if (base[i] == src) return true;
            return false;
        }

        // Under lock: append neighbor (caller ensures room)
        void
        add_neighbor_locked(uint32_t target, uint32_t src) {
            uint32_t deg = degrees[target].load(std::memory_order_relaxed);
            neighbors[static_cast<size_t>(target) * max_slots + deg] = src;
            degrees[target].store(deg + 1, std::memory_order_release);
        }

        // Under lock: copy current neighbor list
        [[nodiscard]]
        std::vector<uint32_t>
        copy_neighbors_locked(uint32_t node) const {
            uint32_t deg = degrees[node].load(std::memory_order_relaxed);
            if (deg > max_slots) deg = max_slots;
            const auto* base = neighbors + static_cast<size_t>(node) * max_slots;
            return {base, base + deg};
        }

        // ── Init: random neighbors ──

        void
        init_random(uint32_t R, uint32_t seed = 42) {
            std::mt19937 rng(seed);
            for (uint32_t i = 0; i < npts; i++) {
                auto* dst = neighbors + static_cast<size_t>(i) * max_slots;
                uint32_t count = 0;
                while (count < R) {
                    uint32_t nbr = rng() % npts;
                    if (nbr == i) continue;
                    bool dup = false;
                    for (uint32_t j = 0; j < count; j++)
                        if (dst[j] == nbr) { dup = true; break; }
                    if (!dup) dst[count++] = nbr;
                }
                degrees[i] = R;
            }
        }
    };

    // L2 distance between two vectors
    inline float
    l2_distance(const float* a, const float* b, uint32_t ndims) {
        float dist = 0.0f;
        for (uint32_t d = 0; d < ndims; d++) {
            float diff = a[d] - b[d];
            dist += diff * diff;
        }
        return dist;
    }

    // Random permutation of [0, n)
    inline std::vector<uint32_t>
    random_permutation(uint32_t n, uint32_t seed = 12345) {
        std::vector<uint32_t> perm(n);
        for (uint32_t i = 0; i < n; i++) perm[i] = i;
        std::mt19937 rng(seed);
        for (uint32_t i = n - 1; i > 0; i--) {
            uint32_t j = rng() % (i + 1);
            std::swap(perm[i], perm[j]);
        }
        return perm;
    }

}

#endif // AISAQ_BUILD_GRAPH_HH
