#ifndef APPS_INCONEL_TEST_CHECK_HH
#define APPS_INCONEL_TEST_CHECK_HH

#include <cstdio>
#include <cstdlib>

// CHECK: hard runtime check that survives -DNDEBUG.
//
// Tests must NOT use assert() because Release builds (-O3 -DNDEBUG) compile
// assert() into nothing — that silently disables every condition the test
// relies on. A test that "passes" in Release with assert() is meaningless:
// it only proves the pipeline didn't crash, not that it produced the right
// answer.
//
// CHECK calls abort() unconditionally on failure regardless of build type.

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s\n  at %s:%d\n",           \
                         #cond, __FILE__, __LINE__);                         \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

#endif //APPS_INCONEL_TEST_CHECK_HH
