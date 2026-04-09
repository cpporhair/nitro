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

    struct scheduler_base;

    // ── PUMP op/sender wrappers ──
    //
    // Each operation gets a req (heap-allocated, deleted after cb), an op
    // with a tag bool, and a sender that builds an op_list. start() is
    // declared here and defined after `scheduler` is fully defined.

    namespace _value_persist {

        struct req {
            std::span<put_entry>                            entries;
            std::move_only_function<void(prepare_persist_result&&)> cb;
            std::move_only_function<void(std::exception_ptr)>       fail;
        };

        struct op {
            constexpr static bool value_persist_op = true;
            scheduler_base*      sched;
            std::span<put_entry> entries;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            scheduler_base*      sched;
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
            scheduler_base* sched;
            uint64_t        round_id;
            bool            ok;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            scheduler_base* sched;
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
            scheduler_base* sched;
            value_ref       vr;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            scheduler_base* sched;
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
            scheduler_base*         sched;
            value_ref               vr;
            std::unique_ptr<char[]> buf;
            uint32_t                buf_size;
            bool                    admit_to_cache;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            scheduler_base*         sched;
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

    // ── scheduler_base ──
    //
    // Non-templated layer holding the four PUMP queues, the schedule_*
    // enqueue helpers, and the sender factory entry points. Senders/ops only
    // need this base — they never see the templated derived class — so the
    // PUMP pipeline machinery (op_pusher / compute_sender_type) doesn't have
    // to know what Cache the scheduler uses.
    //
    // The templated scheduler<Cache> publicly derives from this base; pointer
    // implicit upcasting lets the runtime/registry/sender layers all work in
    // terms of scheduler_base*.

    struct scheduler_base {
        pump::core::per_core::queue<_value_persist::req*>  persist_q_;
        pump::core::per_core::queue<_value_finalize::req*> finalize_q_;
        pump::core::per_core::queue<_value_read::req*>     read_q_;
        pump::core::per_core::queue<_value_fill::req*>     fill_q_;

        explicit
        scheduler_base(size_t queue_depth = 2048)
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
        // tree::lookup_scheduler_base uses for process() / submit_cache().

        auto prepare_persist(std::span<put_entry> entries);
        auto finalize_persist(uint64_t round_id, bool ok);
        auto prepare_read(value_ref vr);
        auto fill_and_decode(value_ref vr, std::unique_ptr<char[]> buf,
                             uint32_t buf_size, bool admit_to_cache);
    };

    // ── scheduler<Cache> ──

    template <core::cache_concept Cache>
    struct scheduler : scheduler_base {
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
        std::vector<std::vector<page_data>> writable_pages_;

        // readonly cache: paddr → 1-LBA page image. Holds raw char[] buffers
        // handed off via release() + put() — the scheduler is the sole
        // owner of the cache, and ~scheduler() below drains it via
        // evict_one() because clock_cache / slru_cache do not free buffers
        // in their own destructors (they only own their slot/node arrays).
        //
        // Decision D1 — multi-LBA bypass: only span_lbas == 1 pages enter
        // here. commit_pages and handle_fill both gate on the span before
        // calling put(); handle_read symmetrically only consults the cache
        // when admit == (span == 1). Multi-LBA full pages are dropped at
        // commit time (round_page's unique_ptr destructor frees them).
        //
        // Lifetime contract for put() / evict_one() (full statement in
        // core/page_cache.hh's cache_concept doc): every "Some(...)" return
        // is a buf the caller must free. Both put-on-existing-key (an
        // overwritten replacement) and put-when-cap-full (an LRU eviction)
        // use the same channel. commit_pages and handle_fill below treat
        // the optional uniformly with `if (auto evicted = ...) delete[]`.
        Cache readonly_cache_;

        // leader-follower in-flight tracking. round_id is a monotonically
        // increasing key only ever queried point-wise (find/erase by id);
        // there is no ordered iteration, so a hash map is preferred over the
        // RB-tree std::map originally used.
        absl::flat_hash_map<uint64_t, std::unique_ptr<round>> inflight_rounds_;
        uint64_t                                              next_round_id_ = 1;

        scheduler(std::span<const uint32_t> class_sizes,
                  uint32_t                  lba_size,
                  paddr                     data_area_base,
                  paddr                     data_area_end,
                  Cache                     cache,
                  size_t                    queue_depth = 2048)
            : scheduler_base(queue_depth)
            , alloc_(class_sizes, lba_size, data_area_base, data_area_end)
            , writable_pages_(class_sizes.size())
            , readonly_cache_(std::move(cache))
            , class_sizes_storage_(class_sizes.begin(), class_sizes.end())
        {
            class_sizes_view_ = std::span<const uint32_t>(
                class_sizes_storage_.data(), class_sizes_storage_.size());
        }

        // ── Destructor ──
        //
        // The cache holds raw char[] buffers handed off by commit_pages /
        // handle_fill via release(). The scheduler is the sole owner of the
        // cache, so on teardown we must drain every remaining buffer through
        // evict_one() — neither clock_cache nor slru_cache frees buffers in
        // their own destructor (they only own slot/node arrays).

        ~scheduler() {
            while (auto e = readonly_cache_.evict_one()) {
                delete[] e->buf;
            }
        }

        // ── advance ──
        //
        // Order matters: finalize first (releases inflight rounds and may
        // return pages to writable_pages_, which makes them available for
        // subsequent persist rounds). persist next (consumes the freshly
        // freed pages). read/fill last.

        bool advance() {
            bool progress = false;

            progress |= finalize_q_.drain([this](_value_finalize::req* r) {
                handle_finalize(r);
            });

            // Persist uses while + try_dequeue: handle_persist internally
            // drains the queue's leftover items as followers, so the next
            // try_dequeue returns nothing and the loop exits naturally.
            while (auto item = persist_q_.try_dequeue()) {
                handle_persist(*item);
                progress = true;
            }

            progress |= read_q_.drain([this](_value_read::req* r) {
                handle_read(r);
            });

            progress |= fill_q_.drain([this](_value_fill::req* r) {
                handle_fill(r);
            });

            return progress;
        }

        template<typename runtime_t>
        bool advance(runtime_t&) { return advance(); }

    private:
        // ════════════════════════════════════════════════════════════════
        //  handle_persist  —  leader-follower round assembly
        // ════════════════════════════════════════════════════════════════

        void
        handle_persist(_value_persist::req* leader_item) {
            auto rnd = std::make_unique<round>();
            rnd->id = next_round_id_++;

            // Drain leftover persist requests as followers.
            std::vector<_value_persist::req*> all_items;
            all_items.push_back(leader_item);
            while (auto item = persist_q_.try_dequeue())
                all_items.push_back(*item);

            // Process every entry across all items, allocating slots and
            // encoding value objects in-place into round pages.
            //
            // On any failure (out of space, encode error) we fail the entire
            // round immediately — leader + all followers — and return without
            // touching the inflight_rounds_ map.

            std::optional<std::exception_ptr> failure;
            for (auto* item : all_items) {
                for (auto& entry : item->entries) {
                    if (!persist_one_entry(*rnd, entry)) {
                        failure = std::make_exception_ptr(
                            std::runtime_error("value::persist: out of space or encode failure"));
                        goto round_failed;
                    }
                }
            }

            // Build write descriptors over round.pages (must be done after
            // all entries are encoded — vector<round_page> must not realloc
            // between encode and dispatch, so we built it inline above).
            rnd->writes.reserve(rnd->pages.size());
            for (auto& page : rnd->pages) {
                rnd->writes.push_back(write_desc{
                    .lba      = page.base.lba,
                    .data     = page.image.get(),
                    .num_lbas = page.span_lbas,
                    .flags    = 0,
                });
            }

            {
                // Stash the round, fire leader cb with writes, leave
                // followers waiting.
                uint64_t rid = rnd->id;
                std::span<write_desc> writes_span(
                    rnd->writes.data(), rnd->writes.size());

                // followers receive the leader_item position 0 → make sure
                // we don't include leader_item in followers vector.
                for (size_t i = 1; i < all_items.size(); ++i)
                    rnd->followers.push_back(all_items[i]);

                inflight_rounds_.emplace(rid, std::move(rnd));

                leader_item->cb(prepare_persist_result{
                    persist_leader{rid, writes_span}
                });
                delete leader_item;
            }
            return;

        round_failed:
            // Roll back any pages we already touched and fail leader +
            // followers. Pages popped from writable_pages_ are restored by
            // rollback_pages; fresh_bump / whole_page sources are simply
            // dropped (they were never durable so this is safe — except the
            // bump head has already moved, which leaks Data Area capacity).
            // v6 accepts this leak: persist failures here only happen on
            // out-of-space conditions and the runtime is about to terminate.
            rollback_pages(*rnd);
            for (auto* item : all_items) {
                item->fail(*failure);
                delete item;
            }
        }

        // ── Allocate a slot for one entry, encoding into the page image. ──
        //
        // Reuses an in-flight round_page if there's one with the same
        // class_idx and free_mask != 0. Otherwise pulls a fresh page via
        // alloc_.acquire_page() and seeds the round_page from open/ready
        // state or a new buffer.

        bool
        persist_one_entry(round& rnd, put_entry& entry) {
            uint32_t total = sizeof(format::value_object_header) + entry.body.size();
            auto ci_opt = format::find_min_class(total, get_class_sizes_view());
            if (!ci_opt) return false;
            uint16_t ci = *ci_opt;

            round_page* page = find_round_page_with_room(rnd, ci);
            if (!page) {
                page = acquire_round_page(rnd, ci);
                if (!page) return false;
            }

            // Pick the lowest set bit (next free slot).
            uint32_t slot = static_cast<uint32_t>(__builtin_ctzll(page->free_mask));
            page->free_mask &= ~(1ULL << slot);

            uint32_t cs = alloc_.class_size(ci);
            uint16_t off = 0;
            if (alloc_.is_sub_lba(ci)) {
                off = static_cast<uint16_t>(slot * cs);
            }

            // Encode value object into the slot.
            std::span<char> slot_span(page->image.get() + off, cs);
            std::span<const char> body_span(entry.body.data(), entry.body.size());
            if (!format::encode_value_object(slot_span, body_span)) {
                // restore the bit so rollback can return the page cleanly
                page->free_mask |= (1ULL << slot);
                return false;
            }

            // Fill the caller's value_ref.
            entry.out_vr->base        = page->base;
            entry.out_vr->byte_offset = off;
            entry.out_vr->len         = static_cast<uint32_t>(entry.body.size());
            entry.out_vr->flags       = 0;
            return true;
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
                std::fprintf(stderr,
                    "value::finalize: unknown round_id %lu — invariant broken\n",
                    static_cast<unsigned long>(item->round_id));
                std::abort();
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
                        char* raw = page.image.release();
                        if (auto evicted = readonly_cache_.put(page.base, raw)) {
                            delete[] evicted->buf;
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

        void
        rollback_pages(round& rnd) {
            for (auto& page : rnd.pages) {
                switch (page.source) {
                case value_page_source::fresh_bump:
                case value_page_source::whole_page:
                    // Pages we allocated this round but never committed.
                    // Drop them; bump head leak is acceptable for v6.
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
            //    admissible (multi-LBA bypasses). The cache stores raw
            //    char[] keyed by paddr; cached_size = lba_size since all
            //    cache entries are 1-LBA by D1.
            if (admit) {
                if (const char* p = readonly_cache_.get(item->vr.base)) {
                    serve_hit_or_fail(item,
                                      std::span<const char>(p, alloc_.lba_size()),
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
            // (bad magic / body_len / crc / truncated) we must surface an
            // exception via item->fail() and leave the cache untouched —
            // otherwise every subsequent reader of this paddr would be
            // served the same poisoned page.
            auto body_opt = try_decode_value(
                std::span<const char>(item->buf.get(), item->buf_size),
                item->vr);
            if (!body_opt) {
                item->fail(std::make_exception_ptr(std::runtime_error(
                    "value::read: corrupt value object on disk (post-NVMe)")));
                delete item;
                return;
            }

            // Verified — admit policy was decided in handle_read and rides
            // along on the fill req:
            //   admit  → release the buf into the cache; the cache becomes
            //            sole owner. If the put evicts another entry, that
            //            buf is freed here.
            //   bypass → unique_ptr stays in item, item->~req() frees it.
            if (item->admit_to_cache) {
                char* raw = item->buf.release();
                if (auto evicted = readonly_cache_.put(item->vr.base, raw)) {
                    delete[] evicted->buf;
                }
            }

            item->cb(std::move(*body_opt));
            delete item;
        }

        // ── helpers ──

        // Decode a value object from an in-memory page image at vr's slot.
        //
        // Returns nullopt on ANY format error (truncated / bad_magic /
        // bad_body_len / bad_crc) — the caller must distinguish "no value"
        // from "on-disk corruption" and surface the latter as an exception.
        // Never returns an empty std::string to mean "decode failed";
        // empty body is a legitimate value.
        //
        // span<const char> is the uniform decode path: writable_pages_,
        // readonly_cache_, and the fill staging buffer all reach this through
        // a span construction at the call site, so the helper is agnostic to
        // who owns the bytes.
        std::optional<std::string>
        try_decode_value(std::span<const char> image, value_ref vr) const {
            if (vr.byte_offset >= image.size()) return std::nullopt;
            auto slot = image.subspan(vr.byte_offset);
            auto d = format::decode_value_object(slot, vr.len);
            if (!d.ok()) return std::nullopt;
            return std::string(d.body.data(), d.body.size());
        }

        // Serve a read req from an in-memory page image, surfacing
        // corruption as item->fail() rather than a silent empty body.
        // Caller is responsible for `delete item` and `return` after this.
        void
        serve_hit_or_fail(_value_read::req* item,
                          std::span<const char> image,
                          const char* source_label) {
            auto body = try_decode_value(image, item->vr);
            if (!body) {
                std::string msg = "value::read: corrupt value object in ";
                msg += source_label;
                item->fail(std::make_exception_ptr(std::runtime_error(msg)));
                return;
            }
            item->cb(read_prepare_result{ read_hit{ std::move(*body) } });
        }

        std::optional<uint16_t>
        class_for_len(uint32_t body_len) const noexcept {
            uint32_t total = sizeof(format::value_object_header) + body_len;
            return format::find_min_class(total, get_class_sizes_view());
        }

        std::span<const uint32_t>
        get_class_sizes_view() const noexcept {
            return class_sizes_view_;
        }

        // Cached view used by find_min_class. class_sizes_storage_ owns the
        // bytes (initialized in the ctor's init list from the input span);
        // class_sizes_view_ is a stable span over that storage built once
        // in the ctor body. Owning a copy means the input span doesn't have
        // to outlive the scheduler. The 16 inline slots match the on-disk
        // value_size_classes upper bound (superblock §2 in
        // on_disk_formats.md), so the small-resident metadata stays inline
        // and avoids a heap allocation per scheduler.
        absl::InlinedVector<uint32_t, 16> class_sizes_storage_;
        std::span<const uint32_t>         class_sizes_view_;
    };

    // ── scheduler_base sender factory deferred definitions ──
    //
    // Defined out-of-line so the sender struct types they return are visible
    // by this point. Same pattern as tree::lookup_scheduler_base::process().

    inline auto
    scheduler_base::prepare_persist(std::span<put_entry> entries) {
        return _value_persist::sender{.sched = this, .entries = entries};
    }

    inline auto
    scheduler_base::finalize_persist(uint64_t round_id, bool ok) {
        return _value_finalize::sender{.sched = this, .round_id = round_id, .ok = ok};
    }

    inline auto
    scheduler_base::prepare_read(value_ref vr) {
        return _value_read::sender{.sched = this, .vr = vr};
    }

    inline auto
    scheduler_base::fill_and_decode(value_ref vr,
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
