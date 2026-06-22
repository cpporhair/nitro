#ifndef APPS_INCONEL_YCSB_WORKLOAD_HH
#define APPS_INCONEL_YCSB_WORKLOAD_HH

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

#include "../core/batch_carrier.hh"
#include "./config.hh"

namespace apps::inconel::ycsb {

    enum class operation_kind : uint8_t {
        read,
        update,
    };

    [[nodiscard]] inline uint64_t
    splitmix64(uint64_t x) noexcept {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    [[nodiscard]] inline uint32_t
    decimal_width(uint64_t value) noexcept {
        uint32_t width = 1;
        while (value >= 10) {
            value /= 10;
            ++width;
        }
        return width;
    }

    [[nodiscard]] inline std::string
    make_key(const config& cfg, uint64_t id) {
        const uint32_t width = std::max<uint32_t>(
            20, decimal_width(cfg.records == 0 ? 0 : cfg.records - 1));
        std::ostringstream out;
        out << cfg.key_prefix
            << std::setw(static_cast<int>(width))
            << std::setfill('0')
            << id;
        return out.str();
    }

    [[nodiscard]] inline std::string
    make_value(const config& cfg, uint64_t id, uint64_t generation) {
        std::string out;
        out.reserve(cfg.value_size);

        std::ostringstream prefix;
        prefix << "id=" << id << ";gen=" << generation
               << ";seed=" << cfg.seed << ";";
        out = prefix.str();

        uint64_t state =
            cfg.seed ^ (id * 0xD6E8FEB86659FD93ULL) ^
            (generation * 0xA5A5A5A5A5A5A5A5ULL);
        while (out.size() < cfg.value_size) {
            state = splitmix64(state);
            for (int shift = 0;
                 shift < 64 && out.size() < cfg.value_size;
                 shift += 8) {
                const auto c = static_cast<char>(
                    'a' + ((state >> shift) & 0x0f));
                out.push_back(c);
            }
        }
        out.resize(cfg.value_size);
        return out;
    }

    [[nodiscard]] inline uint64_t
    operation_key_id(const config& cfg, uint64_t op_index) noexcept {
        return splitmix64(cfg.seed ^ 0xC001D00DULL ^ op_index) % cfg.records;
    }

    [[nodiscard]] inline operation_kind
    choose_operation(const config& cfg, uint64_t op_index) noexcept {
        const auto kind = cfg.run_kind();
        if (kind == workload_kind::c) {
            return operation_kind::read;
        }

        const uint64_t roll =
            splitmix64(cfg.seed ^ 0x9F6ABC3DULL ^ op_index) % 100;
        if (kind == workload_kind::a) {
            return roll < 50 ? operation_kind::read : operation_kind::update;
        }
        if (kind == workload_kind::b) {
            return roll < 95 ? operation_kind::read : operation_kind::update;
        }
        return operation_kind::read;
    }

    [[nodiscard]] inline core::raw_batch_op
    make_put(const config& cfg, uint64_t id, uint64_t generation) {
        return core::raw_batch_op{
            .op = core::write_op_type::put,
            .key = make_key(cfg, id),
            .value = make_value(cfg, id, generation),
        };
    }

}  // namespace apps::inconel::ycsb

#endif  // APPS_INCONEL_YCSB_WORKLOAD_HH
