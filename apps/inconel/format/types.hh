#ifndef APPS_INCONEL_FORMAT_TYPES_HH
#define APPS_INCONEL_FORMAT_TYPES_HH

#include <cstdint>
#include <functional>

namespace apps::inconel::format {

    struct __attribute__((packed)) paddr {
        uint16_t device_id;
        uint64_t lba;

        bool operator==(const paddr&) const = default;

        bool
        operator<(const paddr& rhs) const {
            if (device_id != rhs.device_id) return device_id < rhs.device_id;
            return lba < rhs.lba;
        }

        // Abseil hash adapter — found via ADL by absl::Hash and any
        // absl::flat_hash_{map,set}<paddr, ...> instantiation. The legacy
        // std::hash<paddr> below is kept so existing std::unordered_*
        // call sites outside this step continue to compile.
        //
        // The members are copied into locals first because paddr is
        // __attribute__((packed)) — H::combine() takes its arguments by
        // const&, and binding a reference directly to p.lba would trip
        // -Waddress-of-packed-member.
        template <typename H>
        friend H
        AbslHashValue(H h, const paddr& p) {
            uint16_t dev = p.device_id;
            uint64_t lba = p.lba;
            return H::combine(std::move(h), dev, lba);
        }
    };
    static_assert(sizeof(paddr) == 10);

    struct __attribute__((packed)) range_ref {
        paddr base;
        uint32_t slot_count;
    };
    static_assert(sizeof(range_ref) == 14);

    struct __attribute__((packed)) value_ref {
        paddr    base;
        uint16_t byte_offset;
        uint32_t len;
        uint16_t flags;
    };
    static_assert(sizeof(value_ref) == 18);

    // ── I/O descriptors for batch operations ──

    struct write_desc {
        uint64_t lba;
        const void* data;
        uint32_t num_lbas;
        uint32_t flags;
    };

    struct read_desc {
        uint64_t lba;
        void* buf;
        uint32_t num_lbas;
    };

    struct trim_desc {
        uint64_t lba;
        uint32_t num_lbas;
    };

}

template<>
struct std::hash<apps::inconel::format::paddr> {
    std::size_t
    operator()(const apps::inconel::format::paddr& p) const noexcept {
        auto h1 = std::hash<uint16_t>{}(p.device_id);
        auto h2 = std::hash<uint64_t>{}(p.lba);
        return h1 ^ (h2 * 0x9E3779B97F4A7C15ULL + 0x9E3779B9 + (h1 << 6) + (h1 >> 2));
    }
};

#endif //APPS_INCONEL_FORMAT_TYPES_HH
