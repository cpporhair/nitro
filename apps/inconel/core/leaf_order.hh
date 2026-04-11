#ifndef APPS_INCONEL_CORE_LEAF_ORDER_HH
#define APPS_INCONEL_CORE_LEAF_ORDER_HH

// ── leaf_order.hh ── per-manifest immutable leaf ordering (step 023 §1) ──
//
// Phase 3 (step 023 G1 / D2-D5) introduces the runtime-only immutable
// leaf_order carrier referenced by RSM §4.5, FF §3.4/§3.8, and OV §4.4.
// `leaf_order_index` is a by-value member of `tree_manifest`: when the
// owning manifest snapshot drops its last shared_ptr, every fence byte
// and every span retires together — there is no other pin path. The
// whole index is read-only after construction (RSM §4.5 冻结条目).
//
// Phase 3 only freezes field layout, default-constructibility, and the
// `fence_lower` / `fence_upper` view semantics. The real builder
// (`rebuild_leaf_order_from_tree_delta`, FF §3.8) belongs to the
// Phase 7 root-stable writer step and MUST NOT be anticipated here.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "../format/types.hh"
#include "./panic.hh"

namespace apps::inconel::core {

    using format::paddr;

    // ── leaf_span ──
    //
    // Per-leaf positional record inside the immutable leaf_order index.
    // Fence bytes are NOT carried inline; the index owns a single
    // contiguous `fence_pool` and each span addresses its bounds via
    // {offset, length}. Adjacent leaves share fence storage (D3): for
    // any `i`, the bytes at
    //   `[upper_off[i], upper_off[i] + upper_len[i])`
    // are byte-identical to
    //   `[lower_off[i+1], lower_off[i+1] + lower_len[i+1])`,
    // so we store them once and let both leaves address the same
    // offset. This is a spec, not an optimization — see 023 §容量估算
    // 结论 2: disabling dedupe would ~double a 4 KiB-page 1B KV tree
    // manifest working set to roughly 1.7 GB and immediately violate
    // INDEX.md's capacity constraint.
    //
    // Field widths (D4):
    //   - `fence_*_off : uint32_t`  caps `fence_pool` at 4 GiB
    //   - `fence_*_len : uint16_t`  caps a single fence at 64 KiB
    // Both ceilings sit far above the 1B KV / 32B-key working set; see
    // 023 §容量估算 column "16 KiB / 4 KiB" for the exact budget. The
    // 24-byte layout below assumes 2 bytes of natural padding after the
    // uint16_t length pair; the static_assert locks that in so a future
    // proposal to add a field cannot silently inflate per-leaf cost to
    // 32+ bytes without somebody noticing at compile time.

    struct leaf_span {
        uint32_t fence_lower_off;
        uint32_t fence_upper_off;
        uint16_t fence_lower_len;
        uint16_t fence_upper_len;
        paddr    leaf_range_base;
    };

    static_assert(sizeof(leaf_span) == 24,
                  "leaf_span layout frozen at 24 bytes (023 §1 D4)");
    static_assert(std::is_trivially_copyable_v<leaf_span>,
                  "leaf_span must be a POD so vectors of spans "
                  "copy/move without per-element construction cost");

    // ── leaf_order_index ──
    //
    // Per-manifest immutable leaf order (RSM §4.5 / FF §3.4 / OV §4.4).
    // Owns the fence bytes and the sorted span vector by value: when
    // the owning `tree_manifest` snapshot drops its last shared_ptr,
    // every fence byte and every span retire at the same time. There
    // is no other pin path; callers MUST NOT borrow into
    // `fence_pool` / `spans` beyond the manifest's lifetime.
    //
    // Phase 3 construction sites:
    //   1. Bootstrap empty path — `leaf_order_index{}` (D5). The empty
    //      tree case is covered by `!has_root()` short-circuiting on
    //      the read side (already landed in step 022) and does not
    //      need the index at all.
    //   2. Phase 7 root-stable writer — rebuilds the new index from
    //      the old one + write_plan + allocations + consolidations
    //      (FF §3.8 `rebuild_leaf_order_from_tree_delta`). Phase 3
    //      does NOT implement that and the builder interface stays
    //      deliberately absent from this header.
    //
    // The two `fence_*_view(span)` accessors return `std::string_view`
    // into `fence_pool`. Those views are valid for the same lifetime
    // as the owning `leaf_order_index` — i.e. for as long as the
    // enclosing `tree_manifest` is pinned by some `checkpoint_guard`.
    // They do not allocate and do not copy. On any out-of-bounds
    // {offset, length} — which the spec treats as an irrecoverable
    // invariant break — they call `core::panic_inconsistency`
    // directly rather than relying on `std::string_view::substr`'s
    // throw-or-truncate behavior. See 023 review M-2 for why this is
    // mandatory: a `substr` slice is a silent contract violation when
    // `off <= size() < off + len`, and `noexcept` + stdlib throw
    // would bypass the structured diagnostic entirely.

    struct leaf_order_index {
        // Single contiguous owning byte buffer for every fence-key
        // byte referenced by `spans`. Single-allocation by design:
        // per-leaf small allocations at 1B KV scale are explicitly
        // rejected by 023 §容量估算 / §1 约束 2.
        std::string            fence_pool;

        // Sorted-by-lower-bound leaf positional records. `spans` and
        // `fence_pool` are constructed together and retired together.
        std::vector<leaf_span> spans;

        bool
        empty() const noexcept {
            return spans.empty();
        }

        std::size_t
        size() const noexcept {
            return spans.size();
        }

        std::string_view
        fence_lower(const leaf_span& s) const {
            return fence_slice("leaf_order_index::fence_lower",
                               s.fence_lower_off,
                               s.fence_lower_len);
        }

        std::string_view
        fence_upper(const leaf_span& s) const {
            return fence_slice("leaf_order_index::fence_upper",
                               s.fence_upper_off,
                               s.fence_upper_len);
        }

      private:
        // Shared implementation: reject every {offset, length} pair
        // that does not fit entirely within `fence_pool`. The
        // promotion to `std::size_t` is deliberate — `fence_*_off`
        // is uint32_t and `fence_*_len` is uint16_t, so their sum
        // cannot overflow a 64-bit `size_t`, and checking in `size_t`
        // width lets us compare against `fence_pool.size()` without
        // a mixed-signedness trap.
        std::string_view
        fence_slice(const char* site,
                    uint32_t off_u32,
                    uint16_t len_u16) const {
            const std::size_t pool_size = fence_pool.size();
            const std::size_t off = static_cast<std::size_t>(off_u32);
            const std::size_t len = static_cast<std::size_t>(len_u16);
            if (off > pool_size || len > pool_size - off) {
                panic_inconsistency(
                    site,
                    "out-of-bounds fence slice: off=%u len=%u pool_size=%zu",
                    static_cast<unsigned>(off_u32),
                    static_cast<unsigned>(len_u16),
                    pool_size);
            }
            return std::string_view(fence_pool.data() + off, len);
        }
    };

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_LEAF_ORDER_HH
