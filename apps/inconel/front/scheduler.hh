#ifndef APPS_INCONEL_FRONT_SCHEDULER_HH
#define APPS_INCONEL_FRONT_SCHEDULER_HH

// Front scheduler M05: owner-local active/immutable memtable state plus
// PUMP sender surface for memtable insert, snapshot lookup/scan, seal,
// collect, and release. This file intentionally does not implement WAL
// append, value persistence/read, write_batch pipeline, tree lookup,
// runtime API, seal-round orchestration, frontier switch, or recovery.

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "pump/core/compute_sender_type.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"

#include "../core/batch_carrier.hh"
#include "../core/memtable.hh"
#include "../core/memtable_lookup.hh"

namespace apps::inconel::front {

    struct batch_lookup_item {
        std::string_view              key;
        core::memtable_lookup_result result;
    };

    using batch_lookup_result = std::vector<batch_lookup_item>;

    [[nodiscard]] inline uint64_t
    make_front_gen_id(uint32_t owner_id,
                      uint32_t front_count,
                      uint64_t local_epoch) {
        if (front_count == 0) {
            throw std::invalid_argument(
                "front::make_front_gen_id: front_count must be nonzero");
        }
        if (owner_id >= front_count) {
            throw std::invalid_argument(
                "front::make_front_gen_id: owner_id out of range");
        }

        const uint64_t owner_term = static_cast<uint64_t>(owner_id) + 1;
        if (local_epoch >
            (std::numeric_limits<uint64_t>::max() - owner_term) /
                static_cast<uint64_t>(front_count)) {
            throw std::overflow_error("front::make_front_gen_id: gen_id overflow");
        }
        return local_epoch * static_cast<uint64_t>(front_count) + owner_term;
    }

    [[nodiscard]] inline uint64_t
    front_local_epoch_from_gen_id(uint32_t owner_id,
                                  uint32_t front_count,
                                  uint64_t gen_id) {
        if (front_count == 0) {
            throw std::invalid_argument(
                "front::front_local_epoch_from_gen_id: front_count must be nonzero");
        }
        if (owner_id >= front_count) {
            throw std::invalid_argument(
                "front::front_local_epoch_from_gen_id: owner_id out of range");
        }
        const uint64_t owner_term = static_cast<uint64_t>(owner_id) + 1;
        if (gen_id < owner_term) {
            throw std::invalid_argument(
                "front::front_local_epoch_from_gen_id: gen_id below owner stride");
        }
        const uint64_t delta = gen_id - owner_term;
        if (delta % static_cast<uint64_t>(front_count) != 0) {
            throw std::invalid_argument(
                "front::front_local_epoch_from_gen_id: gen_id is not on owner stride");
        }
        return delta / static_cast<uint64_t>(front_count);
    }

    [[nodiscard]] inline std::shared_ptr<core::memtable_gen>
    make_front_memtable_gen(uint32_t owner_id,
                            uint32_t front_count,
                            uint64_t local_epoch,
                            core::memtable_gen::state st) {
        auto gen = std::make_shared<core::memtable_gen>();
        gen->gen_id = make_front_gen_id(owner_id, front_count, local_epoch);
        gen->st = st;
        gen->front_owner_index = owner_id;
        gen->min_lsn = std::numeric_limits<uint64_t>::max();
        gen->max_lsn = 0;
        return gen;
    }

    class front_sched;

    namespace _front_insert { struct req; struct sender; }
    namespace _front_lookup { struct req; struct sender; }
    namespace _front_batch_lookup { struct req; struct sender; }
    namespace _front_scan { struct req; struct sender; }
    namespace _front_seal { struct req; struct sender; }
    namespace _front_collect { struct req; struct sender; }
    namespace _front_release { struct req; struct sender; }

    class front_sched {
      public:
        front_sched(uint32_t owner_id,
                    uint32_t front_count,
                    std::size_t queue_depth = 1024)
            : front_sched(owner_id,
                          front_count,
                          make_front_memtable_gen(
                              owner_id,
                              front_count,
                              0,
                              core::memtable_gen::state::active),
                          1,
                          queue_depth) {}

        front_sched(uint32_t owner_id,
                    uint32_t front_count,
                    std::shared_ptr<core::memtable_gen> initial_active,
                    uint64_t next_local_gen_epoch,
                    std::size_t queue_depth = 1024)
            : insert_q_(queue_depth)
            , lookup_q_(queue_depth)
            , batch_lookup_q_(queue_depth)
            , scan_q_(queue_depth)
            , seal_q_(queue_depth)
            , collect_q_(queue_depth)
            , release_q_(queue_depth)
            , owner_id_(owner_id)
            , front_count_(front_count)
            , next_local_gen_epoch_(next_local_gen_epoch)
            , active_(std::move(initial_active)) {
            validate_constructor_state(queue_depth);
        }

        ~front_sched();

        front_sched(const front_sched&) = delete;
        front_sched& operator=(const front_sched&) = delete;
        front_sched(front_sched&&) = delete;
        front_sched& operator=(front_sched&&) = delete;

        [[nodiscard]] uint32_t
        owner_id() const noexcept {
            return owner_id_;
        }

        [[nodiscard]] uint32_t
        front_count() const noexcept {
            return front_count_;
        }

        [[nodiscard]] uint64_t
        next_local_gen_epoch_for_testing() const noexcept {
            return next_local_gen_epoch_;
        }

        [[nodiscard]] const std::shared_ptr<core::memtable_gen>&
        active_for_testing() const noexcept {
            return active_;
        }

        [[nodiscard]] const std::vector<std::shared_ptr<core::memtable_gen>>&
        imms_for_testing() const noexcept {
            return imms_;
        }

        [[nodiscard]] _front_insert::sender
        insert_memtable_entries(
            core::front_fragment fragment,
            std::span<const core::canonical_entry> canonical_entries);

        [[nodiscard]] _front_lookup::sender
        lookup_memtable(std::string_view key,
                        uint64_t read_lsn,
                        core::front_read_set frs);

        [[nodiscard]] _front_batch_lookup::sender
        batch_lookup(std::span<const std::string_view> keys,
                     uint64_t read_lsn,
                     core::front_read_set frs);

        [[nodiscard]] _front_scan::sender
        scan_memtable(std::string_view begin,
                      std::string_view end,
                      uint64_t read_lsn,
                      core::front_read_set frs);

        [[nodiscard]] _front_seal::sender
        seal_active();

        [[nodiscard]] _front_collect::sender
        collect_eligible_gens(uint64_t durable_lsn);

        [[nodiscard]] _front_release::sender
        release_gens(std::vector<uint64_t> gen_ids);

        void schedule_insert(_front_insert::req* r);
        void schedule_lookup(_front_lookup::req* r);
        void schedule_batch_lookup(_front_batch_lookup::req* r);
        void schedule_scan(_front_scan::req* r);
        void schedule_seal(_front_seal::req* r);
        void schedule_collect(_front_collect::req* r);
        void schedule_release(_front_release::req* r);

        bool advance();

        template<typename runtime_t>
        bool advance(runtime_t&) { return advance(); }

        void insert_memtable_entries_for_testing(
            core::front_fragment fragment,
            std::span<const core::canonical_entry> canonical_entries) {
            insert_memtable_entries_now(fragment, canonical_entries);
        }

        [[nodiscard]] core::memtable_lookup_result
        lookup_memtable_for_testing(std::string_view key,
                                    uint64_t read_lsn,
                                    core::front_read_set frs) const {
            return lookup_memtable_now(key, read_lsn, frs);
        }

        [[nodiscard]] batch_lookup_result
        batch_lookup_for_testing(std::span<const std::string_view> keys,
                                 uint64_t read_lsn,
                                 core::front_read_set frs) const {
            return batch_lookup_now(keys, read_lsn, frs);
        }

        [[nodiscard]] core::memtable_scan_result
        scan_memtable_for_testing(std::string_view begin,
                                  std::string_view end,
                                  uint64_t read_lsn,
                                  core::front_read_set frs) const {
            return scan_memtable_now(begin, end, read_lsn, frs);
        }

        [[nodiscard]] core::front_read_set
        seal_active_for_testing() {
            return seal_active_now();
        }

        [[nodiscard]] std::vector<std::shared_ptr<core::memtable_gen>>
        collect_eligible_gens_for_testing(uint64_t durable_lsn) const {
            return collect_eligible_gens_now(durable_lsn);
        }

        void release_gens_for_testing(std::vector<uint64_t> gen_ids) {
            release_gens_now(gen_ids);
        }

      private:
        static constexpr uint32_t kMaxInsertPerAdvance = 64;
        static constexpr uint32_t kMaxLookupPerAdvance = 128;
        static constexpr uint32_t kMaxBatchLookupPerAdvance = 64;
        static constexpr uint32_t kMaxScanPerAdvance = 32;
        static constexpr uint32_t kMaxSealPerAdvance = 16;
        static constexpr uint32_t kMaxCollectPerAdvance = 64;
        static constexpr uint32_t kMaxReleasePerAdvance = 64;

        void validate_constructor_state(std::size_t queue_depth) const {
            if (front_count_ == 0) {
                throw std::invalid_argument(
                    "front::front_sched: front_count must be nonzero");
            }
            if (owner_id_ >= front_count_) {
                throw std::invalid_argument(
                    "front::front_sched: owner_id out of range");
            }
            if (queue_depth == 0) {
                throw std::invalid_argument(
                    "front::front_sched: queue_depth must be nonzero");
            }
            if (!active_) {
                throw std::invalid_argument(
                    "front::front_sched: initial active must not be null");
            }
            validate_active_gen(*active_, "initial active");
            const uint64_t active_epoch =
                front_local_epoch_from_gen_id(
                    owner_id_, front_count_, active_->gen_id);
            if (next_local_gen_epoch_ <= active_epoch) {
                throw std::invalid_argument(
                    "front::front_sched: next local gen epoch does not "
                    "advance beyond initial active");
            }
        }

        void validate_active_gen(const core::memtable_gen& gen,
                                 const char* what) const {
            if (gen.st != core::memtable_gen::state::active) {
                throw std::logic_error(
                    std::string("front::front_sched: ") + what +
                    " gen is not active");
            }
            validate_live_gen_owner(gen, what);
        }

        void validate_sealed_gen(const core::memtable_gen& gen,
                                 const char* what) const {
            if (gen.st != core::memtable_gen::state::sealed) {
                throw std::logic_error(
                    std::string("front::front_sched: ") + what +
                    " gen is not sealed");
            }
            validate_live_gen_owner(gen, what);
        }

        void validate_live_gen_owner(const core::memtable_gen& gen,
                                     const char* what) const {
            if (gen.front_owner_index != owner_id_) {
                throw std::logic_error(
                    std::string("front::front_sched: ") + what +
                    " gen owner mismatch");
            }
            (void)front_local_epoch_from_gen_id(
                owner_id_, front_count_, gen.gen_id);
        }

        void validate_active_present() const {
            if (!active_) {
                throw std::logic_error(
                    "front::front_sched: active gen is null");
            }
            validate_active_gen(*active_, "active");
        }

        void validate_snapshot_owner(const core::front_read_set& frs) const {
            if (frs.active) {
                validate_live_gen_owner(*frs.active, "snapshot active");
            }
            for (const auto& gen : frs.imms) {
                if (gen) {
                    validate_live_gen_owner(*gen, "snapshot imm");
                }
            }
        }

        void validate_current_imms() const {
            for (const auto& gen : imms_) {
                if (!gen) {
                    throw std::logic_error(
                        "front::front_sched: current imm gen is null");
                }
                validate_sealed_gen(*gen, "current imm");
            }
        }

        void validate_insert_request(
            const core::front_fragment& fragment,
            std::span<const core::canonical_entry> canonical_entries) const {
            if (fragment.owner != owner_id_) {
                throw std::invalid_argument(
                    "front::front_sched: fragment owner mismatch");
            }
            if (fragment.entry_count != canonical_entries.size()) {
                throw std::invalid_argument(
                    "front::front_sched: fragment entry_count mismatch");
            }
            for (uint32_t idx : fragment.entry_indices) {
                if (idx >= canonical_entries.size()) {
                    throw std::out_of_range(
                        "front::front_sched: fragment entry index out of range");
                }
            }
            validate_active_present();
        }

        void insert_memtable_entries_now(
            const core::front_fragment& fragment,
            std::span<const core::canonical_entry> canonical_entries) {
            validate_insert_request(fragment, canonical_entries);
            apply_insert_validated(fragment, canonical_entries);
        }

        void apply_insert_validated(
            const core::front_fragment& fragment,
            std::span<const core::canonical_entry> canonical_entries) {
            for (uint32_t idx : fragment.entry_indices) {
                const auto& entry = canonical_entries[idx];
                switch (entry.op) {
                case core::write_op_type::put:
                    core::insert_value(
                        *active_, entry.key, fragment.batch_lsn, entry.allocated_vr);
                    break;
                case core::write_op_type::del:
                    core::insert_tombstone(
                        *active_, entry.key, fragment.batch_lsn);
                    break;
                default:
                    throw std::logic_error(
                        "front::front_sched: impossible canonical op");
                }
            }
        }

        [[nodiscard]] core::memtable_lookup_result
        lookup_memtable_now(std::string_view key,
                            uint64_t read_lsn,
                            const core::front_read_set& frs) const {
            validate_snapshot_owner(frs);
            return core::lookup_memtable(key, read_lsn, frs);
        }

        [[nodiscard]] batch_lookup_result
        batch_lookup_now(std::span<const std::string_view> keys,
                         uint64_t read_lsn,
                         const core::front_read_set& frs) const {
            validate_snapshot_owner(frs);
            batch_lookup_result out;
            out.reserve(keys.size());
            for (std::string_view key : keys) {
                out.push_back(batch_lookup_item{
                    .key = key,
                    .result = core::lookup_memtable(key, read_lsn, frs),
                });
            }
            return out;
        }

        [[nodiscard]] core::memtable_scan_result
        scan_memtable_now(std::string_view begin,
                          std::string_view end,
                          uint64_t read_lsn,
                          const core::front_read_set& frs) const {
            validate_snapshot_owner(frs);
            return core::scan_memtable(begin, end, read_lsn, frs);
        }

        [[nodiscard]] core::front_read_set seal_active_now() {
            validate_active_present();

            if (imms_.size() == imms_.max_size()) {
                throw std::length_error(
                    "front::front_sched: immutable gen vector full");
            }
            imms_.reserve(imms_.size() + 1);

            auto old = active_;
            auto next = make_front_memtable_gen(
                owner_id_,
                front_count_,
                next_local_gen_epoch_,
                core::memtable_gen::state::active);

            imms_.insert(imms_.begin(), old);
            old->st = core::memtable_gen::state::sealed;
            active_ = std::move(next);
            ++next_local_gen_epoch_;

            return core::front_read_set{
                .active = active_,
                .imms = imms_,
            };
        }

        [[nodiscard]] std::vector<std::shared_ptr<core::memtable_gen>>
        collect_eligible_gens_now(uint64_t durable_lsn) const {
            validate_current_imms();
            std::vector<std::shared_ptr<core::memtable_gen>> out;
            for (const auto& gen : imms_) {
                if (gen->max_lsn <= durable_lsn) {
                    out.push_back(gen);
                }
            }
            return out;
        }

        void release_gens_now(const std::vector<uint64_t>& gen_ids) {
            validate_current_imms();
            imms_.erase(
                std::remove_if(
                    imms_.begin(),
                    imms_.end(),
                    [&](const std::shared_ptr<core::memtable_gen>& gen) {
                        return std::find(gen_ids.begin(),
                                         gen_ids.end(),
                                         gen->gen_id) != gen_ids.end();
                    }),
                imms_.end());
        }

        void handle_insert(_front_insert::req* r);
        void handle_lookup(_front_lookup::req* r);
        void handle_batch_lookup(_front_batch_lookup::req* r);
        void handle_scan(_front_scan::req* r);
        void handle_seal(_front_seal::req* r);
        void handle_collect(_front_collect::req* r);
        void handle_release(_front_release::req* r);

        pump::core::per_core::queue<_front_insert::req*> insert_q_;
        pump::core::per_core::queue<_front_lookup::req*> lookup_q_;
        pump::core::per_core::queue<_front_batch_lookup::req*> batch_lookup_q_;
        pump::core::per_core::queue<_front_scan::req*> scan_q_;
        pump::core::per_core::queue<_front_seal::req*> seal_q_;
        pump::core::per_core::queue<_front_collect::req*> collect_q_;
        pump::core::per_core::queue<_front_release::req*> release_q_;

        uint32_t owner_id_;
        uint32_t front_count_;
        uint64_t next_local_gen_epoch_;
        std::shared_ptr<core::memtable_gen> active_;
        std::vector<std::shared_ptr<core::memtable_gen>> imms_;
    };

    namespace _front_insert {
        struct req {
            core::front_fragment fragment;
            std::span<const core::canonical_entry> canonical_entries;
            std::move_only_function<void()> cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool front_insert_op = true;
            front_sched* sched = nullptr;
            core::front_fragment fragment;
            std::span<const core::canonical_entry> canonical_entries;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            core::front_fragment fragment;
            std::span<const core::canonical_entry> canonical_entries;

            sender(front_sched* s,
                   core::front_fragment f,
                   std::span<const core::canonical_entry> entries)
                : sched(s)
                , fragment(std::move(f))
                , canonical_entries(entries) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{
                    .sched = sched,
                    .fragment = std::move(fragment),
                    .canonical_entries = canonical_entries,
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_insert

    namespace _front_lookup {
        struct req {
            std::string_view key;
            uint64_t read_lsn = 0;
            core::front_read_set frs;
            std::move_only_function<void(core::memtable_lookup_result&&)> cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool front_lookup_op = true;
            front_sched* sched = nullptr;
            std::string_view key;
            uint64_t read_lsn = 0;
            core::front_read_set frs;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            std::string_view key;
            uint64_t read_lsn = 0;
            core::front_read_set frs;

            sender(front_sched* s,
                   std::string_view k,
                   uint64_t lsn,
                   core::front_read_set snapshot)
                : sched(s), key(k), read_lsn(lsn), frs(std::move(snapshot)) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{
                    .sched = sched,
                    .key = key,
                    .read_lsn = read_lsn,
                    .frs = std::move(frs),
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_lookup

    namespace _front_batch_lookup {
        struct req {
            std::span<const std::string_view> keys;
            uint64_t read_lsn = 0;
            core::front_read_set frs;
            std::move_only_function<void(batch_lookup_result&&)> cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool front_batch_lookup_op = true;
            front_sched* sched = nullptr;
            std::span<const std::string_view> keys;
            uint64_t read_lsn = 0;
            core::front_read_set frs;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            std::span<const std::string_view> keys;
            uint64_t read_lsn = 0;
            core::front_read_set frs;

            sender(front_sched* s,
                   std::span<const std::string_view> ks,
                   uint64_t lsn,
                   core::front_read_set snapshot)
                : sched(s), keys(ks), read_lsn(lsn), frs(std::move(snapshot)) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{
                    .sched = sched,
                    .keys = keys,
                    .read_lsn = read_lsn,
                    .frs = std::move(frs),
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_batch_lookup

    namespace _front_scan {
        struct req {
            std::string_view begin;
            std::string_view end;
            uint64_t read_lsn = 0;
            core::front_read_set frs;
            std::move_only_function<void(core::memtable_scan_result&&)> cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool front_scan_op = true;
            front_sched* sched = nullptr;
            std::string_view begin;
            std::string_view end;
            uint64_t read_lsn = 0;
            core::front_read_set frs;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            std::string_view begin;
            std::string_view end;
            uint64_t read_lsn = 0;
            core::front_read_set frs;

            sender(front_sched* s,
                   std::string_view b,
                   std::string_view e,
                   uint64_t lsn,
                   core::front_read_set snapshot)
                : sched(s), begin(b), end(e), read_lsn(lsn), frs(std::move(snapshot)) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{
                    .sched = sched,
                    .begin = begin,
                    .end = end,
                    .read_lsn = read_lsn,
                    .frs = std::move(frs),
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_scan

    namespace _front_seal {
        struct req {
            std::move_only_function<void(core::front_read_set&&)> cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool front_seal_op = true;
            front_sched* sched = nullptr;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;

            auto make_op() { return op{.sched = sched}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_seal

    namespace _front_collect {
        struct req {
            uint64_t durable_lsn = 0;
            std::move_only_function<void(
                std::vector<std::shared_ptr<core::memtable_gen>>&&)> cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool front_collect_op = true;
            front_sched* sched = nullptr;
            uint64_t durable_lsn = 0;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            uint64_t durable_lsn = 0;

            auto make_op() {
                return op{.sched = sched, .durable_lsn = durable_lsn};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_collect

    namespace _front_release {
        struct req {
            std::vector<uint64_t> gen_ids;
            std::move_only_function<void()> cb;
            std::move_only_function<void(std::exception_ptr)> fail;
        };

        struct op {
            constexpr static bool front_release_op = true;
            front_sched* sched = nullptr;
            std::vector<uint64_t> gen_ids;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            std::vector<uint64_t> gen_ids;

            sender(front_sched* s, std::vector<uint64_t> ids)
                : sched(s), gen_ids(std::move(ids)) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{.sched = sched, .gen_ids = std::move(gen_ids)};
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_release

    inline front_sched::~front_sched() {
        while (auto item = insert_q_.try_dequeue()) delete *item;
        while (auto item = lookup_q_.try_dequeue()) delete *item;
        while (auto item = batch_lookup_q_.try_dequeue()) delete *item;
        while (auto item = scan_q_.try_dequeue()) delete *item;
        while (auto item = seal_q_.try_dequeue()) delete *item;
        while (auto item = collect_q_.try_dequeue()) delete *item;
        while (auto item = release_q_.try_dequeue()) delete *item;
    }

    inline _front_insert::sender
    front_sched::insert_memtable_entries(
        core::front_fragment fragment,
        std::span<const core::canonical_entry> canonical_entries) {
        return _front_insert::sender{
            this, std::move(fragment), canonical_entries};
    }

    inline _front_lookup::sender
    front_sched::lookup_memtable(std::string_view key,
                                 uint64_t read_lsn,
                                 core::front_read_set frs) {
        return _front_lookup::sender{
            this, key, read_lsn, std::move(frs)};
    }

    inline _front_batch_lookup::sender
    front_sched::batch_lookup(std::span<const std::string_view> keys,
                              uint64_t read_lsn,
                              core::front_read_set frs) {
        return _front_batch_lookup::sender{
            this, keys, read_lsn, std::move(frs)};
    }

    inline _front_scan::sender
    front_sched::scan_memtable(std::string_view begin,
                               std::string_view end,
                               uint64_t read_lsn,
                               core::front_read_set frs) {
        return _front_scan::sender{
            this, begin, end, read_lsn, std::move(frs)};
    }

    inline _front_seal::sender
    front_sched::seal_active() {
        return _front_seal::sender{.sched = this};
    }

    inline _front_collect::sender
    front_sched::collect_eligible_gens(uint64_t durable_lsn) {
        return _front_collect::sender{
            .sched = this,
            .durable_lsn = durable_lsn,
        };
    }

    inline _front_release::sender
    front_sched::release_gens(std::vector<uint64_t> gen_ids) {
        return _front_release::sender{this, std::move(gen_ids)};
    }

    inline void
    front_sched::schedule_insert(_front_insert::req* r) {
        if (!insert_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("front::front_sched: insert queue full");
        }
    }

    inline void
    front_sched::schedule_lookup(_front_lookup::req* r) {
        if (!lookup_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("front::front_sched: lookup queue full");
        }
    }

    inline void
    front_sched::schedule_batch_lookup(_front_batch_lookup::req* r) {
        if (!batch_lookup_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error(
                "front::front_sched: batch lookup queue full");
        }
    }

    inline void
    front_sched::schedule_scan(_front_scan::req* r) {
        if (!scan_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("front::front_sched: scan queue full");
        }
    }

    inline void
    front_sched::schedule_seal(_front_seal::req* r) {
        if (!seal_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("front::front_sched: seal queue full");
        }
    }

    inline void
    front_sched::schedule_collect(_front_collect::req* r) {
        if (!collect_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("front::front_sched: collect queue full");
        }
    }

    inline void
    front_sched::schedule_release(_front_release::req* r) {
        if (!release_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error("front::front_sched: release queue full");
        }
    }

    inline void
    front_sched::handle_insert(_front_insert::req* r) {
        std::unique_ptr<_front_insert::req> req(r);
        try {
            validate_insert_request(req->fragment, req->canonical_entries);
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        apply_insert_validated(req->fragment, req->canonical_entries);

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb();
        }
    }

    inline void
    front_sched::handle_lookup(_front_lookup::req* r) {
        std::unique_ptr<_front_lookup::req> req(r);
        core::memtable_lookup_result result;
        try {
            result = lookup_memtable_now(req->key, req->read_lsn, req->frs);
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(std::move(result));
        }
    }

    inline void
    front_sched::handle_batch_lookup(_front_batch_lookup::req* r) {
        std::unique_ptr<_front_batch_lookup::req> req(r);
        batch_lookup_result result;
        try {
            result = batch_lookup_now(req->keys, req->read_lsn, req->frs);
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(std::move(result));
        }
    }

    inline void
    front_sched::handle_scan(_front_scan::req* r) {
        std::unique_ptr<_front_scan::req> req(r);
        core::memtable_scan_result result;
        try {
            result = scan_memtable_now(
                req->begin, req->end, req->read_lsn, req->frs);
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        auto cb = std::move(req->cb);
        if (cb) {
            cb(std::move(result));
        }
        req.reset();
    }

    inline void
    front_sched::handle_seal(_front_seal::req* r) {
        std::unique_ptr<_front_seal::req> req(r);
        core::front_read_set result;
        try {
            result = seal_active_now();
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(std::move(result));
        }
    }

    inline void
    front_sched::handle_collect(_front_collect::req* r) {
        std::unique_ptr<_front_collect::req> req(r);
        std::vector<std::shared_ptr<core::memtable_gen>> result;
        try {
            result = collect_eligible_gens_now(req->durable_lsn);
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(std::move(result));
        }
    }

    inline void
    front_sched::handle_release(_front_release::req* r) {
        std::unique_ptr<_front_release::req> req(r);
        try {
            release_gens_now(req->gen_ids);
        } catch (...) {
            auto fail = std::move(req->fail);
            req.reset();
            if (fail) {
                fail(std::current_exception());
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb();
        }
    }

    inline bool
    front_sched::advance() {
        bool progress = false;

        for (uint32_t i = 0; i < kMaxInsertPerAdvance; ++i) {
            auto item = insert_q_.try_dequeue();
            if (!item) break;
            handle_insert(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxSealPerAdvance; ++i) {
            auto item = seal_q_.try_dequeue();
            if (!item) break;
            handle_seal(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxLookupPerAdvance; ++i) {
            auto item = lookup_q_.try_dequeue();
            if (!item) break;
            handle_lookup(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxBatchLookupPerAdvance; ++i) {
            auto item = batch_lookup_q_.try_dequeue();
            if (!item) break;
            handle_batch_lookup(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxScanPerAdvance; ++i) {
            auto item = scan_q_.try_dequeue();
            if (!item) break;
            handle_scan(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxCollectPerAdvance; ++i) {
            auto item = collect_q_.try_dequeue();
            if (!item) break;
            handle_collect(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxReleasePerAdvance; ++i) {
            auto item = release_q_.try_dequeue();
            if (!item) break;
            handle_release(*item);
            progress = true;
        }

        return progress;
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_insert::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_insert(new req{
            .fragment = std::move(fragment),
            .canonical_entries = canonical_entries,
            .cb = [ctx = ctx, scope = scope]() mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_lookup::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_lookup(new req{
            .key = key,
            .read_lsn = read_lsn,
            .frs = std::move(frs),
            .cb = [ctx = ctx, scope = scope](
                      core::memtable_lookup_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_batch_lookup::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_batch_lookup(new req{
            .keys = keys,
            .read_lsn = read_lsn,
            .frs = std::move(frs),
            .cb = [ctx = ctx, scope = scope](batch_lookup_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_scan::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_scan(new req{
            .begin = begin,
            .end = end,
            .read_lsn = read_lsn,
            .frs = std::move(frs),
            .cb = [ctx = ctx, scope = scope](
                      core::memtable_scan_result&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_seal::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_seal(new req{
            .cb = [ctx = ctx, scope = scope](
                      core::front_read_set&& r) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_collect::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_collect(new req{
            .durable_lsn = durable_lsn,
            .cb = [ctx = ctx, scope = scope](
                      std::vector<std::shared_ptr<core::memtable_gen>>&& r)
                      mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(
                    ctx, scope, std::move(r));
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_release::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_release(new req{
            .gen_ids = std::move(gen_ids),
            .cb = [ctx = ctx, scope = scope]() mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_value(ctx, scope);
            },
            .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
                pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                    ctx, scope, std::move(ep));
            },
        });
    }

}  // namespace apps::inconel::front

namespace pump::core {

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_insert_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_insert::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_lookup_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_lookup::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<
                apps::inconel::core::memtable_lookup_result>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_batch_lookup_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_batch_lookup::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<
                apps::inconel::front::batch_lookup_result>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_scan_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_scan::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<
                apps::inconel::core::memtable_scan_result>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_seal_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_seal::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<apps::inconel::core::front_read_set>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_collect_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_collect::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<
                std::vector<std::shared_ptr<
                    apps::inconel::core::memtable_gen>>>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_release_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_release::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_FRONT_SCHEDULER_HH
