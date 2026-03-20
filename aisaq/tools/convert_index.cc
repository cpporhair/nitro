// Convert aisaq-diskann packed index to PUMP 4K-aligned format
//
// Usage: convert_index <src_prefix> <dst_prefix>
//
// Reads:  {src_prefix}_disk.index        (packed, with bin header)
//         {src_prefix}_pq_compressed.bin  (to detect n_chunks)
// Writes: {dst_prefix}_disk.index        (4K-aligned, 1 node/sector)
// Copies: PQ files are format-compatible, symlinked to dst_prefix

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>

static constexpr uint32_t SECTOR_SIZE = 4096;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 3) {
        printf("Usage: %s <src_prefix> <dst_prefix>\n", argv[0]);
        printf("  Converts aisaq-diskann packed index to PUMP 4K-aligned format.\n");
        return 1;
    }

    const std::string src_prefix = argv[1];
    const std::string dst_prefix = argv[2];

    // ── 1. Read source metadata ──

    std::string src_disk = src_prefix + "_disk.index";
    FILE* src = fopen(src_disk.c_str(), "rb");
    if (!src) { perror(src_disk.c_str()); return 1; }

    // Read sector 0
    uint8_t sector_buf[SECTOR_SIZE];
    fread(sector_buf, 1, SECTOR_SIZE, src);

    // aisaq-diskann format: 8-byte bin header (uint32 nr, uint32 nc) + uint64[] metadata
    uint32_t bin_nr = *reinterpret_cast<uint32_t*>(sector_buf);
    uint32_t bin_nc = *reinterpret_cast<uint32_t*>(sector_buf + 4);
    printf("Source bin header: nr=%u nc=%u\n", bin_nr, bin_nc);

    const auto* vals = reinterpret_cast<const uint64_t*>(sector_buf + 8);
    uint64_t npts              = vals[0];
    uint64_t ndims             = vals[1];
    uint64_t medoid            = vals[2];
    uint64_t src_max_node_len  = vals[3];
    uint64_t src_nnodes_per_sector = vals[4];
    uint64_t num_frozen        = vals[5];
    uint64_t frozen_loc        = vals[6];
    uint64_t has_reorder       = vals[7];
    uint64_t src_file_size     = vals[8];
    uint64_t inline_pq_width   = vals[9];
    uint64_t rearrange_flag    = vals[10];

    printf("Source: npts=%lu ndims=%lu medoid=%lu\n", npts, ndims, medoid);
    printf("  max_node_len=%lu nnodes_per_sector=%lu inline_pq=%lu\n",
           src_max_node_len, src_nnodes_per_sector, inline_pq_width);

    // ── 2. Detect n_chunks from _pq_compressed.bin ──

    std::string pq_path = src_prefix + "_pq_compressed.bin";
    FILE* pq_file = fopen(pq_path.c_str(), "rb");
    if (!pq_file) { perror(pq_path.c_str()); return 1; }
    uint32_t pq_nrows, pq_ncols;
    fread(&pq_nrows, 4, 1, pq_file);
    fread(&pq_ncols, 4, 1, pq_file);
    fclose(pq_file);

    uint32_t n_chunks = pq_ncols;
    printf("  n_chunks=%u (from pq_compressed)\n", n_chunks);

    if (inline_pq_width == 0) {
        printf("ERROR: source index has no inline PQ (inline_pq_width=0).\n");
        printf("  Rebuild with: --use_aisaq --inline_pq <max_degree>\n");
        return 1;
    }

    // ── 3. Compute target parameters ──

    // Source node layout (fixed size, padded to max_degree):
    //   float[ndims] + uint32(nnbrs) + uint32[max_degree] + uint8[inline_pq * n_chunks]
    uint64_t max_degree = inline_pq_width; // inline_pq=64 means max_degree=64
    uint64_t src_coords_size = ndims * sizeof(float);
    uint64_t src_neighbors_offset = src_coords_size + sizeof(uint32_t);
    uint64_t src_pq_offset = src_neighbors_offset + max_degree * sizeof(uint32_t);

    // Target: 4K-aligned, actual degree (no padding), may span multiple sectors
    uint64_t dst_max_node_len = ndims * sizeof(float) + sizeof(uint32_t)
                                + max_degree * sizeof(uint32_t)
                                + max_degree * n_chunks;
    uint32_t dst_sectors_per_node = (dst_max_node_len + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint64_t dst_total_sectors = static_cast<uint64_t>(1 + npts) * dst_sectors_per_node;
    uint64_t dst_file_size = dst_total_sectors * SECTOR_SIZE;

    printf("\nTarget: 4K-aligned, %u sectors/node\n", dst_sectors_per_node);
    printf("  max_node_len=%lu total_sectors=%lu file_size=%lu (%.1f GB)\n",
           dst_max_node_len, dst_total_sectors, dst_file_size,
           dst_file_size / (1024.0 * 1024.0 * 1024.0));

    // ── 4. Create output directory ──

    auto dst_dir = std::filesystem::path(dst_prefix).parent_path();
    if (!dst_dir.empty())
        std::filesystem::create_directories(dst_dir);

    // ── 5. Write target _disk.index ──

    std::string dst_disk = dst_prefix + "_disk.index";
    FILE* dst = fopen(dst_disk.c_str(), "wb");
    if (!dst) { perror(dst_disk.c_str()); return 1; }

    // Write target sector 0: our format (no bin header, raw uint64[])
    memset(sector_buf, 0, SECTOR_SIZE);
    auto* meta = reinterpret_cast<uint64_t*>(sector_buf);
    meta[0] = npts;
    meta[1] = ndims;
    meta[2] = medoid;
    meta[3] = dst_max_node_len;
    meta[4] = 1;  // nnodes_per_sector = 1 (4K-aligned)
    meta[5] = num_frozen;
    meta[6] = frozen_loc;
    meta[7] = has_reorder;
    // has_reorder=0, so:
    meta[8] = dst_file_size;
    meta[9] = inline_pq_width;  // = max_degree
    meta[10] = rearrange_flag;
    fwrite(sector_buf, 1, SECTOR_SIZE, dst);

    // Pad metadata to dst_sectors_per_node sectors
    memset(sector_buf, 0, SECTOR_SIZE);
    for (uint32_t i = 1; i < dst_sectors_per_node; ++i)
        fwrite(sector_buf, 1, SECTOR_SIZE, dst);

    // ── 6. Convert nodes ──

    // Source layout: when nnodes_per_sector > 0, nodes packed within sectors;
    // when nnodes_per_sector == 0, each node spans multiple sectors (sector-aligned)
    uint64_t src_sectors_per_node = (src_max_node_len + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint64_t src_node_alloc = (src_nnodes_per_sector > 0)
        ? src_max_node_len  // packed: node size within sector
        : src_sectors_per_node * SECTOR_SIZE;  // sector-aligned: padded to sector boundary

    printf("\nConverting %lu nodes (src: %lu bytes/node, %lu sectors/node)...\n",
           npts, src_node_alloc, src_sectors_per_node);

    // Read buffer large enough for one source node
    std::vector<uint8_t> src_node_buf(src_node_alloc);
    uint64_t current_src_sector = UINT64_MAX;
    // For packed mode: cache the current source sector
    uint8_t src_sector_buf[SECTOR_SIZE];

    for (uint64_t node_id = 0; node_id < npts; ++node_id) {
        const uint8_t* node_base;

        if (src_nnodes_per_sector > 0) {
            // Packed: multiple nodes per sector
            uint64_t src_sector = 1 + node_id / src_nnodes_per_sector;
            uint64_t src_slot = node_id % src_nnodes_per_sector;
            if (src_sector != current_src_sector) {
                fseek(src, src_sector * SECTOR_SIZE, SEEK_SET);
                fread(src_sector_buf, 1, SECTOR_SIZE, src);
                current_src_sector = src_sector;
            }
            node_base = src_sector_buf + src_slot * src_max_node_len;
        } else {
            // Multi-sector: each node at sector-aligned offset
            uint64_t src_offset = SECTOR_SIZE + node_id * src_node_alloc;
            fseek(src, src_offset, SEEK_SET);
            fread(src_node_buf.data(), 1, src_node_alloc, src);
            node_base = src_node_buf.data();
        }

        // Parse source node
        const float* coords = reinterpret_cast<const float*>(node_base);
        uint32_t nnbrs = *reinterpret_cast<const uint32_t*>(node_base + src_coords_size);
        const uint32_t* neighbor_ids = reinterpret_cast<const uint32_t*>(
            node_base + src_neighbors_offset);
        const uint8_t* inline_pq = node_base + src_pq_offset;

        // Write target node (multi-sector, zero-padded)
        std::vector<uint8_t> node_buf(static_cast<size_t>(dst_sectors_per_node) * SECTOR_SIZE, 0);
        uint8_t* ptr = node_buf.data();

        // Coordinates
        memcpy(ptr, coords, ndims * sizeof(float));
        ptr += ndims * sizeof(float);

        // Neighbor count
        memcpy(ptr, &nnbrs, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        // Neighbor IDs (actual degree, no padding)
        memcpy(ptr, neighbor_ids, nnbrs * sizeof(uint32_t));
        ptr += nnbrs * sizeof(uint32_t);

        // Inline PQ codes (actual degree neighbors)
        memcpy(ptr, inline_pq, nnbrs * n_chunks);

        fwrite(node_buf.data(), 1, node_buf.size(), dst);

        if (node_id % 200000 == 0)
            printf("  %lu / %lu nodes\n", node_id, npts);
    }

    fclose(src);
    fclose(dst);
    printf("  %lu / %lu nodes\n", npts, npts);
    printf("Wrote %s (%.1f GB)\n", dst_disk.c_str(),
           dst_file_size / (1024.0 * 1024.0 * 1024.0));

    // ── 7. Split DiskANN compound _pq_pivots.bin into separate files ──
    //
    // DiskANN format: compound file with multiple bin matrices:
    //   offset 0:    chunk_offsets  (n_chunks+1 × 1, uint32)
    //   offset 4096: centroids     (256 × ndims, float)
    //   after centroids: global_centroid (ndims × 1, float)
    //   after centroid:  chunk_offsets duplicate

    // DiskANN _pq_pivots.bin compound format:
    //   offset 0:     file-offset metadata (NOT chunk_offsets)
    //   offset 4096:  centroids (256 × ndims, float) as bin matrix
    //   after centroids: global centroid (ndims × 1, float) as bin matrix
    //   after centroid:  real chunk_offsets (n_chunks+1 × 1, uint32) as bin matrix
    {
        std::string pivots_path = src_prefix + "_pq_pivots.bin";
        FILE* pf = fopen(pivots_path.c_str(), "rb");
        if (!pf) { perror(pivots_path.c_str()); return 1; }

        // Read centroids from offset 4096
        fseek(pf, SECTOR_SIZE, SEEK_SET);
        uint32_t ct_nr, ct_nc;
        fread(&ct_nr, 4, 1, pf);
        fread(&ct_nc, 4, 1, pf);
        auto* centroids = new float[ct_nr * ct_nc];
        fread(centroids, sizeof(float), ct_nr * ct_nc, pf);

        // Write centroids as _pq_pivots.bin (our format: just the centroid table)
        {
            std::string out = dst_prefix + "_pq_pivots.bin";
            FILE* of = fopen(out.c_str(), "wb");
            fwrite(&ct_nr, 4, 1, of);
            fwrite(&ct_nc, 4, 1, of);
            fwrite(centroids, sizeof(float), ct_nr * ct_nc, of);
            fclose(of);
            printf("  wrote %s (%u × %u)\n", out.c_str(), ct_nr, ct_nc);
        }
        delete[] centroids;

        // Read global centroid (immediately after centroids)
        uint32_t gc_nr, gc_nc;
        fread(&gc_nr, 4, 1, pf);
        fread(&gc_nc, 4, 1, pf);
        auto* global_centroid = new float[gc_nr * gc_nc];
        fread(global_centroid, sizeof(float), gc_nr * gc_nc, pf);

        // Write global centroid (swap to row vector: 1 × ndims)
        {
            std::string out = dst_prefix + "_pq_pivots.bin_centroid.bin";
            FILE* of = fopen(out.c_str(), "wb");
            uint32_t gc_total = gc_nr * gc_nc;
            uint32_t one = 1;
            fwrite(&one, 4, 1, of);
            fwrite(&gc_total, 4, 1, of);
            fwrite(global_centroid, sizeof(float), gc_total, of);
            fclose(of);
            printf("  wrote %s (1 × %u)\n", out.c_str(), gc_total);
        }
        delete[] global_centroid;

        // Read real chunk_offsets (after global centroid)
        uint32_t co_nr, co_nc;
        fread(&co_nr, 4, 1, pf);
        fread(&co_nc, 4, 1, pf);
        auto* chunk_offsets = new uint32_t[co_nr * co_nc];
        fread(chunk_offsets, 4, co_nr * co_nc, pf);

        // Write chunk_offsets as separate file
        {
            std::string out = dst_prefix + "_pq_pivots.bin_chunk_offsets.bin";
            FILE* of = fopen(out.c_str(), "wb");
            fwrite(&co_nr, 4, 1, of);
            fwrite(&co_nc, 4, 1, of);
            fwrite(chunk_offsets, 4, co_nr * co_nc, of);
            fclose(of);
            printf("  wrote %s (%u × %u) = [%u..%u]\n",
                   out.c_str(), co_nr, co_nc, chunk_offsets[0], chunk_offsets[co_nr * co_nc - 1]);
        }
        delete[] chunk_offsets;

        fclose(pf);
    }

    // ── 8. Link/copy remaining compatible files ──

    auto src_abs = std::filesystem::absolute(src_prefix);
    const char* link_suffixes[] = {
        "_pq_compressed.bin",
        "_disk.index_centroids.bin",
        "_disk.index_entry_points.bin",
    };

    for (const auto* suffix : link_suffixes) {
        auto src_path = std::filesystem::path(src_abs.string() + suffix);
        auto dst_path = std::filesystem::path(dst_prefix + suffix);
        if (std::filesystem::exists(src_path)) {
            std::filesystem::remove(dst_path);
            std::filesystem::create_symlink(src_path, dst_path);
            printf("  linked %s\n", suffix);
        }
    }

    printf("\nDone! Run:\n");
    printf("  apps.aisaq %s <query_file> <nvme_addr>\n", dst_prefix.c_str());
    return 0;
}
