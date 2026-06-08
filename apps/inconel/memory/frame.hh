#ifndef APPS_INCONEL_MEMORY_FRAME_HH
#define APPS_INCONEL_MEMORY_FRAME_HH

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include "absl/container/inlined_vector.h"

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
            nvme_scratch,
            superblock_page,
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

    // ── lba_dma_page ──
    //
    // One DMA-resident payload page whose size equals the device LBA size.
    // Multi-LBA logical frames are segmented as multiple lba_dma_page
    // instances; v1 deliberately does not require a DMA-contiguous span and
    // does not model SGL ownership here.

    struct lba_dma_page {
        char*    buf = nullptr;
        uint32_t byte_len = 0;
    };

    // ── segmented_page_frame ──
    //
    // Logical frame carrier for the real NVMe/DMA path. `id.base` and
    // `id.span_lbas` describe the contiguous logical LBA range; `pages`
    // holds one independently allocated LBA-sized DMA payload per logical
    // LBA. A caller that needs byte access must address a specific LBA
    // segment instead of assuming a contiguous buffer.

    struct segmented_page_frame {
        frame_id    id{};
        frame_state st = frame_state::clean_readonly;
        absl::InlinedVector<lba_dma_page*, 4> pages;
        uint32_t    pin_count = 0;
        bool        crc_valid = false;

        [[nodiscard]] uint16_t
        span_lbas() const noexcept {
            return id.span_lbas;
        }

        [[nodiscard]] bool
        complete() const noexcept {
            return id.span_lbas == pages.size();
        }

        [[nodiscard]] uint32_t
        lba_size() const noexcept {
            assert(!pages.empty());
            return pages.front()->byte_len;
        }

        [[nodiscard]] uint64_t
        byte_len() const noexcept {
            return static_cast<uint64_t>(id.span_lbas) * lba_size();
        }

        [[nodiscard]] lba_dma_page*
        page_at(uint16_t offset) noexcept {
            assert(offset < pages.size());
            return pages[offset];
        }

        [[nodiscard]] const lba_dma_page*
        page_at(uint16_t offset) const noexcept {
            assert(offset < pages.size());
            return pages[offset];
        }

        [[nodiscard]] std::span<lba_dma_page* const>
        page_span() const noexcept {
            return {pages.data(), pages.size()};
        }

        [[nodiscard]] std::span<char>
        mutable_lba_bytes(uint16_t offset) noexcept {
            auto* page = page_at(offset);
            return {page->buf, page->byte_len};
        }

        [[nodiscard]] std::span<const char>
        lba_bytes(uint16_t offset) const noexcept {
            const auto* page = page_at(offset);
            return {page->buf, page->byte_len};
        }

        [[nodiscard]] std::span<char>
        mutable_contiguous_bytes(uint64_t byte_offset,
                                 uint32_t bytes) noexcept {
            const uint32_t lba = lba_size();
            const uint64_t page_idx = byte_offset / lba;
            const uint32_t page_off = static_cast<uint32_t>(byte_offset % lba);
            assert(page_idx < pages.size());
            assert(static_cast<uint64_t>(page_off) + bytes <=
                   pages[page_idx]->byte_len);
            return {pages[page_idx]->buf + page_off, bytes};
        }

        [[nodiscard]] std::span<const char>
        contiguous_bytes(uint64_t byte_offset, uint32_t bytes) const noexcept {
            const uint32_t lba = lba_size();
            const uint64_t page_idx = byte_offset / lba;
            const uint32_t page_off = static_cast<uint32_t>(byte_offset % lba);
            assert(page_idx < pages.size());
            assert(static_cast<uint64_t>(page_off) + bytes <=
                   pages[page_idx]->byte_len);
            return {pages[page_idx]->buf + page_off, bytes};
        }

        void
        copy_from(uint64_t byte_offset, const void* src, uint64_t bytes) {
            assert(src != nullptr || bytes == 0);
            assert(byte_offset <= byte_len());
            assert(bytes <= byte_len() - byte_offset);
            const char* in = static_cast<const char*>(src);
            uint64_t copied = 0;
            while (copied < bytes) {
                const uint64_t pos = byte_offset + copied;
                const uint32_t lba = lba_size();
                const uint64_t page_idx = pos / lba;
                const uint32_t page_off = static_cast<uint32_t>(pos % lba);
                auto* page = pages[page_idx];
                const uint64_t n = std::min<uint64_t>(
                    page->byte_len - page_off, bytes - copied);
                std::memcpy(page->buf + page_off, in + copied, n);
                copied += n;
            }
        }

        void
        copy_to(uint64_t byte_offset, void* dst, uint64_t bytes) const {
            assert(dst != nullptr || bytes == 0);
            assert(byte_offset <= byte_len());
            assert(bytes <= byte_len() - byte_offset);
            char* out = static_cast<char*>(dst);
            uint64_t copied = 0;
            while (copied < bytes) {
                const uint64_t pos = byte_offset + copied;
                const uint32_t lba = lba_size();
                const uint64_t page_idx = pos / lba;
                const uint32_t page_off = static_cast<uint32_t>(pos % lba);
                const auto* page = pages[page_idx];
                const uint64_t n = std::min<uint64_t>(
                    page->byte_len - page_off, bytes - copied);
                std::memcpy(out + copied, page->buf + page_off, n);
                copied += n;
            }
        }

        void
        copy_from_contiguous(const void* src, uint64_t bytes) {
            assert(src != nullptr || bytes == 0);
            assert(bytes <= byte_len());
            const char* in = static_cast<const char*>(src);
            uint64_t copied = 0;
            for (auto* page : pages) {
                if (copied == bytes) break;
                const uint64_t n = std::min<uint64_t>(
                    page->byte_len, bytes - copied);
                std::memcpy(page->buf, in + copied, n);
                if (n < page->byte_len) {
                    std::memset(page->buf + n, 0, page->byte_len - n);
                }
                copied += n;
            }
        }

        void
        copy_to_contiguous(void* dst, uint64_t bytes) const {
            assert(dst != nullptr || bytes == 0);
            assert(bytes <= byte_len());
            char* out = static_cast<char*>(dst);
            uint64_t copied = 0;
            for (auto* page : pages) {
                if (copied == bytes) break;
                const uint64_t n = std::min<uint64_t>(
                    page->byte_len, bytes - copied);
                std::memcpy(out + copied, page->buf, n);
                copied += n;
            }
        }
    };

    struct tree_page_frame : segmented_page_frame {
        tree_page_frame() = default;
        explicit tree_page_frame(segmented_page_frame&& frame)
            : segmented_page_frame(std::move(frame)) {}
    };

    struct value_page_frame : segmented_page_frame {
        value_page_frame() = default;
        explicit value_page_frame(segmented_page_frame&& frame)
            : segmented_page_frame(std::move(frame)) {}

        uint16_t class_idx = 0;
        uint16_t slots_per_lba = 0;
        uint16_t free_count = 0;

        enum class open_mode : uint8_t {
            none,
            append,
            hole_fill,
        } mode = open_mode::none;
    };

    using segmented_tree_frame = tree_page_frame;
    using segmented_value_frame = value_page_frame;

    struct frame_read_desc {
        segmented_page_frame* frame = nullptr;
    };

    struct frame_write_desc {
        segmented_page_frame* frame = nullptr;
        uint32_t              flags = 0;
    };

    struct segmented_frame_pin {
        segmented_page_frame* frame = nullptr;

        explicit segmented_frame_pin(segmented_page_frame* f) noexcept
            : frame(f) {
            if (f) ++f->pin_count;
        }

        ~segmented_frame_pin() {
            if (frame) --frame->pin_count;
        }

        segmented_frame_pin(segmented_frame_pin&& o) noexcept
            : frame(o.frame) {
            o.frame = nullptr;
        }

        segmented_frame_pin& operator=(segmented_frame_pin&& o) noexcept {
            if (this != &o) {
                if (frame) --frame->pin_count;
                frame = o.frame;
                o.frame = nullptr;
            }
            return *this;
        }

        segmented_frame_pin(const segmented_frame_pin&) = delete;
        segmented_frame_pin& operator=(const segmented_frame_pin&) = delete;

        explicit operator bool() const noexcept { return frame != nullptr; }
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
