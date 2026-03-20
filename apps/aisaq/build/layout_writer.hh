#ifndef AISAQ_BUILD_LAYOUT_WRITER_HH
#define AISAQ_BUILD_LAYOUT_WRITER_HH

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/types.hh"
#include "graph.hh"

namespace aisaq::build {

    // ── Compute layout parameters ──

    struct layout_params {
        uint64_t max_node_len;      // bytes per node
        uint32_t sectors_per_node;  // spn
        uint64_t max_degree;        // = R (max graph degree)
        uint64_t file_size;
    };

    inline layout_params
    compute_layout(uint32_t npts, uint32_t ndims, uint32_t max_degree,
                   uint32_t n_chunks) {
        layout_params lp;
        lp.max_degree = max_degree;
        lp.max_node_len = ndims * sizeof(float)     // coordinates
                        + sizeof(uint32_t)          // degree
                        + max_degree * sizeof(uint32_t)  // neighbor IDs
                        + max_degree * n_chunks;    // inline PQ
        lp.sectors_per_node = (lp.max_node_len + SECTOR_SIZE - 1) / SECTOR_SIZE;
        lp.file_size = static_cast<uint64_t>(1 + npts) * lp.sectors_per_node * SECTOR_SIZE;
        return lp;
    }

    // ── Build metadata sector (sector 0) ──

    inline void
    fill_metadata_sector(uint8_t* buf, uint32_t npts, uint32_t ndims,
                         uint32_t medoid, const layout_params& lp,
                         uint32_t n_chunks, bool rearranged) {
        std::memset(buf, 0, SECTOR_SIZE);
        auto* meta = reinterpret_cast<uint64_t*>(buf);

        meta[0] = npts;
        meta[1] = ndims;
        meta[2] = medoid;
        meta[3] = lp.max_node_len;
        meta[4] = 1;  // nnodes_per_sector = 1 (4K-aligned)
        meta[5] = 0;  // num_frozen
        meta[6] = 0;  // frozen_loc
        meta[7] = 0;  // has_reorder = 0
        // has_reorder=0 layout:
        meta[8] = lp.file_size;
        meta[9] = lp.max_degree;  // inline_pq_width = max_degree (all neighbors)
        meta[10] = rearranged ? 1 : 0;
    }

    // ── Build one node's sector data ──

    inline void
    build_node_sector(uint8_t* buf, uint32_t node_id,
                      const float* vectors, const build_graph& graph,
                      const uint8_t* pq_codes, uint32_t ndims,
                      uint32_t n_chunks, uint32_t sectors_per_node) {
        std::memset(buf, 0, static_cast<size_t>(sectors_per_node) * SECTOR_SIZE);
        uint8_t* ptr = buf;

        // Coordinates
        const float* vec = vectors + static_cast<size_t>(node_id) * ndims;
        std::memcpy(ptr, vec, ndims * sizeof(float));
        ptr += ndims * sizeof(float);

        // Degree
        uint32_t deg = graph.degree(node_id);
        std::memcpy(ptr, &deg, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        // Neighbor IDs
        auto nbrs = graph.get_neighbors(node_id);
        std::memcpy(ptr, nbrs.data(), deg * sizeof(uint32_t));
        ptr += deg * sizeof(uint32_t);

        // Inline PQ codes for each neighbor
        for (uint32_t j = 0; j < deg; j++) {
            uint32_t nbr_id = nbrs[j];
            const uint8_t* code = pq_codes + static_cast<size_t>(nbr_id) * n_chunks;
            std::memcpy(ptr, code, n_chunks);
            ptr += n_chunks;
        }
    }

    // ── Write auxiliary files ──

    inline void
    write_auxiliary_files(const std::string& prefix,
                          uint32_t medoid, const float* vectors,
                          uint32_t ndims,
                          const std::vector<uint32_t>& entry_points) {
        // Medoids file
        {
            auto path = prefix + "_disk.index_medoids.bin";
            FILE* f = fopen(path.c_str(), "wb");
            uint32_t nr = 1, nc = 1;
            fwrite(&nr, 4, 1, f);
            fwrite(&nc, 4, 1, f);
            fwrite(&medoid, 4, 1, f);
            fclose(f);
            printf("  wrote %s\n", path.c_str());
        }

        // Centroids (medoid full-precision vector)
        {
            auto path = prefix + "_disk.index_centroids.bin";
            FILE* f = fopen(path.c_str(), "wb");
            uint32_t nr = 1, nc = ndims;
            fwrite(&nr, 4, 1, f);
            fwrite(&nc, 4, 1, f);
            const float* med_vec = vectors + static_cast<size_t>(medoid) * ndims;
            fwrite(med_vec, sizeof(float), ndims, f);
            fclose(f);
            printf("  wrote %s (1 × %u)\n", path.c_str(), ndims);
        }

        // Entry points
        if (entry_points.size() > 1) {
            auto path = prefix + "_disk.index_entry_points.bin";
            FILE* f = fopen(path.c_str(), "wb");
            auto nr = static_cast<uint32_t>(entry_points.size());
            uint32_t nc = 1;
            fwrite(&nr, 4, 1, f);
            fwrite(&nc, 4, 1, f);
            fwrite(entry_points.data(), 4, nr, f);
            fclose(f);
            printf("  wrote %s (%u entries)\n", path.c_str(), nr);
        }
    }

    // ── Write the _disk.index file (metadata + all node sectors) ──

    inline void
    write_disk_index_file(const std::string& prefix,
                          uint32_t npts, uint32_t ndims, uint32_t medoid,
                          const layout_params& lp, uint32_t n_chunks,
                          bool rearranged,
                          const float* vectors, const build_graph& graph,
                          const uint8_t* pq_codes) {
        auto path = prefix + "_disk.index";
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) throw std::runtime_error("cannot create: " + path);

        uint32_t spn = lp.sectors_per_node;

        // Metadata sector (padded to spn sectors)
        std::vector<uint8_t> meta_buf(static_cast<size_t>(spn) * SECTOR_SIZE, 0);
        fill_metadata_sector(meta_buf.data(), npts, ndims, medoid, lp,
                             n_chunks, rearranged);
        fwrite(meta_buf.data(), 1, meta_buf.size(), f);

        // Node sectors
        std::vector<uint8_t> node_buf(static_cast<size_t>(spn) * SECTOR_SIZE);
        for (uint32_t i = 0; i < npts; i++) {
            build_node_sector(node_buf.data(), i, vectors, graph,
                              pq_codes, ndims, n_chunks, spn);
            fwrite(node_buf.data(), 1, node_buf.size(), f);

            if (i % 200000 == 0 && i > 0)
                printf("  writing node %u / %u\n", i, npts);
        }

        fclose(f);
        printf("  wrote %s (%.1f GB)\n", path.c_str(),
               lp.file_size / (1024.0 * 1024.0 * 1024.0));
    }

}

#endif // AISAQ_BUILD_LAYOUT_WRITER_HH
