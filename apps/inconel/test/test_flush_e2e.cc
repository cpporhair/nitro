//
// Tree-local flush end-to-end harness (step 033).
//
// Drives three back-to-back flush rounds on the asymmetric topology
// (value core 0, tree_read_domains on cores 2/4/6, owner core 8) against
// a freshly formatted mock disk, then verifies every sampled key lines
// up with an independently-tracked expected state.
//
//   Round 1 (bootstrap)  — N PUTs on [0, N).
//   Round 2 (~2N/5 ops)  — 100 overwrites + 100 tombstones + 200 new keys.
//   Round 3 (~2N/5 ops)  — 50 re-overwrites (double-overwrite on top of r2),
//                          50 single overwrites,
//                          50 tombstones on r1-only keys,
//                          50 tombstones on r2 newcomers,
//                          200 new keys.
//
// Final state covers: pristine-r1, singly-overwritten-r2, singly-
// overwritten-r3, doubly-overwritten-r3, r2-tombstoned, r3-tombstoned
// (both r1-origin and r2-newcomer origins), r2-newcomer surviving,
// r3-newcomer.
//
// Round ranges scale with --num-keys N (default 1000; N >= 1000 is
// required so every range stays non-empty and each round clears the
// "at least 300 ops per round" bar).
//
// Verification strides over the full expected state (~50 samples)
// through tree::lookup, then cross-checks every lookup_value /
// lookup_tombstone against the tracked map and re-reads the value
// bytes via value::read_value for each live sample.
//
// No production code is touched. Any failure aborts with a detailed
// context line so the follow-up session can locate the broken layer
// without rerunning the harness.
//

#include "apps/inconel/test/check.hh"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
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

// ── Multi-round sizing ─────────────────────────────────────────────────
//
// The fixed op-count formulas (see generate_roundN_ops below) require
// enough base keys to carve out the overlapping overwrite / tombstone
// windows. At N = 1000 both round 2 and round 3 clock in at 400 ops
// (> 300 as required); smaller N would shrink the per-range slices to
// zero or push round totals under 300, so the harness refuses to run
// in that regime instead of silently degrading coverage.

constexpr uint32_t kMinNumKeysForMultiRound = 1000;

// ── CLI ─────────────────────────────────────────────────────────────────

struct harness_options {
    uint32_t num_keys = 1000;
};

void
print_usage(const char* argv0) {
    std::printf(
        "usage: %s [--num-keys N]   (default 1000; N must be >= %u)\n",
        argv0, kMinNumKeysForMultiRound);
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
            if (!endp || *endp != '\0' ||
                v < static_cast<long>(kMinNumKeysForMultiRound) ||
                v > 100'000'000L) {
                std::fprintf(stderr,
                    "--num-keys must be an integer in [%u, 100000000]\n",
                    kMinNumKeysForMultiRound);
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

// ── Key / value format ─────────────────────────────────────────────────
//
// All keys across all rounds share the same zero-padded decimal width
// so ascii < order agrees with integer < order for every index in
// [0, max_round_index]. The width is chosen to fit the largest index
// any round will introduce (round 3's newcomer range reaches 3N/2 +
// N/5 - 1, i.e. 1.7N - 1). Values carry a round tag so readback can
// distinguish "value written in which round" from content alone.

uint32_t
key_digits_for(uint32_t max_key_index_inclusive) {
    uint32_t d = 1;
    uint64_t cap = 10;
    while (cap <= max_key_index_inclusive) {
        cap *= 10;
        ++d;
    }
    return std::max<uint32_t>(d, 3);  // floor 3 for tidy output on small N
}

std::string
make_key(uint32_t i, uint32_t digits) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%0*u", digits, i);
    return buf;
}

std::string
make_value(uint32_t i, uint32_t digits, uint32_t round_tag) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "val%u_%0*u", round_tag, digits, i);
    return buf;
}

// ── Round op types ─────────────────────────────────────────────────────

struct round_op {
    enum class kind : uint8_t { put, tombstone };
    kind        k;
    std::string key;
    std::string value;  // non-empty iff k == put
};

struct round_spec {
    uint64_t              gen_id;
    uint64_t              lsn_start;  // first op gets data_ver=lsn_start
    std::vector<round_op> ops;        // sorted ascending by key
};

struct round_outcome {
    tree::tree_flush_result                       result;
    std::shared_ptr<const core::tree_manifest>    manifest;
    std::shared_ptr<const core::checkpoint_guard> next_base_guard;
    uint64_t                                      next_lsn;
};

// ── Round generation ──────────────────────────────────────────────────
//
// Ranges scale with N = num_keys. Each helper produces ops sorted by
// key; the sort step is explicit so the data_ver assignment in
// run_round (lsn_start + position) matches the sequence apply_round_
// to_expected walks, keeping expected data_vers consistent with what
// the flush fold actually saw.

void
append_puts(std::vector<round_op>& ops,
            uint32_t               lo,
            uint32_t               hi,
            uint32_t               digits,
            uint32_t               round_tag) {
    for (uint32_t i = lo; i < hi; ++i) {
        ops.push_back(round_op{
            .k     = round_op::kind::put,
            .key   = make_key(i, digits),
            .value = make_value(i, digits, round_tag),
        });
    }
}

void
append_tombs(std::vector<round_op>& ops,
             uint32_t               lo,
             uint32_t               hi,
             uint32_t               digits) {
    for (uint32_t i = lo; i < hi; ++i) {
        ops.push_back(round_op{
            .k     = round_op::kind::tombstone,
            .key   = make_key(i, digits),
            .value = {},
        });
    }
}

void
sort_ops_by_key(std::vector<round_op>& ops) {
    std::sort(ops.begin(), ops.end(),
              [](const round_op& a, const round_op& b) {
                  return a.key < b.key;
              });
}

std::vector<round_op>
generate_round1_ops(uint32_t num_keys, uint32_t digits) {
    std::vector<round_op> ops;
    ops.reserve(num_keys);
    append_puts(ops, 0, num_keys, digits, 1);
    // Already ascending by construction.
    return ops;
}

std::vector<round_op>
generate_round2_ops(uint32_t N, uint32_t digits) {
    std::vector<round_op> ops;
    // 50 puts that round 3 will overwrite again (double-overwrite set).
    append_puts(ops, N / 20, N / 10, digits, 2);
    // 50 puts that stay at r2 value through round 3.
    append_puts(ops, N / 10, (3 * N) / 20, digits, 2);
    // 100 tombstones on r1-only keys that never come back.
    append_tombs(ops, (3 * N) / 10, (4 * N) / 10, digits);
    // 200 brand-new keys introduced in r2. Of these, [N, N + N/10) and
    // [N + 3N/20, 6N/5) survive; [N + N/10, N + 3N/20) gets tombstoned
    // in r3.
    append_puts(ops, N, (6 * N) / 5, digits, 2);
    sort_ops_by_key(ops);
    return ops;
}

std::vector<round_op>
generate_round3_ops(uint32_t N, uint32_t digits) {
    std::vector<round_op> ops;
    // 50 re-overwrites on the r2 double-overwrite set.
    append_puts(ops, N / 20, N / 10, digits, 3);
    // 50 overwrites on r1-only keys (r1 → r3, no r2 in between).
    append_puts(ops, N / 5, (N / 5) + (N / 20), digits, 3);
    // 50 tombstones on r1-only keys (different region from r2 tombs).
    append_tombs(ops, N / 2, (N / 2) + (N / 20), digits);
    // 50 tombstones on r2 newcomers (must lie inside round 2's new range).
    append_tombs(ops, N + (N / 10), N + ((3 * N) / 20), digits);
    // 200 brand-new keys introduced in r3.
    append_puts(ops, (3 * N) / 2, ((3 * N) / 2) + (N / 5), digits, 3);
    sort_ops_by_key(ops);
    return ops;
}

// ── Expected state ─────────────────────────────────────────────────────
//
// The harness tracks what every key should resolve to after every
// round, independent of the tree. After round R is submitted and
// acknowledged by tree_local_flush, the harness walks R.ops in sort
// order (same as build_sealed_gen) and overwrites/removes/inserts the
// corresponding expected entries, setting each entry's data_ver to
// lsn_start + position. The final lookup phase cross-references
// tree::lookup results against this map: lookup_value matches
// kind::value + bytes + data_ver; lookup_tombstone matches
// kind::tombstone + data_ver.

struct expected_entry {
    enum class kind : uint8_t { value, tombstone };
    kind        k;
    std::string value;     // empty iff k == tombstone
    uint64_t    data_ver;
};

using expected_state = std::map<std::string, expected_entry>;

void
apply_round_to_expected(const round_spec& spec, expected_state& state) {
    for (std::size_t i = 0; i < spec.ops.size(); ++i) {
        const auto&    op  = spec.ops[i];
        const uint64_t ver = spec.lsn_start + static_cast<uint64_t>(i);
        if (op.k == round_op::kind::put) {
            state[op.key] = expected_entry{
                .k        = expected_entry::kind::value,
                .value    = op.value,
                .data_ver = ver,
            };
        } else {
            state[op.key] = expected_entry{
                .k        = expected_entry::kind::tombstone,
                .value    = {},
                .data_ver = ver,
            };
        }
    }
}

// ── Shard map ───────────────────────────────────────────────────────────
//
// Three shards cut at the 1/3 and 2/3 ranks of round 1's sorted keys.
// Bootstrap (pre-round-1) routing goes through this manually-installed
// 3-shard map. Round 1's tree_local_flush tail auto-rebuilds the map
// from the fresh leaf_order (INC-042 fix), so rounds 2/3 route through
// whatever shard layout the current manifest implies -- harness no
// longer needs to republish between rounds.

std::shared_ptr<const core::shard_partition_map>
build_harness_shard_map_for_round1(uint32_t num_keys, uint32_t digits) {
    CHECK(num_keys >= 3);
    core::shard_partition_map map;

    const std::string f0 = make_key(num_keys / 3, digits);
    const std::string f1 = make_key((2 * num_keys) / 3, digits);
    CHECK(f0 < f1);

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

// ── Submit helper ──────────────────────────────────────────────────────

template <typename T, typename SenderBuilder>
T
submit_and_wait(SenderBuilder&& build_sender) {
    auto ctx     = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<T>>();
    auto fut     = promise->get_future();

    // `auto&&` so the promise lambda binds whether the pipeline forwards
    // the tail value as an rvalue or an lvalue -- tree_local_flush's
    // inner flat chain stores the result before the tail push, so it
    // can arrive as an lvalue.
    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::then([promise](auto&& r) {
            promise->set_value(std::forward<decltype(r)>(r));
        })
        >> pump::sender::submit(ctx);

    return fut.get();
}

// ── Value persist ──────────────────────────────────────────────────────
//
// Only ops of kind::put contribute to the persist call. The returned
// vector is indexed in the same order the put ops appear inside
// `round_ops`; build_sealed_gen walks the ops span in parallel with a
// running `put_idx` counter to pair entries with durables.

std::vector<format::value_ref>
persist_put_values(std::span<const round_op> round_ops) {
    std::size_t put_count = 0;
    for (const auto& op : round_ops)
        if (op.k == round_op::kind::put) ++put_count;

    std::vector<format::value_ref> durables(put_count);
    if (put_count == 0) return durables;   // all-tombstone round (unused today)

    std::vector<value::put_entry> entries;
    entries.reserve(put_count);
    std::size_t di = 0;
    for (const auto& op : round_ops) {
        if (op.k != round_op::kind::put) continue;
        entries.push_back(value::put_entry{
            .body   = op.value,
            .out_vr = &durables[di++],
        });
    }

    const bool ok = submit_and_wait<bool>([&]() {
        return value::persist_values(
            std::span<value::put_entry>(entries));
    });
    CHECK(ok && "value::persist_values returned ok=false");
    return durables;
}

// ── Memtable gen ───────────────────────────────────────────────────────
//
// Mixed put/tombstone entries sharing one gen. value ops reference
// durables[put_index] where put_index is the number of prior put ops
// in the same sorted span. Tombstones leave value_handle empty --
// fold preserves them as kind::tombstone winners / losers the same
// way it handles put entries (core/memtable.hh kind enum + losers
// retire list).

std::shared_ptr<core::memtable_gen>
build_sealed_gen(uint64_t                           gen_id,
                 uint32_t                           front_owner_index,
                 uint64_t                           lsn_start,
                 std::span<const round_op>          round_ops,
                 std::span<const format::value_ref> durables) {
    CHECK(!round_ops.empty());
    std::size_t put_count = 0;
    for (const auto& op : round_ops)
        if (op.k == round_op::kind::put) ++put_count;
    CHECK(durables.size() == put_count);

    auto gen = std::make_shared<core::memtable_gen>();
    gen->gen_id            = gen_id;
    gen->front_owner_index = front_owner_index;
    gen->st                = core::memtable_gen::state::sealed;
    gen->min_lsn           = lsn_start;
    gen->max_lsn           = lsn_start + round_ops.size() - 1;

    std::size_t put_idx = 0;
    for (std::size_t i = 0; i < round_ops.size(); ++i) {
        const auto& op       = round_ops[i];
        auto        key_view = gen->kv_arena.allocate(
                           op.key.data(), op.key.size());

        core::memtable_entry entry{};
        entry.data_ver = lsn_start + static_cast<uint64_t>(i);

        if (op.k == round_op::kind::put) {
            auto val_view = gen->kv_arena.allocate(
                op.value.data(), op.value.size());
            entry.k  = core::memtable_entry::kind::value;
            entry.vh = core::value_handle{
                .durable = durables[put_idx++],
                .hot     = core::value_view{
                    .data = val_view.data(),
                    .len  = static_cast<uint32_t>(val_view.size()),
                },
            };
        } else {
            entry.k  = core::memtable_entry::kind::tombstone;
            entry.vh = core::value_handle{};  // unused for tombstones
        }

        auto [it, inserted] = gen->table.try_emplace(key_view);
        CHECK(inserted);
        it->second.push_back(entry);
    }
    return gen;
}

// ── base_guard ─────────────────────────────────────────────────────────

std::shared_ptr<const core::checkpoint_guard>
make_empty_base_guard() {
    auto manifest = std::make_shared<const core::tree_manifest>(
        core::tree_manifest::empty(&runtime::kBootstrapTreeGeometry));
    core::checkpoint_guard guard{};
    guard.manifest = std::move(manifest);
    return std::make_shared<const core::checkpoint_guard>(std::move(guard));
}

std::shared_ptr<const core::checkpoint_guard>
wrap_manifest_as_guard(std::shared_ptr<const core::tree_manifest> manifest) {
    core::checkpoint_guard guard{};
    guard.manifest = std::move(manifest);
    return std::make_shared<const core::checkpoint_guard>(std::move(guard));
}

// ── Validation ─────────────────────────────────────────────────────────

void
validate_flush_result(const tree::tree_flush_result& r,
                      uint64_t                       expected_max_lsn,
                      uint32_t                       expected_front_owner_index,
                      uint64_t                       expected_gen_id,
                      const char*                    round_label) {
    if (r.st != tree::flush_stage_status::ok) {
        std::fprintf(stderr,
            "FAIL: [%s] flush_stage_status != ok (= %u)\n",
            round_label, static_cast<unsigned>(r.st));
        std::abort();
    }
    if (!r.new_manifest) {
        std::fprintf(stderr, "FAIL: [%s] new_manifest is null\n",
                     round_label);
        std::abort();
    }
    const auto& m = *r.new_manifest;
    if (!m.has_root()) {
        std::fprintf(stderr,
            "FAIL: [%s] new_manifest has no root "
            "(root_slot.lba=%lu, leaf_order.size=%zu, slot_map.size=%zu)\n",
            round_label,
            static_cast<unsigned long>(m.root_slot.lba),
            m.leaf_order.size(),
            m.slot_map.size());
        std::abort();
    }
    if (m.leaf_order.size() == 0) {
        std::fprintf(stderr,
            "FAIL: [%s] new_manifest.leaf_order is empty\n",
            round_label);
        std::abort();
    }
    if (r.flushed_max_lsn != expected_max_lsn) {
        std::fprintf(stderr,
            "FAIL: [%s] flushed_max_lsn %lu != expected %lu\n",
            round_label,
            static_cast<unsigned long>(r.flushed_max_lsn),
            static_cast<unsigned long>(expected_max_lsn));
        std::abort();
    }
    auto it = r.flushed_gens_by_front.find(expected_front_owner_index);
    if (it == r.flushed_gens_by_front.end()) {
        std::fprintf(stderr,
            "FAIL: [%s] flushed_gens_by_front missing front_owner_index=%u "
            "(size=%zu)\n",
            round_label,
            expected_front_owner_index,
            r.flushed_gens_by_front.size());
        std::abort();
    }
    bool found_gen = false;
    for (const auto& g : it->second) {
        if (g && g->gen_id == expected_gen_id) {
            found_gen = true;
            break;
        }
    }
    if (!found_gen) {
        std::fprintf(stderr,
            "FAIL: [%s] flushed_gens_by_front[%u] missing gen_id=%lu\n",
            round_label,
            expected_front_owner_index,
            static_cast<unsigned long>(expected_gen_id));
        std::abort();
    }
}

// ── Single round runner ────────────────────────────────────────────────

round_outcome
run_round(const round_spec&                             spec,
          std::shared_ptr<const core::checkpoint_guard> base_guard,
          const char*                                   round_label) {
    CHECK(!spec.ops.empty());

    // 1. Persist every put value; empty-put rounds would skip the call,
    //    but current round generators always include puts so we keep
    //    the CHECK-noisy fall-through.
    auto durables = persist_put_values(spec.ops);

    // 2. Build a sealed memtable_gen off the sorted ops span.
    auto gen = build_sealed_gen(
        spec.gen_id,
        /*front_owner_index=*/ 0,
        spec.lsn_start,
        spec.ops,
        durables);
    const uint64_t gen_max_lsn = gen->max_lsn;

    // 3. Submit tree_local_flush.
    tree::tree_flush_request req{
        .base_guard        = std::move(base_guard),
        .sealed_gens       = {},
        .recovery_safe_lsn = 0,
    };
    req.sealed_gens.push_back(std::move(gen));

    auto req_holder =
        std::make_shared<tree::tree_flush_request>(std::move(req));
    auto result = submit_and_wait<tree::tree_flush_result>([req_holder]() {
        return tree::tree_local_flush(std::move(*req_holder));
    });

    // 4. Validate + wrap the new manifest as the next base_guard.
    validate_flush_result(result,
        /*expected_max_lsn=*/             gen_max_lsn,
        /*expected_front_owner_index=*/   0,
        /*expected_gen_id=*/              spec.gen_id,
        round_label);

    auto manifest        = result.new_manifest;
    auto next_base_guard = wrap_manifest_as_guard(manifest);

    return round_outcome{
        .result          = std::move(result),
        .manifest        = std::move(manifest),
        .next_base_guard = std::move(next_base_guard),
        .next_lsn        = spec.lsn_start + spec.ops.size(),
    };
}

// ── Final verification ─────────────────────────────────────────────────

constexpr std::size_t kTargetReadbackSamples = 50;

struct verify_sample {
    std::string    key;
    expected_entry expected;
};

std::vector<verify_sample>
pick_verify_samples(const expected_state& state) {
    std::vector<verify_sample> flat;
    flat.reserve(state.size());
    for (const auto& [k, v] : state) flat.push_back({k, v});

    const std::size_t n = flat.size();
    if (n == 0) return {};

    const std::size_t stride =
        std::max<std::size_t>(1, n / kTargetReadbackSamples);

    std::vector<verify_sample> out;
    out.reserve(n / stride + 4);
    for (std::size_t i = 0; i < n; i += stride) out.push_back(flat[i]);
    if (out.back().key != flat.back().key) out.push_back(flat.back());
    return out;
}

struct verify_report {
    std::size_t value_checks_ok     = 0;
    std::size_t tombstone_checks_ok = 0;
};

verify_report
verify_against_expected(const core::tree_manifest*       manifest,
                        std::span<const verify_sample>   samples) {
    verify_report report{};

    std::vector<std::string_view> keys;
    keys.reserve(samples.size());
    for (const auto& s : samples) keys.emplace_back(s.key);

    auto lookup_results =
        submit_and_wait<std::vector<tree::lookup_result>>([&]() {
            // tree::lookup is bind_back (with_context(...)(...)), needs a
            // prev sender to seed the chain.
            return pump::sender::just() >> tree::lookup(keys, manifest);
        });
    CHECK(lookup_results.size() == samples.size());

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& exp = samples[i].expected;
        if (exp.k == expected_entry::kind::value) {
            if (!std::holds_alternative<tree::lookup_value>(
                    lookup_results[i])) {
                std::fprintf(stderr,
                    "FAIL: sample [%zu] key=%s expected value (ver=%lu) but "
                    "got lookup variant_index=%zu\n",
                    i, samples[i].key.c_str(),
                    static_cast<unsigned long>(exp.data_ver),
                    lookup_results[i].index());
                std::abort();
            }
            const auto& lv =
                std::get<tree::lookup_value>(lookup_results[i]);
            if (lv.data_ver != exp.data_ver) {
                std::fprintf(stderr,
                    "FAIL: sample [%zu] key=%s value data_ver mismatch: "
                    "got %lu, expected %lu\n",
                    i, samples[i].key.c_str(),
                    static_cast<unsigned long>(lv.data_ver),
                    static_cast<unsigned long>(exp.data_ver));
                std::abort();
            }
            auto got = submit_and_wait<std::string>([vr = lv.vr]() {
                return value::read_value(vr);
            });
            if (got != exp.value) {
                std::fprintf(stderr,
                    "FAIL: sample [%zu] key=%s value bytes mismatch: "
                    "got (len=%zu) '%.*s', expected (len=%zu) '%s'\n",
                    i, samples[i].key.c_str(),
                    got.size(),
                    static_cast<int>(got.size()), got.data(),
                    exp.value.size(),
                    exp.value.c_str());
                std::abort();
            }
            ++report.value_checks_ok;
        } else {
            if (!std::holds_alternative<tree::lookup_tombstone>(
                    lookup_results[i])) {
                std::fprintf(stderr,
                    "FAIL: sample [%zu] key=%s expected tombstone (ver=%lu) "
                    "but got lookup variant_index=%zu\n",
                    i, samples[i].key.c_str(),
                    static_cast<unsigned long>(exp.data_ver),
                    lookup_results[i].index());
                std::abort();
            }
            const auto& lt =
                std::get<tree::lookup_tombstone>(lookup_results[i]);
            if (lt.data_ver != exp.data_ver) {
                std::fprintf(stderr,
                    "FAIL: sample [%zu] key=%s tombstone data_ver mismatch: "
                    "got %lu, expected %lu\n",
                    i, samples[i].key.c_str(),
                    static_cast<unsigned long>(lt.data_ver),
                    static_cast<unsigned long>(exp.data_ver));
                std::abort();
            }
            ++report.tombstone_checks_ok;
        }
    }
    return report;
}

// ── per-round stdout summary helpers ───────────────────────────────────

void
print_round_header(const char*       label,
                   const round_spec& spec) {
    std::size_t put_count = 0;
    std::size_t tomb_count = 0;
    for (const auto& op : spec.ops) {
        if (op.k == round_op::kind::put) ++put_count;
        else                              ++tomb_count;
    }
    std::printf("  %s: %zu ops (%zu put, %zu tombstone, "
                "gen_id=%lu, lsn %lu..%lu)\n",
                label,
                spec.ops.size(),
                put_count,
                tomb_count,
                static_cast<unsigned long>(spec.gen_id),
                static_cast<unsigned long>(spec.lsn_start),
                static_cast<unsigned long>(
                    spec.lsn_start + spec.ops.size() - 1));
}

void
print_round_flush_result(const char*                    label,
                         const tree::tree_flush_result& r) {
    std::printf("    %s flush: st=%u, leaf_order.size=%zu, "
                "root_slot.lba=%lu, flushed_max_lsn=%lu, "
                "flushed_front_count=%zu\n",
                label,
                static_cast<unsigned>(r.st),
                r.new_manifest ? r.new_manifest->leaf_order.size() : 0UL,
                r.new_manifest ? static_cast<unsigned long>(
                    r.new_manifest->root_slot.lba) : 0UL,
                static_cast<unsigned long>(r.flushed_max_lsn),
                r.flushed_gens_by_front.size());
}

void
print_expected_state_summary(const expected_state& state) {
    std::size_t value_count = 0;
    std::size_t tomb_count  = 0;
    for (const auto& [k, v] : state) {
        if (v.k == expected_entry::kind::value) ++value_count;
        else                                    ++tomb_count;
    }
    std::printf("  expected state after 3 rounds: %zu keys "
                "(%zu value, %zu tombstone)\n",
                state.size(), value_count, tomb_count);
}

}  // namespace

// ── Main ──────────────────────────────────────────────────────────────

int
main(int argc, char** argv) {
    // Line-buffer stdout so phase progress reaches the terminal / log
    // before a mid-phase std::abort() -- the block-buffered default
    // swallows every printed line otherwise.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    auto opts = parse_argv(argc, argv);

    std::printf("test_flush_e2e: num_keys=%u, topology cores=[0,2,4,6,8] "
                "rd=[2,4,6] value=%d owner=%d\n",
                opts.num_keys, kValueCore, kOwnerCore);

    // ── Derive global key width ──
    //
    // Round 3's newcomer range reaches 3N/2 + N/5 - 1 = 17N/10 - 1;
    // the digit width must fit that so ascii < agrees with integer <
    // for every key we'll ever mint.
    const uint32_t max_key_index =
        ((3 * opts.num_keys) / 2) + (opts.num_keys / 5) - 1;
    const uint32_t key_digits = key_digits_for(max_key_index);
    std::printf("  key_digits=%u (max_key_index=%u)\n",
                key_digits, max_key_index);

    // ── Disk ──
    auto fmt_opts = derive_format_options();
    auto buf      = format::make_formatted_storage(fmt_opts, kNamespaceBytes);
    mock_nvme::mock_device dev(std::move(buf),
                               kNamespaceBytes,
                               fmt_opts.lba_size);
    std::mutex dev_mtx;
    dev.enable_thread_safety(&dev_mtx);
    std::printf("  formatted mock device: %lu bytes (%lu LBAs @ %u B)\n",
                static_cast<unsigned long>(kNamespaceBytes),
                static_cast<unsigned long>(
                    kNamespaceBytes / fmt_opts.lba_size),
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
    std::printf("  runtime built: bootstrap shard_count=%u "
                "(single-shard placeholder)\n",
                core::registry::current_shard_partitions()->shard_count());

    // ── Custom 3-shard map for round 1 routing ──
    //
    // Only needed before round 1. Post-round-1, tree_local_flush's
    // rebuild_and_publish_shard_partitions tail re-derives the map
    // from new_manifest.leaf_order (INC-042) -- rounds 2/3 route
    // through that auto-rebuilt map.
    auto custom_map = build_harness_shard_map_for_round1(
        opts.num_keys, key_digits);
    rt::publish_shard_partitions(custom_map);
    CHECK(core::registry::current_shard_partitions()->shard_count() == 3);
    std::printf("  published round-1 3-shard map: shard_count=%u "
                "(fences key_%0*u / key_%0*u)\n",
                core::registry::current_shard_partitions()->shard_count(),
                key_digits, opts.num_keys / 3,
                key_digits, (2 * opts.num_keys) / 3);

    // ── Start per-core advance threads ──
    std::atomic<uint32_t>     cores_started{0};
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

    pump::core::this_core_id = 0;
    std::printf("  advance threads up (%u cores)\n",
                static_cast<unsigned>(kCores.size()));

    expected_state state;

    // ── Round 1 ──
    round_spec r1{
        .gen_id    = 1,
        .lsn_start = 1,
        .ops       = generate_round1_ops(opts.num_keys, key_digits),
    };
    print_round_header("round 1", r1);
    auto r1_out = run_round(r1, make_empty_base_guard(), "round 1");
    apply_round_to_expected(r1, state);
    print_round_flush_result("round 1", r1_out.result);

    // ── Round 2 ──
    round_spec r2{
        .gen_id    = 2,
        .lsn_start = r1_out.next_lsn,
        .ops       = generate_round2_ops(opts.num_keys, key_digits),
    };
    print_round_header("round 2", r2);
    auto r2_out = run_round(r2, r1_out.next_base_guard, "round 2");
    apply_round_to_expected(r2, state);
    print_round_flush_result("round 2", r2_out.result);

    // ── Round 3 ──
    round_spec r3{
        .gen_id    = 3,
        .lsn_start = r2_out.next_lsn,
        .ops       = generate_round3_ops(opts.num_keys, key_digits),
    };
    print_round_header("round 3", r3);
    auto r3_out = run_round(r3, r2_out.next_base_guard, "round 3");
    apply_round_to_expected(r3, state);
    print_round_flush_result("round 3", r3_out.result);

    // Drop the now-superseded intermediate guard references; the final
    // manifest is pinned through r3_out.manifest and next_base_guard.
    r1_out.next_base_guard.reset();
    r2_out.next_base_guard.reset();

    // ── Final verification ──
    print_expected_state_summary(state);
    auto samples = pick_verify_samples(state);
    std::printf("  verify %zu sampled keys against expected state "
                "(first=%s .. last=%s)\n",
                samples.size(),
                samples.front().key.c_str(),
                samples.back().key.c_str());
    auto report = verify_against_expected(
        r3_out.manifest.get(), samples);
    std::printf("    readback OK for %zu samples "
                "(%zu value / %zu tombstone)\n",
                samples.size(),
                report.value_checks_ok,
                report.tombstone_checks_ok);

    // ── Stop runtime ──
    for (uint32_t core : kCores) rt->is_running_by_core[core].store(false);
    workers.clear();   // jthread dtors join
    runtime::destroy_runtime<TreeCache, ValueCache>(rt);

    std::printf("all passed\n");
    return 0;
}
