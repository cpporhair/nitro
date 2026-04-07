#ifndef APPS_INCONEL_CORE_REGISTRY_HH
#define APPS_INCONEL_CORE_REGISTRY_HH

#include <cassert>
#include <cstdint>
#include <vector>

#include "pump/core/lock_free_queue.hh"  // for pump::core::this_core_id

#include "../mock_nvme/scheduler.hh"
#include "../tree/scheduler.hh"  // for tree::lookup_scheduler_base

namespace apps::inconel::core::registry {

    // ── Scheduler registry ──
    //
    // Centralized non-templated registry of all scheduler instances. The
    // builder fills this at startup; application code accesses schedulers
    // exclusively through this registry — no scheduler is allowed to invent
    // its own way of finding peers.
    //
    // Layout follows the apps/kv/runtime/scheduler_objects.hh pattern: each
    // scheduler kind owns a list (all instances) and a `by_core` lookup
    // (core_id → instance, may be nullptr for cores that don't host this
    // kind). Singleton schedulers (coord/tree/wal/value, future) become
    // simple inline pointer fields when implemented.
    //
    // The registry stores ONLY non-templated base pointers. The Cache
    // template parameter on lookup_scheduler<Cache> stays inside the builder.

    struct nvme_list {
        std::vector<mock_nvme::scheduler*> list;     // all instances
        std::vector<mock_nvme::scheduler*> by_core;  // index = core_id; nullptr ok
    };
    inline nvme_list nvme_scheds;

    struct tree_lookup_list {
        std::vector<tree::lookup_scheduler_base*> list;
        std::vector<tree::lookup_scheduler_base*> by_core;
    };
    inline tree_lookup_list tree_lookup_scheds;

    // ── Future scheduler slots (placeholder, not implemented yet) ──
    //
    // Singletons:
    //   inline coord::scheduler*       coord_sched       = nullptr;
    //   inline tree::scheduler*        tree_sched        = nullptr;
    //   inline wal::scheduler*         wal_space_sched   = nullptr;
    //   inline value::scheduler*       value_alloc_sched = nullptr;
    //
    // Per-shard:
    //   struct front_list {
    //       std::vector<front::scheduler*> list;
    //       std::vector<front::scheduler*> by_core;
    //   };
    //   inline front_list front_scheds;
    //
    // Adding a new scheduler kind = adding a field here + a constructor block
    // in builder.hh + the type to PUMP runtime's template list. Nothing else
    // changes.

    // ── Initialization helper ──
    //
    // Resize by_core arrays to a given core count. Called by builder before
    // populating individual core slots.

    inline void
    init_capacity(uint32_t max_cores) {
        nvme_scheds.by_core.assign(max_cores, nullptr);
        tree_lookup_scheds.by_core.assign(max_cores, nullptr);
    }

    inline void
    clear() {
        nvme_scheds.list.clear();
        nvme_scheds.by_core.clear();
        tree_lookup_scheds.list.clear();
        tree_lookup_scheds.by_core.clear();
    }

    // ── Per-core fast access (current core) ──
    //
    // Caller must be running on a core that hosts the requested scheduler;
    // configuration validation is the builder's responsibility.

    inline mock_nvme::scheduler*
    local_nvme() {
        auto* s = nvme_scheds.by_core[pump::core::this_core_id];
        assert(s && "current core has no nvme scheduler");
        return s;
    }

    inline tree::lookup_scheduler_base*
    local_tree_lookup() {
        auto* s = tree_lookup_scheds.by_core[pump::core::this_core_id];
        assert(s && "current core has no tree_lookup scheduler");
        return s;
    }

    // ── Per-core access by explicit core_id ──

    inline mock_nvme::scheduler*
    nvme_for_core(uint32_t core) {
        return nvme_scheds.by_core[core];
    }

    inline tree::lookup_scheduler_base*
    tree_lookup_for_core(uint32_t core) {
        return tree_lookup_scheds.by_core[core];
    }

    // ── Cross-shard access by index ──

    inline tree::lookup_scheduler_base*
    tree_lookup_at(uint32_t idx) {
        return tree_lookup_scheds.list[idx % tree_lookup_scheds.list.size()];
    }

    inline uint32_t
    tree_lookup_count() {
        return static_cast<uint32_t>(tree_lookup_scheds.list.size());
    }

    inline uint32_t
    nvme_count() {
        return static_cast<uint32_t>(nvme_scheds.list.size());
    }

    // ── Future routing helpers (uncomment when corresponding scheduler exists) ──
    //
    // inline front::scheduler*
    // route_to_front(uint64_t key_hash) {
    //     return front_scheds.list[key_hash % front_scheds.list.size()];
    // }
    //
    // inline tree::lookup_scheduler_base*
    // home_tree_lookup_for_front(uint32_t front_owner) {
    //     // Stable mapping established at startup; for now a simple modulo.
    //     return tree_lookup_scheds.list[front_owner % tree_lookup_scheds.list.size()];
    // }

}

#endif //APPS_INCONEL_CORE_REGISTRY_HH
