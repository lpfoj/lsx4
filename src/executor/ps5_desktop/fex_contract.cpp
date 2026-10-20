// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/ps5_desktop/fex_contract.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace Executor::Ps5Fex {

std::size_t BuildCurrentHleGuestStub(
    const std::span<std::uint8_t, HleGuestStubSize> code,
    const std::uint64_t thunk,
    const std::span<const std::uint8_t, HleThunkIdentitySize> identity) noexcept {
    std::ranges::fill(code, std::uint8_t{0xcc});
    std::size_t cursor{};
    const auto emit = [&](const std::initializer_list<std::uint8_t> bytes) {
        std::ranges::copy(bytes, code.begin() + cursor);
        cursor += bytes.size();
    };
    const auto emit_u64 = [&](const std::uint64_t value) {
        std::memcpy(code.data() + cursor, &value, sizeof(value));
        cursor += sizeof(value);
    };




    emit({0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00});
    emit({0x48, 0xb8});
    emit_u64(thunk);
    emit({0x48, 0x89, 0x44, 0x24, 0x08});
    emit({0x48, 0x89, 0x7c, 0x24, 0x10});
    emit({0x48, 0x89, 0x74, 0x24, 0x18});
    emit({0x48, 0x89, 0x54, 0x24, 0x20});
    emit({0x48, 0x89, 0x4c, 0x24, 0x28});
    emit({0x4c, 0x89, 0x44, 0x24, 0x30});
    emit({0x4c, 0x89, 0x4c, 0x24, 0x38});
    emit({0x48, 0x8d, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00});
    emit({0x48, 0x89, 0x44, 0x24, 0x40});
    emit({0x4c, 0x89, 0x5c, 0x24, 0x48});
    emit({0x48, 0x89, 0x5c, 0x24, 0x50});
    emit({0x48, 0x89, 0x6c, 0x24, 0x58});
    emit({0x4c, 0x89, 0x64, 0x24, 0x60});
    emit({0x4c, 0x89, 0x6c, 0x24, 0x68});
    emit({0x4c, 0x89, 0x74, 0x24, 0x70});
    emit({0x4c, 0x89, 0x7c, 0x24, 0x78});
    emit({0x48, 0x8b, 0x00});
    emit({0x48, 0x89, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00});
    emit({0x48, 0x89, 0xe7});
    emit({0x0f, 0x3f});
    std::ranges::copy(identity, code.begin() + cursor);
    cursor += identity.size();
    return cursor;
}

std::size_t BuildHleGuestStub(
    const std::span<std::uint8_t, HleGuestStubSize> code,
    const std::uint64_t thunk,
    const std::span<const std::uint8_t, HleThunkIdentitySize> identity) noexcept {
    std::ranges::fill(code, std::uint8_t{0xcc});
    std::size_t cursor{};
    const auto emit = [&](const std::initializer_list<std::uint8_t> bytes) {
        std::ranges::copy(bytes, code.begin() + cursor);
        cursor += bytes.size();
    };
    const auto emit_u64 = [&](const std::uint64_t value) {
        std::memcpy(code.data() + cursor, &value, sizeof(value));
        cursor += sizeof(value);
    };



    emit({0x48, 0x81, 0xec, 0x88, 0x00, 0x00, 0x00});
    emit({0x48, 0xb8});
    emit_u64(thunk);
    emit({0x48, 0x89, 0x44, 0x24, 0x08});
    emit({0x48, 0x89, 0x7c, 0x24, 0x10});
    emit({0x48, 0x89, 0x74, 0x24, 0x18});
    emit({0x48, 0x89, 0x54, 0x24, 0x20});
    emit({0x48, 0x89, 0x4c, 0x24, 0x28});
    emit({0x4c, 0x89, 0x44, 0x24, 0x30});
    emit({0x4c, 0x89, 0x4c, 0x24, 0x38});
    emit({0x48, 0x8d, 0x84, 0x24, 0x88, 0x00, 0x00, 0x00});
    emit({0x48, 0x89, 0x44, 0x24, 0x40});
    emit({0x4c, 0x89, 0x5c, 0x24, 0x48});
    emit({0x48, 0x89, 0x5c, 0x24, 0x50});
    emit({0x48, 0x89, 0x6c, 0x24, 0x58});
    emit({0x4c, 0x89, 0x64, 0x24, 0x60});
    emit({0x4c, 0x89, 0x6c, 0x24, 0x68});
    emit({0x4c, 0x89, 0x74, 0x24, 0x70});
    emit({0x4c, 0x89, 0x7c, 0x24, 0x78});
    emit({0x48, 0x8b, 0x00});
    emit({0x48, 0x89, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00});






    emit({0x48, 0x83, 0xec, 0x08});
    emit({0x48, 0xb8});
    emit_u64(thunk + HleGuestEpilogueOffset);
    emit({0x48, 0x89, 0x04, 0x24});
    emit({0x48, 0x8d, 0x7c, 0x24, 0x08});
    emit({0x0f, 0x3f});
    std::ranges::copy(identity, code.begin() + cursor);
    cursor += identity.size();







    emit({0x48, 0x8b, 0x04, 0x24});
    emit({0x4c, 0x8b, 0x94, 0x24, 0x80, 0x00, 0x00, 0x00});
    emit({0x4c, 0x8b, 0x5c, 0x24, 0x48});
    emit({0x48, 0x8b, 0x5c, 0x24, 0x50});
    emit({0x48, 0x8b, 0x6c, 0x24, 0x58});
    emit({0x4c, 0x8b, 0x64, 0x24, 0x60});
    emit({0x4c, 0x8b, 0x6c, 0x24, 0x68});
    emit({0x4c, 0x8b, 0x74, 0x24, 0x70});
    emit({0x4c, 0x8b, 0x7c, 0x24, 0x78});
    emit({0x48, 0x81, 0xc4, 0x90, 0x00, 0x00, 0x00});
    emit({0x41, 0xff, 0xe2});
    return cursor;
}

bool BuildHleBridgeRequest(const Lsx4FexHleCallFrameV3& source,
                           Lsx4::Translation::HleBridgeRequest& destination) noexcept {
    if (source.struct_size < sizeof(source) || source.abi_version != LSX4_FEX_BACKEND_ABI_V3 ||
        source.native_function == 0) {
        return false;
    }
    destination = {};
    destination.function = source.native_function;
    std::ranges::copy(source.gpr, destination.integer_arguments.begin());
    for (std::size_t index = 0; index != destination.floating_arguments.size(); ++index) {
        destination.floating_arguments[index] = source.xmm[index].low;
    }
    destination.guest_stack = source.guest_rsp;
    return true;
}

void CompleteHleResult(const Lsx4FexHleCallFrameV3& source, const std::uint64_t value,
                       const int fp_result_kind, Lsx4FexHleResultFrameV3& destination) noexcept {
    const std::uint32_t capacity = destination.struct_size;
    destination = {};
    destination.struct_size = capacity;
    destination.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    destination.call_id = source.call_id;
    destination.rax = value;
    destination.sret_address = source.sret_address;
    destination.sret_size = source.sret_size;
    destination.return_class[0] = source.return_class[0];
    destination.return_class[1] = source.return_class[1];
    destination.return_eightbyte_count = source.return_eightbyte_count;
    destination.sret_flags = source.sret_flags;
    destination.abi_descriptor_id = source.abi_descriptor_id;
    if (fp_result_kind == 1) {
        destination.xmm0.low = static_cast<std::uint32_t>(value);
    } else if (fp_result_kind == 2) {
        destination.xmm0.low = value;
    }
    destination.status = LSX4_FEX_STATUS_OK;
}

std::uint64_t StableRangeCookie(const std::uint64_t address, const std::uint64_t size,
                                const std::uint32_t protection,
                                const std::uint32_t range_flags) noexcept {


    std::uint64_t value = address ^ (size << 1u) ^ (static_cast<std::uint64_t>(protection) << 32u) ^
                          range_flags ^ UINT64_C(0x5053354645585235);
    value ^= value >> 30u;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27u;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31u;
    return value != 0 ? value : UINT64_C(0x5053354645580001);
}

ExecutableRangeDelta PlanExecutableRangeUpdate(
    const std::span<const Lsx4FexExecutableRangeV5> published,
    const std::span<const Lsx4FexExecutableRangeV5> current) {
    const auto same_range = [](const Lsx4FexExecutableRangeV5& lhs,
                               const Lsx4FexExecutableRangeV5& rhs) {
        return lhs.address == rhs.address && lhs.size == rhs.size &&
               lhs.range_cookie == rhs.range_cookie && lhs.protection == rhs.protection &&
               lhs.range_flags == rhs.range_flags;
    };
    ExecutableRangeDelta delta;
    delta.removed.reserve(published.size());
    delta.added.reserve(current.size());
    for (const auto& prior : published) {
        if (std::ranges::none_of(
                current, [&](const auto& candidate) { return same_range(prior, candidate); })) {
            delta.removed.push_back(prior);
        }
    }
    for (const auto& candidate : current) {
        if (std::ranges::none_of(published,
                                 [&](const auto& prior) { return same_range(candidate, prior); })) {
            delta.added.push_back(candidate);
        }
    }
    return delta;
}

}
