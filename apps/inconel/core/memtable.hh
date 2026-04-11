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
// Key design choice: each memtable_gen owns a kv_arena that
// holds BOTH key bytes and value bytes for that gen. String
// views into the arena are what the btree_map's key and
// value_handle::hot carry. The arena frees all its chunks
// atomically when the last shared_ptr<memtable_gen> drops,
// retiring keys, values, and hot data together.
//
// This collapses what was previously a separate hot_blob /
// unique_ptr machinery into a single lifetime owner, and
// lets memtable_entry / value_handle be trivially copyable
// PODs.
//
// Any structural change here must also update:
//   - design_overview.md §5.1, §5.3
//   - runtime_state_machine.md §3.1-3.3, §3.5
//   - runtime_memory_and_cache.md §2, §3, §9.3, §9.4
//   - cross_doc_contracts.md §2
//   - read_api_and_pipeline.md §4.4

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/btree_map.h>
#include <absl/container/inlined_vector.h>

#include "../format/types.hh"

namespace apps::inconel::core {

    using format::value_ref;

    // ── gen_arena ────────────────────────────────────────────
    //
    // Per-memtable_gen bump allocator. Holds BOTH key bytes and
    // value bytes for this gen; key string_views and value
    // views in memtable_entry point into it.
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
            if (bump_next + len > bump_end) {
                const std::size_t cap = (len > kChunkBytes) ? len : kChunkBytes;
                auto chunk = std::make_unique<char[]>(cap);
                bump_next = chunk.get();
                bump_end  = chunk.get() + cap;
                chunks.push_back(std::move(chunk));
            }
            char* start = bump_next;
            if (len > 0) {
                std::memcpy(start, src, len);
            }
            bump_next += len;
            return std::string_view{start, len};
        }
    };

    // ── value_view ──────────────────────────────────────────
    //
    // A {pointer, length} view over value bytes. Used as the
    // "hot" half of value_handle (pointing into the owning
    // gen's kv_arena) and as the return type of lookup_memtable
    // on a value hit. Lifetime is tied to the owning gen via
    // the read_handle → cat → prs → shared_ptr<memtable_gen>
    // pin chain (see RSM §3.7, RMC §9.3).

    struct value_view {
        const char* data;
        uint32_t    len;
    };

    // ── value_handle ────────────────────────────────────────
    //
    // Payload of a memtable PUT entry (OV §5.1, RSM §3.3).
    // POD: both fields are trivially copyable. `durable` is
    // the stable on-disk location of the value object; `hot`
    // is a zero-copy view into the owning gen's kv_arena.

    struct value_handle {
        value_ref  durable;
        value_view hot;
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

    // ── retire_list<T> ──────────────────────────────────────
    //
    // Single-writer / single-reader append+drain container.
    // Used for memtable_gen::loser_durable_refs. No internal
    // locking: caller discipline enforces the invariant.

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

      private:
        absl::InlinedVector<T, 16> items_;
    };

    // ── memtable_entry ──────────────────────────────────────
    //
    // Trivially copyable (no unique_ptr, no heap-owning field).
    // Value bytes live in the owning gen's kv_arena; vh.hot is
    // just a view into them. data_ver is semantically
    // equivalent to batch_lsn (OV §6).

    struct memtable_entry {
        uint64_t data_ver;

        enum class kind : uint8_t {
            value,
            tombstone,
        } k;

        value_handle vh;  // valid iff k == kind::value
    };

    // ── memtable_gen ────────────────────────────────────────
    //
    // A generation of the front memtable (RSM §3.2). The owning
    // front_sched is the sole writer while st == state::active.
    // Once sealed, table and kv_arena are immutable to all
    // mutators except the flush round currently folding this
    // gen, which may append to loser_durable_refs only (never
    // to table or arena).
    //
    // kv_arena holds all gen-local bytes (both keys and values).
    // The btree_map's key is std::string_view into kv_arena;
    // memtable_entry.vh.hot is a value_view into the same arena.
    // When the last shared_ptr<memtable_gen> drops, the arena
    // frees all its chunks in one sweep, retiring keys, values,
    // and hot data together.
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

        uint64_t min_lsn = UINT64_MAX;
        uint64_t max_lsn = 0;

        gen_arena kv_arena;

        absl::btree_map<std::string_view,
                        absl::InlinedVector<memtable_entry, 1>>
            table;

        retire_list<retired_value_ref> loser_durable_refs;
    };

    // ── front_read_set ──────────────────────────────────────
    //
    // Readonly snapshot of a single front's active + imms
    // chain, captured at PRS construction time (OV §5.3,
    // RSM §3.1). Each shared_ptr copy bumps the gen's control
    // block refcount; the pin chain keeps every memtable_entry
    // and all its arena-backed bytes alive for the full
    // lifetime of any reader holding this PRS snapshot.

    struct front_read_set {
        std::shared_ptr<memtable_gen>                         active;
        absl::InlinedVector<std::shared_ptr<memtable_gen>, 8> imms;  // newest → oldest
    };

    // ── Invariants enforced at compile time ────────────────

    static_assert(std::is_trivially_copyable_v<value_view>,
                  "value_view must be a POD (pointer + length)");
    static_assert(std::is_trivially_copyable_v<value_handle>,
                  "value_handle must be POD now that hot is a view");
    static_assert(std::is_trivially_copyable_v<memtable_entry>,
                  "memtable_entry must be POD now that value_handle is POD");
    static_assert(sizeof(value_view) <= 16,
                  "value_view should remain pointer + length only");

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_MEMTABLE_HH
