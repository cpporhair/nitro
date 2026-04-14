#ifndef APPS_INCONEL_TREE_SENDER_HH
#define APPS_INCONEL_TREE_SENDER_HH

// ── tree/sender.hh ── module-facing facade (step 022 §9, D12;
//                       extended by step 023 §10, step 025 §5) ──
//
// This header is the single entry point external modules use to talk
// to the tree domain. It #includes the split sub-headers so every
// public PUMP sender and its `op_pusher` / `compute_sender_type`
// specializations become visible in any translation unit:
//
//   - `lookup_scheduler.hh` — point-read `process(state)` sender.
//   - `worker_scheduler.hh` — Phase 5 `_leaf_mapping` sender,
//     Phase 2 `_build_leaf_candidates` stub.
//   - `owner_scheduler.hh` — Phase 5 `_flush_fold` + `_flush_merge`
//     sender pair, replacing the old monolithic `_tree_flush`.
//
// `tree_local_flush()` PUMP pipeline is NOT implemented yet: the
// three individual sender surfaces (fold / leaf_mapping / merge) are
// each tested and exercised in isolation, but the composed pipeline
// has no caller to instantiate it. Implementing an uninstantiated
// template pipeline risks hiding type errors (as demonstrated by
// PUMP's lazy connect<ctx_t>() semantics). The pipeline will be
// implemented when a real caller exists to force full instantiation.

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

    // ── build_leaf_candidates (Phase 2 worker skeleton surface) ──
    inline auto
    build_leaf_candidates(tree_worker_sched_base* worker, flush_worker_req req) {
        return worker->submit_build(req);
    }

    // ── Phase 6: candidate build multi-round pipeline ────────────
    //
    // Encapsulates the worker arm's multi-round loop:
    //   1. worker->process_candidates(state) → candidate_decision
    //   2. if need_read: NVMe read (no cache submit) → loop
    //   3. if done: extract result from state
    //
    // The pipeline creates candidate_build_state on the PUMP context
    // stack. Each round the worker processes all groups with available
    // page data, then collects a bounded batch of miss reads.

    inline pump::coro::return_yields<bool>
    check_candidates_not_done(const candidate_build_state& state) {
        while (!state.all_done)
            co_yield true;
        co_return false;
    }

    inline auto
    on_candidate_need_read(candidate_need_read&& dec) {
        auto n = dec.read_descs.size();
        return just()
            >> with_context(__fwd__(dec))([n]() {
                return loop(n)
                    >> concurrent()
                    >> get_context<candidate_need_read>()
                    >> flat_map([](candidate_need_read& ctx, size_t i) {
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
    build_candidates_for_partition(
        tree_worker_sched_base*           worker,
        std::span<const flush_leaf_group> leaf_groups,
        const core::tree_manifest*        base_manifest,
        uint64_t                          recovery_safe_lsn,
        flush_round_id                    round_id,
        uint32_t                          read_domain_index,
        uint32_t                          page_size,
        uint32_t                          page_lbas)
    {
        auto state = make_candidate_build_state(
            leaf_groups, base_manifest, recovery_safe_lsn,
            page_size, page_lbas, round_id, read_domain_index);

        return with_context(std::move(state))(
            [worker]() {
                return get_context<candidate_build_state>()
                    >> flat_map([worker](candidate_build_state& state) {
                        return just()
                            >> for_each(pump::coro::make_view_able(check_candidates_not_done(state)))
                            >> flat_map([worker, &state](bool) {
                                return worker->submit_process_candidates(&state);
                            })
                            >> visit()
                            >> flat_map([]<typename D>(D&& d) {
                                if constexpr (std::is_same_v<std::decay_t<D>,
                                              candidate_need_read>) {
                                    return on_candidate_need_read(__fwd__(d));
                                } else {
                                    static_assert(
                                        std::is_same_v<std::decay_t<D>,
                                        candidate_done>);
                                    return just(true);
                                }
                            })
                            >> all()
                            >> then([&state](bool) -> flush_worker_result {
                                return std::move(state.result);
                            });
                    });
            }
        );
    }

    // ── tree_local_flush — NOT YET IMPLEMENTED ────────────────────
    //
    // The composed pipeline (fold → fanout → merge) requires a real
    // caller to force full template instantiation. Phase 6 adds
    // build_candidates_for_partition but the top-level pipeline that
    // connects fold → worker fanout → merge is deferred until a
    // coordinator caller exists. Individual sender surfaces are
    // tested via their scheduler advance() handles.

}

#endif //APPS_INCONEL_TREE_SENDER_HH
