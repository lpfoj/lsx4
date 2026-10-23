// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace Lsx4::Translation {

inline constexpr std::uint32_t GuestCpuProfileSchemaVersion = 1;

struct GuestCapabilityPage {
    std::array<std::uint32_t, 4> registers{};

    friend constexpr bool operator==(const GuestCapabilityPage&,
                                     const GuestCapabilityPage&) = default;
};

enum class GuestCpuidSelectorPolicy : std::uint8_t {
    Ignored,
    Exact,
};

struct GuestCpuidRecord {
    std::uint32_t leaf{};
    std::uint32_t subleaf{};
    GuestCapabilityPage value{};
    GuestCpuidSelectorPolicy selector_policy{GuestCpuidSelectorPolicy::Ignored};

    friend constexpr bool operator==(const GuestCpuidRecord&,
                                     const GuestCpuidRecord&) = default;
};

struct GuestCpuProfileIdentity {
    std::uint64_t low{};
    std::uint64_t high{};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return low != 0 || high != 0;
    }

    friend constexpr bool operator==(const GuestCpuProfileIdentity&,
                                     const GuestCpuProfileIdentity&) = default;
};

enum class GuestAlignmentCheckPolicy : std::uint8_t {
    Disabled,
};

enum class GuestMisalignedSsePolicy : std::uint8_t {
    NotAdvertised,
};

enum class GuestXcrUnknownIndexPolicy : std::uint8_t {
    ReturnZero,
};

struct GuestXstatePolicy {
    std::uint64_t xcr0_read_value{};
    std::uint64_t xcr0_writable_mask{};
    std::uint32_t mxcsr_reset_value{};
    std::uint32_t mxcsr_mask{};
    std::uint16_t legacy_save_area_bytes{};
    bool cr4_osfxsr{};
    bool cr4_osxsave{};
    bool xsave_leaf_is_out_of_range{};
    GuestXcrUnknownIndexPolicy unknown_xcr_policy{
        GuestXcrUnknownIndexPolicy::ReturnZero};

    friend constexpr bool operator==(const GuestXstatePolicy&,
                                     const GuestXstatePolicy&) = default;
};

struct GuestArchitecturalPolicy {
    std::uint8_t physical_address_bits{};
    std::uint8_t linear_address_bits{};
    std::uint8_t current_privilege_level{};
    bool cr0_alignment_mask{};
    bool rflags_ac_writable{};
    GuestAlignmentCheckPolicy alignment_check_policy{
        GuestAlignmentCheckPolicy::Disabled};
    GuestMisalignedSsePolicy misaligned_sse_policy{
        GuestMisalignedSsePolicy::NotAdvertised};
    bool jaguar_alignment_msrs_exposed{};
    std::uint32_t exception_policy_version{};

    friend constexpr bool operator==(const GuestArchitecturalPolicy&,
                                     const GuestArchitecturalPolicy&) = default;
};

/*
 * This is the existing LSX4 PS4 virtual CPU, not a physical-Jaguar claim.
 * Records cover every leaf from leaf 0's maximum and every extended leaf from
 * 0x80000000's maximum, including the zero-valued leaves.  Values above those
 * maxima, including leaf 0x0d, are defined to return zero by this profile.
 */
struct GuestCpuProfile {
    std::uint32_t schema_version{};
    std::string_view profile_id{};
    std::array<char, 12> vendor{};
    std::uint32_t family_model_stepping{};
    std::span<const GuestCpuidRecord> cpuid_records{};
    GuestXstatePolicy xstate{};
    GuestArchitecturalPolicy architecture{};
    GuestCpuProfileIdentity identity{};
};

enum class GuestCpuProfileValidationError : std::uint8_t {
    None,
    BadSchema,
    MissingProfileId,
    BadVendor,
    BadRecordOrder,
    IncompleteLeafRange,
    BadSignature,
    BadAddressWidth,
    BadXstatePolicy,
    BadAlignmentPolicy,
    IdentityMismatch,
};

struct GuestCpuProfileValidationResult {
    GuestCpuProfileValidationError error{GuestCpuProfileValidationError::None};

    [[nodiscard]] constexpr bool Succeeded() const noexcept {
        return error == GuestCpuProfileValidationError::None;
    }
};

[[nodiscard]] const GuestCpuProfile& FrozenPs4GuestCpuProfile() noexcept;
[[nodiscard]] GuestCpuProfileIdentity ComputeGuestCpuProfileIdentity(
    const GuestCpuProfile& profile) noexcept;
[[nodiscard]] GuestCpuProfileValidationResult ValidateGuestCpuProfile(
    const GuestCpuProfile& profile) noexcept;

[[nodiscard]] GuestCapabilityPage QueryGuestCapabilityPage(
    std::uint32_t page, std::uint32_t selector) noexcept;
[[nodiscard]] std::uint64_t QueryGuestXcr(std::uint32_t index) noexcept;

}
