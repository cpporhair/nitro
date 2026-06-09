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
// Current target design: memtable_gen owns key bytes and value
// metadata only. Value bodies are durable in the Value Area before
// WAL/memtable insertion, and memtable value hits return durable
// value_ref, not a value body view.
//
// This header still carries the legacy Phase 021 carrier
// value_handle{durable, hot} because the front/memtable module has
// not been replaced yet and flush_e2e still constructs sealed gens
// directly. Treat value_view/hot as compatibility debt tracked by
// INC-055; new code should depend only on durable value_ref.
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
    // Per-memtable_gen bump allocator. Target usage is key bytes
    // only. Legacy flush_e2e/front carriers may still allocate value
    // hot views here until INC-055 removes value_handle::hot.
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
    // Legacy {pointer, length} view over value bytes. Target
    // memtable lookup returns durable value_ref only; this type
    // remains temporarily for the old value_handle carrier and is
    // scheduled for removal by INC-055.

    struct value_view {
        const char* data;
        uint32_t    len;
    };

    // ── value_handle ────────────────────────────────────────
    //
    // Legacy payload of a memtable PUT entry. `durable` is the
    // stable on-disk location and is the only field new code should
    // consume. `hot` is compatibility debt for the pre-INC-055
    // memtable carrier.

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
    // Target value entries carry durable value_ref only; the vh.hot
    // subfield is a legacy compatibility view. data_ver is
    // semantically equivalent to batch_lsn (OV §6).

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
    // gen, which may clear+rebuild loser_durable_refs only
    // (never touch table or arena).
    //
    // kv_arena target usage is key bytes only. The btree_map's key
    // is std::string_view into kv_arena; any memtable_entry.vh.hot
    // view is legacy compatibility state and must not be copied into
    // new designs. When the last shared_ptr<memtable_gen> drops, the
    // arena frees all chunks in one sweep.
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
        std::shared_ptr<memtable_gen>                         active;
        absl::InlinedVector<std::shared_ptr<memtable_gen>, 8> imms;  // newest → oldest
    };

    // ── Invariants enforced at compile time ────────────────

    static_assert(std::is_trivially_copyable_v<value_view>,
                  "value_view must be a POD (pointer + length)");
    static_assert(std::is_trivially_copyable_v<value_handle>,
                  "legacy value_handle must stay POD until INC-055 removes hot");
    static_assert(std::is_trivially_copyable_v<memtable_entry>,
                  "memtable_entry must be POD now that value_handle is POD");
    static_assert(sizeof(value_view) <= 16,
                  "value_view should remain pointer + length only");

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_MEMTABLE_HH
