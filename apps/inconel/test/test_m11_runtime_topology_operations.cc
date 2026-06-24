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

#include "apps/inconel/core/page_cache.hh"
#include "apps/inconel/format/format_profile.hh"
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

namespace m11_fake_nvme {

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
    constexpr static bool m11_fake_nvme_op = true;
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

}  // namespace m11_fake_nvme

namespace pump::core {

template <uint32_t pos, typename scope_t>
requires(pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::m11_fake_nvme_op)
struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
    template <typename ctx_t>
    static void push_value(ctx_t& ctx, scope_t& scope) {
        std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
    }
};

template <typename ctx_t>
struct compute_sender_type<ctx_t, m11_fake_nvme::_io::sender> {
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
    m11_fake_nvme::scheduler* sched = nullptr;

    m11_fake_nvme::scheduler* operator()() const {
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

template <typename T>
void
expect_wal_error(op_result<T>&& result,
                 wal::wal_append_error_reason reason) {
    CHECK(std::holds_alternative<std::exception_ptr>(result));
    try {
        std::rethrow_exception(std::get<std::exception_ptr>(result));
    } catch (const wal::wal_append_error& e) {
        CHECK(e.reason() == reason);
        return;
    } catch (...) {
    }
    CHECK(false);
}

template <typename Exc, typename Fn>
void
expect_throws(Fn&& fn) {
    bool threw = false;
    try {
        std::forward<Fn>(fn)();
    } catch (const Exc&) {
        threw = true;
    }
    CHECK(threw);
}

struct topology_fixture {
    using value_sched_t = value::value_alloc_sched<core::segmented_clock_cache>;

    registry_scope scope;
    std::vector<uint32_t> cores;
    std::vector<uint32_t> front_hosts;
    std::vector<uint32_t> class_sizes{64, 256, 1024, 4096, 16384};
    core::data_area_heads heads{};
    m11_fake_nvme::scheduler nvme;
    value_sched_t value_sched;
    fake_nvme_provider provider;
    wal::segment_geometry geom;
    core::wal_reclaim_frontier wal_frontier;
    core::tree_geometry tree_geom{
        .lba_size = 4096,
        .tree_page_size = 4096,
        .shadow_slots_per_range = 1,
    };
    runtime::front_topology topology;
    std::vector<m11_fake_nvme::scheduler*> fake_nvme_by_owner;

    topology_fixture(std::vector<uint32_t> cores_in = {0},
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
            .wal_reclaim_frontier = &wal_frontier,
            .tree_geometry = &tree_geom,
            .front_queue_depth = front_queue_depth,
            .coord_queue_depth = coord_queue_depth,
            .coord_ready_window = ready_window,
            .front_wal_config = wal_config,
            .front_wal_dma_allocator = memory::make_heap_dma_page_allocator(),
        });

        fake_nvme_by_owner.assign(topology.fronts.size(), &nvme);
        core::registry::nvme_by_front_owner.assign(
            topology.fronts.size(), nullptr);
    }

    ~topology_fixture() {
        delete topology.coord;
        for (auto* fs : topology.fronts) {
            delete fs;
        }
        delete topology.wal_space;
        topology = {};
        core::registry::clear();
    }

    [[nodiscard]] uint32_t front_count() const {
        return static_cast<uint32_t>(topology.fronts.size());
    }

    [[nodiscard]] std::span<m11_fake_nvme::scheduler* const> nvmes() {
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

    submission<bool>
    submit_wal(write_path::write_batch_state& state) {
        return submit_result<bool>([this, &state]() {
            return write_path::write_batch_wal_phase(
                state,
                core::registry::fronts_span(),
                *topology.wal_space,
                nvmes());
        });
    }

    op_result<bool>
    run_value(write_path::write_batch_state& state) {
        auto sub = submit_result<bool>([this, &state]() {
            return write_path::write_batch_value_phase(state, provider);
        });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    bool advance_all(bool advance_nvme = true) {
        bool progress = false;
        progress |= topology.coord->advance();
        progress |= value_sched.advance();
        for (auto* fs : topology.fronts) {
            progress |= fs->advance();
        }
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

    template <typename Pred>
    void drive_until(Pred&& pred,
                     bool advance_nvme = true,
                     uint32_t limit = 200000) {
        for (uint32_t i = 0; !pred() && i < limit; ++i) {
            const bool progress = advance_all(advance_nvme);
            if (!progress) std::this_thread::yield();
        }
        CHECK(pred());
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

    [[nodiscard]] bool active_tables_empty() const {
        for (auto* fs : topology.fronts) {
            if (!fs->active_for_testing()->table.empty()) return false;
        }
        return true;
    }
};

void expect_found(const pipeline::point_get_result& r,
                  std::string_view body) {
    CHECK(r.found);
    CHECK(r.value == body);
}

void expect_not_found(const pipeline::point_get_result& r) {
    CHECK(!r.found);
    CHECK(r.value.empty());
}

void m11_front_topology_builds_and_registers() {
    topology_fixture fx({0, 1, 2, 3}, {1, 3}, 0, 2);

    CHECK(core::registry::coord_sched_singleton() == fx.topology.coord);
    CHECK(core::registry::wal_space_singleton() == fx.topology.wal_space);
    CHECK(core::registry::front_count() == 2);
    auto fronts = core::registry::fronts_span();
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(core::registry::front_at(i) == fronts[i]);
        CHECK(fronts[i] == fx.topology.fronts[i]);
    }
    CHECK(core::registry::front_scheds.by_core[1] == fronts[0]);
    CHECK(core::registry::front_scheds.by_core[3] == fronts[1]);

    auto rh = fx.topology.coord->acquire_read_handle_for_testing();
    CHECK(rh.cat->durable_lsn.load(std::memory_order_acquire) == 0);
    CHECK(rh.cat->epoch == 1);
    CHECK(rh.cat->prs->epoch == 1);
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK((*rh.cat->prs->fronts)[i].active ==
              fronts[i]->active_for_testing());
    }

    auto ctx = fx.topology.coord->assign_batch_lsn_for_testing(
        fx.make_input({
            {.op = core::write_op_type::del, .key = "first", .value = ""},
        }));
    CHECK(ctx.batch_lsn == 1);
}

void m11_front_list_by_core_stable() {
    topology_fixture fx({0, 1, 2, 3}, {1, 3}, 0, 2);
    auto a = core::registry::fronts_span();
    auto b = core::registry::fronts_span();
    CHECK(a.data() == b.data());
    CHECK(a.size() == b.size());
    CHECK(core::registry::front_scheds.list[0]->owner_id() == 0);
    CHECK(core::registry::front_scheds.list[1]->owner_id() == 1);
    CHECK(core::registry::front_scheds.by_core[0] == nullptr);
    CHECK(core::registry::front_scheds.by_core[2] == nullptr);
}

void m11_ops_point_get_end_to_end() {
    topology_fixture fx({0}, {}, 0, 0);
    const std::string live = key_for_owner(0, fx.front_count(), "live");
    const std::string dead = key_for_owner(0, fx.front_count(), "dead");
    const std::string missing = key_for_owner(0, fx.front_count(), "missing");

    (void)expect_ok<write_path::write_batch_result>(fx.run_l3_write({
        {.op = core::write_op_type::put, .key = live, .value = "body"},
    }));
    (void)expect_ok<write_path::write_batch_result>(fx.run_l3_write({
        {.op = core::write_op_type::put, .key = dead, .value = "gone"},
    }));
    (void)expect_ok<write_path::write_batch_result>(fx.run_l3_write({
        {.op = core::write_op_type::del, .key = dead, .value = ""},
    }));

    expect_found(
        expect_ok<pipeline::point_get_result>(fx.run_rt_point_get(live)),
        "body");
    expect_not_found(
        expect_ok<pipeline::point_get_result>(fx.run_rt_point_get(dead)));
    expect_not_found(
        expect_ok<pipeline::point_get_result>(fx.run_rt_point_get(missing)));
}

void m11_ops_write_batch_composes_from_registry_without_side_effect() {
    topology_fixture fx({0}, {}, 0, 0);
    const std::string discarded =
        key_for_owner(0, fx.front_count(), "discarded");
    const uint32_t calls_before = fx.nvme.total_calls();
    {
        auto sender = rt::write_batch(
            fx.make_input({
                {.op = core::write_op_type::put,
                 .key = discarded,
                 .value = "discarded"},
            }),
            fx.provider);
        (void)sender;
    }

    CHECK(fx.nvme.total_calls() == calls_before);
    CHECK(fx.visible_lsn() == 0);
    CHECK(fx.active_tables_empty());

    const std::string key = key_for_owner(0, fx.front_count(), "after");
    auto result = expect_ok<write_path::write_batch_result>(fx.run_l3_write({
        {.op = core::write_op_type::put, .key = key, .value = "ok"},
    }));
    CHECK(result.batch_lsn == 1);
    CHECK(fx.visible_lsn() == 1);
}

void m11_wal_pending_prepare_capacity_decoupled() {
    topology_fixture fx(
        {0},
        {},
        0,
        0,
        1024,
        1024,
        64,
        wal::wal_append_config{.pending_prepare_capacity = 2});

    std::vector<std::unique_ptr<write_path::write_batch_state>> states;
    std::vector<submission<bool>> submissions;
    states.reserve(4);
    submissions.reserve(4);

    auto make_delete_state = [&](std::string key) {
        auto state = std::make_unique<write_path::write_batch_state>(
            fx.assign_state({
                {.op = core::write_op_type::del,
                 .key = std::move(key),
                 .value = ""},
            }));
        CHECK(expect_ok<bool>(fx.run_value(*state)));
        return state;
    };

    fx.nvme.hold_call(m11_fake_nvme::op_kind::write, fx.nvme.writes.calls + 1);
    states.push_back(make_delete_state("held-0"));
    submissions.push_back(fx.submit_wal(*states.back()));
    fx.drive_until([&]() { return fx.nvme.has_held(); });
    CHECK(!ready(submissions[0].fut));

    states.push_back(make_delete_state("pending-1"));
    submissions.push_back(fx.submit_wal(*states.back()));
    fx.drive_steps(64, false);
    CHECK(!ready(submissions[1].fut));

    states.push_back(make_delete_state("pending-2"));
    submissions.push_back(fx.submit_wal(*states.back()));
    fx.drive_steps(64, false);
    CHECK(!ready(submissions[2].fut));

    states.push_back(make_delete_state("overflow-3"));
    auto overflow = fx.submit_wal(*states.back());
    fx.drive_until_ready(overflow, false);
    expect_wal_error<bool>(
        overflow.fut.get(),
        wal::wal_append_error_reason::prepare_queue_full);

    CHECK(fx.nvme.release_held());
    for (auto& sub : submissions) {
        fx.drive_until_ready(sub);
        CHECK(expect_ok<bool>(sub.fut.get()));
    }
}

void m11_topology_validation_failures() {
    auto valid_geom = make_geom();
    core::tree_geometry tree_geom{
        .lba_size = 4096,
        .tree_page_size = 4096,
        .shadow_slots_per_range = 1,
    };
    std::vector<uint32_t> cores{0, 1};
    core::wal_reclaim_frontier wal_frontier;
    auto expect_invalid = [&](runtime::front_topology_options opts) {
        registry_scope scope(4);
        expect_throws<std::invalid_argument>([&]() {
            (void)runtime::build_front_topology(opts);
        });
        CHECK(core::registry::front_scheds.list.empty());
        CHECK(core::registry::coord_sched_singleton_ptr == nullptr);
        CHECK(core::registry::wal_space_sched_singleton_ptr == nullptr);
    };

    expect_invalid(runtime::front_topology_options{
        .cores = cores,
        .front_cores = std::span<const uint32_t>(),
        .coord_core = 3,
        .wal_space_core = -1,
        .wal_geometry = valid_geom,
        .wal_reclaim_frontier = &wal_frontier,
        .tree_geometry = &tree_geom,
    });

    std::vector<uint32_t> bad_front{2};
    expect_invalid(runtime::front_topology_options{
        .cores = cores,
        .front_cores = bad_front,
        .wal_geometry = valid_geom,
        .wal_reclaim_frontier = &wal_frontier,
        .tree_geometry = &tree_geom,
    });

    std::vector<uint32_t> dup_front{1, 1};
    expect_invalid(runtime::front_topology_options{
        .cores = cores,
        .front_cores = dup_front,
        .wal_geometry = valid_geom,
        .wal_reclaim_frontier = &wal_frontier,
        .tree_geometry = &tree_geom,
    });

    expect_invalid(runtime::front_topology_options{
        .cores = cores,
        .wal_space_core = 3,
        .wal_geometry = valid_geom,
        .wal_reclaim_frontier = &wal_frontier,
        .tree_geometry = &tree_geom,
    });

    expect_invalid(runtime::front_topology_options{
        .cores = cores,
        .wal_geometry = valid_geom,
        .wal_reclaim_frontier = &wal_frontier,
        .tree_geometry = &tree_geom,
        .front_queue_depth = 3,
    });

    expect_invalid(runtime::front_topology_options{
        .cores = cores,
        .wal_geometry = valid_geom,
        .wal_reclaim_frontier = &wal_frontier,
        .tree_geometry = &tree_geom,
        .coord_queue_depth = 1,
    });
}

void m11_operations_header_self_contained() {
    CHECK(core::registry::front_count() == 0);
}

void m11_profile_wal_fields_validated() {
    CHECK(format::profile_is_self_consistent(
        format::kBootstrapFormatProfile));

    auto p = format::kBootstrapFormatProfile;
    p.wal_segment_size = p.lba_size + 1;
    CHECK(!format::profile_is_self_consistent(p));

    p = format::kBootstrapFormatProfile;
    p.wal_segment_count = 0;
    CHECK(!format::profile_is_self_consistent(p));

    p = format::kBootstrapFormatProfile;
    p.wal_base_paddr.lba = p.value_data_area_base.lba;
    CHECK(!format::profile_is_self_consistent(p));

    p = format::kBootstrapFormatProfile;
    p.wal_segment_size = p.lba_size;
    CHECK(!format::profile_is_self_consistent(p));
}

}  // namespace

int main() {
    m11_front_topology_builds_and_registers();
    m11_front_list_by_core_stable();
    m11_ops_point_get_end_to_end();
    m11_ops_write_batch_composes_from_registry_without_side_effect();
    m11_wal_pending_prepare_capacity_decoupled();
    m11_topology_validation_failures();
    m11_operations_header_self_contained();
    m11_profile_wal_fields_validated();
    return 0;
}
