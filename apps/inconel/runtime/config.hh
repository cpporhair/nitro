#ifndef APPS_INCONEL_RUNTIME_CONFIG_HH
#define APPS_INCONEL_RUNTIME_CONFIG_HH

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "spdk/env.h"

#include "../value/scheduler.hh"
#include "../wal/scheduler.hh"
#include "./maintenance_scheduler.hh"

namespace apps::inconel::runtime {

    enum class db_boot_mode : uint8_t {
        recover,
        force_format,
    };

    struct db_device_options {
        std::string pci_addr;
        std::string spdk_core_mask;
        std::string spdk_name = "inconel";
        bool init_spdk_env = true;
        uint32_t qpair_depth = 128;
        uint16_t device_id = 0;
    };

    struct db_topology_options {
        std::vector<uint32_t> cores;
        std::vector<uint32_t> front_cores;
        std::vector<uint32_t> read_domain_cores;
        uint32_t main_core = 0;
        int32_t value_core = -1;
        int32_t owner_core = -1;
        int32_t coord_core = -1;
        int32_t wal_space_core = -1;
    };

    struct db_cache_options {
        std::string tree_policy = "clock";
        std::string value_policy = "clock";
        uint32_t tree_capacity = 32;
        uint32_t value_capacity = 32;
    };

    struct db_runtime_tuning {
        std::size_t front_queue_depth = 1024;
        std::size_t coord_queue_depth = 1024;
        std::size_t coord_ready_window = 65536;
        wal::wal_append_config front_wal_config =
            { .pending_prepare_capacity = 64 };

        std::size_t tree_queue_depth = 2048;
        std::size_t value_queue_depth = 2048;
        value::value_io_policy value_io = {};

        std::size_t nvme_queue_depth = 2048;
        std::size_t nvme_local_depth = 128;
        uint64_t nvme_dma_pool_pages_per_core = 4096;
        uint32_t nvme_dma_alignment = 4096;
        int nvme_numa_id = SPDK_ENV_NUMA_ID_ANY;
    };

    struct db_options {
        db_device_options device;
        db_boot_mode boot_mode = db_boot_mode::recover;
        db_topology_options topology;
        db_cache_options cache;
        maintenance_options maintenance = {};
        db_runtime_tuning tuning = {};
    };

    struct db_run_result {
        maintenance_stats_snapshot maintenance;
    };

    [[nodiscard]] inline bool
    db_options_contains_core(const std::vector<uint32_t>& cores,
                             uint32_t core) noexcept {
        for (uint32_t c : cores) {
            if (c == core) return true;
        }
        return false;
    }

    [[nodiscard]] inline bool
    db_options_has_duplicate_core(const std::vector<uint32_t>& cores) noexcept {
        for (std::size_t i = 0; i < cores.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (cores[i] == cores[j]) return true;
            }
        }
        return false;
    }

    inline void
    validate_db_options(const db_options& opts) {
        if (opts.device.pci_addr.empty()) {
            throw std::invalid_argument(
                "runtime::start_db: device.pci_addr is empty");
        }
        if (opts.device.qpair_depth == 0) {
            throw std::invalid_argument(
                "runtime::start_db: device.qpair_depth is 0");
        }
        if (opts.topology.cores.empty()) {
            throw std::invalid_argument(
                "runtime::start_db: topology.cores is empty");
        }
        if (db_options_has_duplicate_core(opts.topology.cores)) {
            throw std::invalid_argument(
                "runtime::start_db: topology.cores contains a duplicate");
        }
        if (!db_options_contains_core(
                opts.topology.cores, opts.topology.main_core)) {
            throw std::invalid_argument(
                "runtime::start_db: topology.main_core is not a member "
                "of topology.cores");
        }
        const uint32_t max_cores = std::thread::hardware_concurrency();
        if (max_cores != 0) {
            for (uint32_t core : opts.topology.cores) {
                if (core >= max_cores) {
                    throw std::invalid_argument(
                        "runtime::start_db: topology.cores contains a core "
                        "outside hardware_concurrency");
                }
            }
        }
        if (opts.cache.tree_policy != "clock" &&
            opts.cache.tree_policy != "slru") {
            throw std::invalid_argument(
                "runtime::start_db: cache.tree_policy must be clock or slru");
        }
        if (opts.cache.value_policy != "clock" &&
            opts.cache.value_policy != "slru") {
            throw std::invalid_argument(
                "runtime::start_db: cache.value_policy must be clock or slru");
        }
        if (opts.cache.tree_capacity == 0) {
            throw std::invalid_argument(
                "runtime::start_db: cache.tree_capacity is 0");
        }
        if (opts.cache.value_capacity == 0) {
            throw std::invalid_argument(
                "runtime::start_db: cache.value_capacity is 0");
        }
    }

}  // namespace apps::inconel::runtime

#endif  // APPS_INCONEL_RUNTIME_CONFIG_HH
