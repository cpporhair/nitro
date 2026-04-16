#ifndef APPS_INCONEL_VALUE_SCHEDULER_HH
#define APPS_INCONEL_VALUE_SCHEDULER_HH

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

#include "../core/data_area_heads.hh"
#include "../core/page_cache.hh"
#include "../core/panic.hh"
#include "../memory/frame.hh"
#include "../format/types.hh"
#include "../format/value_object.hh"
#include "./allocator.hh"

namespace apps::inconel::value {

    using format::paddr;
    using format::value_ref;
    using format::write_desc;

    // ── Public input/output types ──
    //
    // put_entry: caller-supplied entry. The body is borrowed (must outlive
    // the persist round). out_vr is filled in-place by the scheduler with
    // the durable location after the round commits successfully.

    struct put_entry {
        std::string_view body;
        value_ref*       out_vr;
    };

    // prepare_persist sender output. The variant lets the pipeline split
    // into a leader branch (NVMe writes + commit) and a follower branch
    // (already unblocked by leader's finalize) using visit() + if
    // constexpr — both branches can return different sender types this way.

    struct persist_leader {
        uint64_t              round_id;
        std::span<write_desc> writes;
    };

    // Followers piggy-back on the leader's NVMe write. handle_finalize
    // copies the leader's nvme_ok into every follower's `ok` field so the
    // caller can react to a failed round even when it didn't drive the
    // write itself. ok=false means: the leader's NVMe FUA write failed,
    // the round has been rolled back, and the out_vr filled in during
    // prepare_persist must be considered invalid.
    struct persist_follower {
        bool ok;
    };

    using prepare_persist_result = std::variant<persist_leader, persist_follower>;

    // read sender output (variant). hit branches return immediately with the
    // decoded body. miss branches hand the pipeline a buf + paddr; pipeline
    // is responsible for issuing nvme->read into buf, then calling
    // fill_and_decode(vr, buf) to insert into cache + decode + return.

    struct read_hit { std::string body; };

    // read miss → pipeline issues the NVMe read into rm.buf, then hands the
    // buffer back via fill_and_decode for cache admission + decode. The buf
    // is unique_ptr so ownership flows linearly: handle_read alloc → pipeline
    // hold (via context) → fill req → cache (release) or auto-free.
    //
    // admit_to_cache encodes decision D1 (only 1-LBA pages enter the cache):
    //   admit=true   1-LBA / sub-LBA → handle_fill releases buf into cache
    //   admit=false  multi-LBA       → handle_fill drops buf after decode
    // The flag is computed in handle_read and carried through the pipeline
    // because handle_fill no longer has class-size context to recompute it.

    struct read_miss {
        paddr                   base;
        uint32_t                span_lbas;
        std::unique_ptr<char[]> buf;
        uint32_t                buf_size;
        bool                    admit_to_cache;
    };

    using read_prepare_result = std::variant<read_hit, read_miss>;

    // page_data removed in step 019: replaced by value_page_frame-based
    // open_frames_ and allocatable_frames_ (see value_alloc_sched below).

    // ── Forward declarations of req types (used by sender layer) ──

    namespace _value_persist  { struct req; }
    namespace _value_finalize { struct req; }
    namespace _value_read     { struct req; }
    namespace _value_fill     { struct req; }

    struct value_alloc_sched_base;

    // ── PUMP op/sender wrappers ──
    //
    // Each operation gets a req (heap-allocated, deleted after cb), an op
    // with a tag bool, and a sender that builds an op_list. start() is
    // declared here and defined after `value_alloc_sched` is fully defined.

    namespace _value_persist {

        struct req {
            std::span<put_entry>                            entries;
            std::move_only_function<void(prepare_persist_result&&)> cb;
            std::move_only_function<void(std::exception_ptr)>       fail;
        };

        struct op {
            constexpr static bool value_persist_op = true;
            value_alloc_sched_base*      sched;
            std::span<put_entry> entries;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base*      sched;
            std::span<put_entry> entries;

            auto make_op() { return op{.sched = sched, .entries = entries}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    namespace _value_finalize {

        struct req {
            uint64_t                                  round_id;
            bool                                      ok;
            std::move_only_function<void()>           cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool value_finalize_op = true;
            value_alloc_sched_base* sched;
            uint64_t        round_id;
            bool            ok;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base* sched;
            uint64_t        round_id;
            bool            ok;

            auto make_op() { return op{.sched = sched, .round_id = round_id, .ok = ok}; }

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
            value_ref       vr;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base* sched;
            value_ref       vr;

            auto make_op() { return op{.sched = sched, .vr = vr}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    namespace _value_fill {

        struct req {
            value_ref                                          vr;
            std::unique_ptr<char[]>                            buf;
            uint32_t                                           buf_size;
            bool                                               admit_to_cache;
            std::move_only_function<void(std::string&&)>       cb;
            std::move_only_function<void(std::exception_ptr)>  fail;
        };

        struct op {
            constexpr static bool value_fill_op = true;
            value_alloc_sched_base*         sched;
            value_ref               vr;
            std::unique_ptr<char[]> buf;
            uint32_t                buf_size;
            bool                    admit_to_cache;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            value_alloc_sched_base*         sched;
            value_ref               vr;
            std::unique_ptr<char[]> buf;
            uint32_t                buf_size;
            bool                    admit_to_cache;

            auto make_op() {
                return op{
                    .sched          = sched,
                    .vr             = vr,
                    .buf            = std::move(buf),
                    .buf_size       = buf_size,
                    .admit_to_cache = admit_to_cache,
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    // ── value_alloc_sched_base ──
    //
    // Non-templated layer holding the four PUMP queues, the schedule_*
    // enqueue helpers, and the sender factory entry points. Senders/ops only
    // need this base — they never see the templated derived class — so the
    // PUMP pipeline machinery (op_pusher / compute_sender_type) doesn't have
    // to know what Cache the scheduler uses.
    //
    // The templated value_alloc_sched<Cache> publicly derives from this base;
    // pointer implicit upcasting lets the runtime/registry/sender layers all
    // work in terms of value_alloc_sched_base*.

    struct value_alloc_sched_base {
        pump::core::per_core::queue<_value_persist::req*>  persist_q_;
        pump::core::per_core::queue<_value_finalize::req*> finalize_q_;
        pump::core::per_core::queue<_value_read::req*>     read_q_;
        pump::core::per_core::queue<_value_fill::req*>     fill_q_;

        explicit
        value_alloc_sched_base(size_t queue_depth = 2048)
            : persist_q_(queue_depth)
            , finalize_q_(queue_depth)
            , read_q_(queue_depth)
            , fill_q_(queue_depth) {}

        // ── enqueue helpers (called by op::start) ──

        void schedule_persist (_value_persist::req*  r) { persist_q_ .try_enqueue(r); }
        void schedule_finalize(_value_finalize::req* r) { finalize_q_.try_enqueue(r); }
        void schedule_read    (_value_read::req*     r) { read_q_    .try_enqueue(r); }
        void schedule_fill    (_value_fill::req*     r) { fill_q_    .try_enqueue(r); }

        // ── Sender factories ──
        //
        // Declared here, defined inline at the bottom of this header (after
        // the sender struct types are complete). This is the same pattern
        // tree::tree_lookup_sched_base uses for process() / submit_cache().

        auto prepare_persist(std::span<put_entry> entries);
        auto finalize_persist(uint64_t round_id, bool ok);
        auto prepare_read(value_ref vr);
        auto fill_and_decode(value_ref vr, std::unique_ptr<char[]> buf,
                             uint32_t buf_size, bool admit_to_cache);
    };

    // ── value_alloc_sched<Cache> ──

    template <core::cache_concept Cache>
    struct value_alloc_sched : value_alloc_sched_base {
        // ── Per-round state (held in inflight_rounds_) ──
        //
        // Created by leader's prepare handle, consumed by finalize handle.
        // Followers wait inside this round's followers vec.

        struct round_page {
            memory::value_page_frame* frame;
            value_page_source         source;
            uint64_t                  original_free_mask; // for rollback
        };

        struct round {
            uint64_t                              id;
            std::vector<round_page>               pages;
            std::vector<write_desc>               writes;     // built from pages
            std::vector<_value_persist::req*>     followers;  // not the leader
        };

        value_allocator alloc_;

        // ── Resident frame state (step 019) ──
        //
        // open_frames_[ci]:  at most one per class — the active dirty
        //   frame being used by the current or most-recent round. It is
        //   produced by acquire_round_page (which installs the acquired
        //   frame here) and cleared by commit_pages / rollback_pages
        //   when the round finalizes.
        //
        //   Lifecycle within a single round:
        //     acquire_round_page  → dirty_append, installed here
        //     publish_round       → writeback_inflight, still here
        //     handle_read         → readable in any state (freshest source)
        //     commit / rollback   → cleared from here, frame moves to
        //                           allocatable_frames_ (partial) or
        //                           readonly_cache_ (full) or deleted
        //
        //   When acquire_round_page needs a new page but the current
        //   open frame is inflight or full, it calls displace_open_frame
        //   to clear the slot. An inflight displaced frame is owned by
        //   its round_page and will be settled on finalize; a
        //   clean_allocatable displaced frame goes to allocatable_frames_.
        //
        // allocatable_frames_[ci]:  clean_allocatable resident frames with
        //   free slots remaining. Populated by commit (partial pages) and
        //   rollback (writable source). LIFO pop (back) keeps the hottest
        //   page in front.
        //
        // Allocation priority:
        //   1. open_frames_[ci] — if usable (not inflight, has free slots)
        //   2. allocatable_frames_[ci] → reopen as dirty_append
        //   3. whole_pool / fresh_bump
        //
        // Read path priority (all resident states are safe to read):
        //   1. open_frames_[ci] (dirty / inflight)
        //   2. allocatable_frames_[ci] (clean_allocatable)
        //   3. readonly_cache_ (clean_readonly)
        //   4. NVMe miss
        //
        // Spec mapping: runtime_memory_and_cache.md §6.3 open_frames +
        // §8.6. value_page_source::writable names the resident source
        // for rollback purposes.
        std::vector<memory::value_page_frame*>              open_frames_;
        std::vector<std::vector<memory::value_page_frame*>> allocatable_frames_;

        // readonly cache: frame_id → page_frame* for 1-LBA pages. The
        // scheduler owns the page_frame descriptors and their backing
        // buffers; the cache is a non-owning index with pin semantics.
        // ~value_alloc_sched() drains via drain_one() and frees each
        // frame's buf + descriptor.
        //
        // Decision D1 — multi-LBA bypass: only span_lbas == 1 pages enter
        // here. commit_pages and handle_fill both gate on the span before
        // calling put(); handle_read symmetrically only consults the cache
        // when admit == (span == 1). Multi-LBA full pages are dropped at
        // commit time (frame buf + descriptor deleted directly).
        //
        // Lifetime contract for put() / drain_one() (full statement in
        // core/page_cache.hh's cache_concept doc): every "Some(...)" return
        // is a page_frame* the caller must free (buf + descriptor). Both
        // put-on-existing-key (overwrite) and put-when-cap-full (eviction)
        // use the same channel.
        Cache readonly_cache_;

        // leader-follower in-flight tracking. round_id is a monotonically
        // increasing key only ever queried point-wise (find/erase by id);
        // there is no ordered iteration, so a hash map is preferred over the
        // RB-tree std::map originally used.
        absl::flat_hash_map<uint64_t, std::unique_ptr<round>> inflight_rounds_;
        uint64_t                                              next_round_id_ = 1;

        value_alloc_sched(std::span<const uint32_t> class_sizes,
                          uint32_t                  lba_size,
                          paddr                     data_area_base,
                          paddr                     data_area_end,
                          core::data_area_heads*    shared_heads,
                          Cache                     cache,
                          size_t                    queue_depth = 2048)
            : value_alloc_sched_base(queue_depth)
            , alloc_(class_sizes, lba_size, data_area_base, data_area_end,
                     shared_heads)
            , open_frames_(class_sizes.size(), nullptr)
            , allocatable_frames_(class_sizes.size())
            , readonly_cache_(std::move(cache))
            , class_sizes_storage_(class_sizes.begin(), class_sizes.end())
        {
        }

        // ── Destructor ──
        //
        // open_frames_ and inflight round_pages may share frame pointers
        // (the round_page references the same frame that sits in
        // open_frames_). Free in-flight frames first, skipping any that
        // are shared with open_frames_ (those are freed in the
        // open_frames_ loop). Then free open, allocatable, and cache.

        ~value_alloc_sched() {
            for (auto& [id, rnd] : inflight_rounds_) {
                for (auto& page : rnd->pages) {
                    if (!page.frame) continue;
                    uint16_t ci = page.frame->class_idx;
                    if (ci < open_frames_.size() &&
                        open_frames_[ci] == page.frame) continue;
                    delete[] page.frame->buf;
                    delete page.frame;
                }
            }
            for (auto* f : open_frames_) {
                if (f) { delete[] f->buf; delete f; }
            }
            for (auto& list : allocatable_frames_) {
                for (auto* f : list) { delete[] f->buf; delete f; }
            }
            while (auto f = readonly_cache_.drain_one()) {
                delete[] (*f)->buf;
                delete *f;
            }
        }

        // ── advance ──
        //
        // Order matters: finalize first (releases inflight rounds and may
        // install partial pages into open_frames_ / allocatable_frames_,
        // making them available for subsequent persist rounds). persist
        // next (consumes the freshly available pages). read/fill last.
        //
        // INC-029 — bounded per-advance work budget. Each of the four queues
        // gets its own per-advance cap so a hot stream on one queue can't
        // monopolise this scheduler's CPU and starve the other three (or
        // other co-resident schedulers). Constants are private on purpose:
        // these are workload-shaping knobs, not runtime/build configuration
        // surfaces. Persist is counted in *leader rounds* — handle_persist
        // is allowed to absorb up to kMaxFollowersPerRound followers
        // internally, so a single round still represents up to 1 + cap
        // entries of real work; that is why its per-advance budget is
        // smaller than the other three.

        static constexpr uint32_t kMaxFinalizePerAdvance      = 64;
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

            // Persist budget is leader rounds: each handle_persist call still
            // internally absorbs up to kMaxFollowersPerRound followers from
            // persist_q_ via collect_round_items, but the outer advance()
            // only counts that as one round of work. Anything beyond the
            // round-budget stays queued and becomes its own leader on the
            // next advance() invocation.
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

            return progress;
        }

        template<typename runtime_t>
        bool advance(runtime_t&) { return advance(); }

    private:
        // ════════════════════════════════════════════════════════════════
        //  handle_persist  —  leader-follower round assembly
        // ════════════════════════════════════════════════════════════════
        //
        // Round build is split into four explicit phases (no goto, no
        // exception_ptr smuggling):
        //
        //   1. collect_round_items(leader)        — drain followers up to cap
        //   2. build_round(round, items)          — encode every entry
        //   3. finalize_round_writes(round)       — build write_desc list
        //   4. publish_round(round, items, ldr)   — install + fire leader cb
        //
        // build_round returns an explicit `persist_entry_status` so the
        // caller can route each failure mode without inspecting the round
        // state. Only `value_too_large` is recoverable (caller sent a body
        // that doesn't fit any size class — pure caller-driven input).
        // `out_of_space` and `encode_failure` are invariant breaks: v1 has
        // no value reclaim, so an empty bump head means the runtime is in a
        // state we can no longer reason about; an encode failure after a
        // class has been picked means encode_value_object disagreed with
        // find_min_class, which is an internal logic bug. Both panic
        // immediately rather than masquerading as a recoverable exception.

        enum class persist_entry_status : uint8_t {
            ok = 0,
            value_too_large,   // recoverable — caller body exceeds all classes
            out_of_space,      // fatal — no Data Area left, no reclaim path
            encode_failure,    // fatal — encode disagreed with find_min_class
        };

        // INC-028 — bound the per-round work. Without a cap, a single
        // handle_persist invocation could drain the entire persist_q_ as
        // followers, producing arbitrarily large rounds and unbounded tail
        // latency. The cap is a private constant on purpose: this is a
        // workload-shaping knob, not a runtime configuration surface, and
        // promoting it to the start_options struct would expand the public
        // API for an internal tuning parameter that hasn't yet earned a
        // public name. Lift it later if benchmarks demand a different
        // value, but do not feature-flag it now.
        static constexpr uint32_t kMaxFollowersPerRound = 64;

        void
        handle_persist(_value_persist::req* leader_item) {
            auto items = collect_round_items(leader_item);

            auto rnd = std::make_unique<round>();
            rnd->id = next_round_id_++;

            auto status = build_round(*rnd, items);
            if (status == persist_entry_status::value_too_large) {
                // Recoverable: caller's body doesn't fit any class. Roll
                // back every page touched this round (reverse-order so
                // fresh_bump pages can be returned to the device head),
                // then fail every item in this round — leader and
                // followers alike — with a single shared exception_ptr.
                rollback_pages(*rnd);
                auto failure = std::make_exception_ptr(std::runtime_error(
                    "value::persist: body length exceeds all size classes"));
                for (auto* item : items) {
                    item->fail(failure);
                    delete item;
                }
                return;
            }
            if (status == persist_entry_status::out_of_space) {
                core::panic_inconsistency("value::value_alloc_sched::handle_persist",
                    "value Data Area exhausted; v1 has no reclaim path");
            }
            if (status == persist_entry_status::encode_failure) {
                core::panic_inconsistency("value::value_alloc_sched::handle_persist",
                    "encode_value_object failed after class selection — internal logic break");
            }

            // status == ok
            finalize_round_writes(*rnd);
            publish_round(std::move(rnd), items, leader_item);
        }

        // ── collect_round_items ──
        //
        // Drain at most kMaxFollowersPerRound followers from persist_q_ on
        // top of the leader. Anything beyond the cap stays in the queue and
        // will be picked up by the next advance() invocation as the leader
        // of its own round.

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

        // ── build_round ──
        //
        // Walk every entry across every item, encoding value objects into
        // round pages. Stops on the first non-ok status; the caller is
        // responsible for rollback.

        persist_entry_status
        build_round(round& rnd, std::span<_value_persist::req* const> items) {
            for (auto* item : items) {
                for (auto& entry : item->entries) {
                    auto status = append_entry_to_round(rnd, entry);
                    if (status != persist_entry_status::ok) return status;
                }
            }
            return persist_entry_status::ok;
        }

        // ── finalize_round_writes ──
        //
        // Build the write_desc vector over the encoded pages. Must run
        // after build_round has settled the round_page list because
        // write_desc holds raw pointers into each frame's buf; we relied
        // on that contract before by deferring the descriptor build to
        // the end of handle_persist, but it deserves its own named step
        // now that the control flow is no longer linear.

        void
        finalize_round_writes(round& rnd) {
            rnd.writes.reserve(rnd.pages.size());
            for (auto& page : rnd.pages) {
                rnd.writes.push_back(write_desc{
                    .lba      = page.frame->id.base.lba,
                    .data     = page.frame->buf,
                    .num_lbas = page.frame->id.span_lbas,
                    .flags    = 0,
                });
            }
        }

        // ── publish_round ──
        //
        // Freeze all round pages (dirty → writeback_inflight) so the buf
        // is safe for NVMe DMA, install the round into inflight_rounds_,
        // fire the leader's callback with the write list, and stash the
        // followers on the round so handle_finalize can wake them when the
        // FUA write completes. Followers do not include the leader itself.

        void
        publish_round(std::unique_ptr<round> rnd,
                      std::span<_value_persist::req* const> items,
                      _value_persist::req* leader_item) {
            // Freeze: mark all round pages writeback_inflight so the
            // next acquire_round_page knows not to write into them.
            for (auto& page : rnd->pages) {
                page.frame->st   = memory::frame_state::writeback_inflight;
                page.frame->mode = memory::value_page_frame::open_mode::none;
            }

            uint64_t rid = rnd->id;
            std::span<write_desc> writes_span(
                rnd->writes.data(), rnd->writes.size());

            for (size_t i = 1; i < items.size(); ++i)
                rnd->followers.push_back(items[i]);

            inflight_rounds_.emplace(rid, std::move(rnd));

            leader_item->cb(prepare_persist_result{
                persist_leader{rid, writes_span}
            });
            delete leader_item;
        }

        // ── append_entry_to_round ──
        //
        // Allocate a slot for one entry and encode the value object into
        // the page image. Reuses an existing round_page when one of the
        // same class still has a free slot; otherwise pulls a fresh page
        // via acquire_round_page (open_frames → allocatable_frames →
        // whole_pool → fresh_bump).

        persist_entry_status
        append_entry_to_round(round& rnd, put_entry& entry) {
            uint32_t total = sizeof(format::value_object_header) + entry.body.size();
            auto ci_opt = format::find_min_class(total, class_sizes_span());
            if (!ci_opt) return persist_entry_status::value_too_large;
            uint16_t ci = *ci_opt;

            round_page* page = find_round_page_with_room(rnd, ci);
            if (!page) {
                page = acquire_round_page(rnd, ci);
                if (!page) return persist_entry_status::out_of_space;
            }

            // Pick the lowest set bit (next free slot).
            uint32_t slot = static_cast<uint32_t>(__builtin_ctzll(page->frame->free_mask));
            page->frame->free_mask &= ~(1ULL << slot);
            page->frame->free_count--;

            uint32_t cs = alloc_.class_size(ci);
            uint16_t off = 0;
            if (alloc_.is_sub_lba(ci)) {
                off = static_cast<uint16_t>(slot * cs);
            }

            // Encode value object into the slot. A failure here means
            // encode_value_object disagreed with find_min_class about
            // whether the body fits — that is an internal logic break, not
            // a caller-driven recoverable error, so we surface it via the
            // fatal status code instead of "restore the bit and pretend".
            std::span<char> slot_span(page->frame->buf + off, cs);
            std::span<const char> body_span(entry.body.data(), entry.body.size());
            if (!format::encode_value_object(slot_span, body_span)) {
                return persist_entry_status::encode_failure;
            }

            // Fill the caller's value_ref.
            entry.out_vr->base        = page->frame->id.base;
            entry.out_vr->byte_offset = off;
            entry.out_vr->len         = static_cast<uint32_t>(entry.body.size());
            entry.out_vr->flags       = 0;
            return persist_entry_status::ok;
        }

        round_page*
        find_round_page_with_room(round& rnd, uint16_t ci) {
            // search backwards: most-recently-added first (better cache
            // locality + faster termination on the common "still filling
            // the latest page" case)
            for (auto it = rnd.pages.rbegin(); it != rnd.pages.rend(); ++it) {
                if (it->frame->class_idx == ci && it->frame->free_mask != 0)
                    return &*it;
            }
            return nullptr;
        }

        // ── displace_open_frame ──
        //
        // Move the current open frame for class ci out of open_frames_[ci]
        // so a replacement can be installed.
        //
        // Only clean_allocatable frames go to allocatable_frames_ here.
        // Dirty and writeback_inflight frames are owned by a round_page in
        // the current or in-flight round; commit_pages / rollback_pages
        // will settle them. Pushing them here would create a double
        // reference (displace + round cleanup → double free).

        void displace_open_frame(uint16_t ci) {
            auto* old = open_frames_[ci];
            if (!old) return;
            open_frames_[ci] = nullptr;
            if (old->st == memory::frame_state::clean_allocatable) {
                allocatable_frames_[ci].push_back(old);
            }
        }

        round_page*
        acquire_round_page(round& rnd, uint16_t ci) {
            // Priority 1: open_frames_[ci] — reuse if not inflight and has
            // free slots. The frame stays in open_frames_[ci] (shared
            // reference with round_page) so handle_read can hit it.
            if (open_frames_[ci]) {
                auto* frame = open_frames_[ci];
                if (frame->st != memory::frame_state::writeback_inflight &&
                    frame->free_mask != 0) {
                    if (frame->st == memory::frame_state::clean_allocatable) {
                        frame->st   = memory::frame_state::dirty_append;
                        frame->mode = memory::value_page_frame::open_mode::append;
                    }
                    rnd.pages.push_back(round_page{
                        .frame              = frame,
                        .source             = value_page_source::writable,
                        .original_free_mask = frame->free_mask,
                    });
                    return &rnd.pages.back();
                }
                // Current open frame is inflight or full — fall through
                // to acquire a replacement. The old frame remains owned by
                // its round_page; finalize will settle it as clean_readonly
                // (1-LBA full), drop it (multi-LBA full), or roll it back.
            }

            // Priority 2: allocatable_frames_[ci] — clean_allocatable
            // resident frame, reopen as dirty_append for continuation.
            if (!allocatable_frames_[ci].empty()) {
                auto* frame = allocatable_frames_[ci].back();
                allocatable_frames_[ci].pop_back();
                frame->st   = memory::frame_state::dirty_append;
                frame->mode = memory::value_page_frame::open_mode::append;
                displace_open_frame(ci);
                open_frames_[ci] = frame;
                rnd.pages.push_back(round_page{
                    .frame              = frame,
                    .source             = value_page_source::writable,
                    .original_free_mask = frame->free_mask,
                });
                return &rnd.pages.back();
            }

            // Priority 3: allocator (whole_pool / fresh bump).
            auto ar = alloc_.acquire_page(ci);
            if (!ar) return nullptr;

            uint32_t img_bytes = ar->span_lbas * alloc_.lba_size();
            auto* frame = new memory::value_page_frame{};
            frame->id = memory::frame_id{
                ar->page_base,
                static_cast<uint16_t>(ar->span_lbas),
                memory::frame_id::domain::value_page,
            };
            frame->st         = memory::frame_state::dirty_append;
            frame->buf        = new char[img_bytes]();
            frame->byte_len   = img_bytes;
            frame->pin_count  = 0;
            frame->crc_valid  = false;
            frame->class_idx      = ci;
            frame->slots_per_page = static_cast<uint16_t>(alloc_.slots_per_page(ci));
            frame->free_mask      = ar->free_mask;
            frame->free_count     = static_cast<uint16_t>(
                __builtin_popcountll(ar->free_mask));
            frame->mode           = memory::value_page_frame::open_mode::append;

            displace_open_frame(ci);
            open_frames_[ci] = frame;
            rnd.pages.push_back(round_page{
                .frame              = frame,
                .source             = ar->source,
                .original_free_mask = ar->free_mask,
            });
            return &rnd.pages.back();
        }

        // ════════════════════════════════════════════════════════════════
        //  handle_finalize  —  commit or abort an in-flight round
        // ════════════════════════════════════════════════════════════════

        void
        handle_finalize(_value_finalize::req* item) {
            auto it = inflight_rounds_.find(item->round_id);
            if (it == inflight_rounds_.end()) {
                // Invariant broken: every successful prepare yields exactly
                // one finalize, so an unknown round_id means we no longer
                // know what state the scheduler is in. Continuing would just
                // propagate the corruption — abort hard rather than try to
                // surface it as a recoverable exception.
                core::panic_inconsistency("value::value_alloc_sched::handle_finalize",
                    "unknown round_id %lu",
                    static_cast<unsigned long>(item->round_id));
            }

            auto rnd = std::move(it->second);
            inflight_rounds_.erase(it);

            // finalize is the local commit/abort step of the round state
            // machine. Both nvme-ok and nvme-fail are *normal* inputs and
            // both paths return void; the upstream pipeline carries the
            // nvme_ok bool itself if anyone cares about the outcome.
            if (item->ok) {
                commit_pages(*rnd);
            } else {
                rollback_pages(*rnd);
            }

            item->cb();
            // Propagate the leader's nvme_ok to every follower in this
            // round. They share a single FUA write, so they share its
            // success/failure verdict. ok=false followers must NOT trust
            // their out_vr — rollback_pages above has already invalidated
            // the page state those vrs point at.
            for (auto* f : rnd->followers) {
                f->cb(prepare_persist_result{persist_follower{item->ok}});
                delete f;
            }

            delete item;
        }

        void
        commit_pages(round& rnd) {
            for (auto& page : rnd.pages) {
                auto* frame = page.frame;
                uint16_t ci = frame->class_idx;
                // Clear open_frames_ reference: the round is done, this
                // frame is no longer the active dirty target. A newer
                // round may have already displaced us (open != frame);
                // only clear if we're still the current occupant.
                if (open_frames_[ci] == frame) open_frames_[ci] = nullptr;

                if (frame->free_mask == 0) {
                    // Full page. Transition: writeback_inflight → clean_readonly.
                    // Decision D1: only 1-LBA pages enter readonly_cache_.
                    frame->free_count = 0;
                    if (frame->id.span_lbas == 1) {
                        // Transfer buf to a plain page_frame for the cache
                        // (avoids polymorphic-delete UB since page_frame
                        // has no virtual destructor).
                        auto* pf = new memory::page_frame{
                            .id        = frame->id,
                            .st        = memory::frame_state::clean_readonly,
                            .buf       = frame->buf,
                            .byte_len  = frame->byte_len,
                            .pin_count = 0,
                            .crc_valid = false,
                        };
                        frame->buf = nullptr;
                        delete frame;
                        page.frame = nullptr;
                        if (auto evicted = readonly_cache_.put(pf)) {
                            delete[] (*evicted)->buf;
                            delete *evicted;
                        }
                    } else {
                        // multi-LBA full page → drop (no cache value)
                        delete[] frame->buf;
                        delete frame;
                        page.frame = nullptr;
                    }
                } else {
                    // Partial page: writeback_inflight → clean_allocatable.
                    // Enters allocatable_frames_[ci], NOT readonly_cache_.
                    frame->st         = memory::frame_state::clean_allocatable;
                    frame->mode       = memory::value_page_frame::open_mode::none;
                    frame->free_count = static_cast<uint16_t>(
                        __builtin_popcountll(frame->free_mask));
                    allocatable_frames_[ci].push_back(frame);
                    page.frame = nullptr;
                }
            }
        }

        // ── rollback_pages ──
        //
        // Undo every page acquisition the current round made, in reverse
        // order. Reverse order is required so that fresh_bump pages can be
        // returned to the device head — `value_allocator::push_back_bump`
        // only accepts the page that currently sits at bump_head, which is
        // the most recently bumped page, i.e. the last one in rnd.pages.
        // Walking forward would leak every fresh_bump page that isn't the
        // very first acquisition.
        //
        //   - fresh_bump  → push_back_bump (returns LBAs to bump head)
        //   - whole_page  → recycle_whole_page (back into the class pool)
        //   - writable    → clear matching open_frames_ reference, restore
        //                   original free_mask, set clean_allocatable, and
        //                   return it to allocatable_frames_

        void
        rollback_pages(round& rnd) {
            for (auto it = rnd.pages.rbegin(); it != rnd.pages.rend(); ++it) {
                auto& page = *it;
                auto* frame = page.frame;
                uint16_t ci = frame->class_idx;
                if (open_frames_[ci] == frame) open_frames_[ci] = nullptr;
                switch (page.source) {
                case value_page_source::fresh_bump:
                    alloc_.push_back_bump(frame->id.base, frame->id.span_lbas);
                    delete[] frame->buf;
                    delete frame;
                    page.frame = nullptr;
                    break;
                case value_page_source::whole_page:
                    alloc_.recycle_whole_page(ci, frame->id.base);
                    delete[] frame->buf;
                    delete frame;
                    page.frame = nullptr;
                    break;
                case value_page_source::writable:
                    // Resident source: restore free_mask and transition to
                    // clean_allocatable → allocatable_frames_.
                    frame->free_mask  = page.original_free_mask;
                    frame->free_count = static_cast<uint16_t>(
                        __builtin_popcountll(page.original_free_mask));
                    frame->st   = memory::frame_state::clean_allocatable;
                    frame->mode = memory::value_page_frame::open_mode::none;
                    allocatable_frames_[ci].push_back(frame);
                    page.frame = nullptr;
                    break;
                }
            }
        }

        // install_writable_page removed in step 019: commit_pages and
        // rollback_pages now populate allocatable_frames_ directly.

        // ════════════════════════════════════════════════════════════════
        //  handle_read  —  prepare cache lookup, fan out NVMe miss
        // ════════════════════════════════════════════════════════════════

        void
        handle_read(_value_read::req* item) {
            // Determine the class first; we need span_lbas to allocate the
            // miss buffer and to decide cache admission.
            auto ci_opt = class_for_len(item->vr.len);
            if (!ci_opt) {
                item->fail(std::make_exception_ptr(
                    std::runtime_error("value::read: body length exceeds all classes")));
                delete item;
                return;
            }
            uint16_t ci   = *ci_opt;
            uint32_t span = alloc_.span_lbas(ci);

            // Decision D1: only 1-LBA pages (sub-LBA + LBA-equal classes)
            // are cache-admissible. Multi-LBA pages bypass the cache
            // entirely — their hit rate is too low to justify burning
            // capacity, and the buf pool stays single-sized.
            const bool admit = (span == 1);

            // 1. open_frames_[ci] — the active frame for this class
            //    (dirty, inflight, or clean_allocatable). In-memory
            //    image is the freshest resident source and is safe to
            //    read in any of these states.
            if (open_frames_[ci] &&
                open_frames_[ci]->id.base == item->vr.base) {
                serve_hit_or_fail(item,
                    std::span<const char>(open_frames_[ci]->buf,
                                          open_frames_[ci]->byte_len),
                    "open_frames");
                delete item;
                return;
            }

            // 2. allocatable_frames_[ci] — clean_allocatable resident
            //    frames with free slots. Linear scan: N is small (typically
            //    0-2 per class).
            for (auto* f : allocatable_frames_[ci]) {
                if (f->id.base == item->vr.base) {
                    serve_hit_or_fail(item,
                        std::span<const char>(f->buf, f->byte_len),
                        "allocatable_frames");
                    delete item;
                    return;
                }
            }

            // 3. readonly_cache_ — only consulted when this class is
            //    admissible (multi-LBA bypasses). Pin semantics: pin()
            //    returns an RAII frame_pin that keeps the page resident
            //    for the duration of the decode.
            if (admit) {
                auto pin = readonly_cache_.pin(
                    memory::frame_id{item->vr.base, 1,
                        memory::frame_id::domain::value_page});
                if (pin.frame) {
                    serve_hit_or_fail(item,
                        std::span<const char>(pin.frame->buf,
                                              pin.frame->byte_len),
                        "readonly_cache");
                    delete item;
                    return;
                }
            }

            // 4. NVMe miss → tell pipeline to issue NVMe read into a
            //    fresh buf. The unique_ptr flows through the pipeline and
            //    ends up in handle_fill which either releases it into the
            //    cache (admit) or drops it after decode (bypass).
            uint32_t img_bytes = span * alloc_.lba_size();
            auto buf = std::make_unique<char[]>(img_bytes);
            item->cb(read_prepare_result{
                read_miss{item->vr.base, span, std::move(buf), img_bytes, admit}
            });
            delete item;
        }

        // ════════════════════════════════════════════════════════════════
        //  handle_fill  —  insert miss buf into cache + decode + return
        // ════════════════════════════════════════════════════════════════

        void
        handle_fill(_value_fill::req* item) {
            // CRITICAL: decode against the staging buffer FIRST, before
            // touching readonly_cache_. If the on-disk bytes are corrupt
            // (bad magic / body_len / crc / truncated) the on-disk value
            // area is wrong and there is no semantically sound recovery —
            // every subsequent reader of this paddr would be served the
            // same poisoned page. Abort hard with the offending value_ref
            // and decode reason rather than collapse it into an exception.
            auto decoded = try_decode_value(
                std::span<const char>(item->buf.get(), item->buf_size),
                item->vr);
            if (decoded.status != format::value_decode_status::ok) {
                panic_decode_failure(item->vr, decoded.status, "post_nvme");
            }

            // Verified — admit policy was decided in handle_read and rides
            // along on the fill req:
            //   admit  → release the buf into the cache; the cache becomes
            //            sole owner. If the put evicts another entry, that
            //            buf is freed here.
            //   bypass → unique_ptr stays in item, item->~req() frees it.
            if (item->admit_to_cache) {
                auto* pf = new memory::page_frame{
                    .id        = memory::frame_id{item->vr.base, 1,
                                    memory::frame_id::domain::value_page},
                    .st        = memory::frame_state::clean_readonly,
                    .buf       = item->buf.release(),
                    .byte_len  = item->buf_size,
                    .pin_count = 0,
                    .crc_valid = false,
                };
                if (auto evicted = readonly_cache_.put(pf)) {
                    delete[] (*evicted)->buf;
                    delete *evicted;
                }
            }

            item->cb(std::string(decoded.body.data(), decoded.body.size()));
            delete item;
        }

        // ── helpers ──

        // Decode a value object from an in-memory page image at vr's slot.
        //
        // Returns the full format::value_decode_result so callers see the
        // exact failure mode (truncated / bad_magic / bad_body_len /
        // bad_crc) — every Inconel value-read path treats anything other
        // than ok as on-disk corruption and panics, so we never collapse
        // the reason here. Empty body is a legitimate value, distinct
        // from any error.
        //
        // span<const char> is the uniform decode path: open_frames_,
        // allocatable_frames_, readonly_cache_, and the fill staging
        // buffer all reach this through a span construction at the call
        // site, so the helper is agnostic to who owns the bytes.
        format::value_decode_result
        try_decode_value(std::span<const char> image, value_ref vr) const {
            if (vr.byte_offset >= image.size()) {
                return format::value_decode_result{
                    .status = format::value_decode_status::truncated,
                    .body   = {},
                };
            }
            auto slot = image.subspan(vr.byte_offset);
            return format::decode_value_object(slot, vr.len);
        }

        // Serve a read req from an in-memory page image. Decode failures
        // here mean the value area itself is corrupt (the slot we landed
        // on has the wrong bytes), so we panic rather than surface a
        // recoverable exception — see handle_fill for the same reasoning.
        // Caller is responsible for `delete item` and `return` after this.
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
        class_for_len(uint32_t body_len) const noexcept {
            uint32_t total = sizeof(format::value_object_header) + body_len;
            return format::find_min_class(total, class_sizes_span());
        }

        // Construct an ephemeral span over class_sizes_storage_ on demand.
        // The previous version cached this view in a member field
        // (`class_sizes_view_`); that field added zero value because
        // building a span over an InlinedVector is a 16-byte register-pair
        // operation that the compiler reduces to nothing on every
        // optimisation level we ship. Removing the field also removes a
        // post-construction init step that lived in the ctor body —
        // class_sizes_storage_ alone is now the single source of truth.
        std::span<const uint32_t>
        class_sizes_span() const noexcept {
            return std::span<const uint32_t>(
                class_sizes_storage_.data(), class_sizes_storage_.size());
        }

        // class_sizes_storage_ owns the per-class size bytes (initialised
        // in the ctor init list from the input span). Owning a copy means
        // the input span passed to the constructor does not have to
        // outlive the scheduler. The 16 inline slots match the on-disk
        // value_size_classes upper bound (superblock §2 in
        // on_disk_formats.md), so the small-resident metadata stays inline
        // and avoids a heap allocation per scheduler.
        absl::InlinedVector<uint32_t, 16> class_sizes_storage_;
    };

    // ── value_alloc_sched_base sender factory deferred definitions ──
    //
    // Defined out-of-line so the sender struct types they return are visible
    // by this point. Same pattern as tree::tree_lookup_sched_base::process().

    inline auto
    value_alloc_sched_base::prepare_persist(std::span<put_entry> entries) {
        return _value_persist::sender{.sched = this, .entries = entries};
    }

    inline auto
    value_alloc_sched_base::finalize_persist(uint64_t round_id, bool ok) {
        return _value_finalize::sender{.sched = this, .round_id = round_id, .ok = ok};
    }

    inline auto
    value_alloc_sched_base::prepare_read(value_ref vr) {
        return _value_read::sender{.sched = this, .vr = vr};
    }

    inline auto
    value_alloc_sched_base::fill_and_decode(value_ref vr,
                                            std::unique_ptr<char[]> buf,
                                            uint32_t buf_size,
                                            bool admit_to_cache) {
        return _value_fill::sender{
            .sched          = this,
            .vr             = vr,
            .buf            = std::move(buf),
            .buf_size       = buf_size,
            .admit_to_cache = admit_to_cache,
        };
    }

    // ── op::start deferred definitions ──
    //
    // Now that scheduler is fully defined, we can implement each op's
    // start() method which constructs a req and enqueues it on the
    // scheduler's queue.

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
    void _value_fill::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_fill(new req{
            .vr             = vr,
            .buf            = std::move(buf),
            .buf_size       = buf_size,
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

// ── PUMP specializations ──

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

}

#endif //APPS_INCONEL_VALUE_SCHEDULER_HH
