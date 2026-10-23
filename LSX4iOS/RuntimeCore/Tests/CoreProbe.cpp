// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded CPU/host ABI fixture, not a replacement game runtime or HLE library.
#include "executor/dynamic_translation/retiring_execution_core.h"
#include "executor/dynamic_translation/process_memory.h"
#include "executor/dynamic_translation/runtime_bridge_api.h"
#include "executor/dynamic_translation/runtime_gateway.h"
#include "executor/cpu_tuning.h"
#include "core/aerolib/hle_abi.h"
#include "../AppleGuestMemory.h"
#include "ScopedBackingFixture.h"
#include "ScalarI64BoundaryFixture.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <TargetConditionals.h>
#include <mach/mach.h>
extern "C" int LSX4MissingOptionalBackendProbe() __attribute__((weak_import));

unsigned LSX4RunSharedHleAbiProbe(void* storage);
// This isolated CPU harness has no guest kernel. FullConsoleRuntimeProbe owns
// the real positive signal-delivery/continuation contract.
extern "C" int executor_lsx4_android_dispatch_deferred_guest_signal(
    std::int32_t,std::int32_t,std::int32_t,std::int32_t,std::uint32_t,
    std::uint64_t,std::uint64_t,std::int32_t) { return 0; }

namespace {
struct Range { std::uint64_t base; std::size_t bytes; bool executable; };
std::vector<Range> ranges;
constexpr std::uint64_t TestThunk = UINT64_C(0x180001000);
std::uint64_t hleCalls{};
std::string probeReport;
void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
bool Contains(std::uint64_t address, std::size_t bytes, bool executable = false) {
    for (const auto& range : ranges)
        if ((!executable || range.executable) && address >= range.base &&
            address - range.base <= range.bytes && bytes <= range.bytes - (address - range.base))
            return true;
    return false;
}
std::uint64_t TestHle(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                      std::uint64_t d, std::uint64_t e, std::uint64_t f) {
    ++hleCalls;
    return a + 3*b + 5*c + 7*d + 11*e + 13*f;
}
}

extern "C" bool ExecutorJitIsReadableGuestRange(std::uint64_t a, std::size_t n) { return Contains(a,n); }
extern "C" bool ExecutorJitIsExecutableGuestAddress(std::uint64_t a) { return Contains(a,1,true); }
extern "C" bool ExecutorJitIsWritableGuestAddress(std::uint64_t a) { return Contains(a,1); }
extern "C" bool ExecutorJitReadGuestBytes(std::uint64_t a, void* p, std::size_t n) {
    return Contains(a,n) && Lsx4::Translation::ReadProcessGuestMemory(a,p,n);
}
extern "C" bool ExecutorJitReadGuestBytesStable(std::uint64_t a, void* p, std::size_t n) {
    return ExecutorJitReadGuestBytes(a,p,n);
}
extern "C" bool ExecutorJitWriteGuestBytes(std::uint64_t a, const void* p, std::size_t n) {
    return Contains(a,n) && Lsx4::Translation::WriteProcessGuestMemory(a,p,n);
}
extern "C" void executor_lsx4_android_register_guest_readable_range(const void* p, std::size_t n, const char*) {
    ranges.push_back({reinterpret_cast<std::uint64_t>(p),n,false});
}
extern "C" void executor_lsx4_android_note_guest_stack_window(const void* p, std::size_t n, const char* label) {
    executor_lsx4_android_register_guest_readable_range(p,n,label);
}
// Apple HLE slabs can sit below the legacy high-address heuristic. Exercise
// the same exact slab route with a low, synthetic thunk in this CPU fixture.
extern "C" std::uint64_t executor_jit_ensure_hle_thunk_slab() { return TestThunk; }
extern "C" std::uint64_t executor_jit_hle_thunk_slab_base() { return TestThunk; }
extern "C" std::uint64_t executor_jit_hle_thunk_slab_size() { return 1024; }
extern "C" int executor_jit_lookup_hle_thunk(std::uint64_t a, std::uint64_t* native) {
    const bool registered = a == TestThunk || a == reinterpret_cast<std::uint64_t>(&TestHle);
    if (native) *native = registered ? reinterpret_cast<std::uint64_t>(&TestHle) : 0;
    return registered;
}
extern "C" int executor_jit_lookup_leaf_hle_thunk(std::uint64_t, std::uint64_t* native) {
    if (native) *native = 0;
    return 0; // The fixture registers no leaf functions.
}
extern "C" int executor_jit_hle_thunk_returns_zero(std::uint64_t) { return 0; }
extern "C" int executor_jit_classify_hle_thunk(std::uint64_t a) { return executor_jit_lookup_hle_thunk(a,nullptr) ? 2 : 0; }
extern "C" int executor_jit_resolve_hle_thunk(std::uint64_t a, std::uint64_t* native) {
    return executor_jit_lookup_hle_thunk(a,native) ? 2 : 0;
}
extern "C" int executor_jit_hle_fp_result_kind(std::uint64_t) { return 0; }
extern "C" int executor_jit_hle_fp_bridge_selftest() { throw std::runtime_error("FP fixture not registered"); }
namespace Lsx4::Translation {
static HleLeafOutcome RejectFixtureLeaf(std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) {
    return {}; // Correct negative lookup ABI: x0=value, x1=accepted.
}
static std::uint64_t UnexpectedResolvedLeaf(std::uint64_t, std::uint64_t, std::uint64_t,
    std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) {
    std::abort(); // No fixture registered this route; do not manufacture a result.
}
std::uintptr_t SelectiveLeafBridgeEntry() noexcept { return reinterpret_cast<std::uintptr_t>(&RejectFixtureLeaf); }
std::uintptr_t ResolvedLeafBridgeEntry() noexcept { return reinterpret_cast<std::uintptr_t>(&UnexpectedResolvedLeaf); }
std::uint64_t InvokeRuntimeHle(const HleBridgeRequest& request) {
    Check(request.function == reinterpret_cast<std::uint64_t>(&TestHle), "unknown fixture HLE");
    Core::AeroLib::AndroidX64HleRawCall raw{};
    Core::AeroLib::AndroidX64HleRawResult result{};
    raw.native_function=request.function;
    raw.guest_rsp=request.guest_stack;
    std::copy(request.integer_arguments.begin(),request.integer_arguments.end(),raw.gpr.begin());
    constexpr auto descriptor=Core::AeroLib::HleAbiDetail::MakeDescriptor<decltype(&TestHle)>();
    Check(descriptor.invoke(request.function,raw,result),"shared typed HLE marshaller rejected fixture");
    return result.rax;
}
}

const char* LSX4SharedCoreProbeReport() { return probeReport.c_str(); }
int LSX4RunSharedCoreProbe(bool interpreter) {
    try {
        using namespace Executor::Jit;
        Check(LSX4MissingOptionalBackendProbe==nullptr,"absent Mach-O optional backend must be null");
        if (std::getenv("LSX4_TEST_UNLINKED_NATIVE"))
            Executor::CpuTuning::MutableProfile().block_linking = 0;
        if (interpreter)
            Executor::CpuTuning::MutableProfile().execution_mode = Executor::CpuTuning::ExecutionMode::Interpreter;
        Check(ExerciseOwnedGuestStackAccess(),"owned stack access must respect complete spans, leases and imported VMAs");
        Check(ExerciseIndirectEdgeCacheLifetime(),"indirect PIC must retain one edge per destination across eviction and races");
        const auto page = std::size_t(getpagesize());
        auto* memory = static_cast<std::uint8_t*>(mmap(nullptr, 2*page,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        Check(memory != MAP_FAILED, "fixture memory allocation");
        ranges.push_back({reinterpret_cast<std::uint64_t>(memory),2*page,true});
        std::uint64_t scratch{};
        Check(!Lsx4::Translation::ReadProcessGuestScalar(0x10000,scratch), "invalid read rejected");
        Check(!Lsx4::Translation::WriteProcessGuestScalar(0x10000,42), "invalid write rejected");
        Check(!Lsx4::Translation::WriteProcessGuestScalar(reinterpret_cast<std::uint64_t>(&LSX4RunSharedCoreProbe),42), "host image write rejected");
        Check(mprotect(memory+page,page,PROT_NONE)==0, "guard page");
        Check(!Lsx4::Translation::ReadProcessGuestMemory(reinterpret_cast<std::uint64_t>(memory+page-4), &scratch,8), "cross-page read rejected");
        Check(mprotect(memory+page,page,PROT_READ)==0, "read-only page");
        Check(!Lsx4::Translation::WriteProcessGuestScalar(reinterpret_cast<std::uint64_t>(memory+page),42), "read-only write rejected");

        if (!interpreter) {
            Check(ExerciseScalarMoveMemoryContract(memory,page),"scalar SIMD memory loads clear unused XMM lanes and stores preserve adjacent bytes");
            Check(ExerciseInstructionHelperCallBoundary(memory,page),"instruction helpers must preserve registers for arbitrary guest CALLs");
            Check(ExerciseStaticEdgeAdapter(memory,page),"static edge adapter must publish full GPR/SIMD state to a resident target");
            Check(ExerciseNativeFaultDescriptorCapacity(memory,page),
                  "native recovery descriptor capacity/publication/replay contract");
            Check(ExerciseNativeSemanticDescriptorStorage(memory,page),
                  "native semantic descriptor storage/lifetime contract");
        }

        // sum 1..n with native loop branches, store through guest pointer, return.
        const std::array<std::uint8_t,18> sum{0x31,0xc0,0x48,0x85,0xff,0x74,0x08,
            0x48,0x01,0xf8,0x48,0xff,0xcf,0x75,0xf8,0x48,0x89,0x06};
        std::memcpy(memory,sum.data(),sum.size()); memory[sum.size()] = 0xc3;
        LsxEntryPacket call{}; call.arg_count=2; call.args[0]=10000;
        call.args[1]=reinterpret_cast<std::uint64_t>(memory+1024);
        const auto result=DispatchGuestOnArm64(reinterpret_cast<std::uint64_t>(memory),call);
        std::memcpy(&scratch,memory+1024,8);
        Check(result==50005000 && scratch==result,"loop/memory/native return mismatch");

        if (!interpreter && std::getenv("LSX4_TEST_FROZEN_LOOKUP")) {
            // A read-only holder of the configuration mutex must not serialize
            // already compiled dispatch. No configuration is changed while the
            // guest executes; the pre-launch Frozen() contract still applies.
            // The deadline releases the old implementation instead of hanging.
            std::atomic<bool> held{false}, finished{false}, timedOut{false};
            std::thread holder([&] {
                std::unique_lock lock{Executor::CpuTuning::ProfileMutex()};
                held.store(true, std::memory_order_release);
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(!finished.load(std::memory_order_acquire) &&
                      std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
                timedOut.store(!finished.load(std::memory_order_acquire),std::memory_order_release);
            });
            while(!held.load(std::memory_order_acquire)) std::this_thread::yield();
            std::uint64_t repeated{};
            try {
                repeated=DispatchGuestOnArm64(reinterpret_cast<std::uint64_t>(memory),call);
            } catch(...) {
                finished.store(true,std::memory_order_release);holder.join();throw;
            }
            finished.store(true,std::memory_order_release);holder.join();
            Check(repeated==result,"warm dispatch changed guest result");
            Check(!timedOut.load(std::memory_order_acquire),
                  "warm dispatch waited for the configuration mutex");
            std::fprintf(stderr,"[APPLE_FROZEN_LOOKUP_TEST] passed result=%llu mutexHeld=1\n",
                         static_cast<unsigned long long>(repeated));
        }

        // Indirect guest call crosses the shared HLE continuation machinery.
        const std::array<std::uint8_t,4> prefix{0x48,0x83,0xec,0x08};
        std::memcpy(memory+64,prefix.data(),prefix.size());
        memory[68]=0x48; memory[69]=0xb8;
        std::memcpy(memory+70,&TestThunk,8);
        const std::array<std::uint8_t,7> suffix{0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
        std::memcpy(memory+78,suffix.data(),suffix.size());
        call.arg_count=6; call.args={1,2,3,4,5,6,0,0};
        const auto hleResult=DispatchGuestOnArm64(reinterpret_cast<std::uint64_t>(memory+64),call);
        Check(hleResult==183 && hleCalls==1,"HLE arguments/return continuation mismatch");
        // Imported libc functions may resolve directly to a native Mach-O
        // wrapper, outside the synthetic thunk slab and below 1 TiB.
        const auto nativeHle = reinterpret_cast<std::uint64_t>(&TestHle);
        Check((nativeHle >> 40u)==0 && (nativeHle<TestThunk || nativeHle-TestThunk>=1024),
              "raw native HLE fixture must exercise the low address outside the slab");
        std::memcpy(memory+96,memory+64,21);
        std::memcpy(memory+102,&nativeHle,8);
        const auto rawHleResult=DispatchGuestOnArm64(reinterpret_cast<std::uint64_t>(memory+96),call);
        Check(rawHleResult==183 && hleCalls==2,"raw native HLE arguments/return continuation mismatch");
        const auto typedAbiCases=LSX4RunSharedHleAbiProbe(memory+2048);
        if(!interpreter) {
            // Android's Giga profile permits 512 instructions: valid x86
            // blocks can exceed a 2-KiB validation scratch buffer. Exercise
            // the actual decoder/validator/native executor with 3010+ bytes.
            auto* giga=static_cast<std::uint8_t*>(mmap(nullptr,page,
                PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0));
            Check(giga!=MAP_FAILED,"Giga fixture mapping");
            ranges.push_back({reinterpret_cast<std::uint64_t>(giga),page,true});
            for(std::uint64_t i=0;i<301;++i) {
                giga[i*10]=0x48;giga[i*10+1]=0xb8;
                std::memcpy(giga+i*10+2,&i,8);
            }
            giga[3010]=0xc3;
            const auto previous=Executor::CpuTuning::Get();auto large=previous;
            large.jit_block_size=Executor::CpuTuning::BlockSizeMode::Giga;
            Executor::CpuTuning::SetForTesting(large);
            LsxEntryPacket noArgs{};
            const auto value=DispatchGuestOnArm64(reinterpret_cast<std::uint64_t>(giga),noArgs);
            Check(value==300,"Giga block must validate and execute beyond 2 KiB");
            const std::uint64_t replacement=999;
            std::memcpy(giga+3002,&replacement,8);
            Check(DispatchGuestOnArm64(reinterpret_cast<std::uint64_t>(giga),noArgs)==replacement,
                "Giga witness must detect a code mutation beyond the old 2-KiB boundary");
            Executor::CpuTuning::SetForTesting(previous);
            // Published witnesses retain guest addresses for their lifetime.
            // Keep this one bounded page until the fixture process exits.
        }
        // Probe canonical guest VA availability with one untouched page at a
        // time. FIXED without OVERWRITE cannot replace an existing mapping.
        std::string addressSpace="[";
        for(const std::uint64_t requested : {UINT64_C(0x200000000),UINT64_C(0x800000000),
            UINT64_C(0x1000000000),UINT64_C(0x2000000000),UINT64_C(0x3000000000),
            UINT64_C(0x4000000000),UINT64_C(0xc000000000)}) {
            vm_address_t address=requested;
            const auto mapped=vm_allocate(mach_task_self(),&address,page,VM_FLAGS_FIXED);
            if(mapped==KERN_SUCCESS)
                Check(vm_deallocate(mach_task_self(),address,page)==KERN_SUCCESS,"release VA capability probe");
            char item[96]{};
            std::snprintf(item,sizeof(item),"%s{\"address\":%llu,\"kernelResult\":%d}",
                addressSpace.size()>1?",":"",static_cast<unsigned long long>(requested),mapped);
            addressSpace+=item;
        }
        addressSpace+="]";
        task_vm_info_data_t vmInfo{};
        mach_msg_type_number_t vmInfoCount=TASK_VM_INFO_COUNT;
        const bool hasVmInfo=task_info(mach_task_self(),TASK_VM_INFO,
            reinterpret_cast<task_info_t>(&vmInfo),&vmInfoCount)==KERN_SUCCESS &&
            vmInfoCount>=TASK_VM_INFO_REV2_COUNT;
        std::size_t compactGuestBytes{};
        {
            Lsx4::Apple::GuestMemoryLayout guestLibcLayout(5888ULL<<20,false);
            Check(guestLibcLayout.userBytes>=3ULL<<30,
                  "guest-libc arena must accommodate a full 3 GiB GPU reservation");
        }
        {
            Lsx4::Apple::GuestMemoryLayout layout(5888ULL<<20);
            compactGuestBytes=layout.GuestBytes();
            auto* aliasA=mmap(layout.guest,page,PROT_READ|PROT_WRITE,
                MAP_SHARED|MAP_FIXED,layout.backingFd,0);
            auto* aliasB=mmap(layout.guest+page,page,PROT_READ|PROT_WRITE,
                MAP_SHARED|MAP_FIXED,layout.backingFd,0);
            Check(aliasA==layout.guest && aliasB==layout.guest+page,"compact physical aliases map");
            *static_cast<volatile std::uint64_t*>(aliasA)=UINT64_C(0x91e24ba350c76df8);
            const auto backing=layout.AcquireBackingSpan(0,8);
            Check(*static_cast<volatile std::uint64_t*>(aliasB)==UINT64_C(0x91e24ba350c76df8) &&
                *reinterpret_cast<volatile std::uint64_t*>(backing.data)==UINT64_C(0x91e24ba350c76df8),
                "compact physical aliases disagree");
            ScopedBackingFixture::RunLeases(layout,16ULL<<20,Check);
        }

        ScalarI64BoundaryFixture::Run(memory+8192,Check);

        // Real packed integer SIMD guest instructions, including lane wraparound.
        const std::array<std::uint8_t,19> vectorCode{
            0xf3,0x0f,0x6f,0x07, 0xf3,0x0f,0x6f,0x0e,
            0x66,0x0f,0xfe,0xc1, 0xf3,0x0f,0x7f,0x02, 0x8b,0x02,0xc3};
        std::memcpy(memory+128,vectorCode.data(),vectorCode.size());
        const std::array<std::uint32_t,4> lhs{1,0xffffffff,0x7fffffff,0x80000000};
        const std::array<std::uint32_t,4> rhs{5,2,1,0xffffffff};
        std::memcpy(memory+1104,lhs.data(),16);std::memcpy(memory+1120,rhs.data(),16);
        call.arg_count=3;call.args={reinterpret_cast<std::uint64_t>(memory+1104),
            reinterpret_cast<std::uint64_t>(memory+1120),reinterpret_cast<std::uint64_t>(memory+1136)};
        const auto vectorResult=DispatchGuestOnArm64(reinterpret_cast<std::uint64_t>(memory+128),call);
        std::array<std::uint32_t,4> vectorOutput{};std::memcpy(vectorOutput.data(),memory+1136,16);
        Check(vectorResult==6 && vectorOutput==std::array<std::uint32_t,4>{6,1,0x80000000,0x7fffffff},
              "packed SIMD guest lane/overflow mismatch");
        // Streaming PS/PD stores are bit transfers. Exercise both encodings
        // and widths, surrounding memory, and the untouched source YMM upper
        // half through the real decoder and execution path.
        struct StreamingStore { const char* name; std::vector<std::uint8_t> opcode; std::size_t width; };
        const std::array<StreamingStore,6> streamingStores{{
            {"movntps128",{0x0f,0x2b,0x06},16},
            {"movntpd128",{0x66,0x0f,0x2b,0x06},16},
            {"vmovntps128",{0xc5,0xf8,0x2b,0x06},16},
            {"vmovntpd128",{0xc5,0xf9,0x2b,0x06},16},
            {"vmovntps256",{0xc5,0xfc,0x2b,0x06},32},
            {"vmovntpd256",{0xc5,0xfd,0x2b,0x06},32},
        }};
        for(std::size_t index=0;index<streamingStores.size();++index) {
            const auto& test=streamingStores[index];
            std::vector<std::uint8_t> code{0xc5,0xfc,0x10,0x07}; // vmovups ymm0,[rdi]
            code.insert(code.end(),test.opcode.begin(),test.opcode.end());
            const std::array<std::uint8_t,13> tail{
                0xc5,0xfc,0x11,0x02, // vmovups [rdx],ymm0
                0x0f,0xae,0xf8,      // sfence
                0xb8,0x07,0,0,0,0xc3};
            code.insert(code.end(),tail.begin(),tail.end());
            auto* entry=memory+512+index*64;
            std::memcpy(entry,code.data(),code.size());
            auto* destination=memory+4096;
            auto* source=memory+4352;
            auto* witness=memory+4480;
            for(std::size_t byte=0;byte<32;++byte) source[byte]=std::uint8_t(0x91+byte*17);
            std::memset(destination,0xa5,96);std::memset(witness,0,32);
            call.arg_count=3;call.args={reinterpret_cast<std::uint64_t>(source),
                reinterpret_cast<std::uint64_t>(destination+32),reinterpret_cast<std::uint64_t>(witness)};
            Check(DispatchGuestOnArm64(reinterpret_cast<std::uint64_t>(entry),call)==7,test.name);
            Check(std::memcmp(destination+32,source,test.width)==0,"streaming store bit/width mismatch");
            Check(std::all_of(destination,destination+32,[](auto v){return v==0xa5;}) &&
                std::all_of(destination+32+test.width,destination+96,[](auto v){return v==0xa5;}),
                "streaming store overwrote adjacent bytes");
            Check(std::memcmp(witness,source,32)==0,"streaming store changed source YMM state");
            std::printf("[STREAMING_STORE_CONTRACT] %s passed\n",test.name);
        }
        const auto stats=SnapshotTranslationTelemetry();
        if (!interpreter) Check(stats.native_blocks>0 && stats.unsupported_blocks==0,"native coverage missing");
        char report[2048]{};
        std::snprintf(report,sizeof(report),"{\"status\":\"passed\",\"sum\":%llu,\"hleResult\":%llu,\"hleCalls\":%llu,\"nativeBlocks\":%llu,\"helperBlocks\":%llu,\"unsupportedBlocks\":%llu,\"typedAbiCases\":%u,\"packedSimd\":true,\"addressSpace\":%s,\"vmMinimum\":%llu,\"vmMaximum\":%llu,\"compactGuestBytes\":%llu,\"physicalAliasing\":true}",
            result,hleResult,hleCalls,stats.native_blocks,stats.helper_blocks,stats.unsupported_blocks,typedAbiCases,addressSpace.c_str(),
            static_cast<unsigned long long>(hasVmInfo?vmInfo.min_address:0),
            static_cast<unsigned long long>(hasVmInfo?vmInfo.max_address:0),
            static_cast<unsigned long long>(compactGuestBytes));
        probeReport=report;
        std::puts(report);
        // Keep mappings alive through the core's translation-cache destructor.
        return 0;
    } catch(const std::exception& error) {
        probeReport=std::string("failed: ")+error.what();
        std::fprintf(stderr,"shared core probe failed: %s\n",error.what());
        return 1;
    }
}
#if !TARGET_OS_IPHONE
int LSX4RunWarmProfileProbe();
int main(int argc, char** argv) {
    if(argc==2 && std::strcmp(argv[1],"warm-profile-allocation")==0)return LSX4RunWarmProfileProbe();
    return LSX4RunSharedCoreProbe(argc == 2 && std::strcmp(argv[1], "interpreter") == 0);
}
#endif
