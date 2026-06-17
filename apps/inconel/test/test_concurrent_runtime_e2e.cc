// Concurrent runtime e2e harness for the Inconel production facade.
//
// Phase 1 / Gap A intent: drive rt::write_batch, rt::point_get,
// rt::seal_once, and rt::flush_once under a real multi-core rt::run
// runtime. Writers own disjoint key partitions, so each key's final
// state is determined by exactly one writer's acknowledged submission
// order. Readers run during the stress window but do not make strong
// value assertions until writers and readers are quiesced.
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
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/coord/sender.hh"
#include "apps/inconel/core/batch_carrier.hh"
#include "apps/inconel/core/data_area_heads.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/layout_plan.hh"
#include "apps/inconel/format/superblock_builder.hh"
#include "apps/inconel/nvme/runtime_scheduler.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/runtime/facade.hh"
#include "apps/inconel/runtime/run.hh"
#include "pump/core/context.hh"
#include "pump/core/lock_free_queue.hh"
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

// Four advance cores are enough to exercise cross-core SPSC routing without
// making the destructive real-NVMe gate unnecessarily long: core 0 hosts
// coord / WAL-space / value, cores 2 and 4 host read domains, and core 6
// hosts tree owner. Front schedulers are present on all four cores.
const std::vector<uint32_t> kAdvanceCores = {0, 2, 4, 6};
const std::vector<uint32_t> kReadDomainCores = {2, 4};
constexpr int32_t kValueCore = 0;
constexpr int32_t kOwnerCore = 6;

// Caller-only core IDs. These are not runtime advance cores; they give each
// external submitter a private per_core::queue source slot.
constexpr uint32_t kWriterCoreBase = 10;
constexpr uint32_t kReaderCoreBase = 20;
constexpr uint32_t kMaintenanceCore = 30;
constexpr uint32_t kVerifyCore = 40;
constexpr uint32_t kBurstSubmitCore = 50;
constexpr uint32_t kDurableSamplerCore = 51;

constexpr uint32_t kWriterCount = 3;
constexpr uint32_t kReaderCount = 2;
constexpr uint32_t kKeysPerWriter = 768;
constexpr uint32_t kOpsPerBatch = 8;
constexpr uint32_t kBatchesPerWriter = 160;
constexpr uint32_t kBurstBatchesPerWriter = 64;
constexpr uint32_t kMaintenanceRounds = 14;
constexpr uint32_t kVerifyWindow = 64;
constexpr uint32_t kLbaSize = 4096;
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
    std::snprintf(buf, sizeof(buf), "ckv-w%02u-key-%05u",
                  writer_id, key_idx);
    return buf;
}

std::string
make_value(uint32_t writer_id, uint32_t key_idx, uint64_t seq) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "ckv-value-w%02u-k%05u-s%06lu",
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

struct writer_result {
    std::vector<expected_cell> expected;
    uint64_t batches = 0;
    uint64_t puts = 0;
    uint64_t tombstones = 0;
    uint64_t highest_batch_lsn = 0;
};

struct reader_counters {
    uint64_t reads = 0;
    uint64_t found = 0;
    uint64_t not_found = 0;
};

struct maintenance_counters {
    uint64_t seal_rounds = 0;
    uint64_t flush_rounds = 0;
    uint64_t non_noop_flushes = 0;
};

struct write_burst_counters {
    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> acked{0};
    std::atomic<uint64_t> max_inflight{0};

    void update_max(uint64_t depth) {
        uint64_t observed = max_inflight.load(std::memory_order_relaxed);
        while (observed < depth &&
               !max_inflight.compare_exchange_weak(
                   observed,
                   depth,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {}
    }

    void note_submitted() {
        const uint64_t now_submitted =
            submitted.fetch_add(1, std::memory_order_acq_rel) + 1;
        const uint64_t now_acked = acked.load(std::memory_order_acquire);
        update_max(now_submitted - now_acked);
    }

    void note_acked() {
        const uint64_t now_acked =
            acked.fetch_add(1, std::memory_order_acq_rel) + 1;
        const uint64_t now_submitted =
            submitted.load(std::memory_order_acquire);
        update_max(now_submitted >= now_acked ? now_submitted - now_acked : 0);
    }
};

struct durable_sample_stats {
    uint64_t samples = 0;
    uint64_t first_lsn = 0;
    uint64_t last_lsn = 0;
};

struct durable_sampler_control {
    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> samples{0};
};

struct burst_result {
    uint64_t batches = 0;
    uint64_t puts = 0;
    uint64_t tombstones = 0;
    uint64_t highest_batch_lsn = 0;
    uint64_t max_inflight = 0;
    durable_sample_stats durable_samples;
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

submission<write_path::write_batch_result>
submit_counted_write(core::client_batch_buffer input,
                     write_burst_counters* counters) {
    auto ctx = pump::core::make_root_context();
    auto promise =
        std::make_shared<std::promise<op_result<write_path::write_batch_result>>>();
    auto fut = promise->get_future();
    auto caught = std::make_shared<std::exception_ptr>();

    counters->note_submitted();
    try {
        rt::write_batch(std::move(input))
            >> pump::sender::any_exception([caught](std::exception_ptr ep) {
                *caught = std::move(ep);
                return pump::sender::just(write_path::write_batch_result{});
            })
            >> pump::sender::then([promise, caught, counters](auto&& value) {
                counters->note_acked();
                if (*caught) {
                    promise->set_value(*caught);
                } else {
                    promise->set_value(write_path::write_batch_result(
                        std::forward<decltype(value)>(value)));
                }
            })
            >> pump::sender::submit(ctx);
    } catch (...) {
        counters->note_acked();
        promise->set_value(std::current_exception());
    }

    return submission<write_path::write_batch_result>{
        .ctx = std::move(ctx),
        .fut = std::move(fut),
    };
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
    control->samples.store(1, std::memory_order_release);
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
        control->samples.fetch_add(1, std::memory_order_release);
        if (control->stop.load(std::memory_order_acquire)) {
            break;
        }
    }
}

std::vector<std::size_t>
build_drain_permutation(std::size_t count) {
    std::vector<std::size_t> order;
    order.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        order.push_back((i * 37) % count);
    }
    return order;
}

burst_result
run_multi_batch_burst(std::vector<writer_result>& writers) {
    pump::core::this_core_id = kBurstSubmitCore;

    constexpr std::size_t kBurstBatchCount =
        kWriterCount * kBurstBatchesPerWriter;
    write_burst_counters counters;
    durable_sampler_control sampler_control;
    durable_sample_stats sample_stats;
    std::jthread sampler([&]() {
        durable_sampler_main(&sampler_control, &sample_stats);
    });
    while (!sampler_control.ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::vector<uint32_t> submission_writer;
    std::vector<std::vector<planned_op>> planned_batches;
    std::vector<submission<write_path::write_batch_result>> submissions;
    submission_writer.reserve(kBurstBatchCount);
    planned_batches.reserve(kBurstBatchCount);
    submissions.reserve(kBurstBatchCount);

    for (uint32_t batch = 0; batch < kBurstBatchesPerWriter; ++batch) {
        for (uint32_t writer = 0; writer < kWriterCount; ++writer) {
            const uint64_t first_seq =
                (static_cast<uint64_t>(kBatchesPerWriter) + batch) *
                kOpsPerBatch;
            auto planned = build_writer_batch(writer, first_seq);
            auto input = encode_planned_batch(planned);
            submission_writer.push_back(writer);
            planned_batches.push_back(std::move(planned));
            submissions.push_back(
                submit_counted_write(std::move(input), &counters));
        }
    }

    auto drain_order = build_drain_permutation(submissions.size());
    std::vector<write_path::write_batch_result> acks(submissions.size());
    for (std::size_t idx : drain_order) {
        auto ack = expect_ok<write_path::write_batch_result>(
            submissions[idx].fut.get(), "burst write_batch");
        CHECK(ack.entry_count == planned_batches[idx].size());
        CHECK(ack.batch_lsn > 0);
        acks[idx] = ack;
    }

    sampler_control.stop.store(true, std::memory_order_release);
    sampler.join();

    burst_result result;
    result.batches = submissions.size();
    result.max_inflight =
        counters.max_inflight.load(std::memory_order_acquire);
    result.durable_samples = sample_stats;

    CHECK(counters.submitted.load(std::memory_order_acquire) == result.batches);
    CHECK(counters.acked.load(std::memory_order_acquire) == result.batches);
    CHECK(result.max_inflight > 1);
    CHECK(result.durable_samples.samples >= 2);

    std::vector<std::size_t> apply_order;
    apply_order.reserve(planned_batches.size());
    for (std::size_t i = 0; i < planned_batches.size(); ++i) {
        apply_order.push_back(i);
    }
    std::sort(apply_order.begin(), apply_order.end(), [&](auto lhs, auto rhs) {
        return acks[lhs].batch_lsn < acks[rhs].batch_lsn;
    });

    for (std::size_t i : apply_order) {
        const uint32_t writer = submission_writer[i];
        result.highest_batch_lsn =
            std::max(result.highest_batch_lsn, acks[i].batch_lsn);
        writers[writer].highest_batch_lsn =
            std::max(writers[writer].highest_batch_lsn, acks[i].batch_lsn);
        ++writers[writer].batches;
        for (const auto& op : planned_batches[i]) {
            apply_to_expected(op, writers[writer].expected[op.key_idx]);
            if (op.raw.op == core::write_op_type::put) {
                ++writers[writer].puts;
                ++result.puts;
            } else {
                ++writers[writer].tombstones;
                ++result.tombstones;
            }
        }
    }

    return result;
}

struct alloc_snapshot {
    uint64_t tree_head_lba = 0;
    uint64_t value_head_lba = 0;
};

alloc_snapshot
capture_alloc_snapshot() {
    alloc_snapshot s{};
    auto* heads = core::registry::data_area_heads_ptr.get();
    CHECK(heads != nullptr);
    s.tree_head_lba = heads->tree_head_lba.load(std::memory_order_relaxed);
    s.value_head_lba = heads->value_head_lba.load(std::memory_order_relaxed);
    return s;
}

std::size_t
current_imm_count() {
    std::size_t out = 0;
    const uint32_t fronts = core::registry::front_count();
    for (uint32_t owner = 0; owner < fronts; ++owner) {
        out += core::registry::front_at(owner)->imms_for_testing().size();
    }
    return out;
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

class concurrent_runtime_fixture {
  public:
    concurrent_runtime_fixture(std::string pci_addr,
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
              .spdk_name = "inconel_concurrent_runtime_e2e",
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
            .coord_ready_window = 131072,
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

    ~concurrent_runtime_fixture() {
        stop_workers();
        if (rt_ != nullptr) {
            runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt_);
            rt_ = nullptr;
        }
    }

    concurrent_runtime_fixture(const concurrent_runtime_fixture&) = delete;
    concurrent_runtime_fixture& operator=(const concurrent_runtime_fixture&) =
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

void
writer_main(uint32_t writer_id,
            std::promise<writer_result>* out,
            std::atomic<uint64_t>* write_batches_done) {
    pump::core::this_core_id = kWriterCoreBase + writer_id;

    writer_result result;
    result.expected.resize(kKeysPerWriter);

    uint64_t next_seq = 0;
    for (uint32_t batch = 0; batch < kBatchesPerWriter; ++batch) {
        auto planned = build_writer_batch(writer_id, next_seq);
        next_seq += planned.size();

        auto input = encode_planned_batch(planned);
        auto sub = submit_result<write_path::write_batch_result>(
            [input = std::move(input)]() mutable {
                return rt::write_batch(std::move(input));
            });
        auto ack = expect_ok<write_path::write_batch_result>(
            sub.fut.get(), "write_batch");
        CHECK(ack.entry_count == planned.size());
        result.highest_batch_lsn =
            std::max(result.highest_batch_lsn, ack.batch_lsn);

        for (const auto& op : planned) {
            apply_to_expected(op, result.expected[op.key_idx]);
            if (op.raw.op == core::write_op_type::put) {
                ++result.puts;
            } else {
                ++result.tombstones;
            }
        }
        ++result.batches;
        write_batches_done->fetch_add(1, std::memory_order_release);

        if ((batch % 8) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    out->set_value(std::move(result));
}

void
reader_main(uint32_t reader_id,
            std::atomic<bool>* stop,
            reader_counters* counters) {
    pump::core::this_core_id = kReaderCoreBase + reader_id;
    uint64_t state = 0xC0FFEE1234000000ULL ^ reader_id;
    reader_counters local{};

    auto next_rand = [&]() {
        state = state * 2862933555777941757ULL + 3037000493ULL;
        return state;
    };

    while (!stop->load(std::memory_order_acquire)) {
        const uint32_t writer_id =
            static_cast<uint32_t>(next_rand() % kWriterCount);
        const uint32_t key_idx =
            static_cast<uint32_t>((next_rand() >> 11) % kKeysPerWriter);
        std::string key = make_key(writer_id, key_idx);

        auto sub = submit_result<pipeline::point_get_result>(
            [&key]() {
                return rt::point_get(key);
            });
        auto got = expect_ok<pipeline::point_get_result>(
            sub.fut.get(), "reader point_get");
        ++local.reads;
        if (got.found) {
            CHECK(!got.value.empty());
            ++local.found;
        } else {
            ++local.not_found;
        }

        if ((local.reads % 32) == 0) {
            std::this_thread::yield();
        }
    }

    *counters = local;
}

void
maintenance_main(std::atomic<uint64_t>* write_batches_done,
                 maintenance_counters* counters) {
    pump::core::this_core_id = kMaintenanceCore;

    while (write_batches_done->load(std::memory_order_acquire) <
           kWriterCount * 4) {
        std::this_thread::yield();
    }

    maintenance_counters local{};
    for (uint32_t round = 0; round < kMaintenanceRounds; ++round) {
        auto seal = submit_result<pipeline::seal_round_result>(
            []() { return rt::seal_once(); });
        auto seal_result = expect_ok<pipeline::seal_round_result>(
            seal.fut.get(), "seal_once");
        CHECK(seal_result.cat1 != nullptr);
        ++local.seal_rounds;

        auto flush = submit_result<pipeline::flush_round_result>(
            []() { return rt::flush_once(); });
        auto flush_result = expect_ok<pipeline::flush_round_result>(
            flush.fut.get(), "flush_once");
        ++local.flush_rounds;
        if (!flush_result.noop) {
            ++local.non_noop_flushes;
        }

        core::registry::wal_reclaim_frontier_singleton()->publish_exact_min(
            core::wal_reclaim_frontier::no_unreclaimed_lsn);
        CHECK(rt::value_reclaim_stats().partial_into_untracked == 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    *counters = local;
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

void
expect_front_distribution() {
    const uint32_t front_count = core::registry::front_count();
    CHECK(front_count == kAdvanceCores.size());
    std::vector<uint32_t> hits(front_count, 0);
    for (uint32_t writer = 0; writer < kWriterCount; ++writer) {
        for (uint32_t key_idx = 0; key_idx < kKeysPerWriter; ++key_idx) {
            const auto key = make_key(writer, key_idx);
            ++hits[core::key_hash(key) % front_count];
        }
    }
    for (uint32_t owner = 0; owner < front_count; ++owner) {
        CHECK(hits[owner] > 0);
    }
}

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
run_concurrent_runtime_e2e(const harness_options& opts) {
    concurrent_runtime_fixture fx(
        opts.pci_addr, opts.spdk_core_mask, opts.qpair_depth);
    (void)fx.runtime();
    expect_front_distribution();

    const auto alloc_before = capture_alloc_snapshot();
    const auto reclaim_before = rt::value_reclaim_stats();

    std::atomic<uint64_t> write_batches_done{0};
    std::atomic<bool> reader_stop{false};

    std::vector<std::promise<writer_result>> writer_promises(kWriterCount);
    std::vector<std::future<writer_result>> writer_futures;
    writer_futures.reserve(kWriterCount);
    for (auto& p : writer_promises) {
        writer_futures.push_back(p.get_future());
    }

    std::vector<std::jthread> writers;
    writers.reserve(kWriterCount);
    for (uint32_t i = 0; i < kWriterCount; ++i) {
        writers.emplace_back([i, &writer_promises, &write_batches_done]() {
            writer_main(i, &writer_promises[i], &write_batches_done);
        });
    }

    std::vector<reader_counters> reader_stats(kReaderCount);
    std::vector<std::jthread> readers;
    readers.reserve(kReaderCount);
    for (uint32_t i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([i, &reader_stop, &reader_stats]() {
            reader_main(i, &reader_stop, &reader_stats[i]);
        });
    }

    maintenance_counters maintenance_stats{};
    std::jthread maintenance([&]() {
        maintenance_main(&write_batches_done, &maintenance_stats);
    });

    std::vector<writer_result> writer_results;
    writer_results.reserve(kWriterCount);
    for (auto& fut : writer_futures) {
        writer_results.push_back(fut.get());
    }
    writers.clear();
    maintenance.join();

    reader_stop.store(true, std::memory_order_release);
    readers.clear();

    auto burst_stats = run_multi_batch_burst(writer_results);

    final_maintenance_round();
    wait_for_quiesced_reclaim();
    const uint64_t final_durable_lsn =
        sample_durable_lsn(kVerifyCore, "final durable_lsn");
    verify_all_keys(writer_results);

    // Join advance loops before one-shot owner/front snapshots below.
    fx.stop_workers();

    const auto alloc_after = capture_alloc_snapshot();
    const auto reclaim_after = rt::value_reclaim_stats();
    const std::size_t final_imm_count = current_imm_count();
    const bool final_reclaim_idle = reclaim_idle();
    const uint64_t total_batches = [&]() {
        uint64_t total = 0;
        for (const auto& w : writer_results) total += w.batches;
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
    const uint64_t highest_batch_lsn = [&]() {
        uint64_t highest = 0;
        for (const auto& w : writer_results) {
            highest = std::max(highest, w.highest_batch_lsn);
        }
        return highest;
    }();
    const uint64_t tree_delta =
        alloc_after.tree_head_lba >= alloc_before.tree_head_lba
            ? alloc_after.tree_head_lba - alloc_before.tree_head_lba
            : 0;
    const uint64_t value_delta =
        alloc_before.value_head_lba >= alloc_after.value_head_lba
            ? alloc_before.value_head_lba - alloc_after.value_head_lba
            : 0;

    CHECK(maintenance_stats.seal_rounds == kMaintenanceRounds);
    CHECK(maintenance_stats.flush_rounds == kMaintenanceRounds);
    CHECK(maintenance_stats.non_noop_flushes > 0);
    CHECK(write_batches_done.load(std::memory_order_acquire) ==
          kWriterCount * kBatchesPerWriter);
    CHECK(final_imm_count <= core::registry::front_count());
    CHECK(reclaim_after.partial_into_untracked == 0);
    CHECK(final_reclaim_idle);
    CHECK(alloc_after.tree_head_lba >= alloc_before.tree_head_lba);
    CHECK(alloc_before.value_head_lba >= alloc_after.value_head_lba);
    CHECK(final_durable_lsn == highest_batch_lsn);
    CHECK(burst_stats.highest_batch_lsn == highest_batch_lsn);
    CHECK(tree_delta <= 4096);
    CHECK(value_delta <= total_puts + 1024);
    CHECK(reclaim_after.reclaim_total_refs >= reclaim_before.reclaim_total_refs);

    uint64_t total_reads = 0;
    uint64_t total_found = 0;
    uint64_t total_not_found = 0;
    for (const auto& r : reader_stats) {
        total_reads += r.reads;
        total_found += r.found;
        total_not_found += r.not_found;
    }
    CHECK(total_reads > 0);

    std::printf("  writers: batches=%lu puts=%lu tombstones=%lu\n",
                static_cast<unsigned long>(total_batches),
                static_cast<unsigned long>(total_puts),
                static_cast<unsigned long>(total_tombstones));
    std::printf("  burst: batches=%lu max_inflight=%lu samples=%lu "
                "durable_lsn=%lu->%lu highest_lsn=%lu\n",
                static_cast<unsigned long>(burst_stats.batches),
                static_cast<unsigned long>(burst_stats.max_inflight),
                static_cast<unsigned long>(
                    burst_stats.durable_samples.samples),
                static_cast<unsigned long>(
                    burst_stats.durable_samples.first_lsn),
                static_cast<unsigned long>(
                    burst_stats.durable_samples.last_lsn),
                static_cast<unsigned long>(burst_stats.highest_batch_lsn));
    std::printf("  readers: reads=%lu found=%lu not_found=%lu\n",
                static_cast<unsigned long>(total_reads),
                static_cast<unsigned long>(total_found),
                static_cast<unsigned long>(total_not_found));
    std::printf("  maintenance: seals=%lu flushes=%lu non_noop=%lu "
                "imms=%zu\n",
                static_cast<unsigned long>(maintenance_stats.seal_rounds),
                static_cast<unsigned long>(maintenance_stats.flush_rounds),
                static_cast<unsigned long>(
                    maintenance_stats.non_noop_flushes),
                final_imm_count);
    std::printf("  bounded: tree_head_delta=%lu value_head_delta=%lu "
                "value_reclaims=%lu partial_into_untracked=%lu\n",
                static_cast<unsigned long>(tree_delta),
                static_cast<unsigned long>(value_delta),
                static_cast<unsigned long>(reclaim_after.reclaim_total_refs),
                static_cast<unsigned long>(
                    reclaim_after.partial_into_untracked));
}

}  // namespace

int
main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    auto opts = parse_argv(argc, argv);
    std::printf("test_concurrent_runtime_e2e: pci=%s "
                "advance=[0,2,4,6] read_domains=[2,4] "
                "writers=%u readers=%u keys=%u maintenance_rounds=%u\n",
                opts.pci_addr.c_str(),
                kWriterCount,
                kReaderCount,
                kWriterCount * kKeysPerWriter,
                kMaintenanceRounds);
    run_concurrent_runtime_e2e(opts);
    std::printf("all passed\n");
    return 0;
}
