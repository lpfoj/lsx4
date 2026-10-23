// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleHostServices.h"
#include "android/log.h"
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <signal.h>
#include <unistd.h>

void* Lsx4::Apple::MapGuestHeapChunk(std::size_t minimum, std::size_t preferred,
                                   std::size_t& capacity) noexcept {
    return MapGuestHeapChunkUsing(minimum, std::min(preferred, GuestHeapPreferredChunkBytes),
        getpagesize(), capacity,
        [](std::size_t bytes) {
            return mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
        });
}

extern "C" void __clear_cache(void* first,void* last) {
    const auto begin=reinterpret_cast<std::uintptr_t>(first);
    const auto end=reinterpret_cast<std::uintptr_t>(last);
    if(end>begin) sys_icache_invalidate(first,end-begin);
}

extern "C" int __android_log_vprint(int priority,const char* tag,const char* format,va_list args) {
    // Normal play does not format or write info/debug logs. Explicit opt-in is
    // process-scoped so diagnostics cannot silently remain on in later runs.
    static const bool verbose=std::getenv("LSX4_RUNTIME_VERBOSE")!=nullptr;
    if(!verbose && priority<ANDROID_LOG_WARN) return 0;
    std::fprintf(stderr,"[%s] ",tag?tag:"LSX4");
    const int count=std::vfprintf(stderr,format,args);
    std::fputc('\n',stderr);
    return count;
}
extern "C" int __android_log_print(int priority,const char* tag,const char* format,...) {
    va_list args;va_start(args,format);
    const int count=__android_log_vprint(priority,tag,format,args);va_end(args);return count;
}
extern "C" int __android_log_write(int priority,const char* tag,const char* text) {
    return __android_log_print(priority,tag,"%s",text?text:"");
}
// The memory-only probe does not link the translation core. In the full app
// these readers expose its last published fault instruction without decoding.
#if defined(LSX4_APPLE_RUNTIME)
extern "C" std::uint64_t executor_jit_current_fault_guest_rip();
extern "C" std::uint64_t executor_jit_current_fault_active_block_rip();
extern "C" std::uint64_t executor_jit_current_fault_instruction();
extern "C" std::uint32_t executor_jit_current_fault_instruction_meta();
#endif
extern "C" bool executor_lsx4_apple_current_hle_call(std::uint64_t*) noexcept
    __attribute__((weak_import));
// Called only after memory/coherency owners rejected a synchronous fault.
// Avoid allocation, stdio locks and symbol lookup in the signal handler.
static void WriteAppleFaultContext(const char* label, int signal,
    const siginfo_t* info, const void* rawContext, bool nativeFrames=false) noexcept {
    char buffer[4096];char* out=buffer;
    const auto text=[&](const char* value) { while(*value) *out++=*value++; };
    const auto hex=[&](std::uint64_t value) {
        text("0x");bool significant=false;
        for(int shift=60;shift>=0;shift-=4) {
            const unsigned digit=(value>>shift)&15;
            if(digit || significant || !shift) { *out++="0123456789abcdef"[digit];significant=true; }
        }
    };
    text(label);text(" signal=");hex(static_cast<unsigned>(signal));
    text(" code=");hex(info?static_cast<unsigned>(info->si_code):0);
    text(" fault=");hex(info?reinterpret_cast<std::uintptr_t>(info->si_addr):0);
    const auto* context=static_cast<const ucontext_t*>(rawContext);
    if(context && context->uc_mcontext) {
        const auto& state=context->uc_mcontext->__ss;
        text(" pc=");hex(state.__pc);text(" lr=");hex(state.__lr);
        text(" sp=");hex(state.__sp);text(" fp=");hex(state.__fp);
        text(" esr=");hex(context->uc_mcontext->__es.__esr);
        text(" gpr=[");
        for(unsigned index=0;index<29;++index) { if(index)text(",");hex(state.__x[index]); }
        text("]");
        if(nativeFrames) {
            // Failure path only. Mach reads cannot recursively fault on stale frames.
            // A bounded monotonic chain also rejects cycles and pointers outside the
            // interrupted native stack neighborhood, without libc unwinding/locks.
            text(" native_frames=[");
            std::uint64_t frame=state.__fp;
            for(unsigned index=0;index<32;++index) {
                if((frame&15) || frame<state.__sp || frame-state.__sp>8*1024*1024-16) break;
                std::uint64_t record[2]{};vm_size_t frameCopied{};
                if(vm_read_overwrite(mach_task_self(),frame,sizeof(record),
                    reinterpret_cast<vm_address_t>(record),&frameCopied)!=KERN_SUCCESS ||
                    frameCopied!=sizeof(record) || !record[1]) break;
                if(index)text(",");hex(record[1]);
                if(record[0]<=frame) break;
                frame=record[0];
            }
            text("]");
        }
        // Read through Mach so an unmapped PC cannot fault the diagnostic.
        std::uint32_t nativeWords[12]{};vm_size_t copied{};
        if(state.__pc>=16 && vm_read_overwrite(mach_task_self(),state.__pc-16,
            sizeof(nativeWords),reinterpret_cast<vm_address_t>(nativeWords),&copied)==KERN_SUCCESS &&
            copied==sizeof(nativeWords)) {
            text(" native_from=");hex(state.__pc-16);text(" native_words=[");
            for(unsigned index=0;index<12;++index) { if(index)text(",");hex(nativeWords[index]); }
            text("]");
        }
    }
    std::uint64_t hle[8]{};
    if (executor_lsx4_apple_current_hle_call && executor_lsx4_apple_current_hle_call(hle)) {
        text(" hle_rip=");hex(hle[0]);text(" hle_rsp=");hex(hle[1]);
        text(" hle_args=[");
        for(unsigned i=2;i<8;++i) { if(i!=2)text(",");hex(hle[i]); }
        text("]");
        std::uint64_t ret{};vm_size_t copied{};
        if(vm_read_overwrite(mach_task_self(),hle[1],sizeof(ret),
            reinterpret_cast<vm_address_t>(&ret),&copied)==KERN_SUCCESS && copied==sizeof(ret)) {
            text(" hle_return=");hex(ret);
            std::uint8_t code[128]{};
            if(ret>=96 && vm_read_overwrite(mach_task_self(),ret-96,sizeof(code),
                reinterpret_cast<vm_address_t>(code),&copied)==KERN_SUCCESS && copied==sizeof(code)) {
                text(" hle_code_from=");hex(ret-96);text(" hle_code=[");
                for(unsigned i=0;i<sizeof(code);++i) { if(i)text(",");hex(code[i]); }
                text("]");
            }
        }
    }
#if defined(LSX4_APPLE_RUNTIME)
    text(" published_guest_rip=");hex(executor_jit_current_fault_guest_rip());
    text(" active_block_rip=");hex(executor_jit_current_fault_active_block_rip());
    text(" published_instruction=");hex(executor_jit_current_fault_instruction());
    text(" published_instruction_meta=");hex(executor_jit_current_fault_instruction_meta());
#endif
    text("\n");
    (void)write(STDERR_FILENO,buffer,static_cast<std::size_t>(out-buffer));
}
// Explicit terminal-only probe. This is not the guest fault/repair owner.
// Install only for a diagnostic launch; no work is added to successful calls.
static void AppleProbeAbortStack(int signal, siginfo_t* info, void* context) noexcept {
    WriteAppleFaultContext("[APPLE_UNHANDLED_SIGNAL]", signal, info, context, true);
    _exit(128 + signal);
}
__attribute__((constructor)) static void InstallAppleProbeAbortStack() {
    const char* enabled = std::getenv("LSX4_PROBE_ABORT_STACK");
    if (!enabled || enabled[0] != '1' || enabled[1] != '\0') return;
    struct sigaction action{};
    action.sa_sigaction = AppleProbeAbortStack;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    // A launch without a successfully installed probe must fail visibly.
    if (sigaction(SIGABRT, &action, nullptr) != 0) _exit(125);
}
extern "C" void executor_lsx4_apple_note_unhandled_signal(
    int signal, const siginfo_t* info, const void* rawContext) noexcept {
    WriteAppleFaultContext("[APPLE_UNHANDLED_SIGNAL]",signal,info,rawContext,true);
}
extern "C" void executor_lsx4_apple_note_guest_signal(
    int signal, const siginfo_t* info, const void* rawContext) noexcept {
    // Preserve the original context before a nested guest handler can replace
    // its published state or fail itself. Benign guest null probes are bounded.
    static std::atomic<unsigned> count{0};
    static_assert(decltype(count)::is_always_lock_free);
    if(count.load(std::memory_order_relaxed)>=4 ||
       count.fetch_add(1,std::memory_order_relaxed)>=4) return;
    WriteAppleFaultContext("[APPLE_GUEST_SIGNAL_ORIGIN]",signal,info,rawContext);
}
namespace Lsx4::Apple {
static std::atomic<DiagnosticFrameCapture> frameCapture{};
void RequestDiagnosticFrameCapture(DiagnosticFrameCapture capture) noexcept {
    frameCapture.store(capture, std::memory_order_release);
}
DiagnosticFrameCapture TakeDiagnosticFrameCapture() noexcept {
    if (!frameCapture.load(std::memory_order_relaxed)) return nullptr;
    return frameCapture.exchange(nullptr, std::memory_order_acquire);
}
std::uint64_t ContinuousTimeNanoseconds() noexcept {
    // mach_continuous_time includes system sleep, matching Linux BOOTTIME.
    static const auto scale=[] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    return static_cast<std::uint64_t>(
        static_cast<unsigned __int128>(mach_continuous_time()) * scale.numer / scale.denom);
}
bool QueryHostRange(std::uint64_t address,int requiredProtection,std::uint64_t& first,std::uint64_t& last) noexcept {
    first=last=0;
    // The dyld shared cache is a submap. Its outer protection is not the
    // permission of a contained libSystem/libm function; inspect the leaf.
    natural_t depth=0;
    while(depth<64) {
        vm_address_t base=address;vm_size_t bytes{};
        vm_region_submap_info_data_64_t info{};
        mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
        const auto status=vm_region_recurse_64(mach_task_self(),&base,&bytes,&depth,
            reinterpret_cast<vm_region_recurse_info_t>(&info),&count);
        if(status!=KERN_SUCCESS || address<base || address-base>=bytes ||
            bytes>UINT64_MAX-base) return false;
        if(info.is_submap) { ++depth;continue; }
        if((info.protection & requiredProtection)!=requiredProtection) return false;
        first=base;last=base+bytes;return true;
    }
    return false;
}
bool QueryReadableHostRange(std::uint64_t address,std::uint64_t& first,std::uint64_t& last) noexcept {
    return QueryHostRange(address,VM_PROT_READ,first,last);
}
void* ReserveFixedHostRange(std::uint64_t address,std::size_t bytes) noexcept {
    vm_address_t base=address;
    if(!bytes || bytes>UINT64_MAX-address ||
        vm_allocate(mach_task_self(),&base,bytes,VM_FLAGS_FIXED)!=KERN_SUCCESS) return MAP_FAILED;
    return reinterpret_cast<void*>(base);
}
}
