#ifndef AISAQ_PQ_CODEBOOK_HH
#define AISAQ_PQ_CODEBOOK_HH

#include <cstdint>
#include <vector>
#include <string>
#include <cmath>
#include <stdexcept>
#include <immintrin.h>

#include "../common/types.hh"

namespace aisaq::pq {

    // PQ codebook — loaded from DiskANN's _pq_pivots.bin files
    //
    // Three files:
    //   {prefix}_pq_pivots.bin               — float[256 * ndims] centroid table
    //   {prefix}_pq_pivots.bin_centroid.bin   — float[ndims] global centroid
    //   {prefix}_pq_pivots.bin_chunk_offsets.bin — uint32_t[n_chunks+1] dimension ranges
    struct
    codebook {
        uint32_t ndims = 0;
        uint32_t n_chunks = 0;

        // PQ centroid table: [256 * ndims], row-major (original layout)
        float* centroids = nullptr;


        // Global data centroid: [ndims]
        float* global_centroid = nullptr;

        // Chunk dimension boundaries: [n_chunks + 1]
        // chunk c covers dimensions [chunk_offsets[c], chunk_offsets[c+1])
        uint32_t* chunk_offsets = nullptr;

        ~codebook() {
            delete[] centroids;
            delete[] global_centroid;
            delete[] chunk_offsets;
        }

        codebook() = default;
        codebook(const codebook&) = delete;
        codebook& operator=(const codebook&) = delete;
        codebook(codebook&& o) noexcept
            : ndims(o.ndims), n_chunks(o.n_chunks)
            , centroids(o.centroids)
            , global_centroid(o.global_centroid)
            , chunk_offsets(o.chunk_offsets) {
            o.centroids = nullptr;
            o.global_centroid = nullptr;
            o.chunk_offsets = nullptr;
        }

        void
        load(const std::string& prefix) {
            // 1. Load centroid table
            uint32_t nrows, ncols;
            centroids = load_bin_file<float>(
                (prefix + "_pq_pivots.bin").c_str(), nrows, ncols);
            // nrows = 256 (centroids), ncols = ndims
            if (nrows != NUM_PQ_CENTROIDS)
                throw std::runtime_error("pq_pivots: expected 256 centroids, got " + std::to_string(nrows));
            ndims = ncols;

            // 2. Load global centroid
            uint32_t cn, cd;
            global_centroid = load_bin_file<float>(
                (prefix + "_pq_pivots.bin_centroid.bin").c_str(), cn, cd);
            if (cd != ndims)
                throw std::runtime_error("centroid dim mismatch");

            // 3. Load chunk offsets
            uint32_t on, oc;
            chunk_offsets = load_bin_file<uint32_t>(
                (prefix + "_pq_pivots.bin_chunk_offsets.bin").c_str(), on, oc);
            // on * oc = n_chunks + 1
            n_chunks = on * oc - 1;

            if (chunk_offsets[0] != 0 || chunk_offsets[n_chunks] != ndims)
                throw std::runtime_error("chunk_offsets: invalid range");

        }
    };

    // Pre-computed distance table for a single query
    //
    // dist_table[c * 256 + k] = partial L2 distance from query to
    //     centroid k in chunk c's dimensions
    //
    // PQ distance for vector with codes pq[0..n_chunks-1]:
    //   sum_{c=0}^{n_chunks-1} dist_table[c * 256 + pq[c]]
    struct
    dist_table {
        uint32_t n_chunks = 0;
        // [n_chunks * 256] floats, heap-allocated to avoid stack overflow
        std::vector<float> table;

        // Precompute distance table for a query vector
        // query: float[ndims], will be modified (centered against global centroid)
        void
        precompute(const codebook& cb, const float* query_raw) {
            n_chunks = cb.n_chunks;
            table.resize(n_chunks * NUM_PQ_CENTROIDS);

            // Center query: query_centered = query - global_centroid
            alignas(32) float query[2048]; // max ndims
            for (uint32_t j = 0; j < cb.ndims; ++j)
                query[j] = query_raw[j] - cb.global_centroid[j];

            // For each chunk, compute partial L2 to all 256 centroids
            for (uint32_t c = 0; c < cb.n_chunks; ++c) {
                const uint32_t dim_start = cb.chunk_offsets[c];
                const uint32_t dim_end = cb.chunk_offsets[c + 1];
                const uint32_t chunk_dims = dim_end - dim_start;
                float* out = table.data() + c * NUM_PQ_CENTROIDS;

                for (uint32_t k = 0; k < NUM_PQ_CENTROIDS; ++k) {
                    const float* centroid = cb.centroids + k * cb.ndims + dim_start;
                    out[k] = l2_partial(query + dim_start, centroid, chunk_dims);
                }
            }
        }

        // Partial L2 distance with AVX2
        static float
        l2_partial(const float* a, const float* b, uint32_t n) {
#ifdef __AVX2__
            __m256 sum8 = _mm256_setzero_ps();
            uint32_t j = 0;
            for (; j + 8 <= n; j += 8) {
                __m256 va = _mm256_loadu_ps(a + j);
                __m256 vb = _mm256_loadu_ps(b + j);
                __m256 diff = _mm256_sub_ps(va, vb);
                sum8 = _mm256_fmadd_ps(diff, diff, sum8);
            }
            // Horizontal sum of 8 floats
            __m128 hi = _mm256_extractf128_ps(sum8, 1);
            __m128 lo = _mm256_castps256_ps128(sum8);
            __m128 sum4 = _mm_add_ps(lo, hi);
            sum4 = _mm_hadd_ps(sum4, sum4);
            sum4 = _mm_hadd_ps(sum4, sum4);
            float dist = _mm_cvtss_f32(sum4);
            // Remainder
            for (; j < n; ++j) {
                float diff = a[j] - b[j];
                dist += diff * diff;
            }
            return dist;
#else
            float dist = 0.0f;
            for (uint32_t j = 0; j < n; ++j) {
                float diff = a[j] - b[j];
                dist += diff * diff;
            }
            return dist;
#endif
        }

        // Compute PQ distance for a single vector given its PQ codes
        [[nodiscard]]
        float
        lookup(const uint8_t* pq_codes) const {
            float dist = 0.0f;
            for (uint32_t c = 0; c < n_chunks; ++c)
                dist += table[c * NUM_PQ_CENTROIDS + pq_codes[c]];
            return dist;
        }

        // Batch PQ distance: compute distances for n vectors
        // pq_codes: [n * n_chunks], row-major (each row = one vector's codes)
        // dists_out: [n] output distances
        void
        batch_lookup(const uint8_t* pq_codes, uint32_t n, float* dists_out) const {
            std::memset(dists_out, 0, n * sizeof(float));

            for (uint32_t c = 0; c < n_chunks; ++c) {
                const float* chunk_table = table.data() + c * NUM_PQ_CENTROIDS;
                if (c + 1 < n_chunks)
                    _mm_prefetch(reinterpret_cast<const char*>(
                        table.data() + (c + 1) * NUM_PQ_CENTROIDS), _MM_HINT_T0);
                for (uint32_t i = 0; i < n; ++i) {
                    dists_out[i] += chunk_table[pq_codes[i * n_chunks + c]];
                }
            }
        }
    };

}

#endif //AISAQ_PQ_CODEBOOK_HH
