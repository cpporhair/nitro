//
// Tree-local flush end-to-end harness (step 033).
//
// Drives three back-to-back flush rounds on the asymmetric topology
// (value core 0, tree_read_domains on cores 2/4/6, owner core 8) against
// a freshly formatted real NVMe namespace, then verifies every sampled key lines
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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <random>
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
#include "pump/sender/concurrent.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/just.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include "apps/inconel/core/checkpoint_guard.hh"
#include "apps/inconel/core/data_area_heads.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/page_cache.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/shard_partition.hh"
#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/format/format_options.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/layout_plan.hh"
#include "apps/inconel/format/superblock_builder.hh"
#include "apps/inconel/format/types.hh"
#include "apps/inconel/nvme/runtime_scheduler.hh"
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
// Sized off the profile's Value Area top so the real namespace must
// satisfy build_runtime's tier-3 validate_build_inputs gate
// (device_bytes >= profile.value_data_area_end.lba * lba_size). Any
// further scaling happens by editing the profile, not here.

constexpr uint64_t kNamespaceBytes =
    static_cast<uint64_t>(
        format::kBootstrapFormatProfile.value_data_area_end.lba) *
    format::kBootstrapFormatProfile.lba_size;

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
    std::string pci_addr;
    std::string spdk_core_mask;
    uint32_t num_keys           = 1000;
    uint32_t rounds             = 3;        // ≥ 2 (round 1 bootstrap + round 2 mix); round 3+ reuses round-3 pattern with escalating round_tag/gen_id/lsn
    uint32_t readback_samples   = 50;       // approximate sample count; stride derived from expected-state size
    uint32_t concurrent_readers = 2;        // 0 disables; cap 2 (cores 10 + idx must not collide with advance cores 0/2/4/6/8 or main core 0)
    uint32_t reader_batch       = 16;       // keys per reader lookup batch
    uint32_t qpair_depth        = 256;
    bool     force_format       = false;
};

void
print_usage(const char* argv0) {
    std::printf(
        "usage: %s --pci-addr BDF --force-format "
        "[--num-keys N] [--rounds R] [--readback-samples S]\n"
        "         [--concurrent-readers N] [--reader-batch K]\n"
        "         [--spdk-core-mask MASK] [--qpair-depth D]\n"
        "  --pci-addr BDF           PCI BDF of the SPDK-bound NVMe controller\n"
        "                           (or INCONEL_NVME_PCI_ADDR env var)\n"
        "  --force-format           required; writes Inconel metadata to the device\n"
        "  --num-keys N             N >= %u (default 1000)\n"
        "  --rounds   R             R >= 2   (default 3; extra rounds reuse the round-3 op pattern)\n"
        "  --readback-samples S     target number of keys to read back after rounds (default 50)\n"
        "  --concurrent-readers N   0..2 (default 2; 0 disables; readers run on cores 10..10+N-1)\n"
        "  --reader-batch K         keys per reader lookup batch (default 16)\n"
        "  --spdk-core-mask MASK    optional SPDK env core mask override\n"
        "  --qpair-depth D          NVMe qpair depth (default 256)\n",
        argv0, kMinNumKeysForMultiRound);
}

long
parse_long_arg(const char* name, const char* arg, long lo, long hi) {
    char* endp = nullptr;
    long v = std::strtol(arg, &endp, 10);
    if (!endp || *endp != '\0' || v < lo || v > hi) {
        std::fprintf(stderr,
            "%s must be an integer in [%ld, %ld]\n",
            name, lo, hi);
        std::exit(2);
    }
    return v;
}

harness_options
parse_argv(int argc, char** argv) {
    harness_options o;
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        auto want_arg = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                print_usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--num-keys") {
            o.num_keys = static_cast<uint32_t>(parse_long_arg(
                "--num-keys", want_arg("--num-keys"),
                kMinNumKeysForMultiRound, 100'000'000L));
        } else if (a == "--pci-addr") {
            o.pci_addr = want_arg("--pci-addr");
        } else if (a == "--spdk-core-mask") {
            o.spdk_core_mask = want_arg("--spdk-core-mask");
        } else if (a == "--force-format") {
            o.force_format = true;
        } else if (a == "--qpair-depth") {
            o.qpair_depth = static_cast<uint32_t>(parse_long_arg(
                "--qpair-depth", want_arg("--qpair-depth"), 1L, 65535L));
        } else if (a == "--rounds") {
            o.rounds = static_cast<uint32_t>(parse_long_arg(
                "--rounds", want_arg("--rounds"), 2L, 1000L));
        } else if (a == "--readback-samples") {
            o.readback_samples = static_cast<uint32_t>(parse_long_arg(
                "--readback-samples", want_arg("--readback-samples"),
                1L, 10'000'000L));
        } else if (a == "--concurrent-readers") {
            // Hard cap 2 — plan §4.5.7 picks cores {10, 11} deliberately.
            // Growing past 2 requires choosing further unused core ids
            // and reasoning through per_core::queue fan-in again; do that
            // in a follow-up step, not via CLI surgery.
            o.concurrent_readers = static_cast<uint32_t>(parse_long_arg(
                "--concurrent-readers",
                want_arg("--concurrent-readers"),
                0L, 2L));
        } else if (a == "--reader-batch") {
            o.reader_batch = static_cast<uint32_t>(parse_long_arg(
                "--reader-batch", want_arg("--reader-batch"),
                1L, 1024L));
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
    if (o.pci_addr.empty()) {
        if (const char* env = std::getenv("INCONEL_NVME_PCI_ADDR")) {
            o.pci_addr = env;
        }
    }
    if (o.pci_addr.empty()) {
        std::fprintf(stderr, "--pci-addr or INCONEL_NVME_PCI_ADDR is required\n");
        print_usage(argv[0]);
        std::exit(2);
    }
    if (!o.force_format) {
        std::fprintf(stderr,
            "--force-format is required because this test writes to the NVMe device\n");
        print_usage(argv[0]);
        std::exit(2);
    }
    return o;
}

// ── Wall-clock helpers ─────────────────────────────────────────────────
//
// Everything in the harness uses steady_clock (monotonic, immune to
// wall-clock jumps) and reports milliseconds or seconds with one
// decimal so per-round regressions are visible at a glance without
// digging through log-noise.

using clock_t_  = std::chrono::steady_clock;
using tp_t      = clock_t_::time_point;

inline tp_t
now_tp() {
    return clock_t_::now();
}

inline double
ms_between(tp_t a, tp_t b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
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

struct round_timing {
    double persist_ms       = 0.0;
    double build_sealed_ms  = 0.0;
    double flush_ms         = 0.0;
    double total_ms         = 0.0;
};

struct round_outcome {
    tree::tree_flush_result                       result;
    std::shared_ptr<const core::tree_manifest>    manifest;
    std::shared_ptr<const core::checkpoint_guard> next_base_guard;
    uint64_t                                      next_lsn;
    round_timing                                  timing;
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
generate_round3_ops(uint32_t N, uint32_t digits, uint32_t round_tag) {
    std::vector<round_op> ops;
    // 50 re-overwrites on the r2 double-overwrite set.
    append_puts(ops, N / 20, N / 10, digits, round_tag);
    // 50 overwrites on r1-only keys (r1 → r3, no r2 in between).
    append_puts(ops, N / 5, (N / 5) + (N / 20), digits, round_tag);
    // 50 tombstones on r1-only keys (different region from r2 tombs).
    append_tombs(ops, N / 2, (N / 2) + (N / 20), digits);
    // 50 tombstones on r2 newcomers (must lie inside round 2's new range).
    append_tombs(ops, N + (N / 10), N + ((3 * N) / 20), digits);
    // 200 brand-new keys introduced in r3.
    append_puts(ops, (3 * N) / 2, ((3 * N) / 2) + (N / 5), digits, round_tag);
    sort_ops_by_key(ops);
    return ops;
}

// ── Round dispatcher ───────────────────────────────────────────────────
//
// Rounds 1 and 2 each have a unique workload; rounds 3+ reuse the
// round-3 pattern with an escalating `round_tag` (encoded into every
// PUT value so readback can still tell "written in round R" apart from
// earlier generations). Re-running the round-3 pattern exercises
// shadow-CoW slot cycling, tombstone reassertion, and overwrite of
// already-touched keys — what the plan calls "多轮 cascade 累积效应".

std::vector<round_op>
generate_round_ops(uint32_t round_id, uint32_t N, uint32_t digits) {
    if (round_id == 1) return generate_round1_ops(N, digits);
    if (round_id == 2) return generate_round2_ops(N, digits);
    return generate_round3_ops(N, digits, round_id);
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

// ── Concurrent reader snapshot machinery (plan §4.5) ───────────────────
//
// Writer (main thread) publishes one (manifest, expected_state, round_id)
// triple per round-end via atomic shared_ptr store. Readers hold two
// slots — current + previous — so a lookup issued against `current` right
// before a publish swaps is allowed to match either half of the race
// window before the harness treats it as a real mismatch.
//
// The `expected_state` is deep-copied on every publish so readers observe
// an immutable view. Copy cost lands on the main thread; reader load is
// a single atomic shared_ptr load per batch. The copy is linear in the
// state size (~1M entries at N=10^6) which is acceptable for 035 — a
// future delta-patch scheme is outside Phase 2's scope.

struct published_snapshot_t {
    std::shared_ptr<const core::tree_manifest> manifest;
    std::shared_ptr<const expected_state>      expected;
    uint64_t                                   round_id = 0;
};

// libstdc++ ≥ 11 implements std::atomic<std::shared_ptr<T>> (C++20). If
// a future toolchain regresses, plan §7 Q16 authorizes switching to a
// mutex + shared_ptr fallback — publish is off the write-hot path.
std::atomic<std::shared_ptr<published_snapshot_t>> g_current_snap;

std::atomic<bool> g_reader_stop{false};

std::shared_ptr<published_snapshot_t>
make_snapshot(std::shared_ptr<const core::tree_manifest> manifest,
              std::shared_ptr<const expected_state>      expected,
              uint64_t                                   round_id) {
    return std::make_shared<published_snapshot_t>(
        published_snapshot_t{
            .manifest = std::move(manifest),
            .expected = std::move(expected),
            .round_id = round_id,
        });
}

void
publish_empty_snapshot() {
    auto empty_manifest = std::make_shared<const core::tree_manifest>(
        core::tree_manifest::empty(&runtime::kBootstrapTreeGeometry));
    auto empty_expected = std::make_shared<const expected_state>();
    g_current_snap.store(
        make_snapshot(std::move(empty_manifest),
                      std::move(empty_expected),
                      /*round_id=*/ 0),
        std::memory_order_release);
}

void
publish_round_snapshot(std::shared_ptr<const core::tree_manifest> manifest,
                       const expected_state&                      state,
                       uint64_t                                   round_id) {
    auto expected_copy = std::make_shared<const expected_state>(state);
    g_current_snap.store(
        make_snapshot(std::move(manifest),
                      std::move(expected_copy),
                      round_id),
        std::memory_order_release);
}

struct reader_counters {
    uint64_t ok        = 0;   // matched current snapshot on first try
    uint64_t ok_stale  = 0;   // fell back to previous snapshot's expected
    uint64_t ok_retry  = 0;   // reloaded snapshot + re-ran lookup to match
    uint64_t batches   = 0;   // lookup batches issued (not counting retries)
    uint64_t mismatch  = 0;   // real mismatch after retry — abort path
};

// match_all_against implements plan §4.5.4: key present in expected →
// variant + data_ver must match; key absent in expected → lookup must
// return lookup_absent. value bytes are NOT checked — final static
// verify (run under a quiesced writer) covers those after readers join.
bool
match_all_against(std::span<const std::string_view>      keys,
                  std::span<const tree::lookup_result>   results,
                  const expected_state&                  expected) {
    if (keys.size() != results.size()) return false;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        // std::map<std::string, _> lacks a transparent comparator;
        // allocating a std::string per key per batch is trivial
        // overhead at the reader scale Phase 2 runs (16 keys/batch).
        auto it = expected.find(std::string(keys[i]));
        if (it == expected.end()) {
            if (!std::holds_alternative<tree::lookup_absent>(results[i]))
                return false;
            continue;
        }
        const auto& exp = it->second;
        if (exp.k == expected_entry::kind::value) {
            if (!std::holds_alternative<tree::lookup_value>(results[i]))
                return false;
            if (std::get<tree::lookup_value>(results[i]).data_ver !=
                exp.data_ver)
                return false;
        } else {
            if (!std::holds_alternative<tree::lookup_tombstone>(results[i]))
                return false;
            if (std::get<tree::lookup_tombstone>(results[i]).data_ver !=
                exp.data_ver)
                return false;
        }
    }
    return true;
}

// Snapshot key-view cache. Populated from `current.expected` every time
// the reader advances to a new snapshot. string_views are backed by the
// map keys inside the snapshot's expected_state; because the reader
// keeps `current` alive by shared_ptr, those views stay valid across
// picks without extra ownership bookkeeping.
void
rebuild_keys_cache(std::vector<std::string_view>& cache,
                   const expected_state&          expected) {
    cache.clear();
    cache.reserve(expected.size());
    for (const auto& [k, _] : expected)
        cache.emplace_back(k);
}

std::vector<std::string_view>
pick_random_keys(const std::vector<std::string_view>& cache,
                 std::size_t                          k,
                 std::mt19937_64&                     rng) {
    std::vector<std::string_view> out;
    if (cache.empty()) return out;
    out.reserve(k);
    std::uniform_int_distribution<std::size_t>
        dist(0, cache.size() - 1);
    for (std::size_t i = 0; i < k; ++i)
        out.push_back(cache[dist(rng)]);
    return out;
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

// ── Real NVMe bootstrap format ─────────────────────────────────────────

template <typename SenderBuilder>
bool
submit_nvme_and_wait(nvme::runtime_scheduler& sched,
                     SenderBuilder&&          build_sender) {
    auto ctx     = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<bool>>();
    auto fut     = promise->get_future();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::then([promise](bool ok) {
            promise->set_value(ok);
        })
        >> pump::sender::submit(ctx);

    while (fut.wait_for(std::chrono::milliseconds(0)) !=
           std::future_status::ready) {
        sched.advance();
        std::this_thread::yield();
    }
    return fut.get();
}

void
write_superblock_page(nvme::runtime_scheduler&     sched,
                      uint64_t                     lba,
                      const format::superblock&    sb,
                      uint32_t                     lba_size) {
    CHECK(sizeof(sb) <= lba_size);
    std::vector<char> page(lba_size, 0);
    std::memcpy(page.data(), &sb, sizeof(sb));

    const bool ok = submit_nvme_and_wait(sched, [&]() {
        return sched.write(lba, page.data(), 1, nvme::IO_FLAGS_FUA);
    });
    CHECK(ok && "real NVMe superblock write failed");
}

void
format_real_nvme_bootstrap(nvme::runtime_device& dev,
                           const format::format_options& fmt_opts) {
    const auto layout = format::compute_layout(fmt_opts, kNamespaceBytes);
    format::validate_layout(layout);

    const auto sb_a = format::build_superblock(layout, /*generation=*/1);
    const auto sb_b = format::build_superblock(layout, /*generation=*/0);

    pump::core::this_core_id = 0;
    nvme::runtime_scheduler fmt_sched(
        dev.qpair_for_core(0),
        fmt_opts.lba_size,
        /*pool_pages=*/64,
        /*queue_depth=*/128,
        /*local_depth=*/32,
        /*alignment=*/4096,
        SPDK_ENV_NUMA_ID_ANY,
        dev.device_id());

    write_superblock_page(fmt_sched, 0, sb_a, fmt_opts.lba_size);
    write_superblock_page(fmt_sched, 1, sb_b, fmt_opts.lba_size);

    const bool flushed = submit_nvme_and_wait(fmt_sched, [&]() {
        return fmt_sched.flush();
    });
    CHECK(flushed && "real NVMe flush after bootstrap format failed");
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

// ── Concurrent reader main loop (plan §4.5.3) ──────────────────────────
//
// Defined after submit_and_wait so it can call into the submit helper
// directly without a forward declaration. Everything it needs from the
// snapshot-publish block above is already in scope (the types live in
// the same anonymous namespace).

void
reader_main(uint32_t         reader_idx,
            uint32_t         batch_size,
            reader_counters* out_counters) {
    // Claim a non-advance, non-main core id so per_core::queue enqueues
    // from this reader land in a private SPSC slot. Advance workers
    // own cores {0, 2, 4, 6, 8}; the main (submit) thread is on core 0;
    // reader ids {10, 11} are unused and have their own queue slots.
    // MAX_CORES=128 in pump/core/lock_free_queue.hh, so the slot
    // exists; no runtime registration is required (readers don't run
    // rt::run — they just submit and block on the promise).
    pump::core::this_core_id = 10 + reader_idx;

    std::shared_ptr<const published_snapshot_t> current =
        g_current_snap.load(std::memory_order_acquire);
    std::shared_ptr<const published_snapshot_t> previous = current;
    const published_snapshot_t* cached_key_owner = current.get();
    std::vector<std::string_view> key_cache;
    rebuild_keys_cache(key_cache, *current->expected);

    std::mt19937_64 rng(0xBEEF1234ULL ^ reader_idx);

    reader_counters c{};

    while (!g_reader_stop.load(std::memory_order_relaxed)) {
        auto fresh = g_current_snap.load(std::memory_order_acquire);
        if (fresh.get() != current.get()) {
            previous = std::move(current);
            current  = std::move(fresh);
        }
        if (current.get() != cached_key_owner) {
            rebuild_keys_cache(key_cache, *current->expected);
            cached_key_owner = current.get();
        }
        if (key_cache.empty()) {
            // Pre-round-1 state: nothing to read yet. Back off with
            // yield so we don't spin on an empty map rebuild.
            std::this_thread::yield();
            continue;
        }

        auto keys = pick_random_keys(key_cache, batch_size, rng);
        if (keys.empty()) continue;

        // First attempt — align with current snapshot's manifest.
        auto results = submit_and_wait<std::vector<tree::lookup_result>>(
            [&]() {
                return pump::sender::just()
                     >> tree::lookup(keys, current->manifest.get());
            });
        ++c.batches;
        CHECK(results.size() == keys.size());

        if (match_all_against(keys, results, *current->expected)) {
            ++c.ok;
            continue;
        }
        if (previous && previous.get() != current.get() &&
            match_all_against(keys, results, *previous->expected)) {
            ++c.ok_stale;
            continue;
        }

        // Reload once to soak up benign race windows where the publish
        // swapped snapshots between our load and our submit.
        auto reload = g_current_snap.load(std::memory_order_acquire);
        auto results2 = submit_and_wait<std::vector<tree::lookup_result>>(
            [&]() {
                return pump::sender::just()
                     >> tree::lookup(keys, reload->manifest.get());
            });
        ++c.batches;
        if (match_all_against(keys, results2, *reload->expected)) {
            ++c.ok_retry;
            continue;
        }

        // Real mismatch — retry is exhausted and no historical snapshot
        // explains the result. Report with enough context to reproduce,
        // then abort (plan §7: UB/abort is the acceptable outcome, the
        // known_issues entry + reproducer command is the ship gate).
        ++c.mismatch;
        std::fprintf(stderr,
            "FAIL: reader %u mismatch "
            "(current.round=%lu reload.round=%lu batch=%u)\n",
            reader_idx,
            static_cast<unsigned long>(current->round_id),
            static_cast<unsigned long>(reload->round_id),
            batch_size);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            std::fprintf(stderr,
                "  key[%zu]=%.*s initial.index=%zu retry.index=%zu\n",
                i,
                static_cast<int>(keys[i].size()), keys[i].data(),
                results[i].index(),
                results2[i].index());
        }
        *out_counters = c;
        std::abort();
    }

    *out_counters = c;
}

// ── Value persist ──────────────────────────────────────────────────────
//
// Only ops of kind::put contribute to the persist call. The returned
// vector is indexed in the same order the put ops appear inside
// `round_ops`; build_sealed_gen walks the ops span in parallel with a
// running `put_idx` counter to pair entries with durables.

// Persist in fixed-size chunks so this harness stays comfortably below
// per-core NVMe queue capacity. M07 bounds production value persist IO,
// but the e2e harness keeps this conservative chunking until real-disk
// coverage replaces the fallback. The durables array is filled
// shard-by-shard through per-entry `out_vr` pointers, so the caller still
// sees one contiguous result.
constexpr std::size_t kPersistChunkPuts = 1024;

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

    std::size_t issued = 0;
    while (issued < entries.size()) {
        const std::size_t remaining  = entries.size() - issued;
        const std::size_t chunk_size = std::min(kPersistChunkPuts, remaining);
        std::span<value::put_entry> chunk(
            entries.data() + issued, chunk_size);
        const bool ok = submit_and_wait<bool>([chunk]() {
            return value::persist_put_values(chunk);
        });
        CHECK(ok && "value::persist_put_values returned ok=false");
        issued += chunk_size;
    }
    return durables;
}

// ── Memtable gen ───────────────────────────────────────────────────────
//
// Mixed put/tombstone entries sharing one gen. value ops reference
// durables[put_index] where put_index is the number of prior put ops
// in the same sorted span. Memtable entries keep only durable
// value_ref; value bytes stay in the Value Area. Tombstones leave
// value_handle empty -- fold preserves them as kind::tombstone
// winners / losers the same way it handles put entries
// (core/memtable.hh kind enum + losers retire list).

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
            entry.k  = core::memtable_entry::kind::value;
            entry.vh = core::value_handle{
                .durable = durables[put_idx++],
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

    round_timing timing{};
    auto         round_t0 = now_tp();

    // 1. Persist every put value; empty-put rounds would skip the call,
    //    but current round generators always include puts so we keep
    //    the CHECK-noisy fall-through.
    auto persist_t0 = now_tp();
    auto durables   = persist_put_values(spec.ops);
    timing.persist_ms = ms_between(persist_t0, now_tp());

    // 2. Build a sealed memtable_gen off the sorted ops span.
    auto build_t0 = now_tp();
    auto gen      = build_sealed_gen(
        spec.gen_id,
        /*front_owner_index=*/ 0,
        spec.lsn_start,
        spec.ops,
        durables);
    timing.build_sealed_ms = ms_between(build_t0, now_tp());
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
    auto flush_t0 = now_tp();
    auto result   = submit_and_wait<tree::tree_flush_result>([req_holder]() {
        return tree::tree_local_flush(std::move(*req_holder));
    });
    timing.flush_ms = ms_between(flush_t0, now_tp());

    // 4. Validate + wrap the new manifest as the next base_guard.
    validate_flush_result(result,
        /*expected_max_lsn=*/             gen_max_lsn,
        /*expected_front_owner_index=*/   0,
        /*expected_gen_id=*/              spec.gen_id,
        round_label);

    auto manifest        = result.new_manifest;
    auto next_base_guard = wrap_manifest_as_guard(manifest);

    timing.total_ms = ms_between(round_t0, now_tp());

    return round_outcome{
        .result          = std::move(result),
        .manifest        = std::move(manifest),
        .next_base_guard = std::move(next_base_guard),
        .next_lsn        = spec.lsn_start + spec.ops.size(),
        .timing          = timing,
    };
}

// ── Final verification ─────────────────────────────────────────────────

struct verify_sample {
    std::string    key;
    expected_entry expected;
};

std::vector<verify_sample>
pick_verify_samples(const expected_state& state,
                    std::size_t           target_samples) {
    std::vector<verify_sample> flat;
    flat.reserve(state.size());
    for (const auto& [k, v] : state) flat.push_back({k, v});

    const std::size_t n = flat.size();
    if (n == 0) return {};

    const std::size_t effective_target =
        std::max<std::size_t>(1, target_samples);
    const std::size_t stride =
        std::max<std::size_t>(1, n / effective_target);

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

// Concurrency bound for pipeline-internal value readback. Sized so the
// pipeline keeps enough reads in flight to saturate the single
// value_alloc_sched + nvme_sched pair on the value core, but not so
// many that the per-core queue fills up. 32 is a common cap in
// codebase examples (RPC pipelining, aisaq beam search); no reason to
// pick a larger number until we have a profile step to tune it.
constexpr uint32_t kReadbackConcurrency = 32;

// (sample index, value_ref) pair used by the concurrent readback
// pipeline so results can be correlated back to `samples` even after
// a parallel `value::read_value` fan-out finishes out of order.
struct indexed_value_ref {
    std::size_t       sample_idx;
    format::value_ref vr;
};

// Concurrent readback for the live-value subset of `samples`. Returns
// a vector aligned to `live_samples`: `readbacks[k]` is the body that
// `live_samples[k].vr` resolves to. Writing to disjoint indices of a
// pre-sized vector matches the `value::persist_put_values` codebase
// pattern (as_stream → concurrent → flat_map → all, with results
// going to caller-owned state) and sidesteps the INC-041 worry about
// `concurrent + reduce` accumulator contracts: the accumulator is
// per-slot, so reduce has no shared state to fight over.
//
// Tombstones / absent entries don't need value I/O and are checked
// synchronously in the caller's lookup-result walk; they never reach
// this function.
std::vector<std::string>
concurrent_readback(std::span<const indexed_value_ref> live_samples) {
    if (live_samples.empty()) return {};

    // Local copy so the pipeline doesn't depend on caller-owned span
    // memory staying alive through every concurrent flat_map branch.
    std::vector<indexed_value_ref> items(
        live_samples.begin(), live_samples.end());
    std::vector<std::string> readbacks(items.size());

    auto ok = submit_and_wait<bool>([&]() {
        using namespace pump::sender;
        return just()
            >> for_each(items)
            >> concurrent(kReadbackConcurrency)
            >> flat_map([&readbacks, &items](const indexed_value_ref& iv) {
                // Resolve the slot from the item's position in `items`
                // — the pipeline might visit items out of order, so we
                // can't use the iteration index alone. Pointer math on
                // the captured vector is stable because `items` lives
                // for the whole submit_and_wait window.
                const std::size_t slot =
                    static_cast<std::size_t>(&iv - items.data());
                return value::read_value(iv.vr)
                    >> then([&readbacks, slot](std::string&& s) {
                        readbacks[slot] = std::move(s);
                        return true;
                    });
            })
            >> all();
    });
    CHECK(ok && "concurrent readback pipeline reported failure");
    return readbacks;
}

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

    // Phase A: check lookup variant / data_ver synchronously and
    // collect live values into a to-read list.
    std::vector<indexed_value_ref> live_samples;
    live_samples.reserve(samples.size());
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
            live_samples.push_back(indexed_value_ref{i, lv.vr});
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

    // Phase B: fan out value::read_value concurrently via a single
    // pipeline, then byte-compare results sequentially. The returned
    // vector is aligned to `live_samples`, so `readbacks[k]` pairs
    // with `live_samples[k]` regardless of the order the pipeline
    // finished individual reads.
    auto readbacks = concurrent_readback(live_samples);
    CHECK(readbacks.size() == live_samples.size());
    for (std::size_t k = 0; k < live_samples.size(); ++k) {
        const auto& iv  = live_samples[k];
        const auto& got = readbacks[k];
        const auto& exp = samples[iv.sample_idx].expected;
        if (got != exp.value) {
            std::fprintf(stderr,
                "FAIL: sample [%zu] key=%s value bytes mismatch: "
                "got (len=%zu) '%.*s', expected (len=%zu) '%s'\n",
                iv.sample_idx, samples[iv.sample_idx].key.c_str(),
                got.size(),
                static_cast<int>(got.size()), got.data(),
                exp.value.size(),
                exp.value.c_str());
            std::abort();
        }
        ++report.value_checks_ok;
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

// Allocator usage snapshot, captured after a round finishes.
//
// `tree_used_lbas` / `value_used_lbas` are reported relative to the
// profile's Value Area base/end so they stay interpretable no matter
// what profile is loaded:
//
//   tree area grows UP    from profile.value_data_area_base → tree_head
//   value area grows DOWN from profile.value_data_area_end  → value_head
//
// The gap between heads is the remaining capacity. Exhaustion ==
// tree_head catching value_head (tree_allocator panics with
// "data area exhausted" when that happens).
struct alloc_snapshot {
    uint64_t tree_head_lba    = 0;
    uint64_t value_head_lba   = 0;
    uint64_t tree_used_lbas   = 0;
    uint64_t value_used_lbas  = 0;
    uint64_t free_lbas        = 0;
};

alloc_snapshot
capture_alloc_snapshot() {
    const auto& profile = format::kBootstrapFormatProfile;
    alloc_snapshot s{};
    if (auto* heads = core::registry::data_area_heads_ptr.get()) {
        s.tree_head_lba  = heads->tree_head_lba.load(std::memory_order_relaxed);
        s.value_head_lba = heads->value_head_lba.load(std::memory_order_relaxed);
    }
    s.tree_used_lbas =
        s.tree_head_lba >= profile.value_data_area_base.lba
            ? s.tree_head_lba - profile.value_data_area_base.lba
            : 0ULL;
    s.value_used_lbas =
        profile.value_data_area_end.lba >= s.value_head_lba
            ? profile.value_data_area_end.lba - s.value_head_lba
            : 0ULL;
    s.free_lbas =
        s.value_head_lba > s.tree_head_lba
            ? s.value_head_lba - s.tree_head_lba
            : 0ULL;
    return s;
}

void
print_round_flush_result(const char*                    label,
                         const tree::tree_flush_result& r,
                         const round_timing&            timing,
                         const alloc_snapshot&          snap) {
    const std::size_t leaf_count =
        r.new_manifest ? r.new_manifest->leaf_order.size() : 0UL;
    const std::size_t slot_map_size =
        r.new_manifest ? r.new_manifest->slot_map.size() : 0UL;
    const unsigned long root_lba =
        r.new_manifest ? static_cast<unsigned long>(
            r.new_manifest->root_slot.lba) : 0UL;

    std::printf("    %s flush: st=%u, leaf_order.size=%zu, "
                "slot_map.size=%zu, root_slot.lba=%lu, "
                "flushed_max_lsn=%lu, flushed_front_count=%zu\n",
                label,
                static_cast<unsigned>(r.st),
                leaf_count,
                slot_map_size,
                root_lba,
                static_cast<unsigned long>(r.flushed_max_lsn),
                r.flushed_gens_by_front.size());
    std::printf("    %s timing: persist=%.1fms build_sealed=%.1fms "
                "flush=%.1fms total=%.1fms\n",
                label,
                timing.persist_ms,
                timing.build_sealed_ms,
                timing.flush_ms,
                timing.total_ms);
    std::printf("    %s alloc: tree_head=%lu (used=%lu LBAs) "
                "value_head=%lu (used=%lu LBAs) free=%lu LBAs\n",
                label,
                static_cast<unsigned long>(snap.tree_head_lba),
                static_cast<unsigned long>(snap.tree_used_lbas),
                static_cast<unsigned long>(snap.value_head_lba),
                static_cast<unsigned long>(snap.value_used_lbas),
                static_cast<unsigned long>(snap.free_lbas));
}

void
print_expected_state_summary(const expected_state& state,
                             uint32_t              rounds) {
    std::size_t value_count = 0;
    std::size_t tomb_count  = 0;
    for (const auto& [k, v] : state) {
        if (v.k == expected_entry::kind::value) ++value_count;
        else                                    ++tomb_count;
    }
    std::printf("  expected state after %u rounds: %zu keys "
                "(%zu value, %zu tombstone)\n",
                rounds, state.size(), value_count, tomb_count);
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

    std::printf("test_flush_e2e: pci=%s num_keys=%u, rounds=%u, "
                "readback_samples=%u, topology cores=[0,2,4,6,8] "
                "rd=[2,4,6] value=%d owner=%d\n",
                opts.pci_addr.c_str(),
                opts.num_keys,
                opts.rounds,
                opts.readback_samples,
                kValueCore, kOwnerCore);

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

    // ── Real NVMe device ──
    auto fmt_opts = derive_format_options();
    nvme::runtime_device dev(nvme::real_device_options{
        .pci_addr = opts.pci_addr.c_str(),
        .cores = kCores,
        .spdk_core_mask = opts.spdk_core_mask.empty()
            ? nullptr
            : opts.spdk_core_mask.c_str(),
        .spdk_name = "inconel_flush_e2e",
        .init_spdk_env = true,
        .qpair_depth = opts.qpair_depth,
        .device_id = 0,
    });
    std::printf("  opened real NVMe: sector=%u namespace=%lu bytes "
                "(profile needs %lu bytes / %lu LBAs @ %u B)\n",
                dev.sector_size(),
                static_cast<unsigned long>(dev.size_bytes()),
                static_cast<unsigned long>(kNamespaceBytes),
                static_cast<unsigned long>(
                    kNamespaceBytes / fmt_opts.lba_size),
                fmt_opts.lba_size);
    format_real_nvme_bootstrap(dev, fmt_opts);
    std::printf("  formatted real NVMe bootstrap superblocks on %s\n",
                opts.pci_addr.c_str());

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

    // ── Publish the initial (empty) snapshot + spawn concurrent readers ──
    //
    // Empty snapshot is published *before* any reader starts so the
    // first load() cannot observe a null shared_ptr. Readers see an
    // empty expected map initially and idle-yield until round 1's
    // publish populates real keys.
    publish_empty_snapshot();

    std::vector<reader_counters> reader_stats(opts.concurrent_readers);
    std::vector<std::jthread>    readers;
    readers.reserve(opts.concurrent_readers);
    for (uint32_t i = 0; i < opts.concurrent_readers; ++i) {
        readers.emplace_back([i, &opts, &reader_stats]() {
            reader_main(i, opts.reader_batch, &reader_stats[i]);
        });
    }
    if (opts.concurrent_readers > 0) {
        std::printf("  started %u reader(s) on cores 10..%u (batch=%u)\n",
                    opts.concurrent_readers,
                    10 + opts.concurrent_readers - 1,
                    opts.reader_batch);
    } else {
        std::printf("  concurrent readers disabled "
                    "(--concurrent-readers 0)\n");
    }

    expected_state state;

    // ── Rounds 1..K ──
    //
    // Rounds 1 and 2 are one-shot bootstrap/mix. Rounds 3..K reuse the
    // round-3 op pattern with escalating gen_id and lsn so the tree
    // accumulates shadow-slot churn on the same key regions — this is
    // the "多轮 cascade 累积效应" surface the plan calls out.
    //
    // Only the final manifest is pinned through the verify step; every
    // round's intermediate guard is released as soon as the next round
    // starts, mirroring how production coord would retire old guards
    // once a new one is published.

    auto                               all_rounds_t0 = now_tp();
    std::shared_ptr<const core::checkpoint_guard>
                                       base_guard    = make_empty_base_guard();
    std::shared_ptr<const core::tree_manifest>
                                       final_manifest;
    uint64_t                           next_lsn        = 1;
    uint64_t                           total_ops_count = 0;

    for (uint32_t round_id = 1; round_id <= opts.rounds; ++round_id) {
        char label[32];
        std::snprintf(label, sizeof(label), "round %u", round_id);

        round_spec spec{
            .gen_id    = round_id,
            .lsn_start = next_lsn,
            .ops       = generate_round_ops(
                round_id, opts.num_keys, key_digits),
        };
        print_round_header(label, spec);

        auto outcome = run_round(spec, base_guard, label);
        apply_round_to_expected(spec, state);
        total_ops_count += spec.ops.size();

        auto snap = capture_alloc_snapshot();
        print_round_flush_result(label, outcome.result, outcome.timing, snap);

        base_guard     = outcome.next_base_guard;
        final_manifest = outcome.manifest;
        next_lsn       = outcome.next_lsn;

        // Publish this round's (manifest, expected, round_id) atomically.
        // Readers observe the swap on their next g_current_snap.load();
        // the previous snapshot stays alive as long as some reader keeps
        // a shared_ptr to it — that's the "previous" slot on their side,
        // which gives the match_all fallback real history to compare
        // against during the benign race window.
        publish_round_snapshot(outcome.manifest, state, round_id);
    }
    const double all_rounds_ms = ms_between(all_rounds_t0, now_tp());

    CHECK(final_manifest
          && "no rounds executed — harness invariant violated");

    // ── Stop readers before the quiesced final verify ──
    //
    // Plan §4.5.5 stop order: readers join → final verify → advance
    // workers stop → runtime destroy. Final verify is a byte-level
    // byte-wise comparison with no active writer, and it must pass
    // unconditionally; readers winding down first guarantees no
    // concurrent publish races against the verify.
    if (opts.concurrent_readers > 0) {
        g_reader_stop.store(true, std::memory_order_relaxed);
        readers.clear();   // jthread dtors join

        for (uint32_t i = 0; i < opts.concurrent_readers; ++i) {
            const auto& c = reader_stats[i];
            std::printf("  reader %u: batches=%lu ok=%lu ok_stale=%lu "
                        "ok_retry=%lu mismatch=%lu\n",
                        i,
                        static_cast<unsigned long>(c.batches),
                        static_cast<unsigned long>(c.ok),
                        static_cast<unsigned long>(c.ok_stale),
                        static_cast<unsigned long>(c.ok_retry),
                        static_cast<unsigned long>(c.mismatch));
        }
    }

    // ── Final verification ──
    print_expected_state_summary(state, opts.rounds);
    auto verify_t0 = now_tp();
    auto samples   = pick_verify_samples(state, opts.readback_samples);
    std::printf("  verify %zu sampled keys against expected state "
                "(first=%s .. last=%s)\n",
                samples.size(),
                samples.front().key.c_str(),
                samples.back().key.c_str());
    auto report = verify_against_expected(
        final_manifest.get(), samples);
    const double verify_ms = ms_between(verify_t0, now_tp());
    std::printf("    readback OK for %zu samples "
                "(%zu value / %zu tombstone), verify wall=%.1fms\n",
                samples.size(),
                report.value_checks_ok,
                report.tombstone_checks_ok,
                verify_ms);

    // ── Throughput summary ──
    const double seconds = all_rounds_ms / 1000.0;
    const double throughput_ops_per_sec =
        seconds > 0.0 ? static_cast<double>(total_ops_count) / seconds : 0.0;
    std::printf("  all %u rounds: total_ops=%lu wall=%.1fms "
                "(%.0f ops/s)\n",
                opts.rounds,
                static_cast<unsigned long>(total_ops_count),
                all_rounds_ms,
                throughput_ops_per_sec);

    // ── Stop runtime ──
    for (uint32_t core : kCores) rt->is_running_by_core[core].store(false);
    workers.clear();   // jthread dtors join
    runtime::destroy_runtime<TreeCache, ValueCache>(rt);

    std::printf("all passed\n");
    return 0;
}
