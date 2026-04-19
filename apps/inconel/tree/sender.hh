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

#include <cstring>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "pump/core/meta.hh"
#include "pump/coro/coro.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/just.hh"
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
#include "../core/shard_partition.hh"
#include "../core/shard_partition_builder.hh"
#include "../core/tree_manifest.hh"
#include "../core/tree_read_domain.hh"
#include "../format/superblock.hh"
#include "../mock_nvme/sender.hh"
#include "../runtime/facade.hh"

namespace apps::inconel::tree {

    using namespace pump::sender;

    // ── point lookup pipeline (unchanged) ────────────────────────

    inline pump::coro::return_yields<bool>
    check_not_done(const lookup_state& state) {
        while (!state.all_done)
            co_yield true;
        co_return false;
    }

    // `tree_sched` here is the per-shard `tree_lookup_sched_base*`,
    // not the owner singleton — there is one such scheduler per
    // `tree_read_domain`, so the caller (`shard_lookup`) resolves
    // the right one via routing and threads it down. The facade
    // only abstracts away singletons; per-shard pointers stay
    // explicit.
    inline auto
    on_decision_need_read(tree_lookup_sched_base* tree_sched, decision_need_read&& dec) {
        auto n = dec.read_descs.size();
        return just()
            >> with_context(__fwd__(dec))([tree_sched, n]() {
                return loop(n)
                    >> concurrent()
                    >> get_context<decision_need_read>()
                    >> flat_map([](decision_need_read& ctx, size_t i) {
                        return rt::local_nvme()->read(
                            ctx.read_descs[i].lba,
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

    // ── Key-range-routed point lookup ─────────────────────────────
    //
    // Public entry: `lookup(keys, manifest)` — no caller-supplied
    // scheduler pointer. Every key is routed through the globally
    // installed `shard_partition_map` (step 030 §2.6 decision C1):
    //
    //     shard_idx = current_shard_partitions()->route(key)
    //     shard_scheduler = tree_read_domain_at(shard_idx)->lookup
    //
    // The map is the same routing carrier `memtable_fold` uses for
    // the flush path, so every key lands on the same read_domain
    // shard on both reads and flushes — the precondition for the
    // "one leaf page cached once per shard" invariant (RSM §4.7).
    //
    // Empty-tree case (`!manifest->has_root()`) is short-circuited to
    // a vector of `lookup_absent{}` of the requested size, skipping
    // the routing entirely. The bootstrap-installed placeholder map
    // would route every key to shard 0, but `has_root()==false`
    // already tells us the tree has nothing to look up.
    //
    // The shard-local descent pipeline is kept inside `_lookup_impl`
    // so nothing outside the tree module re-acquires the old
    // "hand pick a shard pointer" API (INC-003 / INC-040 closure).

    namespace _lookup_impl {

        struct routed_group {
            uint32_t                      shard_idx;
            std::vector<uint32_t>         input_indices;
            std::vector<std::string_view> keys;
        };

        struct lookup_route_plan {
            std::vector<routed_group> groups;
            size_t                    total_keys = 0;
        };

        struct all_lookup_results {
            std::vector<lookup_result> by_index;

            explicit all_lookup_results(size_t n)
                : by_index(n, lookup_absent{}) {}
            all_lookup_results(all_lookup_results&&) noexcept = default;
            all_lookup_results(const all_lookup_results&) = delete;
            all_lookup_results& operator=(const all_lookup_results&) = delete;
            all_lookup_results& operator=(all_lookup_results&&) = delete;

            std::vector<lookup_result>
            take() { return std::move(by_index); }
        };

        template<typename key_range_t>
        inline lookup_route_plan
        build_route_plan(key_range_t&& keys,
                         const core::tree_manifest* manifest) {
            if (manifest == nullptr) {
                core::panic_inconsistency(
                    "tree::lookup",
                    "tree_manifest pointer must not be null");
            }

            lookup_route_plan plan;

            // Materialize the caller's range into a stable view vector
            // before any routing decisions — `build_route_plan` runs
            // synchronously at sender-construction time, but we want to
            // treat the caller's range as potentially one-shot (e.g.
            // views, ranges-style adaptors) and keep a single well-defined
            // iteration site.
            std::vector<std::string_view> snapshot;
            for (auto&& k : keys)
                snapshot.emplace_back(std::string_view{k});
            plan.total_keys = snapshot.size();

            if (!manifest->has_root()) {
                // Empty tree — no groups produced; outer pipeline's
                // pre-initialized `all_lookup_results` stays all-absent.
                return plan;
            }

            auto partitions = core::registry::current_shard_partitions();
            if (!partitions || partitions->empty()) {
                core::panic_inconsistency(
                    "tree::lookup",
                    "has_root() is true but shard_partition_map is "
                    "not installed — builder contract violation");
            }

            // Pre-size a bucket per shard_idx value. `shard_count()`
            // is the highest `shard_idx + 1`; on the bootstrap map
            // that equals `shards.size()` (1-to-1 with read_domains).
            // A future rebuild may reuse shard_idx across partitions,
            // so K here tracks the number of distinct target
            // read_domains rather than `shards.size()`.
            const uint32_t K = partitions->shard_count();
            std::vector<routed_group> buckets(K);
            for (uint32_t s = 0; s < K; ++s)
                buckets[s].shard_idx = s;

            for (size_t i = 0; i < snapshot.size(); ++i) {
                const auto key = snapshot[i];
                // `route()` panics if the map lacks the +∞ sentinel
                // (builder-enforced invariant). No key-coverage
                // check needed — the map spans (-∞, +∞).
                const uint32_t shard_idx = partitions->route(key);
                buckets[shard_idx].input_indices.push_back(
                    static_cast<uint32_t>(i));
                buckets[shard_idx].keys.push_back(key);
            }

            plan.groups.reserve(K);
            for (auto& b : buckets) {
                if (b.input_indices.empty()) continue;
                plan.groups.push_back(std::move(b));
            }
            return plan;
        }

        // Shard-local descent: every key in `keys` must already route
        // to `tree_sched`'s leaf range (pre-grouped by build_route_plan).
        // This is the old `lookup` body verbatim — pulled down to a
        // private helper so the public API never exposes the shard
        // pointer.
        template<typename key_range_t>
        inline auto
        shard_lookup(tree_lookup_sched_base* tree_sched,
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

    }  // namespace _lookup_impl

    template<typename key_range_t>
    inline auto
    lookup(key_range_t&& keys, const core::tree_manifest* manifest) {
        using _lookup_impl::all_lookup_results;
        using _lookup_impl::lookup_route_plan;

        auto plan = _lookup_impl::build_route_plan(
            std::forward<key_range_t>(keys), manifest);
        const size_t total_keys = plan.total_keys;

        return with_context(std::move(plan), all_lookup_results(total_keys))([manifest]() {
                return get_context<lookup_route_plan, all_lookup_results>()
                    >> flat_map([manifest](const lookup_route_plan& p, all_lookup_results& a) {
                        return just()
                            >> loop(p.groups.size())
                            >> concurrent()
                            >> flat_map([&p, &a, manifest](size_t i) {
                                const auto& g = p.groups[i];
                                auto* rd =  core::registry::tree_read_domain_at(g.shard_idx);
                                auto* sched = rd->lookup_sched;
                                auto indices = g.input_indices;
                                return just()
                                    >> _lookup_impl::shard_lookup(sched, g.keys, manifest)
                                    >> then([&a, idx = std::move(indices)](std::vector<lookup_result>&& r) mutable {
                                        for (size_t k = 0; k < idx.size(); ++k)
                                            a.by_index[idx[k]] =  r[k];
                                    });
                            })
                            >> all();
                    })
                    >> get_context<all_lookup_results>()
                    >> then([](all_lookup_results& alr, bool) {
                        return alr.take();
                    });
            }
        );
    }

    namespace flush_pipeline {
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
        on_flush_round_need_read(flush_round_need_read &&dec) {
            auto n = dec.read_descs.size();
            return just()
                >> with_context(__fwd__(dec))([n]() {
                    return loop(n)
                        >> concurrent()
                        >> get_context<flush_round_need_read>()
                        >> flat_map([](flush_round_need_read &ctx, size_t i) {
                            return rt::local_nvme()->read(
                                ctx.read_descs[i].lba,
                                ctx.read_descs[i].buf,
                                ctx.read_descs[i].num_lbas);
                        })
                        >> all();
                    // No cache submit — flush reads are about to be retired.
                });
        }

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

        // Carried on the PUMP context stack across the root-change
        // superblock update's read → mutate → FUA-write → finish
        // steps. `tree_sched` no longer owns the NVMe side of this
        // flow; it only latches `inflight` via begin/finish.
        struct superblock_update_state {
            flush_round_id     round_id;
            paddr              new_root_base_paddr{};
            uint64_t           active_lba    = 0;
            uint64_t           inactive_lba  = 0;
            superblock_slot    inactive_slot = superblock_slot::A;
            uint32_t           lba_size      = 0;
            std::vector<char>  read_buf;
            std::vector<char>  write_buf;
        };

        inline auto
        perform_superblock_io(begin_update_superblock_result r) {
            superblock_update_state st{
                .round_id            = r.round_id,
                .new_root_base_paddr = r.new_root_base_paddr,
                .active_lba          = r.active_lba,
                .inactive_lba        = r.inactive_lba,
                .inactive_slot       = r.inactive_slot,
                .lba_size            = r.lba_size,
                .read_buf            = std::vector<char>(r.lba_size, '\0'),
                .write_buf           = std::vector<char>(r.lba_size, '\0'),
            };
            // `with_context(v)(body)` is a bind_back — the `flat_map`
            // inside `finalize_root_change` that consumes us needs a
            // complete pipeline, so we seed with `just()`.
            return just()
                >> with_context(std::move(st))([]() {
                return get_context<superblock_update_state>()
                    >> flat_map([](superblock_update_state& s) {
                        return rt::local_nvme()->read(
                            s.active_lba, s.read_buf.data(), 1);
                    })
                    >> get_context<superblock_update_state>()
                    >> then([](superblock_update_state& s, bool read_ok) -> bool {
                        if (!read_ok) return false;
                        format::superblock cur{};
                        std::memcpy(&cur, s.read_buf.data(), sizeof(cur));
                        auto status = format::inspect_superblock(cur);
                        if (status != format::superblock_status::ok) {
                            core::panic_inconsistency(
                                "tree_local_flush::perform_superblock_io",
                                "active superblock invalid: %s",
                                format::superblock_status_to_string(status));
                        }
                        cur.root_base_paddr = s.new_root_base_paddr;
                        cur.generation += 1;
                        cur.crc = 0;
                        cur.crc = format::superblock_compute_crc(cur);
                        std::fill(s.write_buf.begin(), s.write_buf.end(), '\0');
                        std::memcpy(s.write_buf.data(), &cur, sizeof(cur));
                        return true;
                    })
                    >> visit()
                    >> get_context<superblock_update_state>()
                    >> flat_map([]<typename Flag>(
                            superblock_update_state& s, Flag&&) {
                        if constexpr (std::is_same_v<
                                std::decay_t<Flag>, std::true_type>) {
                            return rt::local_nvme()->write(
                                s.inactive_lba,
                                s.write_buf.data(),
                                1,
                                mock_nvme::IO_FLAGS_FUA);
                        } else {
                            return just(false);
                        }
                    })
                    >> get_context<superblock_update_state>()
                    >> flat_map([](superblock_update_state& s,
                                   bool write_ok) {
                        return rt::owner()->submit_finish_update_superblock(
                            finish_update_superblock_request{
                                .round_id      = s.round_id,
                                .inactive_slot = s.inactive_slot,
                                .write_ok      = write_ok,
                            });
                    });
            });
        }

        inline auto
        finalize_root_change(update_superblock_request req) {
            return rt::owner()->submit_begin_update_superblock(std::move(req))
                >> flat_map([](begin_update_superblock_result&& r) {
                    return perform_superblock_io(std::move(r));
                })
                >> then(make_finalize_after_superblock)
                >> flat_map([](finalize_flush_request&& req) {
                    return rt::owner()->submit_finalize_flush_round(
                        std::move(req));
                });
        }

        // Dispatches the unified io batch staged in a single
        // `merge_step_need_io` decision. Reads and writes live in
        // the same `std::vector<merge_io_desc>` and fire through
        // one `as_stream >> concurrent(N) >> visit() >> flat_map`
        // chain — the NVMe scheduler sees all N ops as concurrent
        // in-flight requests, no kind-based serialization.
        //
        // Pointer lifetime:
        //   - read_desc.buf → `merge_round_state::fetched_old_pages[rb].data()`
        //     (tree_sched-owned; lives until finalize_merge)
        //   - write_desc.data → `mem_tree_node::content.data()` inside
        //     `merge_round_state::combined_root` (same lifetime)
        // NVMe read DMAs directly into the fetched buffers; NVMe
        // write DMAs from the tree page buffer. After the iter's
        // all() barrier completes, the next merge_step resume picks
        // up the fresh bytes in place.
        //
        // If `io.has_reads` is set (the seam handler precomputed it
        // so we don't re-scan), the iter ACKs back via
        // `submit_merge_reads_done` so the scheduler clears
        // `waiting_for_reads` and concurrent iters can resume the
        // coroutine.
        inline auto
        handle_merge_step_need_io(merge_step_need_io&& io) {
            const bool has_reads = io.has_reads;
            return just()
                >> as_stream(std::move(io.ios))
                >> concurrent(tree_sched::kWriteBatchConcurrency)
                >> visit()
                >> flat_map([]<typename D>(D&& d) {
                    using T = std::decay_t<D>;
                    auto* nvme = rt::local_nvme();
                    if constexpr (std::is_same_v<T, format::read_desc>) {
                        return nvme->read(d.lba, d.buf, d.num_lbas);
                    } else {
                        static_assert(std::is_same_v<T, format::write_desc>);
                        return nvme->write(
                            d.lba, d.data, d.num_lbas, d.flags);
                    }
                })
                >> all()
                >> then([has_reads](bool) { return has_reads; })
                >> visit()
                >> flat_map([]<typename Flag>(Flag&&) {
                    if constexpr (std::is_same_v<
                            std::decay_t<Flag>, std::true_type>) {
                        return rt::owner()->submit_merge_reads_done();
                    } else {
                        return just();
                    }
                });
        }

        // Yields a ping each time the scheduler should resume the
        // merge coroutine. The payload (round_id + worker_proposals)
        // lives on the `merge_loop_state` in the PUMP context; the
        // scheduler reads it directly through the `ls` pointer the
        // iter handler passes into `submit_merge_step`. The loop
        // terminates when the outer iter handler flips `cpu_done`
        // in response to a `merge_step_done` decision.
        inline pump::coro::return_yields<bool>
        drive_merge(const merge_loop_state& loop_state) {
            while (!loop_state.cpu_done.load(std::memory_order_acquire)) {
                co_yield true;
            }
            // `return_yields<T>` is built on a `yields_promise` whose
            // return_value takes T — trailing co_return needs a
            // sentinel value, never consumed.
            co_return false;
        }

        // Dispatches the commit variant returned by `submit_finalize_merge`.
        //   - done        : propagate the already-built tree_flush_result
        //   - root_stable : call submit_finalize_flush_round directly
        //   - root_change : run the superblock update chain
        //                   (begin → NVMe read/mutate/FUA-write → finish
        //                    → finalize_flush_round)
        inline auto
        continue_after_finalize_merge() {
            return flat_map([]<typename commit_result_t>(
                    commit_result_t&& commit_result) {
                using T = std::remove_cvref_t<commit_result_t>;
                if constexpr (std::is_same_v<T, flush_merge_done>) {
                    return just(std::move(commit_result.result));
                } else if constexpr (std::is_same_v<T, flush_merge_root_stable>) {
                    return rt::owner()->submit_finalize_flush_round(
                        std::move(commit_result.finalize_req));
                } else {
                    static_assert(std::is_same_v<T, flush_merge_root_change>);
                    return finalize_root_change(
                        std::move(commit_result.update_req));
                }
            });
        }

        // The full merge loop: pump the driver coroutine through
        // submit_merge_step with `concurrent(kMergeIterConcurrency)`
        // outer parallelism, handle each yielded decision (dispatch
        // IO, ACK reads, mark cpu_done when the scheduler reports it),
        // and await all iters. When the loop returns, every emitted
        // write for the round is durably queued to NVMe (all() awaited
        // inside each iter) — the caller must still issue a device
        // FLUSH before `submit_finalize_merge`.
        static constexpr uint32_t kMergeIterConcurrency = 8;

        inline auto
        drive_merge_loop(flush_round_id round_id,
                         std::vector<worker_tree_proposal>&& proposals)
        {
            return just()
                >> with_context(merge_loop_state{round_id, __mov__(proposals)})([]() {
                    return get_context<merge_loop_state>()
                        >> then([](merge_loop_state &ls) { return pump::coro::make_view_able(drive_merge(ls)); })
                        >> for_each()
                        >> concurrent(kMergeIterConcurrency)
                        >> get_context<merge_loop_state>()
                        >> flat_map([](merge_loop_state &ls, bool) {
                            return rt::owner()->submit_merge_step(&ls);
                        })
                        >> visit()
                        >> get_context<merge_loop_state>()
                        >> flat_map([]<typename D>(merge_loop_state& ls, D &&dec) {
                            using T = std::decay_t<D>;
                            if constexpr (std::is_same_v<T, merge_step_need_io>) {
                                return handle_merge_step_need_io(__fwd__(dec));
                            } else {
                                static_assert(std::is_same_v<T, merge_step_done>);
                                ls.cpu_done.store(true, std::memory_order_release);
                                return just();
                            }
                        })
                        >> all();
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
            return with_context(__mov__(fr), all_worker_tree_proposal(cnt))([cnt]() {
                return loop(cnt)
                    >> concurrent()
                    >> get_context<flush_fold_result>()
                    >> flat_map([](const flush_fold_result &ctx, size_t i) {
                        return just()
                            >> submit_flush_work(
                                core::registry::tree_read_domain_at(
                                    ctx.partitions[i].read_domain_index)->worker_sched,
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

    // Rebuild the globally installed `shard_partition_map` from the
    // new manifest's `leaf_order` and re-publish it into every
    // read_domain. Runs on whichever scheduler fired the callback
    // the preceding `then(...)` attached to — in the tree_local_flush
    // pipeline that is `tree_sched` itself (the `finalize_flush_round`
    // seam callback resumes on tree_sched's core), so the publish
    // happens single-threaded from the serialized flush commit point.
    //
    // Design intent (RSM §4.7): `tree_sched` is responsible for
    // producing a routing map that matches the committed leaf
    // layout. Bootstrap starts with a single-shard placeholder
    // installed by the builder; after the first successful flush
    // we swap it for a real partition map so subsequent reads and
    // flushes route through `shard_partition_map::route(key)`
    // against fences that actually exist in the tree.
    //
    // Skipped when `new_manifest == nullptr` (empty-round /
    // unsupported / flush-ok=false short-circuits): there is no
    // new layout to publish, so the currently installed map
    // remains valid.
    inline void
    rebuild_and_publish_shard_partitions(const tree_flush_result& r) {
        if (r.new_manifest == nullptr) return;
        const uint32_t K = core::registry::tree_read_domain_count();
        if (K == 0) {
            core::panic_inconsistency(
                "tree::rebuild_and_publish_shard_partitions",
                "no tree_read_domains registered — cannot rebuild "
                "shard_partition_map");
        }
        auto new_map = std::make_shared<const core::shard_partition_map>(
            core::build_initial_shard_partition_map(
                r.new_manifest->leaf_order, K));
        rt::publish_shard_partitions(std::move(new_map));
    }

    // Full owner-side flush transaction. Singleton-only — the owner
    // (`tree_sched`) is resolved internally via `rt::owner()`, so
    // callers only pass the request payload. See `runtime/facade.hh`.
    inline auto
    tree_local_flush(tree_flush_request req) {
        return rt::owner()->submit_flush_fold(std::move(req))
            >> flat_map([](flush_fold_result&& fr) {
                auto round_id = fr.round_id;
                return just()
                    >> flush_pipeline::collect_worker_proposals(std::move(fr))
                    >> flat_map([round_id](std::vector<worker_tree_proposal> &&proposals) {
                        return flush_pipeline::drive_merge_loop(
                            round_id, __mov__(proposals));
                    })
                    >> flat_map([](bool) {
                        // `drive_merge_loop` ends in `>> all()` which
                        // yields a single bool; absorb it here.
                        return rt::local_nvme()->flush();
                    })
                    >> flat_map([round_id](bool flush_ok) {
                        return rt::owner()->submit_finalize_merge(
                            merge_finalize_request{
                                .round_id = round_id,
                                .flush_ok = flush_ok,
                            });
                    })
                    >> visit()
                    >> flush_pipeline::continue_after_finalize_merge()
                    >> then([](auto&& r) -> tree_flush_result {
                        // Keep the routing map in sync with the
                        // committed leaf layout. Safe here because
                        // the preceding finalize seam resumes on
                        // tree_sched's core — the publish runs
                        // single-threaded from the flush commit
                        // point. See `rebuild_and_publish_shard_partitions`.
                        //
                        // `continue_after_finalize_merge` reaches
                        // here from three different variant arms
                        // (done / root_stable / root_change) whose
                        // `push_value` call sites differ in value
                        // category (`just(std::move(...))` vs
                        // scheduler callbacks), so we take a
                        // universal reference and forward on return.
                        rebuild_and_publish_shard_partitions(r);
                        return std::forward<decltype(r)>(r);
                    });
            });
    }

}

#endif //APPS_INCONEL_TREE_SENDER_HH
