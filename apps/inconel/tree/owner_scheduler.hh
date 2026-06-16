#ifndef APPS_INCONEL_TREE_OWNER_SCHEDULER_HH
#define APPS_INCONEL_TREE_OWNER_SCHEDULER_HH

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
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
#include "../core/tree_read_domain.hh"
#include "../core/wal_reclaim_frontier.hh"
#include "../format/superblock.hh"
#include "../format/types.hh"
#include "../memory/dma_page_pool.hh"
#include "../nvme/runtime_scheduler.hh"
#include "./flush_round_state.hh"
#include "./flush_types.hh"
#include "./memtable_fold.hh"
#include "./page_builder.hh"
#include "./page_reader.hh"

namespace apps::inconel::tree {

    struct tree_sched;

    struct recovery_frontier_snapshot {
        uint64_t flush_durable_frontier = 0;
        uint64_t recovery_safe_lsn = 0;
    };

}  // namespace apps::inconel::tree

namespace apps::inconel::core::registry {
    inline nvme::runtime_scheduler* local_nvme();
    inline void post_value_reclaim_values(
        std::vector<format::value_ref>&& dead_values);
    inline void post_wal_reclaim_check(uint64_t flush_durable_frontier);
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
            if (!free_ranges.try_enqueue(std::move(r))) {
                core::panic_inconsistency(
                    "tree_allocator::recycle",
                    "free range queue full");
            }
        }
    };

    struct reclaim_trim_completion {
        uint64_t round_id = 0;
        bool recycle_range = false;
        format::range_ref range{};
    };

    struct reclaim_invalidate_completion {
        uint64_t round_id = 0;
        bool recycle_range = false;
        format::range_ref range{};
    };

    struct tree_mutation_token {
        uint64_t ticket = 0;
    };

    namespace _mutation_gate_acquire {

        struct req {
            std::move_only_function<void(tree_mutation_token)> cb;
        };

        struct op {
            constexpr static bool mutation_gate_acquire_op = true;

            tree_sched* sched = nullptr;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched* sched = nullptr;

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

    }  // namespace _mutation_gate_acquire

    namespace _mutation_gate_release {

        struct req {
            tree_mutation_token token{};
            std::move_only_function<void()> cb;
        };

        struct op {
            constexpr static bool mutation_gate_release_op = true;

            tree_sched* sched = nullptr;
            tree_mutation_token token{};

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched* sched = nullptr;
            tree_mutation_token token{};

            auto
            make_op() {
                return op{ .sched = sched, .token = token };
            }

            template <typename ctx_t>
            auto
            connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };

    }  // namespace _mutation_gate_release

    struct tree_mutation_gate {
        bool held = false;
        uint64_t active_ticket = 0;
        uint64_t next_ticket = 1;
        std::deque<_mutation_gate_acquire::req*> waiters;
    };

    struct active_reclaim_round {
        tree_mutation_token token{};
        uint64_t round_id = 0;
        uint32_t pending_invalidations = 0;
        uint32_t pending_trims = 0;
        uint32_t processed_tasks = 0;
        std::vector<format::value_ref> reclaim_now;
    };

    // ── merge coroutine status codes ─────────────────────────────
    //
    // The yield is a pure status tag: the coroutine stages the
    // actual read/write descriptors into `merge_round_state::pending_ios`
    // before co_yield'ing, and the `submit_merge_step` handler
    // moves them out into the decision payload. See `run_leaf_only_merge()`.
    enum class merge_yield : uint8_t {
        need_io,    // pending_ios staged; outer pipeline must drive them
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
    //   - `worker_leaf_chains` owns every worker-formatted leaf page.
    //   - `detached_subtrees` owns same-range stop subtrees whose
    //     writes/slot-map updates commit without propagating further.
    //   - `fetched_old_frames[slot_paddr]` is the segmented DMA frame the
    //     outer pipeline reads into for owner-side old-internal pages.
    //   - `pending_ios[i]` is a frame read/write descriptor whose frame
    //     lives in `fetched_old_frames` or `writeback_frames`.
    //   - `retired_slots_seen` / `retired_ranges_seen` dedupe inserts
    //     into the linked `flush_round_state.retired` vectors.
    struct merge_round_state {
        flush_round_id                             round_id;
        std::vector<worker_leaf_chain>             worker_leaf_chains;
        absl::flat_hash_map<
            paddr,
            memory::pooled_frame_ptr<memory::segmented_tree_frame>>
                                                    fetched_old_frames;
        std::vector<
            memory::pooled_frame_ptr<memory::segmented_tree_frame>>
                                                    writeback_frames;

        child_ref                                     combined_root;
        std::vector<child_ref>                        detached_subtrees;
        absl::flat_hash_map<paddr, paddr>             leaf_parent_override;
        absl::flat_hash_map<paddr, paddr>             internal_parent_override;
        std::shared_ptr<const core::tree_manifest>    new_manifest;
        bool                                          is_root_change = false;
        paddr                                         new_root_base_paddr{};
        std::vector<format::range_ref>                allocated_ranges;
        absl::flat_hash_set<paddr>                    retired_slots_seen;
        absl::flat_hash_set<paddr>                    retired_ranges_seen;

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

        // Owner-only segmented non-leaf cache keyed by live slot paddr.
        // Read domains never touch non-leaf pages after INC-046.
        absl::flat_hash_map<
            paddr,
            memory::pooled_frame_ptr<memory::segmented_tree_frame>>
            non_leaf_page_cache;

        pump::core::mpmc::queue<core::reclaim_task*> reclaim_q{256};
        std::deque<core::reclaim_task*> pending_reclaim;
        std::vector<core::retired_value_ref> deferred_value_reclaim;
        pump::core::mpmc::queue<reclaim_invalidate_completion*>
            reclaim_invalidate_done_q{256};
        pump::core::mpmc::queue<reclaim_trim_completion*> reclaim_trim_done_q{256};
        tree_mutation_gate mutation_gate;
        std::optional<active_reclaim_round> active_reclaim;
        bool reclaim_gate_requested = false;
        uint64_t next_reclaim_round_id = 1;

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

    namespace _recompute_recovery_frontier {

        struct req {
            std::move_only_function<void(recovery_frontier_snapshot&&)> cb;
        };

        struct op {
            constexpr static bool recompute_recovery_frontier_op = true;

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

    }  // namespace _recompute_recovery_frontier

    namespace _reclaim_trim_complete {

        struct receiver {
            constexpr static bool reclaim_trim_complete_receiver_op = true;

            tree_sched* sched = nullptr;
            uint64_t round_id = 0;
            bool recycle_range = false;
            format::range_ref range{};
        };

    }  // namespace _reclaim_trim_complete

    namespace _owner {

        using format::paddr;

        static inline memory::pooled_frame_ptr<memory::segmented_tree_frame>
        alloc_tree_frame(memory::lba_dma_page_pool& pool,
                         paddr page_base,
                         uint32_t page_lbas,
                         bool zero_fill = false) {
            auto frame = pool.get_typed_frame<memory::segmented_tree_frame>(
                memory::frame_id{
                    page_base,
                    static_cast<uint16_t>(page_lbas),
                    memory::frame_id::domain::tree_node,
                },
                memory::frame_state::clean_readonly,
                zero_fill);
            if (!frame) {
                core::panic_inconsistency(
                    "tree::_owner::alloc_tree_frame",
                    "DMA page allocation failed dev=%u lba=%lu span=%u",
                    static_cast<unsigned>(page_base.device_id),
                    static_cast<unsigned long>(page_base.lba),
                    static_cast<unsigned>(page_lbas));
            }
            return memory::pooled_frame_ptr<memory::segmented_tree_frame>(
                &pool,
                new memory::segmented_tree_frame(std::move(*frame)));
        }

        struct final_leaf_item {
            paddr                   range_base{};
            bool                    is_new = false;
            const mem_tree_node*    new_leaf = nullptr;
            const core::leaf_span*  base_span = nullptr;
            std::string_view        first_key;
        };

        struct leaf_replacement_output {
            paddr            range_base{};
            std::string_view first_key;
        };

        struct leaf_replacement {
            uint32_t old_leaf_idx = UINT32_MAX;
            std::vector<leaf_replacement_output> outputs;
        };

        struct child_interface_delta {
            paddr               old_child_range_base{};
            core::internal_idx  parent_idx = core::kInvalidInternalIdx;
            std::vector<std::unique_ptr<mem_tree_node>> nodes;
            std::vector<std::string>                    sibling_seps;
        };

        struct merge_context {
            const core::tree_manifest* base_manifest = nullptr;
            const core::tree_geometry* geom = nullptr;

            absl::flat_hash_map<paddr, uint32_t> base_leaf_index_by_range;
            absl::flat_hash_map<paddr, uint32_t> base_internal_index_by_range;

            struct subtree_interval {
                uint32_t begin = UINT32_MAX;
                uint32_t end   = 0;
            };
            std::vector<subtree_interval> base_internal_leaf_ranges;
            std::vector<std::vector<uint32_t>> base_internal_children;
        };

        static inline bool
        is_leaf_range(const merge_context& ctx, paddr rb) {
            return ctx.base_leaf_index_by_range.contains(rb);
        }

        static inline bool
        is_internal_range(const merge_context& ctx, paddr rb) {
            return ctx.base_internal_index_by_range.contains(rb);
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
                    return (*n)->new_range_base;
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

        static inline absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1>
        build_internal_pages_owner_sv(
            std::vector<child_ref>&&             new_children,
            std::vector<std::string_view>&&      new_separators,
            paddr                                replaces_old_paddr,
            bool                                 is_new_layer,
            uint32_t                             page_size,
            std::vector<std::string>&            sibling_seps_out)
        {
            sibling_seps_out.clear();

            absl::InlinedVector<std::unique_ptr<mem_tree_node>, 1> result;
            if (new_children.empty()) {
                core::panic_inconsistency(
                    "build_internal_pages_owner_sv",
                    "internal node has zero children");
            }

            auto child_to_range = [](const child_ref& c) -> paddr {
                if (auto* p = std::get_if<paddr>(&c.target)) return *p;
                if (auto* n = std::get_if<std::unique_ptr<mem_tree_node>>(&c.target)) {
                    return (*n)->new_range_base;
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
                        "build_internal_pages_owner_sv",
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
                            child_to_range(new_children[next + j])))
                    {
                        core::panic_inconsistency(
                            "build_internal_pages_owner_sv",
                            "add_child failed despite size precomputation");
                    }
                }
                builder.set_rightmost_child(
                    child_to_range(new_children[next + K - 1]));
                builder.finalize();

                node->children.reserve(K);
                node->separators.reserve(K > 0 ? K - 1 : 0);
                for (uint32_t j = 0; j < K; ++j) {
                    node->children.push_back(std::move(new_children[next + j]));
                }
                for (uint32_t j = 0; j + 1 < K; ++j) {
                    node->separators.emplace_back(new_separators[next + j]);
                }
                result.push_back(std::move(node));

                if (next + K < new_children.size()) {
                    sibling_seps_out.emplace_back(
                        new_separators[next + K - 1]);
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
                                     node->children[i].target)->new_range_base;
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
                                node->children.back().target)->new_range_base;
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
                // CLAUDE.md constraint A: a non-first leaf with an
                // empty fence_lower would break `find_leaf_for_key`'s
                // binary search (empty compares as -∞ and makes
                // every span accept the key) and
                // `build_initial_shard_partition_map` (panics on
                // non-last empty fence_upper which back-references
                // this same value). Two known ways this slot can
                // go empty:
                //   (a) an unchanged base leaf that used to sit at
                //       position 0 in base_manifest (fence_lower=∅)
                //       gets displaced to position >0 in the new
                //       manifest — the walker has no ready source
                //       for a real fence key since the leaf's
                //       content was not re-read; the correct fix is
                //       to derive fence_lower from the new tree's
                //       parent-internal separator rather than
                //       copying base_manifest's stored value.
                //   (b) a new leaf with zero records slipped into
                //       the full-tree bootstrap builder — shouldn't
                //       happen, but
                //       detect here rather than let a silent +∞
                //       sentinel propagate into the new manifest.
                // In either case, the merge pipeline handed the
                // builder data outside its current support surface.
                // Panic with the offending position so a follow-up
                // step can triage which case tripped it.
                if (lowers[i].empty()) {
                    core::panic_inconsistency(
                        "build_leaf_order_full",
                        "non-first leaf %zu has empty fence_lower "
                        "(is_new=%d) — walker cannot reuse "
                        "base_manifest's position-0 fence_lower=∅ at "
                        "a non-zero position, and an empty-content "
                        "new leaf should not reach the full-tree "
                        "builder. "
                        "See INC-043 follow-up: derive fence_lower "
                        "from the new tree structure for displaced "
                        "base leaves.",
                        i,
                        leaf_items[i].is_new ? 1 : 0);
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

        static inline bool
        leaf_node_is_empty(const mem_tree_node* node,
                           uint32_t             page_size)
        {
            if (node == nullptr || node->type != format::node_type::leaf) {
                core::panic_inconsistency(
                    "leaf_node_is_empty",
                    "expected leaf node");
            }
            leaf_page_reader reader;
            if (!reader.parse(node->content.data(), page_size)) {
                core::panic_inconsistency(
                    "leaf_node_is_empty",
                    "leaf page failed validation");
            }
            return reader.record_count() == 0;
        }

        static inline std::string_view
        first_key_of_leaf_node(const mem_tree_node* node,
                               uint32_t             page_size)
        {
            if (node == nullptr || node->type != format::node_type::leaf) {
                core::panic_inconsistency(
                    "first_key_of_leaf_node",
                    "expected leaf node");
            }
            leaf_page_reader reader;
            if (!reader.parse(node->content.data(), page_size)) {
                core::panic_inconsistency(
                    "first_key_of_leaf_node",
                    "leaf page failed validation");
            }
            if (reader.record_count() == 0) return {};
            return reader.get(0).key;
        }

        static inline std::unique_ptr<mem_tree_node>
        make_leaf_node_from_image(leaf_page_image&& image,
                                  paddr            old_range_base,
                                  bool             carries_old_range)
        {
            auto node = std::make_unique<mem_tree_node>();
            node->type = format::node_type::leaf;
            node->content = std::move(image.page);
            if (carries_old_range) {
                node->replaces_old_paddrs.push_back(old_range_base);
            }
            return node;
        }

        static inline void
        plan_fresh_node(mem_tree_node*                 node,
                        tree_allocator&                alloc,
                        std::vector<format::range_ref>& allocated_ranges)
        {
            auto r = alloc.allocate();
            allocated_ranges.push_back(r);
            node->new_range_base = r.base;
            node->new_slot_index = 0;
            node->new_paddr      = r.base;
        }

        template <typename NodeVec, typename retire_slot_fn, typename retire_range_fn>
        static inline void
        plan_group_against_old_range(
            NodeVec&                          nodes,
            paddr                             old_range_base,
            const core::tree_manifest*        base_manifest,
            tree_allocator&                   alloc,
            std::vector<format::range_ref>&   allocated_ranges,
            retire_slot_fn&&                  retire_slot,
            retire_range_fn&&                 retire_range)
        {
            if (nodes.empty()) {
                retire_range(old_range_base);
                return;
            }

            const auto cur_slot = base_manifest->slot_index(old_range_base);
            const bool can_reuse_old =
                cur_slot + 1 < base_manifest->geom->shadow_slots_per_range;

            if (can_reuse_old) {
                auto* node = nodes.front().get();
                node->new_range_base = old_range_base;
                node->new_slot_index = cur_slot + 1;
                node->new_paddr      = base_manifest->geom->slot_paddr(
                    old_range_base, cur_slot + 1);
                retire_slot(base_manifest->resolve(old_range_base));
            } else {
                retire_range(old_range_base);
                plan_fresh_node(nodes.front().get(), alloc, allocated_ranges);
            }

            for (std::size_t i = 1; i < nodes.size(); ++i) {
                plan_fresh_node(nodes[i].get(), alloc, allocated_ranges);
            }
        }

        template <typename NodeVec>
        static inline std::vector<std::unique_ptr<mem_tree_node>>
        move_node_vector(NodeVec&& nodes)
        {
            std::vector<std::unique_ptr<mem_tree_node>> out;
            out.reserve(nodes.size());
            for (auto& node : nodes) {
                out.push_back(std::move(node));
            }
            return out;
        }

        static inline void
        record_direct_child_parent_overrides(
            const merge_context&                        ctx,
            const mem_tree_node*                        parent,
            absl::flat_hash_map<paddr, paddr>&          leaf_parent_override,
            absl::flat_hash_map<paddr, paddr>&          internal_parent_override)
        {
            if (parent == nullptr || parent->type != format::node_type::internal) {
                return;
            }
            const auto parent_rb = parent->new_range_base;
            for (const auto& child : parent->children) {
                if (auto* p = std::get_if<paddr>(&child.target)) {
                    if (ctx.base_leaf_index_by_range.contains(*p)) {
                        leaf_parent_override[*p] = parent_rb;
                    } else if (ctx.base_internal_index_by_range.contains(*p)) {
                        internal_parent_override[*p] = parent_rb;
                    } else {
                        core::panic_inconsistency(
                            "record_direct_child_parent_overrides",
                            "child range dev=%u lba=%lu is neither base leaf nor base internal",
                            static_cast<unsigned>(p->device_id),
                            static_cast<unsigned long>(p->lba));
                    }
                    continue;
                }

                auto* node =
                    std::get<std::unique_ptr<mem_tree_node>>(child.target).get();
                if (node->type == format::node_type::leaf) {
                    leaf_parent_override[node->new_range_base] = parent_rb;
                } else {
                    internal_parent_override[node->new_range_base] = parent_rb;
                }
            }
        }

        static inline void
        collect_nodes_for_writes(const child_ref& ref,
                                 std::vector<const mem_tree_node*>& out)
        {
            auto* node_up = std::get_if<std::unique_ptr<mem_tree_node>>(&ref.target);
            if (node_up == nullptr) return;
            auto* node = node_up->get();
            out.push_back(node);
            for (const auto& child : node->children) {
                collect_nodes_for_writes(child, out);
            }
        }

        static inline core::leaf_order_index
        build_leaf_order_from_replacements(
            const core::tree_manifest*            base_manifest,
            const std::vector<leaf_replacement>&  replacements)
        {
            if (base_manifest == nullptr) {
                core::panic_inconsistency(
                    "build_leaf_order_from_replacements",
                    "base_manifest is null");
            }

            absl::flat_hash_map<uint32_t, const leaf_replacement*> repl_by_idx;
            repl_by_idx.reserve(replacements.size());
            for (const auto& repl : replacements) {
                auto [it, inserted] =
                    repl_by_idx.emplace(repl.old_leaf_idx, &repl);
                if (!inserted) {
                    core::panic_inconsistency(
                        "build_leaf_order_from_replacements",
                        "duplicate replacement for old leaf idx=%u",
                        repl.old_leaf_idx);
                }
            }

            struct final_leaf_span {
                paddr            range_base{};
                std::string_view lower;
                std::string_view upper;
            };

            std::vector<final_leaf_span> final_spans;
            final_spans.reserve(base_manifest->leaf_order.spans.size()
                                + replacements.size());

            const auto& lo = base_manifest->leaf_order;
            for (uint32_t i = 0; i < lo.spans.size(); ++i) {
                auto it = repl_by_idx.find(i);
                if (it == repl_by_idx.end()) {
                    final_spans.push_back(final_leaf_span{
                        .range_base = lo.spans[i].leaf_range_base,
                        .lower      = lo.fence_lower(lo.spans[i]),
                        .upper      = lo.fence_upper(lo.spans[i]),
                    });
                    continue;
                }

                const auto& repl = *it->second;
                if (repl.outputs.empty()) {
                    continue;
                }

                if (repl.outputs.size() == 1) {
                    final_spans.push_back(final_leaf_span{
                        .range_base = repl.outputs[0].range_base,
                        .lower      = lo.fence_lower(lo.spans[i]),
                        .upper      = lo.fence_upper(lo.spans[i]),
                    });
                    continue;
                }

                auto lower = lo.fence_lower(lo.spans[i]);
                for (std::size_t out_idx = 0; out_idx < repl.outputs.size(); ++out_idx) {
                    std::string_view upper;
                    if (out_idx + 1 < repl.outputs.size()) {
                        upper = repl.outputs[out_idx + 1].first_key;
                        if (upper.empty()) {
                            core::panic_inconsistency(
                                "build_leaf_order_from_replacements",
                                "split output %zu of leaf_idx=%u has empty first key",
                                out_idx + 1,
                                static_cast<unsigned>(i));
                        }
                    } else {
                        upper = lo.fence_upper(lo.spans[i]);
                    }
                    final_spans.push_back(final_leaf_span{
                        .range_base = repl.outputs[out_idx].range_base,
                        .lower      = lower,
                        .upper      = upper,
                    });
                    lower = upper;
                }
            }

            if (final_spans.empty()) {
                return {};
            }

            core::leaf_order_index out;
            out.spans.reserve(final_spans.size());

            auto append_fence = [&](std::string_view fence) {
                const auto off = static_cast<uint32_t>(out.fence_pool.size());
                out.fence_pool.append(fence.data(), fence.size());
                return std::pair<uint32_t, uint16_t>{
                    off,
                    static_cast<uint16_t>(fence.size()),
                };
            };

            std::pair<uint32_t, uint16_t> next_lower = append_fence(final_spans[0].lower);
            for (std::size_t i = 0; i < final_spans.size(); ++i) {
                auto lower = next_lower;
                std::pair<uint32_t, uint16_t> upper;
                if (i + 1 < final_spans.size()) {
                    if (final_spans[i].upper.empty()) {
                        core::panic_inconsistency(
                            "build_leaf_order_from_replacements",
                            "non-last leaf %zu has empty upper fence",
                            i);
                    }
                    upper = append_fence(final_spans[i].upper);
                    next_lower = upper;
                } else {
                    upper = {
                        static_cast<uint32_t>(out.fence_pool.size()),
                        0,
                    };
                }

                out.spans.push_back(core::leaf_span{
                    .fence_lower_off = lower.first,
                    .fence_upper_off = upper.first,
                    .fence_lower_len = lower.second,
                    .fence_upper_len = upper.second,
                    .leaf_range_base = final_spans[i].range_base,
                });
            }

            return out;
        }

        static inline core::tree_reverse_topology
        build_reverse_topology_from_overrides(
            const merge_context&                       ctx,
            const core::leaf_order_index&             leaf_order,
            const core::retired_objects&              retired,
            const absl::flat_hash_map<paddr, paddr>&  leaf_parent_override,
            const absl::flat_hash_map<paddr, paddr>&  internal_parent_override,
            const std::vector<const mem_tree_node*>&  written_nodes,
            paddr                                     final_root_range_base,
            bool                                      root_is_leaf)
        {
            absl::flat_hash_set<paddr> retired_ranges;
            retired_ranges.reserve(retired.old_ranges.size());
            for (const auto& rr : retired.old_ranges) {
                retired_ranges.insert(rr.base);
            }

            absl::flat_hash_map<paddr, paddr> parent_rb_by_internal;
            parent_rb_by_internal.reserve(
                ctx.base_manifest->reverse_topology.internal_nodes.size()
                + written_nodes.size());

            for (const auto& entry : ctx.base_manifest->reverse_topology.internal_nodes) {
                if (retired_ranges.contains(entry.range_base)) continue;
                paddr parent_rb{0, 0};
                if (entry.parent_idx != core::kInvalidInternalIdx) {
                    parent_rb =
                        ctx.base_manifest->reverse_topology.internal_nodes[entry.parent_idx].range_base;
                }
                parent_rb_by_internal[entry.range_base] = parent_rb;
            }

            for (const auto& [rb, parent_rb] : internal_parent_override) {
                parent_rb_by_internal[rb] = parent_rb;
            }
            if (!root_is_leaf && final_root_range_base.lba != 0) {
                parent_rb_by_internal[final_root_range_base] = paddr{0, 0};
            }

            for (auto* node : written_nodes) {
                if (node->type != format::node_type::internal) continue;
                if (!parent_rb_by_internal.contains(node->new_range_base)) {
                    core::panic_inconsistency(
                        "build_reverse_topology_from_overrides",
                        "missing parent mapping for internal dev=%u lba=%lu",
                        static_cast<unsigned>(node->new_range_base.device_id),
                        static_cast<unsigned long>(node->new_range_base.lba));
                }
            }

            std::vector<core::internal_node_entry> internals;
            internals.reserve(parent_rb_by_internal.size());
            absl::flat_hash_map<paddr, core::internal_idx> idx_by_rb;
            idx_by_rb.reserve(parent_rb_by_internal.size());

            for (const auto& [rb, _] : parent_rb_by_internal) {
                idx_by_rb.emplace(
                    rb,
                    static_cast<core::internal_idx>(internals.size()));
                internals.push_back(core::internal_node_entry{
                    .range_base = rb,
                    .parent_idx = core::kInvalidInternalIdx,
                });
            }

            for (auto& entry : internals) {
                const auto parent_rb = parent_rb_by_internal.at(entry.range_base);
                if (parent_rb.lba == 0 && parent_rb.device_id == 0) {
                    entry.parent_idx = core::kInvalidInternalIdx;
                } else {
                    entry.parent_idx = idx_by_rb.at(parent_rb);
                }
            }

            std::vector<core::internal_idx> leaf_parent_idx;
            leaf_parent_idx.reserve(leaf_order.spans.size());
            for (const auto& span : leaf_order.spans) {
                const auto leaf_rb = span.leaf_range_base;
                paddr parent_rb{0, 0};
                if (root_is_leaf && leaf_rb == final_root_range_base) {
                    leaf_parent_idx.push_back(core::kInvalidInternalIdx);
                    continue;
                }

                if (auto it = leaf_parent_override.find(leaf_rb);
                    it != leaf_parent_override.end()) {
                    parent_rb = it->second;
                } else if (auto base_it = ctx.base_leaf_index_by_range.find(leaf_rb);
                           base_it != ctx.base_leaf_index_by_range.end()) {
                    const auto base_parent =
                        ctx.base_manifest->reverse_topology.leaf_parent_idx[base_it->second];
                    if (base_parent != core::kInvalidInternalIdx) {
                        parent_rb =
                            ctx.base_manifest->reverse_topology.internal_nodes[base_parent].range_base;
                    }
                } else {
                    core::panic_inconsistency(
                        "build_reverse_topology_from_overrides",
                        "missing parent mapping for leaf dev=%u lba=%lu",
                        static_cast<unsigned>(leaf_rb.device_id),
                        static_cast<unsigned long>(leaf_rb.lba));
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
        rebuild_slot_map_from_roots(
            const core::tree_manifest* base_manifest,
            const core::retired_objects& retired,
            const child_ref*            final_root,
            const std::vector<child_ref>& detached_subtrees,
            absl::flat_hash_map<paddr, uint32_t>& slot_map)
        {
            slot_map = base_manifest->slot_map;
            for (const auto& rr : retired.old_ranges) {
                slot_map.erase(rr.base);
            }

            auto apply_node = [&](const mem_tree_node* node) {
                if (node->replaces_old_paddrs.empty()) {
                    slot_map[node->new_range_base] = node->new_slot_index;
                    return;
                }
                const auto carrier = node->replaces_old_paddrs[0];
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

            std::vector<const mem_tree_node*> nodes;
            if (final_root != nullptr) {
                collect_nodes_for_writes(*final_root, nodes);
            }
            for (const auto& root : detached_subtrees) {
                collect_nodes_for_writes(root, nodes);
            }
            for (auto* node : nodes) {
                apply_node(node);
            }
        }


        static inline merge_coro
        run_leaf_only_merge(
            merge_round_state&                          s,
            const core::tree_manifest*                 base_manifest,
            tree_allocator&                            alloc,
            const core::tree_geometry*                 geom,
            absl::flat_hash_map<
                paddr,
                memory::pooled_frame_ptr<memory::segmented_tree_frame>>&
                non_leaf_cache,
            memory::lba_dma_page_pool&                 frame_pool,
            flush_round_state&                         round)
        {
            constexpr std::size_t kWriteBatchSize = 32;

            const uint32_t page_size = geom->tree_page_size;
            const uint32_t page_lbas = geom->page_lbas();

            merge_context ctx;
            ctx.base_manifest = base_manifest;
            ctx.geom          = geom;
            build_base_indexes(ctx);

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

            for (const auto& chain : s.worker_leaf_chains) {
                for (const auto& item : chain.items) {
                    for (const auto& rv : item.retired_old_values) {
                        round.retired.old_tree_values.push_back(rv);
                    }
                }
            }

            auto finalize_root_group = [&](
                                           auto nodes,
                                           std::vector<std::string> sibling_seps)
                                           -> child_ref {
                if (nodes.empty()) {
                    auto empty = make_empty_root_leaf(page_size);
                    auto* node = std::get<std::unique_ptr<mem_tree_node>>(empty.target).get();
                    plan_fresh_node(node, alloc, s.allocated_ranges);
                    return empty;
                }
                if (nodes.size() == 1) {
                    if (nodes.front()->type == format::node_type::internal) {
                        record_direct_child_parent_overrides(
                            ctx,
                            nodes.front().get(),
                            s.leaf_parent_override,
                            s.internal_parent_override);
                    }
                    return child_ref{ .target = std::move(nodes.front()) };
                }

                std::vector<child_ref> children;
                children.reserve(nodes.size());
                for (auto& node : nodes) {
                    children.push_back(child_ref{ .target = std::move(node) });
                }
                std::vector<std::string> separators = std::move(sibling_seps);

                while (true) {
                    std::vector<std::string> next_sibling_seps;
                    auto layer = build_internal_pages_owner(
                        std::move(children),
                        std::move(separators),
                        paddr{0, 0},
                        /*is_new_layer=*/true,
                        page_size,
                        next_sibling_seps);

                    for (auto& node : layer) {
                        plan_fresh_node(node.get(), alloc, s.allocated_ranges);
                        record_direct_child_parent_overrides(
                            ctx,
                            node.get(),
                            s.leaf_parent_override,
                            s.internal_parent_override);
                    }

                    if (layer.size() == 1) {
                        return child_ref{ .target = std::move(layer.front()) };
                    }

                    children.clear();
                    children.reserve(layer.size());
                    for (auto& node : layer) {
                        children.push_back(child_ref{ .target = std::move(node) });
                    }
                    separators = std::move(next_sibling_seps);
                }
            };

            if (!base_manifest->has_root()) {
                std::vector<std::unique_ptr<mem_tree_node>> leaf_nodes;
                for (auto& chain : s.worker_leaf_chains) {
                    for (auto& item : chain.items) {
                        for (auto& image : item.new_pages) {
                            auto node = make_leaf_node_from_image(
                                std::move(image),
                                paddr{0, 0},
                                /*carries_old_range=*/false);
                            if (leaf_node_is_empty(node.get(), page_size)) {
                                continue;
                            }
                            plan_fresh_node(node.get(), alloc, s.allocated_ranges);
                            leaf_nodes.push_back(std::move(node));
                        }
                    }
                }

                std::stable_sort(
                    leaf_nodes.begin(), leaf_nodes.end(),
                    [page_size](const auto& a, const auto& b) {
                        return first_key_of_leaf_node(a.get(), page_size)
                             < first_key_of_leaf_node(b.get(), page_size);
                    });

                std::vector<std::string> sibling_seps;
                for (std::size_t i = 1; i < leaf_nodes.size(); ++i) {
                    auto sep = first_key_of_leaf_node(leaf_nodes[i].get(), page_size);
                    if (sep.empty()) {
                        core::panic_inconsistency(
                            "run_leaf_only_merge",
                            "bootstrap live leaf has empty first key");
                    }
                    sibling_seps.emplace_back(sep);
                }

                s.combined_root = finalize_root_group(
                    std::move(leaf_nodes),
                    std::move(sibling_seps));

                std::vector<const mem_tree_node*> write_nodes;
                collect_nodes_for_writes(s.combined_root, write_nodes);
                for (auto* node : write_nodes) {
                    auto frame = alloc_tree_frame(
                        frame_pool, node->new_paddr, geom->page_lbas(),
                        /*zero_fill=*/false);
                    frame->copy_from_contiguous(
                        node->content.data(), node->content.size());
                    auto* raw_frame = frame.get();
                    s.writeback_frames.push_back(std::move(frame));
                    s.pending_ios.push_back(merge_io_desc{
                        memory::frame_write_desc{
                            .frame = raw_frame,
                            .flags = 0,
                        }});
                    if (s.pending_ios.size() >= kWriteBatchSize) {
                        co_yield merge_yield::need_io;
                    }
                }
                if (!s.pending_ios.empty()) {
                    co_yield merge_yield::need_io;
                }

                std::vector<final_leaf_item> leaf_items;
                auto new_leaf_order = build_leaf_order_full(
                    ctx, s.combined_root, leaf_items);
                auto new_topology = build_reverse_topology_full(
                    ctx, s.combined_root, leaf_items);
                absl::flat_hash_map<paddr, uint32_t> slot_map;
                rebuild_slot_map_from_roots(
                    base_manifest, round.retired, &s.combined_root,
                    s.detached_subtrees, slot_map);

                s.new_manifest = std::make_shared<const core::tree_manifest>(
                    core::tree_manifest{
                        .root_slot        = root_slot_of(base_manifest, s.combined_root),
                        .slot_map         = std::move(slot_map),
                        .geom             = base_manifest->geom,
                        .leaf_order       = std::move(new_leaf_order),
                        .root_range_base  = root_range_base_of(s.combined_root),
                        .reverse_topology = std::move(new_topology),
                    });
                s.is_root_change      = is_root_change(base_manifest, s.combined_root);
                s.new_root_base_paddr = s.new_manifest->root_range_base;
                co_yield merge_yield::done;
                co_return;
            }

            std::vector<leaf_chain_item> leaf_items;
            for (auto& chain : s.worker_leaf_chains) {
                for (auto& item : chain.items) {
                    leaf_items.push_back(std::move(item));
                }
            }
            std::sort(
                leaf_items.begin(),
                leaf_items.end(),
                [](const leaf_chain_item& a, const leaf_chain_item& b) {
                    return a.old_leaf_idx < b.old_leaf_idx;
                });
            for (std::size_t i = 1; i < leaf_items.size(); ++i) {
                if (leaf_items[i - 1].old_leaf_idx
                    == leaf_items[i].old_leaf_idx)
                {
                    core::panic_inconsistency(
                        "run_leaf_only_merge",
                        "duplicate replacement for old leaf idx=%u",
                        leaf_items[i].old_leaf_idx);
                }
            }

            std::vector<leaf_replacement> leaf_replacements;
            leaf_replacements.reserve(leaf_items.size());
            std::vector<child_interface_delta> current_level;
            std::optional<child_ref> final_root;

            for (auto& item : leaf_items) {
                std::vector<std::unique_ptr<mem_tree_node>> nodes;
                nodes.reserve(item.new_pages.size());
                for (std::size_t i = 0; i < item.new_pages.size(); ++i) {
                    nodes.push_back(make_leaf_node_from_image(
                        std::move(item.new_pages[i]),
                        item.old_range_base,
                        i == 0));
                }
                plan_group_against_old_range(
                    nodes,
                    item.old_range_base,
                    base_manifest,
                    alloc,
                    s.allocated_ranges,
                    retire_slot,
                    retire_range);

                leaf_replacement repl;
                repl.old_leaf_idx = item.old_leaf_idx;
                repl.outputs.reserve(nodes.size());
                for (auto& node : nodes) {
                    repl.outputs.push_back(leaf_replacement_output{
                        .range_base = node->new_range_base,
                        .first_key  = first_key_of_leaf_node(node.get(), page_size),
                    });
                }
                leaf_replacements.push_back(std::move(repl));

                if (item.parent_idx == core::kInvalidInternalIdx) {
                    std::vector<std::string> sibling_seps;
                    sibling_seps.reserve(nodes.size() > 0 ? nodes.size() - 1 : 0);
                    for (std::size_t i = 1; i < nodes.size(); ++i) {
                        sibling_seps.emplace_back(
                            first_key_of_leaf_node(nodes[i].get(), page_size));
                    }
                    final_root.emplace(finalize_root_group(
                        std::move(nodes),
                        std::move(sibling_seps)));
                    continue;
                }

                if (nodes.size() == 1
                    && nodes.front()->new_range_base == item.old_range_base)
                {
                    s.detached_subtrees.push_back(
                        child_ref{ .target = std::move(nodes.front()) });
                    continue;
                }

                child_interface_delta delta;
                delta.old_child_range_base = item.old_range_base;
                delta.parent_idx           = item.parent_idx;
                delta.nodes                = std::move(nodes);
                delta.sibling_seps.reserve(
                    delta.nodes.size() > 0 ? delta.nodes.size() - 1 : 0);
                for (std::size_t i = 1; i < delta.nodes.size(); ++i) {
                    delta.sibling_seps.emplace_back(
                        first_key_of_leaf_node(delta.nodes[i].get(), page_size));
                }
                current_level.push_back(std::move(delta));
            }

            const auto& topo = base_manifest->reverse_topology;
            while (!current_level.empty()) {
                absl::flat_hash_set<paddr> reads_seen;
                for (const auto& delta : current_level) {
                    const auto parent_rb =
                        topo.internal_nodes[delta.parent_idx].range_base;
                    const auto live_slot = base_manifest->resolve(parent_rb);
                    if (non_leaf_cache.contains(live_slot)
                        || s.fetched_old_frames.contains(live_slot)
                        || !reads_seen.insert(live_slot).second) {
                        continue;
                    }
                    auto frame = alloc_tree_frame(
                        frame_pool, live_slot, page_lbas,
                        /*zero_fill=*/false);
                    auto* raw_frame = frame.get();
                    s.fetched_old_frames.emplace(live_slot, std::move(frame));
                    s.pending_ios.push_back(merge_io_desc{
                        memory::frame_read_desc{
                            .frame = raw_frame,
                        }});
                }
                if (!s.pending_ios.empty()) {
                    co_yield merge_yield::need_io;
                }

                absl::flat_hash_map<
                    core::internal_idx,
                    std::vector<child_interface_delta>> grouped;
                for (auto& delta : current_level) {
                    grouped[delta.parent_idx].push_back(std::move(delta));
                }
                current_level.clear();

                std::vector<child_interface_delta> next_level;
                for (auto& [parent_idx, deltas] : grouped) {
                    const auto parent_rb =
                        topo.internal_nodes[parent_idx].range_base;
                    const auto live_slot = base_manifest->resolve(parent_rb);

                    const memory::segmented_tree_frame* old_frame = nullptr;
                    if (auto it = non_leaf_cache.find(live_slot);
                        it != non_leaf_cache.end()) {
                        old_frame = it->second.get();
                    } else if (auto fit = s.fetched_old_frames.find(live_slot);
                               fit != s.fetched_old_frames.end()) {
                        auto [cit, inserted] = non_leaf_cache.try_emplace(
                            live_slot, std::move(fit->second));
                        old_frame = cit->second.get();
                    }
                    if (old_frame == nullptr) {
                        core::panic_inconsistency(
                            "run_leaf_only_merge",
                            "missing old internal page dev=%u lba=%lu",
                            static_cast<unsigned>(live_slot.device_id),
                            static_cast<unsigned long>(live_slot.lba));
                    }

                    segmented_internal_page_reader reader;
                    if (!reader.parse(*old_frame, page_size)) {
                        core::panic_inconsistency(
                            "run_leaf_only_merge",
                            "failed to parse old internal dev=%u lba=%lu",
                            static_cast<unsigned>(live_slot.device_id),
                            static_cast<unsigned long>(live_slot.lba));
                    }

                    absl::flat_hash_map<paddr, std::size_t> delta_by_child;
                    for (std::size_t i = 0; i < deltas.size(); ++i) {
                        if (!delta_by_child.emplace(
                                deltas[i].old_child_range_base, i).second) {
                            core::panic_inconsistency(
                                "run_leaf_only_merge",
                                "multiple deltas target the same parent child "
                                "dev=%u lba=%lu",
                                static_cast<unsigned>(
                                    deltas[i].old_child_range_base.device_id),
                                static_cast<unsigned long>(
                                    deltas[i].old_child_range_base.lba));
                        }
                    }

                    std::vector<child_ref>   new_children;
                    std::vector<std::string> new_separators;
                    const uint16_t old_separator_count = reader.record_count();
                    const uint32_t old_child_count =
                        static_cast<uint32_t>(old_separator_count) + 1;
                    new_children.reserve(old_child_count + deltas.size());
                    new_separators.reserve(old_separator_count + deltas.size());
                    for (uint32_t i = 0; i < old_child_count; ++i) {
                        if (!new_children.empty()) {
                            auto sep = reader.get(static_cast<uint16_t>(i - 1));
                            new_separators.emplace_back(
                                std::move(sep.separator_key));
                        }

                        const paddr old_child =
                            (i < old_separator_count)
                                ? reader.child_base_at(static_cast<uint16_t>(i))
                                : reader.rightmost_child();
                        auto it = delta_by_child.find(old_child);
                        if (it == delta_by_child.end()) {
                            new_children.push_back(child_ref{
                                .target = old_child,
                            });
                            continue;
                        }

                        auto& delta = deltas[it->second];
                        for (std::size_t n = 0; n < delta.nodes.size(); ++n) {
                            if (!new_children.empty() && n > 0) {
                                new_separators.push_back(
                                    std::move(delta.sibling_seps[n - 1]));
                            }
                            new_children.push_back(child_ref{
                                .target = std::move(delta.nodes[n]),
                            });
                        }
                    }

                    std::vector<std::string> sibling_seps;
                    auto built = build_internal_pages_owner(
                        std::move(new_children),
                        std::move(new_separators),
                        parent_rb,
                        /*is_new_layer=*/false,
                        page_size,
                        sibling_seps);
                    plan_group_against_old_range(
                        built,
                        parent_rb,
                        base_manifest,
                        alloc,
                        s.allocated_ranges,
                        retire_slot,
                        retire_range);

                    for (auto& node : built) {
                        record_direct_child_parent_overrides(
                            ctx,
                            node.get(),
                            s.leaf_parent_override,
                            s.internal_parent_override);
                    }

                    const auto parent_parent_idx =
                        topo.internal_nodes[parent_idx].parent_idx;
                    if (parent_parent_idx == core::kInvalidInternalIdx) {
                        final_root.emplace(finalize_root_group(
                            std::move(built),
                            std::move(sibling_seps)));
                        continue;
                    }

                    if (built.size() == 1
                        && built.front()->new_range_base == parent_rb)
                    {
                        s.detached_subtrees.push_back(child_ref{
                            .target = std::move(built.front()),
                        });
                        continue;
                    }

                    next_level.push_back(child_interface_delta{
                        .old_child_range_base = parent_rb,
                        .parent_idx           = parent_parent_idx,
                        .nodes                = move_node_vector(std::move(built)),
                        .sibling_seps         = std::move(sibling_seps),
                    });
                }

                current_level = std::move(next_level);
            }

            if (!final_root.has_value()) {
                s.combined_root = child_ref{
                    .target = base_manifest->root_range_base,
                };
            } else {
                s.combined_root = std::move(*final_root);
            }

            // Canonical leaf-only merge currently does not implement
            // zero-child delete pruning or internal collapse. The
            // manifest builders below derive from leaf replacements
            // plus same-range detached subtrees, so mutating only
            // `combined_root` here would diverge the emitted tree
            // shape from rebuilt `leaf_order` / `reverse_topology`.

            std::vector<const mem_tree_node*> write_nodes;
            collect_nodes_for_writes(s.combined_root, write_nodes);
            for (const auto& subtree : s.detached_subtrees) {
                collect_nodes_for_writes(subtree, write_nodes);
            }
            for (auto* node : write_nodes) {
                auto frame = alloc_tree_frame(
                    frame_pool, node->new_paddr, geom->page_lbas(),
                    /*zero_fill=*/false);
                frame->copy_from_contiguous(
                    node->content.data(), node->content.size());
                auto* raw_frame = frame.get();
                s.writeback_frames.push_back(std::move(frame));
                s.pending_ios.push_back(merge_io_desc{
                    memory::frame_write_desc{
                        .frame = raw_frame,
                        .flags = 0,
                    }});
                if (s.pending_ios.size() >= kWriteBatchSize) {
                    co_yield merge_yield::need_io;
                }
            }
            if (!s.pending_ios.empty()) {
                co_yield merge_yield::need_io;
            }

            const bool root_is_leaf =
                std::holds_alternative<std::unique_ptr<mem_tree_node>>(
                    s.combined_root.target)
                && std::get<std::unique_ptr<mem_tree_node>>(
                       s.combined_root.target)->type == format::node_type::leaf;

            auto new_leaf_order = build_leaf_order_from_replacements(
                base_manifest, leaf_replacements);
            auto new_topology = build_reverse_topology_from_overrides(
                ctx,
                new_leaf_order,
                round.retired,
                s.leaf_parent_override,
                s.internal_parent_override,
                write_nodes,
                root_range_base_of(s.combined_root),
                root_is_leaf);

            absl::flat_hash_map<paddr, uint32_t> slot_map;
            rebuild_slot_map_from_roots(
                base_manifest, round.retired, &s.combined_root,
                s.detached_subtrees, slot_map);

            s.new_manifest = std::make_shared<const core::tree_manifest>(
                core::tree_manifest{
                    .root_slot        = root_slot_of(base_manifest, s.combined_root),
                    .slot_map         = std::move(slot_map),
                    .geom             = base_manifest->geom,
                    .leaf_order       = std::move(new_leaf_order),
                    .root_range_base  = root_range_base_of(s.combined_root),
                    .reverse_topology = std::move(new_topology),
                });
            s.is_root_change      = is_root_change(base_manifest, s.combined_root);
            s.new_root_base_paddr = s.new_manifest->root_range_base;
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
        static constexpr uint32_t kMaxRecoveryFrontierOpsPerAdvance       = 8;
        static constexpr uint32_t kMaxReclaimIngressPerAdvance            = 64;
        static constexpr uint32_t kMaxReclaimInvalidateCompletePerAdvance = 64;
        static constexpr uint32_t kMaxReclaimTasksPerAdvance              = 16;
        static constexpr uint32_t kMaxReclaimTrimCompletePerAdvance       = 64;
        static constexpr uint32_t kMaxValueRefsPerReclaimBatch            = 256;
        static constexpr uint32_t kMaxMutationGateOpsPerAdvance           = 16;
        static constexpr uint32_t kWriteBatchConcurrency                  = 32;

        const core::tree_geometry* geom = nullptr;
        core::wal_reclaim_frontier* wal_frontier = nullptr;
        std::vector<core::tree_read_domain_base*>* read_domains = nullptr;
        // frame_pool 必须声明在 state 之前 → 逆序析构时 state 先析构、frame_pool 后析构，
        // 使 state 内所有 RAII frame-holder（non_leaf_page_cache 的 pooled_frame_ptr、
        // active_merge 的 fetched/writeback frames）析构时把 frame 还给仍存活的 frame_pool。
        // 反序会触发 teardown double-free（058 e2e 顺出）。见 code_quality_standard §2.2 池/缓存析构序。
        memory::lba_dma_page_pool  frame_pool;
        tree_state                 state;
        core::reclaim_sink         sink_handle;
        pump::core::per_core::queue<_flush_fold::req*>               fold_q;
        pump::core::per_core::queue<_merge_step::req*>               merge_step_q;
        pump::core::per_core::queue<_merge_reads_done::req*>         merge_reads_done_q;
        pump::core::per_core::queue<_finalize_merge::req*>           finalize_merge_q;
        pump::core::per_core::queue<_begin_update_superblock::req*>  begin_update_superblock_q;
        pump::core::per_core::queue<_finish_update_superblock::req*> finish_update_superblock_q;
        pump::core::per_core::queue<_finalize_flush_round::req*>     finalize_q;
        pump::core::per_core::queue<_recompute_recovery_frontier::req*> recovery_frontier_q;
        pump::core::per_core::queue<_mutation_gate_acquire::req*>     mutation_gate_acquire_q;
        pump::core::per_core::queue<_mutation_gate_release::req*>     mutation_gate_release_q;
        // Serializes the superblock begin→finish pair: begin latches
        // the flag, finish clears it. The outer pipeline is expected
        // to issue read/mutate/FUA-write between the two seams.
        bool update_superblock_inflight = false;

        explicit
        tree_sched(const core::tree_geometry* g = nullptr,
                   format::paddr              data_area_base = {0, 0},
                   core::data_area_heads*     shared_heads = nullptr,
                   core::wal_reclaim_frontier* wal_frontier_cell = nullptr,
                   std::vector<core::tree_read_domain_base*>* read_domain_list = nullptr,
                   std::size_t                depth = 256,
                   memory::dma_page_allocator frame_allocator =
                       memory::make_heap_dma_page_allocator(),
                   uint32_t                   frame_alignment = 4096,
                   int                        frame_numa_id = -1)
            : geom(g)
            , wal_frontier(wal_frontier_cell)
            , read_domains(read_domain_list)
            , frame_pool(g ? g->lba_size : 4096,
                         frame_alignment,
                         frame_numa_id,
                         frame_allocator)
            , sink_handle{
                  .self = this,
                  .post_retired = &tree_sched::post_retired_thunk,
                  .post_gen_losers = &tree_sched::post_gen_losers_thunk,
              }
            , fold_q(depth)
            , merge_step_q(depth)
            , merge_reads_done_q(depth)
            , finalize_merge_q(depth)
            , begin_update_superblock_q(depth)
            , finish_update_superblock_q(depth)
            , finalize_q(depth)
            , recovery_frontier_q(depth)
            , mutation_gate_acquire_q(depth)
            , mutation_gate_release_q(depth)
        {
            if (wal_frontier == nullptr) {
                throw std::invalid_argument(
                    "tree::tree_sched: wal reclaim frontier must not be null");
            }
            if (read_domains == nullptr) {
                throw std::invalid_argument(
                    "tree::tree_sched: read domain list must not be null");
            }
            state.alloc.head         = data_area_base;
            state.alloc.shared_heads = shared_heads;
            if (geom != nullptr) {
                state.alloc.range_lbas =
                    static_cast<uint32_t>(geom->range_lbas());
                state.alloc.shadow_slots = geom->shadow_slots_per_range;
            }
        }

        ~tree_sched() {
            while (auto item = state.reclaim_q.try_dequeue()) {
                delete *item;
            }
            for (auto* task : state.pending_reclaim) {
                delete task;
            }
            while (auto item = state.reclaim_invalidate_done_q.try_dequeue()) {
                delete *item;
            }
            while (auto item = state.reclaim_trim_done_q.try_dequeue()) {
                delete *item;
            }
            while (auto item = mutation_gate_acquire_q.try_dequeue()) {
                delete *item;
            }
            while (auto item = mutation_gate_release_q.try_dequeue()) {
                delete *item;
            }
            for (auto* waiter : state.mutation_gate.waiters) {
                delete waiter;
            }
        }

        static void
        post_retired_thunk(void* self, core::retired_objects&& retired) {
            static_cast<tree_sched*>(self)->post_retired_impl(
                std::move(retired));
        }

        static void
        post_gen_losers_thunk(void* self, core::retired_value_refs&& losers) {
            static_cast<tree_sched*>(self)->post_gen_losers_impl(
                std::move(losers));
        }

        void
        post_retired_impl(core::retired_objects&& retired) {
            auto* task = new core::reclaim_task{
                .k = core::reclaim_task::kind::retired,
                .retired = std::move(retired),
                .gen_losers = {},
            };
            if (!state.reclaim_q.try_enqueue(std::move(task))) {
                delete task;
                core::panic_inconsistency(
                    "tree::tree_sched::post_retired",
                    "reclaim queue full");
            }
        }

        void
        post_gen_losers_impl(core::retired_value_refs&& losers) {
            auto* task = new core::reclaim_task{
                .k = core::reclaim_task::kind::gen_losers,
                .retired = {},
                .gen_losers = std::move(losers),
            };
            if (!state.reclaim_q.try_enqueue(std::move(task))) {
                delete task;
                core::panic_inconsistency(
                    "tree::tree_sched::post_gen_losers",
                    "reclaim queue full");
            }
        }

        void
        complete_reclaim_trim(bool ok,
                              uint64_t round_id,
                              bool recycle_range,
                              format::range_ref range) {
            if (!ok) {
                core::panic_inconsistency(
                    "tree::tree_sched::complete_reclaim_trim",
                    "tree reclaim TRIM failed");
            }
            auto* completion = new reclaim_trim_completion{
                .round_id = round_id,
                .recycle_range = recycle_range,
                .range = range,
            };
            if (!state.reclaim_trim_done_q.try_enqueue(std::move(completion))) {
                delete completion;
                core::panic_inconsistency(
                    "tree::tree_sched::complete_reclaim_trim",
                    "trim completion queue full");
            }
        }

        void
        submit_reclaim_trim(uint64_t round_id,
                            format::range_ref range,
                            bool recycle_range) {
            if (geom == nullptr) {
                core::panic_inconsistency(
                    "tree::tree_sched::submit_reclaim_trim",
                    "tree geometry is null");
            }
            const uint64_t lbas =
                static_cast<uint64_t>(range.slot_count) * geom->page_lbas();
            if (lbas == 0 ||
                lbas > std::numeric_limits<uint32_t>::max()) {
                core::panic_inconsistency(
                    "tree::tree_sched::submit_reclaim_trim",
                    "invalid trim span lbas=%lu",
                    static_cast<unsigned long>(lbas));
            }
            auto trim_sender = core::registry::local_nvme()->trim(
                range.base.lba, static_cast<uint32_t>(lbas));
            pump::sender::submit(
                std::move(trim_sender),
                pump::core::make_root_context(),
                _reclaim_trim_complete::receiver{
                    .sched = this,
                    .round_id = round_id,
                    .recycle_range = recycle_range,
                    .range = range,
                });
        }

        void
        enqueue_reclaim_invalidate_done(uint64_t round_id,
                                        bool recycle_range,
                                        format::range_ref range) {
            auto* completion = new reclaim_invalidate_completion{
                .round_id = round_id,
                .recycle_range = recycle_range,
                .range = range,
            };
            if (!state.reclaim_invalidate_done_q.try_enqueue(completion)) {
                delete completion;
                core::panic_inconsistency(
                    "tree::tree_sched::enqueue_reclaim_invalidate_done",
                    "invalidate completion queue full");
            }
        }

        void
        submit_reclaim_invalidate(uint64_t round_id,
                                  format::range_ref range,
                                  bool recycle_range) {
            if (geom == nullptr) {
                core::panic_inconsistency(
                    "tree::tree_sched::submit_reclaim_invalidate",
                    "tree geometry is null");
            }
            if (read_domains == nullptr) {
                core::panic_inconsistency(
                    "tree::tree_sched::submit_reclaim_invalidate",
                    "read domain list is null");
            }
            const uint32_t page_lbas = geom->page_lbas();
            auto* domains = read_domains;
            auto fanout = pump::sender::just()
                >> pump::sender::loop(domains->size())
                >> pump::sender::concurrent()
                >> pump::sender::flat_map(
                    [domains, range, page_lbas](std::size_t i) {
                        return (*domains)[i]->submit_invalidate_range(
                            range, page_lbas);
                    })
                >> pump::sender::all()
                >> pump::sender::then(
                    [this, round_id, recycle_range, range](bool ok) {
                        if (!ok) {
                            core::panic_inconsistency(
                                "tree::tree_sched::submit_reclaim_invalidate",
                                "read_domain invalidate fan-out failed");
                        }
                        enqueue_reclaim_invalidate_done(
                            round_id, recycle_range, range);
                    });
            pump::sender::submit(
                std::move(fanout),
                pump::core::make_root_context(),
                pump::sender::the_null_receiver);
        }

        void
        invalidate_reclaim_range_locally(format::range_ref range) {
            for (uint32_t slot = 0; slot < range.slot_count; ++slot) {
                auto base = format::paddr{
                    .device_id = range.base.device_id,
                    .lba = range.base.lba +
                           static_cast<uint64_t>(slot) * geom->page_lbas(),
                };
                state.non_leaf_page_cache.erase(base);
            }
        }

        void
        enqueue_deferred_value(core::retired_value_ref&& ref) {
            auto it = std::upper_bound(
                state.deferred_value_reclaim.begin(),
                state.deferred_value_reclaim.end(),
                ref.data_ver,
                [](uint64_t data_ver, const core::retired_value_ref& cur) {
                    return data_ver < cur.data_ver;
                });
            state.deferred_value_reclaim.insert(it, std::move(ref));
        }

        void
        gate_value_ref(core::retired_value_ref&& ref,
                       std::vector<format::value_ref>& reclaim_now) {
            if (ref.data_ver <= state.recovery_safe_lsn &&
                reclaim_now.size() < kMaxValueRefsPerReclaimBatch) {
                reclaim_now.push_back(ref.vr);
                return;
            }
            enqueue_deferred_value(std::move(ref));
        }

        void
        scan_deferred_values(std::vector<format::value_ref>& reclaim_now) {
            while (!state.deferred_value_reclaim.empty() &&
                   reclaim_now.size() < kMaxValueRefsPerReclaimBatch &&
                   state.deferred_value_reclaim.front().data_ver <=
                       state.recovery_safe_lsn) {
                reclaim_now.push_back(
                    state.deferred_value_reclaim.front().vr);
                state.deferred_value_reclaim.erase(
                    state.deferred_value_reclaim.begin());
            }
        }

        void
        process_reclaim_task(core::reclaim_task& task,
                             active_reclaim_round& round) {
            switch (task.k) {
            case core::reclaim_task::kind::retired:
                for (auto slot : task.retired.old_slots) {
                    auto one_slot = format::range_ref{
                        .base = slot,
                        .slot_count = 1,
                    };
                    ++round.pending_invalidations;
                    submit_reclaim_invalidate(
                        round.round_id, one_slot, /*recycle_range=*/false);
                }
                for (auto range : task.retired.old_ranges) {
                    ++round.pending_invalidations;
                    submit_reclaim_invalidate(
                        round.round_id, range, /*recycle_range=*/true);
                }
                for (auto& ref : task.retired.old_tree_values) {
                    gate_value_ref(std::move(ref), round.reclaim_now);
                }
                break;
            case core::reclaim_task::kind::gen_losers:
                for (auto& ref : task.gen_losers) {
                    gate_value_ref(std::move(ref), round.reclaim_now);
                }
                break;
            }
        }

        bool
        drain_reclaim_ingress() {
            bool progress = false;
            for (uint32_t i = 0; i < kMaxReclaimIngressPerAdvance; ++i) {
                auto item = state.reclaim_q.try_dequeue();
                if (!item) {
                    break;
                }
                state.pending_reclaim.push_back(*item);
                progress = true;
            }
            return progress;
        }

        bool
        drain_reclaim_invalidate_completions() {
            bool progress = false;
            for (uint32_t i = 0;
                 i < kMaxReclaimInvalidateCompletePerAdvance;
                 ++i) {
                auto item = state.reclaim_invalidate_done_q.try_dequeue();
                if (!item) {
                    break;
                }
                std::unique_ptr<reclaim_invalidate_completion> done(*item);
                if (!state.active_reclaim.has_value() ||
                    state.active_reclaim->round_id != done->round_id) {
                    core::panic_inconsistency(
                        "tree::tree_sched::drain_reclaim_invalidate_completions",
                        "invalidate completion for inactive reclaim round");
                }
                auto& round = *state.active_reclaim;
                if (round.pending_invalidations == 0) {
                    core::panic_inconsistency(
                        "tree::tree_sched::drain_reclaim_invalidate_completions",
                        "pending invalidate underflow");
                }
                invalidate_reclaim_range_locally(done->range);
                --round.pending_invalidations;
                ++round.pending_trims;
                submit_reclaim_trim(
                    round.round_id, done->range, done->recycle_range);
                progress = true;
            }
            if (progress) {
                try_finish_reclaim_round();
            }
            return progress;
        }

        bool
        drain_reclaim_trim_completions() {
            bool progress = false;
            for (uint32_t i = 0; i < kMaxReclaimTrimCompletePerAdvance; ++i) {
                auto item = state.reclaim_trim_done_q.try_dequeue();
                if (!item) {
                    break;
                }
                std::unique_ptr<reclaim_trim_completion> done(*item);
                if (!state.active_reclaim.has_value() ||
                    state.active_reclaim->round_id != done->round_id) {
                    core::panic_inconsistency(
                        "tree::tree_sched::drain_reclaim_trim_completions",
                        "TRIM completion for inactive reclaim round");
                }
                auto& round = *state.active_reclaim;
                if (round.pending_trims == 0) {
                    core::panic_inconsistency(
                        "tree::tree_sched::drain_reclaim_trim_completions",
                        "pending TRIM underflow");
                }
                if (done->recycle_range) {
                    state.alloc.recycle(done->range);
                }
                --round.pending_trims;
                progress = true;
            }
            if (progress) {
                try_finish_reclaim_round();
            }
            return progress;
        }

        bool
        try_finish_reclaim_round() {
            if (!state.active_reclaim.has_value()) {
                return false;
            }
            auto& round = *state.active_reclaim;
            if (round.pending_invalidations != 0 || round.pending_trims != 0) {
                return false;
            }

            state.recovery_safe_lsn = recompute_recovery_safe_lsn();
            scan_deferred_values(round.reclaim_now);
            if (!round.reclaim_now.empty()) {
                core::registry::post_value_reclaim_values(
                    std::move(round.reclaim_now));
            }
            if (round.processed_tasks != 0) {
                core::registry::post_wal_reclaim_check(
                    flush_durable_frontier());
            }

            const tree_mutation_token token = round.token;
            state.active_reclaim.reset();
            schedule_mutation_gate_release(
                new _mutation_gate_release::req{ token, []() {} });
            return true;
        }

        void
        begin_reclaim_round(tree_mutation_token token) {
            if (state.active_reclaim.has_value()) {
                core::panic_inconsistency(
                    "tree::tree_sched::begin_reclaim_round",
                    "reclaim round already active");
            }
            auto& round = state.active_reclaim.emplace();
            round.token = token;
            round.round_id = state.next_reclaim_round_id++;
            round.reclaim_now.reserve(kMaxValueRefsPerReclaimBatch);

            uint32_t processed = 0;
            while (!state.pending_reclaim.empty() &&
                   processed < kMaxReclaimTasksPerAdvance) {
                std::unique_ptr<core::reclaim_task> task(
                    state.pending_reclaim.front());
                state.pending_reclaim.pop_front();
                process_reclaim_task(*task, round);
                ++processed;
            }
            round.processed_tasks = processed;
            try_finish_reclaim_round();
        }

        bool
        process_pending_reclaim() {
            if (state.pending_reclaim.empty() ||
                state.active_reclaim.has_value() ||
                state.reclaim_gate_requested) {
                return false;
            }
            state.reclaim_gate_requested = true;
            schedule_mutation_gate_acquire(
                new _mutation_gate_acquire::req{
                    [this](tree_mutation_token token) {
                        state.reclaim_gate_requested = false;
                        begin_reclaim_round(token);
                    } });
            return true;
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

        void
        schedule_recovery_frontier(_recompute_recovery_frontier::req* r) {
            if (!recovery_frontier_q.try_enqueue(r)) {
                delete r;
                throw std::runtime_error(
                    "tree::tree_sched: recovery frontier queue full");
            }
        }

        void
        schedule_mutation_gate_acquire(_mutation_gate_acquire::req* r) {
            if (!mutation_gate_acquire_q.try_enqueue(r)) {
                delete r;
                core::panic_inconsistency(
                    "tree::tree_sched::schedule_mutation_gate_acquire",
                    "mutation gate acquire queue full");
            }
        }

        void
        schedule_mutation_gate_release(_mutation_gate_release::req* r) {
            if (!mutation_gate_release_q.try_enqueue(r)) {
                delete r;
                core::panic_inconsistency(
                    "tree::tree_sched::schedule_mutation_gate_release",
                    "mutation gate release queue full");
            }
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

        auto
        submit_recompute_recovery_frontier() {
            return _recompute_recovery_frontier::sender{ this };
        }

        _mutation_gate_acquire::sender
        submit_acquire_tree_mutation() {
            return _mutation_gate_acquire::sender{ this };
        }

        _mutation_gate_release::sender
        submit_release_tree_mutation(tree_mutation_token token) {
            return _mutation_gate_release::sender{ this, token };
        }

        uint64_t
        flush_durable_frontier() const {
            return std::min(state.flush_max_lsn, state.superblock_safe_lsn);
        }

        uint64_t
        recompute_recovery_safe_lsn() const {
            const uint64_t fd = flush_durable_frontier();
            const uint64_t global_min_unreclaimed =
                wal_frontier->global_min_unreclaimed_lsn.load(
                    std::memory_order_acquire);
            if (global_min_unreclaimed == 0) {
                core::panic_inconsistency(
                    "tree::tree_sched::recompute_recovery_safe_lsn",
                    "global_min_unreclaimed_lsn must be positive");
            }
            const uint64_t wal_frontier_lsn =
                global_min_unreclaimed ==
                        core::wal_reclaim_frontier::no_unreclaimed_lsn
                    ? fd
                    : global_min_unreclaimed - 1;
            return std::min(fd, wal_frontier_lsn);
        }

        memory::pooled_frame_ptr<memory::segmented_tree_frame>
        alloc_frame(format::paddr base,
                    uint32_t span_lbas,
                    memory::frame_id::domain dom,
                    memory::frame_state st = memory::frame_state::clean_readonly,
                    bool zero_fill = false) {
            auto frame = frame_pool.get_typed_frame<memory::segmented_tree_frame>(
                memory::frame_id{
                    base,
                    static_cast<uint16_t>(span_lbas),
                    dom,
                },
                st,
                zero_fill);
            if (!frame) {
                core::panic_inconsistency(
                    "tree::tree_sched::alloc_frame",
                    "DMA page allocation failed dev=%u lba=%lu span=%u dom=%u",
                    static_cast<unsigned>(base.device_id),
                    static_cast<unsigned long>(base.lba),
                    static_cast<unsigned>(span_lbas),
                    static_cast<unsigned>(dom));
            }
            return memory::pooled_frame_ptr<memory::segmented_tree_frame>(
                &frame_pool,
                new memory::segmented_tree_frame(std::move(*frame)));
        }

        tree_mutation_token
        issue_mutation_token() {
            auto& gate = state.mutation_gate;
            auto token = tree_mutation_token{ .ticket = gate.next_ticket++ };
            gate.held = true;
            gate.active_ticket = token.ticket;
            return token;
        }

        void
        wake_mutation_gate_waiter(_mutation_gate_acquire::req* r) {
            auto token = issue_mutation_token();
            r->cb(token);
            delete r;
        }

        void
        handle_mutation_gate_acquire_req(_mutation_gate_acquire::req* r) {
            auto& gate = state.mutation_gate;
            if (!gate.held && gate.waiters.empty()) {
                wake_mutation_gate_waiter(r);
                return;
            }
            gate.waiters.push_back(r);
        }

        void
        handle_mutation_gate_release_req(_mutation_gate_release::req* r) {
            auto& gate = state.mutation_gate;
            if (!gate.held || gate.active_ticket != r->token.ticket ||
                r->token.ticket == 0) {
                core::panic_inconsistency(
                    "tree::tree_sched::handle_mutation_gate_release_req",
                    "invalid mutation gate token");
            }

            _mutation_gate_acquire::req* next = nullptr;
            if (!gate.waiters.empty()) {
                next = gate.waiters.front();
                gate.waiters.pop_front();
            } else {
                gate.held = false;
                gate.active_ticket = 0;
            }

            r->cb();
            delete r;
            if (next != nullptr) {
                wake_mutation_gate_waiter(next);
            }
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
                        if (std::holds_alternative<memory::frame_read_desc>(d)) {
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
            std::vector<worker_leaf_chain> leaf_chains =
                std::move(ls->worker_leaf_chains);

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
            for (const auto& chain : leaf_chains) {
                if (chain.round_id.v != round_id_v) {
                    core::panic_inconsistency(
                        "tree::tree_sched::handle_merge_step_first_call",
                        "worker_leaf_chain round_id mismatch");
                }
            }
            auto& round = *round_it->second;

            merge_round_state ms;
            ms.round_id          = ls->round_id;
            ms.worker_leaf_chains = std::move(leaf_chains);
            ms.st                = round.st;
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
            active.coro.emplace(_owner::run_leaf_only_merge(
                active,
                round.pinned_base_guard->manifest.get(),
                state.alloc,
                round.pinned_base_guard->manifest->geom,
                state.non_leaf_page_cache,
                frame_pool,
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

            if (geom == nullptr) {
                core::panic_inconsistency(
                    "tree::tree_sched::emit_finalize_merge_success",
                    "tree geometry is null");
            }
            for (const auto& old_slot : round.retired.old_slots) {
                state.non_leaf_page_cache.erase(old_slot);
            }
            for (const auto& old_range : round.retired.old_ranges) {
                for (uint32_t slot = 0; slot < geom->shadow_slots_per_range; ++slot) {
                    state.non_leaf_page_cache.erase(
                        geom->slot_paddr(old_range.base, slot));
                }
            }
            std::vector<const mem_tree_node*> cached_nodes;
            _owner::collect_nodes_for_writes(active.combined_root, cached_nodes);
            for (const auto& subtree : active.detached_subtrees) {
                _owner::collect_nodes_for_writes(subtree, cached_nodes);
            }
            for (auto* node : cached_nodes) {
                if (node->type != format::node_type::internal) continue;
                bool installed = false;
                for (auto& frame : active.writeback_frames) {
                    if (!frame || frame->id.base != node->new_paddr) {
                        continue;
                    }
                    state.non_leaf_page_cache.erase(node->new_paddr);
                    state.non_leaf_page_cache.emplace(
                        node->new_paddr, std::move(frame));
                    installed = true;
                    break;
                }
                if (!installed) {
                    core::panic_inconsistency(
                        "tree::tree_sched::emit_finalize_merge_success",
                        "missing writeback frame for internal node dev=%u lba=%lu",
                        static_cast<unsigned>(node->new_paddr.device_id),
                        static_cast<unsigned long>(node->new_paddr.lba));
                }
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

        void
        handle_recompute_recovery_frontier_req(
            _recompute_recovery_frontier::req* r) {
            state.recovery_safe_lsn = recompute_recovery_safe_lsn();
            r->cb(recovery_frontier_snapshot{
                .flush_durable_frontier = flush_durable_frontier(),
                .recovery_safe_lsn = state.recovery_safe_lsn,
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
                mutation_gate_release_q, kMaxMutationGateOpsPerAdvance,
                [this](_mutation_gate_release::req* r) {
                    handle_mutation_gate_release_req(r);
                });

            progress |= drain_queue(
                mutation_gate_acquire_q, kMaxMutationGateOpsPerAdvance,
                [this](_mutation_gate_acquire::req* r) {
                    handle_mutation_gate_acquire_req(r);
                });

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

            progress |= drain_queue(
                recovery_frontier_q, kMaxRecoveryFrontierOpsPerAdvance,
                [this](_recompute_recovery_frontier::req* r) {
                    handle_recompute_recovery_frontier_req(r);
                });

            progress |= drain_reclaim_ingress();
            progress |= drain_reclaim_invalidate_completions();
            progress |= drain_reclaim_trim_completions();
            progress |= process_pending_reclaim();

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

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _recompute_recovery_frontier::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_recovery_frontier(
            new _recompute_recovery_frontier::req{
                [ctx = ctx, scope = scope](
                    recovery_frontier_snapshot&& snapshot) mutable {
                    pump::core::op_pusher<pos + 1, scope_t>::push_value(
                        ctx, scope, std::move(snapshot));
                },
            });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _mutation_gate_acquire::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_mutation_gate_acquire(
            new _mutation_gate_acquire::req{
                [ctx = ctx, scope = scope](tree_mutation_token token) mutable {
                    pump::core::op_pusher<pos + 1, scope_t>::push_value(
                        ctx, scope, token);
                },
            });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _mutation_gate_release::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_mutation_gate_release(
            new _mutation_gate_release::req{
                token,
                [ctx = ctx, scope = scope]() mutable {
                    pump::core::op_pusher<pos + 1, scope_t>::push_value(
                        ctx, scope);
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

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::recompute_recovery_frontier_op)
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
    compute_sender_type<
        ctx_t,
        apps::inconel::tree::_recompute_recovery_frontier::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<
                apps::inconel::tree::recovery_frontier_snapshot>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::mutation_gate_acquire_op)
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
    compute_sender_type<
        ctx_t,
        apps::inconel::tree::_mutation_gate_acquire::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<
                apps::inconel::tree::tree_mutation_token>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::mutation_gate_release_op)
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
    compute_sender_type<
        ctx_t,
        apps::inconel::tree::_mutation_gate_release::sender> {
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
        && (get_current_op_type_t<pos, scope_t>::reclaim_trim_complete_receiver_op)
    struct
    op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template <typename context_t>
        static void
        push_value(context_t& context, scope_t& scope, bool ok) {
            static_assert(context_t::element_type::root_flag,
                          "tree reclaim trim receiver requires root context");
            auto& op = std::get<pos>(scope->get_op_tuple());
            op.sched->complete_reclaim_trim(
                ok, op.round_id, op.recycle_range, op.range);
            delete scope.get();
        }

        template <typename context_t>
        static void
        push_exception(context_t&, scope_t& scope, std::exception_ptr) {
            auto& op = std::get<pos>(scope->get_op_tuple());
            (void)op;
            apps::inconel::core::panic_inconsistency(
                "tree::reclaim_trim_complete",
                "tree reclaim TRIM sender failed");
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_TREE_OWNER_SCHEDULER_HH
