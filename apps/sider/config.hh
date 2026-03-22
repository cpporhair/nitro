#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>

#include "3rd/nlohmann/json.hpp"

namespace sider {

    struct config {
        uint16_t port = 6379;
        uint64_t memory_bytes = 0;      // 0 = no eviction
        int      evict_begin = 90;      // %
        int      evict_urgent = 95;     // %
        // dma_pages is auto-calculated from memory_bytes, not user-configurable.
        uint32_t accept_core = 0;
        std::vector<std::string> nvme;  // PCIe addresses
        std::vector<uint32_t>   cores;  // store core IDs

        uint64_t per_core_memory() const {
            if (memory_bytes == 0 || cores.empty()) return 0;
            return memory_bytes / cores.size();
        }

        uint64_t core_mask() const {
            uint64_t mask = 1ULL << accept_core;
            for (auto id : cores) mask |= 1ULL << id;
            return mask;
        }

        bool has_nvme() const { return !nvme.empty(); }

        std::string core_mask_str() const {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%lx", core_mask());
            return buf;
        }

        // Auto-calculate DMA pool pages from memory limit.
        uint64_t dma_pages() const {
            if (memory_bytes == 0) return 8192;
            return memory_bytes / 4096 + 4096;
        }
    };

    // ── Parse memory string: "256M", "8G", "1024K", "1073741824" ──

    static inline uint64_t parse_memory_string(const std::string& s) {
        if (s.empty()) return 0;
        char* end;
        double val = std::strtod(s.c_str(), &end);
        switch (*end) {
            case 'G': case 'g': return static_cast<uint64_t>(val * 1024 * 1024 * 1024);
            case 'M': case 'm': return static_cast<uint64_t>(val * 1024 * 1024);
            case 'K': case 'k': return static_cast<uint64_t>(val * 1024);
            default: return static_cast<uint64_t>(val);
        }
    }

    // ── Load from JSON file ──

    static inline config load_config(const std::string& path) {
        using json = nlohmann::json;

        std::ifstream ifs(path);
        if (!ifs) throw std::runtime_error("cannot open config: " + path);
        json j = json::parse(ifs);

        config cfg;

        cfg.port         = j.value("port", cfg.port);
        cfg.evict_begin  = j.value("evict_begin", cfg.evict_begin);
        cfg.evict_urgent = j.value("evict_urgent", cfg.evict_urgent);
        cfg.accept_core  = j.value("accept_core", cfg.accept_core);

        if (j.contains("memory")) {
            auto& m = j["memory"];
            if (m.is_string())
                cfg.memory_bytes = parse_memory_string(m.get<std::string>());
            else
                cfg.memory_bytes = m.get<uint64_t>();
        }

        // nvme: array of PCIe address strings
        if (j.contains("nvme")) {
            for (auto& d : j["nvme"])
                cfg.nvme.push_back(d.get<std::string>());
        }

        // cores: array of core IDs
        if (!j.contains("cores") || j["cores"].empty())
            throw std::runtime_error("config: 'cores' array is required");

        for (auto& c : j["cores"])
            cfg.cores.push_back(c.get<uint32_t>());

        // Default accept_core to first core if not specified
        if (!j.contains("accept_core"))
            cfg.accept_core = cfg.cores[0];

        return cfg;
    }

    // ── Build config from CLI args (backward compat) ──

    static inline config config_from_args(int argc, char** argv) {
        config cfg;

        auto get_str = [&](const char* flag) -> const char* {
            for (int i = 1; i + 1 < argc; i++)
                if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
            return nullptr;
        };
        auto get_int = [&](const char* flag, int def) -> int {
            auto* s = get_str(flag);
            return s ? std::atoi(s) : def;
        };

        cfg.port = static_cast<uint16_t>(get_int("--port", 6379));
        cfg.evict_begin  = get_int("--evict-begin", 60);
        cfg.evict_urgent = get_int("--evict-urgent", 90);

        if (auto* m = get_str("--memory"))
            cfg.memory_bytes = parse_memory_string(m);

        if (auto* n = get_str("--nvme")) {
            // Comma-separated PCIe addresses
            std::string s(n);
            std::string cur;
            for (char c : s) {
                if (c == ',') {
                    if (!cur.empty()) { cfg.nvme.push_back(cur); cur.clear(); }
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) cfg.nvme.push_back(cur);
        }

        // Single core (core 0) for CLI mode
        cfg.accept_core = 0;
        cfg.cores.push_back(0);

        return cfg;
    }

    // ── Print config summary ──

    static inline void print_config(const config& cfg) {
        printf("sider config:\n");
        printf("  port: %u\n", cfg.port);
        if (cfg.memory_bytes > 0)
            printf("  memory: %luMB (per-core: %luMB)\n",
                   cfg.memory_bytes / (1024*1024),
                   cfg.per_core_memory() / (1024*1024));
        printf("  eviction: %d%%/%d%%\n", cfg.evict_begin, cfg.evict_urgent);
        printf("  accept_core: %u\n", cfg.accept_core);
        printf("  cores: [");
        for (size_t i = 0; i < cfg.cores.size(); i++)
            printf("%s%u", i ? ", " : "", cfg.cores[i]);
        printf("]\n");
        if (!cfg.nvme.empty()) {
            printf("  nvme: [");
            for (size_t i = 0; i < cfg.nvme.size(); i++)
                printf("%s%s", i ? ", " : "", cfg.nvme[i].c_str());
            printf("]\n");
        }
    }

} // namespace sider
