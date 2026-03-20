#ifndef AISAQ_BUILD_PQ_TRAINER_GPU_HH
#define AISAQ_BUILD_PQ_TRAINER_GPU_HH

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../common/types.hh"
#include "../gpu/kernels.hh"
#include "graph.hh"
#include "pq_trainer.hh"

namespace aisaq::build {

    // ── GPU-accelerated PQ training ──
    //
    // GPU handles: global centroid, k-means (per chunk), full PQ encoding.
    // CPU handles: sampling, k-means++ seed selection (sequential).

    inline pq_result
    train_pq_gpu(const float* vectors, uint32_t npts, uint32_t ndims,
                 uint32_t n_chunks, uint32_t sample_size,
                 uint32_t kmeans_iters, gpu::gpu_context& gpu) {
        pq_result result;
        result.ndims = ndims;
        if (n_chunks == 0) n_chunks = ndims;
        result.n_chunks = n_chunks;

        const uint32_t K = NUM_PQ_CENTROIDS;  // 256
        size_t vec_bytes = static_cast<size_t>(npts) * ndims * sizeof(float);

        // ── Upload all vectors to GPU ──
        auto d_vectors = gpu.alloc(vec_bytes);
        gpu.upload(d_vectors, vectors, vec_bytes);
        gpu.sync();
        printf("  uploaded %u vectors to GPU (%.1f MB)\n", npts, vec_bytes / (1024.0 * 1024.0));

        // ── Global centroid (GPU reduce) ──
        result.global_centroid = new float[ndims];
        auto d_centroid = gpu.alloc(ndims * sizeof(float));
        gpu.zero(d_centroid, ndims * sizeof(float));
        gpu.launch(gpu.kern.mean_reduce, ndims, 256,
                   d_vectors, d_centroid, npts, ndims);
        gpu.sync();
        gpu.download(result.global_centroid, d_centroid, ndims * sizeof(float));
        gpu.sync();
        for (uint32_t d = 0; d < ndims; d++)
            result.global_centroid[d] /= npts;
        gpu.free(d_centroid);
        printf("  global centroid computed (GPU)\n");

        // ── Chunk offsets ──
        result.chunk_offsets = new uint32_t[n_chunks + 1];
        for (uint32_t c = 0; c <= n_chunks; c++)
            result.chunk_offsets[c] = c * ndims / n_chunks;
        result.chunk_offsets[n_chunks] = ndims;

        // ── Sample vectors, center on CPU, upload ──
        uint32_t ns = std::min(sample_size, npts);
        std::mt19937 rng(42);

        std::vector<uint32_t> sample_ids;
        if (ns >= npts) {
            ns = npts;
            sample_ids.resize(npts);
            for (uint32_t i = 0; i < npts; i++) sample_ids[i] = i;
        } else {
            sample_ids.resize(npts);
            for (uint32_t i = 0; i < npts; i++) sample_ids[i] = i;
            std::shuffle(sample_ids.begin(), sample_ids.end(), rng);
            sample_ids.resize(ns);
        }

        // Center samples
        std::vector<float> h_samples(static_cast<size_t>(ns) * ndims);
        for (uint32_t i = 0; i < ns; i++) {
            const float* v = vectors + static_cast<size_t>(sample_ids[i]) * ndims;
            float* s = h_samples.data() + static_cast<size_t>(i) * ndims;
            for (uint32_t d = 0; d < ndims; d++)
                s[d] = v[d] - result.global_centroid[d];
        }

        // Upload centered samples
        size_t sample_bytes = static_cast<size_t>(ns) * ndims * sizeof(float);
        auto d_samples = gpu.alloc(sample_bytes);
        gpu.upload(d_samples, h_samples.data(), sample_bytes);
        gpu.sync();
        printf("  sampled %u vectors, centered, uploaded\n", ns);

        // ── Per-chunk k-means (GPU) ──
        result.centroids = new float[static_cast<size_t>(K) * ndims]();

        // GPU buffers for k-means (reused across chunks)
        uint32_t max_chunk_dim = 0;
        for (uint32_t c = 0; c < n_chunks; c++)
            max_chunk_dim = std::max(max_chunk_dim,
                                     result.chunk_offsets[c + 1] - result.chunk_offsets[c]);

        auto d_seeds = gpu.alloc(static_cast<size_t>(K) * max_chunk_dim * sizeof(float));
        auto d_min_dists = gpu.alloc(ns * sizeof(float));
        auto d_sums = gpu.alloc(static_cast<size_t>(K) * max_chunk_dim * sizeof(float));
        auto d_counts = gpu.alloc(K * sizeof(uint32_t));

        std::vector<float> h_min_dists(ns);
        std::vector<float> h_chunk_cents(static_cast<size_t>(K) * max_chunk_dim);

        unsigned grid_ns = (ns + 255) / 256;

        for (uint32_t c = 0; c < n_chunks; c++) {
            uint32_t dim_start = result.chunk_offsets[c];
            uint32_t dim_end = result.chunk_offsets[c + 1];
            uint32_t chunk_dim = dim_end - dim_start;

            // ── k-means++ seed selection ──
            // First seed: random sample's chunk values
            {
                uint32_t first = rng() % ns;
                const float* src = h_samples.data() + static_cast<size_t>(first) * ndims + dim_start;
                gpu.upload(d_seeds, src, chunk_dim * sizeof(float));
            }

            for (uint32_t s = 1; s < K; s++) {
                // GPU: compute distance to latest seed, update min_dists
                gpu.launch(gpu.kern.kmeans_pp_dist, grid_ns, 256,
                           d_samples, d_seeds, d_min_dists,
                           ns, s, chunk_dim, ndims, dim_start);
                gpu.sync();

                // CPU: download min_dists, weighted random selection
                gpu.download(h_min_dists.data(), d_min_dists, ns * sizeof(float));
                gpu.sync();

                double total = 0;
                for (uint32_t i = 0; i < ns; i++) total += h_min_dists[i];

                std::uniform_real_distribution<double> dist(0.0, total);
                double r = dist(rng);
                double cum = 0;
                uint32_t next = 0;
                for (uint32_t i = 0; i < ns; i++) {
                    cum += h_min_dists[i];
                    if (cum >= r) { next = i; break; }
                }

                // Upload new seed
                const float* src = h_samples.data() + static_cast<size_t>(next) * ndims + dim_start;
                gpu.upload(d_seeds + static_cast<size_t>(s) * chunk_dim * sizeof(float),
                           src, chunk_dim * sizeof(float));
            }

            // ── Lloyd iterations (pure GPU) ──
            unsigned grid_k = K;
            unsigned block_k = std::min(chunk_dim, 256u);

            for (uint32_t iter = 0; iter < kmeans_iters; iter++) {
                gpu.zero(d_sums, static_cast<size_t>(K) * chunk_dim * sizeof(float));
                gpu.zero(d_counts, K * sizeof(uint32_t));

                gpu.launch(gpu.kern.kmeans_assign_accum, grid_ns, 256,
                           d_samples, d_seeds, d_sums, d_counts,
                           ns, K, chunk_dim, ndims, dim_start);

                gpu.launch(gpu.kern.kmeans_divide, grid_k, block_k,
                           d_seeds, d_sums, d_counts, chunk_dim);
            }
            gpu.sync();

            // Download chunk centroids, copy to full centroid table
            gpu.download(h_chunk_cents.data(), d_seeds,
                         static_cast<size_t>(K) * chunk_dim * sizeof(float));
            gpu.sync();

            for (uint32_t k = 0; k < K; k++) {
                const float* src = h_chunk_cents.data() + static_cast<size_t>(k) * chunk_dim;
                float* dst = result.centroids + static_cast<size_t>(k) * ndims + dim_start;
                std::memcpy(dst, src, chunk_dim * sizeof(float));
            }

            if ((c + 1) % 16 == 0 || c + 1 == n_chunks)
                printf("  k-means chunk %u / %u done (GPU)\n", c + 1, n_chunks);
        }

        gpu.free(d_seeds);
        gpu.free(d_min_dists);
        gpu.free(d_sums);
        gpu.free(d_counts);

        // ── Full PQ encoding (GPU) ──
        printf("  encoding %u vectors on GPU ...\n", npts);

        // Upload centroids + global centroid + chunk offsets
        size_t cent_bytes = static_cast<size_t>(K) * ndims * sizeof(float);
        auto d_centroids = gpu.alloc(cent_bytes);
        gpu.upload(d_centroids, result.centroids, cent_bytes);

        auto d_gc = gpu.alloc(ndims * sizeof(float));
        gpu.upload(d_gc, result.global_centroid, ndims * sizeof(float));

        auto d_offsets = gpu.alloc((n_chunks + 1) * sizeof(uint32_t));
        gpu.upload(d_offsets, result.chunk_offsets, (n_chunks + 1) * sizeof(uint32_t));

        size_t code_bytes = static_cast<size_t>(npts) * n_chunks;
        auto d_codes = gpu.alloc(code_bytes);

        unsigned grid_enc = (npts + 255) / 256;
        gpu.launch(gpu.kern.pq_encode, grid_enc, 256,
                   d_vectors, d_gc, d_centroids, d_codes,
                   npts, ndims, n_chunks, d_offsets);
        gpu.sync();

        // Download PQ codes
        result.pq_codes = new uint8_t[code_bytes];
        gpu.download(result.pq_codes, d_codes, code_bytes);
        gpu.sync();

        printf("  encoding complete (GPU): %u vectors × %u chunks\n", npts, n_chunks);

        // Cleanup GPU
        gpu.free(d_vectors);
        gpu.free(d_centroids);
        gpu.free(d_gc);
        gpu.free(d_offsets);
        gpu.free(d_codes);
        gpu.free(d_samples);

        return result;
    }

}

#endif // AISAQ_BUILD_PQ_TRAINER_GPU_HH
