// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef __ANDROID__
#define __ANDROID__ 1
#endif

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "core/aerolib/hle_varargs.h"
#include "core/libraries/libc_internal/printf.h"

namespace {

struct GuestRegion {
    u64 base{};
    const u8* data{};
    std::size_t size{};
};

std::vector<GuestRegion> g_guest_regions;
std::size_t g_guest_read_attempts{};

void ResetGuestRegions() {
    g_guest_regions.clear();
    g_guest_read_attempts = 0;
}

template <typename T, std::size_t N>
void AddGuestRegion(const u64 base, const std::array<T, N>& storage,
                    const std::size_t byte_size = sizeof(storage)) {
    g_guest_regions.push_back(
        {base, reinterpret_cast<const u8*>(storage.data()), byte_size});
}

void PutXmmDouble(Core::AeroLib::AndroidX64HleRawCall& call,
                  const std::size_t index, const double value) {
    std::memcpy(&call.xmm[index].low, &value, sizeof(value));
}

bool NearlyEqual(const double lhs, const double rhs) {
    return std::abs(lhs - rhs) < 1.0e-12;
}

Core::AeroLib::AndroidX64HleRawCall VariadicCall(const u64 guest_rsp,
                                                 const u32 sse_upper_bound) {
    Core::AeroLib::AndroidX64HleRawCall call{};
    call.guest_rsp = guest_rsp;
    call.call_flags = Core::AeroLib::AndroidX64CallVariadic;
    call.sysv_sse_arg_count = sse_upper_bound;
    return call;
}

void TestCanonicalGpAndOverflow() {
    ResetGuestRegions();
    constexpr u64 guest_rsp = 0x100000;
    const std::array<u64, 5> stack{0xfeedface, 70, 71, 72, 73};
    AddGuestRegion(guest_rsp, stack);

    auto call = VariadicCall(guest_rsp, 0);
    call.gpr = {10, 11, 12, 13, 14, 15};
    Common::VaCtx context{};
    assert(Core::AeroLib::BuildAndroidX64VaContext(call, 2, 0, 0, &context));
    for (std::size_t index = 0; index < call.gpr.size(); ++index) {
        assert(context.reg_save_area.gp[index] == call.gpr[index]);
    }
    assert(context.va_list.gp_offset == 16);
    assert(context.va_list.fp_offset == 48);
    assert(reinterpret_cast<u64>(context.va_list.overflow_arg_area) == guest_rsp + 8);
    assert(g_guest_read_attempts == 0);

    for (const u64 expected : std::array<u64, 8>{12, 13, 14, 15, 70, 71, 72, 73}) {
        assert(static_cast<u64>(Common::vaArgLongLong(&context.va_list)) == expected);
    }
    assert(!context.va_list.overflow_read_failed);
    assert(g_guest_read_attempts == 4);
}

void TestNineDoubles() {
    ResetGuestRegions();
    constexpr u64 guest_rsp = 0x200000;
    std::array<u64, 2> stack{0xfeedface, 0};
    const double ninth = 9.25;
    std::memcpy(&stack[1], &ninth, sizeof(ninth));
    AddGuestRegion(guest_rsp, stack);

    auto call = VariadicCall(guest_rsp, 8);
    for (std::size_t index = 0; index < call.xmm.size(); ++index) {
        PutXmmDouble(call, index, static_cast<double>(index) + 1.25);
    }
    Common::VaCtx context{};
    assert(Core::AeroLib::BuildAndroidX64VaContext(call, 0, 0, 0, &context));
    for (std::size_t index = 0; index < call.xmm.size(); ++index) {
        assert(NearlyEqual(Common::vaArgDouble(&context.va_list),
                           static_cast<double>(index) + 1.25));
    }
    assert(NearlyEqual(Common::vaArgDouble(&context.va_list), ninth));
    assert(!context.va_list.overflow_read_failed);
    assert(g_guest_read_attempts == 1);
}

void TestMixedGpFpOrderingAndCaps() {
    ResetGuestRegions();
    constexpr u64 guest_rsp = 0x300000;
    const double fp_overflow = 81.5;
    u64 fp_bits{};
    std::memcpy(&fp_bits, &fp_overflow, sizeof(fp_bits));
    const std::array<u64, 4> stack{0xfeedface, 70, fp_bits, 71};
    AddGuestRegion(guest_rsp, stack);

    auto call = VariadicCall(guest_rsp, 8);
    call.gpr = {100, 101, 102, 103, 104, 105};
    for (std::size_t index = 0; index < call.xmm.size(); ++index) {
        PutXmmDouble(call, index, static_cast<double>(index) + 0.5);
    }
    Common::VaCtx context{};
    assert(Core::AeroLib::BuildAndroidX64VaContext(call, 2, 1, 0, &context));
    for (const s64 expected : std::array<s64, 4>{102, 103, 104, 105}) {
        assert(Common::vaArgLongLong(&context.va_list) == expected);
    }
    for (std::size_t index = 1; index < call.xmm.size(); ++index) {
        assert(NearlyEqual(Common::vaArgDouble(&context.va_list),
                           static_cast<double>(index) + 0.5));
    }
    assert(Common::vaArgLongLong(&context.va_list) == 70);
    assert(NearlyEqual(Common::vaArgDouble(&context.va_list), fp_overflow));
    assert(Common::vaArgLongLong(&context.va_list) == 71);
    assert(!context.va_list.overflow_read_failed);
    assert(g_guest_read_attempts == 3);

    Common::VaCtx capped{};
    assert(Core::AeroLib::BuildAndroidX64VaContext(call, 99, 8, 24, &capped));
    assert(capped.va_list.gp_offset == 48);
    assert(capped.va_list.fp_offset == 176);
    assert(reinterpret_cast<u64>(capped.va_list.overflow_arg_area) == guest_rsp + 8 + 24);
    for (std::size_t index = 0; index < call.gpr.size(); ++index) {
        assert(capped.reg_save_area.gp[index] == call.gpr[index]);
    }
}

void TestFixedStackAndBuilderRejections() {
    ResetGuestRegions();
    constexpr u64 guest_rsp = 0x310000;
    const std::array<u64, 5> stack{0xfeedface, 1, 2, 3,
                                   UINT64_C(0x8877665544332211)};
    AddGuestRegion(guest_rsp, stack);

    auto call = VariadicCall(guest_rsp, 0);
    Common::VaCtx context{};
    assert(Core::AeroLib::BuildAndroidX64VaContext(call, 6, 0, 24, &context));
    assert(reinterpret_cast<u64>(context.va_list.overflow_arg_area) == guest_rsp + 32);
    assert(static_cast<u64>(Common::vaArgLongLong(&context.va_list)) == stack[4]);

    auto missing_flag = call;
    missing_flag.call_flags = 0;
    assert(!Core::AeroLib::BuildAndroidX64VaContext(missing_flag, 0, 0, 0, &context));
    auto excessive_al = call;
    excessive_al.sysv_sse_arg_count = 9;
    assert(!Core::AeroLib::BuildAndroidX64VaContext(excessive_al, 0, 0, 0, &context));
    assert(!Core::AeroLib::BuildAndroidX64VaContext(call, 0, 1, 0, &context));
    assert(!Core::AeroLib::BuildAndroidX64VaContext(call, 0, 0, 4, &context));
    auto address_overflow = VariadicCall(std::numeric_limits<u64>::max() - 4, 0);
    assert(!Core::AeroLib::BuildAndroidX64VaContext(address_overflow, 0, 0, 0,
                                                    &context));
}

void TestGuestLongIsAlwaysLp64() {
    auto call = VariadicCall(0x320000, 0);
    call.gpr[0] = UINT64_C(0xfedcba9876543210);
    Common::VaCtx signed_context{};
    Common::VaCtx unsigned_context{};
    assert(Core::AeroLib::BuildAndroidX64VaContext(call, 0, 0, 0, &signed_context));
    assert(Core::AeroLib::BuildAndroidX64VaContext(call, 0, 0, 0, &unsigned_context));
    static_assert(sizeof(Common::vaArgLong(&signed_context.va_list)) == 8);
    assert(static_cast<u64>(Common::vaArgLong(&signed_context.va_list)) == call.gpr[0]);
    assert(Common::vaArgULong(&unsigned_context.va_list) == call.gpr[0]);
}

void TestGuardCrossFailsClosed() {
    ResetGuestRegions();
    constexpr u64 guest_rsp = 0x400000;
    const std::array<u64, 2> stack{0xfeedface, UINT64_C(0x1122334455667788)};
    AddGuestRegion(guest_rsp, stack, 12);

    auto call = VariadicCall(guest_rsp, 8);
    for (std::size_t index = 0; index < call.xmm.size(); ++index) {
        PutXmmDouble(call, index, static_cast<double>(index) + 1.0);
    }
    Common::VaCtx context{};
    assert(Core::AeroLib::BuildAndroidX64VaContext(call, 0, 0, 0, &context));
    for (std::size_t index = 0; index < call.xmm.size(); ++index) {
        (void)Common::vaArgDouble(&context.va_list);
    }
    assert(Common::vaArgDouble(&context.va_list) == 0.0);
    assert(context.va_list.overflow_read_failed);
    assert(g_guest_read_attempts == 1);
    assert(Common::vaArgDouble(&context.va_list) == 0.0);
    assert(g_guest_read_attempts == 1);
}

void TestSnprintfFailureDoesNotWrite() {
    ResetGuestRegions();
    constexpr u64 guest_rsp = 0x410000;
    const std::array<u64, 1> stack{0xfeedface};
    AddGuestRegion(guest_rsp, stack);
    constexpr char format[] = "%lld %lld %lld %lld";

    auto call = VariadicCall(guest_rsp, 0);
    call.gpr = {0, 64, reinterpret_cast<u64>(format), 11, 12, 13};
    Common::VaCtx context{};
    assert(Core::AeroLib::BuildAndroidX64VaContext(call, 2, 0, 0, &context));
    std::array<char, 64> output{};
    std::memcpy(output.data(), "unchanged", sizeof("unchanged"));
    (void)Libraries::LibcInternal::snprintf_ctx(output.data(), output.size(), &context);
    assert(context.va_list.overflow_read_failed);
    assert(std::strcmp(output.data(), "unchanged") == 0);
}

void TestNullReaderKeepsDirectPath() {
    const std::array<u64, 1> local{UINT64_C(0x8877665544332211)};
    Common::VaList list{};
    list.gp_offset = 48;
    list.overflow_arg_area = const_cast<u64*>(local.data());
    assert(static_cast<u64>(Common::vaArgLongLong(&list)) == local[0]);
    assert(!list.overflow_read_failed);
}

} // namespace

namespace Core::AeroLib {

extern "C" bool ExecutorJitReadGuestBytes(const std::uint64_t address,
                                           void* destination,
                                           const std::size_t size) {
    ++g_guest_read_attempts;
    for (const GuestRegion& region : g_guest_regions) {
        if (address < region.base) {
            continue;
        }
        const u64 offset = address - region.base;
        if (offset <= region.size &&
            size <= region.size - static_cast<std::size_t>(offset)) {
            std::memcpy(destination, region.data + offset, size);
            return true;
        }
    }
    return false;
}

} // namespace Core::AeroLib

int main() {
    static_assert(offsetof(Common::VaRegSave, fp) == 48);
    static_assert(sizeof(Common::VaFpReg) == 16);
    TestCanonicalGpAndOverflow();
    TestNineDoubles();
    TestMixedGpFpOrderingAndCaps();
    TestFixedStackAndBuilderRejections();
    TestGuestLongIsAlwaysLp64();
    TestGuardCrossFailsClosed();
    TestSnprintfFailureDoesNotWrite();
    TestNullReaderKeepsDirectPath();
    std::cout << "SYSV_VARARGS_SMOKE PASS canonical_gp=6 canonical_fp=8 gp_overflow=4 "
                 "doubles=9 mixed_bank_overflow=1 fixed_stack=1 builder_rejects=5 "
                 "guest_lp64=1 guard_cross_reject=1 transactional_snprintf=1 "
                 "direct_null_reader=1\n";
    return 0;
}
