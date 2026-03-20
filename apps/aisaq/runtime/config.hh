#ifndef AISAQ_RUNTIME_CONFIG_HH
#define AISAQ_RUNTIME_CONFIG_HH

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>

#include "3rd/nlohmann/json.hpp"

namespace aisaq::runtime {

    // ── NVMe device (defined once, referenced by name from cores) ──

    struct nvme_device_config {
        std::string name;              // logical name, e.g. "nvme0"
        std::string pcie;              // PCIe address, e.g. "0000:01:00.0"
        uint32_t qpair_depth = 1024;
    };

    // ── Per-core scheduler assignment ──

    struct core_nvme_config {
        std::vector<std::string> devices;   // which nvme devices this core handles
    };

    struct core_config {
        uint32_t id;

        // present = this core runs that scheduler; nullopt = not running
        bool has_task = false;
        bool has_cache = false;

        std::optional<core_nvme_config> nvme;   // nullopt = no nvme on this core
    };

    // ── Build parameters ──

    struct build_config {
        uint32_t R = 64;                // max graph degree
        uint32_t L = 100;               // build-time search list size
        float alpha = 1.2f;             // RobustPrune occlusion factor
        uint32_t max_candidates = 500;  // RobustPrune input truncation
        uint32_t n_chunks = 0;          // PQ chunks (0 = auto: ndims)
        uint32_t sample_size = 256000;  // k-means sample count
        uint32_t kmeans_iters = 12;     // Lloyd iteration count
        uint32_t num_entries = 10;      // multi-entry point count
        bool reorder = false;           // BFS reordering (SPDK+spn=1 无收益，默认关闭)
        int gpu_device = 0;             // GPU device ID (-1 = CPU only)
        std::string ptx_path = "aisaq_kernels.ptx";
    };

    // ── Search parameters ──

    struct search_config {
        uint32_t k = 10;
        uint32_t search_list = 100;
        uint32_t beam_width = 4;
        uint32_t num_probes = 1;
        uint32_t query_concurrent = 64;
    };

    // ── Cache ──

    struct cache_config {
        uint32_t max_nodes = 10000;
    };

    // ── Top-level config ──

    struct config {
        std::string index_prefix;
        uint32_t main_core = 0;        // core that submits the pipeline

        std::optional<build_config> build;
        search_config search;
        cache_config cache;

        std::vector<nvme_device_config> nvme_devices;
        std::vector<core_config> cores;

        // ── helpers ──

        const nvme_device_config*
        find_nvme_device(const std::string& name) const {
            for (auto& d : nvme_devices)
                if (d.name == name) return &d;
            return nullptr;
        }

        std::vector<uint32_t>
        core_ids() const {
            std::vector<uint32_t> ids;
            for (auto& c : cores) ids.push_back(c.id);
            return ids;
        }

        uint64_t
        core_mask() const {
            uint64_t mask = 0;
            for (auto& c : cores) mask |= 1ULL << c.id;
            return mask;
        }
    };

    // ── JSON parsing ──

    inline config
    load_config(const std::string& path) {
        using json = nlohmann::json;

        std::ifstream ifs(path);
        if (!ifs) throw std::runtime_error("cannot open config: " + path);
        json j = json::parse(ifs);

        config cfg;

        // index_prefix, main_core
        cfg.index_prefix = j.value("index_prefix", std::string{});
        cfg.main_core = j.value("main_core", 0u);

        // build (optional — only in build configs)
        if (j.contains("build")) {
            build_config bc;
            auto& b = j["build"];
            bc.R               = b.value("R",               bc.R);
            bc.L               = b.value("L",               bc.L);
            bc.alpha           = b.value("alpha",           bc.alpha);
            bc.max_candidates  = b.value("max_candidates",  bc.max_candidates);
            bc.n_chunks        = b.value("n_chunks",        bc.n_chunks);
            bc.sample_size     = b.value("sample_size",     bc.sample_size);
            bc.kmeans_iters    = b.value("kmeans_iters",    bc.kmeans_iters);
            bc.num_entries     = b.value("num_entries",      bc.num_entries);
            bc.reorder         = b.value("reorder",         bc.reorder);
            bc.gpu_device      = b.value("gpu_device",      bc.gpu_device);
            bc.ptx_path        = b.value("ptx_path",        bc.ptx_path);
            cfg.build = bc;
        }

        // search
        if (j.contains("search")) {
            auto& s = j["search"];
            cfg.search.k               = s.value("k",                cfg.search.k);
            cfg.search.search_list      = s.value("search_list",     cfg.search.search_list);
            cfg.search.beam_width       = s.value("beam_width",      cfg.search.beam_width);
            cfg.search.num_probes       = s.value("num_probes",      cfg.search.num_probes);
            cfg.search.query_concurrent = s.value("query_concurrent", cfg.search.query_concurrent);
        }

        // cache
        if (j.contains("cache")) {
            auto& c = j["cache"];
            cfg.cache.max_nodes = c.value("max_nodes", cfg.cache.max_nodes);
        }

        // nvme devices
        if (j.contains("nvme")) {
            for (auto& d : j["nvme"]) {
                nvme_device_config dc;
                dc.name        = d.at("name").get<std::string>();
                dc.pcie        = d.at("pcie").get<std::string>();
                dc.qpair_depth = d.value("qpair_depth", dc.qpair_depth);
                cfg.nvme_devices.push_back(std::move(dc));
            }
        }

        // cores
        if (!j.contains("cores") || j["cores"].empty())
            throw std::runtime_error("config: 'cores' array is required");

        for (auto& c : j["cores"]) {
            core_config cc;
            cc.id = c.at("id").get<uint32_t>();

            cc.has_task  = c.contains("task");
            cc.has_cache = c.contains("cache");

            if (c.contains("nvme")) {
                core_nvme_config nc;
                for (auto& dev : c["nvme"]["devices"])
                    nc.devices.push_back(dev.get<std::string>());
                cc.nvme = std::move(nc);

                // validate device names
                for (auto& name : cc.nvme->devices)
                    if (!cfg.find_nvme_device(name))
                        throw std::runtime_error(
                            "core " + std::to_string(cc.id) +
                            " references unknown nvme device: " + name);
            }

            cfg.cores.push_back(std::move(cc));
        }

        // Default main_core to first core if not specified
        if (!j.contains("main_core"))
            cfg.main_core = cfg.cores[0].id;

        // Validate main_core is in cores list
        bool found = false;
        for (auto& c : cfg.cores)
            if (c.id == cfg.main_core) { found = true; break; }
        if (!found)
            throw std::runtime_error(
                "config: main_core " + std::to_string(cfg.main_core) +
                " is not in cores list");

        return cfg;
    }

    inline config g_config;
}

#endif //AISAQ_RUNTIME_CONFIG_HH
