// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/dynamic_translation/guest_capability_profile.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace Lsx4::Translation {
namespace {

constexpr GuestCapabilityPage Page(const std::uint32_t eax = 0,
                                   const std::uint32_t ebx = 0,
                                   const std::uint32_t ecx = 0,
                                   const std::uint32_t edx = 0) noexcept {
    return {{{eax, ebx, ecx, edx}}};
}

constexpr std::array kCpuidRecords{
    GuestCpuidRecord{0x00000000u, 0, Page(0x00000007u, 0x68747541u,
                                         0x444d4163u, 0x69746e65u)},
    GuestCpuidRecord{0x00000001u, 0, Page(0x00630f01u, 0x00000800u,
                                         0x3ed80201u, 0x07808131u)},
    GuestCpuidRecord{0x00000002u, 0, Page()},
    GuestCpuidRecord{0x00000003u, 0, Page()},
    GuestCpuidRecord{0x00000004u, 0, Page()},
    GuestCpuidRecord{0x00000005u, 0, Page()},
    GuestCpuidRecord{0x00000006u, 0, Page()},
    GuestCpuidRecord{0x00000007u, 0,
                     Page(0x00000000u, 0x00002128u, 0x00000000u,
                          0x00000000u),
                     GuestCpuidSelectorPolicy::Exact},
    GuestCpuidRecord{0x80000000u, 0, Page(0x80000008u)},
    GuestCpuidRecord{0x80000001u, 0,
                     Page(0, 0, 0x00000020u, 0x2c100800u)},
    GuestCpuidRecord{0x80000002u, 0, Page()},
    GuestCpuidRecord{0x80000003u, 0, Page()},
    GuestCpuidRecord{0x80000004u, 0, Page()},
    GuestCpuidRecord{0x80000005u, 0, Page()},
    GuestCpuidRecord{0x80000006u, 0, Page()},
    GuestCpuidRecord{0x80000007u, 0, Page()},
    GuestCpuidRecord{0x80000008u, 0, Page(0x00003030u)},
};

constexpr std::array<char, 12> kAuthenticAmd{
    'A', 'u', 't', 'h', 'e', 'n', 't', 'i', 'c', 'A', 'M', 'D'};

class StableProfileHasher {
public:
    void AddByte(const std::uint8_t value) noexcept {
        low_ = (low_ ^ value) * UINT64_C(0x00000100000001b3);
        high_ ^= static_cast<std::uint64_t>(value) + UINT64_C(0x9e3779b97f4a7c15) +
                 (high_ << 6u) + (high_ >> 2u);
        high_ *= UINT64_C(0xbf58476d1ce4e5b9);
    }

    void AddBool(const bool value) noexcept {
        AddByte(value ? 1u : 0u);
    }

    template <typename Integer>
    void AddInteger(Integer value) noexcept {
        using Unsigned = std::make_unsigned_t<Integer>;
        static_assert(sizeof(Unsigned) <= sizeof(std::uint64_t));
        std::uint64_t bits = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index != sizeof(Unsigned); ++index) {
            AddByte(static_cast<std::uint8_t>(bits & 0xffu));
            bits >>= 8u;
        }
    }

    void AddString(const std::string_view value) noexcept {
        AddInteger(static_cast<std::uint32_t>(value.size()));
        for (const char byte : value) {
            AddByte(static_cast<std::uint8_t>(byte));
        }
    }

    [[nodiscard]] GuestCpuProfileIdentity Finish() const noexcept {
        GuestCpuProfileIdentity result{low_, high_};
        if (!result.IsValid()) {
            result.low = 1;
        }
        return result;
    }

private:
    std::uint64_t low_{UINT64_C(0xcbf29ce484222325)};
    std::uint64_t high_{UINT64_C(0x6c73783470733431)};
};

const GuestCpuidRecord* FindRecord(const GuestCpuProfile& profile,
                                  const std::uint32_t leaf) noexcept {
    const auto found = std::lower_bound(
        profile.cpuid_records.begin(), profile.cpuid_records.end(), leaf,
        [](const GuestCpuidRecord& record, const std::uint32_t target) {
            return record.leaf < target;
        });
    return found != profile.cpuid_records.end() && found->leaf == leaf
               ? &*found
               : nullptr;
}

bool HasCompleteRange(const GuestCpuProfile& profile, const std::uint32_t first,
                      const std::uint32_t last) noexcept {
    if (last < first) {
        return false;
    }
    const std::uint64_t required_count =
        static_cast<std::uint64_t>(last) - first + 1u;
    if (required_count > profile.cpuid_records.size()) {
        return false;
    }
    for (std::uint64_t offset = 0; offset != required_count; ++offset) {
        if (FindRecord(profile,
                       first + static_cast<std::uint32_t>(offset)) == nullptr) {
            return false;
        }
    }
    return true;
}

GuestCpuProfile BuildFrozenProfile() noexcept {
    GuestCpuProfile profile{
        .schema_version = GuestCpuProfileSchemaVersion,
        .profile_id = "lsx4-ps4-synthetic-v1",
        .vendor = kAuthenticAmd,
        .family_model_stepping = 0x00630f01u,
        .cpuid_records = kCpuidRecords,
        .xstate =
            {
                .xcr0_read_value = 0x7u,
                .xcr0_writable_mask = 0,
                .mxcsr_reset_value = 0x1f80u,
                .mxcsr_mask = 0x0000ffffu,
                .legacy_save_area_bytes = 512,
                .cr4_osfxsr = true,
                .cr4_osxsave = true,
                .xsave_leaf_is_out_of_range = true,
                .unknown_xcr_policy = GuestXcrUnknownIndexPolicy::ReturnZero,
            },
        .architecture =
            {
                .physical_address_bits = 48,
                .linear_address_bits = 48,
                .current_privilege_level = 3,
                .cr0_alignment_mask = false,
                .rflags_ac_writable = true,
                .alignment_check_policy = GuestAlignmentCheckPolicy::Disabled,
                .misaligned_sse_policy = GuestMisalignedSsePolicy::NotAdvertised,
                .jaguar_alignment_msrs_exposed = false,
                .exception_policy_version = 1,
            },
    };
    profile.identity = ComputeGuestCpuProfileIdentity(profile);
    return profile;
}

}

const GuestCpuProfile& FrozenPs4GuestCpuProfile() noexcept {
    static const GuestCpuProfile profile = BuildFrozenProfile();
    return profile;
}

GuestCpuProfileIdentity ComputeGuestCpuProfileIdentity(
    const GuestCpuProfile& profile) noexcept {
    StableProfileHasher hash{};
    hash.AddInteger(profile.schema_version);
    hash.AddString(profile.profile_id);
    for (const char byte : profile.vendor) {
        hash.AddByte(static_cast<std::uint8_t>(byte));
    }
    hash.AddInteger(profile.family_model_stepping);
    hash.AddInteger(static_cast<std::uint32_t>(profile.cpuid_records.size()));
    for (const GuestCpuidRecord& record : profile.cpuid_records) {
        hash.AddInteger(record.leaf);
        hash.AddInteger(record.subleaf);
        for (const std::uint32_t value : record.value.registers) {
            hash.AddInteger(value);
        }
        hash.AddInteger(static_cast<std::uint8_t>(record.selector_policy));
    }
    hash.AddInteger(profile.xstate.xcr0_read_value);
    hash.AddInteger(profile.xstate.xcr0_writable_mask);
    hash.AddInteger(profile.xstate.mxcsr_reset_value);
    hash.AddInteger(profile.xstate.mxcsr_mask);
    hash.AddInteger(profile.xstate.legacy_save_area_bytes);
    hash.AddBool(profile.xstate.cr4_osfxsr);
    hash.AddBool(profile.xstate.cr4_osxsave);
    hash.AddBool(profile.xstate.xsave_leaf_is_out_of_range);
    hash.AddInteger(static_cast<std::uint8_t>(profile.xstate.unknown_xcr_policy));
    hash.AddInteger(profile.architecture.physical_address_bits);
    hash.AddInteger(profile.architecture.linear_address_bits);
    hash.AddInteger(profile.architecture.current_privilege_level);
    hash.AddBool(profile.architecture.cr0_alignment_mask);
    hash.AddBool(profile.architecture.rflags_ac_writable);
    hash.AddInteger(static_cast<std::uint8_t>(
        profile.architecture.alignment_check_policy));
    hash.AddInteger(static_cast<std::uint8_t>(
        profile.architecture.misaligned_sse_policy));
    hash.AddBool(profile.architecture.jaguar_alignment_msrs_exposed);
    hash.AddInteger(profile.architecture.exception_policy_version);
    return hash.Finish();
}

GuestCpuProfileValidationResult ValidateGuestCpuProfile(
    const GuestCpuProfile& profile) noexcept {
    if (profile.schema_version != GuestCpuProfileSchemaVersion) {
        return {GuestCpuProfileValidationError::BadSchema};
    }
    if (profile.profile_id.empty()) {
        return {GuestCpuProfileValidationError::MissingProfileId};
    }
    if (profile.vendor != kAuthenticAmd) {
        return {GuestCpuProfileValidationError::BadVendor};
    }
    for (std::size_t index = 1; index < profile.cpuid_records.size(); ++index) {
        if (profile.cpuid_records[index - 1].leaf >=
            profile.cpuid_records[index].leaf) {
            return {GuestCpuProfileValidationError::BadRecordOrder};
        }
    }
    const GuestCpuidRecord* basic = FindRecord(profile, 0);
    const GuestCpuidRecord* extended = FindRecord(profile, 0x80000000u);
    if (basic == nullptr || extended == nullptr ||
        !HasCompleteRange(profile, 0, basic->value.registers[0]) ||
        !HasCompleteRange(profile, 0x80000000u,
                          extended->value.registers[0])) {
        return {GuestCpuProfileValidationError::IncompleteLeafRange};
    }
    if (basic->value.registers[1] != 0x68747541u ||
        basic->value.registers[2] != 0x444d4163u ||
        basic->value.registers[3] != 0x69746e65u) {
        return {GuestCpuProfileValidationError::BadVendor};
    }
    const GuestCpuidRecord* signature = FindRecord(profile, 1);
    if (signature == nullptr || signature->value.registers[0] !=
                                    profile.family_model_stepping) {
        return {GuestCpuProfileValidationError::BadSignature};
    }
    const GuestCpuidRecord* widths = FindRecord(profile, 0x80000008u);
    if (widths == nullptr ||
        (widths->value.registers[0] & 0xffu) !=
            profile.architecture.physical_address_bits ||
        ((widths->value.registers[0] >> 8u) & 0xffu) !=
            profile.architecture.linear_address_bits) {
        return {GuestCpuProfileValidationError::BadAddressWidth};
    }
    constexpr std::uint32_t Xsave = 1u << 26u;
    constexpr std::uint32_t Osxsave = 1u << 27u;
    constexpr std::uint32_t Avx = 1u << 28u;
    constexpr std::uint32_t Fxsave = 1u << 24u;
    const std::uint32_t leaf1_ecx = signature->value.registers[2];
    const std::uint32_t leaf1_edx = signature->value.registers[3];
    if ((profile.xstate.xcr0_read_value & 1u) == 0 ||
        (profile.xstate.xcr0_writable_mask &
         ~profile.xstate.xcr0_read_value) != 0 ||
        profile.xstate.mxcsr_reset_value != 0x1f80u ||
        profile.xstate.mxcsr_mask == 0 ||
        profile.xstate.legacy_save_area_bytes != 512 ||
        profile.xstate.cr4_osxsave != ((leaf1_ecx & Osxsave) != 0) ||
        profile.xstate.cr4_osfxsr != ((leaf1_edx & Fxsave) != 0) ||
        ((profile.xstate.xcr0_read_value & 0x6u) != 0 &&
         (leaf1_ecx & Xsave) == 0) ||
        ((profile.xstate.xcr0_read_value & 0x4u) != 0 &&
         (leaf1_ecx & Avx) == 0) ||
        (profile.xstate.xsave_leaf_is_out_of_range &&
         basic->value.registers[0] >= 0x0du)) {
        return {GuestCpuProfileValidationError::BadXstatePolicy};
    }
    const GuestCpuidRecord* extended_features =
        FindRecord(profile, 0x80000001u);
    constexpr std::uint32_t MisAlignSse = 1u << 7u;
    if (profile.architecture.current_privilege_level != 3 ||
        profile.architecture.alignment_check_policy !=
            GuestAlignmentCheckPolicy::Disabled ||
        profile.architecture.misaligned_sse_policy !=
            GuestMisalignedSsePolicy::NotAdvertised ||
        profile.architecture.jaguar_alignment_msrs_exposed ||
        profile.architecture.exception_policy_version == 0 ||
        extended_features == nullptr ||
        (extended_features->value.registers[2] & MisAlignSse) != 0) {
        return {GuestCpuProfileValidationError::BadAlignmentPolicy};
    }
    if (!profile.identity.IsValid() ||
        profile.identity != ComputeGuestCpuProfileIdentity(profile)) {
        return {GuestCpuProfileValidationError::IdentityMismatch};
    }
    return {};
}

GuestCapabilityPage QueryGuestCapabilityPage(const std::uint32_t page,
                                             const std::uint32_t selector) noexcept {
    const GuestCpuProfile& profile = FrozenPs4GuestCpuProfile();
    const GuestCpuidRecord* record = FindRecord(profile, page);
    if (record == nullptr ||
        (record->selector_policy == GuestCpuidSelectorPolicy::Exact &&
         record->subleaf != selector)) {
        return {};
    }
    return record->value;
}

std::uint64_t QueryGuestXcr(const std::uint32_t index) noexcept {
    const GuestXstatePolicy& xstate = FrozenPs4GuestCpuProfile().xstate;
    return index == 0 ? xstate.xcr0_read_value : 0;
}

}
