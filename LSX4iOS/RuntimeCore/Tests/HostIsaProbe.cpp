// SPDX-License-Identifier: GPL-2.0-or-later
#include <cerrno>
#include <cstdio>
#include <cstring>
#include "executor/dynamic_translation/host_isa_profile.h"

static unsigned queries;

extern "C" int LSX4TestSysctlByName(const char* name, void* output,
                                   size_t* bytes, void*, size_t) {
    ++queries;
    if (!output || !bytes || *bytes != sizeof(int)) return -1;
    auto& value = *static_cast<int*>(output);
    value = 1;
    if (std::strcmp(name, "hw.optional.arm.FEAT_LSE") == 0 ||
        std::strcmp(name, "hw.optional.arm.FEAT_SHA1") == 0) return 0;
    if (std::strcmp(name, "hw.optional.arm.FEAT_FlagM") == 0) {
        errno = EIO; // Successful queries must not leak errno either.
        return 0;
    }
    if (std::strcmp(name, "hw.optional.arm.FEAT_LRCPC") == 0) {
        *bytes = 1; // A partial result must not authorize an instruction.
        return 0;
    }
    if (std::strcmp(name, "hw.optional.arm.FEAT_LRCPC2") == 0) {
        value = 2; // Require the documented enabled value, not truthiness.
        return 0;
    }
    if (std::strcmp(name, "hw.optional.arm.FEAT_AES") == 0) {
        value = 0;
        return 0;
    }
    errno = ENOENT;
    return -1; // Even with value=1, an unknown/failed query is unavailable.
}

int main() {
    using namespace Executor::Jit;
    errno = EBUSY;
    const auto& profile = GetHostIsaProfile();
    const auto expected = static_cast<std::uint64_t>(HostIsaFeature::Lse) |
                          static_cast<std::uint64_t>(HostIsaFeature::FlagM) |
                          static_cast<std::uint64_t>(HostIsaFeature::Sha1);
    const unsigned first_queries = queries;
    const bool passed = profile.features == expected && profile.hwcap == 0 &&
        profile.hwcap2 == 0 && profile.os_features == 0 &&
        profile.sve_vector_bytes == 0 && errno == EBUSY && first_queries > 0 &&
        &GetHostIsaProfile() == &profile && queries == first_queries;
    std::printf("APPLE_HOST_ISA_BOUNDARY %s invalidResultsRejected=1 errnoPreserved=1 cached=1\n",
                passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
