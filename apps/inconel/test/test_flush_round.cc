#include "apps/inconel/runtime/operations.hh"
#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/coord/sender.hh"
#include "apps/inconel/core/memtable_lookup.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/tree_read_domain.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/layout_plan.hh"
#include "apps/inconel/format/superblock_builder.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/tree/scheduler.hh"
#include "pump/core/context.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

using namespace apps::inconel;

namespace {

constexpr uint32_t kLbaSize = format::kBootstrapFormatProfile.lba_size;
constexpr uint64_t kNamespaceLbas =
    format::kBootstrapFormatProfile.value_data_area_end.lba + 10000;

using tree_cache_t = core::segmented_clock_cache;
using value_cache_t = core::segmented_clock_cache;
using runtime_t = runtime::inconel_runtime_t<tree_cache_t, value_cache_t>;
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

template <typename Exc, typename T>
void
expect_error_contains(op_result<T>&& result, std::string_view needle) {
    CHECK(std::holds_alternative<std::exception_ptr>(result));
    try {
        std::rethrow_exception(std::get<std::exception_ptr>(result));
    } catch (const Exc& e) {
        const std::string msg = e.what();
        CHECK(msg.find(needle) != std::string::npos);
        return;
    } catch (...) {
    }
    CHECK(false);
}

bool
same_paddr(format::paddr a, format::paddr b) noexcept {
    return a.device_id == b.device_id && a.lba == b.lba;
}

bool
same_value_ref(format::value_ref a, format::value_ref b) noexcept {
    return same_paddr(a.base, b.base) &&
           a.byte_offset == b.byte_offset &&
           a.len == b.len &&
           a.flags == b.flags;
}

std::string
key_for_owner(uint32_t owner, uint32_t front_count, std::string_view prefix) {
    for (uint32_t i = 0; i < 200000; ++i) {
        std::string key = std::string(prefix) + "-" + std::to_string(i);
        if (static_cast<uint32_t>(core::key_hash(key) % front_count) ==
            owner) {
            return key;
        }
    }
    CHECK(false);
    return {};
}

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

    const auto sb_a = format::build_superblock(plan, 1);
    const auto sb_b = format::build_superblock(plan, 0);

    std::vector<char> page(kLbaSize, 0);
    std::memcpy(page.data(), &sb_a, sizeof(sb_a));
    CHECK(device.write_bytes(0, std::span<const char>(page.data(),
                                                      page.size())));
    std::fill(page.begin(), page.end(), 0);
    std::memcpy(page.data(), &sb_b, sizeof(sb_b));
    CHECK(device.write_bytes(1, std::span<const char>(page.data(),
                                                      page.size())));
}

struct e2e_fixture {
    std::vector<uint32_t> cores;
    nvme::runtime_device device{kLbaSize, kNamespaceLbas, 0};
    runtime_t* rt = nullptr;

    explicit e2e_fixture(std::vector<uint32_t> cores_in = {0, 1})
        : cores(std::move(cores_in)) {
        format_mock_superblocks(device);
        pump::core::this_core_id = 0;
        runtime::build_options bopts{
            .cores = std::span<const uint32_t>(cores.data(), cores.size()),
            .device = &device,
        };
        rt = runtime::build_runtime<tree_cache_t, value_cache_t>(bopts);
        pump::core::this_core_id = 0;
    }

    ~e2e_fixture() {
        runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt);
    }

    e2e_fixture(const e2e_fixture&) = delete;
    e2e_fixture& operator=(const e2e_fixture&) = delete;

    bool advance_all(bool include_tree = true) {
        bool progress = false;
        for (uint32_t core : cores) {
            std::apply(
                [&](auto*... sched) {
                    auto step = [&](auto* s) {
                        if (s == nullptr) return;
                        using sched_t =
                            std::remove_cv_t<std::remove_pointer_t<decltype(s)>>;
                        if constexpr (std::is_same_v<sched_t, tree::tree_sched> ||
                                      std::is_base_of_v<
                                          core::tree_read_domain_base,
                                          sched_t>) {
                            if (include_tree) progress |= s->advance();
                        } else {
                            progress |= s->advance();
                        }
                    };
                    (step(sched), ...);
                },
                rt->schedulers_by_core[core]);
        }
        return progress;
    }

    bool advance_coord_once() {
        return core::registry::coord_sched_singleton()->advance();
    }

    template <typename Submission>
    void drive_until_ready(Submission& sub,
                           bool include_tree = true,
                           uint32_t limit = 800000) {
        for (uint32_t i = 0; !ready(sub.fut) && i < limit; ++i) {
            if (!advance_all(include_tree)) std::this_thread::yield();
        }
        CHECK(ready(sub.fut));
    }

    [[nodiscard]] core::client_batch_buffer
    make_input(std::vector<core::raw_batch_op> ops) const {
        return core::encode_client_batch(
            std::span<const core::raw_batch_op>(ops.data(), ops.size()));
    }

    submission<write_path::write_batch_result>
    submit_write(std::vector<core::raw_batch_op> ops) {
        return submit_result<write_path::write_batch_result>(
            [this, input = make_input(std::move(ops))]() mutable {
                return rt::write_batch(std::move(input));
            });
    }

    op_result<write_path::write_batch_result>
    run_write(std::vector<core::raw_batch_op> ops) {
        auto sub = submit_write(std::move(ops));
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

    submission<pipeline::seal_round_result>
    submit_seal() {
        return submit_result<pipeline::seal_round_result>(
            []() { return rt::seal_once(); });
    }

    op_result<pipeline::seal_round_result>
    run_seal() {
        auto sub = submit_seal();
        drive_until_ready(sub);
        return sub.fut.get();
    }

    submission<pipeline::flush_round_result>
    submit_flush() {
        return submit_result<pipeline::flush_round_result>(
            []() { return rt::flush_once(); });
    }

    op_result<pipeline::flush_round_result>
    run_flush() {
        auto sub = submit_flush();
        drive_until_ready(sub);
        return sub.fut.get();
    }

    [[nodiscard]] core::read_handle current_handle() const {
        return core::registry::coord_sched_singleton()
            ->acquire_read_handle_for_testing();
    }

    [[nodiscard]] uint64_t cat_epoch() const {
        return core::registry::coord_sched_singleton()->cat_epoch();
    }

    [[nodiscard]] bool gate_open() const {
        return core::registry::coord_sched_singleton()
            ->gate_open_for_testing();
    }

    [[nodiscard]] uint32_t front_count() const {
        return core::registry::front_count();
    }

    [[nodiscard]] uint32_t owner_for_key(std::string_view key) const {
        return static_cast<uint32_t>(core::key_hash(key) % front_count());
    }

    [[nodiscard]] core::memtable_lookup_result
    lookup_with_handle(const core::read_handle& rh, std::string_view key) {
        const uint32_t owner = owner_for_key(key);
        return core::registry::front_at(owner)->lookup_memtable_for_testing(
            key, rh.read_lsn, (*rh.cat->prs->fronts)[owner]);
    }

    [[nodiscard]] std::optional<format::value_ref>
    visible_value_ref(std::string_view key) {
        auto result = lookup_with_handle(current_handle(), key);
        if (auto* hit = std::get_if<core::memtable_value_hit>(&result)) {
            return hit->durable;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string device_value_body(const format::value_ref& vr) {
        const uint32_t total =
            static_cast<uint32_t>(sizeof(format::value_object_header)) +
            vr.len;
        const uint32_t span_lbas =
            (vr.byte_offset + total + kLbaSize - 1) / kLbaSize;
        std::vector<char> page(
            static_cast<std::size_t>(span_lbas) * kLbaSize);
        CHECK(device.read_bytes(vr.base.lba,
                                std::span<char>(page.data(), page.size())));
        auto decoded = format::decode_value_object(
            std::span<const char>(page.data() + vr.byte_offset,
                                  page.size() - vr.byte_offset),
            vr.len);
        CHECK(decoded.status == format::value_decode_status::ok);
        return std::string(decoded.body.data(), decoded.body.size());
    }
};

std::vector<std::vector<uint64_t>>
imm_ids_by_front(const core::read_handle& rh) {
    std::vector<std::vector<uint64_t>> out;
    out.resize(rh.cat->prs->fronts->size());
    for (std::size_t owner = 0; owner < rh.cat->prs->fronts->size(); ++owner) {
        for (const auto& gen : (*rh.cat->prs->fronts)[owner].imms) {
            CHECK(gen != nullptr);
            out[owner].push_back(gen->gen_id);
        }
    }
    return out;
}

bool
contains_id(const std::vector<uint64_t>& ids, uint64_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void
expect_ids_absent_from_prs(const core::read_handle& rh,
                           const std::vector<std::vector<uint64_t>>& ids) {
    for (std::size_t owner = 0; owner < ids.size(); ++owner) {
        const auto& imms = (*rh.cat->prs->fronts)[owner].imms;
        for (uint64_t id : ids[owner]) {
            for (const auto& gen : imms) {
                CHECK(gen != nullptr);
                CHECK(gen->gen_id != id);
            }
        }
    }
}

void
expect_ids_absent_from_local_fronts(
    const std::vector<std::vector<uint64_t>>& ids) {
    for (std::size_t owner = 0; owner < ids.size(); ++owner) {
        const auto& imms =
            core::registry::front_at(static_cast<uint32_t>(owner))
                ->imms_for_testing();
        for (uint64_t id : ids[owner]) {
            for (const auto& gen : imms) {
                CHECK(gen != nullptr);
                CHECK(gen->gen_id != id);
            }
        }
    }
}

void
write_ops_in_batches(e2e_fixture& fx,
                     const std::vector<core::raw_batch_op>& ops,
                     std::size_t batch_size = 32) {
    for (std::size_t off = 0; off < ops.size(); off += batch_size) {
        const std::size_t n = std::min(batch_size, ops.size() - off);
        std::vector<core::raw_batch_op> batch;
        batch.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            batch.push_back(ops[off + i]);
        }
        (void)expect_ok<write_path::write_batch_result>(
            fx.run_write(std::move(batch)));
    }
}

bool
retired_contains_value(const core::retired_objects& retired,
                       format::value_ref vr,
                       uint64_t data_ver) {
    for (const auto& item : retired.old_tree_values) {
        if (item.data_ver == data_ver && same_value_ref(item.vr, vr)) {
            return true;
        }
    }
    return false;
}

bool
retired_contains_range_base(const core::retired_objects& retired,
                            format::paddr base) {
    for (const auto& r : retired.old_ranges) {
        if (same_paddr(r.base, base)) return true;
    }
    return false;
}

std::vector<core::retired_value_ref>
drain_losers(const std::shared_ptr<core::memtable_gen>& gen) {
    std::vector<core::retired_value_ref> out;
    gen->loser_durable_refs.drain([&](core::retired_value_ref rv) {
        out.push_back(rv);
    });
    return out;
}

bool
losers_contain(const std::vector<core::retired_value_ref>& losers,
               format::value_ref vr,
               uint64_t data_ver) {
    for (const auto& item : losers) {
        if (item.data_ver == data_ver && same_value_ref(item.vr, vr)) {
            return true;
        }
    }
    return false;
}

void
verify_expected(e2e_fixture& fx,
                const std::map<std::string, std::optional<std::string>>& exp) {
    for (const auto& [key, value] : exp) {
        auto got = expect_ok<pipeline::point_get_result>(
            fx.run_point_get(key));
        if (value.has_value()) {
            expect_found(got, *value);
        } else {
            expect_not_found(got);
        }
    }
}

void
flush_round_single_visibility_and_imms() {
    e2e_fixture fx;
    struct kv {
        std::string key;
        std::string value;
    };
    std::vector<kv> data = {
        {key_for_owner(0, fx.front_count(), "fr-vis-a"), "value-a"},
        {key_for_owner(1, fx.front_count(), "fr-vis-b"), "value-b"},
        {key_for_owner(0, fx.front_count(), "fr-vis-c"), "value-c"},
        {key_for_owner(1, fx.front_count(), "fr-vis-d"), "value-d"},
    };

    std::vector<core::raw_batch_op> ops;
    for (const auto& item : data) {
        ops.push_back({.op = core::write_op_type::put,
                       .key = item.key,
                       .value = item.value});
    }
    (void)expect_ok<write_path::write_batch_result>(
        fx.run_write(std::move(ops)));
    auto seal = expect_ok<pipeline::seal_round_result>(fx.run_seal());
    CHECK(seal.cat1 != nullptr);

    auto old_rh = fx.current_handle();
    const auto flushed_ids = imm_ids_by_front(old_rh);
    for (const auto& ids : flushed_ids) CHECK(ids.size() == 1);
    auto old_guard = old_rh.cat->prs->tree_guard;
    const uint64_t old_epoch = old_rh.cat->epoch;

    auto flush = expect_ok<pipeline::flush_round_result>(fx.run_flush());
    CHECK(!flush.noop);

    auto new_rh = fx.current_handle();
    CHECK(new_rh.cat->epoch == old_epoch + 1);
    CHECK(new_rh.cat->prs->tree_guard != old_guard);
    CHECK(new_rh.cat->prs->tree_guard->manifest->has_root());
    expect_ids_absent_from_prs(new_rh, flushed_ids);
    expect_ids_absent_from_local_fronts(flushed_ids);

    for (const auto& item : data) {
        CHECK(std::holds_alternative<core::memtable_miss>(
            fx.lookup_with_handle(new_rh, item.key)));
        expect_found(expect_ok<pipeline::point_get_result>(
                         fx.run_point_get(item.key)),
                     item.value);

        auto old_lookup = fx.lookup_with_handle(old_rh, item.key);
        auto* hit = std::get_if<core::memtable_value_hit>(&old_lookup);
        CHECK(hit != nullptr);
        CHECK(fx.device_value_body(hit->durable) == item.value);
    }
}

void
flush_round_durable_lsn_install_inherits_latest() {
    e2e_fixture fx;
    const std::string old_key =
        key_for_owner(0, fx.front_count(), "fr-durable-old");
    const std::string new_key =
        key_for_owner(1, fx.front_count(), "fr-durable-new");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = old_key, .value = "old"},
    }));
    (void)expect_ok<pipeline::seal_round_result>(fx.run_seal());

    const auto before = fx.current_handle();
    const uint64_t capture_floor = before.read_lsn;
    auto flush = fx.submit_flush();
    CHECK(fx.advance_coord_once());
    CHECK(!ready(flush.fut));

    auto write_new = fx.submit_write({
        {.op = core::write_op_type::put, .key = new_key, .value = "new"},
    });
    fx.drive_until_ready(write_new, false);
    auto wr = expect_ok<write_path::write_batch_result>(write_new.fut.get());
    CHECK(wr.batch_lsn > capture_floor);
    CHECK(fx.current_handle().read_lsn >= wr.batch_lsn);

    fx.drive_until_ready(flush);
    auto fr = expect_ok<pipeline::flush_round_result>(flush.fut.get());
    CHECK(!fr.noop);

    auto after = fx.current_handle();
    CHECK(after.read_lsn >= capture_floor);
    CHECK(after.read_lsn >= wr.batch_lsn);
    CHECK(after.cat->durable_lsn.load(std::memory_order_acquire) >=
          wr.batch_lsn);
    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_point_get(old_key)),
                 "old");
    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_point_get(new_key)),
                 "new");
}

void
flush_round_serialization_flush_vs_flush() {
    e2e_fixture fx;
    const std::string key =
        key_for_owner(0, fx.front_count(), "fr-serial");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "v"},
    }));
    (void)expect_ok<pipeline::seal_round_result>(fx.run_seal());

    const auto before = fx.current_handle();
    const auto flushed_ids = imm_ids_by_front(before);

    auto first = fx.submit_flush();
    CHECK(fx.advance_coord_once());
    CHECK(!ready(first.fut));

    auto second = fx.submit_flush();
    fx.drive_until_ready(second, false);
    expect_error_contains<std::logic_error>(
        second.fut.get(), "catalog_update_in_progress");

    fx.drive_until_ready(first);
    auto first_result =
        expect_ok<pipeline::flush_round_result>(first.fut.get());
    CHECK(!first_result.noop);

    const uint64_t epoch_after_first = fx.cat_epoch();
    auto serial_second =
        expect_ok<pipeline::flush_round_result>(fx.run_flush());
    CHECK(serial_second.noop);
    CHECK(fx.cat_epoch() == epoch_after_first);
    expect_ids_absent_from_local_fronts(flushed_ids);
}

void
flush_round_retired_and_losers_accumulate() {
    e2e_fixture fx;
    const std::string key =
        key_for_owner(0, fx.front_count(), "fr-retired");
    const uint32_t owner = fx.owner_for_key(key);

    auto wr0 = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "v1"},
    }));
    auto v1 = fx.visible_value_ref(key);
    CHECK(v1.has_value());
    (void)expect_ok<pipeline::seal_round_result>(fx.run_seal());
    CHECK(!expect_ok<pipeline::flush_round_result>(fx.run_flush()).noop);

    auto wr1 = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "v2"},
    }));
    auto v2 = fx.visible_value_ref(key);
    CHECK(v2.has_value());
    (void)expect_ok<pipeline::seal_round_result>(fx.run_seal());
    const auto gen_v2 =
        core::registry::front_at(owner)->imms_for_testing().front();
    CHECK(gen_v2 != nullptr);

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "v3"},
    }));
    (void)expect_ok<pipeline::seal_round_result>(fx.run_seal());

    auto base = fx.current_handle();
    auto old_guard = base.cat->prs->tree_guard;
    const auto old_root_range = old_guard->manifest->root_range_base;
    const std::size_t old_slots_before =
        old_guard->retired.old_slots.size();
    const std::size_t old_ranges_before =
        old_guard->retired.old_ranges.size();
    const std::size_t old_values_before =
        old_guard->retired.old_tree_values.size();

    CHECK(!expect_ok<pipeline::flush_round_result>(fx.run_flush()).noop);

    CHECK(old_guard->retired.old_slots.size() == old_slots_before);
    CHECK(old_guard->retired.old_ranges.size() == old_ranges_before + 1);
    CHECK(old_guard->retired.old_tree_values.size() == old_values_before + 1);
    CHECK(retired_contains_range_base(old_guard->retired, old_root_range));
    CHECK(retired_contains_value(
        old_guard->retired, *v1, wr0.batch_lsn));

    auto losers = drain_losers(gen_v2);
    CHECK(losers_contain(losers, *v2, wr1.batch_lsn));

    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_point_get(key)),
                 "v3");
}

void
flush_round_noop_keeps_cat_and_imms() {
    e2e_fixture fx;
    auto before = fx.current_handle();
    const auto ids_before = imm_ids_by_front(before);
    const uint64_t epoch_before = fx.cat_epoch();

    auto first = expect_ok<pipeline::flush_round_result>(fx.run_flush());
    CHECK(first.noop);

    auto after = fx.current_handle();
    CHECK(after.cat == before.cat);
    CHECK(fx.cat_epoch() == epoch_before);
    CHECK(imm_ids_by_front(after) == ids_before);

    auto second = expect_ok<pipeline::flush_round_result>(fx.run_flush());
    CHECK(second.noop);
    CHECK(fx.cat_epoch() == epoch_before);
}

void
flush_round_flush_vs_seal_interlock() {
    e2e_fixture fx;
    const std::string k0 =
        key_for_owner(0, fx.front_count(), "fr-b4-a");
    const std::string k1 =
        key_for_owner(1, fx.front_count(), "fr-b4-b");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = k0, .value = "a"},
    }));

    auto seal = fx.submit_seal();
    CHECK(fx.advance_coord_once());
    CHECK(!fx.gate_open());

    auto blocked_flush = fx.submit_flush();
    fx.drive_until_ready(blocked_flush, false);
    expect_error_contains<std::logic_error>(
        blocked_flush.fut.get(), "catalog_update_in_progress");

    fx.drive_until_ready(seal);
    CHECK(expect_ok<pipeline::seal_round_result>(seal.fut.get()).cat1 !=
          nullptr);
    CHECK(fx.gate_open());

    auto flush = fx.submit_flush();
    CHECK(fx.advance_coord_once());
    CHECK(!ready(flush.fut));

    auto blocked_seal = fx.submit_seal();
    fx.drive_until_ready(blocked_seal, false);
    expect_error_contains<std::logic_error>(
        blocked_seal.fut.get(), "catalog_update_in_progress");

    fx.drive_until_ready(flush);
    CHECK(!expect_ok<pipeline::flush_round_result>(flush.fut.get()).noop);
    CHECK(fx.gate_open());

    const uint64_t epoch0 = fx.cat_epoch();
    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = k1, .value = "b"},
    }));
    (void)expect_ok<pipeline::seal_round_result>(fx.run_seal());
    const uint64_t epoch1 = fx.cat_epoch();
    CHECK(epoch1 > epoch0);
    CHECK(!expect_ok<pipeline::flush_round_result>(fx.run_flush()).noop);
    CHECK(fx.cat_epoch() > epoch1);
    CHECK(fx.gate_open());
}

void
flush_round_multiround_root_change_and_winners() {
    e2e_fixture fx;
    constexpr uint32_t kBaseKeys = 512;
    std::vector<std::string> keys;
    keys.reserve(kBaseKeys + 64);
    std::map<std::string, std::optional<std::string>> expected;

    std::vector<core::raw_batch_op> r1;
    for (uint32_t i = 0; i < kBaseKeys; ++i) {
        std::string key = "fr-multi-base-" + std::to_string(i);
        std::string value = "r1-" + std::to_string(i);
        keys.push_back(key);
        expected[key] = value;
        r1.push_back({.op = core::write_op_type::put,
                      .key = key,
                      .value = value});
    }
    write_ops_in_batches(fx, r1);
    (void)expect_ok<pipeline::seal_round_result>(fx.run_seal());
    CHECK(!expect_ok<pipeline::flush_round_result>(fx.run_flush()).noop);

    auto grown = fx.current_handle().cat->prs->tree_guard->manifest;
    CHECK(grown->has_root());
    CHECK(grown->leaf_order.spans.size() > 1);
    CHECK(!grown->reverse_topology.internal_nodes.empty());

    std::vector<core::raw_batch_op> r2;
    for (uint32_t i = 0; i < 80; ++i) {
        std::string value = "r2-over-" + std::to_string(i);
        expected[keys[i]] = value;
        r2.push_back({.op = core::write_op_type::put,
                      .key = keys[i],
                      .value = value});
    }
    for (uint32_t i = 80; i < 160; ++i) {
        expected[keys[i]] = std::nullopt;
        r2.push_back({.op = core::write_op_type::del,
                      .key = keys[i],
                      .value = ""});
    }
    for (uint32_t i = 0; i < 64; ++i) {
        std::string key = "fr-multi-new-" + std::to_string(i);
        std::string value = "r2-new-" + std::to_string(i);
        keys.push_back(key);
        expected[key] = value;
        r2.push_back({.op = core::write_op_type::put,
                      .key = key,
                      .value = value});
    }
    write_ops_in_batches(fx, r2);
    (void)expect_ok<pipeline::seal_round_result>(fx.run_seal());
    CHECK(!expect_ok<pipeline::flush_round_result>(fx.run_flush()).noop);

    std::vector<core::raw_batch_op> r3;
    for (uint32_t i = 0; i < 40; ++i) {
        std::string value = "r3-over-" + std::to_string(i);
        expected[keys[i]] = value;
        r3.push_back({.op = core::write_op_type::put,
                      .key = keys[i],
                      .value = value});
    }
    for (uint32_t i = 100; i < 130; ++i) {
        std::string value = "r3-resurrect-" + std::to_string(i);
        expected[keys[i]] = value;
        r3.push_back({.op = core::write_op_type::put,
                      .key = keys[i],
                      .value = value});
    }
    for (uint32_t i = kBaseKeys; i < kBaseKeys + 20; ++i) {
        expected[keys[i]] = std::nullopt;
        r3.push_back({.op = core::write_op_type::del,
                      .key = keys[i],
                      .value = ""});
    }
    write_ops_in_batches(fx, r3);
    (void)expect_ok<pipeline::seal_round_result>(fx.run_seal());
    CHECK(!expect_ok<pipeline::flush_round_result>(fx.run_flush()).noop);

    verify_expected(fx, expected);
}

}  // namespace

int
main() {
    flush_round_single_visibility_and_imms();
    flush_round_durable_lsn_install_inherits_latest();
    flush_round_serialization_flush_vs_flush();
    flush_round_retired_and_losers_accumulate();
    flush_round_noop_keeps_cat_and_imms();
    flush_round_flush_vs_seal_interlock();
    flush_round_multiround_root_change_and_winners();
    return 0;
}
