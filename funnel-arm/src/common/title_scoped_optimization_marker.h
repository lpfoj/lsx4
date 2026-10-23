// SPDX-FileCopyrightText: Copyright 2026 LSX4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdio>
#include <string_view>

#include "common/memory_patcher.h"

#if defined(__ANDROID__)
#include <unistd.h>
#endif

namespace Common {

// Performance experiment markers used to be process-global files.  That made
// an accepted Bloodborne A/B silently change every other title launched with
// the same application data.  Resolve opt-ins from a title namespace instead:
//
//   files/lsx4-home/title-markers/<TITLE_ID>/<marker>
//
// The global preserve/diagnostic markers intentionally remain separate.  This
// helper is only for run-enable optimization opt-ins whose correctness and
// performance acceptance belong to one game profile.
inline bool TitleScopedOptimizationMarkerEnabled(
    const std::string_view title, const std::string_view marker) noexcept {
#if defined(__ANDROID__)
    const auto safe_component = [](const std::string_view value) {
        if (value.empty()) {
            return false;
        }
        for (const char ch : value) {
            const bool ascii_alnum =
                (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= 'a' && ch <= 'z');
            if (!ascii_alnum && ch != '-' && ch != '_') {
                return false;
            }
        }
        return true;
    };
    if (!safe_component(title) || !safe_component(marker)) {
        return false;
    }

    std::array<char, 512> path{};
    const int length = std::snprintf(
        path.data(), path.size(),
        "/data/data/app.lsx4.android/files/lsx4-home/title-markers/%.*s/%.*s",
        static_cast<int>(title.size()), title.data(),
        static_cast<int>(marker.size()), marker.data());
    return length > 0 && static_cast<size_t>(length) < path.size() &&
           ::access(path.data(), F_OK) == 0;
#else
    (void)title;
    (void)marker;
    return false;
#endif
}

inline bool TitleScopedOptimizationMarkerEnabled(
    const std::string_view marker) noexcept {
    return TitleScopedOptimizationMarkerEnabled(
        MemoryPatcher::g_game_serial, marker);
}

} // namespace Common
