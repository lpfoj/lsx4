// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "common/logging/backend.h"

#include <cstdio>
#if defined(LSX4_APPLE_RUNTIME)
#include <execinfo.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include <atomic>
#include <android/log.h>
#endif

#if defined(ARCH_X86_64)
#define Crash() __asm__ __volatile__("int $3")
#elif defined(ARCH_ARM64)
#define Crash() __asm__ __volatile__("brk 0")
#else
#error "Missing Crash() implementation for target CPU architecture."
#endif

void assert_fail_impl(const char* expression, const char* file, const int line) {
#ifdef __ANDROID__
    static std::atomic<unsigned> s_budget{0};
    const unsigned n = s_budget.fetch_add(1, std::memory_order_relaxed);
    if (n < 200) {
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Assert",
                            "[EXECUTOR_ASSERT_NONFATAL] n=%u expression=%s file=%s line=%d "
                            "continuing (reference-release parity)",
                            n, expression ? expression : "<unknown>",
                            file ? file : "<unknown>", line);
    }
    std::fflush(stdout);
    return;
#else
#if defined(LSX4_APPLE_RUNTIME)
    // Error path only, before emitting the deliberate host breakpoint. No
    // extra work on a successful assertion or inside the signal handler.
    std::fprintf(stderr, "[APPLE_ASSERT_CONTEXT] expression=%s file=%s line=%d\n",
                 expression, file, line);
    void* frames[32];
    const int count = backtrace(frames, 32);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
#else
    (void)expression;
    (void)file;
    (void)line;
#endif
    Common::Log::Stop();
    std::fflush(stdout);
    Crash();
#endif
}

[[noreturn]] void unreachable_impl() {
    Common::Log::Stop();
    std::fflush(stdout);
    Crash();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_FATAL, "LSX4Assert",
                        "[EXECUTOR_ASSERT_FAIL] %s", msg ? msg : "<null>");
#endif
    std::fprintf(stderr, "[EXECUTOR_ASSERT_FAIL] %s\n", msg ? msg : "<null>");
    std::fflush(stderr);
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl(msg, __FILE__, __LINE__);
}
