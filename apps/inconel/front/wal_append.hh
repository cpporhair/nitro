#ifndef APPS_INCONEL_FRONT_WAL_APPEND_HH
#define APPS_INCONEL_FRONT_WAL_APPEND_HH

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../core/wal_stream.hh"
#include "../format/wal.hh"
#include "../memory/dma_page_pool.hh"

namespace apps::inconel::wal {

struct wal_append_config {
  uint32_t max_fua_inflight = 16;
  uint32_t max_pages_per_plan = 16;
  // 0 keeps the historical coupling to front queue_depth. Non-zero sets
  // an independent soft backpressure capacity for pending WAL prepares.
  uint32_t pending_prepare_capacity = 0;
  // Upper bound on how many queued prepare reqs a single coalesced WAL entry
  // plan may merge (group commit fan-out). The plan stays the only in-flight
  // physical write unit; this only caps the participant fan-out woken per
  // commit/abort and bounds one front's queued WAL work per group.
  uint32_t max_participants_per_group = 16;
};

inline void validate_wal_append_config(const wal_append_config &cfg) {
  if (cfg.max_fua_inflight == 0) {
    throw std::invalid_argument(
        "wal::wal_append_config: max_fua_inflight must be nonzero");
  }
  if (cfg.max_pages_per_plan == 0) {
    throw std::invalid_argument(
        "wal::wal_append_config: max_pages_per_plan must be nonzero");
  }
  if (cfg.max_participants_per_group == 0) {
    throw std::invalid_argument(
        "wal::wal_append_config: max_participants_per_group must be nonzero");
  }
  // pending_prepare_capacity == 0 is a valid "follow queue_depth" setting,
  // not an invalid sentinel.
}

struct wal_fragment_cursor {
  uint32_t next_fragment_entry = 0;
};

enum class wal_plan_kind : uint8_t {
  header,
  entries,
  trailer,
};

struct wal_frame_write {
  memory::lba_dma_page_pool *pool = nullptr;
  memory::segmented_page_frame frame;
  memory::frame_write_desc desc{};

  wal_frame_write() = default;
  wal_frame_write(memory::lba_dma_page_pool *p,
                  memory::segmented_page_frame &&f,
                  uint32_t flags = 0) noexcept
      : pool(p), frame(std::move(f)), desc{.frame = &frame, .flags = flags} {}

  wal_frame_write(wal_frame_write &&rhs) noexcept
      : pool(rhs.pool), frame(std::move(rhs.frame)), desc(rhs.desc) {
    desc.frame = pool != nullptr ? &frame : nullptr;
    rhs.pool = nullptr;
    rhs.desc.frame = nullptr;
  }

  wal_frame_write &operator=(wal_frame_write &&rhs) noexcept {
    if (this != &rhs) {
      reset();
      pool = rhs.pool;
      frame = std::move(rhs.frame);
      desc = rhs.desc;
      desc.frame = pool != nullptr ? &frame : nullptr;
      rhs.pool = nullptr;
      rhs.desc.frame = nullptr;
    }
    return *this;
  }

  ~wal_frame_write() { reset(); }

  wal_frame_write(const wal_frame_write &) = delete;
  wal_frame_write &operator=(const wal_frame_write &) = delete;

private:
  void reset() noexcept {
    if (pool != nullptr) {
      try {
        pool->put_frame(std::move(frame));
      } catch (...) {
      }
    }
    pool = nullptr;
    desc.frame = nullptr;
  }
};

// One logical fragment merged into a coalesced entry plan. The leader (the
// prepare caller that issues FUA) is participant[0] with waiter_id == 0; every
// other participant is a follower whose prepare callback is parked on the front
// owner until the leader commits/aborts the single physical plan.
struct wal_plan_participant {
  uint64_t waiter_id = 0;  // 0 == leader self
  wal_fragment_cursor cursor_before{};
  wal_fragment_cursor cursor_after{};
  bool fragment_done = false;
};

struct wal_append_plan {
  wal_append_plan() = default;
  wal_append_plan(wal_append_plan &&) noexcept = default;
  wal_append_plan &operator=(wal_append_plan &&) noexcept = default;
  wal_append_plan(const wal_append_plan &) = delete;
  wal_append_plan &operator=(const wal_append_plan &) = delete;

  uint64_t plan_id = 0;
  wal_plan_kind kind = wal_plan_kind::entries;
  uint32_t stream_id = 0;
  segment_id segment{};
  uint32_t segment_gen = 0;
  uint32_t start_offset = 0;
  uint32_t end_offset = 0;
  uint64_t min_lsn = 0;
  uint64_t max_lsn = 0;
  // Leader-compat fields: mirror participants[0] (the FUA issuer). New code
  // drives per-fragment completion through `participants`; a single-participant
  // plan still works through these legacy fields unchanged.
  wal_fragment_cursor cursor_before{};
  wal_fragment_cursor cursor_after{};
  bool fragment_done = false;
  wal_append_config config{};
  std::optional<sealed_segment_info> sealed_on_commit;
  std::vector<wal_frame_write> writes;
  std::vector<wal_plan_participant> participants;
};

// Leader result: this caller owns the only physical plan, must issue its
// bounded FUA writes, then commit/abort it on the front owner.
struct wal_prepare_issue_plan {
  wal_append_plan plan;
};

// Follower result: the merged WAL bytes are already durable (the leader's FUA
// committed). The caller does no I/O — it only adopts the logical completion.
struct wal_prepare_committed {
  wal_fragment_cursor cursor_after{};
  bool fragment_done = false;
  std::optional<sealed_segment_info> sealed;
};

struct wal_prepare_needs_segment {
  uint32_t stream_id = 0;
  std::optional<sealed_segment_info> sealed;
};

using wal_prepare_result =
    std::variant<wal_prepare_issue_plan,
                 wal_prepare_committed,
                 wal_prepare_needs_segment>;

enum class wal_append_error_reason : uint8_t {
  no_wal_config,
  pending_plan_exists,
  no_active_segment,
  fragment_owner_mismatch,
  fragment_entry_count_mismatch,
  fragment_cursor_out_of_range,
  fragment_entry_index_out_of_range,
  unsupported_op,
  invalid_value_ref,
  key_too_large,
  entry_too_large_for_segment,
  frame_allocation_failed,
  device_failure,
  plan_not_pending,
  plan_id_mismatch,
  prepare_queue_full,
};

class wal_append_error : public std::runtime_error {
public:
  wal_append_error(wal_append_error_reason reason, const char *what)
      : std::runtime_error(what), reason_(reason) {}

  [[nodiscard]] wal_append_error_reason reason() const noexcept {
    return reason_;
  }

private:
  wal_append_error_reason reason_;
};

class wal_stream_state {
public:
  wal_stream_state(uint32_t stream_id, segment_geometry geometry)
      : stream_id_(stream_id), geometry_(geometry) {
    validate_segment_geometry(geometry_);
    trailer_reserved_bytes_ = trailer_reserved_bytes(geometry_);
    lba_shift_ = std::countr_zero(geometry_.lba_size);
    committed_tail_page_.assign(geometry_.lba_size, char{0});
    pending_tail_page_.assign(geometry_.lba_size, char{0});
  }

  [[nodiscard]] uint32_t stream_id() const noexcept { return stream_id_; }

  [[nodiscard]] const segment_geometry &geometry() const noexcept {
    return geometry_;
  }

  [[nodiscard]] segment_runtime *active_segment() const noexcept {
    return active_seg_;
  }

  [[nodiscard]] bool has_active_segment() const noexcept {
    return active_seg_ != nullptr;
  }

  [[nodiscard]] bool header_committed() const noexcept {
    return header_committed_;
  }

  [[nodiscard]] bool has_pending_plan() const noexcept {
    return pending_.has_value();
  }

  [[nodiscard]] uint32_t write_offset() const noexcept { return write_offset_; }

  [[nodiscard]] uint32_t usable_end_offset() const noexcept {
    return geometry_.wal_segment_size - trailer_reserved_bytes_;
  }

  [[nodiscard]] uint32_t segment_bytes() const noexcept {
    return geometry_.wal_segment_size;
  }

  [[nodiscard]] uint32_t lba_size() const noexcept {
    return geometry_.lba_size;
  }

  [[nodiscard]] uint32_t lba_shift() const noexcept { return lba_shift_; }

  [[nodiscard]] format::paddr segment_base() const noexcept {
    return segment_base_;
  }

  [[nodiscard]] std::optional<std::span<const char>>
  tail_image_for(uint64_t page_index) const noexcept {
    if (!committed_tail_valid_ || committed_tail_index_ != page_index) {
      return std::nullopt;
    }
    return std::span<const char>{
        committed_tail_page_.data(), committed_tail_page_.size()};
  }

  [[nodiscard]] bool segment_empty() const noexcept {
    return seg_min_lsn_ == std::numeric_limits<uint64_t>::max();
  }

  [[nodiscard]] uint64_t segment_min_lsn() const noexcept {
    return seg_min_lsn_;
  }

  [[nodiscard]] uint64_t segment_max_lsn() const noexcept {
    return seg_max_lsn_;
  }

  void install_segment(segment_runtime *seg) {
    if (has_pending_plan()) {
      throw std::logic_error(
          "wal::wal_stream_state: cannot install with pending plan");
    }
    if (active_seg_ != nullptr) {
      throw std::logic_error(
          "wal::wal_stream_state: cannot install over active segment");
    }
    if (seg == nullptr) {
      throw std::invalid_argument(
          "wal::wal_stream_state: cannot install null segment");
    }
    if (seg->id.device_id != geometry_.wal_base_paddr.device_id ||
        seg->id.index >= geometry_.wal_segment_count) {
      throw std::logic_error(
          "wal::wal_stream_state: segment geometry mismatch");
    }
    if (seg->st != wal_segment_state::active) {
      throw std::logic_error("wal::wal_stream_state: segment is not active");
    }
    if (seg->owner_stream != stream_id_) {
      throw std::logic_error("wal::wal_stream_state: segment owner mismatch");
    }
    if (seg->segment_gen == 0) {
      throw std::logic_error(
          "wal::wal_stream_state: segment generation is zero");
    }

    active_seg_ = seg;
    segment_base_ = segment_base_paddr(geometry_, seg->id);
    write_offset_ = 0;
    header_committed_ = false;
    seg_min_lsn_ = std::numeric_limits<uint64_t>::max();
    seg_max_lsn_ = 0;
    active_seg_->min_lsn = seg_min_lsn_;
    active_seg_->max_lsn = seg_max_lsn_;
    committed_tail_valid_ = false;
    committed_tail_index_ = 0;
    pending_tail_valid_ = false;
    pending_tail_index_ = 0;
  }

  [[nodiscard]] bool can_fit_entry(uint32_t encoded_len) const noexcept {
    if (active_seg_ == nullptr || encoded_len == 0 || !header_committed_)
      return false;
    if (active_seg_->st != wal_segment_state::active ||
        active_seg_->owner_stream != stream_id_)
      return false;
    const uint32_t usable_end = usable_end_offset();
    if (usable_end <= format::WAL_SEGMENT_HEADER_SIZE)
      return false;
    if (encoded_len > usable_end - format::WAL_SEGMENT_HEADER_SIZE) {
      return false;
    }
    if (write_offset_ > usable_end)
      return false;
    return encoded_len <= usable_end - write_offset_;
  }

  [[nodiscard]] sealed_segment_info make_sealed_info() const {
    if (active_seg_ == nullptr) {
      throw std::logic_error(
          "wal::wal_stream_state: no active segment to seal");
    }
    if (active_seg_->st != wal_segment_state::active ||
        active_seg_->owner_stream != stream_id_) {
      throw std::logic_error(
          "wal::wal_stream_state: active segment ownership changed");
    }
    if (seg_min_lsn_ == std::numeric_limits<uint64_t>::max() ||
        seg_min_lsn_ > seg_max_lsn_) {
      throw std::logic_error(
          "wal::wal_stream_state: cannot seal empty segment");
    }
    if (active_seg_->segment_gen == 0) {
      throw std::logic_error(
          "wal::wal_stream_state: active generation is zero");
    }
    return sealed_segment_info{
        .id = active_seg_->id,
        .segment_gen = active_seg_->segment_gen,
        .min_lsn = seg_min_lsn_,
        .max_lsn = seg_max_lsn_,
    };
  }

  void begin_pending(const wal_append_plan &plan,
                     std::span<const char> last_page_bytes) {
    if (pending_.has_value()) {
      throw wal_append_error(
          wal_append_error_reason::pending_plan_exists,
          "wal::wal_stream_state: pending plan already exists");
    }
    if (active_seg_ == nullptr) {
      throw wal_append_error(wal_append_error_reason::no_active_segment,
                             "wal::wal_stream_state: no active segment");
    }
    if (plan.stream_id != stream_id_ || !(plan.segment == active_seg_->id) ||
        plan.segment_gen != active_seg_->segment_gen) {
      throw std::logic_error("wal::wal_stream_state: plan does not match "
                             "committed cursor");
    }
    const uint32_t usable_end = usable_end_offset();
    switch (plan.kind) {
    case wal_plan_kind::header:
      if (header_committed_ || plan.start_offset != 0 ||
          plan.end_offset != format::WAL_SEGMENT_HEADER_SIZE ||
          plan.sealed_on_commit.has_value()) {
        throw std::logic_error(
            "wal::wal_stream_state: invalid header plan");
      }
      break;
    case wal_plan_kind::entries:
      if (!header_committed_ || plan.start_offset != write_offset_ ||
          plan.end_offset > usable_end || plan.end_offset <= plan.start_offset ||
          plan.sealed_on_commit.has_value()) {
        throw std::logic_error(
            "wal::wal_stream_state: invalid entries plan");
      }
      break;
    case wal_plan_kind::trailer:
      if (plan.start_offset != usable_end ||
          plan.end_offset !=
              usable_end + format::WAL_SEALED_TRAILER_SIZE ||
          !plan.sealed_on_commit.has_value()) {
        throw std::logic_error(
            "wal::wal_stream_state: invalid trailer plan");
      }
      break;
    }

    const bool snapshot_tail = plan.kind != wal_plan_kind::trailer;
    if (snapshot_tail && last_page_bytes.size() != geometry_.lba_size) {
      throw std::logic_error(
          "wal::wal_stream_state: pending tail snapshot size mismatch");
    }
    if (snapshot_tail) {
      std::memcpy(
          pending_tail_page_.data(), last_page_bytes.data(),
          pending_tail_page_.size());
      pending_tail_valid_ = true;
      pending_tail_index_ =
          static_cast<uint64_t>(plan.end_offset - 1) >> lba_shift_;
    } else {
      pending_tail_valid_ = false;
      pending_tail_index_ = 0;
    }

    pending_ = pending_plan{
        .plan_id = plan.plan_id,
        .kind = plan.kind,
        .proposed_write_offset = plan.end_offset,
        .proposed_min_lsn = plan.min_lsn,
        .proposed_max_lsn = plan.max_lsn,
        .sealed = plan.sealed_on_commit,
        .pending_tail_valid = pending_tail_valid_,
        .pending_tail_index = pending_tail_index_,
        .clear_tail_on_commit = plan.kind == wal_plan_kind::trailer,
    };
  }

  [[nodiscard]] std::optional<sealed_segment_info>
  commit_pending(uint64_t plan_id) {
    if (!pending_.has_value()) {
      throw wal_append_error(wal_append_error_reason::plan_not_pending,
                             "wal::wal_stream_state: no pending plan");
    }
    if (pending_->plan_id != plan_id) {
      throw wal_append_error(wal_append_error_reason::plan_id_mismatch,
                             "wal::wal_stream_state: pending plan mismatch");
    }

    std::optional<sealed_segment_info> sealed;
    switch (pending_->kind) {
    case wal_plan_kind::header:
      write_offset_ = pending_->proposed_write_offset;
      header_committed_ = true;
      break;
    case wal_plan_kind::entries:
      write_offset_ = pending_->proposed_write_offset;
      seg_min_lsn_ = pending_->proposed_min_lsn;
      seg_max_lsn_ = pending_->proposed_max_lsn;
      if (active_seg_ != nullptr) {
        active_seg_->min_lsn = seg_min_lsn_;
        active_seg_->max_lsn = seg_max_lsn_;
      }
      break;
    case wal_plan_kind::trailer:
      sealed = pending_->sealed;
      active_seg_ = nullptr;
      segment_base_ = {};
      write_offset_ = 0;
      header_committed_ = false;
      seg_min_lsn_ = std::numeric_limits<uint64_t>::max();
      seg_max_lsn_ = 0;
      break;
    }

    if (pending_->clear_tail_on_commit || sealed.has_value()) {
      committed_tail_valid_ = false;
      committed_tail_index_ = 0;
      std::fill(
          committed_tail_page_.begin(), committed_tail_page_.end(), char{0});
    } else if (pending_->pending_tail_valid) {
      std::swap(committed_tail_page_, pending_tail_page_);
      committed_tail_valid_ = true;
      committed_tail_index_ = pending_->pending_tail_index;
    }
    pending_tail_valid_ = false;
    pending_tail_index_ = 0;
    pending_.reset();
    return sealed;
  }

  void abort_pending(uint64_t plan_id) {
    if (!pending_.has_value()) {
      throw wal_append_error(wal_append_error_reason::plan_not_pending,
                             "wal::wal_stream_state: no pending plan");
    }
    if (pending_->plan_id != plan_id) {
      throw wal_append_error(wal_append_error_reason::plan_id_mismatch,
                             "wal::wal_stream_state: pending plan mismatch");
    }
    pending_tail_valid_ = false;
    pending_tail_index_ = 0;
    pending_.reset();
  }

private:
  struct pending_plan {
    uint64_t plan_id = 0;
    wal_plan_kind kind = wal_plan_kind::entries;
    uint32_t proposed_write_offset = 0;
    uint64_t proposed_min_lsn = 0;
    uint64_t proposed_max_lsn = 0;
    std::optional<sealed_segment_info> sealed;
    bool pending_tail_valid = false;
    uint64_t pending_tail_index = 0;
    bool clear_tail_on_commit = false;
  };

  uint32_t stream_id_;
  segment_geometry geometry_;
  segment_runtime *active_seg_ = nullptr;
  uint32_t lba_shift_ = 0;
  format::paddr segment_base_{};
  uint32_t write_offset_ = 0;
  uint32_t trailer_reserved_bytes_ = 0;
  bool header_committed_ = false;
  uint64_t seg_min_lsn_ = std::numeric_limits<uint64_t>::max();
  uint64_t seg_max_lsn_ = 0;
  std::vector<char> committed_tail_page_;
  bool committed_tail_valid_ = false;
  uint64_t committed_tail_index_ = 0;
  std::vector<char> pending_tail_page_;
  bool pending_tail_valid_ = false;
  uint64_t pending_tail_index_ = 0;
  std::optional<pending_plan> pending_;
};

static_assert(!std::is_copy_constructible_v<wal_frame_write>);
static_assert(std::is_move_constructible_v<wal_frame_write>);
static_assert(!std::is_copy_constructible_v<wal_append_plan>);
static_assert(std::is_move_constructible_v<wal_append_plan>);

} // namespace apps::inconel::wal

#endif // APPS_INCONEL_FRONT_WAL_APPEND_HH
