#ifndef APPS_INCONEL_YCSB_CONFIG_HH
#define APPS_INCONEL_YCSB_CONFIG_HH

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "3rd/nlohmann/json.hpp"

#include "../format/format_profile.hh"
#include "../format/value_object.hh"

namespace apps::inconel::ycsb {

    using json = nlohmann::json;

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
        std::string config_path;
        bool dry_run = false;
        bool print_config = true;
        bool dump_config = false;

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
        std::string expect_file;
        std::string write_expect_file;
        uint64_t expect_samples = 0;
        bool expect_all = false;

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

    inline bool
    has_duplicate_core(std::span<const uint32_t> cores) noexcept {
        for (std::size_t i = 0; i < cores.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (cores[i] == cores[j]) {
                    return true;
                }
            }
        }
        return false;
    }

    inline std::string_view
    workload_name(workload_kind v) noexcept {
        switch (v) {
        case workload_kind::load:
            return "load";
        case workload_kind::a:
            return "a";
        case workload_kind::b:
            return "b";
        case workload_kind::c:
            return "c";
        case workload_kind::update:
            return "update";
        case workload_kind::del:
            return "del";
        case workload_kind::load_a:
            return "load-a";
        case workload_kind::load_b:
            return "load-b";
        case workload_kind::load_c:
            return "load-c";
        }
        return "load";
    }

    inline workload_kind
    parse_workload(std::string_view v) {
        if (v == "load") return workload_kind::load;
        if (v == "a") return workload_kind::a;
        if (v == "b") return workload_kind::b;
        if (v == "c") return workload_kind::c;
        if (v == "update") return workload_kind::update;
        if (v == "delete" || v == "del") return workload_kind::del;
        if (v == "load-a" || v == "load_a") return workload_kind::load_a;
        if (v == "load-b" || v == "load_b") return workload_kind::load_b;
        if (v == "load-c" || v == "load_c") return workload_kind::load_c;
        throw std::invalid_argument("unknown workload");
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

    inline uint64_t
    parse_byte_size(std::string_view value, const char* what) {
        if (value.empty()) {
            throw std::invalid_argument(what);
        }

        uint64_t multiplier = 1;
        char suffix = value.back();
        if (suffix == 'K' || suffix == 'k' || suffix == 'M' ||
            suffix == 'm' || suffix == 'G' || suffix == 'g' ||
            suffix == 'T' || suffix == 't') {
            value.remove_suffix(1);
            switch (suffix) {
            case 'K':
            case 'k':
                multiplier = 1024ull;
                break;
            case 'M':
            case 'm':
                multiplier = 1024ull * 1024ull;
                break;
            case 'G':
            case 'g':
                multiplier = 1024ull * 1024ull * 1024ull;
                break;
            case 'T':
            case 't':
                multiplier = 1024ull * 1024ull * 1024ull * 1024ull;
                break;
            default:
                break;
            }
        }
        const uint64_t base = parse_integer<uint64_t>(value, what);
        if (base > std::numeric_limits<uint64_t>::max() / multiplier) {
            throw std::invalid_argument(what);
        }
        return base * multiplier;
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
        if (has_duplicate_core(out)) {
            throw std::invalid_argument(what);
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

    [[noreturn]] inline void
    throw_json_error(std::string_view path, std::string_view msg) {
        std::string out(path);
        out += ": ";
        out += msg;
        throw std::invalid_argument(out);
    }

    inline std::string
    json_path(std::string_view base, std::string_view key) {
        std::string out(base);
        if (!out.empty()) {
            out += ".";
        }
        out += key;
        return out;
    }

    inline std::string
    json_index_path(std::string_view base, std::size_t idx) {
        std::string out(base);
        out += "[";
        out += std::to_string(idx);
        out += "]";
        return out;
    }

    inline bool
    json_key_allowed(std::string_view key,
                     std::initializer_list<std::string_view> allowed) noexcept {
        for (std::string_view candidate : allowed) {
            if (key == candidate) {
                return true;
            }
        }
        return false;
    }

    inline void
    reject_unknown_json_keys(
        const json& obj,
        std::string_view path,
        std::initializer_list<std::string_view> allowed) {
        if (!obj.is_object()) {
            throw_json_error(path, "must be an object");
        }
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!json_key_allowed(it.key(), allowed)) {
                throw_json_error(json_path(path, it.key()), "unknown key");
            }
        }
    }

    inline const json*
    json_find(const json& obj, std::string_view key) {
        auto it = obj.find(std::string(key));
        if (it == obj.end()) {
            return nullptr;
        }
        return &*it;
    }

    inline std::string
    json_string(const json& value, std::string_view path) {
        if (!value.is_string()) {
            throw_json_error(path, "must be a string");
        }
        return value.get<std::string>();
    }

    inline bool
    json_bool(const json& value, std::string_view path) {
        if (!value.is_boolean()) {
            throw_json_error(path, "must be a boolean");
        }
        return value.get<bool>();
    }

    inline uint64_t
    json_u64(const json& value, std::string_view path) {
        if (value.is_number_unsigned()) {
            return value.get<uint64_t>();
        }
        if (value.is_number_integer()) {
            const int64_t n = value.get<int64_t>();
            if (n >= 0) {
                return static_cast<uint64_t>(n);
            }
        }
        throw_json_error(path, "must be an unsigned integer");
    }

    inline uint32_t
    json_u32(const json& value, std::string_view path) {
        const uint64_t n = json_u64(value, path);
        if (n > std::numeric_limits<uint32_t>::max()) {
            throw_json_error(path, "does not fit uint32");
        }
        return static_cast<uint32_t>(n);
    }

    inline int32_t
    json_i32(const json& value, std::string_view path) {
        if (!value.is_number_integer()) {
            throw_json_error(path, "must be a signed integer");
        }
        const int64_t n = value.get<int64_t>();
        if (n < std::numeric_limits<int32_t>::min() ||
            n > std::numeric_limits<int32_t>::max()) {
            throw_json_error(path, "does not fit int32");
        }
        return static_cast<int32_t>(n);
    }

    inline uint64_t
    json_byte_size(const json& value, std::string_view path) {
        if (value.is_string()) {
            try {
                return parse_byte_size(value.get<std::string>(),
                                       "invalid byte size");
            } catch (const std::invalid_argument&) {
                throw_json_error(path, "invalid byte size");
            }
        }
        return json_u64(value, path);
    }

    inline std::vector<uint32_t>
    json_core_array(const json& value, std::string_view path) {
        if (!value.is_array()) {
            throw_json_error(path, "must be an array");
        }
        std::vector<uint32_t> out;
        out.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
            out.push_back(json_u32(value[i], json_index_path(path, i)));
        }
        if (has_duplicate_core(out)) {
            throw_json_error(path, "must not contain duplicate cores");
        }
        return out;
    }

    inline void
    apply_json_config(config& cfg, const json& root) {
        reject_unknown_json_keys(root,
                                 "config",
                                 {"device",
                                  "runtime",
                                  "maintenance",
                                  "workload",
                                  "verification",
                                  "output"});

        if (const json* device = json_find(root, "device")) {
            reject_unknown_json_keys(*device,
                                     "config.device",
                                     {"pci_addr",
                                      "force_format",
                                      "spdk_core_mask",
                                      "qpair_depth"});
            if (const json* v = json_find(*device, "pci_addr")) {
                cfg.pci_addr = json_string(*v, "config.device.pci_addr");
            }
            if (const json* v = json_find(*device, "force_format")) {
                cfg.force_format = json_bool(*v, "config.device.force_format");
            }
            if (const json* v = json_find(*device, "spdk_core_mask")) {
                cfg.spdk_core_mask =
                    json_string(*v, "config.device.spdk_core_mask");
            }
            if (const json* v = json_find(*device, "qpair_depth")) {
                cfg.qpair_depth = json_u32(*v, "config.device.qpair_depth");
            }
        }

        if (const json* runtime = json_find(root, "runtime")) {
            reject_unknown_json_keys(*runtime,
                                     "config.runtime",
                                     {"cores",
                                      "main_core",
                                      "front_cores",
                                      "value_core",
                                      "owner_core",
                                      "coord_core",
                                      "wal_space_core",
                                      "maintenance_core",
                                      "tree_cache",
                                      "value_cache"});
            if (const json* v = json_find(*runtime, "cores")) {
                cfg.cores = json_core_array(*v, "config.runtime.cores");
            }
            if (const json* v = json_find(*runtime, "main_core")) {
                cfg.main_core = json_u32(*v, "config.runtime.main_core");
            }
            if (const json* v = json_find(*runtime, "front_cores")) {
                cfg.front_cores =
                    json_core_array(*v, "config.runtime.front_cores");
            }
            if (const json* v = json_find(*runtime, "value_core")) {
                cfg.value_core = json_i32(*v, "config.runtime.value_core");
            }
            if (const json* v = json_find(*runtime, "owner_core")) {
                cfg.owner_core = json_i32(*v, "config.runtime.owner_core");
            }
            if (const json* v = json_find(*runtime, "coord_core")) {
                cfg.coord_core = json_i32(*v, "config.runtime.coord_core");
            }
            if (const json* v = json_find(*runtime, "wal_space_core")) {
                cfg.wal_space_core =
                    json_i32(*v, "config.runtime.wal_space_core");
            }
            if (const json* v = json_find(*runtime, "maintenance_core")) {
                cfg.maintenance_core =
                    json_i32(*v, "config.runtime.maintenance_core");
            }
            if (const json* tree_cache = json_find(*runtime, "tree_cache")) {
                reject_unknown_json_keys(*tree_cache,
                                         "config.runtime.tree_cache",
                                         {"policy", "capacity"});
                if (const json* v = json_find(*tree_cache, "policy")) {
                    cfg.tree_cache_policy =
                        json_string(*v, "config.runtime.tree_cache.policy");
                }
                if (const json* v = json_find(*tree_cache, "capacity")) {
                    cfg.tree_cache_capacity =
                        json_u32(*v, "config.runtime.tree_cache.capacity");
                }
            }
            if (const json* value_cache = json_find(*runtime, "value_cache")) {
                reject_unknown_json_keys(*value_cache,
                                         "config.runtime.value_cache",
                                         {"policy", "capacity"});
                if (const json* v = json_find(*value_cache, "policy")) {
                    cfg.value_cache_policy =
                        json_string(*v, "config.runtime.value_cache.policy");
                }
                if (const json* v = json_find(*value_cache, "capacity")) {
                    cfg.value_cache_capacity =
                        json_u32(*v, "config.runtime.value_cache.capacity");
                }
            }
        }

        if (const json* maintenance = json_find(root, "maintenance")) {
            reject_unknown_json_keys(*maintenance,
                                     "config.maintenance",
                                     {"seal_active_bytes",
                                      "total_memtable_bytes",
                                      "wal_seal_percent",
                                      "max_sealed_gens_per_front"});
            if (const json* v = json_find(*maintenance, "seal_active_bytes")) {
                cfg.maintenance_seal_active_bytes =
                    json_byte_size(*v,
                                   "config.maintenance.seal_active_bytes");
            }
            if (const json* v =
                    json_find(*maintenance, "total_memtable_bytes")) {
                cfg.maintenance_total_memtable_bytes =
                    json_byte_size(
                        *v, "config.maintenance.total_memtable_bytes");
            }
            if (const json* v = json_find(*maintenance, "wal_seal_percent")) {
                cfg.maintenance_wal_seal_percent =
                    json_u32(*v, "config.maintenance.wal_seal_percent");
            }
            if (const json* v =
                    json_find(*maintenance, "max_sealed_gens_per_front")) {
                cfg.maintenance_max_sealed_gens_per_front =
                    json_u32(
                        *v,
                        "config.maintenance.max_sealed_gens_per_front");
            }
        }

        if (const json* workload = json_find(root, "workload")) {
            reject_unknown_json_keys(*workload,
                                     "config.workload",
                                     {"kind",
                                      "records",
                                      "operations",
                                      "value_size",
                                      "batch_size",
                                      "inflight",
                                      "seed",
                                      "key_prefix",
                                      "flush_after_load"});
            if (const json* v = json_find(*workload, "kind")) {
                try {
                    cfg.workload =
                        parse_workload(json_string(*v, "config.workload.kind"));
                } catch (const std::invalid_argument&) {
                    throw_json_error("config.workload.kind",
                                     "unknown workload");
                }
            }
            if (const json* v = json_find(*workload, "records")) {
                cfg.records = json_u64(*v, "config.workload.records");
            }
            if (const json* v = json_find(*workload, "operations")) {
                cfg.operations = json_u64(*v, "config.workload.operations");
            }
            if (const json* v = json_find(*workload, "value_size")) {
                cfg.value_size = json_u32(*v, "config.workload.value_size");
            }
            if (const json* v = json_find(*workload, "batch_size")) {
                cfg.batch_size = json_u32(*v, "config.workload.batch_size");
            }
            if (const json* v = json_find(*workload, "inflight")) {
                cfg.inflight = json_u32(*v, "config.workload.inflight");
            }
            if (const json* v = json_find(*workload, "seed")) {
                cfg.seed = json_u64(*v, "config.workload.seed");
            }
            if (const json* v = json_find(*workload, "key_prefix")) {
                cfg.key_prefix =
                    json_string(*v, "config.workload.key_prefix");
            }
            if (const json* v = json_find(*workload, "flush_after_load")) {
                cfg.flush_after_load =
                    json_bool(*v, "config.workload.flush_after_load");
            }
        }

        if (const json* verification = json_find(root, "verification")) {
            reject_unknown_json_keys(
                *verification,
                "config.verification",
                {"samples", "existing", "expected"});
            if (const json* v = json_find(*verification, "samples")) {
                cfg.verify_samples =
                    json_u64(*v, "config.verification.samples");
            }
            if (const json* v = json_find(*verification, "existing")) {
                const std::string existing =
                    json_string(*v, "config.verification.existing");
                if (existing == "none") {
                    cfg.verify_existing_updates = false;
                    cfg.verify_existing_deletes = false;
                } else if (existing == "updates") {
                    cfg.verify_existing_updates = true;
                    cfg.verify_existing_deletes = false;
                } else if (existing == "deletes") {
                    cfg.verify_existing_updates = false;
                    cfg.verify_existing_deletes = true;
                } else {
                    throw_json_error("config.verification.existing",
                                     "must be none, updates, or deletes");
                }
            }
            if (const json* expected = json_find(*verification, "expected")) {
                reject_unknown_json_keys(*expected,
                                         "config.verification.expected",
                                         {"file",
                                          "write_file",
                                          "samples",
                                          "all"});
                if (const json* v = json_find(*expected, "file")) {
                    cfg.expect_file =
                        json_string(*v, "config.verification.expected.file");
                }
                if (const json* v = json_find(*expected, "write_file")) {
                    cfg.write_expect_file = json_string(
                        *v, "config.verification.expected.write_file");
                }
                if (const json* v = json_find(*expected, "samples")) {
                    cfg.expect_samples =
                        json_u64(*v, "config.verification.expected.samples");
                }
                if (const json* v = json_find(*expected, "all")) {
                    cfg.expect_all =
                        json_bool(*v, "config.verification.expected.all");
                }
            }
        }

        if (const json* output = json_find(root, "output")) {
            reject_unknown_json_keys(
                *output, "config.output", {"print_config", "dump_config"});
            if (const json* v = json_find(*output, "print_config")) {
                cfg.print_config = json_bool(*v, "config.output.print_config");
            }
            if (const json* v = json_find(*output, "dump_config")) {
                cfg.dump_config = json_bool(*v, "config.output.dump_config");
            }
        }
    }

    inline void
    load_config_file(config& cfg, const std::string& path) {
        std::ifstream input(path);
        if (!input) {
            throw std::invalid_argument("cannot open --config: " + path);
        }
        try {
            json root = json::parse(input);
            apply_json_config(cfg, root);
        } catch (const json::exception& e) {
            throw std::invalid_argument("config parse error in " + path + ": " +
                                        e.what());
        }
    }

    inline std::string
    scan_config_path(int argc, char** argv) {
        std::string out;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (!option_matches(arg, "--config")) {
                continue;
            }
            if (arg.size() > std::string_view("--config").size() &&
                arg[std::string_view("--config").size()] == '=') {
                out = std::string(
                    arg.substr(std::string_view("--config").size() + 1));
            } else {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("missing --config value");
                }
                out = argv[++i];
            }
            if (out.empty()) {
                throw std::invalid_argument("--config must not be empty");
            }
        }
        return out;
    }

    inline void
    validate_config(const config& cfg) {
        if (cfg.pci_addr.empty()) {
            throw std::invalid_argument(
                "config.device.pci_addr or --pci-addr is required, or set "
                "INCONEL_NVME_PCI_ADDR");
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
        if (has_duplicate_core(cfg.cores)) {
            throw std::invalid_argument("--cores must not contain duplicates");
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
        if (cfg.maintenance_wal_seal_percent == 0 ||
            cfg.maintenance_wal_seal_percent > 100) {
            throw std::invalid_argument(
                "--maintenance-wal-seal-percent must be in [1, 100]");
        }
        if (cfg.maintenance_seal_active_bytes == 0) {
            throw std::invalid_argument(
                "--maintenance-seal-active-bytes must be > 0");
        }
        if (cfg.maintenance_total_memtable_bytes == 0) {
            throw std::invalid_argument(
                "--maintenance-total-memtable-bytes must be > 0");
        }
        if (cfg.maintenance_max_sealed_gens_per_front == 0) {
            throw std::invalid_argument(
                "--maintenance-max-sealed-gens-per-front must be > 0");
        }
        if (has_duplicate_core(cfg.front_cores)) {
            throw std::invalid_argument(
                "--front-cores must not contain duplicates");
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
        if (cfg.tree_cache_capacity == 0) {
            throw std::invalid_argument("--tree-cache-capacity must be > 0");
        }
        if (cfg.value_cache_capacity == 0) {
            throw std::invalid_argument("--value-cache-capacity must be > 0");
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
        const bool expect_verify = cfg.expect_all || cfg.expect_samples != 0;
        const bool expect_active = expect_verify ||
                                   !cfg.expect_file.empty() ||
                                   !cfg.write_expect_file.empty();
        if (cfg.expect_all && cfg.expect_samples != 0) {
            throw std::invalid_argument(
                "--expect-all and --expect-samples are mutually exclusive");
        }
        if (expect_active &&
            (cfg.includes_load() ||
             (cfg.includes_run() && cfg.run_kind() != workload_kind::c)) &&
            cfg.inflight != 1) {
            throw std::invalid_argument(
                "expected-state oracle for mutating workloads requires "
                "--inflight 1; use 066C interval checker for concurrent "
                "mutations");
        }
        if (expect_verify && cfg.expect_file.empty() &&
            !cfg.force_format && !cfg.includes_load()) {
            throw std::invalid_argument(
                "--expect-file is required for expected-state verify unless "
                "the run starts from --force-format or includes load");
        }
        if (!cfg.write_expect_file.empty() && cfg.expect_file.empty() &&
            !cfg.force_format && !cfg.includes_load()) {
            throw std::invalid_argument(
                "--write-expect-file without --expect-file requires "
                "--force-format or a load workload");
        }
    }

    inline void
    apply_cli_overrides(config& cfg, int argc, char** argv) {
        arg_reader args(argc, argv);
        while (!args.done()) {
            std::string_view arg = args.peek();
            if (arg == "--force-format") {
                args.take();
                cfg.force_format = true;
            } else if (arg == "--no-force-format") {
                args.take();
                cfg.force_format = false;
            } else if (arg == "--dry-run") {
                args.take();
                cfg.dry_run = true;
            } else if (arg == "--print-config") {
                args.take();
                cfg.print_config = true;
            } else if (arg == "--no-print-config") {
                args.take();
                cfg.print_config = false;
            } else if (arg == "--dump-config") {
                args.take();
                cfg.dump_config = true;
            } else if (arg == "--flush-after-load") {
                args.take();
                cfg.flush_after_load = true;
            } else if (arg == "--no-flush-after-load") {
                args.take();
                cfg.flush_after_load = false;
            } else if (option_matches(arg, "--config")) {
                cfg.config_path = std::string(args.take_value("--config"));
            } else if (option_matches(arg, "--pci-addr")) {
                cfg.pci_addr = std::string(args.take_value("--pci-addr"));
            } else if (option_matches(arg, "--pci")) {
                cfg.pci_addr = std::string(args.take_value("--pci"));
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
            } else if (option_matches(arg, "--expect-file")) {
                cfg.expect_file =
                    std::string(args.take_value("--expect-file"));
            } else if (option_matches(arg, "--write-expect-file")) {
                cfg.write_expect_file =
                    std::string(args.take_value("--write-expect-file"));
            } else if (option_matches(arg, "--expect-samples")) {
                cfg.expect_samples = parse_integer<uint64_t>(
                    args.take_value("--expect-samples"),
                    "invalid --expect-samples");
            } else if (arg == "--expect-all") {
                args.take();
                cfg.expect_all = true;
            } else if (arg == "--verify-existing-updates") {
                args.take();
                cfg.verify_existing_updates = true;
                cfg.verify_existing_deletes = false;
            } else if (arg == "--verify-existing-deletes") {
                args.take();
                cfg.verify_existing_updates = false;
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
                cfg.maintenance_seal_active_bytes = parse_byte_size(
                    args.take_value("--maintenance-seal-active-bytes"),
                    "invalid --maintenance-seal-active-bytes");
            } else if (option_matches(
                           arg, "--maintenance-total-memtable-bytes")) {
                cfg.maintenance_total_memtable_bytes = parse_byte_size(
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
            } else if (option_matches(arg, "--tree-cache-policy")) {
                cfg.tree_cache_policy =
                    std::string(args.take_value("--tree-cache-policy"));
            } else if (option_matches(arg, "--value-cache")) {
                cfg.value_cache_policy =
                    std::string(args.take_value("--value-cache"));
            } else if (option_matches(arg, "--value-cache-policy")) {
                cfg.value_cache_policy =
                    std::string(args.take_value("--value-cache-policy"));
            } else if (option_matches(arg, "--tree-cache-capacity")) {
                cfg.tree_cache_capacity = parse_integer<uint32_t>(
                    args.take_value("--tree-cache-capacity"),
                    "invalid --tree-cache-capacity");
            } else if (option_matches(arg, "--value-cache-capacity")) {
                cfg.value_cache_capacity = parse_integer<uint32_t>(
                    args.take_value("--value-cache-capacity"),
                    "invalid --value-cache-capacity");
            } else {
                throw std::invalid_argument("unknown option: " +
                                            std::string(arg));
            }
        }
    }

    inline config
    parse_config(int argc, char** argv) {
        config cfg;
        cfg.config_path = scan_config_path(argc, argv);
        if (!cfg.config_path.empty()) {
            load_config_file(cfg, cfg.config_path);
        }

        if (cfg.pci_addr.empty()) {
            if (const char* env = std::getenv("INCONEL_NVME_PCI_ADDR");
                env != nullptr) {
                cfg.pci_addr = env;
            }
        }

        apply_cli_overrides(cfg, argc, argv);
        validate_config(cfg);
        return cfg;
    }

    inline std::string_view
    verification_mode(const config& cfg) noexcept {
        if (cfg.verify_existing_updates) {
            return "updates";
        }
        if (cfg.verify_existing_deletes) {
            return "deletes";
        }
        return "none";
    }

    inline json
    config_to_json(const config& cfg) {
        return json{
            {"device",
             {{"pci_addr", cfg.pci_addr},
              {"force_format", cfg.force_format},
              {"spdk_core_mask", cfg.spdk_core_mask},
              {"qpair_depth", cfg.qpair_depth}}},
            {"runtime",
             {{"cores", cfg.cores},
              {"main_core", cfg.main_core},
              {"front_cores", cfg.front_cores},
              {"value_core", cfg.value_core},
              {"owner_core", cfg.owner_core},
              {"coord_core", cfg.coord_core},
              {"wal_space_core", cfg.wal_space_core},
              {"maintenance_core", cfg.maintenance_core},
              {"tree_cache",
               {{"policy", cfg.tree_cache_policy},
                {"capacity", cfg.tree_cache_capacity}}},
              {"value_cache",
               {{"policy", cfg.value_cache_policy},
                {"capacity", cfg.value_cache_capacity}}}}},
            {"maintenance",
             {{"seal_active_bytes", cfg.maintenance_seal_active_bytes},
              {"total_memtable_bytes", cfg.maintenance_total_memtable_bytes},
              {"wal_seal_percent", cfg.maintenance_wal_seal_percent},
              {"max_sealed_gens_per_front",
               cfg.maintenance_max_sealed_gens_per_front}}},
            {"workload",
             {{"kind", std::string(workload_name(cfg.workload))},
              {"records", cfg.records},
              {"operations", cfg.operations},
              {"value_size", cfg.value_size},
              {"batch_size", cfg.batch_size},
              {"inflight", cfg.inflight},
              {"seed", cfg.seed},
              {"key_prefix", cfg.key_prefix},
              {"flush_after_load", cfg.flush_after_load}}},
            {"verification",
             {{"samples", cfg.verify_samples},
              {"existing", std::string(verification_mode(cfg))},
              {"expected",
               {{"file", cfg.expect_file},
                {"write_file", cfg.write_expect_file},
                {"samples", cfg.expect_samples},
                {"all", cfg.expect_all}}}}},
            {"output",
             {{"print_config", cfg.print_config},
              {"dump_config", cfg.dump_config}}},
        };
    }

    inline void
    print_u32_vector(std::ostream& out, const std::vector<uint32_t>& values) {
        out << "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                out << ", ";
            }
            out << values[i];
        }
        out << "]";
    }

    inline void
    print_effective_config(std::ostream& out, const config& cfg) {
        out << "inconel_ycsb config\n"
            << "  config_path: "
            << (cfg.config_path.empty() ? "<none>" : cfg.config_path) << "\n"
            << "  device.pci_addr: " << cfg.pci_addr << "\n"
            << "  device.force_format: " << (cfg.force_format ? 1 : 0)
            << "\n"
            << "  device.spdk_core_mask: "
            << (cfg.spdk_core_mask.empty() ? "<derived>" : cfg.spdk_core_mask)
            << "\n"
            << "  device.qpair_depth: " << cfg.qpair_depth << "\n"
            << "  workload.kind: " << workload_name(cfg.workload) << "\n"
            << "  workload.records: " << cfg.records << "\n"
            << "  workload.operations: " << cfg.operations << "\n"
            << "  workload.value_size: " << cfg.value_size << "\n"
            << "  workload.batch_size: " << cfg.batch_size << "\n"
            << "  workload.inflight: " << cfg.inflight << "\n"
            << "  workload.seed: " << cfg.seed << "\n"
            << "  workload.key_prefix: " << cfg.key_prefix << "\n"
            << "  workload.flush_after_load: "
            << (cfg.flush_after_load ? 1 : 0) << "\n"
            << "  verification.samples: " << cfg.verify_samples << "\n"
            << "  verification.existing: " << verification_mode(cfg) << "\n"
            << "  verification.expected.file: "
            << (cfg.expect_file.empty() ? "<none>" : cfg.expect_file) << "\n"
            << "  verification.expected.write_file: "
            << (cfg.write_expect_file.empty() ? "<none>"
                                              : cfg.write_expect_file)
            << "\n"
            << "  verification.expected.samples: " << cfg.expect_samples
            << "\n"
            << "  verification.expected.all: " << (cfg.expect_all ? 1 : 0)
            << "\n"
            << "  runtime.cores: ";
        print_u32_vector(out, cfg.cores);
        out << "\n"
            << "  runtime.main_core: " << cfg.main_core << "\n"
            << "  runtime.front_cores: ";
        print_u32_vector(out, cfg.front_cores);
        out << "\n"
            << "  runtime.value_core: " << cfg.value_core << "\n"
            << "  runtime.owner_core: " << cfg.owner_core << "\n"
            << "  runtime.coord_core: " << cfg.coord_core << "\n"
            << "  runtime.wal_space_core: " << cfg.wal_space_core << "\n"
            << "  runtime.maintenance_core: " << cfg.maintenance_core << "\n"
            << "  runtime.tree_cache: " << cfg.tree_cache_policy
            << " capacity=" << cfg.tree_cache_capacity << "\n"
            << "  runtime.value_cache: " << cfg.value_cache_policy
            << " capacity=" << cfg.value_cache_capacity << "\n"
            << "  maintenance.seal_active_bytes: "
            << cfg.maintenance_seal_active_bytes << "\n"
            << "  maintenance.total_memtable_bytes: "
            << cfg.maintenance_total_memtable_bytes << "\n"
            << "  maintenance.wal_seal_percent: "
            << cfg.maintenance_wal_seal_percent << "\n"
            << "  maintenance.max_sealed_gens_per_front: "
            << cfg.maintenance_max_sealed_gens_per_front << "\n";
    }

    inline void
    dump_effective_config_json(std::ostream& out, const config& cfg) {
        out << config_to_json(cfg).dump(2) << "\n";
    }

}  // namespace apps::inconel::ycsb

#endif  // APPS_INCONEL_YCSB_CONFIG_HH
