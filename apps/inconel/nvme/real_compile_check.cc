#include "../core/clock_cache.hh"
#include "../runtime/builder.hh"
#include "./runtime_scheduler.hh"

namespace apps::inconel::nvme {

    void
    instantiate_real_runtime_for_compile_check(runtime_device* dev,
                                               std::span<const uint32_t> cores) {
        runtime::build_options opts{
            .cores = cores,
            .device = dev,
        };
        auto* rt = runtime::build_runtime<core::segmented_clock_cache,
                                          core::segmented_clock_cache>(opts);
        runtime::destroy_runtime<core::segmented_clock_cache,
                                 core::segmented_clock_cache>(rt);
    }

}  // namespace apps::inconel::nvme
