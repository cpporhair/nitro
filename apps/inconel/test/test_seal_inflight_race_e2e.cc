// Seal-vs-in-flight-write e2e harness for the Inconel production facade.
//
// Phase 4 / Gap A / A5 intent: keep many write_batch submissions in flight
// while a maintenance thread aggressively seals with no sleep between seal
// attempts. This exercises the "batch does not cross seal" invariant and the
// publish_gate handoff under real rt::run scheduling on an SPDK NVMe device.
//
// Tier-2 gate-closed observation is intentionally skipped: this e2e only uses
// the existing production sender/API surface, and direct gate_open_for_testing()
// reads from a non-coord thread would not be race-free.
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
constexpr uint32_t kDurableSamplerCore = 51;

constexpr uint32_t kWriterCount = 4;
constexpr uint32_t kKeysPerWriter = 1024;
constexpr uint32_t kOpsPerBatch = 8;
constexpr uint32_t kBatchesPerWriter = 512;
constexpr uint32_t kInflightDepthPerWriter = 48;
constexpr uint32_t kVerifyWindow = 64;
constexpr uint32_t kLbaSize = 4096;
constexpr uint64_t kTotalBatches =
    static_cast<uint64_t>(kWriterCount) * kBatchesPerWriter;

// K=8 is intentionally well above "more than one seal" while still modest
// enough for slow real-NVMe device gates. The test additionally requires these
// seals to start before all write batches have acked.
constexpr uint64_t kMinRacingSealRounds = 8;
constexpr uint64_t kFlushEverySealRounds = 4;

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
    std::snprintf(buf, sizeof(buf), "sir-w%02u-key-%05u",
                  writer_id, key_idx);
    return buf;
}

std::string
make_value(uint32_t writer_id, uint32_t key_idx, uint64_t seq) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "sir-value-w%02u-k%05u-s%06lu",
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

struct acked_batch_record {
    uint32_t writer_id = 0;
    uint64_t batch_lsn = 0;
    std::vector<planned_op> ops;
};

struct writer_result {
    std::vector<acked_batch_record> acked_batches;
    uint64_t batches = 0;
    uint64_t puts = 0;
    uint64_t tombstones = 0;
    uint64_t highest_batch_lsn = 0;
};

struct maintenance_counters {
    uint64_t seal_rounds = 0;
    uint64_t seals_started_before_all_acked = 0;
    uint64_t flush_rounds = 0;
    uint64_t non_noop_flushes = 0;
    uint64_t non_noop_reclaims = 0;
    uint64_t submitted_before_first_seal = 0;
    uint64_t acked_before_first_seal = 0;
    uint64_t acked_after_last_seal = 0;
};

struct write_storm_counters {
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
                     write_storm_counters* counters) {
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

bool
reclaim_idle() {
    const auto* owner = rt::owner();
    return owner->state.reclaim_q.empty() &&
           owner->state.pending_reclaim.empty() &&
           !owner->state.active_reclaim.has_value();
}

tree::reclaim_round_result
run_reclaim(const char* label) {
    pump::core::this_core_id = kMaintenanceCore;
    auto reclaim = submit_result<tree::reclaim_round_result>(
        []() { return rt::reclaim_once(); });
    return expect_ok<tree::reclaim_round_result>(reclaim.fut.get(), label);
}

class seal_inflight_race_fixture {
  public:
    seal_inflight_race_fixture(std::string pci_addr,
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
              .spdk_name = "inconel_seal_inflight_race_e2e",
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
            .maintenance = {.enabled = false},
            .nvme_queue_depth = 2048,
            .nvme_local_depth = 128,
            .nvme_dma_pool_pages_per_core = 4096,
        };
        rt_ = runtime::build_runtime<tree_cache_t, value_cache_t>(bopts);
        start_workers();
    }

    ~seal_inflight_race_fixture() {
        stop_workers();
        if (rt_ != nullptr) {
            runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt_);
            rt_ = nullptr;
        }
    }

    seal_inflight_race_fixture(const seal_inflight_race_fixture&) = delete;
    seal_inflight_race_fixture& operator=(const seal_inflight_race_fixture&) =
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

struct inflight_write {
    std::vector<planned_op> planned;
    submission<write_path::write_batch_result> sub;
};

void
writer_main(uint32_t writer_id,
            std::promise<writer_result>* out,
            write_storm_counters* counters) {
    pump::core::this_core_id = kWriterCoreBase + writer_id;

    writer_result result;
    uint32_t next_batch = 0;
    std::deque<inflight_write> inflight;

    auto submit_next = [&]() {
        const uint64_t first_seq =
            static_cast<uint64_t>(next_batch) * kOpsPerBatch;
        auto planned = build_writer_batch(writer_id, first_seq);
        auto input = encode_planned_batch(planned);
        inflight.push_back(inflight_write{
            .planned = std::move(planned),
            .sub = submit_counted_write(std::move(input), counters),
        });
        ++next_batch;
    };

    while (next_batch < kBatchesPerWriter &&
           inflight.size() < kInflightDepthPerWriter) {
        submit_next();
    }

    while (!inflight.empty()) {
        inflight_write current = std::move(inflight.front());
        inflight.pop_front();

        auto ack = expect_ok<write_path::write_batch_result>(
            current.sub.fut.get(), "write_batch");
        CHECK(ack.entry_count == current.planned.size());
        CHECK(ack.batch_lsn > 0);

        result.highest_batch_lsn =
            std::max(result.highest_batch_lsn, ack.batch_lsn);
        ++result.batches;
        for (const auto& op : current.planned) {
            if (op.raw.op == core::write_op_type::put) {
                ++result.puts;
            } else {
                ++result.tombstones;
            }
        }
        result.acked_batches.push_back(acked_batch_record{
            .writer_id = writer_id,
            .batch_lsn = ack.batch_lsn,
            .ops = std::move(current.planned),
        });

        while (next_batch < kBatchesPerWriter &&
               inflight.size() < kInflightDepthPerWriter) {
            submit_next();
        }
    }

    out->set_value(std::move(result));
}

void
maintenance_main(const write_storm_counters* counters,
                 maintenance_counters* out) {
    pump::core::this_core_id = kMaintenanceCore;

    const uint64_t initial_depth =
        std::min<uint64_t>(kTotalBatches,
                           kWriterCount * kInflightDepthPerWriter);
    while (counters->submitted.load(std::memory_order_acquire) <
           initial_depth) {
        std::this_thread::yield();
    }

    maintenance_counters local{};
    local.submitted_before_first_seal =
        counters->submitted.load(std::memory_order_acquire);
    local.acked_before_first_seal =
        counters->acked.load(std::memory_order_acquire);

    while (true) {
        const bool writes_still_active =
            counters->acked.load(std::memory_order_acquire) < kTotalBatches;

        auto seal = submit_result<pipeline::seal_round_result>(
            []() { return rt::seal_once(); });
        auto seal_result = expect_ok<pipeline::seal_round_result>(
            seal.fut.get(), "seal_once");
        CHECK(seal_result.cat1 != nullptr);
        ++local.seal_rounds;
        if (writes_still_active) {
            ++local.seals_started_before_all_acked;
        }
        local.acked_after_last_seal =
            counters->acked.load(std::memory_order_acquire);

        if ((local.seal_rounds % kFlushEverySealRounds) == 0) {
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
            auto reclaim_result = run_reclaim("reclaim_once");
            if (!reclaim_result.noop) {
                ++local.non_noop_reclaims;
            }
            // partial_into_untracked is an observability counter that is
            // "≈0 at steady state" and carries NO liveness/correctness
            // guarantee (cross_doc_contracts.md §value-side reclaim). Under
            // this eviction-heavy storm it can be transiently non-zero, so do
            // not gate on it; reclaim quiescence is asserted via reclaim_idle().
        }

        if (counters->acked.load(std::memory_order_acquire) == kTotalBatches &&
            local.seal_rounds >= kMinRacingSealRounds) {
            break;
        }
    }

    *out = local;
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
    (void)run_reclaim("final reclaim_once");
}

void
wait_for_quiesced_reclaim() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto reclaim = run_reclaim("wait reclaim_once");
        if (reclaim.noop && reclaim_idle()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Quiescence is reclaim_idle(); partial_into_untracked is an observability
    // counter (≈0 steady state, no liveness guarantee) and is not gated on.
    const auto reclaim = run_reclaim("deadline reclaim_once");
    CHECK(reclaim.noop);
    CHECK(reclaim_idle());
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
verify_all_keys(const std::vector<std::vector<expected_cell>>& oracle) {
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
                oracle[writer][key_idx],
                key_storage[i]);
        }
    }
}

struct oracle_result {
    std::vector<std::vector<expected_cell>> expected;
    uint64_t highest_batch_lsn = 0;
    uint64_t total_batches = 0;
    uint64_t total_puts = 0;
    uint64_t total_tombstones = 0;
};

oracle_result
build_oracle_from_acks(std::vector<writer_result>& writers) {
    std::vector<acked_batch_record> all_acks;
    all_acks.reserve(kTotalBatches);

    oracle_result result;
    result.expected.resize(kWriterCount);
    for (auto& cells : result.expected) {
        cells.resize(kKeysPerWriter);
    }

    for (auto& writer : writers) {
        result.total_batches += writer.batches;
        result.total_puts += writer.puts;
        result.total_tombstones += writer.tombstones;
        result.highest_batch_lsn =
            std::max(result.highest_batch_lsn, writer.highest_batch_lsn);
        for (auto& ack : writer.acked_batches) {
            all_acks.push_back(std::move(ack));
        }
    }

    std::sort(all_acks.begin(), all_acks.end(), [](const auto& lhs,
                                                   const auto& rhs) {
        return lhs.batch_lsn < rhs.batch_lsn;
    });

    uint64_t last_lsn = 0;
    for (const auto& ack : all_acks) {
        CHECK(ack.batch_lsn > last_lsn);
        last_lsn = ack.batch_lsn;
        for (const auto& op : ack.ops) {
            apply_to_expected(
                op, result.expected[ack.writer_id][op.key_idx]);
        }
    }

    CHECK(all_acks.size() == kTotalBatches);
    CHECK(result.total_batches == kTotalBatches);
    CHECK(last_lsn == result.highest_batch_lsn);
    return result;
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
run_seal_inflight_race_e2e(const harness_options& opts) {
    seal_inflight_race_fixture fx(
        opts.pci_addr, opts.spdk_core_mask, opts.qpair_depth);
    (void)fx.runtime();

    write_storm_counters storm_counters;
    durable_sampler_control sampler_control;
    durable_sample_stats sample_stats;
    std::jthread sampler([&]() {
        durable_sampler_main(&sampler_control, &sample_stats);
    });
    while (!sampler_control.ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::vector<std::promise<writer_result>> writer_promises(kWriterCount);
    std::vector<std::future<writer_result>> writer_futures;
    writer_futures.reserve(kWriterCount);
    for (auto& p : writer_promises) {
        writer_futures.push_back(p.get_future());
    }

    std::vector<std::jthread> writers;
    writers.reserve(kWriterCount);
    for (uint32_t i = 0; i < kWriterCount; ++i) {
        writers.emplace_back([i, &writer_promises, &storm_counters]() {
            writer_main(i, &writer_promises[i], &storm_counters);
        });
    }

    maintenance_counters maintenance_stats{};
    std::jthread maintenance([&]() {
        maintenance_main(&storm_counters, &maintenance_stats);
    });

    std::vector<writer_result> writer_results;
    writer_results.reserve(kWriterCount);
    for (auto& fut : writer_futures) {
        writer_results.push_back(fut.get());
    }
    writers.clear();
    maintenance.join();

    final_maintenance_round();
    wait_for_quiesced_reclaim();
    const uint64_t final_durable_lsn =
        sample_durable_lsn(kVerifyCore, "final durable_lsn");

    sampler_control.stop.store(true, std::memory_order_release);
    sampler.join();

    auto oracle = build_oracle_from_acks(writer_results);
    verify_all_keys(oracle.expected);

    const uint64_t submitted =
        storm_counters.submitted.load(std::memory_order_acquire);
    const uint64_t acked =
        storm_counters.acked.load(std::memory_order_acquire);
    const uint64_t max_inflight =
        storm_counters.max_inflight.load(std::memory_order_acquire);

    CHECK(submitted == kTotalBatches);
    CHECK(acked == submitted);
    CHECK(final_durable_lsn == oracle.highest_batch_lsn);
    CHECK(sample_stats.samples >= 2);

    CHECK(maintenance_stats.seal_rounds >= kMinRacingSealRounds);
    CHECK(maintenance_stats.seals_started_before_all_acked >=
          kMinRacingSealRounds);
    CHECK(max_inflight > 1);
    CHECK(maintenance_stats.submitted_before_first_seal >
          maintenance_stats.acked_before_first_seal);
    CHECK(maintenance_stats.acked_after_last_seal >
          maintenance_stats.acked_before_first_seal);
    CHECK(maintenance_stats.non_noop_flushes >= 2);

    // Join advance loops before one-shot owner/front snapshots below.
    fx.stop_workers();
    CHECK(reclaim_idle());
    const auto reclaim_snapshot = rt::value_reclaim_stats();

    std::printf("  write storm: writers=%u batches=%lu ops_per_batch=%u "
                "depth_per_writer=%u submitted=%lu acked=%lu "
                "max_inflight=%lu highest_lsn=%lu\n",
                kWriterCount,
                static_cast<unsigned long>(kTotalBatches),
                kOpsPerBatch,
                kInflightDepthPerWriter,
                static_cast<unsigned long>(submitted),
                static_cast<unsigned long>(acked),
                static_cast<unsigned long>(max_inflight),
                static_cast<unsigned long>(oracle.highest_batch_lsn));
    std::printf("  oracle: puts=%lu tombstones=%lu keys=%u\n",
                static_cast<unsigned long>(oracle.total_puts),
                static_cast<unsigned long>(oracle.total_tombstones),
                kWriterCount * kKeysPerWriter);
    std::printf("  maintenance: seals=%lu racing_seals=%lu flushes=%lu "
                "non_noop=%lu reclaims=%lu K=%lu flush_every=%lu\n",
                static_cast<unsigned long>(maintenance_stats.seal_rounds),
                static_cast<unsigned long>(
                    maintenance_stats.seals_started_before_all_acked),
                static_cast<unsigned long>(maintenance_stats.flush_rounds),
                static_cast<unsigned long>(
                    maintenance_stats.non_noop_flushes),
                static_cast<unsigned long>(
                    maintenance_stats.non_noop_reclaims),
                static_cast<unsigned long>(kMinRacingSealRounds),
                static_cast<unsigned long>(kFlushEverySealRounds));
    std::printf("  overlap: submitted_before_first_seal=%lu "
                "acked_before_first_seal=%lu acked_after_last_seal=%lu\n",
                static_cast<unsigned long>(
                    maintenance_stats.submitted_before_first_seal),
                static_cast<unsigned long>(
                    maintenance_stats.acked_before_first_seal),
                static_cast<unsigned long>(
                    maintenance_stats.acked_after_last_seal));
    std::printf("  durable_lsn: samples=%lu first=%lu last=%lu final=%lu\n",
                static_cast<unsigned long>(sample_stats.samples),
                static_cast<unsigned long>(sample_stats.first_lsn),
                static_cast<unsigned long>(sample_stats.last_lsn),
                static_cast<unsigned long>(final_durable_lsn));
    std::printf("  reclaim (observability, ≈0 steady state, no liveness "
                "guarantee): total_refs=%lu partial_into_untracked=%lu\n",
                static_cast<unsigned long>(reclaim_snapshot.reclaim_total_refs),
                static_cast<unsigned long>(
                    reclaim_snapshot.partial_into_untracked));
    std::printf("  tier2_gate_closed_observation: skipped "
                "(no race-free coord-context observation via allowed API)\n");
}

}  // namespace

int
main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    auto opts = parse_argv(argc, argv);
    std::printf("test_seal_inflight_race_e2e: pci=%s "
                "advance=[0,2,4,6] read_domains=[2,4] "
                "writers=%u keys=%u batches_per_writer=%u "
                "depth_per_writer=%u min_racing_seals=%lu\n",
                opts.pci_addr.c_str(),
                kWriterCount,
                kWriterCount * kKeysPerWriter,
                kBatchesPerWriter,
                kInflightDepthPerWriter,
                static_cast<unsigned long>(kMinRacingSealRounds));
    run_seal_inflight_race_e2e(opts);
    std::printf("all passed\n");
    return 0;
}
