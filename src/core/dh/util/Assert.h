#pragma once

// Assertion macros for dh_core. The core is compiled without exceptions, so a
// failed invariant is reported through a process-wide handler instead of a
// throw. The default handler prints to stderr and aborts; tests install their
// own handler to observe failures without dying.
//
//   DH_ASSERT(cond, "msg")        always compiled
//   DH_DEBUG_ASSERT(cond, "msg")  compiled only when DH_DEBUG is defined
//
// A handler is allowed to return (test doubles do), so code after a failed
// DH_ASSERT must still be memory-safe or unreachable in production builds where
// the default handler aborts.

#include <cstdio>
#include <cstdlib>

namespace dh {

/// `expr` is the stringified condition, `msg` the caller's message (never null).
using AssertHandler = void (*)(const char* expr, const char* msg, const char* file, int line);

namespace detail {

inline void defaultAssertHandler(const char* expr, const char* msg, const char* file, int line) {
    std::fprintf(stderr, "DH_ASSERT failed: %s\n  %s\n  at %s:%d\n", expr, msg, file, line);
    std::fflush(stderr);
    std::abort();
}

inline AssertHandler& assertHandlerSlot() {
    static AssertHandler handler = &defaultAssertHandler;
    return handler;
}

} // namespace detail

/// Installs a new handler and returns the previous one. Passing nullptr restores the default.
inline AssertHandler setAssertHandler(AssertHandler handler) {
    AssertHandler& slot = detail::assertHandlerSlot();
    AssertHandler  old  = slot;
    slot                = handler ? handler : &detail::defaultAssertHandler;
    return old;
}

inline AssertHandler assertHandler() { return detail::assertHandlerSlot(); }

/// Invoked by the macros; may also be called directly for unconditional failures.
inline void assertFail(const char* expr, const char* msg, const char* file, int line) {
    detail::assertHandlerSlot()(expr, msg ? msg : "", file, line);
}

} // namespace dh

#define DH_ASSERT(cond, msg)                                                                                           \
    do {                                                                                                               \
        if (!(cond)) ::dh::assertFail(#cond, (msg), __FILE__, __LINE__);                                               \
    } while (0)

/// Unconditional failure; use for "unreachable" branches.
#define DH_FAIL(msg) ::dh::assertFail("DH_FAIL", (msg), __FILE__, __LINE__)

#ifdef DH_DEBUG
#define DH_DEBUG_ASSERT(cond, msg) DH_ASSERT(cond, msg)
#else
#define DH_DEBUG_ASSERT(cond, msg)                                                                                     \
    do {                                                                                                               \
    } while (0)
#endif
