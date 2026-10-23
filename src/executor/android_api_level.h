// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <sys/system_properties.h>

#include <cstdlib>
#endif

namespace Executor {

[[nodiscard]] inline int AndroidDeviceApiLevel() noexcept {
#if defined(__ANDROID__)
    static const int api_level = []() noexcept {
        int value = 0;
        void* const library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (library != nullptr) {
            using GetDeviceApiLevelFn = int (*)();
            const auto get_device_api_level = reinterpret_cast<GetDeviceApiLevelFn>(
                dlsym(library, "android_get_device_api_level"));
            if (get_device_api_level != nullptr) {
                value = get_device_api_level();
            }
            dlclose(library);
        }

        if (value <= 0) {
            char property[PROP_VALUE_MAX]{};
            if (__system_property_get("ro.build.version.sdk", property) > 0) {
                char* end = nullptr;
                const long parsed = std::strtol(property, &end, 10);
                if (end != property && parsed > 0 && parsed <= 10'000) {
                    value = static_cast<int>(parsed);
                }
            }
        }
        return value;
    }();
    return api_level;
#else
    return 0;
#endif
}

} // namespace Executor
