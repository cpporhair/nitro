#ifndef APPS_INCONEL_TREE_SENDER_HH
#define APPS_INCONEL_TREE_SENDER_HH

// ── tree/sender.hh ── module-facing facade (step 022 §9, D12) ──
//
// This header is the single entry point external modules use to talk
// to the tree domain (see `code_modules.md` §L2.tree). It explicitly
// #includes the split sub-headers so both the public PUMP senders and
// their `op_pusher` / `compute_sender_type` specializations become
// visible in any translation unit that consumes the facade:
//
//   - `lookup_scheduler.hh` — point-read `process(state)` sender,
//     submit_cache sender, the existing `lookup(...)` free function.
//   - `worker_scheduler.hh` — Phase 2 `build_leaf_candidates(...)`
//     sender returning an `unsupported_unimplemented`
//     `flush_candidate_batch`. Real candidate build arrives in
//     Phase 6.
//
// Adding a future tree-side public sender means:
//   1. Define the op/sender and its PUMP specializations inside the
//      owning sub-header.
//   2. Add a free-function wrapper in this facade header (or let
//      callers reach the underlying sender directly — see
//      `build_leaf_candidates` below).
//
// External modules should not #include the sub-headers directly.

#include "pump/core/meta.hh"
#include "pump/coro/coro.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"

#include "./lookup_scheduler.hh"
#include "./worker_scheduler.hh"
#include "../core/registry.hh"
#include "../mock_nvme/sender.hh"

namespace apps::inconel::tree {

    using namespace pump::sender;

    inline pump::coro::return_yields<bool>
    check_not_done(const lookup_state& state) {
        while (!state.all_done)
            co_yield true;
        co_return false;
    }

    // The NVMe scheduler is resolved via core::registry::local_nvme() at the
    // moment the read is issued — that runs on the tree_sched home core after
    // its callback fires, so this_core_id reflects the worker core, not the
    // submitter. Each tree lookup naturally uses its own core's nvme,
    // preserving share-nothing.

    inline auto
    on_decision_need_read(tree_lookup_sched_base* tree_sched, decision_need_read&& dec) {
        auto n = dec.read_descs.size();
        return just()
            >> with_context(__fwd__(dec))([tree_sched, n]() {
                return loop(n)
                    >> concurrent()
                    >> get_context<decision_need_read>()
                    >> flat_map([](decision_need_read& ctx, size_t i) {
                        auto* nvme = core::registry::local_nvme();
                        return nvme->read(ctx.read_descs[i].lba,
                                          ctx.read_descs[i].buf,
                                          ctx.read_descs[i].num_lbas);
                    })
                    >> all()
                    >> get_context<decision_need_read>()
                    >> flat_map([tree_sched](decision_need_read &ctx, bool) mutable {
                        return tree_sched->submit_cache(std::move(ctx.frames));
                    });
            });
    }

    template<typename key_range_t>
    inline auto
    lookup(tree_lookup_sched_base* tree_sched,
           key_range_t&& keys,
           const core::tree_manifest* manifest) {
        return with_context(
            make_lookup_state(std::forward<key_range_t>(keys), manifest))(
            [tree_sched]() {
                return get_context<lookup_state>()
                    >> flat_map([tree_sched](lookup_state& state) {
                        return just()
                            >> for_each(pump::coro::make_view_able(check_not_done(state)))
                            >> flat_map([tree_sched, &state](bool) {
                                return tree_sched->process(state);
                            })
                            >> visit()
                            >> flat_map([tree_sched]<typename D>(D&& decision) mutable {
                                if constexpr (std::is_same_v<std::decay_t<D>, decision_need_read>) {
                                    return on_decision_need_read(tree_sched, __fwd__(decision));
                                } else {
                                    static_assert(std::is_same_v<std::decay_t<D>, decision_done>);
                                    return just(true);
                                }
                            })
                            >> all()
                            >> then([&state](bool) {
                                std::vector<lookup_result> results;
                                results.reserve(state.entries.size());
                                for (auto& e : state.entries)
                                    results.push_back(std::move(e.result));
                                return results;
                            });
                    });
            }
        );
    }

    // ── build_leaf_candidates (Phase 2 worker skeleton surface) ──
    //
    // Facade wrapper over `tree_worker_sched::submit_build`. In Phase 2
    // the underlying handle always returns a
    // `flush_candidate_batch { st = unsupported_unimplemented }` via
    // the value path — this entry exists so any later sender pipeline
    // that will eventually drive real candidate build can already
    // reference a stable public name from `tree/sender.hh`.
    inline auto
    build_leaf_candidates(tree_worker_sched* worker, flush_worker_req req) {
        return worker->submit_build(req);
    }

}

#endif //APPS_INCONEL_TREE_SENDER_HH
