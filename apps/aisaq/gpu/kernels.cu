// AiSAQ GPU kernels for PQ training
//
// Compiled to PTX: nvcc --ptx kernels.cu -o aisaq_kernels.ptx

#include <cstdint>

extern "C" {

// ── Global centroid: sum all vectors per dimension ──
// Grid: (ndims), Block: (256)
// Each block handles one dimension, reducing across all vectors.
__global__ void
mean_reduce(const float* vectors, float* output, uint32_t npts, uint32_t ndims) {
    uint32_t dim = blockIdx.x;
    if (dim >= ndims) return;

    float sum = 0.0f;
    for (uint32_t i = threadIdx.x; i < npts; i += blockDim.x)
        sum += vectors[(size_t)i * ndims + dim];

    // Warp reduction
    for (int offset = warpSize / 2; offset > 0; offset >>= 1)
        sum += __shfl_down_sync(0xFFFFFFFF, sum, offset);

    __shared__ float sdata[32];
    int lane = threadIdx.x % warpSize;
    int wid = threadIdx.x / warpSize;

    if (lane == 0) sdata[wid] = sum;
    __syncthreads();

    // Final reduction by first warp
    if (threadIdx.x < blockDim.x / warpSize) {
        sum = sdata[threadIdx.x];
        for (int offset = (blockDim.x / warpSize) / 2; offset > 0; offset >>= 1)
            sum += __shfl_down_sync(0xFFFFFFFF, sum, offset);
    }

    if (threadIdx.x == 0)
        output[dim] = sum;
}

// ── k-means++: distance to nearest existing seed ──
// Grid: (ceil(N/256)), Block: (256)
// For each sample, compute distance to the latest seed and update min_dists.
// Samples are strided: sample i, chunk dims = samples[i * stride + dim_start .. dim_end)
__global__ void
kmeans_pp_dist(const float* samples, const float* seeds, float* min_dists,
               uint32_t N, uint32_t num_seeds, uint32_t chunk_dim,
               uint32_t stride, uint32_t dim_start) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= N) return;

    const float* pt = samples + (size_t)tid * stride + dim_start;
    const float* seed = seeds + (size_t)(num_seeds - 1) * chunk_dim;

    float dist = 0.0f;
    for (uint32_t d = 0; d < chunk_dim; d++) {
        float diff = pt[d] - seed[d];
        dist += diff * diff;
    }

    if (num_seeds == 1)
        min_dists[tid] = dist;
    else
        min_dists[tid] = fminf(min_dists[tid], dist);
}

// ── Lloyd iteration: assign + accumulate ──
// Grid: (ceil(N/256)), Block: (256)
// Each thread: find nearest centroid for one sample, atomicAdd to sums/counts.
// Samples are strided (same as kmeans_pp_dist).
__global__ void
kmeans_assign_accum(const float* samples, const float* centroids,
                    float* sums, uint32_t* counts,
                    uint32_t N, uint32_t K, uint32_t chunk_dim,
                    uint32_t stride, uint32_t dim_start) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= N) return;

    const float* pt = samples + (size_t)tid * stride + dim_start;

    float best_dist = 1e30f;
    uint32_t best_k = 0;
    for (uint32_t k = 0; k < K; k++) {
        const float* cent = centroids + (size_t)k * chunk_dim;
        float dist = 0.0f;
        for (uint32_t d = 0; d < chunk_dim; d++) {
            float diff = pt[d] - cent[d];
            dist += diff * diff;
        }
        if (dist < best_dist) { best_dist = dist; best_k = k; }
    }

    atomicAdd(&counts[best_k], 1u);
    for (uint32_t d = 0; d < chunk_dim; d++)
        atomicAdd(&sums[best_k * chunk_dim + d], pt[d]);
}

// ── Lloyd: divide sums by counts to get new centroids ──
// Grid: (K), Block: (chunk_dim) — or (256) if chunk_dim > 256
__global__ void
kmeans_divide(float* centroids, const float* sums, const uint32_t* counts,
              uint32_t chunk_dim) {
    uint32_t k = blockIdx.x;
    uint32_t d = threadIdx.x;
    if (d >= chunk_dim) return;

    uint32_t cnt = counts[k];
    centroids[k * chunk_dim + d] = (cnt > 0) ? sums[k * chunk_dim + d] / cnt : 0.0f;
}

// ── PQ encode: assign each vector to nearest centroid per chunk ──
// Grid: (ceil(npts/256)), Block: (256)
// Each thread handles one vector across all chunks.
__global__ void
pq_encode(const float* vectors, const float* global_centroid,
          const float* centroids, uint8_t* codes,
          uint32_t npts, uint32_t ndims,
          uint32_t n_chunks, const uint32_t* chunk_offsets) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= npts) return;

    const float* vec = vectors + (size_t)tid * ndims;

    for (uint32_t c = 0; c < n_chunks; c++) {
        uint32_t dim_start = chunk_offsets[c];
        uint32_t dim_end = chunk_offsets[c + 1];

        float best_dist = 1e30f;
        uint8_t best_k = 0;

        for (uint32_t k = 0; k < 256; k++) {
            float dist = 0.0f;
            for (uint32_t d = dim_start; d < dim_end; d++) {
                float centered = vec[d] - global_centroid[d];
                float diff = centered - centroids[(size_t)k * ndims + d];
                dist += diff * diff;
            }
            if (dist < best_dist) {
                best_dist = dist;
                best_k = (uint8_t)k;
            }
        }

        codes[(size_t)tid * n_chunks + c] = best_k;
    }
}

} // extern "C"
