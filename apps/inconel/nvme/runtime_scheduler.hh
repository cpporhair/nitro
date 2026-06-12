#ifndef APPS_INCONEL_NVME_RUNTIME_SCHEDULER_HH
#define APPS_INCONEL_NVME_RUNTIME_SCHEDULER_HH

#ifdef INCONEL_NVME_MOCK_BACKEND
#include "../mock_nvme/device.hh"
#include "../mock_nvme/scheduler.hh"

#include "spdk/nvme.h"
#else
#include "./real_device.hh"
#include "./real_scheduler.hh"
#endif

namespace apps::inconel::nvme {
#ifdef INCONEL_NVME_MOCK_BACKEND
    constexpr uint32_t IO_FLAGS_FUA =
        SPDK_NVME_IO_FLAGS_FORCE_UNIT_ACCESS;

    using runtime_device = mock_nvme::mock_device;
    using runtime_scheduler = mock_nvme::mock_scheduler;
#else
    using runtime_device = real_device;
    using runtime_scheduler = real_scheduler;
#endif
}

#endif  // APPS_INCONEL_NVME_RUNTIME_SCHEDULER_HH
