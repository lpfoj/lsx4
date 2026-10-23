// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__ANDROID__) && !defined(VK_USE_PLATFORM_ANDROID_KHR)
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <string>

#include <vulkan/vulkan.h>

namespace Vulkan::DriverAxis {

// Configuration is supplied by the Android launcher before any Vulkan object is created.
// The system path remains the default when this function is never called.
int Configure(bool request_turnip, const char* native_library_dir,
              const char* turnip_library_dir, const char* report_path,
              const char* turnip_sha256);

// Returns system_get_instance_proc_addr unchanged unless the requested Turnip bundle passes
// the raw-Vulkan loader, identity and capability preflight. A disposable probe additionally
// creates a logical device; never request that mode from the live renderer process. The returned
// loader and all helper DSOs are retained because Vulkan dispatch pointers may outlive the
// initial instance.
PFN_vkGetInstanceProcAddr SelectGetInstanceProcAddr(
    PFN_vkGetInstanceProcAddr system_get_instance_proc_addr,
    bool disposable_probe = false);

// Reuses information already queried by the renderer; the system-default path performs no
// additional Vulkan calls merely for reporting.
void RecordActualDevice(const VkPhysicalDeviceProperties& properties, const char* driver_name,
                        const char* driver_info);

// Returns the package identity only after the custom loader has successfully
// selected Turnip.  Callers must still combine this raw evidence with the
// actual VkPhysicalDevice identity in the central capability router.
std::string EffectivePackageSha256();

const char* StatusJson();

} // namespace Vulkan::DriverAxis
