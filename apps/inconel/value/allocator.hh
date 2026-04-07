#ifndef APPS_INCONEL_VALUE_ALLOCATOR_HH
#define APPS_INCONEL_VALUE_ALLOCATOR_HH

#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "../format/types.hh"
#include "../format/value_object.hh"

namespace apps::inconel::value {

    using format::paddr;

    // ── Page source enum ──
    //
    // Records why a particular page is being used in the current persist
    // round. Currently consumed only by allocator/scheduler internals; the
    // distinction matters for rollback (fresh_bump pages return to
    // whole_pool, open_frame pages return to open_pages_, etc).

    enum class value_page_source : uint8_t {
        open_frame,    // taken from open_pages_[ci] (still has free slots)
        ready_page,    // taken from ready_pages_[ci] (also has free slots)
        whole_page,    // taken from whole_pool[ci] (recycled empty page)
        fresh_bump,    // freshly bumped from per_device head
    };

    // ── Allocator results ──

    struct value_alloc_result {
        paddr             page_base;
        uint16_t          class_idx;
        uint32_t          span_lbas;
        value_page_source source;
        uint64_t          free_mask;   // bitmask of free slots in this page
    };

    struct value_open_page_meta {
        paddr    page_base;
        uint16_t class_idx;
        uint64_t free_mask;
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

            std::optional<value_open_page_meta> open;
            std::vector<paddr>                  whole_pool;
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
        // Decision tree (v6, no hole reuse):
        //   1. open_pages_[ci] still has free slots? — return open_frame
        //   2. whole_pool[ci] non-empty?            — return whole_page
        //   3. bump fresh from per_device head      — return fresh_bump
        //   4. otherwise nullopt (out of space)

        std::optional<value_alloc_result>
        acquire_page(uint16_t class_idx) noexcept {
            auto& cls = classes_[class_idx];

            if (cls.open.has_value() && cls.open->free_mask != 0) {
                return value_alloc_result{
                    .page_base = cls.open->page_base,
                    .class_idx = class_idx,
                    .span_lbas = cls.span_lbas,
                    .source    = value_page_source::open_frame,
                    .free_mask = cls.open->free_mask,
                };
            }

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

        // ── open page state mutators ──
        //
        // Called by the scheduler after a transaction commits / rolls back
        // and the page is being returned to the active set.

        void
        install_open_page(value_open_page_meta meta) noexcept {
            if (meta.free_mask == 0) {
                classes_[meta.class_idx].open.reset();
                return;
            }
            classes_[meta.class_idx].open = meta;
        }

        void
        close_open_page(uint16_t class_idx) noexcept {
            classes_[class_idx].open.reset();
        }

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
        std::vector<per_class> classes_;
    };

}

#endif //APPS_INCONEL_VALUE_ALLOCATOR_HH
