#ifndef APPS_INCONEL_TREE_LOOKUP_SCHEDULER_HH
#define APPS_INCONEL_TREE_LOOKUP_SCHEDULER_HH

// ── tree_lookup_sched — point-read tree traversal (step 022 §4) ──
//
// Split out of the old `tree/scheduler.hh` in Phase 2. The read-path
// semantics are unchanged: `tree_lookup_sched_base::process(state)`
// still drives a multi-level B+ tree descent, single-flights
// in-flight NVMe reads, and cooperates with the `readonly_frame_cache`
// it owns locally. Phase 2 *intentionally does not* migrate the cache,
// the frame pool, or the single-flight map to a shared
// `tree_read_domain` object (022 D5, §3) — that is deferred to the
// cache ownership migration step before Phase 5/6.
//
// The only lookup-scheduler change Phase 2 actually introduces is a
// new `uint32_t read_domain_index` member on
// `tree_lookup_sched_base`, set at construction time. Pairing with a
// worker on the same core is entirely carried by `registry::by_core`
// plus the matching `read_domain_index` value, not by any named
// runtime object (022 §3 / §7).
//
// File split (022 D12, §9): the op/sender/op_pusher/compute_sender_type
// specializations for both `_tree_lookup` and `_cache_pages` live in
// this header. `tree/scheduler.hh` is now an umbrella shim that
// #includes this header and `worker_scheduler.hh`; `tree/sender.hh`
// is the outward-facing facade.

#include <concepts>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

#include "../core/page_cache.hh"
#include "../core/panic.hh"
#include "../core/tree_manifest.hh"
#include "../memory/frame.hh"
#include "../format/tree_page.hh"
#include "./lookup.hh"
#include "./page_reader.hh"

namespace apps::inconel::tree {

    using namespace format;

    // ── Helper: construct a tree_node frame_id ──

    inline memory::frame_id
    make_tree_frame_id(paddr base, uint32_t page_lbas) {
        return memory::frame_id{
            base,
            static_cast<uint16_t>(page_lbas),
            memory::frame_id::domain::tree_node,
        };
    }

    // ── Lookup state (lives on PUMP context stack) ──

    struct lookup_state {
        struct entry {
            std::string_view key;
            bool resolved = false;
            lookup_result result = lookup_absent{};
            paddr next_page;
        };

        std::vector<entry> entries;
        bool all_done = false;
        uint32_t page_size{};
        uint32_t page_lbas{};
        const core::tree_manifest* manifest{};
    };

    template<typename key_range_t>
    inline auto
    make_lookup_state(key_range_t&& keys, const core::tree_manifest* manifest) {
        // M-2 (review round 2): the lookup path is the first consumer
        // that would touch `manifest->geom` fields, so fail-fast on
        // both tiers of the non-null contract here before anything
        // else is derived from the manifest. A null manifest or a
        // manifest with a null geom pointer is a caller-side
        // construction bug that must not be allowed to silently
        // segfault inside the sender layer.
        if (manifest == nullptr) {
            core::panic_inconsistency(
                "tree::make_lookup_state",
                "tree_manifest pointer must not be null");
        }
        if (manifest->geom == nullptr) {
            core::panic_inconsistency(
                "tree::make_lookup_state",
                "tree_manifest->geom must not be null");
        }

        lookup_state s;
        // Geometry is now sourced from the shared tree_geometry
        // instance hanging off the manifest (step 022 §2 / M-5).
        s.page_size = manifest->geom->tree_page_size;
        s.page_lbas = manifest->page_lbas();
        s.manifest = manifest;
        s.all_done = !manifest->has_root();

        paddr root = manifest->has_root() ? manifest->root_slot : paddr{0, 0};
        for (auto&& k : keys)
            s.entries.push_back({k, false, lookup_absent{}, root});

        if (!manifest->has_root())
            for (auto& e : s.entries) e.resolved = true;

        return s;
    }

    // ── Non-templated scheduler base ──
    //
    // Owns the request queues and exposes enqueue methods that the op layer
    // calls. Holds nothing cache-related so it can be referenced from
    // non-templated op/sender structs.

    namespace _tree_lookup { struct req; }
    namespace _cache_pages { struct req; }

    struct tree_lookup_sched_base {
        // Phase 2 (022 §3): pairing seam is expressed through
        // `read_domain_index` + same-core installation. There is no
        // named `tree_read_domain` runtime object.
        uint32_t read_domain_index;

        // H-1 (review rounds 1+2): every manifest handed to
        // `process()` must carry a geometry whose numeric fields
        // agree with the one this scheduler was constructed with,
        // otherwise `free_frames_` buffer reuse would mix page sizes
        // and let a small buffer back a larger NVMe read. The
        // check is **value-based**, not pointer-identity-based —
        // test fixtures, recovery, and the bootstrap builder may
        // each own their own `tree_geometry` instance as long as the
        // three numeric fields agree. Mismatches are enforced in
        // `process()` via `panic_inconsistency` (not
        // `unsupported_shape_change`) because a geometry mismatch is
        // a caller bug, not a data-shape case.
        const core::tree_geometry* expected_geom;

        pump::core::per_core::queue<_tree_lookup::req*> lookup_queue_;
        pump::core::per_core::queue<_cache_pages::req*> cache_queue_;

        explicit
        tree_lookup_sched_base(uint32_t rdi,
                               const core::tree_geometry* geom,
                               size_t depth)
            : read_domain_index(rdi)
            , expected_geom(geom)
            , lookup_queue_(depth)
            , cache_queue_(depth) {}

        void schedule_lookup(_tree_lookup::req* r) { lookup_queue_.try_enqueue(r); }
        void schedule_cache(_cache_pages::req* r)  { cache_queue_.try_enqueue(r); }

        auto process(lookup_state& state);
        auto submit_cache(std::vector<memory::page_frame*> frames);
    };

    // ── process sender (lookup request) ──

    namespace _tree_lookup {

        struct req {
            lookup_state* state;
            std::move_only_function<void(batch_decision&&)> cb;
            uint32_t wait_gen = 0;
            bool     wake_enqueued = false;
        };

        struct op {
            constexpr static bool tree_lookup_op = true;
            tree_lookup_sched_base* sched;
            lookup_state* state;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_lookup_sched_base* sched;
            lookup_state* state;

            auto make_op() {
                return op{.sched = sched, .state = state};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    // ── submit_cache sender (page cache submission) ──

    namespace _cache_pages {

        struct req {
            std::vector<memory::page_frame*> frames;
            std::move_only_function<void(bool)> cb;
        };

        struct op {
            constexpr static bool cache_pages_op = true;
            tree_lookup_sched_base* sched;
            std::vector<memory::page_frame*> frames;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_lookup_sched_base* sched;
            std::vector<memory::page_frame*> frames;

            auto make_op() {
                return op{.sched = sched, .frames = std::move(frames)};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    // ── Scheduler ──

    template <core::cache_concept Cache>
    struct tree_lookup_sched : tree_lookup_sched_base {
        struct wait_token {
            _tree_lookup::req* req;
            uint32_t           wait_gen;
        };

        struct pending_read {
            std::vector<wait_token> waiters;
        };

        Cache page_cache_;
        absl::flat_hash_map<paddr, pending_read> inflight_reads_;

        // Frame ownership pool. Every page_frame created by this scheduler
        // is recorded here so it can be freed on destruction — regardless
        // of whether the frame is currently in the cache, in free_frames_,
        // or in-flight inside a pipeline request.
        std::vector<memory::page_frame*> all_frames_;

        // Recyclable frames evicted from the cache. Their backing buffer
        // is reused for the next miss read.
        std::vector<memory::page_frame*> free_frames_;

        static constexpr uint32_t kMaxCacheOpsPerAdvance  = 64;
        static constexpr uint32_t kMaxLookupOpsPerAdvance = 64;

        explicit
        tree_lookup_sched(uint32_t rdi,
                          const core::tree_geometry* geom,
                          Cache cache,
                          size_t depth = 2048)
            : tree_lookup_sched_base(rdi, geom, depth)
            , page_cache_(std::move(cache)) {}

        ~tree_lookup_sched() {
            for (auto* f : all_frames_) {
                delete[] f->buf;
                delete f;
            }
        }

        bool advance() {
            bool progress = false;

            for (uint32_t i = 0; i < kMaxCacheOpsPerAdvance; ++i) {
                auto item = cache_queue_.try_dequeue();
                if (!item) break;
                _cache_pages::req* r = *item;

                for (auto* pf : r->frames) {
                    paddr addr = pf->id.base;

                    if (auto evicted = page_cache_.put(pf)) {
                        free_frames_.push_back(*evicted);
                    }

                    auto it = inflight_reads_.find(addr);
                    if (it == inflight_reads_.end()) {
                        core::panic_inconsistency(
                            "tree::tree_lookup_sched::advance",
                            "cache completion without inflight entry "
                            "dev=%u lba=%lu",
                            static_cast<unsigned>(addr.device_id),
                            static_cast<unsigned long>(addr.lba));
                    }
                    pending_read pr = std::move(it->second);
                    inflight_reads_.erase(it);

                    for (auto& tok : pr.waiters) {
                        if (tok.wait_gen != tok.req->wait_gen) continue;
                        if (tok.req->wake_enqueued) continue;
                        tok.req->wake_enqueued = true;
                        lookup_queue_.try_enqueue(tok.req);
                    }
                }
                r->cb(true);
                delete r;

                progress = true;
            }

            for (uint32_t i = 0; i < kMaxLookupOpsPerAdvance; ++i) {
                auto item = lookup_queue_.try_dequeue();
                if (!item) break;
                handle(*item);
                progress = true;
            }

            return progress;
        }

        template<typename runtime_t>
        bool advance(runtime_t&) { return advance(); }

    private:
        void handle(_tree_lookup::req* r) {
            r->wake_enqueued = false;

            auto& s = *r->state;

            process_entries(s);

            if (s.all_done) {
                r->cb(decision_done{});
                delete r;
                return;
            }

            decision_need_read decision;
            prepare_reads(s, decision);
            if (!decision.read_descs.empty()) {
                r->cb(std::move(decision));
                delete r;
                return;
            }

            r->wait_gen += 1;

            absl::flat_hash_set<paddr> wait_pages;
            for (auto& e : s.entries) {
                if (e.resolved) continue;
                if (page_cache_.contains(make_tree_frame_id(e.next_page, s.page_lbas))) continue;
                wait_pages.insert(e.next_page);
            }

            if (wait_pages.empty()) {
                core::panic_inconsistency(
                    "tree::tree_lookup_sched::handle",
                    "park requested with no pages to wait on");
            }

            for (auto page : wait_pages) {
                auto it = inflight_reads_.find(page);
                if (it == inflight_reads_.end()) {
                    core::panic_inconsistency(
                        "tree::tree_lookup_sched::handle",
                        "park page not in inflight_reads_ dev=%u lba=%lu",
                        static_cast<unsigned>(page.device_id),
                        static_cast<unsigned long>(page.lba));
                }
                it->second.waiters.push_back({r, r->wait_gen});
            }
        }

        void process_entries(lookup_state& s) {
            for (auto& e : s.entries) {
                if (e.resolved) continue;

                while (true) {
                    auto pin = page_cache_.pin(
                        make_tree_frame_id(e.next_page, s.page_lbas));
                    if (!pin.frame) break;

                    const char* page = pin.frame->buf;

                    auto status = inspect_tree_page(page, s.page_size);
                    if (status != tree_page_status::ok) {
                        core::panic_inconsistency(
                            "tree::tree_lookup_sched::process_entries",
                            "corrupt tree page dev=%u lba=%lu status=%s",
                            static_cast<unsigned>(e.next_page.device_id),
                            static_cast<unsigned long>(e.next_page.lba),
                            tree_page_status_to_string(status));
                    }

                    auto* hdr = reinterpret_cast<const tree_slot_header*>(page);
                    if (hdr->type == node_type::leaf) {
                        leaf_page_reader reader;
                        reader.parse(page, s.page_size);
                        auto rec = reader.find(e.key);
                        if (!rec.has_value())
                            e.result = lookup_absent{};
                        else if (rec->kind == record_kind::value)
                            e.result = lookup_value{rec->data_ver, rec->vr};
                        else
                            e.result = lookup_tombstone{rec->data_ver};
                        e.resolved = true;
                        break;
                    }

                    internal_page_reader reader;
                    reader.parse(page, s.page_size);
                    paddr child_base = reader.find_child(e.key);
                    e.next_page = s.manifest->resolve(child_base);
                }
            }

            s.all_done = true;
            for (auto& e : s.entries)
                if (!e.resolved) { s.all_done = false; break; }
        }

        void prepare_reads(lookup_state& s, decision_need_read& decision) {
            for (auto& e : s.entries) {
                if (e.resolved) continue;
                if (page_cache_.contains(make_tree_frame_id(e.next_page, s.page_lbas))) continue;
                if (inflight_reads_.contains(e.next_page)) continue;

                memory::page_frame* pf;
                if (!free_frames_.empty()) {
                    pf = free_frames_.back();
                    free_frames_.pop_back();
                    pf->id = make_tree_frame_id(e.next_page, s.page_lbas);
                    pf->st = memory::frame_state::clean_readonly;
                    pf->pin_count = 0;
                    pf->crc_valid = false;
                } else {
                    pf = new memory::page_frame{
                        .id       = make_tree_frame_id(e.next_page, s.page_lbas),
                        .st       = memory::frame_state::clean_readonly,
                        .buf      = new char[s.page_size],
                        .byte_len = s.page_size,
                        .pin_count = 0,
                        .crc_valid = false,
                    };
                    all_frames_.push_back(pf);
                }

                inflight_reads_.emplace(e.next_page, pending_read{});
                decision.frames.push_back(pf);
                decision.read_descs.push_back({
                    .lba      = e.next_page.lba,
                    .buf      = pf->buf,
                    .num_lbas = s.page_lbas,
                });
            }
        }
    };

    // ── Deferred definitions ──

    inline auto
    tree_lookup_sched_base::process(lookup_state& state) {
        // H-1 (review round 2): compare geometry by value, not by
        // pointer identity. The original invariant "every manifest
        // must point at exactly the same tree_geometry object this
        // scheduler was built with" was strictly stronger than the
        // design requirement and broke test fixtures / future
        // recovery paths that own their own geometry carrier. The
        // real correctness condition is that the three numeric
        // fields agree — that is enough to keep free-frame buffer
        // reuse safe, because every manifest will then drive the
        // same page_size / page_lbas.
        if (*state.manifest->geom != *expected_geom) {
            core::panic_inconsistency(
                "tree::tree_lookup_sched_base::process",
                "manifest geometry disagrees with scheduler expected_geom "
                "(rdi=%u, manifest={lba=%u,page=%u,slots=%u}, "
                "expected={lba=%u,page=%u,slots=%u})",
                static_cast<unsigned>(read_domain_index),
                static_cast<unsigned>(state.manifest->geom->lba_size),
                static_cast<unsigned>(state.manifest->geom->tree_page_size),
                static_cast<unsigned>(state.manifest->geom->shadow_slots_per_range),
                static_cast<unsigned>(expected_geom->lba_size),
                static_cast<unsigned>(expected_geom->tree_page_size),
                static_cast<unsigned>(expected_geom->shadow_slots_per_range));
        }
        return _tree_lookup::sender{this, &state};
    }

    inline auto
    tree_lookup_sched_base::submit_cache(std::vector<memory::page_frame*> frames) {
        return _cache_pages::sender{this, std::move(frames)};
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _tree_lookup::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_lookup(new _tree_lookup::req{
            state,
            [ctx = ctx, scope = scope](batch_decision&& d) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(d));
            }
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void _cache_pages::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_cache(new _cache_pages::req{
            std::move(frames),
            [ctx = ctx, scope = scope](bool ok) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, ok);
            }
        });
    }
}

// ── PUMP specializations ──

namespace pump::core {

    // process op_pusher
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::tree_lookup_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::tree::_tree_lookup::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::batch_decision>{};
        }
    };

    // cache_pages op_pusher
    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::cache_pages_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::tree::_cache_pages::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<bool>{};
        }
    };
}

#endif //APPS_INCONEL_TREE_LOOKUP_SCHEDULER_HH
