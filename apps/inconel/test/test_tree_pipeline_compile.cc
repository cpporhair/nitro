//
// Compile-only instantiation test for step 031's tree pipeline.
//
// `tree_local_flush` has no production caller outside of its own
// header, so the whole coroutine + seam + outer-pipeline chain has
// never forced the C++ front end through template instantiation.
// Header-only compilation can succeed even when the full composition
// hits a type-mismatch the moment something actually calls
// `>> submit(ctx)` on it (connect<ctx_t>() is what walks the op_tuple
// and instantiates every op_pusher specialization).
//
// This TU's only job is to drive the compiler through that
// instantiation for both top-level public entry points:
//   - `tree::tree_local_flush(req)` — full flush pipeline
//     (fold → worker fanout → merge loop → device flush → finalize
//      merge → optional superblock update → finalize flush round).
//     The owner scheduler is now resolved internally via `rt::owner()`,
//     so this entry point only takes the request payload.
//   - `tree::lookup(keys, manifest)` — key-range-routed point read
//
// The function is called from `main()` under an always-false guard
// so the body is code-generated (thus template-instantiated) but
// never executed at runtime. That's deliberate — we don't want to
// bring up the full runtime here; we only want the compile barrier.
// A proper end-to-end test belongs in `test_runtime.cc` family.
//

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "pump/core/context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/tree/sender.hh"

namespace {

using namespace apps::inconel;

// Forces the entire `tree_local_flush` sender chain through
// connect<ctx_t>() / op_pusher instantiation. The function is
// guarded to never run (argc < 0), so the compile-time barrier is
// the only thing that matters — `rt::owner()` does not need a real
// singleton installed during compile.
void
compile_check_tree_local_flush() {
    tree::tree_flush_request req{};

    auto ctx = pump::core::make_root_context();
    tree::tree_local_flush(std::move(req))
        >> pump::sender::then([](auto&&) {})
        >> pump::sender::submit(ctx);
}

// Same idea for the point-read path. `lookup` is a bind_back
// operator (needs `just() >>` prefix) that composes per-shard
// fan-out + decision loop + frame cache submission.
void
compile_check_tree_lookup() {
    std::vector<std::string_view> keys;
    const core::tree_manifest* manifest = nullptr;

    auto ctx = pump::core::make_root_context();
    pump::sender::just()
        >> tree::lookup(keys, manifest)
        >> pump::sender::then([](auto&&) {})
        >> pump::sender::submit(ctx);
}

}  // namespace

int
main(int argc, char** /*argv*/) {
    // Never-taken branch. The compiler still type-checks and
    // code-generates the functions above because they are ODR-used
    // here; the runtime branch is dead.
    if (argc < 0) {
        compile_check_tree_local_flush();
        compile_check_tree_lookup();
    }
    std::printf("inconel_test_tree_pipeline_compile: OK\n");
    return 0;
}
