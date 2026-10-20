// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_fiber_hle_bridge.h"

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>

namespace {

using Lsx4::Translation::CpuFrame;
using Lsx4::Translation::IntegerRegister;
using Lsx4::Translation::RegisterIndex;

[[noreturn]] void Fail(const char* stage, const std::uint64_t actual = 0,
                       const std::uint64_t expected = 0) {
    std::fprintf(stderr,
                 "FEX_FIBER_HLE_BRIDGE_SMOKE FAIL stage=%s actual=0x%" PRIx64
                 " expected=0x%" PRIx64 "\n",
                 stage, actual, expected);
    std::fflush(stderr);
    std::_Exit(1);
}

void Require(const bool condition, const char* stage,
             const std::uint64_t actual = 0,
             const std::uint64_t expected = 0) {
    if (!condition) {
        Fail(stage, actual, expected);
    }
}

struct FakeAdapter {
    Lsx4FexGuestContextV3 current{};
    Lsx4FexGuestContextV3 committed{};
    std::int32_t get_status{LSX4_FEX_STATUS_OK};
    std::int32_t set_status{LSX4_FEX_STATUS_OK};
    std::uint32_t get_count{};
    std::uint32_t set_count{};
};

std::int32_t GetGuestContext(void* opaque,
                             const Lsx4FexThreadIdentityV3* identity,
                             Lsx4FexGuestContextV3* context) {
    auto& adapter = *static_cast<FakeAdapter*>(opaque);
    ++adapter.get_count;
    if (adapter.get_status != LSX4_FEX_STATUS_OK) {
        return adapter.get_status;
    }
    if (identity == nullptr || context == nullptr ||
        identity->guest_thread != adapter.current.guest_thread) {
        return LSX4_FEX_STATUS_INVALID_ARGUMENT;
    }
    *context = adapter.current;
    return LSX4_FEX_STATUS_OK;
}

std::int32_t SetGuestContext(void* opaque,
                             const Lsx4FexThreadIdentityV3* identity,
                             const Lsx4FexGuestContextV3* context) {
    auto& adapter = *static_cast<FakeAdapter*>(opaque);
    ++adapter.set_count;
    if (identity == nullptr || context == nullptr ||
        identity->guest_thread != adapter.current.guest_thread) {
        return LSX4_FEX_STATUS_INVALID_ARGUMENT;
    }
    adapter.committed = *context;
    return adapter.set_status;
}

Lsx4FexThreadIdentityV3 MakeIdentity(const std::uint64_t guest_thread) {
    Lsx4FexThreadIdentityV3 identity{};
    identity.struct_size = sizeof(identity);
    identity.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    identity.guest_thread = guest_thread;
    identity.pthread_handle = UINT64_C(0x2222333344445555);
    identity.host_tid = 77;
    return identity;
}

Lsx4FexGuestContextV3 MakeCurrentContext(const std::uint64_t guest_thread,
                                         const std::uint64_t guest_rsp) {
    Lsx4FexGuestContextV3 context{};
    context.struct_size = sizeof(context);
    context.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    for (std::size_t index = 0; index < std::size(context.gpr); ++index) {
        context.gpr[index] = UINT64_C(0x1100000000000000) + index;
    }
    context.gpr[RegisterIndex(IntegerRegister::Stack)] = guest_rsp;
    context.rip = UINT64_C(0x800012345);
    context.rflags = UINT64_C(0x246);
    context.fs_base = UINT64_C(0x710000001000);
    context.gs_base = UINT64_C(0x710000002000);
    for (std::size_t index = 0; index < std::size(context.xmm); ++index) {
        context.xmm[index].low = UINT64_C(0xa000000000000000) + index;
        context.xmm[index].high = UINT64_C(0xb000000000000000) + index;
    }
    context.mxcsr = UINT32_C(0x9fc0);
    context.context_flags =
        Executor::Fex::ScopedFiberHleContextBridge::CommitContextFlags |
        LSX4_FEX_CONTEXT_XMM_VALID;
    context.guest_thread = guest_thread;
    return context;
}

Lsx4FexBackendApiV3 MakeApi(FakeAdapter& adapter) {
    Lsx4FexBackendApiV3 api{};
    api.struct_size = sizeof(api);
    api.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    api.backend_context = &adapter;
    api.get_guest_context = GetGuestContext;
    api.set_guest_context = SetGuestContext;
    return api;
}

void CheckExactTargetSet() {
    constexpr std::array<std::string_view, 5> nids{
        "a0LLrZWac0M", "PFT2S-tJ7Uk", "B0ZX2hx9DMw", "avfGJ94g36Q",
        "ZqhZFuzKT6U"};
    for (const auto nid : nids) {
        Require(Executor::Fex::IsContextReplacingFiberNid(nid),
                "exact-fiber-nid");
    }
    constexpr std::array<std::string_view, 7> negatives{
        "", "sceFiberRun", "xa0LLrZWac0M", "a0LLrZWac0Mx",
        "PFT2S-tJ7U", "B0ZX2hx9DMw:game", "ZqhZFuzKT6U-extra"};
    for (const auto name : negatives) {
        Require(!Executor::Fex::IsContextReplacingFiberNid(name),
                "non-exact-fiber-nid");
    }
}

void CheckReplacementCommitAndCanaries() {
    constexpr std::uint64_t GuestThread = UINT64_C(0x123456789abc);
    constexpr std::uint64_t GuestRsp = UINT64_C(0x700000001000);
    constexpr std::uint64_t GuestReturn = UINT64_C(0x800055550);
    FakeAdapter adapter{};
    adapter.current = MakeCurrentContext(GuestThread, GuestRsp);
    auto api = MakeApi(adapter);
    const auto identity = MakeIdentity(GuestThread);

    CpuFrame outer_live{};
    CpuFrame outer_continuation{};
    outer_live.resume_address = UINT64_C(0xaaaaaaaaaaaaaaaa);
    outer_continuation.resume_address = UINT64_C(0xbbbbbbbbbbbbbbbb);
    Require(Lsx4::Translation::ExchangeLiveStateStorage(&outer_live) == nullptr,
            "outer-live-empty");
    Require(Lsx4::Translation::ExchangeContinuationStorage(&outer_continuation) == nullptr,
            "outer-continuation-empty");
    Require(Lsx4::Translation::RequestLiveStateReplacement(),
            "outer-replacement-request");

    Executor::Fex::ScopedFiberHleContextBridge bridge{
        &api, identity, GuestRsp, GuestReturn};
    Require(bridge.active() && bridge.status() == LSX4_FEX_STATUS_OK,
            "bridge-active", bridge.status(), LSX4_FEX_STATUS_OK);
    Require(adapter.get_count == 1, "get-once", adapter.get_count, 1);

    CpuFrame observed{};
    Require(Lsx4::Translation::PublishedLiveState().CopyTo(observed),
            "published-live-copy");
    for (std::size_t index = 0; index < observed.integer.size(); ++index) {
        Require(observed.integer[index] == adapter.current.gpr[index],
                "published-gpr", observed.integer[index], adapter.current.gpr[index]);
    }
    Require(observed.resume_address == adapter.current.rip,
            "published-rip", observed.resume_address, adapter.current.rip);
    Require(observed.condition_word == adapter.current.rflags,
            "published-rflags", observed.condition_word, adapter.current.rflags);
    Require(std::memcmp(observed.vectors[9].data(), &adapter.current.xmm[9],
                        sizeof(adapter.current.xmm[9])) == 0,
            "published-xmm-low");
    for (std::size_t byte = sizeof(Lsx4FexXmm128); byte < observed.vectors[9].size(); ++byte) {
        Require(observed.vectors[9][byte] == 0, "unavailable-ymm-upper-zero",
                observed.vectors[9][byte], 0);
    }

    CpuFrame continuation{};
    Require(Lsx4::Translation::SnapshotContinuationState(continuation),
            "continuation-copy");
    Require(continuation.resume_address == GuestReturn,
            "continuation-rip", continuation.resume_address, GuestReturn);
    Require(continuation.integer[RegisterIndex(IntegerRegister::Stack)] ==
                GuestRsp + sizeof(std::uint64_t),
            "continuation-rsp",
            continuation.integer[RegisterIndex(IntegerRegister::Stack)],
            GuestRsp + sizeof(std::uint64_t));

    CpuFrame replacement = observed;
    for (std::size_t index = 0; index < replacement.integer.size(); ++index) {
        replacement.integer[index] = UINT64_C(0xc100000000000000) + index;
    }
    replacement.resume_address = UINT64_C(0x8000feed0);
    replacement.condition_word = UINT64_C(0x202);
    replacement.fs_origin = UINT64_C(0x720000001000);
    replacement.gs_origin = UINT64_C(0x720000002000);
    replacement.simd_control = UINT32_C(0xdead);
    for (auto& vector : replacement.vectors) {
        vector.fill(UINT8_C(0xee));
    }
    Require(Lsx4::Translation::PublishedLiveState().ReplaceWith(replacement),
            "publish-replacement");
    Require(Lsx4::Translation::RequestLiveStateReplacement(),
            "request-replacement");
    Require(bridge.Commit() == LSX4_FEX_STATUS_OK,
            "commit-status", bridge.status(), LSX4_FEX_STATUS_OK);
    Require(adapter.set_count == 1, "set-once", adapter.set_count, 1);
    Require(adapter.committed.context_flags ==
                Executor::Fex::ScopedFiberHleContextBridge::CommitContextFlags,
            "commit-flags", adapter.committed.context_flags,
            Executor::Fex::ScopedFiberHleContextBridge::CommitContextFlags);
    Require((adapter.committed.context_flags & LSX4_FEX_CONTEXT_XMM_VALID) == 0,
            "no-partial-vector-commit", adapter.committed.context_flags);
    Require(adapter.committed.mxcsr == 0, "no-partial-mxcsr-commit",
            adapter.committed.mxcsr, 0);
    for (const auto& xmm : adapter.committed.xmm) {
        Require(xmm.low == 0 && xmm.high == 0, "no-partial-xmm-payload");
    }
    for (std::size_t index = 0; index < replacement.integer.size(); ++index) {
        Require(adapter.committed.gpr[index] == replacement.integer[index],
                "commit-gpr", adapter.committed.gpr[index], replacement.integer[index]);
    }
    Require(adapter.committed.rip == replacement.resume_address,
            "commit-rip", adapter.committed.rip, replacement.resume_address);
    Require(adapter.committed.guest_thread == GuestThread,
            "commit-thread", adapter.committed.guest_thread, GuestThread);

    Require(Lsx4::Translation::PublishedLiveState().StorageAddress() ==
                reinterpret_cast<std::uintptr_t>(&outer_live),
            "restore-outer-live");
    CpuFrame restored_continuation{};
    Require(Lsx4::Translation::SnapshotContinuationState(restored_continuation) &&
                restored_continuation.resume_address == outer_continuation.resume_address,
            "restore-outer-continuation");
    Require(Lsx4::Translation::ConsumeLiveStateReplacementRequest(),
            "restore-outer-replacement");
    Require(!Lsx4::Translation::ConsumeLiveStateReplacementRequest(),
            "outer-replacement-consumed-once");
    Require(Lsx4::Translation::ExchangeContinuationStorage(nullptr) ==
                &outer_continuation,
            "clear-outer-continuation");
    Require(Lsx4::Translation::ExchangeLiveStateStorage(nullptr) == &outer_live,
            "clear-outer-live");
}

void CheckNoReplacementAndFailurePaths() {
    constexpr std::uint64_t GuestThread = UINT64_C(0x99887766);
    constexpr std::uint64_t GuestRsp = UINT64_C(0x700000002000);
    FakeAdapter adapter{};
    adapter.current = MakeCurrentContext(GuestThread, GuestRsp);
    auto api = MakeApi(adapter);
    const auto identity = MakeIdentity(GuestThread);

    {
        Executor::Fex::ScopedFiberHleContextBridge bridge{
            &api, identity, GuestRsp, UINT64_C(0x800011110)};
        Require(bridge.active(), "no-replacement-active");
        Require(bridge.Commit() == LSX4_FEX_STATUS_OK,
                "no-replacement-commit");
        Require(adapter.set_count == 0, "no-replacement-no-set",
                adapter.set_count, 0);
    }
    Require(!Lsx4::Translation::PublishedLiveState(),
            "no-replacement-storage-restored");

    {
        Executor::Fex::ScopedFiberHleContextBridge bridge{
            &api, identity, GuestRsp, UINT64_C(0x800022220)};
        Require(bridge.active(), "cancel-active");
        Require(Lsx4::Translation::RequestLiveStateReplacement(),
                "cancel-request");
    }
    Require(adapter.set_count == 0, "cancel-does-not-set", adapter.set_count, 0);
    Require(!Lsx4::Translation::PublishedLiveState(), "cancel-restores-storage");
    Require(!Lsx4::Translation::ConsumeLiveStateReplacementRequest(),
            "cancel-clears-local-request");

    adapter.get_status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    Executor::Fex::ScopedFiberHleContextBridge failed{
        &api, identity, GuestRsp, UINT64_C(0x800033330)};
    Require(!failed.active(), "get-failure-inactive");
    Require(failed.status() == LSX4_FEX_STATUS_BACKEND_FAILURE,
            "get-failure-status", failed.status(), LSX4_FEX_STATUS_BACKEND_FAILURE);
    Require(failed.Commit() == LSX4_FEX_STATUS_BACKEND_FAILURE,
            "get-failure-commit");
    Require(!Lsx4::Translation::PublishedLiveState(),
            "get-failure-no-publication");
}

} // namespace

int main() {
    CheckExactTargetSet();
    CheckReplacementCommitAndCanaries();
    CheckNoReplacementAndFailurePaths();
    std::puts("FEX_FIBER_HLE_BRIDGE_SMOKE OK");
    return 0;
}
