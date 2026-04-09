#ifndef APPS_INCONEL_CORE_PANIC_HH
#define APPS_INCONEL_CORE_PANIC_HH

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace apps::inconel::core {

    // ── inconel panic ──
    //
    // Single fail-fast entry point for any detected on-disk corruption or
    // runtime invariant break (e.g. tree page bad CRC, value object decode
    // failure, manifest miss, unknown round id). Continuing past such a
    // detection has no semantic value — by definition the scheduler no
    // longer knows what state it is in — so we abort hard with a single
    // diagnostic line.
    //
    // No exception wrapping, no custom error hierarchy, no return path.
    // Implementation depends only on stdio + abort so it stays usable from
    // every Inconel layer (format helpers, cache, scheduler) without
    // pulling in extra runtime dependencies.

    [[noreturn]] inline void
    panic_inconsistency(const char* site, const char* fmt, ...) {
        std::fprintf(stderr, "inconel panic: %s: ", site);
        std::va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stderr, fmt, ap);
        va_end(ap);
        std::fputc('\n', stderr);
        // Flush before abort: stderr is at most line-buffered, but
        // POSIX leaves the stream state at abort() unspecified, so any
        // consumer that captures stderr via a pipe (death tests,
        // structured log forwarders) needs an explicit flush to see
        // the diagnostic before SIGABRT delivery.
        std::fflush(stderr);
        std::abort();
    }

}

#endif //APPS_INCONEL_CORE_PANIC_HH
