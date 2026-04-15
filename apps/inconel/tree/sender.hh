#ifndef APPS_INCONEL_TREE_SENDER_HH
#define APPS_INCONEL_TREE_SENDER_HH

// ── tree/sender.hh ── module-facing facade ──
//
// Single entry point external modules use to talk to the tree
// domain. #includes the split sub-headers so every public PUMP
// sender and its `op_pusher` / `compute_sender_type` specializations
// become visible in any translation unit:
//
//   - `lookup_scheduler.hh` — point-read `process(state)` sender.
//   - `worker_scheduler.hh` — Phase 7 single per-round
//     `_flush_round` handle.
//   - `owner_scheduler.hh`  — `_flush_fold` + `_flush_merge`
//     sender pair driving the round owner.
//
// Phase 7 (step 027) free helpers exposed here:
//
//   - `submit_flush_work(worker, req)` — multi-round wrapper that
//     drives the worker's flush_round handle until the proposal
//     is built. Replaces `build_candidates_for_partition` from
//     Phase 6.
//
// The composed `tree_local_flush(...)` pipeline is still NOT
// implemented here — Phase 9 will add it once the merge handle
// produces a real `tree_flush_result`.

#include "pump/core/meta.hh"
#include "pump/coro/coro.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/pop_context.hh"

#include "./lookup_scheduler.hh"
#include "./owner_scheduler.hh"
#include "./worker_scheduler.hh"
#include "../core/registry.hh"
#include "../mock_nvme/sender.hh"

namespace apps::inconel::tree {

    using namespace pump::sender;

    // ── point lookup pipeline (unchanged) ────────────────────────

    inline pump::coro::return_yields<bool>
    check_not_done(const lookup_state& state) {
        while (!state.all_done)
            co_yield true;
        co_return false;
    }

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

    // ── Phase 7: worker flush_work multi-round driver ────────────
    //
    // Encapsulates one worker arm's loop:
    //   1. worker->submit_flush_round(state) → flush_round_decision
    //   2. if need_read: NVMe read (no cache submit) → loop
    //   3. if done: extract worker_tree_proposal from state.result
    //
    // The pipeline creates worker_state on the PUMP context stack
    // via `with_context`. Each round the worker processes as much
    // as it can with available pages, then emits a bounded batch of
    // miss reads. Reads are issued through `core::registry::local_nvme`
    // and do NOT submit results into the tree_lookup cache (flush
    // pages are about to be retired, so caching them would waste
    // capacity).

    inline pump::coro::return_yields<bool>
    check_flush_round_not_done(const worker_state& state) {
        while (!state.all_done)
            co_yield true;
        co_return false;
    }

    inline auto
    on_flush_round_need_read(flush_round_need_read&& dec) {
        auto n = dec.read_descs.size();
        return just()
            >> with_context(__fwd__(dec))([n]() {
                return loop(n)
                    >> concurrent()
                    >> get_context<flush_round_need_read>()
                    >> flat_map([](flush_round_need_read& ctx, size_t i) {
                        auto* nvme = core::registry::local_nvme();
                        return nvme->read(ctx.read_descs[i].lba,
                                          ctx.read_descs[i].buf,
                                          ctx.read_descs[i].num_lbas);
                    })
                    >> all();
                // No cache submit — flush reads are about to be retired.
            });
    }

    inline auto
    submit_flush_work(tree_worker_sched_base* worker,
                      flush_worker_req        req)
    {
        auto state = make_worker_state(req);

        return with_context(std::move(state))(
            [worker]() {
                return get_context<worker_state>()
                    >> flat_map([worker](worker_state& state) {
                        return just()
                            >> for_each(pump::coro::make_view_able(
                                   check_flush_round_not_done(state)))
                            >> flat_map([worker, &state](bool) {
                                return worker->submit_flush_round(&state);
                            })
                            >> visit()
                            >> flat_map([]<typename D>(D&& d) {
                                if constexpr (std::is_same_v<std::decay_t<D>,
                                              flush_round_need_read>) {
                                    return on_flush_round_need_read(__fwd__(d));
                                } else {
                                    static_assert(
                                        std::is_same_v<std::decay_t<D>,
                                        flush_round_done>);
                                    return just(true);
                                }
                            })
                            >> all()
                            >> then([&state](bool) -> worker_tree_proposal {
                                return std::move(state.result);
                            });
                    });
            }
        );
    }

    // ── tree_local_flush — NOT YET IMPLEMENTED ────────────────────
    //
    // Phase 9 will compose:
    //
    //   tree_sched->submit_flush_fold(req)
    //     >> flat_map([](flush_fold_result&& fr) {
    //         return loop(fr.partitions.size())
    //             >> concurrent()
    //             >> flat_map([fr](size_t i) {
    //                 auto& part = fr.partitions[i];
    //                 auto* w = core::registry::tree_worker_at(part.read_domain_index);
    //                 return submit_flush_work(w, flush_worker_req{...});
    //             })
    //             >> to_vector<worker_tree_proposal>()
    //             >> flat_map([fr](auto&& proposals) {
    //                 return tree_sched->submit_flush_merge({fr.round_id,
    //                                                        std::move(proposals)});
    //             });
    //     })
    //
    // Phase 7 leaves the composed pipeline absent because the merge
    // handle still returns unsupported_unimplemented; instantiating
    // a real top-level pipeline before merge can produce a
    // tree_flush_result risks PUMP lazy-connect type errors slipping
    // by unnoticed.

}

#endif //APPS_INCONEL_TREE_SENDER_HH
