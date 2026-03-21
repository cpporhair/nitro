#include <csignal>
#include <cstdio>
#include <cstring>

#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/repeat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/any_exception.hh"
#include "pump/core/context.hh"

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

    static uint16_t parse_port(int argc, char** argv) {
        for (int i = 1; i + 1 < argc; i++) {
            if (std::strcmp(argv[i], "--port") == 0)
                return static_cast<uint16_t>(std::atoi(argv[i + 1]));
        }
        return 6379;
    }

    static uint64_t parse_memory(int argc, char** argv) {
        for (int i = 1; i + 1 < argc; i++) {
            if (std::strcmp(argv[i], "--memory") == 0) {
                char* end;
                double val = std::strtod(argv[i + 1], &end);
                switch (*end) {
                    case 'G': case 'g': return static_cast<uint64_t>(val * 1024 * 1024 * 1024);
                    case 'M': case 'm': return static_cast<uint64_t>(val * 1024 * 1024);
                    case 'K': case 'k': return static_cast<uint64_t>(val * 1024);
                    default: return static_cast<uint64_t>(val);
                }
            }
        }
        return 0;  // no limit
    }

    static int parse_int_flag(int argc, char** argv, const char* flag, int default_val) {
        for (int i = 1; i + 1 < argc; i++) {
            if (std::strcmp(argv[i], flag) == 0)
                return std::atoi(argv[i + 1]);
        }
        return default_val;
    }

    static const char* parse_string_flag(int argc, char** argv, const char* flag) {
        for (int i = 1; i + 1 < argc; i++) {
            if (std::strcmp(argv[i], flag) == 0)
                return argv[i + 1];
        }
        return nullptr;
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

    uint16_t port = sider::parse_port(argc, argv);
    uint64_t memory_limit = sider::parse_memory(argc, argv);
    int evict_begin  = sider::parse_int_flag(argc, argv, "--evict-begin", 60);
    int evict_urgent = sider::parse_int_flag(argc, argv, "--evict-urgent", 90);
    auto* nvme_addr  = sider::parse_string_flag(argc, argv, "--nvme");
    uint64_t dma_pool_pages = sider::parse_int_flag(argc, argv, "--dma-pages", 8192);

    std::signal(SIGINT, sider::signal_handler);
    std::signal(SIGTERM, sider::signal_handler);

    // ── NVMe + DMA init (before any page allocation) ──

    sider::nvme::nvme_allocator* nvme_alloc = nullptr;

    if (nvme_addr) {
        sider::nvme::init(nvme_addr, dma_pool_pages);

        uint64_t disk_pages = sider::nvme::disk_page_count();
        nvme_alloc = new sider::nvme::nvme_allocator(disk_pages);

        printf("NVMe enabled: %s (%lu pages available)\n",
               nvme_addr, disk_pages);
        printf("DMA pool: %lu pages (%luMB)\n",
               dma_pool_pages, dma_pool_pages * 4096 / (1024*1024));
    }

    // ── Scheduler creation ──

    auto* accept_sched  = new sider::server::accept_sched_t(256);
    auto* session_sched = new sider::server::session_sched_t(4096);
    auto* store_sched   = new sider::store::store_scheduler();

    session_sched->init();

    if (memory_limit > 0) {
        store_sched->set_eviction_config(
            memory_limit, evict_begin / 100.0, evict_urgent / 100.0);
    }

    if (nvme_alloc) {
        store_sched->set_nvme(
            sider::nvme::scheduler,
            nvme_alloc,
            sider::nvme::ssd_dev);
    }

    if (accept_sched->init("0.0.0.0", port) < 0) {
        fprintf(stderr, "failed to listen on port %u\n", port);
        return 1;
    }

    sider::accept_loop(accept_sched, session_sched, store_sched);

    printf("sider listening on port %u", port);
    if (memory_limit > 0)
        printf(" (memory: %luMB, evict: %d%%/%d%%)",
               memory_limit / (1024*1024), evict_begin, evict_urgent);
    if (nvme_addr)
        printf(" (nvme: %s)", nvme_addr);
    printf("\n");

    // ── Main loop ──

    while (sider::running.load(std::memory_order_relaxed)) {
        accept_sched->advance();
        session_sched->advance();
        store_sched->advance();
        if (sider::nvme::scheduler)
            sider::nvme::scheduler->advance();
    }

    printf("shutting down\n");
    delete accept_sched;
    delete session_sched;
    delete store_sched;
    delete nvme_alloc;
    return 0;
}
