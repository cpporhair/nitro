#include "apps/inconel/runtime/operations.hh"
#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
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
#include "apps/inconel/core/data_area_heads.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/memtable_lookup.hh"
#include "apps/inconel/core/page_cache.hh"
#include "apps/inconel/core/read_catalog.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/tree_geometry.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/front/sender.hh"
#include "apps/inconel/nvme/frame_io.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/value/sender.hh"
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

namespace m12_fake_nvme {

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
    constexpr static bool m12_fake_nvme_op = true;
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
        while (!held_.empty()) held_.pop_front();
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
        if (r->op == op_kind::write &&
            (r->flags & nvme::IO_FLAGS_FUA) != 0) {
            ++fua_writes;
        }
        pending_.push_back(r);
    }

    void hold_call(op_kind kind, uint32_t call_index) {
        hold_kind_ = kind;
        hold_call_ = call_index;
    }

    [[nodiscard]] bool has_held() const noexcept {
        return !held_.empty();
    }

    bool release_held() {
        if (held_.empty()) return false;
        std::unique_ptr<_io::req> req = std::move(held_.front());
        held_.pop_front();
        return complete_request(*req);
    }

    bool advance_one() {
        if (pending_.empty()) return false;
        std::unique_ptr<_io::req> req(pending_.front());
        pending_.pop_front();

        auto& st = stats_for(req->op);
        CHECK(st.active > 0);
        --st.active;

        if (hold_call_ != 0 && req->op == hold_kind_ &&
            req->call_index == hold_call_) {
            hold_call_ = 0;
            held_.push_back(std::move(req));
            return true;
        }

        return complete_request(*req);
    }

    [[nodiscard]] bool idle() const noexcept {
        return pending_.empty() && held_.empty() &&
               reads.active == 0 &&
               writes.active == 0 &&
               trims.active == 0;
    }

    [[nodiscard]] uint32_t total_calls() const noexcept {
        return reads.calls + writes.calls + trims.calls;
    }

  private:
    using disk_key = std::tuple<uint16_t, uint64_t, uint16_t>;

    std::deque<_io::req*> pending_;
    std::deque<std::unique_ptr<_io::req>> held_;
    std::map<disk_key, std::vector<char>> disk_;
    op_kind hold_kind_ = op_kind::write;
    uint32_t hold_call_ = 0;

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

    bool complete_request(_io::req& req) {
        switch (req.op) {
        case op_kind::write:
            store_frame(*req.frame);
            req.cb(true);
            return true;
        case op_kind::read:
            req.cb(load_frame(*req.frame));
            return true;
        case op_kind::trim:
            trim_disk_range(req.lba, req.num_lbas);
            req.cb(true);
            return true;
        }
        CHECK(false);
        return true;
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

}  // namespace m12_fake_nvme

namespace pump::core {

template <uint32_t pos, typename scope_t>
requires(pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::m12_fake_nvme_op)
struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
    template <typename ctx_t>
    static void push_value(ctx_t& ctx, scope_t& scope) {
        std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
    }
};

template <typename ctx_t>
struct compute_sender_type<ctx_t, m12_fake_nvme::_io::sender> {
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
    m12_fake_nvme::scheduler* sched = nullptr;

    m12_fake_nvme::scheduler* operator()() const {
        CHECK(sched != nullptr);
        return sched;
    }
};

struct registry_scope {
    explicit registry_scope(uint32_t max_cores = 8) {
        pump::core::this_core_id = 0;
        core::registry::clear();
        core::registry::init_capacity(max_cores);
    }

    ~registry_scope() {
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
        >> pump::sender::then([promise, caught](auto&& value) mutable {
            if (*caught) {
                promise->set_value(*caught);
            } else {
                promise->set_value(T(std::move(value)));
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

template <typename Exc, typename T>
void
expect_error(op_result<T>&& result) {
    CHECK(std::holds_alternative<std::exception_ptr>(result));
    try {
        std::rethrow_exception(std::get<std::exception_ptr>(result));
    } catch (const Exc&) {
        return;
    } catch (...) {
    }
    CHECK(false);
}

void
expect_found(const pipeline::point_get_result& r, std::string_view body) {
    CHECK(r.found);
    CHECK(r.value == body);
}

std::shared_ptr<const core::publish_catalog>
build_cat_from_fronts(std::shared_ptr<const core::publish_catalog> old_cat,
                      std::vector<core::front_read_set> fronts) {
    const uint64_t epoch = old_cat->epoch + 1;
    auto prs_fronts =
        std::make_shared<const std::vector<core::front_read_set>>(
            std::move(fronts));
    auto prs = std::make_shared<const core::published_read_set>(
        core::published_read_set{
            .tree_guard = old_cat->prs->tree_guard,
            .fronts = std::move(prs_fronts),
            .epoch = epoch,
        });
    const uint64_t durable =
        old_cat->durable_lsn.load(std::memory_order_acquire);
    return std::make_shared<const core::publish_catalog>(
        std::move(prs), durable, epoch);
}

struct topology_fixture {
    using value_sched_t = value::value_alloc_sched<core::segmented_clock_cache>;

    registry_scope scope;
    std::vector<uint32_t> cores;
    std::vector<uint32_t> front_hosts;
    std::vector<uint32_t> class_sizes{64, 256, 1024, 4096, 16384};
    core::data_area_heads heads{};
    m12_fake_nvme::scheduler nvme;
    value_sched_t value_sched;
    fake_nvme_provider provider;
    wal::segment_geometry geom;
    core::tree_geometry tree_geom{
        .lba_size = 4096,
        .tree_page_size = 4096,
        .shadow_slots_per_range = 1,
    };
    runtime::front_topology topology;
    std::vector<m12_fake_nvme::scheduler*> fake_nvme_by_owner;

    topology_fixture(std::vector<uint32_t> cores_in = {0, 1},
                     std::vector<uint32_t> front_hosts_in = {},
                     int32_t coord_core = -1,
                     int32_t wal_space_core = -1,
                     std::size_t front_queue_depth = 1024,
                     std::size_t coord_queue_depth = 1024,
                     std::size_t ready_window = 64,
                     wal::wal_append_config wal_config =
                         { .pending_prepare_capacity = 64 },
                     wal::segment_geometry geometry = make_geom())
        : scope(8)
        , cores(std::move(cores_in))
        , front_hosts(std::move(front_hosts_in))
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
        , geom(geometry) {
        core::registry::value_alloc_sched = &value_sched;
        topology = runtime::build_front_topology(runtime::front_topology_options{
            .cores = std::span<const uint32_t>(cores.data(), cores.size()),
            .front_cores = std::span<const uint32_t>(
                front_hosts.data(), front_hosts.size()),
            .coord_core = coord_core,
            .wal_space_core = wal_space_core,
            .wal_geometry = geom,
            .tree_geometry = &tree_geom,
            .front_queue_depth = front_queue_depth,
            .coord_queue_depth = coord_queue_depth,
            .coord_ready_window = ready_window,
            .front_wal_config = wal_config,
        });

        fake_nvme_by_owner.assign(topology.fronts.size(), &nvme);
        core::registry::nvme_by_front_owner.assign(
            topology.fronts.size(), nullptr);
    }

    ~topology_fixture() {
        delete topology.coord;
        for (auto* fs : topology.fronts) delete fs;
        delete topology.wal_space;
        topology = {};
        core::registry::clear();
    }

    [[nodiscard]] uint32_t front_count() const {
        return static_cast<uint32_t>(topology.fronts.size());
    }

    [[nodiscard]] std::span<m12_fake_nvme::scheduler* const> nvmes() {
        return {fake_nvme_by_owner.data(), fake_nvme_by_owner.size()};
    }

    [[nodiscard]] core::client_batch_buffer
    make_input(std::vector<core::raw_batch_op> ops) const {
        return core::encode_client_batch(op_span(ops));
    }

    [[nodiscard]] auto compose_l3_write(core::client_batch_buffer input) {
        return write_path::write_batch(
            *topology.coord,
            core::registry::fronts_span(),
            *topology.wal_space,
            nvmes(),
            std::move(input),
            provider);
    }

    submission<write_path::write_batch_result>
    submit_l3_write(core::client_batch_buffer input) {
        return submit_result<write_path::write_batch_result>(
            [this, input = std::move(input)]() mutable {
                return compose_l3_write(std::move(input));
            });
    }

    op_result<write_path::write_batch_result>
    run_l3_write(std::vector<core::raw_batch_op> ops) {
        auto sub = submit_l3_write(make_input(std::move(ops)));
        drive_until_ready(sub);
        return sub.fut.get();
    }

    submission<pipeline::point_get_result>
    submit_rt_point_get(std::string_view key) {
        return submit_result<pipeline::point_get_result>(
            [this, key]() mutable {
                return rt::point_get(key, provider);
            });
    }

    op_result<pipeline::point_get_result>
    run_rt_point_get(std::string_view key) {
        auto sub = submit_rt_point_get(key);
        drive_until_ready(sub);
        return sub.fut.get();
    }

    submission<pipeline::seal_round_result>
    submit_rt_seal() {
        return submit_result<pipeline::seal_round_result>(
            []() { return rt::seal_once(); });
    }

    op_result<pipeline::seal_round_result>
    run_rt_seal() {
        auto sub = submit_rt_seal();
        drive_until_ready(sub);
        return sub.fut.get();
    }

    write_path::write_batch_state
    assign_state(std::vector<core::raw_batch_op> ops) {
        auto input = make_input(std::move(ops));
        auto sub = submit_result<core::batch_ctx>(
            [this, input = std::move(input)]() mutable {
                return coord::assign_batch_lsn(
                    *topology.coord, std::move(input));
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

    void run_value(write_path::write_batch_state& state) {
        CHECK(expect_ok<bool>(run_bool_result([&]() {
            return write_path::write_batch_value_phase(state, provider);
        })));
    }

    void run_wal(write_path::write_batch_state& state) {
        CHECK(expect_ok<bool>(run_bool_result([&]() {
            return write_path::write_batch_wal_phase(
                state,
                core::registry::fronts_span(),
                *topology.wal_space,
                nvmes());
        })));
    }

    void run_memtable(write_path::write_batch_state& state) {
        CHECK(expect_ok<bool>(run_bool_result([&]() {
            return write_path::write_batch_memtable_phase(
                state, core::registry::fronts_span());
        })));
    }

    void run_publish(write_path::write_batch_state& state) {
        CHECK(expect_ok<bool>(run_bool_result([&]() {
            return write_path::write_batch_publish(*topology.coord, state);
        })));
    }

    std::shared_ptr<const core::publish_catalog> run_close_gate() {
        auto sub =
            submit_result<std::shared_ptr<const core::publish_catalog>>(
                [this]() { return coord::close_gate(*topology.coord); });
        drive_until_ready(sub);
        return expect_ok<std::shared_ptr<const core::publish_catalog>>(
            sub.fut.get());
    }

    op_result<std::shared_ptr<const core::publish_catalog>>
    try_close_gate() {
        auto sub =
            submit_result<std::shared_ptr<const core::publish_catalog>>(
                [this]() { return coord::close_gate(*topology.coord); });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    op_result<bool> run_open_gate_result() {
        return run_bool_result([this]() {
            return coord::open_gate(*topology.coord)
                >> pump::sender::then([]() { return true; });
        });
    }

    void run_install_cat(
        std::shared_ptr<const core::publish_catalog> cat) {
        CHECK(expect_ok<bool>(run_bool_result(
            [this, cat = std::move(cat)]() mutable {
                return coord::install_cat(*topology.coord, std::move(cat))
                    >> pump::sender::then([]() { return true; });
            })));
    }

    void run_open_gate() {
        CHECK(expect_ok<bool>(run_open_gate_result()));
    }

    std::vector<core::front_read_set> run_front_seal_active() {
        std::vector<core::front_read_set> out;
        out.reserve(topology.fronts.size());
        for (auto* fs : topology.fronts) {
            auto sub = submit_result<core::front_read_set>(
                [fs]() { return front::seal_active(*fs); });
            drive_until_ready(sub);
            out.push_back(expect_ok<core::front_read_set>(sub.fut.get()));
        }
        return out;
    }

    std::shared_ptr<const core::publish_catalog>
    complete_manual_seal(
        std::shared_ptr<const core::publish_catalog> old_cat) {
        auto front_sets = run_front_seal_active();
        auto cat1 = build_cat_from_fronts(std::move(old_cat),
                                          std::move(front_sets));
        auto cat1_copy = cat1;
        run_install_cat(std::move(cat1));
        run_open_gate();
        return cat1_copy;
    }

    bool advance_all(bool advance_nvme = true) {
        bool progress = false;
        progress |= topology.coord->advance();
        progress |= value_sched.advance();
        for (auto* fs : topology.fronts) progress |= fs->advance();
        progress |= topology.wal_space->advance();
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

    void drive_steps(uint32_t steps, bool advance_nvme = true) {
        for (uint32_t i = 0; i < steps; ++i) {
            const bool progress = advance_all(advance_nvme);
            if (!progress) std::this_thread::yield();
        }
    }

    [[nodiscard]] uint64_t visible_lsn() const {
        return topology.coord->acquire_read_handle_for_testing().read_lsn;
    }

    [[nodiscard]] core::memtable_lookup_result
    lookup_visible(std::string_view key) const {
        const auto rh = topology.coord->acquire_read_handle_for_testing();
        const auto owner = owner_for_key(key, front_count());
        return topology.fronts[owner]->lookup_memtable_for_testing(
            key, rh.read_lsn, (*rh.cat->prs->fronts)[owner]);
    }

    [[nodiscard]] bool active_tables_empty() const {
        for (auto* fs : topology.fronts) {
            if (!fs->active_for_testing()->table.empty()) return false;
        }
        return true;
    }
};

void m12_seal_round_installs_cat1_and_preserves_readers() {
    topology_fixture fx({0, 1}, {}, 0, 0);
    const std::string k0 = key_for_owner(0, fx.front_count(), "m12-a");
    const std::string k1 = key_for_owner(1, fx.front_count(), "m12-b");

    (void)expect_ok<write_path::write_batch_result>(fx.run_l3_write({
        {.op = core::write_op_type::put, .key = k0, .value = "v0"},
    }));
    (void)expect_ok<write_path::write_batch_result>(fx.run_l3_write({
        {.op = core::write_op_type::put, .key = k1, .value = "v1"},
    }));
    CHECK(fx.visible_lsn() == 2);

    auto old = fx.topology.coord->acquire_read_handle_for_testing();
    auto cat0 = old.cat;
    auto old_guard = cat0->prs->tree_guard;
    std::vector<std::shared_ptr<core::memtable_gen>> old_active;
    for (const auto& frs : *cat0->prs->fronts) old_active.push_back(frs.active);

    auto seal = expect_ok<pipeline::seal_round_result>(fx.run_rt_seal());
    auto cat1 = seal.cat1;
    CHECK(cat1 != nullptr);
    CHECK(cat1->epoch == cat0->epoch + 1);
    CHECK(cat1->prs->epoch == cat1->epoch);
    CHECK(cat1->durable_lsn.load(std::memory_order_acquire) == 2);
    CHECK(cat1->prs->tree_guard == old_guard);

    auto newer = fx.topology.coord->acquire_read_handle_for_testing();
    CHECK(newer.cat == cat1);
    CHECK(newer.read_lsn == 2);
    CHECK(old.cat == cat0);
    CHECK(old.read_lsn == 2);

    for (uint32_t i = 0; i < fx.front_count(); ++i) {
        const auto& frs = (*cat1->prs->fronts)[i];
        CHECK(frs.active == fx.topology.fronts[i]->active_for_testing());
        CHECK(frs.active != old_active[i]);
        CHECK(!frs.imms.empty());
        CHECK(frs.imms[0] == old_active[i]);
        CHECK(!fx.topology.fronts[i]->imms_for_testing().empty());
        CHECK(fx.topology.fronts[i]->imms_for_testing()[0] == old_active[i]);
    }

    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_rt_point_get(k0)),
                 "v0");
    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_rt_point_get(k1)),
                 "v1");
}

void m12_publish_parked_while_gate_closed_lands_on_cat1() {
    topology_fixture fx({0, 1}, {}, 0, 0);
    const std::string pre = key_for_owner(0, fx.front_count(), "pre");
    const std::string x = key_for_owner(1, fx.front_count(), "parked");

    (void)expect_ok<write_path::write_batch_result>(fx.run_l3_write({
        {.op = core::write_op_type::put, .key = pre, .value = "pre"},
    }));
    CHECK(fx.visible_lsn() == 1);

    auto state = fx.assign_state({
        {.op = core::write_op_type::put, .key = x, .value = "x"},
    });
    fx.run_value(state);
    fx.run_wal(state);
    fx.run_memtable(state);
    CHECK(state.phase == write_path::write_batch_phase::memtable_applied);

    auto cat0 = fx.run_close_gate();
    const uint64_t d0 = cat0->durable_lsn.load(std::memory_order_acquire);
    CHECK(d0 == 1);

    fx.run_publish(state);
    CHECK(state.phase == write_path::write_batch_phase::published);
    CHECK(cat0->durable_lsn.load(std::memory_order_acquire) == d0);

    auto cat1 = fx.complete_manual_seal(cat0);
    CHECK(cat0->durable_lsn.load(std::memory_order_acquire) == d0);
    CHECK(cat1->durable_lsn.load(std::memory_order_acquire) ==
          state.ctx.batch_lsn);
    auto newer = fx.topology.coord->acquire_read_handle_for_testing();
    CHECK(newer.cat == cat1);
    CHECK(newer.read_lsn == state.ctx.batch_lsn);
}

void m12_concurrent_seal_once_second_fails_fast() {
    topology_fixture fx({0, 1}, {}, 0, 0);

    expect_error<std::logic_error, bool>(fx.run_open_gate_result());
    CHECK(fx.topology.coord->gate_open_for_testing());

    auto cat0 = fx.run_close_gate();
    CHECK(!fx.topology.coord->gate_open_for_testing());
    expect_error<std::logic_error,
                 std::shared_ptr<const core::publish_catalog>>(
        fx.try_close_gate());
    CHECK(!fx.topology.coord->gate_open_for_testing());

    (void)fx.complete_manual_seal(cat0);
    CHECK(fx.topology.coord->gate_open_for_testing());

    auto seal = expect_ok<pipeline::seal_round_result>(fx.run_rt_seal());
    CHECK(seal.cat1 != nullptr);
    CHECK(fx.topology.coord->gate_open_for_testing());
}

void m12_seal_round_compose_without_submit_no_side_effect() {
    topology_fixture fx({0, 1}, {}, 0, 0);
    auto before = fx.topology.coord->acquire_read_handle_for_testing();
    const uint32_t calls_before = fx.nvme.total_calls();
    std::vector<std::shared_ptr<core::memtable_gen>> active_before;
    for (auto* fs : fx.topology.fronts) {
        active_before.push_back(fs->active_for_testing());
    }

    {
        auto sender = rt::seal_once();
        (void)sender;
    }

    auto after = fx.topology.coord->acquire_read_handle_for_testing();
    CHECK(after.cat == before.cat);
    CHECK(after.read_lsn == before.read_lsn);
    CHECK(fx.topology.coord->gate_open_for_testing());
    CHECK(fx.nvme.total_calls() == calls_before);
    CHECK(fx.active_tables_empty());
    for (uint32_t i = 0; i < fx.front_count(); ++i) {
        CHECK(fx.topology.fronts[i]->active_for_testing() ==
              active_before[i]);
        CHECK(fx.topology.fronts[i]->imms_for_testing().empty());
    }
}

}  // namespace

int main() {
    m12_seal_round_installs_cat1_and_preserves_readers();
    m12_publish_parked_while_gate_closed_lands_on_cat1();
    m12_concurrent_seal_once_second_fails_fast();
    m12_seal_round_compose_without_submit_no_side_effect();
    return 0;
}
