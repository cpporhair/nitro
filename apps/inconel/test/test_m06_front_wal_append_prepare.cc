#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <future>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "apps/inconel/core/batch_carrier.hh"
#include "apps/inconel/front/sender.hh"
#include "apps/inconel/format/wal.hh"
#include "apps/inconel/wal/sender.hh"
#include "apps/inconel/write_path/sender.hh"
#include "pump/core/context.hh"
#include "pump/core/compute_sender_type.hh"
#include "pump/core/op_pusher.hh"
#include "pump/core/op_tuple_builder.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

using namespace apps::inconel;

namespace test_fake_nvme {

class scheduler;

namespace _write {

struct req {
    memory::segmented_page_frame* frame = nullptr;
    uint32_t flags = 0;
    uint32_t call_index = 0;
    std::move_only_function<void(bool)> cb;
    std::move_only_function<void(std::exception_ptr)> fail;
};

struct op {
    constexpr static bool m06_fake_nvme_write_op = true;
    scheduler* sched = nullptr;
    memory::segmented_page_frame* frame = nullptr;
    uint32_t flags = 0;

    template <uint32_t pos, typename ctx_t, typename scope_t>
    void start(ctx_t& ctx, scope_t& scope);
};

struct sender {
    scheduler* sched = nullptr;
    memory::segmented_page_frame* frame = nullptr;
    uint32_t flags = 0;

    auto make_op() {
        return op{.sched = sched, .frame = frame, .flags = flags};
    }

    template <typename ctx_t>
    auto connect() {
        return pump::core::builder::op_list_builder<0>().push_back(make_op());
    }
};

}  // namespace _write

class scheduler {
public:
    bool fail_with_false = false;
    bool fail_with_exception = false;
    uint32_t fail_call = 0;
    uint32_t write_calls = 0;
    uint32_t active = 0;
    uint32_t max_active = 0;
    uint32_t fua_writes = 0;

    ~scheduler() {
        while (!pending_.empty()) {
            delete pending_.front();
            pending_.pop_front();
        }
    }

    auto write_frame(memory::segmented_page_frame* frame, uint32_t flags) {
        return _write::sender{.sched = this, .frame = frame, .flags = flags};
    }

    void schedule(_write::req* r) {
        ++write_calls;
        r->call_index = write_calls;
        if ((r->flags & nvme::IO_FLAGS_FUA) != 0) {
            ++fua_writes;
        }
        ++active;
        max_active = std::max(max_active, active);
        pending_.push_back(r);
    }

    bool advance_one() {
        if (pending_.empty()) return false;
        std::unique_ptr<_write::req> req(pending_.front());
        pending_.pop_front();
        CHECK(active > 0);
        --active;

        if (fail_call != 0 && req->call_index == fail_call) {
            if (fail_with_exception) {
                req->fail(std::make_exception_ptr(
                    std::runtime_error("fake WAL FUA exception")));
                return true;
            }
            if (fail_with_false) {
                req->cb(false);
                return true;
            }
        }

        req->cb(true);
        return true;
    }

    [[nodiscard]] bool idle() const noexcept {
        return pending_.empty() && active == 0;
    }

private:
    std::deque<_write::req*> pending_;
};

template <uint32_t pos, typename ctx_t, typename scope_t>
void
_write::op::start(ctx_t& ctx, scope_t& scope) {
    sched->schedule(new req{
        .frame = frame,
        .flags = flags,
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
    && (get_current_op_type_t<pos, scope_t>::m06_fake_nvme_write_op)
struct op_pusher<pos, scope_t> : op_pusher_base<pos, scope_t> {
    template <typename ctx_t>
    static void push_value(ctx_t& ctx, scope_t& scope) {
        std::get<pos>(scope->get_op_tuple()).template start<pos>(ctx, scope);
    }
};

template <typename ctx_t>
struct compute_sender_type<ctx_t, test_fake_nvme::_write::sender> {
    consteval static uint32_t count_value() { return 1; }
    consteval static auto get_value_type_identity() {
        return std::type_identity<bool>{};
    }
};

}  // namespace pump::core

namespace {

wal::segment_geometry
make_geom(uint32_t count = 2,
          uint64_t base_lba = 1000,
          uint32_t segment_size = 4096,
          uint32_t lba_size = 512) {
    return wal::segment_geometry{
        .wal_base_paddr =
            format::paddr{
                .device_id = 0,
                .lba = base_lba,
            },
        .wal_segment_size = segment_size,
        .lba_size = lba_size,
        .wal_segment_count = count,
        .expected_format_version = format::SUPERBLOCK_FORMAT_VERSION_V1,
    };
}

format::value_ref
make_value_ref(uint64_t lba, uint32_t len = 128) {
    return format::value_ref{
        .base = format::paddr{.device_id = 0, .lba = lba},
        .byte_offset = static_cast<uint16_t>(lba % 4096),
        .len = len,
        .flags = static_cast<uint16_t>((lba % 17) + 1),
    };
}

std::span<const core::canonical_entry>
canonical_span(const core::batch_ctx& ctx) {
    return {ctx.canonical_entries.data(), ctx.canonical_entries.size()};
}

core::batch_ctx
make_ctx(std::vector<core::raw_batch_op> ops,
         uint64_t batch_lsn,
         uint32_t front_count = 1) {
    auto ctx = core::build_batch_ctx(
        core::encode_client_batch(std::span<const core::raw_batch_op>{
            ops.data(), ops.size()}),
        batch_lsn,
        front_count);
    for (uint32_t idx : ctx.put_entry_indices) {
        ctx.canonical_entries[idx].allocated_vr =
            make_value_ref(9000 + batch_lsn * 100 + idx);
    }
    return ctx;
}

const core::front_fragment&
only_fragment(const core::batch_ctx& ctx) {
    CHECK(ctx.fragments.size() == 1);
    return ctx.fragments[0];
}

wal::wal_append_plan
take_ready(wal::wal_prepare_result&& result) {
    CHECK(std::holds_alternative<wal::wal_prepare_ready>(result));
    return std::move(std::get<wal::wal_prepare_ready>(result).plan);
}

wal::wal_prepare_needs_segment
take_needs_segment(wal::wal_prepare_result&& result) {
    CHECK(std::holds_alternative<wal::wal_prepare_needs_segment>(result));
    return std::move(std::get<wal::wal_prepare_needs_segment>(result));
}

void
apply_plan_bytes(const wal::segment_geometry& geom,
                 const wal::wal_append_plan& plan,
                 std::vector<char>& segment_bytes) {
    const auto base = wal::segment_base_paddr(geom, plan.segment);
    for (const auto& write : plan.writes) {
        const auto& frame = write.frame;
        CHECK(frame.id.base.lba >= base.lba);
        const uint64_t page_index = frame.id.base.lba - base.lba;
        const uint64_t offset = page_index * geom.lba_size;
        CHECK(offset + geom.lba_size <= segment_bytes.size());
        const auto src = frame.lba_bytes(0);
        CHECK(src.size() == geom.lba_size);
        std::memcpy(segment_bytes.data() + offset, src.data(), src.size());
    }
}

format::decoded_wal_entry
decode_entry_at(const std::vector<char>& segment_bytes,
                uint32_t offset,
                uint32_t segment_gen,
                uint32_t* out_len = nullptr) {
    format::decoded_wal_entry decoded;
    uint32_t len = 0;
    const auto status = format::decode_wal_entry(
        std::span<const char>{
            segment_bytes.data() + offset,
            segment_bytes.size() - offset},
        segment_gen,
        &decoded,
        &len);
    CHECK(status == format::wal_entry_decode_status::ok);
    if (out_len) *out_len = len;
    return decoded;
}

void
install_new_segment(front::front_sched& front_sched,
                    wal::wal_space_sched& wal_space,
                    uint32_t stream_id,
                    std::optional<wal::sealed_segment_info> sealed =
                        std::nullopt) {
    auto* segment =
        wal_space.try_alloc_segment_for_testing(stream_id, sealed);
    CHECK(segment != nullptr);
    front_sched.install_wal_segment_for_testing(segment);
}

using pipeline_result = std::variant<bool, std::exception_ptr>;

template <typename SenderBuilder>
pipeline_result
submit_wal_pipeline_and_drive(front::front_sched& front_sched,
                              wal::wal_space_sched& wal_space,
                              test_fake_nvme::scheduler& fake_nvme,
                              SenderBuilder&& build_sender) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<pipeline_result>>();
    auto fut = promise->get_future();
    auto caught = std::make_shared<std::exception_ptr>();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::any_exception([caught](std::exception_ptr ep) {
            *caught = std::move(ep);
            return pump::sender::just(false);
        })
        >> pump::sender::then([promise, caught](bool ok) mutable {
            if (*caught) {
                promise->set_value(*caught);
            } else {
                promise->set_value(ok);
            }
        })
        >> pump::sender::submit(ctx);

    for (uint32_t i = 0;
         fut.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready &&
         i < 4096;
         ++i) {
        (void)front_sched.advance();
        (void)wal_space.advance();
        (void)fake_nvme.advance_one();
        std::this_thread::yield();
    }

    CHECK(fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    CHECK(fake_nvme.idle());
    return fut.get();
}

void
expect_pipeline_ok(const pipeline_result& result) {
    CHECK(std::holds_alternative<bool>(result));
    CHECK(std::get<bool>(result));
}

void
expect_device_failure(const pipeline_result& result) {
    CHECK(std::holds_alternative<std::exception_ptr>(result));
    try {
        std::rethrow_exception(std::get<std::exception_ptr>(result));
    } catch (const wal::wal_append_error& e) {
        CHECK(e.reason() == wal::wal_append_error_reason::device_failure);
        return;
    } catch (...) {
    }
    CHECK(false);
}

void
put_delete_entries_decode_and_keep_global_count() {
    auto geom = make_geom();
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    install_new_segment(front_sched, wal_space, 0);

    auto ctx = make_ctx(
        {
            {.op = core::write_op_type::put, .key = "alpha", .value = "a"},
            {.op = core::write_op_type::del, .key = "gone", .value = ""},
        },
        11);
    const auto& fragment = only_fragment(ctx);
    std::vector<char> segment_bytes(geom.wal_segment_size, char{0});

    auto header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(header.kind == wal::wal_plan_kind::header);
    apply_plan_bytes(geom, header, segment_bytes);
    auto raw_header = format::wal_segment_header{};
    std::memcpy(&raw_header, segment_bytes.data(), sizeof(raw_header));
    CHECK(format::inspect_wal_segment_header(
              raw_header, geom.expected_format_version) ==
          format::wal_segment_status::ok);
    CHECK(!front_sched.commit_wal_plan_for_testing(header.plan_id).has_value());

    auto entries = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(entries.kind == wal::wal_plan_kind::entries);
    CHECK(entries.fragment_done);
    apply_plan_bytes(geom, entries, segment_bytes);

    uint32_t len0 = 0;
    auto put = decode_entry_at(
        segment_bytes, entries.start_offset, entries.segment_gen, &len0);
    CHECK(put.op_type == format::wal_op_type::put);
    CHECK(put.lsn == ctx.batch_lsn);
    CHECK(put.entry_count == ctx.entry_count);
    CHECK(put.key == "alpha");
    CHECK(put.vr.has_value());
    CHECK(put.vr->base.lba == ctx.canonical_entries[0].allocated_vr.base.lba);

    auto del = decode_entry_at(
        segment_bytes,
        entries.start_offset + len0,
        entries.segment_gen);
    CHECK(del.op_type == format::wal_op_type::del);
    CHECK(del.entry_count == ctx.entry_count);
    CHECK(del.key == "gone");
    CHECK(!del.vr.has_value());
    CHECK(!front_sched.commit_wal_plan_for_testing(entries.plan_id).has_value());
}

void
fragment_entries_use_batch_global_entry_count() {
    auto geom = make_geom();
    wal::wal_space_sched wal_space(geom, 2);

    auto ctx = make_ctx(
        {
            {.op = core::write_op_type::put, .key = "a0", .value = "0"},
            {.op = core::write_op_type::put, .key = "a1", .value = "1"},
            {.op = core::write_op_type::put, .key = "a2", .value = "2"},
            {.op = core::write_op_type::del, .key = "a3", .value = ""},
        },
        12,
        2);
    CHECK(ctx.fragments.size() == 2);
    const auto& fragment = ctx.fragments.front();
    CHECK(fragment.entry_indices.size() < ctx.entry_count);

    front::front_sched front_sched(fragment.owner, 2, geom);
    install_new_segment(front_sched, wal_space, fragment.owner);
    auto header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(!front_sched.commit_wal_plan_for_testing(header.plan_id).has_value());

    std::vector<char> segment_bytes(geom.wal_segment_size, char{0});
    apply_plan_bytes(geom, header, segment_bytes);
    auto entries = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    apply_plan_bytes(geom, entries, segment_bytes);

    uint32_t offset = entries.start_offset;
    for (uint32_t i = 0; i < fragment.entry_indices.size(); ++i) {
        uint32_t len = 0;
        const auto decoded =
            decode_entry_at(segment_bytes, offset, entries.segment_gen, &len);
        CHECK(decoded.entry_count == ctx.entry_count);
        offset += len;
    }
    CHECK(!front_sched.commit_wal_plan_for_testing(entries.plan_id).has_value());
}

void
entry_can_cross_lba_page_without_crossing_segment() {
    auto geom = make_geom(1, 2000, 2048, 128);
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    install_new_segment(front_sched, wal_space, 0);

    const std::string long_key(180, 'k');
    auto ctx = make_ctx(
        {
            {.op = core::write_op_type::put, .key = long_key, .value = "v"},
        },
        13);
    const auto& fragment = only_fragment(ctx);
    std::vector<char> segment_bytes(geom.wal_segment_size, char{0});

    auto header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    apply_plan_bytes(geom, header, segment_bytes);
    CHECK(!front_sched.commit_wal_plan_for_testing(header.plan_id).has_value());

    auto entries = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(entries.writes.size() >= 2);
    CHECK(entries.end_offset > geom.lba_size);
    CHECK(entries.end_offset < wal::segment_usable_end_offset(geom));
    apply_plan_bytes(geom, entries, segment_bytes);

    const auto decoded = decode_entry_at(
        segment_bytes, entries.start_offset, entries.segment_gen);
    CHECK(decoded.op_type == format::wal_op_type::put);
    CHECK(decoded.key == long_key);
    CHECK(!front_sched.commit_wal_plan_for_testing(entries.plan_id).has_value());
}

void
page_budget_bounds_prepare_without_splitting_single_entry() {
    auto geom = make_geom(1, 3000, 2048, 128);
    wal::wal_append_config config{
        .max_fua_inflight = 2,
        .max_pages_per_plan = 1,
    };
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom, config);
    install_new_segment(front_sched, wal_space, 0);

    auto ctx = make_ctx(
        {
            {.op = core::write_op_type::put, .key = std::string(33, 'a'), .value = "0"},
            {.op = core::write_op_type::put, .key = std::string(33, 'b'), .value = "1"},
        },
        14);
    const auto& fragment = only_fragment(ctx);

    auto header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(!front_sched.commit_wal_plan_for_testing(header.plan_id).has_value());

    auto entries = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(entries.kind == wal::wal_plan_kind::entries);
    CHECK(entries.cursor_after.next_fragment_entry == 1);
    CHECK(!entries.fragment_done);
    CHECK(entries.writes.size() == 1);
    CHECK(!front_sched.commit_wal_plan_for_testing(entries.plan_id).has_value());
}

void
segment_rotation_writes_trailer_then_new_header() {
    auto geom = make_geom(2, 4000, 4096, 512);
    wal::wal_append_config config{
        .max_fua_inflight = 4,
        .max_pages_per_plan = 64,
    };
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom, config);
    install_new_segment(front_sched, wal_space, 0);

    std::vector<core::raw_batch_op> ops;
    for (uint32_t i = 0; i < 4; ++i) {
        ops.push_back(core::raw_batch_op{
            .op = core::write_op_type::put,
            .key = std::string(900, static_cast<char>('a' + i)),
            .value = "v",
        });
    }
    auto ctx = make_ctx(std::move(ops), 15);
    const auto& fragment = only_fragment(ctx);
    std::vector<char> first_segment(geom.wal_segment_size, char{0});
    std::vector<char> second_segment(geom.wal_segment_size, char{0});

    auto header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    apply_plan_bytes(geom, header, first_segment);
    CHECK(!front_sched.commit_wal_plan_for_testing(header.plan_id).has_value());

    auto first_entries = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(first_entries.cursor_after.next_fragment_entry == 3);
    CHECK(!first_entries.fragment_done);
    apply_plan_bytes(geom, first_entries, first_segment);
    CHECK(!front_sched.commit_wal_plan_for_testing(
        first_entries.plan_id).has_value());

    auto trailer = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), first_entries.cursor_after));
    CHECK(trailer.kind == wal::wal_plan_kind::trailer);
    apply_plan_bytes(geom, trailer, first_segment);
    format::wal_sealed_trailer raw_trailer{};
    std::memcpy(
        &raw_trailer,
        first_segment.data() + trailer.start_offset,
        sizeof(raw_trailer));
    CHECK(format::inspect_wal_sealed_trailer(raw_trailer) ==
          format::wal_trailer_status::ok);
    CHECK(raw_trailer.write_end == first_entries.end_offset);
    auto sealed = front_sched.commit_wal_plan_for_testing(trailer.plan_id);
    CHECK(sealed.has_value());
    CHECK(sealed->min_lsn == ctx.batch_lsn);
    CHECK(sealed->max_lsn == ctx.batch_lsn);

    install_new_segment(front_sched, wal_space, 0, sealed);
    auto next_header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), first_entries.cursor_after));
    CHECK(next_header.kind == wal::wal_plan_kind::header);
    CHECK(next_header.segment.index != header.segment.index);
    apply_plan_bytes(geom, next_header, second_segment);
    CHECK(!front_sched.commit_wal_plan_for_testing(
        next_header.plan_id).has_value());

    auto last_entry = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), first_entries.cursor_after));
    CHECK(last_entry.fragment_done);
    CHECK(last_entry.cursor_after.next_fragment_entry ==
          fragment.entry_indices.size());
    apply_plan_bytes(geom, last_entry, second_segment);
    const auto decoded = decode_entry_at(
        second_segment, last_entry.start_offset, last_entry.segment_gen);
    CHECK(decoded.entry_count == ctx.entry_count);
    CHECK(!front_sched.commit_wal_plan_for_testing(
        last_entry.plan_id).has_value());
}

void
abort_after_fua_failure_keeps_cursor_and_memtable_unchanged() {
    auto geom = make_geom();
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    install_new_segment(front_sched, wal_space, 0);

    auto ctx = make_ctx(
        {
            {.op = core::write_op_type::put, .key = "no-memtable", .value = "v"},
        },
        16);
    const auto& fragment = only_fragment(ctx);
    auto header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(!front_sched.commit_wal_plan_for_testing(header.plan_id).has_value());
    CHECK(front_sched.wal_write_offset_for_testing() ==
          format::WAL_SEGMENT_HEADER_SIZE);

    auto entries = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(front_sched.wal_write_offset_for_testing() ==
          format::WAL_SEGMENT_HEADER_SIZE);
    CHECK(front_sched.wal_has_pending_plan_for_testing());
    front_sched.abort_wal_plan_for_testing(entries.plan_id);
    CHECK(!front_sched.wal_has_pending_plan_for_testing());
    CHECK(front_sched.wal_write_offset_for_testing() ==
          format::WAL_SEGMENT_HEADER_SIZE);

    const auto result = front_sched.lookup_memtable_for_testing(
        "no-memtable",
        ctx.batch_lsn,
        core::front_read_set{.active = front_sched.active_for_testing()});
    CHECK(std::holds_alternative<core::memtable_miss>(result));
}

void
l3_write_wal_fragment_allocates_rotates_and_issues_bounded_fua() {
    auto geom = make_geom(2, 5000, 4096, 512);
    wal::wal_append_config config{
        .max_fua_inflight = 2,
        .max_pages_per_plan = 64,
    };
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom, config);
    test_fake_nvme::scheduler fake_nvme;

    std::vector<core::raw_batch_op> ops;
    for (uint32_t i = 0; i < 4; ++i) {
        ops.push_back(core::raw_batch_op{
            .op = core::write_op_type::put,
            .key = std::string(900, static_cast<char>('a' + i)),
            .value = "v",
        });
    }
    auto ctx = make_ctx(std::move(ops), 17);
    const auto& fragment = only_fragment(ctx);

    auto result = submit_wal_pipeline_and_drive(
        front_sched,
        wal_space,
        fake_nvme,
        [&] {
            return write_path::write_wal_fragment(
                front_sched,
                wal_space,
                &fake_nvme,
                fragment,
                canonical_span(ctx));
        });
    expect_pipeline_ok(result);

    CHECK(!front_sched.wal_has_pending_plan_for_testing());
    CHECK(fake_nvme.write_calls >= 5);
    CHECK(fake_nvme.fua_writes == fake_nvme.write_calls);
    CHECK(fake_nvme.max_active <= config.max_fua_inflight);
    CHECK(wal_space.sealed_segment_count_for_testing() == 1);

    const auto lookup = front_sched.lookup_memtable_for_testing(
        std::string(900, 'a'),
        ctx.batch_lsn,
        core::front_read_set{.active = front_sched.active_for_testing()});
    CHECK(std::holds_alternative<core::memtable_miss>(lookup));
}

void
l3_write_wal_fragment_false_aborts_and_throws() {
    auto geom = make_geom(2, 6000, 4096, 512);
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    test_fake_nvme::scheduler fake_nvme;
    fake_nvme.fail_call = 2;
    fake_nvme.fail_with_false = true;

    auto ctx = make_ctx(
        {
            {.op = core::write_op_type::put, .key = "fua-false", .value = "v"},
        },
        18);
    const auto& fragment = only_fragment(ctx);

    auto result = submit_wal_pipeline_and_drive(
        front_sched,
        wal_space,
        fake_nvme,
        [&] {
            return write_path::write_wal_fragment(
                front_sched,
                wal_space,
                &fake_nvme,
                fragment,
                canonical_span(ctx));
        });
    expect_device_failure(result);

    CHECK(!front_sched.wal_has_pending_plan_for_testing());
    CHECK(front_sched.wal_write_offset_for_testing() ==
          format::WAL_SEGMENT_HEADER_SIZE);
    const auto lookup = front_sched.lookup_memtable_for_testing(
        "fua-false",
        ctx.batch_lsn,
        core::front_read_set{.active = front_sched.active_for_testing()});
    CHECK(std::holds_alternative<core::memtable_miss>(lookup));
}

void
l3_write_wal_fragment_exception_aborts_and_throws() {
    auto geom = make_geom(2, 7000, 4096, 512);
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    test_fake_nvme::scheduler fake_nvme;
    fake_nvme.fail_call = 2;
    fake_nvme.fail_with_exception = true;

    auto ctx = make_ctx(
        {
            {.op = core::write_op_type::put, .key = "fua-ex", .value = "v"},
        },
        19);
    const auto& fragment = only_fragment(ctx);

    auto result = submit_wal_pipeline_and_drive(
        front_sched,
        wal_space,
        fake_nvme,
        [&] {
            return write_path::write_wal_fragment(
                front_sched,
                wal_space,
                &fake_nvme,
                fragment,
                canonical_span(ctx));
        });
    expect_device_failure(result);

    CHECK(!front_sched.wal_has_pending_plan_for_testing());
    CHECK(front_sched.wal_write_offset_for_testing() ==
          format::WAL_SEGMENT_HEADER_SIZE);
    const auto lookup = front_sched.lookup_memtable_for_testing(
        "fua-ex",
        ctx.batch_lsn,
        core::front_read_set{.active = front_sched.active_for_testing()});
    CHECK(std::holds_alternative<core::memtable_miss>(lookup));
}

void
write_path_uses_bounded_fua_concurrent() {
    std::ifstream write_path_in("apps/inconel/write_path/sender.hh");
    CHECK(write_path_in.good());
    const std::string write_path_source(
        (std::istreambuf_iterator<char>(write_path_in)),
        std::istreambuf_iterator<char>());
    CHECK(write_path_source.find("write_wal_fragment") != std::string::npos);
    CHECK(write_path_source.find("write_frame_range_bounded_fua") !=
          std::string::npos);
    CHECK(write_path_source.find("concurrent(") == std::string::npos);

    std::ifstream nvme_in("apps/inconel/nvme/frame_io.hh");
    CHECK(nvme_in.good());
    const std::string nvme_source(
        (std::istreambuf_iterator<char>(nvme_in)),
        std::istreambuf_iterator<char>());
    CHECK(nvme_source.find("write_frame_range_bounded_fua") !=
          std::string::npos);
    CHECK(nvme_source.find("concurrent(max_inflight)") !=
          std::string::npos);
    CHECK(nvme_source.find("IO_FLAGS_FUA") !=
          std::string::npos);
    CHECK(nvme_source.find("concurrent()") == std::string::npos);

    std::ifstream front_in("apps/inconel/front/sender.hh");
    CHECK(front_in.good());
    const std::string front_source(
        (std::istreambuf_iterator<char>(front_in)),
        std::istreambuf_iterator<char>());
    CHECK(front_source.find("runtime_scheduler") == std::string::npos);
    CHECK(front_source.find("nvme::write_frame") == std::string::npos);
}

}  // namespace

int
main() {
    put_delete_entries_decode_and_keep_global_count();
    fragment_entries_use_batch_global_entry_count();
    entry_can_cross_lba_page_without_crossing_segment();
    page_budget_bounds_prepare_without_splitting_single_entry();
    segment_rotation_writes_trailer_then_new_header();
    abort_after_fua_failure_keeps_cursor_and_memtable_unchanged();
    l3_write_wal_fragment_allocates_rotates_and_issues_bounded_fua();
    l3_write_wal_fragment_false_aborts_and_throws();
    l3_write_wal_fragment_exception_aborts_and_throws();
    write_path_uses_bounded_fua_concurrent();
    return 0;
}
