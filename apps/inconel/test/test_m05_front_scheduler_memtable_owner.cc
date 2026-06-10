#include "apps/inconel/test/check.hh"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "pump/core/context.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include "apps/inconel/core/batch_carrier.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/front/sender.hh"

using namespace apps::inconel;

namespace {

template <typename T>
concept has_hot_member = requires(T t) {
    t.hot;
};

using root_context_t = decltype(pump::core::make_root_context());
using insert_sender_t =
    decltype(front::insert_memtable_entries(
        std::declval<front::front_sched&>(),
        std::declval<core::front_fragment>(),
        std::declval<std::span<const core::canonical_entry>>()));
using lookup_sender_t =
    decltype(front::lookup_memtable(
        std::declval<front::front_sched&>(),
        std::string_view{},
        uint64_t{},
        std::declval<core::front_read_set>()));
using batch_lookup_sender_t =
    decltype(front::batch_lookup(
        std::declval<front::front_sched&>(),
        std::declval<std::span<const std::string_view>>(),
        uint64_t{},
        std::declval<core::front_read_set>()));
using scan_sender_t =
    decltype(front::scan_memtable(
        std::declval<front::front_sched&>(),
        std::string_view{},
        std::string_view{},
        uint64_t{},
        std::declval<core::front_read_set>()));
using seal_sender_t =
    decltype(front::seal_active(std::declval<front::front_sched&>()));
using collect_sender_t =
    decltype(front::collect_eligible_gens(
        std::declval<front::front_sched&>(), uint64_t{}));
using release_sender_t =
    decltype(front::release_gens(
        std::declval<front::front_sched&>(),
        std::declval<std::vector<uint64_t>>()));

static_assert(!has_hot_member<core::value_handle>);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  insert_sender_t>::count_value() == 0);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  lookup_sender_t>::count_value() == 1);
static_assert(std::same_as<
              decltype(pump::core::compute_sender_type<
                       root_context_t,
                       lookup_sender_t>::get_value_type_identity())::type,
              core::memtable_lookup_result>);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  batch_lookup_sender_t>::count_value() == 1);
static_assert(std::same_as<
              decltype(pump::core::compute_sender_type<
                       root_context_t,
                       batch_lookup_sender_t>::get_value_type_identity())::type,
              front::batch_lookup_result>);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  scan_sender_t>::count_value() == 1);
static_assert(std::same_as<
              decltype(pump::core::compute_sender_type<
                       root_context_t,
                       scan_sender_t>::get_value_type_identity())::type,
              core::memtable_scan_result>);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  seal_sender_t>::count_value() == 1);
static_assert(std::same_as<
              decltype(pump::core::compute_sender_type<
                       root_context_t,
                       seal_sender_t>::get_value_type_identity())::type,
              core::front_read_set>);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  collect_sender_t>::count_value() == 1);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  release_sender_t>::count_value() == 0);

struct downstream_marker {};

template <typename Exc, typename Fn>
void
expect_throws(Fn&& fn) {
    bool threw = false;
    try {
        std::forward<Fn>(fn)();
    } catch (const Exc&) {
        threw = true;
    }
    CHECK(threw);
}

format::value_ref
make_value_ref(uint64_t lba, uint32_t len = 128) {
    return format::value_ref{
        .base        = format::paddr{.device_id = 5, .lba = lba},
        .byte_offset = static_cast<uint16_t>(lba % 4096),
        .len         = len,
        .flags       = static_cast<uint16_t>(lba % 31),
    };
}

bool
same_value_ref(const format::value_ref& a,
               const format::value_ref& b) {
    return a.base.device_id == b.base.device_id &&
           a.base.lba == b.base.lba &&
           a.byte_offset == b.byte_offset &&
           a.len == b.len &&
           a.flags == b.flags;
}

std::span<const core::raw_batch_op>
op_span(const std::vector<core::raw_batch_op>& ops) {
    return {ops.data(), ops.size()};
}

std::span<const core::canonical_entry>
canonical_span(const core::batch_ctx& ctx) {
    return {ctx.canonical_entries.data(), ctx.canonical_entries.size()};
}

core::batch_ctx
make_ctx(std::vector<core::raw_batch_op> ops,
         uint64_t batch_lsn,
         uint32_t front_count) {
    auto ctx = core::build_batch_ctx(op_span(ops), batch_lsn, front_count);
    for (uint32_t idx : ctx.put_entry_indices) {
        ctx.canonical_entries[idx].allocated_vr =
            make_value_ref(batch_lsn * 100 + idx);
    }
    return ctx;
}

core::batch_ctx
make_single_front_ctx(std::vector<core::raw_batch_op> ops,
                      uint64_t batch_lsn) {
    return make_ctx(std::move(ops), batch_lsn, 1);
}

std::string
key_for_owner(uint32_t owner,
              uint32_t front_count,
              std::string_view prefix) {
    for (uint32_t i = 0; i < 10000; ++i) {
        std::string key = std::string(prefix) + "-" + std::to_string(i);
        if (static_cast<uint32_t>(core::key_hash(key) % front_count) ==
            owner) {
            return key;
        }
    }
    CHECK(false);
    return {};
}

const core::front_fragment&
only_fragment(const core::batch_ctx& ctx) {
    CHECK(ctx.fragments.size() == 1);
    return ctx.fragments[0];
}

core::front_read_set
current_snapshot(const front::front_sched& sched) {
    return core::front_read_set{
        .active = sched.active_for_testing(),
        .imms   = sched.imms_for_testing(),
    };
}

const core::memtable_value_hit&
expect_value(const core::memtable_lookup_result& result) {
    CHECK(std::holds_alternative<core::memtable_value_hit>(result));
    return std::get<core::memtable_value_hit>(result);
}

void
expect_miss(const core::memtable_lookup_result& result) {
    CHECK(std::holds_alternative<core::memtable_miss>(result));
}

void
expect_tombstone(const core::memtable_lookup_result& result) {
    CHECK(std::holds_alternative<core::memtable_tombstone>(result));
}

template <typename T, typename SenderBuilder>
T
submit_front_and_drive(front::front_sched& sched,
                       SenderBuilder&& build_sender) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<T>>();
    auto fut = promise->get_future();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::then([promise](auto&& r) mutable {
            promise->set_value(std::forward<decltype(r)>(r));
        })
        >> pump::sender::submit(ctx);

    for (uint32_t i = 0;
         fut.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready &&
         i < 128;
         ++i) {
        (void)sched.advance();
        std::this_thread::yield();
    }

    CHECK(fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    return fut.get();
}

template <typename SenderBuilder>
void
submit_front_void_and_drive(front::front_sched& sched,
                            SenderBuilder&& build_sender) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<void>>();
    auto fut = promise->get_future();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::then([promise]() mutable {
            promise->set_value();
        })
        >> pump::sender::submit(ctx);

    for (uint32_t i = 0;
         fut.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready &&
         i < 128;
         ++i) {
        (void)sched.advance();
        std::this_thread::yield();
    }

    CHECK(fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    fut.get();
}

void
insert_consumes_stable_fragment_indices_and_durable_refs() {
    constexpr uint32_t kFrontCount = 3;
    std::vector<core::raw_batch_op> ops;
    for (uint32_t i = 0; i < 24; ++i) {
        ops.push_back(core::raw_batch_op{
            .op = core::write_op_type::put,
            .key = "route-" + std::to_string(i),
            .value = "body-" + std::to_string(i),
        });
    }

    auto ctx = make_ctx(std::move(ops), 10, kFrontCount);
    CHECK(ctx.fragments.size() > 1);

    const auto& fragment = ctx.fragments.front();
    CHECK(fragment.entry_indices.size() < ctx.canonical_entries.size());

    front::front_sched sched(fragment.owner, kFrontCount);
    sched.insert_memtable_entries_for_testing(fragment, canonical_span(ctx));

    std::vector<bool> in_fragment(ctx.canonical_entries.size(), false);
    for (uint32_t idx : fragment.entry_indices) {
        in_fragment[idx] = true;
    }

    const auto snapshot = current_snapshot(sched);
    for (uint32_t idx = 0; idx < ctx.canonical_entries.size(); ++idx) {
        const auto& entry = ctx.canonical_entries[idx];
        auto result = sched.lookup_memtable_for_testing(
            entry.key, ctx.batch_lsn, snapshot);
        if (in_fragment[idx]) {
            const auto& hit = expect_value(result);
            CHECK(same_value_ref(hit.durable, entry.allocated_vr));
        } else {
            expect_miss(result);
        }
    }
}

void
snapshot_lookup_and_scan_ignore_current_front_state() {
    front::front_sched sched(0, 1);

    auto first = make_single_front_ctx(
        {
            {.op = core::write_op_type::put, .key = "a", .value = "old-a"},
            {.op = core::write_op_type::put, .key = "b", .value = "old-b"},
        },
        20);
    sched.insert_memtable_entries_for_testing(
        only_fragment(first), canonical_span(first));

    const auto old_snapshot = current_snapshot(sched);
    const auto old_a_ref = first.canonical_entries[0].allocated_vr;
    const auto old_b_ref = first.canonical_entries[1].allocated_vr;

    (void)sched.seal_active_for_testing();

    auto second = make_single_front_ctx(
        {
            {.op = core::write_op_type::put, .key = "a", .value = "new-a"},
            {.op = core::write_op_type::put, .key = "c", .value = "new-c"},
        },
        21);
    sched.insert_memtable_entries_for_testing(
        only_fragment(second), canonical_span(second));

    auto old_lookup = sched.lookup_memtable_for_testing(
        "a", 100, old_snapshot);
    CHECK(same_value_ref(expect_value(old_lookup).durable, old_a_ref));

    auto current_lookup = sched.lookup_memtable_for_testing(
        "a", 100, current_snapshot(sched));
    CHECK(same_value_ref(expect_value(current_lookup).durable,
                         second.canonical_entries[0].allocated_vr));

    auto rows = sched.scan_memtable_for_testing(
        "a", "z", 100, old_snapshot);
    CHECK(rows.size() == 2);
    CHECK(rows[0].key == "a");
    CHECK(rows[0].data_ver == 20);
    CHECK(same_value_ref(rows[0].vh.durable, old_a_ref));
    CHECK(rows[1].key == "b");
    CHECK(rows[1].data_ver == 20);
    CHECK(same_value_ref(rows[1].vh.durable, old_b_ref));

    auto empty_range = sched.scan_memtable_for_testing(
        "z", "a", 100, old_snapshot);
    CHECK(empty_range.empty());
}

void
batch_lookup_preserves_input_order_and_result_kinds() {
    front::front_sched sched(0, 1);

    auto ctx = make_single_front_ctx(
        {
            {.op = core::write_op_type::put, .key = "alpha", .value = "a"},
            {.op = core::write_op_type::del, .key = "gone", .value = ""},
            {.op = core::write_op_type::put, .key = "beta", .value = "b"},
        },
        30);
    sched.insert_memtable_entries_for_testing(
        only_fragment(ctx), canonical_span(ctx));

    std::vector<std::string_view> keys{"beta", "missing", "gone", "alpha"};
    auto rows = sched.batch_lookup_for_testing(
        std::span<const std::string_view>{keys.data(), keys.size()},
        30,
        current_snapshot(sched));

    CHECK(rows.size() == keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        CHECK(rows[i].key == keys[i]);
        CHECK(rows[i].key.data() == keys[i].data());
        CHECK(rows[i].key.size() == keys[i].size());
    }
    CHECK(same_value_ref(expect_value(rows[0].result).durable,
                         ctx.canonical_entries[2].allocated_vr));
    expect_miss(rows[1].result);
    expect_tombstone(rows[2].result);
    CHECK(same_value_ref(expect_value(rows[3].result).durable,
                         ctx.canonical_entries[0].allocated_vr));

    auto empty = sched.batch_lookup_for_testing(
        std::span<const std::string_view>{}, 30, current_snapshot(sched));
    CHECK(empty.empty());
}

void
empty_snapshot_reuses_m02_miss_and_empty_scan_semantics() {
    front::front_sched sched(0, 1);
    core::front_read_set empty_snapshot;

    expect_miss(sched.lookup_memtable_for_testing(
        "absent", 100, empty_snapshot));

    std::vector<std::string_view> keys{"absent"};
    auto batch = sched.batch_lookup_for_testing(
        std::span<const std::string_view>{keys.data(), keys.size()},
        100,
        empty_snapshot);
    CHECK(batch.size() == 1);
    CHECK(batch[0].key == "absent");
    expect_miss(batch[0].result);

    auto scan = sched.scan_memtable_for_testing(
        "a", "z", 100, empty_snapshot);
    CHECK(scan.empty());
}

void
seal_collect_and_release_use_owner_stride_ids() {
    constexpr uint32_t kOwner = 2;
    constexpr uint32_t kFrontCount = 4;
    front::front_sched sched(kOwner, kFrontCount);

    CHECK(sched.active_for_testing()->gen_id ==
          front::make_front_gen_id(kOwner, kFrontCount, 0));
    CHECK(sched.active_for_testing()->front_owner_index == kOwner);

    const std::string owned_key =
        key_for_owner(kOwner, kFrontCount, "owned");
    auto ctx = make_ctx(
        {
            {.op = core::write_op_type::put, .key = owned_key, .value = "v"},
        },
        40,
        kFrontCount);
    const auto& fragment = only_fragment(ctx);
    CHECK(fragment.owner == kOwner);

    front::front_sched owner_sched(kOwner, kFrontCount);
    owner_sched.insert_memtable_entries_for_testing(
        fragment, canonical_span(ctx));

    auto sealed_snapshot = owner_sched.seal_active_for_testing();
    CHECK(sealed_snapshot.active == owner_sched.active_for_testing());
    CHECK(owner_sched.imms_for_testing().size() == 1);

    const auto old = owner_sched.imms_for_testing()[0];
    CHECK(old->st == core::memtable_gen::state::sealed);
    CHECK(old->front_owner_index == kOwner);
    CHECK(old->gen_id == front::make_front_gen_id(kOwner, kFrontCount, 0));
    CHECK(owner_sched.active_for_testing()->st ==
          core::memtable_gen::state::active);
    CHECK(owner_sched.active_for_testing()->front_owner_index == kOwner);
    CHECK(owner_sched.active_for_testing()->gen_id ==
          front::make_front_gen_id(kOwner, kFrontCount, 1));
    CHECK(owner_sched.next_local_gen_epoch_for_testing() == 2);

    auto none = owner_sched.collect_eligible_gens_for_testing(39);
    CHECK(none.empty());

    auto eligible = owner_sched.collect_eligible_gens_for_testing(40);
    CHECK(eligible.size() == 1);
    CHECK(eligible[0] == old);

    owner_sched.release_gens_for_testing({old->gen_id + kFrontCount, old->gen_id});
    CHECK(owner_sched.imms_for_testing().empty());

    auto empty_sealed = owner_sched.seal_active_for_testing();
    CHECK(empty_sealed.imms.size() == 1);
    CHECK(empty_sealed.imms[0]->front_owner_index == kOwner);
    CHECK(empty_sealed.imms[0]->gen_id ==
          front::make_front_gen_id(kOwner, kFrontCount, 1));
    auto empty_eligible = owner_sched.collect_eligible_gens_for_testing(0);
    CHECK(empty_eligible.size() == 1);
}

void
sender_facade_drives_front_ops_through_pump() {
    front::front_sched sched(0, 1);
    auto ctx = make_single_front_ctx(
        {
            {.op = core::write_op_type::put, .key = "sender-a", .value = "a"},
            {.op = core::write_op_type::put, .key = "sender-b", .value = "b"},
        },
        50);

    submit_front_void_and_drive(sched, [&] {
        return front::insert_memtable_entries(
            sched, only_fragment(ctx), canonical_span(ctx));
    });

    auto hit = submit_front_and_drive<core::memtable_lookup_result>(sched, [&] {
        return front::lookup_memtable(
            sched, "sender-a", 50, current_snapshot(sched));
    });
    CHECK(same_value_ref(expect_value(hit).durable,
                         ctx.canonical_entries[0].allocated_vr));

    std::vector<std::string_view> keys{"sender-b", "sender-a"};
    auto batch = submit_front_and_drive<front::batch_lookup_result>(sched, [&] {
        return front::batch_lookup(
            sched,
            std::span<const std::string_view>{keys.data(), keys.size()},
            50,
            current_snapshot(sched));
    });
    CHECK(batch.size() == 2);
    CHECK(batch[0].key == "sender-b");
    CHECK(batch[1].key == "sender-a");

    auto scan = submit_front_and_drive<core::memtable_scan_result>(sched, [&] {
        return front::scan_memtable(
            sched, "sender-a", "sender-z", 50, current_snapshot(sched));
    });
    CHECK(scan.size() == 2);

    auto sealed = submit_front_and_drive<core::front_read_set>(sched, [&] {
        return front::seal_active(sched);
    });
    CHECK(sealed.imms.size() == 1);
    CHECK(sealed.imms[0]->max_lsn == 50);

    auto eligible =
        submit_front_and_drive<std::vector<std::shared_ptr<core::memtable_gen>>>(
            sched, [&] {
                return front::collect_eligible_gens(sched, 50);
            });
    CHECK(eligible.size() == 1);

    submit_front_void_and_drive(sched, [&] {
        return front::release_gens(
            sched, std::vector<uint64_t>{eligible[0]->gen_id});
    });
    CHECK(sched.imms_for_testing().empty());
}

void
callback_exceptions_propagate_after_state_commit() {
    front::front_sched sched(0, 1);
    uint32_t failures = 0;

    auto ctx = make_single_front_ctx(
        {
            {.op = core::write_op_type::put, .key = "throw-key", .value = "v"},
        },
        60);

    sched.schedule_insert(new front::_front_insert::req{
        .fragment = only_fragment(ctx),
        .canonical_entries = canonical_span(ctx),
        .cb = [] {
            throw downstream_marker{};
        },
        .fail = [&](std::exception_ptr) {
            ++failures;
        },
    });

    expect_throws<downstream_marker>([&] {
        (void)sched.advance();
    });
    CHECK(failures == 0);

    auto hit = sched.lookup_memtable_for_testing(
        "throw-key", 60, current_snapshot(sched));
    CHECK(same_value_ref(expect_value(hit).durable,
                         ctx.canonical_entries[0].allocated_vr));

    sched.schedule_seal(new front::_front_seal::req{
        .cb = [](core::front_read_set&& frs) {
            CHECK(frs.imms.size() == 1);
            throw downstream_marker{};
        },
        .fail = [&](std::exception_ptr) {
            ++failures;
        },
    });

    expect_throws<downstream_marker>([&] {
        (void)sched.advance();
    });
    CHECK(failures == 0);
    CHECK(sched.imms_for_testing().size() == 1);
    CHECK(sched.active_for_testing()->gen_id == front::make_front_gen_id(0, 1, 1));
}

void
scan_callback_keeps_request_snapshot_pin_alive() {
    front::front_sched sched(0, 1);
    auto gen = front::make_front_memtable_gen(
        0, 1, 0, core::memtable_gen::state::active);
    core::insert_value(*gen, "pinned-key", 90, make_value_ref(9000));

    std::weak_ptr<core::memtable_gen> weak = gen;
    core::front_read_set frs{.active = gen};
    gen.reset();

    uint32_t failures = 0;
    bool callback_ran = false;
    sched.schedule_scan(new front::_front_scan::req{
        .begin = "pinned",
        .end = "pinned-z",
        .read_lsn = 90,
        .frs = std::move(frs),
        .cb = [&](core::memtable_scan_result&& rows) {
            CHECK(!weak.expired());
            CHECK(rows.size() == 1);
            CHECK(rows[0].key == "pinned-key");
            CHECK(same_value_ref(rows[0].vh.durable, make_value_ref(9000)));
            callback_ran = true;
        },
        .fail = [&](std::exception_ptr) {
            ++failures;
        },
    });

    CHECK(sched.advance());
    CHECK(callback_ran);
    CHECK(failures == 0);
    CHECK(weak.expired());
}

void
bad_fragment_fails_before_mutation() {
    front::front_sched sched(0, 1);

    auto good = make_single_front_ctx(
        {
            {.op = core::write_op_type::put, .key = "good", .value = "v"},
        },
        70);
    sched.insert_memtable_entries_for_testing(
        only_fragment(good), canonical_span(good));
    CHECK(sched.active_for_testing()->table.size() == 1);
    CHECK(sched.active_for_testing()->max_lsn == 70);

    auto bad = make_single_front_ctx(
        {
            {.op = core::write_op_type::put, .key = "bad", .value = "v"},
        },
        71);
    core::front_fragment bad_fragment = only_fragment(bad);
    bad_fragment.entry_indices.push_back(999);

    uint32_t callbacks = 0;
    uint32_t failures = 0;
    sched.schedule_insert(new front::_front_insert::req{
        .fragment = std::move(bad_fragment),
        .canonical_entries = canonical_span(bad),
        .cb = [&] {
            ++callbacks;
        },
        .fail = [&](std::exception_ptr ep) {
            ++failures;
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::out_of_range&) {
                return;
            }
            CHECK(false);
        },
    });

    CHECK(sched.advance());
    CHECK(callbacks == 0);
    CHECK(failures == 1);
    CHECK(sched.active_for_testing()->table.size() == 1);
    CHECK(sched.active_for_testing()->max_lsn == 70);
    expect_miss(sched.lookup_memtable_for_testing(
        "bad", 71, current_snapshot(sched)));
}

void
memtable_apply_failure_propagates_as_fatal() {
    front::front_sched sched(0, 1);

    std::vector<core::canonical_entry> entries{
        core::canonical_entry{
            .op = static_cast<core::write_op_type>(0xff),
            .key = "fatal",
            .value = "",
            .allocated_vr = make_value_ref(8000),
        },
    };
    core::front_fragment fragment{
        .owner = 0,
        .batch_lsn = 80,
        .entry_count = 1,
        .entry_indices = {0},
    };

    uint32_t callbacks = 0;
    uint32_t failures = 0;
    sched.schedule_insert(new front::_front_insert::req{
        .fragment = std::move(fragment),
        .canonical_entries =
            std::span<const core::canonical_entry>{entries.data(),
                                                   entries.size()},
        .cb = [&] {
            ++callbacks;
        },
        .fail = [&](std::exception_ptr) {
            ++failures;
        },
    });

    expect_throws<std::logic_error>([&] {
        (void)sched.advance();
    });
    CHECK(callbacks == 0);
    CHECK(failures == 0);
    CHECK(sched.active_for_testing()->table.empty());
}

}  // namespace

int
main() {
    insert_consumes_stable_fragment_indices_and_durable_refs();
    snapshot_lookup_and_scan_ignore_current_front_state();
    batch_lookup_preserves_input_order_and_result_kinds();
    empty_snapshot_reuses_m02_miss_and_empty_scan_semantics();
    seal_collect_and_release_use_owner_stride_ids();
    sender_facade_drives_front_ops_through_pump();
    callback_exceptions_propagate_after_state_commit();
    scan_callback_keeps_request_snapshot_pin_alive();
    bad_fragment_fails_before_mutation();
    memtable_apply_failure_propagates_as_fatal();
    return 0;
}
