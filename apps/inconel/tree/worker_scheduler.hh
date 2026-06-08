#ifndef APPS_INCONEL_TREE_WORKER_SCHEDULER_HH
#define APPS_INCONEL_TREE_WORKER_SCHEDULER_HH

// ── tree_worker_sched — leaf-only flush worker ──────────────────
//
// Split into non-templated base + templated derived, mirroring
// tree_lookup_sched_base / tree_lookup_sched<Cache>:
//
//   tree_worker_sched_base  — single per-round queue + schedule /
//                             submit methods. Registry stores base
//                             pointers.
//   tree_worker_sched<Cache> — holds a typed back-reference to the
//                              owning `tree_read_domain<Cache>` and
//                              drives `process_flush_round<Cache>`
//                              against that read_domain's
//                              `node_cache`.
//
// INC-046 keeps the single `_flush_round` handle but narrows its job:
// one round of leaf mapping / old-leaf read / merge / format. The
// wrapper `submit_flush_work` loops the per-round handle with
// NVMe-read dispatch until the worker leaf chain is complete.
//
// Step 030 cache-ownership move: the shared `Cache` instance lives
// on `core::tree_read_domain<Cache>` (RSM §4.7). The worker reaches
// it via `read_domain_->node_cache` — template-specialized on the
// same `Cache`, so `process_flush_round` still receives a raw
// `Cache*` and inlines cache access with no virtual dispatch
// (030 §6.1 decision A). The previous `tree_lookup_sched_base*
// paired_lookup` field is dropped (030 §2.4): it was only a
// placeholder for a future read-throttle hook, never consulted; a
// future caller that needs the paired lookup reaches it through
// `read_domain_->lookup.get()`.
//
// Step 030 `read_domain_index` move (§6.6 decision I1): the field
// lives on `tree_read_domain_base` now. The base's
// `read_domain_index()` virtual getter returns the value from the
// owning read_domain, used only on the diagnostic path in
// `advance()`.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

#include "../core/page_cache.hh"
#include "../core/panic.hh"
#include "../core/tree_geometry.hh"
#include "../memory/dma_page_pool.hh"
#include "./candidate_build.hh"
#include "./flush_types.hh"
#include "./lookup_scheduler.hh"

// Forward-declare the owning read_domain so the worker can hold a
// typed back-reference. The full `core::tree_read_domain<Cache>`
// definition is pulled in by any TU that constructs a
// `tree_worker_sched<Cache>` (step 030 §7 include contract).
namespace apps::inconel::core {
    struct tree_read_domain_base;
    template <cache_concept Cache> struct tree_read_domain;
}

namespace apps::inconel::tree {

    // ── PUMP req / op / sender for flush_round (step 027) ──
    //
    // One handle per worker arm per round. Takes a pointer to the
    // worker_state on the PUMP context stack and returns a
    // flush_round_decision (done | need_read).

    struct tree_worker_sched_base;

    namespace _flush_round {

        struct req {
            worker_state*                                       state;
            std::move_only_function<void(flush_round_decision&&)> cb;
        };

        struct op {
            constexpr static bool flush_round_op = true;

            tree_worker_sched_base* sched;
            worker_state*           state;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_worker_sched_base* sched;
            worker_state*           state;

            auto
            make_op() {
                return op{ .sched = sched, .state = state };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _flush_round

    // ── tree_worker_sched_base (non-templated) ───────────────────

    struct tree_worker_sched_base {
        static constexpr uint32_t kMaxFlushRoundOpsPerAdvance = 16;

        pump::core::per_core::queue<_flush_round::req*> flush_round_q;

        explicit
        tree_worker_sched_base(std::size_t depth = 2048)
            : flush_round_q(depth) {}

        virtual ~tree_worker_sched_base() = default;

        // Mirror of `tree_lookup_sched_base::read_domain_index()`
        // (030 §6.6 decision I1): the field lives on
        // `tree_read_domain_base`, reached through the derived
        // scheduler's back-reference. Used only on the diagnostic
        // panic path in `advance()`.
        virtual uint32_t read_domain_index() const noexcept = 0;

        void
        schedule_flush_round(_flush_round::req* r) {
            flush_round_q.try_enqueue(r);
        }

        // Per-round handle: submit the worker_state for one round of
        // leaf-only read/merge/build. The wrapper sender
        // `submit_flush_work()` drives the loop.
        auto
        submit_flush_round(worker_state* state) {
            return _flush_round::sender{ this, state };
        }
    };

    // ── tree_worker_sched<Cache> (templated) ─────────────────────
    //
    // Holds a typed back-reference to the owning
    // `core::tree_read_domain<Cache>`; the shared `node_cache` lives
    // there. `advance()` invokes `process_flush_round<Cache>` with
    // `&read_domain_->node_cache` — same compile-time cache type as
    // the paired lookup scheduler, so the cache access inlines at
    // every call site (030 §6.1 decision A / §2.4).

    template <core::cache_concept Cache>
    struct tree_worker_sched : tree_worker_sched_base {
        core::tree_read_domain<Cache>* read_domain_ = nullptr;
        memory::lba_dma_page_pool frame_pool_;

        // Constructed by `core::tree_read_domain<Cache>::ctor` with
        // `this` pointing back at the enclosing read_domain (030 §2.8
        // step 5a). The read_domain outlives the scheduler — both
        // sit in the same `unique_ptr` tree.
        explicit
        tree_worker_sched(core::tree_read_domain<Cache>* rd,
                          const core::tree_geometry*      geom,
                          std::size_t                    depth = 2048,
                          memory::dma_page_allocator     frame_allocator =
                              memory::make_heap_dma_page_allocator(),
                          uint32_t                       frame_alignment = 4096,
                          int                            frame_numa_id = -1)
            : tree_worker_sched_base(depth)
            , read_domain_(rd)
            , frame_pool_(geom->lba_size, frame_alignment, frame_numa_id,
                          frame_allocator) {}

        uint32_t
        read_domain_index() const noexcept override {
            return read_domain_->read_domain_index;
        }

        bool
        advance() {
            bool progress = false;

            for (uint32_t i = 0; i < kMaxFlushRoundOpsPerAdvance; ++i) {
                auto item = flush_round_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                if (r->state == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_worker_sched::advance",
                        "_flush_round::req with null worker_state "
                        "(rdi=%u)",
                        static_cast<unsigned>(read_domain_->read_domain_index));
                }
                if (r->state->read_domain_index != read_domain_->read_domain_index) {
                    core::panic_inconsistency(
                        "tree::tree_worker_sched::advance",
                        "worker_state routed to wrong worker: "
                        "state.rdi=%u self.rdi=%u",
                        static_cast<unsigned>(r->state->read_domain_index),
                        static_cast<unsigned>(read_domain_->read_domain_index));
                }

                auto decision = process_flush_round(
                    *r->state, &read_domain_->node_cache, &frame_pool_);
                r->cb(std::move(decision));
                delete r;
                progress = true;
            }

            return progress;
        }

        template <typename runtime_t>
        bool
        advance(runtime_t&) {
            return advance();
        }
    };

    // ── Deferred op::start definition ──

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _flush_round::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_flush_round(new _flush_round::req{
            state,
            [ctx = ctx, scope = scope](flush_round_decision&& d) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(d));
            },
        });
    }

}  // namespace apps::inconel::tree

// ── PUMP specializations ──

namespace pump::core {

    // flush_round
    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::flush_round_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename ctx_t>
        static void
        push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template <typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::tree::_flush_round::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::flush_round_decision>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_TREE_WORKER_SCHEDULER_HH
