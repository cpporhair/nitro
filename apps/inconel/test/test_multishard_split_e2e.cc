// Multi-shard split/repartition e2e for the Inconel production facade.
//
// Phase 8 / Gap A / A8 intent: force real leaf splits across multiple
// flush rounds, then verify cross-round reads through the rebuilt
// shard_partition_map. Requires an SPDK-bound scratch NVMe controller.
// The target namespace is force-formatted on every run.

#include "apps/inconel/runtime/operations.hh"
#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/layout_plan.hh"
#include "apps/inconel/format/superblock_builder.hh"
#include "apps/inconel/format/tree_page.hh"
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

// Keep the same physical cores as the committed real-NVMe e2e fixtures,
// but host a tree_read_domain on each of them so the public partition map
// can grow from the bootstrap placeholder to four routed shards.
const std::vector<uint32_t> kAdvanceCores = {0, 2, 4, 6};
const std::vector<uint32_t> kReadDomainCores = {0, 2, 4, 6};
constexpr int32_t kValueCore = 0;
constexpr int32_t kOwnerCore = 6;

constexpr uint32_t kSubmitCore = 10;
constexpr uint32_t kMaintenanceCore = 30;
constexpr uint32_t kVerifyCore = 40;

constexpr uint32_t kWriterCount = 4;
constexpr uint32_t kRounds = 4;
constexpr uint32_t kNewKeysPerWriterPerRound = 2048;
constexpr uint32_t kKeysPerWriter = kRounds * kNewKeysPerWriterPerRound;
constexpr uint32_t kOpsPerBatch = 32;
constexpr uint32_t kVerifyWindow = 128;
constexpr uint32_t kApproxKeyBytes = 32;
constexpr uint32_t kLeafValueRecordBytes =
    sizeof(format::leaf_record_header) + kApproxKeyBytes +
    sizeof(format::value_ref);
constexpr uint32_t kLeafValueRecordBudgetBytes =
    kLeafValueRecordBytes + sizeof(uint16_t);
constexpr uint32_t kLeafRecordCapacity =
    (format::kBootstrapFormatProfile.tree_page_size -
     sizeof(format::tree_slot_header)) /
    kLeafValueRecordBudgetBytes;
constexpr uint32_t kTotalKeys = kWriterCount * kKeysPerWriter;
constexpr uint32_t kLbaSize = 4096;
constexpr uint64_t kNamespaceBytes =
    static_cast<uint64_t>(
        format::kBootstrapFormatProfile.value_data_area_end.lba) *
    format::kBootstrapFormatProfile.lba_size;

static_assert(kLeafRecordCapacity == 64,
              "A8 sizing expects the current 4 KiB / 32-byte-key leaf "
              "capacity documented by production format headers");
static_assert(kTotalKeys > kLeafRecordCapacity * 100,
              "A8 key count must force many leaf splits");

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
    std::snprintf(buf, sizeof(buf),
                  "a8-w%02u-key-%08u-splitpadding",
                  writer_id,
                  key_idx);
    std::string out(buf);
    CHECK(out.size() == kApproxKeyBytes);
    return out;
}

std::string
make_value(uint32_t writer_id, uint32_t key_idx, uint64_t seq) {
    std::string out;
    out.reserve(96);
    char prefix[96];
    const int n = std::snprintf(prefix, sizeof(prefix),
                                "a8-value-w%02u-k%08u-s%010lu:",
                                writer_id,
                                key_idx,
                                static_cast<unsigned long>(seq));
    const std::size_t prefix_len =
        static_cast<std::size_t>(std::max(n, 0));
    for (std::size_t i = 0; i < 96; ++i) {
        if (i < prefix_len && i < sizeof(prefix) - 1) {
            out.push_back(prefix[i]);
        } else {
            out.push_back(static_cast<char>(
                'A' + ((writer_id * 19 + key_idx * 7 + seq + i) % 26)));
        }
    }
    return out;
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
    uint32_t writer_id = 0;
    uint32_t key_idx = 0;
    core::raw_batch_op raw;
};

struct oracle {
    std::vector<std::vector<expected_cell>> cells;
    uint64_t highest_batch_lsn = 0;
    uint64_t batches = 0;
    uint64_t puts = 0;
    uint64_t tombstones = 0;
    uint32_t written_keys_per_writer = 0;

    oracle() : cells(kWriterCount) {
        for (auto& per_writer : cells) {
            per_writer.resize(kKeysPerWriter);
        }
    }
};

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
apply_to_oracle(const planned_op& op, oracle& o) {
    auto& cell = o.cells[op.writer_id][op.key_idx];
    if (op.raw.op == core::write_op_type::put) {
        cell.tag = expected_cell::kind::value;
        cell.value = op.raw.value;
        ++o.puts;
    } else {
        cell.tag = expected_cell::kind::tombstone;
        cell.value.clear();
        ++o.tombstones;
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

struct lsn_sampler {
    uint64_t last = 0;
    uint64_t samples = 0;

    void sample(uint32_t core, const char* label) {
        const uint64_t current = sample_durable_lsn(core, label);
        if (samples != 0 && current < last) {
            std::fprintf(stderr,
                         "durable_lsn regressed: before=%lu after=%lu\n",
                         static_cast<unsigned long>(last),
                         static_cast<unsigned long>(current));
            CHECK(false);
        }
        last = current;
        ++samples;
    }
};

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
wait_for_quiesced_reclaim() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (reclaim_idle()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(reclaim_idle());
}

struct partition_snapshot {
    std::size_t partition_count = 0;
    uint32_t shard_count = 0;
};

partition_snapshot
capture_partition_snapshot(const char* label) {
    auto map = core::registry::current_shard_partitions();
    if (!map) {
        std::fprintf(stderr, "%s: missing shard_partition_map\n", label);
        CHECK(false);
    }
    return partition_snapshot{
        .partition_count = map->shards.size(),
        .shard_count = map->shard_count(),
    };
}

struct round_partition_snapshot {
    partition_snapshot before;
    partition_snapshot after;
    bool non_noop = false;
};

round_partition_snapshot
maintenance_round(uint32_t round, lsn_sampler& sampler) {
    pump::core::this_core_id = kMaintenanceCore;

    round_partition_snapshot snap{};
    snap.before = capture_partition_snapshot("pre-flush partition map");

    auto seal = submit_result<pipeline::seal_round_result>(
        []() { return rt::seal_once(); });
    auto seal_result = expect_ok<pipeline::seal_round_result>(
        seal.fut.get(), "seal_once");
    CHECK(seal_result.cat1 != nullptr);

    auto flush = submit_result<pipeline::flush_round_result>(
        []() { return rt::flush_once(); });
    auto flush_result = expect_ok<pipeline::flush_round_result>(
        flush.fut.get(), "flush_once");
    snap.non_noop = !flush_result.noop;
    CHECK(snap.non_noop);

    core::registry::wal_reclaim_frontier_singleton()->publish_exact_min(
        core::wal_reclaim_frontier::no_unreclaimed_lsn);
    wait_for_quiesced_reclaim();
    sampler.sample(kMaintenanceCore, "post-maintenance durable_lsn");

    snap.after = capture_partition_snapshot("post-flush partition map");
    std::printf("  round %u partition_map: before=%zu/%u after=%zu/%u "
                "non_noop=%d\n",
                round,
                snap.before.partition_count,
                snap.before.shard_count,
                snap.after.partition_count,
                snap.after.shard_count,
                snap.non_noop ? 1 : 0);
    return snap;
}

std::vector<planned_op>
build_round_workload(uint32_t round) {
    CHECK(round >= 1 && round <= kRounds);
    std::vector<planned_op> out;
    const uint32_t first_new = (round - 1) * kNewKeysPerWriterPerRound;
    const uint32_t seq_round = round * 1000000;
    out.reserve(kWriterCount * (kNewKeysPerWriterPerRound + 320));

    for (uint32_t writer = 0; writer < kWriterCount; ++writer) {
        for (uint32_t i = 0; i < kNewKeysPerWriterPerRound; ++i) {
            const uint32_t key_idx = first_new + i;
            const uint64_t seq =
                seq_round + writer * 100000 + static_cast<uint64_t>(i);
            out.push_back(planned_op{
                .writer_id = writer,
                .key_idx = key_idx,
                .raw = put_op(make_key(writer, key_idx),
                              make_value(writer, key_idx, seq)),
            });
        }

        if (round == 1) continue;

        const uint32_t existing_limit = first_new;
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t key_idx =
                (i * 97 + writer * 131 + round * 17) % existing_limit;
            const uint64_t seq =
                seq_round + 500000 + writer * 100000 +
                static_cast<uint64_t>(i);
            out.push_back(planned_op{
                .writer_id = writer,
                .key_idx = key_idx,
                .raw = put_op(make_key(writer, key_idx),
                              make_value(writer, key_idx, seq)),
            });
        }

        if (round >= 3) {
            for (uint32_t i = 0; i < 64; ++i) {
                const uint32_t key_idx =
                    (i * 193 + writer * 43 + round * 29) % existing_limit;
                out.push_back(planned_op{
                    .writer_id = writer,
                    .key_idx = key_idx,
                    .raw = del_op(make_key(writer, key_idx)),
                });
            }
        }
    }

    return out;
}

void
submit_in_chunks(const std::vector<planned_op>& ops,
                 oracle& o,
                 lsn_sampler& sampler,
                 const char* label) {
    pump::core::this_core_id = kSubmitCore;
    for (std::size_t base = 0; base < ops.size(); base += kOpsPerBatch) {
        const std::size_t count =
            std::min<std::size_t>(kOpsPerBatch, ops.size() - base);
        std::vector<planned_op> chunk;
        chunk.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            chunk.push_back(ops[base + i]);
        }

        auto input = encode_planned_batch(chunk);
        auto sub = submit_result<write_path::write_batch_result>(
            [input = std::move(input)]() mutable {
                return rt::write_batch(std::move(input));
            });
        auto ack = expect_ok<write_path::write_batch_result>(
            sub.fut.get(), label);
        CHECK(ack.entry_count == chunk.size());
        CHECK(ack.batch_lsn > 0);
        o.highest_batch_lsn = std::max(o.highest_batch_lsn, ack.batch_lsn);
        ++o.batches;
        for (const auto& op : chunk) {
            apply_to_oracle(op, o);
        }
        sampler.sample(kSubmitCore, "post-write durable_lsn");
    }
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
                         "value mismatch for %.*s: found=%d got_len=%zu "
                         "expected_len=%zu\n",
                         static_cast<int>(key.size()),
                         key.data(),
                         got.found ? 1 : 0,
                         got.value.size(),
                         expected.value.size());
            CHECK(false);
        }
        break;
    }
}

void
verify_all_written_keys(const oracle& o, uint32_t round) {
    pump::core::this_core_id = kVerifyCore;

    std::vector<std::pair<uint32_t, uint32_t>> keys;
    keys.reserve(kWriterCount * o.written_keys_per_writer);
    for (uint32_t writer = 0; writer < kWriterCount; ++writer) {
        for (uint32_t key_idx = 0; key_idx < o.written_keys_per_writer;
             ++key_idx) {
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
                o.cells[writer][key_idx],
                key_storage[i]);
        }
    }

    std::printf("  round %u verified_keys=%zu\n", round, keys.size());
}

uint32_t
count_routed_shards_for_written_keys(const oracle& o) {
    auto map = core::registry::current_shard_partitions();
    CHECK(map != nullptr);
    std::array<bool, 16> seen{};
    uint32_t distinct = 0;
    for (uint32_t writer = 0; writer < kWriterCount; ++writer) {
        for (uint32_t key_idx = 0; key_idx < o.written_keys_per_writer;
             ++key_idx) {
            const uint32_t shard = map->route(make_key(writer, key_idx));
            CHECK(shard < seen.size());
            if (!seen[shard]) {
                seen[shard] = true;
                ++distinct;
            }
        }
    }
    return distinct;
}

class multishard_split_fixture {
  public:
    multishard_split_fixture(std::string pci_addr,
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
              .spdk_name = "inconel_multishard_split_e2e",
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
            .tree_cache_capacity = 256,
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

    ~multishard_split_fixture() {
        stop_workers();
        if (rt_ != nullptr) {
            runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt_);
            rt_ = nullptr;
        }
    }

    multishard_split_fixture(const multishard_split_fixture&) = delete;
    multishard_split_fixture& operator=(const multishard_split_fixture&) =
        delete;

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
run_multishard_split_e2e(const harness_options& opts) {
    multishard_split_fixture fx(
        opts.pci_addr, opts.spdk_core_mask, opts.qpair_depth);

    const uint32_t read_domain_count =
        core::registry::tree_read_domain_count();
    CHECK(read_domain_count >= 2);
    CHECK(read_domain_count == kReadDomainCores.size());

    oracle o;
    lsn_sampler sampler;
    sampler.sample(kVerifyCore, "initial durable_lsn");

    const auto initial_partitions =
        capture_partition_snapshot("initial partition map");
    CHECK(initial_partitions.partition_count == 1);
    CHECK(initial_partitions.shard_count == 1);

    std::vector<round_partition_snapshot> partition_rounds;
    partition_rounds.reserve(kRounds);
    uint64_t non_noop_flushes = 0;

    for (uint32_t round = 1; round <= kRounds; ++round) {
        auto workload = build_round_workload(round);
        submit_in_chunks(workload, o, sampler, "round write_batch");
        o.written_keys_per_writer = round * kNewKeysPerWriterPerRound;

        auto snap = maintenance_round(round, sampler);
        if (snap.non_noop) ++non_noop_flushes;
        partition_rounds.push_back(snap);

        verify_all_written_keys(o, round);
    }

    const uint64_t final_durable_lsn =
        sample_durable_lsn(kVerifyCore, "final durable_lsn");
    CHECK(final_durable_lsn >= sampler.last);
    CHECK(final_durable_lsn == o.highest_batch_lsn);

    const auto final_partitions =
        capture_partition_snapshot("final partition map");
    CHECK(final_partitions.partition_count > initial_partitions.partition_count);
    CHECK(final_partitions.partition_count == read_domain_count);
    CHECK(final_partitions.shard_count > 1);
    CHECK(non_noop_flushes >= 2);

    const uint32_t routed_shards = count_routed_shards_for_written_keys(o);
    CHECK(routed_shards >= 2);

    verify_all_written_keys(o, kRounds);

    fx.stop_workers();

    const uint64_t expected_leaf_pages =
        (kTotalKeys + kLeafRecordCapacity - 1) / kLeafRecordCapacity;
    std::printf("  sizing: tree_page=%u key_bytes=%u leaf_capacity=%u "
                "total_keys=%u expected_leaf_pages>=%lu rounds=%u\n",
                format::kBootstrapFormatProfile.tree_page_size,
                kApproxKeyBytes,
                kLeafRecordCapacity,
                kTotalKeys,
                static_cast<unsigned long>(expected_leaf_pages),
                kRounds);
    std::printf("  workload: batches=%lu puts=%lu tombstones=%lu "
                "highest_lsn=%lu durable_samples=%lu\n",
                static_cast<unsigned long>(o.batches),
                static_cast<unsigned long>(o.puts),
                static_cast<unsigned long>(o.tombstones),
                static_cast<unsigned long>(o.highest_batch_lsn),
                static_cast<unsigned long>(sampler.samples + 1));
    std::printf("  partitions: initial=%zu/%u final=%zu/%u "
                "routed_shards=%u non_noop_flushes=%lu\n",
                initial_partitions.partition_count,
                initial_partitions.shard_count,
                final_partitions.partition_count,
                final_partitions.shard_count,
                routed_shards,
                static_cast<unsigned long>(non_noop_flushes));
    std::printf("  A8 root-change: not separately confirmable "
                "(covered indirectly)\n");
}

}  // namespace

int
main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    auto opts = parse_argv(argc, argv);
    std::printf("test_multishard_split_e2e: pci=%s "
                "advance=[0,2,4,6] read_domains=[0,2,4,6] "
                "writers=%u rounds=%u keys=%u leaf_capacity=%u\n",
                opts.pci_addr.c_str(),
                kWriterCount,
                kRounds,
                kTotalKeys,
                kLeafRecordCapacity);
    run_multishard_split_e2e(opts);
    std::printf("all passed\n");
    return 0;
}
