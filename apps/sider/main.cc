#include <csignal>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"
#include "pump/core/context.hh"

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

    static std::atomic<bool> running{true};

    static void signal_handler(int) {
        running.store(false, std::memory_order_relaxed);
    }

    static void accept_loop(server::accept_sched_t* accept_sched,
                            server::session_sched_t* session_sched,
                            store::store_scheduler* store_sched) {
        just()
            >> forever()
            >> flat_map([accept_sched](auto&&...) {
                return tcp::wait_connection(accept_sched);
            })
            >> then([session_sched, store_sched](int fd) {
                auto* session = server::sider_factory::create(fd, session_sched);
                server::handle_connection(session_sched, session, store_sched);
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

    std::signal(SIGINT, sider::signal_handler);
    std::signal(SIGTERM, sider::signal_handler);

    // ── NVMe + DMA init ──

    std::vector<sider::nvme::nvme_device> nvme_devs;

    if (cfg.has_nvme()) {
        sider::nvme::init_env(cfg.dma_pages);

        for (auto& addr : cfg.nvme) {
            auto dev = sider::nvme::init_device(addr.c_str());
            nvme_devs.push_back(dev);
        }
    }

    // ── Scheduler creation ──

    auto* accept_sched  = new sider::server::accept_sched_t(256);
    auto* session_sched = new sider::server::session_sched_t(4096);
    auto* store_sched   = new sider::store::store_scheduler();

    session_sched->init();

    uint64_t per_core_mem = cfg.per_core_memory();
    if (per_core_mem > 0) {
        store_sched->set_eviction_config(
            per_core_mem, cfg.evict_begin / 100.0, cfg.evict_urgent / 100.0);
    }

    for (auto& dev : nvme_devs) {
        auto* alloc = new sider::nvme::nvme_allocator(dev.disk_pages);
        store_sched->add_nvme_disk(dev.scheduler, alloc, dev.ssd);
    }

    if (accept_sched->init("0.0.0.0", cfg.port) < 0) {
        fprintf(stderr, "failed to listen on port %u\n", cfg.port);
        return 1;
    }

    sider::accept_loop(accept_sched, session_sched, store_sched);

    printf("sider listening on port %u\n", cfg.port);

    // ── Main loop ──

    while (sider::running.load(std::memory_order_relaxed)) {
        accept_sched->advance();
        session_sched->advance();
        store_sched->advance();
        for (auto& dev : nvme_devs)
            dev.scheduler->advance();
    }

    printf("shutting down\n");
    delete accept_sched;
    delete session_sched;
    for (auto& disk : store_sched->disks_)
        delete disk.allocator;
    delete store_sched;
    return 0;
}
