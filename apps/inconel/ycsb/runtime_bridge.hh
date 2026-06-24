#ifndef APPS_INCONEL_YCSB_RUNTIME_BRIDGE_HH
#define APPS_INCONEL_YCSB_RUNTIME_BRIDGE_HH

#include <string>

#include "../runtime/config.hh"
#include "./config.hh"

namespace apps::inconel::ycsb {

    [[nodiscard]] inline runtime::db_options
    make_db_options(const config& cfg) {
        runtime::db_options out{};
        out.device.pci_addr = cfg.pci_addr;
        out.device.spdk_core_mask = cfg.spdk_core_mask;
        out.device.spdk_name = "inconel_ycsb";
        out.device.qpair_depth = cfg.qpair_depth;
        out.boot_mode = cfg.force_format
            ? runtime::db_boot_mode::force_format
            : runtime::db_boot_mode::recover;

        out.topology.cores = cfg.cores;
        out.topology.front_cores = cfg.front_cores;
        out.topology.main_core = cfg.main_core;
        out.topology.value_core = cfg.value_core;
        out.topology.owner_core = cfg.owner_core;
        out.topology.coord_core = cfg.coord_core;
        out.topology.wal_space_core = cfg.wal_space_core;

        out.cache.tree_policy = cfg.tree_cache_policy;
        out.cache.value_policy = cfg.value_cache_policy;
        out.cache.tree_capacity = cfg.tree_cache_capacity;
        out.cache.value_capacity = cfg.value_cache_capacity;

        out.maintenance.core = cfg.maintenance_core;
        out.maintenance.policy.auto_seal_flush = !cfg.flush_after_load;
        out.maintenance.policy.seal_active_memtable_bytes =
            cfg.maintenance_seal_active_bytes;
        out.maintenance.policy.total_memtable_limit_bytes =
            cfg.maintenance_total_memtable_bytes;
        out.maintenance.policy.wal_seal_used_ratio =
            static_cast<float>(cfg.maintenance_wal_seal_percent) / 100.0f;
        out.maintenance.policy.max_sealed_gens_per_front =
            cfg.maintenance_max_sealed_gens_per_front;
        return out;
    }

}  // namespace apps::inconel::ycsb

#endif  // APPS_INCONEL_YCSB_RUNTIME_BRIDGE_HH
