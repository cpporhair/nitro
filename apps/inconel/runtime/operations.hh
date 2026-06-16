#ifndef APPS_INCONEL_RUNTIME_OPERATIONS_HH
#define APPS_INCONEL_RUNTIME_OPERATIONS_HH

#include <string_view>
#include <utility>

#include "../core/batch_carrier.hh"
#include "../core/registry.hh"
#include "../pipeline/flush_round.hh"
#include "../pipeline/point_get.hh"
#include "../pipeline/seal_round.hh"
#include "../value/sender.hh"
#include "../write_path/write_batch.hh"
#include "./facade.hh"

namespace apps::inconel::rt {

    template <typename NvmeProvider = value::local_nvme_provider>
    [[nodiscard]] inline auto
    write_batch(core::client_batch_buffer&& input,
                NvmeProvider value_nvme = {}) {
        return write_path::write_batch(
            *core::registry::coord_sched_singleton(),
            core::registry::fronts_span(),
            *core::registry::wal_space_singleton(),
            core::registry::nvme_by_front_owner_span(),
            std::move(input),
            value_nvme);
    }

    template <typename NvmeProvider = value::local_nvme_provider>
    [[nodiscard]] inline auto
    point_get(std::string_view key, NvmeProvider value_nvme = {}) {
        return pipeline::point_get(
            *core::registry::coord_sched_singleton(),
            core::registry::fronts_span(),
            key,
            value_nvme);
    }

    [[nodiscard]] inline auto
    seal_once() {
        return pipeline::seal_round(
            *core::registry::coord_sched_singleton(),
            core::registry::fronts_span());
    }

    [[nodiscard]] inline auto
    flush_once() {
        return pipeline::flush_round_once(
            *core::registry::coord_sched_singleton(),
            core::registry::fronts_span(),
            *core::registry::tree_sched_singleton());
    }

}  // namespace apps::inconel::rt

#endif  // APPS_INCONEL_RUNTIME_OPERATIONS_HH
