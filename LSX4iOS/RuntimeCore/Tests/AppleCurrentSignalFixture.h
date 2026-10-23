// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/libraries/kernel/threads/exception.h"
#include "core/signals.h"
#include "core/libraries/kernel/threads/pthread.h"
#include <unistd.h>
namespace Libraries::Kernel {
s32 PS4_SYSV_ABI posix_sigaction(s32,Sigaction*,Sigaction*);
int PS4_SYSV_ABI sceKernelInstallExceptionHandler(s32,OrbisKernelExceptionHandler);
int PS4_SYSV_ABI sceKernelRemoveExceptionHandler(s32);
s32 PS4_SYSV_ABI posix_pthread_kill(PthreadT,s32);
}

namespace AppleCurrentSignalFixture {
using namespace Libraries::Kernel;
static u64 expected_pc{}, continuation{}, observed_pc{}, observed_rax{}, observed_r10{};
static unsigned delivered{};
static int observed_mask{};
static void HandleContext(s32 signal,Ucontext* context) {
    // Bound a broken native-resume loop in this standalone diagnostic process.
    if(++delivered>1) _exit(77);
    if(signal!=POSIX_SIGSEGV || !context) _exit(78);
    sigset_t mask{};pthread_sigmask(SIG_SETMASK,nullptr,&mask);
    observed_mask=sigismember(&mask,SIGSEGV);
    observed_pc=context->uc_mcontext.mc_rip;
    observed_rax=context->uc_mcontext.mc_rax;
    observed_r10=context->uc_mcontext.mc_r10;
    context->uc_mcontext.mc_rip=continuation;
    context->uc_mcontext.mc_rax=35;
}
static void Handle(s32 signal,Siginfo* info,Ucontext* context) {
    if(!info || reinterpret_cast<u64>(info->_si_addr)!=0x10000) _exit(79);
    HandleContext(signal,context);
}
static void HandleKernel(s32 signal,void* context){HandleContext(signal,static_cast<Ucontext*>(context));}
static unsigned async_delivered{}, async_depth{}, async_max_depth{};
static bool host_critical{}, unsafe_delivery{}, async_context_valid{};
static void HandleAsyncKernel(s32 signal,void* raw_context) {
    auto* context=static_cast<Ucontext*>(raw_context);
    ++async_depth;async_max_depth=std::max(async_max_depth,async_depth);
    ++async_delivered;
    unsafe_delivery|=host_critical;
    async_context_valid&=signal==POSIX_SIGUSR1 && context &&
        context->uc_mcontext.mc_rbx==0x123456789ab0ULL && context->uc_mcontext.mc_rsp!=0;
    // One same-number pending delivery is retained; extra sends coalesce.
    if(async_delivered==1) for(unsigned i=0;i<3;++i)
        Require(posix_pthread_kill(g_curthread,POSIX_SIGUSR1)==0,"Nested async send failed");
    Require(async_delivered<=2,"Async signal recursively duplicated");
    --async_depth;
}
static u64 SendAsyncFromHost() {
    host_critical=true;
    for(unsigned i=0;i<3;++i)
        Require(posix_pthread_kill(g_curthread,POSIX_SIGUSR1)==0,"Async send failed");
    unsafe_delivery|=async_delivered!=0;
    host_critical=false;
    return 42;
}
static void RunAsync(void* mapping) {
    Pthread target{};target.name="AppleCurrentAsyncSignalFixture";
    const auto prior=g_curthread;
    stack_t old_stack{};sigaltstack(nullptr,&old_stack);
    target.native_thr.Initialize();g_curthread=&target;
    Sigaction old{};Require(posix_sigaction(POSIX_SIGUSR1,nullptr,&old)==0,"Read async action failed");
    struct Cleanup {
        Pthread& target;Pthread* prior;stack_t old_stack;Sigaction old;
        ~Cleanup(){posix_sigaction(POSIX_SIGUSR1,&old,nullptr);g_curthread=prior;
            target.native_thr.CleanupBeforeReturn();sigaltstack(&old_stack,nullptr);}
    } cleanup{target,prior,old_stack,old};
    const auto handler=Core::AeroLib::GetAndroidX64HleStubForNative("apple-async-kernel-handler",&HandleAsyncKernel);
    Require(sceKernelRemoveExceptionHandler(POSIX_SIGUSR1)==0 &&
        sceKernelInstallExceptionHandler(POSIX_SIGUSR1,reinterpret_cast<OrbisKernelExceptionHandler>(handler))==0,
        "Async kernel handler registration failed");
    const auto sender=Core::AeroLib::GetAndroidX64HleStubForNative("apple-async-host-critical",&SendAsyncFromHost);
    auto code=AppleCurrentHleTokenFixture::Call(sender);
    std::memcpy(mapping,code.data(),code.size());
    for(unsigned repeat=0;repeat<2;++repeat) {
        async_delivered=async_depth=async_max_depth=0;unsafe_delivery=false;async_context_valid=true;
        Executor::Jit::LsxEntryPacket packet{};
        const auto result=Executor::Jit::DispatchGuestOnArm64(reinterpret_cast<u64>(mapping),packet);
        Require(result==42 && async_delivered==2 && !unsafe_delivery && async_max_depth==1 &&
            async_context_valid && target.fex_hle_atomic_flags.load()==0 && target.fex_hle_atomic_depth.load()==0,
            "Current async signal must defer host critical work, preserve context, coalesce and drain without recursion");
        std::fprintf(stderr,"[APPLE_ASYNC_SIGNAL_FIXTURE] repeat=%u deliveries=%u maxDepth=%u deferred=%u context=%u result=%llu\n",
            repeat,async_delivered,async_max_depth,!unsafe_delivery,async_context_valid,result);
    }
}
static void Run(Core::MemoryManager* memory) {
    using namespace Executor::Jit;
    (void)Core::Signals::Instance();
    constexpr std::size_t bytes=16u<<10;
    // The preceding HLE fixture translated its temporary VA. Select a fresh
    // address: direct host writes are not guest SMC notifications, and a saved
    // title profile may explicitly disable instruction-cache inspection.
    const VAddr fixture_hint=memory->GetAddressSpace().UserVirtualBase()+(64ULL<<20);
    void* mapping{};
    Require(memory->MapMemory(&mapping,fixture_hint,bytes,
        Core::MemoryProt::CpuReadWrite,Core::MemoryMapFlags{},Core::VMAType::Flexible,
        "apple-current-signal-test",false)==0,"Signal fixture mapping failed");
    Require(reinterpret_cast<VAddr>(mapping)>=fixture_hint,"Signal fixture reused a prior code address");
    const auto previous=Executor::CpuTuning::Get();
    auto profile=previous;
    profile.execution_mode=Executor::CpuTuning::ExecutionMode::Jit;
    profile.persistent_cache=Executor::CpuTuning::PersistentCacheMode::Off;
    profile.memory_exceptions=1;profile.ignore_bad_accesses=0;
    Executor::CpuTuning::SetForTesting(profile);
    const auto handler=Core::AeroLib::GetAndroidX64HleStubForNative("apple-signal-fixture",&Handle);
    const auto kernel_handler=Core::AeroLib::GetAndroidX64HleStubForNative("apple-signal-kernel-fixture",&HandleKernel);
    Sigaction action{},old{};
    action.sa_flags=POSIX_SA_SIGINFO;
    action.__sigaction_handler.sigaction=reinterpret_cast<decltype(action.__sigaction_handler.sigaction)>(handler);
    Require(posix_sigaction(POSIX_SIGSEGV,&action,&old)==0,"Signal fixture handler registration failed");
    struct Cleanup {
        Core::MemoryManager* memory; void* mapping; Sigaction old;
        Executor::CpuTuning::ProfileV1 profile;
        ~Cleanup(){posix_sigaction(POSIX_SIGSEGV,&old,nullptr);
            Executor::CpuTuning::SetForTesting(profile);
            memory->UnmapMemory(reinterpret_cast<VAddr>(mapping),bytes);}
    } cleanup{memory,mapping,old,previous};
    sigset_t original_mask{};pthread_sigmask(SIG_SETMASK,nullptr,&original_mask);
    const int original_segv=sigismember(&original_mask,SIGSEGV);
    for(unsigned interpreter=0;interpreter<2;++interpreter)
    for(unsigned kernel=0;kernel<2;++kernel) {
    profile.execution_mode=interpreter?Executor::CpuTuning::ExecutionMode::Interpreter:
        Executor::CpuTuning::ExecutionMode::Jit;
    Executor::CpuTuning::SetForTesting(profile);
    Require(sceKernelRemoveExceptionHandler(POSIX_SIGSEGV)==0,"Signal fixture cleanup failed");
    Require(kernel?sceKernelInstallExceptionHandler(POSIX_SIGSEGV,
        reinterpret_cast<OrbisKernelExceptionHandler>(kernel_handler))==0:
        posix_sigaction(POSIX_SIGSEGV,&action,nullptr)==0,"Signal fixture install failed");
    // Fault after changing registers inside the same native block. A stale
    // dispatch-boundary snapshot cannot substitute for the exact fault image.
    auto* phase=static_cast<u8*>(mapping)+(interpreter*2+kernel)*256;
    constexpr u64 vector_canary=0x76543210abcdef09ULL;
    std::vector<u8> code{0x48,0xb8};AppleCurrentHleTokenFixture::Put64(code,vector_canary);
    code.insert(code.end(),{0x66,0x48,0x0f,0x6e,0xc0,0x48,0xb8});
    AppleCurrentHleTokenFixture::Put64(code,0x12345678);
    code.insert(code.end(),{0x49,0xba});AppleCurrentHleTokenFixture::Put64(code,0x34567890);
    expected_pc=reinterpret_cast<u64>(phase)+code.size();continuation=expected_pc+3;
    code.insert(code.end(),{0x48,0x8b,0x07,0x66,0x0f,0xd6,0x06,0x48,0x83,0xc0,0x07,0xc3});
    std::memcpy(phase,code.data(),code.size());
    auto* vector_result=reinterpret_cast<u64*>(static_cast<u8*>(mapping)+8192);
    for(unsigned repeat=0;repeat<2;++repeat){
    delivered=0;
    *vector_result=0;
    LsxEntryPacket packet{};packet.arg_count=2;packet.args[0]=0x10000;
    packet.args[1]=reinterpret_cast<u64>(vector_result);
    const auto result=DispatchGuestOnArm64(reinterpret_cast<u64>(phase),packet);
    std::fprintf(stderr,"[APPLE_SIGNAL_FIXTURE] interpreter=%u kernel=%u repeat=%u deliveries=%u pc=0x%llx expected=0x%llx rax=0x%llx r10=0x%llx result=%llu vector=0x%llx mask=%d\n",
        interpreter,kernel,repeat,delivered,observed_pc,expected_pc,observed_rax,observed_r10,result,*vector_result,observed_mask);
    char detail[512]{};
    std::snprintf(detail,sizeof(detail),"Guest signal mismatch: interpreter=%u kernel=%u repeat=%u deliveries=%u pc=0x%llx expected=0x%llx rax=0x%llx r10=0x%llx result=%llu vector=0x%llx mask=%d expectedMask=%d",
        interpreter,kernel,repeat,delivered,observed_pc,expected_pc,observed_rax,observed_r10,result,*vector_result,observed_mask,original_segv);
    Require(delivered==1 && observed_pc==expected_pc && observed_rax==0x12345678 &&
            observed_r10==0x34567890 && result==42 && *vector_result==vector_canary && observed_mask==original_segv,
            detail);
    }
    }
    profile.execution_mode=Executor::CpuTuning::ExecutionMode::Jit;
    Executor::CpuTuning::SetForTesting(profile);
    RunAsync(static_cast<u8*>(mapping)+4096);
}
}
