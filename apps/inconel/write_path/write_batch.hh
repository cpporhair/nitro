#ifndef APPS_INCONEL_WRITE_PATH_WRITE_BATCH_HH
#define APPS_INCONEL_WRITE_PATH_WRITE_BATCH_HH

#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <utility>

#include "pump/sender/any_exception.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/then.hh"

#include "../coord/sender.hh"
#include "../core/batch_carrier.hh"
#include "../core/panic.hh"
#include "../front/wal_append.hh"
#include "../nvme/runtime_scheduler.hh"
#include "../value/sender.hh"
#include "./sender.hh"
#include "./write_batch_state.hh"

namespace apps::inconel::write_path {

    using namespace pump::sender;

    struct write_batch_result {
        uint64_t batch_lsn = 0;
        uint32_t entry_count = 0;
    };

    [[nodiscard]] inline bool
    is_releasable_write_failure(const std::exception_ptr& ep) noexcept {
        if (!ep) {
            return false;
        }
        try {
            std::rethrow_exception(ep);
        } catch (const value::value_persist_error&) {
            return true;
        } catch (const wal::wal_append_error&) {
            return true;
        } catch (...) {
            return false;
        }
    }

    [[noreturn]] inline void
    fatal_write_batch_failure(const write_batch_state& state,
                              const std::exception_ptr& ep) {
        std::string message = "unknown exception";
        if (ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                message = e.what();
            } catch (...) {
                message = "non-std exception";
            }
        }

        core::panic_inconsistency(
            "write_path::write_batch",
            "fatal failure at phase=%s lsn=%llu: %s",
            write_batch_phase_name(state.phase),
            static_cast<unsigned long long>(state.ctx.batch_lsn),
            message.c_str());
    }

    template <typename nvme_sched_t = nvme::runtime_scheduler,
              typename NvmeProvider = value::local_nvme_provider>
    [[nodiscard]] inline auto
    write_batch(coord::coord_sched& coord_sched,
                std::span<front::front_sched* const> fronts,
                wal::wal_space_sched& wal_space,
                std::span<nvme_sched_t* const> nvme_by_owner,
                core::client_batch_buffer&& input,
                NvmeProvider value_nvme = {}) {
        return coord::assign_batch_lsn(coord_sched, std::move(input))
            >> then([](core::batch_ctx&& ctx) {
                return write_batch_state(std::move(ctx));
            })
            >> push_result_to_context()
            >> get_context<write_batch_state>()
            >> flat_map([fronts,
                          nvme_by_owner,
                          value_nvme,
                          &coord_sched,
                          &wal_space](write_batch_state& state) mutable {
                return write_batch_value_phase(state, value_nvme)
                    >> flat_map([&state,
                                  fronts,
                                  &wal_space,
                                  nvme_by_owner](bool) {
                        return write_batch_wal_phase(
                            state, fronts, wal_space, nvme_by_owner);
                    })
                    >> flat_map([&coord_sched, &state](bool) {
                        // M12/051 §4.1: memtable fan-out dispatch must run
                        // from coord's event queue, ordered against
                        // close_gate's seal_active fan-out dispatch.
                        return coord::enter_memtable_phase(
                            coord_sched, state.ctx.batch_lsn);
                    })
                    >> flat_map([&state, fronts]() {
                        return write_batch_memtable_phase(state, fronts);
                    })
                    >> flat_map([&coord_sched, &state](bool) {
                        return write_batch_publish(coord_sched, state);
                    })
                    >> then([&state](bool) {
                        return write_batch_result{
                            .batch_lsn = state.ctx.batch_lsn,
                            .entry_count = state.ctx.entry_count,
                        };
                    })
                    >> any_exception([&coord_sched, &state](
                                         std::exception_ptr ep) {
                        if (!is_releasable_write_failure(ep) ||
                            !release_allowed(state)) {
                            fatal_write_batch_failure(state, ep);
                        }
                        return write_batch_release(coord_sched, state)
                            >> then([ep = std::move(ep)](bool)
                                        -> write_batch_result {
                                std::rethrow_exception(ep);
                            });
                    });
            })
            >> pop_context();
    }

}  // namespace apps::inconel::write_path

#endif  // APPS_INCONEL_WRITE_PATH_WRITE_BATCH_HH
