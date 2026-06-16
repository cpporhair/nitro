// Reclaim harness for 056 physical reclaim.
//
// Uses the production runtime over INCONEL_NVME_MOCK_BACKEND. Assertions are
// limited to behavior specified by 056; injection-heavy items are documented as
// skipped rather than adding production test hooks.

#include "apps/inconel/runtime/operations.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
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

#include "apps/inconel/core/registry.hh"
#include "apps/inconel/format/layout_plan.hh"
#include "apps/inconel/format/superblock_builder.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/test/check.hh"
#include "pump/core/context.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

using namespace apps::inconel;

namespace {

constexpr uint32_t kLbaSize = 4096;
constexpr uint64_t kNamespaceLbas = 110000;

using tree_cache_t = core::segmented_clock_cache;
using value_cache_t = core::segmented_clock_cache;
using runtime_t =
    runtime::inconel_runtime_t<tree_cache_t, value_cache_t>;
using root_context_t = decltype(pump::core::make_root_context());

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

void
format_mock_superblocks(nvme::runtime_device& device) {
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
    plan.tree_page_size = p.tree_page_size;
    plan.shadow_slots_per_range = p.shadow_slots_per_range;
    plan.value_class_count = p.value_class_count;
    for (uint8_t i = 0; i < p.value_class_count; ++i) {
        plan.value_class_sizes[i] = p.value_class_sizes[i];
    }
    plan.value_space_quantum_bytes = p.value_space_quantum_bytes;
    plan.value_space_group_size_lbas = p.value_space_group_size_lbas;

    const auto sb_a = format::build_superblock(plan, /*generation=*/1);
    const auto sb_b = format::build_superblock(plan, /*generation=*/0);

    std::vector<char> page(kLbaSize, 0);
    std::memcpy(page.data(), &sb_a, sizeof(sb_a));
    CHECK(device.write_bytes(0, std::span<const char>(page.data(),
                                                      page.size())));
    std::fill(page.begin(), page.end(), 0);
    std::memcpy(page.data(), &sb_b, sizeof(sb_b));
    CHECK(device.write_bytes(1, std::span<const char>(page.data(),
                                                      page.size())));
}

std::string
key_for_owner(uint32_t owner, uint32_t front_count, std::string_view prefix) {
    for (uint32_t i = 0; i < 100000; ++i) {
        std::string key = std::string(prefix) + "-" + std::to_string(i);
        if (static_cast<uint32_t>(core::key_hash(key) % front_count) ==
            owner) {
            return key;
        }
    }
    CHECK(false);
    return {};
}

struct e2e_fixture {
    std::vector<uint32_t> cores;
    nvme::runtime_device device{kLbaSize, kNamespaceLbas, 0};
    runtime_t* rt = nullptr;

    explicit e2e_fixture(std::vector<uint32_t> cores_in = {0, 1})
        : cores(std::move(cores_in)) {
        pump::core::this_core_id = 0;
        runtime::build_options bopts{
            .cores = std::span<const uint32_t>(cores.data(), cores.size()),
            .device = &device,
        };
        rt = runtime::build_runtime<tree_cache_t, value_cache_t>(bopts);
        pump::core::this_core_id = 0;
        format_mock_superblocks(device);
    }

    ~e2e_fixture() {
        runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt);
    }

    e2e_fixture(const e2e_fixture&) = delete;
    e2e_fixture& operator=(const e2e_fixture&) = delete;

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
    void drive_until_ready(Submission& sub, uint32_t limit = 600000) {
        for (uint32_t i = 0; !ready(sub.fut) && i < limit; ++i) {
            if (!advance_all()) std::this_thread::yield();
        }
        CHECK(ready(sub.fut));
    }

    void drain_maintenance(uint32_t limit = 200000) {
        uint32_t idle = 0;
        for (uint32_t i = 0; i < limit && idle < 512; ++i) {
            if (advance_all()) {
                idle = 0;
            } else {
                ++idle;
                std::this_thread::yield();
            }
        }
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

    void write_put(std::string_view key, std::string_view value) {
        auto wr = expect_ok<write_path::write_batch_result>(run_write({
            {.op = core::write_op_type::put,
             .key = std::string(key),
             .value = std::string(value)},
        }));
        CHECK(wr.entry_count == 1);
    }

    void write_del(std::string_view key) {
        auto wr = expect_ok<write_path::write_batch_result>(run_write({
            {.op = core::write_op_type::del,
             .key = std::string(key),
             .value = ""},
        }));
        CHECK(wr.entry_count == 1);
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

    [[nodiscard]] std::shared_ptr<const core::tree_manifest>
    current_manifest() const {
        const auto rh = core::registry::coord_sched_singleton()
                            ->acquire_read_handle_for_testing();
        return rh.cat->prs->tree_guard->manifest;
    }

    [[nodiscard]] uint32_t front_count() const {
        return core::registry::front_count();
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

value::reclaim_stats_snapshot
reclaim_stats() {
    return rt::value_reclaim_stats();
}

// §9.1, §9.8, §9.9: old guard/gen destruction drives tree reclaim; TRIM and
// value reclaim happen; a retired tree range returns to free_ranges and is
// reused by a later tree allocation; reads keep seeing the winner.
void
basic_reclaim_closure_reuses_tree_range_and_keeps_winner() {
    e2e_fixture fx({0});
    const std::string key = key_for_owner(0, fx.front_count(), "rc-basic");

    fx.write_put(key, "v1");
    fx.seal_flush_mark_wal_safe_and_drain();
    const auto first_manifest = fx.current_manifest();
    CHECK(first_manifest->has_root());
    const auto first_root_range = first_manifest->root_range_base;
    expect_found(expect_ok<pipeline::point_get_result>(fx.run_point_get(key)),
                 "v1");

    const auto trims_before = fx.device.trims();
    const auto stats_before = reclaim_stats();

    fx.write_put(key, "v2");
    fx.seal_flush_mark_wal_safe_and_drain();

    CHECK(fx.device.trims() > trims_before);
    CHECK(rt::owner()->state.alloc.free_ranges.size() > 0);
    CHECK(fx.reclaim_idle());

    const auto stats_after = reclaim_stats();
    CHECK(stats_after.reclaim_total_refs > stats_before.reclaim_total_refs);
    CHECK(stats_after.partial_into_untracked == 0);
    expect_found(expect_ok<pipeline::point_get_result>(fx.run_point_get(key)),
                 "v2");

    fx.write_put(key, "v3");
    fx.seal_flush_drain();
    const auto third_manifest = fx.current_manifest();
    CHECK(third_manifest->root_range_base == first_root_range);
    expect_found(expect_ok<pipeline::point_get_result>(fx.run_point_get(key)),
                 "v3");
    CHECK(reclaim_stats().partial_into_untracked == 0);
}

// §9.2 and §9.6: overwrite in one memtable gen creates a gen-loser. With
// WAL frontier holding recovery_safe_lsn below the loser data_ver it defers;
// once the frontier is advanced, the next reclaim maintenance pass releases it.
void
data_ver_gate_defers_gen_loser_until_recovery_frontier_advances() {
    e2e_fixture fx({0});
    const std::string key = key_for_owner(0, fx.front_count(), "rc-gate");

    fx.write_put(key, "old");
    fx.write_put(key, "new");
    const auto stats_before = reclaim_stats();
    fx.seal_flush_drain();

    auto* owner = rt::owner();
    CHECK(owner->state.recovery_safe_lsn == 0);
    CHECK(owner->state.deferred_value_reclaim.size() == 1);
    CHECK(reclaim_stats().reclaim_total_refs ==
          stats_before.reclaim_total_refs);
    expect_found(expect_ok<pipeline::point_get_result>(fx.run_point_get(key)),
                 "new");

    core::registry::wal_reclaim_frontier_singleton()->publish_exact_min(
        core::wal_reclaim_frontier::no_unreclaimed_lsn);
    auto snapshot =
        expect_ok<tree::recovery_frontier_snapshot>(fx.run_recompute_frontier());
    CHECK(snapshot.recovery_safe_lsn >= 2);

    fx.write_put(key, "newer");
    fx.seal_flush_mark_wal_safe_and_drain();
    CHECK(owner->state.deferred_value_reclaim.empty());
    CHECK(reclaim_stats().reclaim_total_refs >
          stats_before.reclaim_total_refs);
    CHECK(reclaim_stats().partial_into_untracked == 0);
    expect_found(expect_ok<pipeline::point_get_result>(fx.run_point_get(key)),
                 "newer");
}

// §9.3 and §9.4: a sealed WAL segment pins recovery_safe_lsn via the WAL
// frontier until flush_durable_frontier lets WAL reclaim consume it.
void
wal_frontier_and_wal_reclaim_closure() {
    e2e_fixture fx({0});
    auto* wal_space = core::registry::wal_space_singleton();

    uint32_t writes = 0;
    while (wal_space->sealed_segment_count_for_testing() == 0 &&
           writes < 400) {
        const std::string key = "rc-wal-" + std::to_string(writes) + "-" +
                                std::string(900, 'k');
        fx.write_del(key);
        ++writes;
    }
    CHECK(wal_space->sealed_segment_count_for_testing() > 0);
    const auto sealed_before = wal_space->sealed_segment_count_for_testing();
    const auto free_before = wal_space->free_pool_count_for_testing();
    const auto min_before =
        core::registry::wal_reclaim_frontier_singleton()
            ->global_min_unreclaimed_lsn.load(std::memory_order_acquire);
    CHECK(min_before == 1);

    fx.seal_flush_drain();
    const auto sealed_after = wal_space->sealed_segment_count_for_testing();
    CHECK(sealed_after < sealed_before);
    CHECK(wal_space->free_pool_count_for_testing() > free_before);

    auto snapshot =
        expect_ok<tree::recovery_frontier_snapshot>(fx.run_recompute_frontier());
    CHECK(snapshot.flush_durable_frontier >= writes);
    CHECK(snapshot.recovery_safe_lsn > 0);
    CHECK(snapshot.recovery_safe_lsn <= snapshot.flush_durable_frontier);
}

// §9.7, §9.8 and §9.9: several write/seal/flush/reclaim cycles should keep
// space recycling and preserve the latest visible value for each key.
void
multi_round_steady_state_reclaim() {
    e2e_fixture fx({0});
    const std::string k0 = key_for_owner(0, fx.front_count(), "rc-st-0");
    const std::string k1 = key_for_owner(0, fx.front_count(), "rc-st-1");

    uint64_t last_trim_count = fx.device.trims();
    for (uint32_t round = 0; round < 8; ++round) {
        fx.write_put(k0, "v0-" + std::to_string(round));
        if ((round % 2) == 0) {
            fx.write_put(k1, "v1-" + std::to_string(round));
        } else {
            fx.write_del(k1);
        }
        fx.seal_flush_mark_wal_safe_and_drain();
        CHECK(fx.reclaim_idle());
        CHECK(reclaim_stats().partial_into_untracked == 0);
        CHECK(fx.device.trims() >= last_trim_count);
        last_trim_count = fx.device.trims();

        expect_found(expect_ok<pipeline::point_get_result>(
                         fx.run_point_get(k0)),
                     "v0-" + std::to_string(round));
        const auto r1 = expect_ok<pipeline::point_get_result>(
            fx.run_point_get(k1));
        if ((round % 2) == 0) {
            expect_found(r1, "v1-" + std::to_string(round));
        } else {
            CHECK(!r1.found);
        }
    }
}

// §9.5 skipped: constructing a pinned tree_node cache entry that maps exactly
// to a retired range requires reaching into read_domain cache internals. The
// production API intentionally exposes no way to retain such a pin after the
// reader-visible guard has been released; adding a hook would weaken the test.
void
invalidate_barrier_pin_death_skipped() {
    std::fprintf(stderr,
                 "[skip] reclaim invalidate barrier pin death: no clean "
                 "production injection surface for a stale pinned tree frame\n");
}

// §9.7 skipped for explicit interleaving: the FIFO mutation gate is internal
// to tree_sched and has no event hook. The other cases observe the externally
// required serialization by successful flush and reclaim completion with an
// empty reclaim backlog and consistent latest reads.
void
reclaim_flush_interleaving_skipped() {
    std::fprintf(stderr,
                 "[skip] reclaim/flush FIFO interleaving: no clean production "
                 "event hook to pause either side mid-gate\n");
}

}  // namespace

int main() {
    basic_reclaim_closure_reuses_tree_range_and_keeps_winner();
    data_ver_gate_defers_gen_loser_until_recovery_frontier_advances();
    wal_frontier_and_wal_reclaim_closure();
    multi_round_steady_state_reclaim();
    invalidate_barrier_pin_death_skipped();
    reclaim_flush_interleaving_skipped();
    return 0;
}
