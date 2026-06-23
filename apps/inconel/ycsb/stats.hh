#ifndef APPS_INCONEL_YCSB_STATS_HH
#define APPS_INCONEL_YCSB_STATS_HH

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string>
#include <utility>

namespace apps::inconel::ycsb {

    struct phase_stats {
        std::string name;
        bool ran = false;
        std::chrono::steady_clock::time_point started{};
        std::chrono::steady_clock::time_point finished{};

        std::atomic<uint64_t> generated_ops{0};
        std::atomic<uint64_t> submitted_batches{0};
        std::atomic<uint64_t> acked_entries{0};
        std::atomic<uint64_t> read_found{0};
        std::atomic<uint64_t> read_miss{0};
        std::atomic<uint64_t> write_errors{0};
        std::atomic<uint64_t> read_errors{0};

        explicit phase_stats(std::string phase_name)
            : name(std::move(phase_name)) {}

        void
        start() {
            ran = true;
            started = std::chrono::steady_clock::now();
        }

        void
        stop() {
            finished = std::chrono::steady_clock::now();
        }

        [[nodiscard]] double
        seconds() const {
            if (!ran) {
                return 0.0;
            }
            const auto end = finished == std::chrono::steady_clock::time_point{}
                ? std::chrono::steady_clock::now()
                : finished;
            const auto ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - started).count();
            return static_cast<double>(ns) / 1000000000.0;
        }
    };

    struct all_stats {
        phase_stats load{"load"};
        phase_stats load_flush{"load-flush"};
        phase_stats verify{"verify"};
        phase_stats run{"run"};
        phase_stats expect{"expect"};
    };

    inline void
    print_phase_stats(std::ostream& out, const phase_stats& s) {
        const double elapsed = s.seconds();
        const uint64_t ops = s.generated_ops.load(std::memory_order_relaxed);
        const uint64_t batches =
            s.submitted_batches.load(std::memory_order_relaxed);
        const double ops_sec = elapsed > 0.0 ? static_cast<double>(ops) / elapsed
                                             : 0.0;
        const double batches_sec =
            elapsed > 0.0 ? static_cast<double>(batches) / elapsed : 0.0;

        out << s.name
            << " elapsed_sec=" << std::fixed << std::setprecision(6) << elapsed
            << " ops=" << ops
            << " batches=" << batches
            << " acked_entries="
            << s.acked_entries.load(std::memory_order_relaxed)
            << " read_found=" << s.read_found.load(std::memory_order_relaxed)
            << " read_miss=" << s.read_miss.load(std::memory_order_relaxed)
            << " write_errors="
            << s.write_errors.load(std::memory_order_relaxed)
            << " read_errors=" << s.read_errors.load(std::memory_order_relaxed)
            << " ops_sec=" << std::fixed << std::setprecision(2) << ops_sec
            << " batches_sec=" << std::fixed << std::setprecision(2)
            << batches_sec
            << '\n';
    }

    inline void
    print_all_stats(std::ostream& out, const all_stats& stats) {
        print_phase_stats(out, stats.load);
        print_phase_stats(out, stats.load_flush);
        print_phase_stats(out, stats.verify);
        print_phase_stats(out, stats.run);
        print_phase_stats(out, stats.expect);
    }

    [[nodiscard]] inline uint64_t
    phase_error_count(const phase_stats& stats) noexcept {
        return stats.write_errors.load(std::memory_order_relaxed) +
               stats.read_errors.load(std::memory_order_relaxed);
    }

    [[nodiscard]] inline uint64_t
    total_error_count(const all_stats& stats) noexcept {
        return phase_error_count(stats.load) +
               phase_error_count(stats.load_flush) +
               phase_error_count(stats.verify) +
               phase_error_count(stats.run) +
               phase_error_count(stats.expect);
    }

}  // namespace apps::inconel::ycsb

#endif  // APPS_INCONEL_YCSB_STATS_HH
