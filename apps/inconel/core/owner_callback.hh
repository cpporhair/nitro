#ifndef APPS_INCONEL_CORE_OWNER_CALLBACK_HH
#define APPS_INCONEL_CORE_OWNER_CALLBACK_HH

#include <cstdint>
#include <exception>
#include <expected>
#include <type_traits>
#include <utility>

#include "pump/core/op_pusher.hh"

namespace apps::inconel::core {

    template <typename T>
    using owner_outcome = std::expected<T, std::exception_ptr>;

    template <uint32_t pos, typename scope_t, typename T, typename ctx_t>
    [[nodiscard]] auto
    make_owner_pusher(ctx_t& ctx, scope_t& scope) {
        return [ctx = ctx, scope = scope](owner_outcome<T>&& r) mutable {
            if (r.has_value()) {
                if constexpr (std::is_void_v<T>) {
                    pump::core::op_pusher<pos + 1, scope_t>::push_value(
                        ctx, scope);
                } else {
                    pump::core::op_pusher<pos + 1, scope_t>::push_value(
                        ctx, scope, std::move(*r));
                }
            } else {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(r.error()));
            }
        };
    }

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_OWNER_CALLBACK_HH
