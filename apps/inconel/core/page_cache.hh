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
    // clock_cache/slru_cache for legacy contiguous frames, and
    // segmented_clock_cache/segmented_slru_cache for LBA DMA frames.
    //
    // Compile-time interface contract — no virtual dispatch, no variant
    // visit, all calls are inlined directly at the use site.
    //
    // Ownership rules:
    //
    //   - The cache stores Cache::frame_type* and never frees them on its own.
    //     The caller owns both the frame descriptor and its backing
    //     buffer; the cache is a non-owning index with pin semantics.
    //   - `put(f)` only accepts frames with st == clean_readonly.
    //     Returns optional<frame_type*> whenever the call displaces a
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
    //   - `pin(id)` returns the cache's RAII pin_type. On hit, pin_count is
    //     incremented before the pin is returned. On miss, the pin's
    //     frame pointer is nullptr.
    //   - `take(id)` removes a clean frame from the cache and returns it
    //     to the caller. On miss it returns nullopt. If the matching
    //     frame is still pinned, the cache implementation must fail-fast:
    //     reclaim / recycle paths are not allowed to race with a live pin.
    //   - Runtime eviction (inside put) must skip entries with
    //     pin_count > 0.
    //   - `drain_one()` is the teardown drain: pulls one entry out
    //     regardless of pin state. Returns nullopt when empty.

    template <typename C>
    concept cache_concept = requires(C c, const C cc,
                                     memory::frame_id id,
                                     typename C::frame_type* f) {
        typename C::frame_type;
        typename C::pin_type;
        { c.pin(id) }       -> std::same_as<typename C::pin_type>;
        { c.take(id) }      -> std::same_as<std::optional<typename C::frame_type*>>;
        { c.put(f) }        -> std::same_as<std::optional<typename C::frame_type*>>;
        { cc.contains(id) } -> std::same_as<bool>;
        { cc.size() }       -> std::same_as<uint32_t>;
        { cc.capacity() }   -> std::same_as<uint32_t>;
        { c.drain_one() }   -> std::same_as<std::optional<typename C::frame_type*>>;
    };

    static_assert(cache_concept<clock_cache>);
    static_assert(cache_concept<slru_cache>);
    static_assert(cache_concept<segmented_clock_cache>);
    static_assert(cache_concept<segmented_slru_cache>);

}

#endif //APPS_INCONEL_CORE_PAGE_CACHE_HH
