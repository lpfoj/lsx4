// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/dynamic_translation/guest_capability_profile.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using namespace Lsx4::Translation;

[[noreturn]] void Fail(const char* check) {
    std::fprintf(stderr, "GUEST_CPU_PROFILE_CONTRACT failed=%s\n", check);
    std::abort();
}

void Require(const bool condition, const char* check) {
    if (!condition) {
        Fail(check);
    }
}

}

int main() {
    const GuestCpuProfile& profile = FrozenPs4GuestCpuProfile();
    Require(ValidateGuestCpuProfile(profile).Succeeded(), "frozen-profile-valid");
    Require(profile.schema_version == GuestCpuProfileSchemaVersion,
            "schema-version");
    Require(profile.profile_id == "lsx4-ps4-synthetic-v1", "profile-id");
    Require(profile.vendor ==
                (std::array<char, 12>{'A', 'u', 't', 'h', 'e', 'n', 't', 'i',
                                      'c', 'A', 'M', 'D'}),
            "vendor");
    Require(profile.family_model_stepping == 0x00630f01u, "signature");
    Require(profile.cpuid_records.size() == 17, "complete-record-count");
    Require(profile.architecture.physical_address_bits == 48 &&
                profile.architecture.linear_address_bits == 48,
            "address-width");
    Require(!profile.architecture.cr0_alignment_mask &&
                profile.architecture.current_privilege_level == 3 &&
                profile.architecture.alignment_check_policy ==
                    GuestAlignmentCheckPolicy::Disabled &&
                profile.architecture.misaligned_sse_policy ==
                    GuestMisalignedSsePolicy::NotAdvertised &&
                !profile.architecture.jaguar_alignment_msrs_exposed,
            "frozen-alignment-policy");

    Require(QueryGuestCapabilityPage(0, 99) ==
                GuestCapabilityPage{{{0x00000007u, 0x68747541u, 0x444d4163u,
                                      0x69746e65u}}},
            "vendor-leaf");
    Require(QueryGuestCapabilityPage(1, 77) ==
                GuestCapabilityPage{{{0x00630f01u, 0x00000800u, 0x3ed80201u,
                                      0x07808131u}}},
            "leaf-one");
    Require(QueryGuestCapabilityPage(7, 0).registers[1] == 0x00002128u,
            "leaf-seven-zero-subleaf");
    Require(QueryGuestCapabilityPage(7, 1) == GuestCapabilityPage{},
            "leaf-seven-unknown-subleaf-zero");
    for (std::uint32_t leaf = 2; leaf != 7; ++leaf) {
        Require(QueryGuestCapabilityPage(leaf, 0) == GuestCapabilityPage{},
                "explicit-zero-basic-leaf");
    }
    for (std::uint32_t leaf = 0x80000002u; leaf != 0x80000008u; ++leaf) {
        Require(QueryGuestCapabilityPage(leaf, 0) == GuestCapabilityPage{},
                "explicit-zero-extended-leaf");
    }
    Require(QueryGuestCapabilityPage(0x0du, 0) == GuestCapabilityPage{},
            "out-of-range-xstate-leaf-zero");
    Require(QueryGuestCapabilityPage(0xffffffffu, 0xffffffffu) ==
                GuestCapabilityPage{},
            "unknown-leaf-zero");

    Require(profile.xstate.xcr0_read_value == 0x7u && QueryGuestXcr(0) == 0x7u &&
                QueryGuestXcr(1) == 0 && QueryGuestXcr(0xffffffffu) == 0 &&
                profile.xstate.xcr0_writable_mask == 0 &&
                profile.xstate.mxcsr_reset_value == 0x1f80u &&
                profile.xstate.mxcsr_mask == 0x0000ffffu &&
                profile.xstate.legacy_save_area_bytes == 512 &&
                profile.xstate.cr4_osfxsr && profile.xstate.cr4_osxsave &&
                profile.xstate.xsave_leaf_is_out_of_range,
            "xstate-policy");

    Require(profile.identity.IsValid() &&
                profile.identity == ComputeGuestCpuProfileIdentity(profile),
            "identity-self-check");
    Require(profile.identity.low == UINT64_C(0x61e82729ec820a6c) &&
                profile.identity.high == UINT64_C(0xf0e351bb9bbb26d1),
            "identity-pinned");

    std::vector<GuestCpuidRecord> changed_records(profile.cpuid_records.begin(),
                                                   profile.cpuid_records.end());
    changed_records[1].value.registers[2] ^= 1u;
    GuestCpuProfile changed = profile;
    changed.cpuid_records = changed_records;
    Require(ComputeGuestCpuProfileIdentity(changed) != profile.identity,
            "cpuid-participates-in-identity");
    Require(ValidateGuestCpuProfile(changed).error ==
                GuestCpuProfileValidationError::IdentityMismatch,
            "changed-cpuid-rejected");

    changed = profile;
    changed.xstate.xcr0_read_value ^= 0x4u;
    Require(ComputeGuestCpuProfileIdentity(changed) != profile.identity,
            "xstate-participates-in-identity");
    Require(ValidateGuestCpuProfile(changed).error ==
                GuestCpuProfileValidationError::IdentityMismatch,
            "changed-xstate-rejected");

    changed_records.assign(profile.cpuid_records.begin(),
                           profile.cpuid_records.end());
    changed_records[0].value.registers[0] = 0xffffffffu;
    changed = profile;
    changed.cpuid_records = changed_records;
    changed.identity = ComputeGuestCpuProfileIdentity(changed);
    Require(ValidateGuestCpuProfile(changed).error ==
                GuestCpuProfileValidationError::IncompleteLeafRange,
            "malformed-maximum-leaf-bounded-and-rejected");

    std::fprintf(stdout,
                 "GUEST_CPU_PROFILE_CONTRACT ok identity=%016llx%016llx records=%zu\n",
                 static_cast<unsigned long long>(profile.identity.high),
                 static_cast<unsigned long long>(profile.identity.low),
                 profile.cpuid_records.size());
    return 0;
}
