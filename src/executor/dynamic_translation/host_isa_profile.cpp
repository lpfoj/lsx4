// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/dynamic_translation/host_isa_profile.h"

#include <cerrno>

#if defined(__ANDROID__) && defined(__aarch64__)
#include <android/log.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef PR_GET_MEM_MODEL
#define PR_GET_MEM_MODEL 0x6d4d444c
#endif
#ifndef PR_SET_MEM_MODEL
#define PR_SET_MEM_MODEL 0x4d4d444c
#endif
#ifndef PR_SET_MEM_MODEL_DEFAULT
#define PR_SET_MEM_MODEL_DEFAULT 0
#endif
#ifndef PR_SET_MEM_MODEL_TSO
#define PR_SET_MEM_MODEL_TSO 1
#endif
#endif

namespace Executor::Jit {
namespace {

constexpr std::uint64_t Bit(const HostIsaFeature feature) noexcept {
    return static_cast<std::uint64_t>(feature);
}

constexpr std::uint64_t Bit(const HostOsFeature feature) noexcept {
    return static_cast<std::uint64_t>(feature);
}

HostIsaProfile DetectHostIsaProfile() noexcept {
    HostIsaProfile profile{};
#if defined(__ANDROID__) && defined(__aarch64__)
    profile.hwcap = getauxval(AT_HWCAP);
#if defined(AT_HWCAP2)
    profile.hwcap2 = getauxval(AT_HWCAP2);
#endif

#if defined(HWCAP_ATOMICS)
    if ((profile.hwcap & HWCAP_ATOMICS) != 0) {
        profile.features |= Bit(HostIsaFeature::Lse);
    }
#endif
#if defined(HWCAP_FLAGM)
    if ((profile.hwcap & HWCAP_FLAGM) != 0) {
        profile.features |= Bit(HostIsaFeature::FlagM);
    }
#endif
#if defined(HWCAP_LRCPC)
    if ((profile.hwcap & HWCAP_LRCPC) != 0) {
        profile.features |= Bit(HostIsaFeature::Lrcpc);
    }
#endif
#if defined(HWCAP_ILRCPC)
    if ((profile.hwcap & HWCAP_ILRCPC) != 0) {
        profile.features |= Bit(HostIsaFeature::Ilrcpc);
    }
#endif
#if defined(HWCAP_SVE)
    if ((profile.hwcap & HWCAP_SVE) != 0) {
        profile.features |= Bit(HostIsaFeature::Sve);
    }
#endif
#if defined(HWCAP_AES)
    if ((profile.hwcap & HWCAP_AES) != 0) {
        profile.features |= Bit(HostIsaFeature::Aes);
    }
#endif
#if defined(HWCAP_PMULL)
    if ((profile.hwcap & HWCAP_PMULL) != 0) {
        profile.features |= Bit(HostIsaFeature::Pmull);
    }
#endif
#if defined(HWCAP_SHA1)
    if ((profile.hwcap & HWCAP_SHA1) != 0) {
        profile.features |= Bit(HostIsaFeature::Sha1);
    }
#endif
#if defined(HWCAP_SHA2)
    if ((profile.hwcap & HWCAP_SHA2) != 0) {
        profile.features |= Bit(HostIsaFeature::Sha2);
    }
#endif
#if defined(HWCAP_CRC32)
    if ((profile.hwcap & HWCAP_CRC32) != 0) {
        profile.features |= Bit(HostIsaFeature::Crc32);
    }
#endif
#if defined(HWCAP_FCMA)
    if ((profile.hwcap & HWCAP_FCMA) != 0) {
        profile.features |= Bit(HostIsaFeature::Fcma);
    }
#endif
#if defined(HWCAP2_FLAGM2)
    if ((profile.hwcap2 & HWCAP2_FLAGM2) != 0) {
        profile.features |= Bit(HostIsaFeature::FlagM2);
    }
#endif
#if defined(HWCAP2_LRCPC3)
    if ((profile.hwcap2 & HWCAP2_LRCPC3) != 0) {
        profile.features |= Bit(HostIsaFeature::Lrcpc3);
    }
#endif
#if defined(HWCAP2_SVE2)
    if ((profile.hwcap2 & HWCAP2_SVE2) != 0 &&
        profile.Has(HostIsaFeature::Sve)) {
        profile.features |= Bit(HostIsaFeature::Sve2);
    }
#endif
#if defined(HWCAP2_AFP)
    if ((profile.hwcap2 & HWCAP2_AFP) != 0) {
        profile.features |= Bit(HostIsaFeature::Afp);
    }
#endif
#if defined(HWCAP2_FRINT)
    if ((profile.hwcap2 & HWCAP2_FRINT) != 0) {
        profile.features |= Bit(HostIsaFeature::Frintts);
    }
#endif
#if defined(HWCAP2_ECV)
    if ((profile.hwcap2 & HWCAP2_ECV) != 0) {
        profile.features |= Bit(HostIsaFeature::Ecv);
    }
#endif
#if defined(HWCAP2_RPRES)
    if ((profile.hwcap2 & HWCAP2_RPRES) != 0) {
        profile.features |= Bit(HostIsaFeature::Rpres);
    }
#endif
    profile.sve_vector_bytes = CurrentThreadSveVectorBytes();
    if (profile.sve_vector_bytes == 0) {
        profile.features &= ~(Bit(HostIsaFeature::Sve) |
                              Bit(HostIsaFeature::Sve2));
    } else {
        profile.os_features |= Bit(HostOsFeature::SveContext);
    }
    if (prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0) == PR_SET_MEM_MODEL_TSO) {
        profile.os_features |= Bit(HostOsFeature::HardwareTsoEnabled);
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4-JIT",
        "[EXECUTOR_HOST_ISA] hwcap=0x%llx hwcap2=0x%llx features=0x%llx "
        "osFeatures=0x%llx sveVectorBytes=%u flagm=%d flagm2=%d lrcpc=%d "
        "ilrcpc=%d lrcpc3=%d sve2=%d aes=%d pmull=%d sha1=%d sha2=%d crc32=%d "
        "afp=%d fcma=%d frintts=%d ecv=%d rpres=%d",
        static_cast<unsigned long long>(profile.hwcap),
        static_cast<unsigned long long>(profile.hwcap2),
        static_cast<unsigned long long>(profile.features),
        static_cast<unsigned long long>(profile.os_features),
        profile.sve_vector_bytes,
        profile.Has(HostIsaFeature::FlagM) ? 1 : 0,
        profile.Has(HostIsaFeature::FlagM2) ? 1 : 0,
        profile.Has(HostIsaFeature::Lrcpc) ? 1 : 0,
        profile.Has(HostIsaFeature::Ilrcpc) ? 1 : 0,
        profile.Has(HostIsaFeature::Lrcpc3) ? 1 : 0,
        profile.Has(HostIsaFeature::Sve2) ? 1 : 0,
        profile.Has(HostIsaFeature::Aes) ? 1 : 0,
        profile.Has(HostIsaFeature::Pmull) ? 1 : 0,
        profile.Has(HostIsaFeature::Sha1) ? 1 : 0,
        profile.Has(HostIsaFeature::Sha2) ? 1 : 0,
        profile.Has(HostIsaFeature::Crc32) ? 1 : 0,
        profile.Has(HostIsaFeature::Afp) ? 1 : 0,
        profile.Has(HostIsaFeature::Fcma) ? 1 : 0,
        profile.Has(HostIsaFeature::Frintts) ? 1 : 0,
        profile.Has(HostIsaFeature::Ecv) ? 1 : 0,
        profile.Has(HostIsaFeature::Rpres) ? 1 : 0);
#endif
    return profile;
}

}

std::uint32_t CurrentThreadSveVectorBytes() noexcept {
#if defined(__ANDROID__) && defined(__aarch64__) && defined(PR_SVE_GET_VL) && \
    defined(PR_SVE_VL_LEN_MASK)
    const int vector_length = prctl(PR_SVE_GET_VL);
    if (vector_length > 0) {
        return static_cast<std::uint32_t>(vector_length) &
               static_cast<std::uint32_t>(PR_SVE_VL_LEN_MASK);
    }
#endif
    return 0;
}

HardwareTsoAttempt TryEnableHardwareTsoForCurrentThread() noexcept {
    const int saved_errno = errno;
    HardwareTsoAttempt attempt{};
#if defined(__ANDROID__) && defined(__aarch64__)
    errno = 0;
    attempt.get_before = prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0);
    attempt.get_before_errno = attempt.get_before == -1 ? errno : 0;
    if (attempt.get_before == PR_SET_MEM_MODEL_TSO) {
        attempt.enabled = true;
        attempt.get_after = attempt.get_before;
    } else if (attempt.get_before == PR_SET_MEM_MODEL_DEFAULT) {
        errno = 0;
        attempt.set_result =
            prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO, 0, 0, 0);
        attempt.set_errno = attempt.set_result == -1 ? errno : 0;


        attempt.enabled = attempt.set_result == 0;
        if (attempt.enabled) {
            errno = 0;
            attempt.get_after = prctl(PR_GET_MEM_MODEL, 0, 0, 0, 0);
            attempt.get_after_errno = attempt.get_after == -1 ? errno : 0;
        }
    }
    __android_log_print(
        attempt.enabled ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "LSX4-JIT",
        "[EXECUTOR_HARDWARE_TSO_THREAD] tid=%ld getBefore=%ld "
        "getBeforeErrno=%d set=%ld setErrno=%d getAfter=%ld "
        "getAfterErrno=%d enabled=%d",
        static_cast<long>(syscall(SYS_gettid)), attempt.get_before,
        attempt.get_before_errno, attempt.set_result, attempt.set_errno,
        attempt.get_after, attempt.get_after_errno, attempt.enabled ? 1 : 0);
#endif
    errno = saved_errno;
    return attempt;
}

void RestoreHardwareTsoAfterProbe(const HardwareTsoAttempt& attempt) noexcept {
#if defined(__ANDROID__) && defined(__aarch64__)
    if (!attempt.enabled || attempt.get_before != PR_SET_MEM_MODEL_DEFAULT ||
        attempt.set_result != 0) {
        return;
    }
    const int saved_errno = errno;
    errno = 0;
    const long restore_result =
        prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_DEFAULT, 0, 0, 0);
    const int restore_errno = restore_result == -1 ? errno : 0;
    __android_log_print(
        restore_result == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "LSX4-JIT",
        "[EXECUTOR_HARDWARE_TSO_PROBE_RESTORE] tid=%ld result=%ld errno=%d",
        static_cast<long>(syscall(SYS_gettid)), restore_result, restore_errno);
    errno = saved_errno;
#else
    (void)attempt;
#endif
}

const HostIsaProfile& GetHostIsaProfile() noexcept {
    static const HostIsaProfile profile = DetectHostIsaProfile();
    return profile;
}

}
