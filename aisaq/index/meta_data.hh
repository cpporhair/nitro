#ifndef AISAQ_INDEX_META_DATA_HH
#define AISAQ_INDEX_META_DATA_HH

#include <cstdint>
#include <stdexcept>
#include <string>

#include "../common/types.hh"

namespace aisaq::index {

    // DiskANN _disk.index sector 0 layout:
    // A sequence of uint64_t values at fixed positions
    struct
    meta_data {
        uint64_t npts = 0;              // total vector count
        uint64_t ndims = 0;             // vector dimensionality
        uint64_t medoid = 0;            // entry point node ID
        uint64_t max_node_len = 0;      // bytes per node on disk
        uint64_t nnodes_per_sector = 0; // nodes packed per sector
        uint64_t num_frozen = 0;        // frozen points (usually 0)
        uint64_t frozen_loc = 0;        // frozen point location
        uint64_t has_reorder = 0;       // append reorder data?
        uint64_t reorder_start = 0;     // reorder sector start
        uint64_t reorder_ndims = 0;     // reorder dims
        uint64_t reorder_nvecs = 0;     // reorder vectors per sector
        uint64_t file_size = 0;         // total file size
        uint64_t inline_pq_width = 0;   // inline PQ count (0 = none)
        uint64_t rearrange_flag = 0;    // vector rearrangement applied?

        // Derived values
        uint32_t max_degree = 0;        // computed from max_node_len
        uint32_t pq_len = 0;           // PQ code length (n_chunks)
        uint64_t total_sectors = 0;     // data sectors (excluding sector 0)

        void
        load_from_sector(const void* sector_buf) {
            const auto* vals = static_cast<const uint64_t*>(sector_buf);

            npts              = vals[0];
            ndims             = vals[1];
            medoid            = vals[2];
            max_node_len      = vals[3];
            nnodes_per_sector = vals[4];
            num_frozen        = vals[5];
            frozen_loc        = vals[6];
            has_reorder       = vals[7];

            if (has_reorder) {
                reorder_start  = vals[8];
                reorder_ndims  = vals[9];
                reorder_nvecs  = vals[10];
                file_size      = vals[11];
                inline_pq_width = vals[12];
                rearrange_flag = vals[13];
            } else {
                file_size       = vals[8];
                inline_pq_width = vals[9];
                rearrange_flag  = vals[10];
            }

            compute_derived();
        }

        // Load from _disk.index file (read first sector via stdio)
        void
        load_from_file(const std::string& path) {
            auto* f = fopen(path.c_str(), "rb");
            if (!f)
                throw std::runtime_error("failed to open: " + path);

            alignas(SECTOR_SIZE) uint8_t buf[SECTOR_SIZE];
            if (fread(buf, 1, SECTOR_SIZE, f) != SECTOR_SIZE) {
                fclose(f);
                throw std::runtime_error("failed to read metadata sector: " + path);
            }
            fclose(f);

            load_from_sector(buf);
        }

        // Called after codebook is loaded to set PQ code length and
        // recompute max_degree accounting for inline PQ space
        void
        set_pq_len(uint32_t n_chunks) {
            pq_len = n_chunks;
            if (inline_pq_width > 0) {
                // max_node_len = ndims*4 + 4 + degree*4 + inline_pq_width*n_chunks
                uint64_t fixed = ndims * sizeof(float) + sizeof(uint32_t)
                                 + inline_pq_width * n_chunks;
                max_degree = (max_node_len > fixed)
                    ? (max_node_len - fixed) / sizeof(uint32_t) : 0;
            }
        }

    private:
        void
        compute_derived() {
            // max_node_len = sizeof(float) * ndims + 4 + 4 * degree [+ inline PQ]
            uint64_t coords_size = ndims * sizeof(float);
            uint64_t base_overhead = coords_size + sizeof(uint32_t);
            if (inline_pq_width == 0 && max_node_len > base_overhead)
                max_degree = (max_node_len - base_overhead) / sizeof(uint32_t);
            // When inline_pq_width > 0, max_degree is computed in set_pq_len()
            // after codebook provides n_chunks

            total_sectors = (npts + nnodes_per_sector - 1) / nnodes_per_sector;
        }
    };

}

#endif //AISAQ_INDEX_META_DATA_HH
