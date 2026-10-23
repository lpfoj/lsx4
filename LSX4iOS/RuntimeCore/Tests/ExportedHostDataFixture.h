// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise LIB_OBJ against the production runtime registry and semantic reader.
#pragma once

#include "core/libraries/libs.h"
#include "executor/dynamic_translation/process_memory.h"
#include "executor/dynamic_translation/retiring_execution_core.h"
#include "executor/dynamic_translation/runtime_bridge_api.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace Libraries::Kernel { u64* GetStackChkGuardExportStorage(); }

namespace ExportedHostDataFixture {
inline void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

inline bool SemanticReadRejected(u64 address, std::uint32_t bytes) {
    try {
        (void)Executor::Jit::LoadGuestScalar(address, bytes);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

inline void CheckRejected(u64 address, std::size_t bytes) {
    u64 value{};
    Check(bytes <= sizeof(value), "Export fixture invalid negative span");
    const bool range=ExecutorJitIsReadableGuestRange(address, bytes);
    const bool strict=ExecutorJitReadGuestBytes(address, &value, bytes);
    const bool stable=ExecutorJitReadGuestBytesStable(address, &value, bytes);
    const bool semanticRejected=SemanticReadRejected(address,static_cast<std::uint32_t>(bytes));
    Check(!range && !strict && !stable && semanticRejected,
          "HLE data export admitted bytes outside its declared object");
}

inline void CheckQword(u64 address, u64 expected) {
    u64 strict{}, stable{};
    const bool range=ExecutorJitIsReadableGuestRange(address,sizeof(u64));
    const bool strictOk=ExecutorJitReadGuestBytes(address,&strict,sizeof(strict)) && strict==expected;
    const bool stableOk=ExecutorJitReadGuestBytesStable(address,&stable,sizeof(stable)) && stable==expected;
    bool semanticOk{};
    try { semanticOk=Executor::Jit::LoadGuestScalar(address,sizeof(u64))==expected; }
    catch(const std::runtime_error&) {}
    if(!range || !strictOk || !stableOk || !semanticOk)
        std::fprintf(stderr,"[HLE_DATA_EXPORT_READ_FAILURE] range=%d strict=%d stable=%d semantic=%d\n",
                     range,strictOk,stableOk,semanticOk);
    Check(range && strictOk && stableOk && semanticOk,
          "HLE data export disagrees between strict, stable and semantic readers");
}

inline void CheckSymbol(const Core::Loader::SymbolsResolver& resolver, const char* nid,
                        u64 address) {
    Core::Loader::SymbolResolver query{};
    query.name=nid; query.library="fixture"; query.library_version=1;
    query.module="fixture"; query.type=Core::Loader::SymbolType::Object;
    const auto* symbol=resolver.FindSymbol(query);
    Check(symbol && symbol->virtual_address==address,
          "HLE data export changed the published pointer");
}

inline void Run() {
    // The production registry is append-only. Keep every published test object
    // alive until process exit, and run registration/boundary checks only once.
    static const bool passed=[] {
        const auto page=static_cast<std::size_t>(getpagesize());
        auto* const storage=static_cast<unsigned char*>(
            mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0));
        Check(storage!=MAP_FAILED && page>=1024,"HLE export fixture mmap failed");
        auto* const scalar=reinterpret_cast<u64*>(storage+128);
        constexpr u64 marker=0x71c28d4935e60afbULL;
        *scalar=marker;
        const auto address=reinterpret_cast<u64>(scalar);
        u64 native{};
        Check(Lsx4::Translation::ReadProcessGuestMemory(address,&native,sizeof(native)) &&
              native==marker,"HLE fixture host object is not readable");
        CheckRejected(address,8);

        Core::Loader::SymbolsResolver resolver;
        auto* sym=&resolver;
        unsigned evaluations{};
        const auto object_expression=[&] { ++evaluations; return scalar; };
        LIB_OBJ("scalar","fixture",1,"fixture",object_expression());
        Check(evaluations==1,"LIB_OBJ evaluated its object expression more than once");
        CheckSymbol(resolver,"scalar",address);
        CheckQword(address,marker);
        CheckRejected(address-1,1);
        CheckRejected(address-1,8);
        CheckRejected(address+1,8);
        CheckRejected(address+8,1);
        CheckRejected(reinterpret_cast<u64>(storage+512),8);

        const u64* const alias=scalar;
        LIB_OBJ("alias","fixture",1,"fixture",alias);
        CheckSymbol(resolver,"alias",address);
        CheckQword(address,marker);
        CheckRejected(address+8,1);

        auto* const array=reinterpret_cast<u64 (*)[3]>(storage+256);
        (*array)[0]=marker; (*array)[1]=marker+1; (*array)[2]=marker+2;
        LIB_OBJ("array","fixture",1,"fixture",array);
        const auto arrayAddress=reinterpret_cast<u64>(array);
        CheckSymbol(resolver,"array",arrayAddress);
        Check(ExecutorJitIsReadableGuestRange(arrayAddress,sizeof(*array)),
              "LIB_OBJ lost the complete array extent");
        CheckQword(arrayAddress+16,marker+2);
        CheckRejected(arrayAddress-1,1);
        CheckRejected(arrayAddress+sizeof(*array),1);

        struct Pair { u64 first, second; };
        auto* const pair=reinterpret_cast<Pair*>(storage+384);
        *pair={marker,marker+3};
        LIB_OBJ("pair","fixture",1,"fixture",pair);
        CheckSymbol(resolver,"pair",reinterpret_cast<u64>(pair));
        CheckQword(reinterpret_cast<u64>(&pair->second),marker+3);
        CheckRejected(reinterpret_cast<u64>(pair)+sizeof(*pair),1);

        LIB_OBJ("null","fixture",1,"fixture",static_cast<u64*>(nullptr));
        CheckSymbol(resolver,"null",0);
        CheckRejected(0,8);

        // Use the exact production guard storage and LIB_OBJ declaration route,
        // without starting unrelated kernel service threads in this fixture.
        auto* const guard=Libraries::Kernel::GetStackChkGuardExportStorage();
        const u64 guardValue=*guard;
        LIB_OBJ("f7uOxY9mM1U","libkernel",1,"libkernel",guard);
        CheckQword(reinterpret_cast<u64>(guard),guardValue);
        CheckRejected(reinterpret_cast<u64>(guard)+sizeof(*guard),1);
        Check(*scalar==marker && *guard==guardValue,
              "HLE export read fixture changed guest-visible data");
        return true;
    }();
    Check(passed,"HLE data export fixture failed");
    std::fprintf(stderr,"[APPLE_HLE_DATA_EXPORT_TEST] typedExtent=1 pointerIdentity=1 "
        "singleEvaluation=1 strict=1 stable=1 semantic=1 unownedRejected=1 "
        "adjacentRejected=1 aliases=1 arrayExtent=1 structExtent=1 guard=1 lifetime=process\n");
}
}
