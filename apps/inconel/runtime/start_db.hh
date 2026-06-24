#ifndef APPS_INCONEL_RUNTIME_START_DB_HH
#define APPS_INCONEL_RUNTIME_START_DB_HH

#include <atomic>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include "pump/core/context.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include "../core/page_cache.hh"
#include "./config.hh"
#include "./run.hh"
#include "./session.hh"
#include "./stop_db.hh"

namespace apps::inconel::runtime {

    template <core::cache_concept TreeCache,
              core::cache_concept ValueCache,
              typename AppFactory>
    [[nodiscard]] inline db_run_result
    run_db_with_cache(db_options opts, AppFactory&& app_factory) {
        validate_db_options(opts);
        db_session<TreeCache, ValueCache> session(std::move(opts));
        try {
            session.open_device();
            session.format_or_recover();
            session.build_runtime_instance();

            auto view = make_db_session_view(
                session.rt,
                std::span<const uint32_t>(
                    session.opts.topology.cores.data(),
                    session.opts.topology.cores.size()),
                &session.stop_requested);
            auto ctx_view = view;
            auto ctx = pump::core::make_root_context(std::move(ctx_view));
            std::atomic<uint32_t> initialized_cores{0};

            auto submit_app = [&] {
                try {
                    auto app_sender = [&]() -> decltype(auto) {
                        if constexpr (std::is_invocable_v<
                                          AppFactory&, db_session_view&>) {
                            return app_factory(view);
                        } else if constexpr (std::is_invocable_v<
                                          AppFactory&, db_session_view>) {
                            return app_factory(view);
                        } else if constexpr (std::is_invocable_v<AppFactory&>) {
                            return app_factory();
                        } else {
                            static_assert(
                                std::is_invocable_v<AppFactory&> ||
                                    std::is_invocable_v<
                                        AppFactory&, db_session_view&>,
                                "runtime::start_db app factory must be "
                                "invocable with db_session_view or no args");
                        }
                    }();

                    std::move(app_sender)
                        >> pump::sender::ignore_results()
                        >> stop_db()
                        >> pump::sender::any_exception(
                            [&session](std::exception_ptr ep) mutable {
                                session.app_error = std::move(ep);
                                return stop_db_sender();
                            })
                        >> pump::sender::submit(ctx);
                } catch (...) {
                    session.app_error = std::current_exception();
                    view.stop();
                }
            };

            ::apps::inconel::rt::start(
                session.rt,
                std::span<const uint32_t>(
                    session.opts.topology.cores.data(),
                    session.opts.topology.cores.size()),
                session.opts.topology.main_core,
                [&session, &initialized_cores, &submit_app](
                    auto* rt, uint32_t core) mutable {
                    (void)rt;
                    initialized_cores.fetch_add(
                        1, std::memory_order_acq_rel);
                    if (core == session.opts.topology.main_core) {
                        while (initialized_cores.load(
                                   std::memory_order_acquire) !=
                               session.opts.topology.cores.size()) {
                            std::this_thread::yield();
                        }
                        submit_app();
                    }
                    if (session.stop_requested.load(
                            std::memory_order_acquire)) {
                        session.rt->is_running_by_core[core].store(
                            false, std::memory_order_release);
                    }
                });
            session.collect_stats();
            session.destroy_runtime_instance();
        } catch (...) {
            session.destroy_runtime_instance();
            throw;
        }

        if (session.app_error) {
            std::rethrow_exception(session.app_error);
        }
        return session.result();
    }

    template <core::cache_concept TreeCache, typename AppFactory>
    [[nodiscard]] inline db_run_result
    run_db_with_tree_cache(db_options opts, AppFactory&& app_factory) {
        if (opts.cache.value_policy == "clock") {
            return run_db_with_cache<TreeCache, core::segmented_clock_cache>(
                std::move(opts), std::forward<AppFactory>(app_factory));
        }
        if (opts.cache.value_policy == "slru") {
            return run_db_with_cache<TreeCache, core::segmented_slru_cache>(
                std::move(opts), std::forward<AppFactory>(app_factory));
        }
        throw std::invalid_argument("unknown value cache policy");
    }

    struct start_db_binder {
        db_options opts;

        template <typename AppFactory>
        [[nodiscard]] db_run_result
        operator()(AppFactory&& app_factory) & {
            if (opts.cache.tree_policy == "clock") {
                return run_db_with_tree_cache<core::segmented_clock_cache>(
                    opts, std::forward<AppFactory>(app_factory));
            }
            if (opts.cache.tree_policy == "slru") {
                return run_db_with_tree_cache<core::segmented_slru_cache>(
                    opts, std::forward<AppFactory>(app_factory));
            }
            throw std::invalid_argument("unknown tree cache policy");
        }

        template <typename AppFactory>
        [[nodiscard]] db_run_result
        operator()(AppFactory&& app_factory) && {
            if (opts.cache.tree_policy == "clock") {
                return run_db_with_tree_cache<core::segmented_clock_cache>(
                    std::move(opts), std::forward<AppFactory>(app_factory));
            }
            if (opts.cache.tree_policy == "slru") {
                return run_db_with_tree_cache<core::segmented_slru_cache>(
                    std::move(opts), std::forward<AppFactory>(app_factory));
            }
            throw std::invalid_argument("unknown tree cache policy");
        }
    };

    [[nodiscard]] inline start_db_binder
    start_db(db_options opts) {
        return start_db_binder{.opts = std::move(opts)};
    }

    template <typename AppFactory>
    [[nodiscard]] inline db_run_result
    start_db(db_options opts, AppFactory&& app_factory) {
        return start_db(std::move(opts))(
            std::forward<AppFactory>(app_factory));
    }

}  // namespace apps::inconel::runtime

#endif  // APPS_INCONEL_RUNTIME_START_DB_HH
