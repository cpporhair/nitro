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
#include "server/session.hh"
#include "server/handler.hh"

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

    std::signal(SIGINT, sider::signal_handler);
    std::signal(SIGTERM, sider::signal_handler);

    auto* accept_sched  = new sider::server::accept_sched_t();
    auto* session_sched = new sider::server::session_sched_t();
    auto* store_sched   = new sider::store::store_scheduler();

    if (accept_sched->init("0.0.0.0", port) < 0) {
        fprintf(stderr, "failed to listen on port %u\n", port);
        return 1;
    }

    sider::accept_loop(accept_sched, session_sched, store_sched);

    printf("sider listening on port %u\n", port);

    while (sider::running.load(std::memory_order_relaxed)) {
        accept_sched->advance();
        session_sched->advance();
        store_sched->advance();
    }

    printf("shutting down\n");
    delete accept_sched;
    delete session_sched;
    delete store_sched;
    return 0;
}
