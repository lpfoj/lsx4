// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Executor::Jit {

enum class HostIsaFeature : std::uint64_t {
    Lse = UINT64_C(1) << 0,
    FlagM = UINT64_C(1) << 1,
    FlagM2 = UINT64_C(1) << 2,
    Lrcpc = UINT64_C(1) << 3,
    Ilrcpc = UINT64_C(1) << 4,
    Lrcpc3 = UINT64_C(1) << 5,
    Sve = UINT64_C(1) << 6,
    Sve2 = UINT64_C(1) << 7,
    Aes = UINT64_C(1) << 8,
    Pmull = UINT64_C(1) << 9,
    Sha1 = UINT64_C(1) << 10,
    Sha2 = UINT64_C(1) << 11,
    Crc32 = UINT64_C(1) << 12,
    Afp = UINT64_C(1) << 13,
    Fcma = UINT64_C(1) << 14,
    Frintts = UINT64_C(1) << 15,
    Ecv = UINT64_C(1) << 16,
    Rpres = UINT64_C(1) << 17,
};

enum class HostOsFeature : std::uint64_t {
    SveContext = UINT64_C(1) << 0,
    HardwareTsoEnabled = UINT64_C(1) << 1,
};

struct HostIsaProfile {
    std::uint64_t hwcap{};
    std::uint64_t hwcap2{};
    std::uint64_t features{};
    std::uint64_t os_features{};
    std::uint32_t sve_vector_bytes{};

    [[nodiscard]] bool Has(HostIsaFeature feature) const noexcept {
        return (features & static_cast<std::uint64_t>(feature)) != 0;
    }

    [[nodiscard]] bool HasOs(HostOsFeature feature) const noexcept {
        return (os_features & static_cast<std::uint64_t>(feature)) != 0;
    }
};

struct HardwareTsoAttempt {
    bool enabled{};
    long get_before{-1};
    int get_before_errno{};
    long set_result{-1};
    int set_errno{};
    long get_after{-1};
    int get_after_errno{};
};

[[nodiscard]] const HostIsaProfile& GetHostIsaProfile() noexcept;
[[nodiscard]] std::uint32_t CurrentThreadSveVectorBytes() noexcept;
/*
 * PR_SET_MEM_MODEL is a per-thread contract.  This deliberately isn't cached:
 * every host thread that is about to execute hardware-TSO FEX code must call
 * it (or prove that the thread is already in TSO mode).
 */
[[nodiscard]] HardwareTsoAttempt TryEnableHardwareTsoForCurrentThread() noexcept;
/* Restores DEFAULT only when the preceding probe changed this thread. */
void RestoreHardwareTsoAfterProbe(const HardwareTsoAttempt& attempt) noexcept;

}
