#ifndef APPS_INCONEL_TEST_WAL_TEST_SUPPORT_HH
#define APPS_INCONEL_TEST_WAL_TEST_SUPPORT_HH

#include "apps/inconel/core/wal_reclaim_frontier.hh"
#include "apps/inconel/wal/scheduler.hh"

#define INCONEL_TEST_WAL_SPACE_SCHED(name, geometry, ...)                    \
  apps::inconel::core::wal_reclaim_frontier name##_wal_reclaim_frontier;     \
  apps::inconel::wal::wal_space_sched name(                                  \
      (geometry), &name##_wal_reclaim_frontier __VA_OPT__(, ) __VA_ARGS__)

#endif  // APPS_INCONEL_TEST_WAL_TEST_SUPPORT_HH
