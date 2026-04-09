#ifndef APPS_INCONEL_VALUE_ALLOCATOR_HH
#define APPS_INCONEL_VALUE_ALLOCATOR_HH

#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <absl/container/inlined_vector.h>

#include "../format/types.hh"
#include "../format/value_object.hh"

namespace apps::inconel::value {

    using format::paddr;

    // ── Page source enum ──
    //
    // Records why a particular page is being used in the current persist
    // round. The distinction matters for rollback: writable pages must be
    // returned to writable_pages_[ci] with the original free_mask, while
    // fresh_bump / whole_page pages were never durable and can be dropped.
    //
    // value_page_source::writable is only constructed by the scheduler in
    // acquire_round_page when it pops from writable_pages_[ci];
    // value_allocator::acquire_page never returns it (the allocator does not
    // know about writable pages).

    enum class value_page_source : uint8_t {
        writable,      // popped from writable_pages_[ci] (still has free slots)
        whole_page,    // taken from whole_pool[ci] (recycled empty page)
        fresh_bump,    // freshly bumped from per_device head
    };

    // ── Allocator results ──

    struct value_alloc_result {
        paddr             page_base;
        uint16_t          class_idx;
        uint32_t          span_lbas;
        value_page_source source;     // only whole_page or fresh_bump
        uint64_t          free_mask;  // bitmask of free slots in this page
    };

    // ── Per-device bump head ──
    //
    // v6 simplification: no shared collision detection with tree allocator
    // (no tree_sched yet). The bump head walks down from data_area_end to
    // data_area_base; once it crosses base, allocations fail with nullopt.
    //
    // Future: add shared `data_area_heads*` and atomic load of tree head
    // for collision detection (matches inconel design_doc §10.4).

    struct per_device_value_state {
        uint16_t device_id;
        uint64_t bump_head_lba;     // next allocation site (decreases)
        uint64_t data_area_base_lba;

        per_device_value_state(paddr data_area_base, paddr data_area_end) noexcept
            : device_id(data_area_end.device_id)
            , bump_head_lba(data_area_end.lba)
            , data_area_base_lba(data_area_base.lba)
        {
            assert(data_area_base.device_id == data_area_end.device_id);
            assert(data_area_base.lba <= data_area_end.lba);
        }

        std::optional<paddr>
        bump_next_page(uint32_t span_lbas) noexcept {
            if (bump_head_lba < span_lbas) return std::nullopt;
            uint64_t next = bump_head_lba - span_lbas;
            if (next < data_area_base_lba) return std::nullopt;
            bump_head_lba = next;
            return paddr{device_id, bump_head_lba};
        }

        paddr current_head() const noexcept {
            return paddr{device_id, bump_head_lba};
        }
    };

    // ── value_allocator ──
    //
    // Pure placement state; no I/O, no page images, no DMA. Decides which
    // physical page should host the next slot for a given class. The
    // scheduler layer above owns the page images and the cache.
    //
    // v6 scope: no hole_pool / freed_slots / install_recovered_state /
    // collision detection. Future steps will fill these in when tree_sched
    // can drive the reclaim path.

    class value_allocator {
    public:
        struct per_class {
            uint32_t class_size;     // bytes per slot
            uint32_t span_lbas;      // 1 for sub-LBA / LBA-equal, >=1 for multi-LBA
            bool     sub_lba;        // true if class_size < lba_size
            uint32_t slots_per_page; // sub-LBA: lba_size/class_size; otherwise 1
            uint64_t all_free_mask;  // (1 << slots_per_page) - 1, capped at UINT64_MAX

            std::vector<paddr> whole_pool;
        };

        value_allocator(std::span<const uint32_t> class_sizes,
                        uint32_t                  lba_size,
                        paddr                     data_area_base,
                        paddr                     data_area_end) noexcept
            : dev_(data_area_base, data_area_end)
            , lba_size_(lba_size)
        {
            classes_.reserve(class_sizes.size());
            for (uint32_t cs : class_sizes) {
                per_class pc{};
                pc.class_size = cs;
                if (cs < lba_size) {
                    assert(lba_size % cs == 0 && "sub-LBA class must divide lba_size");
                    pc.sub_lba        = true;
                    pc.span_lbas      = 1;
                    pc.slots_per_page = lba_size / cs;
                } else {
                    assert(cs % lba_size == 0 && "LBA-aligned class must be lba_size multiple");
                    pc.sub_lba        = false;
                    pc.span_lbas      = cs / lba_size;
                    pc.slots_per_page = 1;
                }
                pc.all_free_mask = (pc.slots_per_page < 64)
                    ? ((1ULL << pc.slots_per_page) - 1)
                    : UINT64_MAX;
                classes_.push_back(std::move(pc));
            }
        }

        // ── acquire_page ──
        //
        // Decision tree (v7):
        //   1. whole_pool[ci] non-empty?       — return whole_page
        //   2. bump fresh from per_device head — return fresh_bump
        //   3. otherwise nullopt (out of space)
        //
        // The "writable" path (pages still owned by the scheduler with free
        // slots remaining) is handled entirely by the scheduler — the
        // allocator does not know it exists. acquire_page is only called when
        // the scheduler has exhausted writable_pages_[ci].

        std::optional<value_alloc_result>
        acquire_page(uint16_t class_idx) noexcept {
            auto& cls = classes_[class_idx];

            if (!cls.whole_pool.empty()) {
                paddr base = cls.whole_pool.back();
                cls.whole_pool.pop_back();
                return value_alloc_result{
                    .page_base = base,
                    .class_idx = class_idx,
                    .span_lbas = cls.span_lbas,
                    .source    = value_page_source::whole_page,
                    .free_mask = cls.all_free_mask,
                };
            }

            auto fresh = dev_.bump_next_page(cls.span_lbas);
            if (!fresh) return std::nullopt;
            return value_alloc_result{
                .page_base = *fresh,
                .class_idx = class_idx,
                .span_lbas = cls.span_lbas,
                .source    = value_page_source::fresh_bump,
                .free_mask = cls.all_free_mask,
            };
        }

        // ── reclaim ──
        //
        // Pages whose all slots are once again free can be returned to the
        // whole_pool for reuse without going through bump.

        void
        recycle_whole_page(uint16_t class_idx, paddr page_base) noexcept {
            classes_[class_idx].whole_pool.push_back(page_base);
        }

        // ── inspectors ──

        const per_class&
        get_class(uint16_t class_idx) const noexcept {
            return classes_[class_idx];
        }

        uint16_t
        class_count() const noexcept {
            return static_cast<uint16_t>(classes_.size());
        }

        uint64_t
        all_free_mask(uint16_t class_idx) const noexcept {
            return classes_[class_idx].all_free_mask;
        }

        uint32_t
        slots_per_page(uint16_t class_idx) const noexcept {
            return classes_[class_idx].slots_per_page;
        }

        uint32_t
        span_lbas(uint16_t class_idx) const noexcept {
            return classes_[class_idx].span_lbas;
        }

        bool
        is_sub_lba(uint16_t class_idx) const noexcept {
            return classes_[class_idx].sub_lba;
        }

        uint32_t
        class_size(uint16_t class_idx) const noexcept {
            return classes_[class_idx].class_size;
        }

        uint32_t
        lba_size() const noexcept {
            return lba_size_;
        }

        paddr
        bump_head() const noexcept {
            return dev_.current_head();
        }

    private:
        per_device_value_state dev_;
        uint32_t               lba_size_;
        // on-disk format caps value_size_classes at 16 (superblock §2 in
        // on_disk_formats.md), so the per-class metadata never grows past
        // 16 entries — keep it inline to dodge the heap allocation.
        absl::InlinedVector<per_class, 16> classes_;
    };

}

#endif //APPS_INCONEL_VALUE_ALLOCATOR_HH
