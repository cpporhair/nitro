#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/core/batch_carrier.hh"
#include "apps/inconel/core/page_cache.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/runtime/facade.hh"
#include "apps/inconel/value/sender.hh"
#include "apps/inconel/write_path/sender.hh"
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
    constexpr static bool m07_fake_nvme_op = true;
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

    auto read_frame(memory::segmented_page_frame* frame, uint32_t flags = 0) {
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
                    std::runtime_error("m07 fake nvme exception")));
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

    void seed_frame(format::paddr base,
                    uint16_t span_lbas,
                    std::vector<char> bytes) {
        disk_[key_for(base, span_lbas)] = std::move(bytes);
    }

    void copy_disk_from(const scheduler& other) {
        disk_ = other.disk_;
    }

private:
    using disk_key = std::tuple<uint16_t, uint64_t, uint16_t>;

    std::deque<_io::req*> pending_;
    std::map<disk_key, std::vector<char>> disk_;

    op_stats& stats_for(op_kind kind) {
        switch (kind) {
        case op_kind::read:  return reads;
        case op_kind::write: return writes;
        case op_kind::trim:  return trims;
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
        for (auto it = disk_.begin(); it != disk_.end(); ) {
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
    && (get_current_op_type_t<pos, scope_t>::m07_fake_nvme_op)
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

struct value_fixture {
    using value_sched_t = value::value_alloc_sched<core::segmented_clock_cache>;

    std::vector<uint32_t> class_sizes{64, 256, 1024, 4096, 16384};
    runtime_scope scope;
    core::data_area_heads heads{};
    test_fake_nvme::scheduler nvme;
    value_sched_t sched;
    fake_nvme_provider provider;

    value_fixture(value::value_io_policy policy = {},
                  uint64_t data_area_base_lba = 4000,
                  uint64_t data_area_end_lba = 100000,
                  uint32_t cache_capacity = 128)
        : sched(
              std::span<const uint32_t>(class_sizes.data(), class_sizes.size()),
              kLbaSize,
              format::paddr{0, data_area_base_lba},
              format::paddr{0, data_area_end_lba},
              &heads,
              core::segmented_clock_cache(cache_capacity),
              kQuantumBytes,
              kGroupSizeLbas,
              2048,
              memory::make_heap_dma_page_allocator(),
              4096,
              -1,
              policy)
        , provider{&nvme} {
        core::registry::value_alloc_sched = &sched;
    }
};

core::batch_ctx
make_ctx(std::vector<core::raw_batch_op> ops,
         uint64_t batch_lsn,
         uint32_t front_count = 1) {
    auto input = core::encode_client_batch(
        std::span<const core::raw_batch_op>(ops.data(), ops.size()));
    return core::build_batch_ctx(std::move(input), batch_lsn, front_count);
}

uint32_t
class_size_for_body(std::span<const uint32_t> class_sizes, uint32_t body_len) {
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
    for (size_t i = 0; i < ctx.put_entry_indices.size(); ++i) {
        const auto& a = ctx.canonical_entries[ctx.put_entry_indices[i]];
        const auto& ar = a.allocated_vr;
        const uint32_t a_size =
            class_size_for_body(class_sizes, ar.len);
        for (size_t j = i + 1; j < ctx.put_entry_indices.size(); ++j) {
            const auto& b = ctx.canonical_entries[ctx.put_entry_indices[j]];
            const auto& br = b.allocated_vr;
            if (ar.base != br.base) continue;
            const uint32_t b_size =
                class_size_for_body(class_sizes, br.len);
            const uint32_t a0 = ar.byte_offset;
            const uint32_t a1 = a0 + a_size;
            const uint32_t b0 = br.byte_offset;
            const uint32_t b1 = b0 + b_size;
            CHECK(a1 <= b0 || b1 <= a0);
        }
    }
}

bool
has_two_puts_on_same_page(const core::batch_ctx& ctx) {
    for (size_t i = 0; i < ctx.put_entry_indices.size(); ++i) {
        const auto& a = ctx.canonical_entries[ctx.put_entry_indices[i]];
        for (size_t j = i + 1; j < ctx.put_entry_indices.size(); ++j) {
            const auto& b = ctx.canonical_entries[ctx.put_entry_indices[j]];
            if (a.allocated_vr.base == b.allocated_vr.base) return true;
        }
    }
    return false;
}

using root_context_t = decltype(pump::core::make_root_context());

template <typename T>
using op_result = std::variant<T, std::exception_ptr>;

using void_result = std::variant<std::monostate, std::exception_ptr>;

template <typename T>
struct submission {
    root_context_t ctx;
    std::future<op_result<T>> fut;
};

struct void_submission {
    root_context_t ctx;
    std::future<void_result> fut;
};

template <typename T, typename SenderBuilder>
submission<T>
submit_value(SenderBuilder&& build_sender) {
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

template <typename SenderBuilder>
void_submission
submit_void(SenderBuilder&& build_sender) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<void_result>>();
    auto fut = promise->get_future();
    auto caught = std::make_shared<std::exception_ptr>();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::any_exception([caught](std::exception_ptr ep) {
            *caught = std::move(ep);
            return pump::sender::just();
        })
        >> pump::sender::then([promise, caught]() mutable {
            if (*caught) {
                promise->set_value(*caught);
            } else {
                promise->set_value(std::monostate{});
            }
        })
        >> pump::sender::submit(ctx);

    return void_submission{.ctx = std::move(ctx), .fut = std::move(fut)};
}

template <typename Future>
bool ready(Future& fut) {
    return fut.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
}

template <typename Submission>
void
drive_until_ready(value_fixture& fx,
                  Submission& sub,
                  bool advance_nvme = true) {
    for (uint32_t i = 0; !ready(sub.fut) && i < 200000; ++i) {
        bool progress = false;
        progress |= fx.sched.advance();
        if (advance_nvme) progress |= fx.nvme.advance_one();
        if (!progress) std::this_thread::yield();
    }
    CHECK(ready(sub.fut));
}

template <typename A, typename B>
void
drive_until_both_ready(value_fixture& fx, A& a, B& b) {
    for (uint32_t i = 0; !(ready(a.fut) && ready(b.fut)) && i < 200000; ++i) {
        bool progress = false;
        progress |= fx.sched.advance();
        progress |= fx.nvme.advance_one();
        if (!progress) std::this_thread::yield();
    }
    CHECK(ready(a.fut));
    CHECK(ready(b.fut));
}

void
expect_bool_ok(op_result<bool>&& result) {
    CHECK(std::holds_alternative<bool>(result));
    CHECK(std::get<bool>(result));
}

std::string
expect_string_ok(op_result<std::string>&& result) {
    CHECK(std::holds_alternative<std::string>(result));
    return std::move(std::get<std::string>(result));
}

void
expect_void_ok(void_result&& result) {
    CHECK(std::holds_alternative<std::monostate>(result));
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
persist_ctx_ok(value_fixture& fx, core::batch_ctx& ctx) {
    auto sub = submit_value<bool>([&]() {
        return write_path::persist_put_values(ctx, fx.provider);
    });
    drive_until_ready(fx, sub);
    expect_bool_ok(sub.fut.get());
    CHECK(fx.nvme.idle());
}

void
m07_put_batch_persists_and_fills_value_refs() {
    value_fixture fx;
    auto ctx = make_ctx({
        {.op = core::write_op_type::put, .key = "a", .value = "one"},
        {.op = core::write_op_type::put, .key = "b", .value = "two"},
        {.op = core::write_op_type::put, .key = "c", .value = std::string(180, 'c')},
        {.op = core::write_op_type::put, .key = "d", .value = std::string(800, 'd')},
    }, 701);

    persist_ctx_ok(fx, ctx);

    CHECK(fx.nvme.writes.calls > 0);
    CHECK(fx.nvme.fua_writes == fx.nvme.writes.calls);
    expect_value_refs_nondefault_and_disjoint(ctx, fx.class_sizes);
    CHECK(has_two_puts_on_same_page(ctx));
}

void
m07_read_value_round_trip() {
    value_fixture fx;
    auto ctx = make_ctx({
        {.op = core::write_op_type::put, .key = "a", .value = "alpha-body"},
        {.op = core::write_op_type::put, .key = "b", .value = "beta-body"},
    }, 702);

    auto persist_sub = submit_value<bool>([&]() {
        return write_path::persist_put_values(ctx, fx.provider);
    });

    for (uint32_t i = 0; fx.nvme.writes.calls == 0 && i < 10000; ++i) {
        (void)fx.sched.advance();
    }
    CHECK(fx.nvme.writes.calls > 0);
    CHECK(!ready(persist_sub.fut));

    const auto vr0 = ctx.canonical_entries[0].allocated_vr;
    auto round_read = submit_value<std::string>([&]() {
        return value::read_value(vr0, fx.provider);
    });
    drive_until_ready(fx, round_read, false);
    CHECK(expect_string_ok(round_read.fut.get()) == "alpha-body");
    CHECK(fx.nvme.reads.calls == 0);

    drive_until_ready(fx, persist_sub);
    expect_bool_ok(persist_sub.fut.get());
    CHECK(fx.nvme.idle());

    const auto vr1 = ctx.canonical_entries[1].allocated_vr;
    auto cached_read = submit_value<std::string>([&]() {
        return value::read_value(vr1, fx.provider);
    });
    drive_until_ready(fx, cached_read);
    CHECK(expect_string_ok(cached_read.fut.get()) == "beta-body");
    CHECK(fx.nvme.reads.calls == 0);

    value_fixture miss_fx;
    miss_fx.nvme.copy_disk_from(fx.nvme);
    auto miss_read = submit_value<std::string>([&]() {
        return value::read_value(vr0, miss_fx.provider);
    });
    drive_until_ready(miss_fx, miss_read);
    CHECK(expect_string_ok(miss_read.fut.get()) == "alpha-body");
    CHECK(miss_fx.nvme.reads.calls == 1);
}

void
m07_delete_only_batch_skips_value_module() {
    value_fixture fx;
    auto ctx = make_ctx({
        {.op = core::write_op_type::del, .key = "gone-a", .value = ""},
        {.op = core::write_op_type::del, .key = "gone-b", .value = ""},
    }, 703);

    persist_ctx_ok(fx, ctx);
    CHECK(fx.nvme.total_calls() == 0);

    auto put_ctx = make_ctx({
        {.op = core::write_op_type::put, .key = "ok", .value = "after-delete-only"},
    }, 704);
    persist_ctx_ok(fx, put_ctx);
    expect_value_refs_nondefault_and_disjoint(put_ctx, fx.class_sizes);
}

void
m07_mixed_batch_only_puts_persist() {
    value_fixture fx;
    auto ctx = make_ctx({
        {.op = core::write_op_type::put, .key = "a", .value = "value-a"},
        {.op = core::write_op_type::del, .key = "gone", .value = ""},
        {.op = core::write_op_type::put, .key = "b", .value = "value-b"},
    }, 705);

    persist_ctx_ok(fx, ctx);
    CHECK(ctx.canonical_entries[0].allocated_vr.base.lba != 0);
    CHECK(ctx.canonical_entries[1].allocated_vr.base.lba == 0);
    CHECK(ctx.canonical_entries[1].allocated_vr.len == 0);
    CHECK(ctx.canonical_entries[2].allocated_vr.base.lba != 0);
    CHECK(fx.nvme.writes.calls > 0);
}

void
m07_bounded_write_inflight() {
    value_fixture fx(value::value_io_policy{
        .max_write_inflight = 2,
        .max_read_inflight = 8,
        .max_trim_inflight = 8,
    });

    std::vector<std::string> values;
    std::vector<core::raw_batch_op> ops;
    for (uint32_t i = 0; i < 20; ++i) {
        values.push_back(std::string(3000, static_cast<char>('a' + (i % 20))));
        ops.push_back(core::raw_batch_op{
            .op = core::write_op_type::put,
            .key = std::string("k") + std::to_string(i),
            .value = values.back(),
        });
    }
    auto ctx = make_ctx(std::move(ops), 706);

    persist_ctx_ok(fx, ctx);
    CHECK(fx.nvme.writes.calls > 2);
    CHECK(fx.nvme.writes.max_active <= 2);
    expect_value_refs_nondefault_and_disjoint(ctx, fx.class_sizes);
}

void
m07_bounded_prefill_read_inflight() {
    test_fake_nvme::scheduler fake;
    std::vector<std::vector<char>> frame_storage(5);
    std::vector<memory::lba_dma_page> pages(5);
    std::vector<memory::segmented_page_frame> frames(5);
    std::vector<memory::frame_read_desc> reads;
    reads.reserve(frames.size());

    for (uint32_t i = 0; i < frames.size(); ++i) {
        const auto base = format::paddr{0, 3000 + i};
        std::vector<char> disk_bytes(kLbaSize, static_cast<char>('A' + i));
        fake.seed_frame(base, 1, std::move(disk_bytes));

        frame_storage[i].assign(kLbaSize, char{0});
        pages[i] = memory::lba_dma_page{
            .buf = frame_storage[i].data(),
            .byte_len = kLbaSize,
        };
        frames[i].id = memory::frame_id{
            .base = base,
            .span_lbas = 1,
            .dom = memory::frame_id::domain::value_page,
        };
        frames[i].pages.push_back(&pages[i]);
        reads.push_back(memory::frame_read_desc{.frame = &frames[i]});
    }

    auto sub = submit_value<bool>([&]() {
        return nvme::read_frame_range_bounded(
            &fake,
            std::span<memory::frame_read_desc>(reads.data(), reads.size()),
            2,
            [](memory::frame_read_desc& d) { return d; });
    });
    for (uint32_t i = 0; !ready(sub.fut) && i < 10000; ++i) {
        bool progress = fake.advance_one();
        if (!progress) std::this_thread::yield();
    }
    CHECK(ready(sub.fut));
    expect_bool_ok(sub.fut.get());
    CHECK(fake.reads.calls == reads.size());
    CHECK(fake.reads.max_active <= 2);
    for (uint32_t i = 0; i < frames.size(); ++i) {
        CHECK(frames[i].lba_bytes(0)[0] == static_cast<char>('A' + i));
    }
}

void
m07_persist_failure_maps_to_error_and_rolls_back() {
    value_fixture fx;
    fx.nvme.fail_kind = test_fake_nvme::op_kind::write;
    fx.nvme.fail_call = 1;
    fx.nvme.fail_with_false = true;

    auto ctx = make_ctx({
        {.op = core::write_op_type::put, .key = "a", .value = std::string(3000, 'a')},
        {.op = core::write_op_type::put, .key = "b", .value = std::string(3000, 'b')},
    }, 708);
    auto failed = submit_value<bool>([&]() {
        return write_path::persist_put_values(ctx, fx.provider);
    });
    drive_until_ready(fx, failed);
    expect_value_error(
        failed.fut.get(),
        value::value_persist_error_reason::round_failed);

    fx.nvme.clear_failure();
    auto retry = make_ctx({
        {.op = core::write_op_type::put, .key = "retry", .value = "ok"},
    }, 709);
    persist_ctx_ok(fx, retry);
    expect_value_refs_nondefault_and_disjoint(retry, fx.class_sizes);
}

void
m07_follower_round_merge() {
    {
        value_fixture fx;
        auto a = make_ctx({
            {.op = core::write_op_type::put, .key = "a", .value = "follower-a"},
        }, 710);
        auto b = make_ctx({
            {.op = core::write_op_type::put, .key = "b", .value = "follower-b"},
        }, 711);

        auto sub_a = submit_value<bool>([&]() {
            return write_path::persist_put_values(a, fx.provider);
        });
        auto sub_b = submit_value<bool>([&]() {
            return write_path::persist_put_values(b, fx.provider);
        });
        drive_until_both_ready(fx, sub_a, sub_b);
        expect_bool_ok(sub_a.fut.get());
        expect_bool_ok(sub_b.fut.get());
        expect_value_refs_nondefault_and_disjoint(a, fx.class_sizes);
        expect_value_refs_nondefault_and_disjoint(b, fx.class_sizes);
        CHECK(a.canonical_entries[0].allocated_vr.base !=
              b.canonical_entries[0].allocated_vr.base ||
              a.canonical_entries[0].allocated_vr.byte_offset !=
              b.canonical_entries[0].allocated_vr.byte_offset);
    }

    {
        value_fixture fx;
        fx.nvme.fail_kind = test_fake_nvme::op_kind::write;
        fx.nvme.fail_call = 1;
        fx.nvme.fail_with_false = true;

        auto a = make_ctx({
            {.op = core::write_op_type::put, .key = "fa", .value = std::string(3000, 'a')},
        }, 712);
        auto b = make_ctx({
            {.op = core::write_op_type::put, .key = "fb", .value = std::string(3000, 'b')},
        }, 713);
        auto sub_a = submit_value<bool>([&]() {
            return write_path::persist_put_values(a, fx.provider);
        });
        auto sub_b = submit_value<bool>([&]() {
            return write_path::persist_put_values(b, fx.provider);
        });
        drive_until_both_ready(fx, sub_a, sub_b);
        expect_value_error(
            sub_a.fut.get(),
            value::value_persist_error_reason::round_failed);
        expect_value_error(
            sub_b.fut.get(),
            value::value_persist_error_reason::round_failed);
    }
}

void
m07_oversized_put_fails_fast() {
    value_fixture fx;
    std::string oversized(fx.sched.max_body_len() + 1, 'x');
    auto ctx = make_ctx({
        {.op = core::write_op_type::put, .key = "too-big", .value = oversized},
    }, 714);

    auto failed = submit_value<bool>([&]() {
        return write_path::persist_put_values(ctx, fx.provider);
    });
    drive_until_ready(fx, failed);
    expect_value_error(
        failed.fut.get(),
        value::value_persist_error_reason::oversized_value);
    CHECK(fx.nvme.total_calls() == 0);

    auto retry = make_ctx({
        {.op = core::write_op_type::put, .key = "small", .value = "ok"},
    }, 715);
    persist_ctx_ok(fx, retry);
}

void
m07_trim_drain_bounded() {
    value_fixture fx(value::value_io_policy{
        .max_write_inflight = 8,
        .max_read_inflight = 8,
        .max_trim_inflight = 2,
    });

    std::vector<std::string> values;
    std::vector<core::raw_batch_op> ops;
    for (uint32_t i = 0; i < 6; ++i) {
        values.push_back(std::string(3000, static_cast<char>('a' + i)));
        ops.push_back(core::raw_batch_op{
            .op = core::write_op_type::put,
            .key = std::string("trim") + std::to_string(i),
            .value = values.back(),
        });
    }
    auto ctx = make_ctx(std::move(ops), 716);
    persist_ctx_ok(fx, ctx);

    std::vector<format::value_ref> dead;
    for (uint32_t i = 0; i < ctx.put_entry_indices.size(); i += 2) {
        dead.push_back(
            ctx.canonical_entries[ctx.put_entry_indices[i]].allocated_vr);
    }
    auto reclaim = submit_void([&]() {
        return value::reclaim_values(
            std::span<const format::value_ref>(dead.data(), dead.size()));
    });
    drive_until_ready(fx, reclaim);
    expect_void_ok(reclaim.fut.get());

    auto before_free = fx.sched.space().whole_free_lba_count();
    auto trim = submit_void([&]() {
        return value::drain_trim_pending(fx.provider);
    });
    drive_until_ready(fx, trim);
    expect_void_ok(trim.fut.get());
    CHECK(fx.nvme.trims.calls >= 3);
    CHECK(fx.nvme.trims.max_active <= 2);
    CHECK(fx.sched.space().whole_free_lba_count() == before_free);
}

}  // namespace

int
main() {
    m07_put_batch_persists_and_fills_value_refs();
    m07_read_value_round_trip();
    m07_delete_only_batch_skips_value_module();
    m07_mixed_batch_only_puts_persist();
    m07_bounded_write_inflight();
    m07_bounded_prefill_read_inflight();
    m07_persist_failure_maps_to_error_and_rolls_back();
    m07_follower_round_merge();
    m07_oversized_put_fails_fast();
    m07_trim_drain_bounded();
    return 0;
}
