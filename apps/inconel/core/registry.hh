#ifndef APPS_INCONEL_CORE_REGISTRY_HH
#define APPS_INCONEL_CORE_REGISTRY_HH

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "pump/core/lock_free_queue.hh"  // for pump::core::this_core_id

#include "./data_area_heads.hh"
#include "./panic.hh"
#include "./shard_partition.hh"
#include "./tree_manifest.hh"
#include "./wal_reclaim_frontier.hh"
#include "../nvme/runtime_scheduler.hh"
#include "../tree/scheduler.hh"  // for tree::tree_lookup_sched_base
#include "../value/scheduler.hh" // for value::value_alloc_sched_base

// Forward-declare tree_read_domain_base so the registry can store a
// non-templated list of read_domains. The full struct lives in
// `core/tree_read_domain.hh`; only the base pointer is needed here.
namespace apps::inconel::core {
    struct tree_read_domain_base;
}

namespace apps::inconel::coord {
    struct coord_sched;
}

namespace apps::inconel::front {
    class front_sched;
}

namespace apps::inconel::wal {
    class wal_space_sched;
}

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
    // template parameter on tree_lookup_sched<Cache> stays inside the builder.

    struct nvme_list {
        std::vector<nvme::runtime_scheduler*> list;     // all instances
        std::vector<nvme::runtime_scheduler*> by_core;  // index = core_id; nullptr ok
    };
    inline nvme_list nvme_scheds;

    // Step 030 (§2.7 / §6.4 decision F2) replaces the old
    // `tree_lookup_scheds` / `tree_worker_scheds` double-registry
    // with a single `tree_read_domains` list. The individual
    // schedulers are reached through `tree_read_domain_at(idx)->lookup`
    // / `->worker`. This is the canonical registry dimension for
    // tree read access — no double maintenance.
    struct tree_read_domain_list {
        std::vector<tree_read_domain_base*> list;
        std::vector<tree_read_domain_base*> by_core;
    };
    inline tree_read_domain_list tree_read_domains;

    // value::value_alloc_sched is a global singleton — only one instance
    // exists, pinned to a specific core (cores[0] in the v6 builder). All
    // access goes through value_sched(). The registry stores only the
    // non-templated base pointer; the Cache template parameter stays inside
    // the builder.
    inline value::value_alloc_sched_base* value_alloc_sched = nullptr;
    inline std::shared_ptr<data_area_heads> data_area_heads_ptr;
    inline std::shared_ptr<wal_reclaim_frontier> wal_reclaim_frontier_ptr;

    // tree::tree_sched is a global singleton introduced in step 023
    // (Phase 3 G5, D17/D18/D24). It owns the tree-local flush round
    // state machine and is pinned to cores[0] alongside
    // value::value_alloc_sched. All access goes through
    // `tree_sched_singleton()`; no per-core helper is needed because
    // `tree_sched` has exactly one instance.
    //
    // The field is named `tree_sched_singleton_ptr` (not
    // `tree_sched`) so it does not collide with the `tree::`
    // namespace inside this file.
    inline tree::tree_sched* tree_sched_singleton_ptr = nullptr;

    // coord::coord_sched is introduced in M03 as a global singleton
    // scheduler. The runtime builder is still future work; tests and future
    // builders install the pointer explicitly and access it through
    // coord_sched_singleton().
    inline coord::coord_sched* coord_sched_singleton_ptr = nullptr;

    struct front_list {
        std::vector<front::front_sched*> list;     // owner_id -> instance
        std::vector<front::front_sched*> by_core;  // core_id -> instance/null
    };
    inline front_list front_scheds;

    inline wal::wal_space_sched* wal_space_sched_singleton_ptr = nullptr;

    // owner_id -> that front owner's home-core nvme scheduler. The builder
    // fills this once after all per-core nvme schedulers exist; operations
    // borrow it as a span and never rebuild it per request.
    inline std::vector<nvme::runtime_scheduler*> nvme_by_front_owner;

    // Globally installed `shard_partition_map` (step 030 §2.7). A
    // `shared_ptr<const>` lets a future heat-driven rebuild
    // (issue 2 decision B1, out of scope in step 030) swap the map
    // atomically without coordinating with every read_domain.
    // Accessor / installer live further down the file after the
    // `clear()` helper so the initialization order stays grouped
    // with the other global state.
    inline std::shared_ptr<const shard_partition_map>
        current_shard_partitions_ptr;

    // ── Initialization helper ──
    //
    // Resize by_core arrays to a given core count. Called by builder before
    // populating individual core slots.

    inline void
    init_capacity(uint32_t max_cores) {
        nvme_scheds.by_core.assign(max_cores, nullptr);
        tree_read_domains.by_core.assign(max_cores, nullptr);
        front_scheds.by_core.assign(max_cores, nullptr);
    }

    inline void
    clear() {
        nvme_scheds.list.clear();
        nvme_scheds.by_core.clear();
        tree_read_domains.list.clear();
        tree_read_domains.by_core.clear();
        front_scheds.list.clear();
        front_scheds.by_core.clear();
        wal_space_sched_singleton_ptr = nullptr;
        nvme_by_front_owner.clear();
        value_alloc_sched = nullptr;
        data_area_heads_ptr.reset();
        wal_reclaim_frontier_ptr.reset();
        tree_sched_singleton_ptr = nullptr;
        coord_sched_singleton_ptr = nullptr;
        current_shard_partitions_ptr.reset();
    }

    // ── Singleton access ──
    //
    // value_sched() asserts non-null; the builder is responsible for
    // installing the instance at startup. Application code uses this
    // helper rather than reading the variable directly.

    inline value::value_alloc_sched_base*
    value_sched() {
        assert(value_alloc_sched && "value::value_alloc_sched not registered");
        return value_alloc_sched;
    }

    inline data_area_heads*
    data_area_heads_singleton() {
        assert(data_area_heads_ptr && "core::data_area_heads not registered");
        return data_area_heads_ptr.get();
    }

    inline wal_reclaim_frontier*
    wal_reclaim_frontier_singleton() {
        assert(wal_reclaim_frontier_ptr &&
               "core::wal_reclaim_frontier not registered");
        return wal_reclaim_frontier_ptr.get();
    }

    // tree_sched_singleton() follows the same pattern as value_sched()
    // (023 D24). Application code never touches
    // `tree_sched_singleton_ptr` directly.
    inline tree::tree_sched*
    tree_sched_singleton() {
        assert(tree_sched_singleton_ptr && "tree::tree_sched not registered");
        return tree_sched_singleton_ptr;
    }

    inline coord::coord_sched*
    coord_sched_singleton() {
        assert(coord_sched_singleton_ptr &&
               "coord::coord_sched not registered");
        return coord_sched_singleton_ptr;
    }

    inline front::front_sched*
    front_at(uint32_t owner) {
        if (owner >= front_scheds.list.size() ||
            front_scheds.list[owner] == nullptr) {
            panic_inconsistency("core::registry::front_at",
                "front owner %u is not registered",
                static_cast<unsigned>(owner));
        }
        return front_scheds.list[owner];
    }

    inline uint32_t
    front_count() {
        return static_cast<uint32_t>(front_scheds.list.size());
    }

    inline std::span<front::front_sched* const>
    fronts_span() {
        if (front_scheds.list.empty()) {
            panic_inconsistency("core::registry::fronts_span",
                "front scheduler list is empty");
        }
        return {front_scheds.list.data(), front_scheds.list.size()};
    }

    inline wal::wal_space_sched*
    wal_space_singleton() {
        if (wal_space_sched_singleton_ptr == nullptr) {
            panic_inconsistency("core::registry::wal_space_singleton",
                "wal::wal_space_sched not registered");
        }
        return wal_space_sched_singleton_ptr;
    }

    inline std::span<nvme::runtime_scheduler* const>
    nvme_by_front_owner_span() {
        if (nvme_by_front_owner.empty()) {
            panic_inconsistency("core::registry::nvme_by_front_owner_span",
                "nvme_by_front_owner is empty");
        }
        return {nvme_by_front_owner.data(), nvme_by_front_owner.size()};
    }

    // ── Per-core fast access (current core) ──
    //
    // Caller must be running on a core that hosts the requested scheduler;
    // configuration validation is the builder's responsibility.

    inline nvme::runtime_scheduler*
    local_nvme() {
        auto* s = nvme_scheds.by_core[pump::core::this_core_id];
        assert(s && "current core has no nvme scheduler");
        return s;
    }

    inline tree_read_domain_base*
    local_tree_read_domain() {
        auto* s = tree_read_domains.by_core[pump::core::this_core_id];
        assert(s && "current core has no tree_read_domain");
        return s;
    }

    // ── Per-core access by explicit core_id ──

    inline nvme::runtime_scheduler*
    nvme_for_core(uint32_t core) {
        return nvme_scheds.by_core[core];
    }

    inline tree_read_domain_base*
    tree_read_domain_for_core(uint32_t core) {
        return tree_read_domains.by_core[core];
    }

    // ── Cross-shard access by index ──
    //
    // Step 030 (§6.4 decision F2): the only registry dimension for
    // tree read access is `tree_read_domains`. Lookup and worker
    // schedulers are reached through the returned read_domain struct
    // (casting `tree_read_domain_base*` to
    // `tree_read_domain<Cache>*` where the Cache type is known).

    inline tree_read_domain_base*
    tree_read_domain_at(uint32_t idx) {
        return tree_read_domains.list[idx % tree_read_domains.list.size()];
    }

    inline uint32_t
    tree_read_domain_count() {
        return static_cast<uint32_t>(tree_read_domains.list.size());
    }

    inline uint32_t
    nvme_count() {
        return static_cast<uint32_t>(nvme_scheds.list.size());
    }

    // ── Key-range routing for tree read access ────────────────────────
    //
    // Step 030 (§2.7 / §6.4 decision F2) replaces the old
    // `route_tree_lookup_for_key(manifest, key)` wrapper — which
    // computed `leaf_order.find_leaf_for_key(key) % K` — with a
    // single globally installed `shard_partition_map`. The routing
    // decision is therefore:
    //
    //   shard_idx = current_shard_partitions()->route(key)
    //   read_domain = tree_read_domains.list[shard_idx]
    //
    // Both the read path (`tree::lookup`) and the flush fold path
    // (`memtable_fold`) route through the same map so that every
    // key always lands on the same `tree_read_domain` shard —
    // this is the precondition for the "one tree_node cache shard
    // per leaf range" invariant (RSM §4.7).
    //
    // The map is a `shared_ptr<const shard_partition_map>` so the
    // future heat-driven rebuild (issue 2 decision B1, not in step
    // 030 scope) can swap it atomically without coordinating with
    // every read_domain. Step 030 only calls `install_shard_partitions`
    // once from the builder (bootstrap placeholder map); the install
    // path will be called again from `tree_sched` at frontier switch
    // when rebuild lands.
    //
    // `current_shard_partitions()` returns a fresh `shared_ptr` copy;
    // the caller can keep it alive for the duration of a routing
    // decision without racing against a future rebuild.

    inline std::shared_ptr<const shard_partition_map>
    current_shard_partitions() {
        return current_shard_partitions_ptr;
    }

    inline void
    install_shard_partitions(
        std::shared_ptr<const shard_partition_map> m) {
        current_shard_partitions_ptr = std::move(m);
    }

}

#endif //APPS_INCONEL_CORE_REGISTRY_HH
