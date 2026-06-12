// M13 / 052 §8 — standalone profile binary over the mock NVMe backend.
// NOT a CTest gate; run manually:
//
//   ./build/inconel_profile_m13 [batches] [value_bytes] [keys_per_batch] [cores]
//
// Reports write throughput / latency percentiles and read throughput over
// the full production stack (build_runtime → rt::write_batch /
// rt::point_get). The mock device serializes all I/O behind one mutex, so
// absolute numbers understate a real device's parallelism — compare ratios
// across runs, not absolutes across backends (CLAUDE.md 性能测试方法论).
//
// Authored by the review side per 052 §0 role split.

#include "apps/inconel/runtime/operations.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "apps/inconel/core/registry.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/runtime/run.hh"
#include "pump/core/context.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include <pthread.h>
#include <thread>

using namespace apps::inconel;
using clock_t_ = std::chrono::steady_clock;

namespace {

constexpr uint32_t kLbaSize = 4096;
constexpr uint64_t kNamespaceLbas = 110000;

using tree_cache_t = core::segmented_clock_cache;
using value_cache_t = core::segmented_clock_cache;
using runtime_t = runtime::inconel_runtime_t<tree_cache_t, value_cache_t>;

template <typename T>
using op_result = std::variant<T, std::exception_ptr>;

template <typename T>
struct submission {
    decltype(pump::core::make_root_context()) ctx;
    std::future<op_result<T>> fut;
};

template <typename T, typename SenderBuilder>
submission<T> submit_result(SenderBuilder&& build) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<op_result<T>>>();
    auto fut = promise->get_future();
    auto caught = std::make_shared<std::exception_ptr>();
    std::forward<SenderBuilder>(build)()
        >> pump::sender::any_exception([caught](std::exception_ptr ep) {
            *caught = std::move(ep);
            return pump::sender::just(T{});
        })
        >> pump::sender::then([promise, caught](auto&& v) mutable {
            if (*caught) promise->set_value(*caught);
            else promise->set_value(T(std::forward<decltype(v)>(v)));
        })
        >> pump::sender::submit(ctx);
    return {std::move(ctx), std::move(fut)};
}

template <typename T>
T must(op_result<T>&& r, const char* what) {
    if (std::holds_alternative<std::exception_ptr>(r)) {
        try {
            std::rethrow_exception(std::get<std::exception_ptr>(r));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s failed: %s\n", what, e.what());
        }
        std::exit(1);
    }
    return std::move(std::get<T>(r));
}

void pin_to_core(unsigned core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    const auto idx = static_cast<std::size_t>(
        p * static_cast<double>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + idx, v.end());
    return v[idx];
}

}  // namespace

int main(int argc, char** argv) {
    const uint32_t batches = argc > 1 ? std::atoi(argv[1]) : 20000;
    const uint32_t value_bytes = argc > 2 ? std::atoi(argv[2]) : 256;
    const uint32_t keys_per_batch = argc > 3 ? std::atoi(argv[3]) : 1;
    const uint32_t core_count =
        argc > 4 ? std::atoi(argv[4]) : 4;

    std::vector<uint32_t> cores;
    for (uint32_t c = 0; c < core_count; ++c) cores.push_back(c);

    pump::core::this_core_id = 0;
    nvme::runtime_device device(kLbaSize, kNamespaceLbas, 0);
    runtime::build_options bopts{
        .cores = std::span<const uint32_t>(cores.data(), cores.size()),
        .device = &device,
    };
    auto* rt_ = runtime::build_runtime<tree_cache_t, value_cache_t>(bopts);

    std::vector<std::thread> runtime_threads;
    for (uint32_t core : cores) {
        runtime_threads.emplace_back([rt_, core]() {
            pin_to_core(core);
            rt::run(rt_, core);
        });
    }

    const uint32_t client_core = core_count + 4;
    pump::core::this_core_id = client_core;
    const std::string body(value_bytes, 'p');

    std::printf("inconel_profile_m13: mock backend, cores=%u fronts=%u "
                "batches=%u keys/batch=%u value=%uB\n",
                core_count, core::registry::front_count(), batches,
                keys_per_batch, value_bytes);
    std::printf("note: mock device serializes I/O behind one mutex; "
                "compare ratios, not cross-backend absolutes.\n");

    // ── write phase (windowed, latency per batch) ──
    std::vector<double> write_us;
    write_us.reserve(batches);
    std::vector<std::string> keys;
    keys.reserve(static_cast<std::size_t>(batches) * keys_per_batch);

    constexpr uint32_t kWindow = 16;
    std::vector<std::pair<submission<write_path::write_batch_result>,
                          clock_t_::time_point>> inflight;

    const auto write_t0 = clock_t_::now();
    for (uint32_t i = 0; i < batches; ++i) {
        std::vector<core::raw_batch_op> ops;
        for (uint32_t k = 0; k < keys_per_batch; ++k) {
            keys.push_back("pk-" + std::to_string(i) + "-" +
                           std::to_string(k));
            ops.push_back({.op = core::write_op_type::put,
                           .key = keys.back(),
                           .value = body});
        }
        auto input = core::encode_client_batch(
            std::span<const core::raw_batch_op>(ops.data(), ops.size()));
        inflight.emplace_back(
            submit_result<write_path::write_batch_result>(
                [input = std::move(input)]() mutable {
                    return rt::write_batch(std::move(input));
                }),
            clock_t_::now());
        if (inflight.size() >= kWindow) {
            auto& [sub, t0] = inflight.front();
            (void)must(sub.fut.get(), "write_batch");
            write_us.push_back(
                std::chrono::duration<double, std::micro>(
                    clock_t_::now() - t0)
                    .count());
            inflight.erase(inflight.begin());
        }
    }
    while (!inflight.empty()) {
        auto& [sub, t0] = inflight.front();
        (void)must(sub.fut.get(), "write_batch");
        write_us.push_back(std::chrono::duration<double, std::micro>(
                               clock_t_::now() - t0)
                               .count());
        inflight.erase(inflight.begin());
    }
    const double write_secs =
        std::chrono::duration<double>(clock_t_::now() - write_t0).count();

    // ── read phase (every key, sequential round-trips) ──
    const auto read_t0 = clock_t_::now();
    std::vector<double> read_us;
    read_us.reserve(keys.size());
    for (const auto& key : keys) {
        const auto t0 = clock_t_::now();
        auto sub = submit_result<pipeline::point_get_result>(
            [&key]() { return rt::point_get(key); });
        auto got = must(sub.fut.get(), "point_get");
        if (!got.found || got.value.size() != value_bytes) {
            std::fprintf(stderr, "read verification failed for %s\n",
                         key.c_str());
            return 1;
        }
        read_us.push_back(std::chrono::duration<double, std::micro>(
                              clock_t_::now() - t0)
                              .count());
    }
    const double read_secs =
        std::chrono::duration<double>(clock_t_::now() - read_t0).count();

    for (uint32_t core : cores) rt_->is_running_by_core[core].store(false);
    for (auto& t : runtime_threads) t.join();
    pump::core::this_core_id = 0;

    const auto counts = device.counts();
    std::printf("writes : %u batches in %.3fs = %.0f batch/s, "
                "%.0f kv/s | lat p50=%.1fus p99=%.1fus\n",
                batches, write_secs,
                batches / write_secs,
                batches * static_cast<double>(keys_per_batch) / write_secs,
                pct(write_us, 0.50), pct(write_us, 0.99));
    std::printf("reads  : %zu gets in %.3fs = %.0f get/s | "
                "lat p50=%.1fus p99=%.1fus\n",
                keys.size(), read_secs, keys.size() / read_secs,
                pct(read_us, 0.50), pct(read_us, 0.99));
    std::printf("device : reads=%lu writes=%lu (fua=%lu) trims=%lu "
                "flushes=%lu\n",
                static_cast<unsigned long>(counts.reads),
                static_cast<unsigned long>(counts.writes),
                static_cast<unsigned long>(counts.fua_writes),
                static_cast<unsigned long>(counts.trims),
                static_cast<unsigned long>(counts.flushes));

    runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt_);
    return 0;
}
