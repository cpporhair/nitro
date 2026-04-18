#ifndef APPS_INCONEL_VALUE_SENDER_HH
#define APPS_INCONEL_VALUE_SENDER_HH

#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/just.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"

#include "../core/registry.hh"
#include "../format/types.hh"
#include "../mock_nvme/scheduler.hh"
#include "../runtime/facade.hh"
#include "./scheduler.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"

namespace apps::inconel::value {

    using namespace pump::sender;
    using format::value_ref;

    inline auto
    on_persist_leader(persist_leader&& alt) {
        uint64_t rid = alt.round_id;
        return just()
            >> as_stream(__mov__(alt.writes))
            >> concurrent()
            >> flat_map([](format::write_desc d) {
                return rt::local_nvme()->write(
                    d.lba, d.data, d.num_lbas, mock_nvme::IO_FLAGS_FUA);
            })
            >> all()
            >> flat_map([rid](bool nvme_ok) {
                return rt::value()->finalize_persist(rid, nvme_ok)
                    >> forward_value(nvme_ok);
            });
    }

    // ── persist_values ──
    //
    // Wraps the prepare/finalize round + NVMe FUA dispatch into a single
    // top-level sender that the caller can pipe straight into a `then`
    // (`value::persist_values(entries) >> then(...) >> submit(ctx)`).
    //
    // No scheduler pointer on the API — the value scheduler is a
    // singleton, resolved internally via `rt::value()`. Every Inconel
    // runtime constructed by `runtime::build_runtime` installs it, so
    // application code never has to thread the pointer through.
    //
    // Internally:
    //
    //   prepare_persist                          → variant<leader, follower>
    //     >> visit()                              expand alternatives
    //     >> flat_map(generic lambda):
    //         if leader   → for_each(writes) >> on(local_nvme())
    //                       >> nvme.write_fua >> all()
    //                       >> finalize_persist(round_id, ok)
    //                       >> then([]{return ok;})
    //         if follower → just(true)
    //
    // Both branches must produce the same final value_type (bool here);
    // their sender types can differ — flat() accepts arbitrary senders.

    inline auto
    persist_values(std::span<put_entry> entries) {
        return rt::value()->prepare_persist(entries)
            >> visit()
            >> flat_map([]<typename T>(T &&alt) {
                if constexpr (std::is_same_v<T, persist_leader>) {
                    return on_persist_leader(__fwd__(alt));
                } else {
                    return just(static_cast<bool>(alt.ok));
                }
            });
    }

    inline auto
    on_read_miss(value_ref vr, read_miss&& alt) {
        return just()
            >> with_context(__fwd__(alt), vr)([]() {
                return get_context<read_miss>()
                    >> flat_map([](const read_miss &rm) {
                        return rt::local_nvme()->read(
                            rm.base.lba, rm.buf.get(), rm.span_lbas);
                    })
                    >> false_to_exception(std::runtime_error("value::read_value: NVMe read failed"))
                    >> get_context<read_miss, value_ref>()
                    >> flat_map([](read_miss &rm, const value_ref &vr, bool) mutable {
                        return rt::value()->fill_and_decode(
                            vr, std::move(rm.buf),
                            rm.buf_size, rm.admit_to_cache);
                    });
            });
    }

    // ── read_value ──
    //
    // Wraps prepare_read → (hit/miss) → fill_and_decode into a single
    // top-level sender. Caller pipes:
    //
    //   value::read_value(vr) >> then(callback) >> submit(ctx);

    inline auto
    read_value(value_ref vr) {
        return rt::value()->prepare_read(vr)
            >> visit()
            >> flat_map([vr](auto &&alt) {
                using T = std::decay_t<decltype(alt)>;
                if constexpr (std::is_same_v<T, read_hit>) {
                    return just() >> forward_value(__mov__(alt.body));
                } else {
                    return on_read_miss(vr, __fwd__(alt));
                }
            });
    }

}

#endif //APPS_INCONEL_VALUE_SENDER_HH
