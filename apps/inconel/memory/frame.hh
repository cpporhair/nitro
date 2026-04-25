#ifndef APPS_INCONEL_MEMORY_FRAME_HH
#define APPS_INCONEL_MEMORY_FRAME_HH

#include <cassert>
#include <cstdint>

#include "../format/types.hh"

namespace apps::inconel::memory {

    using format::paddr;

    // ── frame_id ──
    //
    // Identifies a resident page image: which device address, how many LBAs
    // it spans, and which domain owns the interpretation.
    //
    // Cache key is {base, span_lbas, dom}. v1 does not carry a reuse_epoch;
    // the owner must complete a mandatory invalidate barrier before the same
    // address re-enters the cache under a different logical identity (see
    // runtime_memory_and_cache.md §5.2 / §10.2).

    struct frame_id {
        paddr    base;
        uint16_t span_lbas;

        enum class domain : uint8_t {
            tree_node,
            value_page,
            wal_page,
            tree_writeback,
        } dom;

        bool operator==(const frame_id&) const = default;

        template <typename H>
        friend H
        AbslHashValue(H h, const frame_id& f) {
            uint16_t dev  = f.base.device_id;
            uint64_t lba  = f.base.lba;
            uint16_t span = f.span_lbas;
            uint8_t  d    = static_cast<uint8_t>(f.dom);
            return H::combine(std::move(h), dev, lba, span, d);
        }
    };

    // ── frame_state ──
    //
    // Only clean_readonly participates in the readonly frame cache (this
    // step). The remaining states exist for future dirty/writeback tracking
    // and are defined here so the enum is complete from day one.

    enum class frame_state : uint8_t {
        clean_readonly,
        clean_allocatable,
        dirty_append,
        dirty_hole_fill,
        writeback_inflight,
    };

    // ── page_frame ──
    //
    // Runtime descriptor for a resident page image. The backing buffer
    // (buf) is heap-allocated in this step (D2); a later step replaces it
    // with DMA memory from an SPDK pool (INC-016).
    //
    // Ownership model: the cache / pool / dirty set holds the page_frame*;
    // consumers obtain a frame_pin (RAII) that increments pin_count. A
    // frame with pin_count > 0 must not be evicted.

    struct page_frame {
        frame_id    id;
        frame_state st;
        char*       buf;
        uint32_t    byte_len;
        uint32_t    pin_count;
        bool        crc_valid;
    };

    // ── frame_pin ──
    //
    // RAII pin token. Construction increments pin_count; destruction
    // decrements it. Move-only. The pin does not own the frame — the
    // cache / pool retains ownership of the page_frame itself.

    struct frame_pin {
        page_frame* frame = nullptr;

        explicit frame_pin(page_frame* f) noexcept : frame(f) {
            if (f) ++f->pin_count;
        }

        ~frame_pin() {
            if (frame) --frame->pin_count;
        }

        frame_pin(frame_pin&& o) noexcept : frame(o.frame) {
            o.frame = nullptr;
        }

        frame_pin& operator=(frame_pin&& o) noexcept {
            if (this != &o) {
                if (frame) --frame->pin_count;
                frame = o.frame;
                o.frame = nullptr;
            }
            return *this;
        }

        frame_pin(const frame_pin&) = delete;
        frame_pin& operator=(const frame_pin&) = delete;

        explicit operator bool() const noexcept { return frame != nullptr; }
    };

}

#endif //APPS_INCONEL_MEMORY_FRAME_HH
