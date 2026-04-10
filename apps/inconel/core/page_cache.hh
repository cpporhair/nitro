#ifndef APPS_INCONEL_CORE_PAGE_CACHE_HH
#define APPS_INCONEL_CORE_PAGE_CACHE_HH

#include <concepts>
#include <cstdint>
#include <optional>

#include "../memory/frame.hh"
#include "./clock_cache.hh"
#include "./slru_cache.hh"

namespace apps::inconel::core {

    // ── Frame-oriented readonly cache concept ──
    //
    // Any type satisfying this concept can be plugged into the templated
    // tree_lookup_sched and value_alloc_sched. Concrete implementations:
    // clock_cache, slru_cache.
    //
    // Compile-time interface contract — no virtual dispatch, no variant
    // visit, all calls are inlined directly at the use site.
    //
    // Ownership rules:
    //
    //   - The cache stores page_frame* and never frees them on its own.
    //     The caller owns both the page_frame descriptor and its backing
    //     buffer; the cache is a non-owning index with pin semantics.
    //   - `put(f)` only accepts frames with st == clean_readonly.
    //     Returns optional<page_frame*> whenever the call displaces a
    //     previously-held frame:
    //       * key already present + old pin_count == 0 → returned frame
    //         is the OLD entry (the cache now holds the new f).
    //       * key already present + old pin_count > 0 → returned frame
    //         is f itself (replacement rejected; cache keeps the pinned
    //         old frame, size unchanged, no promote/move).
    //       * key absent + cache full → returned frame is the evicted
    //         victim (pin_count == 0 guaranteed).
    //       * key absent + cache full + all entries pinned → returned
    //         frame is f itself (insertion rejected).
    //       * key absent + cache has space → returns nullopt.
    //     In every "Some(...)" case the caller MUST free the returned
    //     frame + its backing buffer.
    //   - `pin(id)` returns an RAII frame_pin. On hit, pin_count is
    //     incremented before the pin is returned. On miss, the pin's
    //     frame pointer is nullptr.
    //   - Runtime eviction (inside put) must skip entries with
    //     pin_count > 0.
    //   - `drain_one()` is the teardown drain: pulls one entry out
    //     regardless of pin state. Returns nullopt when empty.

    template <typename C>
    concept cache_concept = requires(C c, const C cc,
                                     memory::frame_id id,
                                     memory::page_frame* f) {
        { c.pin(id) }       -> std::same_as<memory::frame_pin>;
        { c.put(f) }        -> std::same_as<std::optional<memory::page_frame*>>;
        { cc.contains(id) } -> std::same_as<bool>;
        { cc.size() }       -> std::same_as<uint32_t>;
        { cc.capacity() }   -> std::same_as<uint32_t>;
        { c.drain_one() }   -> std::same_as<std::optional<memory::page_frame*>>;
    };

    static_assert(cache_concept<clock_cache>);
    static_assert(cache_concept<slru_cache>);

}

#endif //APPS_INCONEL_CORE_PAGE_CACHE_HH
