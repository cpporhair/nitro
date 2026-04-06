#ifndef APPS_INCONEL_TREE_SENDER_HH
#define APPS_INCONEL_TREE_SENDER_HH

#include "pump/core/meta.hh"
#include "pump/coro/coro.hh"
#include "pump/sender/generate.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/then.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"

#include "./scheduler.hh"
#include "../mock_nvme/sender.hh"

namespace apps::inconel::tree {

    // ── Generator: yields while !state.all_done ──

    inline pump::coro::return_yields<bool>
    check_not_done(const lookup_state& state) {
        while (!state.all_done)
            co_yield true;
        co_return false;
    }

    // ── Lookup pipeline ──
    //
    //   with_context(state)(
    //     get_context >> then(
    //       for_each(check_not_done)
    //         >> flat_map(tree_sched.process)    → scheduler pushes keys, prepares read_descs
    //         >> then(read_batch)                → batch NVMe read (empty = no-op when done)
    //         >> flat()
    //         >> all()
    //         >> then(collect results)
    //     ) >> flat()
    //   )

    template<typename nvme_sched_t, typename key_range_t>
    inline auto
    lookup(lookup_scheduler<nvme_sched_t>* tree_sched,
           nvme_sched_t* nvme,
           key_range_t&& keys,
           const core::tree_manifest* manifest) {

        return pump::sender::with_context(
            make_lookup_state(std::forward<key_range_t>(keys), manifest))(
            [tree_sched, nvme]() {
                return pump::sender::get_context<lookup_state>()
                    >> pump::sender::then([tree_sched, nvme](lookup_state& state) {
                        return pump::sender::just()
                            >> pump::sender::for_each(
                                pump::coro::make_view_able(check_not_done(state)))
                            >> pump::sender::flat_map([tree_sched, &state](bool) {
                                return tree_sched->process(state);
                            })
                            >> pump::sender::then([&state, nvme](bool) {
                                return pump::sender::just()
                                    >> mock_nvme::read_batch(
                                        std::move(state.read_descs), nvme);
                            })
                            >> pump::sender::flat()
                            >> pump::sender::all()
                            >> pump::sender::then([&state](bool) {
                                std::vector<lookup_result> results;
                                results.reserve(state.entries.size());
                                for (auto& e : state.entries)
                                    results.push_back(std::move(e.result));
                                return results;
                            });
                    })
                    >> pump::sender::flat();
            }
        );
    }

}

#endif //APPS_INCONEL_TREE_SENDER_HH
