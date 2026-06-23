// 066C real-NVMe read/write concurrency checker.
//
// This is a test-only interval checker for a small put-only hot-key workload.
// It uses the production runtime facade and YCSB key/value helpers, then
// verifies post-run read intervals against acknowledged write intervals.

#include "apps/inconel/runtime/operations.hh"
#include "apps/inconel/test/check.hh"
#include "apps/inconel/ycsb/workload.hh"

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/core/batch_carrier.hh"
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

constexpr uint32_t kWriterCore = 10;
constexpr uint32_t kReaderCoreBase = 20;
constexpr std::string_view kDefaultScratchBdf = "0000:04:00.0";
constexpr std::string_view kSystemDiskBdf = "0000:03:00.0";

constexpr uint32_t kReaderCount = 3;
constexpr uint64_t kHotKeyCount = 64;
constexpr uint64_t kInitialGeneration = 1;
constexpr uint32_t kWriteBatchSize = 8;
constexpr uint32_t kUpdateBatches = 512;
constexpr uint32_t kValueSize = 128;
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
            throw std::runtime_error(std::string(label) + " failed: " +
                                     e.what());
        } catch (...) {
            throw std::runtime_error(std::string(label) +
                                     " failed: non-std exception");
        }
    }
    return std::move(std::get<T>(result));
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

enum class scenario_kind : uint8_t {
    c1,
    c3,
};

struct harness_options {
    std::string pci_addr;
    std::string spdk_core_mask;
    uint32_t qpair_depth = 256;
    scenario_kind scenario = scenario_kind::c1;
};

std::string_view
scenario_name(scenario_kind scenario) noexcept {
    return scenario == scenario_kind::c1 ? "c1" : "c3";
}

std::string
allowed_scratch_bdf() {
    if (const char* env = std::getenv("INCONEL_ALLOWED_SCRATCH_BDF")) {
        return env;
    }
    return std::string(kDefaultScratchBdf);
}

ycsb::config
make_ycsb_config() {
    ycsb::config cfg;
    cfg.records = kHotKeyCount;
    cfg.value_size = kValueSize;
    cfg.seed = 0x066C2026ull;
    cfg.key_prefix = "066c-user";
    cfg.print_config = false;
    return cfg;
}

core::client_batch_buffer
encode_ops(std::vector<core::raw_batch_op>& ops) {
    return core::encode_client_batch(
        std::span<const core::raw_batch_op>(ops.data(), ops.size()));
}

struct interval_clock {
    std::atomic<uint64_t> next{1};

    uint64_t mark() {
        return next.fetch_add(1, std::memory_order_acq_rel);
    }
};

struct write_interval {
    uint64_t key_id = 0;
    uint64_t generation = 0;
    uint64_t start_seq = 0;
    uint64_t ack_seq = 0;
};

struct read_interval {
    uint32_t reader_id = 0;
    uint64_t key_id = 0;
    uint64_t start_seq = 0;
    uint64_t end_seq = 0;
    bool found = false;
    std::string value;
};

struct workload_result {
    std::vector<write_interval> writes;
    std::vector<std::vector<read_interval>> reads_by_reader;
    runtime::maintenance_stats_snapshot maintenance{};
};

runtime::maintenance_stats_snapshot
maintenance_snapshot(runtime_t* runtime) {
    runtime::maintenance_stats_snapshot out{};
    bool found = false;
    for (auto* sched :
         runtime->get_schedulers<runtime::maintenance_sched>()) {
        if (sched == nullptr) continue;
        CHECK(!found);
        out = sched->snapshot();
        found = true;
    }
    CHECK(found);
    return out;
}

class checker_fixture {
  public:
    checker_fixture(std::string pci_addr,
                    std::string spdk_core_mask,
                    uint32_t qpair_depth,
                    scenario_kind scenario)
        : pci_addr_(std::move(pci_addr))
        , spdk_core_mask_(std::move(spdk_core_mask))
        , device_(nvme::real_device_options{
              .pci_addr = pci_addr_.c_str(),
              .cores = kAdvanceCores,
              .spdk_core_mask = spdk_core_mask_.empty()
                  ? nullptr
                  : spdk_core_mask_.c_str(),
              .spdk_name = "inconel_ycsb_concurrency_checker_e2e",
              .init_spdk_env = true,
              .qpair_depth = qpair_depth,
              .device_id = 0,
          }) {
        CHECK(device_.size_bytes() >= kNamespaceBytes);
        format_real_superblocks(device_);

        runtime::maintenance_options maintenance{};
        if (scenario == scenario_kind::c1) {
            maintenance.enabled = false;
        } else {
            maintenance.enabled = true;
            maintenance.core = kOwnerCore;
            maintenance.active_gap_ticks = 1;
            maintenance.idle_initial_backoff_ticks = 2;
            maintenance.idle_max_backoff_ticks = 64;
            maintenance.completion_queue_depth = 8;
            maintenance.policy.auto_seal_flush = true;
            maintenance.policy.seal_active_memtable_bytes = 64ull * 1024ull;
            maintenance.policy.total_memtable_limit_bytes = 256ull * 1024ull;
            maintenance.policy.wal_seal_used_ratio = 0.05f;
            maintenance.policy.max_sealed_gens_per_front = 1;
        }

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
            .maintenance = maintenance,
            .nvme_queue_depth = 2048,
            .nvme_local_depth = 128,
            .nvme_dma_pool_pages_per_core = 4096,
        };
        rt_ = runtime::build_runtime<tree_cache_t, value_cache_t>(bopts);
        start_workers();
    }

    ~checker_fixture() {
        stop_workers();
        if (rt_ != nullptr) {
            runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt_);
            rt_ = nullptr;
        }
    }

    checker_fixture(const checker_fixture&) = delete;
    checker_fixture& operator=(const checker_fixture&) = delete;

    [[nodiscard]] runtime_t* runtime() const noexcept {
        return rt_;
    }

    void stop_workers() {
        if (rt_ == nullptr) return;
        for (uint32_t core : kAdvanceCores) {
            rt_->is_running_by_core[core].store(
                false, std::memory_order_release);
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

write_path::write_batch_result
submit_write_batch(std::vector<core::raw_batch_op> ops) {
    auto input = encode_ops(ops);
    auto sub = submit_result<write_path::write_batch_result>(
        [input = std::move(input)]() mutable {
            return rt::write_batch(std::move(input));
        });
    return expect_ok<write_path::write_batch_result>(
        sub.fut.get(), "write_batch");
}

void
preload_hot_keys(const ycsb::config& cfg,
                 interval_clock* clock,
                 std::vector<uint64_t>* generations,
                 std::vector<write_interval>* writes) {
    pump::core::this_core_id = kWriterCore;

    std::vector<core::raw_batch_op> ops;
    ops.reserve(kHotKeyCount);
    for (uint64_t id = 0; id < kHotKeyCount; ++id) {
        (*generations)[id] = kInitialGeneration;
        ops.push_back(ycsb::make_put(cfg, id, kInitialGeneration));
    }

    const uint64_t start = clock->mark();
    auto ack = submit_write_batch(std::move(ops));
    const uint64_t end = clock->mark();
    if (ack.entry_count != kHotKeyCount || ack.batch_lsn == 0) {
        throw std::runtime_error("preload write_batch returned bad ack");
    }

    writes->reserve(kHotKeyCount + kUpdateBatches * kWriteBatchSize);
    for (uint64_t id = 0; id < kHotKeyCount; ++id) {
        writes->push_back(write_interval{
            .key_id = id,
            .generation = kInitialGeneration,
            .start_seq = start,
            .ack_seq = end,
        });
    }
}

void
writer_main(const ycsb::config& cfg,
            interval_clock* clock,
            std::vector<uint64_t>* generations,
            std::vector<write_interval>* writes,
            bool stretch_for_maintenance) {
    pump::core::this_core_id = kWriterCore;

    try {
        for (uint32_t batch = 0; batch < kUpdateBatches; ++batch) {
            std::vector<core::raw_batch_op> ops;
            ops.reserve(kWriteBatchSize);
            struct planned_write {
                uint64_t key_id;
                uint64_t generation;
            };
            std::vector<planned_write> planned;
            planned.reserve(kWriteBatchSize);

            for (uint32_t i = 0; i < kWriteBatchSize; ++i) {
                const uint64_t ordinal =
                    static_cast<uint64_t>(batch) * kWriteBatchSize + i;
                const uint64_t key_id = ordinal % kHotKeyCount;
                const uint64_t generation = ++(*generations)[key_id];
                planned.push_back(planned_write{
                    .key_id = key_id,
                    .generation = generation,
                });
                ops.push_back(ycsb::make_put(cfg, key_id, generation));
            }

            const uint64_t start = clock->mark();
            auto ack = submit_write_batch(std::move(ops));
            const uint64_t end = clock->mark();
            if (ack.entry_count != planned.size() || ack.batch_lsn == 0) {
                throw std::runtime_error("writer write_batch returned bad ack");
            }

            for (const auto& op : planned) {
                writes->push_back(write_interval{
                    .key_id = op.key_id,
                    .generation = op.generation,
                    .start_seq = start,
                    .ack_seq = end,
                });
            }

            if (stretch_for_maintenance) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } else if ((batch % 16) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        }
    } catch (...) {
        throw;
    }
}

void
reader_main(uint32_t reader_id,
            const ycsb::config& cfg,
            interval_clock* clock,
            std::atomic<bool>* stop,
            std::vector<read_interval>* out) {
    pump::core::this_core_id = kReaderCoreBase + reader_id;
    uint64_t state = 0x066C600DCAFE0000ull ^ reader_id;

    auto next_rand = [&]() {
        state = state * 2862933555777941757ULL + 3037000493ULL;
        return state;
    };

    out->reserve(4096);
    while (!stop->load(std::memory_order_acquire)) {
        const uint64_t key_id = next_rand() % kHotKeyCount;
        auto key_holder =
            std::make_shared<std::string>(ycsb::make_key(cfg, key_id));
        const uint64_t start = clock->mark();
        auto sub = submit_result<pipeline::point_get_result>(
            [key_holder]() {
                return rt::point_get(std::string_view(*key_holder))
                    >> pump::sender::then(
                           [key_holder](pipeline::point_get_result result) {
                               (void)key_holder;
                               return result;
                           });
            });
        auto got = expect_ok<pipeline::point_get_result>(
            sub.fut.get(), "point_get");
        const uint64_t end = clock->mark();

        out->push_back(read_interval{
            .reader_id = reader_id,
            .key_id = key_id,
            .start_seq = start,
            .end_seq = end,
            .found = got.found,
            .value = std::move(got.value),
        });

        if ((out->size() % 32) == 0) {
            std::this_thread::yield();
        }
    }
}

void
wait_for_c3_maintenance(runtime_t* runtime,
                        const std::atomic<bool>* writer_done) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snap = maintenance_snapshot(runtime);
        if (snap.failed_rounds != 0) {
            throw std::runtime_error("maintenance failed during c3");
        }
        if (snap.seal_rounds > 0 &&
            snap.flush_rounds > 0 &&
            snap.non_noop_flush_rounds > 0) {
            if (writer_done->load(std::memory_order_acquire)) {
                throw std::runtime_error(
                    "c3 maintenance completed after writer finished");
            }
            return;
        }
        if (writer_done->load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "writer completed before c3 non-noop maintenance");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto snap = maintenance_snapshot(runtime);
    char buf[256];
    std::snprintf(buf,
                  sizeof(buf),
                  "timed out waiting for c3 maintenance: failed=%lu "
                  "seal=%lu flush=%lu non_noop_flush=%lu completed=%lu",
                  static_cast<unsigned long>(snap.failed_rounds),
                  static_cast<unsigned long>(snap.seal_rounds),
                  static_cast<unsigned long>(snap.flush_rounds),
                  static_cast<unsigned long>(snap.non_noop_flush_rounds),
                  static_cast<unsigned long>(snap.completed_rounds));
    throw std::runtime_error(buf);
}

bool
parse_ycsb_value(std::string_view value, uint64_t* id, uint64_t* generation) {
    if (!value.starts_with("id=")) return false;
    const std::size_t id_end = value.find(";gen=");
    if (id_end == std::string_view::npos) return false;
    const std::size_t gen_begin = id_end + 5;
    const std::size_t gen_end = value.find(";seed=", gen_begin);
    if (gen_end == std::string_view::npos) return false;

    auto parse_u64 = [](std::string_view text, uint64_t* out) {
        if (text.empty()) return false;
        uint64_t value = 0;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
            const uint64_t digit = static_cast<uint64_t>(c - '0');
            if (value > (UINT64_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
        *out = value;
        return true;
    };

    return parse_u64(value.substr(3, id_end - 3), id) &&
           parse_u64(value.substr(gen_begin, gen_end - gen_begin),
                     generation);
}

void
print_read_failure(const char* reason,
                   const read_interval& read,
                   uint64_t observed_generation,
                   uint64_t prior_generation) {
    std::fprintf(stderr,
                 "checker failure: %s key=%lu reader=%u "
                 "read=[%lu,%lu] observed_gen=%lu prior_gen=%lu\n",
                 reason,
                 static_cast<unsigned long>(read.key_id),
                 read.reader_id,
                 static_cast<unsigned long>(read.start_seq),
                 static_cast<unsigned long>(read.end_seq),
                 static_cast<unsigned long>(observed_generation),
                 static_cast<unsigned long>(prior_generation));
}

bool
intervals_overlap(const write_interval& write, const read_interval& read) {
    return write.start_seq < read.end_seq && read.start_seq < write.ack_seq;
}

bool
write_is_prior_or_overlapping(const write_interval& write,
                              const read_interval& read) {
    return write.ack_seq <= read.start_seq || intervals_overlap(write, read);
}

bool
run_interval_checker(const ycsb::config& cfg,
                     const std::vector<write_interval>& writes,
                     const std::vector<std::vector<read_interval>>& readers) {
    std::vector<std::vector<const write_interval*>> writes_by_key(kHotKeyCount);
    for (const auto& w : writes) {
        if (w.key_id >= kHotKeyCount) {
            std::fprintf(stderr, "checker failure: write key out of range\n");
            return false;
        }
        writes_by_key[w.key_id].push_back(&w);
    }
    for (auto& per_key : writes_by_key) {
        std::sort(per_key.begin(), per_key.end(), [](auto* lhs, auto* rhs) {
            if (lhs->generation != rhs->generation) {
                return lhs->generation < rhs->generation;
            }
            return lhs->ack_seq < rhs->ack_seq;
        });
    }

    uint64_t read_count = 0;
    for (const auto& reader_reads : readers) {
        std::vector<uint64_t> last_seen(kHotKeyCount, 0);
        for (const auto& read : reader_reads) {
            ++read_count;
            if (read.key_id >= kHotKeyCount) {
                std::fprintf(stderr,
                             "checker failure: read key out of range\n");
                return false;
            }

            if (!read.found) {
                print_read_failure(
                    "point_get returned not_found for put-only key",
                    read,
                    0,
                    last_seen[read.key_id]);
                return false;
            }

            uint64_t observed_id = 0;
            uint64_t observed_generation = 0;
            if (!parse_ycsb_value(
                    read.value, &observed_id, &observed_generation)) {
                print_read_failure(
                    "point_get returned unparsable YCSB value",
                    read,
                    0,
                    last_seen[read.key_id]);
                std::fprintf(stderr, "  value='%s'\n", read.value.c_str());
                return false;
            }
            if (observed_id != read.key_id ||
                read.value != ycsb::make_value(
                                  cfg, read.key_id, observed_generation)) {
                print_read_failure(
                    "point_get returned value for wrong key/generation",
                    read,
                    observed_generation,
                    last_seen[read.key_id]);
                std::fprintf(stderr, "  value='%s'\n", read.value.c_str());
                return false;
            }

            const auto& per_key_writes = writes_by_key[read.key_id];
            uint64_t latest_prior = 0;
            const write_interval* observed_write = nullptr;
            for (const auto* w : per_key_writes) {
                if (w->ack_seq <= read.start_seq) {
                    latest_prior = std::max(latest_prior, w->generation);
                }
                if (w->generation == observed_generation) {
                    observed_write = w;
                }
            }

            if (observed_generation < latest_prior) {
                print_read_failure(
                    "observed generation older than latest prior ACK",
                    read,
                    observed_generation,
                    latest_prior);
                return false;
            }

            if (observed_write == nullptr ||
                !write_is_prior_or_overlapping(*observed_write, read)) {
                print_read_failure(
                    "observed generation is not prior/overlapping ACKed write",
                    read,
                    observed_generation,
                    latest_prior);
                if (observed_write != nullptr) {
                    std::fprintf(stderr,
                                 "  write=[%lu,%lu] gen=%lu\n",
                                 static_cast<unsigned long>(
                                     observed_write->start_seq),
                                 static_cast<unsigned long>(
                                     observed_write->ack_seq),
                                 static_cast<unsigned long>(
                                     observed_write->generation));
                }
                return false;
            }

            if (observed_generation < last_seen[read.key_id]) {
                print_read_failure(
                    "per-reader/per-key generation regressed",
                    read,
                    observed_generation,
                    last_seen[read.key_id]);
                return false;
            }
            last_seen[read.key_id] = observed_generation;
        }
    }

    if (read_count == 0) {
        std::fprintf(stderr, "checker failure: no reads were recorded\n");
        return false;
    }
    return true;
}

workload_result
run_workload(checker_fixture* fx,
             const ycsb::config& cfg,
             scenario_kind scenario) {
    interval_clock clock;
    std::vector<uint64_t> generations(kHotKeyCount, 0);
    workload_result result;
    result.reads_by_reader.resize(kReaderCount);

    preload_hot_keys(cfg, &clock, &generations, &result.writes);

    std::atomic<bool> stop_readers{false};
    std::atomic<bool> writer_done{false};
    std::exception_ptr writer_error;
    std::vector<std::exception_ptr> reader_errors(kReaderCount);

    std::jthread writer([&]() {
        try {
            writer_main(cfg,
                        &clock,
                        &generations,
                        &result.writes,
                        scenario == scenario_kind::c3);
        } catch (...) {
            writer_error = std::current_exception();
        }
        writer_done.store(true, std::memory_order_release);
    });

    std::vector<std::jthread> readers;
    readers.reserve(kReaderCount);
    for (uint32_t i = 0; i < kReaderCount; ++i) {
        readers.emplace_back([&, i]() {
            try {
                reader_main(i,
                            cfg,
                            &clock,
                            &stop_readers,
                            &result.reads_by_reader[i]);
            } catch (...) {
                reader_errors[i] = std::current_exception();
                stop_readers.store(true, std::memory_order_release);
            }
        });
    }

    std::exception_ptr maintenance_error;
    if (scenario == scenario_kind::c3) {
        try {
            wait_for_c3_maintenance(fx->runtime(), &writer_done);
        } catch (...) {
            maintenance_error = std::current_exception();
        }
    }

    writer.join();
    if (writer_error) {
        std::rethrow_exception(writer_error);
    }
    if (maintenance_error) {
        std::rethrow_exception(maintenance_error);
    }
    if (scenario == scenario_kind::c3) {
        result.maintenance = maintenance_snapshot(fx->runtime());
    }

    stop_readers.store(true, std::memory_order_release);
    readers.clear();

    for (auto& ep : reader_errors) {
        if (ep) {
            std::rethrow_exception(ep);
        }
    }

    if (scenario == scenario_kind::c3) {
        if (result.maintenance.failed_rounds != 0 ||
            result.maintenance.seal_rounds == 0 ||
            result.maintenance.flush_rounds == 0 ||
            result.maintenance.non_noop_flush_rounds == 0) {
            throw std::runtime_error("c3 maintenance counter contract failed");
        }
    }

    return result;
}

void
print_usage(const char* argv0) {
    std::printf(
        "usage: %s --pci-addr BDF --scenario c1|c3 "
        "[--spdk-core-mask MASK] [--qpair-depth D]\n"
        "  --pci-addr BDF         PCI BDF of the SPDK-bound scratch NVMe\n"
        "                         controller, or INCONEL_NVME_PCI_ADDR\n"
        "  --scenario c1|c3       c1 disables maintenance; c3 enables "
        "aggressive maintenance\n"
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

scenario_kind
parse_scenario(std::string_view value) {
    if (value == "c1" || value == "1" || value == "off" ||
        value == "maintenance-off") {
        return scenario_kind::c1;
    }
    if (value == "c3" || value == "3" || value == "on" ||
        value == "maintenance-on") {
        return scenario_kind::c3;
    }
    throw std::invalid_argument("unknown --scenario (expected c1 or c3)");
}

harness_options
parse_argv(int argc, char** argv) {
    harness_options opts;
    bool scenario_set = false;
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
        } else if (a == "--scenario") {
            try {
                opts.scenario = parse_scenario(want_arg("--scenario"));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "%s\n", e.what());
                print_usage(argv[0]);
                std::exit(2);
            }
            scenario_set = true;
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
    if (opts.pci_addr.empty() || !scenario_set) {
        if (opts.pci_addr.empty()) {
            std::fprintf(stderr,
                         "--pci-addr or INCONEL_NVME_PCI_ADDR is required\n");
        }
        if (!scenario_set) {
            std::fprintf(stderr, "--scenario c1|c3 is required\n");
        }
        print_usage(argv[0]);
        std::exit(2);
    }
    if (opts.pci_addr == kSystemDiskBdf) {
        std::fprintf(stderr,
                     "refusing known system-disk BDF %.*s\n",
                     static_cast<int>(kSystemDiskBdf.size()),
                     kSystemDiskBdf.data());
        std::exit(2);
    }
    const std::string allowed = allowed_scratch_bdf();
    if (opts.pci_addr != allowed) {
        std::fprintf(stderr,
                     "refusing non-scratch BDF %s; allowed scratch BDF is %s "
                     "(set INCONEL_ALLOWED_SCRATCH_BDF only after checking "
                     "real_nvme_test_guide.md)\n",
                     opts.pci_addr.c_str(),
                     allowed.c_str());
        std::exit(2);
    }
    return opts;
}

int
run_checker(const harness_options& opts) {
    ycsb::config cfg = make_ycsb_config();
    checker_fixture fx(
        opts.pci_addr, opts.spdk_core_mask, opts.qpair_depth, opts.scenario);
    auto result = run_workload(&fx, cfg, opts.scenario);

    if (!run_interval_checker(cfg, result.writes, result.reads_by_reader)) {
        return 1;
    }

    uint64_t read_count = 0;
    for (const auto& reads : result.reads_by_reader) {
        read_count += reads.size();
    }
    std::printf("checker writes=%zu reads=%lu hot_keys=%lu\n",
                result.writes.size(),
                static_cast<unsigned long>(read_count),
                static_cast<unsigned long>(kHotKeyCount));
    if (opts.scenario == scenario_kind::c3) {
        std::printf("checker_maintenance failed=%lu seal=%lu flush=%lu "
                    "non_noop_flush=%lu completed=%lu\n",
                    static_cast<unsigned long>(
                        result.maintenance.failed_rounds),
                    static_cast<unsigned long>(
                        result.maintenance.seal_rounds),
                    static_cast<unsigned long>(
                        result.maintenance.flush_rounds),
                    static_cast<unsigned long>(
                        result.maintenance.non_noop_flush_rounds),
                    static_cast<unsigned long>(
                        result.maintenance.completed_rounds));
    }
    return 0;
}

}  // namespace

int
main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        auto opts = parse_argv(argc, argv);
        std::printf("test_ycsb_concurrency_checker_e2e: pci=%s scenario=%s "
                    "readers=%u hot_keys=%lu update_batches=%u\n",
                    opts.pci_addr.c_str(),
                    scenario_name(opts.scenario).data(),
                    kReaderCount,
                    static_cast<unsigned long>(kHotKeyCount),
                    kUpdateBatches);
        const int rc = run_checker(opts);
        if (rc == 0) {
            std::printf("all passed\n");
        }
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_ycsb_concurrency_checker_e2e failed: %s\n",
                     e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr,
                     "test_ycsb_concurrency_checker_e2e failed: "
                     "non-std exception\n");
        return 1;
    }
}
