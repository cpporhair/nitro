#ifndef APPS_INCONEL_YCSB_EXPECTED_STATE_HH
#define APPS_INCONEL_YCSB_EXPECTED_STATE_HH

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../format/crc32c.hh"
#include "./config.hh"
#include "./workload.hh"

namespace apps::inconel::ycsb {

    constexpr std::string_view kExpectedStateSchema =
        "inconel-ycsb-expected-state-v1";

    enum class expected_kind : uint8_t {
        absent,
        value,
        tombstone,
    };

    struct expected_entry {
        expected_kind kind = expected_kind::absent;
        uint64_t generation = 0;
        uint32_t value_crc32c = 0;
    };

    struct expected_state {
        std::vector<expected_entry> entries;
        json phases = json::array();
    };

    [[nodiscard]] inline const char*
    expected_kind_name(expected_kind kind) noexcept {
        switch (kind) {
        case expected_kind::absent:
            return "absent";
        case expected_kind::value:
            return "value";
        case expected_kind::tombstone:
            return "tombstone";
        }
        return "absent";
    }

    [[nodiscard]] inline expected_kind
    parse_expected_kind(std::string_view text, std::string_view path) {
        if (text == "absent") {
            return expected_kind::absent;
        }
        if (text == "value") {
            return expected_kind::value;
        }
        if (text == "tombstone") {
            return expected_kind::tombstone;
        }
        throw_json_error(path, "must be absent, value, or tombstone");
    }

    [[nodiscard]] inline uint32_t
    value_crc32c_for(const config& cfg, uint64_t id, uint64_t generation) {
        const auto value = make_value(cfg, id, generation);
        return format::crc32c(value.data(), value.size());
    }

    [[nodiscard]] inline std::string
    hex_u32(uint32_t value) {
        std::ostringstream out;
        out << std::hex << std::nouppercase << std::setfill('0')
            << std::setw(8) << value;
        return out.str();
    }

    [[nodiscard]] inline uint32_t
    parse_hex_u32(std::string_view text, std::string_view path) {
        if (text.size() != 8) {
            throw_json_error(path, "must be 8 hex digits");
        }
        uint32_t out = 0;
        for (char c : text) {
            out <<= 4;
            if (c >= '0' && c <= '9') {
                out |= static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                out |= static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                out |= static_cast<uint32_t>(c - 'A' + 10);
            } else {
                throw_json_error(path, "must be 8 hex digits");
            }
        }
        return out;
    }

    [[nodiscard]] inline const json&
    required_json_field(const json& obj,
                        std::string_view key,
                        std::string_view path) {
        if (!obj.is_object()) {
            throw_json_error(path, "must be an object");
        }
        auto it = obj.find(std::string(key));
        if (it == obj.end()) {
            throw_json_error(json_path(path, key), "is required");
        }
        return *it;
    }

    [[nodiscard]] inline expected_state
    make_empty_expected_state(const config& cfg) {
        return expected_state{
            .entries = std::vector<expected_entry>(
                static_cast<std::size_t>(cfg.records)),
            .phases = json::array(),
        };
    }

    inline void
    verify_expected_fingerprint(const config& cfg,
                                const json& fingerprint) {
        reject_unknown_json_keys(fingerprint,
                                 "expected.fingerprint",
                                 {"records",
                                  "value_size",
                                  "seed",
                                  "key_prefix"});
        if (json_u64(required_json_field(fingerprint,
                                          "records",
                                          "expected.fingerprint"),
                     "expected.fingerprint.records") != cfg.records) {
            throw std::invalid_argument(
                "expected.fingerprint.records: does not match config");
        }
        if (json_u32(required_json_field(fingerprint,
                                          "value_size",
                                          "expected.fingerprint"),
                     "expected.fingerprint.value_size") != cfg.value_size) {
            throw std::invalid_argument(
                "expected.fingerprint.value_size: does not match config");
        }
        if (json_u64(required_json_field(fingerprint,
                                          "seed",
                                          "expected.fingerprint"),
                     "expected.fingerprint.seed") != cfg.seed) {
            throw std::invalid_argument(
                "expected.fingerprint.seed: does not match config");
        }
        if (json_string(required_json_field(fingerprint,
                                            "key_prefix",
                                            "expected.fingerprint"),
                        "expected.fingerprint.key_prefix") !=
            cfg.key_prefix) {
            throw std::invalid_argument(
                "expected.fingerprint.key_prefix: does not match config");
        }
    }

    [[nodiscard]] inline expected_entry
    parse_expected_entry(const config& cfg,
                         const json& value,
                         uint64_t id,
                         std::string_view path) {
        reject_unknown_json_keys(
            value, path, {"kind", "generation", "value_crc32c"});
        const auto kind = parse_expected_kind(
            json_string(required_json_field(value, "kind", path),
                        json_path(path, "kind")),
            json_path(path, "kind"));
        const uint64_t generation =
            json_u64(required_json_field(value, "generation", path),
                     json_path(path, "generation"));
        expected_entry out{
            .kind = kind,
            .generation = generation,
            .value_crc32c = 0,
        };
        if (kind == expected_kind::value) {
            out.value_crc32c = parse_hex_u32(
                json_string(required_json_field(value, "value_crc32c", path),
                            json_path(path, "value_crc32c")),
                json_path(path, "value_crc32c"));
            const uint32_t expected = value_crc32c_for(cfg, id, generation);
            if (out.value_crc32c != expected) {
                throw_json_error(json_path(path, "value_crc32c"),
                                 "does not match generated value");
            }
        }
        return out;
    }

    [[nodiscard]] inline expected_state
    load_expected_state_file(const config& cfg, const std::string& path) {
        std::ifstream input(path);
        if (!input) {
            throw std::invalid_argument("cannot open --expect-file: " + path);
        }
        try {
            json root;
            root = json::parse(input);

            reject_unknown_json_keys(root,
                                     "expected",
                                     {"schema",
                                      "fingerprint",
                                      "phases",
                                      "states"});
            const auto schema =
                json_string(required_json_field(root, "schema", "expected"),
                            "expected.schema");
            if (schema != kExpectedStateSchema) {
                throw std::invalid_argument(
                    "expected.schema: unsupported schema");
            }
            verify_expected_fingerprint(
                cfg, required_json_field(root, "fingerprint", "expected"));
            const auto& phases =
                required_json_field(root, "phases", "expected");
            if (!phases.is_array()) {
                throw_json_error("expected.phases", "must be an array");
            }
            const auto& states =
                required_json_field(root, "states", "expected");
            if (!states.is_array()) {
                throw_json_error("expected.states", "must be an array");
            }
            if (states.size() != cfg.records) {
                throw std::invalid_argument(
                    "expected.states: state count does not match records");
            }

            expected_state out{
                .entries = {},
                .phases = phases,
            };
            out.entries.reserve(static_cast<std::size_t>(cfg.records));
            for (uint64_t id = 0; id < cfg.records; ++id) {
                out.entries.push_back(parse_expected_entry(
                    cfg,
                    states[static_cast<std::size_t>(id)],
                    id,
                    json_index_path("expected.states",
                                    static_cast<std::size_t>(id))));
            }
            return out;
        } catch (const json::exception& e) {
            throw std::invalid_argument("expected state parse error in " +
                                        path + ": " + e.what());
        }
    }

    inline void
    set_expected_value(expected_state& state,
                       const config& cfg,
                       uint64_t id,
                       uint64_t generation) {
        if (id >= state.entries.size()) {
            throw std::logic_error("expected state id out of range");
        }
        state.entries[static_cast<std::size_t>(id)] = expected_entry{
            .kind = expected_kind::value,
            .generation = generation,
            .value_crc32c = value_crc32c_for(cfg, id, generation),
        };
    }

    inline void
    set_expected_tombstone(expected_state& state,
                           uint64_t id,
                           uint64_t generation) {
        if (id >= state.entries.size()) {
            throw std::logic_error("expected state id out of range");
        }
        state.entries[static_cast<std::size_t>(id)] = expected_entry{
            .kind = expected_kind::tombstone,
            .generation = generation,
            .value_crc32c = 0,
        };
    }

    inline void
    apply_expected_load_range(expected_state& state,
                              const config& cfg,
                              uint64_t first,
                              uint64_t last) {
        for (uint64_t id = first; id < last; ++id) {
            set_expected_value(state, cfg, id, 0);
        }
    }

    inline void
    apply_expected_run_operation(expected_state& state,
                                 const config& cfg,
                                 uint64_t op_index) {
        const auto kind = choose_operation(cfg, op_index);
        if (kind == operation_kind::read) {
            return;
        }
        const uint64_t id = operation_key_id(cfg, op_index);
        if (kind == operation_kind::del) {
            set_expected_tombstone(state, id, op_index + 1);
        } else {
            set_expected_value(state, cfg, id, op_index + 1);
        }
    }

    inline void
    append_expected_load_phase(expected_state& state, const config& cfg) {
        if (cfg.load_records() == 0) {
            return;
        }
        state.phases.push_back(json{
            {"kind", "load"},
            {"records", cfg.load_records()},
        });
    }

    inline void
    append_expected_run_phase(expected_state& state, const config& cfg) {
        if (cfg.run_operations() == 0) {
            return;
        }
        state.phases.push_back(json{
            {"kind", std::string(workload_name(cfg.run_kind()))},
            {"operations", cfg.run_operations()},
        });
    }

    [[nodiscard]] inline std::vector<uint64_t>
    select_expected_verify_ids(const config& cfg) {
        std::vector<uint64_t> out;
        if (cfg.expect_all) {
            out.reserve(static_cast<std::size_t>(cfg.records));
            for (uint64_t id = 0; id < cfg.records; ++id) {
                out.push_back(id);
            }
            return out;
        }

        out.reserve(static_cast<std::size_t>(cfg.expect_samples));
        for (uint64_t i = 0; i < cfg.expect_samples; ++i) {
            const uint64_t id =
                cfg.expect_samples >= cfg.records
                    ? i % cfg.records
                    : splitmix64(cfg.seed ^ 0x455850454354ULL ^ i) %
                          cfg.records;
            out.push_back(id);
        }
        return out;
    }

    [[nodiscard]] inline json
    expected_state_to_json(const config& cfg, const expected_state& state) {
        if (state.entries.size() != cfg.records) {
            throw std::logic_error(
                "expected state size does not match config records");
        }

        json states = json::array();
        for (uint64_t id = 0; id < cfg.records; ++id) {
            const auto& entry = state.entries[static_cast<std::size_t>(id)];
            json item{
                {"kind", expected_kind_name(entry.kind)},
                {"generation", entry.generation},
            };
            if (entry.kind == expected_kind::value) {
                const uint32_t crc =
                    value_crc32c_for(cfg, id, entry.generation);
                if (crc != entry.value_crc32c) {
                    throw std::logic_error(
                        "expected state value hash drifted before write");
                }
                item["value_crc32c"] = hex_u32(entry.value_crc32c);
            }
            states.push_back(std::move(item));
        }

        return json{
            {"schema", std::string(kExpectedStateSchema)},
            {"fingerprint",
             {{"records", cfg.records},
              {"value_size", cfg.value_size},
              {"seed", cfg.seed},
              {"key_prefix", cfg.key_prefix}}},
            {"phases", state.phases},
            {"states", std::move(states)},
        };
    }

    inline void
    write_expected_state_file(const config& cfg,
                              const expected_state& state,
                              const std::string& path) {
        std::ofstream output(path);
        if (!output) {
            throw std::runtime_error("cannot open --write-expect-file: " +
                                     path);
        }
        output << expected_state_to_json(cfg, state).dump(2) << "\n";
        if (!output) {
            throw std::runtime_error("failed writing --write-expect-file: " +
                                     path);
        }
    }

}  // namespace apps::inconel::ycsb

#endif  // APPS_INCONEL_YCSB_EXPECTED_STATE_HH
