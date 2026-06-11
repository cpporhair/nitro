#ifndef APPS_INCONEL_WAL_SCHEDULER_HH
#define APPS_INCONEL_WAL_SCHEDULER_HH

// WAL space scheduler M04: segment pool ownership, sealed-segment reclaim,
// and pending allocation backpressure.
//
// This file intentionally does not implement WAL byte append, segment
// header/trailer writes, NVMe FUA, front-owner orchestration, write-batch
// pipeline plumbing, recovery scanning, or runtime builder/API wiring.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

#include "../core/wal_stream.hh"

namespace apps::inconel::wal {

class wal_space_sched;

namespace _wal_alloc {
struct req;
struct sender;
} // namespace _wal_alloc
namespace _wal_reclaim {
struct req;
struct sender;
} // namespace _wal_reclaim

namespace _wal_alloc {

struct req {
  uint32_t stream_id = 0;
  std::optional<sealed_segment_info> sealed;
  bool sealed_consumed = false;
  std::move_only_function<void(segment_runtime *)> cb;
  std::move_only_function<void(std::exception_ptr)> fail;
};

struct op {
  constexpr static bool wal_alloc_segment_op = true;
  wal_space_sched *sched = nullptr;
  uint32_t stream_id = 0;
  std::optional<sealed_segment_info> sealed;

  template <uint32_t pos, typename ctx_t, typename scope_t>
  void start(ctx_t &ctx, scope_t &scope);
};

struct sender {
  wal_space_sched *sched = nullptr;
  uint32_t stream_id = 0;
  std::optional<sealed_segment_info> sealed;

  sender(wal_space_sched *s, uint32_t stream,
         std::optional<sealed_segment_info> sealed_info)
      : sched(s), stream_id(stream), sealed(std::move(sealed_info)) {}

  sender(sender &&) noexcept = default;
  sender &operator=(sender &&) noexcept = default;
  sender(const sender &) = delete;
  sender &operator=(const sender &) = delete;

  auto make_op() {
    return op{
        .sched = sched,
        .stream_id = stream_id,
        .sealed = std::move(sealed),
    };
  }

  template <typename ctx_t> auto connect() {
    return pump::core::builder::op_list_builder<0>().push_back(make_op());
  }
};
} // namespace _wal_alloc

namespace _wal_reclaim {

struct req {
  uint64_t recovery_safe_lsn = 0;
  std::move_only_function<void()> cb;
  std::move_only_function<void(std::exception_ptr)> fail;
};

struct op {
  constexpr static bool wal_reclaim_check_op = true;
  wal_space_sched *sched = nullptr;
  uint64_t recovery_safe_lsn = 0;

  template <uint32_t pos, typename ctx_t, typename scope_t>
  void start(ctx_t &ctx, scope_t &scope);
};

struct sender {
  wal_space_sched *sched = nullptr;
  uint64_t recovery_safe_lsn = 0;

  auto make_op() {
    return op{
        .sched = sched,
        .recovery_safe_lsn = recovery_safe_lsn,
    };
  }

  template <typename ctx_t> auto connect() {
    return pump::core::builder::op_list_builder<0>().push_back(make_op());
  }
};
} // namespace _wal_reclaim

class wal_space_sched {
public:
  explicit wal_space_sched(segment_geometry geometry, uint32_t stream_count = 0,
                           std::size_t queue_depth = 1024,
                           std::size_t pending_alloc_capacity = 0)
      : alloc_q_(queue_depth), reclaim_q_(queue_depth), geometry_(geometry),
        stream_count_(stream_count),
        pending_alloc_capacity_(pending_alloc_capacity == 0
                                    ? geometry.wal_segment_count
                                    : pending_alloc_capacity) {
    validate_segment_geometry(geometry_);
    if (pending_alloc_capacity_ == 0) {
      throw std::invalid_argument(
          "wal::wal_space_sched: pending capacity must be nonzero");
    }
    if (pending_alloc_capacity_ < geometry_.wal_segment_count) {
      throw std::invalid_argument(
          "wal::wal_space_sched: pending capacity below segment count");
    }

    slots_.reserve(geometry_.wal_segment_count);
    free_pool_.reserve(geometry_.wal_segment_count);
    sealed_segments_.reserve(geometry_.wal_segment_count);
    pending_allocs_.reserve(pending_alloc_capacity_);

    for (uint32_t i = 0; i < geometry_.wal_segment_count; ++i) {
      slots_.push_back(segment_runtime{
          .id =
              segment_id{
                  .device_id = geometry_.wal_base_paddr.device_id,
                  .index = i,
              },
          .owner_stream = std::numeric_limits<uint32_t>::max(),
          .segment_gen = 0,
          .st = wal_segment_state::free,
          .min_lsn = 0,
          .max_lsn = 0,
      });
    }
  }

  ~wal_space_sched();

  wal_space_sched(const wal_space_sched &) = delete;
  wal_space_sched &operator=(const wal_space_sched &) = delete;
  wal_space_sched(wal_space_sched &&) = delete;
  wal_space_sched &operator=(wal_space_sched &&) = delete;

  [[nodiscard]] _wal_alloc::sender
  alloc_segment(uint32_t stream_id,
                std::optional<sealed_segment_info> sealed = std::nullopt) {
    return _wal_alloc::sender{this, stream_id, std::move(sealed)};
  }

  [[nodiscard]] _wal_reclaim::sender reclaim_check(uint64_t recovery_safe_lsn) {
    return _wal_reclaim::sender{
        .sched = this,
        .recovery_safe_lsn = recovery_safe_lsn,
    };
  }

  [[nodiscard]] const segment_geometry &geometry() const noexcept {
    return geometry_;
  }

  [[nodiscard]] std::size_t segment_count() const noexcept {
    return slots_.size();
  }

  [[nodiscard]] std::size_t used_segment_count() const noexcept {
    return used_segment_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t sealed_segment_count_for_testing() const noexcept {
    return sealed_segments_.size();
  }

  [[nodiscard]] std::size_t free_pool_count_for_testing() const noexcept {
    return free_pool_.size();
  }

  [[nodiscard]] std::size_t pending_alloc_count_for_testing() const noexcept {
    return pending_allocs_.size() - pending_alloc_head_;
  }

  [[nodiscard]] uint32_t alloc_head_for_testing() const noexcept {
    return alloc_head_;
  }

  [[nodiscard]] const segment_runtime &
  segment_for_testing(uint32_t index) const {
    return slot_for_index(index);
  }

  [[nodiscard]] segment_runtime *try_alloc_segment_for_testing(
      uint32_t stream_id,
      std::optional<sealed_segment_info> sealed = std::nullopt) {
    validate_stream_id(stream_id);
    if (sealed.has_value()) {
      record_sealed_segment(stream_id, *sealed);
    }
    return try_allocate_now(stream_id);
  }

  void reclaim_check_for_testing(uint64_t recovery_safe_lsn) {
    reclaim_segments(recovery_safe_lsn);
    drain_pending_allocs();
  }

  void schedule_alloc(_wal_alloc::req *r);
  void schedule_reclaim(_wal_reclaim::req *r);

  bool advance();

  template <typename runtime_t> bool advance(runtime_t &) { return advance(); }

private:
  static constexpr uint32_t kMaxAllocPerAdvance = 128;
  static constexpr uint32_t kMaxReclaimPerAdvance = 64;

  void validate_stream_id(uint32_t stream_id) const {
    if (stream_count_ != 0 && stream_id >= stream_count_) {
      throw std::out_of_range("wal::wal_space_sched: stream_id out of range");
    }
  }

  [[nodiscard]] segment_runtime &slot_for_index(uint32_t index) {
    if (index >= slots_.size()) {
      throw std::out_of_range(
          "wal::wal_space_sched: segment index out of range");
    }
    return slots_[index];
  }

  [[nodiscard]] const segment_runtime &slot_for_index(uint32_t index) const {
    if (index >= slots_.size()) {
      throw std::out_of_range(
          "wal::wal_space_sched: segment index out of range");
    }
    return slots_[index];
  }

  [[nodiscard]] bool sealed_segment_already_recorded(
      const sealed_segment_info &info) const noexcept {
    for (const auto &existing : sealed_segments_) {
      if (existing.id == info.id && existing.segment_gen == info.segment_gen) {
        return true;
      }
    }
    return false;
  }

  void record_sealed_segment(uint32_t stream_id,
                             const sealed_segment_info &info) {
    validate_stream_id(stream_id);
    if (info.id.device_id != geometry_.wal_base_paddr.device_id) {
      throw std::logic_error("wal::wal_space_sched: sealed device mismatch");
    }
    if (info.id.index >= geometry_.wal_segment_count) {
      throw std::logic_error(
          "wal::wal_space_sched: sealed segment index out of range");
    }
    if (info.segment_gen == 0) {
      throw std::logic_error("wal::wal_space_sched: sealed generation is zero");
    }
    if (info.min_lsn > info.max_lsn) {
      throw std::logic_error(
          "wal::wal_space_sched: sealed segment has empty LSN range");
    }

    segment_runtime &slot = slot_for_index(info.id.index);
    if (!(slot.id == info.id)) {
      throw std::logic_error("wal::wal_space_sched: slot identity mismatch");
    }
    if (slot.st != wal_segment_state::active) {
      throw std::logic_error(
          "wal::wal_space_sched: sealed segment is not active");
    }
    if (slot.owner_stream != stream_id) {
      throw std::logic_error("wal::wal_space_sched: sealed owner mismatch");
    }
    if (slot.segment_gen != info.segment_gen) {
      throw std::logic_error(
          "wal::wal_space_sched: sealed generation mismatch");
    }
    if (sealed_segment_already_recorded(info)) {
      throw std::logic_error("wal::wal_space_sched: duplicate sealed segment");
    }
    if (sealed_segments_.size() >= geometry_.wal_segment_count) {
      throw std::logic_error(
          "wal::wal_space_sched: sealed segment vector full");
    }

    sealed_segments_.push_back(info);
    slot.st = wal_segment_state::sealed;
    slot.min_lsn = info.min_lsn;
    slot.max_lsn = info.max_lsn;
  }

  [[nodiscard]] segment_runtime *try_allocate_now(uint32_t stream_id) {
    validate_stream_id(stream_id);

    segment_alloc_entry entry{};
    if (!free_pool_.empty()) {
      entry = free_pool_.back();
      free_pool_.pop_back();
    } else if (alloc_head_ < geometry_.wal_segment_count) {
      const uint32_t index = alloc_head_++;
      entry = segment_alloc_entry{
          .id =
              segment_id{
                  .device_id = geometry_.wal_base_paddr.device_id,
                  .index = index,
              },
          .next_gen = 1,
      };
    } else {
      return nullptr;
    }

    if (entry.id.device_id != geometry_.wal_base_paddr.device_id ||
        entry.id.index >= geometry_.wal_segment_count) {
      throw std::logic_error(
          "wal::wal_space_sched: free pool entry out of range");
    }
    if (entry.next_gen == 0) {
      throw std::logic_error(
          "wal::wal_space_sched: segment generation overflowed");
    }

    segment_runtime &slot = slot_for_index(entry.id.index);
    if (!(slot.id == entry.id)) {
      throw std::logic_error(
          "wal::wal_space_sched: allocation slot identity mismatch");
    }
    if (slot.st != wal_segment_state::free) {
      throw std::logic_error(
          "wal::wal_space_sched: allocation slot is not free");
    }
    if (entry.next_gen <= slot.segment_gen) {
      throw std::logic_error(
          "wal::wal_space_sched: non-increasing segment generation");
    }

    slot.owner_stream = stream_id;
    slot.segment_gen = entry.next_gen;
    slot.st = wal_segment_state::active;
    slot.min_lsn = std::numeric_limits<uint64_t>::max();
    slot.max_lsn = 0;
    used_segment_count_.fetch_add(1, std::memory_order_relaxed);
    return &slot;
  }

  void reclaim_segments(uint64_t recovery_safe_lsn) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < sealed_segments_.size(); ++read) {
      const sealed_segment_info info = sealed_segments_[read];
      if (info.max_lsn > recovery_safe_lsn) {
        if (write != read)
          sealed_segments_[write] = info;
        ++write;
        continue;
      }

      segment_runtime &slot = slot_for_index(info.id.index);
      if (slot.st != wal_segment_state::sealed || !(slot.id == info.id) ||
          slot.segment_gen != info.segment_gen) {
        throw std::logic_error(
            "wal::wal_space_sched: sealed slot invariant broken");
      }
      if (info.segment_gen == std::numeric_limits<uint32_t>::max()) {
        throw std::logic_error(
            "wal::wal_space_sched: segment generation overflow");
      }
      if (free_pool_.size() >= geometry_.wal_segment_count) {
        throw std::logic_error("wal::wal_space_sched: free pool vector full");
      }

      free_pool_.push_back(segment_alloc_entry{
          .id = info.id,
          .next_gen = info.segment_gen + 1,
      });
      slot.owner_stream = std::numeric_limits<uint32_t>::max();
      slot.st = wal_segment_state::free;
      slot.min_lsn = 0;
      slot.max_lsn = 0;
      used_segment_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    sealed_segments_.resize(write);
  }

  void compact_pending_if_needed() {
    if (pending_alloc_head_ == 0)
      return;
    if (pending_alloc_head_ == pending_allocs_.size()) {
      pending_allocs_.clear();
      pending_alloc_head_ = 0;
      return;
    }
    if (pending_allocs_.size() < pending_alloc_capacity_)
      return;

    pending_allocs_.erase(pending_allocs_.begin(),
                          pending_allocs_.begin() +
                              static_cast<std::ptrdiff_t>(pending_alloc_head_));
    pending_alloc_head_ = 0;
  }

  void push_pending_alloc(_wal_alloc::req *r) {
    compact_pending_if_needed();
    if (pending_allocs_.size() >= pending_alloc_capacity_) {
      throw std::runtime_error(
          "wal::wal_space_sched: pending allocation queue full");
    }
    pending_allocs_.push_back(r);
  }

  bool has_reclaim_free_segment() const noexcept { return !free_pool_.empty(); }

  void drain_pending_allocs() {
    while (pending_alloc_head_ < pending_allocs_.size() &&
           has_reclaim_free_segment()) {
      _wal_alloc::req *raw = pending_allocs_[pending_alloc_head_++];
      if (pending_alloc_head_ == pending_allocs_.size()) {
        pending_allocs_.clear();
        pending_alloc_head_ = 0;
      }

      std::unique_ptr<_wal_alloc::req> req(raw);
      if (req->sealed.has_value() && !req->sealed_consumed) {
        throw std::logic_error(
            "wal::wal_space_sched: pending sealed info was not "
            "consumed");
      }

      segment_runtime *seg = try_allocate_now(req->stream_id);
      if (seg == nullptr) {
        throw std::logic_error(
            "wal::wal_space_sched: free segment disappeared");
      }

      auto cb = std::move(req->cb);
      req.reset();
      if (cb) {
        cb(seg);
      }
    }
  }

  void handle_alloc(_wal_alloc::req *r);
  void handle_reclaim(_wal_reclaim::req *r);

  pump::core::per_core::queue<_wal_alloc::req *> alloc_q_;
  pump::core::per_core::queue<_wal_reclaim::req *> reclaim_q_;

  segment_geometry geometry_;
  uint32_t stream_count_ = 0;
  std::vector<segment_runtime> slots_;
  uint32_t alloc_head_ = 0;
  std::vector<segment_alloc_entry> free_pool_;
  std::vector<sealed_segment_info> sealed_segments_;
  std::vector<_wal_alloc::req *> pending_allocs_;
  std::size_t pending_alloc_head_ = 0;
  std::size_t pending_alloc_capacity_ = 0;
  std::atomic<std::size_t> used_segment_count_{0};
};

inline wal_space_sched::~wal_space_sched() {
  while (auto item = alloc_q_.try_dequeue())
    delete *item;
  while (auto item = reclaim_q_.try_dequeue())
    delete *item;
  for (std::size_t i = pending_alloc_head_; i < pending_allocs_.size(); ++i) {
    delete pending_allocs_[i];
  }
}

inline void wal_space_sched::schedule_alloc(_wal_alloc::req *r) {
  if (!alloc_q_.try_enqueue(r)) {
    delete r;
    throw std::runtime_error("wal::wal_space_sched: alloc queue full");
  }
}

inline void wal_space_sched::schedule_reclaim(_wal_reclaim::req *r) {
  if (!reclaim_q_.try_enqueue(r)) {
    delete r;
    throw std::runtime_error("wal::wal_space_sched: reclaim queue full");
  }
}

inline void wal_space_sched::handle_alloc(_wal_alloc::req *r) {
  std::unique_ptr<_wal_alloc::req> req(r);
  try {
    validate_stream_id(req->stream_id);
    if (req->sealed.has_value() && !req->sealed_consumed) {
      record_sealed_segment(req->stream_id, *req->sealed);
      req->sealed_consumed = true;
    }
  } catch (...) {
    auto fail = std::move(req->fail);
    req.reset();
    if (fail) {
      fail(std::current_exception());
    }
    return;
  }

  segment_runtime *seg = try_allocate_now(req->stream_id);
  if (seg == nullptr) {
    push_pending_alloc(req.get());
    (void)req.release();
    return;
  }

  auto cb = std::move(req->cb);
  req.reset();
  if (cb) {
    cb(seg);
  }
}

inline void wal_space_sched::handle_reclaim(_wal_reclaim::req *r) {
  std::unique_ptr<_wal_reclaim::req> req(r);
  try {
    reclaim_segments(req->recovery_safe_lsn);
  } catch (...) {
    auto fail = std::move(req->fail);
    req.reset();
    if (fail) {
      fail(std::current_exception());
    }
    return;
  }

  auto cb = std::move(req->cb);
  req.reset();

  drain_pending_allocs();

  if (cb) {
    cb();
  }
}

inline bool wal_space_sched::advance() {
  bool progress = false;

  for (uint32_t i = 0; i < kMaxAllocPerAdvance; ++i) {
    auto item = alloc_q_.try_dequeue();
    if (!item)
      break;
    handle_alloc(*item);
    progress = true;
  }

  for (uint32_t i = 0; i < kMaxReclaimPerAdvance; ++i) {
    auto item = reclaim_q_.try_dequeue();
    if (!item)
      break;
    handle_reclaim(*item);
    progress = true;
  }

  return progress;
}

template <uint32_t pos, typename ctx_t, typename scope_t>
void _wal_alloc::op::start(ctx_t &ctx, scope_t &scope) {
  sched->schedule_alloc(new req{
      .stream_id = stream_id,
      .sealed = std::move(sealed),
      .sealed_consumed = false,
      .cb =
          [ctx = ctx, scope = scope](segment_runtime *seg) mutable {
            pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope,
                                                                seg);
          },
      .fail =
          [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
            pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                ctx, scope, std::move(ep));
          },
  });
}

template <uint32_t pos, typename ctx_t, typename scope_t>
void _wal_reclaim::op::start(ctx_t &ctx, scope_t &scope) {
  sched->schedule_reclaim(new req{
      .recovery_safe_lsn = recovery_safe_lsn,
      .cb =
          [ctx = ctx, scope = scope]() mutable {
            pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
          },
      .fail =
          [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
            pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                ctx, scope, std::move(ep));
          },
  });
}

} // namespace apps::inconel::wal

namespace pump::core {

template <uint32_t pos, typename scope_t>
  requires(pos <
           std::tuple_size_v<typename scope_t::element_type::op_tuple_type>) &&
          (get_current_op_type_t<pos, scope_t>::wal_alloc_segment_op)
struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
  template <typename ctx_t> static void push_value(ctx_t &ctx, scope_t &scope) {
    std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
  }
};

template <typename ctx_t>
struct compute_sender_type<ctx_t, apps::inconel::wal::_wal_alloc::sender> {
  consteval static uint32_t count_value() { return 1; }
  consteval static auto get_value_type_identity() {
    return std::type_identity<apps::inconel::wal::segment_runtime *>{};
  }
};

template <uint32_t pos, typename scope_t>
  requires(pos <
           std::tuple_size_v<typename scope_t::element_type::op_tuple_type>) &&
          (get_current_op_type_t<pos, scope_t>::wal_reclaim_check_op)
struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
  template <typename ctx_t> static void push_value(ctx_t &ctx, scope_t &scope) {
    std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
  }
};

template <typename ctx_t>
struct compute_sender_type<ctx_t, apps::inconel::wal::_wal_reclaim::sender> {
  consteval static uint32_t count_value() { return 0; }
  consteval static auto get_value_type_identity() {
    return std::type_identity<void>{};
  }
};

} // namespace pump::core

#endif // APPS_INCONEL_WAL_SCHEDULER_HH
