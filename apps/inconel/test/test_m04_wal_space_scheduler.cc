#include "apps/inconel/test/check.hh"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "pump/core/context.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include "apps/inconel/front/wal_append.hh"
#include "apps/inconel/wal/sender.hh"

using namespace apps::inconel;

namespace {

using root_context_t = decltype(pump::core::make_root_context());
using alloc_sender_t =
    decltype(wal::alloc_segment(std::declval<wal::wal_space_sched &>(), 1));
using reclaim_sender_t =
    decltype(wal::reclaim_check(std::declval<wal::wal_space_sched &>(), 1));

static_assert(pump::core::compute_sender_type<root_context_t,
                                              alloc_sender_t>::count_value() ==
              1);
static_assert(
    std::same_as<decltype(pump::core::compute_sender_type<
                          root_context_t,
                          alloc_sender_t>::get_value_type_identity())::type,
                 wal::segment_runtime *>);
static_assert(pump::core::compute_sender_type<
                  root_context_t, reclaim_sender_t>::count_value() == 0);

struct downstream_marker {};

template <typename Exc, typename Fn> void expect_throws(Fn &&fn) {
  bool threw = false;
  try {
    std::forward<Fn>(fn)();
  } catch (const Exc &) {
    threw = true;
  }
  CHECK(threw);
}

wal::segment_geometry make_geom(uint32_t count = 2, uint64_t base_lba = 1000,
                                uint32_t segment_size = 4096,
                                uint32_t lba_size = 512) {
  return wal::segment_geometry{
      .wal_base_paddr =
          format::paddr{
              .device_id = 0,
              .lba = base_lba,
          },
      .wal_segment_size = segment_size,
      .lba_size = lba_size,
      .wal_segment_count = count,
      .expected_format_version = format::SUPERBLOCK_FORMAT_VERSION_V1,
  };
}

void begin_pending_with_zero_tail(wal::wal_stream_state &stream,
                                  const wal::wal_append_plan &plan) {
  std::vector<char> page(stream.lba_size(), char{0});
  stream.begin_pending(plan, page);
}

template <typename T, typename SenderBuilder>
T submit_wal_and_drive(wal::wal_space_sched &sched,
                       SenderBuilder &&build_sender) {
  auto ctx = pump::core::make_root_context();
  auto promise = std::make_shared<std::promise<T>>();
  auto fut = promise->get_future();

  std::forward<SenderBuilder>(build_sender)() >>
      pump::sender::then([promise](auto &&r) mutable {
        promise->set_value(std::forward<decltype(r)>(r));
      }) >>
      pump::sender::submit(ctx);

  for (uint32_t i = 0; fut.wait_for(std::chrono::milliseconds(0)) !=
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
void submit_wal_void_and_drive(wal::wal_space_sched &sched,
                               SenderBuilder &&build_sender) {
  auto ctx = pump::core::make_root_context();
  auto promise = std::make_shared<std::promise<void>>();
  auto fut = promise->get_future();

  std::forward<SenderBuilder>(build_sender)() >>
      pump::sender::then([promise]() mutable { promise->set_value(); }) >>
      pump::sender::submit(ctx);

  for (uint32_t i = 0; fut.wait_for(std::chrono::milliseconds(0)) !=
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

wal::sealed_segment_info seal_active(wal::segment_geometry geom,
                                     wal::segment_runtime *seg,
                                     uint32_t stream_id, uint64_t min_lsn,
                                     uint64_t max_lsn) {
  wal::wal_stream_state stream(stream_id, geom);
  stream.install_segment(seg);
  wal::wal_append_plan header;
  header.plan_id = 1;
  header.kind = wal::wal_plan_kind::header;
  header.stream_id = stream_id;
  header.segment = seg->id;
  header.segment_gen = seg->segment_gen;
  header.start_offset = 0;
  header.end_offset = format::WAL_SEGMENT_HEADER_SIZE;
  begin_pending_with_zero_tail(stream, header);
  CHECK(!stream.commit_pending(header.plan_id).has_value());

  wal::wal_append_plan first;
  first.plan_id = 2;
  first.kind = wal::wal_plan_kind::entries;
  first.stream_id = stream_id;
  first.segment = seg->id;
  first.segment_gen = seg->segment_gen;
  first.start_offset = stream.write_offset();
  first.end_offset = first.start_offset + 64;
  first.min_lsn = min_lsn;
  first.max_lsn = min_lsn;
  begin_pending_with_zero_tail(stream, first);
  CHECK(!stream.commit_pending(first.plan_id).has_value());

  if (max_lsn != min_lsn) {
    wal::wal_append_plan second;
    second.plan_id = 3;
    second.kind = wal::wal_plan_kind::entries;
    second.stream_id = stream_id;
    second.segment = seg->id;
    second.segment_gen = seg->segment_gen;
    second.start_offset = stream.write_offset();
    second.end_offset = second.start_offset + 64;
    second.min_lsn = min_lsn;
    second.max_lsn = max_lsn;
    begin_pending_with_zero_tail(stream, second);
    CHECK(!stream.commit_pending(second.plan_id).has_value());
  }
  return stream.make_sealed_info();
}

void geometry_validation_and_base_address_use_wal_base() {
  auto geom = make_geom(4, 9000, 4096, 512);
  const auto p = wal::segment_base_paddr(geom, wal::segment_id{
                                                   .device_id = 0,
                                                   .index = 2,
                                               });
  CHECK(p.device_id == 0);
  CHECK(p.lba == 9000 + 2 * (4096 / 512));
  CHECK(wal::trailer_reserved_bytes(geom) == 512);
  CHECK(wal::segment_usable_end_offset(geom) == 4096 - 512);

  const auto can_fit_max_entry = make_geom(1, 1000, 4096, 512);
  wal::wal_space_sched fit_sched(can_fit_max_entry);
  CHECK(wal::segment_usable_end_offset(can_fit_max_entry) -
            format::WAL_SEGMENT_HEADER_SIZE >=
        wal::kMaxSupportedWalEntrySize);
  CHECK(fit_sched.try_alloc_segment_for_testing(0) != nullptr);

  expect_throws<std::invalid_argument>([] {
    wal::wal_space_sched sched(make_geom(0));
    (void)sched;
  });
  expect_throws<std::invalid_argument>([] {
    wal::wal_space_sched sched(make_geom(1, 1000, 4097, 512));
    (void)sched;
  });
  expect_throws<std::invalid_argument>([] {
    const uint32_t non_power_lba =
        format::WAL_SEGMENT_HEADER_SIZE + wal::kMaxSupportedWalEntrySize;
    wal::wal_space_sched sched(
        make_geom(1, 1000, non_power_lba * 2, non_power_lba));
    (void)sched;
  });
  expect_throws<std::invalid_argument>([] {
    auto bad = make_geom(1);
    bad.wal_base_paddr.device_id = 1;
    wal::wal_space_sched sched(bad);
    (void)sched;
  });
  expect_throws<std::invalid_argument>([] {
    wal::wal_space_sched sched(make_geom(1, 1000, 1024, 512));
    (void)sched;
  });
}

void test_only_helper_can_observe_empty_pool_without_production_success() {
  wal::wal_space_sched sched(make_geom(2), 2);

  auto *a = sched.try_alloc_segment_for_testing(0);
  auto *b = sched.try_alloc_segment_for_testing(1);
  auto *c = sched.try_alloc_segment_for_testing(0);

  CHECK(a != nullptr);
  CHECK(b != nullptr);
  CHECK(c == nullptr);
  CHECK(a->id.index == 0);
  CHECK(b->id.index == 1);
  CHECK(a->segment_gen == 1);
  CHECK(b->segment_gen == 1);
  CHECK(a->st == wal::wal_segment_state::active);
  CHECK(b->st == wal::wal_segment_state::active);
  CHECK(sched.alloc_head_for_testing() == 2);
  CHECK(sched.used_segment_count() == 2);
}

void stream_state_tracks_offsets_fit_and_sealed_lsn_range() {
  auto geom = make_geom(1);
  wal::wal_space_sched sched(geom, 2);
  auto *seg = sched.try_alloc_segment_for_testing(1);

  wal::wal_stream_state stream(1, geom);
  expect_throws<std::logic_error>([&] { (void)stream.make_sealed_info(); });

  stream.install_segment(seg);
  CHECK(stream.active_segment() == seg);
  CHECK(stream.write_offset() == 0);
  CHECK(!stream.header_committed());
  CHECK(stream.usable_end_offset() == 4096 - 512);
  CHECK(!stream.can_fit_entry(wal::kMaxSupportedWalEntrySize));
  expect_throws<std::logic_error>([&] { (void)stream.make_sealed_info(); });

  wal::wal_append_plan header;
  header.plan_id = 1;
  header.kind = wal::wal_plan_kind::header;
  header.stream_id = 1;
  header.segment = seg->id;
  header.segment_gen = seg->segment_gen;
  header.start_offset = 0;
  header.end_offset = format::WAL_SEGMENT_HEADER_SIZE;
  begin_pending_with_zero_tail(stream, header);
  CHECK(stream.write_offset() == 0);
  CHECK(!stream.header_committed());
  CHECK(!stream.commit_pending(header.plan_id).has_value());
  CHECK(stream.write_offset() == format::WAL_SEGMENT_HEADER_SIZE);
  CHECK(stream.header_committed());
  CHECK(stream.can_fit_entry(wal::kMaxSupportedWalEntrySize));

  wal::wal_append_plan pending;
  pending.plan_id = 2;
  pending.kind = wal::wal_plan_kind::entries;
  pending.stream_id = 1;
  pending.segment = seg->id;
  pending.segment_gen = seg->segment_gen;
  pending.start_offset = stream.write_offset();
  pending.end_offset = pending.start_offset + 128;
  pending.min_lsn = 10;
  pending.max_lsn = 10;
  begin_pending_with_zero_tail(stream, pending);
  CHECK(stream.write_offset() == format::WAL_SEGMENT_HEADER_SIZE);
  CHECK(seg->min_lsn == std::numeric_limits<uint64_t>::max());
  stream.abort_pending(pending.plan_id);
  CHECK(stream.write_offset() == format::WAL_SEGMENT_HEADER_SIZE);

  begin_pending_with_zero_tail(stream, pending);
  CHECK(!stream.commit_pending(pending.plan_id).has_value());

  wal::wal_append_plan second;
  second.plan_id = 3;
  second.kind = wal::wal_plan_kind::entries;
  second.stream_id = 1;
  second.segment = seg->id;
  second.segment_gen = seg->segment_gen;
  second.start_offset = stream.write_offset();
  second.end_offset = second.start_offset + 256;
  second.min_lsn = 10;
  second.max_lsn = 12;
  begin_pending_with_zero_tail(stream, second);
  CHECK(!stream.commit_pending(second.plan_id).has_value());

  CHECK(stream.write_offset() == format::WAL_SEGMENT_HEADER_SIZE + 128 + 256);
  CHECK(seg->min_lsn == 10);
  CHECK(seg->max_lsn == 12);

  auto info = stream.make_sealed_info();
  CHECK(info.id == seg->id);
  CHECK(info.segment_gen == seg->segment_gen);
  CHECK(info.min_lsn == 10);
  CHECK(info.max_lsn == 12);

  CHECK(!stream.can_fit_entry(stream.usable_end_offset()));
}

void sealed_segments_reclaim_to_free_pool_with_incremented_generation() {
  auto geom = make_geom(2, 7000);
  wal::wal_space_sched sched(geom, 1);

  auto *first = sched.try_alloc_segment_for_testing(0);
  auto info = seal_active(geom, first, 0, 5, 7);
  auto *second = sched.try_alloc_segment_for_testing(0, info);
  CHECK(second != nullptr);
  CHECK(second->id.index == 1);
  CHECK(sched.sealed_segment_count_for_testing() == 1);
  CHECK(sched.used_segment_count() == 2);

  sched.reclaim_check_for_testing(6);
  CHECK(sched.sealed_segment_count_for_testing() == 1);
  CHECK(sched.free_pool_count_for_testing() == 0);

  sched.reclaim_check_for_testing(7);
  CHECK(sched.sealed_segment_count_for_testing() == 0);
  CHECK(sched.free_pool_count_for_testing() == 1);
  CHECK(sched.used_segment_count() == 1);

  auto *recycled = sched.try_alloc_segment_for_testing(0);
  CHECK(recycled == first);
  CHECK(recycled->id.index == 0);
  CHECK(recycled->segment_gen == 2);
  CHECK(sched.used_segment_count() == 2);

  const auto base = wal::segment_base_paddr(geom, recycled->id);
  CHECK(base.lba == 7000);
}

void pending_alloc_records_sealed_once_and_wakes_after_reclaim() {
  auto geom = make_geom(1);
  wal::wal_space_sched sched(geom, 2, 8, 4);

  auto *seg = sched.try_alloc_segment_for_testing(0);
  auto info = seal_active(geom, seg, 0, 8, 8);

  std::vector<uint32_t> callbacks;
  uint32_t failures = 0;
  bool reclaim_done = false;

  sched.schedule_alloc(new wal::_wal_alloc::req{
      .stream_id = 0,
      .sealed = info,
      .sealed_consumed = false,
      .cb =
          [&](core::owner_outcome<wal::segment_runtime *>&& r) {
            if (!r.has_value()) {
              ++failures;
              return;
            }
            auto* allocated = *r;
            CHECK(allocated == seg);
            CHECK(allocated->segment_gen == 2);
            callbacks.push_back(allocated->owner_stream);
          },
  });

  CHECK(sched.advance());
  CHECK(callbacks.empty());
  CHECK(failures == 0);
  CHECK(sched.pending_alloc_count_for_testing() == 1);
  CHECK(sched.sealed_segment_count_for_testing() == 1);
  CHECK(sched.segment_for_testing(0).st == wal::wal_segment_state::sealed);

  CHECK(!sched.advance());
  CHECK(sched.pending_alloc_count_for_testing() == 1);
  CHECK(sched.sealed_segment_count_for_testing() == 1);

  sched.schedule_reclaim(new wal::_wal_reclaim::req{
      .recovery_safe_lsn = 8,
      .cb = [&](core::owner_outcome<void>&& r) {
        if (r.has_value()) {
          reclaim_done = true;
        } else {
          ++failures;
        }
      },
  });

  CHECK(sched.advance());
  CHECK((callbacks == std::vector<uint32_t>{0}));
  CHECK(reclaim_done);
  CHECK(failures == 0);
  CHECK(sched.pending_alloc_count_for_testing() == 0);
  CHECK(sched.sealed_segment_count_for_testing() == 0);
  CHECK(sched.free_pool_count_for_testing() == 0);
  CHECK(sched.used_segment_count() == 1);
  CHECK(sched.segment_for_testing(0).st == wal::wal_segment_state::active);
}

void pending_alloc_fifo_order_is_driven_by_reclaim() {
  auto geom = make_geom(2);
  wal::wal_space_sched sched(geom, 4, 8, 4);

  auto *first = sched.try_alloc_segment_for_testing(0);
  auto *second = sched.try_alloc_segment_for_testing(1);
  auto first_info = seal_active(geom, first, 0, 10, 10);
  auto second_info = seal_active(geom, second, 1, 20, 20);

  std::vector<uint32_t> owners;
  auto enqueue_waiter = [&](uint32_t stream_id) {
    sched.schedule_alloc(new wal::_wal_alloc::req{
        .stream_id = stream_id,
        .sealed = std::nullopt,
        .sealed_consumed = false,
        .cb =
            [&](core::owner_outcome<wal::segment_runtime *>&& r) {
              CHECK(r.has_value());
              auto* allocated = *r;
              owners.push_back(allocated->owner_stream);
            },
    });
  };

  enqueue_waiter(2);
  enqueue_waiter(3);
  CHECK(sched.advance());
  CHECK(owners.empty());
  CHECK(sched.pending_alloc_count_for_testing() == 2);

  CHECK(sched.try_alloc_segment_for_testing(0, first_info) == nullptr);
  CHECK(sched.try_alloc_segment_for_testing(1, second_info) == nullptr);
  CHECK(sched.sealed_segment_count_for_testing() == 2);

  sched.reclaim_check_for_testing(10);
  CHECK((owners == std::vector<uint32_t>{2}));
  CHECK(sched.pending_alloc_count_for_testing() == 1);

  sched.reclaim_check_for_testing(20);
  CHECK((owners == std::vector<uint32_t>{2, 3}));
  CHECK(sched.pending_alloc_count_for_testing() == 0);
}

void duplicate_or_stale_sealed_info_goes_to_fail_without_second_record() {
  auto geom = make_geom(2);
  wal::wal_space_sched sched(geom, 1);

  auto *first = sched.try_alloc_segment_for_testing(0);
  auto info = seal_active(geom, first, 0, 1, 1);
  CHECK(sched.try_alloc_segment_for_testing(0, info) != nullptr);
  CHECK(sched.sealed_segment_count_for_testing() == 1);

  uint32_t callbacks = 0;
  uint32_t failures = 0;
  sched.schedule_alloc(new wal::_wal_alloc::req{
      .stream_id = 0,
      .sealed = info,
      .sealed_consumed = false,
      .cb =
          [&](core::owner_outcome<wal::segment_runtime *>&& r) {
            if (r.has_value()) {
              ++callbacks;
              return;
            }
            ++failures;
            try {
              std::rethrow_exception(r.error());
            } catch (const std::logic_error &) {
              return;
            }
            CHECK(false);
          },
  });

  CHECK(sched.advance());
  CHECK(callbacks == 0);
  CHECK(failures == 1);
  CHECK(sched.sealed_segment_count_for_testing() == 1);
}

void callback_exceptions_propagate_after_state_commit() {
  wal::wal_space_sched sched(make_geom(1), 2);
  uint32_t failures = 0;

  sched.schedule_alloc(new wal::_wal_alloc::req{
      .stream_id = 0,
      .sealed = std::nullopt,
      .sealed_consumed = false,
      .cb =
          [&](core::owner_outcome<wal::segment_runtime *>&& r) {
            if (!r.has_value()) {
              ++failures;
              return;
            }
            auto* seg = *r;
            CHECK(seg->st == wal::wal_segment_state::active);
            throw downstream_marker{};
          },
  });

  expect_throws<downstream_marker>([&] { (void)sched.advance(); });
  CHECK(failures == 0);
  CHECK(sched.used_segment_count() == 1);
  CHECK(sched.segment_for_testing(0).st == wal::wal_segment_state::active);
}

void pending_callback_exceptions_propagate_out_of_reclaim_path() {
  auto geom = make_geom(1);
  wal::wal_space_sched sched(geom, 1, 8, 2);
  auto *seg = sched.try_alloc_segment_for_testing(0);
  auto info = seal_active(geom, seg, 0, 6, 6);
  uint32_t failures = 0;

  sched.schedule_alloc(new wal::_wal_alloc::req{
      .stream_id = 0,
      .sealed = info,
      .sealed_consumed = false,
      .cb = [&](core::owner_outcome<wal::segment_runtime *>&& r) {
        if (r.has_value()) {
          throw downstream_marker{};
        }
        ++failures;
      },
  });

  CHECK(sched.advance());
  CHECK(sched.pending_alloc_count_for_testing() == 1);

  expect_throws<downstream_marker>([&] { sched.reclaim_check_for_testing(6); });
  CHECK(failures == 0);
  CHECK(sched.pending_alloc_count_for_testing() == 0);
  CHECK(sched.segment_for_testing(0).st == wal::wal_segment_state::active);
  CHECK(sched.segment_for_testing(0).segment_gen == 2);
}

void sender_facade_drives_wal_ops_through_pump() {
  wal::wal_space_sched sched(make_geom(2), 2);

  auto *seg = submit_wal_and_drive<wal::segment_runtime *>(
      sched, [&] { return wal::alloc_segment(sched, 1); });
  CHECK(seg != nullptr);
  CHECK(seg->owner_stream == 1);
  CHECK(seg->segment_gen == 1);

  submit_wal_void_and_drive(sched,
                            [&] { return wal::reclaim_check(sched, 0); });
}

} // namespace

int main() {
  geometry_validation_and_base_address_use_wal_base();
  test_only_helper_can_observe_empty_pool_without_production_success();
  stream_state_tracks_offsets_fit_and_sealed_lsn_range();
  sealed_segments_reclaim_to_free_pool_with_incremented_generation();
  pending_alloc_records_sealed_once_and_wakes_after_reclaim();
  pending_alloc_fifo_order_is_driven_by_reclaim();
  duplicate_or_stale_sealed_info_goes_to_fail_without_second_record();
  callback_exceptions_propagate_after_state_commit();
  pending_callback_exceptions_propagate_out_of_reclaim_path();
  sender_facade_drives_wal_ops_through_pump();
  return 0;
}
