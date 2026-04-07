#ifndef APPS_INCONEL_VALUE_SCHEDULER_HH
#define APPS_INCONEL_VALUE_SCHEDULER_HH

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

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

    struct read_hit  { std::string body; };
    struct read_miss { paddr base; uint32_t span_lbas; std::shared_ptr<std::vector<char>> buf; };
    using read_prepare_result = std::variant<read_hit, read_miss>;

    // ── Internal page_data ──
    //
    // A page that's currently held by the scheduler in writable state
    // (open / ready) — i.e. still has free slots and the in-memory image is
    // already durable on NVMe.

    struct page_data {
        paddr             base;
        uint64_t          free_mask;
        std::vector<char> image;
    };

    // ── Forward declarations of req types (used by sender layer) ──

    namespace _value_persist  { struct req; }
    namespace _value_finalize { struct req; }
    namespace _value_read     { struct req; }
    namespace _value_fill     { struct req; }

    struct scheduler;

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
            scheduler*           sched;
            std::span<put_entry> entries;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            scheduler*           sched;
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
            scheduler* sched;
            uint64_t   round_id;
            bool       ok;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            scheduler* sched;
            uint64_t   round_id;
            bool       ok;

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
            scheduler* sched;
            value_ref  vr;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            scheduler* sched;
            value_ref  vr;

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
            std::shared_ptr<std::vector<char>>                 buf;
            std::move_only_function<void(std::string&&)>       cb;
            std::move_only_function<void(std::exception_ptr)>  fail;
        };

        struct op {
            constexpr static bool value_fill_op = true;
            scheduler*                         sched;
            value_ref                          vr;
            std::shared_ptr<std::vector<char>> buf;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            scheduler*                         sched;
            value_ref                          vr;
            std::shared_ptr<std::vector<char>> buf;

            auto make_op() { return op{.sched = sched, .vr = vr, .buf = std::move(buf)}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    // ── Scheduler ──

    struct scheduler {
        // ── Per-round state (held in inflight_rounds_) ──
        //
        // Created by leader's prepare handle, consumed by finalize handle.
        // Followers wait inside this round's followers vec.

        struct round_page {
            paddr             base;
            uint16_t          class_idx;
            uint32_t          span_lbas;
            value_page_source source;
            uint64_t          original_free_mask; // for rollback
            uint64_t          free_mask;          // updated as slots are filled
            std::vector<char> image;
        };

        struct round {
            uint64_t                              id;
            std::vector<round_page>               pages;
            std::vector<write_desc>               writes;     // built from pages
            std::vector<_value_persist::req*>     followers;  // not the leader
        };

        value_allocator alloc_;

        // writable state — per class
        std::vector<std::optional<page_data>> open_pages_;
        std::vector<std::vector<page_data>>   ready_pages_;

        // readonly cache: paddr → page image bytes (full LBA span)
        // v6: simple inconel-style unbounded map; future step replaces with
        // page_cache_concept (clock/slru) + buf reuse pool.
        std::map<paddr, std::vector<char>> readonly_cache_;

        // leader-follower in-flight tracking
        std::map<uint64_t, std::unique_ptr<round>> inflight_rounds_;
        uint64_t                                   next_round_id_ = 1;

        // 4 PUMP queues, one per request kind
        pump::core::per_core::queue<_value_persist::req*>  persist_q_;
        pump::core::per_core::queue<_value_finalize::req*> finalize_q_;
        pump::core::per_core::queue<_value_read::req*>     read_q_;
        pump::core::per_core::queue<_value_fill::req*>     fill_q_;

        scheduler(std::span<const uint32_t> class_sizes,
                  uint32_t                  lba_size,
                  paddr                     data_area_base,
                  paddr                     data_area_end,
                  size_t                    queue_depth = 2048)
            : alloc_(class_sizes, lba_size, data_area_base, data_area_end)
            , open_pages_(class_sizes.size())
            , ready_pages_(class_sizes.size())
            , persist_q_(queue_depth)
            , finalize_q_(queue_depth)
            , read_q_(queue_depth)
            , fill_q_(queue_depth)
            , class_sizes_storage_(class_sizes.begin(), class_sizes.end())
        {
            class_sizes_view_ = std::span<const uint32_t>(class_sizes_storage_);
        }

        // ── enqueue helpers (called by op::start) ──

        void schedule_persist (_value_persist::req*  r) { persist_q_ .try_enqueue(r); }
        void schedule_finalize(_value_finalize::req* r) { finalize_q_.try_enqueue(r); }
        void schedule_read    (_value_read::req*     r) { read_q_    .try_enqueue(r); }
        void schedule_fill    (_value_fill::req*     r) { fill_q_    .try_enqueue(r); }

        // ── advance ──
        //
        // Order matters: finalize first (releases inflight rounds and may
        // free pages back to open_pages_, which makes them available for
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

        // ── Sender factories ──

        auto prepare_persist(std::span<put_entry> entries) {
            return _value_persist::sender{.sched = this, .entries = entries};
        }

        auto finalize_persist(uint64_t round_id, bool ok) {
            return _value_finalize::sender{.sched = this, .round_id = round_id, .ok = ok};
        }

        auto prepare_read(value_ref vr) {
            return _value_read::sender{.sched = this, .vr = vr};
        }

        auto fill_and_decode(value_ref vr, std::shared_ptr<std::vector<char>> buf) {
            return _value_fill::sender{.sched = this, .vr = vr, .buf = std::move(buf)};
        }

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
                    .data     = page.image.data(),
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
            // followers. Pages from open_pages_ / ready_pages_ are restored
            // by rollback_pages; fresh_bump / whole_page sources are simply
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
            std::span<char> slot_span(page->image.data() + off, cs);
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
            // 1. Take from open_pages_[ci] if present
            if (open_pages_[ci].has_value()) {
                auto pd = std::move(*open_pages_[ci]);
                open_pages_[ci].reset();
                alloc_.close_open_page(ci);
                rnd.pages.push_back(round_page{
                    .base               = pd.base,
                    .class_idx          = ci,
                    .span_lbas          = alloc_.span_lbas(ci),
                    .source             = value_page_source::open_frame,
                    .original_free_mask = pd.free_mask,
                    .free_mask          = pd.free_mask,
                    .image              = std::move(pd.image),
                });
                return &rnd.pages.back();
            }

            // 2. Take from ready_pages_[ci] back if present
            if (!ready_pages_[ci].empty()) {
                auto pd = std::move(ready_pages_[ci].back());
                ready_pages_[ci].pop_back();
                rnd.pages.push_back(round_page{
                    .base               = pd.base,
                    .class_idx          = ci,
                    .span_lbas          = alloc_.span_lbas(ci),
                    .source             = value_page_source::ready_page,
                    .original_free_mask = pd.free_mask,
                    .free_mask          = pd.free_mask,
                    .image              = std::move(pd.image),
                });
                return &rnd.pages.back();
            }

            // 3. Ask allocator (whole_pool / bump)
            auto ar = alloc_.acquire_page(ci);
            if (!ar) return nullptr;

            uint32_t img_bytes = ar->span_lbas * alloc_.lba_size();
            std::vector<char> image(img_bytes, 0);   // fresh page → zero
            rnd.pages.push_back(round_page{
                .base               = ar->page_base,
                .class_idx          = ci,
                .span_lbas          = ar->span_lbas,
                .source             = ar->source,
                .original_free_mask = ar->free_mask,
                .free_mask          = ar->free_mask,
                .image              = std::move(image),
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
                    // page is full → move into readonly cache
                    readonly_cache_[page.base] = std::move(page.image);
                } else {
                    // still has free slots → put back as a writable page
                    install_writable_page(page.class_idx, page.base,
                                          page.free_mask, std::move(page.image));
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
                case value_page_source::open_frame:
                case value_page_source::ready_page:
                    // Restore with the original (pre-round) free_mask.
                    install_writable_page(page.class_idx, page.base,
                                          page.original_free_mask,
                                          std::move(page.image));
                    break;
                }
            }
        }

        void
        install_writable_page(uint16_t ci, paddr base,
                              uint64_t free_mask, std::vector<char>&& image) {
            page_data pd{.base = base, .free_mask = free_mask, .image = std::move(image)};
            if (!open_pages_[ci].has_value()) {
                open_pages_[ci] = std::move(pd);
                alloc_.install_open_page({base, ci, free_mask});
                return;
            }
            ready_pages_[ci].push_back(std::move(pd));
        }

        // ════════════════════════════════════════════════════════════════
        //  handle_read  —  prepare cache lookup, fan out NVMe miss
        // ════════════════════════════════════════════════════════════════

        void
        handle_read(_value_read::req* item) {
            // Determine the class first; we need span_lbas to allocate the
            // miss buffer.
            auto ci_opt = class_for_len(item->vr.len);
            if (!ci_opt) {
                item->fail(std::make_exception_ptr(
                    std::runtime_error("value::read: body length exceeds all classes")));
                delete item;
                return;
            }
            uint16_t ci = *ci_opt;

            // 1. open_pages_[ci]
            if (open_pages_[ci].has_value() && open_pages_[ci]->base == item->vr.base) {
                serve_hit_or_fail(item, open_pages_[ci]->image, "open_pages");
                delete item;
                return;
            }

            // 2. ready_pages_[ci]
            for (auto& pd : ready_pages_[ci]) {
                if (pd.base == item->vr.base) {
                    serve_hit_or_fail(item, pd.image, "ready_pages");
                    delete item;
                    return;
                }
            }

            // 3. readonly_cache_
            if (auto it = readonly_cache_.find(item->vr.base); it != readonly_cache_.end()) {
                serve_hit_or_fail(item, it->second, "readonly_cache");
                delete item;
                return;
            }

            // 4. miss → tell pipeline to issue NVMe read into a fresh buf
            uint32_t span = alloc_.span_lbas(ci);
            uint32_t img_bytes = span * alloc_.lba_size();
            auto buf = std::make_shared<std::vector<char>>(img_bytes, char{0});
            item->cb(read_prepare_result{
                read_miss{item->vr.base, span, std::move(buf)}
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
            auto body_opt = try_decode_value(*item->buf, item->vr);
            if (!body_opt) {
                item->fail(std::make_exception_ptr(std::runtime_error(
                    "value::read: corrupt value object on disk (post-NVMe)")));
                delete item;
                return;
            }

            // Verified — safe to move the staging buffer into the cache.
            // body_opt was copy-constructed from the buffer's bytes above,
            // so the move-out below cannot dangle it.
            readonly_cache_[item->vr.base] = std::move(*item->buf);

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
        std::optional<std::string>
        try_decode_value(const std::vector<char>& image, value_ref vr) const {
            std::span<const char> slot(image.data() + vr.byte_offset,
                                       image.size()  - vr.byte_offset);
            auto d = format::decode_value_object(slot, vr.len);
            if (!d.ok()) return std::nullopt;
            return std::string(d.body.data(), d.body.size());
        }

        // Serve a read req from an in-memory page image, surfacing
        // corruption as item->fail() rather than a silent empty body.
        // Caller is responsible for `delete item` and `return` after this.
        void
        serve_hit_or_fail(_value_read::req* item,
                          const std::vector<char>& image,
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

        // Cached view used by find_min_class. Built once in the constructor
        // body below — placed here to keep handle_persist inline-friendly.
        std::vector<uint32_t> class_sizes_storage_;
        std::span<const uint32_t> class_sizes_view_;

    public:
        // Hook called by the constructor to populate the cached class sizes
        // view. We can't initialize class_sizes_storage_ inside the
        // initializer list cleanly because the constructor takes a span and
        // we want to own the storage.
        void
        init_class_sizes_view(std::span<const uint32_t> class_sizes) noexcept {
            class_sizes_storage_.assign(class_sizes.begin(), class_sizes.end());
            class_sizes_view_ = std::span<const uint32_t>(class_sizes_storage_);
        }
    };

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
            .vr  = vr,
            .buf = std::move(buf),
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
