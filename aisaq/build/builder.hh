#ifndef AISAQ_BUILD_BUILDER_HH
#define AISAQ_BUILD_BUILDER_HH

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../common/types.hh"
#include "../runtime/config.hh"
#include "../gpu/kernels.hh"
#include "graph.hh"
#include "pq_trainer.hh"
#include "pq_trainer_gpu.hh"
#include "vamana.hh"
#include "reorder.hh"
#include "layout_writer.hh"

namespace aisaq::build {

    // ── Load base vectors from .fbin file ──

    inline float*
    load_base_vectors(const char* path, uint32_t& npts, uint32_t& ndims) {
        printf("Loading base vectors from %s ...\n", path);
        float* vectors = load_bin_file<float>(path, npts, ndims);
        printf("  %u vectors, %u dims (%.1f MB)\n",
               npts, ndims,
               static_cast<double>(npts) * ndims * sizeof(float) / (1024.0 * 1024.0));
        return vectors;
    }

    // ── Stage 2: Vamana graph construction (multi-threaded) ──

    inline void
    vamana_build_parallel(build_graph& graph, const float* vectors,
                          uint32_t npts, uint32_t ndims,
                          uint32_t medoid, const runtime::build_config& cfg,
                          uint32_t num_threads) {
        auto t0 = std::chrono::high_resolution_clock::now();
        printf("\n=== Stage 2: Vamana Graph Construction ===\n");
        printf("  npts=%u R=%u L=%u alpha=%.1f threads=%u\n",
               npts, cfg.R, cfg.L, cfg.alpha, num_threads);

        auto perm = random_permutation(npts);
        std::atomic<uint32_t> progress{0};

        // Build pass: dynamic work stealing (matches DiskANN's omp dynamic scheduling)
        auto build_worker = [&](uint32_t) {
            vamana_scratch scratch(cfg.L, npts);
            std::vector<search::neighbor> expanded_pool;
            while (true) {
                uint32_t idx = progress.fetch_add(1, std::memory_order_relaxed);
                if (idx >= npts) break;
                uint32_t node_id = perm[idx];

                greedy_search(graph, vectors, ndims, node_id, medoid,
                              cfg.L, scratch, expanded_pool);

                // Build prune pool from expanded nodes (matches DiskANN)
                search::candidate_queue prune_cands(
                    static_cast<uint32_t>(expanded_pool.size()));
                for (auto& nb : expanded_pool) {
                    if (nb.id != node_id)
                        prune_cands.insert(nb.id, nb.distance);
                }

                auto pruned = robust_prune(prune_cands, vectors, ndims,
                                           node_id, cfg.R, cfg.alpha,
                                           cfg.max_candidates);

                graph.lock(node_id);
                graph.set_neighbors(node_id, pruned);
                graph.unlock(node_id);

                // inter_insert: add reverse edges with re-pruning
                inter_insert(graph, vectors, ndims, node_id, pruned,
                             cfg.R, cfg.alpha, cfg.max_candidates);

                if (idx % 100000 == 0)
                    printf("  build %u / %u\n", idx, npts);
            }
        };

        std::vector<std::thread> threads;
        for (uint32_t t = 0; t < num_threads; t++)
            threads.emplace_back(build_worker, t);
        for (auto& t : threads) t.join();

        // Cleanup pass: inter_insert maintains slack_R (1.3*R), prune to R for disk layout
        printf("  cleanup: pruning to R=%u ...\n", cfg.R);
        progress.store(0);

        auto cleanup_worker = [&](uint32_t) {
            while (true) {
                uint32_t i = progress.fetch_add(1, std::memory_order_relaxed);
                if (i >= npts) break;
                if (graph.degree(i) > cfg.R)
                    prune_to_degree(graph, vectors, ndims, i,
                                    cfg.R, cfg.alpha, cfg.max_candidates);
            }
        };

        threads.clear();
        for (uint32_t t = 0; t < num_threads; t++)
            threads.emplace_back(cleanup_worker, t);
        for (auto& t : threads) t.join();

        auto t1 = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        printf("  Stage 2 complete in %ld ms\n", ms);
    }

    // ── Full build pipeline (synchronous) ──

    inline void
    build_index(const runtime::build_config& cfg,
                const std::string& index_prefix,
                const char* base_vectors_path,
                uint32_t num_threads) {
        auto t_total = std::chrono::high_resolution_clock::now();

        // Load base vectors
        uint32_t npts, ndims;
        float* vectors = load_base_vectors(base_vectors_path, npts, ndims);

        uint32_t n_chunks = cfg.n_chunks;
        if (n_chunks == 0) n_chunks = ndims;

        // ── Stage 1: PQ Training ──
        printf("\n=== Stage 1: PQ Training ===\n");
        auto t0 = std::chrono::high_resolution_clock::now();

        pq_result pq;
        if (cfg.gpu_device >= 0) {
            try {
                gpu::gpu_context gpu(cfg.gpu_device, cfg.ptx_path.c_str());
                pq = train_pq_gpu(vectors, npts, ndims,
                                  n_chunks, cfg.sample_size, cfg.kmeans_iters, gpu);
            } catch (const std::exception& e) {
                printf("  GPU init failed (%s), falling back to CPU\n", e.what());
                pq = train_pq(vectors, npts, ndims,
                              n_chunks, cfg.sample_size, cfg.kmeans_iters);
            }
        } else {
            pq = train_pq(vectors, npts, ndims,
                          n_chunks, cfg.sample_size, cfg.kmeans_iters);
        }
        write_pq_files(pq, index_prefix, npts);

        auto t1 = std::chrono::high_resolution_clock::now();
        printf("  Stage 1 complete in %ld ms\n",
               std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

        // Compute medoid (always use real data centroid, independent of PQ centering)
        printf("\nComputing medoid ...\n");
        auto* real_centroid = new float[ndims]();
        for (uint32_t i = 0; i < npts; i++) {
            const float* v = vectors + static_cast<size_t>(i) * ndims;
            for (uint32_t d = 0; d < ndims; d++)
                real_centroid[d] += v[d];
        }
        for (uint32_t d = 0; d < ndims; d++)
            real_centroid[d] /= npts;
        uint32_t medoid = compute_medoid(vectors, npts, ndims, real_centroid);
        delete[] real_centroid;
        printf("  medoid = %u\n", medoid);

        // Init graph
        uint32_t max_slots = static_cast<uint32_t>(cfg.R * 1.3f) + 2;
        build_graph graph(npts, max_slots);
        // Graph starts empty — incrementally built by greedy search (same as DiskANN reference)
        printf("  graph: %u nodes × %u max_slots (%.1f MB)\n",
               npts, max_slots,
               static_cast<double>(npts) * max_slots * 4 / (1024.0 * 1024.0));

        // ── Stage 2: Vamana ──
        vamana_build_parallel(graph, vectors, npts, ndims, medoid, cfg, num_threads);

        // ── Stage 3: Post-processing ──
        printf("\n=== Stage 3: Post-processing ===\n");
        t0 = std::chrono::high_resolution_clock::now();

        auto entry_points = select_entry_points(
            graph, vectors, ndims, medoid, cfg.num_entries);

        bool rearranged = false;
        if (cfg.reorder) {
            auto res = bfs_reorder(graph, vectors, pq.pq_codes,
                                   npts, ndims, n_chunks,
                                   medoid, entry_points);
            medoid = res.new_medoid;
            entry_points = std::move(res.new_entries);
            rearranged = true;
        }

        t1 = std::chrono::high_resolution_clock::now();
        printf("  Stage 3 complete in %ld ms\n",
               std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

        // ── Stage 4: Disk layout writing ──
        printf("\n=== Stage 4: Disk Layout Writing ===\n");
        t0 = std::chrono::high_resolution_clock::now();

        auto layout = compute_layout(npts, ndims, cfg.R, n_chunks);
        printf("  max_node_len=%lu spn=%u file_size=%.1f GB\n",
               layout.max_node_len, layout.sectors_per_node,
               layout.file_size / (1024.0 * 1024.0 * 1024.0));

        write_disk_index_file(index_prefix, npts, ndims, medoid,
                              layout, n_chunks, rearranged,
                              vectors, graph, pq.pq_codes);

        write_auxiliary_files(index_prefix, medoid, vectors, ndims, entry_points);

        t1 = std::chrono::high_resolution_clock::now();
        printf("  Stage 4 complete in %ld ms\n",
               std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

        // ── Summary ──
        auto t_end = std::chrono::high_resolution_clock::now();
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t_end - t_total).count();

        printf("\n=== Build Complete (%.1f s) ===\n", total_ms / 1000.0);
        printf("  Index: %s\n", index_prefix.c_str());
        printf("  Vectors: %u, Dims: %u, Degree: %u, Chunks: %u\n",
               npts, ndims, cfg.R, n_chunks);
        printf("  Medoid: %u, Entries: %zu, Reordered: %s\n",
               medoid, entry_points.size(), rearranged ? "yes" : "no");
        printf("  Disk: spn=%u, %.1f GB\n",
               layout.sectors_per_node,
               layout.file_size / (1024.0 * 1024.0 * 1024.0));
        printf("\nTo search:\n");
        printf("  1. %s <config_search.json> write\n", "apps.aisaq");
        printf("  2. %s <config_search.json> search <query.fbin> [gt]\n", "apps.aisaq");

        delete[] vectors;
    }

}

#endif // AISAQ_BUILD_BUILDER_HH
