// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/assert.h"
#include "common/decoder.h"
#include "common/signal_context.h"
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <array>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#if defined(LSX4_APPLE_FEX)
#include <atomic>
#include <unistd.h>
extern "C" bool executor_lsx4_apple_fex_precheck_memory_owner(int, const siginfo_t*);
extern "C" void executor_lsx4_apple_fex_normalize_signal(int*, siginfo_t*, void*);
#endif
#ifdef __ANDROID__
#include <android/log.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#include <unwind.h>
#endif
#include <atomic>
#include <chrono>
#include <thread>
#endif
#ifdef ARCH_X86_64
#include "common/x86_decoder.h"
#endif
#endif

#ifndef _WIN32
namespace Libraries::Kernel {
void SigactionHandler(int native_signum, siginfo_t* inf, ucontext_t* raw_context);
}
#endif

#if defined(LSX4_APPLE_RUNTIME)
extern "C" void executor_lsx4_apple_note_unhandled_signal(
    int signal, const siginfo_t* info, const void* rawContext) noexcept;
extern "C" void executor_lsx4_apple_note_guest_signal(
    int signal, const siginfo_t* info, const void* rawContext) noexcept;
extern "C" int executor_jit_has_synchronous_guest_fault_frame(const void*);
extern "C" int executor_jit_defer_synchronous_guest_fault(
    s32,s32,s32,s32,u32,u64,s32,const void*);
#endif

namespace Core {

#if defined(_WIN32)

static LONG WINAPI SignalHandler(EXCEPTION_POINTERS* pExp) noexcept {
    const auto* signals = Signals::Instance();

    bool handled = false;
    switch (pExp->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        handled = signals->DispatchAccessViolation(
            pExp, reinterpret_cast<void*>(pExp->ExceptionRecord->ExceptionInformation[1]));
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        handled = signals->DispatchIllegalInstruction(pExp);
        break;
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        return EXCEPTION_CONTINUE_EXECUTION;
    default:
        break;
    }

    return handled ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

bool HasSleepSignalHandler() {
    return true;
}

#else

static void FillFexSynchronousSignalMask(sigset_t* mask) {
    sigemptyset(mask);
    // Never add hardware-generated synchronous faults here. POSIX leaves the
    // result undefined if SIGSEGV/SIGBUS/SIGILL/SIGFPE is generated while
    // blocked.  In particular, a SIGBUS emulator may itself hit SIGSEGV at a
    // page boundary.  The asynchronous wake uses a persistent queue, so one
    // coalesced pending wake is sufficient after synchronous recovery.
    sigaddset(mask, SIGURG);
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_FEX)
static struct sigaction g_previous_sigsegv {};
static struct sigaction g_previous_sigbus {};
static struct sigaction g_previous_sigill {};
static struct sigaction g_previous_sigtrap {};
static struct sigaction g_previous_sigurg {};
static struct sigaction g_previous_sigvtalrm {};
static bool g_fault_signal_handlers_installed = false;
static bool g_jit_signal_handlers_installed = false;
static bool g_fex_signal_handlers_installed = false;

enum class FexSignalPreparePhase : int {
    Idle = 0,
    Preparing = 1,
    Prepared = 2,
};

constexpr std::array<int, 5> kFexOwnedSignals{
    SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGURG,
};
static std::atomic<FexSignalPreparePhase> g_fex_signal_prepare_phase{
    FexSignalPreparePhase::Idle};
static std::array<struct sigaction, kFexOwnedSignals.size()> g_fex_prepare_previous{};
static std::array<std::atomic<bool>, kFexOwnedSignals.size()> g_fex_prepare_snapshot_ready{};
static std::array<std::atomic<bool>, kFexOwnedSignals.size()> g_fex_prepare_installed{};

extern "C" int executor_lsx4_android_runtime_jit_active() __attribute__((weak));
extern "C" int executor_lsx4_android_fault_signal_owner() __attribute__((weak));
extern "C" int executor_lsx4_android_fex_signal_prepare_authorized() __attribute__((weak));
extern "C" int executor_lsx4_android_fex_signal_api_active() __attribute__((weak));
extern "C" int executor_lsx4_android_fex_handle_host_signal(
    int signum, const void* signal_info, void* raw_context) __attribute__((weak));
extern "C" int executor_jit_has_synchronous_guest_fault_frame(
    const void* raw_context) __attribute__((weak));
extern "C" int executor_jit_defer_synchronous_guest_fault(
    s32 native_sig, s32 si_code, s32 si_errno, s32 source_pid, u32 source_uid, u64 fault_addr,
    s32 is_write, const void* raw_context) __attribute__((weak));
extern "C" u64 executor_jit_current_fault_guest_rip() __attribute__((weak));
extern "C" u64 executor_jit_current_fault_active_block_rip() __attribute__((weak));
extern "C" u64 executor_jit_current_fault_instruction() __attribute__((weak));
extern "C" u32 executor_jit_current_fault_instruction_meta() __attribute__((weak));
extern "C" u64 executor_jit_current_fault_gpr(u32 index) __attribute__((weak));
extern "C" u32 executor_jit_current_fault_recent_rips(u64* out, u32 capacity)
    __attribute__((weak));
extern "C" u32 executor_jit_current_fault_active_block_bytes(u8* out, u32 capacity)
    __attribute__((weak));
extern "C" u32 executor_jit_current_fault_recent_block(u32 newest_index, u64* rip_out,
                                                               u8* bytes_out, u32 capacity)
    __attribute__((weak));
extern "C" u32 executor_jit_debug_signal_fault_sidecar(const void* raw_context,
                                                          u64* out, u32 capacity)
    __attribute__((weak));

static bool AndroidJitActive() {
    return executor_lsx4_android_runtime_jit_active != nullptr &&
           executor_lsx4_android_runtime_jit_active() != 0;
}

static int AndroidFaultSignalOwner() {
    return executor_lsx4_android_fault_signal_owner != nullptr
               ? executor_lsx4_android_fault_signal_owner()
               : 0;
}

static bool AndroidCurrentOwnsFaultSignals() {
    if (executor_lsx4_android_fault_signal_owner != nullptr) {
        return AndroidFaultSignalOwner() == 1;
    }
    const int owner = AndroidFaultSignalOwner();
    return owner == 1 ||
           (AndroidJitActive() &&
            std::getenv("EXECUTOR_FEX_OWNS_FAULT_SIGNALS") == nullptr);
}

static bool AndroidFexOwnsFaultSignals() {
    return executor_lsx4_android_fault_signal_owner != nullptr &&
           executor_lsx4_android_fex_signal_api_active != nullptr &&
           AndroidFaultSignalOwner() == 2 &&
           executor_lsx4_android_fex_signal_api_active() != 0;
}

static bool TryHandleFexHostSignal(int sig, siginfo_t* info,
                                   void* raw_context) {
    if (!AndroidFexOwnsFaultSignals() ||
        executor_lsx4_android_fex_handle_host_signal == nullptr) {
        return false;
    }
    return executor_lsx4_android_fex_handle_host_signal(
               sig, info, raw_context) == 1;
}

static bool JitSynchronousGuestFaultCanResume(const void* raw_context) {
    return executor_jit_has_synchronous_guest_fault_frame != nullptr &&
           executor_jit_has_synchronous_guest_fault_frame(raw_context) != 0;
}

static bool DeferJitSynchronousGuestFault(int sig, const siginfo_t* info,
                                         bool is_write,
                                         const void* raw_context) {
    if (executor_jit_defer_synchronous_guest_fault == nullptr) {
        return false;
    }
    return executor_jit_defer_synchronous_guest_fault(
               sig, info ? info->si_code : 0, info ? info->si_errno : 0,
               info ? info->si_pid : 0, info ? info->si_uid : 0,
               reinterpret_cast<u64>(info ? info->si_addr : nullptr),
               is_write ? 1 : 0, raw_context) != 0;
}

static struct sigaction& PreviousActionForSignal(int sig) {
    switch (sig) {
    case SIGSEGV:
        return g_previous_sigsegv;
    case SIGBUS:
        return g_previous_sigbus;
    case SIGILL:
        return g_previous_sigill;
    case SIGTRAP:
        return g_previous_sigtrap;
    case SIGURG:
        return g_previous_sigurg;
    default:
        return g_previous_sigsegv;
    }
}

static std::size_t FexSignalIndex(const int sig) {
    for (std::size_t index = 0; index < kFexOwnedSignals.size(); ++index) {
        if (kFexOwnedSignals[index] == sig) {
            return index;
        }
    }
    return kFexOwnedSignals.size();
}

static bool IsSignalHandlerAction(const struct sigaction& action) {
    return (action.sa_flags & SA_SIGINFO) != 0 && action.sa_sigaction == SignalHandler;
}

bool HasSleepSignalHandler() {
    struct sigaction action{};
    return sigaction(SIGSLEEP, nullptr, &action) == 0 && IsSignalHandlerAction(action);
}

static bool IsPreparedFexAction(const struct sigaction& action) {
    constexpr int required_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    return IsSignalHandlerAction(action) &&
           (action.sa_flags & required_flags) == required_flags &&
           sigismember(&action.sa_mask, SIGURG) == 1;
}

static bool InvokePreparedPreviousAction(const int sig, siginfo_t* const info,
                                         void* const raw_context) {
    if (AndroidFexOwnsFaultSignals()) {
        return false;
    }
    if (g_fex_signal_prepare_phase.load(std::memory_order_acquire) ==
        FexSignalPreparePhase::Idle) {
        return false;
    }
    const std::size_t index = FexSignalIndex(sig);
    if (index == kFexOwnedSignals.size() ||
        !g_fex_prepare_snapshot_ready[index].load(std::memory_order_acquire)) {
        return false;
    }

    const struct sigaction& previous = g_fex_prepare_previous[index];
    if (IsSignalHandlerAction(previous)) {
        // SIGSEGV/SIGBUS normally belonged to this dispatcher before FEX was
        // staged. Continue through its pre-FEX path below.
        return false;
    }
    if (previous.sa_handler == SIG_IGN ||
        (previous.sa_handler == SIG_DFL && sig == SIGURG)) {
        return true;
    }
    if (previous.sa_handler == SIG_DFL || previous.sa_handler == nullptr) {
        // Preserve the kernel default rather than swallowing a trap during the
        // short prepared-but-uncommitted activation window.
        (void)sigaction(sig, &previous, nullptr);
#if defined(LSX4_APPLE_FEX)
        (void)pthread_kill(pthread_self(), sig);
#else
        (void)syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sig);
#endif
        _exit(128 + sig);
    }

    sigset_t block_mask = previous.sa_mask;
    if ((previous.sa_flags & SA_NODEFER) == 0) {
        sigaddset(&block_mask, sig);
    }
    sigset_t old_mask{};
    const bool mask_changed = sigprocmask(SIG_BLOCK, &block_mask, &old_mask) == 0;
    if ((previous.sa_flags & SA_RESETHAND) != 0) {
        struct sigaction reset{};
        reset.sa_handler = SIG_DFL;
        sigemptyset(&reset.sa_mask);
        (void)sigaction(sig, &reset, nullptr);
    }
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(sig, info, raw_context);
    } else {
        previous.sa_handler(sig);
    }
    if (mask_changed) {
        (void)sigprocmask(SIG_SETMASK, &old_mask, nullptr);
    }
    return true;
}

static bool AddressIsInNamedMap(void* address, const char* needle) {
    if (!address || !needle) {
        return false;
    }
    const auto target = reinterpret_cast<uintptr_t>(address);
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) {
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        if (std::sscanf(line, "%llx-%llx", &start, &end) == 2 && target >= start &&
            target < end && std::strstr(line, needle)) {
            std::fclose(maps);
            return true;
        }
    }
    std::fclose(maps);
    return false;
}

static bool IsGuestNullCheckFault(int sig, const siginfo_t* info) {
    if (sig != SIGSEGV || info == nullptr) {
        return false;
    }
    const auto fault = reinterpret_cast<std::uintptr_t>(info->si_addr);
    return info->si_code == SEGV_MAPERR && fault < 0x10000;
}

static bool ChainPreviousSignalHandler(int sig, siginfo_t* info, void* raw_context,
                                       const char* reason) {
    auto& previous = PreviousActionForSignal(sig);
    if ((previous.sa_flags & SA_SIGINFO) && previous.sa_sigaction &&
        previous.sa_sigaction != SignalHandler) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_FEX_SIGNAL_CHAIN] sig=%d reason=%s pc=%p fault=%p "
                            "prev_sigaction=%p flags=0x%x",
                            sig, reason ? reason : "<none>", Common::GetRip(raw_context),
                            info ? info->si_addr : nullptr,
                            reinterpret_cast<void*>(previous.sa_sigaction), previous.sa_flags);
        previous.sa_sigaction(sig, info, raw_context);
        return true;
    }
    if (previous.sa_handler && previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN &&
        reinterpret_cast<void*>(previous.sa_handler) != reinterpret_cast<void*>(SignalHandler)) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_FEX_SIGNAL_CHAIN] sig=%d reason=%s pc=%p fault=%p "
                            "prev_handler=%p flags=0x%x",
                            sig, reason ? reason : "<none>", Common::GetRip(raw_context),
                            info ? info->si_addr : nullptr,
                            reinterpret_cast<void*>(previous.sa_handler), previous.sa_flags);
        previous.sa_handler(sig);
        return true;
    }
    return false;
}
#endif

static std::string DisassembleInstruction(void* code_address) {
    char buffer[256] = "<unable to decode>";

#ifdef ARCH_X86_64
    X86DecodedInstruction instruction;
    X86DecodedOperand operands[X86_MAX_OPERAND_COUNT];
    const auto status =
        Common::Decoder::Instance()->decodeInstruction(instruction, operands, code_address);
    if (X86_SUCCESS(status)) {
        X86Formatter formatter;
        X86FormatterInit(&formatter, X86_FORMATTER_STYLE_INTEL);
        X86FormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible, buffer, sizeof(buffer),
                                        reinterpret_cast<u64>(code_address), nullptr);
    }
#endif

    return buffer;
}

#ifdef __ANDROID__
static bool TryFixupMisalignedReleaseAcquire(int sig, siginfo_t* info, void* raw_context) {
    if (sig != SIGBUS || raw_context == nullptr || info == nullptr || info->si_code != BUS_ADRALN) {
        return false;
    }
    auto& mc = reinterpret_cast<ucontext_t*>(raw_context)->uc_mcontext;
    const auto insn = *reinterpret_cast<const std::uint32_t*>(mc.pc);
    const unsigned rt = insn & 0x1fu;
    const unsigned rn = (insn >> 5) & 0x1fu;
    const std::uint64_t addr = (rn == 31) ? mc.sp : mc.regs[rn];
    const auto load_reg = [&](unsigned i) -> std::uint64_t { return i == 31 ? 0 : mc.regs[i]; };
    const auto store_reg = [&](unsigned i, std::uint64_t v) { if (i != 31) mc.regs[i] = v; };
    std::atomic_thread_fence(std::memory_order_seq_cst);
    bool handled = true;
    switch (insn & 0xFFFFFC00u) {
    case 0xC89FFC00u: { const std::uint64_t v = load_reg(rt);
        std::memcpy(reinterpret_cast<void*>(addr), &v, 8); break; }
    case 0x889FFC00u: { const std::uint32_t v = static_cast<std::uint32_t>(load_reg(rt));
        std::memcpy(reinterpret_cast<void*>(addr), &v, 4); break; }
    case 0x489FFC00u: { const std::uint16_t v = static_cast<std::uint16_t>(load_reg(rt));
        std::memcpy(reinterpret_cast<void*>(addr), &v, 2); break; }
    case 0xC8DFFC00u: { std::uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(addr), 8);
        store_reg(rt, v); break; }
    case 0x88DFFC00u: { std::uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(addr), 4);
        store_reg(rt, static_cast<std::uint64_t>(v)); break; }
    case 0x48DFFC00u: { std::uint16_t v; std::memcpy(&v, reinterpret_cast<const void*>(addr), 2);
        store_reg(rt, static_cast<std::uint64_t>(v)); break; }
    case 0xF8BFC000u: { std::uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(addr), 8);
        store_reg(rt, v); break; }
    case 0xB8BFC000u: { std::uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(addr), 4);
        store_reg(rt, static_cast<std::uint64_t>(v)); break; }
    case 0x78BFC000u: { std::uint16_t v; std::memcpy(&v, reinterpret_cast<const void*>(addr), 2);
        store_reg(rt, static_cast<std::uint64_t>(v)); break; }
    default: handled = false; break;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!handled) {
        return false;
    }
    mc.pc += 4;
    static std::atomic<int> total{0};
    const int n = total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 64) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_FEX_UNALIGN_FIXUP] n=%d insn=0x%08x addr=0x%llx rt=%u rn=%u",
                            n, insn, static_cast<unsigned long long>(addr), rt, rn);
    }
    if (n <= 8 || (n & 0xfff) == 0) {
        char line[128];
        const int len = std::snprintf(line, sizeof(line),
                                      "UNALIGN_FIXUP n=%d insn=0x%08x addr=0x%llx\n", n, insn,
                                      static_cast<unsigned long long>(addr));
        const int fd = ::open("/data/data/app.lsx4.android/files/executor-unalign.log",
                              O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            (void)::write(fd, line, static_cast<size_t>(len > 0 ? len : 0));
            ::close(fd);
        }
    }
    return true;
}
#endif

#ifdef __ANDROID__
static bool TrySkipFaultingGuestStore(int sig, siginfo_t* info, void* raw_context) {
    if (sig != SIGSEGV || raw_context == nullptr) {
        return false;
    }
    static const bool enabled = [] {
        const char* v = std::getenv("EXECUTOR_SKIP_FAULTING_STORES");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    if (!enabled || !Common::IsWriteError(raw_context)) {
        return false;
    }
    auto& mc = reinterpret_cast<ucontext_t*>(raw_context)->uc_mcontext;
    const auto insn = *reinterpret_cast<const std::uint32_t*>(mc.pc);
    const bool is_str = ((insn & 0x3B000000u) == 0x38000000u) ||
                        ((insn & 0x3BC00000u) == 0x29000000u) ||
                        ((insn & 0x3BC00000u) == 0x28800000u) ||
                        ((insn & 0x3BC00000u) == 0x29800000u);
    if (!is_str) {
        return false;
    }
    mc.pc += 4;
    static std::atomic<int> total{0};
    const int n = total.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 128) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_SKIP_FAULTING_STORE] n=%d insn=0x%08x faultAddr=%p pc=0x%llx",
                            n, insn, info ? info->si_addr : nullptr,
                            static_cast<unsigned long long>(mc.pc - 4));
    }
    return true;
}
#endif

#if defined(LSX4_APPLE_RUNTIME)
std::atomic<int> g_executor_render_tid{0};
#endif

#ifdef __ANDROID__
std::atomic<int> g_executor_render_tid{0};
}

namespace boost {
void assertion_failed(char const* expr, char const* function, char const* file, long line) {
    static std::atomic<int> budget{64};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                        "[EXECUTOR_BOOST_ASSERT] expr='%s' fn='%s' at %s:%ld", expr ? expr : "?",
                        function ? function : "?", file ? file : "?", line);
    char buf[512];
    const int n = std::snprintf(buf, sizeof(buf), "BOOST_ASSERT expr='%s' fn='%s' %s:%ld\n",
                                expr ? expr : "?", function ? function : "?", file ? file : "?", line);
    const int fd = ::open("/data/data/app.lsx4.android/files/lsx4-home/jit-checkfail.txt",
                          O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        (void)::write(fd, buf, static_cast<size_t>(n > 0 ? n : 0));
        ::close(fd);
    }
}
void assertion_failed_msg(char const* expr, char const* msg, char const* function, char const* file,
                          long line) {
    assertion_failed(expr, function, file, line);
    (void)msg;
}
}

namespace Core {

struct BtState {
    void** pcs;
    int count;
    int cap;
};
static _Unwind_Reason_Code BtTrace(struct _Unwind_Context* ctx, void* arg) {
    auto* st = static_cast<BtState*>(arg);
    if (st->count >= st->cap) {
        return _URC_END_OF_STACK;
    }
    const uintptr_t ip = _Unwind_GetIP(ctx);
    if (ip != 0) {
        st->pcs[st->count++] = reinterpret_cast<void*>(ip);
    }
    return _URC_NO_REASON;
}
static void CaptureAbortBacktrace(int sig) {
    static std::atomic<int> budget{8};
    if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        return;
    }
    void* pcs[64];
    BtState st{pcs, 0, 64};
    _Unwind_Backtrace(&BtTrace, &st);
    Dl_info info{};
    char line[256];
    const int fd = ::open("/data/data/app.lsx4.android/files/lsx4-home/jit-checkfail.txt",
                          O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return;
    }
    int n = std::snprintf(line, sizeof(line), "ABORT_BACKTRACE sig=%d tid=%d frames=%d\n", sig,
                          static_cast<int>(gettid()), st.count);
    (void)::write(fd, line, static_cast<size_t>(n > 0 ? n : 0));
    for (int i = 0; i < st.count; ++i) {
        const char* fname = "?";
        uintptr_t off = 0;
        if (dladdr(pcs[i], &info) && info.dli_fname) {
            fname = info.dli_fname;
            off = reinterpret_cast<uintptr_t>(pcs[i]) - reinterpret_cast<uintptr_t>(info.dli_fbase);
        }
        n = std::snprintf(line, sizeof(line), "  #%02d pc=%p %s+0x%lx\n", i, pcs[i], fname,
                          static_cast<unsigned long>(off));
        (void)::write(fd, line, static_cast<size_t>(n > 0 ? n : 0));
    }
    ::close(fd);
    __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                        "[EXECUTOR_ABORT_BACKTRACE] sig=%d tid=%d frames=%d (see jit-checkfail.txt)",
                        sig, static_cast<int>(gettid()), st.count);
}

static bool TryParkNonRenderThreadOnFault(int sig, void* code_address, siginfo_t* info) {
    static const bool enabled = [] {
        const char* v = std::getenv("EXECUTOR_PARK_NONRENDER_ON_FAULT");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    if (!enabled) {
        return false;
    }
    static const bool park_all = [] {
        const char* v = std::getenv("EXECUTOR_PARK_ALL_ON_FAULT");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    const int tid = static_cast<int>(gettid());
    const int render_tid = g_executor_render_tid.load(std::memory_order_relaxed);
    if (tid == getpid()) {
        return false;
    }
    if (!park_all && render_tid != 0 && tid == render_tid) {
        return false;
    }
    static std::atomic<int> parked{0};
    const int n = parked.fetch_add(1, std::memory_order_relaxed) + 1;
    __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                        "[EXECUTOR_PARK_NONRENDER_ON_FAULT] n=%d sig=%d tid=%d renderTid=%d pc=%p "
                        "fault=%p (parking worker thread so render can present)",
                        n, sig, tid, render_tid, code_address, info ? info->si_addr : nullptr);
    for (;;) {
        struct timespec ts{3600, 0};
        nanosleep(&ts, nullptr);
    }
}
#endif

void SignalHandler(int sig, siginfo_t* info, void* raw_context) {
    const auto* signals = Signals::Instance();
    bool access_violation_prechecked = false;
#if defined(__ANDROID__) || defined(LSX4_APPLE_FEX)
    if (InvokePreparedPreviousAction(sig, info, raw_context)) {
        return;
    }
#if defined(LSX4_APPLE_FEX)
    siginfo_t normalized{};
    if (AndroidFexOwnsFaultSignals() && info) {
        normalized = *info;
        executor_lsx4_apple_fex_normalize_signal(&sig, &normalized, raw_context);
        info = &normalized;
    }
#endif
    // A generated guest access can hit a PageManager mprotect watch before it
    // is a guest-visible fault.  Let the watch owner repair a precise ACCERR
    // first; only an unowned fault may enter FEX's guest-signal path.  MAPERR
    // is also owner-checked on Apple because PROT_NONE may remove the PTE.
    // Other synchronous signals remain FEX-first. Remember the
    // negative result so the same signal is never dispatched twice below.
    const bool precheck_owner =
#if defined(LSX4_APPLE_FEX)
        executor_lsx4_apple_fex_precheck_memory_owner(sig, info);
#else
        sig == SIGSEGV && info != nullptr && info->si_code == SEGV_ACCERR;
#endif
    if (AndroidFexOwnsFaultSignals() && precheck_owner) {
        access_violation_prechecked = true;
        const bool repaired = signals->DispatchAccessViolation(raw_context, info->si_addr);
#if defined(LSX4_APPLE_FEX)
        if (info->si_code == SEGV_MAPERR) {
            static std::atomic<unsigned> maperr_owner_notes{0};
            if (maperr_owner_notes.fetch_add(1, std::memory_order_relaxed) < 4) {
                __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                    "[APPLE_FEX_MAPERR_OWNER] address=%p repaired=%u",
                    info->si_addr, repaired ? 1u : 0u);
            }
        }
#endif
        if (repaired) return;
    }
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGTRAP ||
        sig == SIGURG) {
        if (TryHandleFexHostSignal(sig, info, raw_context)) {
            return;
        }
        if (AndroidFexOwnsFaultSignals() && sig == SIGTRAP) {
            if (ChainPreviousSignalHandler(sig, info, raw_context,
                                           "fex_unhandled_trap")) {
                return;
            }
#if defined(LSX4_APPLE_FEX)
            // An unowned host breakpoint is fatal. Raising a second BRK from
            // this signal owner recursively re-enters the same handler and
            // obscures the first failure while starving the main thread.
            executor_lsx4_apple_note_unhandled_signal(sig, info, raw_context);
            static constexpr char message[] = "[APPLE_FEX_UNHANDLED_TRAP] terminating\n";
            (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
            ::_exit(128 + SIGTRAP);
#else
            UNREACHABLE_MSG("Unhandled FEX SIGTRAP at code address {}",
                            fmt::ptr(Common::GetRip(raw_context)));
#endif
        }
    }
#endif
#ifdef __ANDROID__
    const bool jit_parity = AndroidJitActive();
    if (!jit_parity) {
        if (TryFixupMisalignedReleaseAcquire(sig, info, raw_context)) {
            return;
        }
        if (TrySkipFaultingGuestStore(sig, info, raw_context)) {
            return;
        }
    }
    if (sig == SIGABRT) {
        CaptureAbortBacktrace(sig);
    }
    if (sig == SIGABRT && TryParkNonRenderThreadOnFault(sig, Common::GetRip(raw_context), info)) {
        return;
    }
#endif

    auto* code_address = Common::GetRip(raw_context);

    switch (sig) {
    case SIGSEGV:
    case SIGBUS: {
        const bool is_write = Common::IsWriteError(raw_context);
        if (!access_violation_prechecked &&
            signals->DispatchAccessViolation(raw_context, info->si_addr)) {
            break;
        }
#ifdef __ANDROID__
        const bool jit_fault_can_resume =
            jit_parity && JitSynchronousGuestFaultCanResume(raw_context);
        if (jit_parity && !jit_fault_can_resume) {
            static std::atomic<u32> no_resume_log_count{0};
            const u32 index = no_resume_log_count.fetch_add(1, std::memory_order_relaxed);
            if (index < 4) {
                const auto* const context = static_cast<const ucontext_t*>(raw_context);
                std::array<u64, 10> sidecar{};
                const u32 sidecar_count = executor_jit_debug_signal_fault_sidecar
                                              ? executor_jit_debug_signal_fault_sidecar(
                                                    raw_context, sidecar.data(),
                                                    sidecar.size())
                                              : 0;
                __android_log_print(
                    ANDROID_LOG_ERROR, "LSX4Native",
                    "[EXECUTOR_JIT_FAULT_NO_RESUME] n=%u sig=%d code=%d fault=%p write=%d "
                    "hostPc=0x%llx x19=0x%llx x21=0x%llx x28=0x%llx "
                    "guestRip=0x%llx activeRip=0x%llx "
                    "sidecar=%u/%llu region=%llu begin=0x%llx end=0x%llx off=%llu "
                    "sites=%llu prev=%llu/0x%llx next=%llu/0x%llx",
                    index + 1, sig, info ? info->si_code : 0,
                    info ? info->si_addr : nullptr, is_write ? 1 : 0,
                    static_cast<unsigned long long>(
                        context ? context->uc_mcontext.pc : 0),
                    static_cast<unsigned long long>(
                        context ? context->uc_mcontext.regs[19] : 0),
                    static_cast<unsigned long long>(
                        context ? context->uc_mcontext.regs[21] : 0),
                    static_cast<unsigned long long>(
                        context ? context->uc_mcontext.regs[28] : 0),
                    static_cast<unsigned long long>(
                        executor_jit_current_fault_guest_rip
                            ? executor_jit_current_fault_guest_rip()
                            : 0),
                    static_cast<unsigned long long>(
                        executor_jit_current_fault_active_block_rip
                            ? executor_jit_current_fault_active_block_rip()
                            : 0),
                    sidecar_count,
                    static_cast<unsigned long long>(sidecar[0]),
                    static_cast<unsigned long long>(sidecar[1]),
                    static_cast<unsigned long long>(sidecar[2]),
                    static_cast<unsigned long long>(sidecar[3]),
                    static_cast<unsigned long long>(sidecar[4]),
                    static_cast<unsigned long long>(sidecar[5]),
                    static_cast<unsigned long long>(sidecar[6]),
                    static_cast<unsigned long long>(sidecar[7]),
                    static_cast<unsigned long long>(sidecar[8]),
                    static_cast<unsigned long long>(sidecar[9]));
            }
        }
        if (jit_fault_can_resume) {
#if defined(__aarch64__)
            static std::atomic<u32> deferred_capture_count{0};
            const u32 capture =
                deferred_capture_count.fetch_add(1, std::memory_order_relaxed);
            if (capture < 32) {
                const auto* uc = static_cast<const ucontext_t*>(raw_context);
                const u64 host_pc =
                    uc ? static_cast<u64>(uc->uc_mcontext.pc) : 0;
                const u64 host_lr =
                    uc ? static_cast<u64>(uc->uc_mcontext.regs[30]) : 0;
                const u64 host_sp =
                    uc ? static_cast<u64>(uc->uc_mcontext.sp) : 0;
                const u64 host_fp =
                    uc ? static_cast<u64>(uc->uc_mcontext.regs[29]) : 0;
                const u64 guest_rip =
                    executor_jit_current_fault_guest_rip != nullptr
                        ? executor_jit_current_fault_guest_rip()
                        : 0;
                const u64 active_rip =
                    executor_jit_current_fault_active_block_rip != nullptr
                        ? executor_jit_current_fault_active_block_rip()
                        : 0;
                __android_log_print(
                    ANDROID_LOG_ERROR, "LSX4Native",
                    "[LSX4_DEFERRED_SIGNAL] phase=capture native=%d code=%d "
                    "fault=%p write=%d hostPc=0x%llx hostLr=0x%llx "
                    "hostSp=0x%llx hostFp=0x%llx x1=0x%llx x3=0x%llx "
                    "x5=0x%llx x7=0x%llx x10=0x%llx x11=0x%llx "
                    "x19=0x%llx x21=0x%llx x22=0x%llx x29=0x%llx "
                    "x30=0x%llx guestRip=0x%llx "
                    "activeRip=0x%llx",
                    sig, info ? info->si_code : 0,
                    info ? info->si_addr : nullptr, is_write ? 1 : 0,
                    static_cast<unsigned long long>(host_pc),
                    static_cast<unsigned long long>(host_lr),
                    static_cast<unsigned long long>(host_sp),
                    static_cast<unsigned long long>(host_fp),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[1] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[3] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[5] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[7] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[10] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[11] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[19] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[21] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[22] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[29] : 0),
                    static_cast<unsigned long long>(
                        uc ? uc->uc_mcontext.regs[30] : 0),
                    static_cast<unsigned long long>(guest_rip),
                    static_cast<unsigned long long>(active_rip));
            }
#endif
            if (DeferJitSynchronousGuestFault(
                    sig, info, is_write, raw_context)) {
                return;
            }
            UNREACHABLE_MSG(
                "JIT synchronous guest access fault could not be deferred");
        }
        {
            static std::atomic<int> budget{12};
            if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
                const auto gh = Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)];
                const u64 guest_rip = executor_jit_current_fault_guest_rip != nullptr
                                          ? executor_jit_current_fault_guest_rip()
                                          : 0;
                const u64 active_rip =
                    executor_jit_current_fault_active_block_rip != nullptr
                        ? executor_jit_current_fault_active_block_rip()
                        : 0;
                const u64 instruction = executor_jit_current_fault_instruction != nullptr
                                            ? executor_jit_current_fault_instruction()
                                            : 0;
                const u32 instruction_meta =
                    executor_jit_current_fault_instruction_meta != nullptr
                        ? executor_jit_current_fault_instruction_meta()
                        : 0;
                const auto fault_gpr = [](const u32 index) -> u64 {
                    return executor_jit_current_fault_gpr != nullptr
                               ? executor_jit_current_fault_gpr(index)
                               : 0;
                };
                u64 recent_rips[16]{};
                const u32 recent_count = executor_jit_current_fault_recent_rips != nullptr
                                             ? executor_jit_current_fault_recent_rips(
                                                   recent_rips, 16u)
                                             : 0;
                u8 active_bytes[64]{};
                const u32 active_bytes_count =
                    executor_jit_current_fault_active_block_bytes != nullptr
                        ? executor_jit_current_fault_active_block_bytes(
                              active_bytes, 64u)
                        : 0;
                char line[768];
                int n = std::snprintf(
                    line, sizeof(line),
                    "SEGV sig=%d code=%d si_addr=%p host_rip=%p is_write=%d guest_handler=%p "
                    "guest_rip=0x%llx active_rip=0x%llx mnemonic=%u length=%u bytes_le=0x%llx "
                    "rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx rsp=0x%llx rbp=0x%llx "
                    "rsi=0x%llx rdi=0x%llx recent=",
                    sig, info ? info->si_code : -1, info ? info->si_addr : nullptr,
                    code_address, is_write ? 1 : 0, reinterpret_cast<void*>(gh),
                    static_cast<unsigned long long>(guest_rip),
                    static_cast<unsigned long long>(active_rip), instruction_meta & 0x00ffffffu,
                    instruction_meta >> 24u, static_cast<unsigned long long>(instruction),
                    static_cast<unsigned long long>(fault_gpr(0)),
                    static_cast<unsigned long long>(fault_gpr(1)),
                    static_cast<unsigned long long>(fault_gpr(2)),
                    static_cast<unsigned long long>(fault_gpr(3)),
                    static_cast<unsigned long long>(fault_gpr(4)),
                    static_cast<unsigned long long>(fault_gpr(5)),
                    static_cast<unsigned long long>(fault_gpr(6)),
                    static_cast<unsigned long long>(fault_gpr(7)));
                for (u32 index = 0; index < recent_count && n > 0 &&
                                    static_cast<size_t>(n) < sizeof(line); ++index) {
                    n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                                       "%s%llx", index == 0 ? "" : ",",
                                       static_cast<unsigned long long>(recent_rips[index]));
                }
                if (n > 0 && static_cast<size_t>(n) < sizeof(line)) {
                    n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                                       " blockBytes=");
                }
                for (u32 index = 0; index < active_bytes_count && n > 0 &&
                                    static_cast<size_t>(n) + 3 < sizeof(line); ++index) {
                    n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "%02x",
                                       active_bytes[index]);
                }
                if (n > 0 && static_cast<size_t>(n) + 2 < sizeof(line)) {
                    line[n++] = '\n';
                    line[n] = '\0';
                }
                int fd = ::open("/data/data/app.lsx4.android/files/executor-segv.log",
                                O_WRONLY | O_CREAT | O_APPEND, 0600);
                if (fd >= 0) {
                    (void)::write(fd, line, static_cast<size_t>(n > 0 ? n : 0));
                    if (executor_jit_current_fault_recent_block != nullptr) {
                        for (u32 recent_index = 0; recent_index < 12; ++recent_index) {
                            u64 recent_rip = 0;
                            u8 recent_bytes[128]{};
                            const u32 recent_size = executor_jit_current_fault_recent_block(
                                recent_index, &recent_rip, recent_bytes, 128u);
                            if (recent_size == 0) {
                                break;
                            }
                            char recent_line[320];
                            int recent_n = std::snprintf(
                                recent_line, sizeof(recent_line),
                                "RECENT_BLOCK newest=%u rip=0x%llx bytes=", recent_index,
                                static_cast<unsigned long long>(recent_rip));
                            for (u32 byte_index = 0; byte_index < recent_size && recent_n > 0 &&
                                                   static_cast<size_t>(recent_n) + 3 <
                                                       sizeof(recent_line);
                                 ++byte_index) {
                                recent_n += std::snprintf(
                                    recent_line + recent_n,
                                    sizeof(recent_line) - static_cast<size_t>(recent_n), "%02x",
                                    recent_bytes[byte_index]);
                            }
                            if (recent_n > 0 &&
                                static_cast<size_t>(recent_n) + 2 < sizeof(recent_line)) {
                                recent_line[recent_n++] = '\n';
                            }
                            (void)::write(fd, recent_line,
                                          static_cast<size_t>(recent_n > 0 ? recent_n : 0));
                        }
                    }
                    ::close(fd);
                }
            }
        }
#endif
        {
#ifdef __ANDROID__
            if (jit_parity) {
                const s32 guest_sig = Libraries::Kernel::NativeToOrbisSignal(sig);
                if (Libraries::Kernel::HasSignalHandler(guest_sig) &&
                    jit_fault_can_resume) {
                    if (DeferJitSynchronousGuestFault(
                            sig, info, is_write, raw_context)) {
                        return;
                    }
                    UNREACHABLE_MSG(
                        "JIT synchronous guest access fault could not be deferred");
                }
                if (!jit_fault_can_resume &&
                    ChainPreviousSignalHandler(sig, info, raw_context,
                                               "jit_host_access_no_resume")) {
                    return;
                }
                UNREACHABLE_MSG("Unhandled JIT access violation at code address {}: {} address {}",
                                fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                                fmt::ptr(info->si_addr));
            }
#endif
#ifdef __ANDROID__
            if (IsGuestNullCheckFault(sig, info) &&
                Libraries::Kernel::Handlers[Libraries::Kernel::NativeToOrbisSignal(sig)]) {
                static std::atomic<int> log_budget{64};
                if (log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
                    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                        "[EXECUTOR_FEX_SIGNAL_ROUTE] sig=%d reason=guest_null_check "
                                        "pc=%p fault=%p",
                                        sig, code_address, info ? info->si_addr : nullptr);
                }
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
#endif
#ifdef __ANDROID__
            if (AddressIsInNamedMap(code_address, "FEXMem") &&
                ChainPreviousSignalHandler(sig, info, raw_context, "fexmem_access")) {
                return;
            }
            if (TryParkNonRenderThreadOnFault(sig, code_address, info)) {
                return;
            }
            if (ChainPreviousSignalHandler(sig, info, raw_context, "android_unhandled_access")) {
                return;
            }
#endif
            if (Libraries::Kernel::HasSignalHandler(
                    Libraries::Kernel::NativeToOrbisSignal(sig))) {
#if defined(LSX4_APPLE_RUNTIME)
                executor_lsx4_apple_note_guest_signal(sig, info, raw_context);
                if (executor_jit_has_synchronous_guest_fault_frame(raw_context) &&
                    executor_jit_defer_synchronous_guest_fault(sig,
                        info ? info->si_code : 0, info ? info->si_errno : 0,
                        info ? info->si_pid : 0, info ? info->si_uid : 0,
                        reinterpret_cast<u64>(info ? info->si_addr : nullptr),
                        Common::IsWriteError(raw_context) ? 1 : 0, raw_context)) return;
#endif
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
#if defined(LSX4_APPLE_RUNTIME)
            executor_lsx4_apple_note_unhandled_signal(sig, info, raw_context);
#endif
            UNREACHABLE_MSG("Unhandled access violation at code address {}: {} address {}",
                            fmt::ptr(code_address), is_write ? "Write to" : "Read from",
                            fmt::ptr(info->si_addr));
        }
        break;
    }
    case SIGILL:
        if (!signals->DispatchIllegalInstruction(raw_context)) {
#ifdef __ANDROID__
            if (jit_parity) {
                if (!JitSynchronousGuestFaultCanResume(raw_context)) {
                    const auto* const context = static_cast<const ucontext_t*>(raw_context);
                    __android_log_print(
                        ANDROID_LOG_ERROR, "LSX4Native",
                        "[EXECUTOR_JIT_FAULT_NO_RESUME] sig=%d code=%d fault=%p "
                        "hostPc=0x%llx x19=0x%llx x21=0x%llx x28=0x%llx "
                        "guestRip=0x%llx activeRip=0x%llx",
                        sig, info ? info->si_code : 0,
                        info ? info->si_addr : nullptr,
                        static_cast<unsigned long long>(
                            context ? context->uc_mcontext.pc : 0),
                        static_cast<unsigned long long>(
                            context ? context->uc_mcontext.regs[19] : 0),
                        static_cast<unsigned long long>(
                            context ? context->uc_mcontext.regs[21] : 0),
                        static_cast<unsigned long long>(
                            context ? context->uc_mcontext.regs[28] : 0),
                        static_cast<unsigned long long>(
                            executor_jit_current_fault_guest_rip
                                ? executor_jit_current_fault_guest_rip()
                                : 0),
                        static_cast<unsigned long long>(
                            executor_jit_current_fault_active_block_rip
                                ? executor_jit_current_fault_active_block_rip()
                                : 0));
                }
                const s32 guest_sig = Libraries::Kernel::NativeToOrbisSignal(sig);
                if (Libraries::Kernel::HasSignalHandler(guest_sig) &&
                    JitSynchronousGuestFaultCanResume(raw_context)) {
                    if (DeferJitSynchronousGuestFault(
                            sig, info, false, raw_context)) {
                        return;
                    }
                    UNREACHABLE_MSG(
                        "JIT synchronous guest illegal instruction could not be deferred");
                }
                if (!JitSynchronousGuestFaultCanResume(raw_context) &&
                    ChainPreviousSignalHandler(sig, info, raw_context,
                                               "jit_host_ill_no_resume")) {
                    return;
                }
                UNREACHABLE_MSG("Unhandled JIT illegal instruction at code address {}: {}",
                                fmt::ptr(code_address), DisassembleInstruction(code_address));
            }
            if (AddressIsInNamedMap(code_address, "FEXMem") &&
                ChainPreviousSignalHandler(sig, info, raw_context, "fexmem_ill")) {
                return;
            }
            if (ChainPreviousSignalHandler(sig, info, raw_context, "android_unhandled_ill")) {
                return;
            }
#endif
            if (Libraries::Kernel::HasSignalHandler(
                    Libraries::Kernel::NativeToOrbisSignal(sig))) {
#if defined(LSX4_APPLE_RUNTIME)
                executor_lsx4_apple_note_guest_signal(sig, info, raw_context);
                if (executor_jit_has_synchronous_guest_fault_frame(raw_context) &&
                    executor_jit_defer_synchronous_guest_fault(sig,
                        info ? info->si_code : 0, info ? info->si_errno : 0,
                        info ? info->si_pid : 0, info ? info->si_uid : 0,
                        reinterpret_cast<u64>(info ? info->si_addr : nullptr),
                        Common::IsWriteError(raw_context) ? 1 : 0, raw_context)) return;
#endif
                Libraries::Kernel::SigactionHandler(sig, info,
                                                    reinterpret_cast<ucontext_t*>(raw_context));
                return;
            }
#if defined(LSX4_APPLE_RUNTIME)
            executor_lsx4_apple_note_unhandled_signal(sig, info, raw_context);
#endif
            UNREACHABLE_MSG("Unhandled illegal instruction at code address {}: {}",
                            fmt::ptr(code_address), DisassembleInstruction(code_address));
        }
        break;
    default:
        if (sig == SIGSLEEP) {
            sigset_t sigset;
            sigemptyset(&sigset);
            sigaddset(&sigset, SIGSLEEP);
            sigwait(&sigset, &sig);
        }
        break;
    }
}

#if defined(LSX4_APPLE_RUNTIME)
#if !defined(LSX4_APPLE_FEX)
bool HasSleepSignalHandler() {
    struct sigaction action{};
    return sigaction(SIGSLEEP, nullptr, &action) == 0 &&
           (action.sa_flags & SA_SIGINFO) != 0 && action.sa_sigaction == SignalHandler;
}
#endif
extern "C" void executor_lsx4_install_jit_signal_handlers() {
    (void)Signals::Instance();
}
#endif

#endif

#ifdef __ANDROID__
static void ReassertBusHandlerOnce() {
    if (!AndroidCurrentOwnsFaultSignals()) {
        return;
    }
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    struct sigaction prev{};
    if (sigaction(SIGBUS, &action, &prev) == 0 && prev.sa_sigaction != SignalHandler) {
        g_previous_sigbus = prev;
        static std::atomic<int> budget{8};
        if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
            __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                                "[EXECUTOR_FEX_UNALIGN_REINSTALL] re-owned SIGBUS prev=%p",
                                reinterpret_cast<void*>(prev.sa_sigaction));
        }
    }
}

extern "C" void executor_lsx4_reinstall_bus_handler() {
    static std::once_flag once;
    std::call_once(once, [] {
        ReassertBusHandlerOnce();
        std::thread([] {
            for (int i = 0; i < 600; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ReassertBusHandlerOnce();
            }
        }).detach();
    });
}

#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_FEX)
static int RollbackPreparedFexSignalHandlers() {
    if (AndroidFexOwnsFaultSignals()) {
        return -1;
    }
    const auto phase = g_fex_signal_prepare_phase.load(std::memory_order_acquire);
    if (phase == FexSignalPreparePhase::Idle) {
        return 0;
    }

    bool restored = true;
    for (std::size_t offset = 0; offset < kFexOwnedSignals.size(); ++offset) {
        const std::size_t index = kFexOwnedSignals.size() - 1 - offset;
        if (!g_fex_prepare_installed[index].load(std::memory_order_acquire)) {
            g_fex_prepare_snapshot_ready[index].store(false, std::memory_order_release);
            continue;
        }
        struct sigaction current{};
        if (sigaction(kFexOwnedSignals[index], nullptr, &current) != 0) {
            restored = false;
        } else if (IsPreparedFexAction(current)) {
            if (sigaction(kFexOwnedSignals[index], &g_fex_prepare_previous[index], nullptr) != 0) {
                restored = false;
            }
        } else {
            // A later owner replaced us. Never clobber that action while
            // rolling back an activation that never became public.
            restored = false;
        }
        g_fex_prepare_installed[index].store(false, std::memory_order_release);
        g_fex_prepare_snapshot_ready[index].store(false, std::memory_order_release);
    }
    g_fex_signal_handlers_installed = false;
    g_fex_signal_prepare_phase.store(FexSignalPreparePhase::Idle,
                                     std::memory_order_release);
    return restored ? 0 : -1;
}

static bool VerifyPreparedFexSignalHandlers() {
    if (g_fex_signal_prepare_phase.load(std::memory_order_acquire) !=
        FexSignalPreparePhase::Prepared) {
        return false;
    }
    for (std::size_t index = 0; index < kFexOwnedSignals.size(); ++index) {
        if (!g_fex_prepare_snapshot_ready[index].load(std::memory_order_acquire) ||
            !g_fex_prepare_installed[index].load(std::memory_order_acquire)) {
            return false;
        }
        struct sigaction current{};
        if (sigaction(kFexOwnedSignals[index], nullptr, &current) != 0 ||
            !IsPreparedFexAction(current)) {
            return false;
        }
    }
    return true;
}

static int PrepareFexSignalHandlers() {
    const auto initial_phase =
        g_fex_signal_prepare_phase.load(std::memory_order_acquire);
    const bool preparation_authorized =
        executor_lsx4_android_fex_signal_prepare_authorized != nullptr &&
        executor_lsx4_android_fex_signal_prepare_authorized() != 0;
    if (initial_phase == FexSignalPreparePhase::Prepared) {
        return (preparation_authorized || AndroidFexOwnsFaultSignals()) &&
                       VerifyPreparedFexSignalHandlers()
                   ? 0
                   : -1;
    }
    if (!preparation_authorized) {
        return -1;
    }
    auto expected = FexSignalPreparePhase::Idle;
    if (!g_fex_signal_prepare_phase.compare_exchange_strong(
            expected, FexSignalPreparePhase::Preparing, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return expected == FexSignalPreparePhase::Prepared &&
                       VerifyPreparedFexSignalHandlers()
                   ? 0
                   : -1;
    }

    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    // A deferred FEX landing may synchronously fault while emulating the guest
    // access. The adapter owns the bounded nested-fault recovery policy, so the
    // kernel must not implicitly block a second instance of the same signal.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    FillFexSynchronousSignalMask(&action.sa_mask);

    for (std::size_t index = 0; index < kFexOwnedSignals.size(); ++index) {
        const int signal = kFexOwnedSignals[index];
        struct sigaction current{};
        if (sigaction(signal, nullptr, &current) != 0) {
            (void)RollbackPreparedFexSignalHandlers();
            return -1;
        }
        g_fex_prepare_previous[index] = current;
        g_fex_prepare_snapshot_ready[index].store(true, std::memory_order_release);
        if (!IsSignalHandlerAction(current)) {
            PreviousActionForSignal(signal) = current;
        }
        if (sigaction(signal, &action, nullptr) != 0) {
            g_fex_prepare_snapshot_ready[index].store(false, std::memory_order_release);
            (void)RollbackPreparedFexSignalHandlers();
            return -1;
        }
        g_fex_prepare_installed[index].store(true, std::memory_order_release);

        struct sigaction installed{};
        if (sigaction(signal, nullptr, &installed) != 0 ||
            !IsPreparedFexAction(installed)) {
            (void)RollbackPreparedFexSignalHandlers();
            return -1;
        }
    }

    g_fault_signal_handlers_installed = true;
    g_fex_signal_handlers_installed = true;
    g_fex_signal_prepare_phase.store(FexSignalPreparePhase::Prepared,
                                     std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_FEX_SIGNAL_MODE] phase=prepared "
                        "signals=SIGSEGV,SIGBUS,SIGILL,SIGTRAP,SIGURG");
    return 0;
}

#endif
#ifdef __ANDROID__
extern "C" void executor_lsx4_install_jit_signal_handlers() {
    if (!AndroidCurrentOwnsFaultSignals() || g_jit_signal_handlers_installed) {
        return;
    }
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    ASSERT_MSG(sigaction(SIGSEGV, &action, &g_previous_sigsegv) == 0,
               "Failed to register JIT SIGSEGV handler.");
    ASSERT_MSG(sigaction(SIGBUS, &action, &g_previous_sigbus) == 0,
               "Failed to register JIT SIGBUS handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, &g_previous_sigill) == 0,
               "Failed to register JIT SIGILL handler.");
    ASSERT_MSG(sigaction(SIGURG, &action, &g_previous_sigurg) == 0,
               "Failed to register JIT SIGURG handler.");
    ASSERT_MSG(sigaction(SIGVTALRM, &action, &g_previous_sigvtalrm) == 0,
               "Failed to register JIT SIGVTALRM handler.");
    g_fault_signal_handlers_installed = true;
    g_jit_signal_handlers_installed = true;
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_JIT_SIGNAL_OWNER] "
                        "signals=SIGSEGV,SIGBUS,SIGILL,SIGURG,SIGVTALRM");
}
#endif

SignalDispatch::SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(handle = AddVectoredExceptionHandler(0, SignalHandler),
               "Failed to register exception handler.");
#else
    struct sigaction action{};
    action.sa_sigaction = SignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);

#ifdef __ANDROID__
    const bool jit_active = AndroidJitActive();
    if (AndroidCurrentOwnsFaultSignals()) {
        executor_lsx4_install_jit_signal_handlers();
    } else if (AndroidFexOwnsFaultSignals()) {
        ASSERT_MSG(g_fex_signal_handlers_installed &&
                       g_fex_signal_prepare_phase.load(std::memory_order_acquire) ==
                           FexSignalPreparePhase::Prepared,
                   "FEX signal owner was published before its handlers were prepared.");
    } else {
        ASSERT_MSG(sigaction(SIGSEGV, &action, &g_previous_sigsegv) == 0,
                   "Failed to register access violation signal handler.");
        ASSERT_MSG(sigaction(SIGBUS, &action, &g_previous_sigbus) == 0,
                   "Failed to register bus error signal handler.");
        if (const char* pv = std::getenv("EXECUTOR_PARK_NONRENDER_ON_FAULT");
            pv != nullptr && pv[0] != '\0' && std::strcmp(pv, "0") != 0) {
            struct sigaction abrt_action{};
            abrt_action.sa_sigaction = SignalHandler;
            abrt_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&abrt_action.sa_mask);
            sigaction(SIGABRT, &abrt_action, nullptr);
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_PARK_NONRENDER_ON_FAULT] owns SIGABRT=1");
        }
        g_fault_signal_handlers_installed = true;
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_FEX_SIGNAL_MODE] owner=%s "
                            "shadps4=SIGSEGV,SIGBUS fex=SIGILL jit=%d",
                            jit_active ? "jit" : "split",
                            jit_active ? 1 : 0);
    }
#else
#if defined(LSX4_APPLE_RUNTIME)
    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0,
               "Failed to register Apple access violation signal handler.");
    ASSERT_MSG(sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to register Apple bus error signal handler.");
#endif
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to register illegal instruction signal handler.");
#endif
#ifdef __ANDROID__
    if (!jit_active) {
        ASSERT_MSG(sigaction(SIGSLEEP, &action, nullptr) == 0,
                   "Failed to register sleep signal handler.");
    }
#else
    ASSERT_MSG(sigaction(SIGSLEEP, &action, nullptr) == 0,
               "Failed to register sleep signal handler.");
#endif
#endif
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_FEX)
extern "C" int executor_lsx4_prepare_fex_signal_handlers() {
    // Construct the split dispatcher first, then transactionally replace its
    // actions while the public owner remains unselected.
    (void)Signals::Instance();
    return PrepareFexSignalHandlers();
}

extern "C" int executor_lsx4_rollback_prepared_fex_signal_handlers() {
    return RollbackPreparedFexSignalHandlers();
}
#endif

SignalDispatch::~SignalDispatch() {
#if defined(_WIN32)
    ASSERT_MSG(RemoveVectoredExceptionHandler(handle), "Failed to remove exception handler.");
#else
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);

#if defined(__ANDROID__) || defined(LSX4_APPLE_FEX)
    if (g_fault_signal_handlers_installed) {
        ASSERT_MSG(sigaction(SIGSEGV, &g_previous_sigsegv, nullptr) == 0,
                   "Failed to restore access violation signal handler.");
        if (g_jit_signal_handlers_installed) {
            ASSERT_MSG(sigaction(SIGBUS, &g_previous_sigbus, nullptr) == 0,
                       "Failed to restore bus signal handler.");
            ASSERT_MSG(sigaction(SIGILL, &g_previous_sigill, nullptr) == 0,
                       "Failed to restore illegal-instruction signal handler.");
            ASSERT_MSG(sigaction(SIGURG, &g_previous_sigurg, nullptr) == 0,
                       "Failed to restore urgent signal handler.");
            ASSERT_MSG(sigaction(SIGVTALRM, &g_previous_sigvtalrm, nullptr) == 0,
                       "Failed to restore virtual-alarm signal handler.");
            g_jit_signal_handlers_installed = false;
        } else if (g_fex_signal_handlers_installed) {
            ASSERT_MSG(sigaction(SIGBUS, &g_previous_sigbus, nullptr) == 0,
                       "Failed to restore FEX-owner bus signal handler.");
            ASSERT_MSG(sigaction(SIGILL, &g_previous_sigill, nullptr) == 0,
                       "Failed to restore FEX-owner illegal-instruction handler.");
            ASSERT_MSG(sigaction(SIGTRAP, &g_previous_sigtrap, nullptr) == 0,
                       "Failed to restore FEX-owner trap signal handler.");
            ASSERT_MSG(sigaction(SIGURG, &g_previous_sigurg, nullptr) == 0,
                       "Failed to restore FEX-owner urgent signal handler.");
            g_fex_signal_handlers_installed = false;
            for (std::size_t index = 0; index < kFexOwnedSignals.size(); ++index) {
                g_fex_prepare_installed[index].store(false, std::memory_order_relaxed);
                g_fex_prepare_snapshot_ready[index].store(false, std::memory_order_relaxed);
            }
            g_fex_signal_prepare_phase.store(FexSignalPreparePhase::Idle,
                                             std::memory_order_release);
        }
        g_fault_signal_handlers_installed = false;
    }
#else
    ASSERT_MSG(sigaction(SIGSEGV, &action, nullptr) == 0 &&
                   sigaction(SIGBUS, &action, nullptr) == 0,
               "Failed to remove access violation signal handler.");
    ASSERT_MSG(sigaction(SIGILL, &action, nullptr) == 0,
               "Failed to remove illegal instruction signal handler.");
#endif
#endif
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& [handler, _] : access_violation_handlers) {
        if (handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& [handler, _] : illegal_instruction_handlers) {
        if (handler(context)) {
            return true;
        }
    }
    return false;
}

}
