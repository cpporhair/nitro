#ifndef APPS_INCONEL_YCSB_RUNNER_HH
#define APPS_INCONEL_YCSB_RUNNER_HH

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "pump/sender/any_exception.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/just.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"

#include "../pipeline/point_get.hh"
#include "../runtime/operations.hh"
#include "../runtime/run.hh"
#include "../write_path/write_batch.hh"
#include "./config.hh"
#include "./stats.hh"
#include "./workload.hh"

namespace apps::inconel::ycsb {

    struct run_state {
        std::shared_ptr<config> cfg;
        std::shared_ptr<all_stats> stats;
        std::exception_ptr error;
        std::atomic<bool> completed{false};
    };

    template <typename Runtime>
    inline void
    stop_runtime(Runtime* rt, const std::shared_ptr<const std::vector<uint32_t>>& cores) {
        for (uint32_t core : *cores) {
            rt->is_running_by_core[core].store(
                false, std::memory_order_release);
        }
    }

    [[nodiscard]] inline auto
    rethrow_bool_sender(std::exception_ptr ep) {
        return pump::sender::just()
            >> pump::sender::then([ep = std::move(ep)]() -> bool {
                std::rethrow_exception(ep);
            });
    }

    [[nodiscard]] inline uint64_t
    ceil_div(uint64_t n, uint64_t d) noexcept {
        return n == 0 ? 0 : ((n - 1) / d) + 1;
    }

    [[nodiscard]] inline core::client_batch_buffer
    encode_ops(std::vector<core::raw_batch_op>& ops) {
        return core::encode_client_batch(
            std::span<const core::raw_batch_op>(ops.data(), ops.size()));
    }

    [[nodiscard]] inline auto
    submit_write_ops(std::shared_ptr<phase_stats> stats,
                     std::vector<core::raw_batch_op> ops,
                     uint64_t logical_ops) {
        auto input = encode_ops(ops);
        return rt::write_batch(std::move(input))
            >> pump::sender::then(
                [stats, logical_ops](write_path::write_batch_result result) {
                    stats->generated_ops.fetch_add(
                        logical_ops, std::memory_order_relaxed);
                    stats->submitted_batches.fetch_add(
                        1, std::memory_order_relaxed);
                    stats->acked_entries.fetch_add(
                        result.entry_count, std::memory_order_relaxed);
                    return true;
                })
            >> pump::sender::any_exception(
                [stats](std::exception_ptr ep) mutable {
                    stats->write_errors.fetch_add(
                        1, std::memory_order_relaxed);
                    return rethrow_bool_sender(std::move(ep));
                });
    }

    [[nodiscard]] inline auto
    submit_read(std::shared_ptr<phase_stats> stats, std::string key) {
        auto key_holder = std::make_shared<std::string>(std::move(key));
        return pump::sender::just()
            >> pump::sender::flat_map([key_holder]() {
                return rt::point_get(std::string_view(*key_holder));
            })
            >> pump::sender::then(
                [stats, key_holder](pipeline::point_get_result result) {
                    (void)key_holder;
                    stats->generated_ops.fetch_add(
                        1, std::memory_order_relaxed);
                    if (result.found) {
                        stats->read_found.fetch_add(
                            1, std::memory_order_relaxed);
                    } else {
                        stats->read_miss.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    return true;
                })
            >> pump::sender::any_exception(
                [stats](std::exception_ptr ep) mutable {
                    stats->read_errors.fetch_add(
                        1, std::memory_order_relaxed);
                    return rethrow_bool_sender(std::move(ep));
                });
    }

    [[nodiscard]] inline auto
    submit_verify_point(std::shared_ptr<phase_stats> stats,
                        std::string key,
                        std::string expected,
                        bool expect_found) {
        auto key_holder = std::make_shared<std::string>(std::move(key));
        auto expected_holder =
            std::make_shared<std::string>(std::move(expected));
        return pump::sender::just()
            >> pump::sender::flat_map([key_holder]() {
                return rt::point_get(std::string_view(*key_holder));
            })
            >> pump::sender::then(
                [stats,
                 key_holder,
                 expected_holder,
                 expect_found](pipeline::point_get_result result) {
                    stats->generated_ops.fetch_add(
                        1, std::memory_order_relaxed);
                    if (expect_found &&
                        (!result.found || result.value != *expected_holder)) {
                        stats->read_errors.fetch_add(
                            1, std::memory_order_relaxed);
                        throw std::runtime_error(
                            "inconel_ycsb: verification point_get mismatch "
                            "for key " + *key_holder);
                    }
                    if (!expect_found && result.found) {
                        stats->read_errors.fetch_add(
                            1, std::memory_order_relaxed);
                        throw std::runtime_error(
                            "inconel_ycsb: verification expected tombstone "
                            "for key " + *key_holder);
                    }
                    stats->read_found.fetch_add(
                        result.found ? 1 : 0, std::memory_order_relaxed);
                    stats->read_miss.fetch_add(
                        result.found ? 0 : 1, std::memory_order_relaxed);
                    return true;
                })
            >> pump::sender::any_exception(
                [stats](std::exception_ptr ep) mutable {
                    return rethrow_bool_sender(std::move(ep));
                });
    }

    [[nodiscard]] inline auto
    submit_verify_read(std::shared_ptr<phase_stats> stats,
                       std::string key,
                       std::string expected) {
        return submit_verify_point(
            std::move(stats), std::move(key), std::move(expected), true);
    }

    [[nodiscard]] inline auto
    submit_verify_miss(std::shared_ptr<phase_stats> stats, std::string key) {
        return submit_verify_point(
            std::move(stats), std::move(key), std::string{}, false);
    }

    [[nodiscard]] inline auto
    run_load_phase(std::shared_ptr<config> cfg,
                   std::shared_ptr<phase_stats> stats) {
        return pump::sender::just()
            >> pump::sender::then([stats]() {
                stats->start();
                return true;
            })
            >> pump::sender::flat_map([cfg, stats](bool) {
                const uint64_t records = cfg->load_records();
                const uint64_t batch_count =
                    ceil_div(records, cfg->batch_size);
                return pump::sender::just()
                    >> pump::sender::loop(batch_count)
                    >> pump::sender::concurrent(cfg->inflight)
                    >> pump::sender::flat_map([cfg, stats](std::size_t idx) {
                        const uint64_t first =
                            static_cast<uint64_t>(idx) * cfg->batch_size;
                        const uint64_t last =
                            std::min<uint64_t>(
                                cfg->load_records(),
                                first + cfg->batch_size);
                        std::vector<core::raw_batch_op> ops;
                        ops.reserve(static_cast<std::size_t>(last - first));
                        for (uint64_t id = first; id < last; ++id) {
                            ops.push_back(make_put(*cfg, id, 0));
                        }
                        return submit_write_ops(
                            stats,
                            std::move(ops),
                            last - first);
                    })
                    >> pump::sender::reduce();
            })
            >> pump::sender::then([stats](bool ok) {
                stats->stop();
                return ok;
            });
    }

    [[nodiscard]] inline auto
    run_load_flush_phase(std::shared_ptr<phase_stats> stats) {
        return pump::sender::just()
            >> pump::sender::then([stats]() {
                stats->start();
                return true;
            })
            >> pump::sender::flat_map([](bool) {
                return rt::seal_once();
            })
            >> pump::sender::flat_map([](pipeline::seal_round_result) {
                return rt::flush_once();
            })
            >> pump::sender::then(
                [stats](pipeline::flush_round_result result) {
                    stats->generated_ops.fetch_add(
                        1, std::memory_order_relaxed);
                    if (!result.noop) {
                        stats->submitted_batches.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    stats->stop();
                    return true;
                })
            >> pump::sender::any_exception(
                [stats](std::exception_ptr ep) mutable {
                    stats->write_errors.fetch_add(
                        1, std::memory_order_relaxed);
                    stats->stop();
                    return rethrow_bool_sender(std::move(ep));
                });
    }

    [[nodiscard]] inline auto
    maybe_load_flush_phase(std::shared_ptr<config> cfg,
                           std::shared_ptr<phase_stats> stats) {
        auto noop = pump::sender::just(true);
        auto flush = run_load_flush_phase(std::move(stats));
        using noop_t = decltype(noop);
        using flush_t = decltype(flush);
        if (cfg->includes_load() && cfg->flush_after_load) {
            return std::variant<noop_t, flush_t>(
                std::in_place_index<1>, std::move(flush));
        }
        return std::variant<noop_t, flush_t>(
            std::in_place_index<0>, std::move(noop));
    }

    [[nodiscard]] inline auto
    run_verify_phase(std::shared_ptr<config> cfg,
                     std::shared_ptr<phase_stats> stats) {
        return pump::sender::just()
            >> pump::sender::then([stats]() {
                stats->start();
                return true;
            })
            >> pump::sender::flat_map([cfg, stats](bool) {
                const uint64_t samples =
                    (cfg->includes_load() ||
                     cfg->verify_existing_updates ||
                     cfg->verify_existing_deletes)
                        ? cfg->verify_samples
                        : 0;
                return pump::sender::just()
                    >> pump::sender::loop(samples)
                    >> pump::sender::concurrent(cfg->inflight)
                    >> pump::sender::flat_map(
                        [cfg, stats, samples](std::size_t i) {
                        const uint64_t id =
                            samples >= cfg->records
                                ? static_cast<uint64_t>(i) % cfg->records
                                : splitmix64(
                                      cfg->seed ^ 0x564552494659ULL ^ i)
                                      % cfg->records;
                        if (cfg->verify_existing_deletes) {
                            return submit_verify_miss(
                                stats, make_key(*cfg, id));
                        }
                        const uint64_t generation =
                            cfg->verify_existing_updates
                                ? expected_update_generation_for_key(
                                      *cfg, id)
                                : 0;
                        return submit_verify_read(
                            stats,
                            make_key(*cfg, id),
                            make_value(*cfg, id, generation));
                    })
                    >> pump::sender::reduce();
            })
            >> pump::sender::then([stats](bool ok) {
                stats->stop();
                return ok;
            });
    }

    [[nodiscard]] inline auto
    submit_run_operation(std::shared_ptr<config> cfg,
                         std::shared_ptr<phase_stats> stats,
                         uint64_t op_index) {
        const uint64_t id = operation_key_id(*cfg, op_index);
        using read_sender_t =
            decltype(submit_read(stats, make_key(*cfg, id)));
        using write_sender_t =
            decltype(submit_write_ops(
                stats,
                std::vector<core::raw_batch_op>{make_put(*cfg, id, op_index + 1)},
                1));

        const auto op_kind = choose_operation(*cfg, op_index);
        if (op_kind == operation_kind::read) {
            return std::variant<read_sender_t, write_sender_t>(
                std::in_place_index<0>,
                submit_read(stats, make_key(*cfg, id)));
        }

        std::vector<core::raw_batch_op> ops;
        if (op_kind == operation_kind::del) {
            ops.push_back(make_delete(*cfg, id));
        } else {
            ops.push_back(make_put(*cfg, id, op_index + 1));
        }
        return std::variant<read_sender_t, write_sender_t>(
            std::in_place_index<1>,
            submit_write_ops(stats, std::move(ops), 1));
    }

    [[nodiscard]] inline auto
    run_mixed_phase(std::shared_ptr<config> cfg,
                    std::shared_ptr<phase_stats> stats) {
        return pump::sender::just()
            >> pump::sender::then([stats]() {
                stats->start();
                return true;
            })
            >> pump::sender::flat_map([cfg, stats](bool) {
                const uint64_t ops = cfg->run_operations();
                return pump::sender::just()
                    >> pump::sender::loop(ops)
                    >> pump::sender::concurrent(cfg->inflight)
                    >> pump::sender::flat_map([cfg, stats](std::size_t i) {
                        return submit_run_operation(
                            cfg, stats, static_cast<uint64_t>(i));
                    })
                    >> pump::sender::reduce();
            })
            >> pump::sender::then([stats](bool ok) {
                stats->stop();
                return ok;
            });
    }

    [[nodiscard]] inline auto
    run_workload(std::shared_ptr<run_state> state) {
        return run_load_phase(state->cfg, std::shared_ptr<phase_stats>(
                                              state->stats, &state->stats->load))
            >> pump::sender::flat_map([state](bool) {
                return maybe_load_flush_phase(
                    state->cfg,
                    std::shared_ptr<phase_stats>(
                        state->stats, &state->stats->load_flush));
            })
            >> pump::sender::flat_map([state](bool) {
                return run_verify_phase(
                    state->cfg,
                    std::shared_ptr<phase_stats>(
                        state->stats, &state->stats->verify));
            })
            >> pump::sender::flat_map([state](bool) {
                return run_mixed_phase(
                    state->cfg,
                    std::shared_ptr<phase_stats>(
                        state->stats, &state->stats->run));
            })
            >> pump::sender::then([](bool) {
                return true;
            });
    }

}  // namespace apps::inconel::ycsb

#endif  // APPS_INCONEL_YCSB_RUNNER_HH
