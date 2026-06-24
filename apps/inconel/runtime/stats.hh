#ifndef APPS_INCONEL_RUNTIME_STATS_HH
#define APPS_INCONEL_RUNTIME_STATS_HH

#include <algorithm>

#include "./maintenance_scheduler.hh"

namespace apps::inconel::runtime {

    template <typename Runtime>
    [[nodiscard]] inline maintenance_stats_snapshot
    collect_maintenance_stats(Runtime* rt) {
        maintenance_stats_snapshot out{};
        for (auto* sched : rt->template get_schedulers<maintenance_sched>()) {
            if (sched == nullptr) {
                continue;
            }
            const auto snap = sched->snapshot();
            out.enabled = out.enabled || snap.enabled;
            out.stopping = out.stopping || snap.stopping;
            out.inflight = out.inflight || snap.inflight;
            out.cooldown_ticks =
                std::max(out.cooldown_ticks, snap.cooldown_ticks);
            out.idle_backoff_ticks =
                std::max(out.idle_backoff_ticks, snap.idle_backoff_ticks);
            out.launched_rounds += snap.launched_rounds;
            out.completed_rounds += snap.completed_rounds;
            out.failed_rounds += snap.failed_rounds;
            out.work_rounds += snap.work_rounds;
            out.noop_rounds += snap.noop_rounds;
            out.seal_rounds += snap.seal_rounds;
            out.flush_rounds += snap.flush_rounds;
            out.non_noop_flush_rounds += snap.non_noop_flush_rounds;
        }
        return out;
    }

}  // namespace apps::inconel::runtime

#endif  // APPS_INCONEL_RUNTIME_STATS_HH
