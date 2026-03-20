#ifndef AISAQ_INDEX_NODE_ACCESSOR_HH
#define AISAQ_INDEX_NODE_ACCESSOR_HH

#include <cstdint>

namespace aisaq::index {

    // Parses a single node from a sector buffer
    //
    // Node disk layout:
    //   float[ndims]               — full-precision vector coordinates
    //   uint32_t                   — neighbor count (actual degree)
    //   uint32_t[degree]           — neighbor node IDs
    //   [uint32_t]                 — original ID (if rearranged)
    //   [uint8_t[pq_len] × inline_count] — inline PQ codes for first N neighbors
    struct
    node_accessor {
        const uint8_t* base;
        uint32_t ndims;
        uint32_t pq_len;         // n_chunks (PQ code bytes per vector)
        uint32_t inline_pq_count; // how many neighbors have inline PQ codes

        node_accessor(const void* buf, uint32_t ndims, uint32_t pq_len = 0, uint32_t inline_pq = 0)
            : base(static_cast<const uint8_t*>(buf))
            , ndims(ndims), pq_len(pq_len), inline_pq_count(inline_pq) {}

        [[nodiscard]]
        const float*
        get_vector() const {
            return reinterpret_cast<const float*>(base);
        }

        [[nodiscard]]
        uint32_t
        get_degree() const {
            return *reinterpret_cast<const uint32_t*>(base + ndims * sizeof(float));
        }

        [[nodiscard]]
        const uint32_t*
        get_neighbors() const {
            return reinterpret_cast<const uint32_t*>(base + ndims * sizeof(float) + sizeof(uint32_t));
        }

        [[nodiscard]]
        uint32_t
        get_neighbor(uint32_t i) const {
            return get_neighbors()[i];
        }

        // Get inline PQ codes for the i-th neighbor (only valid if i < inline_pq_count)
        [[nodiscard]]
        const uint8_t*
        get_inline_pq(uint32_t i) const {
            // PQ codes start after: vector + count + neighbors
            const uint8_t* pq_start = base + ndims * sizeof(float)
                                           + sizeof(uint32_t)
                                           + get_degree() * sizeof(uint32_t);
            return pq_start + i * pq_len;
        }

        // Check if the i-th neighbor has inline PQ codes
        [[nodiscard]]
        bool
        has_inline_pq(uint32_t i) const {
            return inline_pq_count > 0 && i < inline_pq_count;
        }
    };

    // Compute the device index and sector ID for a given node ID.
    // Node-level striping: node k → device k % num_devices.
    // Within each device, nodes are packed contiguously with a metadata gap at sector 0.
    // For num_devices=1: identical to original (node_id + 1) * spn.
    struct
    sector_locator {
        uint64_t nnodes_per_sector;
        uint32_t max_node_len;
        uint32_t sectors_per_node;  // ceil(max_node_len / 4096)
        uint32_t num_devices = 1;   // number of NVMe devices (node-level striping)

        [[nodiscard]]
        uint32_t
        get_device(uint64_t node_id) const {
            return node_id % num_devices;
        }

        [[nodiscard]]
        uint64_t
        get_sector_id(uint64_t node_id) const {
            // For N=1: (node_id / 1 + 1) * spn = (node_id + 1) * spn (backward compatible)
            // For N>1: nodes distributed across devices, each device has its own sector space
            return (node_id / num_devices + 1) * sectors_per_node;
        }

        [[nodiscard]]
        uint32_t
        get_offset_in_sector(uint64_t node_id) const {
            return 0; // 4K-aligned: node always starts at offset 0
        }
    };

}

#endif //AISAQ_INDEX_NODE_ACCESSOR_HH
