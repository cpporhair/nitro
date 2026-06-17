// 058 steady-state e2e harness — real SPDK NVMe backend.
//
// Drives the coord-orchestrated steady-state background loop
//   write_batch → seal_once → flush_once (frontier switch / CAT2) →
//   physical reclaim (TRIM + value reclaim) → recovery_safe_lsn advance
// over many rounds against a freshly formatted real NVMe namespace, and
// asserts correctness (point_get readback vs an in-test oracle),
// stability (no panic; bounded live gens / tree+value heads), and the
// flush-shape coverage matrix F1-F8 (§3 of the plan).
//
// Test-only runtime builder note:
// build_runtime() is intentionally pinned to the bootstrap disk profile, whose
// current shadow_slots_per_range is 1. F2/F3 require exercising next-slot and
// slot exhaustion, so this test constructs the same production schedulers over
// a real NVMe device with a test-local tree_geometry carrying 4 shadow slots.
// shadow_slots=4 is a valid production geometry, not a code fork — no
// production hook or production code path is changed.
//
// Requires an SPDK-bound NVMe controller: pass --pci-addr BDF (or set
// INCONEL_NVME_PCI_ADDR). The target namespace is force-formatted on
// every run, so point it at a scratch device only.

#include "apps/inconel/runtime/operations.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/core/data_area_heads.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/shard_partition_builder.hh"
#include "apps/inconel/core/tree_read_domain.hh"
#include "apps/inconel/format/layout_plan.hh"
#include "apps/inconel/format/superblock_builder.hh"
#include "apps/inconel/format/tree_page.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/nvme/runtime_scheduler.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/runtime/facade.hh"
#include "apps/inconel/test/check.hh"
#include "apps/inconel/tree/page_reader.hh"
#include "pump/core/context.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

using namespace apps::inconel;

namespace {

constexpr uint32_t kLbaSize = 4096;
constexpr uint64_t kNamespaceLbas = 110000;
constexpr uint32_t kShadowSlots = 4;
constexpr uint32_t kSteadyRounds = 14;

using tree_cache_t = core::segmented_clock_cache;
using value_cache_t = core::segmented_clock_cache;
using runtime_t =
    runtime::inconel_runtime_t<tree_cache_t, value_cache_t>;
using root_context_t = decltype(pump::core::make_root_context());

constexpr core::tree_geometry kSteadyTreeGeometry{
    .lba_size = kLbaSize,
    .tree_page_size = kLbaSize,
    .shadow_slots_per_range = kShadowSlots,
};

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

template <typename Future>
bool
ready(Future& fut) {
    return fut.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
}

template <typename T>
T
expect_ok(op_result<T>&& result) {
    if (std::holds_alternative<std::exception_ptr>(result)) {
        try {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "operation failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "operation failed: non-std exception\n");
        }
        CHECK(false);
    }
    return std::move(std::get<T>(result));
}

format::layout_plan
steady_layout_plan() {
    const auto& p = format::kBootstrapFormatProfile;
    format::layout_plan plan{};
    plan.lba_size = p.lba_size;
    plan.namespace_size = kNamespaceLbas * static_cast<uint64_t>(kLbaSize);
    plan.total_lbas = kNamespaceLbas;
    plan.wal_base_paddr = p.wal_base_paddr;
    plan.wal_segment_size = p.wal_segment_size;
    plan.wal_segment_count = p.wal_segment_count;
    plan.wal_segment_lbas = p.wal_segment_size / p.lba_size;
    plan.data_area_base_paddr = p.value_data_area_base;
    plan.data_area_end_paddr = p.value_data_area_end;
    plan.tree_page_size = kSteadyTreeGeometry.tree_page_size;
    plan.shadow_slots_per_range = kSteadyTreeGeometry.shadow_slots_per_range;
    plan.value_class_count = p.value_class_count;
    for (uint8_t i = 0; i < p.value_class_count; ++i) {
        plan.value_class_sizes[i] = p.value_class_sizes[i];
    }
    plan.value_space_quantum_bytes = p.value_space_quantum_bytes;
    plan.value_space_group_size_lbas = p.value_space_group_size_lbas;
    return plan;
}

// Drive a transient NVMe scheduler to completion. Used only for the
// bootstrap superblock format, which runs before the runtime (and its
// per-core advance loop) exists, so it owns its own advance loop —
// mirrors test_flush_e2e's real-NVMe bootstrap.
template <typename SenderBuilder>
bool
submit_nvme_and_wait(nvme::runtime_scheduler& sched,
                     SenderBuilder&&          build_sender) {
    auto ctx     = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<bool>>();
    auto fut     = promise->get_future();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::then([promise](bool ok) { promise->set_value(ok); })
        >> pump::sender::submit(ctx);

    while (fut.wait_for(std::chrono::milliseconds(0)) !=
           std::future_status::ready) {
        sched.advance();
        std::this_thread::yield();
    }
    return fut.get();
}

void
write_superblock_page(nvme::runtime_scheduler&  sched,
                      uint64_t                  lba,
                      const format::superblock& sb) {
    std::vector<char> page(kLbaSize, 0);
    std::memcpy(page.data(), &sb, sizeof(sb));
    const bool ok = submit_nvme_and_wait(sched, [&]() {
        return sched.write(lba, page.data(), 1, nvme::IO_FLAGS_FUA);
    });
    CHECK(ok && "real NVMe superblock write failed");
}

// Format superblock A/B onto the real namespace through a transient
// scheduler bound to fmt_core's qpair. The scheduler borrows the qpair
// (real_device owns it) and releases it on destruction, so the runtime
// can claim the same qpair afterwards.
void
format_real_superblocks(nvme::runtime_device& device, uint32_t fmt_core) {
    const auto sb_a = format::build_superblock(steady_layout_plan(), 1);
    const auto sb_b = format::build_superblock(steady_layout_plan(), 0);

    pump::core::this_core_id = fmt_core;
    nvme::runtime_scheduler fmt_sched(
        device.qpair_for_core(fmt_core),
        kLbaSize,
        /*pool_pages=*/  64,
        /*queue_depth=*/ 128,
        /*local_depth=*/ 32,
        /*alignment=*/   4096,
        SPDK_ENV_NUMA_ID_ANY,
        device.device_id());

    write_superblock_page(fmt_sched, 0, sb_a);
    write_superblock_page(fmt_sched, 1, sb_b);

    const bool flushed = submit_nvme_and_wait(fmt_sched, [&]() {
        return fmt_sched.flush();
    });
    CHECK(flushed && "real NVMe flush after steady bootstrap format failed");
}

runtime_t*
build_steady_runtime(std::span<const uint32_t> cores,
                     nvme::runtime_device* device) {
    const auto& profile = format::kBootstrapFormatProfile;
    CHECK(device != nullptr);
    CHECK(!cores.empty());
    for (uint32_t core : cores) {
        (void)device->qpair_for_core(core);
    }

    uint32_t max_core = 0;
    for (uint32_t core : cores) max_core = std::max(max_core, core);
    const uint32_t capacity =
        std::max<uint32_t>(std::thread::hardware_concurrency(), max_core + 1);

    core::registry::clear();
    core::registry::init_capacity(capacity);

    auto* rt = new runtime_t();
    auto shared_heads = std::make_shared<core::data_area_heads>();
    auto wal_frontier = std::make_shared<core::wal_reclaim_frontier>();
    core::registry::data_area_heads_ptr = shared_heads;
    core::registry::wal_reclaim_frontier_ptr = wal_frontier;

    auto bootstrap_map =
        std::make_shared<const core::shard_partition_map>(
            core::build_initial_shard_partition_map(
                core::leaf_order_index{},
                static_cast<uint32_t>(cores.size())));
    core::registry::install_shard_partitions(bootstrap_map);

    auto front_topo = runtime::build_front_topology(
        runtime::front_topology_options{
            .cores = cores,
            .front_cores = cores,
            .coord_core = static_cast<int32_t>(cores.front()),
            .wal_space_core = static_cast<int32_t>(cores.front()),
            .wal_geometry = runtime::wal_geometry_from_profile(profile),
            .wal_reclaim_frontier = wal_frontier.get(),
            .tree_geometry = &kSteadyTreeGeometry,
            .front_queue_depth = 2048,
            .coord_queue_depth = 2048,
            .coord_ready_window = 131072,
            .front_wal_config = {.max_fua_inflight = 16,
                                 .max_pages_per_plan = 16,
                                 .pending_prepare_capacity = 128,
                                 .max_participants_per_group = 16},
        });

    using value_sched_t = value::value_alloc_sched<value_cache_t>;
    const uint32_t singleton_core = cores.front();
    for (std::size_t rd_idx = 0; rd_idx < cores.size(); ++rd_idx) {
        const uint32_t core = cores[rd_idx];
        pump::core::this_core_id = core;

        auto* nvme_sched = new nvme::runtime_scheduler(
            device->qpair_for_core(core),
            profile.lba_size,
            4096,
            4096,
            256,
            4096,
            SPDK_ENV_NUMA_ID_ANY,
            device->device_id());

        auto* read_domain = new core::tree_read_domain<tree_cache_t>(
            static_cast<uint32_t>(rd_idx),
            core::registry::current_shard_partitions(),
            tree_cache_t(128),
            &kSteadyTreeGeometry,
            4096,
            runtime::make_runtime_dma_page_allocator(),
            4096,
            SPDK_ENV_NUMA_ID_ANY);

        value_sched_t* value_sched = nullptr;
        if (core == singleton_core) {
            value_sched = new value_sched_t(
                profile.class_sizes(),
                profile.lba_size,
                profile.value_data_area_base,
                profile.value_data_area_end,
                shared_heads.get(),
                value_cache_t(128),
                profile.value_space_quantum_bytes,
                profile.value_space_group_size_lbas,
                4096,
                runtime::make_runtime_dma_page_allocator(),
                4096,
                SPDK_ENV_NUMA_ID_ANY,
                value::value_io_policy{.max_write_inflight = 64,
                                       .max_read_inflight = 64,
                                       .max_trim_inflight = 32});
            core::registry::value_alloc_sched = value_sched;
        }

        tree::tree_sched* tree_sched = nullptr;
        if (core == singleton_core) {
            tree_sched = new tree::tree_sched(
                &kSteadyTreeGeometry,
                profile.value_data_area_base,
                shared_heads.get(),
                wal_frontier.get(),
                &core::registry::tree_read_domains.list,
                4096,
                runtime::make_runtime_dma_page_allocator(),
                4096,
                SPDK_ENV_NUMA_ID_ANY);
            core::registry::tree_sched_singleton_ptr = tree_sched;
            core::set_reclaim_sink(&tree_sched->sink_handle);
            shared_heads->tree_head_lba.store(
                tree_sched->state.alloc.head.lba,
                std::memory_order_relaxed);
        }

        coord::coord_sched* coord_sched =
            core == front_topo.coord_core ? front_topo.coord : nullptr;
        front::front_sched* front_sched =
            core < core::registry::front_scheds.by_core.size()
                ? core::registry::front_scheds.by_core[core]
                : nullptr;
        wal::wal_space_sched* wal_space =
            core == front_topo.wal_space_core ? front_topo.wal_space
                                              : nullptr;

        rt->add_core_schedulers(core,
                                nvme_sched,
                                read_domain,
                                value_sched,
                                tree_sched,
                                coord_sched,
                                front_sched,
                                wal_space);

        core::registry::nvme_scheds.list.push_back(nvme_sched);
        core::registry::nvme_scheds.by_core[core] = nvme_sched;
        core::registry::tree_read_domains.list.push_back(read_domain);
        core::registry::tree_read_domains.by_core[core] = read_domain;
    }

    core::registry::nvme_by_front_owner.assign(front_topo.fronts.size(),
                                               nullptr);
    for (std::size_t owner = 0; owner < front_topo.fronts.size(); ++owner) {
        const uint32_t front_core = front_topo.front_cores[owner];
        auto* nvme = core::registry::nvme_scheds.by_core[front_core];
        CHECK(nvme != nullptr);
        core::registry::nvme_by_front_owner[owner] = nvme;
    }

    rt::publish_shard_partitions(bootstrap_map);
    return rt;
}

std::string
pad_u32(uint32_t v) {
    std::ostringstream os;
    os << std::setw(6) << std::setfill('0') << v;
    return os.str();
}

std::string
key_for_owner(uint32_t owner,
              uint32_t front_count,
              std::string prefix,
              uint32_t target_len = 64) {
    if (prefix.size() + 8 < target_len) {
        prefix.append(target_len - prefix.size() - 8, 'k');
    }
    for (uint32_t i = 0; i < 200000; ++i) {
        std::string key = prefix + "-" + std::to_string(i);
        if (static_cast<uint32_t>(core::key_hash(key) % front_count) ==
            owner) {
            return key;
        }
    }
    CHECK(false);
    return {};
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

struct oracle {
    std::map<std::string, std::optional<std::string>> expected;

    void apply(const core::raw_batch_op& op) {
        if (op.op == core::write_op_type::put) {
            expected[op.key] = op.value;
        } else {
            expected[op.key] = std::nullopt;
        }
    }

    void apply(std::span<const core::raw_batch_op> ops) {
        for (const auto& op : ops) apply(op);
    }
};

struct tree_record_result {
    enum class kind : uint8_t { absent, value, tombstone };
    kind tag = kind::absent;
    format::value_ref vr{};
    uint64_t data_ver = 0;
};

struct steady_fixture {
    std::string pci_addr;
    std::vector<uint32_t> cores;
    nvme::runtime_device device;
    runtime_t* rt = nullptr;

    steady_fixture(std::string pci_addr_in,
                   std::vector<uint32_t> cores_in = {0, 1})
        : pci_addr(std::move(pci_addr_in))
        , cores(std::move(cores_in))
        , device(nvme::real_device_options{
              .pci_addr       = pci_addr.c_str(),
              .cores          = cores,
              .spdk_core_mask = nullptr,
              .spdk_name      = "inconel_steady_e2e",
              .init_spdk_env  = true,
              .qpair_depth    = 256,
              .device_id      = 0,
          }) {
        CHECK(device.size_bytes() >=
              kNamespaceLbas * static_cast<uint64_t>(kLbaSize));
        // Format must precede build: the transient fmt scheduler borrows
        // cores.front()'s qpair and releases it before build_steady_runtime
        // claims the same qpair for the runtime's core-0 nvme scheduler.
        pump::core::this_core_id = cores.front();
        format_real_superblocks(device, cores.front());
        rt = build_steady_runtime(
            std::span<const uint32_t>(cores.data(), cores.size()),
            &device);
        pump::core::this_core_id = cores.front();
    }

    ~steady_fixture() {
        runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt);
    }

    steady_fixture(const steady_fixture&) = delete;
    steady_fixture& operator=(const steady_fixture&) = delete;

    bool advance_all() {
        bool progress = false;
        for (uint32_t core : cores) {
            std::apply(
                [&](auto*... sched) {
                    auto step = [&](auto* s) {
                        if (s != nullptr) progress |= s->advance();
                    };
                    (step(sched), ...);
                },
                rt->schedulers_by_core[core]);
        }
        return progress;
    }

    template <typename Submission>
    void drive_until_ready(Submission& sub, uint32_t limit = 900000) {
        for (uint32_t i = 0; !ready(sub.fut) && i < limit; ++i) {
            if (!advance_all()) std::this_thread::yield();
        }
        CHECK(ready(sub.fut));
    }

    void drain_maintenance(uint32_t limit = 300000) {
        uint32_t idle = 0;
        for (uint32_t i = 0; i < limit && idle < 1024; ++i) {
            if (advance_all()) {
                idle = 0;
            } else {
                ++idle;
                std::this_thread::yield();
            }
        }
    }

    void seal_flush_drain() {
        {
            auto seal = expect_ok<pipeline::seal_round_result>(run_seal());
            CHECK(seal.cat1 != nullptr);
            auto flush = expect_ok<pipeline::flush_round_result>(run_flush());
            CHECK(!flush.noop);
        }
        drain_maintenance();
    }

    [[nodiscard]] core::client_batch_buffer
    make_input(std::vector<core::raw_batch_op> ops) const {
        return core::encode_client_batch(
            std::span<const core::raw_batch_op>(ops.data(), ops.size()));
    }

    op_result<write_path::write_batch_result>
    run_write(std::vector<core::raw_batch_op> ops) {
        auto sub = submit_result<write_path::write_batch_result>(
            [this, input = make_input(std::move(ops))]() mutable {
                return rt::write_batch(std::move(input));
            });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    op_result<pipeline::point_get_result>
    run_point_get(std::string_view key) {
        auto sub = submit_result<pipeline::point_get_result>(
            [key]() { return rt::point_get(key); });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    op_result<pipeline::seal_round_result>
    run_seal() {
        auto sub = submit_result<pipeline::seal_round_result>(
            []() { return rt::seal_once(); });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    op_result<pipeline::flush_round_result>
    run_flush() {
        auto sub = submit_result<pipeline::flush_round_result>(
            []() { return rt::flush_once(); });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    op_result<tree::recovery_frontier_snapshot>
    run_recompute_frontier() {
        auto sub = submit_result<tree::recovery_frontier_snapshot>(
            []() { return rt::owner()->submit_recompute_recovery_frontier(); });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    void write_ops(std::vector<core::raw_batch_op> ops, oracle& o) {
        const auto span = std::span<const core::raw_batch_op>(ops.data(),
                                                             ops.size());
        o.apply(span);
        auto wr = expect_ok<write_path::write_batch_result>(
            run_write(std::move(ops)));
        CHECK(wr.entry_count > 0);
    }

    void write_put(std::string key, std::string value, oracle& o) {
        std::vector<core::raw_batch_op> ops;
        ops.push_back(put_op(std::move(key), std::move(value)));
        write_ops(std::move(ops), o);
    }

    void write_del(std::string key, oracle& o) {
        std::vector<core::raw_batch_op> ops;
        ops.push_back(del_op(std::move(key)));
        write_ops(std::move(ops), o);
    }

    void seal_flush_mark_wal_safe_and_drain() {
        {
            auto seal = expect_ok<pipeline::seal_round_result>(run_seal());
            CHECK(seal.cat1 != nullptr);
            auto flush = expect_ok<pipeline::flush_round_result>(run_flush());
            CHECK(!flush.noop);
        }
        core::registry::wal_reclaim_frontier_singleton()->publish_exact_min(
            core::wal_reclaim_frontier::no_unreclaimed_lsn);
        drain_maintenance();
    }

    [[nodiscard]] uint32_t front_count() const {
        return core::registry::front_count();
    }

    [[nodiscard]] uint64_t cat_epoch() const {
        return core::registry::coord_sched_singleton()->cat_epoch();
    }

    [[nodiscard]] core::read_handle acquire_handle() const {
        return core::registry::coord_sched_singleton()
            ->acquire_read_handle_for_testing();
    }

    [[nodiscard]] std::shared_ptr<const core::tree_manifest>
    current_manifest() const {
        return acquire_handle().cat->prs->tree_guard->manifest;
    }

    // Raw LBA read through the runtime's core-front NVMe scheduler.
    // Replaces the mock device's synchronous read_bytes: on the real
    // backend every byte-level inspection (tree pages, value objects,
    // superblocks) must go through a scheduler sender driven to
    // completion by advance_all.
    [[nodiscard]] std::vector<char> read_lbas(uint64_t lba,
                                              uint32_t num_lbas) {
        auto* sched = core::registry::nvme_scheds.by_core[cores.front()];
        CHECK(sched != nullptr);
        std::vector<char> buf(
            static_cast<std::size_t>(num_lbas) * kLbaSize);
        pump::core::this_core_id = cores.front();
        auto sub = submit_result<bool>([sched, lba, num_lbas, &buf]() {
            return sched->read(lba, buf.data(), num_lbas);
        });
        drive_until_ready(sub);
        CHECK(expect_ok<bool>(sub.fut.get()) &&
              "real NVMe inspection read failed");
        return buf;
    }

    [[nodiscard]] format::superblock read_superblock(uint64_t lba) {
        auto page = read_lbas(lba, 1);
        format::superblock sb{};
        std::memcpy(&sb, page.data(), sizeof(sb));
        CHECK(format::inspect_superblock(sb) ==
              format::superblock_status::ok);
        return sb;
    }

    [[nodiscard]] std::vector<char> read_tree_page(format::paddr slot) {
        const uint32_t num_lbas =
            kSteadyTreeGeometry.tree_page_size / kLbaSize;
        auto page = read_lbas(slot.lba, num_lbas);
        CHECK(format::tree_page_validate(page.data(), page.size()));
        return page;
    }

    [[nodiscard]] format::node_type node_type_at(format::paddr slot) {
        auto page = read_tree_page(slot);
        const auto* hdr =
            reinterpret_cast<const format::tree_slot_header*>(page.data());
        return hdr->type;
    }

    [[nodiscard]] tree_record_result
    manifest_lookup_record(const core::tree_manifest& manifest,
                           std::string_view key) {
        if (!manifest.has_root()) return {};

        format::paddr slot = manifest.root_slot;
        for (uint32_t depth = 0; depth < 16; ++depth) {
            auto page = read_tree_page(slot);
            const auto* hdr =
                reinterpret_cast<const format::tree_slot_header*>(page.data());
            if (hdr->type == format::node_type::leaf) {
                tree::leaf_page_reader reader;
                CHECK(reader.parse(page.data(), page.size()));
                auto rec = reader.find(key);
                if (!rec.has_value()) return {};
                if (rec->kind == format::record_kind::tombstone) {
                    return tree_record_result{
                        .tag = tree_record_result::kind::tombstone,
                        .vr = {},
                        .data_ver = rec->data_ver,
                    };
                }
                return tree_record_result{
                    .tag = tree_record_result::kind::value,
                    .vr = rec->vr,
                    .data_ver = rec->data_ver,
                };
            }

            CHECK(hdr->type == format::node_type::internal);
            tree::internal_page_reader reader;
            CHECK(reader.parse(page.data(), page.size()));
            const auto child_base = reader.find_child(key);
            slot = manifest.resolve(child_base);
        }
        CHECK(false);
        return {};
    }

    [[nodiscard]] std::string device_value_body(const format::value_ref& vr) {
        const uint32_t total =
            static_cast<uint32_t>(sizeof(format::value_object_header)) +
            vr.len;
        const uint32_t span_lbas =
            (vr.byte_offset + total + kLbaSize - 1) / kLbaSize;
        auto page = read_lbas(vr.base.lba, span_lbas);
        auto decoded = format::decode_value_object(
            std::span<const char>(page.data() + vr.byte_offset,
                                  page.size() - vr.byte_offset),
            vr.len);
        CHECK(decoded.status == format::value_decode_status::ok);
        return std::string(decoded.body.data(), decoded.body.size());
    }

    [[nodiscard]] std::optional<std::string>
    manifest_get_body(const core::tree_manifest& manifest,
                      std::string_view key) {
        auto rec = manifest_lookup_record(manifest, key);
        if (rec.tag != tree_record_result::kind::value) return std::nullopt;
        return device_value_body(rec.vr);
    }

    [[nodiscard]] std::optional<std::string>
    snapshot_get_body(const core::read_handle& rh, std::string_view key) {
        const auto owner =
            static_cast<uint32_t>(core::key_hash(key) % front_count());
        auto mt = core::registry::front_at(owner)->lookup_memtable_for_testing(
            key, rh.read_lsn, (*rh.cat->prs->fronts)[owner]);
        if (auto* hit = std::get_if<core::memtable_value_hit>(&mt)) {
            return device_value_body(hit->durable);
        }
        if (std::holds_alternative<core::memtable_tombstone>(mt)) {
            return std::nullopt;
        }
        return manifest_get_body(*rh.cat->prs->tree_guard->manifest, key);
    }

    [[nodiscard]] format::paddr leaf_range_for_key(
        const core::tree_manifest& manifest,
        std::string_view key) const {
        const auto idx = manifest.leaf_order.find_leaf_for_key(key);
        CHECK(idx < manifest.leaf_order.spans.size());
        return manifest.leaf_order.spans[idx].leaf_range_base;
    }

    [[nodiscard]] uint32_t slot_index(const core::tree_manifest& manifest,
                                      format::paddr range_base) const {
        return manifest.slot_index(range_base);
    }

    [[nodiscard]] uint64_t tree_head_lba() const {
        return rt::owner()->state.alloc.head.lba;
    }

    [[nodiscard]] std::size_t tree_free_range_count() const {
        return rt::owner()->state.alloc.free_ranges.size();
    }

    [[nodiscard]] uint64_t value_head_lba() const {
        return core::registry::data_area_heads_singleton()
            ->value_head_lba.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t current_imm_count() const {
        std::size_t out = 0;
        for (uint32_t owner = 0; owner < front_count(); ++owner) {
            out += core::registry::front_at(owner)->imms_for_testing().size();
        }
        return out;
    }

    [[nodiscard]] bool reclaim_idle() const {
        const auto* owner = rt::owner();
        return owner->state.reclaim_q.empty() &&
               owner->state.pending_reclaim.empty() &&
               !owner->state.active_reclaim.has_value() &&
               owner->state.reclaim_invalidate_done_q.empty() &&
               owner->state.reclaim_trim_done_q.empty() &&
               !owner->state.reclaim_gate_requested;
    }
};

void
expect_found(const pipeline::point_get_result& r, std::string_view body) {
    CHECK(r.found);
    CHECK(r.value == body);
}

void
expect_not_found(const pipeline::point_get_result& r) {
    CHECK(!r.found);
    CHECK(r.value.empty());
}

void
expect_oracle(steady_fixture& fx,
              const oracle& o,
              std::span<const std::string> keys) {
    for (const auto& key : keys) {
        auto it = o.expected.find(key);
        CHECK(it != o.expected.end());
        auto result = expect_ok<pipeline::point_get_result>(
            fx.run_point_get(key));
        if (it->second.has_value()) {
            expect_found(result, *it->second);
        } else {
            expect_not_found(result);
        }
    }
}

void
expect_leaf_order_sorted(const core::tree_manifest& manifest) {
    const auto& order = manifest.leaf_order;
    CHECK(!order.empty());
    for (std::size_t i = 0; i < order.spans.size(); ++i) {
        const auto& span = order.spans[i];
        CHECK(manifest.slot_map.find(span.leaf_range_base) !=
              manifest.slot_map.end());
        const auto lower = order.fence_lower(span);
        const auto upper = order.fence_upper(span);
        if (i == 0) CHECK(lower.empty());
        if (i + 1 == order.spans.size()) CHECK(upper.empty());
        if (!lower.empty() && !upper.empty()) CHECK(lower < upper);
        if (i + 1 < order.spans.size()) {
            CHECK(upper == order.fence_lower(order.spans[i + 1]));
        }
    }
}

uint16_t
leaf_record_count(steady_fixture& fx,
                  const core::tree_manifest& manifest,
                  format::paddr leaf_range) {
    auto page = fx.read_tree_page(manifest.resolve(leaf_range));
    tree::leaf_page_reader reader;
    CHECK(reader.parse(page.data(), page.size()));
    return reader.record_count();
}

bool
root_has_internal_child(steady_fixture& fx,
                        const core::tree_manifest& manifest) {
    if (!manifest.has_root()) return false;
    auto root_page = fx.read_tree_page(manifest.root_slot);
    const auto* hdr =
        reinterpret_cast<const format::tree_slot_header*>(root_page.data());
    if (hdr->type != format::node_type::internal) return false;
    tree::internal_page_reader reader;
    CHECK(reader.parse(root_page.data(), root_page.size()));
    std::vector<format::paddr> children;
    for (uint16_t i = 0; i < reader.record_count(); ++i) {
        children.push_back(reader.get(i).child_base);
    }
    children.push_back(reader.rightmost_child());
    for (auto child_base : children) {
        auto child_slot = manifest.resolve(child_base);
        if (fx.node_type_at(child_slot) == format::node_type::internal) {
            return true;
        }
    }
    return false;
}

void
coverage_f7_delete_and_empty_leaf(steady_fixture& fx,
                                  oracle& o,
                                  std::vector<std::string>& samples) {
    const auto key = key_for_owner(0, fx.front_count(), "f7-empty", 96);
    fx.write_put(key, "f7-live", o);
    fx.seal_flush_mark_wal_safe_and_drain();
    fx.write_del(key, o);
    fx.seal_flush_mark_wal_safe_and_drain();
    expect_not_found(expect_ok<pipeline::point_get_result>(
        fx.run_point_get(key)));

    const auto manifest = fx.current_manifest();
    CHECK(manifest->leaf_order.size() == 1);
    const auto delete_leaf_records = leaf_record_count(
        fx, *manifest, manifest->leaf_order.spans[0].leaf_range_base);
    // The first delete flush captures recovery_safe_lsn before the delete's
    // own data_ver becomes safe, so the conservative shape may still carry a
    // tombstone. F7's hard semantic assertion is not_found; prune/collapse is
    // intentionally not asserted in 058.
    CHECK(delete_leaf_records <= 1);

    const auto missing =
        key_for_owner(1 % fx.front_count(), fx.front_count(),
                      "f7-missing", 96);
    fx.write_del(missing, o);
    fx.seal_flush_mark_wal_safe_and_drain();
    expect_not_found(expect_ok<pipeline::point_get_result>(
        fx.run_point_get(missing)));
    std::fprintf(stderr,
                 "[F7] tombstone through flush: existing delete not_found, "
                 "missing delete not_found, delete leaf records=%u\n",
                 static_cast<unsigned>(delete_leaf_records));
    samples.push_back(key);
    samples.push_back(missing);
}

void
coverage_f8_noop_round(steady_fixture& fx) {
    const auto epoch_before = fx.cat_epoch();
    const auto manifest_before = fx.current_manifest();
    const auto imms_before = fx.current_imm_count();
    auto flush = expect_ok<pipeline::flush_round_result>(fx.run_flush());
    CHECK(flush.noop);
    CHECK(fx.cat_epoch() == epoch_before);
    CHECK(fx.current_manifest().get() == manifest_before.get());
    CHECK(fx.current_imm_count() == imms_before);
    CHECK(core::registry::coord_sched_singleton()->gate_open_for_testing());
}

std::vector<std::string>
bootstrap_split_tree(steady_fixture& fx, oracle& o) {
    std::vector<core::raw_batch_op> ops;
    std::vector<std::string> keys;
    for (uint32_t i = 0; i < 18; ++i) {
        const uint32_t owner = i % fx.front_count();
        auto key = key_for_owner(owner,
                                 fx.front_count(),
                                 "f1-seed-" + pad_u32(i),
                                 720);
        keys.push_back(key);
        ops.push_back(put_op(key, "seed-v" + std::to_string(i)));
    }
    fx.write_ops(std::move(ops), o);
    fx.seal_flush_mark_wal_safe_and_drain();

    const auto manifest = fx.current_manifest();
    CHECK(manifest->has_root());
    CHECK(manifest->leaf_order.size() >= 2);
    CHECK(fx.node_type_at(manifest->root_slot) == format::node_type::internal);
    expect_leaf_order_sorted(*manifest);
    return keys;
}

struct shadow_target {
    std::string key;
    format::paddr leaf_range{};
};

shadow_target
coverage_f1_root_stable(steady_fixture& fx,
                        oracle& o,
                        std::vector<std::string>& keys) {
    const auto before = fx.current_manifest();
    const auto key = keys[keys.size() / 2];
    const auto old_body = o.expected.at(key).value();
    const auto old_handle = fx.acquire_handle();
    const auto leaf_range = fx.leaf_range_for_key(*before, key);
    const auto leaf_slot_before = fx.slot_index(*before, leaf_range);
    const auto root_slot_before = before->root_slot;
    const auto root_range_before = before->root_range_base;

    fx.write_put(key, "f1-new", o);
    fx.seal_flush_mark_wal_safe_and_drain();

    const auto after = fx.current_manifest();
    CHECK(after->root_range_base == root_range_before);
    CHECK(after->root_slot == root_slot_before);
    CHECK(fx.leaf_range_for_key(*after, key) == leaf_range);
    CHECK(fx.slot_index(*after, leaf_range) == leaf_slot_before + 1);
    expect_found(expect_ok<pipeline::point_get_result>(fx.run_point_get(key)),
                 "f1-new");
    auto old = fx.snapshot_get_body(old_handle, key);
    CHECK(old.has_value());
    CHECK(*old == old_body);
    std::fprintf(stderr,
                 "[F1] root-stable overwrite: root lba=%lu leaf lba=%lu "
                 "slot %u->%u\n",
                 static_cast<unsigned long>(root_range_before.lba),
                 static_cast<unsigned long>(leaf_range.lba),
                 leaf_slot_before,
                 fx.slot_index(*after, leaf_range));
    return shadow_target{.key = key, .leaf_range = leaf_range};
}

void
coverage_f2_shadow_cow_cross_round(steady_fixture& fx,
                                   oracle& o,
                                   const shadow_target& target) {
    auto manifest = fx.current_manifest();
    CHECK(fx.leaf_range_for_key(*manifest, target.key) == target.leaf_range);
    uint32_t prev_slot = fx.slot_index(*manifest, target.leaf_range);
    CHECK(prev_slot == 1);
    for (uint32_t round = 0; round < 2; ++round) {
        const std::string body = "f2-round-" + std::to_string(round);
        fx.write_put(target.key, body, o);
        fx.seal_flush_mark_wal_safe_and_drain();
        manifest = fx.current_manifest();
        CHECK(fx.leaf_range_for_key(*manifest, target.key) ==
              target.leaf_range);
        const uint32_t cur = fx.slot_index(*manifest, target.leaf_range);
        CHECK(cur == prev_slot + 1);
        expect_found(expect_ok<pipeline::point_get_result>(
                         fx.run_point_get(target.key)),
                     body);
        prev_slot = cur;
    }
    std::fprintf(stderr,
                 "[F2] same leaf range lba=%lu advanced across rounds to "
                 "slot=%u without panic\n",
                 static_cast<unsigned long>(target.leaf_range.lba),
                 prev_slot);
}

format::paddr
coverage_f3_slot_exhaustion_consolidates(steady_fixture& fx,
                                         oracle& o,
                                         const shadow_target& target) {
    const auto before = fx.current_manifest();
    CHECK(fx.slot_index(*before, target.leaf_range) == kShadowSlots - 1);
    const auto free_before = fx.tree_free_range_count();
    fx.write_put(target.key, "f3-new-range", o);
    fx.seal_flush_mark_wal_safe_and_drain();

    const auto after = fx.current_manifest();
    const auto new_range = fx.leaf_range_for_key(*after, target.key);
    CHECK(new_range != target.leaf_range);
    CHECK(fx.slot_index(*after, new_range) == 0);
    // Old range retired → reclaimed (TRIM + returned to the tree
    // allocator free list). On the real backend the device TRIM count is
    // not observable, so the engine-side signal is the freed range
    // landing in free_ranges; F4 then proves it is reused.
    CHECK(fx.tree_free_range_count() > free_before);
    CHECK(fx.reclaim_idle());
    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_point_get(target.key)),
                 "f3-new-range");
    std::fprintf(stderr,
                 "[F3] exhausted range lba=%lu consolidated to new range "
                 "lba=%lu; free_ranges %zu->%zu\n",
                 static_cast<unsigned long>(target.leaf_range.lba),
                 static_cast<unsigned long>(new_range.lba),
                 free_before,
                 fx.tree_free_range_count());
    return target.leaf_range;
}

void
coverage_f4_leaf_split_reuses_free_range(steady_fixture& fx,
                                         oracle& o,
                                         std::vector<std::string>& samples,
                                         format::paddr expected_reused_range) {
    const auto before = fx.current_manifest();
    const auto leaf_count_before = before->leaf_order.size();
    const auto head_before = fx.tree_head_lba();
    std::vector<core::raw_batch_op> ops;
    for (uint32_t i = 0; i < 12; ++i) {
        const uint32_t owner = i % fx.front_count();
        auto key = key_for_owner(owner,
                                 fx.front_count(),
                                 "f4-high-" + pad_u32(i),
                                 900);
        samples.push_back(key);
        ops.push_back(put_op(key, "f4-v" + std::to_string(i)));
    }
    fx.write_ops(std::move(ops), o);
    fx.seal_flush_mark_wal_safe_and_drain();

    const auto after = fx.current_manifest();
    CHECK(after->leaf_order.size() > leaf_count_before);
    CHECK(after->slot_map.find(expected_reused_range) !=
          after->slot_map.end());
    CHECK(fx.tree_head_lba() <= head_before + 64);
    expect_leaf_order_sorted(*after);
    expect_oracle(fx, o, std::span<const std::string>(samples.data(),
                                                     samples.size()));
    std::fprintf(stderr,
                 "[F4] leaf split leaf_count %zu->%zu; recycled range "
                 "lba=%lu present in manifest\n",
                 leaf_count_before,
                 after->leaf_order.size(),
                 static_cast<unsigned long>(expected_reused_range.lba));
}

void
coverage_f5_root_change(steady_fixture& fx,
                        oracle& o,
                        std::vector<std::string>& samples) {
    const auto before = fx.current_manifest();
    const auto root_before = before->root_range_base;
    const std::string long_common_prefix(900, 'z');
    uint32_t batches = 0;
    uint32_t ops_total = 0;
    auto after = before;
    for (; batches < 6 && after->root_range_base == root_before; ++batches) {
        std::vector<core::raw_batch_op> ops;
        for (uint32_t i = 0; i < 48; ++i) {
            const uint32_t op_index = batches * 48 + i;
            const uint32_t owner = op_index % fx.front_count();
            auto key = key_for_owner(owner,
                                     fx.front_count(),
                                     long_common_prefix + "f5-root-" +
                                         pad_u32(batches) + "-" +
                                         pad_u32(i),
                                     64);
            samples.push_back(key);
            ops.push_back(put_op(key, "f5-v" + std::to_string(op_index)));
        }
        ops_total += static_cast<uint32_t>(ops.size());
        fx.write_ops(std::move(ops), o);
        fx.seal_flush_mark_wal_safe_and_drain();
        after = fx.current_manifest();
    }

    CHECK(after->root_range_base != root_before);
    CHECK(root_has_internal_child(fx, *after));
    auto sb_a = fx.read_superblock(0);
    auto sb_b = fx.read_superblock(1);
    const auto chosen = format::choose_newer_superblock(sb_a, sb_b);
    CHECK(chosen.chosen != nullptr);
    CHECK(chosen.chosen->root_base_paddr == after->root_range_base);
    expect_leaf_order_sorted(*after);
    expect_oracle(fx, o, std::span<const std::string>(samples.data(),
                                                     samples.size()));
    std::fprintf(stderr,
                 "[F5] root range lba %lu->%lu after %u batches/%u ops; "
                 "root has internal child\n",
                 static_cast<unsigned long>(root_before.lba),
                 static_cast<unsigned long>(after->root_range_base.lba),
                 batches,
                 ops_total);
}

void
coverage_f6_cross_gen_winner(steady_fixture& fx,
                             oracle& o,
                             std::vector<std::string>& samples) {
    const auto key = key_for_owner(0, fx.front_count(), "f6-winner", 160);
    samples.push_back(key);
    fx.write_put(key, "f6-v1", o);
    CHECK(expect_ok<pipeline::seal_round_result>(fx.run_seal()).cat1 !=
          nullptr);
    fx.write_put(key, "f6-v2", o);
    CHECK(expect_ok<pipeline::seal_round_result>(fx.run_seal()).cat1 !=
          nullptr);
    fx.write_put(key, "f6-v3", o);
    {
        auto flush = expect_ok<pipeline::flush_round_result>(fx.run_flush());
        CHECK(!flush.noop);
    }
    core::registry::wal_reclaim_frontier_singleton()->publish_exact_min(
        core::wal_reclaim_frontier::no_unreclaimed_lsn);
    fx.drain_maintenance();
    expect_found(expect_ok<pipeline::point_get_result>(fx.run_point_get(key)),
                 "f6-v3");
    std::fprintf(stderr,
                 "[F6] same key across sealed gens read latest winner f6-v3\n");
}

void
drive_wal_reclaim_probe(steady_fixture& fx, oracle& o) {
    auto* wal_space = core::registry::wal_space_singleton();
    uint32_t writes = 0;
    const auto target_sealed = static_cast<std::size_t>(fx.front_count());
    while (wal_space->sealed_segment_count_for_testing() < target_sealed &&
           writes < 1200) {
        auto key = key_for_owner(writes % fx.front_count(),
                                 fx.front_count(),
                                 "wal-probe-" + pad_u32(writes),
                                 930);
        fx.write_del(key, o);
        ++writes;
    }
    CHECK(wal_space->sealed_segment_count_for_testing() >= target_sealed);
    const auto sealed_before = wal_space->sealed_segment_count_for_testing();
    const auto free_before = wal_space->free_pool_count_for_testing();
    const auto global_min_before =
        core::registry::wal_reclaim_frontier_singleton()
            ->global_min_unreclaimed_lsn.load(std::memory_order_acquire);
    fx.seal_flush_drain();
    CHECK(wal_space->sealed_segment_count_for_testing() < sealed_before);
    CHECK(wal_space->free_pool_count_for_testing() > free_before);
    auto snapshot =
        expect_ok<tree::recovery_frontier_snapshot>(
            fx.run_recompute_frontier());
    if (snapshot.recovery_safe_lsn == 0) {
        const auto global_min_after =
            core::registry::wal_reclaim_frontier_singleton()
                ->global_min_unreclaimed_lsn.load(std::memory_order_acquire);
        std::fprintf(stderr,
                     "[WAL debug] writes=%u sealed %zu->%zu free %zu->%zu "
                     "frontier=%lu recovery=%lu global_min %lu->%lu "
                     "owner_recovery=%lu\n",
                     writes,
                     sealed_before,
                     wal_space->sealed_segment_count_for_testing(),
                     free_before,
                     wal_space->free_pool_count_for_testing(),
                     static_cast<unsigned long>(
                         snapshot.flush_durable_frontier),
                     static_cast<unsigned long>(snapshot.recovery_safe_lsn),
                     static_cast<unsigned long>(global_min_before),
                     static_cast<unsigned long>(global_min_after),
                     static_cast<unsigned long>(
                         rt::owner()->state.recovery_safe_lsn));
    }
    CHECK(snapshot.flush_durable_frontier > 0);
    CHECK(snapshot.recovery_safe_lsn > 0);
    CHECK(snapshot.recovery_safe_lsn <= snapshot.flush_durable_frontier);
    std::fprintf(stderr,
                 "[WAL] sealed segments reclaimed after %u long delete "
                 "writes\n",
                 writes);
}

void
steady_long_run(steady_fixture& fx,
                oracle& o,
                std::vector<std::string>& samples) {
    const auto stats_before = rt::value_reclaim_stats();
    const auto tree_head_before = fx.tree_head_lba();
    const auto value_head_before = fx.value_head_lba();
    uint64_t last_recovery_safe = rt::owner()->state.recovery_safe_lsn;

    std::vector<core::read_handle> old_handles;
    for (uint32_t round = 0; round < kSteadyRounds; ++round) {
        std::vector<core::raw_batch_op> ops;
        for (uint32_t i = 0; i < 6; ++i) {
            const uint32_t selector = (round + i) % 5;
            if (!samples.empty() && selector <= 2) {
                const auto& key = samples[(round * 7 + i) % samples.size()];
                if (selector == 2) {
                    ops.push_back(del_op(key));
                } else {
                    ops.push_back(put_op(
                        key,
                        "steady-r" + std::to_string(round) +
                            "-i" + std::to_string(i)));
                }
            } else {
                const uint32_t owner = (round + i) % fx.front_count();
                auto key = key_for_owner(owner,
                                         fx.front_count(),
                                         "steady-new-" + pad_u32(round) +
                                             "-" + pad_u32(i),
                                         180);
                samples.push_back(key);
                ops.push_back(put_op(
                    key,
                    "steady-new-v" + std::to_string(round) +
                        "-" + std::to_string(i)));
            }
        }

        if ((round % 4) == 0) {
            old_handles.push_back(fx.acquire_handle());
        }
        fx.write_ops(std::move(ops), o);
        fx.seal_flush_drain();
        if (old_handles.size() > 2) {
            old_handles.erase(old_handles.begin());
        }

        auto snapshot =
            expect_ok<tree::recovery_frontier_snapshot>(
                fx.run_recompute_frontier());
        if (snapshot.recovery_safe_lsn < last_recovery_safe) {
            const auto global_min =
                core::registry::wal_reclaim_frontier_singleton()
                    ->global_min_unreclaimed_lsn.load(
                        std::memory_order_acquire);
            std::fprintf(stderr,
                         "[steady debug] round=%u recovery %lu->%lu "
                         "frontier=%lu global_min=%lu owner_recovery=%lu\n",
                         round,
                         static_cast<unsigned long>(last_recovery_safe),
                         static_cast<unsigned long>(
                             snapshot.recovery_safe_lsn),
                         static_cast<unsigned long>(
                             snapshot.flush_durable_frontier),
                         static_cast<unsigned long>(global_min),
                         static_cast<unsigned long>(
                             rt::owner()->state.recovery_safe_lsn));
        }
        CHECK(snapshot.recovery_safe_lsn >= last_recovery_safe);
        last_recovery_safe = snapshot.recovery_safe_lsn;
        core::registry::wal_reclaim_frontier_singleton()->publish_exact_min(
            core::wal_reclaim_frontier::no_unreclaimed_lsn);
        fx.drain_maintenance();
        CHECK(fx.current_imm_count() <= fx.front_count());
        CHECK(rt::value_reclaim_stats().partial_into_untracked == 0);
        CHECK(fx.reclaim_idle());

        std::vector<std::string> probe;
        for (std::size_t i = round % 3; i < samples.size() && probe.size() < 16;
             i += 5) {
            probe.push_back(samples[i]);
        }
        expect_oracle(fx, o, std::span<const std::string>(probe.data(),
                                                         probe.size()));
    }
    old_handles.clear();
    fx.drain_maintenance();

    const auto stats_after = rt::value_reclaim_stats();
    CHECK(stats_after.reclaim_total_refs > stats_before.reclaim_total_refs);
    CHECK(stats_after.partial_into_untracked == 0);
    CHECK(fx.tree_head_lba() <= tree_head_before + 160);
    CHECK(value_head_before >= fx.value_head_lba());
    CHECK(value_head_before - fx.value_head_lba() <= 96);
    CHECK(fx.current_imm_count() <= fx.front_count());
    CHECK(fx.reclaim_idle());
    std::fprintf(stderr,
                 "[steady] rounds=%u samples=%zu tree_head_delta=%ld "
                 "value_head_delta=%ld value_reclaims=%lu free_ranges=%zu\n",
                 kSteadyRounds,
                 samples.size(),
                 static_cast<long>(fx.tree_head_lba() - tree_head_before),
                 static_cast<long>(value_head_before - fx.value_head_lba()),
                 static_cast<unsigned long>(stats_after.reclaim_total_refs),
                 fx.tree_free_range_count());
}

void
run_steady_e2e(const std::string& pci_addr) {
    steady_fixture fx(pci_addr, {0, 1});
    oracle o;
    std::vector<std::string> samples;

    coverage_f7_delete_and_empty_leaf(fx, o, samples);
    coverage_f8_noop_round(fx);

    auto seed_keys = bootstrap_split_tree(fx, o);
    samples.insert(samples.end(), seed_keys.begin(), seed_keys.end());
    const auto target = coverage_f1_root_stable(fx, o, seed_keys);
    coverage_f2_shadow_cow_cross_round(fx, o, target);
    const auto freed_range = coverage_f3_slot_exhaustion_consolidates(
        fx, o, target);
    coverage_f4_leaf_split_reuses_free_range(fx, o, samples, freed_range);
    coverage_f5_root_change(fx, o, samples);
    coverage_f6_cross_gen_winner(fx, o, samples);
    drive_wal_reclaim_probe(fx, o);
    steady_long_run(fx, o, samples);

    expect_oracle(fx, o, std::span<const std::string>(samples.data(),
                                                     samples.size()));
}

}  // namespace

int main(int argc, char** argv) {
    // Line-buffer stdout so phase progress reaches the log before a
    // mid-phase std::abort() (CHECK) — block buffering swallows it.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    std::string pci_addr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--pci-addr" && i + 1 < argc) {
            pci_addr = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage: %s --pci-addr BDF   (or set INCONEL_NVME_PCI_ADDR)\n"
                "  WARNING: the target NVMe namespace is force-formatted on "
                "every run; point it at a scratch device only.\n",
                argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            return 2;
        }
    }
    if (pci_addr.empty()) {
        if (const char* env = std::getenv("INCONEL_NVME_PCI_ADDR")) {
            pci_addr = env;
        }
    }
    if (pci_addr.empty()) {
        std::fprintf(stderr,
            "--pci-addr BDF or INCONEL_NVME_PCI_ADDR is required "
            "(the device is force-formatted; use a scratch namespace)\n");
        return 2;
    }

    std::printf("test_steady_e2e: pci=%s rounds=%u shadow_slots=%u "
                "cores=[0,1]\n",
                pci_addr.c_str(), kSteadyRounds, kShadowSlots);
    run_steady_e2e(pci_addr);
    std::printf("all passed\n");
    return 0;
}
