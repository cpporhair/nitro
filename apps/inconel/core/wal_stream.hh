#ifndef APPS_INCONEL_CORE_WAL_STREAM_HH
#define APPS_INCONEL_CORE_WAL_STREAM_HH

#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "../format/superblock.hh"
#include "../format/types.hh"
#include "../format/wal.hh"

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
  if (!std::has_single_bit(geometry.lba_size)) {
    throw std::invalid_argument(
        "wal::segment_geometry: lba_size must be a power of two");
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

} // namespace apps::inconel::wal

#endif // APPS_INCONEL_CORE_WAL_STREAM_HH
