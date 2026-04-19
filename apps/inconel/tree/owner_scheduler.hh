#ifndef APPS_INCONEL_TREE_OWNER_SCHEDULER_HH
#define APPS_INCONEL_TREE_OWNER_SCHEDULER_HH

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/context.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/meta.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/just.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "../core/data_area_heads.hh"
#include "../core/panic.hh"
#include "../format/superblock.hh"
#include "../format/types.hh"
#include "../mock_nvme/sender.hh"
#include "./flush_round_state.hh"
#include "./flush_types.hh"
#include "./memtable_fold.hh"
#include "./page_builder.hh"
#include "./page_reader.hh"

namespace apps::inconel::tree {

    struct reclaim_task;
    struct tree_sched;

}  // namespace apps::inconel::tree

namespace apps::inconel::core::registry {
    inline mock_nvme::scheduler* local_nvme();
}

namespace apps::inconel::tree {
    struct tree_allocator {
        format::paddr                         head{};
        pump::core::local::queue<format::range_ref> free_ranges{4096};
        core::data_area_heads*               shared_heads = nullptr;
        uint32_t                             range_lbas   = 0;
        uint32_t                             shadow_slots = 0;

        format::range_ref
        allocate() {
            if (auto reused = free_ranges.try_dequeue()) {
                return *reused;
            }

            const auto next_end_lba = head.lba + range_lbas;
            if (shared_heads != nullptr) {
                const auto value_low =
                    shared_heads->value_head_lba.load(std::memory_order_relaxed);
                if (next_end_lba > value_low) {
                    core::panic_inconsistency(
                        "tree_allocator::allocate",
                        "data area exhausted (tree_next_end=%lu value_low=%lu)",
                        static_cast<unsigned long>(next_end_lba),
                        static_cast<unsigned long>(value_low));
                }
            }

            auto out = format::range_ref{
                .base       = head,
                .slot_count = shadow_slots,
            };
            head.lba = next_end_lba;
            if (shared_heads != nullptr) {
                shared_heads->tree_head_lba.store(
                    head.lba, std::memory_order_relaxed);
            }
            return out;
        }

        void
        push_back_bump(format::range_ref r) {
            if (r.base.lba + range_lbas != head.lba) {
                core::panic_inconsistency(
                    "tree_allocator::push_back_bump",
                    "range is not the last bumped range "
                    "(range_end=%lu head=%lu)",
                    static_cast<unsigned long>(r.base.lba + range_lbas),
                    static_cast<unsigned long>(head.lba));
            }
            head.lba = r.base.lba;
            if (shared_heads != nullptr) {
                shared_heads->tree_head_lba.store(
                    head.lba, std::memory_order_relaxed);
            }
        }

        void
        recycle(format::range_ref r) {
            free_ranges.try_enqueue(std::move(r));
        }
    };

    // ── merge coroutine status codes (yielded by run_merge) ──────
    //
    // The yield is a pure status tag: the coroutine stages the
    // actual read/write descriptors into `merge_round_state::pending_*`
    // before co_yield'ing, and the `submit_merge_step` handler
    // moves them out into the decision payload. See `run_merge()`.
    enum class merge_yield : uint8_t {
        need_io,    // pending_reads / pending_writes staged; outer pipeline must drive them
        yield_cpu,  // no IO, but CPU has been on-shift too long; let main loop breathe
        done,       // all CPU done; all emitted writes are in flight/complete
    };

    // Custom C++20 coroutine type: move-only handle, manual resume.
    // Not bound to any PUMP sender — the scheduler's `advance()`
    // drives it synchronously via `resume()`.
    struct merge_coro {
        struct promise_type {
            merge_yield              current_value = merge_yield::done;
            std::exception_ptr       unhandled{};

            merge_coro
            get_return_object() {
                return merge_coro{
                    std::coroutine_handle<promise_type>::from_promise(*this)
                };
            }
            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend()  noexcept { return {}; }
            void return_void() noexcept {}
            void unhandled_exception() noexcept {
                unhandled = std::current_exception();
            }
            std::suspend_always yield_value(merge_yield v) noexcept {
                current_value = v;
                return {};
            }
        };
        using handle_t = std::coroutine_handle<promise_type>;

        explicit merge_coro(handle_t h_) noexcept : h(h_) {}
        merge_coro(const merge_coro&) = delete;
        merge_coro& operator=(const merge_coro&) = delete;
        merge_coro(merge_coro&& o) noexcept
            : h(std::exchange(o.h, {})) {}
        merge_coro& operator=(merge_coro&& o) noexcept {
            if (h) h.destroy();
            h = std::exchange(o.h, {});
            return *this;
        }
        ~merge_coro() { if (h) h.destroy(); }

        bool done() const noexcept { return h.done(); }
        void resume() {
            h.resume();
            if (h.done() && h.promise().unhandled) {
                std::rethrow_exception(h.promise().unhandled);
            }
        }
        merge_yield current() const noexcept {
            return h.promise().current_value;
        }

    private:
        handle_t h;
    };

    namespace _owner { struct merge_context; }

    // Held in `tree_state.active_merge` for the lifetime of one
    // flush round's merge phase (single-flush invariant — only
    // one of these exists at a time). The coroutine yields at
    // io / cpu-budget boundaries; `submit_merge_step` resumes it
    // and translates the yield into a seam decision.
    //
    // Ownership map:
    //   - `worker_proposals` owns every `mem_tree_node` the workers
    //     produced; `combined_root` ends up pointing into those
    //     (via `child_ref::target`) after merge.
    //   - `fetched_old_pages[rb]` is the landing buffer the outer
    //     pipeline DMAs into for owner-side reads of shared ancestors;
    //     the coroutine re-reads them in place on resume.
    //   - `pending_ios[i]` is either `format::read_desc` whose buf
    //     points into `fetched_old_pages[rb].data()`, or
    //     `format::write_desc` whose data points into mem_tree_node
    //     content owned by `combined_root`. The seam handler moves
    //     the vector out into a `merge_step_need_io` decision
    //     unchanged — no per-kind bucketing.
    //   - `retired_slots_seen` / `retired_ranges_seen` dedupe inserts
    //     into the linked `flush_round_state.retired` vectors.
    struct merge_round_state {
        flush_round_id                                round_id;
        std::vector<worker_tree_proposal>             worker_proposals;

        // shared_ptr (instead of optional) so `_owner::merge_context`
        // can be forward-declared here — the full definition lives
        // deeper in the `_owner` namespace below. Only one holder
        // exists at a time; not actually shared.
        std::shared_ptr<_owner::merge_context>        ctx;
        absl::flat_hash_map<paddr, std::vector<char>> fetched_old_pages;

        child_ref                                     combined_root;
        std::shared_ptr<const core::tree_manifest>    new_manifest;
        bool                                          is_root_change = false;
        paddr                                         new_root_base_paddr{};
        std::vector<format::range_ref>                allocated_ranges;
        absl::flat_hash_set<paddr>                    retired_slots_seen;
        absl::flat_hash_set<paddr>                    retired_ranges_seen;

        // Post-order walk cursor for the assign+emit phase. Pointers
        // into `combined_root` are stable because we only mutate
        // node fields in place (new_paddr / content / etc.), never
        // restructure the tree during walking.
        struct walk_frame {
            child_ref*  ref         = nullptr;
            std::size_t next_child  = 0;
        };
        std::vector<walk_frame>                       walk_stack;

        // Coroutine ↔ scheduler handshake slot. A single unified
        // batch of NVMe ops — reads and writes interleave freely in
        // one vector so the outer pipeline can fire them all as one
        // `concurrent` stream instead of serializing by kind. Staged
        // by the coroutine before co_yield'ing need_io; the seam
        // handler moves it out into the returned decision.
        std::vector<merge_io_desc>                    pending_ios;

        // Single-flush / single-core: plain bool, no atomics.
        // Set to true by the seam handler whenever it emits a batch
        // that contains at least one read (its buffer gets DMA'd
        // by the outer pipeline and the next coroutine resume reads
        // it in place); cleared by `submit_merge_reads_done` after
        // the outer pipeline awaits the batch.
        bool                                          waiting_for_reads = false;

        flush_stage_status                            st = flush_stage_status::ok;

        std::optional<merge_coro>                     coro;
    };

    struct tree_state {
        tree_allocator alloc;
        uint64_t       flush_max_lsn       = 0;
        uint64_t       superblock_safe_lsn = 0;
        uint64_t       recovery_safe_lsn   = 0;
        superblock_slot active_superblock_slot = superblock_slot::A;

        pump::core::per_core::queue<reclaim_task*> reclaim_q{256};

        absl::flat_hash_map<uint64_t, std::unique_ptr<flush_round_state>>
            active_rounds;

        // Single-flush invariant: at most one merge phase in flight
        // at a time. Set by `submit_merge_step(start)`, cleared by
        // `submit_finalize_merge`.
        std::optional<merge_round_state> active_merge;

        uint64_t next_round_id = 1;
    };

    static inline auto
    build_flushed_gens_by_front(
        const absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>& gens)
    {
        absl::flat_hash_map<uint32_t,
                            absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>>
            result;
        for (const auto& g : gens) {
            if (g->front_owner_index == UINT32_MAX) {
                core::panic_inconsistency(
                    "build_flushed_gens_by_front",
                    "memtable_gen.front_owner_index not initialized");
            }
            result[g->front_owner_index].push_back(g);
        }
        return result;
    }

    namespace _flush_fold {

        struct req {
            tree_flush_request                                 args;
            std::move_only_function<void(flush_fold_result&&)> cb;
        };

        struct op {
            constexpr static bool flush_fold_op = true;

            tree_sched*        sched;
            tree_flush_request  args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*        sched;
            tree_flush_request  args;

            auto
            make_op() {
                return op{ .sched = sched, .args = std::move(args) };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _flush_fold

    namespace _merge_step {

        struct req {
            merge_loop_state*                                    ls;
            std::move_only_function<void(merge_step_decision&&)> cb;
        };

        struct op {
            constexpr static bool merge_step_op = true;

            tree_sched*        sched;
            merge_loop_state*  ls;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*        sched;
            merge_loop_state*  ls;

            auto
            make_op() {
                return op{ .sched = sched, .ls = ls };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _merge_step

    namespace _merge_reads_done {

        struct req {
            std::move_only_function<void()> cb;
        };

        struct op {
            constexpr static bool merge_reads_done_op = true;

            tree_sched* sched;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched* sched;

            auto
            make_op() {
                return op{ .sched = sched };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _merge_reads_done

    namespace _finalize_merge {

        struct req {
            merge_finalize_request                                args;
            std::move_only_function<void(merge_finalize_result&&)> cb;
        };

        struct op {
            constexpr static bool finalize_merge_op = true;

            tree_sched*            sched;
            merge_finalize_request args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*            sched;
            merge_finalize_request args;

            auto
            make_op() {
                return op{ .sched = sched, .args = std::move(args) };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _finalize_merge

    namespace _begin_update_superblock {

        struct req {
            update_superblock_request                                       args;
            std::move_only_function<void(begin_update_superblock_result&&)> cb;
        };

        struct op {
            constexpr static bool begin_update_superblock_op = true;

            tree_sched*                sched;
            update_superblock_request  args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*                sched;
            update_superblock_request  args;

            auto
            make_op() {
                return op{ .sched = sched, .args = std::move(args) };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _begin_update_superblock

    namespace _finish_update_superblock {

        struct req {
            finish_update_superblock_request                         args;
            std::move_only_function<void(update_superblock_result&&)> cb;
        };

        struct op {
            constexpr static bool finish_update_superblock_op = true;

            tree_sched*                       sched;
            finish_update_superblock_request  args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*                       sched;
            finish_update_superblock_request  args;

            auto
            make_op() {
                return op{ .sched = sched, .args = std::move(args) };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _finish_update_superblock

    namespace _finalize_flush_round {

        struct req {
            finalize_flush_request                              args;
            std::move_only_function<void(tree_flush_result&&)> cb;
        };

        struct op {
            constexpr static bool finalize_flush_round_op = true;

            tree_sched*             sched;
            finalize_flush_request  args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*             sched;
            finalize_flush_request  args;

            auto
            make_op() {
                return op{ .sched = sched, .args = std::move(args) };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _finalize_flush_round

    namespace _owner {

        using format::paddr;

        struct proposal_group_view {
            uint32_t                         worker_index = 0;
            absl::InlinedVector<paddr, 2>    covered_old_paddrs;
            std::vector<const mem_tree_node*> nodes;
            std::vector<std::string_view>    sibling_seps;
        };

        struct merged_group {
            std::vector<std::unique_ptr<mem_tree_node>> nodes;
            std::vector<std::string>                    sibling_seps;
        };

        struct final_leaf_item {
            paddr                   range_base{};
            bool                    is_new = false;
            const mem_tree_node*    new_leaf = nullptr;
            const core::leaf_span*  base_span = nullptr;
            std::string_view        first_key;
        };

        struct merge_context {
            const core::tree_manifest* base_manifest = nullptr;
            const core::tree_geometry* geom = nullptr;
            const std::vector<worker_tree_proposal>* proposals = nullptr;

            // Non-owning view of the owner-side read landing zone
            // (lives in `merge_round_state::fetched_old_pages`). When
            // `touched_old_page_for` can't find the shared-ancestor
            // internal page bytes in any contrib's `touched_old_pages`,
            // it falls back here before panicking. The pre-scan pass
            // in `run_merge` populates the buffers up front; the NVMe
            // read path DMAs bytes directly into them.
            const absl::flat_hash_map<paddr, std::vector<char>>*
                fetched_old_pages = nullptr;

            std::vector<proposal_group_view> group_storage;
            absl::flat_hash_map<paddr, std::vector<const proposal_group_view*>>
                contrib_index;
            absl::flat_hash_map<paddr, std::unique_ptr<merged_group>> merged_cache;

            absl::flat_hash_map<paddr, uint32_t> base_leaf_index_by_range;
            absl::flat_hash_map<paddr, uint32_t> base_internal_index_by_range;

            struct subtree_interval {
                uint32_t begin = UINT32_MAX;
                uint32_t end   = 0;
            };
            std::vector<subtree_interval> base_internal_leaf_ranges;
            std::vector<std::vector<uint32_t>> base_internal_children;
        };

        static inline std::unique_ptr<mem_tree_node>
        clone_node(const mem_tree_node* src) {
            auto dst = std::make_unique<mem_tree_node>();
            dst->type               = src->type;
            dst->content            = src->content;
            dst->replaces_old_paddrs = src->replaces_old_paddrs;
            dst->separators         = src->separators;
            dst->new_range_base     = src->new_range_base;
            dst->new_slot_index     = src->new_slot_index;
            dst->new_paddr          = src->new_paddr;
            dst->children.reserve(src->children.size());
            for (const auto& child : src->children) {
                if (auto* p = std::get_if<paddr>(&child.target)) {
                    dst->children.push_back(child_ref{ .target = *p });
                } else {
                    dst->children.push_back(child_ref{
                        .target = clone_node(
                            std::get<std::unique_ptr<mem_tree_node>>(child.target).get()),
                    });
                }
            }
            return dst;
        }

        static inline merged_group
        clone_group_view(const proposal_group_view& view) {
            merged_group out;
            out.nodes.reserve(view.nodes.size());
            out.sibling_seps.reserve(view.sibling_seps.size());
            for (auto* n : view.nodes) {
                out.nodes.push_back(clone_node(n));
            }
            for (auto sep : view.sibling_seps) {
                out.sibling_seps.emplace_back(sep);
            }
            return out;
        }

        static inline merged_group
        clone_group(const merged_group& src) {
            merged_group out;
            out.nodes.reserve(src.nodes.size());
            out.sibling_seps = src.sibling_seps;
            for (const auto& n : src.nodes) {
                out.nodes.push_back(clone_node(n.get()));
            }
            return out;
        }

        static inline bool
        is_leaf_range(const merge_context& ctx, paddr rb) {
            return ctx.base_leaf_index_by_range.contains(rb);
        }

        static inline bool
        is_internal_range(const merge_context& ctx, paddr rb) {
            return ctx.base_internal_index_by_range.contains(rb);
        }

        static inline void
        append_group_to_children(
            merged_group&&            group,
            std::vector<child_ref>&   children,
            std::vector<std::string>& separators)
        {
            for (std::size_t i = 0; i < group.nodes.size(); ++i) {
                if (i > 0) {
                    separators.push_back(std::move(group.sibling_seps[i - 1]));
                }
                children.push_back(child_ref{
                    .target = std::move(group.nodes[i]),
                });
            }
        }

        static inline absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1>
        build_internal_pages_owner(
            std::vector<child_ref>&&             new_children,
            std::vector<std::string>&&           new_separators,
            paddr                                replaces_old_paddr,
            bool                                 is_new_layer,
            uint32_t                             page_size,
            std::vector<std::string>&            sibling_seps_out)
        {
            sibling_seps_out.clear();

            absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1> result;
            if (new_children.empty()) {
                core::panic_inconsistency(
                    "build_internal_pages_owner",
                    "internal node has zero children");
            }

            auto child_to_paddr = [](const child_ref& c) -> paddr {
                if (auto* p = std::get_if<paddr>(&c.target)) return *p;
                if (auto* n = std::get_if<std::unique_ptr<mem_tree_node>>(&c.target)) {
                    return (*n)->new_paddr;
                }
                return {0, 0};
            };

            std::size_t next = 0;
            while (next < new_children.size()) {
                uint32_t used = sizeof(format::tree_slot_header);
                uint32_t K = 0;
                for (std::size_t j = next; j < new_children.size(); ++j) {
                    uint32_t add = 0;
                    if (K == 0) {
                        add = sizeof(format::paddr);
                    } else {
                        add = format::internal_record_size(
                                  static_cast<uint16_t>(
                                      new_separators[next + K - 1].size()))
                            + sizeof(uint16_t);
                    }
                    if (used + add > page_size) break;
                    used += add;
                    ++K;
                }
                if (K == 0) {
                    core::panic_inconsistency(
                        "build_internal_pages_owner",
                        "single child does not fit in page");
                }

                auto node = std::make_unique<mem_tree_node>();
                node->type = format::node_type::internal;
                if (result.empty() && !is_new_layer) {
                    node->replaces_old_paddrs.push_back(replaces_old_paddr);
                }
                node->content.resize(page_size);

                internal_page_builder builder;
                builder.init(node->content.data(), page_size);
                for (uint32_t j = 0; j + 1 < K; ++j) {
                    if (!builder.add_child(
                            new_separators[next + j],
                            child_to_paddr(new_children[next + j])))
                    {
                        core::panic_inconsistency(
                            "build_internal_pages_owner",
                            "add_child failed despite size precomputation");
                    }
                }
                builder.set_rightmost_child(child_to_paddr(new_children[next + K - 1]));
                builder.finalize();

                node->children.reserve(K);
                node->separators.reserve(K > 0 ? K - 1 : 0);
                for (uint32_t j = 0; j < K; ++j) {
                    node->children.push_back(std::move(new_children[next + j]));
                }
                for (uint32_t j = 0; j + 1 < K; ++j) {
                    node->separators.push_back(std::move(new_separators[next + j]));
                }
                result.push_back(std::move(node));

                if (next + K < new_children.size()) {
                    sibling_seps_out.push_back(std::move(new_separators[next + K - 1]));
                }
                next += K;
            }

            return result;
        }

        static inline void
        reformat_internal_node(mem_tree_node* node, uint32_t page_size) {
            internal_page_builder builder;
            builder.init(node->content.data(), page_size);
            for (std::size_t i = 0; i + 1 < node->children.size(); ++i) {
                paddr child_base = {0, 0};
                if (auto* p = std::get_if<paddr>(&node->children[i].target)) {
                    child_base = *p;
                } else {
                    child_base = std::get<std::unique_ptr<mem_tree_node>>(
                                     node->children[i].target)->new_paddr;
                }
                if (!builder.add_child(node->separators[i], child_base)) {
                    core::panic_inconsistency(
                        "reformat_internal_node",
                        "rebuild failed while rewriting internal content");
                }
            }
            paddr rightmost = {0, 0};
            if (auto* p = std::get_if<paddr>(&node->children.back().target)) {
                rightmost = *p;
            } else {
                rightmost = std::get<std::unique_ptr<mem_tree_node>>(
                                node->children.back().target)->new_paddr;
            }
            builder.set_rightmost_child(rightmost);
            builder.finalize();
        }

        static inline void
        build_base_indexes(merge_context& ctx) {
            const auto& lo = ctx.base_manifest->leaf_order;
            const auto& topo = ctx.base_manifest->reverse_topology;

            for (uint32_t i = 0; i < lo.spans.size(); ++i) {
                ctx.base_leaf_index_by_range.emplace(lo.spans[i].leaf_range_base, i);
            }
            ctx.base_internal_leaf_ranges.assign(
                topo.internal_nodes.size(), merge_context::subtree_interval{});
            ctx.base_internal_children.assign(topo.internal_nodes.size(), {});

            for (uint32_t i = 0; i < topo.internal_nodes.size(); ++i) {
                ctx.base_internal_index_by_range.emplace(
                    topo.internal_nodes[i].range_base, i);
                auto parent = topo.internal_nodes[i].parent_idx;
                if (parent != core::kInvalidInternalIdx) {
                    ctx.base_internal_children[parent].push_back(i);
                }
            }

            for (uint32_t leaf_idx = 0; leaf_idx < topo.leaf_parent_idx.size(); ++leaf_idx) {
                auto p = topo.leaf_parent_idx[leaf_idx];
                while (p != core::kInvalidInternalIdx) {
                    auto& iv = ctx.base_internal_leaf_ranges[p];
                    iv.begin = std::min(iv.begin, leaf_idx);
                    iv.end   = std::max(iv.end, leaf_idx + 1);
                    p = topo.internal_nodes[p].parent_idx;
                }
            }
        }

        static inline void
        add_group_view(merge_context& ctx, proposal_group_view&& view) {
            ctx.group_storage.push_back(std::move(view));
            auto* ptr = &ctx.group_storage.back();
            for (auto rb : ptr->covered_old_paddrs) {
                ctx.contrib_index[rb].push_back(ptr);
            }
        }

        static inline void
        index_node_children(merge_context& ctx,
                            uint32_t       worker_index,
                            const mem_tree_node* node);

        static inline void
        index_group_nodes(merge_context& ctx,
                          uint32_t       worker_index,
                          const proposal_group_view& group) {
            for (auto* n : group.nodes) {
                if (n->type == format::node_type::internal) {
                    index_node_children(ctx, worker_index, n);
                }
            }
        }

        static inline void
        index_node_children(merge_context& ctx,
                            uint32_t       worker_index,
                            const mem_tree_node* node)
        {
            std::size_t i = 0;
            while (i < node->children.size()) {
                auto* child_up = std::get_if<std::unique_ptr<mem_tree_node>>(
                    &node->children[i].target);
                if (child_up == nullptr) {
                    ++i;
                    continue;
                }

                auto* first = child_up->get();
                if (first->replaces_old_paddrs.empty()) {
                    core::panic_inconsistency(
                        "index_node_children",
                        "group start without covered old paddrs");
                }

                proposal_group_view view;
                view.worker_index = worker_index;
                view.covered_old_paddrs = first->replaces_old_paddrs;
                view.nodes.push_back(first);

                std::size_t j = i + 1;
                while (j < node->children.size()) {
                    auto* next_up = std::get_if<std::unique_ptr<mem_tree_node>>(
                        &node->children[j].target);
                    if (next_up == nullptr) break;
                    if (!next_up->get()->replaces_old_paddrs.empty()) break;
                    view.sibling_seps.push_back(node->separators[j - 1]);
                    view.nodes.push_back(next_up->get());
                    ++j;
                }

                add_group_view(ctx, std::move(view));
                index_group_nodes(ctx, worker_index, ctx.group_storage.back());
                i = j;
            }
        }

        static inline void
        index_root_group(merge_context& ctx,
                         uint32_t       worker_index,
                         const worker_tree_proposal& proposal)
        {
            if (proposal.root == nullptr) return;

            if (!proposal.root->replaces_old_paddrs.empty()) {
                proposal_group_view view;
                view.worker_index = worker_index;
                view.covered_old_paddrs = proposal.root->replaces_old_paddrs;
                view.nodes.push_back(proposal.root.get());
                add_group_view(ctx, std::move(view));
                index_group_nodes(ctx, worker_index, ctx.group_storage.back());
                return;
            }

            if (proposal.root->type != format::node_type::internal) {
                core::panic_inconsistency(
                    "index_root_group",
                    "root wrapper without old paddrs must be internal");
            }

            proposal_group_view view;
            view.worker_index = worker_index;
            view.covered_old_paddrs.push_back(ctx.base_manifest->root_range_base);
            for (std::size_t i = 0; i < proposal.root->children.size(); ++i) {
                auto* child_up = std::get_if<std::unique_ptr<mem_tree_node>>(
                    &proposal.root->children[i].target);
                if (child_up == nullptr) {
                    core::panic_inconsistency(
                        "index_root_group",
                        "root split wrapper contains paddr child");
                }
                view.nodes.push_back(child_up->get());
                if (i > 0) {
                    view.sibling_seps.push_back(proposal.root->separators[i - 1]);
                }
            }
            add_group_view(ctx, std::move(view));
            index_group_nodes(ctx, worker_index, ctx.group_storage.back());
        }

        // Forward declaration: `make_empty_root_leaf` is defined
        // further down (alongside the prune helpers) but the
        // bootstrap combine helper below needs it. Declaration-only
        // here keeps the bootstrap helpers grouped with the other
        // merge-context factories without reordering the unrelated
        // prune code.
        static inline child_ref
        make_empty_root_leaf(uint32_t page_size);

        static inline merge_context
        make_merge_context(const core::tree_manifest*            base_manifest,
                           const std::vector<worker_tree_proposal>* proposals)
        {
            merge_context ctx;
            ctx.base_manifest = base_manifest;
            ctx.geom          = base_manifest->geom;
            ctx.proposals     = proposals;
            build_base_indexes(ctx);
            for (uint32_t i = 0; i < proposals->size(); ++i) {
                index_root_group(ctx, i, (*proposals)[i]);
            }
            return ctx;
        }

        // Bootstrap-safe ctx factory. `index_root_group` assumes every
        // worker root either (a) replaces an old paddr recorded in the
        // base manifest, or (b) is an internal wrapper whose covered
        // paddr resolves to `base_manifest->root_range_base`. Neither
        // branch makes sense on an empty base_manifest (there is no
        // old root, and `root_range_base` is the zero sentinel), so the
        // bootstrap path SKIPS root-group indexing entirely and falls
        // back to `combine_worker_roots_for_bootstrap` to stitch the
        // worker-supplied subtrees together from scratch.
        //
        // `build_base_indexes` is still called because the Phase 4
        // manifest builders (`build_leaf_order_full`,
        // `build_reverse_topology_full`) rely on a populated ctx even
        // when all its hash maps end up empty — they only read the
        // lookup sides to classify paddr-ref children, and bootstrap
        // has none.
        static inline merge_context
        make_merge_context_bootstrap(
            const core::tree_manifest*               base_manifest,
            const std::vector<worker_tree_proposal>* proposals)
        {
            merge_context ctx;
            ctx.base_manifest = base_manifest;
            ctx.geom          = base_manifest->geom;
            ctx.proposals     = proposals;
            build_base_indexes(ctx);
            return ctx;
        }

        // First key reachable through a worker's root subtree. Used
        // for ordering worker roots and synthesizing the separator
        // key when the owner wraps multiple worker roots into a fresh
        // internal layer on the bootstrap path.
        //
        // Walks the leftmost child at each internal level until a
        // leaf is reached; returns that leaf's first record key.
        // Panics on internals that already hold a paddr-ref child —
        // bootstrap worker subtrees are mem_tree_node-only, so any
        // paddr slot here would indicate the caller mixed a
        // non-bootstrap subtree into the bootstrap path.
        static inline std::string_view
        first_key_of_worker_root(const mem_tree_node* n, uint32_t page_size)
        {
            while (n->type == format::node_type::internal) {
                if (n->children.empty()) {
                    core::panic_inconsistency(
                        "first_key_of_worker_root",
                        "bootstrap internal has zero children");
                }
                auto* first_child_up = std::get_if<std::unique_ptr<mem_tree_node>>(
                    &n->children.front().target);
                if (first_child_up == nullptr) {
                    core::panic_inconsistency(
                        "first_key_of_worker_root",
                        "bootstrap internal has a paddr-ref child — "
                        "bootstrap subtrees must be fully in memory");
                }
                n = first_child_up->get();
            }
            leaf_page_reader reader;
            if (!reader.parse(n->content.data(), page_size)) {
                core::panic_inconsistency(
                    "first_key_of_worker_root",
                    "bootstrap leaf failed validation");
            }
            if (reader.record_count() == 0) {
                // Empty leaf — the owner prune step will retire it;
                // callers that need a separator key must not reach
                // here (all usable leaves carry at least one record
                // by construction of build_leaves_from_sorted_keys).
                return std::string_view{};
            }
            return reader.get(0).key;
        }

        // Combine per-worker bootstrap roots (each either a single
        // leaf or a single-layer internal wrapping leaves) into a
        // single subtree rooted at the returned `child_ref`.
        //
        // Sort worker roots by first-key so the wrapped tree's
        // in-order traversal matches global key order (workers arrive
        // in shard_idx order from the fan-in collector, which is
        // already key-order under the current bootstrap
        // `shard_partition_map`, but we sort defensively in case a
        // future rebuild policy reorders shards).
        //
        // If only one worker contributed a non-null root, that root
        // is adopted as-is. Otherwise the worker roots become children
        // of a fresh internal layer; if the new layer itself overflows
        // a page (many workers / long separator keys), another fresh
        // layer is added on top until the layer collapses to a single
        // page — same structural rule the non-bootstrap
        // `finalize_root_group` uses.
        //
        // On an empty input (all workers produced null proposals, or
        // there were no workers) an empty leaf root is returned so the
        // downstream prune step has something to retire.
        //
        // Invariants:
        //   - Every returned subtree has `replaces_old_paddrs.empty()`
        //     on every mem_tree_node, so `assign_and_emit_node`'s
        //     fresh-range branch fires uniformly.
        //   - Mixed-height subtrees (some workers produced a single
        //     leaf, others an internal wrapping leaves) are accepted
        //     as-is: the read path stops at `type == leaf` so it
        //     reaches the right leaf regardless of depth variance.
        //     Future flushes will rebalance via shape-changing paths.
        static inline child_ref
        combine_worker_roots_for_bootstrap(
            std::vector<worker_tree_proposal>& proposals,
            const core::tree_geometry*         geom)
        {
            const uint32_t page_size = geom->tree_page_size;

            std::vector<std::unique_ptr<mem_tree_node>> roots;
            roots.reserve(proposals.size());
            for (auto& p : proposals) {
                if (p.root != nullptr) roots.push_back(std::move(p.root));
            }

            if (roots.empty()) {
                return make_empty_root_leaf(page_size);
            }

            std::stable_sort(
                roots.begin(), roots.end(),
                [page_size](const auto& a, const auto& b) {
                    return first_key_of_worker_root(a.get(), page_size)
                         < first_key_of_worker_root(b.get(), page_size);
                });

            if (roots.size() == 1) {
                return child_ref{ .target = std::move(roots.front()) };
            }

            std::vector<child_ref>   children;
            std::vector<std::string> separators;
            children.reserve(roots.size());
            separators.reserve(roots.size() - 1);
            for (std::size_t i = 0; i < roots.size(); ++i) {
                if (i > 0) {
                    auto sep = first_key_of_worker_root(
                        roots[i].get(), page_size);
                    if (sep.empty()) {
                        core::panic_inconsistency(
                            "combine_worker_roots_for_bootstrap",
                            "worker root has no first key to synthesize "
                            "a separator (bootstrap produced an empty "
                            "subtree — prune step should have removed "
                            "it before reaching here)");
                    }
                    separators.emplace_back(sep);
                }
                children.push_back(child_ref{ .target = std::move(roots[i]) });
            }

            std::vector<std::string> sibling_seps;
            while (true) {
                auto layer = build_internal_pages_owner(
                    std::move(children), std::move(separators),
                    paddr{0, 0},
                    /*is_new_layer=*/true,
                    page_size, sibling_seps);
                if (layer.size() == 1) {
                    return child_ref{ .target = std::move(layer.front()) };
                }
                children.clear();
                separators = std::move(sibling_seps);
                sibling_seps.clear();
                children.reserve(layer.size());
                for (auto& node : layer) {
                    children.push_back(child_ref{ .target = std::move(node) });
                }
            }
        }

        // Returns a valid ref to old internal page bytes if any contrib
        // walked through `rb` (touched_old_pages) or the owner's
        // pre-scan read it from disk (fetched_old_pages). Returns
        // nullptr only when the rb is genuinely uncached — callers
        // that can't tolerate nullptr must panic themselves. Callers
        // in the pure build phase (after pre-scan reads returned)
        // should treat nullptr as an invariant violation.
        static inline const std::vector<char>*
        try_get_old_page_bytes(const merge_context& ctx, paddr rb,
                               const std::vector<const proposal_group_view*>& contribs)
        {
            for (auto* g : contribs) {
                auto it = (*ctx.proposals)[g->worker_index].touched_old_pages.find(rb);
                if (it != (*ctx.proposals)[g->worker_index].touched_old_pages.end()) {
                    return &it->second;
                }
            }
            if (ctx.fetched_old_pages != nullptr) {
                auto it = ctx.fetched_old_pages->find(rb);
                if (it != ctx.fetched_old_pages->end()) {
                    return &it->second;
                }
            }
            return nullptr;
        }

        static inline const std::vector<char>&
        touched_old_page_for(const merge_context& ctx,
                             paddr                rb,
                             const std::vector<const proposal_group_view*>& contribs)
        {
            if (auto* p = try_get_old_page_bytes(ctx, rb, contribs)) {
                return *p;
            }
            core::panic_inconsistency(
                "touched_old_page_for",
                "missing old internal page bytes after pre-scan reads "
                "(dev=%u lba=%lu) — build phase must not reach a rb that "
                "wasn't either touched by a worker or fetched by owner",
                static_cast<unsigned>(rb.device_id),
                static_cast<unsigned long>(rb.lba));
        }

        static inline const merged_group&
        merge_old_paddr(merge_context& ctx, paddr rb);

        static inline merged_group
        materialize_group_view(const proposal_group_view& view) {
            return clone_group_view(view);
        }

        struct substitution_iv {
            uint32_t start_old_slot = 0;
            uint32_t end_old_slot   = 0;
            bool     passthrough    = false;

            enum class source_kind : uint8_t { raw_group, merged_child };
            source_kind             kind = source_kind::raw_group;
            const proposal_group_view* raw_view = nullptr;
            paddr                   merged_rb{};
        };

        static inline merged_group
        materialize_substitution(merge_context& ctx, const substitution_iv& iv) {
            if (iv.kind == substitution_iv::source_kind::merged_child) {
                return clone_group(merge_old_paddr(ctx, iv.merged_rb));
            }
            return materialize_group_view(*iv.raw_view);
        }

        static inline const merged_group&
        merge_internal_old_paddr(merge_context& ctx, paddr rb)
        {
            auto it = ctx.merged_cache.find(rb);
            if (it != ctx.merged_cache.end()) return *it->second;

            auto contrib_it = ctx.contrib_index.find(rb);
            if (contrib_it == ctx.contrib_index.end() || contrib_it->second.empty()) {
                core::panic_inconsistency(
                    "merge_internal_old_paddr",
                    "no contributors for internal dev=%u lba=%lu",
                    static_cast<unsigned>(rb.device_id),
                    static_cast<unsigned long>(rb.lba));
            }
            const auto& contribs = contrib_it->second;
            if (contribs.size() == 1) {
                auto holder = std::make_unique<merged_group>(
                    materialize_group_view(*contribs.front()));
                auto* ptr = holder.get();
                ctx.merged_cache.emplace(rb, std::move(holder));
                return *ptr;
            }

            internal_page_reader reader;
            const auto& old_content = touched_old_page_for(ctx, rb, contribs);
            if (!reader.parse(old_content.data(), ctx.geom->tree_page_size)) {
                core::panic_inconsistency(
                    "merge_internal_old_paddr",
                    "failed to decode shared-ancestor internal page "
                    "(dev=%u lba=%lu)",
                    static_cast<unsigned>(rb.device_id),
                    static_cast<unsigned long>(rb.lba));
            }

            std::vector<paddr> old_children;
            std::vector<std::string_view> old_separators;
            old_children.reserve(static_cast<std::size_t>(reader.record_count()) + 1);
            old_separators.reserve(reader.record_count());
            for (uint16_t i = 0; i < reader.record_count(); ++i) {
                auto e = reader.get(i);
                old_children.push_back(e.child_base);
                old_separators.push_back(e.separator_key);
            }
            old_children.push_back(reader.rightmost_child());

            absl::flat_hash_map<paddr, uint32_t> old_child_index;
            for (uint32_t i = 0; i < old_children.size(); ++i) {
                old_child_index.emplace(old_children[i], i);
            }

            std::vector<std::vector<substitution_iv>> per_contrib;
            per_contrib.reserve(contribs.size());

            for (auto* contrib : contribs) {
                std::vector<const child_ref*> flat_children;
                std::vector<std::string_view> flat_separators;
                for (std::size_t group_idx = 0; group_idx < contrib->nodes.size(); ++group_idx) {
                    auto* node = contrib->nodes[group_idx];
                    for (std::size_t child_idx = 0; child_idx < node->children.size(); ++child_idx) {
                        flat_children.push_back(&node->children[child_idx]);
                        if (child_idx + 1 < node->children.size()) {
                            flat_separators.push_back(node->separators[child_idx]);
                        }
                    }
                    if (group_idx + 1 < contrib->nodes.size()) {
                        flat_separators.push_back(contrib->sibling_seps[group_idx]);
                    }
                }
                if (!flat_children.empty()
                    && flat_separators.size() + 1 != flat_children.size()) {
                    core::panic_inconsistency(
                        "merge_internal_old_paddr",
                        "flattened internal child sequence is malformed");
                }

                std::vector<substitution_iv> subs;
                std::size_t j = 0;
                while (j < flat_children.size()) {
                    if (auto* p = std::get_if<paddr>(&flat_children[j]->target)) {
                        auto idx_it = old_child_index.find(*p);
                        if (idx_it == old_child_index.end()) {
                            core::panic_inconsistency(
                                "merge_internal_old_paddr",
                                "passthrough child not found in old page "
                                "(dev=%u lba=%lu)",
                                static_cast<unsigned>(p->device_id),
                                static_cast<unsigned long>(p->lba));
                        }
                        subs.push_back(substitution_iv{
                            .start_old_slot = idx_it->second,
                            .end_old_slot   = idx_it->second,
                            .passthrough    = true,
                        });
                        ++j;
                        continue;
                    }

                    auto* first = std::get<std::unique_ptr<mem_tree_node>>(
                                      flat_children[j]->target).get();
                    std::size_t k = j + 1;
                    while (k < flat_children.size()) {
                        auto* next_up = std::get_if<std::unique_ptr<mem_tree_node>>(
                            &flat_children[k]->target);
                        if (next_up == nullptr) break;
                        if (!next_up->get()->replaces_old_paddrs.empty()) break;
                        ++k;
                    }

                    if (first->replaces_old_paddrs.empty()) {
                        core::panic_inconsistency(
                            "merge_internal_old_paddr",
                            "group start without covered old paddrs");
                    }

                    uint32_t start = UINT32_MAX;
                    uint32_t end   = 0;
                    absl::flat_hash_set<uint32_t> seen_slots;
                    for (auto covered_rb : first->replaces_old_paddrs) {
                        auto idx_it = old_child_index.find(covered_rb);
                        if (idx_it == old_child_index.end()) {
                            core::panic_inconsistency(
                                "merge_internal_old_paddr",
                                "covered child not found in old page "
                                "(dev=%u lba=%lu)",
                                static_cast<unsigned>(covered_rb.device_id),
                                static_cast<unsigned long>(covered_rb.lba));
                        }
                        start = std::min(start, idx_it->second);
                        end   = std::max(end, idx_it->second);
                        seen_slots.insert(idx_it->second);
                    }
                    for (uint32_t slot = start; slot <= end; ++slot) {
                        if (!seen_slots.contains(slot)) {
                            core::panic_inconsistency(
                                "merge_internal_old_paddr",
                                "covered child slots are not contiguous");
                        }
                    }

                    substitution_iv iv{
                        .start_old_slot = start,
                        .end_old_slot   = end,
                        .passthrough    = false,
                        .kind           = substitution_iv::source_kind::raw_group,
                        .raw_view       = contrib,
                    };
                    if (first->type == format::node_type::internal
                        && first->replaces_old_paddrs.size() == 1) {
                        auto child_rb = first->replaces_old_paddrs[0];
                        auto child_contrib = ctx.contrib_index.find(child_rb);
                        if (child_contrib != ctx.contrib_index.end()
                            && child_contrib->second.size() > 1) {
                            iv.kind      = substitution_iv::source_kind::merged_child;
                            iv.merged_rb = child_rb;
                        }
                    }
                    subs.push_back(std::move(iv));
                    j = k;
                }

                per_contrib.push_back(std::move(subs));
            }

            std::vector<std::vector<const substitution_iv*>> owner_slot_view(
                old_children.size());
            for (const auto& subs : per_contrib) {
                for (const auto& iv : subs) {
                    if (iv.passthrough) continue;
                    for (uint32_t slot = iv.start_old_slot; slot <= iv.end_old_slot; ++slot) {
                        owner_slot_view[slot].push_back(&iv);
                    }
                }
            }

            std::vector<child_ref>   new_children;
            std::vector<std::string> new_separators;
            bool have_prev_segment = false;
            uint32_t i = 0;
            while (i < old_children.size()) {
                auto& view = owner_slot_view[i];

                const substitution_iv* chosen = nullptr;
                for (auto* candidate : view) {
                    if (chosen == nullptr) {
                        chosen = candidate;
                        continue;
                    }
                    bool same = false;
                    if (chosen->kind == substitution_iv::source_kind::merged_child
                        && candidate->kind == substitution_iv::source_kind::merged_child) {
                        same = (chosen->merged_rb == candidate->merged_rb);
                    } else if (chosen->kind == substitution_iv::source_kind::raw_group
                               && candidate->kind == substitution_iv::source_kind::raw_group) {
                        same = (chosen->raw_view == candidate->raw_view);
                    }
                    if (!same) {
                        core::panic_inconsistency(
                            "merge_internal_old_paddr",
                            "multiple worker substitutions target the same "
                            "base slot (slot=%u)",
                            static_cast<unsigned>(i));
                    }
                }

                if (have_prev_segment) {
                    new_separators.emplace_back(old_separators[i - 1]);
                }

                if (chosen == nullptr) {
                    new_children.push_back(child_ref{ .target = old_children[i] });
                    have_prev_segment = true;
                    ++i;
                    continue;
                }

                append_group_to_children(
                    materialize_substitution(ctx, *chosen),
                    new_children,
                    new_separators);
                have_prev_segment = true;
                i = chosen->end_old_slot + 1;
            }

            auto holder = std::make_unique<merged_group>();
            auto built = build_internal_pages_owner(
                std::move(new_children),
                std::move(new_separators),
                rb,
                /*is_new_layer=*/false,
                ctx.geom->tree_page_size,
                holder->sibling_seps);
            for (auto& node : built) {
                holder->nodes.push_back(std::move(node));
            }

            auto* ptr = holder.get();
            ctx.merged_cache.emplace(rb, std::move(holder));
            return *ptr;
        }

        static inline const merged_group&
        merge_old_paddr(merge_context& ctx, paddr rb) {
            if (auto it = ctx.merged_cache.find(rb); it != ctx.merged_cache.end()) {
                return *it->second;
            }

            auto contrib_it = ctx.contrib_index.find(rb);
            if (contrib_it == ctx.contrib_index.end() || contrib_it->second.empty()) {
                core::panic_inconsistency(
                    "merge_old_paddr",
                    "missing contributor for dev=%u lba=%lu",
                    static_cast<unsigned>(rb.device_id),
                    static_cast<unsigned long>(rb.lba));
            }

            if (is_leaf_range(ctx, rb)) {
                if (contrib_it->second.size() > 1) {
                    core::panic_inconsistency(
                        "merge_old_paddr",
                        "multiple workers contributed the same leaf "
                        "(dev=%u lba=%lu)",
                        static_cast<unsigned>(rb.device_id),
                        static_cast<unsigned long>(rb.lba));
                }
                auto holder = std::make_unique<merged_group>(
                    materialize_group_view(*contrib_it->second.front()));
                auto* ptr = holder.get();
                ctx.merged_cache.emplace(rb, std::move(holder));
                return *ptr;
            }

            return merge_internal_old_paddr(ctx, rb);
        }

        static inline child_ref
        finalize_root_group(merge_context& ctx, const merged_group& root_group) {
            merged_group cur = clone_group(root_group);
            if (cur.nodes.empty()) {
                core::panic_inconsistency(
                    "finalize_root_group",
                    "root group is empty");
            }

            while (cur.nodes.size() > 1) {
                std::vector<child_ref> children;
                std::vector<std::string> separators = std::move(cur.sibling_seps);
                children.reserve(cur.nodes.size());
                for (auto& n : cur.nodes) {
                    children.push_back(child_ref{ .target = std::move(n) });
                }
                cur.nodes.clear();
                auto next = build_internal_pages_owner(
                    std::move(children),
                    std::move(separators),
                    paddr{0, 0},
                    /*is_new_layer=*/true,
                    ctx.geom->tree_page_size,
                    cur.sibling_seps);
                for (auto& n : next) {
                    cur.nodes.push_back(std::move(n));
                }
            }

            return child_ref{ .target = std::move(cur.nodes.front()) };
        }

        template <typename retire_range_fn>
        static inline std::optional<child_ref>
        prune_child_ref(child_ref&& child,
                        uint32_t page_size,
                        retire_range_fn&& retire_range)
        {
            if (std::holds_alternative<paddr>(child.target)) {
                return std::move(child);
            }

            auto node = std::move(std::get<std::unique_ptr<mem_tree_node>>(child.target));
            if (node->type == format::node_type::leaf) {
                leaf_page_reader reader;
                if (!reader.parse(node->content.data(), page_size)) {
                    core::panic_inconsistency(
                        "prune_child_ref",
                        "leaf candidate failed validation during prune");
                }
                if (reader.record_count() > 0) {
                    return child_ref{ .target = std::move(node) };
                }
                for (auto rb : node->replaces_old_paddrs) {
                    retire_range(rb);
                }
                return std::nullopt;
            }

            std::vector<child_ref>   kept_children;
            std::vector<std::string> kept_seps;
            for (std::size_t i = 0; i < node->children.size(); ++i) {
                auto kept = prune_child_ref(
                    std::move(node->children[i]), page_size, retire_range);
                if (!kept.has_value()) continue;
                if (!kept_children.empty()) {
                    kept_seps.push_back(node->separators[i - 1]);
                }
                kept_children.push_back(std::move(*kept));
            }

            if (kept_children.empty()) {
                for (auto rb : node->replaces_old_paddrs) {
                    retire_range(rb);
                }
                return std::nullopt;
            }

            if (kept_children.size() == 1) {
                for (auto rb : node->replaces_old_paddrs) {
                    retire_range(rb);
                }
                return std::move(kept_children.front());
            }

            node->children   = std::move(kept_children);
            node->separators = std::move(kept_seps);
            node->content.resize(page_size);
            reformat_internal_node(node.get(), page_size);
            return child_ref{ .target = std::move(node) };
        }

        static inline child_ref
        make_empty_root_leaf(uint32_t page_size) {
            auto node = std::make_unique<mem_tree_node>();
            node->type = format::node_type::leaf;
            node->content.resize(page_size);
            leaf_page_builder builder;
            builder.init(node->content.data(), page_size);
            builder.finalize();
            return child_ref{ .target = std::move(node) };
        }

        template <typename retire_slot_fn, typename retire_range_fn>
        static inline void
        assign_planned_paddrs(child_ref&                    root,
                              const core::tree_manifest*    base_manifest,
                              tree_allocator&               alloc,
                              std::vector<format::range_ref>& allocated_ranges,
                              retire_slot_fn&&             retire_slot,
                              retire_range_fn&&            retire_range)
        {
            auto plan = [&](auto&& self, child_ref& ref) -> void {
                if (auto* p = std::get_if<paddr>(&ref.target)) {
                    (void)p;
                    return;
                }

                auto* node = std::get<std::unique_ptr<mem_tree_node>>(ref.target).get();
                for (auto& child : node->children) {
                    self(self, child);
                }

                auto fresh_range = [&]() {
                    auto r = alloc.allocate();
                    allocated_ranges.push_back(r);
                    node->new_range_base = r.base;
                    node->new_slot_index = 0;
                    node->new_paddr      = r.base;
                };

                if (node->replaces_old_paddrs.empty()) {
                    fresh_range();
                } else {
                    auto carrier = node->replaces_old_paddrs[0];
                    auto cur_slot = base_manifest->slot_index(carrier);
                    if (node->replaces_old_paddrs.size() > 1) {
                        for (std::size_t i = 1; i < node->replaces_old_paddrs.size(); ++i) {
                            retire_range(node->replaces_old_paddrs[i]);
                        }
                    }

                    if (cur_slot + 1 < base_manifest->geom->shadow_slots_per_range) {
                        node->new_range_base = carrier;
                        node->new_slot_index = cur_slot + 1;
                        node->new_paddr = base_manifest->geom->slot_paddr(
                            carrier, cur_slot + 1);
                        retire_slot(base_manifest->resolve(carrier));
                    } else {
                        fresh_range();
                        retire_range(carrier);
                    }
                }

                if (node->type == format::node_type::internal) {
                    node->content.resize(base_manifest->geom->tree_page_size);
                    reformat_internal_node(node, base_manifest->geom->tree_page_size);
                } else {
                    auto* hdr = reinterpret_cast<format::tree_slot_header*>(
                        node->content.data());
                    hdr->page_crc = format::tree_page_compute_crc(
                        node->content.data(), base_manifest->geom->tree_page_size);
                }
            };

            plan(plan, root);
        }

        static inline void
        collect_write_descs(const core::tree_geometry* geom,
                            const child_ref&          root,
                            std::vector<format::write_desc>& writes)
        {
            auto walk = [&](auto&& self, const child_ref& ref) -> void {
                if (auto* p = std::get_if<paddr>(&ref.target)) {
                    (void)p;
                    return;
                }
                auto* node = std::get<std::unique_ptr<mem_tree_node>>(ref.target).get();
                for (const auto& child : node->children) {
                    self(self, child);
                }
                writes.push_back(format::write_desc{
                    .lba      = node->new_paddr.lba,
                    .data     = node->content.data(),
                    .num_lbas = geom->page_lbas(),
                    .flags    = 0,
                });
            };
            walk(walk, root);
        }

        static inline void
        collect_leaf_items(const merge_context&           ctx,
                           const child_ref&               root,
                           std::vector<final_leaf_item>&  out)
        {
            auto append_base_range = [&](uint32_t begin, uint32_t end) {
                for (uint32_t i = begin; i < end; ++i) {
                    out.push_back(final_leaf_item{
                        .range_base = ctx.base_manifest->leaf_order.spans[i].leaf_range_base,
                        .is_new     = false,
                        .new_leaf   = nullptr,
                        .base_span  = &ctx.base_manifest->leaf_order.spans[i],
                        .first_key  = {},
                    });
                }
            };

            auto walk = [&](auto&& self, const child_ref& ref) -> void {
                if (auto* p = std::get_if<paddr>(&ref.target)) {
                    if (is_leaf_range(ctx, *p)) {
                        append_base_range(
                            ctx.base_leaf_index_by_range.at(*p),
                            ctx.base_leaf_index_by_range.at(*p) + 1);
                        return;
                    }
                    auto idx = ctx.base_internal_index_by_range.at(*p);
                    auto iv = ctx.base_internal_leaf_ranges[idx];
                    append_base_range(iv.begin, iv.end);
                    return;
                }

                auto* node = std::get<std::unique_ptr<mem_tree_node>>(ref.target).get();
                if (node->type == format::node_type::leaf) {
                    leaf_page_reader reader;
                    if (!reader.parse(node->content.data(), ctx.geom->tree_page_size)) {
                        core::panic_inconsistency(
                            "collect_leaf_items",
                            "planned leaf failed validation");
                    }
                    std::string_view first_key{};
                    if (reader.record_count() > 0) {
                        first_key = reader.get(0).key;
                    }
                    out.push_back(final_leaf_item{
                        .range_base = node->new_range_base,
                        .is_new     = true,
                        .new_leaf   = node,
                        .base_span  = nullptr,
                        .first_key  = first_key,
                    });
                    return;
                }

                for (const auto& child : node->children) {
                    self(self, child);
                }
            };

            walk(walk, root);
        }

        static inline core::leaf_order_index
        build_leaf_order_full(const merge_context&          ctx,
                              const child_ref&              root,
                              std::vector<final_leaf_item>& leaf_items)
        {
            leaf_items.clear();
            collect_leaf_items(ctx, root, leaf_items);
            if (leaf_items.empty()) return {};

            core::leaf_order_index out;
            out.spans.reserve(leaf_items.size());

            auto append_lower = [&](std::string_view lower) {
                auto off = static_cast<uint32_t>(out.fence_pool.size());
                out.fence_pool.append(lower.data(), lower.size());
                return std::pair<uint32_t, uint16_t>{
                    off,
                    static_cast<uint16_t>(lower.size()),
                };
            };

            std::vector<std::string_view> lowers(leaf_items.size());
            for (std::size_t i = 0; i < leaf_items.size(); ++i) {
                if (i == 0) {
                    lowers[i] = std::string_view{};
                    continue;
                }
                if (leaf_items[i].is_new) {
                    lowers[i] = leaf_items[i].first_key;
                } else {
                    lowers[i] =
                        ctx.base_manifest->leaf_order.fence_lower(*leaf_items[i].base_span);
                }
            }

            for (std::size_t i = 0; i < leaf_items.size(); ++i) {
                auto [off, len] = append_lower(lowers[i]);
                out.spans.push_back(core::leaf_span{
                    .fence_lower_off = off,
                    .fence_upper_off = 0,
                    .fence_lower_len = len,
                    .fence_upper_len = 0,
                    .leaf_range_base = leaf_items[i].range_base,
                });
                if (i > 0) {
                    out.spans[i - 1].fence_upper_off = off;
                    out.spans[i - 1].fence_upper_len = len;
                }
            }
            const auto empty_off = static_cast<uint32_t>(out.fence_pool.size());
            out.spans.back().fence_upper_off = empty_off;
            out.spans.back().fence_upper_len = 0;

            return out;
        }

        static inline core::tree_reverse_topology
        build_reverse_topology_full(const merge_context&               ctx,
                                    const child_ref&                   root,
                                    const std::vector<final_leaf_item>& leaf_items)
        {
            struct topo_collect {
                std::vector<std::pair<paddr, paddr>> new_internal_parents;
                absl::flat_hash_map<paddr, paddr>    direct_leaf_parent;
                absl::flat_hash_map<paddr, paddr>    unchanged_subtree_parent;
            } collect;

            auto walk = [&](auto&& self, const child_ref& ref, paddr parent_rb) -> void {
                if (auto* p = std::get_if<paddr>(&ref.target)) {
                    if (is_leaf_range(ctx, *p)) {
                        collect.direct_leaf_parent[*p] = parent_rb;
                    } else {
                        collect.unchanged_subtree_parent[*p] = parent_rb;
                    }
                    return;
                }

                auto* node = std::get<std::unique_ptr<mem_tree_node>>(ref.target).get();
                if (node->type == format::node_type::leaf) {
                    collect.direct_leaf_parent[node->new_range_base] = parent_rb;
                    return;
                }

                collect.new_internal_parents.push_back(
                    { node->new_range_base, parent_rb });
                for (const auto& child : node->children) {
                    self(self, child, node->new_range_base);
                }
            };
            walk(walk, root, paddr{0, 0});

            absl::flat_hash_map<paddr, paddr> final_old_internal_parent;
            absl::flat_hash_set<paddr> visited_old_internal;

            auto dfs_old = [&](auto&& self, uint32_t idx, paddr parent_rb) -> void {
                auto self_rb = ctx.base_manifest->reverse_topology.internal_nodes[idx].range_base;
                if (!visited_old_internal.insert(self_rb).second) return;
                final_old_internal_parent[self_rb] = parent_rb;
                for (auto child_idx : ctx.base_internal_children[idx]) {
                    self(self, child_idx, self_rb);
                }
            };

            for (const auto& [root_rb, parent_rb] : collect.unchanged_subtree_parent) {
                auto idx_it = ctx.base_internal_index_by_range.find(root_rb);
                if (idx_it == ctx.base_internal_index_by_range.end()) {
                    core::panic_inconsistency(
                        "build_reverse_topology_full",
                        "unchanged subtree root is not an internal "
                        "(dev=%u lba=%lu)",
                        static_cast<unsigned>(root_rb.device_id),
                        static_cast<unsigned long>(root_rb.lba));
                }
                dfs_old(dfs_old, idx_it->second, parent_rb);
            }

            std::vector<core::internal_node_entry> internals;
            std::vector<paddr> parent_rbs;
            absl::flat_hash_map<paddr, core::internal_idx> idx_by_rb;

            internals.reserve(
                collect.new_internal_parents.size() + final_old_internal_parent.size());
            parent_rbs.reserve(internals.capacity());

            for (const auto& [rb, parent_rb] : collect.new_internal_parents) {
                idx_by_rb.emplace(rb, static_cast<core::internal_idx>(internals.size()));
                internals.push_back(core::internal_node_entry{
                    .range_base = rb,
                    .parent_idx = core::kInvalidInternalIdx,
                });
                parent_rbs.push_back(parent_rb);
            }
            for (const auto& [rb, parent_rb] : final_old_internal_parent) {
                if (idx_by_rb.contains(rb)) continue;
                idx_by_rb.emplace(rb, static_cast<core::internal_idx>(internals.size()));
                internals.push_back(core::internal_node_entry{
                    .range_base = rb,
                    .parent_idx = core::kInvalidInternalIdx,
                });
                parent_rbs.push_back(parent_rb);
            }

            for (std::size_t i = 0; i < internals.size(); ++i) {
                if (parent_rbs[i].lba == 0 && parent_rbs[i].device_id == 0) {
                    internals[i].parent_idx = core::kInvalidInternalIdx;
                } else {
                    internals[i].parent_idx = idx_by_rb.at(parent_rbs[i]);
                }
            }

            std::vector<core::internal_idx> leaf_parent_idx;
            leaf_parent_idx.reserve(leaf_items.size());
            for (const auto& item : leaf_items) {
                paddr parent_rb{0, 0};
                auto direct_it = collect.direct_leaf_parent.find(item.range_base);
                if (direct_it != collect.direct_leaf_parent.end()) {
                    parent_rb = direct_it->second;
                } else {
                    auto leaf_it = ctx.base_leaf_index_by_range.find(item.range_base);
                    if (leaf_it == ctx.base_leaf_index_by_range.end()) {
                        core::panic_inconsistency(
                            "build_reverse_topology_full",
                            "leaf parent missing for new leaf "
                            "(dev=%u lba=%lu)",
                            static_cast<unsigned>(item.range_base.device_id),
                            static_cast<unsigned long>(item.range_base.lba));
                    }
                    auto old_parent =
                        ctx.base_manifest->reverse_topology.leaf_parent_idx[leaf_it->second];
                    if (old_parent != core::kInvalidInternalIdx) {
                        parent_rb =
                            ctx.base_manifest->reverse_topology.internal_nodes[old_parent].range_base;
                    }
                }

                if (parent_rb.lba == 0 && parent_rb.device_id == 0) {
                    leaf_parent_idx.push_back(core::kInvalidInternalIdx);
                } else {
                    leaf_parent_idx.push_back(idx_by_rb.at(parent_rb));
                }
            }

            return core::tree_reverse_topology{
                .leaf_parent_idx = std::move(leaf_parent_idx),
                .internal_nodes  = std::move(internals),
            };
        }

        static inline void
        rebuild_slot_map(const core::tree_manifest*     base_manifest,
                         const child_ref&               root,
                         const core::retired_objects&   retired,
                         absl::flat_hash_map<paddr, uint32_t>& slot_map)
        {
            slot_map = base_manifest->slot_map;
            for (const auto& rr : retired.old_ranges) {
                slot_map.erase(rr.base);
            }

            auto walk = [&](auto&& self, const child_ref& ref) -> void {
                if (auto* p = std::get_if<paddr>(&ref.target)) {
                    (void)p;
                    return;
                }
                auto* node = std::get<std::unique_ptr<mem_tree_node>>(ref.target).get();
                for (const auto& child : node->children) {
                    self(self, child);
                }
                if (node->replaces_old_paddrs.empty()) {
                    slot_map[node->new_range_base] = node->new_slot_index;
                    return;
                }

                auto carrier = node->replaces_old_paddrs[0];
                for (std::size_t i = 1; i < node->replaces_old_paddrs.size(); ++i) {
                    slot_map.erase(node->replaces_old_paddrs[i]);
                }
                if (node->new_range_base == carrier) {
                    slot_map[carrier] = node->new_slot_index;
                } else {
                    slot_map.erase(carrier);
                    slot_map[node->new_range_base] = node->new_slot_index;
                }
            };
            walk(walk, root);
        }

        static inline paddr
        root_range_base_of(const child_ref& root) {
            if (auto* p = std::get_if<paddr>(&root.target)) return *p;
            return std::get<std::unique_ptr<mem_tree_node>>(root.target)->new_range_base;
        }

        static inline paddr
        root_slot_of(const core::tree_manifest* base_manifest,
                     const child_ref&           root)
        {
            if (auto* p = std::get_if<paddr>(&root.target)) {
                return base_manifest->resolve(*p);
            }
            return std::get<std::unique_ptr<mem_tree_node>>(root.target)->new_paddr;
        }

        static inline bool
        is_root_change(const core::tree_manifest* base_manifest,
                       const child_ref&           root)
        {
            if (auto* p = std::get_if<paddr>(&root.target)) {
                return *p != base_manifest->root_range_base;
            }
            auto* node = std::get<std::unique_ptr<mem_tree_node>>(root.target).get();
            return !(node->replaces_old_paddrs.size() == 1
                     && node->replaces_old_paddrs[0] == base_manifest->root_range_base
                     && node->new_range_base == base_manifest->root_range_base);
        }

        // ── Per-node assign + emit (post-order, iterative) ───────
        //
        // Mirrors the inner `plan(...)` lambda of the old
        // `assign_planned_paddrs` AND the old `collect_write_descs`
        // fused together: when a node's children are all processed,
        // allocate its new_paddr, reformat/CRC its content, push a
        // write_desc into `s.pending_writes`.
        //
        // Inputs are threaded through the closure by `run_merge`
        // (base_manifest / alloc / geom / retire_*). The node's
        // `content` pointer goes into `write_desc.data`; it stays
        // valid for the lifetime of `combined_root`, which the
        // merge_round_state owns until `submit_finalize_merge`.
        template <typename retire_slot_fn, typename retire_range_fn>
        static inline void
        assign_and_emit_node(
            mem_tree_node*             node,
            const core::tree_manifest* base_manifest,
            tree_allocator&            alloc,
            const core::tree_geometry* geom,
            std::vector<format::range_ref>& allocated_ranges,
            std::vector<merge_io_desc>&     pending_ios,
            retire_slot_fn&&           retire_slot,
            retire_range_fn&&          retire_range)
        {
            auto fresh_range = [&]() {
                auto r = alloc.allocate();
                allocated_ranges.push_back(r);
                node->new_range_base = r.base;
                node->new_slot_index = 0;
                node->new_paddr      = r.base;
            };

            if (node->replaces_old_paddrs.empty()) {
                fresh_range();
            } else {
                auto carrier  = node->replaces_old_paddrs[0];
                auto cur_slot = base_manifest->slot_index(carrier);
                if (node->replaces_old_paddrs.size() > 1) {
                    for (std::size_t i = 1;
                         i < node->replaces_old_paddrs.size(); ++i) {
                        retire_range(node->replaces_old_paddrs[i]);
                    }
                }
                if (cur_slot + 1 < geom->shadow_slots_per_range) {
                    node->new_range_base = carrier;
                    node->new_slot_index = cur_slot + 1;
                    node->new_paddr = geom->slot_paddr(carrier, cur_slot + 1);
                    retire_slot(base_manifest->resolve(carrier));
                } else {
                    fresh_range();
                    retire_range(carrier);
                }
            }

            if (node->type == format::node_type::internal) {
                node->content.resize(geom->tree_page_size);
                reformat_internal_node(node, geom->tree_page_size);
            } else {
                auto* hdr = reinterpret_cast<format::tree_slot_header*>(
                    node->content.data());
                hdr->page_crc = format::tree_page_compute_crc(
                    node->content.data(), geom->tree_page_size);
            }

            pending_ios.push_back(merge_io_desc{
                format::write_desc{
                    .lba      = node->new_paddr.lba,
                    .data     = node->content.data(),
                    .num_lbas = geom->page_lbas(),
                    .flags    = 0,
                }});
        }

        // ── Resumable merge coroutine ────────────────────────────
        //
        // Entry seam for the merge phase. Drives the full sequence
        // (build merge_context → pre-scan owner reads → build
        // combined_root → prune → post-order assign+emit walk →
        // build leaf_order / topology / slot_map → new_manifest)
        // with `co_yield`s at every read/write boundary and
        // periodically for CPU cooperation.
        //
        // Yields only status codes; all payload staging happens in
        // `merge_round_state::pending_reads / pending_writes` and
        // in the per-node `write_desc.data` pointers into
        // `combined_root`. See `merge_round_state` doc-comment for
        // the full pointer lifetime story.
        //
        // Parameters:
        //   s             — the coroutine's working state (stable
        //                   address, lives in tree_state.active_merge)
        //   base_manifest — pinned via active_rounds[round_id].pinned_base_guard
        //   alloc         — tree_sched's allocator (for new tree slots)
        //   geom          — tree geometry (page_size / page_lbas / ...)
        //   round         — tree_sched's flush_round_state for this
        //                   round (holds retired_*, st, pinned_gens,
        //                   flushed_max_lsn — the coroutine appends
        //                   into round.retired as it goes)
        static inline merge_coro
        run_merge(merge_round_state&         s,
                  const core::tree_manifest* base_manifest,
                  tree_allocator&            alloc,
                  const core::tree_geometry* geom,
                  flush_round_state&         round)
        {
            // Budget tuning constants — keep flush responsive so
            // other schedulers can advance on the same core.
            constexpr std::size_t kWriteBatchSize  = 32;
            constexpr std::size_t kCpuYieldBudget  = 64;

            const uint32_t page_size = geom->tree_page_size;
            const uint32_t page_lbas = geom->page_lbas();

            const bool is_bootstrap = !base_manifest->has_root();

            // ── Phase 1: build merge_context ──
            //
            // Non-bootstrap: index proposals against the existing
            // tree (leaf_order + root-group contribs), then pre-scan
            // any shared-ancestor internal pages missing from
            // worker-side `touched_old_pages`.
            //
            // Bootstrap (`!has_root()`): no old root → no
            // `index_root_group` (it would push the `{0,0}` root
            // sentinel as a contrib and the pre-scan would try to
            // read LBA 0), no pre-scan reads. The ctx is still
            // constructed so the Phase 4 manifest builders — which
            // read ctx unconditionally — have a valid (empty) base
            // to work against.
            s.ctx = std::make_shared<merge_context>(
                is_bootstrap
                    ? make_merge_context_bootstrap(
                          base_manifest, &s.worker_proposals)
                    : make_merge_context(
                          base_manifest, &s.worker_proposals));
            s.ctx->fetched_old_pages = &s.fetched_old_pages;

            if (!is_bootstrap) {
                for (const auto& [rb, contribs] : s.ctx->contrib_index) {
                    if (contribs.size() <= 1) continue;
                    if (try_get_old_page_bytes(*s.ctx, rb, contribs) != nullptr)
                        continue;
                    auto& buf = s.fetched_old_pages[rb];
                    buf.assign(page_size, '\0');
                    s.pending_ios.push_back(merge_io_desc{
                        format::read_desc{
                            .lba      = rb.lba,
                            .buf      = buf.data(),
                            .num_lbas = page_lbas,
                        }});
                }
                if (!s.pending_ios.empty()) {
                    co_yield merge_yield::need_io;
                    // On resume: NVMe has DMA'd every buffer; the next
                    // call into merge_internal_old_paddr will see the
                    // fetched bytes via try_get_old_page_bytes.
                }
            }

            // ── Phase 2: build combined_root ─────────────────────
            //
            // Non-bootstrap: merge worker contribs against the old
            // tree, starting at `root_range_base`.
            //
            // Bootstrap: stitch the worker-supplied subtrees
            // together into a fresh tree. `combine_worker_roots_for_bootstrap`
            // moves each `proposal.root` out and builds a new
            // internal layer (or layers) if more than one worker
            // contributed. All resulting mem_tree_nodes carry
            // `replaces_old_paddrs.empty()` so the post-order
            // assign+emit walk allocates fresh ranges for every
            // page.
            if (is_bootstrap) {
                s.combined_root =
                    combine_worker_roots_for_bootstrap(
                        s.worker_proposals, geom);
            } else {
                const auto& root_group = merge_old_paddr(
                    *s.ctx, base_manifest->root_range_base);
                s.combined_root = finalize_root_group(*s.ctx, root_group);
            }

            for (const auto& proposal : s.worker_proposals) {
                for (const auto& rv : proposal.retired_old_values) {
                    round.retired.old_tree_values.push_back(rv);
                }
            }

            auto retire_slot = [&](paddr slot) {
                if (s.retired_slots_seen.insert(slot).second) {
                    round.retired.old_slots.push_back(slot);
                }
            };
            auto retire_range = [&](paddr rb) {
                if (s.retired_ranges_seen.insert(rb).second) {
                    round.retired.old_ranges.push_back(
                        geom->range_ref_from_base(rb));
                }
            };

            auto pruned = prune_child_ref(
                std::move(s.combined_root), page_size, retire_range);
            if (!pruned.has_value()) {
                s.combined_root = make_empty_root_leaf(page_size);
            } else {
                s.combined_root = std::move(*pruned);
            }

            // ── Phase 3: iterative post-order walk: assign + emit ──
            s.walk_stack.push_back(
                merge_round_state::walk_frame{
                    .ref = &s.combined_root,
                    .next_child = 0,
                });

            std::size_t cpu_budget_counter = 0;
            while (!s.walk_stack.empty()) {
                auto& frame = s.walk_stack.back();
                auto* node_up = std::get_if<std::unique_ptr<mem_tree_node>>(
                    &frame.ref->target);
                if (node_up == nullptr) {
                    // Passthrough paddr child — nothing to do.
                    s.walk_stack.pop_back();
                    continue;
                }
                auto* node = node_up->get();
                if (frame.next_child < node->children.size()) {
                    auto& next = node->children[frame.next_child++];
                    s.walk_stack.push_back(
                        merge_round_state::walk_frame{
                            .ref = &next,
                            .next_child = 0,
                        });
                    continue;
                }

                // All children processed — post-order visit.
                assign_and_emit_node(
                    node, base_manifest, alloc, geom,
                    s.allocated_ranges, s.pending_ios,
                    retire_slot, retire_range);
                s.walk_stack.pop_back();

                if (s.pending_ios.size() >= kWriteBatchSize) {
                    co_yield merge_yield::need_io;
                    cpu_budget_counter = 0;
                    continue;
                }
                if (++cpu_budget_counter >= kCpuYieldBudget) {
                    co_yield merge_yield::yield_cpu;
                    cpu_budget_counter = 0;
                }
            }

            if (!s.pending_ios.empty()) {
                co_yield merge_yield::need_io;
            }

            // ── Phase 4: build new_manifest metadata ─────────────
            std::vector<final_leaf_item> leaf_items;
            auto new_leaf_order = build_leaf_order_full(
                *s.ctx, s.combined_root, leaf_items);
            auto new_topology = build_reverse_topology_full(
                *s.ctx, s.combined_root, leaf_items);
            absl::flat_hash_map<paddr, uint32_t> slot_map;
            rebuild_slot_map(
                base_manifest, s.combined_root, round.retired, slot_map);

            s.new_manifest = std::make_shared<const core::tree_manifest>(
                core::tree_manifest{
                    .root_slot        = root_slot_of(
                                            base_manifest, s.combined_root),
                    .slot_map         = std::move(slot_map),
                    .geom             = base_manifest->geom,
                    .leaf_order       = std::move(new_leaf_order),
                    .root_range_base  = root_range_base_of(s.combined_root),
                    .reverse_topology = std::move(new_topology),
                });
            s.is_root_change        = is_root_change(
                                          base_manifest, s.combined_root);
            s.new_root_base_paddr   = s.new_manifest->root_range_base;

            co_yield merge_yield::done;
            co_return;
        }

    }  // namespace _owner

    struct tree_sched {
        static constexpr uint32_t kMaxFoldOpsPerAdvance                   = 8;
        static constexpr uint32_t kMaxMergeStepOpsPerAdvance              = 16;
        static constexpr uint32_t kMaxMergeReadsDoneOpsPerAdvance         = 4;
        static constexpr uint32_t kMaxFinalizeMergeOpsPerAdvance          = 4;
        static constexpr uint32_t kMaxBeginUpdateSuperblockOpsPerAdvance  = 1;
        static constexpr uint32_t kMaxFinishUpdateSuperblockOpsPerAdvance = 1;
        static constexpr uint32_t kMaxFinalizeOpsPerAdvance               = 8;
        static constexpr uint32_t kWriteBatchConcurrency                  = 32;

        const core::tree_geometry* geom = nullptr;
        tree_state                 state;
        pump::core::per_core::queue<_flush_fold::req*>               fold_q;
        pump::core::per_core::queue<_merge_step::req*>               merge_step_q;
        pump::core::per_core::queue<_merge_reads_done::req*>         merge_reads_done_q;
        pump::core::per_core::queue<_finalize_merge::req*>           finalize_merge_q;
        pump::core::per_core::queue<_begin_update_superblock::req*>  begin_update_superblock_q;
        pump::core::per_core::queue<_finish_update_superblock::req*> finish_update_superblock_q;
        pump::core::per_core::queue<_finalize_flush_round::req*>     finalize_q;
        // Serializes the superblock begin→finish pair: begin latches
        // the flag, finish clears it. The outer pipeline is expected
        // to issue read/mutate/FUA-write between the two seams.
        bool update_superblock_inflight = false;

        explicit
        tree_sched(const core::tree_geometry* g = nullptr,
                   format::paddr              data_area_base = {0, 0},
                   core::data_area_heads*     shared_heads = nullptr,
                   std::size_t                depth = 256)
            : geom(g)
            , fold_q(depth)
            , merge_step_q(depth)
            , merge_reads_done_q(depth)
            , finalize_merge_q(depth)
            , begin_update_superblock_q(depth)
            , finish_update_superblock_q(depth)
            , finalize_q(depth)
        {
            state.alloc.head         = data_area_base;
            state.alloc.shared_heads = shared_heads;
            if (geom != nullptr) {
                state.alloc.range_lbas =
                    static_cast<uint32_t>(geom->range_lbas());
                state.alloc.shadow_slots = geom->shadow_slots_per_range;
            }
        }

        void
        schedule_fold(_flush_fold::req* r) {
            fold_q.try_enqueue(r);
        }

        void
        schedule_merge_step(_merge_step::req* r) {
            merge_step_q.try_enqueue(r);
        }

        void
        schedule_merge_reads_done(_merge_reads_done::req* r) {
            merge_reads_done_q.try_enqueue(r);
        }

        void
        schedule_finalize_merge(_finalize_merge::req* r) {
            finalize_merge_q.try_enqueue(r);
        }

        void
        schedule_begin_update_superblock(_begin_update_superblock::req* r) {
            begin_update_superblock_q.try_enqueue(r);
        }

        void
        schedule_finish_update_superblock(_finish_update_superblock::req* r) {
            finish_update_superblock_q.try_enqueue(r);
        }

        void
        schedule_finalize(_finalize_flush_round::req* r) {
            finalize_q.try_enqueue(r);
        }

        auto
        submit_flush_fold(tree_flush_request args) {
            return _flush_fold::sender{ this, std::move(args) };
        }

        auto
        submit_merge_step(merge_loop_state* ls) {
            return _merge_step::sender{ this, ls };
        }

        auto
        submit_merge_reads_done() {
            return _merge_reads_done::sender{ this };
        }

        auto
        submit_finalize_merge(merge_finalize_request args) {
            return _finalize_merge::sender{ this, std::move(args) };
        }

        auto
        submit_begin_update_superblock(update_superblock_request args) {
            return _begin_update_superblock::sender{ this, std::move(args) };
        }

        auto
        submit_finish_update_superblock(finish_update_superblock_request args) {
            return _finish_update_superblock::sender{ this, std::move(args) };
        }

        auto
        submit_finalize_flush_round(finalize_flush_request args) {
            return _finalize_flush_round::sender{ this, std::move(args) };
        }

        uint64_t
        recompute_recovery_safe_lsn() const {
            return std::min(state.flush_max_lsn, state.superblock_safe_lsn);
        }

        // Builds the merge_step_decision for the just-yielded state.
        // Moves staged pending_ios out of `active_merge` into the
        // returned need_io payload. Scans once to precompute
        // `has_reads` so the outer pipeline knows whether to ACK
        // back via `submit_merge_reads_done`.
        merge_step_decision
        build_merge_step_decision_after_yield() {
            auto& active = *state.active_merge;
            if (active.coro->done()) {
                return merge_step_decision{ merge_step_done{} };
            }
            switch (active.coro->current()) {
                case merge_yield::need_io: {
                    bool has_reads = false;
                    for (const auto& d : active.pending_ios) {
                        if (std::holds_alternative<format::read_desc>(d)) {
                            has_reads = true;
                            break;
                        }
                    }
                    merge_step_need_io io{
                        .ios       = std::move(active.pending_ios),
                        .has_reads = has_reads,
                    };
                    active.pending_ios.clear();
                    if (has_reads) active.waiting_for_reads = true;
                    return merge_step_decision{ std::move(io) };
                }
                case merge_yield::yield_cpu:
                    // No IO this round; outer pipeline will loop back
                    // and drain other schedulers on the next main-loop tick.
                    return merge_step_decision{
                        merge_step_need_io{ {}, false }
                    };
                case merge_yield::done:
                    return merge_step_decision{ merge_step_done{} };
            }
            __builtin_unreachable();
        }

        // Rollback path shared by `finalize_merge(flush_ok=false)` and
        // any future error path that must release allocator ranges
        // already recorded in `active_merge.allocated_ranges`.
        void
        rollback_allocated_ranges_from_merge(merge_round_state& active) {
            for (auto it = active.allocated_ranges.rbegin();
                 it != active.allocated_ranges.rend(); ++it) {
                state.alloc.push_back_bump(*it);
            }
            active.allocated_ranges.clear();
        }

        // ── fold request validation + round allocation ───────────
        //
        // Extracted from the old monolithic advance(). Runs every
        // fold_q req through input validation, allocates a
        // `flush_round_state` in `state.active_rounds`, computes
        // the workset + partitions, and fires the cb with the
        // appropriate `flush_fold_result`.
        void
        handle_fold_req(_flush_fold::req* r) {
            if (r->args.base_guard == nullptr) {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_fold_req",
                    "tree_flush_request.base_guard is null");
            }
            if (r->args.base_guard->manifest == nullptr) {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_fold_req",
                    "tree_flush_request.base_guard->manifest is null");
            }

            // Zero round — no sealed_gens, nothing to fold. Emit a
            // trivial ok result with round_id=0; the downstream
            // merge/finalize seams short-circuit on round_id==0.
            if (r->args.sealed_gens.empty()) {
                r->cb(flush_fold_result{
                    .round_id           = flush_round_id{0},
                    .st                 = flush_stage_status::ok,
                    .partitions         = {},
                    .base_manifest      = r->args.base_guard->manifest.get(),
                    .recovery_safe_lsn  = 0,
                });
                delete r;
                return;
            }

            validate_fold_sealed_gens(r->args.sealed_gens);

            auto round_id = flush_round_id{ state.next_round_id++ };
            auto& round   = allocate_fold_round(std::move(r->args), round_id);
            fold_pinned_gens(round);

            if (round.workset.empty()) {
                r->cb(flush_fold_result{
                    .round_id          = round_id,
                    .st                = flush_stage_status::ok,
                    .partitions        = {},
                    .base_manifest     = round.pinned_base_guard->manifest.get(),
                    .recovery_safe_lsn = round.recovery_safe_lsn,
                });
                delete r;
                return;
            }

            // Step 030: partition count is determined by the
            // installed `shard_partition_map`. `build_key_partitions`
            // panics internally if the map is not installed.
            auto partition_st = build_key_partitions(
                round, round.pinned_base_guard->manifest.get());

            if (partition_st != flush_stage_status::ok) {
                round.st = partition_st;
                r->cb(flush_fold_result{
                    .round_id          = round_id,
                    .st                = partition_st,
                    .partitions        = {},
                    .base_manifest     = round.pinned_base_guard->manifest.get(),
                    .recovery_safe_lsn = round.recovery_safe_lsn,
                });
                delete r;
                return;
            }

            r->cb(flush_fold_result{
                .round_id          = round_id,
                .st                = flush_stage_status::ok,
                .partitions        = std::move(round.partitions),
                .base_manifest     = round.pinned_base_guard->manifest.get(),
                .recovery_safe_lsn = round.recovery_safe_lsn,
            });
            delete r;
        }

        // Validates that `sealed_gens` are non-null, actually sealed,
        // and have distinct gen_ids. Panics on violation.
        static void
        validate_fold_sealed_gens(
            const absl::InlinedVector<std::shared_ptr<core::memtable_gen>, 8>&
                sealed_gens)
        {
            absl::flat_hash_set<uint64_t> seen_ids;
            for (const auto& g : sealed_gens) {
                if (g == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_sched::handle_fold_req",
                        "sealed_gens contains null gen");
                }
                if (g->st != core::memtable_gen::state::sealed) {
                    core::panic_inconsistency(
                        "tree::tree_sched::handle_fold_req",
                        "sealed_gens contains non-sealed gen");
                }
                if (!seen_ids.insert(g->gen_id).second) {
                    core::panic_inconsistency(
                        "tree::tree_sched::handle_fold_req",
                        "sealed_gens contains duplicate gen_id");
                }
            }
        }

        // Allocates a `flush_round_state` for a fresh fold round,
        // registers it in `state.active_rounds`, and returns a
        // reference. The round starts with `flushed_max_lsn` set to
        // `max(pinned_gens.max_lsn)`; workset + partitions are filled
        // in by subsequent fold stages.
        flush_round_state&
        allocate_fold_round(tree_flush_request args, flush_round_id round_id) {
            auto rs = std::make_unique<flush_round_state>();
            rs->round_id          = round_id;
            rs->pinned_base_guard = std::move(args.base_guard);
            rs->pinned_gens       = std::move(args.sealed_gens);
            rs->recovery_safe_lsn = args.recovery_safe_lsn;

            rs->flushed_max_lsn = 0;
            for (auto& g : rs->pinned_gens) {
                rs->flushed_max_lsn = std::max(rs->flushed_max_lsn, g->max_lsn);
            }

            auto [it, _] = state.active_rounds.emplace(
                round_id.v, std::move(rs));
            return *it->second;
        }

        // ── merge_step request ──
        //
        // Drives the merge coroutine one resume per call. The caller
        // passes a `merge_loop_state*` (kept in the outer pipeline's
        // PUMP context). First call (when `active_merge` is empty)
        // seeds and starts the coroutine; subsequent calls just
        // resume past the previous yield.
        //
        // `waiting_for_reads` gates concurrent-iter access: while
        // set, any call returns an empty need_io (iter no-op) so
        // we don't resume the coroutine before the outer pipeline
        // has ACK'd the pending reads via `submit_merge_reads_done`.
        void
        handle_merge_step_req(_merge_step::req* r) {
            auto* ls = r->ls;
            if (ls == nullptr) {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_merge_step_req",
                    "merge_loop_state pointer is null");
            }

            if (!state.active_merge.has_value()) {
                handle_merge_step_first_call(r, ls);
            } else {
                handle_merge_step_resume_call(r);
            }
        }

        // First call for this round: move payload out of the loop
        // state, seed `active_merge`, and — if the round actually
        // has work — start the coroutine and return its first yield.
        void
        handle_merge_step_first_call(_merge_step::req* r,
                                     merge_loop_state* ls)
        {
            const uint64_t round_id_v = ls->round_id.v;
            std::vector<worker_tree_proposal> proposals =
                std::move(ls->worker_proposals);

            // Zero round — no active_rounds entry, no merge to run.
            if (round_id_v == 0) {
                r->cb(merge_step_decision{ merge_step_done{} });
                delete r;
                return;
            }

            auto round_it = state.active_rounds.find(round_id_v);
            if (round_it == state.active_rounds.end()) {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_merge_step_first_call",
                    "round_id %lu not in active_rounds",
                    static_cast<unsigned long>(round_id_v));
            }
            for (const auto& wp : proposals) {
                if (wp.round_id.v != round_id_v) {
                    core::panic_inconsistency(
                        "tree::tree_sched::handle_merge_step_first_call",
                        "worker_proposal round_id mismatch");
                }
            }
            auto& round = *round_it->second;

            merge_round_state ms;
            ms.round_id         = ls->round_id;
            ms.worker_proposals = std::move(proposals);
            ms.st               = round.st;
            state.active_merge.emplace(std::move(ms));

            // Short-circuit: fold reported unsupported, or workset was
            // empty → no merge work. Leave `active_merge` seeded but
            // without a coroutine; finalize_merge will emit a done
            // variant with the gens_by_front result.
            if (round.st != flush_stage_status::ok
                || round.workset.empty())
            {
                r->cb(merge_step_decision{ merge_step_done{} });
                delete r;
                return;
            }

            auto& active = *state.active_merge;
            active.coro.emplace(_owner::run_merge(
                active,
                round.pinned_base_guard->manifest.get(),
                state.alloc,
                round.pinned_base_guard->manifest->geom,
                round));
            active.coro->resume();
            r->cb(build_merge_step_decision_after_yield());
            delete r;
        }

        // Subsequent call for this round: resume the coroutine past
        // its last yield and return whatever the new yield emitted.
        // Short-circuits early-exit paths (no coroutine / done /
        // waiting-for-reads) before touching the coroutine handle.
        void
        handle_merge_step_resume_call(_merge_step::req* r) {
            auto& active = *state.active_merge;

            if (!active.coro.has_value()) {
                // Short-circuit path seeded state without a coroutine.
                r->cb(merge_step_decision{ merge_step_done{} });
                delete r;
                return;
            }
            if (active.waiting_for_reads) {
                r->cb(merge_step_decision{
                    merge_step_need_io{ {}, false } });
                delete r;
                return;
            }
            if (active.coro->done()) {
                r->cb(merge_step_decision{ merge_step_done{} });
                delete r;
                return;
            }

            active.coro->resume();
            r->cb(build_merge_step_decision_after_yield());
            delete r;
        }

        // ── merge_reads_done request ──
        //
        // Clears `waiting_for_reads` so concurrent merge_step iters
        // can resume the coroutine. Called by the outer pipeline
        // after awaiting a batch of reads emitted by the coroutine's
        // previous yield.
        void
        handle_merge_reads_done_req(_merge_reads_done::req* r) {
            if (state.active_merge.has_value()) {
                state.active_merge->waiting_for_reads = false;
            }
            r->cb();
            delete r;
        }

        // ── finalize_merge request ──
        //
        // Called once per round after the outer pipeline drained all
        // merge_step iters AND issued a device FLUSH. Produces the
        // commit variant (done / root_stable / root_change), transfers
        // `new_manifest` onto `flush_round_state`, and clears
        // `active_merge`. On `flush_ok=false` rolls back allocator
        // ranges before emitting a failure-done variant.
        void
        handle_finalize_merge_req(_finalize_merge::req* r) {
            const uint64_t round_id_v = r->args.round_id.v;

            if (round_id_v == 0) {
                // Zero round — no active state to commit.
                r->cb(merge_finalize_result{
                    flush_merge_done{
                        .result = tree_flush_result{
                            .st              = flush_stage_status::ok,
                            .flushed_max_lsn = 0,
                        },
                    },
                });
                delete r;
                return;
            }

            validate_finalize_merge_round(round_id_v);

            auto& round  = *state.active_rounds.find(round_id_v)->second;
            auto& active = *state.active_merge;

            if (!r->args.flush_ok) {
                emit_finalize_merge_failure(r, round, active, round_id_v);
                return;
            }
            emit_finalize_merge_success(r, round, active, round_id_v);
        }

        // Common precondition check for the flush_ok=true/false paths.
        void
        validate_finalize_merge_round(uint64_t round_id_v) {
            if (!state.active_merge.has_value()) {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_finalize_merge_req",
                    "no active merge for round_id %lu",
                    static_cast<unsigned long>(round_id_v));
            }
            if (state.active_merge->round_id.v != round_id_v) {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_finalize_merge_req",
                    "active_merge round_id mismatch");
            }
            if (state.active_rounds.find(round_id_v)
                == state.active_rounds.end())
            {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_finalize_merge_req",
                    "round_id %lu not in active_rounds",
                    static_cast<unsigned long>(round_id_v));
            }
        }

        // flush_ok=false path: rollback allocator, drop active_merge,
        // erase the active round, emit a failure done variant.
        void
        emit_finalize_merge_failure(_finalize_merge::req* r,
                                    flush_round_state&    round,
                                    merge_round_state&    active,
                                    uint64_t              round_id_v)
        {
            rollback_allocated_ranges_from_merge(active);
            state.active_merge.reset();

            auto gens_by_front = build_flushed_gens_by_front(round.pinned_gens);
            auto flushed_max_lsn_v = round.flushed_max_lsn;
            state.active_rounds.erase(round_id_v);

            r->cb(merge_finalize_result{
                flush_merge_done{
                    .result = tree_flush_result{
                        .st                    = flush_stage_status::unsupported_unimplemented,
                        .new_manifest          = nullptr,
                        .retired               = {},
                        .flushed_gens_by_front = std::move(gens_by_front),
                        .flushed_max_lsn       = flushed_max_lsn_v,
                    },
                },
            });
            delete r;
        }

        // flush_ok=true path: dispatch across coroutine-ran vs
        // short-circuit vs root_change / root_stable.
        void
        emit_finalize_merge_success(_finalize_merge::req* r,
                                    flush_round_state&    round,
                                    merge_round_state&    active,
                                    uint64_t              round_id_v)
        {
            const bool had_coro              = active.coro.has_value();
            const bool is_root_change        = active.is_root_change;
            const paddr new_root_base_paddr  = active.new_root_base_paddr;
            auto       new_manifest_owned   = std::move(active.new_manifest);
            const flush_stage_status active_st = active.st;

            // Early short-circuit (coroutine never ran / fold failure):
            // nothing durable was committed; emit done with empty result.
            if (!had_coro || active_st != flush_stage_status::ok) {
                state.active_merge.reset();
                auto gens_by_front = build_flushed_gens_by_front(
                    round.pinned_gens);
                auto flushed_max_lsn_v = round.flushed_max_lsn;
                state.active_rounds.erase(round_id_v);
                r->cb(merge_finalize_result{
                    flush_merge_done{
                        .result = tree_flush_result{
                            .st                    = active_st,
                            .new_manifest          = nullptr,
                            .retired               = {},
                            .flushed_gens_by_front = std::move(gens_by_front),
                            .flushed_max_lsn       = flushed_max_lsn_v,
                        },
                    },
                });
                delete r;
                return;
            }

            // Happy path: transfer new_manifest onto the round so
            // finalize_flush_round can commit it.
            round.new_manifest = std::move(new_manifest_owned);
            state.active_merge.reset();

            if (is_root_change) {
                r->cb(merge_finalize_result{
                    flush_merge_root_change{
                        .update_req = update_superblock_request{
                            .round_id            = flush_round_id{round_id_v},
                            .new_root_base_paddr = new_root_base_paddr,
                        },
                    },
                });
            } else {
                r->cb(merge_finalize_result{
                    flush_merge_root_stable{
                        .finalize_req = finalize_flush_request{
                            .round_id       = flush_round_id{round_id_v},
                            .ok             = true,
                            .committed_slot = std::nullopt,
                        },
                    },
                });
            }
            delete r;
        }

        // ── begin_update_superblock request ──
        //
        // Latches `update_superblock_inflight=true`, computes the
        // active/inactive LBA pair, and hands it to the outer pipeline.
        // The outer pipeline does the NVMe read / mutate / FUA write,
        // then calls `submit_finish_update_superblock`.
        void
        handle_begin_update_superblock_req(_begin_update_superblock::req* r)
        {
            if (geom == nullptr) {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_begin_update_superblock_req",
                    "tree geometry is null");
            }

            const bool a_is_active =
                state.active_superblock_slot == superblock_slot::A;
            const uint64_t active_lba    = a_is_active ? 0 : 1;
            const uint64_t inactive_lba  = a_is_active ? 1 : 0;
            const superblock_slot inactive_slot =
                a_is_active ? superblock_slot::B : superblock_slot::A;

            update_superblock_inflight = true;

            r->cb(begin_update_superblock_result{
                .round_id            = r->args.round_id,
                .new_root_base_paddr = r->args.new_root_base_paddr,
                .active_lba          = active_lba,
                .inactive_lba        = inactive_lba,
                .inactive_slot       = inactive_slot,
                .lba_size            = geom->lba_size,
            });
            delete r;
        }

        // ── finish_update_superblock request ──
        //
        // Clears `update_superblock_inflight` and surfaces the
        // outer pipeline's write outcome as an `update_superblock_result`.
        void
        handle_finish_update_superblock_req(_finish_update_superblock::req* r)
        {
            if (!update_superblock_inflight) {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_finish_update_superblock_req",
                    "finish without an inflight begin for round_id=%lu",
                    static_cast<unsigned long>(r->args.round_id.v));
            }
            update_superblock_inflight = false;

            r->cb(update_superblock_result{
                .round_id       = r->args.round_id,
                .ok             = r->args.write_ok,
                .committed_slot = r->args.inactive_slot,
            });
            delete r;
        }

        // ── finalize_flush_round request ──
        //
        // The very last seam of the flush pipeline's commit phase.
        // Consumes `active_rounds[round_id]`, updates tree_state
        // counters (flush_max_lsn / superblock_safe_lsn / recovery_safe_lsn)
        // and optionally flips `active_superblock_slot`, then returns
        // the final `tree_flush_result`.
        void
        handle_finalize_flush_round_req(_finalize_flush_round::req* r) {
            const uint64_t round_id_v = r->args.round_id.v;

            if (round_id_v == 0) {
                r->cb(tree_flush_result{
                    .st              = flush_stage_status::ok,
                    .flushed_max_lsn = 0,
                });
                delete r;
                return;
            }

            auto round_it = state.active_rounds.find(round_id_v);
            if (round_it == state.active_rounds.end()) {
                core::panic_inconsistency(
                    "tree_sched::handle_finalize_flush_round_req",
                    "round_id %lu not in active_rounds",
                    static_cast<unsigned long>(round_id_v));
            }

            auto round = std::move(round_it->second);
            state.active_rounds.erase(round_it);

            if (r->args.committed_slot.has_value()) {
                state.active_superblock_slot = *r->args.committed_slot;
            }
            state.flush_max_lsn = std::max(
                state.flush_max_lsn, round->flushed_max_lsn);
            state.superblock_safe_lsn = std::max(
                state.superblock_safe_lsn, round->flushed_max_lsn);
            state.recovery_safe_lsn = recompute_recovery_safe_lsn();

            r->cb(tree_flush_result{
                .st                    = flush_stage_status::ok,
                .new_manifest          = round->new_manifest,
                .retired               = std::move(round->retired),
                .flushed_gens_by_front = build_flushed_gens_by_front(
                                             round->pinned_gens),
                .flushed_max_lsn       = round->flushed_max_lsn,
            });
            delete r;
        }

        // Dequeues up to `max_ops` requests from `q` and dispatches
        // each via the per-req member handler. Returns true iff at
        // least one request was processed this tick. Stops early on
        // queue empty. Each handler is responsible for its own
        // `delete r` after firing the cb.
        template <typename Q, typename Handler>
        bool
        drain_queue(Q& q, uint32_t max_ops, Handler&& handler) {
            bool progress = false;
            for (uint32_t i = 0; i < max_ops; ++i) {
                auto item = q.try_dequeue();
                if (!item) break;
                handler(*item);
                progress = true;
            }
            return progress;
        }

        // Scheduler main tick. Drains each request queue up to its
        // per-advance cap and dispatches to the matching per-req
        // handler. Ordering matters: fold → merge_step → reads_done
        // → finalize_merge → begin/finish_update_superblock → finalize.
        // The begin_update_superblock drain is additionally gated on
        // `update_superblock_inflight` to serialize the begin→finish
        // pair.
        bool
        advance() {
            bool progress = false;

            progress |= drain_queue(
                fold_q, kMaxFoldOpsPerAdvance,
                [this](_flush_fold::req* r) { handle_fold_req(r); });

            progress |= drain_queue(
                merge_step_q, kMaxMergeStepOpsPerAdvance,
                [this](_merge_step::req* r) { handle_merge_step_req(r); });

            progress |= drain_queue(
                merge_reads_done_q, kMaxMergeReadsDoneOpsPerAdvance,
                [this](_merge_reads_done::req* r) {
                    handle_merge_reads_done_req(r);
                });

            progress |= drain_queue(
                finalize_merge_q, kMaxFinalizeMergeOpsPerAdvance,
                [this](_finalize_merge::req* r) {
                    handle_finalize_merge_req(r);
                });

            // begin_update_superblock is gated on `!inflight` so the
            // begin→finish pair stays serialized; cap the drain count
            // at the lesser of the per-advance limit and the inflight
            // gate (which effectively makes it at most 1 per tick).
            if (!update_superblock_inflight) {
                progress |= drain_queue(
                    begin_update_superblock_q,
                    kMaxBeginUpdateSuperblockOpsPerAdvance,
                    [this](_begin_update_superblock::req* r) {
                        handle_begin_update_superblock_req(r);
                    });
            }

            progress |= drain_queue(
                finish_update_superblock_q,
                kMaxFinishUpdateSuperblockOpsPerAdvance,
                [this](_finish_update_superblock::req* r) {
                    handle_finish_update_superblock_req(r);
                });

            progress |= drain_queue(
                finalize_q, kMaxFinalizeOpsPerAdvance,
                [this](_finalize_flush_round::req* r) {
                    handle_finalize_flush_round_req(r);
                });

            return progress;
        }

        template <typename runtime_t>
        bool
        advance(runtime_t&) {
            return advance();
        }
    };

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _flush_fold::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_fold(new _flush_fold::req{
            std::move(args),
            [ctx = ctx, scope = scope](flush_fold_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _merge_step::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_merge_step(new _merge_step::req{
            ls,
            [ctx = ctx, scope = scope](merge_step_decision&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _merge_reads_done::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_merge_reads_done(new _merge_reads_done::req{
            [ctx = ctx, scope = scope]() mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope);
            },
        });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _finalize_merge::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_finalize_merge(new _finalize_merge::req{
            std::move(args),
            [ctx = ctx, scope = scope](merge_finalize_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _begin_update_superblock::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_begin_update_superblock(new _begin_update_superblock::req{
            std::move(args),
            [ctx = ctx, scope = scope](begin_update_superblock_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _finish_update_superblock::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_finish_update_superblock(new _finish_update_superblock::req{
            std::move(args),
            [ctx = ctx, scope = scope](update_superblock_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _finalize_flush_round::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_finalize(new _finalize_flush_round::req{
            std::move(args),
            [ctx = ctx, scope = scope](tree_flush_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

}  // namespace apps::inconel::tree

namespace pump::core {

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::flush_fold_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_flush_fold::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::flush_fold_result>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::merge_step_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_merge_step::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<
                apps::inconel::tree::merge_step_decision>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::merge_reads_done_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_merge_reads_done::sender> {
        consteval static uint32_t
        count_value() {
            return 0;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::finalize_merge_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_finalize_merge::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<
                apps::inconel::tree::merge_finalize_result>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::begin_update_superblock_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_begin_update_superblock::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<
                apps::inconel::tree::begin_update_superblock_result>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::finish_update_superblock_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_finish_update_superblock::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<
                apps::inconel::tree::update_superblock_result>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::finalize_flush_round_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_finalize_flush_round::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::tree_flush_result>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_TREE_OWNER_SCHEDULER_HH
