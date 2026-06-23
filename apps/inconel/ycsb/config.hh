#ifndef APPS_INCONEL_YCSB_CONFIG_HH
#define APPS_INCONEL_YCSB_CONFIG_HH

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../format/format_profile.hh"
#include "../format/value_object.hh"

namespace apps::inconel::ycsb {

    enum class workload_kind : uint8_t {
        load,
        a,
        b,
        c,
        update,
        del,
        load_a,
        load_b,
        load_c,
    };

    struct config {
        std::string pci_addr;
        bool force_format = false;

        workload_kind workload = workload_kind::load_c;
        uint64_t records = 10000;
        uint64_t operations = 10000;
        uint32_t value_size = 256;
        uint32_t batch_size = 1;
        uint32_t inflight = 64;
        uint64_t seed = 1;
        std::string key_prefix = "user";
        bool flush_after_load = false;
        uint64_t verify_samples = 0;
        bool verify_existing_updates = false;
        bool verify_existing_deletes = false;

        std::vector<uint32_t> cores = {0, 1, 2, 3};
        std::vector<uint32_t> front_cores;
        uint32_t main_core = 0;
        int32_t value_core = -1;
        int32_t owner_core = -1;
        int32_t coord_core = -1;
        int32_t wal_space_core = -1;
        int32_t maintenance_core = -1;
        uint64_t maintenance_seal_active_bytes = 256ull * 1024ull * 1024ull;
        uint64_t maintenance_total_memtable_bytes = 1024ull * 1024ull * 1024ull;
        uint32_t maintenance_wal_seal_percent = 70;
        uint32_t maintenance_max_sealed_gens_per_front = 4;
        std::string spdk_core_mask;
        uint32_t qpair_depth = 128;

        std::string tree_cache_policy = "clock";
        std::string value_cache_policy = "clock";
        uint32_t tree_cache_capacity = 1024;
        uint32_t value_cache_capacity = 4096;

        [[nodiscard]] bool
        includes_load() const noexcept {
            return workload == workload_kind::load ||
                   workload == workload_kind::load_a ||
                   workload == workload_kind::load_b ||
                   workload == workload_kind::load_c;
        }

        [[nodiscard]] bool
        includes_run() const noexcept {
            return workload == workload_kind::a ||
                   workload == workload_kind::b ||
                   workload == workload_kind::c ||
                   workload == workload_kind::update ||
                   workload == workload_kind::del ||
                   workload == workload_kind::load_a ||
                   workload == workload_kind::load_b ||
                   workload == workload_kind::load_c;
        }

        [[nodiscard]] workload_kind
        run_kind() const noexcept {
            switch (workload) {
            case workload_kind::a:
            case workload_kind::load_a:
                return workload_kind::a;
            case workload_kind::b:
            case workload_kind::load_b:
                return workload_kind::b;
            case workload_kind::c:
            case workload_kind::load_c:
                return workload_kind::c;
            case workload_kind::update:
                return workload_kind::update;
            case workload_kind::del:
                return workload_kind::del;
            case workload_kind::load:
                return workload_kind::load;
            }
            return workload_kind::load;
        }

        [[nodiscard]] uint64_t
        load_records() const noexcept {
            return includes_load() ? records : 0;
        }

        [[nodiscard]] uint64_t
        run_operations() const noexcept {
            return includes_run() ? operations : 0;
        }
    };

    inline bool
    contains_core(std::span<const uint32_t> cores, uint32_t core) noexcept {
        for (uint32_t c : cores) {
            if (c == core) return true;
        }
        return false;
    }

    inline workload_kind
    parse_workload(std::string_view v) {
        if (v == "load") return workload_kind::load;
        if (v == "a") return workload_kind::a;
        if (v == "b") return workload_kind::b;
        if (v == "c") return workload_kind::c;
        if (v == "update") return workload_kind::update;
        if (v == "delete") return workload_kind::del;
        if (v == "load-a") return workload_kind::load_a;
        if (v == "load-b") return workload_kind::load_b;
        if (v == "load-c") return workload_kind::load_c;
        throw std::invalid_argument("unknown --workload");
    }

    template <typename T>
    inline T
    parse_integer(std::string_view value, const char* what) {
        T out{};
        const char* first = value.data();
        const char* last = value.data() + value.size();
        auto [ptr, ec] = std::from_chars(first, last, out);
        if (ec != std::errc{} || ptr != last) {
            throw std::invalid_argument(what);
        }
        return out;
    }

    inline std::vector<uint32_t>
    parse_core_csv(std::string_view text, const char* what) {
        if (text.empty()) {
            throw std::invalid_argument(what);
        }
        std::vector<uint32_t> out;
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t comma = text.find(',', start);
            const std::size_t end =
                comma == std::string_view::npos ? text.size() : comma;
            if (end == start) {
                throw std::invalid_argument(what);
            }
            out.push_back(parse_integer<uint32_t>(
                text.substr(start, end - start), what));
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
        for (std::size_t i = 0; i < out.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (out[i] == out[j]) {
                    throw std::invalid_argument(what);
                }
            }
        }
        return out;
    }

    class arg_reader {
      public:
        arg_reader(int argc, char** argv)
            : argc_(argc), argv_(argv) {}

        [[nodiscard]] bool
        done() const noexcept {
            return index_ >= argc_;
        }

        [[nodiscard]] std::string_view
        peek() const {
            return argv_[index_];
        }

        std::string_view
        take() {
            if (done()) {
                throw std::invalid_argument("missing argument");
            }
            return argv_[index_++];
        }

        std::string_view
        take_value(std::string_view name) {
            std::string_view arg = take();
            if (arg.starts_with(name) && arg.size() > name.size() &&
                arg[name.size()] == '=') {
                return arg.substr(name.size() + 1);
            }
            if (arg != name) {
                throw std::invalid_argument("internal parser mismatch");
            }
            if (done()) {
                throw std::invalid_argument("missing option value");
            }
            return take();
        }

      private:
        int argc_;
        char** argv_;
        int index_ = 1;
    };

    inline bool
    option_matches(std::string_view arg, std::string_view name) noexcept {
        return arg == name ||
               (arg.starts_with(name) && arg.size() > name.size() &&
                arg[name.size()] == '=');
    }

    inline void
    validate_config(const config& cfg) {
        if (cfg.pci_addr.empty()) {
            throw std::invalid_argument(
                "--pci-addr is required, or set INCONEL_NVME_PCI_ADDR");
        }
        if (cfg.pci_addr == "0000:03:00.0") {
            throw std::invalid_argument(
                "refusing known system-disk BDF 0000:03:00.0");
        }
        if (cfg.records == 0) {
            throw std::invalid_argument("--records must be > 0");
        }
        if (cfg.includes_run() && cfg.operations == 0) {
            throw std::invalid_argument(
                "--operations must be > 0 for run workloads");
        }
        if (cfg.value_size == 0) {
            throw std::invalid_argument("--value-size must be > 0");
        }
        const auto cls = format::kBootstrapFormatProfile.class_sizes();
        const uint32_t largest_class = cls.empty() ? 0 : cls.back();
        if (largest_class <= sizeof(format::value_object_header) ||
            cfg.value_size >
                largest_class - sizeof(format::value_object_header)) {
            throw std::invalid_argument(
                "--value-size exceeds the bootstrap value class table");
        }
        if (cfg.batch_size == 0) {
            throw std::invalid_argument("--batch-size must be > 0");
        }
        if (cfg.inflight == 0) {
            throw std::invalid_argument("--inflight must be > 0");
        }
        if (cfg.cores.empty()) {
            throw std::invalid_argument("--cores must not be empty");
        }
        if (!contains_core(cfg.cores, cfg.main_core)) {
            throw std::invalid_argument("--main-core must be in --cores");
        }
        auto validate_optional_core = [&](int32_t core, const char* what) {
            if (core < -1) {
                throw std::invalid_argument(what);
            }
            if (core >= 0 &&
                !contains_core(cfg.cores, static_cast<uint32_t>(core))) {
                throw std::invalid_argument(what);
            }
        };
        validate_optional_core(cfg.value_core, "--value-core must be in --cores");
        validate_optional_core(cfg.owner_core, "--owner-core must be in --cores");
        validate_optional_core(cfg.coord_core, "--coord-core must be in --cores");
        validate_optional_core(
            cfg.wal_space_core, "--wal-space-core must be in --cores");
        validate_optional_core(
            cfg.maintenance_core, "--maintenance-core must be in --cores");
        if (cfg.maintenance_wal_seal_percent > 100) {
            throw std::invalid_argument(
                "--maintenance-wal-seal-percent must be <= 100");
        }
        if (cfg.maintenance_max_sealed_gens_per_front == 0) {
            throw std::invalid_argument(
                "--maintenance-max-sealed-gens-per-front must be > 0");
        }
        for (uint32_t core : cfg.front_cores) {
            if (!contains_core(cfg.cores, core)) {
                throw std::invalid_argument("--front-cores must be in --cores");
            }
        }
        if (cfg.tree_cache_policy != "clock" &&
            cfg.tree_cache_policy != "slru") {
            throw std::invalid_argument("--tree-cache must be clock or slru");
        }
        if (cfg.value_cache_policy != "clock" &&
            cfg.value_cache_policy != "slru") {
            throw std::invalid_argument("--value-cache must be clock or slru");
        }
        if (cfg.qpair_depth == 0) {
            throw std::invalid_argument("--qpair-depth must be > 0");
        }
        if (cfg.verify_existing_updates && cfg.verify_existing_deletes) {
            throw std::invalid_argument(
                "--verify-existing-updates and --verify-existing-deletes "
                "are mutually exclusive");
        }
        if ((cfg.verify_existing_updates || cfg.verify_existing_deletes) &&
            cfg.verify_samples == 0) {
            throw std::invalid_argument(
                "--verify-samples must be > 0 for existing-state verify");
        }
        if (cfg.verify_existing_updates && cfg.workload != workload_kind::c) {
            throw std::invalid_argument(
                "--verify-existing-updates expects --workload c");
        }
        if (cfg.verify_existing_deletes && cfg.workload != workload_kind::c) {
            throw std::invalid_argument(
                "--verify-existing-deletes expects --workload c");
        }
        if (cfg.verify_existing_deletes && cfg.operations < cfg.records) {
            throw std::invalid_argument(
                "--verify-existing-deletes requires --operations >= --records");
        }
    }

    inline config
    parse_config(int argc, char** argv) {
        config cfg;
        if (const char* env = std::getenv("INCONEL_NVME_PCI_ADDR");
            env != nullptr) {
            cfg.pci_addr = env;
        }

        arg_reader args(argc, argv);
        while (!args.done()) {
            std::string_view arg = args.peek();
            if (arg == "--force-format") {
                args.take();
                cfg.force_format = true;
            } else if (arg == "--flush-after-load") {
                args.take();
                cfg.flush_after_load = true;
            } else if (arg == "--no-flush-after-load") {
                args.take();
                cfg.flush_after_load = false;
            } else if (option_matches(arg, "--pci-addr")) {
                cfg.pci_addr = std::string(args.take_value("--pci-addr"));
            } else if (option_matches(arg, "--workload")) {
                cfg.workload = parse_workload(args.take_value("--workload"));
            } else if (option_matches(arg, "--records")) {
                cfg.records = parse_integer<uint64_t>(
                    args.take_value("--records"), "invalid --records");
            } else if (option_matches(arg, "--operations")) {
                cfg.operations = parse_integer<uint64_t>(
                    args.take_value("--operations"), "invalid --operations");
            } else if (option_matches(arg, "--value-size")) {
                cfg.value_size = parse_integer<uint32_t>(
                    args.take_value("--value-size"), "invalid --value-size");
            } else if (option_matches(arg, "--batch-size")) {
                cfg.batch_size = parse_integer<uint32_t>(
                    args.take_value("--batch-size"), "invalid --batch-size");
            } else if (option_matches(arg, "--inflight")) {
                cfg.inflight = parse_integer<uint32_t>(
                    args.take_value("--inflight"), "invalid --inflight");
            } else if (option_matches(arg, "--seed")) {
                cfg.seed = parse_integer<uint64_t>(
                    args.take_value("--seed"), "invalid --seed");
            } else if (option_matches(arg, "--key-prefix")) {
                cfg.key_prefix = std::string(args.take_value("--key-prefix"));
            } else if (option_matches(arg, "--verify-samples")) {
                cfg.verify_samples = parse_integer<uint64_t>(
                    args.take_value("--verify-samples"),
                    "invalid --verify-samples");
            } else if (arg == "--verify-existing-updates") {
                args.take();
                cfg.verify_existing_updates = true;
            } else if (arg == "--verify-existing-deletes") {
                args.take();
                cfg.verify_existing_deletes = true;
            } else if (option_matches(arg, "--cores")) {
                cfg.cores = parse_core_csv(
                    args.take_value("--cores"), "invalid --cores");
            } else if (option_matches(arg, "--front-cores")) {
                cfg.front_cores = parse_core_csv(
                    args.take_value("--front-cores"), "invalid --front-cores");
            } else if (option_matches(arg, "--main-core")) {
                cfg.main_core = parse_integer<uint32_t>(
                    args.take_value("--main-core"), "invalid --main-core");
            } else if (option_matches(arg, "--value-core")) {
                cfg.value_core = parse_integer<int32_t>(
                    args.take_value("--value-core"), "invalid --value-core");
            } else if (option_matches(arg, "--owner-core")) {
                cfg.owner_core = parse_integer<int32_t>(
                    args.take_value("--owner-core"), "invalid --owner-core");
            } else if (option_matches(arg, "--coord-core")) {
                cfg.coord_core = parse_integer<int32_t>(
                    args.take_value("--coord-core"), "invalid --coord-core");
            } else if (option_matches(arg, "--wal-space-core")) {
                cfg.wal_space_core = parse_integer<int32_t>(
                    args.take_value("--wal-space-core"),
                    "invalid --wal-space-core");
            } else if (option_matches(arg, "--maintenance-core")) {
                cfg.maintenance_core = parse_integer<int32_t>(
                    args.take_value("--maintenance-core"),
                    "invalid --maintenance-core");
            } else if (option_matches(arg,
                                      "--maintenance-seal-active-bytes")) {
                cfg.maintenance_seal_active_bytes = parse_integer<uint64_t>(
                    args.take_value("--maintenance-seal-active-bytes"),
                    "invalid --maintenance-seal-active-bytes");
            } else if (option_matches(
                           arg, "--maintenance-total-memtable-bytes")) {
                cfg.maintenance_total_memtable_bytes = parse_integer<uint64_t>(
                    args.take_value("--maintenance-total-memtable-bytes"),
                    "invalid --maintenance-total-memtable-bytes");
            } else if (option_matches(arg,
                                      "--maintenance-wal-seal-percent")) {
                cfg.maintenance_wal_seal_percent = parse_integer<uint32_t>(
                    args.take_value("--maintenance-wal-seal-percent"),
                    "invalid --maintenance-wal-seal-percent");
            } else if (option_matches(
                           arg, "--maintenance-max-sealed-gens-per-front")) {
                cfg.maintenance_max_sealed_gens_per_front =
                    parse_integer<uint32_t>(
                        args.take_value(
                            "--maintenance-max-sealed-gens-per-front"),
                        "invalid --maintenance-max-sealed-gens-per-front");
            } else if (option_matches(arg, "--spdk-core-mask")) {
                cfg.spdk_core_mask =
                    std::string(args.take_value("--spdk-core-mask"));
            } else if (option_matches(arg, "--qpair-depth")) {
                cfg.qpair_depth = parse_integer<uint32_t>(
                    args.take_value("--qpair-depth"), "invalid --qpair-depth");
            } else if (option_matches(arg, "--tree-cache")) {
                cfg.tree_cache_policy =
                    std::string(args.take_value("--tree-cache"));
            } else if (option_matches(arg, "--value-cache")) {
                cfg.value_cache_policy =
                    std::string(args.take_value("--value-cache"));
            } else if (option_matches(arg, "--tree-cache-capacity")) {
                cfg.tree_cache_capacity = parse_integer<uint32_t>(
                    args.take_value("--tree-cache-capacity"),
                    "invalid --tree-cache-capacity");
            } else if (option_matches(arg, "--value-cache-capacity")) {
                cfg.value_cache_capacity = parse_integer<uint32_t>(
                    args.take_value("--value-cache-capacity"),
                    "invalid --value-cache-capacity");
            } else {
                throw std::invalid_argument("unknown option");
            }
        }

        validate_config(cfg);
        return cfg;
    }

}  // namespace apps::inconel::ycsb

#endif  // APPS_INCONEL_YCSB_CONFIG_HH
