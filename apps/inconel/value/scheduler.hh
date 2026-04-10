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

    // ── Internal page_data ──
    //
    // A page held by the scheduler in writable state — i.e. its NVMe image is
    // already durable but it still has free slots, so the next persist round
    // for the same class will reuse this page in-memory rather than allocate
    // a fresh one. v7 keeps a flat per-class queue (writable_pages_[ci]) and
    // pops the most recently installed page first (LIFO) so the hottest
    // partial page is always picked up next.
    //
    // image is unique_ptr<char[]> rather than vector<char> so the same buffer
    // representation flows through writable_pages_, the cache, and (later) a
    // DMA pool — there is no vector→buffer→vector copy at any boundary.
    // image_size is required because char[] does not carry its own length.

    struct page_data {
        paddr                   base;
        uint64_t                free_mask;
        std::unique_ptr<char[]> image;
        uint32_t                image_size;
    };

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
            paddr                   base;
            uint16_t                class_idx;
            uint32_t                span_lbas;
            value_page_source       source;
            uint64_t                original_free_mask; // for rollback
            uint64_t                free_mask;          // updated as slots are filled
            std::unique_ptr<char[]> image;
            uint32_t                image_size;
        };

        struct round {
            uint64_t                              id;
            std::vector<round_page>               pages;
            std::vector<write_desc>               writes;     // built from pages
            std::vector<_value_persist::req*>     followers;  // not the leader
        };

        value_allocator alloc_;

        // writable pages — per class. Each entry is a page that's already
        // durable on NVMe but still has at least one free slot, so the next
        // persist round for the same class will reuse it in-memory. LIFO
        // (pop_back) keeps the hottest partial page in front.
        //
        // Spec mapping (see runtime_memory_and_cache.md §6.3 and §8.6):
        // `writable_pages_[ci]` is the engineering landing of the spec's
        // "open frame held across persist rounds" idea. It is the resident
        // continuation of a previously-durable partially-free page that the
        // owner keeps live so the next round can keep filling it without
        // re-reading it from device. It is **not** the spec's full
        // `hole_page_list` (non-resident placement metadata), nor the spec's
        // `open_frames[class_idx]` per-class single open DMA frame; the v1
        // implementation collapses both into this per-class queue because
        // there is no separate non-resident hole tracking yet and only one
        // page is being filled at a time per class anyway. The
        // `value_page_source::writable` enum value names exactly this
        // population — pages popped here come back here on rollback.
        std::vector<std::vector<page_data>> writable_pages_;

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
        // commit time (round_page's unique_ptr destructor frees them).
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
                          Cache                     cache,
                          size_t                    queue_depth = 2048)
            : value_alloc_sched_base(queue_depth)
            , alloc_(class_sizes, lba_size, data_area_base, data_area_end)
            , writable_pages_(class_sizes.size())
            , readonly_cache_(std::move(cache))
            , class_sizes_storage_(class_sizes.begin(), class_sizes.end())
        {
        }

        // ── Destructor ──
        //
        // The cache holds non-owning page_frame* pointers. The scheduler is
        // the sole owner of the backing buffers and frame descriptors, so
        // on teardown we drain every remaining frame through drain_one()
        // and free both the buffer and descriptor.

        ~value_alloc_sched() {
            while (auto f = readonly_cache_.drain_one()) {
                delete[] (*f)->buf;
                delete *f;
            }
        }

        // ── advance ──
        //
        // Order matters: finalize first (releases inflight rounds and may
        // return pages to writable_pages_, which makes them available for
        // subsequent persist rounds). persist next (consumes the freshly
        // freed pages). read/fill last.
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
        // write_desc holds raw pointers into round_page.image; we relied on
        // that contract before by deferring the descriptor build to the
        // end of handle_persist, but it deserves its own named step now
        // that the control flow is no longer linear.

        void
        finalize_round_writes(round& rnd) {
            rnd.writes.reserve(rnd.pages.size());
            for (auto& page : rnd.pages) {
                rnd.writes.push_back(write_desc{
                    .lba      = page.base.lba,
                    .data     = page.image.get(),
                    .num_lbas = page.span_lbas,
                    .flags    = 0,
                });
            }
        }

        // ── publish_round ──
        //
        // Install the round into inflight_rounds_, fire the leader's
        // callback with the write list, and stash the followers on the
        // round so handle_finalize can wake them when the FUA write
        // completes. Followers do not include the leader itself.

        void
        publish_round(std::unique_ptr<round> rnd,
                      std::span<_value_persist::req* const> items,
                      _value_persist::req* leader_item) {
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
        // the page image. Reuses an in-flight round_page when one of the
        // same class still has a free slot; otherwise pulls a fresh page
        // via acquire_round_page (writable → whole_pool → fresh_bump).

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
            uint32_t slot = static_cast<uint32_t>(__builtin_ctzll(page->free_mask));
            page->free_mask &= ~(1ULL << slot);

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
            std::span<char> slot_span(page->image.get() + off, cs);
            std::span<const char> body_span(entry.body.data(), entry.body.size());
            if (!format::encode_value_object(slot_span, body_span)) {
                return persist_entry_status::encode_failure;
            }

            // Fill the caller's value_ref.
            entry.out_vr->base        = page->base;
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
                if (it->class_idx == ci && it->free_mask != 0)
                    return &*it;
            }
            return nullptr;
        }

        round_page*
        acquire_round_page(round& rnd, uint16_t ci) {
            // 1. Pop the most recent writable page for this class (LIFO).
            //    All entries in writable_pages_[ci] have free slots and a
            //    durable NVMe image we can mutate in-memory.
            if (!writable_pages_[ci].empty()) {
                auto pd = std::move(writable_pages_[ci].back());
                writable_pages_[ci].pop_back();
                rnd.pages.push_back(round_page{
                    .base               = pd.base,
                    .class_idx          = ci,
                    .span_lbas          = alloc_.span_lbas(ci),
                    .source             = value_page_source::writable,
                    .original_free_mask = pd.free_mask,
                    .free_mask          = pd.free_mask,
                    .image              = std::move(pd.image),
                    .image_size         = pd.image_size,
                });
                return &rnd.pages.back();
            }

            // 2. Ask allocator (whole_pool / bump)
            auto ar = alloc_.acquire_page(ci);
            if (!ar) return nullptr;

            uint32_t img_bytes = ar->span_lbas * alloc_.lba_size();
            // make_unique<char[]>(N) value-initializes (zero-fills) — same
            // semantics as the previous vector<char>(N, 0).
            auto image = std::make_unique<char[]>(img_bytes);
            rnd.pages.push_back(round_page{
                .base               = ar->page_base,
                .class_idx          = ci,
                .span_lbas          = ar->span_lbas,
                .source             = ar->source,
                .original_free_mask = ar->free_mask,
                .free_mask          = ar->free_mask,
                .image              = std::move(image),
                .image_size         = img_bytes,
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
                if (page.free_mask == 0) {
                    // Full page. Decision D1: only 1-LBA pages enter the
                    // readonly cache. Multi-LBA full pages are dropped here
                    // (the round_page's unique_ptr destructor frees the buf
                    // when the round goes out of scope) — admitting them
                    // would waste bounded-cache capacity since handle_read
                    // bypasses the cache for span > 1 and the page would
                    // never be hit again, only evict useful 1-LBA entries.
                    if (page.span_lbas == 1) {
                        auto* pf = new memory::page_frame{
                            .id        = memory::frame_id{page.base, 1,
                                            memory::frame_id::domain::value_page},
                            .st        = memory::frame_state::clean_readonly,
                            .buf       = page.image.release(),
                            .byte_len  = page.image_size,
                            .pin_count = 0,
                            .crc_valid = false,
                        };
                        if (auto evicted = readonly_cache_.put(pf)) {
                            delete[] (*evicted)->buf;
                            delete *evicted;
                        }
                    }
                    // span > 1: page.image's unique_ptr<char[]> destructor
                    // frees the buf at end of round.
                } else {
                    // still has free slots → put back as a writable page
                    install_writable_page(page.class_idx, page.base,
                                          page.free_mask, std::move(page.image),
                                          page.image_size);
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
        //   - writable    → restore via install_writable_page with the
        //                   original (pre-round) free_mask
        //
        // INC-017: previously fresh_bump and whole_page were silently
        // dropped here, leaking Data Area capacity. The "accept bump head
        // leak" comment is gone.

        void
        rollback_pages(round& rnd) {
            for (auto it = rnd.pages.rbegin(); it != rnd.pages.rend(); ++it) {
                auto& page = *it;
                switch (page.source) {
                case value_page_source::fresh_bump:
                    alloc_.push_back_bump(page.base, page.span_lbas);
                    break;
                case value_page_source::whole_page:
                    alloc_.recycle_whole_page(page.class_idx, page.base);
                    break;
                case value_page_source::writable:
                    // Restore with the original (pre-round) free_mask.
                    install_writable_page(page.class_idx, page.base,
                                          page.original_free_mask,
                                          std::move(page.image),
                                          page.image_size);
                    break;
                }
            }
        }

        void
        install_writable_page(uint16_t ci, paddr base, uint64_t free_mask,
                              std::unique_ptr<char[]>&& image,
                              uint32_t image_size) {
            // v7: single per-class queue, no special "active" slot. Newest
            // installs land at the back so the next acquire_round_page picks
            // them up first (hot path stays in cache).
            writable_pages_[ci].push_back(page_data{
                .base       = base,
                .free_mask  = free_mask,
                .image      = std::move(image),
                .image_size = image_size,
            });
        }

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

            // 1. writable_pages_[ci] — pages held by the scheduler with free
            //    slots remaining. Their NVMe image is durable but the
            //    in-memory copy is the freshest source for any reader (and
            //    every value object encoded so far this round is only here).
            //    Linear scan: N is small (typically 0-2 per class) so this is
            //    cheaper than maintaining a paddr index.
            for (auto& pd : writable_pages_[ci]) {
                if (pd.base == item->vr.base) {
                    serve_hit_or_fail(item,
                                      std::span<const char>(pd.image.get(), pd.image_size),
                                      "writable_pages");
                    delete item;
                    return;
                }
            }

            // 2. readonly_cache_ — only consulted when this class is
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

            // 3. miss → tell pipeline to issue NVMe read into a fresh buf.
            //    The unique_ptr flows through the pipeline (see read_miss
            //    and on_read_miss in sender.hh) and ends up in handle_fill
            //    which either releases it into the cache (admit) or drops
            //    it after decode (bypass).
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
        // span<const char> is the uniform decode path: writable_pages_,
        // readonly_cache_, and the fill staging buffer all reach this
        // through a span construction at the call site, so the helper is
        // agnostic to who owns the bytes.
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
