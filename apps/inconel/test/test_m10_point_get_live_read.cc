#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/coord/sender.hh"
#include "apps/inconel/core/batch_carrier.hh"
#include "apps/inconel/core/checkpoint_guard.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/page_cache.hh"
#include "apps/inconel/core/read_catalog.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/shard_partition_builder.hh"
#include "apps/inconel/core/tree_geometry.hh"
#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/core/tree_read_domain.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/format/wal.hh"
#include "apps/inconel/front/sender.hh"
#include "apps/inconel/pipeline/point_get.hh"
#include "apps/inconel/tree/page_builder.hh"
#include "apps/inconel/value/sender.hh"
#include "apps/inconel/wal/sender.hh"
#include "apps/inconel/write_path/write_batch.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/context.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

using namespace apps::inconel;

namespace m10_fake_nvme {

enum class op_kind {
    read,
    write,
    trim,
};

class scheduler;

namespace _io {

struct req {
    op_kind op = op_kind::read;
    memory::segmented_page_frame* frame = nullptr;
    uint32_t flags = 0;
    uint64_t lba = 0;
    uint32_t num_lbas = 0;
    uint32_t call_index = 0;
    std::move_only_function<void(bool)> cb;
    std::move_only_function<void(std::exception_ptr)> fail;
};

struct op {
    constexpr static bool m10_fake_nvme_op = true;
    scheduler* sched = nullptr;
    op_kind kind = op_kind::read;
    memory::segmented_page_frame* frame = nullptr;
    uint32_t flags = 0;
    uint64_t lba = 0;
    uint32_t num_lbas = 0;

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void start(ctx_t& ctx, scope_t& scope);
};

struct sender {
    scheduler* sched = nullptr;
    op_kind kind = op_kind::read;
    memory::segmented_page_frame* frame = nullptr;
    uint32_t flags = 0;
    uint64_t lba = 0;
    uint32_t num_lbas = 0;

    auto make_op() {
        return op{
            .sched = sched,
            .kind = kind,
            .frame = frame,
            .flags = flags,
            .lba = lba,
            .num_lbas = num_lbas,
        };
    }

    template <typename ctx_t>
    auto connect() {
        return pump::core::builder::op_list_builder<0>().push_back(make_op());
    }
};

}  // namespace _io

struct op_stats {
    uint32_t calls = 0;
    uint32_t active = 0;
    uint32_t max_active = 0;
};

class scheduler {
public:
    uint32_t fua_writes = 0;
    op_stats reads;
    op_stats writes;
    op_stats trims;

    ~scheduler() {
        while (!pending_.empty()) {
            delete pending_.front();
            pending_.pop_front();
        }
    }

    auto read_frame(memory::segmented_page_frame* frame,
                    uint32_t flags = 0) {
        return _io::sender{
            .sched = this,
            .kind = op_kind::read,
            .frame = frame,
            .flags = flags,
        };
    }

    auto write_frame(memory::segmented_page_frame* frame, uint32_t flags) {
        return _io::sender{
            .sched = this,
            .kind = op_kind::write,
            .frame = frame,
            .flags = flags,
        };
    }

    auto trim(uint64_t lba, uint32_t num_lbas) {
        return _io::sender{
            .sched = this,
            .kind = op_kind::trim,
            .lba = lba,
            .num_lbas = num_lbas,
        };
    }

    void schedule(_io::req* r) {
        auto& st = stats_for(r->op);
        r->call_index = ++st.calls;
        ++st.active;
        st.max_active = std::max(st.max_active, st.active);
        if (r->op == op_kind::write &&
            (r->flags & nvme::IO_FLAGS_FUA) != 0) {
            ++fua_writes;
        }
        pending_.push_back(r);
    }

    bool advance_one() {
        if (pending_.empty()) return false;
        std::unique_ptr<_io::req> req(pending_.front());
        pending_.pop_front();

        auto& st = stats_for(req->op);
        CHECK(st.active > 0);
        --st.active;

        switch (req->op) {
        case op_kind::write:
            store_frame(*req->frame);
            req->cb(true);
            return true;
        case op_kind::read:
            req->cb(load_frame(*req->frame));
            return true;
        case op_kind::trim:
            trim_disk_range(req->lba, req->num_lbas);
            req->cb(true);
            return true;
        }
        CHECK(false);
        return true;
    }

    [[nodiscard]] bool idle() const noexcept {
        return pending_.empty() &&
               reads.active == 0 &&
               writes.active == 0 &&
               trims.active == 0;
    }

    [[nodiscard]] uint32_t total_calls() const noexcept {
        return reads.calls + writes.calls + trims.calls;
    }

    void copy_disk_from(const scheduler& other) {
        disk_ = other.disk_;
    }

    [[nodiscard]] std::vector<char>
    segment_bytes(const wal::segment_geometry& geom,
                  wal::segment_id id) const {
        const auto base = wal::segment_base_paddr(geom, id);
        const uint64_t segment_lbas = geom.wal_segment_size / geom.lba_size;
        std::vector<char> out(geom.wal_segment_size, char{0});

        for (const auto& [key, bytes] : disk_) {
            const auto [device_id, lba, span_lbas] = key;
            if (device_id != base.device_id) continue;
            if (lba < base.lba || lba >= base.lba + segment_lbas) continue;

            const uint64_t offset_lbas = lba - base.lba;
            const uint64_t offset_bytes = offset_lbas * geom.lba_size;
            CHECK(offset_bytes + bytes.size() <= out.size());
            CHECK(bytes.size() == uint64_t{span_lbas} * geom.lba_size);
            std::memcpy(out.data() + offset_bytes, bytes.data(), bytes.size());
        }

        return out;
    }

private:
    using disk_key = std::tuple<uint16_t, uint64_t, uint16_t>;

    std::deque<_io::req*> pending_;
    std::map<disk_key, std::vector<char>> disk_;

    op_stats& stats_for(op_kind kind) {
        switch (kind) {
        case op_kind::read: return reads;
        case op_kind::write: return writes;
        case op_kind::trim: return trims;
        }
        return reads;
    }

    static disk_key key_for(format::paddr base, uint16_t span_lbas) {
        return {
            static_cast<uint16_t>(base.device_id),
            static_cast<uint64_t>(base.lba),
            span_lbas,
        };
    }

    static disk_key key_for(const memory::segmented_page_frame& frame) {
        return key_for(frame.id.base, frame.id.span_lbas);
    }

    void store_frame(const memory::segmented_page_frame& frame) {
        std::vector<char> bytes(frame.byte_len());
        frame.copy_to_contiguous(bytes.data(), bytes.size());
        disk_[key_for(frame)] = std::move(bytes);
    }

    bool load_frame(memory::segmented_page_frame& frame) const {
        auto it = disk_.find(key_for(frame));
        if (it == disk_.end()) return false;
        frame.copy_from_contiguous(it->second.data(), it->second.size());
        return true;
    }

    void trim_disk_range(uint64_t lba, uint32_t num_lbas) {
        const uint64_t end = lba + num_lbas;
        for (auto it = disk_.begin(); it != disk_.end();) {
            const uint64_t frame_lba = std::get<1>(it->first);
            if (frame_lba >= lba && frame_lba < end) {
                it = disk_.erase(it);
            } else {
                ++it;
            }
        }
    }
};

template <uint32_t pos, typename ctx_t, typename scope_t>
void
_io::op::start(ctx_t& ctx, scope_t& scope) {
    sched->schedule(new req{
        .op = kind,
        .frame = frame,
        .flags = flags,
        .lba = lba,
        .num_lbas = num_lbas,
        .cb = [ctx = ctx, scope = scope](bool ok) mutable {
            pump::core::op_pusher<pos + 1, scope_t>::push_value(
                ctx, scope, ok);
        },
        .fail = [ctx = ctx, scope = scope](std::exception_ptr ep) mutable {
            pump::core::op_pusher<pos + 1, scope_t>::push_exception(
                ctx, scope, std::move(ep));
        },
    });
}

}  // namespace m10_fake_nvme

namespace pump::core {

template <uint32_t pos, typename scope_t>
requires(pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::m10_fake_nvme_op)
struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
    template <typename ctx_t>
    static void push_value(ctx_t& ctx, scope_t& scope) {
        std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
    }
};

template <typename ctx_t>
struct compute_sender_type<ctx_t, m10_fake_nvme::_io::sender> {
    consteval static uint32_t count_value() { return 1; }
    consteval static auto get_value_type_identity() {
        return std::type_identity<bool>{};
    }
};

}  // namespace pump::core

namespace {

constexpr uint32_t kLbaSize = 4096;
constexpr uint32_t kQuantumBytes = 64;
constexpr uint32_t kGroupSizeLbas = (256u * 1024u * 1024u) / kLbaSize;

struct fake_nvme_provider {
    m10_fake_nvme::scheduler* sched = nullptr;

    m10_fake_nvme::scheduler* operator()() const {
        CHECK(sched != nullptr);
        return sched;
    }
};

struct runtime_scope {
    runtime_scope() {
        pump::core::this_core_id = 0;
        core::registry::clear();
        core::registry::init_capacity(1);
    }

    ~runtime_scope() {
        core::registry::clear();
    }
};

wal::segment_geometry
make_geom(uint32_t count = 32,
          uint64_t base_lba = 1000,
          uint32_t segment_size = 65536,
          uint32_t lba_size = kLbaSize) {
    return wal::segment_geometry{
        .wal_base_paddr = format::paddr{.device_id = 0, .lba = base_lba},
        .wal_segment_size = segment_size,
        .lba_size = lba_size,
        .wal_segment_count = count,
        .expected_format_version = format::SUPERBLOCK_FORMAT_VERSION_V1,
    };
}

std::shared_ptr<core::checkpoint_guard>
make_guard() {
    static const core::tree_geometry kGeom{
        .lba_size = 4096,
        .tree_page_size = 4096,
        .shadow_slots_per_range = 1,
    };
    auto guard = std::make_shared<core::checkpoint_guard>();
    guard->manifest =
        std::make_shared<const core::tree_manifest>(
            core::tree_manifest::empty(&kGeom));
    return guard;
}

std::shared_ptr<const core::publish_catalog>
make_cat_from_active(
    const std::vector<std::shared_ptr<core::memtable_gen>>& active,
    uint64_t durable_lsn,
    uint64_t epoch) {
    auto fronts = std::make_shared<std::vector<core::front_read_set>>();
    fronts->reserve(active.size());
    for (const auto& gen : active) {
        fronts->push_back(core::front_read_set{.active = gen, .imms = {}});
    }
    std::shared_ptr<const std::vector<core::front_read_set>> fronts_const =
        fronts;
    auto prs = std::make_shared<core::published_read_set>(
        core::published_read_set{
            .tree_guard = make_guard(),
            .fronts = std::move(fronts_const),
            .epoch = epoch,
        });
    return std::make_shared<core::publish_catalog>(
        std::move(prs), durable_lsn, epoch);
}

std::shared_ptr<const core::publish_catalog>
make_cat_with_manifest(
    const std::vector<std::shared_ptr<core::memtable_gen>>& active,
    std::shared_ptr<const core::tree_manifest> manifest,
    uint64_t durable_lsn,
    uint64_t epoch) {
    auto fronts = std::make_shared<std::vector<core::front_read_set>>();
    fronts->reserve(active.size());
    for (const auto& gen : active) {
        fronts->push_back(core::front_read_set{.active = gen, .imms = {}});
    }
    std::shared_ptr<const std::vector<core::front_read_set>> fronts_const =
        fronts;

    auto guard = std::make_shared<core::checkpoint_guard>();
    guard->manifest = std::move(manifest);
    auto prs = std::make_shared<core::published_read_set>(
        core::published_read_set{
            .tree_guard = std::move(guard),
            .fronts = std::move(fronts_const),
            .epoch = epoch,
        });
    return std::make_shared<core::publish_catalog>(
        std::move(prs), durable_lsn, epoch);
}

std::span<const core::raw_batch_op>
op_span(const std::vector<core::raw_batch_op>& ops) {
    return {ops.data(), ops.size()};
}

std::string
key_for_owner(uint32_t owner,
              uint32_t front_count,
              std::string_view prefix) {
    for (uint32_t i = 0; i < 100000; ++i) {
        std::string key = std::string(prefix) + "-" + std::to_string(i);
        if (static_cast<uint32_t>(core::key_hash(key) % front_count) ==
            owner) {
            return key;
        }
    }
    CHECK(false);
    return {};
}

uint32_t
owner_for_key(std::string_view key, uint32_t front_count) {
    return static_cast<uint32_t>(core::key_hash(key) % front_count);
}

using root_context_t = decltype(pump::core::make_root_context());

template <typename T>
using op_result = std::variant<T, std::exception_ptr>;

template <typename T>
struct submission {
    root_context_t ctx;
    std::future<op_result<T>> fut;
};

template <typename T, typename SenderBuilder>
submission<T>
submit_result(SenderBuilder&& build_sender) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<op_result<T>>>();
    auto fut = promise->get_future();
    auto caught = std::make_shared<std::exception_ptr>();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::any_exception([caught](std::exception_ptr ep) {
            *caught = std::move(ep);
            return pump::sender::just(T{});
        })
        >> pump::sender::then(
            [promise, caught](auto&& value) mutable {
                if (*caught) {
                    promise->set_value(*caught);
                } else {
                    promise->set_value(
                        T(std::move(value)));
                }
            })
        >> pump::sender::submit(ctx);

    return submission<T>{.ctx = std::move(ctx), .fut = std::move(fut)};
}

template <typename Future>
bool ready(Future& fut) {
    return fut.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
}

template <typename T>
T expect_ok(op_result<T>&& result) {
    CHECK(std::holds_alternative<T>(result));
    return std::move(std::get<T>(result));
}

struct write_fixture {
    using value_sched_t = value::value_alloc_sched<core::segmented_clock_cache>;

    std::vector<uint32_t> class_sizes{64, 256, 1024, 4096, 16384};
    uint32_t front_count = 0;
    runtime_scope scope;
    core::data_area_heads heads{};
    m10_fake_nvme::scheduler nvme;
    value_sched_t value_sched;
    value_sched_t* extra_value_sched = nullptr;
    fake_nvme_provider provider;
    wal::segment_geometry geom;
    wal::wal_space_sched wal_space;
    core::tree_geometry tree_geom{
        .lba_size = 4096,
        .tree_page_size = 4096,
        .shadow_slots_per_range = 1,
    };
    std::vector<std::shared_ptr<core::memtable_gen>> active_gens;
    std::vector<std::unique_ptr<front::front_sched>> front_storage;
    std::vector<front::front_sched*> front_ptrs;
    std::vector<m10_fake_nvme::scheduler*> nvme_ptrs;
    std::shared_ptr<const core::publish_catalog> initial_cat;
    std::unique_ptr<coord::coord_sched> coord;
    std::shared_ptr<const core::shard_partition_map> tree_partitions;
    std::unique_ptr<core::tree_read_domain<core::segmented_clock_cache>>
        tree_domain;

    explicit write_fixture(uint32_t front_count_in = 2,
                           std::size_t front_queue_depth = 1024,
                           std::size_t coord_queue_depth = 1024,
                           std::size_t ready_window = 64,
                           wal::wal_append_config wal_config = {},
                           wal::segment_geometry geometry = make_geom())
        : front_count(front_count_in)
        , value_sched(
              std::span<const uint32_t>(class_sizes.data(),
                                        class_sizes.size()),
              kLbaSize,
              format::paddr{0, 10000},
              format::paddr{0, 200000},
              &heads,
              core::segmented_clock_cache(128),
              kQuantumBytes,
              kGroupSizeLbas,
              2048,
              memory::make_heap_dma_page_allocator(),
              4096,
              -1,
              value::value_io_policy{})
        , provider{&nvme}
        , geom(geometry)
        , wal_space(geom, front_count_in) {
        CHECK(front_count > 0);
        core::registry::value_alloc_sched = &value_sched;

        active_gens.reserve(front_count);
        front_storage.reserve(front_count);
        front_ptrs.reserve(front_count);
        nvme_ptrs.reserve(front_count);
        for (uint32_t i = 0; i < front_count; ++i) {
            auto active = front::make_front_memtable_gen(
                i, front_count, 0, core::memtable_gen::state::active);
            active_gens.push_back(active);
            front_storage.push_back(std::make_unique<front::front_sched>(
                i,
                front_count,
                active,
                1,
                geom,
                wal_config,
                front_queue_depth));
            front_ptrs.push_back(front_storage.back().get());
            nvme_ptrs.push_back(&nvme);
        }

        initial_cat = make_cat_from_active(active_gens, 0, 1);
        coord = std::make_unique<coord::coord_sched>(
            initial_cat,
            front_count,
            1,
            ready_window,
            coord_queue_depth);
    }

    [[nodiscard]] std::span<front::front_sched* const> fronts() {
        return {front_ptrs.data(), front_ptrs.size()};
    }

    [[nodiscard]] std::span<m10_fake_nvme::scheduler* const> nvmes() {
        return {nvme_ptrs.data(), nvme_ptrs.size()};
    }

    [[nodiscard]] core::client_batch_buffer
    make_input(std::vector<core::raw_batch_op> ops) const {
        return core::encode_client_batch(op_span(ops));
    }

    [[nodiscard]] auto compose_write(core::client_batch_buffer input) {
        return write_path::write_batch(
            *coord,
            fronts(),
            wal_space,
            nvmes(),
            std::move(input),
            provider);
    }

    [[nodiscard]] auto compose_point_get(std::string_view key) {
        return pipeline::point_get(*coord, fronts(), key, provider);
    }

    submission<write_path::write_batch_result>
    submit_write_buffer(core::client_batch_buffer input) {
        return submit_result<write_path::write_batch_result>(
            [this, input = std::move(input)]() mutable {
                return compose_write(std::move(input));
            });
    }

    submission<write_path::write_batch_result>
    submit_write(std::vector<core::raw_batch_op> ops) {
        return submit_write_buffer(make_input(std::move(ops)));
    }

    submission<pipeline::point_get_result>
    submit_point_get(std::string_view key) {
        return submit_result<pipeline::point_get_result>(
            [this, key]() mutable {
                return compose_point_get(key);
            });
    }

    bool advance_all(bool advance_nvme = true) {
        bool progress = false;
        progress |= coord->advance();
        progress |= value_sched.advance();
        if (extra_value_sched != nullptr) {
            progress |= extra_value_sched->advance();
        }
        if (tree_domain) {
            progress |= tree_domain->advance();
        }
        for (auto& front_sched : front_storage) {
            progress |= front_sched->advance();
        }
        progress |= wal_space.advance();
        if (advance_nvme) progress |= nvme.advance_one();
        return progress;
    }

    template <typename Submission>
    void drive_until_ready(Submission& sub,
                           bool advance_nvme = true,
                           uint32_t limit = 200000) {
        for (uint32_t i = 0; !ready(sub.fut) && i < limit; ++i) {
            const bool progress = advance_all(advance_nvme);
            if (!progress) std::this_thread::yield();
        }
        CHECK(ready(sub.fut));
    }

    op_result<write_path::write_batch_result>
    run_write(std::vector<core::raw_batch_op> ops) {
        auto sub = submit_write(std::move(ops));
        drive_until_ready(sub);
        return sub.fut.get();
    }

    op_result<pipeline::point_get_result>
    run_point_get(std::string_view key) {
        auto sub = submit_point_get(key);
        drive_until_ready(sub);
        return sub.fut.get();
    }

    write_path::write_batch_state
    assign_state(std::vector<core::raw_batch_op> ops) {
        auto input = make_input(std::move(ops));
        auto sub = submit_result<core::batch_ctx>(
            [this, input = std::move(input)]() mutable {
                return coord::assign_batch_lsn(*coord, std::move(input));
            });
        drive_until_ready(sub);
        return write_path::write_batch_state(
            expect_ok<core::batch_ctx>(sub.fut.get()));
    }

    template <typename SenderBuilder>
    op_result<bool>
    run_bool_result(SenderBuilder&& build_sender,
                    bool advance_nvme = true) {
        auto sub = submit_result<bool>(
            std::forward<SenderBuilder>(build_sender));
        drive_until_ready(sub, advance_nvme);
        return sub.fut.get();
    }

    void expect_bool_ok(op_result<bool>&& result) {
        CHECK(std::holds_alternative<bool>(result));
        CHECK(std::get<bool>(result));
    }

    void run_value(write_path::write_batch_state& state) {
        expect_bool_ok(run_bool_result([&]() {
            return write_path::write_batch_value_phase(state, provider);
        }));
    }

    void run_wal(write_path::write_batch_state& state) {
        expect_bool_ok(run_bool_result([&]() {
            return write_path::write_batch_wal_phase(
                state, fronts(), wal_space, nvmes());
        }));
    }

    void run_memtable(write_path::write_batch_state& state) {
        expect_bool_ok(run_bool_result([&]() {
            return write_path::write_batch_memtable_phase(state, fronts());
        }));
    }

    void run_publish(write_path::write_batch_state& state) {
        expect_bool_ok(run_bool_result([&]() {
            return write_path::write_batch_publish(*coord, state);
        }));
    }

    [[nodiscard]] uint64_t visible_lsn() const {
        return coord->acquire_read_handle_for_testing().read_lsn;
    }

    [[nodiscard]] core::memtable_lookup_result
    lookup_visible(std::string_view key) const {
        const auto rh = coord->acquire_read_handle_for_testing();
        const auto owner = owner_for_key(key, front_count);
        return front_storage[owner]->lookup_memtable_for_testing(
            key, rh.read_lsn, (*rh.cat->prs->fronts)[owner]);
    }

    void ensure_tree_domain() {
        if (tree_domain) return;

        core::leaf_order_index empty_order;
        auto partitions = std::make_shared<const core::shard_partition_map>(
            core::build_initial_shard_partition_map(empty_order, 1));
        core::registry::install_shard_partitions(partitions);
        tree_partitions = partitions;

        tree_domain =
            std::make_unique<core::tree_read_domain<
                core::segmented_clock_cache>>(
                0,
                tree_partitions,
                core::segmented_clock_cache(128),
                &tree_geom,
                128,
                memory::make_heap_dma_page_allocator(),
                4096,
                -1);
        core::registry::tree_read_domains.list.push_back(tree_domain.get());
        core::registry::tree_read_domains.by_core[0] = tree_domain.get();
    }

    [[nodiscard]] std::vector<std::shared_ptr<core::memtable_gen>>
    make_empty_active_gens(uint64_t local_epoch = 20) const {
        std::vector<std::shared_ptr<core::memtable_gen>> out;
        out.reserve(front_count);
        for (uint32_t i = 0; i < front_count; ++i) {
            out.push_back(front::make_front_memtable_gen(
                i, front_count, local_epoch, core::memtable_gen::state::active));
        }
        return out;
    }

    void install_tree_cat(
        std::shared_ptr<const core::tree_manifest> manifest,
        const std::vector<std::shared_ptr<core::memtable_gen>>& fronts_snapshot,
        uint64_t durable_lsn,
        uint64_t epoch) {
        coord->install_cat_for_testing(make_cat_with_manifest(
            fronts_snapshot, std::move(manifest), durable_lsn, epoch));
    }
};

const format::value_ref&
expect_value_ref(const core::memtable_lookup_result& result) {
    CHECK(std::holds_alternative<core::memtable_value_hit>(result));
    return std::get<core::memtable_value_hit>(result).durable;
}

struct tree_leaf_fixture {
    std::shared_ptr<const core::tree_manifest> manifest;
    std::vector<char> leaf_image;
};

tree_leaf_fixture
make_tree_leaf_fixture(write_fixture& fx,
                       std::string_view key,
                       std::optional<format::value_ref> vr,
                       uint64_t data_ver) {
    fx.ensure_tree_domain();

    tree_leaf_fixture out;
    out.leaf_image.assign(fx.tree_geom.tree_page_size, char{0});

    tree::leaf_page_builder builder;
    builder.init(out.leaf_image.data(), fx.tree_geom.tree_page_size);
    if (vr.has_value()) {
        CHECK(builder.add_value(key, data_ver, *vr));
    } else {
        CHECK(builder.add_tombstone(key, data_ver));
    }
    builder.finalize();

    const format::paddr root_range_base{0, 500000};
    core::leaf_order_index order;
    order.spans.push_back(core::leaf_span{
        .fence_lower_off = 0,
        .fence_upper_off = 0,
        .fence_lower_len = 0,
        .fence_upper_len = 0,
        .leaf_range_base = root_range_base,
    });

    core::tree_manifest manifest;
    manifest.root_range_base = root_range_base;
    manifest.root_slot = fx.tree_geom.slot_paddr(root_range_base, 0);
    manifest.slot_map.emplace(root_range_base, 0);
    manifest.geom = &fx.tree_geom;
    manifest.leaf_order = std::move(order);
    out.manifest =
        std::make_shared<const core::tree_manifest>(std::move(manifest));
    return out;
}

void
preheat_tree_leaf(write_fixture& fx,
                  std::string_view key,
                  const tree_leaf_fixture& leaf) {
    fx.ensure_tree_domain();

    std::string_view keys[] = {key};
    auto state = tree::make_lookup_state(
        std::span<const std::string_view>(keys, 1), leaf.manifest.get());
    auto* sched = fx.tree_domain->lookup_sched;

    auto read_sub = submit_result<tree::batch_decision>([&]() {
        return sched->process(state);
    });
    fx.drive_until_ready(read_sub);
    auto decision = expect_ok<tree::batch_decision>(read_sub.fut.get());
    CHECK(std::holds_alternative<tree::decision_need_read>(decision));
    auto frames = std::move(std::get<tree::decision_need_read>(decision).frames);
    CHECK(!frames.empty());
    for (auto* frame : frames) {
        frame->copy_from_contiguous(leaf.leaf_image.data(),
                                    leaf.leaf_image.size());
    }

    auto cache_sub = submit_result<bool>(
        [sched, frames = std::move(frames)]() mutable {
            return sched->submit_cache(std::move(frames));
        });
    fx.drive_until_ready(cache_sub);
    CHECK(expect_ok<bool>(cache_sub.fut.get()));

    auto done_sub = submit_result<tree::batch_decision>([&]() {
        return sched->process(state);
    });
    fx.drive_until_ready(done_sub);
    auto done = expect_ok<tree::batch_decision>(done_sub.fut.get());
    CHECK(std::holds_alternative<tree::decision_done>(done));
    CHECK(state.all_done);
    CHECK(state.entries.size() == 1);
    CHECK(state.entries[0].resolved);
}

void
expect_found(const pipeline::point_get_result& r, std::string_view body) {
    CHECK(r.found);
    CHECK(r.value == body);
}

void
expect_not_found(const pipeline::point_get_result& r) {
    CHECK(!r.found);
    CHECK(r.value.empty());
}

void
m10_put_then_point_get_returns_body() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "put-get");
    const std::string body(100, 'p');

    auto wr = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = body},
    }));
    CHECK(wr.batch_lsn == 1);
    CHECK(fx.visible_lsn() == 1);

    const uint32_t reads_before = fx.nvme.reads.calls;
    auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_found(got, body);
    CHECK(fx.nvme.reads.calls == reads_before);
}

void
m10_overwrite_returns_latest() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "overwrite");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "v1"},
    }));
    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "v2"},
    }));
    CHECK(fx.visible_lsn() == 2);

    auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_found(got, "v2");
}

void
m10_delete_returns_not_found() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "delete");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "before-delete"},
    }));
    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::del, .key = key, .value = ""},
    }));

    const uint32_t reads_before = fx.nvme.reads.calls;
    auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_not_found(got);
    CHECK(fx.nvme.reads.calls == reads_before);
}

void
m10_missing_key_not_found_via_empty_tree() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "missing");

    const uint32_t reads_before = fx.nvme.reads.calls;
    auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_not_found(got);
    CHECK(fx.nvme.reads.calls == reads_before);
}

void
m10_cross_front_point_get() {
    write_fixture fx(2);
    const std::string key0 = key_for_owner(0, fx.front_count, "front-zero");
    const std::string key1 = key_for_owner(1, fx.front_count, "front-one");

    auto wr = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key0, .value = "body-zero"},
        {.op = core::write_op_type::put, .key = key1, .value = "body-one"},
    }));
    CHECK(wr.batch_lsn == 1);
    CHECK(wr.entry_count == 2);

    auto got0 = expect_ok<pipeline::point_get_result>(fx.run_point_get(key0));
    auto got1 = expect_ok<pipeline::point_get_result>(fx.run_point_get(key1));
    expect_found(got0, "body-zero");
    expect_found(got1, "body-one");

    const std::string missing0 =
        key_for_owner(0, fx.front_count, "missing-zero");
    const std::string missing1 =
        key_for_owner(1, fx.front_count, "missing-one");
    CHECK(missing0 != key0);
    CHECK(missing1 != key1);
    expect_not_found(
        expect_ok<pipeline::point_get_result>(fx.run_point_get(missing0)));
    expect_not_found(
        expect_ok<pipeline::point_get_result>(fx.run_point_get(missing1)));
}

void
m10_unpublished_write_invisible_then_visible() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "unpublished");

    auto state = fx.assign_state({
        {.op = core::write_op_type::put, .key = key, .value = "parked-body"},
    });
    CHECK(state.ctx.batch_lsn == 1);
    fx.run_value(state);
    fx.run_wal(state);
    CHECK(state.phase == write_path::write_batch_phase::wal_durable);
    CHECK(fx.visible_lsn() == 0);

    auto before = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_not_found(before);

    fx.run_memtable(state);
    fx.run_publish(state);
    CHECK(state.phase == write_path::write_batch_phase::published);
    CHECK(fx.visible_lsn() == 1);

    auto after = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_found(after, "parked-body");
}

void
m10_memtable_hit_reads_value_from_nvme_when_cache_cold() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "cold-cache");
    const std::string body = "cold-cache-body";

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = body},
    }));

    core::data_area_heads cold_heads{};
    write_fixture::value_sched_t cold_value_sched(
        std::span<const uint32_t>(fx.class_sizes.data(), fx.class_sizes.size()),
        kLbaSize,
        format::paddr{0, 10000},
        format::paddr{0, 200000},
        &cold_heads,
        core::segmented_clock_cache(128),
        kQuantumBytes,
        kGroupSizeLbas,
        2048,
        memory::make_heap_dma_page_allocator(),
        4096,
        -1,
        value::value_io_policy{});
    core::registry::value_alloc_sched = &cold_value_sched;
    fx.extra_value_sched = &cold_value_sched;

    const uint32_t reads_before = fx.nvme.reads.calls;
    auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_found(got, body);
    CHECK(fx.nvme.reads.calls > reads_before);
}

void
m10_compose_without_submit_has_no_owner_side_effect() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "compose-only");

    const uint32_t calls_before = fx.nvme.total_calls();
    {
        auto sender = fx.compose_point_get(key);
        (void)sender;
    }

    CHECK(fx.nvme.total_calls() == calls_before);
    CHECK(fx.visible_lsn() == 0);
    CHECK(!fx.advance_all());
}

void
m10_point_get_unaffected_by_closed_gate() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "closed-gate");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "gate-body"},
    }));
    CHECK(fx.visible_lsn() == 1);

    fx.coord->close_gate_for_testing();
    auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_found(got, "gate-body");
    fx.coord->open_gate_for_testing();
}

void
m10_tree_hit_value_via_manifest() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "tree-hit");
    const std::string body = "tree-value-body";

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = body},
    }));
    const auto vr = expect_value_ref(fx.lookup_visible(key));

    auto leaf = make_tree_leaf_fixture(fx, key, vr, 1);
    preheat_tree_leaf(fx, key, leaf);
    CHECK(fx.visible_lsn() == 1);
    fx.install_tree_cat(
        leaf.manifest, fx.make_empty_active_gens(), fx.visible_lsn(), 2);

    auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_found(got, body);
}

void
m10_tree_tombstone_not_found() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "tree-tombstone");
    const std::string seed_key =
        key_for_owner(1, fx.front_count, "tree-tombstone-seed");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = seed_key, .value = "seed"},
    }));
    CHECK(fx.visible_lsn() == 1);

    auto leaf = make_tree_leaf_fixture(fx, key, std::nullopt, fx.visible_lsn());
    preheat_tree_leaf(fx, key, leaf);
    fx.install_tree_cat(
        leaf.manifest, fx.make_empty_active_gens(), fx.visible_lsn(), 2);

    auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
    expect_not_found(got);
}

void
m10_memtable_winner_shadows_tree() {
    {
        write_fixture fx;
        const std::string key =
            key_for_owner(0, fx.front_count, "shadow-delete");
        const std::string body = "tree-shadowed-by-delete";

        (void)expect_ok<write_path::write_batch_result>(fx.run_write({
            {.op = core::write_op_type::put, .key = key, .value = body},
        }));
        const auto vr = expect_value_ref(fx.lookup_visible(key));
        auto leaf = make_tree_leaf_fixture(fx, key, vr, 1);
        preheat_tree_leaf(fx, key, leaf);

        (void)expect_ok<write_path::write_batch_result>(fx.run_write({
            {.op = core::write_op_type::del, .key = key, .value = ""},
        }));
        CHECK(fx.visible_lsn() == 2);
        fx.install_tree_cat(leaf.manifest, fx.active_gens, fx.visible_lsn(), 3);

        auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
        expect_not_found(got);
    }

    {
        write_fixture fx;
        const std::string key =
            key_for_owner(0, fx.front_count, "shadow-put");

        (void)expect_ok<write_path::write_batch_result>(fx.run_write({
            {.op = core::write_op_type::put, .key = key, .value = "tree-v1"},
        }));
        const auto vr = expect_value_ref(fx.lookup_visible(key));
        auto leaf = make_tree_leaf_fixture(fx, key, vr, 1);
        preheat_tree_leaf(fx, key, leaf);

        (void)expect_ok<write_path::write_batch_result>(fx.run_write({
            {.op = core::write_op_type::put, .key = key, .value = "mem-v2"},
        }));
        CHECK(fx.visible_lsn() == 2);
        fx.install_tree_cat(leaf.manifest, fx.active_gens, fx.visible_lsn(), 3);

        auto got = expect_ok<pipeline::point_get_result>(fx.run_point_get(key));
        expect_found(got, "mem-v2");
    }
}

}  // namespace

int
main() {
    m10_put_then_point_get_returns_body();
    m10_overwrite_returns_latest();
    m10_delete_returns_not_found();
    m10_missing_key_not_found_via_empty_tree();
    m10_cross_front_point_get();
    m10_unpublished_write_invisible_then_visible();
    m10_memtable_hit_reads_value_from_nvme_when_cache_cold();
    m10_compose_without_submit_has_no_owner_side_effect();
    m10_point_get_unaffected_by_closed_gate();
    m10_tree_hit_value_via_manifest();
    m10_tree_tombstone_not_found();
    m10_memtable_winner_shadows_tree();
    return 0;
}
