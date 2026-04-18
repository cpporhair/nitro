//
// Tree lookup → value read integration test
//
// Verifies the read half of the GET path: a key goes through tree::lookup,
// the returned value_ref goes through value::read_value, and the body bytes
// match what we put on the device.
//
// Both layers are seeded directly via mock_nvme synchronous helpers
// (test_write_raw / do_write) — value::persist_values is intentionally NOT
// used. This isolates the integration to the read path: format encode/decode,
// tree traversal, value cache miss + NVMe read, and value object decoding.
//

#include <csignal>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "apps/inconel/test/check.hh"

#include "pump/sender/any_exception.hh"
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/submit.hh"

#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/shard_partition_builder.hh"
#include "apps/inconel/core/tree_read_domain.hh"
#include "apps/inconel/format/types.hh"
#include "apps/inconel/format/value_object.hh"
#include "apps/inconel/tree/page_builder.hh"
#include "apps/inconel/tree/sender.hh"
#include "apps/inconel/value/scheduler.hh"
#include "apps/inconel/value/sender.hh"

using namespace apps::inconel::format;
using namespace apps::inconel::core;
using namespace apps::inconel::mock_nvme;
using namespace apps::inconel::tree;
namespace value = apps::inconel::value;

namespace {

constexpr uint32_t LBA_SIZE       = 4096;
constexpr uint32_t TREE_PAGE_SIZE = 4096;
constexpr uint64_t TOTAL_LBAS     = 8192;          // 32 MiB device

constexpr uint64_t LEAF_LBA            = 1000;     // tree leaf (root)
constexpr uint64_t VALUE_LBA           = 2000;     // sub-LBA value page
constexpr uint64_t DATA_AREA_BASE_LBA  = 4000;     // value data area lower
constexpr uint64_t DATA_AREA_END_LBA   = 8000;     // bumps from here downward

constexpr uint32_t VALUE_CLASS_SIZE = 256;         // sub-LBA, 16 slots/LBA
const tree_geometry kTreeGeom{
    .lba_size = LBA_SIZE,
    .tree_page_size = TREE_PAGE_SIZE,
    .shadow_slots_per_range = 1,
};

const std::vector<uint32_t> CLASS_SIZES = {
    64, 256, 1024, 4096, 16384,
};

// Lay out N sub-LBA value objects of class_size into one LBA-sized page.
// Identical to test_value.cc's helper of the same name; duplicated to keep
// the test self-contained and avoid cross-test header pulls.
void build_subLba_page_image(std::vector<char>& page,
                              uint32_t class_size,
                              const std::vector<std::string>& bodies) {
    page.assign(LBA_SIZE, char{0});
    uint32_t slots = LBA_SIZE / class_size;
    CHECK(bodies.size() <= slots);
    for (size_t i = 0; i < bodies.size(); ++i) {
        std::span<char> slot(page.data() + i * class_size, class_size);
        std::span<const char> body(bodies[i].data(), bodies[i].size());
        bool ok = encode_value_object(slot, body);
        CHECK(ok);
    }
}

// ── Test environment ──
//
// Single-leaf root tree (LEAF_LBA), one value page (VALUE_LBA), and three
// records {key_i → value_ref_i} pointing into successive sub-LBA slots of
// the value page.
//
// All schedulers run on this_core_id == 0; manual advance loop drives them
// (no share_nothing worker thread, so no leak surface from worker shutdown).

struct test_env {
    mock_device                                 dev;
    scheduler                                   nvme_sched;
    leaf_order_index                            leaf_order;
    std::shared_ptr<const shard_partition_map>  partitions;
    tree_read_domain<clock_cache>               read_domain;
    value::value_alloc_sched<clock_cache>       value_sched;
    tree_manifest                               manifest;

    std::vector<std::string> keys;
    std::vector<std::string> bodies;
    std::vector<value_ref>   refs;

    explicit test_env(uint32_t value_cache_cap = 32)
        : dev(LBA_SIZE * TOTAL_LBAS, LBA_SIZE)
        , nvme_sched(&dev)
        , leaf_order(build_single_leaf_order())
        , partitions(std::make_shared<const shard_partition_map>(
              build_initial_shard_partition_map(leaf_order, 1)))
        , read_domain(/*rdi=*/0, partitions, clock_cache(32), &kTreeGeom)
        , value_sched(std::span<const uint32_t>(CLASS_SIZES),
                      LBA_SIZE,
                      paddr{0, DATA_AREA_BASE_LBA},
                      paddr{0, DATA_AREA_END_LBA},
                      clock_cache(value_cache_cap))
    {
        pump::core::this_core_id = 0;

        registry::clear();
        registry::init_capacity(8);
        registry::install_shard_partitions(partitions);
        registry::nvme_scheds.list.push_back(&nvme_sched);
        registry::nvme_scheds.by_core[0] = &nvme_sched;
        registry::tree_read_domains.list.push_back(&read_domain);
        registry::tree_read_domains.by_core[0] = &read_domain;
        registry::value_alloc_sched = &value_sched;

        seed_value_page();
        seed_leaf_page();
        configure_manifest();
    }

    static leaf_order_index build_single_leaf_order() {
        leaf_order_index idx;
        idx.fence_pool = "";
        idx.spans.push_back({
            .fence_lower_off = 0,
            .fence_upper_off = 0,
            .fence_lower_len = 0,
            .fence_upper_len = 0,
            .leaf_range_base = paddr{0, LEAF_LBA},
        });
        return idx;
    }

    ~test_env() {
        registry::clear();
    }

private:
    void seed_value_page() {
        bodies = {
            "alpha-body",
            "bravo-mid-length",
            "charlie-quite-a-bit-longer-than-the-others-XYZ",
        };

        std::vector<char> page;
        build_subLba_page_image(page, VALUE_CLASS_SIZE, bodies);

        void* dst = dev.test_write_raw(VALUE_LBA);
        std::memcpy(dst, page.data(), LBA_SIZE);

        refs.clear();
        for (size_t i = 0; i < bodies.size(); ++i) {
            refs.push_back(value_ref{
                .base        = paddr{0, VALUE_LBA},
                .byte_offset = static_cast<uint16_t>(i * VALUE_CLASS_SIZE),
                .len         = static_cast<uint32_t>(bodies[i].size()),
                .flags       = 0,
            });
        }
    }

    void seed_leaf_page() {
        keys = { "key-alpha", "key-bravo", "key-charlie" };

        alignas(64) char page[TREE_PAGE_SIZE];
        leaf_page_builder b;
        b.init(page, TREE_PAGE_SIZE);
        for (size_t i = 0; i < keys.size(); ++i) {
            bool ok = b.add_value(keys[i],
                                  /*data_ver*/ static_cast<uint64_t>(100 + i),
                                  refs[i]);
            CHECK(ok);
        }
        b.finalize();
        bool wrote = dev.do_write(LEAF_LBA, page, 1);
        CHECK(wrote);
    }

    void configure_manifest() {
        manifest.geom           = &kTreeGeom;
        manifest.root_slot      = paddr{0, LEAF_LBA};
        manifest.slot_map[paddr{0, LEAF_LBA}] = 0;
        // Single-leaf-root tree: leaf_order is the same single-span
        // index used when constructing the shard_partition_map above.
        manifest.leaf_order = leaf_order;
    }
};

// drive all schedulers manually until `done` flips to true
void advance_until(test_env& env, bool& done, int max_iters = 500) {
    for (int i = 0; i < max_iters && !done; ++i) {
        env.read_domain.advance();   // drives lookup + worker arms
        env.nvme_sched.advance();
        env.value_sched.advance();
    }
    CHECK(done);
}

// ════════════════════════════════════════════════════════════════
//  case_1: single pipeline
//
//  just()
//    >> tree::lookup
//    >> flat_map(stream lookup_results
//                  >> flat_map(value::read_value)
//                  >> reduce into vector<string>)
//    >> then(verify bodies)
//    >> submit
//
//  This is the integration we actually want to test: a single submit
//  exercises tree → value scheduler hand-off through PUMP pipeline
//  composition, not two independent submits glued by test code.
// ════════════════════════════════════════════════════════════════

void case_1_lookup_then_read() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    std::vector<std::string_view> key_views;
    for (auto& k : env.keys) key_views.push_back(k);

    namespace ps = pump::sender;

    bool done = false;
    std::vector<std::string> got;

    ps::just()
        >> lookup(key_views, &env.manifest)
        >> ps::flat_map([sched = &env.value_sched]
                        (std::vector<lookup_result>&& results) {
            return ps::just()
                >> ps::as_stream(__mov__(results))
                >> ps::flat_map([sched](auto&& r) {
                    // case_1 setup guarantees every key hits a value record;
                    // tombstone/absent are exercised separately in case_2.
                    auto& lv = std::get<lookup_value>(r);
                    return value::read_value(lv.vr);
                })
                >> ps::reduce(
                    std::vector<std::string>{},
                    [](std::vector<std::string>& acc, auto&& v) {
                        using V = std::remove_cvref_t<decltype(v)>;
                        if constexpr (std::is_same_v<V, std::string>) {
                            acc.push_back(std::forward<decltype(v)>(v));
                        }
                        // monostate / exception_ptr — case_1 setup never
                        // produces these, so silently drop. case_2 covers
                        // the absent path with an isolated test.
                    });
        })
        >> ps::then([&](std::vector<std::string>&& bodies) {
            got = std::move(bodies);
            done = true;
        })
        >> ps::submit(ctx);

    advance_until(env, done);

    CHECK(got.size() == env.bodies.size());
    for (size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i] == env.bodies[i]);
    }

    printf("  case_1_lookup_then_read: OK (%zu keys via single pipeline)\n",
           env.keys.size());
}

// ════════════════════════════════════════════════════════════════
//  case_2: missing key → lookup_absent, no value read attempted
// ════════════════════════════════════════════════════════════════

void case_2_missing_key() {
    test_env env;
    auto ctx = pump::core::make_root_context();

    std::vector<std::string_view> key_views = { "key-nope" };
    std::vector<lookup_result> looked_up;
    bool tree_done = false;
    pump::sender::just()
        >> lookup(key_views, &env.manifest)
        >> pump::sender::then([&](std::vector<lookup_result>&& r) {
            looked_up = std::move(r);
            tree_done = true;
        })
        >> pump::sender::submit(ctx);

    advance_until(env, tree_done);

    CHECK(looked_up.size() == 1);
    CHECK(std::holds_alternative<lookup_absent>(looked_up[0]));

    printf("  case_2_missing_key: OK (lookup_absent for unknown key)\n");
}

// ════════════════════════════════════════════════════════════════
//  case_3: corrupt value page → process MUST panic (death test)
//
//  Step 009 hardening: decode corruption is no longer surfaced as a
//  recoverable exception. value::value_alloc_sched::handle_fill calls
//  core::panic_inconsistency() *before* it touches readonly_cache_,
//  so the only observable evidence is the SIGABRT it raises and the
//  diagnostic line it writes to stderr just before aborting.
//
//  We can't observe a panic from inside the same process, so the
//  read runs in a forked child whose stderr is redirected into a
//  pipe. The parent waits for the child, then asserts:
//
//    1. The child died via SIGABRT  (WIFSIGNALED + WTERMSIG)
//    2. The captured stderr contains the panic-site identifiers
//       ("inconel panic" / "corrupt value object" / "source=post_nvme"
//       / "status=bad_magic") so we know the right path fired and
//       not some unrelated abort (e.g. CHECK failure).
//
//  Cache-poisoning is no longer observable in this test: handle_fill
//  panics before any readonly_cache_.put(), so it is structurally
//  impossible for the corrupt page to enter the cache. The old
//  "second read also misses" assertion is dead under the new design
//  and has been removed.
// ════════════════════════════════════════════════════════════════

void case_3_corrupt_value_page() {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        std::perror("case_3: pipe");
        std::abort();
    }

    pid_t pid = fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        // ── Child ──
        // Redirect stderr into the pipe so the parent can read the
        // panic diagnostic. Close the read end; close the write end's
        // original FD after dup2 so EOF reaches the parent the moment
        // the child exits.
        close(pipefd[0]);
        if (dup2(pipefd[1], STDERR_FILENO) < 0) _exit(127);
        close(pipefd[1]);

        test_env env;

        // Smash the magic bytes of slot 0 (env.refs[0]'s slot).
        // env.refs[0].byte_offset == 0; value_object_header.magic
        // occupies the first 4 bytes of that slot.
        void* dst  = env.dev.test_write_raw(VALUE_LBA);
        char* page = static_cast<char*>(dst);
        page[0] = 0; page[1] = 0; page[2] = 0; page[3] = 0;

        auto ctx = pump::core::make_root_context();
        namespace ps = pump::sender;
        bool done = false;

        // Drive the read. handle_fill will panic during the second
        // advance pass, before this `then` ever runs.
        value::read_value(env.refs[0])
            >> ps::then([&done](auto&&) { done = true; })
            >> ps::submit(ctx);

        advance_until(env, done);

        // Reaching here means the panic did not fire — that itself
        // is the regression we are testing for. Exit with a normal
        // status so the parent's WIFSIGNALED / WTERMSIG checks fail
        // loudly instead of silently passing.
        _exit(0);
    }

    // ── Parent ──
    close(pipefd[1]);

    char   stderr_buf[4096] = {};
    size_t total = 0;
    while (total + 1 < sizeof(stderr_buf)) {
        ssize_t n = read(pipefd[0], stderr_buf + total,
                         sizeof(stderr_buf) - 1 - total);
        if (n <= 0) break;
        total += static_cast<size_t>(n);
    }
    close(pipefd[0]);

    int   status = 0;
    pid_t w      = waitpid(pid, &status, 0);
    CHECK(w == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);

    std::string captured(stderr_buf, total);
    CHECK(captured.find("inconel panic")        != std::string::npos);
    CHECK(captured.find("corrupt value object") != std::string::npos);
    CHECK(captured.find("source=post_nvme")     != std::string::npos);
    CHECK(captured.find("status=bad_magic")     != std::string::npos);

    printf("  case_3_corrupt_value_page: OK "
           "(child SIGABRT on bad_magic via post_nvme, %zu bytes captured)\n",
           total);
}

// ════════════════════════════════════════════════════════════════
//  case_4: NVMe read failure → read_value must throw, cache stays clean
//
//  Procedure:
//   1. Construct an out-of-range value_ref pointing past total_lbas.
//      mock_device::do_read (device.hh:71) returns false when
//      lba + num_lbas > total_lbas, and mock_nvme::scheduler::advance
//      (scheduler.hh:110) propagates that false straight to the cb,
//      so read_value's false_to_exception branch fires.
//   2. The exception message must mention "NVMe read failed" — that
//      proves we landed in the false_to_exception branch and not, say,
//      the corruption branch from case_3.
//   3. A second attempt at the same vr must ALSO go to NVMe (read
//      counter increments). If the failed first read had somehow
//      polluted readonly_cache_, the second attempt would hit cache
//      and we'd never re-issue NVMe.
// ════════════════════════════════════════════════════════════════

void case_4_nvme_read_failure() {
    test_env env;
    auto ctx = pump::core::make_root_context();
    namespace ps = pump::sender;

    // Way past the device end. value::value_alloc_sched::handle_read computes
    // span_lbas from vr.len (any sane class is fine) then constructs
    // a read_miss whose pipeline issues nvme.read at this bogus LBA.
    value_ref bogus{
        .base        = paddr{0, TOTAL_LBAS + 100},
        .byte_offset = 0,
        .len         = 64,
        .flags       = 0,
    };

    auto run_one_read = [&](std::string& err_out, bool& done_out) {
        value::read_value(bogus)
            >> ps::any_exception([&err_out](std::exception_ptr ep) {
                try { std::rethrow_exception(ep); }
                catch (const std::exception& e) { err_out = e.what(); }
                catch (...) { err_out = "<non-std exception>"; }
                return ps::just(std::string{"<failed>"});
            })
            >> ps::then([&done_out](auto&&) { done_out = true; })
            >> ps::submit(ctx);
    };

    env.dev.reset_io_counters();

    // ── First read: must throw with NVMe error ──
    std::string err1;
    bool done1 = false;
    run_one_read(err1, done1);
    advance_until(env, done1);
    CHECK(!err1.empty());
    CHECK(err1.find("NVMe read failed") != std::string::npos);
    auto reads_after_1 = env.dev.get_read_count();
    // Note: do_read returns false BEFORE incrementing read_count_ (it
    // bails out at the bounds check), so the counter actually stays at
    // 0 here. We instead prove the *attempt* happened by the exception
    // existence, and prove cache wasn't populated by re-running below.

    // ── Second read: must ALSO throw (cache must NOT have a stale entry) ──
    std::string err2;
    bool done2 = false;
    run_one_read(err2, done2);
    advance_until(env, done2);
    CHECK(!err2.empty());
    CHECK(err2.find("NVMe read failed") != std::string::npos);

    printf("  case_4_nvme_read_failure: OK (caught \"%s\", "
           "device read counter stayed at %lu, cache clean across 2 attempts)\n",
           err1.c_str(),
           static_cast<unsigned long>(reads_after_1));
}

// ════════════════════════════════════════════════════════════════
//  case_5: cache eviction isolation
//
//  Builds a fresh test_env with value_cache_capacity=2, then seeds 5
//  distinct 1-LBA value pages directly via test_write_raw and reads them
//  twice through value::read_value:
//
//    Round 1 (cold cache): all 5 reads must miss → exactly 5 NVMe reads.
//    Round 2 (after fill): with cap=2 the cache holds at most 2 of the 5
//                          entries, so re-reading all 5 must miss at least
//                          N - cap = 3 of them. An unbounded cache would
//                          add 0 reads in round 2, so this lower bound is
//                          a falsifier for "cache is unbounded".
//
//  Round 2 also exercises the cache.put → evict → delete[] path repeatedly,
//  giving step 9's manual valgrind run something to check. The test_env
//  destructor at the end of the function then drives
//  value::value_alloc_sched<Cache>::~value_alloc_sched → readonly_cache_.evict_one() drain,
//  freeing the two surviving cached buffers. ASAN is not run on this test
//  binary (share_nothing teardown bug, see feedback_share_nothing_no_drain),
//  so leak verification is left to that valgrind pass.
//
//  case_5 deliberately bypasses the tree lookup path: cache behavior is
//  independent of how the value_ref was obtained, and the case_2/case_3
//  flow already covers tree → value handoff. Keeping this test focused
//  on cache eviction makes the failure mode (round-2 read count) easy to
//  attribute.
// ════════════════════════════════════════════════════════════════

void case_5_cache_eviction_isolation() {
    test_env env(/*value_cache_cap*/ 2);

    constexpr int      N         = 5;
    constexpr uint64_t SEED_BASE = 5000;

    std::vector<std::string> body_strs;
    std::vector<value_ref>   vrs;
    body_strs.reserve(N);
    vrs.reserve(N);
    for (int i = 0; i < N; ++i) {
        std::string body = "evict-isolation-" + std::to_string(i);
        std::vector<char> page;
        build_subLba_page_image(page, VALUE_CLASS_SIZE, {body});
        void* dst = env.dev.test_write_raw(SEED_BASE + i);
        std::memcpy(dst, page.data(), LBA_SIZE);

        body_strs.push_back(std::move(body));
        vrs.push_back(value_ref{
            .base        = paddr{0, SEED_BASE + i},
            .byte_offset = 0,
            .len         = static_cast<uint32_t>(body_strs.back().size()),
            .flags       = 0,
        });
    }

    env.dev.reset_io_counters();

    namespace ps = pump::sender;
    auto ctx = pump::core::make_root_context();

    auto read_one = [&](value_ref vr, std::string& got, bool& done) {
        value::read_value(vr)
            >> ps::then([&got, &done](std::string s) {
                got  = std::move(s);
                done = true;
            })
            >> ps::submit(ctx);
    };

    // Round 1: cold cache. Each fresh paddr → NVMe miss → fill_and_decode
    // admits the page into the cache (1-LBA, span=1, admit=true).
    for (int i = 0; i < N; ++i) {
        std::string got;
        bool        done = false;
        read_one(vrs[i], got, done);
        advance_until(env, done);
        CHECK(got == body_strs[i]);
    }
    auto reads_round1 = env.dev.get_read_count();
    CHECK(reads_round1 == N);

    // Round 2: cap=2 means at most 2 of the 5 entries survive in the cache,
    // so the other ≥3 reads must hit NVMe again.
    for (int i = 0; i < N; ++i) {
        std::string got;
        bool        done = false;
        read_one(vrs[i], got, done);
        advance_until(env, done);
        CHECK(got == body_strs[i]);
    }
    auto reads_round2 = env.dev.get_read_count();
    auto round2_misses = reads_round2 - reads_round1;
    CHECK(round2_misses >= 3);
    CHECK(round2_misses <= N);

    printf("  case_5_cache_eviction_isolation: OK "
           "(round1=%lu reads, round2=+%lu misses, cap=2 forces ≥3)\n",
           static_cast<unsigned long>(reads_round1),
           static_cast<unsigned long>(round2_misses));
}

}  // namespace

int main() {
    printf("inconel tree lookup + value read integration test:\n");
    case_1_lookup_then_read();
    case_2_missing_key();
    case_3_corrupt_value_page();
    case_4_nvme_read_failure();
    case_5_cache_eviction_isolation();
    printf("all passed\n");
    return 0;
}
