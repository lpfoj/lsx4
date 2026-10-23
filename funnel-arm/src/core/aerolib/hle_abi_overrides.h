// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)

#include "core/aerolib/hle_abi.h"

namespace Libraries::AudioOut {
union OrbisAudioOutParamExtendedInformation;
}

namespace Libraries::Ajm {
union AjmInstanceFlags;
}

namespace Common {
struct SlotId;
}

namespace Libraries::Rtc {
struct OrbisRtcTick;
}

namespace Core::AeroLib {

// The public PS4 type is a 32-bit bit-field union.  AMD64 SysV classifies
// that single eightbyte as INTEGER.  Keep this explicit: C++ cannot reflect
// aggregate fields and guessing aggregate classes is deliberately forbidden.
template <>
struct AndroidX64HleTypeAbiOverride<
    Libraries::AudioOut::OrbisAudioOutParamExtendedInformation> {
    static constexpr bool enabled = true;
    static constexpr bool memory = false;
    static constexpr u8 count = 1;
    static constexpr std::array<AndroidX64SysVClass, 2> classes{
        AndroidX64SysVClass::Integer, AndroidX64SysVClass::NoClass};
};

// AJM instance flags are one guest u64 bit-field union.  AMD64 SysV passes
// this value in one INTEGER register; declaring that explicitly keeps the
// Android x86-to-ARM HLE bridge from rejecting sceAjmInstanceCreate.
template <>
struct AndroidX64HleTypeAbiOverride<Libraries::Ajm::AjmInstanceFlags> {
    static constexpr bool enabled = true;
    static constexpr bool memory = false;
    static constexpr u8 count = 1;
    static constexpr std::array<AndroidX64SysVClass, 2> classes{
        AndroidX64SysVClass::Integer, AndroidX64SysVClass::NoClass};
};

// Slot handles are a single guest u32.  They are C++ aggregates only as an
// implementation detail; AMD64 SysV classifies the containing eightbyte as
// INTEGER.
template <>
struct AndroidX64HleTypeAbiOverride<Common::SlotId> {
    static constexpr bool enabled = true;
    static constexpr bool memory = false;
    static constexpr u8 count = 1;
    static constexpr std::array<AndroidX64SysVClass, 2> classes{
        AndroidX64SysVClass::Integer, AndroidX64SysVClass::NoClass};
};

// OrbisRtcTick contains one guest u64 and therefore has one INTEGER
// eightbyte when passed by value.
template <>
struct AndroidX64HleTypeAbiOverride<Libraries::Rtc::OrbisRtcTick> {
    static constexpr bool enabled = true;
    static constexpr bool memory = false;
    static constexpr u8 count = 1;
    static constexpr std::array<AndroidX64SysVClass, 2> classes{
        AndroidX64SysVClass::Integer, AndroidX64SysVClass::NoClass};
};

} // namespace Core::AeroLib

#endif // __ANDROID__
