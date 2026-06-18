// Write backpressure e2e harness for the Inconel production facade.
//
// Phase 3 / Gap A / A3 intent: run the real runtime with a deliberately
// tiny coord ready window so concurrent writers overflow the ready set and
// force pending assign backpressure. A hard-cap overflow is timing-dependent;
// when it appears, the failed batch is retried and is not applied to the
// oracle until a later successful ack.
//
// Requires an SPDK-bound scratch NVMe controller. The target namespace is
// force-formatted on every run.

#include "apps/inconel/runtime/operations.hh"
#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/coord/sender.hh"
#include "apps/inconel/core/batch_carrier.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/layout_plan.hh"
#include "apps/inconel/format/superblock_builder.hh"
#include "apps/inconel/nvme/runtime_scheduler.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/runtime/facade.hh"
#include "apps/inconel/runtime/run.hh"
#include "pump/core/context.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

using namespace apps::inconel;

namespace {

using tree_cache_t = core::segmented_clock_cache;
using value_cache_t = core::segmented_clock_cache;
using runtime_t = runtime::inconel_runtime_t<tree_cache_t, value_cache_t>;
using root_context_t = decltype(pump::core::make_root_context());

const std::vector<uint32_t> kAdvanceCores = {0, 2, 4, 6};
const std::vector<uint32_t> kReadDomainCores = {2, 4};
constexpr int32_t kValueCore = 0;
constexpr int32_t kOwnerCore = 6;

constexpr uint32_t kWriterCoreBase = 10;
constexpr uint32_t kMaintenanceCore = 30;
constexpr uint32_t kVerifyCore = 40;
constexpr uint32_t kDurableSamplerCore = 41;

constexpr uint32_t kWriterCount = 4;
constexpr uint32_t kKeysPerWriter = 512;
constexpr uint32_t kOpsPerBatch = 8;
constexpr uint32_t kBatchesPerWriter = 96;
constexpr uint32_t kInflightPerWriter = 64;
constexpr uint32_t kMaxOverflowRetriesPerBatch = 20000;
constexpr uint32_t kVerifyWindow = 64;
constexpr uint32_t kLbaSize = 4096;
constexpr char kOverflowMessage[] = "coord assign backpressure overflow";
constexpr uint64_t kNamespaceBytes =
    static_cast<uint64_t>(
        format::kBootstrapFormatProfile.value_data_area_end.lba) *
    format::kBootstrapFormatProfile.lba_size;

template <typename T>
using op_result = std::variant<T, std::exception_ptr>;

template <typename T>
struct submission {
    root_context_t ctx;
    std::future<op_result<T>> fut;
};

template <typename T, typename SenderBuilder>
submission<T>
submit_result(SenderBuilder&& build_sender) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<op_result<T>>>();
    auto fut = promise->get_future();
    auto caught = std::make_shared<std::exception_ptr>();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::any_exception([caught](std::exception_ptr ep) {
            *caught = std::move(ep);
            return pump::sender::just(T{});
        })
        >> pump::sender::then([promise, caught](auto&& value) mutable {
            if (*caught) {
                promise->set_value(*caught);
            } else {
                promise->set_value(T(std::forward<decltype(value)>(value)));
            }
        })
        >> pump::sender::submit(ctx);

    return submission<T>{.ctx = std::move(ctx), .fut = std::move(fut)};
}

submission<write_path::write_batch_result>
submit_write_attempt(core::client_batch_buffer input) {
    auto ctx = pump::core::make_root_context();
    auto promise =
        std::make_shared<std::promise<op_result<write_path::write_batch_result>>>();
    auto fut = promise->get_future();
    auto caught = std::make_shared<std::exception_ptr>();

    try {
        rt::write_batch(std::move(input))
            >> pump::sender::any_exception([caught](std::exception_ptr ep) {
                *caught = std::move(ep);
                return pump::sender::just(write_path::write_batch_result{});
            })
            >> pump::sender::then([promise, caught](auto&& value) mutable {
                if (*caught) {
                    promise->set_value(*caught);
                } else {
                    promise->set_value(write_path::write_batch_result(
                        std::forward<decltype(value)>(value)));
                }
            })
            >> pump::sender::submit(ctx);
    } catch (...) {
        promise->set_value(std::current_exception());
    }

    return submission<write_path::write_batch_result>{
        .ctx = std::move(ctx),
        .fut = std::move(fut),
    };
}

template <typename T>
T
expect_ok(op_result<T>&& result, const char* label) {
    if (std::holds_alternative<std::exception_ptr>(result)) {
        try {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s failed: %s\n", label, e.what());
        } catch (...) {
            std::fprintf(stderr, "%s failed: non-std exception\n", label);
        }
        CHECK(false);
    }
    return std::move(std::get<T>(result));
}

bool
is_coord_overflow(const std::exception_ptr& ep) {
    if (!ep) return false;
    try {
        std::rethrow_exception(ep);
    } catch (const std::runtime_error& e) {
        return std::strcmp(e.what(), kOverflowMessage) == 0;
    } catch (...) {
        return false;
    }
}

[[noreturn]] void
fail_unexpected_exception(const std::exception_ptr& ep, const char* label) {
    try {
        std::rethrow_exception(ep);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s failed unexpectedly: %s\n", label, e.what());
    } catch (...) {
        std::fprintf(stderr, "%s failed unexpectedly: non-std exception\n",
                     label);
    }
    CHECK(false);
    std::abort();
}

template <typename T, typename BuildAt>
std::vector<submission<T>>
submit_many_results(std::size_t count, BuildAt&& build_at) {
    std::vector<submission<T>> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(submit_result<T>([&, i]() {
            return std::forward<BuildAt>(build_at)(i);
        }));
    }
    return out;
}

template <typename T>
std::vector<T>
await_all_ok(std::vector<submission<T>>& submissions, const char* label) {
    std::vector<T> out;
    out.reserve(submissions.size());
    for (auto& sub : submissions) {
        out.push_back(expect_ok<T>(sub.fut.get(), label));
    }
    return out;
}

format::layout_plan
bootstrap_layout_plan() {
    const auto& p = format::kBootstrapFormatProfile;
    format::layout_plan plan{};
    plan.lba_size = p.lba_size;
    plan.namespace_size = kNamespaceBytes;
    plan.total_lbas = kNamespaceBytes / p.lba_size;
    plan.wal_base_paddr = p.wal_base_paddr;
    plan.wal_segment_size = p.wal_segment_size;
    plan.wal_segment_count = p.wal_segment_count;
    plan.wal_segment_lbas = p.wal_segment_size / p.lba_size;
    plan.data_area_base_paddr = p.value_data_area_base;
    plan.data_area_end_paddr = p.value_data_area_end;
    plan.tree_page_size = p.tree_page_size;
    plan.shadow_slots_per_range = p.shadow_slots_per_range;
    plan.value_class_count = p.value_class_count;
    for (uint8_t i = 0; i < p.value_class_count; ++i) {
        plan.value_class_sizes[i] = p.value_class_sizes[i];
    }
    plan.value_space_quantum_bytes = p.value_space_quantum_bytes;
    plan.value_space_group_size_lbas = p.value_space_group_size_lbas;
    return plan;
}

template <typename SenderBuilder>
bool
submit_nvme_and_wait(nvme::runtime_scheduler& sched,
                     SenderBuilder&& build_sender) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<bool>>();
    auto fut = promise->get_future();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::then([promise](bool ok) {
            promise->set_value(ok);
        })
        >> pump::sender::submit(ctx);

    while (fut.wait_for(std::chrono::milliseconds(0)) !=
           std::future_status::ready) {
        sched.advance();
        std::this_thread::yield();
    }
    return fut.get();
}

void
write_superblock_page(nvme::runtime_scheduler& sched,
                      uint64_t lba,
                      const format::superblock& sb) {
    std::vector<char> page(kLbaSize, 0);
    std::memcpy(page.data(), &sb, sizeof(sb));
    const bool ok = submit_nvme_and_wait(sched, [&]() {
        return sched.write(lba, page.data(), 1, nvme::IO_FLAGS_FUA);
    });
    CHECK(ok && "real NVMe superblock write failed");
}

void
format_real_superblocks(nvme::runtime_device& device) {
    const auto layout = bootstrap_layout_plan();
    const auto sb_a = format::build_superblock(layout, 1);
    const auto sb_b = format::build_superblock(layout, 0);

    pump::core::this_core_id = kAdvanceCores.front();
    nvme::runtime_scheduler fmt_sched(
        device.qpair_for_core(kAdvanceCores.front()),
        layout.lba_size,
        /*pool_pages=*/64,
        /*queue_depth=*/128,
        /*local_depth=*/32,
        /*alignment=*/4096,
        SPDK_ENV_NUMA_ID_ANY,
        device.device_id());

    write_superblock_page(fmt_sched, 0, sb_a);
    write_superblock_page(fmt_sched, 1, sb_b);

    const bool flushed = submit_nvme_and_wait(fmt_sched, [&]() {
        return fmt_sched.flush();
    });
    CHECK(flushed && "real NVMe flush after bootstrap format failed");
}

std::string
make_key(uint32_t writer_id, uint32_t key_idx) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "wbp-w%02u-key-%05u",
                  writer_id, key_idx);
    return buf;
}

std::string
make_value(uint32_t writer_id, uint32_t key_idx, uint64_t seq) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "wbp-value-w%02u-k%05u-s%06lu",
                  writer_id,
                  key_idx,
                  static_cast<unsigned long>(seq));
    return buf;
}

core::raw_batch_op
put_op(std::string key, std::string value) {
    return core::raw_batch_op{
        .op = core::write_op_type::put,
        .key = std::move(key),
        .value = std::move(value),
    };
}

core::raw_batch_op
del_op(std::string key) {
    return core::raw_batch_op{
        .op = core::write_op_type::del,
        .key = std::move(key),
        .value = "",
    };
}

struct expected_cell {
    enum class kind : uint8_t { never_written, value, tombstone };
    kind tag = kind::never_written;
    std::string value;
};

struct planned_op {
    uint32_t key_idx = 0;
    uint64_t seq = 0;
    core::raw_batch_op raw;
};

struct planned_batch {
    std::vector<planned_op> ops;
    uint32_t overflow_retries = 0;
    uint64_t batch_lsn = 0;
};

struct writer_result {
    std::vector<expected_cell> expected;
    uint64_t batches = 0;
    uint64_t attempts = 0;
    uint64_t overflow_attempts = 0;
    uint64_t puts = 0;
    uint64_t tombstones = 0;
    uint64_t highest_batch_lsn = 0;
    uint64_t max_inflight = 0;
};

struct durable_sample_stats {
    uint64_t samples = 0;
    uint64_t first_lsn = 0;
    uint64_t last_lsn = 0;
};

struct durable_sampler_control {
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};
};

std::vector<planned_op>
build_writer_batch(uint32_t writer_id, uint64_t first_seq) {
    std::vector<planned_op> ops;
    ops.reserve(kOpsPerBatch);
    for (uint32_t i = 0; i < kOpsPerBatch; ++i) {
        const uint64_t seq = first_seq + i;
        const uint32_t key_idx =
            static_cast<uint32_t>((seq * 17 + writer_id * 31) %
                                  kKeysPerWriter);
        const bool tombstone = (seq % 7) == 3;
        std::string key = make_key(writer_id, key_idx);
        core::raw_batch_op raw = tombstone
            ? del_op(std::move(key))
            : put_op(std::move(key), make_value(writer_id, key_idx, seq));
        ops.push_back(planned_op{
            .key_idx = key_idx,
            .seq = seq,
            .raw = std::move(raw),
        });
    }
    return ops;
}

core::client_batch_buffer
encode_planned_batch(const std::vector<planned_op>& planned) {
    std::vector<core::raw_batch_op> raw;
    raw.reserve(planned.size());
    for (const auto& op : planned) {
        raw.push_back(core::raw_batch_op{
            .op = op.raw.op,
            .key = op.raw.key,
            .value = op.raw.value,
        });
    }

    return core::encode_client_batch(
        std::span<const core::raw_batch_op>(raw.data(), raw.size()));
}

void
apply_to_expected(const planned_op& op, expected_cell& cell) {
    if (op.raw.op == core::write_op_type::put) {
        cell.tag = expected_cell::kind::value;
        cell.value = op.raw.value;
    } else {
        cell.tag = expected_cell::kind::tombstone;
        cell.value.clear();
    }
}

uint64_t
sample_durable_lsn(uint32_t caller_core, const char* label) {
    pump::core::this_core_id = caller_core;
    auto sub = submit_result<core::read_handle>([]() {
        return coord::acquire_read_handle(*core::registry::coord_sched_singleton());
    });
    auto handle = expect_ok<core::read_handle>(sub.fut.get(), label);
    return handle.read_lsn;
}

void
durable_sampler_main(durable_sampler_control* control,
                     durable_sample_stats* stats) {
    uint64_t last = sample_durable_lsn(
        kDurableSamplerCore, "durable_lsn sample");
    stats->first_lsn = last;
    stats->last_lsn = last;
    stats->samples = 1;
    control->ready.store(true, std::memory_order_release);

    while (true) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        const uint64_t current = sample_durable_lsn(
            kDurableSamplerCore, "durable_lsn sample");
        if (current < last) {
            std::fprintf(stderr,
                         "durable_lsn regressed: before=%lu after=%lu\n",
                         static_cast<unsigned long>(last),
                         static_cast<unsigned long>(current));
            CHECK(false);
        }
        last = current;
        stats->last_lsn = current;
        stats->samples++;
        if (control->stop.load(std::memory_order_acquire)) {
            break;
        }
    }
}

void
backoff_after_overflow(uint32_t retry_count) {
    if (retry_count < 16) {
        std::this_thread::yield();
        return;
    }
    const auto micros = std::min<uint32_t>(retry_count * 10, 1000);
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
}

struct active_attempt {
    uint32_t batch_idx = 0;
    submission<write_path::write_batch_result> sub;
};

void
writer_main(uint32_t writer_id,
            std::promise<writer_result>* out,
            std::atomic<uint64_t>* global_overflows) {
    pump::core::this_core_id = kWriterCoreBase + writer_id;

    std::vector<planned_batch> batches;
    batches.reserve(kBatchesPerWriter);
    for (uint32_t batch = 0; batch < kBatchesPerWriter; ++batch) {
        batches.push_back(planned_batch{
            .ops = build_writer_batch(
                writer_id,
                static_cast<uint64_t>(batch) * kOpsPerBatch),
        });
    }

    std::deque<uint32_t> ready;
    for (uint32_t i = 0; i < kBatchesPerWriter; ++i) {
        ready.push_back(i);
    }

    writer_result result;
    result.expected.resize(kKeysPerWriter);
    std::vector<std::unique_ptr<active_attempt>> active;
    active.reserve(kInflightPerWriter);

    while (!ready.empty() || !active.empty()) {
        while (!ready.empty() && active.size() < kInflightPerWriter) {
            const uint32_t batch_idx = ready.front();
            ready.pop_front();
            auto input = encode_planned_batch(batches[batch_idx].ops);
            active.push_back(std::make_unique<active_attempt>(
                active_attempt{.batch_idx = batch_idx,
                               .sub = submit_write_attempt(std::move(input))}));
            ++result.attempts;
            result.max_inflight =
                std::max<uint64_t>(result.max_inflight, active.size());
        }

        bool progressed = false;
        for (std::size_t i = 0; i < active.size();) {
            auto& attempt = *active[i];
            if (attempt.sub.fut.wait_for(std::chrono::milliseconds(0)) !=
                std::future_status::ready) {
                ++i;
                continue;
            }

            progressed = true;
            auto outcome = attempt.sub.fut.get();
            const uint32_t batch_idx = attempt.batch_idx;
            active[i] = std::move(active.back());
            active.pop_back();

            if (std::holds_alternative<std::exception_ptr>(outcome)) {
                const auto& ep = std::get<std::exception_ptr>(outcome);
                if (!is_coord_overflow(ep)) {
                    fail_unexpected_exception(ep, "write_batch");
                }
                auto& planned = batches[batch_idx];
                ++planned.overflow_retries;
                ++result.overflow_attempts;
                global_overflows->fetch_add(1, std::memory_order_acq_rel);
                if (planned.overflow_retries > kMaxOverflowRetriesPerBatch) {
                    std::fprintf(stderr,
                                 "writer %u batch %u exceeded overflow retry "
                                 "limit %u\n",
                                 writer_id,
                                 batch_idx,
                                 kMaxOverflowRetriesPerBatch);
                    CHECK(false);
                }
                backoff_after_overflow(planned.overflow_retries);
                ready.push_back(batch_idx);
                continue;
            }

            auto ack =
                std::move(std::get<write_path::write_batch_result>(outcome));
            CHECK(ack.entry_count == batches[batch_idx].ops.size());
            CHECK(ack.batch_lsn > 0);
            CHECK(batches[batch_idx].batch_lsn == 0);
            batches[batch_idx].batch_lsn = ack.batch_lsn;
        }

        if (!progressed) {
            std::this_thread::yield();
        }
    }

    std::vector<uint32_t> apply_order;
    apply_order.reserve(batches.size());
    for (uint32_t i = 0; i < batches.size(); ++i) {
        CHECK(batches[i].batch_lsn > 0);
        apply_order.push_back(i);
    }
    std::sort(apply_order.begin(), apply_order.end(), [&](auto lhs, auto rhs) {
        return batches[lhs].batch_lsn < batches[rhs].batch_lsn;
    });

    for (uint32_t batch_idx : apply_order) {
        const auto& batch = batches[batch_idx];
        result.highest_batch_lsn =
            std::max(result.highest_batch_lsn, batch.batch_lsn);
        ++result.batches;
        for (const auto& op : batch.ops) {
            apply_to_expected(op, result.expected[op.key_idx]);
            if (op.raw.op == core::write_op_type::put) {
                ++result.puts;
            } else {
                ++result.tombstones;
            }
        }
    }

    CHECK(result.batches == kBatchesPerWriter);
    CHECK(result.max_inflight >= kInflightPerWriter);
    out->set_value(std::move(result));
}

bool
reclaim_idle() {
    const auto* owner = rt::owner();
    return owner->state.reclaim_q.empty() &&
           owner->state.pending_reclaim.empty() &&
           !owner->state.active_reclaim.has_value() &&
           owner->state.reclaim_invalidate_done_q.empty() &&
           owner->state.reclaim_trim_done_q.empty() &&
           !owner->state.reclaim_gate_requested;
}

void
final_maintenance_round() {
    pump::core::this_core_id = kMaintenanceCore;
    auto seal = submit_result<pipeline::seal_round_result>(
        []() { return rt::seal_once(); });
    auto seal_result = expect_ok<pipeline::seal_round_result>(
        seal.fut.get(), "final seal_once");
    CHECK(seal_result.cat1 != nullptr);

    auto flush = submit_result<pipeline::flush_round_result>(
        []() { return rt::flush_once(); });
    (void)expect_ok<pipeline::flush_round_result>(
        flush.fut.get(), "final flush_once");

    core::registry::wal_reclaim_frontier_singleton()->publish_exact_min(
        core::wal_reclaim_frontier::no_unreclaimed_lsn);
}

void
wait_for_quiesced_reclaim() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (reclaim_idle() &&
            rt::value_reclaim_stats().partial_into_untracked == 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(reclaim_idle());
    CHECK(rt::value_reclaim_stats().partial_into_untracked == 0);
}

void
expect_point_get_matches(const pipeline::point_get_result& got,
                         const expected_cell& expected,
                         std::string_view key) {
    switch (expected.tag) {
    case expected_cell::kind::never_written:
    case expected_cell::kind::tombstone:
        if (got.found || !got.value.empty()) {
            std::fprintf(stderr,
                         "expected not_found for %.*s, got found=%d len=%zu\n",
                         static_cast<int>(key.size()),
                         key.data(),
                         got.found ? 1 : 0,
                         got.value.size());
            CHECK(false);
        }
        break;
    case expected_cell::kind::value:
        if (!got.found || got.value != expected.value) {
            std::fprintf(stderr,
                         "value mismatch for %.*s: found=%d got='%s' "
                         "expected='%s'\n",
                         static_cast<int>(key.size()),
                         key.data(),
                         got.found ? 1 : 0,
                         got.value.c_str(),
                         expected.value.c_str());
            CHECK(false);
        }
        break;
    }
}

void
verify_all_keys(const std::vector<writer_result>& writers) {
    pump::core::this_core_id = kVerifyCore;

    std::vector<std::pair<uint32_t, uint32_t>> keys;
    keys.reserve(kWriterCount * kKeysPerWriter);
    for (uint32_t writer = 0; writer < kWriterCount; ++writer) {
        for (uint32_t key_idx = 0; key_idx < kKeysPerWriter; ++key_idx) {
            keys.push_back({writer, key_idx});
        }
    }

    for (std::size_t base = 0; base < keys.size(); base += kVerifyWindow) {
        const std::size_t count =
            std::min<std::size_t>(kVerifyWindow, keys.size() - base);
        std::vector<std::string> key_storage;
        key_storage.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto [writer, key_idx] = keys[base + i];
            key_storage.push_back(make_key(writer, key_idx));
        }

        auto subs = submit_many_results<pipeline::point_get_result>(
            count, [&](std::size_t i) {
                return rt::point_get(key_storage[i]);
            });
        auto results = await_all_ok<pipeline::point_get_result>(
            subs, "verify point_get");

        for (std::size_t i = 0; i < count; ++i) {
            const auto [writer, key_idx] = keys[base + i];
            expect_point_get_matches(
                results[i],
                writers[writer].expected[key_idx],
                key_storage[i]);
        }
    }
}

class write_backpressure_fixture {
  public:
    write_backpressure_fixture(std::string pci_addr,
                               std::string spdk_core_mask,
                               uint32_t qpair_depth)
        : pci_addr_(std::move(pci_addr))
        , spdk_core_mask_(std::move(spdk_core_mask))
        , device_(nvme::real_device_options{
              .pci_addr = pci_addr_.c_str(),
              .cores = kAdvanceCores,
              .spdk_core_mask = spdk_core_mask_.empty()
                  ? nullptr
                  : spdk_core_mask_.c_str(),
              .spdk_name = "inconel_write_backpressure_e2e",
              .init_spdk_env = true,
              .qpair_depth = qpair_depth,
              .device_id = 0,
          }) {
        CHECK(device_.size_bytes() >= kNamespaceBytes);
        format_real_superblocks(device_);

        runtime::build_options bopts{
            .cores = kAdvanceCores,
            .device = &device_,
            .read_domain_cores = kReadDomainCores,
            .value_core = kValueCore,
            .owner_core = kOwnerCore,
            .front_queue_depth = 4096,
            .coord_queue_depth = 4096,
            .coord_ready_window = 8,
            .front_wal_config = {.max_fua_inflight = 16,
                                 .max_pages_per_plan = 16,
                                 .pending_prepare_capacity = 256,
                                 .max_participants_per_group = 16},
            .tree_cache_capacity = 128,
            .value_cache_capacity = 128,
            .tree_queue_depth = 4096,
            .value_queue_depth = 4096,
            .value_io_policy = {.max_write_inflight = 64,
                                .max_read_inflight = 64,
                                .max_trim_inflight = 32},
            .nvme_queue_depth = 2048,
            .nvme_local_depth = 128,
            .nvme_dma_pool_pages_per_core = 4096,
        };
        rt_ = runtime::build_runtime<tree_cache_t, value_cache_t>(bopts);
        start_workers();
    }

    ~write_backpressure_fixture() {
        stop_workers();
        if (rt_ != nullptr) {
            runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt_);
            rt_ = nullptr;
        }
    }

    write_backpressure_fixture(const write_backpressure_fixture&) = delete;
    write_backpressure_fixture& operator=(const write_backpressure_fixture&) =
        delete;

    [[nodiscard]] runtime_t* runtime() const noexcept {
        return rt_;
    }

    void stop_workers() {
        if (rt_ == nullptr) return;
        for (uint32_t core : kAdvanceCores) {
            rt_->is_running_by_core[core].store(false);
        }
        workers_.clear();
    }

  private:
    void start_workers() {
        std::atomic<uint32_t> cores_started{0};
        workers_.reserve(kAdvanceCores.size());
        for (uint32_t core : kAdvanceCores) {
            workers_.emplace_back([this, core, &cores_started]() {
                rt::run(rt_, core, [&cores_started](auto*, uint32_t) {
                    cores_started.fetch_add(1, std::memory_order_release);
                });
            });
        }
        while (cores_started.load(std::memory_order_acquire) <
               kAdvanceCores.size()) {
            std::this_thread::yield();
        }
    }

    std::string pci_addr_;
    std::string spdk_core_mask_;
    nvme::runtime_device device_;
    runtime_t* rt_ = nullptr;
    std::vector<std::jthread> workers_;
};

struct harness_options {
    std::string pci_addr;
    std::string spdk_core_mask;
    uint32_t qpair_depth = 256;
};

void
print_usage(const char* argv0) {
    std::printf(
        "usage: %s --pci-addr BDF [--spdk-core-mask MASK] "
        "[--qpair-depth D]\n"
        "  --pci-addr BDF         PCI BDF of the SPDK-bound scratch NVMe\n"
        "                         controller, or INCONEL_NVME_PCI_ADDR\n"
        "  --spdk-core-mask MASK  optional SPDK env core mask override\n"
        "  --qpair-depth D        NVMe qpair depth (default 256)\n",
        argv0);
}

long
parse_long_arg(const char* name, const char* arg, long lo, long hi) {
    char* endp = nullptr;
    long v = std::strtol(arg, &endp, 10);
    if (!endp || *endp != '\0' || v < lo || v > hi) {
        std::fprintf(stderr, "%s must be an integer in [%ld, %ld]\n",
                     name, lo, hi);
        std::exit(2);
    }
    return v;
}

harness_options
parse_argv(int argc, char** argv) {
    harness_options opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        auto want_arg = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                print_usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "--pci-addr") {
            opts.pci_addr = want_arg("--pci-addr");
        } else if (a == "--spdk-core-mask") {
            opts.spdk_core_mask = want_arg("--spdk-core-mask");
        } else if (a == "--qpair-depth") {
            opts.qpair_depth = static_cast<uint32_t>(parse_long_arg(
                "--qpair-depth", want_arg("--qpair-depth"), 1, 65535));
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown arg: %.*s\n",
                         static_cast<int>(a.size()),
                         a.data());
            print_usage(argv[0]);
            std::exit(2);
        }
    }

    if (opts.pci_addr.empty()) {
        if (const char* env = std::getenv("INCONEL_NVME_PCI_ADDR")) {
            opts.pci_addr = env;
        }
    }
    if (opts.pci_addr.empty()) {
        std::fprintf(stderr,
                     "--pci-addr or INCONEL_NVME_PCI_ADDR is required\n");
        print_usage(argv[0]);
        std::exit(2);
    }
    return opts;
}

void
run_write_backpressure_e2e(const harness_options& opts) {
    write_backpressure_fixture fx(
        opts.pci_addr, opts.spdk_core_mask, opts.qpair_depth);
    (void)fx.runtime();

    durable_sampler_control sampler_control;
    durable_sample_stats sample_stats;
    std::jthread sampler([&]() {
        durable_sampler_main(&sampler_control, &sample_stats);
    });
    while (!sampler_control.ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::atomic<uint64_t> overflow_counter{0};
    std::vector<std::promise<writer_result>> writer_promises(kWriterCount);
    std::vector<std::future<writer_result>> writer_futures;
    writer_futures.reserve(kWriterCount);
    for (auto& p : writer_promises) {
        writer_futures.push_back(p.get_future());
    }

    std::vector<std::jthread> writers;
    writers.reserve(kWriterCount);
    for (uint32_t i = 0; i < kWriterCount; ++i) {
        writers.emplace_back([i, &writer_promises, &overflow_counter]() {
            writer_main(i, &writer_promises[i], &overflow_counter);
        });
    }

    std::vector<writer_result> writer_results;
    writer_results.reserve(kWriterCount);
    for (auto& fut : writer_futures) {
        writer_results.push_back(fut.get());
    }
    writers.clear();

    sampler_control.stop.store(true, std::memory_order_release);
    sampler.join();

    final_maintenance_round();
    wait_for_quiesced_reclaim();
    const uint64_t final_durable_lsn =
        sample_durable_lsn(kVerifyCore, "final durable_lsn");
    verify_all_keys(writer_results);

    const uint64_t highest_batch_lsn = [&]() {
        uint64_t highest = 0;
        for (const auto& w : writer_results) {
            highest = std::max(highest, w.highest_batch_lsn);
        }
        return highest;
    }();
    const uint64_t total_batches = [&]() {
        uint64_t total = 0;
        for (const auto& w : writer_results) total += w.batches;
        return total;
    }();
    const uint64_t total_attempts = [&]() {
        uint64_t total = 0;
        for (const auto& w : writer_results) total += w.attempts;
        return total;
    }();
    const uint64_t total_overflows = [&]() {
        uint64_t total = 0;
        for (const auto& w : writer_results) total += w.overflow_attempts;
        return total;
    }();
    const uint64_t total_puts = [&]() {
        uint64_t total = 0;
        for (const auto& w : writer_results) total += w.puts;
        return total;
    }();
    const uint64_t total_tombstones = [&]() {
        uint64_t total = 0;
        for (const auto& w : writer_results) total += w.tombstones;
        return total;
    }();
    const uint64_t max_writer_inflight = [&]() {
        uint64_t max_seen = 0;
        for (const auto& w : writer_results) {
            max_seen = std::max(max_seen, w.max_inflight);
        }
        return max_seen;
    }();

    CHECK(total_batches == kWriterCount * kBatchesPerWriter);
    CHECK(total_attempts >= total_batches);
    CHECK(total_overflows ==
          overflow_counter.load(std::memory_order_acquire));
    CHECK(max_writer_inflight >= kInflightPerWriter);
    CHECK(sample_stats.samples >= 2);
    CHECK(final_durable_lsn == highest_batch_lsn);
    CHECK(rt::value_reclaim_stats().partial_into_untracked == 0);

    std::printf("  writers: batches=%lu attempts=%lu puts=%lu "
                "tombstones=%lu max_writer_inflight=%lu\n",
                static_cast<unsigned long>(total_batches),
                static_cast<unsigned long>(total_attempts),
                static_cast<unsigned long>(total_puts),
                static_cast<unsigned long>(total_tombstones),
                static_cast<unsigned long>(max_writer_inflight));
    std::printf("  durable_lsn samples=%lu first=%lu last=%lu final=%lu "
                "highest_acked=%lu\n",
                static_cast<unsigned long>(sample_stats.samples),
                static_cast<unsigned long>(sample_stats.first_lsn),
                static_cast<unsigned long>(sample_stats.last_lsn),
                static_cast<unsigned long>(final_durable_lsn),
                static_cast<unsigned long>(highest_batch_lsn));

    if (total_overflows > 0) {
        std::printf("A3 overflow probe: triggered counter=%lu\n",
                    static_cast<unsigned long>(total_overflows));
    } else {
        std::printf("A3 overflow probe: not triggered "
                    "(timing-dependent, no fault injection)\n");
    }
}

}  // namespace

int
main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    auto opts = parse_argv(argc, argv);
    std::printf("test_write_backpressure_e2e: pci=%s "
                "advance=[0,2,4,6] read_domains=[2,4] "
                "writers=%u keys=%u coord_ready_window=8 "
                "inflight_per_writer=%u\n",
                opts.pci_addr.c_str(),
                kWriterCount,
                kWriterCount * kKeysPerWriter,
                kInflightPerWriter);
    run_write_backpressure_e2e(opts);
    std::printf("all passed\n");
    return 0;
}
