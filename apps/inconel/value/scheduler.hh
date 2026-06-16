#ifndef APPS_INCONEL_VALUE_SCHEDULER_HH
#define APPS_INCONEL_VALUE_SCHEDULER_HH

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/inlined_vector.h>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

#include "../core/data_area_heads.hh"
#include "../core/page_cache.hh"
#include "../core/panic.hh"
#include "../format/types.hh"
#include "../format/value_object.hh"
#include "../memory/dma_page_pool.hh"
#include "../memory/frame.hh"
#include "./space_manager.hh"

namespace apps::inconel::value {

    using format::paddr;
    using format::trim_desc;
    using format::value_ref;

    // ── Public input/output types ─────────────────────────────────────────

    struct reclaim_stats {
        std::atomic<uint64_t> reclaim_total_refs{0};
        std::atomic<uint64_t> partial_into_dirty{0};
        std::atomic<uint64_t> partial_into_open{0};
        std::atomic<uint64_t> partial_into_allocatable{0};
        std::atomic<uint64_t> partial_into_cache{0};
        std::atomic<uint64_t> partial_into_hole{0};
        std::atomic<uint64_t> partial_into_untracked{0};
        std::atomic<uint64_t> whole_into_dirty{0};
        std::atomic<uint64_t> whole_clears_existing{0};
        std::atomic<uint64_t> whole_already_pending{0};
        std::atomic<uint64_t> dropped_freed_mask_zero{0};
    };

    struct reclaim_stats_snapshot {
        uint64_t reclaim_total_refs = 0;
        uint64_t partial_into_dirty = 0;
        uint64_t partial_into_open = 0;
        uint64_t partial_into_allocatable = 0;
        uint64_t partial_into_cache = 0;
        uint64_t partial_into_hole = 0;
        uint64_t partial_into_untracked = 0;
        uint64_t whole_into_dirty = 0;
        uint64_t whole_clears_existing = 0;
        uint64_t whole_already_pending = 0;
        uint64_t dropped_freed_mask_zero = 0;
    };
    //
    // put_entry: caller-supplied entry. The body is borrowed (must outlive
    // the persist round). out_vr is filled in-place by the scheduler with
    // the durable location after the round commits successfully.

    struct put_entry {
        std::string_view body;
        value_ref*       out_vr;
    };

    struct value_io_policy {
        uint32_t max_write_inflight = 32;
        uint32_t max_read_inflight  = 32;
        uint32_t max_trim_inflight  = 16;
    };

    // prepare_persist sender output. The variant lets the pipeline split
    // into a leader branch (NVMe writes + commit), a prefill branch
    // (non-resident partial pages need a read first), and a follower
    // branch (already unblocked by leader's finalize).

    struct persist_leader {
        uint64_t                           round_id;
        std::span<memory::frame_write_desc> writes;
        uint32_t                           max_write_inflight;
    };

    struct persist_prefill {
        uint64_t                          round_id;
        std::span<memory::frame_read_desc> reads;
        uint32_t                          max_read_inflight;
    };

    struct trim_batch {
        uint64_t             batch_id;
        std::span<trim_desc> trims;
        uint32_t             max_trim_inflight;
    };

    struct trim_idle {};

    using prepare_trim_result = std::variant<trim_idle, trim_batch>;

    // Followers piggy-back on the leader's NVMe write. handle_finalize
    // copies the leader's nvme_ok into every follower's `ok` field so the
    // caller can react to a failed round even when it didn't drive the
    // write itself. ok=false means: the leader's NVMe FUA write failed,
    // the round has been rolled back, and the out_vr filled in during
    // prepare_persist must be considered invalid.
    struct persist_follower {
        bool ok;
    };

    using prepare_persist_result = std::variant<
        persist_leader,
        persist_prefill,
        persist_follower>;

    // read sender output (variant). hit branches return immediately with the
    // decoded body. miss branches hand the pipeline a buf + paddr; pipeline
    // is responsible for issuing nvme->read into buf, then calling
    // fill_and_decode(vr, buf) to insert into cache + decode + return.

    struct read_hit { std::string body; };

    // read miss → pipeline issues the NVMe read into rm.frame, then hands
    // the frame back via fill_and_decode for cache admission + decode.
    //
    // admit_to_cache encodes the policy that only span_lbas == 1 pages are
    // cache-admissible — the readonly cache pool is single-sized for
    // simplicity, multi-LBA hit rate is too low to justify the capacity hit.

    struct read_miss {
        paddr                                           base;
        uint32_t                                        span_lbas;
        memory::pooled_frame_ptr<memory::segmented_page_frame> frame;
        bool                                            admit_to_cache;
    };

    using read_prepare_result = std::variant<read_hit, read_miss>;

    // ── Forward declarations of req types (used by sender layer) ──

    namespace _value_persist       { struct req; }
    namespace _value_finalize      { struct req; }
    namespace _value_continue      { struct req; }
    namespace _value_read          { struct req; }
    namespace _value_fill          { struct req; }
    namespace _value_reclaim       { struct req; }
    namespace _value_trim_prepare  { struct req; }
    namespace _value_trim_complete { struct req; }

    struct value_alloc_sched_base;

    namespace _value_persist {

        struct req {
            std::span<put_entry>                                    entries;
            std::move_only_function<void(prepare_persist_result&&)> cb;
            std::move_only_function<void(std::exception_ptr)>       fail;
        };

        struct op {
            constexpr static bool value_persist_op = true;
            value_alloc_sched_base*  sched;
            std::span<put_entry>     entries;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base* sched;
            std::span<put_entry>    entries;

            auto make_op() { return op{.sched = sched, .entries = entries}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    namespace _value_finalize {

        struct req {
            uint64_t                                          round_id;
            bool                                              ok;
            std::move_only_function<void()>                   cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool value_finalize_op = true;
            value_alloc_sched_base* sched;
            uint64_t                round_id;
            bool                    ok;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base* sched;
            uint64_t                round_id;
            bool                    ok;

            auto make_op() { return op{.sched = sched, .round_id = round_id, .ok = ok}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    namespace _value_continue {

        struct req {
            uint64_t                                          round_id;
            bool                                              read_ok;
            std::move_only_function<void(persist_leader&&)>   cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool value_continue_op = true;
            value_alloc_sched_base* sched;
            uint64_t                round_id;
            bool                    read_ok;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base* sched;
            uint64_t                round_id;
            bool                    read_ok;

            auto make_op() { return op{.sched = sched, .round_id = round_id, .read_ok = read_ok}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    namespace _value_read {

        struct req {
            value_ref                                            vr;
            std::move_only_function<void(read_prepare_result&&)> cb;
            std::move_only_function<void(std::exception_ptr)>    fail;
        };

        struct op {
            constexpr static bool value_read_op = true;
            value_alloc_sched_base* sched;
            value_ref               vr;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base* sched;
            value_ref               vr;

            auto make_op() { return op{.sched = sched, .vr = vr}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    namespace _value_fill {

        struct req {
            value_ref                                         vr;
            memory::pooled_frame_ptr<memory::segmented_page_frame> frame;
            bool                                              admit_to_cache;
            std::move_only_function<void(std::string&&)>      cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool value_fill_op = true;
            value_alloc_sched_base* sched;
            value_ref               vr;
            memory::pooled_frame_ptr<memory::segmented_page_frame> frame;
            bool                    admit_to_cache;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base* sched;
            value_ref               vr;
            memory::pooled_frame_ptr<memory::segmented_page_frame> frame;
            bool                    admit_to_cache;

            auto make_op() {
                return op{
                    .sched          = sched,
                    .vr             = vr,
                    .frame          = std::move(frame),
                    .admit_to_cache = admit_to_cache,
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    namespace _value_reclaim {

        struct req {
            std::vector<value_ref>                            dead_values;
            std::move_only_function<void()>                   cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool value_reclaim_op = true;
            value_alloc_sched_base*    sched;
            std::span<const value_ref> dead_values;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base*    sched;
            std::span<const value_ref> dead_values;

            auto make_op() {
                return op{
                    .sched       = sched,
                    .dead_values = dead_values,
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    namespace _value_trim_prepare {

        struct req {
            std::move_only_function<void(prepare_trim_result&&)> cb;
            std::move_only_function<void(std::exception_ptr)>    fail;
        };

        struct op {
            constexpr static bool value_trim_prepare_op = true;
            value_alloc_sched_base* sched;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base* sched;

            auto make_op() { return op{.sched = sched}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    namespace _value_trim_complete {

        struct req {
            uint64_t                                          batch_id;
            bool                                              ok;
            std::move_only_function<void()>                   cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool value_trim_complete_op = true;
            value_alloc_sched_base* sched;
            uint64_t                batch_id;
            bool                    ok;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base* sched;
            uint64_t                batch_id;
            bool                    ok;

            auto make_op() {
                return op{
                    .sched    = sched,
                    .batch_id = batch_id,
                    .ok       = ok,
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    // ── value_alloc_sched_base ─────────────────────────────────────────────
    //
    // Non-templated layer holding the PUMP queues, the schedule_*
    // enqueue helpers, and the sender factory entry points. Senders / ops
    // only depend on this base — they never see the templated derived class —
    // so the PUMP pipeline machinery doesn't have to know what readonly
    // Cache the scheduler uses.

    struct value_alloc_sched_base {
        pump::core::per_core::queue<_value_persist::req*>       persist_q_;
        pump::core::per_core::queue<_value_finalize::req*>      finalize_q_;
        pump::core::per_core::queue<_value_continue::req*>      continue_q_;
        pump::core::per_core::queue<_value_read::req*>          read_q_;
        pump::core::per_core::queue<_value_fill::req*>          fill_q_;
        pump::core::per_core::queue<_value_reclaim::req*>       reclaim_q_;
        pump::core::per_core::queue<_value_trim_prepare::req*>  trim_prepare_q_;
        pump::core::per_core::queue<_value_trim_complete::req*> trim_complete_q_;
        const value_io_policy                                   io_policy_;
        uint32_t                                                max_body_len_ = 0;
        reclaim_stats                                           reclaim_stats_;

        explicit
        value_alloc_sched_base(size_t          queue_depth = 2048,
                               value_io_policy io_policy = {})
            : persist_q_(queue_depth)
            , finalize_q_(queue_depth)
            , continue_q_(queue_depth)
            , read_q_(queue_depth)
            , fill_q_(queue_depth)
            , reclaim_q_(queue_depth)
            , trim_prepare_q_(queue_depth)
            , trim_complete_q_(queue_depth)
            , io_policy_(io_policy) {
            validate_io_policy_(io_policy_);
        }

        [[nodiscard]] const value_io_policy& io_policy() const noexcept {
            return io_policy_;
        }

        [[nodiscard]] uint32_t max_body_len() const noexcept {
            return max_body_len_;
        }

        [[nodiscard]] reclaim_stats_snapshot
        inspect_reclaim_stats() const noexcept {
            return reclaim_stats_snapshot{
                .reclaim_total_refs = reclaim_stats_.reclaim_total_refs.load(
                    std::memory_order_relaxed),
                .partial_into_dirty = reclaim_stats_.partial_into_dirty.load(
                    std::memory_order_relaxed),
                .partial_into_open = reclaim_stats_.partial_into_open.load(
                    std::memory_order_relaxed),
                .partial_into_allocatable =
                    reclaim_stats_.partial_into_allocatable.load(
                        std::memory_order_relaxed),
                .partial_into_cache = reclaim_stats_.partial_into_cache.load(
                    std::memory_order_relaxed),
                .partial_into_hole = reclaim_stats_.partial_into_hole.load(
                    std::memory_order_relaxed),
                .partial_into_untracked =
                    reclaim_stats_.partial_into_untracked.load(
                        std::memory_order_relaxed),
                .whole_into_dirty = reclaim_stats_.whole_into_dirty.load(
                    std::memory_order_relaxed),
                .whole_clears_existing =
                    reclaim_stats_.whole_clears_existing.load(
                        std::memory_order_relaxed),
                .whole_already_pending =
                    reclaim_stats_.whole_already_pending.load(
                        std::memory_order_relaxed),
                .dropped_freed_mask_zero =
                    reclaim_stats_.dropped_freed_mask_zero.load(
                        std::memory_order_relaxed),
            };
        }

        static void
        validate_io_policy_(const value_io_policy& policy) {
            if (policy.max_write_inflight == 0 ||
                policy.max_read_inflight == 0 ||
                policy.max_trim_inflight == 0) {
                core::panic_inconsistency(
                    "value::value_alloc_sched_base::ctor",
                    "value_io_policy fields must be non-zero (write=%u read=%u trim=%u)",
                    static_cast<unsigned>(policy.max_write_inflight),
                    static_cast<unsigned>(policy.max_read_inflight),
                    static_cast<unsigned>(policy.max_trim_inflight));
            }
        }

        // ── enqueue helpers (called by op::start) ──

        void schedule_persist      (_value_persist::req*       r) { persist_q_      .try_enqueue(r); }
        void schedule_finalize     (_value_finalize::req*      r) { finalize_q_     .try_enqueue(r); }
        void schedule_continue     (_value_continue::req*      r) { continue_q_     .try_enqueue(r); }
        void schedule_read         (_value_read::req*          r) { read_q_         .try_enqueue(r); }
        void schedule_fill         (_value_fill::req*          r) { fill_q_         .try_enqueue(r); }
        void schedule_reclaim      (_value_reclaim::req*       r) { reclaim_q_      .try_enqueue(r); }
        void schedule_trim_prepare (_value_trim_prepare::req*  r) { trim_prepare_q_ .try_enqueue(r); }
        void schedule_trim_complete(_value_trim_complete::req* r) { trim_complete_q_.try_enqueue(r); }

        // ── Sender factories ──

        auto prepare_persist(std::span<put_entry> entries);
        auto finalize_persist(uint64_t round_id, bool ok);
        auto continue_persist(uint64_t round_id, bool read_ok);
        auto prepare_read(value_ref vr);
        auto fill_and_decode(
            value_ref vr,
            memory::pooled_frame_ptr<memory::segmented_page_frame> frame,
            bool admit_to_cache);
        auto reclaim_values(std::span<const value_ref> dead_values);
        auto prepare_trim_batch();
        auto complete_trim_batch(uint64_t batch_id, bool ok);
    };

    // ── value_alloc_sched<Cache> ───────────────────────────────────────────
    //
    // Wires put / read / reclaim / trim through a `value_space_manager` (037
    // plan §"独立类边界"). The scheduler owns NO free-space truth: every
    // logical allocation, reclaim, trim plan, and recovery rebuild goes
    // through `space_`. The scheduler's responsibility is the runtime
    // membrane around the manager:
    //   - leader/follower batching for persist rounds,
    //   - DMA buffer lifecycle (readonly_cache_ + resident_partial_ +
    //     in-flight round frames),
    //   - NVMe write / read / trim descriptor construction,
    //   - PUMP req / sender plumbing.

    template <core::cache_concept Cache>
    struct value_alloc_sched : value_alloc_sched_base {
        // ── Per-class lookup table (mirror of manager's classes) ──
        //
        // Cached locally so hot paths (encode, frame size compute) avoid an
        // indirect manager call per claim. `value_space_manager_config` is
        // the single source of truth for the inputs; this table is rebuilt
        // from it during construction.

        struct class_info {
            uint32_t class_size;
            uint32_t span_lbas;
            uint32_t alloc_quantums;
            bool     sub_lba;
        };

        // ── Round-scoped state ──
        //
        // Each persist round captures a value_space_round handle from the
        // manager + the per-page DMA frames the scheduler holds during the
        // round. byte_claims live inside the manager round; we keep a
        // parallel `claims` vector so encode/finalize don't need to touch
        // the manager state again.

        struct round_page {
            memory::segmented_page_frame* frame;        // DMA buffer for this page
            paddr               page_base;
            uint16_t            span_lbas;
            byte_claim_source   src;
            uint64_t            cache_epoch_in;          // valid for cached_partial src
            uint16_t            starting_free_quantums;  // pre-claim free count
            uint16_t            consumed_quantums;       // sum of this round's claim sizes on this page
            bool                needs_prefill;           // true iff src == nonresident_partial
            bool                prefill_loaded;
        };

        struct round {
            enum class stage : uint8_t {
                prefill_pending,
                writeback_inflight,
            };

            uint64_t                id;
            stage                   st = stage::prefill_pending;

            // Followers ONLY (the leader's req is delete-on-publish via the
            // publish_prefill / publish_round handshake; storing it here
            // would dangle through the prefill→continue gap). On round
            // settlement (commit / rollback / continue-failure) every
            // follower MUST be notified exactly once and then deleted.
            std::vector<_value_persist::req*> followers;

            // Snapshot of put_entry pointers in claim order, taken at
            // handle_persist time. Survives the publish_prefill delete of
            // the leader item so handle_continue can encode without
            // walking any persist req. The pointed-to put_entry objects
            // are owned by the original callers and stay alive for the
            // duration of the persist pipeline (the same lifetime contract
            // that put_entry::body string_view already relies on).
            std::vector<put_entry*> entries_flat;

            std::vector<round_page>  pages;
            std::vector<byte_claim>  claims;       // parallel to entries_flat
            std::vector<memory::frame_read_desc>   reads;
            std::vector<memory::frame_write_desc>  writes;
            value_space_round        space_round;
            uint64_t                 lowest_fresh_lba;  // min(page_base.lba) over fresh pages
        };

        // ── Resident partial pages (post-commit, write-reuse) ──
        //
        // Indexed by page_base because mixed-class pages no longer belong
        // to a single class. The cache_epoch field is the residency token
        // paired with manager.cached_partial_index entries.

        struct resident_partial_entry {
            memory::segmented_page_frame* frame;
            uint16_t            span_lbas;
            uint64_t            cache_epoch;
        };

        // ── Inflight trim batch state ──
        //
        // The manager owns the withheld free LBAs (trim_inflight_) until
        // complete_trim is called. The scheduler keeps the matching
        // trim_desc[] alive across the asynchronous NVMe phase so the
        // pipeline can drain `trim_batch.trims` after prepare returns.

        struct trim_batch_state {
            uint64_t                                  batch_id;
            apps::inconel::value::trim_plan_id        plan_id;
            std::vector<format::trim_desc>            trims;
        };

        std::unique_ptr<value_space_manager>                  space_;
        absl::InlinedVector<class_info, 16>                   class_table_;

        // resident_partial_ is the scheduler-side mirror of the manager's
        // cached_partial_index: every page listed here MUST also have a
        // cached_partial_index entry under the same cache_epoch (the
        // residency contract). Eviction (whether driven by budget or by
        // explicit reclaim that emptied the page) MUST update both sides
        // in lockstep.
        absl::flat_hash_map<paddr, resident_partial_entry>    resident_partial_;

        // dirty_round_pages_ holds page_base values currently owned by an
        // inflight round_page. Reads against these pages must use the
        // round's frame (it is the freshest source). Reclaims are deferred
        // until commit/abort so we don't update manager metadata while the
        // round still has the old layout in flight.
        absl::flat_hash_set<paddr>                            dirty_round_pages_;
        absl::flat_hash_map<paddr, std::vector<value_ref>>    deferred_releases_;

        Cache                                                 readonly_cache_;

        absl::flat_hash_map<uint64_t, std::unique_ptr<round>> inflight_rounds_;
        uint64_t                                              next_round_id_   = 1;
        uint64_t                                              next_cache_epoch_ = 1;
        uint64_t                                              next_heat_seq_   = 1;

        absl::flat_hash_map<uint64_t,
                            std::unique_ptr<trim_batch_state>> inflight_trim_batches_;
        uint64_t                                              next_trim_batch_id_ = 1;

        core::data_area_heads* shared_heads_      = nullptr;
        uint64_t               value_low_watermark_lba_ = 0;
        uint64_t               data_area_end_lba_       = 0;
        uint16_t               device_id_                = 0;
        uint32_t               lba_size_                 = 0;
        memory::lba_dma_page_pool                         frame_pool_;
        uint32_t               quantums_per_lba_         = 0;
        uint32_t               quantum_bytes_            = 0;

        // Cached partial DMA-frame budget mirrors manager's cap so the
        // scheduler's resident_partial_ stays bounded even when the manager's
        // index would otherwise admit indefinitely. Soft eviction kicks in
        // before hitting the manager-side budget so the two layers stay
        // in sync without explicit cross-call accounting.
        uint64_t resident_partial_budget_pages_ = 0;

        // ── Constructor ──

        value_alloc_sched(std::span<const uint32_t> class_sizes,
                          uint32_t                  lba_size,
                          paddr                     data_area_base,
                          paddr                     data_area_end,
                          core::data_area_heads*    shared_heads,
                          Cache                     cache,
                          uint32_t                  value_space_quantum_bytes,
                          uint32_t                  value_space_group_size_lbas,
                          size_t                    queue_depth = 2048,
                          memory::dma_page_allocator frame_allocator =
                              memory::make_heap_dma_page_allocator(),
                          uint32_t                  frame_alignment = 4096,
                          int                       frame_numa_id = -1,
                          value_io_policy           io_policy = {})
            : value_alloc_sched_base(queue_depth, io_policy)
            , readonly_cache_(std::move(cache))
            , shared_heads_(shared_heads)
            , data_area_end_lba_(data_area_end.lba)
            , device_id_(data_area_base.device_id)
            , lba_size_(lba_size)
            , frame_pool_(lba_size, frame_alignment, frame_numa_id,
                          frame_allocator)
            , quantum_bytes_(value_space_quantum_bytes)
            , class_sizes_storage_(class_sizes.begin(), class_sizes.end())
        {
            if (data_area_base.device_id != data_area_end.device_id) {
                core::panic_inconsistency(
                    "value::value_alloc_sched::ctor",
                    "data_area_base/end device_id mismatch (%u vs %u)",
                    static_cast<unsigned>(data_area_base.device_id),
                    static_cast<unsigned>(data_area_end.device_id));
            }
            if (data_area_base.lba >= data_area_end.lba) {
                core::panic_inconsistency(
                    "value::value_alloc_sched::ctor",
                    "data_area_base.lba >= data_area_end.lba (%lu vs %lu)",
                    static_cast<unsigned long>(data_area_base.lba),
                    static_cast<unsigned long>(data_area_end.lba));
            }

            quantums_per_lba_ = lba_size_ / quantum_bytes_;
            value_low_watermark_lba_ = data_area_end_lba_;
            if (shared_heads_ != nullptr) {
                shared_heads_->value_head_lba.store(
                    value_low_watermark_lba_, std::memory_order_relaxed);
            }

            // Build the local class_info table from class_sizes. The same
            // shape rules the manager enforces (sub-LBA = lba_size·2^-n,
            // multi-LBA = lba_size·2^m) are presumed already validated by
            // runtime::validate_build_inputs.
            class_table_.reserve(class_sizes_storage_.size());
            for (uint32_t cs : class_sizes_storage_) {
                class_info ci{};
                ci.class_size = cs;
                if (cs < lba_size_) {
                    ci.sub_lba        = true;
                    ci.span_lbas      = 1;
                    ci.alloc_quantums = cs / quantum_bytes_;
                } else {
                    ci.sub_lba        = false;
                    ci.span_lbas      = cs / lba_size_;
                    ci.alloc_quantums = (cs / quantum_bytes_);
                }
                class_table_.push_back(ci);
            }
            uint32_t largest_class_size = 0;
            for (const auto& ci : class_table_) {
                largest_class_size = std::max(largest_class_size, ci.class_size);
            }
            if (largest_class_size <= sizeof(format::value_object_header)) {
                core::panic_inconsistency(
                    "value::value_alloc_sched::ctor",
                    "largest value class too small for value object header (class=%u header=%u)",
                    static_cast<unsigned>(largest_class_size),
                    static_cast<unsigned>(sizeof(format::value_object_header)));
            }
            max_body_len_ =
                largest_class_size -
                static_cast<uint32_t>(sizeof(format::value_object_header));

            value_space_manager_config cfg{};
            cfg.lba_size                    = lba_size_;
            cfg.value_space_quantum_bytes   = quantum_bytes_;
            cfg.value_space_group_size_lbas = value_space_group_size_lbas;
            cfg.device_id                   = device_id_;
            cfg.data_area_base_lba          = data_area_base.lba;
            cfg.data_area_end_lba           = data_area_end.lba;
            cfg.value_class_sizes           = std::span<const uint32_t>(
                class_sizes_storage_.data(), class_sizes_storage_.size());
            cfg.object_header_bytes         = sizeof(format::value_object_header);
            cfg.shared_heads                = shared_heads_;
            space_ = std::make_unique<value_space_manager>(std::move(cfg));

            // Page-budget mirror: cap resident_partial_ at
            // budget_bytes / lba_size pages so the scheduler does not
            // outrun the manager's cached_partial_index admission cap.
            resident_partial_budget_pages_ =
                space_->config().value_cached_partial_budget_bytes / lba_size_;
            if (resident_partial_budget_pages_ == 0) {
                resident_partial_budget_pages_ = 1;
            }
        }

        // ── Destructor ──

        ~value_alloc_sched() {
            // In-flight rounds: round_pages may share a frame with
            // resident_partial_ when a cached candidate was pinned but the
            // round never reached commit. Free the frame from the round
            // path; the resident_partial_ entry will then be skipped (its
            // pointer matches one of the round's frames).
            absl::flat_hash_set<memory::segmented_page_frame*> freed;
            for (auto& [id, rnd] : inflight_rounds_) {
                for (auto& page : rnd->pages) {
                    if (page.frame == nullptr) continue;
                    if (freed.insert(page.frame).second) {
                        destroy_frame(page.frame);
                    }
                    page.frame = nullptr;
                }
            }
            for (auto& [page_base, entry] : resident_partial_) {
                if (entry.frame == nullptr) continue;
                if (freed.insert(entry.frame).second) {
                    destroy_frame(entry.frame);
                }
            }
            while (auto f = readonly_cache_.drain_one()) {
                destroy_frame(*f);
            }
        }

        // ── advance ──
        //
        // Order matters: finalize first (releases inflight rounds and may
        // install partial pages into resident_partial_, making them
        // available for subsequent persist rounds). persist next (consumes
        // freshly available pages). read/fill last.

        static constexpr uint32_t kMaxFinalizePerAdvance      = 64;
        static constexpr uint32_t kMaxReclaimPerAdvance       = 64;
        static constexpr uint32_t kMaxTrimPreparePerAdvance   = 16;
        static constexpr uint32_t kMaxTrimCompletePerAdvance  = 16;
        static constexpr uint32_t kMaxContinuePerAdvance      = 32;
        static constexpr uint32_t kMaxPersistRoundsPerAdvance = 32;
        static constexpr uint32_t kMaxReadPerAdvance          = 64;
        static constexpr uint32_t kMaxFillPerAdvance          = 64;

        bool advance() {
            bool progress = false;

            for (uint32_t i = 0; i < kMaxFinalizePerAdvance; ++i) {
                auto item = finalize_q_.try_dequeue();
                if (!item) break;
                handle_finalize(*item);
                progress = true;
            }

            for (uint32_t i = 0; i < kMaxReclaimPerAdvance; ++i) {
                auto item = reclaim_q_.try_dequeue();
                if (!item) break;
                handle_reclaim(*item);
                progress = true;
            }

            for (uint32_t i = 0; i < kMaxTrimCompletePerAdvance; ++i) {
                auto item = trim_complete_q_.try_dequeue();
                if (!item) break;
                handle_complete_trim_batch(*item);
                progress = true;
            }

            for (uint32_t i = 0; i < kMaxContinuePerAdvance; ++i) {
                auto item = continue_q_.try_dequeue();
                if (!item) break;
                handle_continue(*item);
                progress = true;
            }

            for (uint32_t i = 0; i < kMaxPersistRoundsPerAdvance; ++i) {
                auto item = persist_q_.try_dequeue();
                if (!item) break;
                handle_persist(*item);
                progress = true;
            }

            for (uint32_t i = 0; i < kMaxReadPerAdvance; ++i) {
                auto item = read_q_.try_dequeue();
                if (!item) break;
                handle_read(*item);
                progress = true;
            }

            for (uint32_t i = 0; i < kMaxFillPerAdvance; ++i) {
                auto item = fill_q_.try_dequeue();
                if (!item) break;
                handle_fill(*item);
                progress = true;
            }

            for (uint32_t i = 0; i < kMaxTrimPreparePerAdvance; ++i) {
                auto item = trim_prepare_q_.try_dequeue();
                if (!item) break;
                handle_prepare_trim_batch(*item);
                progress = true;
            }

            return progress;
        }

        template<typename runtime_t>
        bool advance(runtime_t&) { return advance(); }

        // ── Recovery installation ──
        //
        // Single entry point for the (future) recovery pipeline to hand the
        // tree/WAL-derived live extents to the value subsystem. The manager
        // rebuilds free-space truth from the complement; the scheduler
        // republishes the value-low watermark so tree allocation backs off
        // from any prefix already touched by value.
        //
        // Out of scope here: rebuilding resident_partial_ / readonly_cache_.
        // 037 plan §"Recovery" rule 5 ("cached_partial_index 不参与
        // recovery") — caches re-fill at runtime via writeback / reads.

        void
        install_recovered_value_space(
            std::span<const live_value_extent> live_extents,
            uint64_t                           tree_alloc_head_lba,
            std::span<const dead_class_hint>   hints) {
            space_->install_recovered_state(
                live_extents,
                tree_alloc_head_lba,
                data_area_end_lba_,
                hints);

            // Republish value low watermark = lowest LBA still claimed by
            // value-side metadata. After install, the lowest claimed LBA is
            // either (a) the lowest live extent base, or (b) data_area_end
            // when the area is empty. tree_alloc_head_lba lower-bounds it
            // because nothing below the tree head belongs to value anymore.
            uint64_t lowest = data_area_end_lba_;
            for (const auto& e : live_extents) {
                if (e.base.lba < lowest) lowest = e.base.lba;
            }
            if (lowest < tree_alloc_head_lba) lowest = tree_alloc_head_lba;
            value_low_watermark_lba_ = lowest;
            if (shared_heads_ != nullptr) {
                shared_heads_->value_head_lba.store(
                    value_low_watermark_lba_, std::memory_order_relaxed);
            }
        }

        // Inspector for runtime / tests that want to see the manager.
        value_space_manager& space() noexcept { return *space_; }

    private:
        // ════════════════════════════════════════════════════════════════
        //  handle_persist  —  leader-follower round assembly
        // ════════════════════════════════════════════════════════════════
        //
        // Phases:
        //   1. collect_round_items(leader)        — drain followers up to cap
        //   2. build allocation_request[]         — class lookup, alloc_bytes
        //   3. space_->allocate_batch             — manager places claims
        //   4. translate claims → round_pages     — pin/take/alloc DMA frames
        //   5. encode_round_entries (or defer if reads.empty() == false)
        //   6. publish_prefill / publish_round    — fire leader cb
        //
        // Recoverable failure: caller body exceeds all classes →
        // value_too_large → fail items + abort space round.
        //
        // Fatal failure: out of space, encode disagrees with class table,
        // stale cached candidate, NVMe path corruption — panic. These mean
        // an upstream invariant has broken and silent-fail would propagate
        // corruption.

        enum class persist_entry_status : uint8_t {
            ok = 0,
            value_too_large,   // recoverable
            out_of_space,      // fatal
            encode_failure,    // fatal
            stale_cached,      // fatal — cached_partial_index drift
        };

        static constexpr uint32_t kMaxFollowersPerRound = 64;

        void
        handle_persist(_value_persist::req* leader_item) {
            auto items = collect_round_items(leader_item);

            // Build the allocation_request[] over the entire item set.
            // entries_flat parallels reqs[] so we can fill out_vr in claim
            // order during translation.
            std::vector<allocation_request> reqs;
            std::vector<put_entry*>         entries_flat;
            uint32_t total_entries = 0;
            for (auto* item : items) total_entries += item->entries.size();
            reqs.reserve(total_entries);
            entries_flat.reserve(total_entries);

            for (auto* item : items) {
                for (auto& entry : item->entries) {
                    auto ci_opt = class_for_body_len(entry.body.size());
                    if (!ci_opt) {
                        // Recoverable: this body exceeds every class. Fail
                        // the entire round (we can't easily abort a single
                        // entry — its sibling claims may already be live in
                        // the manager). The caller can re-batch the
                        // surviving entries.
                        auto failure = std::make_exception_ptr(std::runtime_error(
                            "value::persist: body length exceeds all size classes"));
                        for (auto* it : items) {
                            it->fail(failure);
                            delete it;
                        }
                        return;
                    }
                    const class_info& ci = class_table_[*ci_opt];
                    reqs.push_back(allocation_request{
                        .entry_index    = static_cast<uint32_t>(reqs.size()),
                        .class_idx      = *ci_opt,
                        .alloc_bytes    = ci.class_size,
                        .alloc_quantums = ci.alloc_quantums,
                    });
                    entries_flat.push_back(&entry);
                }
            }

            auto rnd = std::make_unique<round>();
            rnd->id = next_round_id_++;
            // Followers are items[1..]; the leader (items[0]) is settled by
            // publish_{prefill,round} and never stored in the round, so a
            // post-publish access cannot race a delete on the leader.
            rnd->followers.reserve(items.size() > 1 ? items.size() - 1 : 0);
            for (size_t i = 1; i < items.size(); ++i) {
                rnd->followers.push_back(items[i]);
            }
            rnd->entries_flat = entries_flat;  // snapshot survives leader delete
            rnd->space_round = space_->begin_round();
            rnd->lowest_fresh_lba = data_area_end_lba_;

            auto claims = space_->allocate_batch(rnd->space_round, reqs);
            if (claims.empty()) {
                // Manager rejected the batch (space exhausted or hard gate).
                // Per project rule "禁止 silent fallback" + "10亿 KV 起步",
                // out-of-space is a fatal invariant break, not a recoverable
                // condition — at v1 we have no compaction loop to retry
                // against. Manager has already rolled back its round.
                space_->abort(std::move(rnd->space_round));
                core::panic_inconsistency(
                    "value::value_alloc_sched::handle_persist",
                    "value_space_manager::allocate_batch returned empty for round %lu (entries=%u)",
                    static_cast<unsigned long>(rnd->id),
                    static_cast<unsigned>(reqs.size()));
            }
            rnd->claims = std::move(claims);

            auto status = translate_claims_into_round(*rnd, entries_flat);
            if (status == persist_entry_status::stale_cached) {
                // Stale cached candidate: manager's index pointed to a page
                // we no longer hold. Fatal because the manager's own
                // bookkeeping just contradicted the scheduler's mirror.
                // erase calls already fired in-line so the index is back
                // in sync; the round is undone wholesale.
                rollback_partial_round(*rnd);
                core::panic_inconsistency(
                    "value::value_alloc_sched::handle_persist",
                    "stale cached_partial claim for round %lu",
                    static_cast<unsigned long>(rnd->id));
            }
            if (status == persist_entry_status::encode_failure) {
                rollback_partial_round(*rnd);
                core::panic_inconsistency(
                    "value::value_alloc_sched::handle_persist",
                    "encode_value_object failed during round %lu",
                    static_cast<unsigned long>(rnd->id));
            }

            // `items.front()` is the leader. It is NOT in the round
            // (round.followers stores items[1..] only) and is consumed —
            // cb'd then deleted — by publish_prefill / publish_round.
            _value_persist::req* leader = items.front();

            if (!rnd->reads.empty()) {
                publish_prefill(std::move(rnd), leader);
                return;
            }

            status = encode_round_entries(*rnd, entries_flat);
            if (status != persist_entry_status::ok) {
                rollback_partial_round(*rnd);
                core::panic_inconsistency(
                    "value::value_alloc_sched::handle_persist",
                    "encode_round_entries failed (status=%u) for round %lu",
                    static_cast<unsigned>(status),
                    static_cast<unsigned long>(rnd->id));
            }

            finalize_round_writes(*rnd);
            publish_round(std::move(rnd), leader);
        }

        std::vector<_value_persist::req*>
        collect_round_items(_value_persist::req* leader_item) {
            std::vector<_value_persist::req*> items;
            items.reserve(1 + kMaxFollowersPerRound);
            items.push_back(leader_item);
            while (items.size() < 1u + kMaxFollowersPerRound) {
                auto next = persist_q_.try_dequeue();
                if (!next) break;
                items.push_back(*next);
            }
            return items;
        }

        // ── translate_claims_into_round ──
        //
        // Walk `claims` once and produce round_pages + per-entry value_ref
        // fills. Each unique page_base materializes a single round_page
        // with the appropriate DMA frame:
        //   cached_partial      → pin existing resident_partial_ entry
        //   new_whole_page      → fresh zero-filled DMA buffer
        //   nonresident_partial → fresh DMA buffer + frame_read_desc prefill
        //
        // entries_flat parallels rnd.claims; we also fill each entry's
        // out_vr in this pass so the caller sees the durable address as
        // soon as publish_round / publish_prefill fires.

        persist_entry_status
        translate_claims_into_round(round&                       rnd,
                                    const std::vector<put_entry*>& entries_flat) {
            absl::flat_hash_map<paddr, uint32_t> page_index_by_base;
            page_index_by_base.reserve(rnd.claims.size());

            for (uint32_t i = 0; i < rnd.claims.size(); ++i) {
                const byte_claim& c = rnd.claims[i];
                const class_info& ci = class_table_[c.class_idx];

                auto [iit, inserted] =
                    page_index_by_base.try_emplace(c.page_base,
                        static_cast<uint32_t>(rnd.pages.size()));
                if (inserted) {
                    round_page rp{};
                    rp.page_base       = c.page_base;
                    rp.span_lbas       = static_cast<uint16_t>(ci.span_lbas);
                    rp.src             = c.src;
                    rp.cache_epoch_in  = c.cache_epoch;
                    rp.consumed_quantums = 0;
                    rp.needs_prefill   = false;
                    rp.prefill_loaded  = true;

                    switch (c.src) {
                    case byte_claim_source::cached_partial: {
                        auto rit = resident_partial_.find(c.page_base);
                        if (rit == resident_partial_.end() ||
                            rit->second.cache_epoch != c.cache_epoch) {
                            // The manager believed this page was cached but
                            // the scheduler no longer holds it (eviction +
                            // missed notification). Sync the manager and
                            // bail out — the entire round will rollback.
                            space_->erase_cached_partial(
                                c.page_base, c.cache_epoch);
                            return persist_entry_status::stale_cached;
                        }
                        rp.frame = rit->second.frame;
                        rp.span_lbas = rit->second.span_lbas;
                        // The page entered our hands "cached partial" with
                        // some pre-existing free quantums. Remember that
                        // baseline so commit can compute the post-claim
                        // residual and refresh the cached entry properly.
                        rp.starting_free_quantums =
                            cached_starting_free_for_(c.page_base);
                        // Frame leaves the resident_partial_ table for the
                        // duration of the round — it is now "owned" by the
                        // round_page. Manager-side index entry is also
                        // erased so a concurrent re-admission cannot race.
                        space_->erase_cached_partial(c.page_base, c.cache_epoch);
                        resident_partial_.erase(rit);
                        break;
                    }
                    case byte_claim_source::new_whole_page: {
                        rp.frame = alloc_dma_frame(c.page_base, ci.span_lbas,
                                                   /*zero_fill=*/true);
                        rp.starting_free_quantums =
                            (ci.span_lbas == 1)
                                ? static_cast<uint16_t>(quantums_per_lba_)
                                : 0;
                        if (c.page_base.lba < rnd.lowest_fresh_lba) {
                            rnd.lowest_fresh_lba = c.page_base.lba;
                        }
                        break;
                    }
                    case byte_claim_source::nonresident_partial: {
                        rp.frame = alloc_dma_frame(c.page_base, ci.span_lbas,
                                                   /*zero_fill=*/false);
                        rp.starting_free_quantums = 0;  // unknown to scheduler;
                                                        // recomputed at commit
                                                        // via consumed only
                        rp.needs_prefill  = true;
                        rp.prefill_loaded = false;
                        rnd.reads.push_back(memory::frame_read_desc{
                            .frame = rp.frame,
                        });
                        break;
                    }
                    }

                    rnd.pages.push_back(rp);
                    dirty_round_pages_.insert(c.page_base);
                }

                round_page& rp = rnd.pages[iit->second];
                rp.consumed_quantums = static_cast<uint16_t>(
                    rp.consumed_quantums + ci.alloc_quantums);

                // Fill the caller's value_ref. byte_offset narrowed back to
                // uint16_t — manager guarantees byte_offset < lba_size, and
                // lba_size validation rejects anything that would overflow
                // the field.
                put_entry* e = entries_flat[i];
                e->out_vr->base        = c.page_base;
                e->out_vr->byte_offset = static_cast<uint16_t>(c.byte_offset);
                e->out_vr->len         = static_cast<uint32_t>(e->body.size());
                e->out_vr->flags       = 0;
            }

            return persist_entry_status::ok;
        }

        // ── encode_round_entries ──
        //
        // After every page in the round is resident (fresh / cached
        // immediately; nonresident_partial after prefill), encode each
        // borrowed body into the slot reserved by translate_claims_into_round.

        persist_entry_status
        encode_round_entries(round&                         rnd,
                             const std::vector<put_entry*>& entries_flat) {
            absl::flat_hash_map<paddr, memory::segmented_page_frame*> pages;
            pages.reserve(rnd.pages.size());
            for (auto& page : rnd.pages) {
                if (!page.prefill_loaded) {
                    return persist_entry_status::encode_failure;
                }
                pages.emplace(page.page_base, page.frame);
            }

            for (uint32_t i = 0; i < rnd.claims.size(); ++i) {
                const byte_claim& c  = rnd.claims[i];
                const class_info& ci = class_table_[c.class_idx];
                auto it = pages.find(c.page_base);
                if (it == pages.end()) {
                    return persist_entry_status::encode_failure;
                }
                auto* frame = it->second;
                std::span<const char> body_span(
                    entries_flat[i]->body.data(),
                    entries_flat[i]->body.size());
                if (!format::encode_value_object_slot(
                        *frame,
                        c.byte_offset, ci.class_size, body_span)) {
                    return persist_entry_status::encode_failure;
                }
            }

            return persist_entry_status::ok;
        }

        void
        finalize_round_writes(round& rnd) {
            rnd.writes.reserve(rnd.pages.size());
            for (auto& page : rnd.pages) {
                rnd.writes.push_back(memory::frame_write_desc{
                    .frame = page.frame,
                    .flags = 0,
                });
            }
        }

        // ── publish_prefill / publish_round ──

        void
        publish_prefill(std::unique_ptr<round> rnd,
                        _value_persist::req* leader_item) {
            uint64_t rid = rnd->id;
            auto [it, inserted] = inflight_rounds_.emplace(rid, std::move(rnd));
            if (!inserted) {
                core::panic_inconsistency("value::value_alloc_sched::publish_prefill",
                    "duplicate round_id %lu",
                    static_cast<unsigned long>(rid));
            }
            auto* stored = it->second.get();
            leader_item->cb(prepare_persist_result{
                persist_prefill{
                    .round_id = rid,
                    .reads    = std::span<memory::frame_read_desc>(
                        stored->reads.data(), stored->reads.size()),
                    .max_read_inflight = io_policy().max_read_inflight,
                }
            });
            delete leader_item;
        }

        std::span<memory::frame_write_desc>
        freeze_round_for_writeback(round& rnd) {
            for (auto& page : rnd.pages) {
                if (page.frame == nullptr) continue;
                page.frame->st = memory::frame_state::writeback_inflight;
            }
            rnd.st = round::stage::writeback_inflight;
            return std::span<memory::frame_write_desc>(
                rnd.writes.data(), rnd.writes.size());
        }

        void
        publish_round(std::unique_ptr<round> rnd,
                      _value_persist::req* leader_item) {
            uint64_t rid = rnd->id;
            auto [it, inserted] = inflight_rounds_.emplace(rid, std::move(rnd));
            if (!inserted) {
                core::panic_inconsistency("value::value_alloc_sched::publish_round",
                    "duplicate round_id %lu",
                    static_cast<unsigned long>(rid));
            }
            auto* stored = it->second.get();
            auto writes_span = freeze_round_for_writeback(*stored);
            leader_item->cb(prepare_persist_result{
                persist_leader{
                    .round_id           = rid,
                    .writes             = writes_span,
                    .max_write_inflight = io_policy().max_write_inflight,
                }
            });
            delete leader_item;

            // Now that the round is committed to writeback, propagate the
            // value-low watermark from any fresh extents the round carved.
            if (stored->lowest_fresh_lba < value_low_watermark_lba_) {
                value_low_watermark_lba_ = stored->lowest_fresh_lba;
                publish_value_head_();
            }
        }

        // ── handle_continue / handle_finalize  —  resume / settle rounds ──

        void
        handle_continue(_value_continue::req* item) {
            auto it = inflight_rounds_.find(item->round_id);
            if (it == inflight_rounds_.end()) {
                core::panic_inconsistency("value::value_alloc_sched::handle_continue",
                    "unknown round_id %lu",
                    static_cast<unsigned long>(item->round_id));
            }
            auto* rnd = it->second.get();
            if (rnd->st != round::stage::prefill_pending) {
                core::panic_inconsistency("value::value_alloc_sched::handle_continue",
                    "round %lu is not waiting for prefill",
                    static_cast<unsigned long>(item->round_id));
            }

            if (!item->read_ok) {
                auto failure = std::make_exception_ptr(std::runtime_error(
                    "value::persist: nonresident_partial prefill read failed"));
                // Followers were never cb'd — they're still suspended in
                // their persist_put_values pipelines. Notify each with
                // persist_follower{ok=false} (matches the leader-failure
                // semantics handle_finalize uses). Failing them via
                // f->fail(...) would also work but persist_follower{false}
                // keeps the public surface uniform: every follower sees a
                // persist_follower variant exactly once and decides by `ok`.
                for (auto* f : rnd->followers) {
                    f->cb(prepare_persist_result{persist_follower{false}});
                    delete f;
                }
                rnd->followers.clear();

                rollback_published_round(*rnd, /*nvme_ok=*/false);
                inflight_rounds_.erase(it);
                item->fail(failure);
                delete item;
                return;
            }

            for (auto& page : rnd->pages) {
                if (page.needs_prefill && !page.prefill_loaded) {
                    page.prefill_loaded = true;
                }
            }
            rnd->reads.clear();

            auto status = encode_round_entries(*rnd, rnd->entries_flat);
            if (status != persist_entry_status::ok) {
                core::panic_inconsistency("value::value_alloc_sched::handle_continue",
                    "encode after prefill failed (status=%u) for round %lu",
                    static_cast<unsigned>(status),
                    static_cast<unsigned long>(item->round_id));
            }
            finalize_round_writes(*rnd);

            auto writes_span = freeze_round_for_writeback(*rnd);

            const uint64_t lowest_fresh = rnd->lowest_fresh_lba;
            item->cb(persist_leader{
                .round_id           = rnd->id,
                .writes             = writes_span,
                .max_write_inflight = io_policy().max_write_inflight,
            });
            delete item;

            if (lowest_fresh < value_low_watermark_lba_) {
                value_low_watermark_lba_ = lowest_fresh;
                publish_value_head_();
            }
        }

        void
        handle_finalize(_value_finalize::req* item) {
            auto it = inflight_rounds_.find(item->round_id);
            if (it == inflight_rounds_.end()) {
                core::panic_inconsistency("value::value_alloc_sched::handle_finalize",
                    "unknown round_id %lu",
                    static_cast<unsigned long>(item->round_id));
            }
            auto rnd = std::move(it->second);
            inflight_rounds_.erase(it);

            if (rnd->st != round::stage::writeback_inflight) {
                core::panic_inconsistency("value::value_alloc_sched::handle_finalize",
                    "round %lu finalized before writeback stage",
                    static_cast<unsigned long>(item->round_id));
            }

            if (item->ok) {
                commit_round(*rnd);
            } else {
                rollback_published_round(*rnd, /*nvme_ok=*/false);
            }

            item->cb();
            for (auto* f : rnd->followers) {
                f->cb(prepare_persist_result{persist_follower{item->ok}});
                delete f;
            }
            rnd->followers.clear();
            delete item;
        }

        // ── commit_round ──
        //
        // NVMe write succeeded → manager commits the value_space_round
        // (fresh-page tail partials become real partial nodes). Per page,
        // the scheduler then decides what to do with the DMA frame:
        //   - all-class-bits consumed (full page) + 1-LBA → readonly_cache_
        //   - all-class-bits consumed + multi-LBA       → free DMA buffer
        //   - leftover quantums                          → resident_partial_
        //                                                  + mark_cached_partial
        //
        // Deferred reclaims for these pages (queued in deferred_releases_
        // while the round was dirty) are flushed AFTER the commit so they
        // see the post-commit page state.

        void
        commit_round(round& rnd) {
            // Snapshot page metadata before manager consumes the round.
            std::vector<paddr> committed_pages;
            committed_pages.reserve(rnd.pages.size());
            for (auto& page : rnd.pages) {
                committed_pages.push_back(page.page_base);
            }

            space_->commit(std::move(rnd.space_round));

            for (auto& page : rnd.pages) {
                dirty_round_pages_.erase(page.page_base);

                if (page.frame == nullptr) continue;

                bool full;
                if (page.src == byte_claim_source::new_whole_page &&
                    page.span_lbas == 1) {
                    // Sub-LBA / LBA-equal fresh page: full iff the round
                    // consumed all quantums of the page.
                    full = (page.consumed_quantums >= quantums_per_lba_);
                } else if (page.src == byte_claim_source::new_whole_page) {
                    // Multi-LBA fresh page: always exactly one entry, always full.
                    full = true;
                } else {
                    // Existing partial reuse (cached or non-resident). The
                    // manager removes the partial node when free_count
                    // reaches 0 (page now full); otherwise the node stays.
                    // page_is_partial is therefore the post-commit signal
                    // for "still has free quantums".
                    full = !space_->page_is_partial(page.page_base);
                }

                if (full) {
                    if (page.span_lbas == 1) {
                        // Demote to clean_readonly and admit to cache.
                        page.frame->st = memory::frame_state::clean_readonly;
                        if (auto evicted = readonly_cache_.put(page.frame)) {
                            destroy_frame(*evicted);
                        }
                    } else {
                        destroy_frame(page.frame);
                    }
                    page.frame = nullptr;
                    continue;
                }

                // Partial — install in resident_partial_ and notify manager.
                ensure_resident_partial_room_();
                const uint64_t epoch    = next_cache_epoch_++;
                const uint64_t heat_seq = next_heat_seq_++;
                page.frame->st = memory::frame_state::clean_allocatable;
                resident_partial_.emplace(page.page_base, resident_partial_entry{
                    .frame       = page.frame,
                    .span_lbas   = page.span_lbas,
                    .cache_epoch = epoch,
                });
                space_->mark_cached_partial(cached_partial_update{
                    .page_base   = page.page_base,
                    .kind        = cached_partial_kind::active_tail,
                    .heat_seq    = heat_seq,
                    .cache_epoch = epoch,
                });
                page.frame = nullptr;
            }

            flush_deferred_releases_(committed_pages);
        }

        // ── rollback_published_round ──
        //
        // NVMe writeback failed (or prefill failed before publish). Manager
        // already inverts metadata via `abort()`. The scheduler frees DMA
        // frames; cached_partial pages that were pinned during the round
        // are reinstalled to resident_partial_ + mark_cached_partial so
        // future rounds can still pick them up (the manager's rollback
        // recreates the partial node with the original bits).

        void
        rollback_published_round(round& rnd, bool nvme_ok) {
            (void)nvme_ok;

            std::vector<paddr> touched_pages;
            touched_pages.reserve(rnd.pages.size());
            for (auto& page : rnd.pages) {
                touched_pages.push_back(page.page_base);
            }

            // Capture which pages came from cached_partial — after abort,
            // the manager has restored their bits, and we want to put the
            // frame back in resident_partial_ instead of leaking.
            absl::flat_hash_map<
                paddr,
                std::pair<memory::segmented_page_frame*, uint16_t>>
                returnable_cached;
            for (auto& page : rnd.pages) {
                if (page.frame == nullptr) continue;
                if (page.src == byte_claim_source::cached_partial) {
                    returnable_cached.emplace(page.page_base,
                        std::make_pair(page.frame, page.span_lbas));
                    page.frame = nullptr;
                }
            }

            space_->abort(std::move(rnd.space_round));

            for (auto& page : rnd.pages) {
                dirty_round_pages_.erase(page.page_base);
                if (page.frame == nullptr) continue;
                destroy_frame(page.frame);
                page.frame = nullptr;
            }

            for (auto& [page_base, fp] : returnable_cached) {
                ensure_resident_partial_room_();
                const uint64_t epoch    = next_cache_epoch_++;
                const uint64_t heat_seq = next_heat_seq_++;
                fp.first->st = memory::frame_state::clean_allocatable;
                resident_partial_.emplace(page_base, resident_partial_entry{
                    .frame       = fp.first,
                    .span_lbas   = fp.second,
                    .cache_epoch = epoch,
                });
                space_->mark_cached_partial(cached_partial_update{
                    .page_base   = page_base,
                    .kind        = cached_partial_kind::active_tail,
                    .heat_seq    = heat_seq,
                    .cache_epoch = epoch,
                });
            }

            flush_deferred_releases_(touched_pages);
        }

        // Used when handle_persist itself bails out before publishing the
        // round into inflight_rounds_. Same body as rollback_published_round
        // but no NVMe phase to honor.
        void
        rollback_partial_round(round& rnd) {
            rollback_published_round(rnd, /*nvme_ok=*/false);
        }

        // ════════════════════════════════════════════════════════════════
        //  handle_reclaim  —  release dead value_refs
        // ════════════════════════════════════════════════════════════════

        void
        handle_reclaim(_value_reclaim::req* item) {
            reclaim_stats_.reclaim_total_refs.fetch_add(
                item->dead_values.size(), std::memory_order_relaxed);
            // Group by page_base. Each page's release is one of three states:
            //   - dirty (currently in an inflight round) → defer until commit
            //   - quiescent → release now, then refresh resident_partial_
            //                 cache entry if the page is still partial.
            absl::flat_hash_map<paddr, std::vector<value_ref>> by_page;
            for (const value_ref& vr : item->dead_values) {
                by_page[vr.base].push_back(vr);
            }

            std::vector<value_ref> immediate;
            immediate.reserve(item->dead_values.size());
            for (auto& [page_base, refs] : by_page) {
                if (dirty_round_pages_.contains(page_base)) {
                    // Defer until the dirty round completes — its commit /
                    // rollback path will replay these.
                    bump_reclaim_dirty_stats_(refs);
                    auto& pending = deferred_releases_[page_base];
                    pending.insert(pending.end(), refs.begin(), refs.end());
                    continue;
                }
                bump_reclaim_immediate_stats_(refs);
                immediate.insert(immediate.end(), refs.begin(), refs.end());
            }

            if (!immediate.empty()) {
                space_->release_values(
                    std::span<const value_ref>(immediate.data(), immediate.size()));
                refresh_caches_after_release_(immediate);
            }

            item->cb();
            delete item;
        }

        // ── refresh_caches_after_release_ ──
        //
        // After space_->release_values has updated manager metadata, walk
        // each unique affected page once and reconcile the scheduler's
        // resident_partial_ + readonly_cache_:
        //   - page now partial AND in resident_partial_ → refresh
        //     mark_cached_partial entry with new heat_seq / cache_epoch
        //     so manager's selection score reflects the post-reclaim free
        //     summary.
        //   - page now partial AND in readonly_cache_ (stale full image)
        //     → take frame, promote to resident_partial_, mark_cached_partial.
        //     This is the "reclaim hit on resident page" path from 037
        //     §"Cached Partial Admission" rule 5: the page was cached from
        //     an earlier write, reclaim opens room, and we can serve future
        //     writes against it without a prefill read.
        //   - page no longer partial (all-free or whole-page reclaim)
        //     → evict from BOTH resident_partial_ and readonly_cache_;
        //     the LBA is back in global_free_extents and a future fresh
        //     allocation may reuse it under a new identity.

        void
        refresh_caches_after_release_(std::span<const value_ref> released) {
            absl::flat_hash_set<paddr> seen;
            for (const value_ref& vr : released) {
                if (!seen.insert(vr.base).second) continue;
                const uint64_t partial_refs =
                    partial_ref_count_for_page_(released, vr.base);
                if (space_->page_is_partial(vr.base)) {
                    refresh_cached_partial_for_(vr.base, partial_refs);
                } else {
                    if (partial_refs != 0) {
                        reclaim_stats_.partial_into_hole.fetch_add(
                            partial_refs, std::memory_order_relaxed);
                    }
                    evict_resident_for_(vr.base);
                }
            }
        }

        void
        refresh_cached_partial_for_(paddr page_base, uint64_t partial_refs) {
            // Resident write-reuse frame: just refresh manager-side score.
            if (auto rit = resident_partial_.find(page_base);
                rit != resident_partial_.end()) {
                if (partial_refs != 0) {
                    reclaim_stats_.partial_into_open.fetch_add(
                        partial_refs, std::memory_order_relaxed);
                }
                const uint64_t new_epoch = next_cache_epoch_++;
                const uint64_t heat_seq  = next_heat_seq_++;
                space_->erase_cached_partial(page_base, rit->second.cache_epoch);
                rit->second.cache_epoch = new_epoch;
                space_->mark_cached_partial(cached_partial_update{
                    .page_base   = page_base,
                    .kind        = cached_partial_kind::cached_free_candidate,
                    .heat_seq    = heat_seq,
                    .cache_epoch = new_epoch,
                });
                return;
            }

            // Readonly cache hit — page is still cached as a full image.
            // Take it out of the readonly cache and install in
            // resident_partial_ so the next put can reuse the freed run
            // without a prefill read. Multi-LBA pages never enter the
            // readonly cache (D1), so span_lbas = 1 here is always
            // correct.
            auto cached = readonly_cache_.take(memory::frame_id{
                page_base, 1,
                memory::frame_id::domain::value_page,
            });
            if (!cached) {
                if (partial_refs != 0) {
                    reclaim_stats_.partial_into_untracked.fetch_add(
                        partial_refs, std::memory_order_relaxed);
                }
                return;  // not currently cached; manager-side
                         // partial node persists for future NRP
                         // selection under pressure mode.
            }
            if (partial_refs != 0) {
                reclaim_stats_.partial_into_cache.fetch_add(
                    partial_refs, std::memory_order_relaxed);
            }

            ensure_resident_partial_room_();
            const uint64_t epoch    = next_cache_epoch_++;
            const uint64_t heat_seq = next_heat_seq_++;
            (*cached)->st = memory::frame_state::clean_allocatable;
            resident_partial_.emplace(page_base, resident_partial_entry{
                .frame       = *cached,
                .span_lbas   = 1,
                .cache_epoch = epoch,
            });
            space_->mark_cached_partial(cached_partial_update{
                .page_base   = page_base,
                .kind        = cached_partial_kind::cached_free_candidate,
                .heat_seq    = heat_seq,
                .cache_epoch = epoch,
            });
        }

        void
        evict_resident_for_(paddr page_base) {
            if (auto rit = resident_partial_.find(page_base);
                rit != resident_partial_.end()) {
                space_->erase_cached_partial(page_base, rit->second.cache_epoch);
                destroy_frame(rit->second.frame);
                resident_partial_.erase(rit);
            }
            if (auto cached = readonly_cache_.take(memory::frame_id{
                    page_base, 1,
                    memory::frame_id::domain::value_page,
                })) {
                destroy_frame(*cached);
            }
        }

        // ── flush_deferred_releases_ ──
        //
        // Apply any reclaims that arrived while a page was inflight (and
        // thus was in dirty_round_pages_). After commit/rollback the page
        // is no longer dirty so those reclaims can take effect against the
        // settled metadata.

        void
        flush_deferred_releases_(const std::vector<paddr>& pages) {
            std::vector<value_ref> batch;
            for (const paddr& p : pages) {
                auto it = deferred_releases_.find(p);
                if (it == deferred_releases_.end()) continue;
                batch.insert(batch.end(), it->second.begin(), it->second.end());
                deferred_releases_.erase(it);
            }
            if (batch.empty()) return;
            bump_reclaim_immediate_stats_(batch);
            space_->release_values(
                std::span<const value_ref>(batch.data(), batch.size()));
            refresh_caches_after_release_(batch);
        }

        [[nodiscard]] bool
        reclaim_ref_is_whole_(const value_ref& vr) const noexcept {
            return vr.len >= lba_size_;
        }

        void
        bump_reclaim_dirty_stats_(std::span<const value_ref> refs) {
            uint64_t whole = 0;
            uint64_t partial = 0;
            for (const auto& vr : refs) {
                if (reclaim_ref_is_whole_(vr)) ++whole;
                else ++partial;
            }
            if (whole != 0) {
                reclaim_stats_.whole_into_dirty.fetch_add(
                    whole, std::memory_order_relaxed);
            }
            if (partial != 0) {
                reclaim_stats_.partial_into_dirty.fetch_add(
                    partial, std::memory_order_relaxed);
            }
        }

        void
        bump_reclaim_immediate_stats_(std::span<const value_ref> refs) {
            uint64_t whole = 0;
            for (const auto& vr : refs) {
                if (reclaim_ref_is_whole_(vr)) ++whole;
            }
            if (whole != 0) {
                reclaim_stats_.whole_clears_existing.fetch_add(
                    whole, std::memory_order_relaxed);
            }
        }

        [[nodiscard]] uint64_t
        partial_ref_count_for_page_(std::span<const value_ref> refs,
                                    paddr page_base) const noexcept {
            uint64_t count = 0;
            for (const auto& vr : refs) {
                if (vr.base == page_base && !reclaim_ref_is_whole_(vr)) {
                    ++count;
                }
            }
            return count;
        }

        // ════════════════════════════════════════════════════════════════
        //  handle_prepare_trim_batch / handle_complete_trim_batch
        // ════════════════════════════════════════════════════════════════

        // No structural cap: the manager itself enforces per-batch limits
        // via prepare_trim's max_ranges / max_lbas arguments. Here we pass
        // generous defaults; tighter pacing is the policy layer's call once
        // background TRIM cadence is finalized (037 plan §"TRIM").
        static constexpr uint32_t kDefaultTrimMaxRanges = 64;
        static constexpr uint32_t kDefaultTrimMaxLbas   = 1u << 20;

        void
        handle_prepare_trim_batch(_value_trim_prepare::req* item) {
            auto plan = space_->prepare_trim(
                kDefaultTrimMaxRanges, kDefaultTrimMaxLbas);
            if (plan.ranges.empty()) {
                item->cb(prepare_trim_result{trim_idle{}});
                delete item;
                return;
            }

            auto state = std::make_unique<trim_batch_state>();
            state->batch_id = next_trim_batch_id_++;
            state->plan_id  = plan.id;
            state->trims.reserve(plan.ranges.size());
            for (const auto& tr : plan.ranges) {
                state->trims.push_back(format::trim_desc{
                    .lba      = tr.lba,
                    .num_lbas = tr.len_lbas,
                });
            }

            const uint64_t batch_id = state->batch_id;
            auto trims_span = std::span<format::trim_desc>(state->trims);
            inflight_trim_batches_.emplace(batch_id, std::move(state));

            item->cb(prepare_trim_result{
                trim_batch{
                    .batch_id          = batch_id,
                    .trims             = trims_span,
                    .max_trim_inflight = io_policy().max_trim_inflight,
                }
            });
            delete item;
        }

        void
        handle_complete_trim_batch(_value_trim_complete::req* item) {
            auto it = inflight_trim_batches_.find(item->batch_id);
            if (it == inflight_trim_batches_.end()) {
                core::panic_inconsistency(
                    "value::value_alloc_sched::handle_complete_trim_batch",
                    "unknown trim batch_id %lu",
                    static_cast<unsigned long>(item->batch_id));
            }
            auto state = std::move(it->second);
            inflight_trim_batches_.erase(it);

            // Manager-side withhold/restore: ok=true releases the LBAs as
            // truly trimmed, ok=false returns them to global_free_extents
            // so a future prepare_trim can retry.
            space_->complete_trim(state->plan_id, item->ok);

            if (!item->ok) {
                item->fail(std::make_exception_ptr(
                    std::runtime_error("value::reclaim: NVMe trim failed")));
                delete item;
                return;
            }
            item->cb();
            delete item;
        }

        // ════════════════════════════════════════════════════════════════
        //  handle_read  —  prepare cache lookup, fan out NVMe miss
        // ════════════════════════════════════════════════════════════════

        void
        handle_read(_value_read::req* item) {
            auto ci_opt = class_for_body_len(item->vr.len);
            if (!ci_opt) {
                item->fail(std::make_exception_ptr(
                    std::runtime_error("value::read: body length exceeds all classes")));
                delete item;
                return;
            }
            const class_info& ci = class_table_[*ci_opt];
            const uint32_t span = ci.span_lbas;
            const bool admit = (span == 1);

            // 1. Inflight round frames (freshest source).
            if (auto* rfp = lookup_round_frame_(item->vr.base)) {
                serve_frame_hit_or_fail(item, *rfp, "round_frame");
                delete item;
                return;
            }

            // 2. Resident partial pages (post-commit, write-reuse).
            if (auto rit = resident_partial_.find(item->vr.base);
                rit != resident_partial_.end()) {
                serve_frame_hit_or_fail(
                    item, *rit->second.frame, "resident_partial");
                delete item;
                return;
            }

            // 3. Readonly cache (clean full pages, 1-LBA only).
            if (admit) {
                auto pin = readonly_cache_.pin(memory::frame_id{
                    item->vr.base, 1,
                    memory::frame_id::domain::value_page,
                });
                if (pin.frame) {
                    serve_frame_hit_or_fail(
                        item, *pin.frame, "readonly_cache");
                    delete item;
                    return;
                }
            }

            // 4. NVMe miss.
            auto frame = alloc_pooled_frame(
                item->vr.base, span, memory::frame_state::clean_readonly,
                /*zero_fill=*/false);
            item->cb(read_prepare_result{
                read_miss{item->vr.base, span, std::move(frame), admit}
            });
            delete item;
        }

        // ════════════════════════════════════════════════════════════════
        //  handle_fill  —  insert miss buf into cache + decode + return
        // ════════════════════════════════════════════════════════════════

        void
        handle_fill(_value_fill::req* item) {
            if (!item->frame) {
                item->fail(std::make_exception_ptr(
                    std::runtime_error("value::fill: missing frame")));
                delete item;
                return;
            }
            auto decoded = try_decode_value(*item->frame.get(), item->vr);
            if (decoded.first != format::value_decode_status::ok) {
                panic_decode_failure(item->vr, decoded.first, "post_nvme");
            }

            if (item->admit_to_cache) {
                auto* pf = item->frame.release();
                if (auto evicted = readonly_cache_.put(pf)) {
                    destroy_frame(*evicted);
                }
            }

            item->cb(std::move(decoded.second));
            delete item;
        }

        // ── helpers ──

        format::value_decode_result
        try_decode_value(std::span<const char> image, value_ref vr) const {
            auto ci_opt = class_for_body_len(vr.len);
            if (!ci_opt) {
                return format::value_decode_result{
                    .status = format::value_decode_status::bad_body_len,
                    .body   = {},
                };
            }
            const class_info& ci = class_table_[*ci_opt];
            return format::decode_value_object_slot(
                image, vr.byte_offset, ci.class_size, vr.len);
        }

        std::pair<format::value_decode_status, std::string>
        try_decode_value(const memory::segmented_page_frame& frame,
                         value_ref vr) const {
            auto ci_opt = class_for_body_len(vr.len);
            if (!ci_opt) {
                return {format::value_decode_status::bad_body_len, {}};
            }
            const class_info& ci = class_table_[*ci_opt];
            std::string body;
            body.resize(vr.len);
            auto status = format::decode_value_object_slot_to(
                frame, vr.byte_offset, ci.class_size, vr.len,
                std::span<char>(body.data(), body.size()));
            if (status != format::value_decode_status::ok) {
                body.clear();
            }
            return {status, std::move(body)};
        }

        void
        serve_hit_or_fail(_value_read::req* item,
                          std::span<const char> image,
                          const char* source_label) {
            auto decoded = try_decode_value(image, item->vr);
            if (decoded.status != format::value_decode_status::ok) {
                panic_decode_failure(item->vr, decoded.status, source_label);
            }
            item->cb(read_prepare_result{
                read_hit{ std::string(decoded.body.data(), decoded.body.size()) }
            });
        }

        void
        serve_frame_hit_or_fail(_value_read::req* item,
                                const memory::segmented_page_frame& frame,
                                const char* source_label) {
            auto decoded = try_decode_value(frame, item->vr);
            if (decoded.first != format::value_decode_status::ok) {
                panic_decode_failure(item->vr, decoded.first, source_label);
            }
            item->cb(read_prepare_result{
                read_hit{std::move(decoded.second)}
            });
        }

        [[noreturn]] static void
        panic_decode_failure(const value_ref& vr,
                             format::value_decode_status status,
                             const char* source_label) {
            core::panic_inconsistency("value::value_alloc_sched::decode",
                "corrupt value object source=%s dev=%u lba=%lu byte_offset=%u len=%u status=%s",
                source_label,
                static_cast<unsigned>(vr.base.device_id),
                static_cast<unsigned long>(vr.base.lba),
                static_cast<unsigned>(vr.byte_offset),
                static_cast<unsigned>(vr.len),
                value_decode_status_to_string(status));
        }

        static const char*
        value_decode_status_to_string(format::value_decode_status s) {
            switch (s) {
            case format::value_decode_status::ok:           return "ok";
            case format::value_decode_status::truncated:    return "truncated";
            case format::value_decode_status::bad_magic:    return "bad_magic";
            case format::value_decode_status::bad_body_len: return "bad_body_len";
            case format::value_decode_status::bad_crc:      return "bad_crc";
            }
            return "unknown";
        }

        std::optional<uint16_t>
        class_for_body_len(uint32_t body_len) const noexcept {
            const uint64_t total =
                static_cast<uint64_t>(body_len) + sizeof(format::value_object_header);
            for (uint16_t i = 0; i < class_table_.size(); ++i) {
                if (class_table_[i].class_size >= total) return i;
            }
            return std::nullopt;
        }

        // ── DMA frame allocation / destruction ──

        memory::pooled_frame_ptr<memory::segmented_page_frame>
        alloc_pooled_frame(paddr page_base,
                           uint32_t span_lbas,
                           memory::frame_state state,
                           bool zero_fill) {
            auto frame = frame_pool_.get_typed_frame<memory::segmented_page_frame>(
                memory::frame_id{
                    page_base,
                    static_cast<uint16_t>(span_lbas),
                    memory::frame_id::domain::value_page,
                },
                state,
                zero_fill);
            if (!frame) {
                throw std::runtime_error(
                    "value::value_alloc_sched::alloc_pooled_frame: DMA page allocation failed");
            }
            auto* raw = new memory::segmented_page_frame(std::move(*frame));
            return memory::pooled_frame_ptr<memory::segmented_page_frame>(
                &frame_pool_, raw);
        }

        memory::segmented_page_frame*
        alloc_dma_frame(paddr page_base, uint32_t span_lbas, bool zero_fill) {
            auto frame = alloc_pooled_frame(
                page_base, span_lbas, memory::frame_state::dirty_append,
                zero_fill);
            return frame.release();
        }

        void
        destroy_frame(memory::segmented_page_frame* frame) {
            if (!frame) return;
            frame_pool_.put_frame(std::move(*frame));
            delete frame;
        }

        uint16_t
        cached_starting_free_for_(paddr page_base) const noexcept {
            return space_->cached_partial_free_quantum_count(page_base);
        }

        // ── lookup_round_frame_ ──
        //
        // Walk inflight rounds and find any round_page whose page_base
        // matches. Ordering preference: rounds with page is the same set,
        // so we just take the first match. With the cap on rounds-per-
        // advance and per-round entries this stays O(1) amortized.

        memory::segmented_page_frame*
        lookup_round_frame_(paddr page_base) noexcept {
            auto it = dirty_round_pages_.find(page_base);
            if (it == dirty_round_pages_.end()) return nullptr;
            for (auto& [id, rnd] : inflight_rounds_) {
                for (auto& page : rnd->pages) {
                    if (page.page_base == page_base && page.frame != nullptr) {
                        return page.frame;
                    }
                }
            }
            return nullptr;
        }

        // ── ensure_resident_partial_room_ ──
        //
        // Soft cap on resident_partial_ pages; eviction picks the oldest
        // entry (by cache_epoch) and frees its DMA buffer + manager index
        // entry. Pin-tracking on the cache_concept already prevents
        // eviction-vs-pin races; here resident_partial_ frames are not
        // exposed via pin/take by anyone outside the scheduler, so we can
        // free directly.

        void
        ensure_resident_partial_room_() {
            if (resident_partial_.size() < resident_partial_budget_pages_) return;
            // Pick the smallest cache_epoch (oldest admission).
            paddr    victim{};
            uint64_t victim_epoch = UINT64_MAX;
            for (const auto& [pb, e] : resident_partial_) {
                if (e.cache_epoch < victim_epoch) {
                    victim_epoch = e.cache_epoch;
                    victim       = pb;
                }
            }
            auto vit = resident_partial_.find(victim);
            if (vit == resident_partial_.end()) return;
            space_->erase_cached_partial(victim, vit->second.cache_epoch);
            destroy_frame(vit->second.frame);
            resident_partial_.erase(vit);
        }

        void
        publish_value_head_() noexcept {
            if (shared_heads_ == nullptr) return;
            shared_heads_->value_head_lba.store(
                value_low_watermark_lba_, std::memory_order_relaxed);
        }

        // class_sizes_storage_ owns the per-class size bytes (initialised
        // in the ctor init list from the input span). Owning a copy means
        // the input span passed to the constructor does not have to
        // outlive the scheduler.
        absl::InlinedVector<uint32_t, 16> class_sizes_storage_;
    };

    template <>
    struct value_alloc_sched<core::clock_cache>
        : value_alloc_sched<core::segmented_clock_cache> {
        using base = value_alloc_sched<core::segmented_clock_cache>;

        value_alloc_sched(std::span<const uint32_t> class_sizes,
                          uint32_t                  lba_size,
                          paddr                     data_area_base,
                          paddr                     data_area_end,
                          core::data_area_heads*    shared_heads,
                          core::clock_cache         cache,
                          uint32_t                  value_space_quantum_bytes,
                          uint32_t                  value_space_group_size_lbas,
                          size_t                    queue_depth = 2048,
                          memory::dma_page_allocator frame_allocator =
                              memory::make_heap_dma_page_allocator(),
                          uint32_t                  frame_alignment = 4096,
                          int                       frame_numa_id = -1,
                          value_io_policy           io_policy = {})
            : base(class_sizes,
                   lba_size,
                   data_area_base,
                   data_area_end,
                   shared_heads,
                   core::segmented_clock_cache(cache.capacity()),
                   value_space_quantum_bytes,
                   value_space_group_size_lbas,
                   queue_depth,
                   std::move(frame_allocator),
                   frame_alignment,
                   frame_numa_id,
                   io_policy) {}
    };

    template <>
    struct value_alloc_sched<core::slru_cache>
        : value_alloc_sched<core::segmented_slru_cache> {
        using base = value_alloc_sched<core::segmented_slru_cache>;

        value_alloc_sched(std::span<const uint32_t> class_sizes,
                          uint32_t                  lba_size,
                          paddr                     data_area_base,
                          paddr                     data_area_end,
                          core::data_area_heads*    shared_heads,
                          core::slru_cache          cache,
                          uint32_t                  value_space_quantum_bytes,
                          uint32_t                  value_space_group_size_lbas,
                          size_t                    queue_depth = 2048,
                          memory::dma_page_allocator frame_allocator =
                              memory::make_heap_dma_page_allocator(),
                          uint32_t                  frame_alignment = 4096,
                          int                       frame_numa_id = -1,
                          value_io_policy           io_policy = {})
            : base(class_sizes,
                   lba_size,
                   data_area_base,
                   data_area_end,
                   shared_heads,
                   core::segmented_slru_cache(cache.capacity()),
                   value_space_quantum_bytes,
                   value_space_group_size_lbas,
                   queue_depth,
                   std::move(frame_allocator),
                   frame_alignment,
                   frame_numa_id,
                   io_policy) {}
    };

    // ── value_alloc_sched_base sender factory deferred definitions ─────────

    inline auto
    value_alloc_sched_base::prepare_persist(std::span<put_entry> entries) {
        return _value_persist::sender{.sched = this, .entries = entries};
    }

    inline auto
    value_alloc_sched_base::finalize_persist(uint64_t round_id, bool ok) {
        return _value_finalize::sender{.sched = this, .round_id = round_id, .ok = ok};
    }

    inline auto
    value_alloc_sched_base::continue_persist(uint64_t round_id, bool read_ok) {
        return _value_continue::sender{
            .sched    = this,
            .round_id = round_id,
            .read_ok  = read_ok,
        };
    }

    inline auto
    value_alloc_sched_base::prepare_read(value_ref vr) {
        return _value_read::sender{.sched = this, .vr = vr};
    }

    inline auto
    value_alloc_sched_base::fill_and_decode(value_ref vr,
                                            memory::pooled_frame_ptr<
                                                memory::segmented_page_frame> frame,
                                            bool admit_to_cache) {
        return _value_fill::sender{
            .sched          = this,
            .vr             = vr,
            .frame          = std::move(frame),
            .admit_to_cache = admit_to_cache,
        };
    }

    inline auto
    value_alloc_sched_base::reclaim_values(std::span<const value_ref> dead_values) {
        return _value_reclaim::sender{
            .sched       = this,
            .dead_values = dead_values,
        };
    }

    inline auto
    value_alloc_sched_base::prepare_trim_batch() {
        return _value_trim_prepare::sender{.sched = this};
    }

    inline auto
    value_alloc_sched_base::complete_trim_batch(uint64_t batch_id, bool ok) {
        return _value_trim_complete::sender{
            .sched    = this,
            .batch_id = batch_id,
            .ok       = ok,
        };
    }

    // ── op::start deferred definitions ──

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _value_persist::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_persist(new req{
            .entries = entries,
            .cb = [ctx = ctx, scope = scope](prepare_persist_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _value_finalize::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_finalize(new req{
            .round_id = round_id,
            .ok       = ok,
            .cb = [ctx = ctx, scope = scope]() mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _value_continue::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_continue(new req{
            .round_id = round_id,
            .read_ok  = read_ok,
            .cb = [ctx = ctx, scope = scope](persist_leader&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _value_read::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_read(new req{
            .vr = vr,
            .cb = [ctx = ctx, scope = scope](read_prepare_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _value_reclaim::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_reclaim(new req{
            .dead_values = std::vector<value_ref>(
                dead_values.begin(), dead_values.end()),
            .cb = [ctx = ctx, scope = scope]() mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _value_trim_prepare::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_trim_prepare(new req{
            .cb = [ctx = ctx, scope = scope](prepare_trim_result&& batch) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(batch));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _value_trim_complete::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_trim_complete(new req{
            .batch_id = batch_id,
            .ok       = ok,
            .cb = [ctx = ctx, scope = scope]() mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _value_fill::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_fill(new req{
            .vr             = vr,
            .frame          = std::move(frame),
            .admit_to_cache = admit_to_cache,
            .cb = [ctx = ctx, scope = scope](std::string&& s) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(s));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

}

// ── PUMP specializations ────────────────────────────────────────────────────

namespace pump::core {

    // value_persist
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::value_persist_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::value::_value_persist::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<apps::inconel::value::prepare_persist_result>{};
        }
    };

    // value_finalize → void (no value)
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::value_finalize_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::value::_value_finalize::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    // value_continue
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::value_continue_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::value::_value_continue::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<apps::inconel::value::persist_leader>{};
        }
    };

    // value_read
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::value_read_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::value::_value_read::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<apps::inconel::value::read_prepare_result>{};
        }
    };

    // value_fill
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::value_fill_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::value::_value_fill::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<std::string>{};
        }
    };

    // value_reclaim → void
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::value_reclaim_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::value::_value_reclaim::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    // value_trim_prepare
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::value_trim_prepare_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::value::_value_trim_prepare::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<apps::inconel::value::prepare_trim_result>{};
        }
    };

    // value_trim_complete → void
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::value_trim_complete_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::value::_value_trim_complete::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

}

#endif //APPS_INCONEL_VALUE_SCHEDULER_HH
