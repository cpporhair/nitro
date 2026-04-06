#ifndef APPS_INCONEL_TREE_SCHEDULER_HH
#define APPS_INCONEL_TREE_SCHEDULER_HH

#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
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
        uint32_t page_size;
        uint32_t page_lbas;
        const core::tree_manifest* manifest;

        std::unordered_map<paddr, const char*> loaded;
        std::vector<std::unique_ptr<char[]>> bufs;
        std::vector<format::read_desc> read_descs;
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

    // ── req / op / sender ──

    template<typename nvme_sched_t>
    struct lookup_scheduler;

    namespace _tree_lookup {

        struct req {
            lookup_state* state;
            std::move_only_function<void(bool)> cb;
        };

        template<typename sched_t>
        struct op {
            constexpr static bool tree_lookup_op = true;
            sched_t* sched;
            lookup_state* state;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope) {
                sched->schedule(new req{
                    state,
                    [ctx = ctx, scope = scope](bool ok) mutable {
                        pump::core::op_pusher<pos + 1, scope_t>::push_value(
                            ctx, scope, ok);
                    }
                });
            }
        };

        template<typename sched_t>
        struct sender {
            sched_t* sched;
            lookup_state* state;

            auto make_op() {
                return op<sched_t>{.sched = sched, .state = state};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>().push_back(make_op());
            }
        };
    }

    // ── Scheduler ──

    template<typename nvme_sched_t>
    struct lookup_scheduler {
        nvme_sched_t* nvme;
        pump::core::per_core::queue<_tree_lookup::req*> req_queue;

        explicit
        lookup_scheduler(nvme_sched_t* nvme_s, size_t depth = 2048)
            : nvme(nvme_s), req_queue(depth) {}

        void schedule(_tree_lookup::req* r) { req_queue.try_enqueue(r); }

        bool advance() {
            return req_queue.drain([this](_tree_lookup::req* r) { handle(r); });
        }

        template<typename runtime_t>
        bool advance(runtime_t&) { return advance(); }

        auto process(lookup_state& state) {
            return _tree_lookup::sender<lookup_scheduler>{this, &state};
        }

    private:
        void handle(_tree_lookup::req* r) {
            auto& s = *r->state;

            if (s.first_call) {
                s.first_call = false;
                if (!s.manifest->has_root()) {
                    s.all_done = true;
                    s.read_descs.clear();
                } else {
                    prepare_reads(s);
                }
                r->cb(true);
                delete r;
                return;
            }

            // Push all unresolved keys as far as loaded pages allow
            for (auto& e : s.entries) {
                if (e.resolved) continue;

                while (true) {
                    auto it = s.loaded.find(e.next_page);
                    if (it == s.loaded.end()) break;

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

            if (s.all_done)
                s.read_descs.clear();
            else
                prepare_reads(s);

            r->cb(true);
            delete r;
        }

        static void prepare_reads(lookup_state& s) {
            std::unordered_map<paddr, size_t> page_index;
            for (auto& e : s.entries) {
                if (e.resolved) continue;
                if (page_index.count(e.next_page)) continue;
                page_index[e.next_page] = s.bufs.size();
                s.bufs.push_back(std::make_unique<char[]>(s.page_size));
            }

            s.read_descs.clear();
            s.loaded.clear();
            for (auto& [addr, idx] : page_index) {
                char* buf = s.bufs[idx].get();
                s.loaded[addr] = buf;
                s.read_descs.push_back({
                    .lba = addr.lba,
                    .buf = buf,
                    .num_lbas = s.page_lbas,
                });
            }
        }
    };
}

// ── PUMP specializations ──

namespace pump::core {
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

    template<typename ctx_t, typename sched_t>
    struct
    compute_sender_type<ctx_t, apps::inconel::tree::_tree_lookup::sender<sched_t>> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<bool>{};
        }
    };
}

#endif //APPS_INCONEL_TREE_SCHEDULER_HH
