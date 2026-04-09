#ifndef APPS_INCONEL_TREE_SCHEDULER_HH
#define APPS_INCONEL_TREE_SCHEDULER_HH

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
#include "../format/tree_page.hh"
#include "./lookup.hh"
#include "./page_reader.hh"

namespace apps::inconel::tree {

    using namespace format;

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
        lookup_state s;
        s.page_size = manifest->tree_page_size;
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
        pump::core::per_core::queue<_tree_lookup::req*> lookup_queue_;
        pump::core::per_core::queue<_cache_pages::req*> cache_queue_;

        explicit
        tree_lookup_sched_base(size_t depth)
            : lookup_queue_(depth), cache_queue_(depth) {}

        void schedule_lookup(_tree_lookup::req* r) { lookup_queue_.try_enqueue(r); }
        void schedule_cache(_cache_pages::req* r)  { cache_queue_.try_enqueue(r); }

        // Sender constructors — they only need the base pointer; the cache
        // type is irrelevant to pipeline composition.
        auto process(lookup_state& state);
        auto submit_cache(std::vector<std::pair<paddr, char*>> page_map);
    };

    // ── process sender (lookup request) ──
    //
    // op/sender are non-templated; they only need access to enqueue methods
    // exposed by tree_lookup_sched_base. The cache type is irrelevant here.

    namespace _tree_lookup {

        struct req {
            lookup_state* state;
            std::move_only_function<void(batch_decision&&)> cb;
            uint32_t wait_gen = 0;       // bumped each time req is parked
            bool     wake_enqueued = false;  // set when re-queued, cleared on dispatch
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
            std::vector<std::pair<paddr, char*>> page_map;
            std::move_only_function<void(bool)> cb;
        };

        struct op {
            constexpr static bool cache_pages_op = true;
            tree_lookup_sched_base* sched;
            std::vector<std::pair<paddr, char*>> page_map;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_lookup_sched_base* sched;
            std::vector<std::pair<paddr, char*>> page_map;

            auto make_op() {
                return op{.sched = sched, .page_map = std::move(page_map)};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    // ── Scheduler ──
    //
    // Templated on the cache type. The Cache satisfies core::cache_concept,
    // so all get/put/contains calls are inlined directly — no virtual
    // dispatch, no variant visit, no abstraction overhead.

    template <core::cache_concept Cache>
    struct tree_lookup_sched : tree_lookup_sched_base {
        // Per-page single-flight bookkeeping. A wait_token records which
        // generation of a req registered to wait for a given page; the
        // generation lets stale registrations be filtered at completion
        // time without having to walk every other inflight entry the req
        // is also waiting on.
        struct wait_token {
            _tree_lookup::req* req;
            uint32_t           wait_gen;
        };

        struct pending_read {
            std::vector<wait_token> waiters;
        };

        Cache page_cache_;
        absl::flat_hash_map<paddr, pending_read> inflight_reads_;  // per-page single-flight
        std::vector<std::unique_ptr<char[]>> owned_bufs_;          // buffer ownership pool
        std::vector<char*> free_bufs_;                             // recyclable evicted buffers

        // INC-029 — bounded per-advance work budget. Without a cap, a single
        // advance() invocation could drain the entire queue, starving other
        // schedulers and producing tail latency proportional to queue depth.
        // Constants are private on purpose: this is a workload-shaping knob,
        // not a runtime/build configuration surface. Cache completions and
        // lookup requests get separate budgets so a hot lookup stream can't
        // crowd out cache fills (or vice versa) within the same advance().
        static constexpr uint32_t kMaxCacheOpsPerAdvance  = 64;
        static constexpr uint32_t kMaxLookupOpsPerAdvance = 64;

        explicit
        tree_lookup_sched(Cache cache, size_t depth = 2048)
            : tree_lookup_sched_base(depth)
            , page_cache_(std::move(cache)) {}

        bool advance() {
            bool progress = false;

            // Cache completions first: installing fresh pages may unblock
            // waiters registered against those exact pages. For each page
            // in the completed batch we install it into the cache, pop the
            // matching pending_read entry, and re-queue every still-valid
            // waiter exactly once (guarded by wait_gen + wake_enqueued).
            for (uint32_t i = 0; i < kMaxCacheOpsPerAdvance; ++i) {
                auto item = cache_queue_.try_dequeue();
                if (!item) break;
                _cache_pages::req* r = *item;

                for (auto& [addr, buf] : r->page_map) {
                    auto evicted = page_cache_.put(addr, buf);
                    if (evicted.has_value())
                        free_bufs_.push_back(evicted->buf);

                    // prepare_reads() emplaces an inflight_reads_ entry
                    // for every read it actually issues, so a completion
                    // landing here without a matching slot is an internal
                    // bookkeeping break, not a benign race. Fail loudly
                    // instead of silently dropping the wakeup, otherwise
                    // any req parked on this page would hang forever.
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
            // This dispatch consumes whatever wake credit we had; any
            // re-park below must bump wait_gen and re-arm wake_enqueued
            // through the inflight_reads_ path.
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

            // No new reads to issue: every unresolved entry's next_page
            // is already in flight (or just landed). Park this req on
            // each such page under a fresh wait_gen so that any one of
            // them can re-queue us exactly once.
            r->wait_gen += 1;

            absl::flat_hash_set<paddr> wait_pages;
            for (auto& e : s.entries) {
                if (e.resolved) continue;
                if (page_cache_.contains(e.next_page)) continue;
                wait_pages.insert(e.next_page);
            }

            if (wait_pages.empty()) {
                // process_entries() saw no resolvable entry but every
                // unresolved entry's next_page is already cached — that
                // means the cache claims to have a page we somehow could
                // not consume. There is no consistent recovery from this.
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
                    const char* page = page_cache_.get(e.next_page);
                    if (page == nullptr) break;

                    auto status = inspect_tree_page(page, s.page_size);
                    if (status != tree_page_status::ok) {
                        // Corruption detected on a page we already
                        // believed cached — there is no semantically
                        // sound recovery from "the on-disk B+tree is
                        // wrong", so we abort with enough context to
                        // identify the offending slot and its failure
                        // mode rather than silently turning it into
                        // lookup_absent.
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
                if (page_cache_.contains(e.next_page)) continue;

                // Single-flight: if any prior req already issued a read
                // for this page, skip — we will park on its pending_read
                // when handle() runs out of new reads to schedule.
                if (inflight_reads_.contains(e.next_page)) continue;

                char* buf;
                if (!free_bufs_.empty()) {
                    buf = free_bufs_.back();
                    free_bufs_.pop_back();
                } else {
                    owned_bufs_.push_back(std::make_unique<char[]>(s.page_size));
                    buf = owned_bufs_.back().get();
                }
                inflight_reads_.emplace(e.next_page, pending_read{});
                decision.page_map.emplace_back(e.next_page, buf);
                decision.read_descs.push_back({
                    .lba = e.next_page.lba,
                    .buf = buf,
                    .num_lbas = s.page_lbas,
                });
            }
        }
    };

    // ── Deferred definitions (need complete sender types) ──

    inline auto
    tree_lookup_sched_base::process(lookup_state& state) {
        return _tree_lookup::sender{this, &state};
    }

    inline auto
    tree_lookup_sched_base::submit_cache(std::vector<std::pair<paddr, char*>> page_map) {
        return _cache_pages::sender{this, std::move(page_map)};
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
            std::move(page_map),
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

#endif //APPS_INCONEL_TREE_SCHEDULER_HH
