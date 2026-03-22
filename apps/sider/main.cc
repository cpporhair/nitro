#include <csignal>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"
#include "pump/core/context.hh"

#include "env/runtime/share_nothing.hh"

#include "config.hh"
#include "store/scheduler.hh"
#include "store/scheduler_impl.hh"
#include "server/session.hh"
#include "server/handler.hh"
#include "nvme/init.hh"
#include "nvme/allocator.hh"

namespace sider {
    using namespace pump::sender;
    namespace tcp = pump::scheduler::tcp;

    // ── Per-core state ──

    struct core_state {
        server::session_sched_t* session_sched = nullptr;
        store::store_scheduler*  store_sched   = nullptr;
    };

    // ── NVMe group scheduler: wraps per-disk NVMe schedulers for one core ──

    struct nvme_group_scheduler {
        std::vector<nvme::nvme_scheduler_t*> schedulers;
        bool advance() {
            bool progress = false;
            for (auto* s : schedulers)
                progress |= s->advance();
            return progress;
        }
    };

    // ── Runtime type ──

    using runtime_t = pump::env::runtime::global_runtime_t<
        server::accept_sched_t,
        server::session_sched_t,
        store::store_scheduler,
        nvme_group_scheduler
    >;

    // ── DPDK thread launcher (for spdk_mempool per-lcore cache) ──

    struct dpdk_launcher {
        static void launch(uint32_t core, auto&& fn) {
            auto* f = new std::move_only_function<void()>(
                std::forward<decltype(fn)>(fn));
            spdk_env_thread_launch_pinned(core, [](void* arg) -> int {
                auto* func = static_cast<std::move_only_function<void()>*>(arg);
                (*func)();
                delete func;
                return 0;
            }, f);
        }
    };

    // ── Signal handling ──

    static runtime_t* g_runtime = nullptr;

    static void signal_handler(int) {
        if (g_runtime)
            for (auto& flag : g_runtime->is_running_by_core)
                flag.store(false, std::memory_order_relaxed);
    }

    // ── Accept loop with round-robin connection distribution ──

    static void accept_loop(server::accept_sched_t* accept_sched,
                            std::span<core_state> cores,
                            store::store_scheduler** stores, uint32_t num_stores) {
        just()
            >> forever()
            >> flat_map([accept_sched](auto&&...) {
                return tcp::wait_connection(accept_sched);
            })
            >> then([cores, stores, num_stores](int fd) {
                static uint32_t rr = 0;
                auto& target = cores[rr++ % cores.size()];
                auto* session = server::sider_factory::create(
                    fd, target.session_sched);
                server::handle_connection(
                    target.session_sched, session, stores, num_stores);
            })
            >> any_exception([](std::exception_ptr) {
                return just();
            })
            >> reduce()
            >> submit(pump::core::make_root_context());
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ── Load config ──

    sider::config cfg;
    {
        const char* config_path = nullptr;
        for (int i = 1; i + 1 < argc; i++)
            if (std::strcmp(argv[i], "--config") == 0) { config_path = argv[i + 1]; break; }

        if (config_path)
            cfg = sider::load_config(config_path);
        else
            cfg = sider::config_from_args(argc, argv);
    }

    sider::print_config(cfg);

    uint32_t num_cores = static_cast<uint32_t>(cfg.cores.size());

    // ── NVMe + DMA init ──

    std::vector<sider::nvme::nvme_device> nvme_devs;

    if (cfg.has_nvme()) {
        auto mask_str = cfg.core_mask_str();
        sider::nvme::init_env(mask_str.c_str(), cfg.dma_pages());

        for (auto& addr : cfg.nvme) {
            auto dev = sider::nvme::init_device(addr.c_str(), num_cores);
            nvme_devs.push_back(std::move(dev));
        }
    }

    // ── Create per-disk LBA pools (shared across cores) ──

    std::vector<sider::nvme::lba_pool*> lba_pools;
    for (auto& dev : nvme_devs)
        lba_pools.push_back(new sider::nvme::lba_pool(dev.disk_pages));

    // ── Create per-core schedulers ──

    std::vector<sider::core_state> cores(num_cores);
    std::vector<sider::nvme_group_scheduler*> nvme_groups(num_cores);

    uint64_t per_core_mem = cfg.per_core_memory();
    // NVMe allocator watermarks: low ≈ pages that fit in per-core memory / 64.
    uint32_t alloc_low  = per_core_mem > 0
        ? static_cast<uint32_t>(per_core_mem / 4096 / 64) : 512;
    uint32_t alloc_high = alloc_low * 2;

    for (uint32_t ci = 0; ci < num_cores; ci++) {
        auto& cs = cores[ci];

        // Set this_core_id so per_core::queue owner_core_ matches the core
        // that will run advance(). Without this, cross-core enqueue would
        // use local_q_ (no atomics) from the wrong thread → data race.
        pump::core::this_core_id = cfg.cores[ci];

        // TCP session scheduler
        cs.session_sched = new sider::server::session_sched_t(4096);
        cs.session_sched->init();

        // Store scheduler
        cs.store_sched = new sider::store::store_scheduler();
        if (per_core_mem > 0) {
            cs.store_sched->set_eviction_config(
                per_core_mem, cfg.evict_begin / 100.0, cfg.evict_urgent / 100.0);
        }

        // NVMe: shared pool per disk, per-core allocator with watermarks
        auto* nvme_group = new sider::nvme_group_scheduler();
        for (size_t di = 0; di < nvme_devs.size(); di++) {
            auto* alloc = new sider::nvme::nvme_allocator(
                lba_pools[di], alloc_low, alloc_high);

            auto& pc = nvme_devs[di].per_core[ci];
            cs.store_sched->add_nvme_disk(pc.scheduler, alloc, nvme_devs[di].ssd);
            nvme_group->schedulers.push_back(pc.scheduler);
        }
        nvme_groups[ci] = nvme_group;
    }

    // ── TCP accept scheduler (accept_core only) ──

    pump::core::this_core_id = cfg.accept_core;
    auto* accept_sched = new sider::server::accept_sched_t(256);
    if (accept_sched->init("0.0.0.0", cfg.port) < 0) {
        fprintf(stderr, "failed to listen on port %u\n", cfg.port);
        return 1;
    }

    // ── Build runtime ──

    auto* runtime = new sider::runtime_t();
    sider::g_runtime = runtime;

    for (uint32_t ci = 0; ci < num_cores; ci++) {
        uint32_t core_id = cfg.cores[ci];
        auto* accept = (core_id == cfg.accept_core) ? accept_sched : nullptr;
        runtime->add_core_schedulers(
            core_id, accept,
            cores[ci].session_sched,
            cores[ci].store_sched,
            nvme_groups[ci]);
    }

    // ── Signal handlers ──

    std::signal(SIGINT, sider::signal_handler);
    std::signal(SIGTERM, sider::signal_handler);

    // ── Build store routing table (all cores' store_schedulers) ──

    std::vector<sider::store::store_scheduler*> all_stores(num_cores);
    for (uint32_t ci = 0; ci < num_cores; ci++)
        all_stores[ci] = cores[ci].store_sched;

    // ── Fire accept loop (non-blocking pipeline submission) ──

    sider::accept_loop(accept_sched, cores,
                       all_stores.data(), num_cores);

    printf("sider listening on port %u (%u cores)\n", cfg.port, num_cores);

    // ── Pin main thread to accept_core ──

    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cfg.accept_core, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }

    // ── Launch per-core threads and block on main core ──

    auto no_init = [](auto*, uint32_t) {};
    std::span<const uint32_t> core_span{cfg.cores};

    if (cfg.has_nvme()) {
        pump::env::runtime::start<sider::dpdk_launcher>(
            runtime, core_span, cfg.accept_core, no_init);
    } else {
        pump::env::runtime::start(
            runtime, core_span, cfg.accept_core);
    }

    // ── Cleanup (after all cores stop) ──

    printf("shutting down\n");

    if (cfg.has_nvme())
        spdk_env_thread_wait_all();

    delete accept_sched;
    for (uint32_t ci = 0; ci < num_cores; ci++) {
        for (auto& disk : cores[ci].store_sched->disks_)
            delete disk.allocator;
        delete cores[ci].store_sched;
        delete cores[ci].session_sched;
        delete nvme_groups[ci];
    }
    for (auto* p : lba_pools) delete p;
    delete runtime;
    return 0;
}
