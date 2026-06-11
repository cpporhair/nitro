#ifndef APPS_INCONEL_CORE_WAL_STREAM_HH
#define APPS_INCONEL_CORE_WAL_STREAM_HH

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "../format/superblock.hh"
#include "../format/types.hh"
#include "../format/wal.hh"
#include "../memory/dma_page_pool.hh"
#include "./batch_carrier.hh"

namespace apps::inconel::wal {

inline constexpr uint32_t kMaxSupportedWalKeyBytes = 1024;
inline constexpr uint32_t kMaxSupportedWalEntrySize =
    format::wal_put_entry_size(kMaxSupportedWalKeyBytes);
static_assert(kMaxSupportedWalKeyBytes <= format::WAL_PUT_MAX_KEY_LEN);
static_assert(kMaxSupportedWalKeyBytes <= format::WAL_DELETE_MAX_KEY_LEN);

struct segment_id {
  uint16_t device_id = 0;
  uint32_t index = 0;

  bool operator==(const segment_id &) const = default;
};

enum class wal_segment_state : uint8_t {
  free = 1,
  active = 2,
  sealed = 3,
};

struct segment_runtime {
  segment_id id{};
  uint32_t owner_stream = std::numeric_limits<uint32_t>::max();
  uint32_t segment_gen = 0;
  wal_segment_state st = wal_segment_state::free;
  uint64_t min_lsn = 0;
  uint64_t max_lsn = 0;
};

struct segment_alloc_entry {
  segment_id id{};
  uint32_t next_gen = 0;
};

struct sealed_segment_info {
  segment_id id{};
  uint32_t segment_gen = 0;
  uint64_t min_lsn = 0;
  uint64_t max_lsn = 0;
};

struct segment_geometry {
  format::paddr wal_base_paddr{
      .device_id = 0,
      .lba = 0,
  };
  uint32_t wal_segment_size = 0;
  uint32_t lba_size = 0;
  uint32_t wal_segment_count = 0;
  uint32_t expected_format_version = format::SUPERBLOCK_FORMAT_VERSION_V1;
};

[[nodiscard]] inline uint32_t align_up_u32(uint32_t value, uint32_t align) {
  if (align == 0) {
    throw std::invalid_argument("wal: align must be nonzero");
  }
  const uint64_t rounded =
      ((static_cast<uint64_t>(value) + align - 1) / align) * align;
  if (rounded > std::numeric_limits<uint32_t>::max()) {
    throw std::invalid_argument("wal: aligned value overflows u32");
  }
  return static_cast<uint32_t>(rounded);
}

[[nodiscard]] inline uint32_t
trailer_reserved_bytes(const segment_geometry &geometry) {
  return align_up_u32(format::WAL_SEALED_TRAILER_SIZE, geometry.lba_size);
}

inline void validate_segment_geometry(const segment_geometry &geometry) {
  if (geometry.expected_format_version !=
      format::SUPERBLOCK_FORMAT_VERSION_V1) {
    throw std::invalid_argument(
        "wal::segment_geometry: unsupported format version");
  }
  if (geometry.wal_base_paddr.device_id != 0) {
    throw std::invalid_argument(
        "wal::segment_geometry: v1 WAL device_id must be 0");
  }
  if (geometry.wal_segment_count == 0) {
    throw std::invalid_argument(
        "wal::segment_geometry: wal_segment_count must be nonzero");
  }
  if (geometry.lba_size == 0) {
    throw std::invalid_argument(
        "wal::segment_geometry: lba_size must be nonzero");
  }
  if (geometry.wal_segment_size == 0) {
    throw std::invalid_argument(
        "wal::segment_geometry: wal_segment_size must be nonzero");
  }
  if (geometry.wal_segment_size % geometry.lba_size != 0) {
    throw std::invalid_argument(
        "wal::segment_geometry: wal_segment_size must be LBA-aligned");
  }

  const uint32_t trailer_reserved = trailer_reserved_bytes(geometry);
  if (format::WAL_SEGMENT_HEADER_SIZE + trailer_reserved >=
      geometry.wal_segment_size) {
    throw std::invalid_argument(
        "wal::segment_geometry: header and trailer fill segment");
  }
  const uint32_t entry_area = geometry.wal_segment_size -
                              format::WAL_SEGMENT_HEADER_SIZE -
                              trailer_reserved;
  if (entry_area < kMaxSupportedWalEntrySize) {
    throw std::invalid_argument(
        "wal::segment_geometry: segment cannot fit max supported WAL "
        "entry");
  }
}

[[nodiscard]] inline uint32_t
segment_usable_end_offset(const segment_geometry &geometry) {
  validate_segment_geometry(geometry);
  return geometry.wal_segment_size - trailer_reserved_bytes(geometry);
}

[[nodiscard]] inline format::paddr
segment_base_paddr(const segment_geometry &geometry, segment_id id) {
  validate_segment_geometry(geometry);
  if (id.device_id != geometry.wal_base_paddr.device_id) {
    throw std::out_of_range("wal::segment_base_paddr: segment device mismatch");
  }
  if (id.index >= geometry.wal_segment_count) {
    throw std::out_of_range(
        "wal::segment_base_paddr: segment index out of range");
  }

  const uint64_t segment_lbas = geometry.wal_segment_size / geometry.lba_size;
  if (id.index != 0 && segment_lbas > (std::numeric_limits<uint64_t>::max() -
                                       geometry.wal_base_paddr.lba) /
                                          id.index) {
    throw std::overflow_error("wal::segment_base_paddr: segment LBA overflow");
  }

  return format::paddr{
      .device_id = geometry.wal_base_paddr.device_id,
      .lba = geometry.wal_base_paddr.lba +
             static_cast<uint64_t>(id.index) * segment_lbas,
  };
}

struct wal_append_config {
  uint32_t max_fua_inflight = 4;
  uint32_t max_pages_per_plan = 4;
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
  memory::pooled_frame_ptr<memory::segmented_page_frame> frame;
  memory::frame_write_desc desc{};

  wal_frame_write() = default;
  wal_frame_write(memory::pooled_frame_ptr<memory::segmented_page_frame> f,
                  uint32_t flags = 0) noexcept
      : frame(std::move(f)), desc{.frame = frame.get(), .flags = flags} {}

  wal_frame_write(wal_frame_write &&rhs) noexcept
      : frame(std::move(rhs.frame)), desc(rhs.desc) {
    desc.frame = frame.get();
  }

  wal_frame_write &operator=(wal_frame_write &&rhs) noexcept {
    if (this != &rhs) {
      frame = std::move(rhs.frame);
      desc = rhs.desc;
      desc.frame = frame.get();
    }
    return *this;
  }

  wal_frame_write(const wal_frame_write &) = delete;
  wal_frame_write &operator=(const wal_frame_write &) = delete;
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
  wal_fragment_cursor cursor_before{};
  wal_fragment_cursor cursor_after{};
  bool fragment_done = false;
  wal_append_config config{};
  std::optional<sealed_segment_info> sealed_on_commit;
  std::vector<wal_frame_write> writes;
};

struct wal_prepare_ready {
  wal_append_plan plan;
};

struct wal_prepare_needs_segment {
  uint32_t stream_id = 0;
  std::optional<sealed_segment_info> sealed;
};

using wal_prepare_result =
    std::variant<wal_prepare_ready, wal_prepare_needs_segment>;

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
    write_offset_ = 0;
    header_committed_ = false;
    seg_min_lsn_ = std::numeric_limits<uint64_t>::max();
    seg_max_lsn_ = 0;
    active_seg_->min_lsn = seg_min_lsn_;
    active_seg_->max_lsn = seg_max_lsn_;
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

  void begin_pending(const wal_append_plan &plan) {
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
        plan.segment_gen != active_seg_->segment_gen ||
        plan.start_offset != write_offset_) {
      throw std::logic_error("wal::wal_stream_state: plan does not match "
                             "committed cursor");
    }

    pending_ = pending_plan{
        .plan_id = plan.plan_id,
        .kind = plan.kind,
        .proposed_write_offset = plan.end_offset,
        .proposed_min_lsn = plan.min_lsn,
        .proposed_max_lsn = plan.max_lsn,
        .sealed = plan.sealed_on_commit,
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
      write_offset_ = 0;
      header_committed_ = false;
      seg_min_lsn_ = std::numeric_limits<uint64_t>::max();
      seg_max_lsn_ = 0;
      break;
    }

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
  };

  uint32_t stream_id_;
  segment_geometry geometry_;
  segment_runtime *active_seg_ = nullptr;
  uint32_t write_offset_ = 0;
  uint32_t trailer_reserved_bytes_ = 0;
  bool header_committed_ = false;
  uint64_t seg_min_lsn_ = std::numeric_limits<uint64_t>::max();
  uint64_t seg_max_lsn_ = 0;
  std::optional<pending_plan> pending_;
};

static_assert(!std::is_copy_constructible_v<wal_frame_write>);
static_assert(std::is_move_constructible_v<wal_frame_write>);
static_assert(!std::is_copy_constructible_v<wal_append_plan>);
static_assert(std::is_move_constructible_v<wal_append_plan>);

} // namespace apps::inconel::wal

#endif // APPS_INCONEL_CORE_WAL_STREAM_HH
