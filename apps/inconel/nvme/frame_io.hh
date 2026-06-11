#ifndef APPS_INCONEL_NVME_FRAME_IO_HH
#define APPS_INCONEL_NVME_FRAME_IO_HH

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/just.hh"
#include "pump/sender/reduce.hh"

#include "../memory/frame.hh"
#include "./runtime_scheduler.hh"

namespace apps::inconel::nvme {

    inline auto
    read_frame(runtime_scheduler* sched,
               memory::segmented_page_frame* frame,
               uint32_t flags = 0) {
        assert(sched != nullptr);
        assert(frame != nullptr);
        assert(frame->complete());
        return sched->read_frame(frame, flags);
    }

    inline auto
    write_frame(runtime_scheduler* sched,
                memory::segmented_page_frame* frame,
                uint32_t flags = 0) {
        assert(sched != nullptr);
        assert(frame != nullptr);
        assert(frame->complete());
        return sched->write_frame(frame, flags);
    }

    inline auto
    read_frame(runtime_scheduler* sched,
               memory::frame_read_desc desc,
               uint32_t flags = 0) {
        return read_frame(sched, desc.frame, flags);
    }

    inline auto
    write_frame(runtime_scheduler* sched,
                memory::frame_write_desc desc) {
        return write_frame(sched, desc.frame, desc.flags);
    }

    template <typename sched_t, typename item_t, typename get_desc_t>
    inline auto
    write_frame_range_bounded(sched_t* sched,
                              std::span<item_t> writes,
                              uint32_t max_inflight,
                              uint32_t extra_flags,
                              get_desc_t&& get_desc) {
        assert(sched != nullptr);
        assert(max_inflight != 0);
        return pump::sender::just()
            >> pump::sender::as_stream(std::span<item_t>{writes})
            >> pump::sender::concurrent(max_inflight)
            >> pump::sender::flat_map(
                [sched,
                 extra_flags,
                 get_desc = std::forward<get_desc_t>(get_desc)](
                    item_t& item) mutable {
                    auto desc = get_desc(item);
                    assert(desc.frame != nullptr);
                    desc.flags |= extra_flags;
                    return sched->write_frame(desc.frame, desc.flags);
                })
            >> pump::sender::all();
    }

    template <typename sched_t, typename item_t, typename get_desc_t>
    inline auto
    write_frame_range_bounded_fua(sched_t* sched,
                                  std::span<item_t> writes,
                                  uint32_t max_inflight,
                                  get_desc_t&& get_desc) {
        return write_frame_range_bounded(
            sched,
            writes,
            max_inflight,
            IO_FLAGS_FUA,
            std::forward<get_desc_t>(get_desc));
    }

}  // namespace apps::inconel::nvme

#endif  // APPS_INCONEL_NVME_FRAME_IO_HH
