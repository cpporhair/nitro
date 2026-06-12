#ifndef APPS_INCONEL_WRITE_PATH_WRITE_BATCH_STATE_HH
#define APPS_INCONEL_WRITE_PATH_WRITE_BATCH_STATE_HH

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "../core/batch_carrier.hh"

namespace apps::inconel::write_path {

    enum class write_batch_phase : uint8_t {
        assigned,
        value_durable,
        wal_durable,
        memtable_applying,
        memtable_applied,
        published,
        released,
    };

    [[nodiscard]] inline const char*
    write_batch_phase_name(write_batch_phase phase) noexcept {
        switch (phase) {
        case write_batch_phase::assigned:
            return "assigned";
        case write_batch_phase::value_durable:
            return "value_durable";
        case write_batch_phase::wal_durable:
            return "wal_durable";
        case write_batch_phase::memtable_applying:
            return "memtable_applying";
        case write_batch_phase::memtable_applied:
            return "memtable_applied";
        case write_batch_phase::published:
            return "published";
        case write_batch_phase::released:
            return "released";
        }
        return "unknown";
    }

    [[nodiscard]] inline std::string
    write_batch_phase_error(const char* site,
                            const char* expectation,
                            write_batch_phase actual) {
        std::string msg = site == nullptr ? "write_batch_state" : site;
        msg += ": expected ";
        msg += expectation;
        msg += ", actual ";
        msg += write_batch_phase_name(actual);
        return msg;
    }

    struct write_batch_state {
        core::batch_ctx ctx;
        write_batch_phase phase = write_batch_phase::assigned;

        explicit write_batch_state(core::batch_ctx&& assigned_ctx)
            : ctx(std::move(assigned_ctx)) {
            if (ctx.entry_count == 0) {
                throw std::invalid_argument(
                    "write_batch_state: entry_count must be non-zero");
            }
            if (ctx.canonical_entries.empty()) {
                throw std::invalid_argument(
                    "write_batch_state: canonical_entries must be non-empty");
            }
            if (ctx.fragments.empty()) {
                throw std::invalid_argument(
                    "write_batch_state: fragments must be non-empty");
            }
            if (ctx.batch_lsn == 0) {
                throw std::invalid_argument(
                    "write_batch_state: batch_lsn must be non-zero");
            }
        }

        write_batch_state(const write_batch_state&) = delete;
        write_batch_state&
        operator=(const write_batch_state&) = delete;

        write_batch_state(write_batch_state&&) noexcept = default;
        write_batch_state&
        operator=(write_batch_state&&) noexcept = default;
    };

    inline void
    require_write_batch_phase(const write_batch_state& state,
                              write_batch_phase expected,
                              const char* site) {
        if (state.phase != expected) {
            throw std::logic_error(write_batch_phase_error(
                site, write_batch_phase_name(expected), state.phase));
        }
    }

    inline void
    advance_write_batch_phase(write_batch_state& state,
                              write_batch_phase expected,
                              write_batch_phase next,
                              const char* site) {
        require_write_batch_phase(state, expected, site);
        state.phase = next;
    }

    inline void
    require_release_allowed(const write_batch_state& state, const char* site) {
        switch (state.phase) {
        case write_batch_phase::assigned:
        case write_batch_phase::value_durable:
        case write_batch_phase::wal_durable:
            return;
        case write_batch_phase::memtable_applying:
        case write_batch_phase::memtable_applied:
        case write_batch_phase::published:
        case write_batch_phase::released:
            break;
        }
        throw std::logic_error(
            write_batch_phase_error(site,
                                    "assigned, value_durable, or wal_durable",
                                    state.phase));
    }

} // namespace apps::inconel::write_path

#endif // APPS_INCONEL_WRITE_PATH_WRITE_BATCH_STATE_HH
