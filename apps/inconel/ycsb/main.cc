#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pump/core/context.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include "../core/page_cache.hh"
#include "../nvme/real_device.hh"
#include "../recovery/boot.hh"
#include "../runtime/builder.hh"
#include "../runtime/run.hh"
#include "./config.hh"
#include "./format_device.hh"
#include "./runner.hh"
#include "./stats.hh"

namespace apps::inconel::ycsb {

    inline void
    print_usage(std::ostream& out) {
        out
            << "usage: inconel_ycsb --pci-addr BDF [--force-format] [options]\n"
            << "\n"
            << "workload:\n"
            << "  --workload load|a|b|c|update|delete|load-a|load-b|load-c\n"
            << "  --records N --operations N --value-size BYTES\n"
            << "  --batch-size N --inflight N --seed N --verify-samples N\n"
            << "  --verify-existing-updates --verify-existing-deletes\n"
            << "  --flush-after-load --no-flush-after-load\n"
            << "\n"
            << "runtime:\n"
            << "  --cores 0,1,2,3 --main-core 0 --front-cores 0,1\n"
            << "  --value-core N --owner-core N --coord-core N\n"
            << "  --wal-space-core N --maintenance-core N\n"
            << "  --maintenance-seal-active-bytes N\n"
            << "  --maintenance-total-memtable-bytes N\n"
            << "  --maintenance-wal-seal-percent N\n"
            << "  --maintenance-max-sealed-gens-per-front N\n"
            << "  --tree-cache clock|slru --value-cache clock|slru\n"
            << "  --tree-cache-capacity N --value-cache-capacity N\n";
    }

    inline bool
    wants_help(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--help" || arg == "-h") {
                return true;
            }
        }
        return false;
    }

    template <typename Runtime>
    [[nodiscard]] inline runtime::maintenance_stats_snapshot
    collect_maintenance_stats(Runtime* rt) {
        runtime::maintenance_stats_snapshot out{};
        for (auto* sched :
             rt->template get_schedulers<runtime::maintenance_sched>()) {
            if (sched == nullptr) {
                continue;
            }
            const auto snap = sched->snapshot();
            out.enabled = out.enabled || snap.enabled;
            out.stopping = out.stopping || snap.stopping;
            out.inflight = out.inflight || snap.inflight;
            out.cooldown_ticks =
                std::max(out.cooldown_ticks, snap.cooldown_ticks);
            out.idle_backoff_ticks =
                std::max(out.idle_backoff_ticks, snap.idle_backoff_ticks);
            out.launched_rounds += snap.launched_rounds;
            out.completed_rounds += snap.completed_rounds;
            out.failed_rounds += snap.failed_rounds;
            out.work_rounds += snap.work_rounds;
            out.noop_rounds += snap.noop_rounds;
            out.seal_rounds += snap.seal_rounds;
            out.flush_rounds += snap.flush_rounds;
            out.non_noop_flush_rounds += snap.non_noop_flush_rounds;
        }
        return out;
    }

    inline void
    print_maintenance_stats(
        std::ostream& out,
        const runtime::maintenance_stats_snapshot& stats) {
        out << "maintenance"
            << " enabled=" << (stats.enabled ? 1 : 0)
            << " launched=" << stats.launched_rounds
            << " completed=" << stats.completed_rounds
            << " failed=" << stats.failed_rounds
            << " work=" << stats.work_rounds
            << " noop=" << stats.noop_rounds
            << " seal=" << stats.seal_rounds
            << " flush=" << stats.flush_rounds
            << " non_noop_flush=" << stats.non_noop_flush_rounds
            << "\n";
    }

    template <typename Runtime>
    inline void
    submit_app_root(Runtime* rt,
                    std::shared_ptr<run_state> state,
                    std::shared_ptr<const std::vector<uint32_t>> cores) {
        auto ctx = pump::core::make_root_context();
        run_workload(state)
            >> pump::sender::then([rt, state, cores](bool) {
                state->completed.store(true, std::memory_order_release);
                stop_runtime(rt, cores);
                return true;
            })
            >> pump::sender::any_exception(
                [rt, state, cores](std::exception_ptr ep) mutable {
                    state->error = std::move(ep);
                    state->completed.store(true, std::memory_order_release);
                    stop_runtime(rt, cores);
                    return pump::sender::just(false);
                })
            >> pump::sender::submit(ctx);
    }

    template <core::cache_concept TreeCache, core::cache_concept ValueCache>
    int
    run_with_cache_policy(config cfg) {
        nvme::real_device device(nvme::real_device_options{
            .pci_addr = cfg.pci_addr.c_str(),
            .cores = std::span<const uint32_t>(cfg.cores.data(),
                                               cfg.cores.size()),
            .spdk_core_mask =
                cfg.spdk_core_mask.empty() ? nullptr : cfg.spdk_core_mask.c_str(),
            .spdk_name = "inconel_ycsb",
            .init_spdk_env = true,
            .qpair_depth = cfg.qpair_depth,
            .device_id = 0,
        });

        if (cfg.force_format) {
            force_format_device(device, cfg.main_core);
        }
        std::optional<recovery::recovered_boot_state> recovered_boot;
        if (!cfg.force_format) {
            recovered_boot.emplace(
                recovery::recover_empty_clean_boot(device, cfg.main_core));
        }

        auto state = std::make_shared<run_state>();
        state->cfg = std::make_shared<config>(std::move(cfg));
        state->stats = std::make_shared<all_stats>();
        auto cores = std::make_shared<const std::vector<uint32_t>>(
            state->cfg->cores);

        runtime::maintenance_options maintenance{};
        maintenance.core = state->cfg->maintenance_core;
        maintenance.policy.auto_seal_flush = !state->cfg->flush_after_load;
        maintenance.policy.seal_active_memtable_bytes =
            state->cfg->maintenance_seal_active_bytes;
        maintenance.policy.total_memtable_limit_bytes =
            state->cfg->maintenance_total_memtable_bytes;
        maintenance.policy.wal_seal_used_ratio =
            static_cast<float>(state->cfg->maintenance_wal_seal_percent) /
            100.0f;
        maintenance.policy.max_sealed_gens_per_front =
            state->cfg->maintenance_max_sealed_gens_per_front;

        runtime::build_options bopts{
            .cores = std::span<const uint32_t>(
                state->cfg->cores.data(), state->cfg->cores.size()),
            .device = &device,
            .disk_profile =
                recovered_boot.has_value() ? &recovered_boot->profile : nullptr,
            .recovered_state =
                recovered_boot.has_value()
                    ? &recovered_boot->runtime_state
                    : nullptr,
            .read_domain_cores = {},
            .value_core = state->cfg->value_core,
            .owner_core = state->cfg->owner_core,
            .front_cores = std::span<const uint32_t>(
                state->cfg->front_cores.data(),
                state->cfg->front_cores.size()),
            .coord_core = state->cfg->coord_core,
            .wal_space_core = state->cfg->wal_space_core,
            .tree_cache_capacity = state->cfg->tree_cache_capacity,
            .value_cache_capacity = state->cfg->value_cache_capacity,
            .maintenance = maintenance,
        };

        runtime::inconel_runtime_t<TreeCache, ValueCache>* rt = nullptr;
        runtime::maintenance_stats_snapshot maintenance_stats{};
        try {
            rt = runtime::build_runtime<TreeCache, ValueCache>(bopts);
            rt::start(
                rt,
                std::span<const uint32_t>(
                    state->cfg->cores.data(), state->cfg->cores.size()),
                state->cfg->main_core,
                [state, cores](auto* runtime, uint32_t core) {
                    if (core == state->cfg->main_core) {
                        submit_app_root(runtime, state, cores);
                    }
                });
            maintenance_stats = collect_maintenance_stats(rt);
            runtime::destroy_runtime<TreeCache, ValueCache>(rt);
            rt = nullptr;
        } catch (...) {
            if (rt != nullptr) {
                runtime::destroy_runtime<TreeCache, ValueCache>(rt);
            }
            throw;
        }

        if (state->error) {
            std::rethrow_exception(state->error);
        }
        print_all_stats(std::cout, *state->stats);
        print_maintenance_stats(std::cout, maintenance_stats);
        const uint64_t error_count = total_error_count(*state->stats);
        if (error_count != 0) {
            std::cerr << "inconel_ycsb: completed with error counters="
                      << error_count << '\n';
            return 1;
        }
        return 0;
    }

    template <core::cache_concept TreeCache>
    int
    run_with_tree_policy(config cfg) {
        if (cfg.value_cache_policy == "clock") {
            return run_with_cache_policy<TreeCache, core::segmented_clock_cache>(
                std::move(cfg));
        }
        if (cfg.value_cache_policy == "slru") {
            return run_with_cache_policy<TreeCache, core::segmented_slru_cache>(
                std::move(cfg));
        }
        throw std::invalid_argument("unknown value cache policy");
    }

    inline int
    run(config cfg) {
        if (cfg.tree_cache_policy == "clock") {
            return run_with_tree_policy<core::segmented_clock_cache>(
                std::move(cfg));
        }
        if (cfg.tree_cache_policy == "slru") {
            return run_with_tree_policy<core::segmented_slru_cache>(
                std::move(cfg));
        }
        throw std::invalid_argument("unknown tree cache policy");
    }

}  // namespace apps::inconel::ycsb

int
main(int argc, char** argv) {
    try {
        if (apps::inconel::ycsb::wants_help(argc, argv)) {
            apps::inconel::ycsb::print_usage(std::cout);
            return 0;
        }
        auto cfg = apps::inconel::ycsb::parse_config(argc, argv);
        return apps::inconel::ycsb::run(std::move(cfg));
    } catch (const std::invalid_argument& e) {
        std::cerr << "inconel_ycsb: " << e.what() << "\n\n";
        apps::inconel::ycsb::print_usage(std::cerr);
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "inconel_ycsb: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "inconel_ycsb: unknown error\n";
        return 1;
    }
}
