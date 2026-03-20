// Generate a minimal DiskANN-format test index for correctness validation
// Usage: gen_test_index <output_prefix> [npts] [ndims] [degree]
//
// Creates:
//   {prefix}_disk.index          — sector 0 metadata + sector 1..N node data
//   {prefix}_pq_pivots.bin       — PQ centroid table (256 × ndims)
//   {prefix}_pq_pivots.bin_centroid.bin       — global centroid
//   {prefix}_pq_pivots.bin_chunk_offsets.bin  — chunk dimension ranges
//   {prefix}_pq_compressed.bin   — PQ codes (npts × n_chunks)
//   {prefix}_query.bin           — 10 random query vectors

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <random>
#include <algorithm>
#include <numeric>

static constexpr uint32_t SECTOR_SIZE = 4096;

// Write DiskANN bin format: uint32_t nrows, uint32_t ncols, T[nrows * ncols]
template <typename T>
void write_bin(const char* path, const T* data, uint32_t nrows, uint32_t ncols) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(&nrows, sizeof(uint32_t), 1, f);
    fwrite(&ncols, sizeof(uint32_t), 1, f);
    fwrite(data, sizeof(T), static_cast<size_t>(nrows) * ncols, f);
    fclose(f);
    printf("  wrote %s (%u × %u)\n", path, nrows, ncols);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <output_prefix> [npts=100] [ndims=8] [degree=4]\n", argv[0]);
        return 1;
    }

    const char* prefix = argv[1];
    uint32_t npts = argc > 2 ? atoi(argv[2]) : 100;
    uint32_t ndims = argc > 3 ? atoi(argv[3]) : 8;
    uint32_t degree = argc > 4 ? atoi(argv[4]) : 4;
    uint32_t n_chunks = std::max(1u, ndims / 4); // 4 dims per chunk

    printf("Generating test index: npts=%u ndims=%u degree=%u n_chunks=%u\n",
           npts, ndims, degree, n_chunks);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Generate random vectors
    std::vector<float> vectors(static_cast<size_t>(npts) * ndims);
    for (auto& v : vectors) v = dist(rng);

    // ── 1. PQ codebook ──

    // Random centroids: 256 × ndims
    std::vector<float> centroids(256 * ndims);
    for (auto& c : centroids) c = dist(rng);
    write_bin((std::string(prefix) + "_pq_pivots.bin").c_str(),
              centroids.data(), 256, ndims);

    // Global centroid: mean of all vectors
    std::vector<float> centroid(ndims, 0.0f);
    for (uint32_t i = 0; i < npts; ++i)
        for (uint32_t j = 0; j < ndims; ++j)
            centroid[j] += vectors[i * ndims + j];
    for (uint32_t j = 0; j < ndims; ++j)
        centroid[j] /= npts;
    write_bin((std::string(prefix) + "_pq_pivots.bin_centroid.bin").c_str(),
              centroid.data(), 1, ndims);

    // Chunk offsets: [0, 4, 8, ..., ndims]
    std::vector<uint32_t> chunk_offsets(n_chunks + 1);
    for (uint32_t c = 0; c <= n_chunks; ++c)
        chunk_offsets[c] = c * ndims / n_chunks;
    chunk_offsets[n_chunks] = ndims; // ensure last = ndims
    write_bin((std::string(prefix) + "_pq_pivots.bin_chunk_offsets.bin").c_str(),
              chunk_offsets.data(), n_chunks + 1, 1);

    // ── 2. PQ compressed data ──

    // Assign nearest centroid per chunk for each vector
    std::vector<uint8_t> pq_codes(static_cast<size_t>(npts) * n_chunks);
    for (uint32_t i = 0; i < npts; ++i) {
        const float* vec = vectors.data() + i * ndims;
        for (uint32_t c = 0; c < n_chunks; ++c) {
            uint32_t dim_start = chunk_offsets[c];
            uint32_t dim_end = chunk_offsets[c + 1];

            // Find nearest centroid in this chunk
            float best_dist = std::numeric_limits<float>::max();
            uint8_t best_k = 0;
            for (uint32_t k = 0; k < 256; ++k) {
                float d = 0;
                for (uint32_t j = dim_start; j < dim_end; ++j) {
                    float diff = centroids[k * ndims + j] - (vec[j] - centroid[j]);
                    d += diff * diff;
                }
                if (d < best_dist) {
                    best_dist = d;
                    best_k = k;
                }
            }
            pq_codes[i * n_chunks + c] = best_k;
        }
    }
    write_bin((std::string(prefix) + "_pq_compressed.bin").c_str(),
              pq_codes.data(), npts, n_chunks);

    // ── 3. Disk index ──

    // Node layout: float[ndims] + uint32_t(degree) + uint32_t[degree] + uint8_t[n_chunks * degree]
    // 4K-aligned: one node per sector, inline PQ for ALL neighbors (DRAM-free search)
    uint32_t max_node_len = ndims * sizeof(float) + sizeof(uint32_t)
                            + degree * sizeof(uint32_t) + degree * n_chunks;
    uint32_t nnodes_per_sector = 1;  // 4K aligned: one node per sector
    uint32_t total_sectors = npts;

    printf("  max_node_len=%u (4K-aligned, 1 node/sector) total_sectors=%u\n",
           max_node_len, total_sectors);

    // Build simple graph: each node connects to degree nearest neighbors (brute force)
    std::vector<std::vector<uint32_t>> neighbors(npts);
    for (uint32_t i = 0; i < npts; ++i) {
        // Compute distances to all other nodes
        std::vector<std::pair<float, uint32_t>> dists;
        for (uint32_t j = 0; j < npts; ++j) {
            if (i == j) continue;
            float d = 0;
            for (uint32_t k = 0; k < ndims; ++k) {
                float diff = vectors[i * ndims + k] - vectors[j * ndims + k];
                d += diff * diff;
            }
            dists.push_back({d, j});
        }
        std::sort(dists.begin(), dists.end());
        for (uint32_t d = 0; d < std::min(degree, (uint32_t)dists.size()); ++d)
            neighbors[i].push_back(dists[d].second);
    }

    // Medoid: node with minimum average distance
    uint32_t medoid = 0;
    float min_avg = std::numeric_limits<float>::max();
    for (uint32_t i = 0; i < npts; ++i) {
        float avg = 0;
        for (uint32_t j = 0; j < npts; ++j) {
            float d = 0;
            for (uint32_t k = 0; k < ndims; ++k) {
                float diff = vectors[i * ndims + k] - vectors[j * ndims + k];
                d += diff * diff;
            }
            avg += d;
        }
        if (avg < min_avg) { min_avg = avg; medoid = i; }
    }
    printf("  medoid=%u\n", medoid);

    std::string disk_path = std::string(prefix) + "_disk.index";
    FILE* f = fopen(disk_path.c_str(), "wb");
    if (!f) { perror(disk_path.c_str()); return 1; }

    // Sector 0: metadata
    uint64_t meta[64] = {};
    meta[0] = npts;
    meta[1] = ndims;
    meta[2] = medoid;
    meta[3] = max_node_len;
    meta[4] = nnodes_per_sector;
    meta[5] = 0; // num_frozen
    meta[6] = 0; // frozen_loc
    meta[7] = 0; // has_reorder
    // no reorder fields
    meta[8] = (1 + total_sectors) * SECTOR_SIZE; // file_size
    meta[9] = degree;  // inline_pq_width = max_degree (all neighbors have inline PQ)
    meta[10] = 0; // rearrange_flag

    uint8_t sector_buf[SECTOR_SIZE] = {};
    memcpy(sector_buf, meta, sizeof(meta));
    fwrite(sector_buf, 1, SECTOR_SIZE, f);

    // Sectors 1..N: one node per sector (4K-aligned)
    for (uint32_t node_id = 0; node_id < npts; ++node_id) {
        memset(sector_buf, 0, SECTOR_SIZE);
        uint8_t* ptr = sector_buf;

        // Copy vector
        memcpy(ptr, vectors.data() + node_id * ndims, ndims * sizeof(float));
        ptr += ndims * sizeof(float);

        // Neighbor count
        uint32_t deg = neighbors[node_id].size();
        memcpy(ptr, &deg, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        // Neighbor IDs
        memcpy(ptr, neighbors[node_id].data(), deg * sizeof(uint32_t));
        ptr += deg * sizeof(uint32_t);

        // Inline PQ codes for each neighbor (DRAM-free: embedded in sector)
        for (uint32_t d = 0; d < deg; ++d) {
            uint32_t nbr = neighbors[node_id][d];
            memcpy(ptr, pq_codes.data() + nbr * n_chunks, n_chunks);
            ptr += n_chunks;
        }

        fwrite(sector_buf, 1, SECTOR_SIZE, f);
    }
    fclose(f);
    printf("  wrote %s (%u sectors)\n", disk_path.c_str(), 1 + total_sectors);

    // ── 4. Medoid centroids file ──
    write_bin((std::string(prefix) + "_disk.index_centroids.bin").c_str(),
              vectors.data() + medoid * ndims, 1, ndims);

    // ── 5. Query vectors ──
    uint32_t nqueries = 10;
    std::vector<float> queries(nqueries * ndims);
    for (auto& q : queries) q = dist(rng);
    write_bin((std::string(prefix) + "_query.bin").c_str(),
              queries.data(), nqueries, ndims);

    // ── 6. Ground truth (brute-force exact search) ──
    printf("\nGround truth (brute-force top-10):\n");
    for (uint32_t q = 0; q < nqueries; ++q) {
        const float* qvec = queries.data() + q * ndims;
        std::vector<std::pair<float, uint32_t>> dists;
        for (uint32_t i = 0; i < npts; ++i) {
            float d = 0;
            for (uint32_t j = 0; j < ndims; ++j) {
                float diff = qvec[j] - vectors[i * ndims + j];
                d += diff * diff;
            }
            dists.push_back({d, i});
        }
        std::sort(dists.begin(), dists.end());

        printf("  Query %u: ", q);
        for (uint32_t i = 0; i < std::min(10u, npts); ++i)
            printf("%u(%.3f) ", dists[i].second, dists[i].first);
        printf("\n");
    }

    printf("\nDone! Run: apps.aisaq %s %s_query.bin\n", prefix, prefix);
    return 0;
}
