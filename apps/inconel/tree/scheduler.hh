#ifndef APPS_INCONEL_TREE_SCHEDULER_HH
#define APPS_INCONEL_TREE_SCHEDULER_HH

#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"

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
        bool first_call = true;
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

    // ── Forward declare scheduler ──

    struct lookup_scheduler;

    // ── process sender (lookup request) ──

    namespace _tree_lookup {

        struct req {
            lookup_state* state;
            std::move_only_function<void(batch_decision&&)> cb;
            req* next = nullptr;  // intrusive waiter list
        };

        struct op {
            constexpr static bool tree_lookup_op = true;
            lookup_scheduler* sched;
            lookup_state* state;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            lookup_scheduler* sched;
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
            lookup_scheduler* sched;
            std::vector<std::pair<paddr, char*>> page_map;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            lookup_scheduler* sched;
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

    struct lookup_scheduler {
        pump::core::per_core::queue<_tree_lookup::req*> lookup_queue_;
        pump::core::per_core::queue<_cache_pages::req*> cache_queue_;

        std::unordered_map<paddr, const char*> page_cache_;   // ready pages
        std::unordered_set<paddr> loading_pages_;              // reads in-flight
        std::vector<std::unique_ptr<char[]>> cache_bufs_;
        _tree_lookup::req* waiters_head_ = nullptr;             // intrusive waiter list

        explicit
        lookup_scheduler(size_t depth = 2048)
            : lookup_queue_(depth), cache_queue_(depth) {}

        void schedule_lookup(_tree_lookup::req* r) { lookup_queue_.try_enqueue(r); }
        void schedule_cache(_cache_pages::req* r) { cache_queue_.try_enqueue(r); }

        bool advance() {
            bool a = cache_queue_.drain([this](_cache_pages::req* r) {
                for (auto& [addr, buf] : r->page_map) {
                    page_cache_[addr] = buf;
                    loading_pages_.erase(addr);
                }
                r->cb(true);
                delete r;

                // Wake waiters that can progress (page cached or not loading)
                _tree_lookup::req* still_waiting = nullptr;
                auto* cur = waiters_head_;
                waiters_head_ = nullptr;
                while (cur) {
                    auto* next = cur->next;
                    cur->next = nullptr;
                    bool can_progress = false;
                    for (auto& e : cur->state->entries) {
                        if (e.resolved) continue;
                        if (!loading_pages_.count(e.next_page)) {
                            can_progress = true;
                            break;
                        }
                    }
                    if (can_progress) {
                        lookup_queue_.try_enqueue(cur);
                    } else {
                        cur->next = still_waiting;
                        still_waiting = cur;
                    }
                    cur = next;
                }
                waiters_head_ = still_waiting;
            });
            bool b = lookup_queue_.drain([this](_tree_lookup::req* r) { handle(r); });
            return a || b;
        }

        template<typename runtime_t>
        bool advance(runtime_t&) { return advance(); }

        auto process(lookup_state& state) {
            return _tree_lookup::sender{this, &state};
        }

        auto submit_cache(std::vector<std::pair<paddr, char*>> page_map) {
            return _cache_pages::sender{this, std::move(page_map)};
        }

    private:
        void handle(_tree_lookup::req* r) {
            auto& s = *r->state;

            if (s.first_call) {
                s.first_call = false;
                if (!s.manifest->has_root()) {
                    s.all_done = true;
                    r->cb(decision_done{});
                    delete r;
                    return;
                }
            }

            process_entries(s);

            if (s.all_done) {
                r->cb(decision_done{});
                delete r;
            } else {
                decision_need_read decision;
                prepare_reads(s, decision);
                if (!decision.read_descs.empty()) {
                    r->cb(std::move(decision));
                    delete r;
                } else {
                    // All needed pages are loading — wait for cache
                    r->next = waiters_head_;
                    waiters_head_ = r;
                }
            }
        }

        void process_entries(lookup_state& s) {
            for (auto& e : s.entries) {
                if (e.resolved) continue;

                while (true) {
                    auto it = page_cache_.find(e.next_page);
                    if (it == page_cache_.end()) break;

                    const char* page = it->second;
                    if (!tree_page_validate(page, s.page_size)) {
                        e.result = lookup_absent{};
                        e.resolved = true;
                        break;
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
                if (page_cache_.count(e.next_page)) continue;
                if (loading_pages_.count(e.next_page)) continue;

                cache_bufs_.push_back(std::make_unique<char[]>(s.page_size));
                char* buf = cache_bufs_.back().get();
                loading_pages_.insert(e.next_page);
                decision.page_map.emplace_back(e.next_page, buf);
                decision.read_descs.push_back({
                    .lba = e.next_page.lba,
                    .buf = buf,
                    .num_lbas = s.page_lbas,
                });
            }
        }
    };

    // ── Deferred op::start() definitions (need complete lookup_scheduler) ──

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
