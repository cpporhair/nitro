//
// Tree-local flush end-to-end harness (step 033).
//
// Runs one full flush transaction from a freshly formatted mock disk:
//
//   1. format::make_formatted_storage -> byte buffer mimicking a new disk.
//   2. runtime::build_runtime with the asymmetric topology (value on core 0,
//      three tree_read_domains on cores 2/4/6, owner on core 8).
//   3. Install a custom 3-shard shard_partition_map so the N keys fan out
//      across three workers -- the bootstrap placeholder is single-shard.
//   4. Persist N real values through value::persist_values to obtain N
//      durable value_refs.
//   5. Build a sealed memtable_gen with N PUT entries that point at those
//      value_refs.
//   6. Submit tree::tree_local_flush(req) once; block on the result.
//   7. Readback a handful of keys via tree::lookup + value::read_value.
//
// Parameterisation: --num-keys N (default 1000). Every derived quantity
// (key digit width, fence boundaries, readback samples) scales with N so
// the same harness runs at 1e3 or 1e5 without structural change.
//
// No production code is modified. If the harness aborts
// (flush st != ok / null manifest / readback mismatch), that surfaces a
// production bug (Phase 9 empty-tree bootstrap is the first suspect) and
// is fixed in a follow-up session.
//

#include "apps/inconel/test/check.hh"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "env/runtime/share_nothing.hh"
#include "pump/core/context.hh"
#include "pump/core/lock_free_queue.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include "apps/inconel/core/checkpoint_guard.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/page_cache.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/shard_partition.hh"
#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/format/format_options.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/formatted_storage.hh"
#include "apps/inconel/format/types.hh"
#include "apps/inconel/mock_nvme/device.hh"
#include "apps/inconel/mock_nvme/scheduler.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/runtime/facade.hh"
#include "apps/inconel/runtime/run.hh"
#include "apps/inconel/tree/flush_types.hh"
#include "apps/inconel/tree/lookup.hh"
#include "apps/inconel/tree/sender.hh"
#include "apps/inconel/value/scheduler.hh"
#include "apps/inconel/value/sender.hh"

using namespace apps::inconel;

using TreeCache  = core::clock_cache;
using ValueCache = core::clock_cache;

namespace {

// ── Topology (hardcoded; step 031 §12.1 target wiring) ──────────────────

const std::vector<uint32_t> kCores           = {0, 2, 4, 6, 8};
const std::vector<uint32_t> kReadDomainCores = {2, 4, 6};
constexpr int32_t           kValueCore       = 0;
constexpr int32_t           kOwnerCore       = 8;

// ── Disk size ───────────────────────────────────────────────────────────
//
// kBootstrapFormatProfile pins value_data_area_end.lba = 8000, so the
// device namespace must cover at least 8000 * lba_size bytes for
// build_runtime's tier-3 validate_build_inputs gate. 8000 * 4096 = 32 MiB.

constexpr uint64_t kNamespaceBytes =
    uint64_t{8000} * format::kBootstrapFormatProfile.lba_size;

// ── CLI ─────────────────────────────────────────────────────────────────

struct harness_options {
    uint32_t num_keys = 1000;
};

void
print_usage(const char* argv0) {
    std::printf("usage: %s [--num-keys N]   (default 1000, range 3..100000000)\n",
                argv0);
}

harness_options
parse_argv(int argc, char** argv) {
    harness_options o;
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a == "--num-keys") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--num-keys requires a value\n");
                print_usage(argv[0]);
                std::exit(2);
            }
            char* endp = nullptr;
            long v = std::strtol(argv[++i], &endp, 10);
            if (!endp || *endp != '\0' || v < 3 || v > 100'000'000L) {
                std::fprintf(stderr, "--num-keys must be an integer in [3, 100000000]\n");
                std::exit(2);
            }
            o.num_keys = static_cast<uint32_t>(v);
        } else if (a == "--help" || a == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown arg: %.*s\n",
                         static_cast<int>(a.size()), a.data());
            print_usage(argv[0]);
            std::exit(2);
        }
    }
    return o;
}

// ── Record generation ───────────────────────────────────────────────────
//
// key_digits is chosen so the ascii comparison of "key_%0Nd"-formatted
// keys agrees with the integer ordering for the whole run. Below 1000
// keys we keep the width at 3 for readability of logs.

struct key_spec {
    uint32_t num_keys;
    uint32_t key_digits;
};

key_spec
make_key_spec(uint32_t num_keys) {
    uint32_t d = 1;
    uint64_t cap = 10;
    while (cap < num_keys) { cap *= 10; ++d; }
    if (d < 3) d = 3;
    return { .num_keys = num_keys, .key_digits = d };
}

struct kv_record {
    std::string key;
    std::string value;
};

std::vector<kv_record>
generate_kv_records(const key_spec& spec) {
    std::vector<kv_record> out;
    out.reserve(spec.num_keys);
    char buf[64];
    for (uint32_t i = 0; i < spec.num_keys; ++i) {
        std::snprintf(buf, sizeof(buf), "key_%0*u", spec.key_digits, i);
        std::string key = buf;
        std::snprintf(buf, sizeof(buf), "val_%0*u", spec.key_digits, i);
        std::string value = buf;
        out.push_back({std::move(key), std::move(value)});
    }
    return out;
}

// ── Shard map ───────────────────────────────────────────────────────────
//
// Three shards, cut at the 1/3 and 2/3 rank of the sorted record vector.
// Last shard carries the +∞ sentinel (fence_upper_len == 0).
//
// The bootstrap map installed by runtime::build_runtime is single-shard
// ((-∞, +∞) → 0) because leaf_order is empty at boot. Harness replaces
// it via rt::publish_shard_partitions so the flush fold actually fans
// out to three read_domains.

std::shared_ptr<const core::shard_partition_map>
build_harness_shard_map(const std::vector<kv_record>& sorted_records) {
    CHECK(sorted_records.size() >= 3);
    const std::size_t n = sorted_records.size();
    const std::string_view f0 = sorted_records[n / 3].key;
    const std::string_view f1 = sorted_records[(2 * n) / 3].key;
    CHECK(f0 < f1);

    core::shard_partition_map map;
    map.fence_pool.reserve(f0.size() + f1.size());

    core::shard_partition sp0{
        .fence_upper_off = static_cast<uint32_t>(map.fence_pool.size()),
        .fence_upper_len = static_cast<uint16_t>(f0.size()),
        ._pad0           = 0,
        .shard_idx       = 0,
    };
    map.fence_pool.append(f0);
    map.shards.push_back(sp0);

    core::shard_partition sp1{
        .fence_upper_off = static_cast<uint32_t>(map.fence_pool.size()),
        .fence_upper_len = static_cast<uint16_t>(f1.size()),
        ._pad0           = 0,
        .shard_idx       = 1,
    };
    map.fence_pool.append(f1);
    map.shards.push_back(sp1);

    map.shards.push_back(core::shard_partition{
        .fence_upper_off = 0,
        .fence_upper_len = 0,
        ._pad0           = 0,
        .shard_idx       = 2,
    });

    return std::make_shared<const core::shard_partition_map>(std::move(map));
}

// ── Format options ──────────────────────────────────────────────────────

format::format_options
derive_format_options() {
    const auto& p = format::kBootstrapFormatProfile;
    format::format_options opts{};
    opts.lba_size               = p.lba_size;
    opts.tree_page_size         = p.tree_page_size;
    opts.shadow_slots_per_range = p.shadow_slots_per_range;
    opts.value_class_count      = p.value_class_count;
    for (uint8_t i = 0; i < p.value_class_count; ++i)
        opts.value_class_sizes[i] = p.value_class_sizes[i];
    opts.wal_segment_size  = 1u << 20;   // 1 MiB = 256 LBAs at 4 KiB
    opts.wal_segment_count = 8;
    return opts;
}

// ── Submit helper: block on a single-value pipeline ─────────────────────
//
// Used across the three pipeline phases (persist / flush / lookup /
// read_value). Each phase ends with a single value on its `then` tail,
// so we thread it through a std::promise<T> and block the main thread
// on the future. Pumping is done by the per-core advance threads started
// earlier, so the main-thread block here does not stall scheduler
// progress.
//
// The `SenderBuilder` callable must return a COMPLETE sender chain (it
// is appended with `>> then(...) >> submit(ctx)` only). Most tree/value
// entry points (`value::persist_values`, `value::read_value`,
// `tree::tree_local_flush`) already start with a scheduler sender and
// are complete chains on their own. Entry points that are bind_back --
// `tree::lookup`, which wraps `with_context(...)(...)` -- must prefix
// themselves with `pump::sender::just() >>` inside the builder lambda.
//
// Context is created per-submit so it is reaped once the pipeline
// releases its reference via the captured promise shared_ptr.

template <typename T, typename SenderBuilder>
T
submit_and_wait(SenderBuilder&& build_sender) {
    auto ctx = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<T>>();
    auto fut = promise->get_future();

    // `auto&&` so the promise lambda binds whether the pipeline forwards
    // the tail value as an rvalue or an lvalue -- `tree_local_flush`'s
    // inner flat chain goes through a few hops that store the result
    // before the tail push, so it can arrive as an lvalue.
    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::then([promise](auto&& r) {
            promise->set_value(std::forward<decltype(r)>(r));
        })
        >> pump::sender::submit(ctx);

    return fut.get();
}

// ── Value persist ──────────────────────────────────────────────────────
//
// Drives one value::persist_values pipeline over the entire record set.
// The scheduler's leader/follower logic pipelines the writes internally;
// the harness only needs one top-level submit per batch. `entries` backs
// both the `body` string_views (borrowed) and the `out_vr` slots the
// scheduler writes into -- both must outlive the submit.

std::vector<format::value_ref>
persist_all_values(const std::vector<kv_record>& records) {
    const std::size_t n = records.size();
    std::vector<format::value_ref> durables(n);
    std::vector<value::put_entry> entries;
    entries.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        entries.push_back(value::put_entry{
            .body   = records[i].value,
            .out_vr = &durables[i],
        });
    }

    const bool ok = submit_and_wait<bool>([&]() {
        return value::persist_values(std::span<value::put_entry>(entries));
    });
    CHECK(ok && "value::persist_values returned ok=false");
    return durables;
}

// ── Memtable gen ───────────────────────────────────────────────────────
//
// Single-writer construction: the gen is not yet shared with any reader
// while this function runs, so direct table population is safe.
//
// kv_arena owns every key byte AND every value byte referenced by the
// resulting memtable_entry view fields -- lifetime is tied to the gen's
// shared_ptr. Copying out of the caller's records on construction lets
// the caller drop its std::string vector immediately after this call.

std::shared_ptr<core::memtable_gen>
build_sealed_gen(uint64_t                              gen_id,
                 uint32_t                              front_owner_index,
                 uint64_t                              lsn_start,
                 std::span<const kv_record>            records,
                 std::span<const format::value_ref>    durables) {
    CHECK(records.size() == durables.size());
    CHECK(!records.empty());

    auto gen = std::make_shared<core::memtable_gen>();
    gen->gen_id            = gen_id;
    gen->front_owner_index = front_owner_index;
    gen->st                = core::memtable_gen::state::sealed;
    gen->min_lsn           = lsn_start;
    gen->max_lsn           = lsn_start + records.size() - 1;

    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];
        auto key_view = gen->kv_arena.allocate(rec.key.data(), rec.key.size());
        auto val_view = gen->kv_arena.allocate(rec.value.data(), rec.value.size());

        core::memtable_entry entry{
            .data_ver = lsn_start + static_cast<uint64_t>(i),
            .k        = core::memtable_entry::kind::value,
            .vh       = core::value_handle{
                .durable = durables[i],
                .hot     = core::value_view{
                    .data = val_view.data(),
                    .len  = static_cast<uint32_t>(val_view.size()),
                },
            },
        };
        auto [it, inserted] = gen->table.try_emplace(key_view);
        CHECK(inserted);
        it->second.push_back(entry);
    }
    return gen;
}

// ── base_guard ─────────────────────────────────────────────────────────
//
// Initial checkpoint_guard pins an empty tree_manifest so the flush
// request's base_guard->manifest is non-null (tree_flush_request
// contract). The geometry pointer MUST be the same instance that
// tree_sched and every tree_read_domain were built with (builder.hh
// passes &runtime::kBootstrapTreeGeometry into both), so we use the
// same address here.

std::shared_ptr<const core::checkpoint_guard>
make_empty_base_guard() {
    auto manifest = std::make_shared<const core::tree_manifest>(
        core::tree_manifest::empty(&runtime::kBootstrapTreeGeometry));
    core::checkpoint_guard guard{};
    guard.manifest = std::move(manifest);
    return std::make_shared<const core::checkpoint_guard>(std::move(guard));
}

// ── Single flush round ─────────────────────────────────────────────────

struct flush_round_outcome {
    tree::tree_flush_result                    result;
    std::shared_ptr<const core::tree_manifest> manifest_for_readback;
};

flush_round_outcome
run_one_flush_round(std::shared_ptr<core::memtable_gen>           gen,
                    std::shared_ptr<const core::checkpoint_guard> base_guard,
                    uint64_t                                      recovery_safe_lsn) {
    tree::tree_flush_request req{
        .base_guard        = std::move(base_guard),
        .sealed_gens       = {},
        .recovery_safe_lsn = recovery_safe_lsn,
    };
    req.sealed_gens.push_back(std::move(gen));

    // `req` is consumed by the pipeline; we capture it into a wrapper so
    // the submit_and_wait template's SenderBuilder takes no args. Using a
    // shared_ptr keeps the wrapper copyable (the lambda may be moved into
    // the sender expression), without forcing us to fight move-only
    // semantics through the template.
    auto req_holder = std::make_shared<tree::tree_flush_request>(std::move(req));

    auto result = submit_and_wait<tree::tree_flush_result>([req_holder]() {
        return tree::tree_local_flush(std::move(*req_holder));
    });
    auto manifest_pin = result.new_manifest;
    return flush_round_outcome{
        .result                = std::move(result),
        .manifest_for_readback = std::move(manifest_pin),
    };
}

// ── Validation ─────────────────────────────────────────────────────────
//
// Single entry that prints a structured failure context for any assertion
// that breaks -- the output is consumed by the follow-up session fixing
// the production bug, so include enough state to decide which §7 of the
// plan broke without re-running the harness.

void
validate_flush_result(const tree::tree_flush_result& r,
                      uint64_t                       expected_max_lsn,
                      uint32_t                       expected_front_owner_index,
                      uint64_t                       expected_gen_id) {
    if (r.st != tree::flush_stage_status::ok) {
        std::fprintf(stderr,
            "FAIL: flush_stage_status != ok (= %u)\n",
            static_cast<unsigned>(r.st));
        std::abort();
    }
    if (!r.new_manifest) {
        std::fprintf(stderr, "FAIL: new_manifest is null\n");
        std::abort();
    }
    const auto& m = *r.new_manifest;
    if (!m.has_root()) {
        std::fprintf(stderr,
            "FAIL: new_manifest has no root "
            "(root_slot.lba=%lu, root_range_base.lba=%lu, leaf_order.size=%zu, "
            "slot_map.size=%zu)\n",
            static_cast<unsigned long>(m.root_slot.lba),
            static_cast<unsigned long>(m.root_range_base.lba),
            m.leaf_order.size(),
            m.slot_map.size());
        std::abort();
    }
    if (m.leaf_order.size() == 0) {
        std::fprintf(stderr,
            "FAIL: new_manifest.leaf_order is empty "
            "(root_slot.lba=%lu, slot_map.size=%zu)\n",
            static_cast<unsigned long>(m.root_slot.lba),
            m.slot_map.size());
        std::abort();
    }
    if (r.flushed_max_lsn != expected_max_lsn) {
        std::fprintf(stderr,
            "FAIL: flushed_max_lsn %lu != expected %lu\n",
            static_cast<unsigned long>(r.flushed_max_lsn),
            static_cast<unsigned long>(expected_max_lsn));
        std::abort();
    }
    auto it = r.flushed_gens_by_front.find(expected_front_owner_index);
    if (it == r.flushed_gens_by_front.end()) {
        std::fprintf(stderr,
            "FAIL: flushed_gens_by_front missing front_owner_index=%u "
            "(size=%zu)\n",
            expected_front_owner_index,
            r.flushed_gens_by_front.size());
        std::abort();
    }
    bool found_gen = false;
    for (const auto& g : it->second) {
        if (g && g->gen_id == expected_gen_id) { found_gen = true; break; }
    }
    if (!found_gen) {
        std::fprintf(stderr,
            "FAIL: flushed_gens_by_front[%u] missing gen_id=%lu "
            "(group_size=%zu)\n",
            expected_front_owner_index,
            static_cast<unsigned long>(expected_gen_id),
            it->second.size());
        std::abort();
    }
}

// ── Readback sampling ──────────────────────────────────────────────────
//
// Evenly strided samples across the sorted record vector. Stride is
// chosen so the resulting sample count lands near `kTargetReadbackSamples`
// regardless of `num_keys` (`1000 → ~50`, `100_000 → ~50` still -- the
// cap keeps the single-batch `tree::lookup` call cheap on large runs).
// Index 0 and N-1 are always included so the extremes and the second-
// shard / third-shard boundaries are both covered, and the three shard
// boundaries (N/3, 2N/3) get a nearby sample each.
//
// Rationale for striding over "hardcode 5 boundary points": the earlier
// 5-sample set exercised each shard exactly once, which only proves the
// routing decision works for the boundary. Striding across every leaf
// page in the post-flush manifest (`leaf_order.size` is 12 for
// num_keys=1000 at the current geometry, so ~50 samples ≈ 4 per leaf)
// actually exercises the `tree_read_domain.node_cache` miss/fill path
// on every leaf, the per-shard single-flight path, and values from
// every sub-LBA class that landed in the first flush.

constexpr std::size_t kTargetReadbackSamples = 50;

struct readback_sample {
    std::string key;
    std::string expected_value;
};

std::vector<readback_sample>
pick_samples(const std::vector<kv_record>& records) {
    const std::size_t n = records.size();
    if (n == 0) return {};

    // Stride chosen to land near kTargetReadbackSamples; always >= 1 so
    // small N (< kTargetReadbackSamples) degrades gracefully to "every
    // record". max() guard means tiny runs become a full readback set
    // but never call with stride 0.
    const std::size_t stride = std::max<std::size_t>(
        1, n / kTargetReadbackSamples);

    std::vector<readback_sample> out;
    out.reserve(n / stride + 4);

    auto push_idx = [&](std::size_t idx) {
        out.push_back({ records[idx].key, records[idx].value });
    };

    for (std::size_t i = 0; i < n; i += stride) push_idx(i);
    if (out.back().key != records.back().key) push_idx(n - 1);
    return out;
}

void
verify_readback_samples(const core::tree_manifest*           manifest,
                        std::span<const readback_sample>     samples) {
    std::vector<std::string_view> keys;
    keys.reserve(samples.size());
    for (const auto& s : samples) keys.emplace_back(s.key);

    auto lookup_results = submit_and_wait<std::vector<tree::lookup_result>>([&]() {
        // `tree::lookup` is bind_back (internally `with_context(...)(...)`),
        // so it needs a prev sender. `just()` seeds the chain.
        return pump::sender::just() >> tree::lookup(keys, manifest);
    });
    CHECK(lookup_results.size() == samples.size());

    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (!std::holds_alternative<tree::lookup_value>(lookup_results[i])) {
            std::fprintf(stderr,
                "FAIL: readback sample [%zu] key=%s: not lookup_value (variant_index=%zu)\n",
                i, samples[i].key.c_str(), lookup_results[i].index());
            std::abort();
        }
        const auto& lv = std::get<tree::lookup_value>(lookup_results[i]);

        auto got = submit_and_wait<std::string>([vr = lv.vr]() {
            return value::read_value(vr);
        });
        if (got != samples[i].expected_value) {
            std::fprintf(stderr,
                "FAIL: readback sample [%zu] key=%s value mismatch: "
                "got (len=%zu) '%.*s', expected (len=%zu) '%s'\n",
                i, samples[i].key.c_str(),
                got.size(), static_cast<int>(got.size()), got.data(),
                samples[i].expected_value.size(),
                samples[i].expected_value.c_str());
            std::abort();
        }
    }
}

}  // namespace

// ── Main ──────────────────────────────────────────────────────────────

int
main(int argc, char** argv) {
    // Line-buffer stdout so phase progress reaches the terminal / log file
    // before a mid-phase std::abort() from validate_flush_result -- the
    // block-buffered default swallows every printed line otherwise.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    auto opts = parse_argv(argc, argv);

    std::printf("test_flush_e2e: num_keys=%u, topology cores=[0,2,4,6,8] "
                "rd=[2,4,6] value=%d owner=%d\n",
                opts.num_keys, kValueCore, kOwnerCore);

    // ── Records ──
    auto spec    = make_key_spec(opts.num_keys);
    auto records = generate_kv_records(spec);
    std::printf("  generated %zu kv records (key_digits=%u, first=%s, last=%s)\n",
                records.size(),
                spec.key_digits,
                records.front().key.c_str(),
                records.back().key.c_str());

    // ── Disk ──
    auto fmt_opts = derive_format_options();
    auto buf      = format::make_formatted_storage(fmt_opts, kNamespaceBytes);
    mock_nvme::mock_device dev(std::move(buf), kNamespaceBytes, fmt_opts.lba_size);
    std::mutex dev_mtx;
    dev.enable_thread_safety(&dev_mtx);
    std::printf("  formatted mock device: %lu bytes (%lu LBAs @ %u B)\n",
                static_cast<unsigned long>(kNamespaceBytes),
                static_cast<unsigned long>(kNamespaceBytes / fmt_opts.lba_size),
                fmt_opts.lba_size);

    // ── Runtime ──
    runtime::build_options bopts{
        .cores             = kCores,
        .device            = &dev,
        .read_domain_cores = kReadDomainCores,
        .value_core        = kValueCore,
        .owner_core        = kOwnerCore,
    };
    auto* rt = runtime::build_runtime<TreeCache, ValueCache>(bopts);
    std::printf("  runtime built: bootstrap shard_count=%u (single-shard placeholder)\n",
                core::registry::current_shard_partitions()->shard_count());

    // ── Custom 3-shard map ──
    auto custom_map = build_harness_shard_map(records);
    rt::publish_shard_partitions(custom_map);
    CHECK(core::registry::current_shard_partitions()->shard_count() == 3);
    std::printf("  published 3-shard map: shard_count=%u\n",
                core::registry::current_shard_partitions()->shard_count());

    // ── Start per-core advance threads ──
    std::atomic<uint32_t> cores_started{0};
    std::vector<std::jthread> workers;
    workers.reserve(kCores.size());
    for (uint32_t core : kCores) {
        workers.emplace_back([rt, core, &cores_started]() {
            rt::run(rt, core, [&cores_started](auto*, uint32_t) {
                cores_started.fetch_add(1, std::memory_order_release);
            });
        });
    }
    while (cores_started.load(std::memory_order_acquire) <
           static_cast<uint32_t>(kCores.size())) {
        std::this_thread::yield();
    }

    // Main thread submits from core_id 0; pump::core::this_core_id only
    // steers per_core::queue enqueue routing, any id in the runtime's
    // per-core-array bounds works.
    pump::core::this_core_id = 0;
    std::printf("  advance threads up (%u cores)\n",
                static_cast<unsigned>(kCores.size()));

    // ── Phase 1: persist values ──
    std::printf("  phase 1: persist %zu values\n", records.size());
    auto durables = persist_all_values(records);
    std::printf("    persisted %zu value_refs\n", durables.size());

    // ── Phase 2: build sealed gen ──
    std::printf("  phase 2: build sealed gen_id=1 (%zu entries, lsn 1..%zu)\n",
                records.size(), records.size());
    auto gen = build_sealed_gen(
        /*gen_id=*/            1,
        /*front_owner_index=*/ 0,
        /*lsn_start=*/         1,
        records,
        durables);
    const uint64_t gen_max_lsn = gen->max_lsn;

    // ── Phase 3: flush ──
    std::printf("  phase 3: tree_local_flush\n");
    auto base_guard = make_empty_base_guard();
    auto outcome    = run_one_flush_round(gen, base_guard,
                                          /*recovery_safe_lsn=*/ 0);
    std::printf("    flush returned: st=%u, "
                "leaf_order.size=%zu, root_slot.lba=%lu, "
                "flushed_max_lsn=%lu, flushed_front_count=%zu\n",
                static_cast<unsigned>(outcome.result.st),
                outcome.result.new_manifest
                    ? outcome.result.new_manifest->leaf_order.size()
                    : 0,
                outcome.result.new_manifest
                    ? static_cast<unsigned long>(outcome.result.new_manifest->root_slot.lba)
                    : 0UL,
                static_cast<unsigned long>(outcome.result.flushed_max_lsn),
                outcome.result.flushed_gens_by_front.size());

    validate_flush_result(outcome.result,
                          /*expected_max_lsn=*/           gen_max_lsn,
                          /*expected_front_owner_index=*/ 0,
                          /*expected_gen_id=*/            1);

    // Drop the local gen reference; flushed_gens_by_front pins what the
    // reclaim pipeline will need, and readback only touches the
    // post-flush manifest.
    gen.reset();

    // ── Phase 4: readback ──
    auto samples = pick_samples(records);
    std::printf("  phase 4: readback %zu sample keys (first=%s .. last=%s)\n",
                samples.size(),
                samples.front().key.c_str(),
                samples.back().key.c_str());
    verify_readback_samples(outcome.manifest_for_readback.get(), samples);
    std::printf("    readback OK for %zu samples\n", samples.size());

    // ── Stop runtime ──
    for (uint32_t core : kCores) rt->is_running_by_core[core].store(false);
    workers.clear();   // jthread dtors join
    runtime::destroy_runtime<TreeCache, ValueCache>(rt);

    std::printf("all passed\n");
    return 0;
}
