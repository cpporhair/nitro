#ifndef AISAQ_BUILD_PQ_TRAINER_HH
#define AISAQ_BUILD_PQ_TRAINER_HH

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "../common/types.hh"
#include "graph.hh"

namespace aisaq::build {

    struct pq_result {
        float* centroids = nullptr;         // [256 * ndims]
        float* global_centroid = nullptr;   // [ndims]
        uint32_t* chunk_offsets = nullptr;  // [n_chunks + 1]
        uint8_t* pq_codes = nullptr;        // [npts * n_chunks]
        uint32_t n_chunks = 0;
        uint32_t ndims = 0;

        ~pq_result() {
            delete[] centroids;
            delete[] global_centroid;
            delete[] chunk_offsets;
            delete[] pq_codes;
        }

        pq_result(const pq_result&) = delete;
        pq_result& operator=(const pq_result&) = delete;

        pq_result& operator=(pq_result&& o) noexcept {
            if (this != &o) {
                delete[] centroids; delete[] global_centroid;
                delete[] chunk_offsets; delete[] pq_codes;
                centroids = o.centroids; global_centroid = o.global_centroid;
                chunk_offsets = o.chunk_offsets; pq_codes = o.pq_codes;
                n_chunks = o.n_chunks; ndims = o.ndims;
                o.centroids = nullptr; o.global_centroid = nullptr;
                o.chunk_offsets = nullptr; o.pq_codes = nullptr;
            }
            return *this;
        }

        pq_result(pq_result&& o) noexcept
            : centroids(o.centroids), global_centroid(o.global_centroid)
            , chunk_offsets(o.chunk_offsets), pq_codes(o.pq_codes)
            , n_chunks(o.n_chunks), ndims(o.ndims) {
            o.centroids = nullptr;
            o.global_centroid = nullptr;
            o.chunk_offsets = nullptr;
            o.pq_codes = nullptr;
        }
        pq_result() = default;
    };

    // ── k-means++ seed selection ──

    inline void
    kmeans_pp_init(const float* data, uint32_t n, uint32_t dim,
                   float* seeds, uint32_t k, std::mt19937& rng) {
        // First seed: random
        uint32_t first = rng() % n;
        std::memcpy(seeds, data + static_cast<size_t>(first) * dim,
                    dim * sizeof(float));

        std::vector<float> min_dists(n, std::numeric_limits<float>::max());

        for (uint32_t s = 1; s < k; s++) {
            // Update min distances to nearest existing seed
            const float* new_seed = seeds + static_cast<size_t>(s - 1) * dim;
            double total = 0;
            for (uint32_t i = 0; i < n; i++) {
                float d = l2_distance(data + static_cast<size_t>(i) * dim,
                                      new_seed, dim);
                min_dists[i] = std::min(min_dists[i], d);
                total += min_dists[i];
            }

            // Weighted random selection
            std::uniform_real_distribution<double> dist(0.0, total);
            double r = dist(rng);
            double cum = 0;
            uint32_t next = 0;
            for (uint32_t i = 0; i < n; i++) {
                cum += min_dists[i];
                if (cum >= r) { next = i; break; }
            }

            std::memcpy(seeds + static_cast<size_t>(s) * dim,
                        data + static_cast<size_t>(next) * dim,
                        dim * sizeof(float));
        }
    }

    // ── Lloyd iteration for one chunk ──

    inline void
    kmeans_lloyd(const float* data, uint32_t n, uint32_t dim,
                 float* centroids, uint32_t k, uint32_t iters) {
        std::vector<uint32_t> assignments(n);
        std::vector<float> sums(static_cast<size_t>(k) * dim);
        std::vector<uint32_t> counts(k);

        for (uint32_t iter = 0; iter < iters; iter++) {
            // Assign each point to nearest centroid
            for (uint32_t i = 0; i < n; i++) {
                const float* pt = data + static_cast<size_t>(i) * dim;
                float best_dist = std::numeric_limits<float>::max();
                uint32_t best_k = 0;
                for (uint32_t c = 0; c < k; c++) {
                    float d = l2_distance(pt, centroids + static_cast<size_t>(c) * dim, dim);
                    if (d < best_dist) { best_dist = d; best_k = c; }
                }
                assignments[i] = best_k;
            }

            // Update centroids
            std::fill(sums.begin(), sums.end(), 0.0f);
            std::fill(counts.begin(), counts.end(), 0u);

            for (uint32_t i = 0; i < n; i++) {
                uint32_t c = assignments[i];
                counts[c]++;
                const float* pt = data + static_cast<size_t>(i) * dim;
                float* s = sums.data() + static_cast<size_t>(c) * dim;
                for (uint32_t d = 0; d < dim; d++)
                    s[d] += pt[d];
            }

            for (uint32_t c = 0; c < k; c++) {
                float* cent = centroids + static_cast<size_t>(c) * dim;
                if (counts[c] > 0) {
                    float* s = sums.data() + static_cast<size_t>(c) * dim;
                    for (uint32_t d = 0; d < dim; d++)
                        cent[d] = s[d] / counts[c];
                }
            }
        }
    }

    // ── PQ Training (CPU) ──

    inline pq_result
    train_pq(const float* vectors, uint32_t npts, uint32_t ndims,
             uint32_t n_chunks, uint32_t sample_size,
             uint32_t kmeans_iters) {
        pq_result result;
        result.ndims = ndims;
        result.n_chunks = n_chunks;
        if (n_chunks == 0) n_chunks = ndims;
        result.n_chunks = n_chunks;

        // 1. Compute global centroid
        result.global_centroid = new float[ndims]();
        for (uint32_t i = 0; i < npts; i++) {
            const float* v = vectors + static_cast<size_t>(i) * ndims;
            for (uint32_t d = 0; d < ndims; d++)
                result.global_centroid[d] += v[d];
        }
        for (uint32_t d = 0; d < ndims; d++)
            result.global_centroid[d] /= npts;
        printf("  global centroid computed\n");

        // 2. Compute chunk offsets (uniform partitioning)
        result.chunk_offsets = new uint32_t[n_chunks + 1];
        for (uint32_t c = 0; c <= n_chunks; c++)
            result.chunk_offsets[c] = c * ndims / n_chunks;
        result.chunk_offsets[n_chunks] = ndims;

        // 3. Sample and center vectors
        // Use all vectors when sample_size >= npts (important for 1D chunks
        // where 256 centroids must align with all distinct values)
        uint32_t ns = std::min(sample_size, npts);
        std::mt19937 rng(42);

        std::vector<uint32_t> sample_ids;
        if (ns >= npts) {
            // Use all vectors
            ns = npts;
            sample_ids.resize(npts);
            for (uint32_t i = 0; i < npts; i++) sample_ids[i] = i;
        } else {
            sample_ids.resize(npts);
            for (uint32_t i = 0; i < npts; i++) sample_ids[i] = i;
            std::shuffle(sample_ids.begin(), sample_ids.end(), rng);
            sample_ids.resize(ns);
        }

        // Center the samples
        std::vector<float> samples(static_cast<size_t>(ns) * ndims);
        for (uint32_t i = 0; i < ns; i++) {
            const float* v = vectors + static_cast<size_t>(sample_ids[i]) * ndims;
            float* s = samples.data() + static_cast<size_t>(i) * ndims;
            for (uint32_t d = 0; d < ndims; d++)
                s[d] = v[d] - result.global_centroid[d];
        }
        printf("  %s %u vectors, centered\n", ns == npts ? "using all" : "sampled", ns);

        // 4. Per-chunk k-means
        result.centroids = new float[static_cast<size_t>(NUM_PQ_CENTROIDS) * ndims]();

        for (uint32_t c = 0; c < n_chunks; c++) {
            uint32_t dim_start = result.chunk_offsets[c];
            uint32_t dim_end = result.chunk_offsets[c + 1];
            uint32_t chunk_dim = dim_end - dim_start;

            // Extract chunk sub-dimensions from centered samples
            std::vector<float> chunk_data(static_cast<size_t>(ns) * chunk_dim);
            for (uint32_t i = 0; i < ns; i++) {
                const float* s = samples.data() + static_cast<size_t>(i) * ndims + dim_start;
                float* cd = chunk_data.data() + static_cast<size_t>(i) * chunk_dim;
                std::memcpy(cd, s, chunk_dim * sizeof(float));
            }

            // k-means++ init + Lloyd
            std::vector<float> chunk_cents(static_cast<size_t>(NUM_PQ_CENTROIDS) * chunk_dim);
            kmeans_pp_init(chunk_data.data(), ns, chunk_dim,
                          chunk_cents.data(), NUM_PQ_CENTROIDS, rng);
            kmeans_lloyd(chunk_data.data(), ns, chunk_dim,
                        chunk_cents.data(), NUM_PQ_CENTROIDS, kmeans_iters);

            // Copy chunk centroids to full centroid table
            for (uint32_t k = 0; k < NUM_PQ_CENTROIDS; k++) {
                const float* src = chunk_cents.data() + static_cast<size_t>(k) * chunk_dim;
                float* dst = result.centroids + static_cast<size_t>(k) * ndims + dim_start;
                std::memcpy(dst, src, chunk_dim * sizeof(float));
            }

            if ((c + 1) % 16 == 0 || c + 1 == n_chunks)
                printf("  k-means chunk %u / %u done\n", c + 1, n_chunks);
        }

        // 5. Encode all vectors
        result.pq_codes = new uint8_t[static_cast<size_t>(npts) * n_chunks];

        for (uint32_t i = 0; i < npts; i++) {
            const float* v = vectors + static_cast<size_t>(i) * ndims;
            uint8_t* code = result.pq_codes + static_cast<size_t>(i) * n_chunks;

            for (uint32_t c = 0; c < n_chunks; c++) {
                uint32_t dim_start = result.chunk_offsets[c];
                uint32_t dim_end = result.chunk_offsets[c + 1];

                float best_dist = std::numeric_limits<float>::max();
                uint8_t best_k = 0;
                for (uint32_t k = 0; k < NUM_PQ_CENTROIDS; k++) {
                    float dist = 0.0f;
                    for (uint32_t d = dim_start; d < dim_end; d++) {
                        float centered = v[d] - result.global_centroid[d];
                        float diff = centered - result.centroids[static_cast<size_t>(k) * ndims + d];
                        dist += diff * diff;
                    }
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_k = static_cast<uint8_t>(k);
                    }
                }
                code[c] = best_k;
            }

            if (i % 200000 == 0 && i > 0)
                printf("  encoding %u / %u\n", i, npts);
        }
        printf("  encoding complete: %u vectors × %u chunks\n", npts, n_chunks);

        return result;
    }

    // ── Write PQ output files ──

    inline void
    write_pq_files(const pq_result& pq, const std::string& prefix, uint32_t npts) {
        // Centroids: [256 × ndims]
        {
            auto path = prefix + "_pq_pivots.bin";
            FILE* f = fopen(path.c_str(), "wb");
            uint32_t nr = NUM_PQ_CENTROIDS, nc = pq.ndims;
            fwrite(&nr, 4, 1, f);
            fwrite(&nc, 4, 1, f);
            fwrite(pq.centroids, sizeof(float),
                   static_cast<size_t>(nr) * nc, f);
            fclose(f);
            printf("  wrote %s (%u × %u)\n", path.c_str(), nr, nc);
        }

        // Global centroid: [1 × ndims]
        {
            auto path = prefix + "_pq_pivots.bin_centroid.bin";
            FILE* f = fopen(path.c_str(), "wb");
            uint32_t nr = 1, nc = pq.ndims;
            fwrite(&nr, 4, 1, f);
            fwrite(&nc, 4, 1, f);
            fwrite(pq.global_centroid, sizeof(float), nc, f);
            fclose(f);
            printf("  wrote %s (1 × %u)\n", path.c_str(), nc);
        }

        // Chunk offsets: [n_chunks+1 × 1]
        {
            auto path = prefix + "_pq_pivots.bin_chunk_offsets.bin";
            FILE* f = fopen(path.c_str(), "wb");
            uint32_t nr = pq.n_chunks + 1, nc = 1;
            fwrite(&nr, 4, 1, f);
            fwrite(&nc, 4, 1, f);
            fwrite(pq.chunk_offsets, 4, nr, f);
            fclose(f);
            printf("  wrote %s (%u × 1)\n", path.c_str(), nr);
        }

        // PQ codes: [npts × n_chunks]
        {
            auto path = prefix + "_pq_compressed.bin";
            FILE* f = fopen(path.c_str(), "wb");
            uint32_t nr = npts, nc = pq.n_chunks;
            fwrite(&nr, 4, 1, f);
            fwrite(&nc, 4, 1, f);
            fwrite(pq.pq_codes, 1, static_cast<size_t>(nr) * nc, f);
            fclose(f);
            printf("  wrote %s (%u × %u)\n", path.c_str(), nr, nc);
        }
    }

}

#endif // AISAQ_BUILD_PQ_TRAINER_HH
