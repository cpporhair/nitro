#include <csignal>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

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

    // Split comma-separated string into vector.
    static std::vector<std::string> split_csv(const char* s) {
        std::vector<std::string> result;
        std::string cur;
        for (const char* p = s; *p; p++) {
            if (*p == ',') {
                if (!cur.empty()) { result.push_back(cur); cur.clear(); }
            } else {
                cur += *p;
            }
        }
        if (!cur.empty()) result.push_back(cur);
        return result;
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
    auto* nvme_arg   = sider::parse_string_flag(argc, argv, "--nvme");
    uint64_t dma_pool_pages = sider::parse_int_flag(argc, argv, "--dma-pages", 8192);

    std::signal(SIGINT, sider::signal_handler);
    std::signal(SIGTERM, sider::signal_handler);

    // ── NVMe + DMA init ──

    std::vector<sider::nvme::nvme_device> nvme_devs;

    if (nvme_arg) {
        auto addrs = sider::split_csv(nvme_arg);
        sider::nvme::init_env(dma_pool_pages);

        for (auto& addr : addrs) {
            auto dev = sider::nvme::init_device(addr.c_str());
            nvme_devs.push_back(dev);
            printf("NVMe disk %zu: %s (%lu pages)\n",
                   nvme_devs.size() - 1, addr.c_str(), dev.disk_pages);
        }
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

    for (auto& dev : nvme_devs) {
        auto* alloc = new sider::nvme::nvme_allocator(dev.disk_pages);
        store_sched->add_nvme_disk(dev.scheduler, alloc, dev.ssd);
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
    if (!nvme_devs.empty())
        printf(" (nvme: %zu disk%s)", nvme_devs.size(),
               nvme_devs.size() > 1 ? "s" : "");
    printf("\n");

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
