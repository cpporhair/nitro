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
#include "apps/inconel/core/wal_reclaim_frontier.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/format/wal.hh"
#include "apps/inconel/front/sender.hh"
#include "apps/inconel/value/sender.hh"
#include "apps/inconel/wal/sender.hh"
#include "apps/inconel/write_path/sender.hh"
#include "apps/inconel/write_path/write_batch_state.hh"
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

    bool advance_one() {
        if (pending_.empty()) return false;
        std::unique_ptr<_io::req> req(pending_.front());
        pending_.pop_front();

        auto& st = stats_for(req->op);
        CHECK(st.active > 0);
        --st.active;

        if (fail_call != 0 && req->op == fail_kind &&
            req->call_index == fail_call) {
            if (fail_with_exception) {
                req->fail(std::make_exception_ptr(
                    std::runtime_error("fake nvme exception")));
                return true;
            }
            if (fail_with_false) {
                req->cb(false);
                return true;
            }
        }

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

struct value_ref_less {
    bool operator()(const format::value_ref& a,
                    const format::value_ref& b) const noexcept {
        return std::tie(a.base.device_id, a.base.lba, a.byte_offset, a.len,
                        a.flags) <
               std::tie(b.base.device_id, b.base.lba, b.byte_offset, b.len,
                        b.flags);
    }
};

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

std::span<const core::canonical_entry>
canonical_span(const core::batch_ctx& ctx) {
    return {ctx.canonical_entries.data(), ctx.canonical_entries.size()};
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

const core::canonical_entry&
find_entry(const core::batch_ctx& ctx, std::string_view key) {
    for (const auto& entry : ctx.canonical_entries) {
        if (entry.key == key) return entry;
    }
    CHECK(false);
    return ctx.canonical_entries[0];
}

uint32_t
class_size_for_body(std::span<const uint32_t> class_sizes,
                    uint32_t body_len) {
    const uint32_t total =
        body_len + static_cast<uint32_t>(sizeof(format::value_object_header));
    for (uint32_t cs : class_sizes) {
        if (cs >= total) return cs;
    }
    CHECK(false);
    return 0;
}

void
expect_value_refs_nondefault_and_disjoint(
    const core::batch_ctx& ctx,
    std::span<const uint32_t> class_sizes) {
    for (uint32_t idx : ctx.put_entry_indices) {
        const auto& entry = ctx.canonical_entries[idx];
        CHECK(entry.allocated_vr.base.lba != 0);
        CHECK(entry.allocated_vr.len == entry.value.size());
    }
    for (std::size_t i = 0; i < ctx.put_entry_indices.size(); ++i) {
        const auto& a = ctx.canonical_entries[ctx.put_entry_indices[i]];
        const auto& ar = a.allocated_vr;
        const uint32_t a_size = class_size_for_body(class_sizes, ar.len);
        for (std::size_t j = i + 1; j < ctx.put_entry_indices.size(); ++j) {
            const auto& b = ctx.canonical_entries[ctx.put_entry_indices[j]];
            const auto& br = b.allocated_vr;
            if (ar.base != br.base) continue;
            const uint32_t b_size = class_size_for_body(class_sizes, br.len);
            const uint32_t a0 = ar.byte_offset;
            const uint32_t a1 = a0 + a_size;
            const uint32_t b0 = br.byte_offset;
            const uint32_t b1 = b0 + b_size;
            CHECK(a1 <= b0 || b1 <= a0);
        }
    }
}

void
expect_delete_value_ref_default(const core::canonical_entry& entry) {
    CHECK(entry.allocated_vr.base.lba == 0);
    CHECK(entry.allocated_vr.byte_offset == 0);
    CHECK(entry.allocated_vr.len == 0);
    CHECK(entry.allocated_vr.flags == 0);
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

void
expect_bool_ok(op_result<bool>&& result) {
    CHECK(expect_ok<bool>(std::move(result)));
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
expect_value_error(op_result<bool>&& result,
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

void
expect_wal_error(op_result<bool>&& result,
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
    core::wal_reclaim_frontier wal_frontier;
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
        , wal_space(geom, &wal_frontier, front_count_in) {
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

    void drive_idle(uint32_t limit = 200000) {
        for (uint32_t i = 0; i < limit; ++i) {
            const bool progress = advance_all(true);
            if (!progress) break;
        }
        CHECK(nvme.idle());
    }

    write_path::write_batch_state
    assign_state(std::vector<core::raw_batch_op> ops) {
        auto input = core::encode_client_batch(op_span(ops));
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

    submission<bool>
    submit_wal(write_path::write_batch_state& state) {
        return submit_result<bool>([&]() {
            return write_path::write_batch_wal_phase(
                state,
                std::span<front::front_sched* const>(
                    front_ptrs.data(), front_ptrs.size()),
                wal_space,
                std::span<test_fake_nvme::scheduler* const>(
                    nvme_ptrs.data(), nvme_ptrs.size()));
        });
    }

    op_result<bool>
    value_result(write_path::write_batch_state& state) {
        return run_bool_result([&]() {
            return write_path::write_batch_value_phase(state, provider);
        });
    }

    op_result<bool>
    wal_result(write_path::write_batch_state& state,
               bool advance_nvme = true) {
        auto sub = submit_wal(state);
        drive_until_ready(sub, advance_nvme);
        return sub.fut.get();
    }

    op_result<bool>
    memtable_result(write_path::write_batch_state& state) {
        return run_bool_result([&]() {
            return write_path::write_batch_memtable_phase(
                state,
                std::span<front::front_sched* const>(
                    front_ptrs.data(), front_ptrs.size()));
        });
    }

    op_result<bool>
    publish_result(write_path::write_batch_state& state) {
        return run_bool_result([&]() {
            return write_path::write_batch_publish(*coord, state);
        });
    }

    op_result<bool>
    release_result(write_path::write_batch_state& state) {
        return run_bool_result([&]() {
            return write_path::write_batch_release(*coord, state);
        });
    }

    void run_value(write_path::write_batch_state& state) {
        expect_bool_ok(value_result(state));
    }

    void run_wal(write_path::write_batch_state& state,
                 bool advance_nvme = true) {
        expect_bool_ok(wal_result(state, advance_nvme));
    }

    void run_memtable(write_path::write_batch_state& state) {
        expect_bool_ok(memtable_result(state));
    }

    void run_publish(write_path::write_batch_state& state) {
        expect_bool_ok(publish_result(state));
    }

    void run_release(write_path::write_batch_state& state) {
        expect_bool_ok(release_result(state));
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

std::unique_ptr<write_path::write_batch_state>
single_put_to_wal(write_fixture& fx,
                  std::string key,
                  std::string value) {
    auto state = std::make_unique<write_path::write_batch_state>(
        fx.assign_state({
            {.op = core::write_op_type::put,
             .key = std::move(key),
             .value = std::move(value)},
        }));
    fx.run_value(*state);
    fx.run_wal(*state);
    CHECK(state->phase == write_path::write_batch_phase::wal_durable);
    return state;
}

void
finish_published(write_fixture& fx, write_path::write_batch_state& state) {
    fx.run_memtable(state);
    fx.run_publish(state);
    CHECK(state.phase == write_path::write_batch_phase::published);
}

void
m08_baseline_single_batch_put_delete_full_path() {
    write_fixture fx(3);
    const std::string update_key = key_for_owner(0, fx.front_count, "update");
    const std::string delete_key = key_for_owner(0, fx.front_count, "gone");
    const std::string other_key = key_for_owner(1, fx.front_count, "other");
    const std::string untouched_key =
        key_for_owner(2, fx.front_count, "untouched");

    auto state = fx.assign_state({
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
    });

    CHECK(state.ctx.batch_lsn == 1);
    CHECK(state.ctx.entry_count == 3);
    CHECK(state.phase == write_path::write_batch_phase::assigned);

    const auto& update_entry = find_entry(state.ctx, update_key);
    const auto& delete_entry = find_entry(state.ctx, delete_key);
    const auto& other_entry = find_entry(state.ctx, other_key);
    CHECK(update_entry.op == core::write_op_type::put);
    CHECK(update_entry.value == "new-value");
    CHECK(delete_entry.op == core::write_op_type::del);
    CHECK(delete_entry.value.empty());
    CHECK(other_entry.op == core::write_op_type::put);
    CHECK(other_entry.value == "front-one");

    fx.run_value(state);
    CHECK(state.phase == write_path::write_batch_phase::value_durable);
    expect_value_refs_nondefault_and_disjoint(state.ctx, fx.class_sizes);
    expect_delete_value_ref_default(delete_entry);

    fx.run_wal(state);
    CHECK(state.phase == write_path::write_batch_phase::wal_durable);

    const auto wal_entries = collect_all_wal_entries(fx);
    CHECK(wal_entries.size() == state.ctx.entry_count);
    const auto& update_wal = find_wal_entry(wal_entries, update_key);
    CHECK(update_wal.stream_id == owner_for_key(update_key, fx.front_count));
    CHECK(update_wal.op_type == format::wal_op_type::put);
    CHECK(update_wal.lsn == 1);
    CHECK(update_wal.entry_count == state.ctx.entry_count);
    CHECK(update_wal.vr.has_value());
    CHECK(same_value_ref(*update_wal.vr, update_entry.allocated_vr));

    const auto& delete_wal = find_wal_entry(wal_entries, delete_key);
    CHECK(delete_wal.stream_id == owner_for_key(delete_key, fx.front_count));
    CHECK(delete_wal.op_type == format::wal_op_type::del);
    CHECK(delete_wal.lsn == 1);
    CHECK(delete_wal.entry_count == state.ctx.entry_count);
    CHECK(!delete_wal.vr.has_value());

    const auto& other_wal = find_wal_entry(wal_entries, other_key);
    CHECK(other_wal.stream_id == owner_for_key(other_key, fx.front_count));
    CHECK(other_wal.op_type == format::wal_op_type::put);
    CHECK(other_wal.lsn == 1);
    CHECK(other_wal.entry_count == state.ctx.entry_count);
    CHECK(other_wal.vr.has_value());
    CHECK(same_value_ref(*other_wal.vr, other_entry.allocated_vr));
    CHECK(collect_wal_entries(fx, owner_for_key(untouched_key,
                                                fx.front_count)).empty());

    fx.run_memtable(state);
    CHECK(state.phase == write_path::write_batch_phase::memtable_applied);
    fx.run_publish(state);
    CHECK(state.phase == write_path::write_batch_phase::published);

    CHECK(fx.visible_lsn() == 1);
    expect_visible_value(fx, update_key, update_entry.allocated_vr);
    expect_tombstone(fx.lookup_visible(delete_key));
    expect_visible_value(fx, other_key, other_entry.allocated_vr);
    expect_miss(fx.lookup_visible(untouched_key));
}

void
m08_delete_only_batch_full_path() {
    write_fixture fx;
    const std::string delete_key = key_for_owner(0, fx.front_count, "del");
    auto state = fx.assign_state({
        {.op = core::write_op_type::del,
         .key = delete_key,
         .value = ""},
    });

    const uint32_t writes_before_value = fx.nvme.writes.calls;
    fx.run_value(state);
    CHECK(state.phase == write_path::write_batch_phase::value_durable);
    CHECK(fx.nvme.writes.calls == writes_before_value);
    expect_delete_value_ref_default(find_entry(state.ctx, delete_key));

    fx.run_wal(state);
    CHECK(state.phase == write_path::write_batch_phase::wal_durable);
    const auto wal_entries = collect_all_wal_entries(fx);
    CHECK(wal_entries.size() == 1);
    CHECK(wal_entries[0].op_type == format::wal_op_type::del);
    CHECK(wal_entries[0].key == delete_key);
    CHECK(wal_entries[0].lsn == state.ctx.batch_lsn);
    CHECK(!wal_entries[0].vr.has_value());

    fx.run_memtable(state);
    fx.run_publish(state);
    CHECK(state.phase == write_path::write_batch_phase::published);
    CHECK(fx.visible_lsn() == state.ctx.batch_lsn);
    expect_tombstone(fx.lookup_visible(delete_key));
}

void
m08_state_owns_ctx_without_second_copy() {
    static_assert(!std::is_copy_constructible_v<write_path::write_batch_state>);
    static_assert(!std::is_copy_assignable_v<write_path::write_batch_state>);
    static_assert(std::is_nothrow_move_constructible_v<
                  write_path::write_batch_state>);
    static_assert(std::is_nothrow_move_assignable_v<
                  write_path::write_batch_state>);

    write_fixture fx;
    auto state = fx.assign_state({
        {.op = core::write_op_type::put,
         .key = key_for_owner(0, fx.front_count, "park"),
         .value = "body"},
    });

    const auto* entries = state.ctx.canonical_entries.data();
    const auto* fragments = state.ctx.fragments.data();
    const auto* input_bytes = state.ctx.input.bytes.data();

    std::vector<std::unique_ptr<write_path::write_batch_state>> parked;
    parked.push_back(
        std::make_unique<write_path::write_batch_state>(std::move(state)));
    CHECK(parked[0]->ctx.canonical_entries.data() == entries);
    CHECK(parked[0]->ctx.fragments.data() == fragments);
    CHECK(parked[0]->ctx.input.bytes.data() == input_bytes);

    write_path::write_batch_state restored(std::move(*parked[0]));
    parked.clear();
    CHECK(restored.ctx.canonical_entries.data() == entries);
    CHECK(restored.ctx.fragments.data() == fragments);
    CHECK(restored.ctx.input.bytes.data() == input_bytes);
    CHECK(restored.phase == write_path::write_batch_phase::assigned);
}

void
m08_inflight_parks_after_wal_without_publish() {
    write_fixture fx;
    std::vector<std::string> keys{
        key_for_owner(0, fx.front_count, "park-a"),
        key_for_owner(1, fx.front_count, "park-b"),
        key_for_owner(0, fx.front_count, "park-c"),
    };

    std::vector<std::unique_ptr<write_path::write_batch_state>> parked;
    parked.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        parked.push_back(single_put_to_wal(
            fx, keys[i], "value-" + std::to_string(i)));
        CHECK(parked.back()->ctx.batch_lsn == i + 1);
    }

    CHECK(fx.visible_lsn() == 0);
    for (const auto& front_sched : fx.front_storage) {
        CHECK(front_sched->active_for_testing()->table.empty());
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        CHECK(parked[i]->phase == write_path::write_batch_phase::wal_durable);
        expect_miss(fx.lookup_visible(keys[i]));
    }

    const auto wal_entries = collect_all_wal_entries(fx);
    CHECK(wal_entries.size() == keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& wal_entry = find_wal_entry(wal_entries, keys[i]);
        CHECK(wal_entry.op_type == format::wal_op_type::put);
        CHECK(wal_entry.lsn == i + 1);
    }
}

void
m08_out_of_order_finish_advances_durable_lsn_gap_free() {
    write_fixture fx;
    const std::string key1 = key_for_owner(0, fx.front_count, "finish-a");
    const std::string key2 = key_for_owner(1, fx.front_count, "finish-b");
    const std::string key3 = key_for_owner(0, fx.front_count, "finish-c");

    auto first = single_put_to_wal(fx, key1, "v1");
    auto second = single_put_to_wal(fx, key2, "v2");
    auto third = single_put_to_wal(fx, key3, "v3");

    const auto first_ref = find_entry(first->ctx, key1).allocated_vr;
    const auto second_ref = find_entry(second->ctx, key2).allocated_vr;
    const auto third_ref = find_entry(third->ctx, key3).allocated_vr;

    finish_published(fx, *first);
    CHECK(fx.visible_lsn() == 1);
    expect_visible_value(fx, key1, first_ref);
    expect_miss(fx.lookup_visible(key2));
    expect_miss(fx.lookup_visible(key3));

    finish_published(fx, *third);
    CHECK(fx.visible_lsn() == 1);
    expect_memtable_data_ver(fx, key3, third->ctx.batch_lsn);
    CHECK(third->ctx.batch_lsn > fx.visible_lsn());
    expect_miss(fx.lookup_visible(key3));

    finish_published(fx, *second);
    CHECK(fx.visible_lsn() == 3);
    expect_visible_value(fx, key1, first_ref);
    expect_visible_value(fx, key2, second_ref);
    expect_visible_value(fx, key3, third_ref);
}

void
m08_release_parked_fills_hole_without_visibility() {
    write_fixture fx;
    const std::string released_key =
        key_for_owner(0, fx.front_count, "release-a");
    const std::string published_key =
        key_for_owner(1, fx.front_count, "release-b");

    auto released = single_put_to_wal(fx, released_key, "drop");
    auto published = single_put_to_wal(fx, published_key, "keep");
    const auto published_ref = find_entry(published->ctx,
                                          published_key).allocated_vr;

    finish_published(fx, *published);
    CHECK(fx.visible_lsn() == 0);
    expect_miss(fx.lookup_visible(released_key));
    expect_miss(fx.lookup_visible(published_key));

    fx.run_release(*released);
    CHECK(released->phase == write_path::write_batch_phase::released);
    CHECK(fx.visible_lsn() == 2);
    expect_miss(fx.lookup_visible(released_key));
    CHECK(!front_memtable_has_key(fx, released_key));
    expect_visible_value(fx, published_key, published_ref);
}

void
m08_wal_failure_maps_to_release_and_unblocks_later_batch() {
    write_fixture fx;
    const std::string failed_key =
        key_for_owner(0, fx.front_count, "wal-fail");
    auto failed = fx.assign_state({
        {.op = core::write_op_type::put,
         .key = failed_key,
         .value = "bad"},
    });
    fx.run_value(failed);

    fx.nvme.fail_kind = test_fake_nvme::op_kind::write;
    fx.nvme.fail_call = fx.nvme.writes.calls + 1;
    fx.nvme.fail_with_false = true;
    expect_wal_error(
        fx.wal_result(failed),
        wal::wal_append_error_reason::device_failure);
    CHECK(failed.phase == write_path::write_batch_phase::value_durable);
    CHECK(!front_memtable_has_key(fx, failed_key));

    fx.run_release(failed);
    CHECK(failed.phase == write_path::write_batch_phase::released);
    CHECK(fx.visible_lsn() == 1);
    fx.nvme.clear_failure();

    const std::string retry_key =
        key_for_owner(0, fx.front_count, "wal-retry");
    auto retry = single_put_to_wal(fx, retry_key, "ok");
    const auto retry_ref = find_entry(retry->ctx, retry_key).allocated_vr;
    finish_published(fx, *retry);
    CHECK(fx.visible_lsn() == 2);
    expect_visible_value(fx, retry_key, retry_ref);
}

void
m08_value_failure_maps_to_release() {
    write_fixture fx;
    const std::string failed_key =
        key_for_owner(0, fx.front_count, "value-fail");
    auto failed = fx.assign_state({
        {.op = core::write_op_type::put,
         .key = failed_key,
         .value = std::string(3000, 'v')},
    });

    fx.nvme.fail_kind = test_fake_nvme::op_kind::write;
    fx.nvme.fail_call = 1;
    fx.nvme.fail_with_false = true;
    expect_value_error(
        fx.value_result(failed),
        value::value_persist_error_reason::round_failed);
    CHECK(failed.phase == write_path::write_batch_phase::assigned);
    CHECK(!front_memtable_has_key(fx, failed_key));

    fx.run_release(failed);
    CHECK(failed.phase == write_path::write_batch_phase::released);
    CHECK(fx.visible_lsn() == 1);
    fx.nvme.clear_failure();

    const std::string retry_key =
        key_for_owner(0, fx.front_count, "value-retry");
    auto retry = single_put_to_wal(fx, retry_key, "ok");
    const auto retry_ref = find_entry(retry->ctx, retry_key).allocated_vr;
    finish_published(fx, *retry);
    CHECK(fx.visible_lsn() == 2);
    expect_visible_value(fx, retry_key, retry_ref);
}

void
m08_prepare_queue_full_overflow_releases_without_blocking_others() {
    write_fixture fx(1, 2);
    const std::string key1 = key_for_owner(0, fx.front_count, "fifo-a");
    const std::string key2 = key_for_owner(0, fx.front_count, "fifo-b");
    const std::string key3 = key_for_owner(0, fx.front_count, "fifo-c");
    const std::string key4 = key_for_owner(0, fx.front_count, "fifo-d");

    auto first = fx.assign_state({
        {.op = core::write_op_type::del, .key = key1, .value = ""},
    });
    auto second = fx.assign_state({
        {.op = core::write_op_type::del, .key = key2, .value = ""},
    });
    auto third = fx.assign_state({
        {.op = core::write_op_type::del, .key = key3, .value = ""},
    });
    auto overflow = fx.assign_state({
        {.op = core::write_op_type::del, .key = key4, .value = ""},
    });
    fx.run_value(first);
    fx.run_value(second);
    fx.run_value(third);
    fx.run_value(overflow);

    auto sub1 = fx.submit_wal(first);
    for (uint32_t i = 0; fx.nvme.writes.active == 0 && i < 1024; ++i) {
        (void)fx.advance_all(false);
        std::this_thread::yield();
    }
    CHECK(fx.nvme.writes.active > 0);
    CHECK(!ready(sub1.fut));

    auto sub2 = fx.submit_wal(second);
    bool moved_to_pending = false;
    for (uint32_t i = 0; !moved_to_pending && i < 1024; ++i) {
        moved_to_pending = fx.front_storage[0]->advance();
        std::this_thread::yield();
    }
    CHECK(moved_to_pending);
    CHECK(!ready(sub2.fut));

    auto sub3 = fx.submit_wal(third);
    moved_to_pending = false;
    for (uint32_t i = 0; !moved_to_pending && i < 1024; ++i) {
        moved_to_pending = fx.front_storage[0]->advance();
        std::this_thread::yield();
    }
    CHECK(moved_to_pending);
    CHECK(!ready(sub3.fut));

    auto sub4 = fx.submit_wal(overflow);
    fx.drive_until_ready(sub4, false);
    expect_wal_error(
        sub4.fut.get(),
        wal::wal_append_error_reason::prepare_queue_full);
    CHECK(overflow.phase == write_path::write_batch_phase::value_durable);
    fx.run_release(overflow);
    CHECK(overflow.phase == write_path::write_batch_phase::released);
    CHECK(fx.visible_lsn() == 0);

    fx.drive_until_ready(sub1);
    expect_bool_ok(sub1.fut.get());
    CHECK(first.phase == write_path::write_batch_phase::wal_durable);
    fx.drive_until_ready(sub2);
    expect_bool_ok(sub2.fut.get());
    CHECK(second.phase == write_path::write_batch_phase::wal_durable);
    fx.drive_until_ready(sub3);
    expect_bool_ok(sub3.fut.get());
    CHECK(third.phase == write_path::write_batch_phase::wal_durable);

    finish_published(fx, first);
    CHECK(fx.visible_lsn() == 1);
    finish_published(fx, second);
    CHECK(fx.visible_lsn() == 2);
    finish_published(fx, third);
    CHECK(fx.visible_lsn() == 4);
    expect_tombstone(fx.lookup_visible(key1));
    expect_tombstone(fx.lookup_visible(key2));
    expect_tombstone(fx.lookup_visible(key3));
    expect_miss(fx.lookup_visible(key4));
}

void
m08_release_after_memtable_phase_starts_is_rejected() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "late-release");
    auto state = single_put_to_wal(fx, key, "body");
    fx.run_memtable(*state);
    CHECK(state->phase == write_path::write_batch_phase::memtable_applied);
    CHECK(fx.visible_lsn() == 0);

    expect_error<std::logic_error>(fx.release_result(*state));
    CHECK(state->phase == write_path::write_batch_phase::memtable_applied);
    CHECK(fx.visible_lsn() == 0);

    fx.run_publish(*state);
    CHECK(fx.visible_lsn() == 1);
}

void
m08_publish_before_memtable_applied_is_rejected() {
    write_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count, "early-publish");
    auto state = single_put_to_wal(fx, key, "body");
    CHECK(state->phase == write_path::write_batch_phase::wal_durable);

    expect_error<std::logic_error>(fx.publish_result(*state));
    CHECK(state->phase == write_path::write_batch_phase::wal_durable);
    CHECK(fx.visible_lsn() == 0);

    const auto value_ref = find_entry(state->ctx, key).allocated_vr;
    finish_published(fx, *state);
    CHECK(fx.visible_lsn() == 1);
    expect_visible_value(fx, key, value_ref);
}

}  // namespace

int
main() {
    m08_baseline_single_batch_put_delete_full_path();
    m08_delete_only_batch_full_path();
    m08_state_owns_ctx_without_second_copy();
    m08_inflight_parks_after_wal_without_publish();
    m08_out_of_order_finish_advances_durable_lsn_gap_free();
    m08_release_parked_fills_hole_without_visibility();
    m08_wal_failure_maps_to_release_and_unblocks_later_batch();
    m08_value_failure_maps_to_release();
    m08_prepare_queue_full_overflow_releases_without_blocking_others();
    m08_release_after_memtable_phase_starts_is_rejected();
    m08_publish_before_memtable_applied_is_rejected();
    return 0;
}
