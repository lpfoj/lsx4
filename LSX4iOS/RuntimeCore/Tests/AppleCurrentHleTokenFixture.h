// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/aerolib/stubs.h"
#include "executor/cpu_tuning.h"
#include "executor/dynamic_translation/retiring_execution_core.h"
#include "executor/dynamic_translation/runtime_gateway.h"
#include <array>
#include <cstring>
#include <vector>

namespace AppleCurrentHleTokenFixture {
using namespace Executor::Jit;
static unsigned calls{}, nested_calls{}, active_depth{}, maximum_depth{};
static unsigned wide_calls{}, mixed_calls{};
static u64 nested_entry{};
static u64 Leaf(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    ++calls;
    maximum_depth=std::max(maximum_depth,active_depth);
    return a+3*b+5*c+7*d+11*e+13*f;
}
static u64 Wide(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f,
                u64 g, u64 h, u64 i, u64 j) {
    ++wide_calls;
    return a + 3*b + 5*c + 7*d + 11*e + 13*f + 17*g + 19*h + 23*i + 29*j;
}
static u64 Mixed(u64 a, double x0, u64 b, float x1, u64 c, double x2,
                 u64 d, double x3, u64 e, double x4, u64 f, double x5,
                 u64 g, double x6, double x7, double x8) {
    ++mixed_calls;
    return a+3*b+5*c+7*d+11*e+13*f+17*g+
        static_cast<u64>(x0+3*x1+5*x2+7*x3+11*x4+13*x5+17*x6+19*x7+23*x8);
}
static double FloatingResult(u64 a, double b, float c) { return a+3*b+5*c; }
static u64 DoubleBits(double value) { u64 bits{};std::memcpy(&bits,&value,8);return bits; }
static u64 Nested(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    struct Depth { Depth(){++active_depth;} ~Depth(){--active_depth;} } depth;
    ++nested_calls;
    volatile u64 local[]{0x19683ac5,0x7531be90,0xabcdef23,0x98765410};
    const std::array<u64,4> copy{local[0],local[1],local[2],local[3]};
    LsxEntryPacket packet{};packet.arg_count=6;packet.args={a,b,c,d,e,f,0,0};
    const u64 result=DispatchGuestOnArm64(nested_entry,packet);
    for(unsigned i=0;i<4;++i)
        Require(local[i]==copy[i],"Nested HLE dispatch damaged host callback storage");
    return result+17;
}
static void Put64(std::vector<u8>& code,u64 value) {
    const auto* bytes=reinterpret_cast<const u8*>(&value);code.insert(code.end(),bytes,bytes+8);
}
static std::vector<u8> Call(u64 target) {
    std::vector<u8> code{0x53,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    // Five pushes keep SysV call alignment; all callee-saved guest registers
    // receive distinct canaries, observed only after the HLE returns.
    constexpr std::array<u8,5> regs{0xbb,0xbc,0xbd,0xbe,0xbf};
    constexpr std::array<u64,5> values{0x123456789ab0ULL,0x13579abcde10ULL,
        0x2468abcdef20ULL,0x35679abcde30ULL,0x45689abcde40ULL};
    for(unsigned i=0;i<5;++i){code.push_back(i==0?0x48:0x49);code.push_back(regs[i]);Put64(code,values[i]);}
    code.insert(code.end(),{0x48,0xb8});Put64(code,target);code.insert(code.end(),{0xff,0xd0});
    std::array<std::size_t,5> failures{};
    for(unsigned i=0;i<5;++i){
        code.insert(code.end(),{0x49,0xba});Put64(code,values[i]);
        code.push_back(i==0?0x4c:0x4d);code.push_back(0x39);
        code.push_back(i==0?0xd3:static_cast<u8>(0xd4+i-1));
        code.insert(code.end(),{0x75,0});failures[i]=code.size()-1;
    }
    code.insert(code.end(),{0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5b,0xc3});
    const auto failure=code.size();code.insert(code.end(),{0x0f,0x0b});
    for(auto index:failures){Require(failure-index-1<128,"HLE fixture branch out of range");code[index]=u8(failure-index-1);}
    return code;
}
static void Run(Core::MemoryManager* memory) {
    constexpr std::size_t bytes=16u<<10;
    void* mapping{};
    Require(memory->MapMemory(&mapping,Core::DEFAULT_MAPPING_BASE,bytes,
        Core::MemoryProt::CpuReadWrite,Core::MemoryMapFlags{},Core::VMAType::Flexible,
        "apple-current-hle-token-test",false)==0,"HLE token fixture mapping failed");
    struct Cleanup {
        Core::MemoryManager* memory; void* mapping;
        Executor::CpuTuning::ProfileV1 profile=Executor::CpuTuning::Get();
        ~Cleanup(){Executor::CpuTuning::SetForTesting(profile);memory->UnmapMemory(reinterpret_cast<VAddr>(mapping),bytes);}
    } cleanup{memory,mapping};
    auto* code=static_cast<u8*>(mapping);
    const auto leaf=Core::AeroLib::GetAndroidX64HleStubForNative("apple-token-fixture-leaf",&Leaf);
    const auto nested=Core::AeroLib::GetAndroidX64HleStubForNative("apple-token-fixture-nested",&Nested);
    const auto is_token=[](u64 value){
        const auto* data=reinterpret_cast<const u8*>(value);
        return value!=0 && data[0]==0x0f && data[1]==0x0b && data[2]==0xcc;
    };
    Require(is_token(leaf) && is_token(nested),"Current HLE fixture received a legacy wrapper, not a dispatch token");
    u64 resolved{};
    Require(Core::AeroLib::TryGetAndroidX64NativeHleTarget(leaf,&resolved) &&
            resolved==reinterpret_cast<u64>(&Leaf),"HLE dispatch token lost its target registration");
    Require(Core::AeroLib::GetAndroidX64HleStubForNative("apple-token-fixture-leaf",&Leaf)==leaf,
            "HLE dispatch token identity changed on cache hit");
    calls=nested_calls=active_depth=maximum_depth=0;
    LsxEntryPacket packet{};packet.arg_count=6;packet.args={1,2,3,4,5,6,0,0};
    constexpr u64 expected=183;
    for(unsigned interpreter=0;interpreter<2;++interpreter){
        auto profile=cleanup.profile;
        profile.execution_mode=interpreter?Executor::CpuTuning::ExecutionMode::Interpreter:
            Executor::CpuTuning::ExecutionMode::Jit;
        profile.persistent_cache=Executor::CpuTuning::PersistentCacheMode::Off;
        Executor::CpuTuning::SetForTesting(profile);
        auto* phase=code+interpreter*4096;
        const auto call=Call(leaf);std::memcpy(phase,call.data(),call.size());
        std::vector<u8> tail{0x48,0xb8};Put64(tail,leaf);tail.insert(tail.end(),{0xff,0xe0});
        std::memcpy(phase+512,tail.data(),tail.size());
        nested_entry=reinterpret_cast<u64>(phase);
        const auto outer=Call(nested);std::memcpy(phase+1024,outer.data(),outer.size());
        const auto before=SnapshotTranslationTelemetry();
        Require(DispatchGuestOnArm64(reinterpret_cast<u64>(phase),packet)==expected,
                "Registered HLE token CALL or preserved guest registers failed");
        Require(DispatchGuestOnArm64(reinterpret_cast<u64>(phase+512),packet)==expected,
                "Registered HLE token tail JMP failed");
        Require(DispatchGuestOnArm64(leaf,packet)==expected,"Registered HLE token root dispatcher failed");
        Require(DispatchGuestOnArm64(reinterpret_cast<u64>(phase+1024),packet)==expected+17,
                "Nested host/guest/HLE token callback failed");
        const auto after=SnapshotTranslationTelemetry();
        Require(interpreter?after.helper_blocks>before.helper_blocks:after.native_blocks>before.native_blocks,
                "HLE fixture did not exercise its requested native/semantic route");
        Require(calls==4*(interpreter+1) && nested_calls==interpreter+1 && active_depth==0 && maximum_depth==1,
                "HLE token lost/duplicated invocation or leaked nested callback state");
        Require(is_token(leaf) && is_token(nested),"HLE dispatch modified token bytes");
    }
    // An identical but unregistered token must not fabricate a successful HLE.
    // Interpreter execution contains the negative case without a native trap.
    std::memcpy(code+8192,reinterpret_cast<void*>(leaf),16);
    bool rejected=false;
    try{(void)DispatchGuestOnArm64(reinterpret_cast<u64>(code+8192),packet);}
    catch(const std::exception&){rejected=true;}
    Require(rejected && calls==8 && nested_calls==2,"Unregistered HLE token was silently accepted");
    const auto wide=Core::AeroLib::GetAndroidX64HleStubForNative("apple-token-fixture-wide",&Wide);
    for(unsigned interpreter=0;interpreter<2;++interpreter) {
        auto profile=cleanup.profile;
        profile.execution_mode=interpreter?Executor::CpuTuning::ExecutionMode::Interpreter:
            Executor::CpuTuning::ExecutionMode::Jit;
        profile.persistent_cache=Executor::CpuTuning::PersistentCacheMode::Off;
        Executor::CpuTuning::SetForTesting(profile);
        // Four SysV stack arguments plus alignment padding. Host ARM64 has
        // different register/stack cutoffs, so a generic eight-word call fails.
        std::vector<u8> call{0x48,0x83,0xec,0x28};
        for(unsigned i=0;i<4;++i) {
            call.insert(call.end(),{0x48,0xb8});Put64(call,7+i);
            call.insert(call.end(),{0x48,0x89,0x44,0x24,static_cast<u8>(i*8)});
        }
        call.insert(call.end(),{0x48,0xb8});Put64(call,wide);
        call.insert(call.end(),{0xff,0xd0,0x48,0x83,0xc4,0x28,0xc3});
        auto* entry=code+10240+interpreter*1024;
        std::memcpy(entry,call.data(),call.size());
        const auto observed=DispatchGuestOnArm64(reinterpret_cast<u64>(entry),packet);
        std::fprintf(stderr,"[APPLE_CURRENT_HLE_STACK_TEST] interpreter=%u observed=%llu expected=%llu\n",
                     interpreter,observed,Wide(1,2,3,4,5,6,7,8,9,10));
        Require(observed==Wide(1,2,3,4,5,6,7,8,9,10),
                "Current HLE lost SysV arguments seven through ten");
    }
    const auto calls_before_reject=wide_calls;
    for(const u64 invalid_stack : {UINT64_C(0x10000), reinterpret_cast<u64>(code)+bytes-16}) {
        Lsx4::Translation::HleBridgeRequest request{};
        request.function=reinterpret_cast<u64>(&Wide);
        request.integer_arguments={1,2,3,4,5,6};
        request.guest_stack=invalid_stack;
        bool failed=false;
        try { (void)Lsx4::Translation::InvokeRuntimeHle(request); }
        catch(const std::exception&) { failed=true; }
        Require(failed && wide_calls==calls_before_reject,
                "Current HLE fabricated success or invoked target on a partial/unowned stack");
    }
    std::fprintf(stderr,"[APPLE_CURRENT_HLE_STACK_REJECT_TEST] unowned=1 partial=1 targetUncalled=1\n");
    const auto mixed=Core::AeroLib::GetAndroidX64HleStubForNative("apple-token-fixture-mixed",&Mixed);
    const auto floating=Core::AeroLib::GetAndroidX64HleStubForNative("apple-token-fixture-floating",&FloatingResult);
    for(unsigned interpreter=0;interpreter<2;++interpreter) {
        auto profile=cleanup.profile;
        profile.execution_mode=interpreter?Executor::CpuTuning::ExecutionMode::Interpreter:
            Executor::CpuTuning::ExecutionMode::Jit;
        profile.persistent_cache=Executor::CpuTuning::PersistentCacheMode::Off;
        Executor::CpuTuning::SetForTesting(profile);
        // Exhaust both independent SysV argument banks. The seventh integer
        // and ninth FP argument spill to stack in declaration order.
        std::vector<u8> call{0x48,0x83,0xec,0x18};
        for(unsigned i=0;i<8;++i) {
            call.insert(call.end(),{0x48,0xb8});
            Put64(call,i==1?UINT64_C(0x40000000):DoubleBits(i+1));
            call.insert(call.end(),{0x66,0x48,0x0f,0x6e,static_cast<u8>(0xc0+i*8)});
        }
        for(unsigned i=0;i<2;++i) {
            call.insert(call.end(),{0x48,0xb8});Put64(call,i?DoubleBits(9):7);
            call.insert(call.end(),{0x48,0x89,0x44,0x24,static_cast<u8>(i*8)});
        }
        call.insert(call.end(),{0x48,0xb8});Put64(call,mixed);
        call.insert(call.end(),{0xff,0xd0,0x48,0x83,0xc4,0x18,0xc3});
        auto* entry=code+12288+interpreter*1536;
        std::memcpy(entry,call.data(),call.size());
        const auto observed=DispatchGuestOnArm64(reinterpret_cast<u64>(entry),packet);
        // An unrecognized symbol still needs its registered scalar FP result
        // in guest XMM0, not the host integer callback return register.
        std::vector<u8> fp{0x48,0x83,0xec,0x08,0x48,0xb8};Put64(fp,DoubleBits(2));
        fp.insert(fp.end(),{0x66,0x48,0x0f,0x6e,0xc0,0x48,0xb8});Put64(fp,0x40400000);
        fp.insert(fp.end(),{0x66,0x48,0x0f,0x6e,0xc8,0x48,0xb8});Put64(fp,floating);
        fp.insert(fp.end(),{0xff,0xd0,0x66,0x48,0x0f,0x7e,0xc0,0x48,0x83,0xc4,0x08,0xc3});
        std::memcpy(entry+768,fp.data(),fp.size());
        const auto fp_observed=DispatchGuestOnArm64(reinterpret_cast<u64>(entry+768),packet);
        const auto expected_mixed=Mixed(1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,9);
        std::fprintf(stderr,"[APPLE_CURRENT_HLE_SCALAR_TEST] interpreter=%u mixed=%llu expected=%llu fp=0x%llx expectedFp=0x%llx\n",
            interpreter,observed,expected_mixed,fp_observed,DoubleBits(22));
        Require(observed==expected_mixed && fp_observed==DoubleBits(22),
                "Current HLE lost mixed scalar arguments or FP result");
    }
    const auto mixed_before_reject=mixed_calls;
    for(const u64 invalid_stack : {UINT64_C(0x10000), reinterpret_cast<u64>(code)+bytes-16}) {
        Lsx4::Translation::HleBridgeRequest request{};
        request.function=reinterpret_cast<u64>(&Mixed);
        request.integer_arguments={1,2,3,4,5,6};
        request.guest_stack=invalid_stack;
        bool failed=false;
        try { (void)Lsx4::Translation::InvokeRuntimeHle(request); }
        catch(const std::exception&) { failed=true; }
        Require(failed && mixed_calls==mixed_before_reject,
                "Mixed scalar HLE invoked target on a partial/unowned spill range");
    }
    std::fprintf(stderr,"[APPLE_CURRENT_HLE_SCALAR_REJECT_TEST] unowned=1 partial=1 targetUncalled=1\n");
    std::fprintf(stderr,"[APPLE_CURRENT_HLE_TOKEN_TEST] nativeCall=1 nativeTail=1 dispatcher=1 "
        "interpreterCall=1 interpreterTail=1 nestedCallbacks=2 preserved=1 poisonUnchanged=1 "
        "unknownRejected=1 scratchPoolBytes=0 calls=%u\n",calls);
}
} // namespace AppleCurrentHleTokenFixture
