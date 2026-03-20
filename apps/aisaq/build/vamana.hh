#ifndef AISAQ_BUILD_VAMANA_HH
#define AISAQ_BUILD_VAMANA_HH

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "../search/state.hh"
#include "graph.hh"

namespace aisaq::build {

    // Thread-local scratch for Vamana construction
    struct vamana_scratch {
        search::candidate_queue candidates;
        search::visited_set visited;

        vamana_scratch(uint32_t L, uint32_t npts)
            : candidates(L), visited(npts) {}

        void reset() {
            candidates.clear();
            visited.clear();
        }
    };

    // ── Greedy best-first search on in-memory graph ──
    //
    // expanded_pool: receives ALL expanded nodes (for prune candidates).
    //                DiskANN uses this larger pool for better pruning.

    inline void
    greedy_search(const build_graph& graph, const float* vectors,
                  uint32_t ndims, uint32_t query_id, uint32_t start_id,
                  uint32_t L, vamana_scratch& scratch,
                  std::vector<search::neighbor>& expanded_pool) {
        scratch.reset();
        scratch.candidates = search::candidate_queue(L);
        scratch.visited.clear();
        expanded_pool.clear();

        const float* query = vectors + static_cast<size_t>(query_id) * ndims;

        float start_dist = l2_distance(
            query, vectors + static_cast<size_t>(start_id) * ndims, ndims);
        scratch.candidates.insert(start_id, start_dist);
        scratch.visited.test_and_set(start_id);

        while (scratch.candidates.has_unexpanded()) {
            // Find closest unexpanded
            uint32_t best_pos = UINT32_MAX;
            for (uint32_t i = 0; i < scratch.candidates.size(); i++) {
                if (!scratch.candidates[i].expanded) {
                    best_pos = i;
                    break;
                }
            }
            if (best_pos == UINT32_MAX) break;

            scratch.candidates[best_pos].expanded = true;
            uint32_t cur_id = scratch.candidates[best_pos].id;
            float cur_dist = scratch.candidates[best_pos].distance;

            // Collect expanded node for prune pool (matches DiskANN)
            expanded_pool.push_back({cur_id, cur_dist, true});

            // Expand neighbors
            auto nbrs = graph.get_neighbors(cur_id);
            for (uint32_t nbr : nbrs) {
                if (nbr >= graph.npts) continue;
                if (scratch.visited.test_and_set(nbr)) continue;
                float dist = l2_distance(
                    query, vectors + static_cast<size_t>(nbr) * ndims, ndims);
                scratch.candidates.insert(nbr, dist);
            }
        }
    }

    // ── RobustPrune: select diverse neighbors ──
    //
    // Candidates must be sorted by distance (ascending).
    // Returns at most R neighbors that are not occluded by already-selected ones.

    inline std::vector<uint32_t>
    robust_prune(const search::candidate_queue& candidates,
                 const float* vectors, uint32_t ndims,
                 uint32_t node_id, uint32_t R, float alpha,
                 uint32_t max_candidates) {
        uint32_t n = std::min(candidates.size(), max_candidates);
        std::vector<uint32_t> selected;
        selected.reserve(R);

        // Float occlusion factor per candidate (matches DiskANN occlude_list)
        std::vector<float> occlude_factor(n, 0.0f);

        // Multi-pass: cur_alpha = 1.0, 1.2, 1.44, ... until >= alpha
        float cur_alpha = 1.0f;
        while (cur_alpha <= alpha + 1e-6f && selected.size() < R) {
            for (uint32_t i = 0; i < n && selected.size() < R; i++) {
                if (occlude_factor[i] > cur_alpha) continue;

                uint32_t p_id = candidates[i].id;
                if (p_id == node_id) continue;

                // Select this candidate
                occlude_factor[i] = std::numeric_limits<float>::max();
                selected.push_back(p_id);

                // Update occlusion factors for remaining candidates
                const float* p_vec = vectors + static_cast<size_t>(p_id) * ndims;
                for (uint32_t j = i + 1; j < n; j++) {
                    if (occlude_factor[j] >= alpha) continue;

                    float djk = l2_distance(
                        p_vec,
                        vectors + static_cast<size_t>(candidates[j].id) * ndims,
                        ndims);
                    if (djk == 0.0f) {
                        occlude_factor[j] = std::numeric_limits<float>::max();
                        continue;
                    }

                    occlude_factor[j] = std::max(
                        occlude_factor[j],
                        candidates[j].distance / djk);
                }
            }
            cur_alpha *= 1.2f;
        }

        // Saturate: fill remaining slots greedily (closest first)
        if (alpha > 1.0f) {
            for (uint32_t i = 0; i < n && selected.size() < R; i++) {
                uint32_t c_id = candidates[i].id;
                if (c_id == node_id) continue;
                bool already = false;
                for (uint32_t s : selected)
                    if (s == c_id) { already = true; break; }
                if (!already)
                    selected.push_back(c_id);
            }
        }

        return selected;
    }

    // ── Prune a node's existing neighbors to <= R ──

    inline void
    prune_to_degree(build_graph& graph, const float* vectors,
                    uint32_t ndims, uint32_t node_id,
                    uint32_t R, float alpha, uint32_t max_candidates) {
        auto nbrs = graph.get_neighbors(node_id);
        const float* node_vec = vectors + static_cast<size_t>(node_id) * ndims;

        // Build candidate queue from existing neighbors
        search::candidate_queue cands(static_cast<uint32_t>(nbrs.size()));
        for (uint32_t nbr : nbrs) {
            float dist = l2_distance(
                node_vec, vectors + static_cast<size_t>(nbr) * ndims, ndims);
            cands.insert(nbr, dist);
        }

        auto pruned = robust_prune(cands, vectors, ndims, node_id,
                                   R, alpha, max_candidates);
        graph.set_neighbors(node_id, pruned);
    }

    // ── inter_insert: add reverse edges with re-pruning (matches DiskANN) ──
    //
    // For each neighbor in pruned_list, add node_id as their reverse edge.
    // If the neighbor's list is full, re-prune to maintain quality.

    inline void
    inter_insert(build_graph& graph, const float* vectors, uint32_t ndims,
                 uint32_t node_id, std::span<const uint32_t> pruned_list,
                 uint32_t R, float alpha, uint32_t max_candidates) {
        constexpr float GRAPH_SLACK_FACTOR = 1.3f;
        uint32_t slack_R = static_cast<uint32_t>(GRAPH_SLACK_FACTOR * R);

        for (uint32_t des : pruned_list) {
            graph.lock(des);

            // Skip if already a neighbor
            if (graph.has_neighbor_locked(des, node_id)) {
                graph.unlock(des);
                continue;
            }

            uint32_t deg = graph.degree(des);
            if (deg < slack_R) {
                // Room available: just append
                graph.add_neighbor_locked(des, node_id);
                graph.unlock(des);
            } else {
                // Full: copy neighbors, unlock, prune, re-lock, set
                auto copy = graph.copy_neighbors_locked(des);
                graph.unlock(des);

                copy.push_back(node_id);

                // Build candidate queue for pruning
                const float* des_vec = vectors + static_cast<size_t>(des) * ndims;
                search::candidate_queue cands(static_cast<uint32_t>(copy.size()));
                for (uint32_t nbr : copy) {
                    if (nbr == des) continue;
                    float dist = l2_distance(
                        des_vec, vectors + static_cast<size_t>(nbr) * ndims, ndims);
                    cands.insert(nbr, dist);
                }

                auto new_nbrs = robust_prune(cands, vectors, ndims, des,
                                             R, alpha, max_candidates);

                graph.lock(des);
                graph.set_neighbors(des, new_nbrs);
                graph.unlock(des);
            }
        }
    }

    // ── Compute medoid: point closest to the global centroid ──

    inline uint32_t
    compute_medoid(const float* vectors, uint32_t npts, uint32_t ndims,
                   const float* global_centroid) {
        float best_dist = std::numeric_limits<float>::max();
        uint32_t best_id = 0;

        for (uint32_t i = 0; i < npts; i++) {
            float dist = l2_distance(
                vectors + static_cast<size_t>(i) * ndims,
                global_centroid, ndims);
            if (dist < best_dist) {
                best_dist = dist;
                best_id = i;
            }
        }
        return best_id;
    }

}

#endif // AISAQ_BUILD_VAMANA_HH
