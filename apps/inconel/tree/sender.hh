#ifndef APPS_INCONEL_TREE_SENDER_HH
#define APPS_INCONEL_TREE_SENDER_HH

#include "pump/core/meta.hh"
#include "pump/coro/coro.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"
#include "pump/sender/visit.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"

#include "./scheduler.hh"
#include "../mock_nvme/sender.hh"

namespace apps::inconel::tree {

    using namespace pump::sender;

    inline pump::coro::return_yields<bool>
    check_not_done(const lookup_state& state) {
        while (!state.all_done)
            co_yield true;
        co_return false;
    }

    template<typename nvme_sched_t, typename key_range_t>
    inline auto
    lookup(lookup_scheduler<nvme_sched_t>* tree_sched,
           nvme_sched_t* nvme,
           key_range_t&& keys,
           const core::tree_manifest* manifest) {

        return with_context(
            make_lookup_state(std::forward<key_range_t>(keys), manifest))(
            [tree_sched, nvme]() {
                return get_context<lookup_state>()
                    >> flat_map([tree_sched, nvme](lookup_state& state) {
                        return just()
                            >> for_each(pump::coro::make_view_able(check_not_done(state)))
                            >> flat_map([tree_sched, &state](bool) {
                                return tree_sched->process(state);
                            })
                            >> visit()
                            >> flat_map([&state, nvme]<typename D>(D&&) {
                                if constexpr (std::is_same_v<D, decision_need_read>) {
                                    return just() >> mock_nvme::read_batch(std::move(state.read_descs), nvme);
                                } else {
                                    return just(true);
                                }
                            })
                            >> all()
                            >> then([&state](bool) {
                                std::vector<lookup_result> results;
                                results.reserve(state.entries.size());
                                for (auto& e : state.entries)
                                    results.push_back(std::move(e.result));
                                return results;
                            });
                    });
            }
        );
    }

}

#endif //APPS_INCONEL_TREE_SENDER_HH
