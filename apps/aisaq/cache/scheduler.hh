#ifndef AISAQ_CACHE_SCHEDULER_HH
#define AISAQ_CACHE_SCHEDULER_HH

#include "node_cache.hh"
#include "pump/core/lock_free_queue.hh"

namespace aisaq::cache {

    // Cache scheduler — serializes all cache mutations (insert, promote, evict).
    //
    // Search pipeline only does:
    //   read path:  bitmap test + pointer deref (zero-cost, no scheduler)
    //   write path:  schedule_insert() — alloc+memcpy at call site, enqueue O(1)
    //   promote:     schedule_promote() — enqueue node_id, O(1)
    //
    // Scheduler advance() processes queues:
    //   promotes:  move entry to LRU head
    //   inserts:   set data pointer + set bitmap + LRU enqueue (+ evict if full)
    //
    // Uses per_core::queue for multi-core readiness (each source core has dedicated SPSC).

    struct
    cache_scheduler {
        global_node_cache* cache;

        struct insert_req {
            uint32_t node_id;
            uint8_t* data;   // heap-allocated, ownership transferred to scheduler
        };

        pump::core::per_core::queue<insert_req> insert_q;
        pump::core::per_core::queue<uint32_t, false> promote_q;  // overflow silently dropped

        explicit
        cache_scheduler(global_node_cache* c)
            : cache(c), insert_q(4096), promote_q(4096) {}

        // Fire-and-forget insert: copies src to heap buffer at call site, enqueues.
        // DMA buffer is safe to reuse immediately after this returns.
        void
        schedule_insert(uint32_t node_id, const void* src, uint32_t len) {
            if (cache->is_cached(node_id)) return;
            auto* buf = new uint8_t[len];
            std::memcpy(buf, src, len);
            insert_q.try_enqueue({node_id, buf});
        }

        // Fire-and-forget LRU promote (called on cache hit).
        // Overflow silently dropped — missed promote only slightly affects LRU accuracy.
        void
        schedule_promote(uint32_t node_id) {
            promote_q.try_enqueue(node_id);
        }

        // Called by runtime main loop (| fold, every iteration)
        bool
        advance() {
            bool did_work = false;

            did_work |= promote_q.drain([this](uint32_t node_id) {
                cache->promote(node_id);
            });

            did_work |= insert_q.drain([this](insert_req&& req) {
                if (cache->is_cached(req.node_id)) {
                    delete[] req.data;
                    return;
                }
                cache->insert(req.node_id, req.data);
            });

            return did_work;
        }
    };

}

#endif //AISAQ_CACHE_SCHEDULER_HH
