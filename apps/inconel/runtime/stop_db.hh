#ifndef APPS_INCONEL_RUNTIME_STOP_DB_HH
#define APPS_INCONEL_RUNTIME_STOP_DB_HH

#include "pump/sender/flat.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"

#include "./session.hh"

namespace apps::inconel::runtime {

    [[nodiscard]] inline auto
    stop_db_sender() {
        return pump::sender::just()
            >> pump::sender::get_context<db_session_view>()
            >> pump::sender::then([](db_session_view& view) {
                view.stop();
            });
    }

    [[nodiscard]] inline auto
    stop_db() {
        return pump::sender::flat_map([](auto&&...) {
            return stop_db_sender();
        });
    }

}  // namespace apps::inconel::runtime

#endif  // APPS_INCONEL_RUNTIME_STOP_DB_HH
