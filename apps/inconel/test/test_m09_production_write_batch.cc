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
#include "apps/inconel/core/batch_carrier.hh"
#include "apps/inconel/core/checkpoint_guard.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/page_cache.hh"
#include "apps/inconel/core/read_catalog.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/tree_geometry.hh"
#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/format/wal.hh"
#include "apps/inconel/front/sender.hh"
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

namespace test_fake_nvme {

enum class op_kind {
    read,
    write,
    trim,
};

enum class held_completion {
    succeed,
    fail_with_false,
    fail_with_exception,
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
    constexpr static bool batch_fake_nvme_op = true;
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
    op_kind fail_kind = op_kind::write;
    uint32_t fail_call = 0;
    bool fail_with_false = false;
    bool fail_with_exception = false;

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

    void hold_call(op_kind kind, uint32_t call_index) {
        hold_kind_ = kind;
        hold_call_ = call_index;
    }

    bool release_held(held_completion completion = held_completion::succeed) {
        if (held_.empty()) return false;
        std::unique_ptr<_io::req> req = std::move(held_.front());
        held_.pop_front();
        return complete_request(*req, completion);
    }

    [[nodiscard]] bool has_held() const noexcept {
        return !held_.empty();
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

        return complete_request(*req, std::nullopt);
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

    void clear_failure() {
        fail_call = 0;
        fail_with_false = false;
        fail_with_exception = false;
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

    bool complete_request(_io::req& req,
                          std::optional<held_completion> forced) {
        if (forced.has_value()) {
            switch (*forced) {
            case held_completion::fail_with_exception:
                req.fail(std::make_exception_ptr(
                    std::runtime_error("fake nvme held exception")));
                return true;
            case held_completion::fail_with_false:
                req.cb(false);
                return true;
            case held_completion::succeed:
                break;
            }
        } else if (fail_call != 0 && req.op == fail_kind &&
                   req.call_index == fail_call) {
            if (fail_with_exception) {
                req.fail(std::make_exception_ptr(
                    std::runtime_error("fake nvme exception")));
                return true;
            }
            if (fail_with_false) {
                req.cb(false);
                return true;
            }
        }

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

}  // namespace test_fake_nvme

namespace pump::core {

template <uint32_t pos, typename scope_t>
requires(pos < std::tuple_size_v<typename scope_t::element_type::op_tuple_type>)
    && (get_current_op_type_t<pos, scope_t>::batch_fake_nvme_op)
struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
    template <typename ctx_t>
    static void push_value(ctx_t& ctx, scope_t& scope) {
        std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
    }
};

template <typename ctx_t>
struct compute_sender_type<ctx_t, test_fake_nvme::_io::sender> {
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
    test_fake_nvme::scheduler* sched = nullptr;

    test_fake_nvme::scheduler* operator()() const {
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

bool
same_value_ref(const format::value_ref& a,
               const format::value_ref& b) {
    return a.base.device_id == b.base.device_id &&
           a.base.lba == b.base.lba &&
           a.byte_offset == b.byte_offset &&
           a.len == b.len &&
           a.flags == b.flags;
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
                        T(std::forward<decltype(value)>(value)));
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

template <typename T>
void
expect_value_error(op_result<T>&& result,
                   value::value_persist_error_reason reason) {
    CHECK(std::holds_alternative<std::exception_ptr>(result));
    try {
        std::rethrow_exception(std::get<std::exception_ptr>(result));
    } catch (const value::value_persist_error& e) {
        CHECK(e.reason() == reason);
        return;
    } catch (...) {
    }
    CHECK(false);
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

struct write_fixture {
    using value_sched_t = value::value_alloc_sched<core::segmented_clock_cache>;

    std::vector<uint32_t> class_sizes{64, 256, 1024, 4096, 16384};
    uint32_t front_count = 0;
    runtime_scope scope;
    core::data_area_heads heads{};
    test_fake_nvme::scheduler nvme;
    value_sched_t value_sched;
    fake_nvme_provider provider;
    wal::segment_geometry geom;
    wal::wal_space_sched wal_space;
    std::vector<std::shared_ptr<core::memtable_gen>> active_gens;
    std::vector<std::unique_ptr<front::front_sched>> front_storage;
    std::vector<front::front_sched*> front_ptrs;
    std::vector<test_fake_nvme::scheduler*> nvme_ptrs;
    std::shared_ptr<const core::publish_catalog> initial_cat;
    std::unique_ptr<coord::coord_sched> coord;

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

    [[nodiscard]] std::span<test_fake_nvme::scheduler* const> nvmes() {
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

    bool advance_all(bool advance_nvme = true) {
        bool progress = false;
        progress |= coord->advance();
        progress |= value_sched.advance();
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

    [[nodiscard]] core::memtable_lookup_result
    lookup_at(std::string_view key, uint64_t read_lsn) const {
        const auto owner = owner_for_key(key, front_count);
        return front_storage[owner]->lookup_memtable_for_testing(
            key, read_lsn, (*initial_cat->prs->fronts)[owner]);
    }
};

struct decoded_entry_copy {
    uint32_t stream_id = 0;
    format::wal_op_type op_type = format::wal_op_type::put;
    uint64_t lsn = 0;
    uint32_t entry_count = 0;
    std::string key;
    std::optional<format::value_ref> vr;
};

format::wal_segment_header
read_segment_header(const std::vector<char>& segment_bytes) {
    CHECK(segment_bytes.size() >= sizeof(format::wal_segment_header));
    format::wal_segment_header raw_header{};
    std::memcpy(&raw_header, segment_bytes.data(), sizeof(raw_header));
    return raw_header;
}

std::vector<decoded_entry_copy>
collect_wal_entries(const write_fixture& fx, uint32_t owner) {
    std::vector<decoded_entry_copy> out;
    for (uint32_t idx = 0; idx < fx.geom.wal_segment_count; ++idx) {
        auto bytes = fx.nvme.segment_bytes(
            fx.geom,
            wal::segment_id{.device_id = fx.geom.wal_base_paddr.device_id,
                            .index = idx});
        const auto header = read_segment_header(bytes);
        if (format::inspect_wal_segment_header(
                header, fx.geom.expected_format_version) !=
            format::wal_segment_status::ok) {
            continue;
        }
        if (header.stream_id != owner) continue;

        uint32_t offset = format::WAL_SEGMENT_HEADER_SIZE;
        const uint32_t limit = wal::segment_usable_end_offset(fx.geom);
        while (offset < limit) {
            uint32_t len = 0;
            format::decoded_wal_entry decoded;
            const auto status = format::decode_wal_entry(
                std::span<const char>{
                    bytes.data() + offset,
                    bytes.size() - offset},
                header.segment_gen,
                &decoded,
                &len);
            if (status != format::wal_entry_decode_status::ok) break;
            CHECK(len > 0);
            out.push_back(decoded_entry_copy{
                .stream_id = header.stream_id,
                .op_type = decoded.op_type,
                .lsn = decoded.lsn,
                .entry_count = decoded.entry_count,
                .key = std::string(decoded.key),
                .vr = decoded.vr,
            });
            offset += len;
        }
    }
    return out;
}

std::vector<decoded_entry_copy>
collect_all_wal_entries(const write_fixture& fx) {
    std::vector<decoded_entry_copy> all;
    for (uint32_t owner = 0; owner < fx.front_count; ++owner) {
        auto entries = collect_wal_entries(fx, owner);
        all.insert(all.end(), entries.begin(), entries.end());
    }
    return all;
}

const decoded_entry_copy&
find_wal_entry(const std::vector<decoded_entry_copy>& entries,
               std::string_view key) {
    for (const auto& entry : entries) {
        if (entry.key == key) return entry;
    }
    CHECK(false);
    return entries[0];
}

const core::memtable_value_hit&
expect_value(const core::memtable_lookup_result& result) {
    CHECK(std::holds_alternative<core::memtable_value_hit>(result));
    return std::get<core::memtable_value_hit>(result);
}

void
expect_tombstone(const core::memtable_lookup_result& result) {
    CHECK(std::holds_alternative<core::memtable_tombstone>(result));
}

void
expect_miss(const core::memtable_lookup_result& result) {
    CHECK(std::holds_alternative<core::memtable_miss>(result));
}

void
expect_visible_value(const write_fixture& fx,
                     std::string_view key,
                     const format::value_ref& expected) {
    const auto hit = expect_value(fx.lookup_visible(key));
    CHECK(same_value_ref(hit.durable, expected));
}

bool
front_memtable_has_key(const write_fixture& fx, std::string_view key) {
    const auto owner = owner_for_key(key, fx.front_count);
    const auto& table = fx.front_storage[owner]->active_for_testing()->table;
    return table.find(key) != table.end();
}

bool
wal_has_key(const write_fixture& fx, std::string_view key) {
    const auto entries = collect_all_wal_entries(fx);
    for (const auto& entry : entries) {
        if (entry.key == key) return true;
    }
    return false;
}

void
expect_memtable_data_ver(const write_fixture& fx,
                         std::string_view key,
                         uint64_t data_ver) {
    const auto owner = owner_for_key(key, fx.front_count);
    const auto& table = fx.front_storage[owner]->active_for_testing()->table;
    auto it = table.find(key);
    CHECK(it != table.end());
    bool found = false;
    for (const auto& entry : it->second) {
        if (entry.data_ver == data_ver) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

void
m09_success_path_matches_baseline_semantics() {
    write_fixture fx(3);
    const std::string update_key = key_for_owner(0, fx.front_count, "update");
    const std::string delete_key = key_for_owner(0, fx.front_count, "gone");
    const std::string other_key = key_for_owner(1, fx.front_count, "other");
    const std::string untouched_key =
        key_for_owner(2, fx.front_count, "untouched");

    auto result = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put,
         .key = update_key,
         .value = "old-value"},
        {.op = core::write_op_type::put,
         .key = delete_key,
         .value = "will-delete"},
        {.op = core::write_op_type::put,
         .key = update_key,
         .value = "new-value"},
        {.op = core::write_op_type::del,
         .key = delete_key,
         .value = ""},
        {.op = core::write_op_type::put,
         .key = other_key,
         .value = "front-one"},
    }));

    CHECK(result.batch_lsn == 1);
    CHECK(result.entry_count == 3);
    CHECK(fx.visible_lsn() == 1);

    const auto wal_entries = collect_all_wal_entries(fx);
    CHECK(wal_entries.size() == result.entry_count);

    const auto& update_wal = find_wal_entry(wal_entries, update_key);
    CHECK(update_wal.stream_id == owner_for_key(update_key, fx.front_count));
    CHECK(update_wal.op_type == format::wal_op_type::put);
    CHECK(update_wal.lsn == 1);
    CHECK(update_wal.entry_count == result.entry_count);
    CHECK(update_wal.vr.has_value());
    CHECK(update_wal.vr->base.lba != 0);
    CHECK(update_wal.vr->len == std::string_view("new-value").size());

    const auto& delete_wal = find_wal_entry(wal_entries, delete_key);
    CHECK(delete_wal.stream_id == owner_for_key(delete_key, fx.front_count));
    CHECK(delete_wal.op_type == format::wal_op_type::del);
    CHECK(delete_wal.lsn == 1);
    CHECK(delete_wal.entry_count == result.entry_count);
    CHECK(!delete_wal.vr.has_value());

    const auto& other_wal = find_wal_entry(wal_entries, other_key);
    CHECK(other_wal.stream_id == owner_for_key(other_key, fx.front_count));
    CHECK(other_wal.op_type == format::wal_op_type::put);
    CHECK(other_wal.lsn == 1);
    CHECK(other_wal.entry_count == result.entry_count);
    CHECK(other_wal.vr.has_value());
    CHECK(other_wal.vr->base.lba != 0);
    CHECK(other_wal.vr->len == std::string_view("front-one").size());
    CHECK(collect_wal_entries(
              fx,
              owner_for_key(untouched_key, fx.front_count)).empty());

    expect_visible_value(fx, update_key, *update_wal.vr);
    expect_tombstone(fx.lookup_visible(delete_key));
    expect_visible_value(fx, other_key, *other_wal.vr);
    expect_miss(fx.lookup_visible(untouched_key));
}

void
m09_concurrent_batches_publish_gap_free() {
    write_fixture fx;
    const std::string key1 = key_for_owner(0, fx.front_count, "batch-a");
    const std::string key2 = key_for_owner(1, fx.front_count, "batch-b");
    const std::string key3 = key_for_owner(0, fx.front_count, "batch-c");

    auto sub1 = fx.submit_write({
        {.op = core::write_op_type::put, .key = key1, .value = "v1"},
    });
    auto sub2 = fx.submit_write({
        {.op = core::write_op_type::put, .key = key2, .value = "v2"},
    });
    auto sub3 = fx.submit_write({
        {.op = core::write_op_type::put, .key = key3, .value = "v3"},
    });

    for (uint32_t i = 0;
         !(ready(sub1.fut) && ready(sub2.fut) && ready(sub3.fut)) &&
         i < 200000;
         ++i) {
        const bool progress = fx.advance_all();
        if (!progress) std::this_thread::yield();
    }
    CHECK(ready(sub1.fut));
    CHECK(ready(sub2.fut));
    CHECK(ready(sub3.fut));

    auto r1 = expect_ok<write_path::write_batch_result>(sub1.fut.get());
    auto r2 = expect_ok<write_path::write_batch_result>(sub2.fut.get());
    auto r3 = expect_ok<write_path::write_batch_result>(sub3.fut.get());

    CHECK(r1.batch_lsn == 1);
    CHECK(r2.batch_lsn == 2);
    CHECK(r3.batch_lsn == 3);
    CHECK(r1.entry_count == 1);
    CHECK(r2.entry_count == 1);
    CHECK(r3.entry_count == 1);
    CHECK(fx.visible_lsn() == 3);

    expect_memtable_data_ver(fx, key1, r1.batch_lsn);
    expect_memtable_data_ver(fx, key2, r2.batch_lsn);
    expect_memtable_data_ver(fx, key3, r3.batch_lsn);

    const auto wal_entries = collect_all_wal_entries(fx);
    CHECK(find_wal_entry(wal_entries, key1).lsn == r1.batch_lsn);
    CHECK(find_wal_entry(wal_entries, key2).lsn == r2.batch_lsn);
    CHECK(find_wal_entry(wal_entries, key3).lsn == r3.batch_lsn);
}

void
m09_value_failure_releases_and_stays_invisible() {
    write_fixture fx;
    const std::string failed_key =
        key_for_owner(0, fx.front_count, "value-fail");

    fx.nvme.fail_kind = test_fake_nvme::op_kind::write;
    fx.nvme.fail_call = 1;
    fx.nvme.fail_with_false = true;
    expect_value_error(
        fx.run_write({
            {.op = core::write_op_type::put,
             .key = failed_key,
             .value = std::string(3000, 'v')},
        }),
        value::value_persist_error_reason::round_failed);
    CHECK(fx.visible_lsn() == 1);
    CHECK(!front_memtable_has_key(fx, failed_key));
    expect_miss(fx.lookup_visible(failed_key));
    CHECK(!wal_has_key(fx, failed_key));
    fx.nvme.clear_failure();

    const std::string retry_key =
        key_for_owner(0, fx.front_count, "value-retry");
    auto retry = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put,
         .key = retry_key,
         .value = "ok"},
    }));
    CHECK(retry.batch_lsn == 2);
    CHECK(fx.visible_lsn() == 2);
    expect_miss(fx.lookup_visible(failed_key));
    expect_memtable_data_ver(fx, retry_key, retry.batch_lsn);
}

void
m09_wal_failure_releases_and_memtable_invisible() {
    write_fixture fx;
    const std::string failed_key =
        key_for_owner(0, fx.front_count, "wal-fail");

    fx.nvme.fail_kind = test_fake_nvme::op_kind::write;
    fx.nvme.fail_call = 2;
    fx.nvme.fail_with_false = true;
    expect_wal_error(
        fx.run_write({
            {.op = core::write_op_type::put,
             .key = failed_key,
             .value = std::string(3000, 'w')},
        }),
        wal::wal_append_error_reason::device_failure);
    CHECK(fx.nvme.writes.calls > 1);
    CHECK(fx.visible_lsn() == 1);
    CHECK(!front_memtable_has_key(fx, failed_key));
    expect_miss(fx.lookup_visible(failed_key));
    CHECK(!wal_has_key(fx, failed_key));
    fx.nvme.clear_failure();

    const std::string retry_key =
        key_for_owner(0, fx.front_count, "wal-retry");
    auto retry = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put,
         .key = retry_key,
         .value = "ok"},
    }));
    CHECK(retry.batch_lsn == 2);
    CHECK(fx.visible_lsn() == 2);
    expect_miss(fx.lookup_visible(failed_key));
    expect_memtable_data_ver(fx, retry_key, retry.batch_lsn);
}

void
m09_release_of_failed_earlier_batch_unblocks_later_success() {
    write_fixture fx;
    const std::string failed_key =
        key_for_owner(0, fx.front_count, "held-fail");
    const std::string later_key =
        key_for_owner(1, fx.front_count, "held-later");

    fx.nvme.hold_call(test_fake_nvme::op_kind::write, 1);
    auto failed = fx.submit_write({
        {.op = core::write_op_type::del, .key = failed_key, .value = ""},
    });

    for (uint32_t i = 0; !fx.nvme.has_held() && i < 200000; ++i) {
        const bool progress = fx.advance_all();
        if (!progress) std::this_thread::yield();
    }
    CHECK(fx.nvme.has_held());
    CHECK(!ready(failed.fut));
    CHECK(fx.visible_lsn() == 0);

    auto later = fx.submit_write({
        {.op = core::write_op_type::put, .key = later_key, .value = "keep"},
    });
    fx.drive_until_ready(later);
    auto later_result =
        expect_ok<write_path::write_batch_result>(later.fut.get());
    CHECK(later_result.batch_lsn == 2);
    CHECK(fx.visible_lsn() == 0);
    expect_miss(fx.lookup_visible(later_key));
    CHECK(!ready(failed.fut));

    CHECK(fx.nvme.release_held(
        test_fake_nvme::held_completion::fail_with_false));
    fx.drive_until_ready(failed);
    expect_wal_error(
        failed.fut.get(),
        wal::wal_append_error_reason::device_failure);
    CHECK(fx.visible_lsn() == 2);
    CHECK(!front_memtable_has_key(fx, failed_key));
    expect_miss(fx.lookup_visible(failed_key));
    expect_memtable_data_ver(fx, later_key, later_result.batch_lsn);
}

void
m09_pre_lsn_failure_propagates_without_lsn_or_release() {
    write_fixture fx;
    core::client_batch_buffer bad;
    bad.bytes.push_back(std::byte{0x01});

    auto failed = fx.submit_write_buffer(std::move(bad));
    fx.drive_until_ready(failed);
    expect_error<std::invalid_argument>(failed.fut.get());
    CHECK(fx.visible_lsn() == 0);
    CHECK(fx.nvme.total_calls() == 0);
    for (const auto& front_sched : fx.front_storage) {
        CHECK(front_sched->active_for_testing()->table.empty());
    }

    const std::string key = key_for_owner(0, fx.front_count, "after-bad");
    auto result = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "ok"},
    }));
    CHECK(result.batch_lsn == 1);
    CHECK(result.entry_count == 1);
    CHECK(fx.visible_lsn() == 1);
    expect_memtable_data_ver(fx, key, result.batch_lsn);
}

void
m09_compose_without_submit_has_no_owner_side_effect() {
    write_fixture fx;
    const std::string discarded_key =
        key_for_owner(0, fx.front_count, "discard");

    {
        auto sender = fx.compose_write(fx.make_input({
            {.op = core::write_op_type::put,
             .key = discarded_key,
             .value = "discarded"},
        }));
        (void)sender;
    }

    CHECK(fx.nvme.total_calls() == 0);
    CHECK(fx.visible_lsn() == 0);
    for (const auto& front_sched : fx.front_storage) {
        CHECK(front_sched->active_for_testing()->table.empty());
    }

    const std::string key = key_for_owner(0, fx.front_count, "after-compose");
    auto result = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "ok"},
    }));
    CHECK(result.batch_lsn == 1);
    CHECK(result.entry_count == 1);
    CHECK(fx.visible_lsn() == 1);
    expect_memtable_data_ver(fx, key, result.batch_lsn);
}

void
m09_failure_classification_unit() {
    std::exception_ptr none;
    CHECK(!write_path::is_releasable_write_failure(none));
    CHECK(write_path::is_releasable_write_failure(
        std::make_exception_ptr(value::value_persist_error(
            value::value_persist_error_reason::round_failed,
            "value"))));
    CHECK(write_path::is_releasable_write_failure(
        std::make_exception_ptr(wal::wal_append_error(
            wal::wal_append_error_reason::device_failure,
            "wal"))));
    CHECK(!write_path::is_releasable_write_failure(
        std::make_exception_ptr(std::logic_error("logic"))));
    CHECK(!write_path::is_releasable_write_failure(
        std::make_exception_ptr(std::invalid_argument("invalid"))));
    CHECK(!write_path::is_releasable_write_failure(
        std::make_exception_ptr(std::runtime_error("runtime"))));
    CHECK(!write_path::is_releasable_write_failure(
        std::make_exception_ptr(42)));
}

void
m09_oversized_value_fails_with_release() {
    write_fixture fx;
    const std::string failed_key =
        key_for_owner(0, fx.front_count, "oversized");
    const uint32_t writes_before = fx.nvme.writes.calls;

    expect_value_error(
        fx.run_write({
            {.op = core::write_op_type::put,
             .key = failed_key,
             .value = std::string(20000, 'x')},
        }),
        value::value_persist_error_reason::oversized_value);
    CHECK(fx.nvme.writes.calls == writes_before);
    CHECK(fx.visible_lsn() == 1);
    CHECK(!front_memtable_has_key(fx, failed_key));
    expect_miss(fx.lookup_visible(failed_key));
    CHECK(!wal_has_key(fx, failed_key));

    const std::string retry_key =
        key_for_owner(0, fx.front_count, "oversized-retry");
    auto retry = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put,
         .key = retry_key,
         .value = "ok"},
    }));
    CHECK(retry.batch_lsn == 2);
    CHECK(fx.visible_lsn() == 2);
    expect_miss(fx.lookup_visible(failed_key));
    expect_memtable_data_ver(fx, retry_key, retry.batch_lsn);
}

}  // namespace

int
main() {
    m09_success_path_matches_baseline_semantics();
    m09_concurrent_batches_publish_gap_free();
    m09_value_failure_releases_and_stays_invisible();
    m09_wal_failure_releases_and_memtable_invisible();
    m09_release_of_failed_earlier_batch_unblocks_later_success();
    m09_pre_lsn_failure_propagates_without_lsn_or_release();
    m09_compose_without_submit_has_no_owner_side_effect();
    m09_failure_classification_unit();
    m09_oversized_value_fails_with_release();
    return 0;
}
