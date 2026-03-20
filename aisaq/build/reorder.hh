#ifndef AISAQ_BUILD_REORDER_HH
#define AISAQ_BUILD_REORDER_HH

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "graph.hh"

namespace aisaq::build {

    // ── Multi-entry point selection via farthest-point sampling ──

    inline std::vector<uint32_t>
    select_entry_points(const build_graph& graph, const float* vectors,
                        uint32_t ndims, uint32_t medoid,
                        uint32_t num_entries) {
        if (num_entries <= 1)
            return {medoid};

        // BFS from medoid, collect candidates
        uint32_t candidate_count = num_entries * 10;
        std::vector<uint32_t> candidates;
        candidates.reserve(candidate_count);

        std::vector<uint64_t> visited((graph.npts + 63) / 64, 0);
        auto mark = [&](uint32_t id) {
            visited[id / 64] |= 1ULL << (id % 64);
        };
        auto is_visited = [&](uint32_t id) -> bool {
            return visited[id / 64] & (1ULL << (id % 64));
        };

        std::deque<uint32_t> bfs_q;
        mark(medoid);
        bfs_q.push_back(medoid);
        candidates.push_back(medoid);

        while (!bfs_q.empty() && candidates.size() < candidate_count) {
            uint32_t cur = bfs_q.front();
            bfs_q.pop_front();
            for (uint32_t nbr : graph.get_neighbors(cur)) {
                if (nbr < graph.npts && !is_visited(nbr)) {
                    mark(nbr);
                    bfs_q.push_back(nbr);
                    candidates.push_back(nbr);
                    if (candidates.size() >= candidate_count) break;
                }
            }
        }

        // Farthest-point sampling: greedily pick most distant from already-selected
        std::vector<uint32_t> entries;
        entries.reserve(num_entries);
        entries.push_back(medoid);

        std::vector<float> min_dists(candidates.size(),
                                     std::numeric_limits<float>::max());

        for (uint32_t s = 1; s < num_entries && s < candidates.size(); s++) {
            // Update min distance to already-selected set
            uint32_t last = entries.back();
            const float* last_vec = vectors + static_cast<size_t>(last) * ndims;

            float best_dist = -1;
            uint32_t best_idx = 0;

            for (uint32_t i = 0; i < candidates.size(); i++) {
                float d = l2_distance(
                    vectors + static_cast<size_t>(candidates[i]) * ndims,
                    last_vec, ndims);
                min_dists[i] = std::min(min_dists[i], d);
                if (min_dists[i] > best_dist) {
                    best_dist = min_dists[i];
                    best_idx = i;
                }
            }

            entries.push_back(candidates[best_idx]);
            min_dists[best_idx] = 0; // mark as selected
        }

        printf("  selected %zu entry points\n", entries.size());
        return entries;
    }

    // ── BFS reorder: remap node IDs to BFS order from medoid ──

    struct reorder_result {
        std::vector<uint32_t> new_id;   // new_id[old] = bfs_rank
        uint32_t new_medoid;
        std::vector<uint32_t> new_entries;
    };

    inline reorder_result
    bfs_reorder(build_graph& graph, float* vectors, uint8_t* pq_codes,
                uint32_t npts, uint32_t ndims, uint32_t n_chunks,
                uint32_t medoid, const std::vector<uint32_t>& entry_points) {
        printf("  BFS reordering %u nodes ...\n", npts);

        // 1. BFS traversal order
        std::vector<uint32_t> bfs_order;
        bfs_order.reserve(npts);

        std::vector<uint64_t> visited((npts + 63) / 64, 0);
        auto mark = [&](uint32_t id) {
            visited[id / 64] |= 1ULL << (id % 64);
        };
        auto is_visited = [&](uint32_t id) -> bool {
            return visited[id / 64] & (1ULL << (id % 64));
        };

        std::deque<uint32_t> q;
        mark(medoid);
        q.push_back(medoid);
        bfs_order.push_back(medoid);

        while (!q.empty()) {
            uint32_t cur = q.front();
            q.pop_front();
            for (uint32_t nbr : graph.get_neighbors(cur)) {
                if (nbr < npts && !is_visited(nbr)) {
                    mark(nbr);
                    q.push_back(nbr);
                    bfs_order.push_back(nbr);
                }
            }
        }

        // Handle disconnected nodes
        for (uint32_t i = 0; i < npts; i++) {
            if (!is_visited(i)) {
                bfs_order.push_back(i);
            }
        }

        // 2. Build mapping: new_id[old_id] = bfs_rank
        reorder_result res;
        res.new_id.resize(npts);
        for (uint32_t rank = 0; rank < npts; rank++)
            res.new_id[bfs_order[rank]] = rank;

        // 3. Remap graph neighbor IDs
        for (uint32_t old_id = 0; old_id < npts; old_id++) {
            auto nbrs = graph.get_neighbors(old_id);
            auto* ptr = const_cast<uint32_t*>(nbrs.data());
            for (size_t j = 0; j < nbrs.size(); j++)
                ptr[j] = res.new_id[ptr[j]];
        }

        // 4. Reorder vectors using temp buffer
        {
            size_t vec_bytes = static_cast<size_t>(npts) * ndims * sizeof(float);
            auto* tmp = new float[static_cast<size_t>(npts) * ndims];
            std::memcpy(tmp, vectors, vec_bytes);
            for (uint32_t old_id = 0; old_id < npts; old_id++) {
                uint32_t new_pos = res.new_id[old_id];
                std::memcpy(vectors + static_cast<size_t>(new_pos) * ndims,
                            tmp + static_cast<size_t>(old_id) * ndims,
                            ndims * sizeof(float));
            }
            delete[] tmp;
        }

        // 5. Reorder PQ codes
        {
            size_t code_bytes = static_cast<size_t>(npts) * n_chunks;
            auto* tmp = new uint8_t[code_bytes];
            std::memcpy(tmp, pq_codes, code_bytes);
            for (uint32_t old_id = 0; old_id < npts; old_id++) {
                uint32_t new_pos = res.new_id[old_id];
                std::memcpy(pq_codes + static_cast<size_t>(new_pos) * n_chunks,
                            tmp + static_cast<size_t>(old_id) * n_chunks,
                            n_chunks);
            }
            delete[] tmp;
        }

        // 6. Reorder graph itself (neighbor arrays)
        {
            uint32_t ms = graph.max_slots;
            size_t total = static_cast<size_t>(npts) * ms;
            auto* tmp_nbrs = new uint32_t[total];
            auto* tmp_degs = new uint32_t[npts];
            std::memcpy(tmp_nbrs, graph.neighbors, total * sizeof(uint32_t));
            for (uint32_t i = 0; i < npts; i++)
                tmp_degs[i] = graph.degree(i);

            for (uint32_t old_id = 0; old_id < npts; old_id++) {
                uint32_t new_pos = res.new_id[old_id];
                std::memcpy(graph.neighbors + static_cast<size_t>(new_pos) * ms,
                            tmp_nbrs + static_cast<size_t>(old_id) * ms,
                            ms * sizeof(uint32_t));
                graph.degrees[new_pos].store(tmp_degs[old_id],
                                              std::memory_order_relaxed);
            }
            delete[] tmp_nbrs;
            delete[] tmp_degs;
        }

        // 7. Remap medoid and entry points
        res.new_medoid = res.new_id[medoid];
        res.new_entries.reserve(entry_points.size());
        for (uint32_t ep : entry_points)
            res.new_entries.push_back(res.new_id[ep]);

        printf("  reorder complete: medoid %u → %u\n", medoid, res.new_medoid);
        return res;
    }

}

#endif // AISAQ_BUILD_REORDER_HH
