#include "apps/inconel/test/check.hh"

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "pump/core/context.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include "apps/inconel/coord/sender.hh"
#include "apps/inconel/core/batch_carrier.hh"
#include "apps/inconel/core/checkpoint_guard.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/read_catalog.hh"
#include "apps/inconel/core/tree_geometry.hh"
#include "apps/inconel/core/tree_manifest.hh"

using namespace apps::inconel;

namespace {

std::atomic<bool>     g_count_allocations{false};
std::atomic<uint64_t> g_allocation_count{0};

void
note_allocation() noexcept {
    if (g_count_allocations.load(std::memory_order_relaxed)) {
        g_allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

void*
operator new(std::size_t n) {
    note_allocation();
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}

void*
operator new[](std::size_t n) {
    note_allocation();
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}

void*
operator new(std::size_t n, std::align_val_t align) {
    note_allocation();
    void* p = nullptr;
    const std::size_t alignment = static_cast<std::size_t>(align);
    const std::size_t size = n == 0 ? alignment : n;
    if (posix_memalign(&p, alignment, size) == 0) return p;
    throw std::bad_alloc();
}

void*
operator new[](std::size_t n, std::align_val_t align) {
    return operator new(n, align);
}

void
operator delete(void* p) noexcept {
    std::free(p);
}

void
operator delete[](void* p) noexcept {
    std::free(p);
}

void
operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void
operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

void
operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}

void
operator delete[](void* p, std::align_val_t) noexcept {
    std::free(p);
}

void
operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}

void
operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}

namespace {

template <typename T>
concept has_hot_member = requires(T t) {
    t.hot;
};

static_assert(!has_hot_member<core::value_handle>);
static_assert(std::same_as<decltype(std::declval<core::canonical_entry>()
                                        .allocated_vr),
                           format::value_ref>);
static_assert(!std::is_copy_constructible_v<core::batch_ctx>);
static_assert(!std::is_copy_constructible_v<core::client_batch_buffer>);

using root_context_t = decltype(pump::core::make_root_context());
using assign_sender_t =
    decltype(coord::assign_batch_lsn(
        std::declval<coord::coord_sched&>(),
        std::declval<core::client_batch_buffer&&>()));
using publish_sender_t =
    decltype(coord::publish_batch(std::declval<coord::coord_sched&>(), 1));
using release_sender_t =
    decltype(coord::release_batch(std::declval<coord::coord_sched&>(), 1));
using read_sender_t =
    decltype(coord::acquire_read_handle(std::declval<coord::coord_sched&>()));

static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  assign_sender_t>::count_value() == 1);
static_assert(std::same_as<
              decltype(pump::core::compute_sender_type<
                       root_context_t,
                       assign_sender_t>::get_value_type_identity())::type,
              core::batch_ctx>);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  publish_sender_t>::count_value() == 0);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  release_sender_t>::count_value() == 0);
static_assert(pump::core::compute_sender_type<
                  root_context_t,
                  read_sender_t>::count_value() == 1);
static_assert(std::same_as<
              decltype(pump::core::compute_sender_type<
                       root_context_t,
                       read_sender_t>::get_value_type_identity())::type,
              core::read_handle>);

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

template <typename T, typename SenderBuilder>
T
submit_coord_and_drive(coord::coord_sched& sched,
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
submit_coord_void_and_drive(coord::coord_sched& sched,
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

std::shared_ptr<core::memtable_gen>
make_gen(uint64_t gen_id,
         uint32_t front_owner_index,
         core::memtable_gen::state st = core::memtable_gen::state::active) {
    auto gen = std::make_shared<core::memtable_gen>();
    gen->gen_id = gen_id;
    gen->front_owner_index = front_owner_index;
    gen->st = st;
    return gen;
}

std::shared_ptr<const std::vector<core::front_read_set>>
make_fronts(uint32_t count, uint64_t gen_base = 1000) {
    auto fronts = std::make_shared<std::vector<core::front_read_set>>();
    fronts->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        fronts->push_back(core::front_read_set{
            .active = make_gen(gen_base + i, i),
            .imms   = {},
        });
    }
    return fronts;
}

std::shared_ptr<core::checkpoint_guard>
make_guard() {
    static const core::tree_geometry kGeom{
        .lba_size               = 4096,
        .tree_page_size         = 4096,
        .shadow_slots_per_range = 1,
    };

    auto guard = std::make_shared<core::checkpoint_guard>();
    guard->manifest =
        std::make_shared<const core::tree_manifest>(
            core::tree_manifest::empty(&kGeom));
    return guard;
}

std::shared_ptr<const core::published_read_set>
make_prs(std::shared_ptr<core::checkpoint_guard> guard,
         std::shared_ptr<const std::vector<core::front_read_set>> fronts,
         uint64_t epoch) {
    return std::make_shared<core::published_read_set>(
        core::published_read_set{
            .tree_guard = std::move(guard),
            .fronts     = std::move(fronts),
            .epoch      = epoch,
        });
}

std::shared_ptr<const core::publish_catalog>
make_cat(std::shared_ptr<const core::published_read_set> prs,
         uint64_t durable_lsn,
         uint64_t epoch) {
    return std::make_shared<core::publish_catalog>(
        std::move(prs), durable_lsn, epoch);
}

std::shared_ptr<const core::publish_catalog>
make_cat(uint64_t durable_lsn, uint64_t epoch, uint32_t front_count) {
    return make_cat(make_prs(make_guard(), make_fronts(front_count), epoch),
                    durable_lsn,
                    epoch);
}

std::span<const core::raw_batch_op>
op_span(const std::vector<core::raw_batch_op>& ops) {
    return {ops.data(), ops.size()};
}

core::client_batch_buffer
make_batch(std::string key, std::string value = "value") {
    std::vector<core::raw_batch_op> ops{
        {.op = core::write_op_type::put,
         .key = std::move(key),
         .value = std::move(value)},
    };
    return core::encode_client_batch(op_span(ops));
}

core::client_batch_buffer
make_delete_batch(std::string key) {
    std::vector<core::raw_batch_op> ops{
        {.op = core::write_op_type::del,
         .key = std::move(key),
         .value = ""},
    };
    return core::encode_client_batch(op_span(ops));
}

core::client_batch_buffer
make_duplicate_key_batch() {
    std::vector<core::raw_batch_op> ops{
        {.op = core::write_op_type::put, .key = "same", .value = "old"},
        {.op = core::write_op_type::put, .key = "a", .value = "va"},
        {.op = core::write_op_type::del, .key = "same", .value = ""},
    };
    return core::encode_client_batch(op_span(ops));
}

core::client_batch_buffer
make_empty_batch() {
    std::vector<core::raw_batch_op> ops;
    return core::encode_client_batch(op_span(ops));
}

core::client_batch_buffer
make_malformed_batch() {
    core::client_batch_buffer b;
    b.bytes.push_back(std::byte{0x01});
    return b;
}

uint64_t
visible_lsn(const coord::coord_sched& sched) {
    return sched.acquire_read_handle_for_testing().read_lsn;
}

void
construction_checks_initial_cat_contract() {
    expect_throws<std::invalid_argument>([] {
        coord::coord_sched sched(nullptr, 1, 1, 4);
        (void)sched;
    });

    expect_throws<std::invalid_argument>([] {
        coord::coord_sched sched(make_cat(0, 1, 1), 0, 1, 4);
        (void)sched;
    });

    expect_throws<std::invalid_argument>([] {
        coord::coord_sched sched(make_cat(0, 1, 1), 2, 1, 4);
        (void)sched;
    });

    expect_throws<std::invalid_argument>([] {
        coord::coord_sched sched(make_cat(5, 1, 1), 1, 5, 4);
        (void)sched;
    });

    expect_throws<std::invalid_argument>([] {
        coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 3);
        (void)sched;
    });
}

void
assign_batch_lsn_is_gap_free_and_uses_m01_carrier() {
    coord::coord_sched sched(make_cat(0, 7, 3), 3, 1, 8);

    auto ctx1 = sched.assign_batch_lsn_for_testing(make_duplicate_key_batch());
    auto ctx2 = sched.assign_batch_lsn_for_testing(make_delete_batch("dead"));

    CHECK(ctx1.batch_lsn == 1);
    CHECK(ctx2.batch_lsn == 2);
    CHECK(sched.next_lsn_for_testing() == 3);

    CHECK(ctx1.entry_count == 2);
    CHECK(ctx1.canonical_entries[0].key == "a");
    CHECK(ctx1.canonical_entries[0].value == "va");
    CHECK(ctx1.canonical_entries[1].key == "same");
    CHECK(ctx1.canonical_entries[1].op == core::write_op_type::del);
    CHECK(ctx1.canonical_entries[1].value.empty());

    CHECK(!ctx1.fragments.empty());
    for (const auto& f : ctx1.fragments) {
        CHECK(f.batch_lsn == 1);
        CHECK(f.entry_count == ctx1.entry_count);
        CHECK(f.owner < sched.front_count());
        for (uint32_t idx : f.entry_indices) {
            CHECK(idx < ctx1.canonical_entries.size());
            CHECK(static_cast<uint32_t>(
                      core::key_hash(ctx1.canonical_entries[idx].key) %
                      sched.front_count()) == f.owner);
        }
    }
}

void
assign_rejects_bad_or_empty_input_without_consuming_lsn() {
    coord::coord_sched sched(make_cat(9, 10, 1), 1, 10, 4);

    expect_throws<std::invalid_argument>([&] {
        (void)sched.assign_batch_lsn_for_testing(make_empty_batch());
    });
    expect_throws<std::invalid_argument>([&] {
        (void)sched.assign_batch_lsn_for_testing(make_malformed_batch());
    });

    auto ctx = sched.assign_batch_lsn_for_testing(make_batch("ok"));
    CHECK(ctx.batch_lsn == 10);
    CHECK(sched.next_lsn_for_testing() == 11);
}

void
publish_advances_only_contiguous_resolved_prefix() {
    coord::coord_sched sched(make_cat(0, 1, 2), 2, 1, 8);
    (void)sched.assign_batch_lsn_for_testing(make_batch("a"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("b"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("c"));

    sched.publish_batch_for_testing(1);
    CHECK(visible_lsn(sched) == 1);
    CHECK(sched.ready_base_for_testing() == 2);

    sched.publish_batch_for_testing(3);
    CHECK(visible_lsn(sched) == 1);
    CHECK(sched.ready_base_for_testing() == 2);

    sched.publish_batch_for_testing(2);
    CHECK(visible_lsn(sched) == 3);
    CHECK(sched.ready_base_for_testing() == 4);
}

void
release_fills_hole_and_never_skips_terminal_gap() {
    coord::coord_sched sched(make_cat(0, 1, 2), 2, 1, 8);
    (void)sched.assign_batch_lsn_for_testing(make_batch("a"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("b"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("c"));

    sched.publish_batch_for_testing(1);
    sched.publish_batch_for_testing(3);
    CHECK(visible_lsn(sched) == 1);

    sched.release_batch_for_testing(2);
    CHECK(visible_lsn(sched) == 3);
    CHECK(sched.ready_base_for_testing() == 4);
}

void
terminal_invalid_lsn_signals_fail_fast_errors() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 4);
    (void)sched.assign_batch_lsn_for_testing(make_batch("a"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("b"));

    sched.publish_batch_for_testing(2);
    expect_throws<std::logic_error>([&] {
        sched.release_batch_for_testing(2);
    });

    sched.publish_batch_for_testing(1);
    expect_throws<std::logic_error>([&] {
        sched.release_batch_for_testing(1);
    });

    expect_throws<std::logic_error>([&] {
        sched.publish_batch_for_testing(3);
    });

    coord::ready_window ready(1, 2);
    expect_throws<std::logic_error>([&] {
        ready.mark_resolved(3, 4);
    });
}

void
ready_base_tracks_consumed_prefix() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 8);
    (void)sched.assign_batch_lsn_for_testing(make_batch("a"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("b"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("c"));

    CHECK(sched.ready_base_for_testing() == 1);
    sched.publish_batch_for_testing(1);
    CHECK(sched.ready_base_for_testing() == 2);
    sched.release_batch_for_testing(2);
    CHECK(sched.ready_base_for_testing() == 3);
    sched.publish_batch_for_testing(3);
    CHECK(sched.ready_base_for_testing() == 4);
}

void
closed_gate_accumulates_pending_prefix_without_losing_it() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 8);
    (void)sched.assign_batch_lsn_for_testing(make_batch("a"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("b"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("c"));

    sched.close_gate_for_testing();
    sched.publish_batch_for_testing(1);
    CHECK(visible_lsn(sched) == 0);
    CHECK(sched.gate_pending_for_testing() == 1);

    sched.close_gate_for_testing();
    CHECK(sched.gate_pending_for_testing() == 1);

    sched.publish_batch_for_testing(3);
    CHECK(visible_lsn(sched) == 0);
    CHECK(sched.gate_pending_for_testing() == 1);

    sched.release_batch_for_testing(2);
    CHECK(visible_lsn(sched) == 0);
    CHECK(sched.gate_pending_for_testing() == 3);

    sched.open_gate_for_testing();
    CHECK(sched.gate_open_for_testing());
    CHECK(sched.gate_pending_for_testing() == 0);
    CHECK(visible_lsn(sched) == 3);
}

void
closed_gate_pending_applies_to_current_cat_after_switch() {
    auto cat0 = make_cat(0, 1, 1);
    auto cat1 = make_cat(0, 2, 1);
    coord::coord_sched sched(cat0, 1, 1, 8);
    (void)sched.assign_batch_lsn_for_testing(make_batch("a"));

    auto old = sched.acquire_read_handle_for_testing();
    sched.close_gate_for_testing();
    sched.publish_batch_for_testing(1);
    CHECK(cat0->durable_lsn.load(std::memory_order_acquire) == 0);
    CHECK(sched.gate_pending_for_testing() == 1);

    sched.install_cat_for_testing(cat1);
    sched.open_gate_for_testing();

    CHECK(old.cat == cat0);
    CHECK(old.read_lsn == 0);
    CHECK(cat0->durable_lsn.load(std::memory_order_acquire) == 0);
    CHECK(cat1->durable_lsn.load(std::memory_order_acquire) == 1);
    CHECK(sched.acquire_read_handle_for_testing().cat == cat1);
    CHECK(sched.acquire_read_handle_for_testing().read_lsn == 1);
}

void
pending_assign_fifo_waits_without_consuming_lsn() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 2);
    std::vector<uint64_t> assigned;
    uint32_t failures = 0;

    auto submit = [&](std::string key) {
        sched.enqueue_assign_batch_lsn_for_testing(
            make_batch(std::move(key)),
            [&](core::batch_ctx&& ctx) {
                assigned.push_back(ctx.batch_lsn);
            },
            [&](std::exception_ptr) {
                ++failures;
            });
    };

    submit("a");
    submit("b");
    CHECK(sched.advance());
    CHECK((assigned == std::vector<uint64_t>{1, 2}));
    CHECK(sched.next_lsn_for_testing() == 3);

    submit("c");
    submit("d");
    CHECK(sched.advance());
    CHECK((assigned == std::vector<uint64_t>{1, 2}));
    CHECK(sched.next_lsn_for_testing() == 3);

    sched.publish_batch_for_testing(1);
    CHECK((assigned == std::vector<uint64_t>{1, 2, 3}));
    CHECK(sched.next_lsn_for_testing() == 4);

    sched.release_batch_for_testing(2);
    CHECK((assigned == std::vector<uint64_t>{1, 2, 3, 4}));
    CHECK(sched.next_lsn_for_testing() == 5);
    CHECK(failures == 0);
}

void
pending_malformed_assign_fails_before_queueing() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 1, 4, 2);
    auto first = sched.assign_batch_lsn_for_testing(make_batch("a"));
    CHECK(first.batch_lsn == 1);
    CHECK(sched.next_lsn_for_testing() == 2);

    std::vector<uint64_t> assigned;
    std::string malformed_error;
    sched.enqueue_assign_batch_lsn_for_testing(
        make_malformed_batch(),
        [&](core::batch_ctx&& ctx) {
            assigned.push_back(ctx.batch_lsn);
        },
        [&](std::exception_ptr ep) {
            try {
                std::rethrow_exception(std::move(ep));
            } catch (const std::invalid_argument& e) {
                malformed_error = e.what();
            } catch (...) {
                malformed_error = "wrong exception";
            }
        });

    CHECK(sched.advance());
    CHECK(!malformed_error.empty());
    CHECK(assigned.empty());
    CHECK(sched.next_lsn_for_testing() == 2);

    sched.enqueue_assign_batch_lsn_for_testing(
        make_batch("b"),
        [&](core::batch_ctx&& ctx) {
            assigned.push_back(ctx.batch_lsn);
        },
        [&](std::exception_ptr) {
            malformed_error = "unexpected valid failure";
        });

    CHECK(sched.advance());
    CHECK(assigned.empty());
    CHECK(sched.next_lsn_for_testing() == 2);

    sched.publish_batch_for_testing(first.batch_lsn);
    CHECK((assigned == std::vector<uint64_t>{2}));
    CHECK(sched.next_lsn_for_testing() == 3);
}

void
pending_assign_overflow_fails_before_lsn_assignment() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 1, 4, 1);
    auto first = sched.assign_batch_lsn_for_testing(make_batch("a"));
    CHECK(first.batch_lsn == 1);
    CHECK(sched.next_lsn_for_testing() == 2);

    std::vector<uint64_t> assigned;
    uint32_t unexpected_failures = 0;
    sched.enqueue_assign_batch_lsn_for_testing(
        make_batch("b"),
        [&](core::batch_ctx&& ctx) {
            assigned.push_back(ctx.batch_lsn);
        },
        [&](std::exception_ptr) {
            ++unexpected_failures;
        });

    CHECK(sched.advance());
    CHECK(assigned.empty());
    CHECK(unexpected_failures == 0);
    CHECK(sched.next_lsn_for_testing() == 2);

    std::string overflow_error;
    sched.enqueue_assign_batch_lsn_for_testing(
        make_batch("c"),
        [&](core::batch_ctx&& ctx) {
            assigned.push_back(ctx.batch_lsn);
        },
        [&](std::exception_ptr ep) {
            try {
                std::rethrow_exception(std::move(ep));
            } catch (const std::runtime_error& e) {
                overflow_error = e.what();
            } catch (...) {
                overflow_error = "wrong exception";
            }
        });

    CHECK(sched.advance());
    CHECK(overflow_error == "coord assign backpressure overflow");
    CHECK(assigned.empty());
    CHECK(sched.next_lsn_for_testing() == 2);

    sched.publish_batch_for_testing(first.batch_lsn);
    CHECK((assigned == std::vector<uint64_t>{2}));
    CHECK(unexpected_failures == 0);
    CHECK(sched.next_lsn_for_testing() == 3);
}

void
acquire_read_handle_uses_cat_snapshot_and_stable_read_lsn() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 8);
    (void)sched.assign_batch_lsn_for_testing(make_batch("a"));

    core::read_handle old = sched.acquire_read_handle_for_testing();
    sched.publish_batch_for_testing(1);
    core::read_handle newer = sched.acquire_read_handle_for_testing();

    CHECK(old.read_lsn == 0);
    CHECK(newer.read_lsn == 1);
    CHECK(old.cat == newer.cat);
}

void
old_handle_pins_cat_prs_guard_manifest_and_fronts() {
    auto old_active = make_gen(500, 0);
    auto old_fronts = std::make_shared<std::vector<core::front_read_set>>();
    old_fronts->push_back(core::front_read_set{
        .active = old_active,
        .imms   = {},
    });
    std::shared_ptr<const std::vector<core::front_read_set>>
        old_fronts_const = old_fronts;
    auto old_guard = make_guard();
    auto old_prs = make_prs(old_guard, old_fronts_const, 20);
    auto old_cat = make_cat(old_prs, 5, 20);

    auto new_cat = make_cat(5, 21, 1);
    coord::coord_sched sched(old_cat, 1, 6, 8);

    std::weak_ptr<const core::publish_catalog> weak_cat = old_cat;
    std::weak_ptr<const core::published_read_set> weak_prs = old_prs;
    std::weak_ptr<core::checkpoint_guard> weak_guard = old_guard;
    std::weak_ptr<const core::tree_manifest> weak_manifest =
        old_guard->manifest;
    std::weak_ptr<const std::vector<core::front_read_set>> weak_fronts =
        old_fronts_const;
    std::weak_ptr<core::memtable_gen> weak_active = old_active;

    core::read_handle old_rh = sched.acquire_read_handle_for_testing();
    sched.install_cat_for_testing(new_cat);

    old_cat.reset();
    old_prs.reset();
    old_fronts_const.reset();
    old_fronts.reset();
    old_guard.reset();
    old_active.reset();

    CHECK(!weak_cat.expired());
    CHECK(!weak_prs.expired());
    CHECK(!weak_guard.expired());
    CHECK(!weak_manifest.expired());
    CHECK(!weak_fronts.expired());
    CHECK(!weak_active.expired());

    old_rh = {};

    CHECK(weak_cat.expired());
    CHECK(weak_prs.expired());
    CHECK(weak_guard.expired());
    CHECK(weak_manifest.expired());
    CHECK(weak_fronts.expired());
    CHECK(weak_active.expired());
}

void
downstream_callback_throw_does_not_become_coord_failure() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 8);
    uint32_t assign_failures = 0;

    sched.enqueue_assign_batch_lsn_for_testing(
        make_batch("a"),
        [&](core::batch_ctx&& ctx) {
            CHECK(ctx.batch_lsn == 1);
            throw downstream_marker{};
        },
        [&](std::exception_ptr) {
            ++assign_failures;
        });

    expect_throws<downstream_marker>([&] {
        (void)sched.advance();
    });
    CHECK(assign_failures == 0);
    CHECK(sched.next_lsn_for_testing() == 2);

    sched.publish_batch_for_testing(1);
    CHECK(visible_lsn(sched) == 1);

    auto next = sched.assign_batch_lsn_for_testing(make_batch("b"));
    CHECK(next.batch_lsn == 2);

    uint32_t terminal_failures = 0;
    sched.schedule_publish(new coord::_coord_publish::req{
        .batch_lsn = 2,
        .cb = [&](core::owner_outcome<void>&& r) {
            if (r.has_value()) {
                throw downstream_marker{};
            }
            ++terminal_failures;
        },
    });
    expect_throws<downstream_marker>([&] {
        (void)sched.advance();
    });
    CHECK(terminal_failures == 0);
    CHECK(visible_lsn(sched) == 2);

    auto released = sched.assign_batch_lsn_for_testing(make_batch("c"));
    CHECK(released.batch_lsn == 3);
    sched.schedule_release(new coord::_coord_release::req{
        .batch_lsn = 3,
        .cb = [&](core::owner_outcome<void>&& r) {
            if (r.has_value()) {
                throw downstream_marker{};
            }
            ++terminal_failures;
        },
    });
    expect_throws<downstream_marker>([&] {
        (void)sched.advance();
    });
    CHECK(terminal_failures == 0);
    CHECK(visible_lsn(sched) == 3);

    uint32_t read_failures = 0;
    sched.schedule_read(new coord::_coord_read::req{
        .cb = [&](core::owner_outcome<core::read_handle>&& r) {
            if (r.has_value()) {
                CHECK(r->read_lsn == 3);
                throw downstream_marker{};
            }
            ++read_failures;
        },
    });
    expect_throws<downstream_marker>([&] {
        (void)sched.advance();
    });
    CHECK(read_failures == 0);
}

void
sender_facade_drives_coord_ops_through_pump() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 8);

    auto first = submit_coord_and_drive<core::batch_ctx>(sched, [&] {
        return coord::assign_batch_lsn(sched, make_batch("sender-a"));
    });
    CHECK(first.batch_lsn == 1);
    CHECK(first.entry_count == 1);

    submit_coord_void_and_drive(sched, [&] {
        return coord::publish_batch(sched, first.batch_lsn);
    });

    auto rh = submit_coord_and_drive<core::read_handle>(sched, [&] {
        return coord::acquire_read_handle(sched);
    });
    CHECK(rh.read_lsn == 1);
    CHECK(rh.cat == sched.acquire_read_handle_for_testing().cat);

    auto second = submit_coord_and_drive<core::batch_ctx>(sched, [&] {
        return coord::assign_batch_lsn(sched, make_batch("sender-b"));
    });
    CHECK(second.batch_lsn == 2);

    submit_coord_void_and_drive(sched, [&] {
        return coord::release_batch(sched, second.batch_lsn);
    });
    CHECK(visible_lsn(sched) == 2);
}

void
terminal_publish_release_do_not_allocate_without_pending_assigns() {
    coord::coord_sched sched(make_cat(0, 1, 1), 1, 1, 8);
    (void)sched.assign_batch_lsn_for_testing(make_batch("a"));
    (void)sched.assign_batch_lsn_for_testing(make_batch("b"));

    g_allocation_count.store(0, std::memory_order_relaxed);
    g_count_allocations.store(true, std::memory_order_relaxed);
    sched.publish_batch_for_testing(1);
    sched.release_batch_for_testing(2);
    g_count_allocations.store(false, std::memory_order_relaxed);

    CHECK(g_allocation_count.load(std::memory_order_relaxed) == 0);
    CHECK(visible_lsn(sched) == 2);
}

}  // namespace

int
main() {
    construction_checks_initial_cat_contract();
    assign_batch_lsn_is_gap_free_and_uses_m01_carrier();
    assign_rejects_bad_or_empty_input_without_consuming_lsn();
    publish_advances_only_contiguous_resolved_prefix();
    release_fills_hole_and_never_skips_terminal_gap();
    terminal_invalid_lsn_signals_fail_fast_errors();
    ready_base_tracks_consumed_prefix();
    closed_gate_accumulates_pending_prefix_without_losing_it();
    closed_gate_pending_applies_to_current_cat_after_switch();
    pending_assign_fifo_waits_without_consuming_lsn();
    pending_malformed_assign_fails_before_queueing();
    pending_assign_overflow_fails_before_lsn_assignment();
    acquire_read_handle_uses_cat_snapshot_and_stable_read_lsn();
    old_handle_pins_cat_prs_guard_manifest_and_fronts();
    downstream_callback_throw_does_not_become_coord_failure();
    sender_facade_drives_coord_ops_through_pump();
    terminal_publish_release_do_not_allocate_without_pending_assigns();
    return 0;
}
