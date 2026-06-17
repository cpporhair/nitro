#ifndef APPS_INCONEL_NVME_RUNTIME_SCHEDULER_HH
#define APPS_INCONEL_NVME_RUNTIME_SCHEDULER_HH

#include "./real_device.hh"
#include "./real_scheduler.hh"

namespace apps::inconel::nvme {
    using runtime_device = real_device;
    using runtime_scheduler = real_scheduler;
}

#endif  // APPS_INCONEL_NVME_RUNTIME_SCHEDULER_HH
