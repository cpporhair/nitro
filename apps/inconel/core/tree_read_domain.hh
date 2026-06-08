#ifndef APPS_INCONEL_CORE_TREE_READ_DOMAIN_HH
#define APPS_INCONEL_CORE_TREE_READ_DOMAIN_HH

// ── tree_read_domain.hh ── per-shard tree read runtime object (step 030) ──
//
// Step 022 D5 deliberately did NOT physicalize `tree_read_domain`: at
// that time the "pairing seam" between a lookup shard and its worker
// shard was carried entirely by `registry::by_core` plus a matching
// `read_domain_index` on each scheduler. Step 030 lifts the object
// because three independent forces all now pull in the same direction:
//
//   (a) INC-040 routing requires a non-leaf-bound carrier that sits
//       above the scheduler layer and is owned by neither the
//       lookup nor the worker scheduler;
//   (b) the `tree_node` `readonly_frame_cache` semantically belongs
//       to the read_domain (RSM §4.7 / RMC §6.1), not to
//       `tree_lookup_sched<Cache>` which physically held it until
//       step 030;
//   (c) PUMP tuple entries drop from two per read_domain (lookup +
//       worker) to one (the read_domain itself), giving a single
//       scheduling unit per core and an obvious owner for the
//       shared cache.
//
// Ownership model (030 §2.3 / §6.5 decision G1):
//
//   runtime (PUMP tuple)
//     └── unique_ptr<tree_read_domain<Cache>>          (K instances, per-core)
//           ├── shared_ptr<const shard_partition_map>  (shared across K)
//           ├── Cache node_cache                       (per-domain shard)
//           ├── unique_ptr<tree_lookup_sched<Cache>>   lookup
//           │     └── tree_read_domain<Cache>*         (back-ref, raw)
//           └── unique_ptr<tree_worker_sched<Cache>>   worker
//                 └── tree_read_domain<Cache>*         (back-ref, raw)
//
// Both schedulers are template-specialized on the SAME `Cache` type,
// so when they reach into `read_domain_->node_cache.pin(...)` the call
// site is inlined — zero virtual dispatch, matching the project's
// "极速 KV 引擎" performance target (030 §6.1 decision A).
//
// Registry stores `tree_read_domain_base*` (non-templated base) so
// the per-shard list stays Cache-agnostic, matching the existing
// `value::value_alloc_sched_base` / `tree::tree_lookup_sched_base`
// registry pattern.
//
// Step 2 skeleton (030 §7 step 2):
//   - forward-declare the two scheduler templates;
//   - declare ctor / dtor / advance() with no inline body (pure
//     declarations). The definitions sit at the bottom of the
//     header and require the full scheduler types to be visible
//     — that part is filled by step 4 once step 3 has refactored
//     the schedulers to hold `tree_read_domain<Cache>*`.
//
// Include order contract:
//   - Scheduler headers forward-declare `tree_read_domain` /
//     `tree_read_domain_base` but do NOT #include this header, so
//     there is no cycle.
//   - This header's out-of-class template definitions pull in the
//     scheduler headers at the bottom (added in step 4). Any TU
//     that #includes this header therefore sees both types.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "./page_cache.hh"        // cache_concept
#include "./shard_partition.hh"
#include "./tree_geometry.hh"
#include "../memory/dma_page_pool.hh"

// Forward-declare the schedulers the read_domain owns. The full
// definitions live in `tree/lookup_scheduler.hh` /
// `tree/worker_scheduler.hh` and are pulled in at the bottom of this
// header once the out-of-class implementations need them.
namespace apps::inconel::tree {

    struct tree_lookup_sched_base;
    struct tree_worker_sched_base;

    template <::apps::inconel::core::cache_concept Cache>
    struct tree_lookup_sched;

    template <::apps::inconel::core::cache_concept Cache>
    struct tree_worker_sched;

}  // namespace apps::inconel::tree

namespace apps::inconel::core {

    // ── tree_read_domain_base ──────────────────────────────────────
    //
    // Non-templated base. Registry stores `tree_read_domain_base*`;
    // application-layer code accesses the templated derived struct
    // only through the builder, test fixtures, or via the umbrella
    // scheduler facade (`tree/scheduler.hh`).
    //
    // `advance()` is virtual — it sits on the per-core outer loop,
    // is called once per round, and never appears on a hot-read
    // path. The virtual dispatch cost is amortized across every
    // lookup / worker operation processed in the round.

    struct tree_read_domain_base {
        uint32_t read_domain_index;

        // `partitions` lives on the base so the publish path (see
        // `runtime/facade.hh::publish_shard_partitions`) can refresh
        // every read_domain's snapshot without knowing the `Cache`
        // specialization. It is an immutable `shared_ptr<const ...>`
        // by construction — swapping the pointer atomically is the
        // only way to change what a read_domain routes against.
        std::shared_ptr<const shard_partition_map> partitions;

        // Non-templated base pointers to the owned schedulers.
        // `tree_read_domain<Cache>::ctor` fills these in once the
        // unique_ptr<tree_*_sched<Cache>> members are constructed.
        // Storing them here lets non-templated callers (the public
        // `tree::lookup` fan-out in `tree/sender.hh`, the
        // `submit_flush_work` wrapper on the flush path, the
        // registry's per-shard lookup) reach the schedulers without
        // knowing `Cache` — zero virtual dispatch on the routing
        // path (030 §6.1 performance target). Application code with
        // a typed `tree_read_domain<Cache>*` handle still goes
        // through `lookup.get()` / `worker.get()` directly.
        tree::tree_lookup_sched_base* lookup_sched = nullptr;
        tree::tree_worker_sched_base* worker_sched = nullptr;

        tree_read_domain_base(uint32_t rdi,
                              std::shared_ptr<const shard_partition_map> parts)
            : read_domain_index(rdi), partitions(std::move(parts)) {}

        virtual bool advance() = 0;
        virtual ~tree_read_domain_base() = default;

        // Non-copyable, non-movable — instances are owned via
        // unique_ptr and referenced by raw pointer from schedulers.
        tree_read_domain_base(const tree_read_domain_base&)            = delete;
        tree_read_domain_base& operator=(const tree_read_domain_base&) = delete;
        tree_read_domain_base(tree_read_domain_base&&)                 = delete;
        tree_read_domain_base& operator=(tree_read_domain_base&&)      = delete;
    };

    // ── tree_read_domain<Cache> ────────────────────────────────────
    //
    // Physical read_domain. Owns the routing snapshot, the shared
    // `tree_node` page cache, and the two schedulers that access
    // that cache. Both schedulers are template-specialized on the
    // same `Cache`, so `read_domain_->node_cache.*` calls inline
    // at every hot-path site.
    //
    // `read_domain_index` (inherited from `tree_read_domain_base`)
    // is the numeric identifier used by registry lookup and by
    // `shard_partition.shard_idx`. It is NOT used to pair lookup
    // with worker — the pairing is now physical (both schedulers
    // hold back-refs to this struct).

    template <cache_concept Cache>
    struct tree_read_domain : tree_read_domain_base {
        Cache                                      node_cache;
        std::unique_ptr<tree::tree_lookup_sched<Cache>> lookup;
        std::unique_ptr<tree::tree_worker_sched<Cache>> worker;

        tree_read_domain(uint32_t                                    rdi,
                         std::shared_ptr<const shard_partition_map>  parts,
                         Cache                                       cache,
                         const tree_geometry*                        geom,
                         std::size_t                                 queue_depth = 2048,
                         memory::dma_page_allocator                  frame_allocator =
                             memory::make_heap_dma_page_allocator(),
                         uint32_t                                    frame_alignment = 4096,
                         int                                         frame_numa_id = -1);

        ~tree_read_domain() override;

        bool advance() override;

        // Runtime tuple driver. The PUMP share-nothing runner calls
        // `sched->advance(runtime)` on every registered object per
        // round; delegate to the no-arg form above.
        template <typename Runtime>
        bool advance(Runtime&) { return advance(); }
    };

}  // namespace apps::inconel::core

// ─────────────────────────────────────────────────────────────────
// Step 4 implementations (030 §7 step 4).
//
// The scheduler headers forward-declare `tree_read_domain` /
// `tree_read_domain_base` but do not `#include` this file, so pulling
// them in here does not create a cycle. Any TU that includes
// `core/tree_read_domain.hh` therefore gets both scheduler templates
// fully defined along with the out-of-class member definitions below.
// ─────────────────────────────────────────────────────────────────

#include "../tree/lookup_scheduler.hh"
#include "../tree/worker_scheduler.hh"

namespace apps::inconel::core {

    template <cache_concept Cache>
    tree_read_domain<Cache>::tree_read_domain(
        uint32_t                                    rdi,
        std::shared_ptr<const shard_partition_map>  parts,
        Cache                                       cache,
        const tree_geometry*                        geom,
        std::size_t                                 queue_depth,
        memory::dma_page_allocator                  frame_allocator,
        uint32_t                                    frame_alignment,
        int                                         frame_numa_id)
        : tree_read_domain_base(rdi, std::move(parts))
        , node_cache(std::move(cache))
        , lookup(std::make_unique<tree::tree_lookup_sched<Cache>>(
              this, geom, queue_depth, frame_allocator, frame_alignment,
              frame_numa_id))
        , worker(std::make_unique<tree::tree_worker_sched<Cache>>(
              this, geom, queue_depth, frame_allocator, frame_alignment,
              frame_numa_id)) {
        // Publish base pointers after unique_ptrs are constructed.
        // Non-templated callers see the schedulers through the base
        // class; typed callers still reach them via `lookup.get()`
        // and `worker.get()`.
        tree_read_domain_base::lookup_sched = lookup.get();
        tree_read_domain_base::worker_sched = worker.get();
    }

    template <cache_concept Cache>
    tree_read_domain<Cache>::~tree_read_domain() = default;

    template <cache_concept Cache>
    bool
    tree_read_domain<Cache>::advance() {
        // Drive both schedulers in one round. Either can make
        // progress independently — OR the flags so callers see
        // "progress happened" when at least one arm advanced.
        const bool lookup_progress = lookup->advance();
        const bool worker_progress = worker->advance();
        return lookup_progress || worker_progress;
    }

    template <>
    struct tree_read_domain<clock_cache> : tree_read_domain<segmented_clock_cache> {
        using base = tree_read_domain<segmented_clock_cache>;

        tree_read_domain(uint32_t                                   rdi,
                         std::shared_ptr<const shard_partition_map> parts,
                         clock_cache                                cache,
                         const tree_geometry*                       geom,
                         std::size_t                                queue_depth = 2048,
                         memory::dma_page_allocator                 frame_allocator =
                             memory::make_heap_dma_page_allocator(),
                         uint32_t                                   frame_alignment = 4096,
                         int                                        frame_numa_id = -1)
            : base(rdi,
                   std::move(parts),
                   segmented_clock_cache(cache.capacity()),
                   geom,
                   queue_depth,
                   std::move(frame_allocator),
                   frame_alignment,
                   frame_numa_id) {}
    };

    template <>
    struct tree_read_domain<slru_cache> : tree_read_domain<segmented_slru_cache> {
        using base = tree_read_domain<segmented_slru_cache>;

        tree_read_domain(uint32_t                                   rdi,
                         std::shared_ptr<const shard_partition_map> parts,
                         slru_cache                                 cache,
                         const tree_geometry*                       geom,
                         std::size_t                                queue_depth = 2048,
                         memory::dma_page_allocator                 frame_allocator =
                             memory::make_heap_dma_page_allocator(),
                         uint32_t                                   frame_alignment = 4096,
                         int                                        frame_numa_id = -1)
            : base(rdi,
                   std::move(parts),
                   segmented_slru_cache(cache.capacity()),
                   geom,
                   queue_depth,
                   std::move(frame_allocator),
                   frame_alignment,
                   frame_numa_id) {}
    };

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_TREE_READ_DOMAIN_HH
