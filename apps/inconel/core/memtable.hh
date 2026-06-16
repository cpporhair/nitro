#ifndef APPS_INCONEL_CORE_MEMTABLE_HH
#define APPS_INCONEL_CORE_MEMTABLE_HH

// ── Why this file exists ────────────────────────────────────
//
// Cross-module memtable runtime types consumed by the front
// scheduler, the read pipeline, and the tree-local flush
// pipeline. Type definitions only — no scheduler, no handle,
// no advance loop, no factory with business logic. See
// ai_context/inconel/plan/021_front_input_memtable_carrier.md
// for the Phase 1 scope rationale.
//
// memtable_gen owns key bytes and value metadata only. Value bodies
// are durable in the Value Area before WAL/memtable insertion, and
// memtable value hits return durable value_ref, not a value body view.
//
// Any structural change here must also update:
//   - design_overview.md §5.1, §5.3
//   - runtime_state_machine.md §3.1-3.3, §3.5
//   - runtime_memory_and_cache.md §2, §3, §9.3, §9.4
//   - cross_doc_contracts.md §2
//   - read_api_and_pipeline.md §4.4

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/btree_map.h>
#include <absl/container/inlined_vector.h>

#include "../format/types.hh"

namespace apps::inconel::core {

    using format::value_ref;

    struct retired_objects;

    // ── gen_arena ────────────────────────────────────────────
    //
    // Per-memtable_gen bump allocator. It owns key bytes only.
    //
    // Single-writer: only the owning front_sched mutates the
    // arena while the gen is active. Once the gen is sealed,
    // the arena is frozen. When the last
    // shared_ptr<memtable_gen> drops, the arena frees all its
    // chunks atomically alongside the gen.
    //
    // Chunk size is 64 KB by default. An oversized single
    // entry (e.g. a value larger than a chunk) gets a
    // dedicated chunk of size max(len, kChunkBytes). No
    // internal locking: caller discipline enforces the single-
    // writer invariant.

    struct gen_arena {
        static constexpr std::size_t kChunkBytes = 64 * 1024;

        std::vector<std::unique_ptr<char[]>> chunks;
        char*                                bump_next = nullptr;
        char*                                bump_end  = nullptr;

        // Copy `len` bytes of `src` into the arena and return
        // a string_view over the allocated slice. The returned
        // view is valid as long as `*this` is alive (which is
        // the lifetime of the owning memtable_gen).
        std::string_view
        allocate(const char* src, std::size_t len) {
            if (len == 0) return {};
            if (bump_next == nullptr ||
                static_cast<std::size_t>(bump_end - bump_next) < len) {
                const std::size_t cap = (len > kChunkBytes) ? len : kChunkBytes;
                auto chunk = std::make_unique<char[]>(cap);
                bump_next = chunk.get();
                bump_end  = chunk.get() + cap;
                chunks.push_back(std::move(chunk));
            }
            char* start = bump_next;
            std::memcpy(start, src, len);
            bump_next += len;
            return std::string_view{start, len};
        }
    };

    // ── value_handle ────────────────────────────────────────
    //
    // Payload of a memtable PUT entry. This is only a durable
    // locator; memtable does not own or cache value bytes.

    struct value_handle {
        value_ref durable;
    };

    // ── retired_value_ref ───────────────────────────────────
    //
    // {value_ref, data_ver} tuple used by retire lists for
    // value reclamation decisions against recovery_safe_lsn
    // (RSM §3.3, FF §5.1).

    struct retired_value_ref {
        value_ref vr;
        uint64_t  data_ver;
    };

    using retired_value_refs = absl::InlinedVector<retired_value_ref, 16>;

    struct reclaim_sink {
        void* self = nullptr;
        void (*post_retired)(void*, retired_objects&&) = nullptr;
        void (*post_gen_losers)(void*, retired_value_refs&&) = nullptr;
    };

    inline std::atomic<reclaim_sink*> active_reclaim_sink_cell{nullptr};

    inline void
    set_reclaim_sink(reclaim_sink* sink) noexcept {
        active_reclaim_sink_cell.store(sink, std::memory_order_release);
    }

    [[nodiscard]] inline reclaim_sink*
    active_reclaim_sink() noexcept {
        return active_reclaim_sink_cell.load(std::memory_order_acquire);
    }

    // ── retire_list<T> ──────────────────────────────────────
    //
    // Single-writer / single-reader container.
    // Used for memtable_gen::loser_durable_refs. Normal lifecycle
    // is push -> drain; tree flush fold may clear+rebuild the list
    // when retrying an unfinished round on the same sealed gen.
    // No internal locking: caller discipline enforces the invariant.

    template <typename T>
    struct retire_list {
        void
        push(T v) {
            items_.push_back(std::move(v));
        }

        template <typename F>
        void
        drain(F&& f) {
            for (auto& item : items_) {
                std::forward<F>(f)(std::move(item));
            }
            items_.clear();
        }

        std::size_t
        size() const noexcept {
            return items_.size();
        }

        void
        clear() noexcept {
            items_.clear();
        }

      private:
        absl::InlinedVector<T, 16> items_;
    };

    // ── memtable_entry ──────────────────────────────────────
    //
    // Trivially copyable (no unique_ptr, no heap-owning field).
    // Value entries carry durable value_ref only. data_ver is
    // semantically equivalent to batch_lsn (OV §6).

    struct memtable_entry {
        uint64_t data_ver;

        enum class kind : uint8_t {
            value,
            tombstone,
        } k;

        value_handle vh;  // valid iff k == kind::value
    };

    struct memtable_value_hit {
        value_ref durable;
    };

    struct memtable_tombstone {};
    struct memtable_miss {};

    using memtable_lookup_result =
        std::variant<memtable_value_hit, memtable_tombstone, memtable_miss>;

    // ── memtable_gen ────────────────────────────────────────
    //
    // A generation of the front memtable (RSM §3.2). The owning
    // front_sched is the sole writer while st == state::active.
    // Once sealed, table and kv_arena are immutable to all
    // mutators except the flush round currently folding this
    // gen, which may clear+rebuild loser_durable_refs only
    // (never touch table or arena).
    //
    // kv_arena stores key bytes only. The btree_map's key is
    // std::string_view into kv_arena. When the last
    // shared_ptr<memtable_gen> drops, the arena frees all chunks in
    // one sweep.
    //
    // Declaration order: kv_arena declared BEFORE table so that
    // reverse-order destruction destroys table first. Purely
    // defensive — table destruction does not dereference arena
    // slices — but the order makes the dependency obvious.
    //
    // Lifetime is managed by std::shared_ptr<memtable_gen>. The
    // shared_ptr control block refcount is the only cross-
    // thread atomic gate for the entire memtable object graph.
    // Construction: std::make_shared<memtable_gen>(...). See
    // step 021 D7 and cross_doc §2.

    struct memtable_gen {
        uint64_t gen_id;

        enum class state : uint8_t {
            active,
            sealed,
        } st;

        uint32_t front_owner_index = UINT32_MAX;  // Phase 4 D17
                                                   // UINT32_MAX = invalid sentinel

        uint64_t min_lsn = UINT64_MAX;
        uint64_t max_lsn = 0;

        gen_arena kv_arena;

        absl::btree_map<std::string_view,
                        absl::InlinedVector<memtable_entry, 1>>
            table;

        retire_list<retired_value_ref> loser_durable_refs;

        ~memtable_gen() {
            if (loser_durable_refs.size() == 0) {
                return;
            }

            retired_value_refs losers;
            loser_durable_refs.drain([&losers](retired_value_ref&& ref) {
                losers.push_back(std::move(ref));
            });
            if (auto* sink = active_reclaim_sink()) {
                sink->post_gen_losers(sink->self, std::move(losers));
            }
        }
    };

    // ── front_read_set ──────────────────────────────────────
    //
    // Readonly snapshot of a single front's active + imms
    // chain, captured at PRS construction time (OV §5.3,
    // RSM §3.1). Each shared_ptr copy bumps the gen's control
    // block refcount; the pin chain keeps every memtable_entry and
    // key view alive for the full lifetime of any reader holding this
    // PRS snapshot. Value body residency belongs to value_alloc_sched,
    // not the memtable pin chain.

    struct front_read_set {
        std::shared_ptr<memtable_gen>              active;
        std::vector<std::shared_ptr<memtable_gen>> imms;  // newest → oldest
    };

    inline void
    update_lsn_bounds(memtable_gen& gen, uint64_t data_ver) noexcept {
        if (data_ver < gen.min_lsn) gen.min_lsn = data_ver;
        if (data_ver > gen.max_lsn) gen.max_lsn = data_ver;
    }

    inline absl::InlinedVector<memtable_entry, 1>&
    ensure_versions_for_key(memtable_gen& gen, std::string_view key) {
        auto it = gen.table.lower_bound(key);
        if (it != gen.table.end() && it->first == key) {
            return it->second;
        }

        const auto arena_key = gen.kv_arena.allocate(key.data(), key.size());
        it = gen.table.try_emplace(it, arena_key);
        return it->second;
    }

    inline void
    insert_value(memtable_gen& gen,
                 std::string_view key,
                 uint64_t data_ver,
                 value_ref durable) {
        auto& versions = ensure_versions_for_key(gen, key);
        versions.push_back(memtable_entry{
            .data_ver = data_ver,
            .k        = memtable_entry::kind::value,
            .vh       = value_handle{.durable = durable},
        });
        update_lsn_bounds(gen, data_ver);
    }

    inline void
    insert_tombstone(memtable_gen& gen,
                     std::string_view key,
                     uint64_t data_ver) {
        auto& versions = ensure_versions_for_key(gen, key);
        versions.push_back(memtable_entry{
            .data_ver = data_ver,
            .k        = memtable_entry::kind::tombstone,
            .vh       = {},
        });
        update_lsn_bounds(gen, data_ver);
    }

    inline const memtable_entry*
    find_visible_entry(const memtable_gen& gen,
                       std::string_view key,
                       uint64_t read_lsn) {
        const auto it = gen.table.find(key);
        if (it == gen.table.end()) return nullptr;

        const memtable_entry* best = nullptr;
        for (const auto& entry : it->second) {
            if (entry.data_ver <= read_lsn &&
                (!best || entry.data_ver > best->data_ver)) {
                best = &entry;
            }
        }
        return best;
    }

    inline memtable_lookup_result
    lookup_visible(const memtable_gen& gen,
                   std::string_view key,
                   uint64_t read_lsn) {
        const auto* entry = find_visible_entry(gen, key, read_lsn);
        if (entry == nullptr) return memtable_miss{};
        if (entry->k == memtable_entry::kind::value) {
            return memtable_value_hit{.durable = entry->vh.durable};
        }
        return memtable_tombstone{};
    }

    // ── Invariants enforced at compile time ────────────────

    static_assert(std::is_trivially_copyable_v<value_handle>,
                  "value_handle must stay POD: durable value_ref only");
    static_assert(std::is_trivially_copyable_v<memtable_entry>,
                  "memtable_entry must be POD now that value_handle is POD");

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_MEMTABLE_HH
