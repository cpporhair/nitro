#ifndef APPS_INCONEL_CORE_READ_CATALOG_HH
#define APPS_INCONEL_CORE_READ_CATALOG_HH

// Read-side catalog carrier for M02. This is a CPU-only helper for the
// CAT/PRS/read_handle pin chain; it is not coord_state and does not
// implement publish/release/gate logic.

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "./checkpoint_guard.hh"
#include "./memtable.hh"

namespace apps::inconel::core {

    struct published_read_set {
        std::shared_ptr<checkpoint_guard>                  tree_guard;
        std::shared_ptr<const std::vector<front_read_set>> fronts;
        uint64_t                                           epoch = 0;
    };

    struct publish_catalog {
        std::shared_ptr<const published_read_set> prs;
        mutable std::atomic<uint64_t> durable_lsn;
        uint64_t epoch = 0;

        publish_catalog(std::shared_ptr<const published_read_set> prs_in,
                        uint64_t durable_lsn_in,
                        uint64_t epoch_in)
            : prs(std::move(prs_in))
            , durable_lsn(durable_lsn_in)
            , epoch(epoch_in) {
            if (!prs) {
                throw std::invalid_argument("publish_catalog: prs must not be null");
            }
            if (!prs->tree_guard) {
                throw std::invalid_argument(
                    "publish_catalog: tree_guard must not be null");
            }
            if (!prs->tree_guard->manifest) {
                throw std::invalid_argument(
                    "publish_catalog: tree_guard manifest must not be null");
            }
            if (!prs->fronts) {
                throw std::invalid_argument("publish_catalog: fronts must not be null");
            }
            if (prs->fronts->empty()) {
                throw std::invalid_argument("publish_catalog: fronts must not be empty");
            }
            if (prs->epoch != epoch) {
                throw std::invalid_argument("publish_catalog: epoch must match prs");
            }
        }
    };

    struct read_handle {
        std::shared_ptr<const publish_catalog> cat;
        uint64_t read_lsn = 0;
    };

    class catalog_store {
      public:
        explicit catalog_store(std::shared_ptr<const publish_catalog> initial_cat)
            : cat_(checked_cat(std::move(initial_cat))) {}

        [[nodiscard]] read_handle
        acquire_read_handle() const noexcept {
            auto cat = cat_.load(std::memory_order_acquire);
            auto lsn = cat->durable_lsn.load(std::memory_order_acquire);
            return read_handle{.cat = std::move(cat), .read_lsn = lsn};
        }

        void
        install_cat(std::shared_ptr<const publish_catalog> new_cat) {
            cat_.store(checked_cat(std::move(new_cat)), std::memory_order_release);
        }

        [[nodiscard]] std::shared_ptr<const publish_catalog>
        current_cat() const noexcept {
            return cat_.load(std::memory_order_acquire);
        }

      private:
        static std::shared_ptr<const publish_catalog>
        checked_cat(std::shared_ptr<const publish_catalog> cat) {
            if (!cat) {
                throw std::invalid_argument("catalog_store: CAT must not be null");
            }
            return cat;
        }

        std::atomic<std::shared_ptr<const publish_catalog>> cat_;
    };

}  // namespace apps::inconel::core

#endif  // APPS_INCONEL_CORE_READ_CATALOG_HH
