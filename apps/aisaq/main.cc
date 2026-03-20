// AiSAQ-DiskANN on PUMP
//
// Pipeline-composed beam search with NVMe sector reads.
// SPDK NVMe scheduler for async DMA reads, beam-width concurrent per iteration.

#include <cstdio>
#include <chrono>
#include <deque>
#include <memory>

#include "env/runtime/share_nothing.hh"
#include "env/scheduler/task/tasks_scheduler.hh"
#include "env/scheduler/nvme/scheduler.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/on.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/generate.hh"
#include "pump/core/context.hh"

#include "index/index.hh"
#include "search/beam_search.hh"
#include "cache/node_cache.hh"
#include "cache/scheduler.hh"
#include "nvme/init.hh"
#include "nvme/sector_page.hh"
#include "runtime/config.hh"
#include "build/builder.hh"

using namespace pump;
using namespace pump::sender;
using task_scheduler_t = pump::scheduler::task::scheduler;
using preemptive_scheduler_t = pump::scheduler::task::preemptive_scheduler;
using nvme_scheduler_t = aisaq::nvme::nvme_scheduler_t;
using cache_scheduler_t = aisaq::cache::cache_scheduler;

// Wrapper: one per core, holds N device schedulers. Runtime advance() polls all devices.
struct multi_nvme_scheduler {
    nvme_scheduler_t* devs[aisaq::nvme::MAX_DEVICES] = {};
    uint32_t count = 0;

    bool advance() {
        bool any = false;
        for (uint32_t i = 0; i < count; ++i)
            any |= devs[i]->advance();
        return any;
    }
};

using runtime_t = env::runtime::global_runtime_t<task_scheduler_t, multi_nvme_scheduler, cache_scheduler_t>;

namespace aisaq {

    // ── Write disk index file to NVMe device ──
    //
    // Pipeline: read file → for_each(sector) >> concurrent >> nvme::put >> reduce
    // Runs inside the advance loop (NVMe writes are async).

    // Pre-load disk index file into memory (call before submit, sync)
    struct index_file_data {
        uint8_t* data = nullptr;
        uint32_t total_sectors = 0;
        ~index_file_data() { delete[] data; }
    };

    inline auto
    load_index_file(const std::string& disk_path) {
        auto* ifd = new index_file_data();
        FILE* f = fopen(disk_path.c_str(), "rb");
        if (!f) throw std::runtime_error("cannot open: " + disk_path);
        fseek(f, 0, SEEK_END);
        size_t file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        ifd->total_sectors = file_size / SECTOR_SIZE;
        ifd->data = new uint8_t[file_size];
        fread(ifd->data, 1, file_size, f);
        fclose(f);
        printf("Loaded %u sectors from %s (%.1f MB)\n",
               ifd->total_sectors, disk_path.c_str(), file_size / (1024.0 * 1024.0));
        return ifd;
    }

    inline auto
    write_index_to_nvme(index_file_data* ifd, const index::sector_locator& loc,
                        nvme_scheduler_t* const* nvme_scheds) {
        uint32_t npts = ifd->total_sectors / loc.sectors_per_node - 1;
        uint32_t spn = loc.sectors_per_node;
        uint32_t num_dev = loc.num_devices;
        uint32_t total_writes = npts * spn;

        return flat_map([ifd, spn, num_dev, total_writes, nvme_scheds]() {
            printf("Writing %u nodes (%u sectors) to %u NVMe device(s) ...\n",
                   total_writes / spn, total_writes, num_dev);

            return just(std::views::iota(0u, total_writes))
                >> for_each()
                >> concurrent(256)
                >> flat_map([ifd, spn, num_dev, total_writes, nvme_scheds](uint32_t write_idx) {
                    uint32_t node_id = write_idx / spn;
                    uint32_t s = write_idx % spn;
                    uint32_t dev = node_id % num_dev;
                    uint64_t src_sector = static_cast<uint64_t>(node_id + 1) * spn + s;
                    uint64_t dst_sector = static_cast<uint64_t>(node_id / num_dev + 1) * spn + s;

                    auto* page = nvme::alloc_sector_page(dst_sector, nvme::g_ssds[dev]);
                    memcpy(page->payload, ifd->data + src_sector * SECTOR_SIZE, SECTOR_SIZE);

                    if (write_idx % 100000 == 0)
                        printf("  writing node %u / %u (dev %u)\n",
                               node_id, total_writes / spn, dev);

                    return nvme_scheds[dev]->put(page)
                        >> then([page](pump::scheduler::nvme::put::res<nvme::sector_page>&& res) {
                            nvme::free_sector_page(res.page);
                        });
                })
                >> reduce()
                >> then([ifd](bool) {
                    delete ifd;
                    printf("NVMe write complete\n");
                });
        });
    }

    // ── Benchmark context: query iterator + debug state ──

    // Ground truth for recall computation
    struct ground_truth {
        uint32_t* ids = nullptr;  // [nqueries * gt_k]
        uint32_t nqueries = 0;
        uint32_t gt_k = 0;
        double total_recall = 0;
        uint32_t recall_count = 0;

        ~ground_truth() { delete[] ids; }

        void load(const char* path) {
            FILE* f = fopen(path, "rb");
            if (!f) { printf("  no ground truth file\n"); return; }
            fread(&nqueries, 4, 1, f);
            fread(&gt_k, 4, 1, f);
            ids = new uint32_t[static_cast<size_t>(nqueries) * gt_k];
            fread(ids, 4, static_cast<size_t>(nqueries) * gt_k, f);
            fclose(f);
            printf("  loaded ground truth: %u queries × %u neighbors\n", nqueries, gt_k);
        }

        // Compute recall@K for query q_idx against result IDs
        float recall_at_k(uint32_t q_idx, const uint32_t* result_ids, uint32_t k) const {
            if (!ids || q_idx >= nqueries) return 0.0f;
            const uint32_t* gt = ids + static_cast<size_t>(q_idx) * gt_k;
            uint32_t hit = 0;
            for (uint32_t i = 0; i < k; ++i)
                for (uint32_t j = 0; j < k && j < gt_k; ++j)
                    if (result_ids[i] == gt[j]) { ++hit; break; }
            return static_cast<float>(hit) / k;
        }
    };

    struct
    benchmark_ctx {
        const float* queries;
        uint32_t ndims;
        uint32_t nqueries;
        ground_truth* gt;
    };

    // Per-core query counter for diagnostics
    inline std::atomic<uint32_t> g_core_query_count[128] = {};

    // Per-query stats, aggregated via reduce (no shared mutable state)
    struct
    query_stats {
        uint64_t io_ns = 0;
        uint64_t cpu_ns = 0;
        uint64_t io_count = 0;
        uint64_t iter_count = 0;
        double recall = 0;
        uint32_t recall_count = 0;
    };

    // ── BFS cache warmup: read nodes from NVMe level-by-level from medoid ──
    //
    // BFS from medoid → expand neighbors → cache hot nodes near graph entry point.
    // Each BFS level: batch NVMe reads → cache insert + discover neighbors → next level.
    // Mirrors beam_search_iteration pattern (for_each >> concurrent >> flat_map >> reduce).

    struct warmup_state {
        // BFS state
        std::deque<uint32_t> bfs_q;
        std::vector<uint64_t> visited;  // bitset
        cache::global_node_cache* cache;
        const index::disk_index* idx;
        uint32_t target;
        bool done = false;

        // Pre-allocated DMA buffers (reused per BFS level)
        static constexpr uint32_t BATCH = 64;
        nvme::node_read_buf bufs[BATCH];
        struct pending_node { uint32_t node_id; void* dma; };
        pending_node pending[BATCH];
        uint32_t buf_idx = 0;

        warmup_state(cache::global_node_cache* c, const index::disk_index* idx_,
                     uint32_t t)
            : cache(c), idx(idx_), target(t)
            , visited((idx_->meta.npts + 63) / 64, 0) {
            for (auto& b : bufs) b.acquire();
            // Seed BFS with medoid + entry points
            seed(static_cast<uint32_t>(idx->meta.medoid));
            for (auto id : idx->all_entry_ids)
                seed(id);
        }

        ~warmup_state() {
            for (auto& b : bufs) b.release();
        }

        warmup_state(const warmup_state&) = delete;
        warmup_state& operator=(const warmup_state&) = delete;

        void seed(uint32_t id) {
            if (id < idx->meta.npts && !test_visited(id)) {
                set_visited(id);
                bfs_q.push_back(id);
            }
        }

        bool test_visited(uint32_t id) const {
            return visited[id / 64] & (1ULL << (id % 64));
        }

        void set_visited(uint32_t id) {
            visited[id / 64] |= (1ULL << (id % 64));
        }

        // Prepare one batch from BFS queue
        std::vector<search::beam_read> prepare_batch() {
            buf_idx = 0;
            std::vector<search::beam_read> reads;
            while (!bfs_q.empty() && reads.size() < BATCH && cache->count < target) {
                uint32_t nid = bfs_q.front();
                bfs_q.pop_front();
                reads.push_back({nid, idx->locator.get_sector_id(nid),
                                 idx->locator.get_device(nid)});
            }
            if (reads.empty() || cache->count >= target)
                done = true;
            return reads;
        }

        nvme::node_read_buf& acquire_buf(uint32_t node_id) {
            auto& nb = bufs[buf_idx % BATCH];
            pending[buf_idx % BATCH] = {node_id, nb.dma_buf};
            buf_idx++;
            return nb;
        }

        // Process batch: cache insert + BFS neighbor discovery
        void process_batch() {
            uint32_t node_len = idx->locator.max_node_len;
            for (uint32_t i = 0; i < buf_idx && i < BATCH; ++i) {
                auto& p = pending[i];
                cache->insert_copy(p.node_id, p.dma, node_len);

                // Parse neighbors → expand BFS
                index::node_accessor node(static_cast<uint8_t*>(p.dma),
                    idx->meta.ndims, idx->codebook.n_chunks, idx->meta.inline_pq_width);
                uint32_t degree = node.get_degree();
                for (uint32_t j = 0; j < degree; ++j) {
                    uint32_t nbr = node.get_neighbor(j);
                    if (nbr < idx->meta.npts && !test_visited(nbr)) {
                        set_visited(nbr);
                        bfs_q.push_back(nbr);
                    }
                }
            }
        }
    };

    inline auto
    make_warmup_coro(warmup_state& ws) -> pump::coro::return_yields<bool> {
        uint32_t max_levels = ws.target / warmup_state::BATCH + 1;
        for (uint32_t i = 0; i < max_levels; ++i) {
            if (ws.done) break;
            co_yield true;
        }
        co_return false;
    }

    inline auto
    warmup_cache(cache::cache_scheduler* cache_sched, const index::disk_index& idx,
                 uint32_t target) {
        uint32_t n = std::min(target, static_cast<uint32_t>(idx.meta.npts));
        auto ws = std::make_shared<warmup_state>(
            cache_sched->cache, &idx, n);

        printf("  BFS cache warmup: target %u nodes (medoid=%lu) ...\n",
               n, idx.meta.medoid);

        return flat_map([ws]() {
            return just()
                >> then([ws]() { return pump::coro::make_view_able(make_warmup_coro(*ws)); })
                >> for_each()
                >> then([ws](auto&&) { return ws->prepare_batch(); })
                >> for_each()
                >> concurrent(warmup_state::BATCH)
                >> flat_map([ws](auto&& r) {
                    auto& nb = ws->acquire_buf(r.node_id);
                    auto* ssd = aisaq::search::g_ssd[r.device];
                    auto* nvme = aisaq::search::g_nvme[r.device];
                    nb.setup(r.sector_id, ssd);
                    return just()
                        >> pump::scheduler::nvme::get_pages(
                               nb.page_range(), nvme);
                })
                >> reduce()
                >> then([ws](bool) { ws->process_batch(); })
                >> reduce()
                >> then([ws](bool) {
                    printf("  BFS warmup complete: %u / %u nodes cached\n",
                           ws->cache->count, ws->cache->max_nodes);
                });
        });
    }

    // ── Run benchmark: all queries through search pipeline ──
    //
    // Pipeline: push_context(benchmark_ctx) >> loop(n)
    //   >> get_context >> then(next_query) >> search >> get_context >> then(print)
    //   >> reduce >> pop_context >> then(stats)

    inline auto
    run_benchmark(const index::disk_index& idx,
                  const float* queries, uint32_t nqueries, uint32_t ndims,
                  const search::search_config& cfg,
                  ground_truth* gt,
                  std::shared_ptr<std::chrono::high_resolution_clock::time_point> t0,
                  uint32_t query_concurrent) {
        return push_context(benchmark_ctx{queries, ndims, nqueries, gt})
            >> loop(nqueries)
            >> concurrent(query_concurrent)
            >> push_result_to_context()  // save q_idx before switching core
            >> on(search::g_preemptive->as_task()) // dispatch to any compute core
            >> get_context<uint64_t>()   // retrieve q_idx
            >> get_context<benchmark_ctx>()
            >> then([](benchmark_ctx& bc, auto&& q_idx) {
                return bc.queries + static_cast<size_t>(q_idx) * bc.ndims;
            })
            >> search::search(idx, cfg)
            >> get_context<uint64_t>()
            >> get_context<benchmark_ctx>()
            >> then([](benchmark_ctx& bc, auto&& q_idx, search::search_result&& sr) {
                g_core_query_count[pump::core::this_core_id].fetch_add(1, std::memory_order_relaxed);
                query_stats qs;
                qs.io_ns = sr.io_ns;
                qs.cpu_ns = sr.cpu_ns;
                qs.io_count = sr.io_count;
                qs.iter_count = sr.iter_count;
                if (bc.gt && bc.gt->ids) {
                    std::vector<uint32_t> ids(sr.results.size());
                    for (size_t i = 0; i < sr.results.size(); ++i)
                        ids[i] = sr.results[i].id;
                    uint32_t k = std::min(static_cast<uint32_t>(ids.size()), 10u);
                    qs.recall = bc.gt->recall_at_k(
                        static_cast<uint32_t>(q_idx), ids.data(), k);
                    qs.recall_count = 1;
                }
                return qs;
            })
            >> pop_context()  // pop q_idx
            >> reduce(query_stats{}, [](query_stats& acc, auto&& qs) {
                if constexpr (std::is_same_v<std::decay_t<decltype(qs)>, query_stats>) {
                    acc.io_ns += qs.io_ns;
                    acc.cpu_ns += qs.cpu_ns;
                    acc.io_count += qs.io_count;
                    acc.iter_count += qs.iter_count;
                    acc.recall += qs.recall;
                    acc.recall_count += qs.recall_count;
                }
            })
            >> then([t0, nqueries](query_stats&& qs) {
                auto t1 = std::chrono::high_resolution_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - *t0).count();
                printf("\n%u queries in %ld ms", nqueries, ms);
                // Per-core distribution
                printf("\n  Per-core:");
                for (uint32_t i = 0; i < 128; ++i) {
                    uint32_t cnt = g_core_query_count[i].load(std::memory_order_relaxed);
                    if (cnt > 0) printf(" cpu%u=%u", i, cnt);
                }
                if (ms > 0) printf(" (%.1f QPS)", nqueries * 1000.0 / ms);
                if (qs.recall_count > 0)
                    printf(", Recall@%u = %.2f%%",
                           10, 100.0 * qs.recall / qs.recall_count);
                double io_ms = qs.io_ns / 1e6;
                double cpu_ms = qs.cpu_ns / 1e6;
                printf("\n  IO: %.1f ms (%.1f%%), CPU(process_node): %.1f ms (%.1f%%)",
                       io_ms, 100.0 * io_ms / (io_ms + cpu_ms),
                       cpu_ms, 100.0 * cpu_ms / (io_ms + cpu_ms));
                printf(", per-query: IO=%.1f us, CPU=%.1f us",
                       io_ms * 1000.0 / nqueries, cpu_ms * 1000.0 / nqueries);
                printf("\n  Mean IOs/query: %.1f, Mean iters/query: %.1f",
                       static_cast<double>(qs.io_count) / nqueries,
                       static_cast<double>(qs.iter_count) / nqueries);
                printf("\n");
                _exit(0);
            })
            >> pop_context();
    }

}

// ── Convert runtime::search_config → search::search_config ──

static aisaq::search::search_config
to_search_config(const aisaq::runtime::search_config& rc) {
    return {
        .k           = rc.k,
        .search_list = rc.search_list,
        .beam_width  = rc.beam_width,
        .num_probes  = rc.num_probes,
    };
}

// ── Initialize runtime: per-core schedulers ──

static runtime_t*
init_runtime(const aisaq::runtime::config& cfg,
             aisaq::cache::global_node_cache* node_cache) {
    auto* runtime = new runtime_t();
    uint32_t num_dev = aisaq::nvme::g_num_devices;

    for (auto& core : cfg.cores) {
        task_scheduler_t* task_sched = nullptr;
        multi_nvme_scheduler* multi_nvme = nullptr;
        cache_scheduler_t* cache_sched = nullptr;

        if (core.has_task)
            task_sched = new task_scheduler_t(core.id, 8192);

        if (core.nvme && !core.nvme->devices.empty()) {
            multi_nvme = new multi_nvme_scheduler();
            for (uint32_t d = 0; d < num_dev; ++d)
                multi_nvme->devs[d] = aisaq::nvme::create_nvme_scheduler(
                    d, core.id, cfg.nvme_devices[d].qpair_depth);
            multi_nvme->count = num_dev;
        }

        if (core.has_cache && node_cache)
            cache_sched = new cache_scheduler_t(node_cache);

        runtime->add_core_schedulers(core.id, task_sched, multi_nvme, cache_sched);
        printf("  core %u: task=%s nvme=%s(%u) cache=%s\n",
               core.id,
               task_sched ? "yes" : "no",
               multi_nvme ? "yes" : "no", multi_nvme ? multi_nvme->count : 0,
               cache_sched ? "yes" : "no");
    }

    return runtime;
}

// ── Start worker cores, run main core ──

static void
pin_thread_to_core(uint32_t core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

static void
init_thread_globals(runtime_t* runtime, uint32_t core_id) {
    auto* multi_nvme = runtime->get_by_core<multi_nvme_scheduler>(core_id);
    auto* cache = runtime->get_by_core<cache_scheduler_t>(core_id);
    if (!multi_nvme) {
        for (auto& v : runtime->get_schedulers<multi_nvme_scheduler>())
            if (v) { multi_nvme = v; break; }
    }
    if (!cache) {
        for (auto& v : runtime->get_schedulers<cache_scheduler_t>())
            if (v) { cache = v; break; }
    }
    if (multi_nvme) {
        for (uint32_t i = 0; i < multi_nvme->count; ++i)
            aisaq::search::g_nvme[i] = multi_nvme->devs[i];
    }
    aisaq::search::g_task = runtime->get_by_core<task_scheduler_t>(core_id);
    aisaq::search::g_cache = cache;
}

static void
start_all_cores(runtime_t* runtime, const aisaq::runtime::config& cfg) {
    uint32_t main_core = cfg.main_core;

    for (auto& core : cfg.cores) {
        if (core.id == main_core) continue;
        std::thread([runtime, cid = core.id]() {
            pin_thread_to_core(cid);
            init_thread_globals(runtime, cid);
            env::runtime::run(runtime, cid);
        }).detach();
    }

    pin_thread_to_core(main_core);
    env::runtime::run(runtime, main_core);
}

// ── Mode: write ──

static void
mode_write(const aisaq::runtime::config& cfg) {
    std::vector<aisaq::nvme::device_init_config> dev_cfgs;
    for (auto& d : cfg.nvme_devices)
        dev_cfgs.push_back({d.pcie, d.qpair_depth});
    uint32_t nvme_core_count = 0;
    for (auto& c : cfg.cores) if (c.nvme) nvme_core_count++;
    aisaq::nvme::init_spdk_env(dev_cfgs, cfg.core_mask(), std::max(1u, nvme_core_count));

    auto* runtime = init_runtime(cfg, nullptr);

    auto* idx = new aisaq::index::disk_index();
    idx->load(cfg.index_prefix.c_str());
    idx->locator.num_devices = aisaq::nvme::g_num_devices;

    std::string disk_path = cfg.index_prefix + "_disk.index";
    auto* ifd = aisaq::load_index_file(disk_path);

    uint32_t nvme_core = cfg.main_core;
    for (auto& c : cfg.cores)
        if (c.nvme) { nvme_core = c.id; break; }
    auto* multi_nvme = runtime->get_by_core<multi_nvme_scheduler>(nvme_core);

    auto ctx = pump::core::make_root_context();
    pump::core::this_core_id = cfg.main_core;
    just() >> aisaq::write_index_to_nvme(ifd, idx->locator, multi_nvme->devs)
        >> then([]() { printf("Done.\n"); _exit(0); })
        >> submit(ctx);
    start_all_cores(runtime, cfg);
}

// ── Mode: search ──

static void
mode_search(const aisaq::runtime::config& cfg, const char* query_path, const char* gt_path) {
    std::vector<aisaq::nvme::device_init_config> dev_cfgs;
    for (auto& d : cfg.nvme_devices)
        dev_cfgs.push_back({d.pcie, d.qpair_depth});
    uint32_t nvme_core_count = 0;
    for (auto& c : cfg.cores) if (c.nvme) nvme_core_count++;
    aisaq::nvme::init_spdk_env(dev_cfgs, cfg.core_mask(), std::max(1u, nvme_core_count));

    printf("Loading index from %s ...\n", cfg.index_prefix.c_str());
    auto* idx = new aisaq::index::disk_index();
    idx->load(cfg.index_prefix.c_str());
    idx->locator.num_devices = aisaq::nvme::g_num_devices;
    printf("Index loaded: %lu vectors, %lu dims, %u PQ chunks, %u NVMe devices\n",
           idx->meta.npts, idx->meta.ndims, idx->codebook.n_chunks,
           aisaq::nvme::g_num_devices);

    uint32_t cache_nodes = cfg.cache.max_nodes;
    aisaq::cache::global_node_cache* node_cache = nullptr;
    if (cache_nodes > 0) {
        node_cache = new aisaq::cache::global_node_cache();
        node_cache->init(idx->meta.npts, idx->locator.max_node_len, cache_nodes);
        printf("Cache initialized: %u nodes capacity (%.1f MB data)\n",
               cache_nodes,
               static_cast<double>(cache_nodes) * idx->locator.max_node_len / (1024.0 * 1024.0));
    }

    auto* runtime = init_runtime(cfg, node_cache);

    uint32_t main_core = cfg.main_core;
    for (uint32_t i = 0; i < aisaq::nvme::g_num_devices; ++i)
        aisaq::search::g_ssd[i] = aisaq::nvme::g_ssds[i];
    aisaq::search::g_num_devices = aisaq::nvme::g_num_devices;
    aisaq::search::g_preemptive = runtime->any_scheduler<task_scheduler_t>();

    aisaq::search::g_task_count = 0;
    for (auto &c: cfg.cores) {
        auto *ts = runtime->get_by_core<task_scheduler_t>(c.id);
        if (ts)
            aisaq::search::g_task_list[aisaq::search::g_task_count++] = ts;
    }


    cache_scheduler_t* warmup_cache = nullptr;
    for (auto& c : cfg.cores)
        if (!warmup_cache) warmup_cache = runtime->get_by_core<cache_scheduler_t>(c.id);

    init_thread_globals(runtime, main_core);

    uint32_t nqueries, ndims;
    float* queries = aisaq::load_bin_file<float>(query_path, nqueries, ndims);

    auto search_cfg = to_search_config(cfg.search);
    uint32_t query_concurrent = cfg.search.query_concurrent;

    printf("Loaded %u queries (dim=%u), searching K=%u L=%u W=%u probes=%u concurrent=%u cache=%u devs=%u\n\n",
           nqueries, ndims, search_cfg.k, search_cfg.search_list, search_cfg.beam_width,
           search_cfg.num_probes, query_concurrent, cache_nodes,
           aisaq::nvme::g_num_devices);

    auto* gt = new aisaq::ground_truth();
    if (gt_path) gt->load(gt_path);

    auto ctx = pump::core::make_root_context();
    pump::core::this_core_id = main_core;

    auto t0 = std::make_shared<std::chrono::high_resolution_clock::time_point>(
        std::chrono::high_resolution_clock::now());

    if (warmup_cache) {
        just()
            >> aisaq::warmup_cache(warmup_cache, *idx, cache_nodes)
            >> then([t0]() { *t0 = std::chrono::high_resolution_clock::now(); })
            >> aisaq::run_benchmark(*idx, queries, nqueries, ndims, search_cfg,
                                    gt, t0, query_concurrent)
            >> submit(ctx);
    } else {
        just()
            >> aisaq::run_benchmark(*idx, queries, nqueries, ndims, search_cfg,
                                    gt, t0, query_concurrent)
            >> submit(ctx);
    }

    start_all_cores(runtime, cfg);
}

// ── Mode: build ──

static void
mode_build(const aisaq::runtime::config& cfg, const char* base_vectors_path) {
    if (!cfg.build)
        throw std::runtime_error("config missing 'build' section");

    uint32_t num_threads = 0;
    for (auto& c : cfg.cores)
        if (c.has_task) num_threads++;
    if (num_threads == 0) num_threads = 1;

    aisaq::build::build_index(*cfg.build, cfg.index_prefix,
                              base_vectors_path, num_threads);
}

// ── Entry point ──

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 3) {
        printf("Usage:\n");
        printf("  %s <config.json> write\n", argv[0]);
        printf("  %s <config.json> search <query_file> [gt_file]\n", argv[0]);
        printf("  %s <config_build.json> build <base_vectors.fbin>\n", argv[0]);
        return 1;
    }

    auto cfg = aisaq::runtime::load_config(argv[1]);
    std::string mode = argv[2];

    if (mode == "write") {
        mode_write(cfg);
    } else if (mode == "search") {
        if (argc < 4) {
            printf("Usage: %s <config.json> search <query_file> [gt_file]\n", argv[0]);
            return 1;
        }
        const char* query_path = argv[3];
        const char* gt_path = argc > 4 ? argv[4] : nullptr;
        mode_search(cfg, query_path, gt_path);
    } else if (mode == "build") {
        if (argc < 4) {
            printf("Usage: %s <config_build.json> build <base_vectors.fbin>\n", argv[0]);
            return 1;
        }
        mode_build(cfg, argv[3]);
    } else {
        printf("Unknown mode: %s (use 'write', 'search', or 'build')\n", mode.c_str());
        return 1;
    }

    return 0;
}
