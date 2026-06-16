#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
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
#include "apps/inconel/test/wal_test_support.hh"
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
    CHECK(std::holds_alternative<wal::wal_prepare_issue_plan>(result));
    return std::move(std::get<wal::wal_prepare_issue_plan>(result).plan);
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

std::vector<uint64_t>
plan_page_indices(const wal::segment_geometry& geom,
                  const wal::wal_append_plan& plan) {
    const auto base = wal::segment_base_paddr(geom, plan.segment);
    std::vector<uint64_t> pages;
    pages.reserve(plan.writes.size());
    for (const auto& write : plan.writes) {
        CHECK(write.frame.id.span_lbas == 1);
        CHECK(write.frame.id.base.lba >= base.lba);
        pages.push_back(write.frame.id.base.lba - base.lba);
    }
    std::sort(pages.begin(), pages.end());
    CHECK(std::unique(pages.begin(), pages.end()) == pages.end());
    return pages;
}

format::wal_segment_header
read_segment_header(const std::vector<char>& segment_bytes) {
    CHECK(segment_bytes.size() >= sizeof(format::wal_segment_header));
    format::wal_segment_header raw_header{};
    std::memcpy(&raw_header, segment_bytes.data(), sizeof(raw_header));
    return raw_header;
}

format::wal_sealed_trailer
read_fixed_tail_trailer(const wal::segment_geometry& geom,
                        const std::vector<char>& segment_bytes) {
    const uint32_t fixed_tail_offset =
        geom.wal_segment_size - wal::trailer_reserved_bytes(geom);
    CHECK(fixed_tail_offset + sizeof(format::wal_sealed_trailer) <=
          segment_bytes.size());

    format::wal_sealed_trailer raw_trailer{};
    std::memcpy(
        &raw_trailer,
        segment_bytes.data() + fixed_tail_offset,
        sizeof(raw_trailer));
    return raw_trailer;
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

uint32_t
decode_entries_until_stop(const std::vector<char>& segment_bytes,
                          uint32_t offset,
                          uint32_t limit,
                          uint32_t segment_gen,
                          uint32_t* out_count = nullptr) {
    CHECK(offset <= limit);
    CHECK(limit <= segment_bytes.size());

    uint32_t count = 0;
    while (offset < limit) {
        uint32_t len = 0;
        format::decoded_wal_entry decoded;
        const auto status = format::decode_wal_entry(
            std::span<const char>{
                segment_bytes.data() + offset,
                limit - offset},
            segment_gen,
            &decoded,
            &len);
        if (status != format::wal_entry_decode_status::ok) break;
        CHECK(len > 0);
        CHECK(offset + len <= limit);
        offset += len;
        ++count;
    }

    if (out_count) *out_count = count;
    return offset;
}

void
expect_entries_decode_exactly(const std::vector<char>& segment_bytes,
                              uint32_t offset,
                              uint32_t end,
                              uint32_t segment_gen,
                              uint32_t expected_count) {
    uint32_t count = 0;
    const uint32_t decoded_end = decode_entries_until_stop(
        segment_bytes, offset, end, segment_gen, &count);
    CHECK(decoded_end == end);
    CHECK(count == expected_count);
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
using root_context_t = decltype(pump::core::make_root_context());
using prepare_probe_result =
    std::variant<wal::wal_prepare_result, std::exception_ptr>;

struct pipeline_submission {
    root_context_t ctx;
    std::future<pipeline_result> fut;
};

struct prepare_submission {
    root_context_t ctx;
    std::future<prepare_probe_result> fut;
};

template <typename SenderBuilder>
pipeline_submission
submit_wal_pipeline(SenderBuilder&& build_sender) {
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

    return pipeline_submission{.ctx = std::move(ctx), .fut = std::move(fut)};
}

prepare_submission
submit_prepare(front::front_sched& front_sched,
               const core::front_fragment& fragment,
               std::span<const core::canonical_entry> canonical_entries,
               wal::wal_fragment_cursor cursor) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<prepare_probe_result>>();
    auto fut = promise->get_future();

    front::prepare_wal_fragment(
        front_sched, fragment, canonical_entries, cursor)
        >> pump::sender::then([promise](wal::wal_prepare_result&& result) {
            promise->set_value(std::move(result));
        })
        >> pump::sender::any_exception([promise](std::exception_ptr ep) {
            promise->set_value(std::move(ep));
            return pump::sender::just();
        })
        >> pump::sender::submit(ctx);

    return prepare_submission{.ctx = std::move(ctx), .fut = std::move(fut)};
}

template <typename SenderBuilder>
pipeline_result
submit_wal_pipeline_and_drive(front::front_sched& front_sched,
                              wal::wal_space_sched& wal_space,
                              test_fake_nvme::scheduler& fake_nvme,
                              SenderBuilder&& build_sender) {
    auto submission =
        submit_wal_pipeline(std::forward<SenderBuilder>(build_sender));

    for (uint32_t i = 0;
         submission.fut.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready &&
         i < 4096;
         ++i) {
        (void)front_sched.advance();
        (void)wal_space.advance();
        (void)fake_nvme.advance_one();
        std::this_thread::yield();
    }

    CHECK(submission.fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    CHECK(fake_nvme.idle());
    return submission.fut.get();
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
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
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
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 2);

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
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
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
m06_middle_pages_not_zeroed_but_suffix_zeroed() {
    auto geom = make_geom(1, 13000, 2048, 128);
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
    front::front_sched front_sched(0, 1, geom);
    install_new_segment(front_sched, wal_space, 0);

    const std::string long_key(320, 'm');
    auto ctx = make_ctx(
        {
            {.op = core::write_op_type::put, .key = long_key, .value = "v"},
        },
        32);
    const auto& fragment = only_fragment(ctx);
    std::vector<char> segment_bytes(
        geom.wal_segment_size, static_cast<char>(0x7f));

    auto header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    apply_plan_bytes(geom, header, segment_bytes);
    CHECK(!front_sched.commit_wal_plan_for_testing(header.plan_id).has_value());

    auto entries = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(entries.kind == wal::wal_plan_kind::entries);
    const auto pages = plan_page_indices(geom, entries);
    CHECK(pages.size() >= 3);
    apply_plan_bytes(geom, entries, segment_bytes);

    expect_entries_decode_exactly(
        segment_bytes,
        entries.start_offset,
        entries.end_offset,
        entries.segment_gen,
        static_cast<uint32_t>(fragment.entry_indices.size()));

    const uint64_t first_page = pages.front();
    const uint64_t last_page = pages.back();
    for (const auto page : pages) {
        if (page == first_page || page == last_page) continue;
        const auto begin =
            segment_bytes.begin() +
            static_cast<std::ptrdiff_t>(page * geom.lba_size);
        const auto end = begin + static_cast<std::ptrdiff_t>(geom.lba_size);
        CHECK(std::any_of(begin, end, [](char c) { return c != char{0}; }));
    }

    const uint32_t end_mod = entries.end_offset & (geom.lba_size - 1);
    CHECK(end_mod != 0);
    const uint64_t last_page_start = last_page * geom.lba_size;
    for (uint32_t i = end_mod; i < geom.lba_size; ++i) {
        CHECK(segment_bytes[last_page_start + i] == char{0});
    }

    CHECK(!front_sched.commit_wal_plan_for_testing(entries.plan_id).has_value());
}

void
page_budget_bounds_prepare_without_splitting_single_entry() {
    auto geom = make_geom(1, 3000, 2048, 128);
    wal::wal_append_config config{
        .max_fua_inflight = 2,
        .max_pages_per_plan = 1,
    };
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
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
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
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
    CHECK(trailer.start_offset == wal::segment_usable_end_offset(geom));
    apply_plan_bytes(geom, trailer, first_segment);
    const auto raw_trailer = read_fixed_tail_trailer(geom, first_segment);
    CHECK(format::inspect_wal_sealed_trailer(raw_trailer) ==
          format::wal_trailer_status::ok);
    CHECK(raw_trailer.write_end == first_entries.end_offset);
    const auto entry_pages = plan_page_indices(geom, first_entries);
    const auto trailer_pages = plan_page_indices(geom, trailer);
    for (const auto trailer_page : trailer_pages) {
        CHECK(std::find(entry_pages.begin(), entry_pages.end(), trailer_page) ==
              entry_pages.end());
    }
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
m06_trailer_at_fixed_tail_region_decodes_for_recovery_view() {
    auto geom = make_geom(2, 11000, 4096, 512);
    wal::wal_append_config config{
        .max_fua_inflight = 4,
        .max_pages_per_plan = 64,
    };
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
    front::front_sched front_sched(0, 1, geom, config);
    install_new_segment(front_sched, wal_space, 0);

    std::vector<core::raw_batch_op> ops;
    for (uint32_t i = 0; i < 4; ++i) {
        ops.push_back(core::raw_batch_op{
            .op = core::write_op_type::put,
            .key = std::string(900, static_cast<char>('r' + i)),
            .value = "v",
        });
    }
    auto ctx = make_ctx(std::move(ops), 29);
    const auto& fragment = only_fragment(ctx);
    std::vector<char> segment_bytes(geom.wal_segment_size, char{0});

    auto header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    apply_plan_bytes(geom, header, segment_bytes);
    CHECK(!front_sched.commit_wal_plan_for_testing(header.plan_id).has_value());

    auto entries = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), {}));
    CHECK(!entries.fragment_done);
    apply_plan_bytes(geom, entries, segment_bytes);
    CHECK(!front_sched.commit_wal_plan_for_testing(
        entries.plan_id).has_value());

    auto trailer = take_ready(front_sched.prepare_wal_fragment_for_testing(
        fragment, canonical_span(ctx), entries.cursor_after));
    CHECK(trailer.kind == wal::wal_plan_kind::trailer);
    apply_plan_bytes(geom, trailer, segment_bytes);

    const auto raw_header = read_segment_header(segment_bytes);
    CHECK(format::inspect_wal_segment_header(
              raw_header, geom.expected_format_version) ==
          format::wal_segment_status::ok);

    const auto raw_trailer = read_fixed_tail_trailer(geom, segment_bytes);
    CHECK(raw_trailer.magic == format::WAL_SEAL_MAGIC);
    CHECK(raw_trailer.segment_gen == raw_header.segment_gen);
    CHECK(raw_trailer.crc == format::wal_sealed_trailer_crc(raw_trailer));
    CHECK(format::inspect_wal_sealed_trailer(raw_trailer) ==
          format::wal_trailer_status::ok);

    const uint32_t fixed_tail_offset =
        geom.wal_segment_size - wal::trailer_reserved_bytes(geom);
    uint32_t decoded_count = 0;
    const uint32_t decoded_end = decode_entries_until_stop(
        segment_bytes,
        format::WAL_SEGMENT_HEADER_SIZE,
        fixed_tail_offset,
        raw_header.segment_gen,
        &decoded_count);
    CHECK(decoded_count == entries.cursor_after.next_fragment_entry);
    CHECK(raw_trailer.write_end == decoded_end);
    CHECK(raw_trailer.min_lsn == ctx.batch_lsn);
    CHECK(raw_trailer.max_lsn == ctx.batch_lsn);

    auto sealed = front_sched.commit_wal_plan_for_testing(trailer.plan_id);
    CHECK(sealed.has_value());
}

void
abort_after_fua_failure_keeps_cursor_and_memtable_unchanged() {
    auto geom = make_geom();
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
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
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
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
m06_frames_return_to_pool_on_front_commit() {
    auto geom = make_geom(2, 12000, 4096, 512);
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
    front::front_sched front_sched(0, 1, geom);
    test_fake_nvme::scheduler fake_nvme;

    auto warm_ctx = make_ctx(
        {{.op = core::write_op_type::put, .key = "pool-warm", .value = "v"}},
        30);
    const auto& warm_fragment = only_fragment(warm_ctx);
    auto warm_result = submit_wal_pipeline_and_drive(
        front_sched,
        wal_space,
        fake_nvme,
        [&] {
            return write_path::write_wal_fragment(
                front_sched,
                wal_space,
                &fake_nvme,
                warm_fragment,
                canonical_span(warm_ctx));
        });
    expect_pipeline_ok(warm_result);

    const auto initial_free =
        front_sched.wal_frame_pool_free_pages_for_testing();
    CHECK(initial_free > 0);
    CHECK(!front_sched.wal_has_pending_plan_for_testing());

    auto ctx = make_ctx(
        {{.op = core::write_op_type::put, .key = "pool-held", .value = "v"}},
        31);
    const auto& fragment = only_fragment(ctx);
    auto submission = submit_wal_pipeline([&] {
        return write_path::write_wal_fragment(
            front_sched, wal_space, &fake_nvme, fragment, canonical_span(ctx));
    });

    for (uint32_t i = 0;
         fake_nvme.active == 0 &&
         submission.fut.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready &&
         i < 128;
         ++i) {
        (void)front_sched.advance();
        (void)wal_space.advance();
        std::this_thread::yield();
    }
    CHECK(fake_nvme.active > 0);

    while (!fake_nvme.idle()) {
        (void)fake_nvme.advance_one();
    }
    CHECK(fake_nvme.idle());
    CHECK(submission.fut.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::ready);
    CHECK(front_sched.wal_has_pending_plan_for_testing());
    CHECK(front_sched.wal_frame_pool_free_pages_for_testing() < initial_free);

    CHECK(front_sched.advance());
    CHECK(front_sched.wal_frame_pool_free_pages_for_testing() == initial_free);
    CHECK(submission.fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    expect_pipeline_ok(submission.fut.get());
}

void
m06_concurrent_fragments_serialize_through_wal_gate() {
    auto geom = make_geom(4, 8000, 4096, 512);
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
    front::front_sched front_sched(0, 1, geom);
    test_fake_nvme::scheduler fake_nvme;

    auto ctx_a = make_ctx(
        {{.op = core::write_op_type::put, .key = "gate-a", .value = "v"}},
        21);
    auto ctx_b = make_ctx(
        {{.op = core::write_op_type::put, .key = "gate-b", .value = "v"}},
        22);
    const auto& frag_a = only_fragment(ctx_a);
    const auto& frag_b = only_fragment(ctx_b);

    auto sub_a = submit_wal_pipeline([&] {
        return write_path::write_wal_fragment(
            front_sched, wal_space, &fake_nvme, frag_a, canonical_span(ctx_a));
    });
    auto sub_b = submit_wal_pipeline([&] {
        return write_path::write_wal_fragment(
            front_sched, wal_space, &fake_nvme, frag_b, canonical_span(ctx_b));
    });

    for (uint32_t i = 0;
         (sub_a.fut.wait_for(std::chrono::milliseconds(0)) !=
              std::future_status::ready ||
          sub_b.fut.wait_for(std::chrono::milliseconds(0)) !=
              std::future_status::ready) &&
         i < 8192;
         ++i) {
        (void)front_sched.advance();
        (void)wal_space.advance();
        (void)fake_nvme.advance_one();
        std::this_thread::yield();
    }

    CHECK(sub_a.fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    CHECK(sub_b.fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    CHECK(fake_nvme.idle());
    expect_pipeline_ok(sub_a.fut.get());
    expect_pipeline_ok(sub_b.fut.get());
}

void
m06_prepare_fifo_wakes_after_rotation_install() {
    auto geom = make_geom(3, 9000, 4096, 512);
    wal::wal_append_config config{
        .max_fua_inflight = 2,
        .max_pages_per_plan = 64,
    };
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
    front::front_sched front_sched(0, 1, geom, config);
    test_fake_nvme::scheduler fake_nvme;

    std::vector<core::raw_batch_op> large_ops;
    for (uint32_t i = 0; i < 4; ++i) {
        large_ops.push_back(core::raw_batch_op{
            .op = core::write_op_type::put,
            .key = std::string(900, static_cast<char>('k' + i)),
            .value = "v",
        });
    }
    auto ctx_a = make_ctx(std::move(large_ops), 23);
    auto ctx_b = make_ctx(
        {{.op = core::write_op_type::put, .key = "fifo-b", .value = "v"}},
        24);
    const auto& frag_a = only_fragment(ctx_a);
    const auto& frag_b = only_fragment(ctx_b);

    auto sub_a = submit_wal_pipeline([&] {
        return write_path::write_wal_fragment(
            front_sched, wal_space, &fake_nvme, frag_a, canonical_span(ctx_a));
    });

    auto sub_b = submit_wal_pipeline([&] {
        return write_path::write_wal_fragment(
            front_sched, wal_space, &fake_nvme, frag_b, canonical_span(ctx_b));
    });

    for (uint32_t i = 0;
         (sub_a.fut.wait_for(std::chrono::milliseconds(0)) !=
              std::future_status::ready ||
          sub_b.fut.wait_for(std::chrono::milliseconds(0)) !=
              std::future_status::ready) &&
         i < 8192;
         ++i) {
        (void)front_sched.advance();
        (void)wal_space.advance();
        (void)fake_nvme.advance_one();
        std::this_thread::yield();
    }

    CHECK(sub_a.fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    CHECK(sub_b.fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    expect_pipeline_ok(sub_a.fut.get());
    expect_pipeline_ok(sub_b.fut.get());
}

void
m06_prepare_queue_full_fails_with_explicit_reason() {
    auto geom = make_geom(2, 10000, 4096, 512);
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
    front::front_sched front_sched(0, 1, geom, {}, 2);
    install_new_segment(front_sched, wal_space, 0);

    auto ctx_busy = make_ctx(
        {{.op = core::write_op_type::put, .key = "busy", .value = "v"}},
        25);
    const auto& busy_fragment = only_fragment(ctx_busy);
    auto header = take_ready(front_sched.prepare_wal_fragment_for_testing(
        busy_fragment, canonical_span(ctx_busy), {}));

    auto ctx_a = make_ctx(
        {{.op = core::write_op_type::put, .key = "queued", .value = "v"}},
        26);
    auto ctx_b = make_ctx(
        {{.op = core::write_op_type::put, .key = "overflow", .value = "v"}},
        27);
    auto ctx_c = make_ctx(
        {{.op = core::write_op_type::put, .key = "overflow-2", .value = "v"}},
        28);
    auto sub_a = submit_prepare(
        front_sched, only_fragment(ctx_a), canonical_span(ctx_a), {});
    (void)front_sched.advance();
    CHECK(sub_a.fut.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::ready);

    auto sub_b = submit_prepare(
        front_sched, only_fragment(ctx_b), canonical_span(ctx_b), {});
    (void)front_sched.advance();
    CHECK(sub_b.fut.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::ready);
    auto sub_c = submit_prepare(
        front_sched, only_fragment(ctx_c), canonical_span(ctx_c), {});
    (void)front_sched.advance();
    CHECK(sub_c.fut.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    auto result = sub_c.fut.get();
    CHECK(std::holds_alternative<std::exception_ptr>(result));
    try {
        std::rethrow_exception(std::get<std::exception_ptr>(result));
    } catch (const wal::wal_append_error& e) {
        CHECK(e.reason() == wal::wal_append_error_reason::prepare_queue_full);
    }

    front_sched.abort_wal_plan_for_testing(header.plan_id);
}

void
l3_write_wal_fragment_false_aborts_and_throws() {
    auto geom = make_geom(2, 6000, 4096, 512);
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
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
    INCONEL_TEST_WAL_SPACE_SCHED(wal_space, geom, 1);
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
    m06_middle_pages_not_zeroed_but_suffix_zeroed();
    page_budget_bounds_prepare_without_splitting_single_entry();
    segment_rotation_writes_trailer_then_new_header();
    m06_trailer_at_fixed_tail_region_decodes_for_recovery_view();
    abort_after_fua_failure_keeps_cursor_and_memtable_unchanged();
    l3_write_wal_fragment_allocates_rotates_and_issues_bounded_fua();
    m06_frames_return_to_pool_on_front_commit();
    m06_concurrent_fragments_serialize_through_wal_gate();
    m06_prepare_fifo_wakes_after_rotation_install();
    m06_prepare_queue_full_fails_with_explicit_reason();
    l3_write_wal_fragment_false_aborts_and_throws();
    l3_write_wal_fragment_exception_aborts_and_throws();
    write_path_uses_bounded_fua_concurrent();
    return 0;
}
