#ifndef APPS_INCONEL_RUNTIME_SESSION_HH
#define APPS_INCONEL_RUNTIME_SESSION_HH

#include <atomic>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

#include "../nvme/real_device.hh"
#include "../recovery/boot.hh"
#include "./builder.hh"
#include "./config.hh"
#include "./format_device.hh"
#include "./stats.hh"

namespace apps::inconel::runtime {

    struct db_session_view {
        void* runtime = nullptr;
        std::span<const uint32_t> cores;
        std::atomic<bool>* stop_requested = nullptr;
        void (*stop_fn)(const db_session_view&) = nullptr;

        void stop() const {
            if (runtime == nullptr || stop_fn == nullptr) {
                throw std::logic_error("runtime::db_session_view is empty");
            }
            stop_fn(*this);
        }

        [[nodiscard]] bool stop_was_requested() const noexcept {
            return stop_requested != nullptr &&
                   stop_requested->load(std::memory_order_acquire);
        }
    };

    template <typename Runtime>
    [[nodiscard]] inline db_session_view
    make_db_session_view(Runtime* rt,
                         std::span<const uint32_t> cores,
                         std::atomic<bool>* stop_requested) noexcept {
        return db_session_view{
            .runtime = rt,
            .cores = cores,
            .stop_requested = stop_requested,
            .stop_fn = +[](const db_session_view& view) {
                if (view.stop_requested != nullptr) {
                    view.stop_requested->store(
                        true, std::memory_order_release);
                }
                auto* runtime = static_cast<Runtime*>(view.runtime);
                request_maintenance_disable(runtime);
                for (uint32_t core : view.cores) {
                    runtime->is_running_by_core[core].store(
                        false, std::memory_order_release);
                }
            },
        };
    }

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    struct db_session {
        using runtime_type = inconel_runtime_t<TreeCache, ValueCache>;

        db_options opts;
        std::optional<nvme::real_device> device;
        std::optional<recovery::recovered_boot_state> recovered_boot;
        runtime_type* rt = nullptr;
        maintenance_stats_snapshot maintenance_stats{};
        std::exception_ptr app_error;
        std::atomic<bool> stop_requested{false};

        explicit db_session(db_options options)
            : opts(std::move(options)) {}

        void open_device() {
            device.emplace(nvme::real_device_options{
                .pci_addr = opts.device.pci_addr.c_str(),
                .cores = std::span<const uint32_t>(
                    opts.topology.cores.data(), opts.topology.cores.size()),
                .spdk_core_mask = opts.device.spdk_core_mask.empty()
                    ? nullptr
                    : opts.device.spdk_core_mask.c_str(),
                .spdk_name = opts.device.spdk_name.c_str(),
                .init_spdk_env = opts.device.init_spdk_env,
                .qpair_depth = opts.device.qpair_depth,
                .device_id = opts.device.device_id,
            });
        }

        void format_or_recover() {
            if (opts.boot_mode == db_boot_mode::force_format) {
                auto formatted =
                    force_format_device(*device, opts.topology.main_core);
                recovered_boot.emplace(recovery::recovered_boot_state{
                    .profile = formatted.profile,
                    .tree_geometry =
                        recovery::tree_geometry_from_profile(
                            formatted.profile),
                    .superblock_source = formatted.active_superblock_source,
                    .superblock_generation = formatted.superblock_generation,
                    .runtime_state = std::move(formatted.clean_runtime),
                });
                return;
            }
            recovered_boot.emplace(
                recovery::recover_empty_clean_boot(
                    *device, opts.topology.main_core));
        }

        [[nodiscard]] build_options make_build_options() {
            return build_options{
                .cores = std::span<const uint32_t>(
                    opts.topology.cores.data(), opts.topology.cores.size()),
                .device = &*device,
                .disk_profile = recovered_boot.has_value()
                    ? &recovered_boot->profile
                    : nullptr,
                .recovered_state = recovered_boot.has_value()
                    ? &recovered_boot->runtime_state
                    : nullptr,
                .read_domain_cores = std::span<const uint32_t>(
                    opts.topology.read_domain_cores.data(),
                    opts.topology.read_domain_cores.size()),
                .value_core = opts.topology.value_core,
                .owner_core = opts.topology.owner_core,
                .front_cores = std::span<const uint32_t>(
                    opts.topology.front_cores.data(),
                    opts.topology.front_cores.size()),
                .coord_core = opts.topology.coord_core,
                .wal_space_core = opts.topology.wal_space_core,
                .front_queue_depth = opts.tuning.front_queue_depth,
                .coord_queue_depth = opts.tuning.coord_queue_depth,
                .coord_ready_window = opts.tuning.coord_ready_window,
                .front_wal_config = opts.tuning.front_wal_config,
                .tree_cache_capacity = opts.cache.tree_capacity,
                .value_cache_capacity = opts.cache.value_capacity,
                .tree_queue_depth = opts.tuning.tree_queue_depth,
                .value_queue_depth = opts.tuning.value_queue_depth,
                .value_io_policy = opts.tuning.value_io,
                .maintenance = opts.maintenance,
                .nvme_queue_depth = opts.tuning.nvme_queue_depth,
                .nvme_local_depth = opts.tuning.nvme_local_depth,
                .nvme_dma_pool_pages_per_core =
                    opts.tuning.nvme_dma_pool_pages_per_core,
                .nvme_dma_alignment = opts.tuning.nvme_dma_alignment,
                .nvme_numa_id = opts.tuning.nvme_numa_id,
            };
        }

        void build_runtime_instance() {
            auto bopts = make_build_options();
            rt = build_runtime<TreeCache, ValueCache>(bopts);
        }

        void collect_stats() {
            if (rt != nullptr) {
                maintenance_stats = collect_maintenance_stats(rt);
            }
        }

        void destroy_runtime_instance() noexcept {
            if (rt != nullptr) {
                destroy_runtime<TreeCache, ValueCache>(rt);
                rt = nullptr;
            }
        }

        [[nodiscard]] db_run_result result() const noexcept {
            return db_run_result{.maintenance = maintenance_stats};
        }
    };

}  // namespace apps::inconel::runtime

#endif  // APPS_INCONEL_RUNTIME_SESSION_HH
