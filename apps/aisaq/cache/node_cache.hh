#ifndef AISAQ_CACHE_NODE_CACHE_HH
#define AISAQ_CACHE_NODE_CACHE_HH

#include <atomic>
#include <cstdint>
#include <cstring>

namespace aisaq::cache {

    // Two-layer global node cache (§9 design):
    //
    // Layer 1: atomic presence bitmap — any core reads (relaxed load), only write path sets/clears.
    //   SIFT1M: 128KB, SIFT1B: 125MB (fixed cost, independent of cache size).
    //
    // Layer 2: data pointers + FIFO eviction — managed by write path (single-threaded per scheduler).
    //   node_id → uint8_t* heap buffer. Eviction frees oldest inserted node.
    //
    // Read path (hot, zero scheduling):
    //   bitmap.test(node_id) + data[node_id] pointer deref. No cross-core communication.
    //   Cached data is immutable after insertion (only evicted as a whole).
    //
    // Write path (cold, single-threaded):
    //   insert_copy(node_id, src, len) — alloc + memcpy + set bitmap + FIFO enqueue.
    //   Eviction: FIFO tail removal + clear bitmap + free buffer.
    //   Single-core: called directly. Multi-core: wrap in cache scheduler queue.

    struct
    global_node_cache {
        // Layer 1: presence bitmap
        std::atomic<uint64_t>* bitmap = nullptr;

        // Layer 2: data pointers (indexed by node_id)
        uint8_t** data = nullptr;

        uint64_t npts = 0;
        uint32_t node_len = 0;
        uint32_t max_nodes = 0;

        // FIFO eviction: doubly-linked intrusive list
        struct entry {
            uint32_t node_id;
            entry* prev;
            entry* next;
        };

        entry sentinel{};          // sentinel.next = newest, sentinel.prev = oldest
        entry** entry_index = nullptr;  // [npts] node_id → entry* (nullptr if not cached)
        entry* entry_pool = nullptr;    // pre-allocated pool of max_nodes entries
        uint32_t pool_next = 0;
        entry* free_list = nullptr;
        uint32_t count = 0;

        global_node_cache() = default;
        global_node_cache(const global_node_cache&) = delete;
        global_node_cache& operator=(const global_node_cache&) = delete;

        void
        init(uint64_t npts_, uint32_t node_len_, uint32_t max_nodes_) {
            npts = npts_;
            node_len = node_len_;
            max_nodes = max_nodes_;

            uint64_t bwords = (npts + 63) / 64;
            bitmap = new std::atomic<uint64_t>[bwords]();
            data = new uint8_t*[npts]();
            entry_index = new entry*[npts]();
            entry_pool = new entry[max_nodes];

            sentinel.prev = &sentinel;
            sentinel.next = &sentinel;
        }

        ~global_node_cache() {
            for (uint64_t i = 0; i < npts; ++i)
                delete[] data[i];
            delete[] data;
            delete[] bitmap;
            delete[] entry_index;
            delete[] entry_pool;
        }

        // ── Read path (any core, zero scheduling) ──

        [[nodiscard]]
        bool
        is_cached(uint32_t node_id) const {
            uint64_t word = node_id / 64;
            uint64_t bit = node_id % 64;
            return bitmap[word].load(std::memory_order_relaxed) & (1ULL << bit);
        }

        [[nodiscard]]
        const uint8_t*
        get_data(uint32_t node_id) const {
            return data[node_id];
        }

        // Bitmap test + data pointer lookup. Returns nullptr if not cached.
        [[nodiscard]]
        const uint8_t*
        lookup(uint32_t node_id) const {
            if (!is_cached(node_id)) return nullptr;
            return data[node_id];
        }

        // ── LRU promotion (single-core: call from read path directly) ──
        // Multi-core: wrap in scheduler queue ("touch" request).

        void
        promote(uint32_t node_id) {
            auto* e = entry_index[node_id];
            if (e && e != sentinel.next) {
                list_remove(e);
                list_push_front(e);
            }
        }

        // ── Write path (single-threaded, called by cache scheduler or directly) ──

        // Insert node data. Copies src buffer. Evicts oldest if at capacity.
        // Returns true if inserted, false if already cached.
        bool
        insert_copy(uint32_t node_id, const void* src, uint32_t len) {
            if (is_cached(node_id)) return false;

            // Evict if at capacity
            while (count >= max_nodes)
                evict_oldest();

            // Allocate and copy
            auto* buf = new uint8_t[len];
            std::memcpy(buf, src, len);
            data[node_id] = buf;

            // Set bitmap bit (release so readers on other cores see the data)
            set_bit(node_id);

            // Add to FIFO head (newest)
            auto* e = alloc_entry();
            e->node_id = node_id;
            list_push_front(e);
            entry_index[node_id] = e;
            count++;
            return true;
        }

        // Insert with pre-allocated buffer. Caller transfers ownership.
        bool
        insert(uint32_t node_id, uint8_t* buf) {
            if (is_cached(node_id)) {
                delete[] buf;
                return false;
            }

            while (count >= max_nodes)
                evict_oldest();

            data[node_id] = buf;
            set_bit(node_id);

            auto* e = alloc_entry();
            e->node_id = node_id;
            list_push_front(e);
            entry_index[node_id] = e;
            count++;
            return true;
        }

    private:
        void
        set_bit(uint32_t node_id) {
            uint64_t word = node_id / 64;
            uint64_t bit = node_id % 64;
            bitmap[word].store(
                bitmap[word].load(std::memory_order_relaxed) | (1ULL << bit),
                std::memory_order_release);
        }

        void
        clear_bit(uint32_t node_id) {
            uint64_t word = node_id / 64;
            uint64_t bit = node_id % 64;
            bitmap[word].store(
                bitmap[word].load(std::memory_order_relaxed) & ~(1ULL << bit),
                std::memory_order_release);
        }

        void
        evict_oldest() {
            entry* oldest = sentinel.prev;
            if (oldest == &sentinel) return;

            uint32_t victim = oldest->node_id;

            // Clear bitmap (readers stop seeing this node)
            clear_bit(victim);

            // Free data
            delete[] data[victim];
            data[victim] = nullptr;

            // Remove from FIFO list
            list_remove(oldest);
            entry_index[victim] = nullptr;
            free_entry(oldest);
            count--;
        }

        entry*
        alloc_entry() {
            if (free_list) {
                auto* e = free_list;
                free_list = free_list->next;
                return e;
            }
            return &entry_pool[pool_next++];
        }

        void
        free_entry(entry* e) {
            e->next = free_list;
            free_list = e;
        }

        void
        list_push_front(entry* e) {
            e->next = sentinel.next;
            e->prev = &sentinel;
            sentinel.next->prev = e;
            sentinel.next = e;
        }

        void
        list_remove(entry* e) {
            e->prev->next = e->next;
            e->next->prev = e->prev;
        }
    };

}

#endif //AISAQ_CACHE_NODE_CACHE_HH
