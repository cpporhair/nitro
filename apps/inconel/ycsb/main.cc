#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../runtime/start_db.hh"
#include "./config.hh"
#include "./expected_state.hh"
#include "./runner.hh"
#include "./runtime_bridge.hh"
#include "./stats.hh"

namespace apps::inconel::ycsb {

    inline void
    print_usage(std::ostream& out) {
        out
            << "usage: inconel_ycsb [--config FILE] --pci-addr BDF "
               "[--force-format] [options]\n"
            << "\n"
            << "config:\n"
            << "  --config FILE --dry-run --print-config --no-print-config\n"
            << "  --dump-config\n"
            << "\n"
            << "device:\n"
            << "  --pci-addr BDF | --pci BDF\n"
            << "  --force-format --no-force-format\n"
            << "  --spdk-core-mask MASK --qpair-depth N\n"
            << "\n"
            << "workload:\n"
            << "  --workload load|a|b|c|update|delete|load-a|load-b|load-c\n"
            << "  --records N --operations N --value-size BYTES\n"
            << "  --batch-size N --inflight N --seed N --key-prefix PREFIX\n"
            << "  --verify-samples N\n"
            << "  --verify-existing-updates --verify-existing-deletes\n"
            << "  --expect-file FILE --write-expect-file FILE\n"
            << "  --expect-samples N --expect-all\n"
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
            << "  --tree-cache clock|slru --tree-cache-policy clock|slru\n"
            << "  --value-cache clock|slru --value-cache-policy clock|slru\n"
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

    inline int
    run(config cfg) {
        std::shared_ptr<expected_state> expected;
        if (!cfg.expect_file.empty() || !cfg.write_expect_file.empty() ||
            cfg.expect_samples != 0 || cfg.expect_all) {
            if (!cfg.expect_file.empty()) {
                expected = std::make_shared<expected_state>(
                    load_expected_state_file(cfg, cfg.expect_file));
            } else {
                expected = std::make_shared<expected_state>(
                    make_empty_expected_state(cfg));
            }
        }

        auto state = std::make_shared<run_state>();
        state->cfg = std::make_shared<config>(std::move(cfg));
        state->stats = std::make_shared<all_stats>();
        state->expected = std::move(expected);

        auto result = runtime::start_db(make_db_options(*state->cfg))(
            [state]() {
                return run_workload(state);
            });
        print_all_stats(std::cout, *state->stats);
        print_maintenance_stats(std::cout, result.maintenance);
        const uint64_t error_count = total_error_count(*state->stats);
        if (error_count != 0) {
            std::cerr << "inconel_ycsb: completed with error counters="
                      << error_count << '\n';
            return 1;
        }
        if (state->expected && !state->cfg->write_expect_file.empty()) {
            write_expected_state_file(
                *state->cfg, *state->expected, state->cfg->write_expect_file);
        }
        return 0;
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
        if (cfg.print_config) {
            apps::inconel::ycsb::print_effective_config(std::cout, cfg);
        }
        if (cfg.dump_config) {
            apps::inconel::ycsb::dump_effective_config_json(std::cout, cfg);
        }
        if (cfg.dry_run) {
            return 0;
        }
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
