// M14: front WAL entry group commit (INC-057 / 054).
//
// Verifies that idle front WAL coalesces queued prepares from the same FIFO
// into one physical plan (one tail-LBA full-page FUA), that the leader issues
// the only I/O while followers only adopt a `committed` completion, that a FUA
// failure fails every participant before the memtable phase, and that the
// rotation boundary / bad-follower / pool-return invariants hold.

#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
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
    constexpr static bool m14_fake_nvme_write_op = true;
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

// Records every FUA page write so the test can reconstruct the durable segment
// image, count tail-LBA writes, and observe the max in-flight depth per LBA.
class scheduler {
public:
    struct recorded_write {
        uint64_t lba = 0;
        uint32_t flags = 0;
        std::vector<char> bytes;
    };

    bool fail_with_false = false;
    bool fail_with_exception = false;
    uint32_t fail_call = 0;
    uint32_t write_calls = 0;
    uint32_t active = 0;
    uint32_t max_active = 0;
    uint32_t fua_writes = 0;
    std::vector<recorded_write> records;
    // active in-flight writes per LBA; the WAL invariant is this never exceeds 1.
    std::deque<uint64_t> inflight_lbas;
    uint32_t max_inflight_same_lba = 0;

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
        const uint64_t lba = r->frame->id.base.lba;
        const auto src = r->frame->lba_bytes(0);
        records.push_back(recorded_write{
            .lba = lba,
            .flags = r->flags,
            .bytes = std::vector<char>(src.begin(), src.end()),
        });
        uint32_t same = 1;
        for (uint64_t in : inflight_lbas) {
            if (in == lba) ++same;
        }
        inflight_lbas.push_back(lba);
        max_inflight_same_lba = std::max(max_inflight_same_lba, same);
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
        for (auto it = inflight_lbas.begin(); it != inflight_lbas.end(); ++it) {
            if (*it == req->frame->id.base.lba) {
                inflight_lbas.erase(it);
                break;
            }
        }

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
    && (get_current_op_type_t<pos, scope_t>::m14_fake_nvme_write_op)
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
make_geom(uint32_t count = 4,
          uint64_t base_lba = 20000,
          uint32_t segment_size = 16384,
          uint32_t lba_size = 4096) {
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
make_value_ref(uint64_t lba, uint32_t len = 64) {
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
take_issue_plan(wal::wal_prepare_result&& result) {
    CHECK(std::holds_alternative<wal::wal_prepare_issue_plan>(result));
    return std::move(std::get<wal::wal_prepare_issue_plan>(result).plan);
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

// Commit the segment header via the synchronous test surface so subsequent
// prepares go straight to entry plans. The header frames return to the pool
// when the local plan leaves scope.
void
commit_header(front::front_sched& front_sched,
              const core::front_fragment& any_fragment,
              std::span<const core::canonical_entry> entries) {
    auto header = take_issue_plan(front_sched.prepare_wal_fragment_for_testing(
        any_fragment, entries, {}));
    CHECK(header.kind == wal::wal_plan_kind::header);
    CHECK(!front_sched.commit_wal_plan_for_testing(header.plan_id).has_value());
}

using root_context_t = decltype(pump::core::make_root_context());
using pipeline_result = std::variant<bool, std::exception_ptr>;
using prepare_probe_result =
    std::variant<wal::wal_prepare_result, std::exception_ptr>;
using commit_probe_result =
    std::variant<std::optional<wal::sealed_segment_info>, std::exception_ptr>;

struct pipeline_submission {
    root_context_t ctx;
    std::future<pipeline_result> fut;
};

struct prepare_submission {
    root_context_t ctx;
    std::future<prepare_probe_result> fut;
};

struct commit_submission {
    root_context_t ctx;
    std::future<commit_probe_result> fut;
};

template <typename nvme_t>
pipeline_submission
submit_write_wal_fragment(front::front_sched& front_sched,
                          wal::wal_space_sched& wal_space,
                          nvme_t& fake_nvme,
                          const core::front_fragment& fragment,
                          std::span<const core::canonical_entry> entries) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<pipeline_result>>();
    auto fut = promise->get_future();
    auto caught = std::make_shared<std::exception_ptr>();

    write_path::write_wal_fragment(
        front_sched, wal_space, &fake_nvme, fragment, entries)
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
               std::span<const core::canonical_entry> entries,
               wal::wal_fragment_cursor cursor) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<prepare_probe_result>>();
    auto fut = promise->get_future();

    front::prepare_wal_fragment(front_sched, fragment, entries, cursor)
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

commit_submission
submit_commit(front::front_sched& front_sched,
              uint64_t plan_id,
              std::vector<wal::wal_frame_write> writes) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<commit_probe_result>>();
    auto fut = promise->get_future();

    front::commit_wal_plan(front_sched, plan_id, std::move(writes))
        >> pump::sender::then(
               [promise](std::optional<wal::sealed_segment_info>&& sealed) {
                   promise->set_value(std::move(sealed));
               })
        >> pump::sender::any_exception([promise](std::exception_ptr ep) {
            promise->set_value(std::move(ep));
            return pump::sender::just();
        })
        >> pump::sender::submit(ctx);

    return commit_submission{.ctx = std::move(ctx), .fut = std::move(fut)};
}

template <typename Fut>
bool
ready(const Fut& fut) {
    return fut.wait_for(std::chrono::milliseconds(0)) ==
           std::future_status::ready;
}

bool
is_issue_plan(const prepare_probe_result& r) {
    return std::holds_alternative<wal::wal_prepare_result>(r) &&
           std::holds_alternative<wal::wal_prepare_issue_plan>(
               std::get<wal::wal_prepare_result>(r));
}

bool
is_committed(const prepare_probe_result& r) {
    return std::holds_alternative<wal::wal_prepare_result>(r) &&
           std::holds_alternative<wal::wal_prepare_committed>(
               std::get<wal::wal_prepare_result>(r));
}

std::vector<char>
reconstruct_segment(const test_fake_nvme::scheduler& nvme,
                    const wal::segment_geometry& geom,
                    uint64_t segment_base_lba) {
    std::vector<char> bytes(geom.wal_segment_size, char{0});
    for (const auto& rec : nvme.records) {
        CHECK(rec.lba >= segment_base_lba);
        const uint64_t page_index = rec.lba - segment_base_lba;
        const uint64_t off = page_index * geom.lba_size;
        CHECK(off + rec.bytes.size() <= bytes.size());
        std::memcpy(bytes.data() + off, rec.bytes.data(), rec.bytes.size());
    }
    return bytes;
}

uint64_t
segment_base_lba(const wal::segment_geometry& geom, front::front_sched&) {
    // All tests install the first free segment (index 0); the recorded writes
    // confirm the absolute LBA matches.
    return wal::segment_base_paddr(
               geom, wal::segment_id{.device_id = 0, .index = 0})
        .lba;
}

// ---------------------------------------------------------------------------

void
m14_wal_group_commit_single_lba_many_batches() {
    auto geom = make_geom();
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    test_fake_nvme::scheduler fake_nvme;
    install_new_segment(front_sched, wal_space, 0);

    constexpr uint32_t N = 8;
    std::vector<core::batch_ctx> ctxs;
    ctxs.reserve(N);
    for (uint32_t i = 0; i < N; ++i) {
        ctxs.push_back(make_ctx(
            {{.op = core::write_op_type::del,
              .key = std::string("k") + static_cast<char>('0' + i),
              .value = ""}},
            100 + i));
    }
    commit_header(front_sched, only_fragment(ctxs[0]), canonical_span(ctxs[0]));

    // Submit every fragment before advancing: the first becomes a solo leader
    // (FIFO empty when its group forms) and the rest queue.
    std::vector<pipeline_submission> subs;
    subs.reserve(N);
    for (uint32_t i = 0; i < N; ++i) {
        subs.push_back(submit_write_wal_fragment(
            front_sched, wal_space, fake_nvme, only_fragment(ctxs[i]),
            canonical_span(ctxs[i])));
    }

    // One advance: leader[0] issues its FUA; followers queue.
    (void)front_sched.advance();
    CHECK(fake_nvme.active == 1);
    CHECK(fake_nvme.fua_writes == 1);

    // Complete leader[0]'s FUA, then drain so the 7 queued fragments coalesce
    // into a single group plan.
    CHECK(fake_nvme.advance_one());
    (void)front_sched.advance();
    (void)wal_space.advance();
    CHECK(fake_nvme.active == 1);    // exactly one group plan in flight
    CHECK(fake_nvme.fua_writes == 2);  // solo leader + one coalesced group

    // Drain everything to completion.
    for (uint32_t i = 0; i < 4096; ++i) {
        bool all_ready = true;
        for (const auto& s : subs) all_ready = all_ready && ready(s.fut);
        if (all_ready && fake_nvme.idle()) break;
        (void)front_sched.advance();
        (void)wal_space.advance();
        (void)fake_nvme.advance_one();
        std::this_thread::yield();
    }
    for (auto& s : subs) {
        CHECK(ready(s.fut));
        auto r = s.fut.get();
        CHECK(std::holds_alternative<bool>(r));
        CHECK(std::get<bool>(r));
    }

    // The whole run wrote the tail LBA exactly twice (solo + group), never
    // converging to N, and never two writes to the same LBA in flight.
    CHECK(fake_nvme.fua_writes == 2);
    CHECK(fake_nvme.fua_writes < N);
    CHECK(fake_nvme.max_inflight_same_lba == 1);

    // Reassemble all batches from the durable WAL bytes by lsn + entry_count.
    const auto base = segment_base_lba(geom, front_sched);
    const auto seg = reconstruct_segment(fake_nvme, geom, base);
    const uint32_t fixed_tail =
        geom.wal_segment_size - wal::trailer_reserved_bytes(geom);
    format::wal_segment_header header{};
    std::memcpy(&header, seg.data(), sizeof(header));
    CHECK(format::inspect_wal_segment_header(
              header, geom.expected_format_version) ==
          format::wal_segment_status::ok);

    std::vector<std::string> keys;
    uint32_t offset = format::WAL_SEGMENT_HEADER_SIZE;
    while (offset < fixed_tail) {
        uint32_t len = 0;
        format::decoded_wal_entry decoded;
        const auto status = format::decode_wal_entry(
            std::span<const char>{seg.data() + offset, fixed_tail - offset},
            header.segment_gen, &decoded, &len);
        if (status != format::wal_entry_decode_status::ok) break;
        CHECK(decoded.op_type == format::wal_op_type::del);
        CHECK(decoded.entry_count == 1);
        keys.emplace_back(decoded.key);
        offset += len;
    }
    std::sort(keys.begin(), keys.end());
    CHECK(keys.size() == N);
    for (uint32_t i = 0; i < N; ++i) {
        CHECK(keys[i] == std::string("k") + static_cast<char>('0' + i));
    }
}

void
m14_wal_group_commit_followers_do_not_issue_nvme() {
    auto geom = make_geom();
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    install_new_segment(front_sched, wal_space, 0);

    auto ctx_gate = make_ctx(
        {{.op = core::write_op_type::del, .key = "gate", .value = ""}}, 200);
    commit_header(front_sched, only_fragment(ctx_gate),
                  canonical_span(ctx_gate));

    // Hold a pending plan so the leader/followers queue behind it.
    auto gate = take_issue_plan(front_sched.prepare_wal_fragment_for_testing(
        only_fragment(ctx_gate), canonical_span(ctx_gate), {}));
    CHECK(gate.kind == wal::wal_plan_kind::entries);
    CHECK(front_sched.wal_has_pending_plan_for_testing());

    auto ctx_l = make_ctx(
        {{.op = core::write_op_type::del, .key = "leader", .value = ""}}, 201);
    auto ctx_f1 = make_ctx(
        {{.op = core::write_op_type::del, .key = "follow1", .value = ""}}, 202);
    auto ctx_f2 = make_ctx(
        {{.op = core::write_op_type::del, .key = "follow2", .value = ""}}, 203);

    auto leader = submit_prepare(front_sched, only_fragment(ctx_l),
                                 canonical_span(ctx_l), {});
    (void)front_sched.advance();
    auto f1 = submit_prepare(front_sched, only_fragment(ctx_f1),
                             canonical_span(ctx_f1), {});
    (void)front_sched.advance();
    auto f2 = submit_prepare(front_sched, only_fragment(ctx_f2),
                             canonical_span(ctx_f2), {});
    (void)front_sched.advance();
    // All three are queued behind the gate; none resolved yet.
    CHECK(!ready(leader.fut));
    CHECK(!ready(f1.fut));
    CHECK(!ready(f2.fut));

    // Commit the gate -> drain -> coalesce leader + f1 + f2 into one group.
    auto gate_commit =
        submit_commit(front_sched, gate.plan_id, std::move(gate.writes));
    (void)front_sched.advance();
    CHECK(ready(gate_commit.fut));

    // Only the leader gets a plan to issue. The followers are parked on the
    // front owner until the leader commits — they never reach an issue_plan
    // branch, so they never touch the nvme scheduler.
    CHECK(ready(leader.fut));
    auto leader_res = leader.fut.get();
    CHECK(is_issue_plan(leader_res));
    auto plan = take_issue_plan(
        std::move(std::get<wal::wal_prepare_result>(leader_res)));
    CHECK(plan.participants.size() == 3);
    CHECK(plan.participants[0].waiter_id == 0);  // leader self
    CHECK(!ready(f1.fut));  // followers parked, not issuing I/O
    CHECK(!ready(f2.fut));

    // The leader commits the single physical plan; the front owner fans out a
    // `committed` completion to each follower (no I/O on the follower path).
    auto leader_commit =
        submit_commit(front_sched, plan.plan_id, std::move(plan.writes));
    (void)front_sched.advance();
    CHECK(ready(leader_commit.fut));
    CHECK(ready(f1.fut));
    CHECK(ready(f2.fut));
    CHECK(is_committed(f1.fut.get()));
    CHECK(is_committed(f2.fut.get()));
}

void
m14_wal_group_commit_fua_failure_releases_all_participants() {
    auto geom = make_geom();
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    test_fake_nvme::scheduler fake_nvme;
    install_new_segment(front_sched, wal_space, 0);

    constexpr uint32_t N = 4;
    std::vector<core::batch_ctx> ctxs;
    ctxs.reserve(N);
    for (uint32_t i = 0; i < N; ++i) {
        ctxs.push_back(make_ctx(
            {{.op = core::write_op_type::del,
              .key = std::string("fail") + static_cast<char>('0' + i),
              .value = ""}},
            300 + i));
    }
    commit_header(front_sched, only_fragment(ctxs[0]), canonical_span(ctxs[0]));

    // Solo leader is call #1 (succeeds); the coalesced group is call #2 (fails).
    fake_nvme.fail_call = 2;
    fake_nvme.fail_with_false = true;

    std::vector<pipeline_submission> subs;
    subs.reserve(N);
    for (uint32_t i = 0; i < N; ++i) {
        subs.push_back(submit_write_wal_fragment(
            front_sched, wal_space, fake_nvme, only_fragment(ctxs[i]),
            canonical_span(ctxs[i])));
    }
    (void)front_sched.advance();  // leader[0] issues call #1
    CHECK(fake_nvme.advance_one());  // complete call #1 (success)

    for (uint32_t i = 0; i < 4096; ++i) {
        bool all_ready = true;
        for (const auto& s : subs) all_ready = all_ready && ready(s.fut);
        if (all_ready && fake_nvme.idle()) break;
        (void)front_sched.advance();
        (void)wal_space.advance();
        (void)fake_nvme.advance_one();
        std::this_thread::yield();
    }

    // Leader[0] succeeds; the 3 group participants all fail before memtable.
    auto r0 = subs[0].fut.get();
    CHECK(std::holds_alternative<bool>(r0));
    CHECK(std::get<bool>(r0));
    for (uint32_t i = 1; i < N; ++i) {
        auto r = subs[i].fut.get();
        CHECK(std::holds_alternative<std::exception_ptr>(r));
        try {
            std::rethrow_exception(std::get<std::exception_ptr>(r));
        } catch (const wal::wal_append_error& e) {
            CHECK(e.reason() == wal::wal_append_error_reason::device_failure);
        }
    }

    // No memtable insert happened in the WAL phase for any group participant.
    for (uint32_t i = 0; i < N; ++i) {
        const auto lookup = front_sched.lookup_memtable_for_testing(
            std::string("fail") + static_cast<char>('0' + i), 300 + i,
            core::front_read_set{.active = front_sched.active_for_testing()});
        CHECK(std::holds_alternative<core::memtable_miss>(lookup));
    }

    // Committed offset did not advance past leader[0]; a retry of the failed
    // batches succeeds from the same offset.
    const uint32_t off_before = front_sched.wal_write_offset_for_testing();
    std::vector<pipeline_submission> retry;
    retry.reserve(N - 1);
    for (uint32_t i = 1; i < N; ++i) {
        retry.push_back(submit_write_wal_fragment(
            front_sched, wal_space, fake_nvme, only_fragment(ctxs[i]),
            canonical_span(ctxs[i])));
    }
    for (uint32_t i = 0; i < 4096; ++i) {
        bool all_ready = true;
        for (const auto& s : retry) all_ready = all_ready && ready(s.fut);
        if (all_ready && fake_nvme.idle()) break;
        (void)front_sched.advance();
        (void)wal_space.advance();
        (void)fake_nvme.advance_one();
        std::this_thread::yield();
    }
    for (auto& s : retry) {
        CHECK(ready(s.fut));
        auto r = s.fut.get();
        CHECK(std::holds_alternative<bool>(r));
        CHECK(std::get<bool>(r));
    }
    CHECK(front_sched.wal_write_offset_for_testing() > off_before);
}

void
m14_wal_group_commit_rotation_boundary_stops_group() {
    // Valid segment (must fit the max WAL entry), filled until only a small tail
    // remains. A short DELETE fits that tail; a long DELETE needs rotation.
    auto geom = make_geom(3, 21000, 4096, 512);
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    install_new_segment(front_sched, wal_space, 0);

    auto ctx_seed = make_ctx(
        {{.op = core::write_op_type::del, .key = "seed", .value = ""}}, 400);
    commit_header(front_sched, only_fragment(ctx_seed),
                  canonical_span(ctx_seed));

    // Fill the segment until only a small amount of tail space remains.
    const uint32_t usable = wal::segment_usable_end_offset(geom);
    uint64_t fill_lsn = 410;
    std::deque<core::batch_ctx> fillers;
    while (true) {
        const uint32_t before = front_sched.wal_write_offset_for_testing();
        if (usable - before < 220) break;
        fillers.push_back(make_ctx(
            {{.op = core::write_op_type::del,
              .key = std::string(64, 'x'),
              .value = ""}},
            fill_lsn++));
        auto plan = take_issue_plan(front_sched.prepare_wal_fragment_for_testing(
            only_fragment(fillers.back()), canonical_span(fillers.back()), {}));
        CHECK(plan.kind == wal::wal_plan_kind::entries);
        CHECK(!front_sched.commit_wal_plan_for_testing(plan.plan_id)
                   .has_value());
    }
    const uint32_t remaining =
        usable - front_sched.wal_write_offset_for_testing();
    CHECK(remaining < 220);

    // Hold a gate plan so seed (short) and big (long) queue behind it. A 1-char
    // DELETE comfortably fits; a 240-char DELETE (~270B) exceeds the < 220B
    // tail even before A consumes any of it.
    auto ctx_gate = make_ctx(
        {{.op = core::write_op_type::del, .key = "g", .value = ""}}, 401);
    auto gate = take_issue_plan(front_sched.prepare_wal_fragment_for_testing(
        only_fragment(ctx_gate), canonical_span(ctx_gate), {}));
    CHECK(front_sched.wal_has_pending_plan_for_testing());

    auto ctx_a = make_ctx(
        {{.op = core::write_op_type::del, .key = "a", .value = ""}}, 402);
    auto ctx_b = make_ctx(
        {{.op = core::write_op_type::del,
          .key = std::string(240, 'b'),
          .value = ""}},
        403);

    auto a = submit_prepare(front_sched, only_fragment(ctx_a),
                            canonical_span(ctx_a), {});
    (void)front_sched.advance();
    auto b = submit_prepare(front_sched, only_fragment(ctx_b),
                            canonical_span(ctx_b), {});
    (void)front_sched.advance();
    CHECK(!ready(a.fut));
    CHECK(!ready(b.fut));

    // Commit gate -> drain -> A merges (fits), B hits the rotation boundary and
    // stays in the FIFO.
    auto gate_commit =
        submit_commit(front_sched, gate.plan_id, std::move(gate.writes));
    (void)front_sched.advance();
    CHECK(ready(gate_commit.fut));

    CHECK(ready(a.fut));
    CHECK(!ready(b.fut));  // B left in FIFO
    auto a_plan =
        take_issue_plan(std::move(std::get<wal::wal_prepare_result>(a.fut.get())));
    CHECK(a_plan.kind == wal::wal_plan_kind::entries);
    CHECK(a_plan.participants.size() == 1);  // group contains only A

    // Commit A -> drain -> B becomes the seed and triggers a trailer (rotation).
    auto a_commit =
        submit_commit(front_sched, a_plan.plan_id, std::move(a_plan.writes));
    (void)front_sched.advance();
    CHECK(ready(a_commit.fut));
    CHECK(ready(b.fut));
    auto b_res = b.fut.get();
    CHECK(is_issue_plan(b_res));
    auto b_plan =
        take_issue_plan(std::move(std::get<wal::wal_prepare_result>(b_res)));
    CHECK(b_plan.kind == wal::wal_plan_kind::trailer);

    front_sched.abort_wal_plan_for_testing(b_plan.plan_id);
}

void
m14_wal_group_commit_bad_follower_does_not_block_fifo() {
    auto geom = make_geom();
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    install_new_segment(front_sched, wal_space, 0);

    auto ctx_gate = make_ctx(
        {{.op = core::write_op_type::del, .key = "gate", .value = ""}}, 500);
    commit_header(front_sched, only_fragment(ctx_gate),
                  canonical_span(ctx_gate));
    auto gate = take_issue_plan(front_sched.prepare_wal_fragment_for_testing(
        only_fragment(ctx_gate), canonical_span(ctx_gate), {}));
    CHECK(front_sched.wal_has_pending_plan_for_testing());

    auto ctx_seed = make_ctx(
        {{.op = core::write_op_type::del, .key = "good-seed", .value = ""}},
        501);
    auto ctx_next = make_ctx(
        {{.op = core::write_op_type::del, .key = "good-next", .value = ""}},
        502);
    // A malformed follower: entry_indices references an out-of-range entry.
    auto ctx_bad = make_ctx(
        {{.op = core::write_op_type::del, .key = "bad", .value = ""}}, 503);
    core::front_fragment bad_fragment = ctx_bad.fragments[0];
    bad_fragment.entry_indices.assign(1, 999u);

    auto seed = submit_prepare(front_sched, only_fragment(ctx_seed),
                               canonical_span(ctx_seed), {});
    (void)front_sched.advance();
    auto bad = submit_prepare(front_sched, bad_fragment,
                              canonical_span(ctx_bad), {});
    (void)front_sched.advance();
    auto next = submit_prepare(front_sched, only_fragment(ctx_next),
                               canonical_span(ctx_next), {});
    (void)front_sched.advance();
    CHECK(!ready(seed.fut));
    CHECK(!ready(bad.fut));
    CHECK(!ready(next.fut));

    // Commit gate -> drain: seed leads, bad fails its own callback, next still
    // merges as a follower.
    auto gate_commit =
        submit_commit(front_sched, gate.plan_id, std::move(gate.writes));
    (void)front_sched.advance();
    CHECK(ready(gate_commit.fut));

    CHECK(ready(seed.fut));
    auto seed_res = seed.fut.get();
    CHECK(is_issue_plan(seed_res));
    auto seed_plan =
        take_issue_plan(std::move(std::get<wal::wal_prepare_result>(seed_res)));
    CHECK(seed_plan.participants.size() == 2);  // seed + next, bad excluded

    CHECK(ready(bad.fut));
    auto bad_res = bad.fut.get();
    CHECK(std::holds_alternative<std::exception_ptr>(bad_res));
    try {
        std::rethrow_exception(std::get<std::exception_ptr>(bad_res));
    } catch (const wal::wal_append_error& e) {
        CHECK(e.reason() ==
              wal::wal_append_error_reason::fragment_entry_index_out_of_range);
    }

    // next is a follower: parked until the leader commits.
    CHECK(!ready(next.fut));
    auto seed_commit = submit_commit(front_sched, seed_plan.plan_id,
                                     std::move(seed_plan.writes));
    (void)front_sched.advance();
    CHECK(ready(seed_commit.fut));
    CHECK(ready(next.fut));
    CHECK(is_committed(next.fut.get()));
}

void
m14_wal_group_commit_pool_return_on_front_commit() {
    auto geom = make_geom();
    wal::wal_space_sched wal_space(geom, 1);
    front::front_sched front_sched(0, 1, geom);
    test_fake_nvme::scheduler fake_nvme;
    install_new_segment(front_sched, wal_space, 0);

    // Warm-up batch drives the header + one entry to completion so all frames
    // are returned to the pool before measuring the baseline.
    auto warm = make_ctx(
        {{.op = core::write_op_type::del, .key = "warm", .value = ""}}, 600);
    {
        auto sub = submit_write_wal_fragment(front_sched, wal_space, fake_nvme,
                                             only_fragment(warm),
                                             canonical_span(warm));
        for (uint32_t i = 0; i < 4096; ++i) {
            if (ready(sub.fut) && fake_nvme.idle()) break;
            (void)front_sched.advance();
            (void)wal_space.advance();
            (void)fake_nvme.advance_one();
            std::this_thread::yield();
        }
        CHECK(ready(sub.fut));
    }
    const auto initial_free =
        front_sched.wal_frame_pool_free_pages_for_testing();
    CHECK(initial_free > 0);
    CHECK(!front_sched.wal_has_pending_plan_for_testing());

    // Build a coalesced group: leader[0] solo, then the rest merge.
    constexpr uint32_t N = 4;
    std::vector<core::batch_ctx> ctxs;
    ctxs.reserve(N);
    for (uint32_t i = 0; i < N; ++i) {
        ctxs.push_back(make_ctx(
            {{.op = core::write_op_type::del,
              .key = std::string("p") + static_cast<char>('0' + i),
              .value = ""}},
            610 + i));
    }
    std::vector<pipeline_submission> subs;
    subs.reserve(N);
    for (uint32_t i = 0; i < N; ++i) {
        subs.push_back(submit_write_wal_fragment(
            front_sched, wal_space, fake_nvme, only_fragment(ctxs[i]),
            canonical_span(ctxs[i])));
    }
    (void)front_sched.advance();        // leader[0] issues call #1
    CHECK(fake_nvme.advance_one());     // complete call #1
    (void)front_sched.advance();        // commit leader[0], drain -> group plan
    CHECK(fake_nvme.active == 1);       // group FUA now in flight

    // Complete the group FUA but do NOT advance the front yet: the group's
    // frames are still held by the in-flight commit req.
    while (!fake_nvme.idle()) {
        (void)fake_nvme.advance_one();
    }
    CHECK(front_sched.wal_has_pending_plan_for_testing());
    CHECK(front_sched.wal_frame_pool_free_pages_for_testing() < initial_free);

    // The front commit returns the group's frames to the pool on the front
    // thread.
    for (uint32_t i = 0; i < 4096; ++i) {
        bool all_ready = true;
        for (const auto& s : subs) all_ready = all_ready && ready(s.fut);
        if (all_ready && !front_sched.wal_has_pending_plan_for_testing()) break;
        (void)front_sched.advance();
        (void)wal_space.advance();
        (void)fake_nvme.advance_one();
        std::this_thread::yield();
    }
    for (auto& s : subs) {
        CHECK(ready(s.fut));
        auto r = s.fut.get();
        CHECK(std::holds_alternative<bool>(r));
        CHECK(std::get<bool>(r));
    }
    CHECK(front_sched.wal_frame_pool_free_pages_for_testing() == initial_free);
}

}  // namespace

int
main() {
    m14_wal_group_commit_single_lba_many_batches();
    m14_wal_group_commit_followers_do_not_issue_nvme();
    m14_wal_group_commit_fua_failure_releases_all_participants();
    m14_wal_group_commit_rotation_boundary_stops_group();
    m14_wal_group_commit_bad_follower_does_not_block_fifo();
    m14_wal_group_commit_pool_return_on_front_commit();
    return 0;
}
