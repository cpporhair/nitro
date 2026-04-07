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

    template <typename C>
    concept cache_concept = requires(C c, const C cc, paddr k, char* b) {
        { c.get(k) }       -> std::same_as<const char*>;
        { c.put(k, b) }    -> std::same_as<std::optional<evicted_entry>>;
        { cc.contains(k) } -> std::same_as<bool>;
        { cc.size() }      -> std::same_as<uint32_t>;
        { cc.capacity() }  -> std::same_as<uint32_t>;
    };

    static_assert(cache_concept<clock_cache>);
    static_assert(cache_concept<slru_cache>);

}

#endif //APPS_INCONEL_CORE_PAGE_CACHE_HH
