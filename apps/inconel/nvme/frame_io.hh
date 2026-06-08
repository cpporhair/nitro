#ifndef APPS_INCONEL_NVME_FRAME_IO_HH
#define APPS_INCONEL_NVME_FRAME_IO_HH

#include <cassert>
#include <cstddef>
#include <cstdint>

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

}  // namespace apps::inconel::nvme

#endif  // APPS_INCONEL_NVME_FRAME_IO_HH
