#ifndef APPS_INCONEL_CORE_PAGE_CACHE_HH
#define APPS_INCONEL_CORE_PAGE_CACHE_HH

#include <concepts>
#include <cstdint>
#include <optional>

#include "../format/types.hh"
#include "./clock_cache.hh"
#include "./slru_cache.hh"

namespace apps::inconel::core {

    // ── Cache concept ──
    //
    // Any type satisfying this concept can be plugged into the templated
    // lookup_scheduler. Concrete implementations: clock_cache, slru_cache.
    //
    // Compile-time interface contract — no virtual dispatch, no variant
    // visit, all calls are inlined directly at the use site.
    //
    // Ownership rules for `put(k, b)` and `evict_one()`:
    //
    //   - The cache stores raw `char*` and never frees them on its own. The
    //     caller transfers ownership of `b` to the cache by calling `put`.
    //   - `put` returns `optional<evicted_entry>` whenever the call displaces
    //     a previously-owned buffer:
    //       * key already present → returned entry holds the OLD buf for k
    //         (the cache now holds the new b for k).
    //       * key absent + cache full → returned entry holds the (key, buf)
    //         that the cache evicted to make room.
    //       * key absent + cache has space → returns nullopt.
    //     In every "Some(...)" case the caller MUST free the returned buf,
    //     otherwise it leaks. Silently overwriting same-key puts (e.g. two
    //     concurrent fills for the same paddr inside one advance round)
    //     would otherwise drop ownership of the previous allocation.
    //   - `evict_one` is the teardown drain: pulls one entry out and returns
    //     its (key, buf), letting the caller free every admitted buf via a
    //     loop. Returns nullopt when the cache is empty.

    template <typename C>
    concept cache_concept = requires(C c, const C cc, paddr k, char* b) {
        { c.get(k) }       -> std::same_as<const char*>;
        { c.put(k, b) }    -> std::same_as<std::optional<evicted_entry>>;
        { cc.contains(k) } -> std::same_as<bool>;
        { cc.size() }      -> std::same_as<uint32_t>;
        { cc.capacity() }  -> std::same_as<uint32_t>;
        { c.evict_one() }  -> std::same_as<std::optional<evicted_entry>>;
    };

    static_assert(cache_concept<clock_cache>);
    static_assert(cache_concept<slru_cache>);

}

#endif //APPS_INCONEL_CORE_PAGE_CACHE_HH
