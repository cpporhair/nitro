#ifndef APPS_INCONEL_VALUE_SENDER_HH
#define APPS_INCONEL_VALUE_SENDER_HH

#include <span>
#include <stdexcept>
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
#include "../nvme/frame_io.hh"
#include "../nvme/runtime_scheduler.hh"
#include "../runtime/facade.hh"
#include "./scheduler.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"

namespace apps::inconel::value {

    using namespace pump::sender;
    using format::value_ref;

    // 046 §7.4 thread constraint: leader, prefill, and read-miss NVMe
    // continuations run after the value owner callback has published its
    // carrier. Do not insert cross-core `on(...)` hops in those continuations,
    // and do not resolve NvmeProvider at top-level sender construction time.
    struct local_nvme_provider {
        nvme::runtime_scheduler* operator()() const {
            return rt::local_nvme();
        }
    };

    enum class value_persist_error_reason {
        oversized_value,
        round_failed,
    };

    class value_persist_error : public std::runtime_error {
    public:
        value_persist_error(value_persist_error_reason reason,
                            std::string                message)
            : std::runtime_error(std::move(message))
            , reason_(reason) {}

        [[nodiscard]] value_persist_error_reason reason() const noexcept {
            return reason_;
        }

    private:
        value_persist_error_reason reason_;
    };

    template <typename NvmeProvider = local_nvme_provider>
    inline auto
    drain_trim_pending(NvmeProvider nvme = {}) {
        return rt::value()->prepare_trim_batch()
            >> visit()
            >> flat_map([nvme]<typename T>(T&& alt) mutable {
                using alt_t = std::decay_t<T>;
                if constexpr (std::is_same_v<alt_t, trim_batch>) {
                    uint64_t batch_id = alt.batch_id;
                    return just()
                        >> as_stream(std::span<format::trim_desc>{alt.trims})
                        >> concurrent(alt.max_trim_inflight)
                        >> flat_map([nvme](format::trim_desc d) mutable {
                            return nvme()->trim(d.lba, d.num_lbas);
                        })
                        >> all()
                        >> flat_map([batch_id](bool trim_ok) {
                            return rt::value()->complete_trim_batch(batch_id, trim_ok);
                        });
                } else {
                    return just();
                }
            });
    }

    template <typename NvmeProvider>
    inline auto
    on_persist_leader(persist_leader&& alt, NvmeProvider nvme) {
        uint64_t rid = alt.round_id;
        return ::apps::inconel::nvme::write_frame_range_bounded_fua(
                nvme(),
                alt.writes,
                alt.max_write_inflight,
                [](memory::frame_write_desc& d) { return d; })
            >> flat_map([rid](bool nvme_ok) {
                return rt::value()->finalize_persist(rid, nvme_ok)
                    >> forward_value(nvme_ok);
            });
    }

    template <typename NvmeProvider>
    inline auto
    on_persist_prefill(persist_prefill&& alt, NvmeProvider nvme) {
        uint64_t rid = alt.round_id;
        return ::apps::inconel::nvme::read_frame_range_bounded(
                nvme(),
                alt.reads,
                alt.max_read_inflight,
                [](memory::frame_read_desc& d) { return d; })
            >> flat_map([rid](bool read_ok) {
                return rt::value()->continue_persist(rid, read_ok);
            })
            >> flat_map([nvme](persist_leader&& leader) mutable {
                return on_persist_leader(__mov__(leader), nvme);
            });
    }

    // ── persist_put_values ──
    //
    // Wraps the prepare/finalize round + NVMe FUA dispatch into a single
    // top-level sender that the caller can pipe straight into a `then`
    // (`value::persist_put_values(entries) >> then(...) >> submit(ctx)`).
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

    template <typename NvmeProvider = local_nvme_provider>
    inline auto
    persist_put_values(std::span<put_entry> entries, NvmeProvider nvme = {}) {
        return rt::value()->prepare_persist(entries)
            >> visit()
            >> flat_map([nvme]<typename T>(T &&alt) mutable {
                using alt_t = std::decay_t<T>;
                if constexpr (std::is_same_v<alt_t, persist_leader>) {
                    return on_persist_leader(__fwd__(alt), nvme);
                } else if constexpr (std::is_same_v<alt_t, persist_prefill>) {
                    return on_persist_prefill(__fwd__(alt), nvme);
                } else {
                    return just(static_cast<bool>(alt.ok));
                }
            });
    }

    template <typename NvmeProvider>
    inline auto
    on_read_miss(value_ref vr, read_miss&& alt, NvmeProvider nvme) {
        return just()
            >> with_context(__fwd__(alt), vr)([nvme]() mutable {
                return get_context<read_miss>()
                    >> flat_map([nvme](read_miss &rm) mutable {
                        return nvme()->read_frame(rm.frame.get());
                    })
                    >> false_to_exception(std::runtime_error("value::read_value: NVMe read failed"))
                    >> get_context<read_miss, value_ref>()
                    >> flat_map([](read_miss &rm, const value_ref &vr, bool) mutable {
                        return rt::value()->fill_and_decode(
                            vr, std::move(rm.frame), rm.admit_to_cache);
                    });
            });
    }

    // ── read_value ──
    //
    // Wraps prepare_read → (hit/miss) → fill_and_decode into a single
    // top-level sender. Caller pipes:
    //
    //   value::read_value(vr) >> then(callback) >> submit(ctx);

    template <typename NvmeProvider = local_nvme_provider>
    inline auto
    read_value(value_ref vr, NvmeProvider nvme = {}) {
        return rt::value()->prepare_read(vr)
            >> visit()
            >> flat_map([vr, nvme](auto &&alt) mutable {
                using T = std::decay_t<decltype(alt)>;
                if constexpr (std::is_same_v<T, read_hit>) {
                    return just() >> forward_value(__mov__(alt.body));
                } else {
                    return on_read_miss(vr, __fwd__(alt), nvme);
                }
            });
    }

    inline auto
    reclaim_values(std::span<const value_ref> dead_values) {
        return rt::value()->reclaim_values(dead_values);
    }

}

#endif //APPS_INCONEL_VALUE_SENDER_HH
