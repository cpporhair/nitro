#ifndef APPS_INCONEL_MOCK_NVME_SENDER_HH
#define APPS_INCONEL_MOCK_NVME_SENDER_HH

#include "./scheduler.hh"

#include "pump/sender/generate.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/reduce.hh"

namespace apps::inconel::mock_nvme {

    // ── I/O descriptor for batch operations ──

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

    // ── Single-item senders (scheduler → bool) ──

    inline auto
    write_one(scheduler* s, uint64_t lba, const void* data, uint32_t num_lbas, uint32_t flags = 0) {
        return s->write(lba, data, num_lbas, flags);
    }

    inline auto
    read_one(scheduler* s, uint64_t lba, void* buf, uint32_t num_lbas) {
        return s->read(lba, buf, num_lbas);
    }

    inline auto
    trim_one(scheduler* s, uint64_t lba, uint32_t num_lbas) {
        return s->trim(lba, num_lbas);
    }

    // ── Batch senders (range of descriptors → concurrent → all) ──

    template<typename range_t>
    inline auto
    write_batch(range_t&& descs, scheduler* s) {
        return pump::sender::as_stream(__fwd__(descs))
            >> pump::sender::concurrent()
            >> pump::sender::flat_map([s](auto&& d) {
                return s->write(d.lba, d.data, d.num_lbas, d.flags);
            })
            >> pump::sender::all();
    }

    template<typename range_t>
    inline auto
    read_batch(range_t&& descs, scheduler* s) {
        return pump::sender::as_stream(__fwd__(descs))
            >> pump::sender::concurrent()
            >> pump::sender::flat_map([s](auto&& d) {
                return s->read(d.lba, d.buf, d.num_lbas);
            })
            >> pump::sender::all();
    }

    template<typename range_t>
    inline auto
    trim_batch(range_t&& descs, scheduler* s) {
        return pump::sender::as_stream(__fwd__(descs))
            >> pump::sender::concurrent()
            >> pump::sender::flat_map([s](auto&& d) {
                return s->trim(d.lba, d.num_lbas);
            })
            >> pump::sender::all();
    }

}

#endif //APPS_INCONEL_MOCK_NVME_SENDER_HH
