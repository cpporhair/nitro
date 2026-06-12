// M13 / 052 — E2E matrix over the production runtime with the mock NVMe
// backend (INCONEL_NVME_MOCK_BACKEND). Tests 1-11 of the dev-plan matrix:
// full production stack (build_runtime → registry → rt:: operations) with
// device-level byte verification through mock_device readback.
//
// Authored by the review side per 052 §0 role split.

#include "apps/inconel/runtime/operations.hh"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/core/memtable_lookup.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/format/wal.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/test/check.hh"
#include "pump/core/context.hh"
#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

using namespace apps::inconel;

namespace {

constexpr uint32_t kLbaSize = 4096;
// Bootstrap profile's data area ends at LBA 100000; leave headroom.
constexpr uint64_t kNamespaceLbas = 110000;

using tree_cache_t = core::segmented_clock_cache;
using value_cache_t = core::segmented_clock_cache;
using runtime_t =
    runtime::inconel_runtime_t<tree_cache_t, value_cache_t>;

// ── submission plumbing (house pattern) ────────────────────────

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
                promise->set_value(T(std::forward<decltype(value)>(value)));
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
    if (std::holds_alternative<std::exception_ptr>(result)) {
        try {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "operation failed: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "operation failed: non-std exception\n");
        }
        CHECK(false);
    }
    return std::move(std::get<T>(result));
}

// ── e2e fixture: production build_runtime over the mock backend ──

struct e2e_fixture {
    std::vector<uint32_t> cores;
    nvme::runtime_device device{kLbaSize, kNamespaceLbas, 0};
    runtime_t* rt = nullptr;

    explicit e2e_fixture(std::vector<uint32_t> cores_in = {0, 1})
        : cores(std::move(cores_in)) {
        pump::core::this_core_id = 0;
        runtime::build_options bopts{
            .cores = std::span<const uint32_t>(cores.data(), cores.size()),
            .device = &device,
        };
        rt = runtime::build_runtime<tree_cache_t, value_cache_t>(bopts);
        // build_runtime's per-core loop leaves this_core_id at the last
        // core; the single-thread driver acts as a client on core 0.
        pump::core::this_core_id = 0;
    }

    ~e2e_fixture() {
        runtime::destroy_runtime<tree_cache_t, value_cache_t>(rt);
    }

    e2e_fixture(const e2e_fixture&) = delete;
    e2e_fixture& operator=(const e2e_fixture&) = delete;

    // Advance every scheduler the builder registered, across all cores,
    // from this single driver thread (m11/m12 precedent: per_core queue
    // drains are owner-core-agnostic; the local fast path is just unused).
    bool advance_all() {
        bool progress = false;
        for (uint32_t core : cores) {
            std::apply(
                [&](auto*... sched) {
                    auto step = [&](auto* s) {
                        if (s != nullptr) progress |= s->advance();
                    };
                    (step(sched), ...);
                },
                rt->schedulers_by_core[core]);
        }
        return progress;
    }

    template <typename Submission>
    void drive_until_ready(Submission& sub, uint32_t limit = 400000) {
        for (uint32_t i = 0; !ready(sub.fut) && i < limit; ++i) {
            if (!advance_all()) std::this_thread::yield();
        }
        CHECK(ready(sub.fut));
    }

    // ── rt:: operation drivers (default NvmeProvider — zero injection;
    //    value reads resolve rt::local_nvme() = the mock scheduler) ──

    [[nodiscard]] core::client_batch_buffer
    make_input(std::vector<core::raw_batch_op> ops) const {
        return core::encode_client_batch(
            std::span<const core::raw_batch_op>(ops.data(), ops.size()));
    }

    op_result<write_path::write_batch_result>
    run_write(std::vector<core::raw_batch_op> ops) {
        auto sub = submit_result<write_path::write_batch_result>(
            [this, input = make_input(std::move(ops))]() mutable {
                return rt::write_batch(std::move(input));
            });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    op_result<pipeline::point_get_result>
    run_point_get(std::string_view key) {
        auto sub = submit_result<pipeline::point_get_result>(
            [key]() { return rt::point_get(key); });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    op_result<pipeline::seal_round_result>
    run_seal() {
        auto sub = submit_result<pipeline::seal_round_result>(
            []() { return rt::seal_once(); });
        drive_until_ready(sub);
        return sub.fut.get();
    }

    // ── observation helpers ─────────────────────────────────────

    [[nodiscard]] uint64_t visible_lsn() const {
        return core::registry::coord_sched_singleton()
            ->acquire_read_handle_for_testing()
            .read_lsn;
    }

    [[nodiscard]] uint32_t front_count() const {
        return core::registry::front_count();
    }

    [[nodiscard]] core::memtable_lookup_result
    lookup_visible(std::string_view key) {
        const auto rh = core::registry::coord_sched_singleton()
                            ->acquire_read_handle_for_testing();
        const auto owner = static_cast<uint32_t>(
            core::key_hash(key) % front_count());
        return core::registry::front_at(owner)->lookup_memtable_for_testing(
            key, rh.read_lsn, (*rh.cat->prs->fronts)[owner]);
    }

    [[nodiscard]] std::optional<format::value_ref>
    visible_value_ref(std::string_view key) {
        auto result = lookup_visible(key);
        if (auto* hit = std::get_if<core::memtable_value_hit>(&result)) {
            return hit->durable;
        }
        return std::nullopt;
    }

    // Read the durable value object at `vr` straight off the mock device
    // and decode it with the production format helper (magic + len + crc).
    [[nodiscard]] std::string
    device_value_body(const format::value_ref& vr) {
        const uint32_t total =
            static_cast<uint32_t>(sizeof(format::value_object_header)) +
            vr.len;
        const uint32_t span_lbas =
            (vr.byte_offset + total + kLbaSize - 1) / kLbaSize;
        std::vector<char> page(
            static_cast<std::size_t>(span_lbas) * kLbaSize);
        CHECK(device.read_bytes(vr.base.lba,
                                std::span<char>(page.data(), page.size())));
        auto decoded = format::decode_value_object(
            std::span<const char>(page.data() + vr.byte_offset,
                                  page.size() - vr.byte_offset),
            vr.len);
        CHECK(decoded.status == format::value_decode_status::ok);
        return std::string(decoded.body.data(), decoded.body.size());
    }
};

// ── WAL readback (device bytes → decoded entries) ──────────────

struct decoded_wal_entry_copy {
    uint32_t stream_id = 0;
    format::wal_op_type op_type = format::wal_op_type::put;
    uint64_t lsn = 0;
    uint32_t entry_count = 0;
    std::string key;
    std::optional<format::value_ref> vr;
};

std::vector<decoded_wal_entry_copy>
collect_wal_entries(e2e_fixture& fx) {
    const auto& profile = format::kBootstrapFormatProfile;
    const auto geom = runtime::wal_geometry_from_profile(profile);
    const uint64_t segment_lbas = geom.wal_segment_size / geom.lba_size;

    std::vector<decoded_wal_entry_copy> out;
    std::vector<char> seg(geom.wal_segment_size);

    for (uint32_t idx = 0; idx < geom.wal_segment_count; ++idx) {
        const uint64_t base_lba =
            geom.wal_base_paddr.lba + idx * segment_lbas;
        CHECK(fx.device.read_bytes(
            base_lba, std::span<char>(seg.data(), seg.size())));

        format::wal_segment_header header{};
        std::memcpy(&header, seg.data(), sizeof(header));
        if (format::inspect_wal_segment_header(
                header, geom.expected_format_version) !=
            format::wal_segment_status::ok) {
            continue;
        }

        uint32_t offset = format::WAL_SEGMENT_HEADER_SIZE;
        const uint32_t limit = wal::segment_usable_end_offset(geom);
        while (offset < limit) {
            uint32_t len = 0;
            format::decoded_wal_entry decoded;
            const auto status = format::decode_wal_entry(
                std::span<const char>(seg.data() + offset,
                                      seg.size() - offset),
                header.segment_gen,
                &decoded,
                &len);
            if (status != format::wal_entry_decode_status::ok) break;
            CHECK(len > 0);
            out.push_back(decoded_wal_entry_copy{
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

bool
same_value_ref(const format::value_ref& a, const format::value_ref& b) {
    return a.base.device_id == b.base.device_id &&
           a.base.lba == b.base.lba &&
           a.byte_offset == b.byte_offset &&
           a.len == b.len &&
           a.flags == b.flags;
}

std::string
key_for_owner(uint32_t owner, uint32_t front_count, std::string_view prefix) {
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

void expect_found(const pipeline::point_get_result& r,
                  std::string_view body) {
    CHECK(r.found);
    CHECK(r.value == body);
}

void expect_not_found(const pipeline::point_get_result& r) {
    CHECK(!r.found);
    CHECK(r.value.empty());
}

// ── matrix 1: single PUT value lands on the device ─────────────

void m13_single_put_value_on_device() {
    e2e_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count(), "m13-one");
    const std::string body(100, 'x');

    auto wr = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = body},
    }));
    CHECK(wr.batch_lsn == 1);
    CHECK(fx.visible_lsn() == 1);

    auto vr = fx.visible_value_ref(key);
    CHECK(vr.has_value());
    CHECK(vr->len == body.size());
    CHECK(fx.device_value_body(*vr) == body);
}

// ── matrix 2: multi-key fan-out reaches both fronts' WAL streams ─

void m13_multi_key_fanout_two_fronts() {
    e2e_fixture fx;
    const std::string k0 = key_for_owner(0, fx.front_count(), "m13-f0");
    const std::string k1 = key_for_owner(1, fx.front_count(), "m13-f1");

    auto wr = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = k0, .value = "body-f0"},
        {.op = core::write_op_type::put, .key = k1, .value = "body-f1"},
    }));
    CHECK(wr.entry_count == 2);

    auto entries = collect_wal_entries(fx);
    bool saw0 = false;
    bool saw1 = false;
    for (const auto& e : entries) {
        if (e.key == k0) {
            saw0 = true;
            CHECK(e.stream_id ==
                  core::key_hash(k0) % fx.front_count());
            CHECK(e.entry_count == 2);
        }
        if (e.key == k1) {
            saw1 = true;
            CHECK(e.stream_id ==
                  core::key_hash(k1) % fx.front_count());
            CHECK(e.entry_count == 2);
        }
    }
    CHECK(saw0);
    CHECK(saw1);

    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_point_get(k0)),
                 "body-f0");
    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_point_get(k1)),
                 "body-f1");
}

// ── matrix 3: same-key canonicalization (last-op-wins, one record) ─

void m13_same_key_canonicalization() {
    e2e_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count(), "m13-canon");

    auto wr = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "v1"},
        {.op = core::write_op_type::put, .key = key, .value = "v2"},
    }));
    CHECK(wr.entry_count == 1);

    auto entries = collect_wal_entries(fx);
    uint32_t hits = 0;
    for (const auto& e : entries) {
        if (e.key == key) {
            ++hits;
            CHECK(e.entry_count == 1);
            CHECK(e.op_type == format::wal_op_type::put);
        }
    }
    CHECK(hits == 1);

    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_point_get(key)),
                 "v2");
}

// ── matrix 4: PUT then DELETE — tombstone masks, old bytes remain ─

void m13_put_then_delete() {
    e2e_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count(), "m13-pd");
    const std::string body = "to-be-deleted";

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = body},
    }));
    auto vr = fx.visible_value_ref(key);
    CHECK(vr.has_value());

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::del, .key = key, .value = ""},
    }));

    expect_not_found(expect_ok<pipeline::point_get_result>(
        fx.run_point_get(key)));

    // The durable value object is not reclaimed by the write path; the
    // bytes must still decode (reclaim is a flush/guard concern).
    CHECK(fx.device_value_body(*vr) == body);

    auto entries = collect_wal_entries(fx);
    bool saw_put = false;
    bool saw_del = false;
    for (const auto& e : entries) {
        if (e.key != key) continue;
        if (e.op_type == format::wal_op_type::put) saw_put = true;
        if (e.op_type == format::wal_op_type::del) saw_del = true;
    }
    CHECK(saw_put);
    CHECK(saw_del);
}

// ── matrix 5: DELETE-only batch performs no value-area write ────

void m13_delete_only_no_value_write() {
    e2e_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count(), "m13-del");

    // Snapshot the top of the value area (the value allocator carves
    // downward from data_area_end; a fresh runtime's first value write
    // would land in this window).
    const auto& profile = format::kBootstrapFormatProfile;
    constexpr uint32_t kProbeLbas = 256;
    const uint64_t probe_base =
        profile.value_data_area_end.lba - kProbeLbas;
    std::vector<char> before(
        static_cast<std::size_t>(kProbeLbas) * kLbaSize);
    CHECK(fx.device.read_bytes(
        probe_base, std::span<char>(before.data(), before.size())));

    auto wr = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::del, .key = key, .value = ""},
    }));
    CHECK(wr.batch_lsn == 1);

    std::vector<char> after(before.size());
    CHECK(fx.device.read_bytes(
        probe_base, std::span<char>(after.data(), after.size())));
    CHECK(before == after);

    // And the tombstone is visible (no value_ref behind it).
    CHECK(!fx.visible_value_ref(key).has_value());
    CHECK(std::holds_alternative<core::memtable_tombstone>(
        fx.lookup_visible(key)));
}

// ── matrix 6: WAL entry value_ref matches the memtable's ────────

void m13_wal_decode_matches_memtable_vr() {
    e2e_fixture fx;
    const std::string key = key_for_owner(1, fx.front_count(), "m13-vr");
    const std::string body = "vr-triangle";

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = body},
    }));

    auto vr = fx.visible_value_ref(key);
    CHECK(vr.has_value());

    auto entries = collect_wal_entries(fx);
    bool saw = false;
    for (const auto& e : entries) {
        if (e.key != key) continue;
        saw = true;
        CHECK(e.vr.has_value());
        CHECK(same_value_ref(*e.vr, *vr));
    }
    CHECK(saw);
    CHECK(fx.device_value_body(*vr) == body);
}

// ── matrix 7: sequential LSN advance over consecutive batches ───

void m13_sequential_lsn_advance() {
    e2e_fixture fx;
    for (uint64_t i = 1; i <= 3; ++i) {
        const std::string key =
            key_for_owner(static_cast<uint32_t>(i) % fx.front_count(),
                          fx.front_count(),
                          "m13-seq" + std::to_string(i));
        auto wr = expect_ok<write_path::write_batch_result>(fx.run_write({
            {.op = core::write_op_type::put,
             .key = key,
             .value = "v" + std::to_string(i)},
        }));
        CHECK(wr.batch_lsn == i);
        CHECK(fx.visible_lsn() == i);
    }
}

// ── matrix 8: memtable visible after write (entry-level) ────────

void m13_memtable_visible_after_write() {
    e2e_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count(), "m13-vis");

    auto wr = expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "visible"},
    }));

    auto result = fx.lookup_visible(key);
    auto* hit = std::get_if<core::memtable_value_hit>(&result);
    CHECK(hit != nullptr);
    CHECK(hit->durable.len == 7);
    (void)wr;
}

// ── matrix 9: point_get after write (rt:: full chain) ───────────

void m13_point_get_after_write() {
    e2e_fixture fx;
    const std::string key = key_for_owner(1, fx.front_count(), "m13-get");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "get-body"},
    }));
    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_point_get(key)),
                 "get-body");

    const std::string missing =
        key_for_owner(0, fx.front_count(), "m13-missing");
    expect_not_found(expect_ok<pipeline::point_get_result>(
        fx.run_point_get(missing)));
}

// ── matrix 10: overwrite reads latest ───────────────────────────

void m13_overwrite_read_latest() {
    e2e_fixture fx;
    const std::string key = key_for_owner(0, fx.front_count(), "m13-ow");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "old"},
    }));
    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "new"},
    }));

    expect_found(expect_ok<pipeline::point_get_result>(
                     fx.run_point_get(key)),
                 "new");
}

// ── matrix 11: delete reads not_found ───────────────────────────

void m13_delete_read_not_found() {
    e2e_fixture fx;
    const std::string key = key_for_owner(1, fx.front_count(), "m13-dnf");

    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::put, .key = key, .value = "soon-gone"},
    }));
    (void)expect_ok<write_path::write_batch_result>(fx.run_write({
        {.op = core::write_op_type::del, .key = key, .value = ""},
    }));

    expect_not_found(expect_ok<pipeline::point_get_result>(
        fx.run_point_get(key)));
}

}  // namespace

int main() {
    m13_single_put_value_on_device();
    m13_multi_key_fanout_two_fronts();
    m13_same_key_canonicalization();
    m13_put_then_delete();
    m13_delete_only_no_value_write();
    m13_wal_decode_matches_memtable_vr();
    m13_sequential_lsn_advance();
    m13_memtable_visible_after_write();
    m13_point_get_after_write();
    m13_overwrite_read_latest();
    m13_delete_read_not_found();
    return 0;
}
