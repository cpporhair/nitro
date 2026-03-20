#ifndef AISAQ_INDEX_INDEX_HH
#define AISAQ_INDEX_INDEX_HH

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>

#include "meta_data.hh"
#include "node_accessor.hh"
#include "../pq/codebook.hh"
#include "../common/types.hh"
#include "../nvme/sector_page.hh"

namespace aisaq::index {

    // Static cache: frequently accessed nodes (BFS from medoid) kept in memory
    struct
    node_cache {
        // node_id → heap-allocated copy of the node's sector data
        std::unordered_map<uint32_t, uint8_t*> nodes;

        ~node_cache() {
            for (auto& [id, buf] : nodes)
                delete[] buf;
        }

        void
        insert(uint32_t node_id, const void* sector_buf, uint32_t offset, uint32_t node_len) {
            auto* copy = new uint8_t[node_len];
            std::memcpy(copy, static_cast<const uint8_t*>(sector_buf) + offset, node_len);
            nodes[node_id] = copy;
        }

        [[nodiscard]]
        const uint8_t*
        lookup(uint32_t node_id) const {
            auto it = nodes.find(node_id);
            return it != nodes.end() ? it->second : nullptr;
        }

        [[nodiscard]]
        bool
        contains(uint32_t node_id) const {
            return nodes.contains(node_id);
        }
    };

    // Complete index state — loaded once, shared (read-only) across search requests
    struct
    disk_index {
        meta_data meta;
        pq::codebook codebook;
        sector_locator locator;

        // Static cache
        node_cache cache;

        // Medoid full-precision vector (for initial distance computation)
        float* medoid_vector = nullptr;

        // Entry point IDs (medoid first, then multi-entry points)
        std::vector<uint32_t> all_entry_ids;

        // PQ codes for entry points only (tiny: at most ~1000 * n_chunks bytes)
        // entry_pq_codes[i * n_chunks .. (i+1) * n_chunks) = PQ codes for all_entry_ids[i]
        uint8_t* entry_pq_codes = nullptr;

        ~disk_index() {
            delete[] entry_pq_codes;
            delete[] medoid_vector;
        }

        disk_index() = default;
        disk_index(const disk_index&) = delete;
        disk_index& operator=(const disk_index&) = delete;

        // Load index from files
        void
        load(const std::string& prefix) {
            // 1. Load metadata from _disk.index sector 0
            meta.load_from_file(prefix + "_disk.index");
            printf("  loaded metadata: npts=%lu ndims=%lu medoid=%lu nnodes/sector=%lu\n",
                   meta.npts, meta.ndims, meta.medoid, meta.nnodes_per_sector);

            // 2. Load PQ codebook
            codebook.load(prefix);
            printf("  loaded PQ codebook: ndims=%u n_chunks=%u\n",
                   codebook.ndims, codebook.n_chunks);

            // 3. Set PQ length in metadata → recompute max_degree for inline PQ
            meta.set_pq_len(codebook.n_chunks);

            // 4. Set up sector locator + DMA pool for node reads
            uint32_t spn = (meta.max_node_len + SECTOR_SIZE - 1) / SECTOR_SIZE;
            locator = {meta.nnodes_per_sector, static_cast<uint32_t>(meta.max_node_len), spn};
            nvme::init_node_pool(spn);
            if (spn > 1)
                printf("  multi-sector nodes: %u sectors/node (%lu bytes)\n", spn, meta.max_node_len);

            // 5. Load medoid centroids (full-precision vector for entry point)
            load_medoid_vector(prefix);

            // 6. Load multiple entry points (optional)
            load_multi_entry_points(prefix);

            // 7. Load PQ codes for entry points only (not full dataset)
            load_entry_point_pq_codes(prefix);

            if (meta.inline_pq_width > 0)
                printf("  inline PQ: width=%lu (DRAM-free search)\n", meta.inline_pq_width);
            else
                printf("  warning: inline_pq_width=0, search requires in-memory PQ fallback\n");
        }

        // Get PQ codes for the i-th entry point (index into all_entry_ids)
        [[nodiscard]]
        const uint8_t*
        get_entry_pq_codes(uint32_t idx) const {
            return entry_pq_codes + static_cast<size_t>(idx) * codebook.n_chunks;
        }

    private:
        void
        load_medoid_vector(const std::string& prefix) {
            auto path = prefix + "_disk.index_centroids.bin";
            auto* f = fopen(path.c_str(), "rb");
            if (!f) {
                printf("  warning: medoid centroids file not found\n");
                return;
            }
            fclose(f);

            uint32_t nrows, ncols;
            medoid_vector = load_bin_file<float>(path.c_str(), nrows, ncols);
            printf("  loaded medoid vector: %u × %u\n", nrows, ncols);
        }

        void
        load_multi_entry_points(const std::string& prefix) {
            auto path = prefix + "_disk.index_entry_points.bin";
            auto* f = fopen(path.c_str(), "rb");
            if (!f)
                return;
            fclose(f);

            uint32_t nrows, ncols;
            auto* data = load_bin_file<uint32_t>(path.c_str(), nrows, ncols);
            std::vector<uint32_t> eps(data, data + static_cast<size_t>(nrows) * ncols);
            delete[] data;
            printf("  loaded %zu multi-entry points\n", eps.size());

            // Store for later use in load_entry_point_pq_codes
            for (auto id : eps)
                if (id != meta.medoid)
                    all_entry_ids.push_back(id);
        }

        void
        load_entry_point_pq_codes(const std::string& prefix) {
            // Build entry ID list: medoid first
            all_entry_ids.insert(all_entry_ids.begin(), meta.medoid);

            auto path = prefix + "_pq_compressed.bin";
            auto* f = fopen(path.c_str(), "rb");
            if (!f)
                throw std::runtime_error("cannot open: " + path);

            uint32_t nrows, ncols;
            fread(&nrows, sizeof(uint32_t), 1, f);
            fread(&ncols, sizeof(uint32_t), 1, f);

            uint32_t n = all_entry_ids.size();
            entry_pq_codes = new uint8_t[n * ncols];

            // Read only the rows we need (fseek to each entry point)
            for (uint32_t i = 0; i < n; ++i) {
                long offset = 8 + static_cast<long>(all_entry_ids[i]) * ncols;
                fseek(f, offset, SEEK_SET);
                fread(entry_pq_codes + i * ncols, 1, ncols, f);
            }
            fclose(f);

            printf("  loaded PQ codes for %u entry points (DRAM-free: no full pq_data)\n", n);
        }
    };

}

#endif //AISAQ_INDEX_INDEX_HH
