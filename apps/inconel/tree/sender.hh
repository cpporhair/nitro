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
// `tree_local_flush(...)` now composes the full owner-side flush
// transaction: fold, worker fanout, owner merge/write, optional
// superblock update, and final round publication.

#include <thread>

#include "pump/core/meta.hh"
#include "pump/coro/coro.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/pop_context.hh"

#include "./lookup_scheduler.hh"
#include "./owner_scheduler.hh"
#include "./worker_scheduler.hh"
#include "../core/panic.hh"
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

    namespace flush_pipeline {

        inline void
        panic_update_superblock_failure(const update_superblock_result& r) {
            if (r.ok) return;
            core::panic_inconsistency(
                "tree_local_flush",
                "update_superblock failed for round_id=%lu",
                static_cast<unsigned long>(r.round_id.v));
        }

        inline finalize_flush_request
        make_finalize_after_superblock(update_superblock_result&& r) {
            panic_update_superblock_failure(r);
            return finalize_flush_request{
                .round_id = r.round_id,
                .ok = true,
                .committed_slot =
                    std::optional<superblock_slot>{r.committed_slot},
            };
        }

        inline auto
        finalize_root_change(tree_sched* owner, update_superblock_request req) {
            return owner->submit_update_superblock(std::move(req))
                >> then(make_finalize_after_superblock)
                >> flat_map([owner](finalize_flush_request&& req) {
                    return owner->submit_finalize_flush_round(std::move(req));
                });
        }

        inline auto
        continue_after_merge(tree_sched* owner) {
            return flat_map([owner]<typename merge_result_t>(merge_result_t &&merge_result) {
                using T = std::remove_cvref_t<merge_result_t>;
                if constexpr (std::is_same_v<T, flush_merge_done>) {
                    return just(std::move(merge_result.result));
                } else if constexpr (std::is_same_v<T, flush_merge_root_stable>) {
                    return owner->submit_finalize_flush_round(
                        std::move(merge_result.finalize_req));
                } else {
                    static_assert(std::is_same_v<T, flush_merge_root_change>);
                    return finalize_root_change(
                        owner, std::move(merge_result.update_req));
                }
            });
        }

        inline auto
        merge_worker_proposals(tree_sched* owner, flush_round_id round_id) {
            return flat_map([owner, round_id](std::vector<worker_tree_proposal> &&proposals) {
                return owner->submit_flush_merge(
                    flush_merge_request{
                        .round_id = round_id,
                        .worker_proposals = std::move(proposals),
                    }
                );
            });
        }

        inline auto
        make_flush_worker_req(const flush_fold_result &ctx, size_t i) {
            return flush_worker_req{
                .round_id = ctx.round_id,
                .read_domain_index = ctx.partitions[i].read_domain_index,
                .base_manifest = ctx.base_manifest,
                .recovery_safe_lsn = ctx.recovery_safe_lsn,
                .key_groups = ctx.partitions[i].groups,
            };
        }

        inline auto
        submit_flush_work(tree_worker_sched_base *worker, flush_worker_req req) {
            return with_context(make_worker_state(req))([worker]() {
                return get_context<worker_state>()
                    >> then([](const worker_state &state) {
                        return pump::coro::make_view_able(check_flush_round_not_done(state));
                    })
                    >> for_each()
                    >> get_context<worker_state>()
                    >> flat_map([worker](worker_state &state, bool) {
                        return worker->submit_flush_round(&state);
                    })
                    >> visit()
                    >> flat_map([]<typename D>(D &&d) {
                        if constexpr (std::is_same_v<std::decay_t<D>, flush_round_need_read>) {
                            return on_flush_round_need_read(__fwd__(d));
                        } else {
                            static_assert(std::is_same_v<std::decay_t<D>, flush_round_done>);
                            return just(true);
                        }
                    })
                    >> all()
                    >> get_context<worker_state>()
                    >> then([](worker_state &state, bool) -> worker_tree_proposal {
                        return std::move(state.result);
                    });
            });
        }

        struct
        all_worker_tree_proposal {
            std::vector<std::optional<worker_tree_proposal>> proposals_by_core;
            all_worker_tree_proposal(size_t max) : proposals_by_core(max) {}
            all_worker_tree_proposal(all_worker_tree_proposal&) = delete;
            all_worker_tree_proposal(all_worker_tree_proposal&& o)  noexcept : proposals_by_core(std::move(o.proposals_by_core)) {}

            auto
            get_result() {
                std::vector<worker_tree_proposal> result;
                result.reserve(proposals_by_core.size());
                for (auto& proposal : proposals_by_core) {
                    if (proposal)
                        result.push_back(std::move(*proposal));
                }
                return result;
            }

            auto
            push_result(worker_tree_proposal&& p, size_t i) {
                proposals_by_core[i].emplace(std::move(p));
            }
        };

        inline auto
        collect_worker_proposals(flush_fold_result&& fr) {
            auto cnt = fr.partitions.size();
            return with_context(__fwd__(fr), all_worker_tree_proposal(cnt))([cnt]() {
                return loop(cnt)
                    >> concurrent()
                    >> get_context<flush_fold_result>()
                    >> flat_map([](const flush_fold_result &ctx, size_t i) {
                        return just()
                            >> submit_flush_work(
                                core::registry::tree_worker_at(ctx.partitions[i].read_domain_index),
                                make_flush_worker_req(ctx, i)
                            )
                            >> get_context<all_worker_tree_proposal>()
                            >> then([i](all_worker_tree_proposal &ps, worker_tree_proposal &&p) {
                                ps.push_result(__fwd__(p), i);
                            });
                    })
                    >> all()
                    >> get_context<all_worker_tree_proposal>()
                    >> then([](all_worker_tree_proposal &proposals, const bool ok) {
                        if (ok) [[likely]]
                            return proposals.get_result();
                        throw std::runtime_error("tree_local_flush: worker round failed");
                    });
            });
        }

    }  // namespace _flush_pipeline

    inline auto
    tree_local_flush(tree_sched* owner, tree_flush_request req) {
        return owner->submit_flush_fold(std::move(req))
            >> flat_map([owner](flush_fold_result&& fr) {
                auto round_id = fr.round_id;
                return just()
                    >> flush_pipeline::collect_worker_proposals(std::move(fr))
                    >> flush_pipeline::merge_worker_proposals(owner, round_id)
                    >> visit()
                    >> flush_pipeline::continue_after_merge(owner);
            });
    }

}

#endif //APPS_INCONEL_TREE_SENDER_HH
