#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" int executor_lsx4_runtime_set_managed_optimization(int, int);

namespace AppleManagedOptionControl {
// Launch-only experiment: retain every original Android resolver call, then
// disable selected options before CPU/profile/game initialization. No polling,
// persistence, title routing, or changes when the diagnostic is absent.
inline bool Parse(const char* value, std::uint32_t& mask) {
    mask = 0;
    if (!value) return true;
    if (!*value) return false;
    for (const char* p = value; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        if (mask > 4194302u / 10u) return false;
        mask = mask * 10u + static_cast<unsigned>(*p - '0');
        if (mask > 4194302u) return false;
    }
    return mask != 0 && (mask & 1u) == 0;
}
inline bool Apply() {
    std::uint32_t mask = 0;
    if (!Parse(std::getenv("LSX4_APPLE_MANAGED_DISABLE_MASK"), mask)) return false;
    if (!mask) return true;
    for (int option = 1; option <= 21; ++option) {
        if ((mask & (1u << option)) &&
            executor_lsx4_runtime_set_managed_optimization(option, 0) < 0) return false;
    }
    std::fprintf(stderr, "[APPLE_MANAGED_OPTION_CONTROL] disabledMask=%u diagnostic=1\n", mask);
    return true;
}
}
