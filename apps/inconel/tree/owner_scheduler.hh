#ifndef APPS_INCONEL_TREE_OWNER_SCHEDULER_HH
#define APPS_INCONEL_TREE_OWNER_SCHEDULER_HH

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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

    struct pending_write_state {
        flush_round_id                                     round_id;
        child_ref                                          combined_root;
        std::shared_ptr<const core::tree_manifest>         new_manifest;
        bool                                               is_root_change = false;
        std::vector<format::write_desc>                    writes;
        std::vector<format::range_ref>                     allocated_ranges;
        std::move_only_function<void(flush_merge_result&&)> cb;
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
        absl::flat_hash_map<uint64_t, std::unique_ptr<pending_write_state>>
            pending_writes;
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

    namespace _flush_merge {

        struct req {
            flush_merge_request                               args;
            std::move_only_function<void(flush_merge_result&&)> cb;
        };

        struct op {
            constexpr static bool flush_merge_op = true;

            tree_sched*         sched;
            flush_merge_request args;

            template <uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            tree_sched*         sched;
            flush_merge_request args;

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

    }  // namespace _flush_merge

    namespace _update_superblock {

        struct req {
            update_superblock_request                                args;
            std::move_only_function<void(update_superblock_result&&)> cb;
        };

        struct op {
            constexpr static bool update_superblock_op = true;

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

    }  // namespace _update_superblock

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

        static inline const std::vector<char>&
        touched_old_page_for(const merge_context& ctx,
                             paddr                rb,
                             const std::vector<const proposal_group_view*>& contribs)
        {
            for (auto* g : contribs) {
                auto it = (*ctx.proposals)[g->worker_index].touched_old_pages.find(rb);
                if (it != (*ctx.proposals)[g->worker_index].touched_old_pages.end()) {
                    return it->second;
                }
            }
            core::panic_inconsistency(
                "touched_old_page_for",
                "missing touched_old_pages entry for internal "
                "(dev=%u lba=%lu)",
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

    }  // namespace _owner

    struct tree_sched {
        static constexpr uint32_t kMaxFoldOpsPerAdvance             = 8;
        static constexpr uint32_t kMaxMergeOpsPerAdvance            = 8;
        static constexpr uint32_t kMaxUpdateSuperblockOpsPerAdvance = 1;
        static constexpr uint32_t kMaxFinalizeOpsPerAdvance         = 8;
        static constexpr uint32_t kWriteBatchConcurrency            = 32;

        const core::tree_geometry* geom = nullptr;
        tree_state                 state;
        pump::core::per_core::queue<_flush_fold::req*>  fold_q;
        pump::core::per_core::queue<_flush_merge::req*> merge_q;
        pump::core::per_core::queue<_update_superblock::req*> update_superblock_q;
        pump::core::per_core::queue<_finalize_flush_round::req*> finalize_q;
        bool update_superblock_inflight = false;

        explicit
        tree_sched(const core::tree_geometry* g = nullptr,
                   format::paddr              data_area_base = {0, 0},
                   core::data_area_heads*     shared_heads = nullptr,
                   std::size_t                depth = 256)
            : geom(g)
            , fold_q(depth)
            , merge_q(depth)
            , update_superblock_q(depth)
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
        schedule_merge(_flush_merge::req* r) {
            merge_q.try_enqueue(r);
        }

        void
        schedule_update_superblock(_update_superblock::req* r) {
            update_superblock_q.try_enqueue(r);
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
        submit_flush_merge(flush_merge_request args) {
            return _flush_merge::sender{ this, std::move(args) };
        }

        auto
        submit_update_superblock(update_superblock_request args) {
            return _update_superblock::sender{ this, std::move(args) };
        }

        auto
        submit_finalize_flush_round(finalize_flush_request args) {
            return _finalize_flush_round::sender{ this, std::move(args) };
        }

        uint64_t
        recompute_recovery_safe_lsn() const {
            return std::min(state.flush_max_lsn, state.superblock_safe_lsn);
        }

        tree_flush_result
        fail_pending_round(uint64_t round_id, bool rollback_allocated_ranges) {
            auto pending_it = state.pending_writes.find(round_id);
            if (pending_it == state.pending_writes.end()) {
                core::panic_inconsistency(
                    "tree_sched::fail_pending_round",
                    "pending round_id %lu not found",
                    static_cast<unsigned long>(round_id));
            }
            auto round_it = state.active_rounds.find(round_id);
            if (round_it == state.active_rounds.end()) {
                core::panic_inconsistency(
                    "tree_sched::fail_pending_round",
                    "active round_id %lu not found",
                    static_cast<unsigned long>(round_id));
            }

            auto pending = std::move(pending_it->second);
            auto round   = std::move(round_it->second);
            state.pending_writes.erase(pending_it);
            state.active_rounds.erase(round_it);

            if (rollback_allocated_ranges) {
                for (auto it = pending->allocated_ranges.rbegin();
                     it != pending->allocated_ranges.rend(); ++it) {
                    state.alloc.push_back_bump(*it);
                }
            }

            return tree_flush_result{
                .st              = flush_stage_status::unsupported_unimplemented,
                .new_manifest    = nullptr,
                .retired         = {},
                .flushed_gens_by_front =
                    absl::flat_hash_map<
                        uint32_t,
                        absl::InlinedVector<
                            std::shared_ptr<core::memtable_gen>, 8>>{},
                .flushed_max_lsn = round->flushed_max_lsn,
            };
        }

        tree_flush_result
        succeed_pending_round(uint64_t round_id,
                              std::optional<superblock_slot> committed_slot =
                                  std::nullopt)
        {
            auto pending_it = state.pending_writes.find(round_id);
            if (pending_it == state.pending_writes.end()) {
                core::panic_inconsistency(
                    "tree_sched::succeed_pending_round",
                    "pending round_id %lu not found",
                    static_cast<unsigned long>(round_id));
            }
            auto round_it = state.active_rounds.find(round_id);
            if (round_it == state.active_rounds.end()) {
                core::panic_inconsistency(
                    "tree_sched::succeed_pending_round",
                    "active round_id %lu not found",
                    static_cast<unsigned long>(round_id));
            }

            auto pending = std::move(pending_it->second);
            auto round   = std::move(round_it->second);
            state.pending_writes.erase(pending_it);
            state.active_rounds.erase(round_it);

            if (committed_slot.has_value()) {
                state.active_superblock_slot = *committed_slot;
            }
            state.flush_max_lsn = std::max(
                state.flush_max_lsn, round->flushed_max_lsn);
            state.superblock_safe_lsn = std::max(
                state.superblock_safe_lsn, round->flushed_max_lsn);
            state.recovery_safe_lsn = recompute_recovery_safe_lsn();

            return tree_flush_result{
                .st              = flush_stage_status::ok,
                .new_manifest    = pending->new_manifest,
                .retired         = std::move(round->retired),
                .flushed_gens_by_front =
                    build_flushed_gens_by_front(round->pinned_gens),
                .flushed_max_lsn = round->flushed_max_lsn,
            };
        }

        void
        complete_pending_tree_writes(uint64_t round_id, bool write_ok) {
            auto pending_it = state.pending_writes.find(round_id);
            if (pending_it == state.pending_writes.end()) {
                core::panic_inconsistency(
                    "tree_sched::complete_pending_tree_writes",
                    "pending round_id %lu not found",
                    static_cast<unsigned long>(round_id));
            }

            auto cb = std::move(pending_it->second->cb);
            if (!write_ok) {
                cb(flush_merge_result{
                    flush_merge_done{
                        .result = fail_pending_round(
                            round_id, /*rollback_allocated_ranges=*/true),
                    },
                });
                return;
            }

            auto& pending = pending_it->second;
            if (pending->is_root_change) {
                cb(flush_merge_result{
                    flush_merge_root_change{
                        .update_req = update_superblock_request{
                            .round_id = flush_round_id{round_id},
                            .new_root_base_paddr =
                                pending->new_manifest->root_range_base,
                        },
                    },
                });
                return;
            }

            cb(flush_merge_result{
                flush_merge_root_stable{
                    .finalize_req = finalize_flush_request{
                        .round_id       = flush_round_id{round_id},
                        .ok             = true,
                        .committed_slot = std::nullopt,
                    },
                },
            });
        }

        void
        start_pending_write(uint64_t round_id) {
            auto it = state.pending_writes.find(round_id);
            if (it == state.pending_writes.end()) {
                core::panic_inconsistency(
                    "tree_sched::start_pending_write",
                    "pending round_id %lu not found",
                    static_cast<unsigned long>(round_id));
            }

            if (it->second->writes.empty()) {
                complete_pending_tree_writes(round_id, true);
                return;
            }

            auto writes = it->second->writes;
            auto* nvme = core::registry::local_nvme();

            pump::sender::as_stream(std::move(writes))
                >> pump::sender::concurrent(kWriteBatchConcurrency)
                >> pump::sender::flat_map([nvme](format::write_desc d) {
                    return nvme->write(d.lba, d.data, d.num_lbas, d.flags);
                })
                >> pump::sender::all()
                >> pump::sender::then([this, nvme, round_id](bool ok) {
                    if (!ok) {
                        complete_pending_tree_writes(round_id, false);
                        return;
                    }
                    nvme->flush()
                        >> pump::sender::then([this, round_id](bool flush_ok) {
                            complete_pending_tree_writes(round_id, flush_ok);
                        })
                        >> pump::sender::submit(pump::core::make_root_context());
                })
                >> pump::sender::submit(pump::core::make_root_context());
        }

        bool
        advance() {
            bool progress = false;

            for (uint32_t i = 0; i < kMaxFoldOpsPerAdvance; ++i) {
                auto item = fold_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                if (r->args.base_guard == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance(fold)",
                        "tree_flush_request.base_guard is null");
                }
                if (r->args.base_guard->manifest == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance(fold)",
                        "tree_flush_request.base_guard->manifest is null");
                }

                if (r->args.sealed_gens.empty()) {
                    flush_fold_result res{
                        .round_id           = flush_round_id{0},
                        .st                 = flush_stage_status::ok,
                        .partitions         = {},
                        .base_manifest      = r->args.base_guard->manifest.get(),
                        .recovery_safe_lsn  = 0,
                    };
                    r->cb(std::move(res));
                    delete r;
                    progress = true;
                    continue;
                }

                {
                    absl::flat_hash_set<uint64_t> seen_ids;
                    for (const auto& g : r->args.sealed_gens) {
                        if (g == nullptr) {
                            core::panic_inconsistency(
                                "tree::tree_sched::advance(fold)",
                                "sealed_gens contains null gen");
                        }
                        if (g->st != core::memtable_gen::state::sealed) {
                            core::panic_inconsistency(
                                "tree::tree_sched::advance(fold)",
                                "sealed_gens contains non-sealed gen");
                        }
                        if (!seen_ids.insert(g->gen_id).second) {
                            core::panic_inconsistency(
                                "tree::tree_sched::advance(fold)",
                                "sealed_gens contains duplicate gen_id");
                        }
                    }
                }

                auto rs = std::make_unique<flush_round_state>();
                rs->round_id          = flush_round_id{ state.next_round_id++ };
                rs->pinned_base_guard = std::move(r->args.base_guard);
                rs->pinned_gens       = std::move(r->args.sealed_gens);
                rs->recovery_safe_lsn = r->args.recovery_safe_lsn;

                rs->flushed_max_lsn = 0;
                for (auto& g : rs->pinned_gens) {
                    rs->flushed_max_lsn = std::max(rs->flushed_max_lsn, g->max_lsn);
                }

                auto round_id_v = rs->round_id.v;
                state.active_rounds.emplace(round_id_v, std::move(rs));
                auto& round = *state.active_rounds[round_id_v];

                fold_pinned_gens(round);

                if (round.workset.empty()) {
                    flush_fold_result res{
                        .round_id           = flush_round_id{round_id_v},
                        .st                 = flush_stage_status::ok,
                        .partitions         = {},
                        .base_manifest      = round.pinned_base_guard->manifest.get(),
                        .recovery_safe_lsn  = round.recovery_safe_lsn,
                    };
                    r->cb(std::move(res));
                    delete r;
                    progress = true;
                    continue;
                }

                // Step 030: partition count is determined by the
                // installed `shard_partition_map`; the old
                // registry-driven worker_count read is gone (030
                // §6.4 F2). `build_key_partitions` panics internally
                // if the map is not installed.
                auto partition_st = build_key_partitions(
                    round,
                    round.pinned_base_guard->manifest.get());

                if (partition_st != flush_stage_status::ok) {
                    round.st = partition_st;
                    flush_fold_result res{
                        .round_id           = flush_round_id{round_id_v},
                        .st                 = partition_st,
                        .partitions         = {},
                        .base_manifest      = round.pinned_base_guard->manifest.get(),
                        .recovery_safe_lsn  = round.recovery_safe_lsn,
                    };
                    r->cb(std::move(res));
                    delete r;
                    progress = true;
                    continue;
                }

                flush_fold_result res{
                    .round_id           = flush_round_id{round_id_v},
                    .st                 = flush_stage_status::ok,
                    .partitions         = std::move(round.partitions),
                    .base_manifest      = round.pinned_base_guard->manifest.get(),
                    .recovery_safe_lsn  = round.recovery_safe_lsn,
                };
                r->cb(std::move(res));
                delete r;
                progress = true;
            }

            for (uint32_t i = 0; i < kMaxMergeOpsPerAdvance; ++i) {
                auto item = merge_q.try_dequeue();
                if (!item) break;
                auto* r = *item;
                auto round_id_v = r->args.round_id.v;

                if (round_id_v == 0) {
                    tree_flush_result res{
                        .st              = flush_stage_status::ok,
                        .flushed_max_lsn = 0,
                    };
                    r->cb(flush_merge_result{
                        flush_merge_done{ .result = std::move(res) },
                    });
                    delete r;
                    progress = true;
                    continue;
                }

                auto it = state.active_rounds.find(round_id_v);
                if (it == state.active_rounds.end()) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance(merge)",
                        "round_id %lu not in active_rounds",
                        static_cast<unsigned long>(round_id_v));
                }
                auto& round = *it->second;

                for (const auto& wp : r->args.worker_proposals) {
                    if (wp.round_id.v != round_id_v) {
                        core::panic_inconsistency(
                            "tree::tree_sched::advance(merge)",
                            "worker_proposal round_id mismatch");
                    }
                }

                if (round.st != flush_stage_status::ok) {
                    auto gens_by_front = build_flushed_gens_by_front(round.pinned_gens);
                    auto flushed_max_lsn_v = round.flushed_max_lsn;
                    state.active_rounds.erase(round_id_v);
                    tree_flush_result res{
                        .st                    = round.st,
                        .flushed_gens_by_front = std::move(gens_by_front),
                        .flushed_max_lsn       = flushed_max_lsn_v,
                    };
                    r->cb(flush_merge_result{
                        flush_merge_done{ .result = std::move(res) },
                    });
                    delete r;
                    progress = true;
                    continue;
                }

                if (round.workset.empty()) {
                    auto gens_by_front = build_flushed_gens_by_front(round.pinned_gens);
                    auto flushed_max_lsn_v = round.flushed_max_lsn;
                    state.active_rounds.erase(round_id_v);
                    tree_flush_result res{
                        .st                    = flush_stage_status::ok,
                        .flushed_gens_by_front = std::move(gens_by_front),
                        .flushed_max_lsn       = flushed_max_lsn_v,
                    };
                    r->cb(flush_merge_result{
                        flush_merge_done{ .result = std::move(res) },
                    });
                    delete r;
                    progress = true;
                    continue;
                }

                _owner::merge_context mctx = _owner::make_merge_context(
                    round.pinned_base_guard->manifest.get(),
                    &r->args.worker_proposals);

                for (const auto& proposal : r->args.worker_proposals) {
                    for (const auto& rv : proposal.retired_old_values) {
                        round.retired.old_tree_values.push_back(rv);
                    }
                }

                const auto& root_group = _owner::merge_old_paddr(
                    mctx, round.pinned_base_guard->manifest->root_range_base);
                auto combined_root = _owner::finalize_root_group(mctx, root_group);

                absl::flat_hash_set<paddr> retired_slots_seen;
                absl::flat_hash_set<paddr> retired_ranges_seen;
                auto retire_slot = [&](paddr slot) {
                    if (retired_slots_seen.insert(slot).second) {
                        round.retired.old_slots.push_back(slot);
                    }
                };
                auto retire_range = [&](paddr rb) {
                    if (retired_ranges_seen.insert(rb).second) {
                        round.retired.old_ranges.push_back(
                            round.pinned_base_guard->manifest->geom->range_ref_from_base(rb));
                    }
                };

                auto pruned = _owner::prune_child_ref(
                    std::move(combined_root),
                    mctx.geom->tree_page_size,
                    retire_range);
                if (!pruned.has_value()) {
                    combined_root = _owner::make_empty_root_leaf(mctx.geom->tree_page_size);
                } else {
                    combined_root = std::move(*pruned);
                }

                std::vector<format::range_ref> allocated_ranges;
                _owner::assign_planned_paddrs(
                    combined_root,
                    round.pinned_base_guard->manifest.get(),
                    state.alloc,
                    allocated_ranges,
                    retire_slot,
                    retire_range);

                std::vector<format::write_desc> writes;
                writes.reserve(64);
                _owner::collect_write_descs(mctx.geom, combined_root, writes);

                std::vector<_owner::final_leaf_item> leaf_items;
                auto new_leaf_order = _owner::build_leaf_order_full(
                    mctx, combined_root, leaf_items);
                auto new_topology = _owner::build_reverse_topology_full(
                    mctx, combined_root, leaf_items);

                absl::flat_hash_map<paddr, uint32_t> slot_map;
                _owner::rebuild_slot_map(
                    round.pinned_base_guard->manifest.get(),
                    combined_root,
                    round.retired,
                    slot_map);

                auto new_manifest = std::make_shared<const core::tree_manifest>(
                    core::tree_manifest{
                        .root_slot        =
                            _owner::root_slot_of(
                                round.pinned_base_guard->manifest.get(),
                                combined_root),
                        .slot_map         = std::move(slot_map),
                        .geom             = round.pinned_base_guard->manifest->geom,
                        .leaf_order       = std::move(new_leaf_order),
                        .root_range_base  = _owner::root_range_base_of(combined_root),
                        .reverse_topology = std::move(new_topology),
                    });

                auto pending = std::make_unique<pending_write_state>();
                pending->round_id         = flush_round_id{round_id_v};
                pending->combined_root    = std::move(combined_root);
                pending->new_manifest     = std::move(new_manifest);
                pending->is_root_change   = _owner::is_root_change(
                    round.pinned_base_guard->manifest.get(),
                    pending->combined_root);
                pending->writes           = std::move(writes);
                pending->allocated_ranges = std::move(allocated_ranges);
                pending->cb               = std::move(r->cb);

                state.pending_writes.emplace(round_id_v, std::move(pending));
                delete r;
                start_pending_write(round_id_v);
                progress = true;
            }

            for (uint32_t i = 0;
                 i < kMaxUpdateSuperblockOpsPerAdvance && !update_superblock_inflight;
                 ++i) {
                auto item = update_superblock_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                if (geom == nullptr) {
                    core::panic_inconsistency(
                        "tree::tree_sched::advance(update_superblock)",
                        "tree geometry is null");
                }

                const uint64_t active_lba =
                    (state.active_superblock_slot == superblock_slot::A) ? 0 : 1;
                const uint64_t inactive_lba =
                    (state.active_superblock_slot == superblock_slot::A) ? 1 : 0;
                const superblock_slot inactive_slot =
                    (state.active_superblock_slot == superblock_slot::A)
                        ? superblock_slot::B
                        : superblock_slot::A;

                auto read_buf =
                    std::make_shared<std::vector<char>>(geom->lba_size, '\0');
                auto write_buf =
                    std::make_shared<std::vector<char>>(geom->lba_size, '\0');
                auto* nvme = core::registry::local_nvme();
                update_superblock_inflight = true;

                pump::sender::just()
                    >> pump::sender::flat_map([nvme, active_lba, read_buf]() {
                        return nvme->read(active_lba, read_buf->data(), 1);
                    })
                    >> pump::sender::then(
                        [this, nvme, r, write_buf, read_buf, inactive_lba,
                         inactive_slot](bool ok) {
                            if (!ok) {
                                update_superblock_inflight = false;
                                r->cb(update_superblock_result{
                                    .round_id       = r->args.round_id,
                                    .ok             = false,
                                    .committed_slot = inactive_slot,
                                });
                                delete r;
                                return;
                            }

                            format::superblock cur{};
                            std::memcpy(&cur, read_buf->data(), sizeof(cur));
                            auto st = format::inspect_superblock(cur);
                            if (st != format::superblock_status::ok) {
                                core::panic_inconsistency(
                                    "tree::tree_sched::advance(update_superblock)",
                                    "active superblock invalid: %s",
                                    format::superblock_status_to_string(st));
                            }

                            cur.root_base_paddr = r->args.new_root_base_paddr;
                            cur.generation += 1;
                            cur.crc = 0;
                            cur.crc = format::superblock_compute_crc(cur);

                            std::fill(write_buf->begin(), write_buf->end(), '\0');
                            std::memcpy(write_buf->data(), &cur, sizeof(cur));
                            nvme->write(
                                inactive_lba,
                                write_buf->data(),
                                1,
                                mock_nvme::IO_FLAGS_FUA)
                                >> pump::sender::then(
                                    [this, r, inactive_slot](bool write_ok) {
                                        update_superblock_inflight = false;
                                        r->cb(update_superblock_result{
                                            .round_id       = r->args.round_id,
                                            .ok             = write_ok,
                                            .committed_slot = inactive_slot,
                                        });
                                        delete r;
                                    })
                                >> pump::sender::submit(
                                    pump::core::make_root_context());
                        })
                    >> pump::sender::submit(pump::core::make_root_context());
                progress = true;
            }

            for (uint32_t i = 0; i < kMaxFinalizeOpsPerAdvance; ++i) {
                auto item = finalize_q.try_dequeue();
                if (!item) break;
                auto* r = *item;

                tree_flush_result res = r->args.ok
                    ? succeed_pending_round(
                          r->args.round_id.v, r->args.committed_slot)
                    : fail_pending_round(
                          r->args.round_id.v,
                          /*rollback_allocated_ranges=*/false);

                r->cb(std::move(res));
                delete r;
                progress = true;
            }

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
    _flush_merge::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_merge(new _flush_merge::req{
            std::move(args),
            [ctx = ctx, scope = scope](flush_merge_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
        });
    }

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void
    _update_superblock::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_update_superblock(new _update_superblock::req{
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
        && (get_current_op_type_t<pos, scope_t>::flush_merge_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_flush_merge::sender> {
        consteval static uint32_t
        count_value() {
            return 1;
        }
        consteval static auto
        get_value_type_identity() {
            return std::type_identity<apps::inconel::tree::flush_merge_result>{};
        }
    };

    template <uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::update_superblock_op)
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
    compute_sender_type<ctx_t, apps::inconel::tree::_update_superblock::sender> {
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
