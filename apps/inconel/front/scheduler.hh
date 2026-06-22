#ifndef APPS_INCONEL_FRONT_SCHEDULER_HH
#define APPS_INCONEL_FRONT_SCHEDULER_HH

// Front scheduler M05: owner-local active/immutable memtable state plus
// PUMP sender surface for memtable insert, snapshot lookup/scan, seal,
// collect, and release. This file intentionally does not implement WAL
// append, value persistence/read, write_batch pipeline, tree lookup,
// runtime API, seal-round orchestration, frontier switch, or recovery.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
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
#include "../core/owner_callback.hh"
#include "../format/wal.hh"
#include "../memory/dma_page_pool.hh"
#include "./wal_append.hh"

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
    namespace _front_wal_prepare { struct req; struct sender; }
    namespace _front_wal_install { struct req; struct sender; }
    namespace _front_wal_commit { struct req; struct sender; }
    namespace _front_wal_abort { struct req; struct sender; }

    struct borrowed_front_fragment {
        const core::front_fragment* ptr = nullptr;

        borrowed_front_fragment() = default;
        borrowed_front_fragment(const core::front_fragment* fragment) noexcept
            : ptr(fragment) {}
        borrowed_front_fragment(const core::front_fragment& fragment) noexcept
            : ptr(&fragment) {}

        [[nodiscard]] explicit operator bool() const noexcept {
            return ptr != nullptr;
        }

        [[nodiscard]] const core::front_fragment& get() const noexcept {
            return *ptr;
        }
    };

    // A follower prepare req merged into the current in-flight WAL entry group.
    // The front owner parks its prepare callback here until the leader commits
    // (deliver `wal_prepare_committed`) or aborts (deliver a WAL device failure).
    struct wal_group_parked_waiter {
        wal::wal_fragment_cursor cursor_after{};
        bool fragment_done = false;
        std::move_only_function<void(
            core::owner_outcome<wal::wal_prepare_result>&&)> cb;
    };

    // Logical waiters attached to the single in-flight physical plan `plan_id`.
    // Only set when at least one follower was coalesced; a lone-participant
    // plan leaves this empty and the leader drives completion directly.
    struct wal_group_plan_state {
        uint64_t plan_id = 0;
        std::vector<wal_group_parked_waiter> waiters;
    };

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
                    wal::segment_geometry wal_geometry,
                    wal::wal_append_config wal_config = {},
                    std::size_t queue_depth = 1024,
                    memory::dma_page_allocator wal_dma_alloc =
                        memory::make_heap_dma_page_allocator())
            : front_sched(owner_id, front_count, queue_depth) {
            configure_wal(std::move(wal_geometry), wal_config,
                          std::move(wal_dma_alloc));
        }

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
            , wal_prepare_q_(queue_depth)
            , wal_install_q_(queue_depth)
            , wal_commit_q_(queue_depth)
            , wal_abort_q_(queue_depth)
            , owner_id_(owner_id)
            , front_count_(front_count)
            , next_local_gen_epoch_(next_local_gen_epoch)
            , active_(std::move(initial_active))
            , queue_depth_(queue_depth)
            , wal_pending_prepare_capacity_(queue_depth) {
            validate_constructor_state(queue_depth);
            publish_memtable_pressure();
        }

        front_sched(uint32_t owner_id,
                    uint32_t front_count,
                    std::shared_ptr<core::memtable_gen> initial_active,
                    uint64_t next_local_gen_epoch,
                    wal::segment_geometry wal_geometry,
                    wal::wal_append_config wal_config = {},
                    std::size_t queue_depth = 1024,
                    memory::dma_page_allocator wal_dma_alloc =
                        memory::make_heap_dma_page_allocator())
            : front_sched(owner_id,
                          front_count,
                          std::move(initial_active),
                          next_local_gen_epoch,
                          queue_depth) {
            configure_wal(std::move(wal_geometry), wal_config,
                          std::move(wal_dma_alloc));
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

        [[nodiscard]] uint64_t
        active_memtable_bytes() const noexcept {
            return active_memtable_bytes_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] uint64_t
        sealed_memtable_bytes() const noexcept {
            return sealed_memtable_bytes_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] uint64_t
        total_memtable_bytes() const noexcept {
            return active_memtable_bytes() + sealed_memtable_bytes();
        }

        [[nodiscard]] uint32_t
        sealed_gen_count() const noexcept {
            return sealed_gen_count_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] bool
        active_memtable_has_entries() const noexcept {
            return active_memtable_has_entries_.load(
                std::memory_order_relaxed);
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
            const core::front_fragment& fragment,
            std::span<const core::canonical_entry> canonical_entries);

        [[nodiscard]] _front_lookup::sender
        lookup_memtable(std::string_view key,
                        uint64_t read_lsn,
                        core::front_read_set frs);

        [[nodiscard]] _front_lookup::sender
        lookup_memtable(std::string_view key,
                        uint64_t read_lsn,
                        const core::front_read_set* frs);

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

        [[nodiscard]] _front_wal_prepare::sender
        prepare_wal_fragment(
            const core::front_fragment& fragment,
            std::span<const core::canonical_entry> canonical_entries,
            wal::wal_fragment_cursor cursor);

        [[nodiscard]] _front_wal_install::sender
        install_wal_segment(wal::segment_runtime* segment);

        [[nodiscard]] _front_wal_commit::sender
        commit_wal_plan(uint64_t plan_id,
                        std::vector<wal::wal_frame_write> writes = {});

        [[nodiscard]] _front_wal_abort::sender
        abort_wal_plan(uint64_t plan_id,
                       std::vector<wal::wal_frame_write> writes = {});

        void schedule_insert(_front_insert::req* r);
        void schedule_lookup(_front_lookup::req* r);
        void schedule_batch_lookup(_front_batch_lookup::req* r);
        void schedule_scan(_front_scan::req* r);
        void schedule_seal(_front_seal::req* r);
        void schedule_collect(_front_collect::req* r);
        void schedule_release(_front_release::req* r);
        void schedule_wal_prepare(_front_wal_prepare::req* r);
        void schedule_wal_install(_front_wal_install::req* r);
        void schedule_wal_commit(_front_wal_commit::req* r);
        void schedule_wal_abort(_front_wal_abort::req* r);

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

        void configure_wal_for_testing(
            wal::segment_geometry geometry,
            wal::wal_append_config config = {}) {
            configure_wal(std::move(geometry), config);
        }

        [[nodiscard]] wal::wal_prepare_result
        prepare_wal_fragment_for_testing(
            core::front_fragment fragment,
            std::span<const core::canonical_entry> canonical_entries,
            wal::wal_fragment_cursor cursor) {
            return prepare_wal_fragment_now(
                fragment, canonical_entries, cursor, wal_config_);
        }

        void install_wal_segment_for_testing(wal::segment_runtime* segment) {
            install_wal_segment_now(segment);
        }

        [[nodiscard]] std::optional<wal::sealed_segment_info>
        commit_wal_plan_for_testing(uint64_t plan_id) {
            return commit_wal_plan_now(plan_id);
        }

        void abort_wal_plan_for_testing(uint64_t plan_id) {
            abort_wal_plan_now(plan_id);
        }

        [[nodiscard]] uint32_t
        wal_write_offset_for_testing() const {
            return wal_ ? wal_->write_offset() : 0;
        }

        [[nodiscard]] bool
        wal_has_pending_plan_for_testing() const {
            return wal_ && wal_->has_pending_plan();
        }

        [[nodiscard]] std::size_t
        wal_frame_pool_free_pages_for_testing() const noexcept {
            return wal_frame_pool_ ? wal_frame_pool_->free_page_count() : 0;
        }

      private:
        static constexpr uint32_t kMaxInsertPerAdvance = 64;
        static constexpr uint32_t kMaxLookupPerAdvance = 128;
        static constexpr uint32_t kMaxBatchLookupPerAdvance = 64;
        static constexpr uint32_t kMaxScanPerAdvance = 32;
        static constexpr uint32_t kMaxSealPerAdvance = 16;
        static constexpr uint32_t kMaxCollectPerAdvance = 64;
        static constexpr uint32_t kMaxReleasePerAdvance = 64;
        static constexpr uint32_t kMaxWalPreparePerAdvance = 64;
        static constexpr uint32_t kMaxWalInstallPerAdvance = 64;
        static constexpr uint32_t kMaxWalCommitPerAdvance = 64;
        static constexpr uint32_t kMaxWalAbortPerAdvance = 64;

        struct wal_page_builder {
            uint64_t page_index = 0;
            wal::wal_frame_write write;
        };

        // Accumulating state for a coalesced entry plan. Pages, append offset,
        // proposed segment lsn range and participants grow as each fragment is
        // merged in FIFO order; a lone-participant plan is just a group of one.
        struct wal_entry_group_builder {
            std::vector<wal_page_builder> pages;
            uint32_t plan_start_offset = 0;   // == stream.write_offset() at start
            uint32_t offset = 0;              // current append position
            uint32_t usable_end = 0;          // segment usable end (excl trailer)
            uint32_t entry_area = 0;          // usable_end - HEADER_SIZE
            uint32_t segment_gen = 0;
            uint32_t lba_shift = 0;
            uint64_t proposed_min = 0;
            uint64_t proposed_max = 0;
            uint32_t entries_total = 0;
            std::vector<wal::wal_plan_participant> participants;
        };

        enum class wal_group_append_status : uint8_t {
            appended,             // merged >= 1 entry for this fragment
            would_exceed_budget,  // page / participant budget reached
            needs_rotation,       // next entry will not fit the segment tail
            validation_error,     // fragment/entry rejected (see `error`)
        };

        struct wal_group_append_result {
            wal_group_append_status status =
                wal_group_append_status::validation_error;
            wal::wal_fragment_cursor cursor_after{};
            bool fragment_done = false;
            uint32_t entries_appended = 0;
            std::exception_ptr error{};
        };

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

        void validate_snapshot_owner_fast(
            const core::front_read_set& frs) const {
            if (frs.active && frs.active->front_owner_index != owner_id_) {
                throw std::logic_error(
                    "front::front_sched: snapshot active gen owner mismatch");
            }
            for (const auto& gen : frs.imms) {
                if (gen && gen->front_owner_index != owner_id_) {
                    throw std::logic_error(
                        "front::front_sched: snapshot imm gen owner mismatch");
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

        void configure_wal(wal::segment_geometry geometry,
                           wal::wal_append_config config,
                           memory::dma_page_allocator wal_dma_alloc =
                               memory::make_heap_dma_page_allocator());

        [[nodiscard]] wal::wal_stream_state& require_wal();
        [[nodiscard]] const wal::wal_stream_state& require_wal() const;

        void validate_wal_fragment_request(
            const core::front_fragment& fragment,
            std::span<const core::canonical_entry> canonical_entries,
            wal::wal_fragment_cursor cursor) const;

        [[nodiscard]] wal::wal_prepare_result
        prepare_wal_fragment_now(
            const core::front_fragment& fragment,
            std::span<const core::canonical_entry> canonical_entries,
            wal::wal_fragment_cursor cursor,
            wal::wal_append_config config);

        void install_wal_segment_now(wal::segment_runtime* segment);

        [[nodiscard]] std::optional<wal::sealed_segment_info>
        commit_wal_plan_now(uint64_t plan_id);

        void abort_wal_plan_now(uint64_t plan_id);

        [[nodiscard]] wal::wal_prepare_result
        prepare_wal_header_plan(const core::front_fragment& fragment,
                                wal::wal_fragment_cursor cursor);

        // Builds the seed fragment's plan. When `coalesce`, also merges queued
        // followers from `wal_pending_prepares_` into one entry plan and parks
        // their callbacks in `wal_pending_group_`. The returned issue_plan goes
        // to the seed/leader. Single-participant when coalesce is false (test
        // entry) or no follower fits.
        [[nodiscard]] wal::wal_prepare_result
        prepare_wal_fragment_core(
            const core::front_fragment& fragment,
            std::span<const core::canonical_entry> canonical_entries,
            wal::wal_fragment_cursor cursor,
            wal::wal_append_config config,
            bool coalesce);

        [[nodiscard]] wal_entry_group_builder
        make_wal_entry_group_builder(wal::wal_append_config config);

        [[nodiscard]] wal_group_append_result
        try_append_fragment_to_wal_group(
            wal_entry_group_builder& builder,
            const core::front_fragment& fragment,
            std::span<const core::canonical_entry> canonical_entries,
            wal::wal_fragment_cursor cursor,
            wal::wal_append_config config,
            uint64_t waiter_id);

        void drain_followers_into_wal_group(
            wal_entry_group_builder& builder,
            std::vector<wal_group_parked_waiter>& waiters,
            wal::wal_append_config config);

        [[nodiscard]] wal::wal_append_plan
        build_wal_entry_group_plan(wal_entry_group_builder& builder,
                                   wal::wal_append_config config);

        void wake_wal_group_committed(
            uint64_t plan_id,
            const std::optional<wal::sealed_segment_info>& sealed);
        void fail_wal_group(uint64_t plan_id, std::exception_ptr ep);

        [[nodiscard]] wal::wal_prepare_result
        prepare_wal_trailer_plan(const core::front_fragment& fragment,
                                 wal::wal_fragment_cursor cursor);

        [[nodiscard]] wal::wal_prepare_result
        finalize_wal_plan(wal::wal_append_plan&& plan);

        [[nodiscard]] wal::wal_frame_write
        make_wal_page_write(uint64_t page_index);

        [[nodiscard]] wal_page_builder&
        wal_page_for(std::vector<wal_page_builder>& pages,
                     uint64_t page_index,
                     uint32_t plan_start_offset);

        void scatter_to_wal_pages(std::vector<wal_page_builder>& pages,
                                  uint32_t plan_start_offset,
                                  uint32_t segment_offset,
                                  std::span<const char> bytes);

        void scatter_wal_entry_parts(
            std::vector<wal_page_builder>& pages,
            uint32_t plan_start_offset,
            uint32_t segment_offset,
            const format::wal_entry_parts& parts);

        void zero_wal_plan_suffix(std::vector<wal_page_builder>& pages,
                                  uint32_t end_offset);

        [[nodiscard]] static bool value_ref_is_valid(
            const format::value_ref& vr) noexcept;

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
            publish_active_pressure();
        }

        [[nodiscard]] core::memtable_lookup_result
        lookup_memtable_now(std::string_view key,
                            uint64_t read_lsn,
                            const core::front_read_set& frs) const {
            validate_snapshot_owner_fast(frs);
            return core::lookup_memtable(key, read_lsn, frs);
        }

        [[nodiscard]] batch_lookup_result
        batch_lookup_now(std::span<const std::string_view> keys,
                         uint64_t read_lsn,
                         const core::front_read_set& frs) const {
            validate_snapshot_owner_fast(frs);
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
            validate_snapshot_owner_fast(frs);
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
            publish_memtable_pressure();

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
            publish_sealed_pressure();
        }

        void handle_insert(_front_insert::req* r);
        void handle_lookup(_front_lookup::req* r);
        void handle_batch_lookup(_front_batch_lookup::req* r);
        void handle_scan(_front_scan::req* r);
        void handle_seal(_front_seal::req* r);
        void handle_collect(_front_collect::req* r);
        void handle_release(_front_release::req* r);
        void handle_wal_prepare(_front_wal_prepare::req* r);
        void handle_wal_install(_front_wal_install::req* r);
        void handle_wal_commit(_front_wal_commit::req* r);
        void handle_wal_abort(_front_wal_abort::req* r);
        void run_wal_prepare(std::unique_ptr<_front_wal_prepare::req> req);
        void fail_wal_prepare(std::unique_ptr<_front_wal_prepare::req> req,
                              std::exception_ptr ep);
        void drain_wal_pending_prepares();

        [[nodiscard]] bool wal_busy() const noexcept {
            return (wal_ && wal_->has_pending_plan()) ||
                   wal_awaiting_segment_;
        }

        void publish_active_pressure() noexcept {
            const bool has_entries =
                active_ && core::memtable_gen_has_entries(*active_);
            active_memtable_has_entries_.store(
                has_entries, std::memory_order_relaxed);
            active_memtable_bytes_.store(
                active_
                    ? core::approximate_memtable_gen_bytes(*active_)
                    : 0,
                std::memory_order_relaxed);
        }

        void publish_sealed_pressure() noexcept {
            uint64_t bytes = 0;
            for (const auto& gen : imms_) {
                if (gen) {
                    bytes += core::approximate_memtable_gen_bytes(*gen);
                }
            }
            sealed_memtable_bytes_.store(bytes, std::memory_order_relaxed);
            sealed_gen_count_.store(
                static_cast<uint32_t>(imms_.size()),
                std::memory_order_relaxed);
        }

        void publish_memtable_pressure() noexcept {
            publish_active_pressure();
            publish_sealed_pressure();
        }

        pump::core::per_core::queue<_front_insert::req*> insert_q_;
        pump::core::per_core::queue<_front_lookup::req*> lookup_q_;
        pump::core::per_core::queue<_front_batch_lookup::req*> batch_lookup_q_;
        pump::core::per_core::queue<_front_scan::req*> scan_q_;
        pump::core::per_core::queue<_front_seal::req*> seal_q_;
        pump::core::per_core::queue<_front_collect::req*> collect_q_;
        pump::core::per_core::queue<_front_release::req*> release_q_;
        pump::core::per_core::queue<_front_wal_prepare::req*> wal_prepare_q_;
        pump::core::per_core::queue<_front_wal_install::req*> wal_install_q_;
        pump::core::per_core::queue<_front_wal_commit::req*> wal_commit_q_;
        pump::core::per_core::queue<_front_wal_abort::req*> wal_abort_q_;

        uint32_t owner_id_;
        uint32_t front_count_;
        uint64_t next_local_gen_epoch_;
        std::shared_ptr<core::memtable_gen> active_;
        std::atomic<uint64_t> active_memtable_bytes_{0};
        std::atomic<uint64_t> sealed_memtable_bytes_{0};
        std::atomic<uint32_t> sealed_gen_count_{0};
        std::atomic<bool> active_memtable_has_entries_{false};
        std::size_t queue_depth_ = 0;
        std::vector<std::shared_ptr<core::memtable_gen>> imms_;
        std::optional<wal::wal_stream_state> wal_;
        wal::wal_append_config wal_config_{};
        std::unique_ptr<memory::lba_dma_page_pool> wal_frame_pool_;
        bool wal_awaiting_segment_ = false;
        std::deque<_front_wal_prepare::req*> wal_pending_prepares_;
        std::size_t wal_pending_prepare_capacity_ = 0;
        uint64_t next_wal_plan_id_ = 1;
        uint64_t next_wal_waiter_id_ = 1;
        // Followers merged into the current in-flight entry plan. There is at
        // most one in-flight physical plan, so at most one group at a time.
        std::optional<wal_group_plan_state> wal_pending_group_;

        [[nodiscard]] static std::size_t
        resolve_wal_pending_prepare_capacity(
            wal::wal_append_config config,
            std::size_t queue_depth) noexcept {
            return config.pending_prepare_capacity != 0
                ? config.pending_prepare_capacity
                : queue_depth;
        }
    };

    namespace _front_insert {
        struct req {
            borrowed_front_fragment fragment;
            std::span<const core::canonical_entry> canonical_entries;
            std::move_only_function<void(core::owner_outcome<void>&&)> cb;
        };

        struct op {
            constexpr static bool front_insert_op = true;
            front_sched* sched = nullptr;
            const core::front_fragment* fragment = nullptr;
            std::span<const core::canonical_entry> canonical_entries;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            const core::front_fragment* fragment = nullptr;
            std::span<const core::canonical_entry> canonical_entries;

            sender(front_sched* s,
                   const core::front_fragment& f,
                   std::span<const core::canonical_entry> entries)
                : sched(s)
                , fragment(&f)
                , canonical_entries(entries) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{
                    .sched = sched,
                    .fragment = fragment,
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
            core::front_read_set owned;
            const core::front_read_set* borrowed = nullptr;
            std::move_only_function<void(
                core::owner_outcome<core::memtable_lookup_result>&&)> cb;
        };

        struct op {
            constexpr static bool front_lookup_op = true;
            front_sched* sched = nullptr;
            std::string_view key;
            uint64_t read_lsn = 0;
            core::front_read_set owned;
            const core::front_read_set* borrowed = nullptr;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            std::string_view key;
            uint64_t read_lsn = 0;
            core::front_read_set owned;
            const core::front_read_set* borrowed = nullptr;

            sender(front_sched* s,
                   std::string_view k,
                   uint64_t lsn,
                   core::front_read_set snapshot)
                : sched(s)
                , key(k)
                , read_lsn(lsn)
                , owned(std::move(snapshot))
                , borrowed(nullptr) {}

            sender(front_sched* s,
                   std::string_view k,
                   uint64_t lsn,
                   const core::front_read_set* snapshot)
                : sched(s)
                , key(k)
                , read_lsn(lsn)
                , borrowed(snapshot) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{
                    .sched = sched,
                    .key = key,
                    .read_lsn = read_lsn,
                    .owned = std::move(owned),
                    .borrowed = borrowed,
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
            std::move_only_function<void(
                core::owner_outcome<batch_lookup_result>&&)> cb;
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
            std::move_only_function<void(
                core::owner_outcome<core::memtable_scan_result>&&)> cb;
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
            std::move_only_function<void(
                core::owner_outcome<core::front_read_set>&&)> cb;
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
                core::owner_outcome<
                    std::vector<std::shared_ptr<core::memtable_gen>>>&&)> cb;
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
            std::move_only_function<void(core::owner_outcome<void>&&)> cb;
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

    namespace _front_wal_prepare {
        struct req {
            borrowed_front_fragment fragment;
            std::span<const core::canonical_entry> canonical_entries;
            wal::wal_fragment_cursor cursor;
            std::move_only_function<void(
                core::owner_outcome<wal::wal_prepare_result>&&)> cb;
        };

        struct op {
            constexpr static bool front_wal_prepare_op = true;
            front_sched* sched = nullptr;
            const core::front_fragment* fragment = nullptr;
            std::span<const core::canonical_entry> canonical_entries;
            wal::wal_fragment_cursor cursor;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            const core::front_fragment* fragment = nullptr;
            std::span<const core::canonical_entry> canonical_entries;
            wal::wal_fragment_cursor cursor;

            sender(front_sched* s,
                   const core::front_fragment& f,
                   std::span<const core::canonical_entry> entries,
                   wal::wal_fragment_cursor c)
                : sched(s)
                , fragment(&f)
                , canonical_entries(entries)
                , cursor(c) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{
                    .sched = sched,
                    .fragment = fragment,
                    .canonical_entries = canonical_entries,
                    .cursor = cursor,
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_wal_prepare

    namespace _front_wal_install {
        struct req {
            wal::segment_runtime* segment = nullptr;
            std::move_only_function<void(core::owner_outcome<void>&&)> cb;
        };

        struct op {
            constexpr static bool front_wal_install_op = true;
            front_sched* sched = nullptr;
            wal::segment_runtime* segment = nullptr;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            wal::segment_runtime* segment = nullptr;

            auto make_op() { return op{.sched = sched, .segment = segment}; }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_wal_install

    namespace _front_wal_commit {
        struct req {
            uint64_t plan_id = 0;
            std::vector<wal::wal_frame_write> writes;
            std::move_only_function<void(
                core::owner_outcome<
                    std::optional<wal::sealed_segment_info>>&&)> cb;
        };

        struct op {
            constexpr static bool front_wal_commit_op = true;
            front_sched* sched = nullptr;
            uint64_t plan_id = 0;
            std::vector<wal::wal_frame_write> writes;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            uint64_t plan_id = 0;
            std::vector<wal::wal_frame_write> writes;

            sender() = default;
            sender(front_sched* s,
                   uint64_t id,
                   std::vector<wal::wal_frame_write> ws)
                : sched(s), plan_id(id), writes(std::move(ws)) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{
                    .sched = sched,
                    .plan_id = plan_id,
                    .writes = std::move(writes),
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_wal_commit

    namespace _front_wal_abort {
        struct req {
            uint64_t plan_id = 0;
            std::vector<wal::wal_frame_write> writes;
            std::move_only_function<void(core::owner_outcome<void>&&)> cb;
        };

        struct op {
            constexpr static bool front_wal_abort_op = true;
            front_sched* sched = nullptr;
            uint64_t plan_id = 0;
            std::vector<wal::wal_frame_write> writes;

            template<uint32_t pos, typename ctx_t, typename scope_t>
            void start(ctx_t& ctx, scope_t& scope);
        };

        struct sender {
            front_sched* sched = nullptr;
            uint64_t plan_id = 0;
            std::vector<wal::wal_frame_write> writes;

            sender() = default;
            sender(front_sched* s,
                   uint64_t id,
                   std::vector<wal::wal_frame_write> ws)
                : sched(s), plan_id(id), writes(std::move(ws)) {}

            sender(sender&&) noexcept = default;
            sender& operator=(sender&&) noexcept = default;
            sender(const sender&) = delete;
            sender& operator=(const sender&) = delete;

            auto make_op() {
                return op{
                    .sched = sched,
                    .plan_id = plan_id,
                    .writes = std::move(writes),
                };
            }

            template<typename ctx_t>
            auto connect() {
                return pump::core::builder::op_list_builder<0>()
                    .push_back(make_op());
            }
        };
    }  // namespace _front_wal_abort

    inline front_sched::~front_sched() {
        while (auto item = insert_q_.try_dequeue()) delete *item;
        while (auto item = lookup_q_.try_dequeue()) delete *item;
        while (auto item = batch_lookup_q_.try_dequeue()) delete *item;
        while (auto item = scan_q_.try_dequeue()) delete *item;
        while (auto item = seal_q_.try_dequeue()) delete *item;
        while (auto item = collect_q_.try_dequeue()) delete *item;
        while (auto item = release_q_.try_dequeue()) delete *item;
        while (auto item = wal_prepare_q_.try_dequeue()) delete *item;
        while (auto item = wal_install_q_.try_dequeue()) delete *item;
        while (auto item = wal_commit_q_.try_dequeue()) delete *item;
        while (auto item = wal_abort_q_.try_dequeue()) delete *item;
        for (auto* req : wal_pending_prepares_) delete req;
    }

    inline _front_insert::sender
    front_sched::insert_memtable_entries(
        const core::front_fragment& fragment,
        std::span<const core::canonical_entry> canonical_entries) {
        return _front_insert::sender{
            this, fragment, canonical_entries};
    }

    inline _front_lookup::sender
    front_sched::lookup_memtable(std::string_view key,
                                 uint64_t read_lsn,
                                 core::front_read_set frs) {
        return _front_lookup::sender{
            this, key, read_lsn, std::move(frs)};
    }

    inline _front_lookup::sender
    front_sched::lookup_memtable(std::string_view key,
                                 uint64_t read_lsn,
                                 const core::front_read_set* frs) {
        if (frs == nullptr) {
            throw std::invalid_argument(
                "front::front_sched: lookup borrowed front_read_set is null");
        }
        return _front_lookup::sender{this, key, read_lsn, frs};
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

    inline _front_wal_prepare::sender
    front_sched::prepare_wal_fragment(
        const core::front_fragment& fragment,
        std::span<const core::canonical_entry> canonical_entries,
        wal::wal_fragment_cursor cursor) {
        return _front_wal_prepare::sender{
            this, fragment, canonical_entries, cursor};
    }

    inline _front_wal_install::sender
    front_sched::install_wal_segment(wal::segment_runtime* segment) {
        return _front_wal_install::sender{
            .sched = this,
            .segment = segment,
        };
    }

    inline _front_wal_commit::sender
    front_sched::commit_wal_plan(
        uint64_t plan_id,
        std::vector<wal::wal_frame_write> writes) {
        return _front_wal_commit::sender{
            this, plan_id, std::move(writes)};
    }

    inline _front_wal_abort::sender
    front_sched::abort_wal_plan(
        uint64_t plan_id,
        std::vector<wal::wal_frame_write> writes) {
        return _front_wal_abort::sender{
            this, plan_id, std::move(writes)};
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
    front_sched::schedule_wal_prepare(_front_wal_prepare::req* r) {
        if (!wal_prepare_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error(
                "front::front_sched: wal prepare queue full");
        }
    }

    inline void
    front_sched::schedule_wal_install(_front_wal_install::req* r) {
        if (!wal_install_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error(
                "front::front_sched: wal install queue full");
        }
    }

    inline void
    front_sched::schedule_wal_commit(_front_wal_commit::req* r) {
        if (!wal_commit_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error(
                "front::front_sched: wal commit queue full");
        }
    }

    inline void
    front_sched::schedule_wal_abort(_front_wal_abort::req* r) {
        if (!wal_abort_q_.try_enqueue(r)) {
            delete r;
            throw std::runtime_error(
                "front::front_sched: wal abort queue full");
        }
    }

    inline void
    front_sched::configure_wal(wal::segment_geometry geometry,
                               wal::wal_append_config config,
                               memory::dma_page_allocator wal_dma_alloc) {
        wal::validate_segment_geometry(geometry);
        wal::validate_wal_append_config(config);
        wal_.emplace(owner_id_, geometry);
        wal_config_ = config;
        wal_pending_prepare_capacity_ =
            resolve_wal_pending_prepare_capacity(config, queue_depth_);
        // WAL frames are FUA-written straight to NVMe, so the pool must use
        // the runtime DMA allocator (SPDK hugepage memory on the real
        // backend); a heap allocator fails vtophys on real hardware. The
        // allocator is supplied by the caller (build_front_topology passes
        // make_runtime_dma_page_allocator); the heap default only serves
        // unit tests that never touch a real device.
        wal_frame_pool_ = std::make_unique<memory::lba_dma_page_pool>(
            geometry.lba_size,
            geometry.lba_size,
            0,
            std::move(wal_dma_alloc));
        next_wal_plan_id_ = 1;
    }

    inline wal::wal_stream_state&
    front_sched::require_wal() {
        if (!wal_ || !wal_frame_pool_) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::no_wal_config,
                "front::front_sched: WAL is not configured");
        }
        return *wal_;
    }

    inline const wal::wal_stream_state&
    front_sched::require_wal() const {
        if (!wal_ || !wal_frame_pool_) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::no_wal_config,
                "front::front_sched: WAL is not configured");
        }
        return *wal_;
    }

    inline bool
    front_sched::value_ref_is_valid(const format::value_ref& vr) noexcept {
        return vr.base.device_id != 0 || vr.base.lba != 0 ||
               vr.byte_offset != 0 || vr.len != 0 || vr.flags != 0;
    }

    inline void
    front_sched::validate_wal_fragment_request(
        const core::front_fragment& fragment,
        std::span<const core::canonical_entry> canonical_entries,
        wal::wal_fragment_cursor cursor) const {
        if (fragment.owner != owner_id_) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::fragment_owner_mismatch,
                "front::front_sched: WAL fragment owner mismatch");
        }
        if (fragment.entry_count != canonical_entries.size()) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::fragment_entry_count_mismatch,
                "front::front_sched: WAL fragment entry_count mismatch");
        }
        if (cursor.next_fragment_entry > fragment.entry_indices.size()) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::fragment_cursor_out_of_range,
                "front::front_sched: WAL fragment cursor out of range");
        }
        validate_active_present();
    }

    inline wal::wal_frame_write
    front_sched::make_wal_page_write(uint64_t page_index) {
        auto& stream = require_wal();
        auto* segment = stream.active_segment();
        if (segment == nullptr) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::no_active_segment,
                "front::front_sched: no active WAL segment");
        }

        const auto segment_base = stream.segment_base();
        auto frame = wal_frame_pool_->get_frame(
            memory::frame_id{
                .base = format::paddr{
                    .device_id = segment_base.device_id,
                    .lba = segment_base.lba + page_index,
                },
                .span_lbas = 1,
                .dom = memory::frame_id::domain::wal_page,
            },
            memory::frame_state::dirty_append,
            false);
        if (!frame) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::frame_allocation_failed,
                "front::front_sched: failed to allocate WAL frame");
        }

        return wal::wal_frame_write{wal_frame_pool_.get(), std::move(*frame)};
    }

    inline front_sched::wal_page_builder&
    front_sched::wal_page_for(std::vector<wal_page_builder>& pages,
                              uint64_t page_index,
                              uint32_t plan_start_offset) {
        if (!pages.empty() && pages.back().page_index == page_index) {
            return pages.back();
        }

        for (auto it = pages.begin(); it != pages.end(); ++it) {
            if (it->page_index == page_index) return *it;
        }

        wal_page_builder next{
            .page_index = page_index,
            .write = make_wal_page_write(page_index),
        };
        auto page_bytes = next.write.frame.mutable_lba_bytes(0);
        auto& stream = require_wal();
        if (auto tail = stream.tail_image_for(page_index)) {
            if (tail->size() != page_bytes.size()) {
                throw std::logic_error(
                    "front::front_sched: WAL tail image size mismatch");
            }
            std::memcpy(page_bytes.data(), tail->data(), page_bytes.size());
        } else {
            const uint64_t page_start = page_index << stream.lba_shift();
            if (page_start < plan_start_offset) {
                const uint64_t prefix =
                    std::min<uint64_t>(
                        page_bytes.size(),
                        static_cast<uint64_t>(plan_start_offset) - page_start);
                std::memset(page_bytes.data(), 0, prefix);
            }
        }

        auto insert_at = pages.end();
        while (insert_at != pages.begin() &&
               std::prev(insert_at)->page_index > page_index) {
            --insert_at;
        }
        return *pages.insert(insert_at, std::move(next));
    }

    inline void
    front_sched::scatter_to_wal_pages(
        std::vector<wal_page_builder>& pages,
        uint32_t plan_start_offset,
        uint32_t segment_offset,
        std::span<const char> bytes) {
        if (bytes.empty()) return;

        const auto& stream = require_wal();
        const uint32_t lba_size = stream.lba_size();
        const uint32_t lba_mask = lba_size - 1;
        uint64_t copied = 0;
        while (copied < bytes.size()) {
            const uint64_t pos =
                static_cast<uint64_t>(segment_offset) + copied;
            const uint64_t page_index = pos >> stream.lba_shift();
            const uint32_t page_offset =
                static_cast<uint32_t>(pos & lba_mask);
            auto& page = wal_page_for(pages, page_index, plan_start_offset);
            auto page_bytes = page.write.frame.mutable_lba_bytes(0);
            const uint64_t n = std::min<uint64_t>(
                page_bytes.size() - page_offset, bytes.size() - copied);
            std::memcpy(
                page_bytes.data() + page_offset, bytes.data() + copied, n);
            copied += n;
        }
    }

    inline void
    front_sched::scatter_wal_entry_parts(
        std::vector<wal_page_builder>& pages,
        uint32_t plan_start_offset,
        uint32_t segment_offset,
        const format::wal_entry_parts& parts) {
        uint32_t off = segment_offset;
        scatter_to_wal_pages(
            pages,
            plan_start_offset,
            off,
            std::span<const char>{
                reinterpret_cast<const char*>(&parts.header),
                sizeof(parts.header)});
        off += sizeof(parts.header);
        scatter_to_wal_pages(pages, plan_start_offset, off, parts.value_ref_bytes);
        off += static_cast<uint32_t>(parts.value_ref_bytes.size());
        scatter_to_wal_pages(pages, plan_start_offset, off, parts.key_bytes);
        off += static_cast<uint32_t>(parts.key_bytes.size());
        scatter_to_wal_pages(
            pages,
            plan_start_offset,
            off,
            std::span<const char>{
                reinterpret_cast<const char*>(&parts.crc),
                sizeof(parts.crc)});
    }

    inline void
    front_sched::zero_wal_plan_suffix(std::vector<wal_page_builder>& pages,
                                      uint32_t end_offset) {
        if (pages.empty()) return;
        const auto& stream = require_wal();
        const uint32_t end_mod = end_offset & (stream.lba_size() - 1);
        if (end_mod == 0) return;

        auto bytes = pages.back().write.frame.mutable_lba_bytes(0);
        if (end_mod > bytes.size()) {
            throw std::logic_error(
                "front::front_sched: WAL suffix offset exceeds page size");
        }
        std::memset(bytes.data() + end_mod, 0, bytes.size() - end_mod);
    }

    inline wal::wal_prepare_result
    front_sched::finalize_wal_plan(wal::wal_append_plan&& plan) {
        auto& stream = require_wal();
        std::span<const char> last_page_bytes{};
        if (plan.kind != wal::wal_plan_kind::trailer) {
            if (plan.writes.empty()) {
                throw std::logic_error(
                    "front::front_sched: WAL plan has no tail page");
            }
            last_page_bytes = plan.writes.back().frame.lba_bytes(0);
        }
        stream.begin_pending(plan, last_page_bytes);
        return wal::wal_prepare_issue_plan{.plan = std::move(plan)};
    }

    inline wal::wal_prepare_result
    front_sched::prepare_wal_header_plan(const core::front_fragment& fragment,
                                         wal::wal_fragment_cursor cursor) {
        auto& stream = require_wal();
        auto* segment = stream.active_segment();
        if (segment == nullptr) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::no_active_segment,
                "front::front_sched: no active WAL segment");
        }

        std::vector<wal_page_builder> pages;
        pages.reserve(wal_config_.max_pages_per_plan);
        const auto header = format::make_wal_segment_header(
            segment->id.index,
            segment->id.device_id,
            stream.stream_id(),
            segment->segment_gen,
            stream.geometry().expected_format_version);
        scatter_to_wal_pages(
            pages,
            0,
            0,
            std::span<const char>{
                reinterpret_cast<const char*>(&header),
                sizeof(header)});
        zero_wal_plan_suffix(pages, format::WAL_SEGMENT_HEADER_SIZE);

        wal::wal_append_plan plan;
        plan.plan_id = next_wal_plan_id_++;
        plan.kind = wal::wal_plan_kind::header;
        plan.stream_id = stream.stream_id();
        plan.segment = segment->id;
        plan.segment_gen = segment->segment_gen;
        plan.start_offset = 0;
        plan.end_offset = format::WAL_SEGMENT_HEADER_SIZE;
        plan.cursor_before = cursor;
        plan.cursor_after = cursor;
        plan.fragment_done =
            cursor.next_fragment_entry >= fragment.entry_indices.size();
        plan.config = wal_config_;
        plan.writes.reserve(pages.size());
        for (auto& page : pages) {
            plan.writes.push_back(std::move(page.write));
        }
        return finalize_wal_plan(std::move(plan));
    }

    inline wal::wal_prepare_result
    front_sched::prepare_wal_trailer_plan(const core::front_fragment& fragment,
                                          wal::wal_fragment_cursor cursor) {
        auto& stream = require_wal();
        auto* segment = stream.active_segment();
        if (segment == nullptr) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::no_active_segment,
                "front::front_sched: no active WAL segment");
        }

        const auto sealed = stream.make_sealed_info();
        const uint32_t write_end_field = stream.write_offset();
        const uint32_t start = stream.usable_end_offset();
        const auto trailer = format::make_wal_sealed_trailer(
            segment->segment_gen,
            write_end_field,
            sealed.min_lsn,
            sealed.max_lsn);

        std::vector<wal_page_builder> pages;
        pages.reserve(wal_config_.max_pages_per_plan);
        scatter_to_wal_pages(
            pages,
            start,
            start,
            std::span<const char>{
                reinterpret_cast<const char*>(&trailer),
                sizeof(trailer)});
        zero_wal_plan_suffix(pages, start + format::WAL_SEALED_TRAILER_SIZE);

        wal::wal_append_plan plan;
        plan.plan_id = next_wal_plan_id_++;
        plan.kind = wal::wal_plan_kind::trailer;
        plan.stream_id = stream.stream_id();
        plan.segment = segment->id;
        plan.segment_gen = segment->segment_gen;
        plan.start_offset = start;
        plan.end_offset = start + format::WAL_SEALED_TRAILER_SIZE;
        plan.min_lsn = sealed.min_lsn;
        plan.max_lsn = sealed.max_lsn;
        plan.cursor_before = cursor;
        plan.cursor_after = cursor;
        plan.fragment_done =
            cursor.next_fragment_entry >= fragment.entry_indices.size();
        plan.config = wal_config_;
        plan.sealed_on_commit = sealed;
        plan.writes.reserve(pages.size());
        for (auto& page : pages) {
            plan.writes.push_back(std::move(page.write));
        }
        return finalize_wal_plan(std::move(plan));
    }

    inline front_sched::wal_entry_group_builder
    front_sched::make_wal_entry_group_builder(wal::wal_append_config config) {
        auto& stream = require_wal();
        auto* segment = stream.active_segment();
        if (segment == nullptr) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::no_active_segment,
                "front::front_sched: no active WAL segment");
        }
        wal_entry_group_builder builder;
        builder.pages.reserve(config.max_pages_per_plan);
        builder.plan_start_offset = stream.write_offset();
        builder.offset = stream.write_offset();
        builder.usable_end = stream.usable_end_offset();
        builder.entry_area =
            builder.usable_end - format::WAL_SEGMENT_HEADER_SIZE;
        builder.segment_gen = segment->segment_gen;
        builder.lba_shift = stream.lba_shift();
        builder.proposed_min =
            stream.segment_empty() ? std::numeric_limits<uint64_t>::max()
                                   : stream.segment_min_lsn();
        builder.proposed_max =
            stream.segment_empty() ? 0 : stream.segment_max_lsn();
        builder.entries_total = 0;
        return builder;
    }

    // Measure-then-scatter: validate/measure how much of `fragment` (from
    // `cursor`) fits the running group, then scatter exactly those entries.
    // The builder is mutated only on `appended`, so a follower that fails
    // validation or hits a budget/rotation boundary never corrupts the shared
    // group image (INC-057 / 054 §8.3, §9). WAL append is contiguous, so the
    // plan's page count is the closed range [plan_first_page, last_page].
    inline front_sched::wal_group_append_result
    front_sched::try_append_fragment_to_wal_group(
        wal_entry_group_builder& builder,
        const core::front_fragment& fragment,
        std::span<const core::canonical_entry> canonical_entries,
        wal::wal_fragment_cursor cursor,
        wal::wal_append_config config,
        uint64_t waiter_id) {
        const auto fragment_done_at = [&](wal::wal_fragment_cursor c) {
            return c.next_fragment_entry >= fragment.entry_indices.size();
        };
        if (fragment_done_at(cursor)) {
            return wal_group_append_result{
                .status = wal_group_append_status::validation_error,
                .cursor_after = cursor,
                .fragment_done = true,
                .error = std::make_exception_ptr(wal::wal_append_error(
                    wal::wal_append_error_reason::fragment_cursor_out_of_range,
                    "front::front_sched: WAL fragment is already complete")),
            };
        }

        const uint32_t plan_first_page =
            builder.plan_start_offset >> builder.lba_shift;
        uint32_t measure_offset = builder.offset;
        auto measure_cursor = cursor;
        std::vector<format::wal_entry_parts> measured;
        uint64_t local_min = std::numeric_limits<uint64_t>::max();
        uint64_t local_max = 0;
        wal_group_append_status boundary =
            wal_group_append_status::would_exceed_budget;
        bool hit_boundary = false;

        while (measure_cursor.next_fragment_entry <
               fragment.entry_indices.size()) {
            const uint32_t entry_index =
                fragment.entry_indices[measure_cursor.next_fragment_entry];
            if (entry_index >= canonical_entries.size()) {
                return wal_group_append_result{
                    .status = wal_group_append_status::validation_error,
                    .error = std::make_exception_ptr(wal::wal_append_error(
                        wal::wal_append_error_reason::
                            fragment_entry_index_out_of_range,
                        "front::front_sched: WAL fragment entry index out of "
                        "range")),
                };
            }
            const auto& entry = canonical_entries[entry_index];
            if (entry.key.size() > wal::kMaxSupportedWalKeyBytes) {
                return wal_group_append_result{
                    .status = wal_group_append_status::validation_error,
                    .error = std::make_exception_ptr(wal::wal_append_error(
                        wal::wal_append_error_reason::key_too_large,
                        "front::front_sched: WAL key too large")),
                };
            }

            format::wal_entry_parts parts;
            switch (entry.op) {
            case core::write_op_type::put:
                if (!value_ref_is_valid(entry.allocated_vr)) {
                    return wal_group_append_result{
                        .status = wal_group_append_status::validation_error,
                        .error = std::make_exception_ptr(wal::wal_append_error(
                            wal::wal_append_error_reason::invalid_value_ref,
                            "front::front_sched: PUT WAL entry missing "
                            "value_ref")),
                    };
                }
                parts = format::make_wal_put_entry_parts_unchecked(
                    builder.segment_gen,
                    fragment.batch_lsn,
                    fragment.entry_count,
                    entry.key,
                    entry.allocated_vr);
                break;
            case core::write_op_type::del:
                parts = format::make_wal_delete_entry_parts_unchecked(
                    builder.segment_gen,
                    fragment.batch_lsn,
                    fragment.entry_count,
                    entry.key);
                break;
            default:
                return wal_group_append_result{
                    .status = wal_group_append_status::validation_error,
                    .error = std::make_exception_ptr(wal::wal_append_error(
                        wal::wal_append_error_reason::unsupported_op,
                        "front::front_sched: unsupported WAL op")),
                };
            }
            if (parts.total_len > builder.entry_area) {
                return wal_group_append_result{
                    .status = wal_group_append_status::validation_error,
                    .error = std::make_exception_ptr(wal::wal_append_error(
                        wal::wal_append_error_reason::
                            entry_too_large_for_segment,
                        "front::front_sched: WAL entry cannot fit a segment")),
                };
            }
            if (parts.total_len > builder.usable_end - measure_offset) {
                boundary = wal_group_append_status::needs_rotation;
                hit_boundary = true;
                break;
            }
            const uint32_t last_page =
                (measure_offset + parts.total_len - 1) >> builder.lba_shift;
            const uint32_t page_count_after = last_page - plan_first_page + 1;
            if ((builder.entries_total + measured.size()) > 0 &&
                page_count_after > config.max_pages_per_plan) {
                boundary = wal_group_append_status::would_exceed_budget;
                hit_boundary = true;
                break;
            }

            measured.push_back(parts);
            measure_offset += parts.total_len;
            if (fragment.batch_lsn < local_min) local_min = fragment.batch_lsn;
            if (fragment.batch_lsn > local_max) local_max = fragment.batch_lsn;
            ++measure_cursor.next_fragment_entry;
        }

        if (measured.empty()) {
            return wal_group_append_result{
                .status = hit_boundary
                              ? boundary
                              : wal_group_append_status::would_exceed_budget,
                .cursor_after = cursor,
                .fragment_done = fragment_done_at(cursor),
            };
        }

        // Commit the measured run into the shared group image.
        for (const auto& parts : measured) {
            scatter_wal_entry_parts(
                builder.pages, builder.plan_start_offset, builder.offset,
                parts);
            builder.offset += parts.total_len;
        }
        builder.entries_total += static_cast<uint32_t>(measured.size());
        if (local_min < builder.proposed_min) builder.proposed_min = local_min;
        if (local_max > builder.proposed_max) builder.proposed_max = local_max;

        const bool done = fragment_done_at(measure_cursor);
        builder.participants.push_back(wal::wal_plan_participant{
            .waiter_id = waiter_id,
            .cursor_before = cursor,
            .cursor_after = measure_cursor,
            .fragment_done = done,
        });
        return wal_group_append_result{
            .status = wal_group_append_status::appended,
            .cursor_after = measure_cursor,
            .fragment_done = done,
            .entries_appended = static_cast<uint32_t>(measured.size()),
        };
    }

    // Coalesce queued followers (FIFO) into the seed's plan. A follower that
    // hits a page/participant budget or a segment-rotation boundary is left at
    // the FIFO head for a later group (FIFO fairness, 054 §13.1); a malformed
    // follower fails its own callback and does not block the rest (054 §8.3).
    inline void
    front_sched::drain_followers_into_wal_group(
        wal_entry_group_builder& builder,
        std::vector<wal_group_parked_waiter>& waiters,
        wal::wal_append_config config) {
        while (builder.participants.size() <
                   config.max_participants_per_group &&
               !wal_pending_prepares_.empty()) {
            _front_wal_prepare::req* peek = wal_pending_prepares_.front();
            if (peek->fragment) {
                auto res = try_append_fragment_to_wal_group(
                    builder, peek->fragment.get(), peek->canonical_entries,
                    peek->cursor, config, next_wal_waiter_id_);
                if (res.status ==
                        wal_group_append_status::would_exceed_budget ||
                    res.status == wal_group_append_status::needs_rotation) {
                    // Cannot merge now; preserve FIFO order for a later group.
                    return;
                }
                wal_pending_prepares_.pop_front();
                std::unique_ptr<_front_wal_prepare::req> owned(peek);
                if (res.status ==
                    wal_group_append_status::validation_error) {
                    auto cb = std::move(owned->cb);
                    owned.reset();
                    if (cb) cb(std::unexpected(res.error));
                    continue;
                }
                // appended
                waiters.push_back(wal_group_parked_waiter{
                    .cursor_after = res.cursor_after,
                    .fragment_done = res.fragment_done,
                    .cb = std::move(owned->cb),
                });
                owned.reset();
                ++next_wal_waiter_id_;
                continue;
            }
            // Null-fragment follower: drop with an error, keep draining.
            wal_pending_prepares_.pop_front();
            std::unique_ptr<_front_wal_prepare::req> owned(peek);
            auto cb = std::move(owned->cb);
            owned.reset();
            if (cb) {
                cb(std::unexpected(std::make_exception_ptr(
                    std::invalid_argument(
                        "front::front_sched: WAL follower fragment is null"))));
            }
        }
    }

    inline wal::wal_append_plan
    front_sched::build_wal_entry_group_plan(wal_entry_group_builder& builder,
                                            wal::wal_append_config config) {
        auto& stream = require_wal();
        auto* segment = stream.active_segment();
        if (segment == nullptr) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::no_active_segment,
                "front::front_sched: no active WAL segment");
        }
        if (builder.participants.empty()) {
            throw std::logic_error(
                "front::front_sched: WAL entry group has no leader");
        }
        zero_wal_plan_suffix(builder.pages, builder.offset);

        wal::wal_append_plan plan;
        plan.plan_id = next_wal_plan_id_++;
        plan.kind = wal::wal_plan_kind::entries;
        plan.stream_id = stream.stream_id();
        plan.segment = segment->id;
        plan.segment_gen = builder.segment_gen;
        plan.start_offset = builder.plan_start_offset;
        plan.end_offset = builder.offset;
        plan.min_lsn = builder.proposed_min;
        plan.max_lsn = builder.proposed_max;
        // Leader-compat fields mirror participants[0] (the FUA issuer).
        const auto& leader = builder.participants.front();
        plan.cursor_before = leader.cursor_before;
        plan.cursor_after = leader.cursor_after;
        plan.fragment_done = leader.fragment_done;
        plan.config = config;
        plan.writes.reserve(builder.pages.size());
        for (auto& page : builder.pages) {
            plan.writes.push_back(std::move(page.write));
        }
        plan.participants = std::move(builder.participants);
        return plan;
    }

    inline wal::wal_prepare_result
    front_sched::prepare_wal_fragment_core(
        const core::front_fragment& fragment,
        std::span<const core::canonical_entry> canonical_entries,
        wal::wal_fragment_cursor cursor,
        wal::wal_append_config config,
        bool coalesce) {
        validate_wal_fragment_request(fragment, canonical_entries, cursor);
        wal::validate_wal_append_config(config);
        auto& stream = require_wal();
        if (stream.has_pending_plan()) {
            throw wal::wal_append_error(
                wal::wal_append_error_reason::pending_plan_exists,
                "front::front_sched: WAL plan already pending");
        }
        if (!stream.has_active_segment()) {
            return wal::wal_prepare_needs_segment{
                .stream_id = owner_id_,
                .sealed = std::nullopt,
            };
        }
        if (!stream.header_committed()) {
            return prepare_wal_header_plan(fragment, cursor);
        }

        auto builder = make_wal_entry_group_builder(config);
        auto seed = try_append_fragment_to_wal_group(
            builder, fragment, canonical_entries, cursor, config,
            /*waiter_id=*/0);
        switch (seed.status) {
        case wal_group_append_status::validation_error:
            std::rethrow_exception(seed.error);
        case wal_group_append_status::needs_rotation:
            // Empty group, seed's next entry needs a fresh segment: seal first.
            return prepare_wal_trailer_plan(fragment, cursor);
        case wal_group_append_status::would_exceed_budget:
            // The first entry of an empty group is always admitted, so the seed
            // cannot exceed the page budget here.
            throw std::logic_error(
                "front::front_sched: WAL seed exceeded budget with empty group");
        case wal_group_append_status::appended:
            break;
        }

        // Once a follower is drained out of the FIFO its callback only lives in
        // `waiters`, so EVERYTHING from the drain through begin_pending must be
        // covered: any throw here (frame allocation, scatter, finalize) has to
        // release every already-merged follower with the same error, or its
        // sender waits forever. The seed gets the same error via the rethrow
        // (run_wal_prepare -> fail_wal_prepare). The currently-failing follower
        // is still in the FIFO (drain pops only after try_append succeeds) and
        // is retried by a later group once frame pressure clears.
        std::vector<wal_group_parked_waiter> waiters;
        wal::wal_append_plan plan;
        try {
            if (coalesce) {
                drain_followers_into_wal_group(builder, waiters, config);
            }
            plan = build_wal_entry_group_plan(builder, config);
            if (plan.writes.empty()) {
                throw std::logic_error(
                    "front::front_sched: WAL entry plan has no tail page");
            }
            // One tail snapshot per coalesced plan (054 §10).
            stream.begin_pending(plan, plan.writes.back().frame.lba_bytes(0));
        } catch (...) {
            auto ep = std::current_exception();
            for (auto& w : waiters) {
                if (w.cb) w.cb(std::unexpected(ep));
            }
            throw;
        }

        if (!waiters.empty()) {
            wal_pending_group_ = wal_group_plan_state{
                .plan_id = plan.plan_id,
                .waiters = std::move(waiters),
            };
        }
        return wal::wal_prepare_issue_plan{.plan = std::move(plan)};
    }

    inline wal::wal_prepare_result
    front_sched::prepare_wal_fragment_now(
        const core::front_fragment& fragment,
        std::span<const core::canonical_entry> canonical_entries,
        wal::wal_fragment_cursor cursor,
        wal::wal_append_config config) {
        // Single-participant path (testing / cold start): no FIFO coalescing.
        return prepare_wal_fragment_core(
            fragment, canonical_entries, cursor, config, /*coalesce=*/false);
    }

    inline void
    front_sched::install_wal_segment_now(wal::segment_runtime* segment) {
        require_wal().install_segment(segment);
    }

    inline std::optional<wal::sealed_segment_info>
    front_sched::commit_wal_plan_now(uint64_t plan_id) {
        auto& stream = require_wal();
        return stream.commit_pending(plan_id);
    }

    inline void
    front_sched::abort_wal_plan_now(uint64_t plan_id) {
        auto& stream = require_wal();
        stream.abort_pending(plan_id);
    }

    inline void
    front_sched::handle_insert(_front_insert::req* r) {
        std::unique_ptr<_front_insert::req> req(r);
        try {
            if (!req->fragment) {
                throw std::invalid_argument(
                    "front::front_sched: insert fragment is null");
            }
            validate_insert_request(
                req->fragment.get(), req->canonical_entries);
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        apply_insert_validated(
            req->fragment.get(), req->canonical_entries);

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<void>{});
        }
    }

    inline void
    front_sched::handle_lookup(_front_lookup::req* r) {
        std::unique_ptr<_front_lookup::req> req(r);
        core::memtable_lookup_result result;
        try {
            const core::front_read_set* frs =
                req->borrowed != nullptr ? req->borrowed : &req->owned;
            result = lookup_memtable_now(req->key, req->read_lsn, *frs);
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<core::memtable_lookup_result>{
                std::move(result)});
        }
    }

    inline void
    front_sched::handle_batch_lookup(_front_batch_lookup::req* r) {
        std::unique_ptr<_front_batch_lookup::req> req(r);
        batch_lookup_result result;
        try {
            result = batch_lookup_now(req->keys, req->read_lsn, req->frs);
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<batch_lookup_result>{std::move(result)});
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
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        if (cb) {
            cb(core::owner_outcome<core::memtable_scan_result>{
                std::move(result)});
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
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<core::front_read_set>{std::move(result)});
        }
    }

    inline void
    front_sched::handle_collect(_front_collect::req* r) {
        std::unique_ptr<_front_collect::req> req(r);
        std::vector<std::shared_ptr<core::memtable_gen>> result;
        try {
            result = collect_eligible_gens_now(req->durable_lsn);
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<
                std::vector<std::shared_ptr<core::memtable_gen>>>{
                    std::move(result)});
        }
    }

    inline void
    front_sched::handle_release(_front_release::req* r) {
        std::unique_ptr<_front_release::req> req(r);
        try {
            release_gens_now(req->gen_ids);
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<void>{});
        }
    }

    inline void
    front_sched::handle_wal_prepare(_front_wal_prepare::req* r) {
        std::unique_ptr<_front_wal_prepare::req> req(r);
        if (wal_busy()) {
            if (wal_pending_prepares_.size() >=
                wal_pending_prepare_capacity_) {
                fail_wal_prepare(
                    std::move(req),
                    std::make_exception_ptr(wal::wal_append_error(
                        wal::wal_append_error_reason::prepare_queue_full,
                        "front::front_sched: WAL prepare queue full")));
                return;
            }
            wal_pending_prepares_.push_back(req.release());
            return;
        }
        run_wal_prepare(std::move(req));
    }

    inline void
    front_sched::run_wal_prepare(
        std::unique_ptr<_front_wal_prepare::req> req) {
        wal::wal_prepare_result result;
        try {
            if (!req->fragment) {
                throw std::invalid_argument(
                    "front::front_sched: WAL fragment is null");
            }
            // The leader seed merges any already-queued followers from
            // wal_pending_prepares_ into one physical plan (INC-057 / 054 §8).
            result = prepare_wal_fragment_core(
                req->fragment.get(),
                req->canonical_entries,
                req->cursor,
                wal_config_,
                /*coalesce=*/true);
        } catch (...) {
            fail_wal_prepare(std::move(req), std::current_exception());
            return;
        }

        if (std::holds_alternative<wal::wal_prepare_needs_segment>(result)) {
            wal_awaiting_segment_ = true;
        }
        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(core::owner_outcome<wal::wal_prepare_result>{
                std::move(result)});
        }
    }

    inline void
    front_sched::fail_wal_prepare(
        std::unique_ptr<_front_wal_prepare::req> req,
        std::exception_ptr ep) {
        auto cb = std::move(req->cb);
        req.reset();
        if (cb) {
            cb(std::unexpected(std::move(ep)));
        }
    }

    inline void
    front_sched::wake_wal_group_committed(
        uint64_t plan_id,
        const std::optional<wal::sealed_segment_info>& sealed) {
        // Only fan out to the group attached to THIS plan. There is one
        // in-flight physical plan today so this always matches, but the guard
        // keeps a future state-machine change from waking unrelated followers.
        if (!wal_pending_group_.has_value() ||
            wal_pending_group_->plan_id != plan_id) {
            return;
        }
        auto group = std::move(*wal_pending_group_);
        wal_pending_group_.reset();
        for (auto& waiter : group.waiters) {
            if (!waiter.cb) continue;
            waiter.cb(core::owner_outcome<wal::wal_prepare_result>{
                wal::wal_prepare_committed{
                    .cursor_after = waiter.cursor_after,
                    .fragment_done = waiter.fragment_done,
                    // Entry-group commits never seal; followers only adopt the
                    // durable cursor. Trailer/header stay single-participant.
                    .sealed = sealed,
                }});
        }
    }

    inline void
    front_sched::fail_wal_group(uint64_t plan_id, std::exception_ptr ep) {
        if (!wal_pending_group_.has_value() ||
            wal_pending_group_->plan_id != plan_id) {
            return;
        }
        auto group = std::move(*wal_pending_group_);
        wal_pending_group_.reset();
        for (auto& waiter : group.waiters) {
            if (waiter.cb) {
                waiter.cb(std::unexpected(ep));
            }
        }
    }

    inline void
    front_sched::drain_wal_pending_prepares() {
        while (!wal_busy() && !wal_pending_prepares_.empty()) {
            std::unique_ptr<_front_wal_prepare::req> req(
                wal_pending_prepares_.front());
            wal_pending_prepares_.pop_front();
            run_wal_prepare(std::move(req));
        }
    }

    inline void
    front_sched::handle_wal_install(_front_wal_install::req* r) {
        std::unique_ptr<_front_wal_install::req> req(r);
        try {
            if (!wal_awaiting_segment_) {
                throw std::logic_error(
                    "front::front_sched: WAL install without awaiting segment");
            }
            install_wal_segment_now(req->segment);
            wal_awaiting_segment_ = false;
        } catch (...) {
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(std::current_exception()));
            }
            return;
        }

        auto cb = std::move(req->cb);
        req.reset();
        drain_wal_pending_prepares();
        if (cb) {
            cb(core::owner_outcome<void>{});
        }
    }

    inline void
    front_sched::handle_wal_commit(_front_wal_commit::req* r) {
        std::unique_ptr<_front_wal_commit::req> req(r);
        const uint64_t plan_id = req->plan_id;
        std::optional<wal::sealed_segment_info> result;
        try {
            result = commit_wal_plan_now(plan_id);
        } catch (...) {
            // Commit invariant failure is a runtime bug; still wake any parked
            // followers so they fail rather than strand (054 §14).
            auto ep = std::current_exception();
            fail_wal_group(plan_id, ep);
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(ep));
            }
            return;
        }

        // The single physical plan is durable. Fan-out the same durable
        // completion to every merged follower before recycling the leader
        // frames and draining the next group (054 §11.1).
        wake_wal_group_committed(plan_id, result);
        auto cb = std::move(req->cb);
        req.reset();
        if (!result.has_value()) {
            drain_wal_pending_prepares();
        }
        if (cb) {
            cb(core::owner_outcome<std::optional<wal::sealed_segment_info>>{
                std::move(result)});
        }
    }

    inline void
    front_sched::handle_wal_abort(_front_wal_abort::req* r) {
        std::unique_ptr<_front_wal_abort::req> req(r);
        const uint64_t plan_id = req->plan_id;
        try {
            abort_wal_plan_now(plan_id);
        } catch (...) {
            auto ep = std::current_exception();
            fail_wal_group(plan_id, ep);
            auto cb = std::move(req->cb);
            req.reset();
            if (cb) {
                cb(std::unexpected(ep));
            }
            return;
        }

        // The committed cursor did not advance. Fan-out the WAL device failure
        // to every merged follower so all batches in the group fail before the
        // memtable phase and go through release (054 §11.2). The leader fails
        // in its own L3 issue path after this abort acknowledges.
        fail_wal_group(plan_id,
                       std::make_exception_ptr(wal::wal_append_error(
                           wal::wal_append_error_reason::device_failure,
                           "front::front_sched: WAL group plan FUA failed")));
        auto cb = std::move(req->cb);
        req.reset();
        drain_wal_pending_prepares();
        if (cb) {
            cb(core::owner_outcome<void>{});
        }
    }

    inline bool
    front_sched::advance() {
        bool progress = false;

        for (uint32_t i = 0; i < kMaxWalInstallPerAdvance; ++i) {
            auto item = wal_install_q_.try_dequeue();
            if (!item) break;
            handle_wal_install(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxWalAbortPerAdvance; ++i) {
            auto item = wal_abort_q_.try_dequeue();
            if (!item) break;
            handle_wal_abort(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxWalCommitPerAdvance; ++i) {
            auto item = wal_commit_q_.try_dequeue();
            if (!item) break;
            handle_wal_commit(*item);
            progress = true;
        }

        for (uint32_t i = 0; i < kMaxWalPreparePerAdvance; ++i) {
            auto item = wal_prepare_q_.try_dequeue();
            if (!item) break;
            handle_wal_prepare(*item);
            progress = true;
        }

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
            .fragment = fragment,
            .canonical_entries = canonical_entries,
            .cb = core::make_owner_pusher<pos, scope_t, void>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_lookup::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_lookup(new req{
            .key = key,
            .read_lsn = read_lsn,
            .owned = std::move(owned),
            .borrowed = borrowed,
            .cb = core::make_owner_pusher<
                pos, scope_t, core::memtable_lookup_result>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_batch_lookup::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_batch_lookup(new req{
            .keys = keys,
            .read_lsn = read_lsn,
            .frs = std::move(frs),
            .cb = core::make_owner_pusher<
                pos, scope_t, batch_lookup_result>(ctx, scope),
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
            .cb = core::make_owner_pusher<
                pos, scope_t, core::memtable_scan_result>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_seal::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_seal(new req{
            .cb = core::make_owner_pusher<
                pos, scope_t, core::front_read_set>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_collect::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_collect(new req{
            .durable_lsn = durable_lsn,
            .cb = core::make_owner_pusher<
                pos,
                scope_t,
                std::vector<std::shared_ptr<core::memtable_gen>>>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_release::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_release(new req{
            .gen_ids = std::move(gen_ids),
            .cb = core::make_owner_pusher<pos, scope_t, void>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_wal_prepare::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_wal_prepare(new req{
            .fragment = fragment,
            .canonical_entries = canonical_entries,
            .cursor = cursor,
            .cb = core::make_owner_pusher<
                pos, scope_t, wal::wal_prepare_result>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_wal_install::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_wal_install(new req{
            .segment = segment,
            .cb = core::make_owner_pusher<pos, scope_t, void>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_wal_commit::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_wal_commit(new req{
            .plan_id = plan_id,
            .writes = std::move(writes),
            .cb = core::make_owner_pusher<
                pos,
                scope_t,
                std::optional<wal::sealed_segment_info>>(ctx, scope),
        });
    }

    template<uint32_t pos, typename ctx_t, typename scope_t>
    void
    _front_wal_abort::op::start(ctx_t& ctx, scope_t& scope) {
        sched->schedule_wal_abort(new req{
            .plan_id = plan_id,
            .writes = std::move(writes),
            .cb = core::make_owner_pusher<pos, scope_t, void>(ctx, scope),
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

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_wal_prepare_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_wal_prepare::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<
                apps::inconel::wal::wal_prepare_result>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_wal_install_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_wal_install::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_wal_commit_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_wal_commit::sender> {
        consteval static uint32_t count_value() { return 1; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<
                std::optional<apps::inconel::wal::sealed_segment_info>>{};
        }
    };

    template<uint32_t pos, typename scope_t>
    requires (pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
        && (get_current_op_type_t<pos, scope_t>::front_wal_abort_op)
    struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
        template<typename ctx_t>
        static void push_value(ctx_t& ctx, scope_t& scope) {
            std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
        }
    };

    template<typename ctx_t>
    struct compute_sender_type<
        ctx_t,
        apps::inconel::front::_front_wal_abort::sender> {
        consteval static uint32_t count_value() { return 0; }
        consteval static auto get_value_type_identity() {
            return std::type_identity<void>{};
        }
    };

}  // namespace pump::core

#endif  // APPS_INCONEL_FRONT_SCHEDULER_HH
