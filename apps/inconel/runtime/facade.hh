#ifndef APPS_INCONEL_RUNTIME_FACADE_HH
#define APPS_INCONEL_RUNTIME_FACADE_HH

// ── runtime/facade.hh ── single-entry handle for application code ──
//
// Business pipelines (write path, read path, flush, recovery) reach
// Inconel's schedulers through this facade only. The underlying
// state lives in `core::registry` — the facade is a thin wrapper
// that:
//
//   - packages together the singleton accessors (`value`, `owner`),
//     the per-core local accessor (`local_nvme`) and the routing
//     accessors (`route_to_read_domain`, `partition_sorted_keys`)
//     behind a shape that reads as a single API surface;
//   - enforces the two-step `publish_shard_partitions` contract so
//     a rebuild installs the global pointer AND refreshes every
//     read_domain snapshot in one call — doing them separately has
//     historically been an easy thing to miss.
//
// All helpers are `inline` because there is no code to link; they
// are either direct forwarders or a handful of ops. The facade
// intentionally does not hold state of its own — the single source
// of truth for every field below is `core::registry`.

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "../core/registry.hh"
#include "../core/shard_partition.hh"
#include "../core/tree_read_domain.hh"
#include "../nvme/runtime_scheduler.hh"
#include "../tree/scheduler.hh"        // tree::tree_sched
#include "../value/scheduler.hh"       // value::value_alloc_sched_base

namespace apps::inconel::rt {

    // ── Singleton accessors ────────────────────────────────────────

    inline value::value_alloc_sched_base*
    value() {
        return core::registry::value_sched();
    }

    inline tree::tree_sched*
    owner() {
        return core::registry::tree_sched_singleton();
    }

    // ── Per-core accessor ──────────────────────────────────────────
    //
    // Must be called from a thread whose `pump::core::this_core_id`
    // matches one of the cores that hosts an nvme scheduler — i.e.
    // every core listed in `build_options::cores`. Asserts non-null
    // to catch misconfigured topologies early.

    inline nvme::runtime_scheduler*
    local_nvme() {
        return core::registry::local_nvme();
    }

    // ── Routing ────────────────────────────────────────────────────
    //
    // `route_to_read_domain(key)` hides the two-step lookup
    // (`current_shard_partitions()->route(key)` →
    // `tree_read_domain_at(shard_idx)`). Caller receives a
    // non-templated `tree_read_domain_base*`; typed callers that
    // need to reach the underlying `tree_read_domain<Cache>` do so
    // after narrowing on the Cache template in their TU.
    //
    // Pre: a shard_partition_map has been published. The bootstrap
    // placeholder installed by `build_runtime` satisfies this from
    // the first core's first advance onwards — reads that race
    // against `build_runtime` should be short-circuited via
    // `manifest->has_root()` before hitting this path.

    inline core::tree_read_domain_base*
    route_to_read_domain(std::string_view key) {
        auto partitions = core::registry::current_shard_partitions();
        assert(partitions && "rt::route_to_read_domain: no shard_partition_map "
                             "installed (build_runtime must run first)");
        const uint32_t shard_idx = partitions->route(key);
        return core::registry::tree_read_domain_at(shard_idx);
    }

    // Batch routing for sorted keys — hot path for flush. Sink is
    // called once per non-empty contiguous run:
    //
    //   sink(uint32_t shard_idx, std::size_t lo, std::size_t hi)
    //
    // where `[lo, hi)` indexes into `sorted_keys`. Same precondition
    // as `route_to_read_domain` — the map must be installed.

    template <typename Sink>
    inline void
    partition_sorted_keys(std::span<const std::string_view> sorted_keys,
                          Sink&& sink) {
        auto partitions = core::registry::current_shard_partitions();
        assert(partitions && "rt::partition_sorted_keys: no shard_partition_map "
                             "installed (build_runtime must run first)");
        partitions->partition_sorted_keys(sorted_keys,
                                          std::forward<Sink>(sink));
    }

    // ── Rebuild / publish ──────────────────────────────────────────
    //
    // Installs a new shard_partition_map and propagates the snapshot
    // into every read_domain's `partitions` member so flush / read
    // decisions made from a read_domain's local snapshot see the same
    // map as decisions made through `current_shard_partitions()`.
    //
    // Two-step ordering is deliberate:
    //   1. install the global pointer first so new readers (including
    //      paths that route through `route_to_read_domain`) pick up
    //      the new map immediately;
    //   2. refresh each read_domain's snapshot so cached per-domain
    //      decisions (future flush routing) also converge.
    //
    // Both steps are fast (single pointer swap + N shared_ptr
    // assignments) and run on the caller's core — `tree_sched` owns
    // this publish site today; other call sites must only call it
    // from a serialized seam.

    inline void
    publish_shard_partitions(
        std::shared_ptr<const core::shard_partition_map> m) {
        core::registry::install_shard_partitions(m);
        for (auto* rd : core::registry::tree_read_domains.list) {
            rd->partitions = m;
        }
    }

}  // namespace apps::inconel::rt

#endif  // APPS_INCONEL_RUNTIME_FACADE_HH
