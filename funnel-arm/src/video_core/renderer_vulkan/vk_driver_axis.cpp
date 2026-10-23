// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_driver_axis.h"

#if defined(__ANDROID__)

#include <android/api-level.h>
#include <android/log.h>
#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Vulkan::DriverAxis {
namespace {

constexpr std::string_view AdrenoToolsName = "libadrenotools.so";
constexpr std::string_view MainHookName = "libmain_hook.so";
constexpr std::string_view HookImplName = "libhook_impl.so";
constexpr std::string_view TurnipName = "libvulkan_lsx4_turnip.so";
constexpr std::string_view TurnipAutotuneMarkerName = "turnip-autotune-algo";
constexpr std::string_view TurnipAutotuneEnvironmentName = "TU_AUTOTUNE_ALGO";
constexpr std::size_t TurnipAutotuneMarkerMaxBytes = 64;
constexpr std::array TurnipAutotuneAlgorithms{
    std::string_view{"bandwidth"},
    std::string_view{"prefer_sysmem"},
    std::string_view{"profiled_imm"},
};
constexpr std::string_view Adreno840Name = "Adreno (TM) 840";
constexpr std::string_view IncompatibleAdreno840Turnip2620Sha256 =
    "0968ca33df5978be972090751222275ccfe992d5c306ca606f2150246285989d";
constexpr std::string_view IncompatibleAdreno840Turnip2621Sha256 =
    "0423fb21c93deadee03296610aeb57197939642bd114ab0ee0976b9112e101f4";
constexpr std::string_view IncompatibleAdreno840TurnipGen8V25Sha256 =
    "0a3fbb59829c5c889c9de95feb05d2dc34ca8f5a5af1396bb162cfeac672898d";
constexpr int AdrenoToolsDriverCustom = 1 << 0;

using OpenLibvulkanFn = void* (*)(int, int, const char*, const char*, const char*, const char*,
                                  const char*, void**);

struct State {
    bool requested_turnip{};
    bool selection_locked{};
    std::string native_library_dir;
    std::string turnip_library_dir;
    std::string report_path;
    std::string turnip_path;
    std::string turnip_sha256;
    std::string effective{"system"};
    std::string fallback_reason{"none"};
    std::string driver_name;
    std::string driver_info;
    std::string driver_version;
    std::string driver_id;
    std::string device_name;
    std::string turnip_autotune_algo{"default"};
    std::string turnip_autotune_source{"default"};
    bool turnip_autotune_marker_applied{};
    bool turnip_autotune_previous_environment_present{};
    std::string turnip_autotune_previous_environment_value;
    std::uint32_t api_version{};
    bool mapping_verified{};
    bool logical_device_probe_passed{};
    void* adrenotools_library{};
    void* isolated_vulkan_loader{};
    PFN_vkGetInstanceProcAddr selected_gipa{};
    std::string status_json;
};

std::mutex state_mutex;
State state;

std::filesystem::path LibraryPath(const std::string_view name) {
    return std::filesystem::path(state.native_library_dir) / name;
}

std::filesystem::path TurnipLibraryPath() {
    return std::filesystem::path(state.turnip_library_dir) / TurnipName;
}

std::string NormalizePath(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return (error ? path.lexically_normal() : canonical).string();
}

bool IsRegularFile(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

std::filesystem::path DiagnosticFile(const std::string_view name) {
    if (state.report_path.empty()) {
        return {};
    }
    return std::filesystem::path(state.report_path).parent_path() / name;
}

bool DiagnosticMarkerPresent(const std::string_view name) {
    const auto path = DiagnosticFile(name);
    return !path.empty() && IsRegularFile(path);
}

bool IsTurnipAutotuneAlgorithm(const std::string_view value) {
    return std::ranges::find(TurnipAutotuneAlgorithms, value) !=
           TurnipAutotuneAlgorithms.end();
}

std::string TurnipAutotuneEnvironmentEvidence(const char* value) {
    if (value == nullptr) {
        return "default";
    }
    if (IsTurnipAutotuneAlgorithm(value)) {
        return value;
    }
    return "external";
}

bool ReadTurnipAutotuneMarkerLocked(std::string& algorithm) {
    const auto path = DiagnosticFile(TurnipAutotuneMarkerName);
    if (path.empty() || !IsRegularFile(path)) {
        return false;
    }

    std::error_code error;
    const auto marker_size = std::filesystem::file_size(path, error);
    if (error || marker_size == 0 || marker_size > TurnipAutotuneMarkerMaxBytes) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_VULKAN_DRIVER_AXIS] ignored invalid Turnip "
                            "autotune marker path=%s",
                            path.c_str());
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    std::string contents(static_cast<std::size_t>(marker_size), '\0');
    if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size())) ||
        input.peek() != std::char_traits<char>::eof()) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_VULKAN_DRIVER_AXIS] ignored unreadable Turnip "
                            "autotune marker path=%s",
                            path.c_str());
        return false;
    }

    if (contents.ends_with("\r\n")) {
        contents.resize(contents.size() - 2);
    } else if (contents.ends_with('\n')) {
        contents.pop_back();
    }
    if (!IsTurnipAutotuneAlgorithm(contents)) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_VULKAN_DRIVER_AXIS] ignored invalid Turnip "
                            "autotune marker path=%s",
                            path.c_str());
        return false;
    }

    algorithm = std::move(contents);
    return true;
}

void ApplyTurnipAutotuneMarkerLocked() {
    const char* previous = std::getenv(TurnipAutotuneEnvironmentName.data());
    state.turnip_autotune_algo = TurnipAutotuneEnvironmentEvidence(previous);
    state.turnip_autotune_source = previous == nullptr ? "default" : "environment";
    state.turnip_autotune_marker_applied = false;

    std::string algorithm;
    if (!ReadTurnipAutotuneMarkerLocked(algorithm)) {
        return;
    }

    state.turnip_autotune_previous_environment_present = previous != nullptr;
    state.turnip_autotune_previous_environment_value = previous ? previous : "";
    if (setenv(TurnipAutotuneEnvironmentName.data(), algorithm.c_str(), 1) != 0) {
        const int saved_errno = errno;
        state.turnip_autotune_previous_environment_present = false;
        state.turnip_autotune_previous_environment_value.clear();
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_VULKAN_DRIVER_AXIS] failed to apply Turnip "
                            "autotune marker errno=%d",
                            saved_errno);
        return;
    }

    state.turnip_autotune_algo = algorithm;
    state.turnip_autotune_source = "marker";
    state.turnip_autotune_marker_applied = true;
    __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_VULKAN_DRIVER_AXIS] diagnostic TU_AUTOTUNE_ALGO=%s",
                        algorithm.c_str());
}

void RestoreTurnipAutotuneEnvironmentLocked() {
    if (!state.turnip_autotune_marker_applied) {
        return;
    }

    const int result = state.turnip_autotune_previous_environment_present
                           ? setenv(TurnipAutotuneEnvironmentName.data(),
                                    state.turnip_autotune_previous_environment_value.c_str(), 1)
                           : unsetenv(TurnipAutotuneEnvironmentName.data());
    if (result != 0) {
        const int saved_errno = errno;
        state.turnip_autotune_source = "restore-failed";
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_VULKAN_DRIVER_AXIS] failed to restore "
                            "TU_AUTOTUNE_ALGO errno=%d",
                            saved_errno);
        return;
    }

    state.turnip_autotune_algo =
        TurnipAutotuneEnvironmentEvidence(
            state.turnip_autotune_previous_environment_present
                ? state.turnip_autotune_previous_environment_value.c_str()
                : nullptr);
    state.turnip_autotune_source = state.turnip_autotune_previous_environment_present
                                       ? "restored-environment"
                                       : "restored-default";
    state.turnip_autotune_marker_applied = false;
    state.turnip_autotune_previous_environment_present = false;
    state.turnip_autotune_previous_environment_value.clear();
}

bool ApplyTurnipDebugOptionsFileLocked(const std::string_view file_name,
                                       const char* environment_name) {
    const auto path = DiagnosticFile(file_name);
    if (path.empty() || !IsRegularFile(path)) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    std::string options;
    std::getline(input, options);
    const bool valid = !options.empty() && options.size() <= 256 &&
                       std::ranges::all_of(options, [](const unsigned char character) {
                           return (character >= 'a' && character <= 'z') ||
                                  (character >= '0' && character <= '9') ||
                                  character == '_' || character == ',';
                       });
    if (!valid) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_VULKAN_DRIVER_AXIS] ignored invalid Turnip debug "
                            "options path=%s",
                            path.c_str());
        return false;
    }
    setenv(environment_name, options.c_str(), 1);
    __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_VULKAN_DRIVER_AXIS] diagnostic %s=%s path=%s",
                        environment_name, options.c_str(), path.c_str());
    return true;
}

void ApplyTurnipDebugOptionsLocked() {
    const bool tu_debug =
        ApplyTurnipDebugOptionsFileLocked("turnip-debug-options", "TU_DEBUG");
    const bool ir3_debug =
        ApplyTurnipDebugOptionsFileLocked("turnip-ir3-debug-options", "IR3_SHADER_DEBUG");
    const bool force_ps_const = DiagnosticMarkerPresent("turnip-force-ps-const");
    if (force_ps_const) {
        setenv("EXECUTOR_FORCE_PS_CONST_SOLID", "1", 1);
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_VULKAN_DRIVER_AXIS] diagnostic "
                            "EXECUTOR_FORCE_PS_CONST_SOLID=1");
    }
    const bool force_fs_image_const =
        DiagnosticMarkerPresent("turnip-force-fs-image-const");
    if (force_fs_image_const) {
        setenv("EXECUTOR_FORCE_FS_IMAGE_CONST", "1", 1);
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_VULKAN_DRIVER_AXIS] diagnostic "
                            "EXECUTOR_FORCE_FS_IMAGE_CONST=1");
    }
    if (!tu_debug && !ir3_debug && !force_ps_const && !force_fs_image_const) {
        return;
    }
    // Several Turnip debug flags alter backend compilation without contributing
    // to Mesa's on-disk shader-cache key. Diagnostic runs must observe the flag,
    // not a shader compiled by an earlier default-mode process.
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                        "[EXECUTOR_VULKAN_DRIVER_AXIS] diagnostic mesaShaderCache=disabled");
}

std::string VersionString(const std::uint32_t version) {
    return std::to_string(VK_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_VERSION_MINOR(version)) + "." +
           std::to_string(VK_VERSION_PATCH(version));
}

void RefreshStatusLocked() {
    const bool effective_turnip = state.effective == "turnip";
    const char* const requested_source =
        state.turnip_library_dir == state.native_library_dir ? "bundled" : "imported";
    nlohmann::json report{
        {"schema", "lsx4.vulkan-driver-axis.v1"},
        {"requested", state.requested_turnip ? "turnip" : "system"},
        {"effective", state.effective},
        {"fallbackReason", state.fallback_reason},
        {"driverName", state.driver_name},
        {"driverInfo", state.driver_info},
        {"driverVersion", state.driver_version},
        {"driverId", state.driver_id},
        {"deviceName", state.device_name},
        {"turnipAutotuneAlgo", state.turnip_autotune_algo},
        {"turnipAutotuneSource", state.turnip_autotune_source},
        {"turnipAutotuneMarkerApplied", state.turnip_autotune_marker_applied},
        {"apiVersion", VersionString(state.api_version)},
        {"requestedPath", state.requested_turnip ? state.turnip_path : "/system/lib64/libvulkan.so"},
        {"requestedSha256", state.requested_turnip ? state.turnip_sha256 : ""},
        {"requestedPackageSource", state.requested_turnip ? requested_source : "system"},
        {"path", effective_turnip ? state.turnip_path : "/system/lib64/libvulkan.so"},
        {"sha256", effective_turnip ? state.turnip_sha256 : ""},
        {"packageSource", effective_turnip ? requested_source : "system"},
        {"mappingVerified", state.mapping_verified},
        {"logicalDeviceProbePassed", state.logical_device_probe_passed},
        {"selectionLocked", state.selection_locked},
        {"validationAvailable", !effective_turnip},
        {"requiredInstanceExtensions",
         {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME}},
        {"requiredDeviceExtensions", {VK_KHR_SWAPCHAIN_EXTENSION_NAME}},
    };
    state.status_json = report.dump();

    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_VULKAN_DRIVER_AXIS] %s", state.status_json.c_str());
    if (state.report_path.empty()) {
        return;
    }
    const std::filesystem::path destination{state.report_path};
    const std::filesystem::path temporary = destination.string() + ".tmp";
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_VULKAN_DRIVER_AXIS] report-open-failed path=%s",
                            state.report_path.c_str());
        return;
    }
    output << state.status_json << '\n';
    output.close();
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(destination, error);
        error.clear();
        std::filesystem::rename(temporary, destination, error);
    }
}

PFN_vkGetInstanceProcAddr FallbackLocked(PFN_vkGetInstanceProcAddr system_gipa,
                                         const char* reason) {
    RestoreTurnipAutotuneEnvironmentLocked();
    state.effective = "system";
    state.fallback_reason = reason;
    state.mapping_verified = false;
    state.logical_device_probe_passed = false;
    state.selected_gipa = system_gipa;
    RefreshStatusLocked();
    return system_gipa;
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::ranges::any_of(extensions, [&](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

bool TurnipMappingIsPresent() {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (!state.turnip_path.empty() && line.find(state.turnip_path) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const char* PreflightLocked(PFN_vkGetInstanceProcAddr gipa,
                            const bool disposable_probe) {
    const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    const auto enumerate_instance_extensions =
        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    const auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (enumerate_version == nullptr || enumerate_instance_extensions == nullptr ||
        create_instance == nullptr) {
        return "missing-global-entrypoint";
    }

    std::uint32_t loader_api_version = VK_API_VERSION_1_0;
    if (enumerate_version(&loader_api_version) != VK_SUCCESS ||
        loader_api_version < VK_API_VERSION_1_3) {
        return "insufficient-instance-api";
    }

    std::uint32_t extension_count{};
    VkResult result = enumerate_instance_extensions(nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> instance_extensions(extension_count);
    if (result == VK_SUCCESS && extension_count != 0) {
        result = enumerate_instance_extensions(nullptr, &extension_count,
                                               instance_extensions.data());
        instance_extensions.resize(extension_count);
    }
    if (result != VK_SUCCESS) {
        return "instance-extension-query-failed";
    }
    if (!HasExtension(instance_extensions, VK_KHR_SURFACE_EXTENSION_NAME)) {
        return "missing-vk-khr-surface";
    }
    if (!HasExtension(instance_extensions, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        return "missing-vk-khr-android-surface";
    }

    constexpr std::array enabled_extensions{VK_KHR_SURFACE_EXTENSION_NAME,
                                             VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "LSX4 Turnip preflight",
        .applicationVersion = 1,
        .pEngineName = "LSX4",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
        .enabledExtensionCount = static_cast<std::uint32_t>(enabled_extensions.size()),
        .ppEnabledExtensionNames = enabled_extensions.data(),
    };
    VkInstance instance{};
    result = create_instance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        return "create-instance-failed";
    }

    const auto destroy_instance =
        reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    const auto enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        gipa(instance, "vkEnumeratePhysicalDevices"));
    const auto enumerate_device_extensions =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        gipa(instance, "vkGetPhysicalDeviceProperties"));
    const auto get_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        gipa(instance, "vkGetPhysicalDeviceProperties2"));
    const auto get_queue_families =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            gipa(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto create_device =
        reinterpret_cast<PFN_vkCreateDevice>(gipa(instance, "vkCreateDevice"));
    const auto get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        gipa(instance, "vkGetDeviceProcAddr"));
    if (destroy_instance == nullptr || enumerate_devices == nullptr ||
        enumerate_device_extensions == nullptr || get_properties == nullptr ||
        get_queue_families == nullptr || create_device == nullptr ||
        get_device_proc_addr == nullptr) {
        if (destroy_instance != nullptr) {
            destroy_instance(instance, nullptr);
        }
        return "missing-instance-entrypoint";
    }

    std::uint32_t device_count{};
    result = enumerate_devices(instance, &device_count, nullptr);
    std::vector<VkPhysicalDevice> devices(device_count);
    if (result == VK_SUCCESS && device_count != 0) {
        result = enumerate_devices(instance, &device_count, devices.data());
        devices.resize(device_count);
    }
    if (result != VK_SUCCESS || devices.empty()) {
        destroy_instance(instance, nullptr);
        return "no-physical-device";
    }

    VkPhysicalDevice chosen{};
    std::uint32_t chosen_queue_family = UINT32_MAX;
    for (const VkPhysicalDevice device : devices) {
        std::uint32_t queue_count{};
        get_queue_families(device, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        if (queue_count != 0) {
            get_queue_families(device, &queue_count, queues.data());
        }
        for (std::uint32_t index = 0; index < queues.size(); ++index) {
            if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                chosen = device;
                chosen_queue_family = index;
                break;
            }
        }
        if (chosen != VK_NULL_HANDLE) {
            break;
        }
    }
    if (chosen == VK_NULL_HANDLE || chosen_queue_family == UINT32_MAX) {
        destroy_instance(instance, nullptr);
        return "missing-graphics-queue";
    }

    std::uint32_t device_extension_count{};
    result = enumerate_device_extensions(chosen, nullptr, &device_extension_count, nullptr);
    std::vector<VkExtensionProperties> device_extensions(device_extension_count);
    if (result == VK_SUCCESS && device_extension_count != 0) {
        result = enumerate_device_extensions(chosen, nullptr, &device_extension_count,
                                             device_extensions.data());
        device_extensions.resize(device_extension_count);
    }
    if (result != VK_SUCCESS ||
        !HasExtension(device_extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        destroy_instance(instance, nullptr);
        return "missing-vk-khr-swapchain";
    }

    VkPhysicalDeviceProperties properties{};
    get_properties(chosen, &properties);
    state.api_version = properties.apiVersion;
    state.device_name = properties.deviceName;
    state.driver_version = std::to_string(properties.driverVersion);
    VkPhysicalDeviceDriverProperties driver{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    if (get_properties2 != nullptr) {
        VkPhysicalDeviceProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driver,
        };
        get_properties2(chosen, &properties2);
        state.driver_name = driver.driverName;
        state.driver_info = driver.driverInfo;
        state.driver_id = std::to_string(static_cast<std::uint32_t>(driver.driverID));
    } else {
        destroy_instance(instance, nullptr);
        return "missing-driver-properties";
    }

    std::string driver_identity = state.driver_name + " " + state.driver_info;
    std::ranges::transform(driver_identity, driver_identity.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (driver.driverID != VK_DRIVER_ID_MESA_TURNIP ||
        (driver_identity.find("mesa") == std::string::npos &&
         driver_identity.find("turnip") == std::string::npos)) {
        destroy_instance(instance, nullptr);
        return "custom-driver-identity-mismatch";
    }

    // Creating and destroying a throwaway Turnip device can perturb process-wide
    // driver state. The import verifier runs in a disposable :driverprobe
    // process, so keep the destructive half of the preflight there. The live
    // renderer still verifies the loader, instance, physical-device identity,
    // queue family and required extensions before accepting the selected GIPA.
    if (disposable_probe) {
        constexpr float queue_priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_create_info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = chosen_queue_family,
            .queueCount = 1,
            .pQueuePriorities = &queue_priority,
        };
        constexpr std::array enabled_device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        const VkDeviceCreateInfo device_create_info{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue_create_info,
            .enabledExtensionCount =
                static_cast<std::uint32_t>(enabled_device_extensions.size()),
            .ppEnabledExtensionNames = enabled_device_extensions.data(),
        };
        VkDevice logical_device{};
        result = create_device(chosen, &device_create_info, nullptr, &logical_device);
        if (result != VK_SUCCESS || logical_device == VK_NULL_HANDLE) {
            destroy_instance(instance, nullptr);
            return "create-device-failed";
        }
        const auto destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(
            get_device_proc_addr(logical_device, "vkDestroyDevice"));
        if (destroy_device == nullptr) {
            destroy_instance(instance, nullptr);
            return "missing-destroy-device";
        }
        destroy_device(logical_device, nullptr);
        state.logical_device_probe_passed = true;
    }
    destroy_instance(instance, nullptr);

    if (!TurnipMappingIsPresent()) {
        return "custom-driver-not-effective";
    }
    state.mapping_verified = true;
    // This exact app-private Mesa build cannot obtain QTI gralloc metadata on
    // the Vivo Adreno 840.  A native-buffer layout fix restores WSI, but the
    // driver still corrupts guest draw output (reproducible horizontal rows)
    // and runs at roughly half the system-driver frame rate.  Fail closed for
    // the measured artifact/device pairs; an unmeasured rebuilt driver falls
    // out of this guard because its content hash changes.
    const bool known_incompatible_sha =
        state.turnip_sha256 == IncompatibleAdreno840Turnip2620Sha256 ||
        state.turnip_sha256 == IncompatibleAdreno840Turnip2621Sha256 ||
        state.turnip_sha256 == IncompatibleAdreno840TurnipGen8V25Sha256;
    const bool known_incompatible =
        state.device_name.find(Adreno840Name) != std::string::npos &&
        known_incompatible_sha;
    if (known_incompatible &&
        !DiagnosticMarkerPresent("vulkan-driver-allow-known-incompatible")) {
        return "known-incompatible-adreno840-draw-layout";
    }
    if (known_incompatible) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_VULKAN_DRIVER_AXIS] diagnostic override for known-incompatible "
            "Adreno 840 Turnip pair");
    }
    return nullptr;
}

} // namespace

int Configure(const bool request_turnip, const char* native_library_dir,
              const char* turnip_library_dir, const char* report_path,
              const char* turnip_sha256) {
    std::lock_guard lock(state_mutex);
    if (state.selection_locked) {
        return state.effective == "turnip" ? 1 : 0;
    }
    state.requested_turnip = request_turnip;
    state.native_library_dir = NormalizePath(native_library_dir ? native_library_dir : "");
    state.turnip_library_dir = NormalizePath(turnip_library_dir ? turnip_library_dir : "");
    if (state.turnip_library_dir.empty()) {
        state.turnip_library_dir = state.native_library_dir;
    }
    state.report_path = report_path ? report_path : "";
    state.turnip_path = NormalizePath(TurnipLibraryPath());
    state.turnip_sha256 = turnip_sha256 ? turnip_sha256 : "";
    state.effective = "system";
    state.fallback_reason = "none";
    state.driver_name.clear();
    state.driver_info.clear();
    state.driver_version.clear();
    state.driver_id.clear();
    state.device_name.clear();
    state.turnip_autotune_algo = "default";
    state.turnip_autotune_source = "default";
    state.turnip_autotune_marker_applied = false;
    state.turnip_autotune_previous_environment_present = false;
    state.turnip_autotune_previous_environment_value.clear();
    state.api_version = 0;
    state.mapping_verified = false;
    state.logical_device_probe_passed = false;
    RefreshStatusLocked();
    return 0;
}

PFN_vkGetInstanceProcAddr SelectGetInstanceProcAddr(
    const PFN_vkGetInstanceProcAddr system_get_instance_proc_addr,
    const bool disposable_probe) {
    std::lock_guard lock(state_mutex);
    if (state.selection_locked) {
        return state.selected_gipa ? state.selected_gipa : system_get_instance_proc_addr;
    }
    state.selection_locked = true;
    state.selected_gipa = system_get_instance_proc_addr;
    if (!state.requested_turnip) {
        state.effective = "system";
        state.fallback_reason = "none";
        RefreshStatusLocked();
        return system_get_instance_proc_addr;
    }
    if (android_get_device_api_level() < 28) {
        return FallbackLocked(system_get_instance_proc_addr, "android-api-too-old");
    }
    if (state.native_library_dir.empty()) {
        return FallbackLocked(system_get_instance_proc_addr, "invalid-native-library-dir");
    }
    if (state.turnip_library_dir.empty()) {
        return FallbackLocked(system_get_instance_proc_addr, "invalid-turnip-library-dir");
    }
    if (!IsRegularFile(TurnipLibraryPath())) {
        return FallbackLocked(system_get_instance_proc_addr, "driver-file-missing");
    }
    if (!IsRegularFile(LibraryPath(AdrenoToolsName)) ||
        !IsRegularFile(LibraryPath(MainHookName)) ||
        !IsRegularFile(LibraryPath(HookImplName))) {
        return FallbackLocked(system_get_instance_proc_addr, "loader-bundle-incomplete");
    }

    ApplyTurnipAutotuneMarkerLocked();
    ApplyTurnipDebugOptionsLocked();

    state.adrenotools_library =
        dlopen(LibraryPath(AdrenoToolsName).c_str(), RTLD_NOW | RTLD_LOCAL);
    if (state.adrenotools_library == nullptr) {
        return FallbackLocked(system_get_instance_proc_addr, "adrenotools-dlopen-failed");
    }
    const auto open_libvulkan = reinterpret_cast<OpenLibvulkanFn>(
        dlsym(state.adrenotools_library, "adrenotools_open_libvulkan"));
    if (open_libvulkan == nullptr) {
        return FallbackLocked(system_get_instance_proc_addr, "adrenotools-symbol-missing");
    }

    std::string hook_dir = state.native_library_dir;
    if (!hook_dir.ends_with('/')) {
        hook_dir.push_back('/');
    }
    std::string driver_dir = state.turnip_library_dir;
    if (!driver_dir.ends_with('/')) {
        driver_dir.push_back('/');
    }
    std::string temporary_dir = std::filesystem::path(state.report_path).parent_path().string();
    if (temporary_dir.empty()) {
        temporary_dir = state.native_library_dir;
    }
    state.isolated_vulkan_loader =
        open_libvulkan(RTLD_NOW | RTLD_LOCAL, AdrenoToolsDriverCustom,
                       temporary_dir.c_str(), hook_dir.c_str(), driver_dir.c_str(),
                       TurnipName.data(), nullptr, nullptr);
    if (state.isolated_vulkan_loader == nullptr) {
        return FallbackLocked(system_get_instance_proc_addr, "isolated-loader-open-failed");
    }
    const auto custom_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(state.isolated_vulkan_loader, "vkGetInstanceProcAddr"));
    if (custom_gipa == nullptr) {
        return FallbackLocked(system_get_instance_proc_addr, "isolated-loader-gipa-missing");
    }
    if (const char* reason = PreflightLocked(custom_gipa, disposable_probe); reason != nullptr) {
        return FallbackLocked(system_get_instance_proc_addr, reason);
    }

    state.effective = "turnip";
    state.fallback_reason = "none";
    state.selected_gipa = custom_gipa;
    RefreshStatusLocked();
    return custom_gipa;
}

void RecordActualDevice(const VkPhysicalDeviceProperties& properties, const char* driver_name,
                        const char* driver_info) {
    std::lock_guard lock(state_mutex);
    state.api_version = properties.apiVersion;
    state.device_name = properties.deviceName;
    state.driver_version = std::to_string(properties.driverVersion);
    state.driver_name = driver_name ? driver_name : "";
    state.driver_info = driver_info ? driver_info : "";
    RefreshStatusLocked();
}

std::string EffectivePackageSha256() {
    std::lock_guard lock(state_mutex);
    return state.effective == "turnip" ? state.turnip_sha256 : std::string{};
}

const char* StatusJson() {
    static thread_local std::string snapshot;
    std::lock_guard lock(state_mutex);
    if (state.status_json.empty()) {
        RefreshStatusLocked();
    }
    snapshot = state.status_json;
    return snapshot.c_str();
}

} // namespace Vulkan::DriverAxis

#else

namespace Vulkan::DriverAxis {

int Configure(bool, const char*, const char*, const char*, const char*) {
    return 0;
}

PFN_vkGetInstanceProcAddr SelectGetInstanceProcAddr(
    PFN_vkGetInstanceProcAddr system_get_instance_proc_addr, bool) {
    return system_get_instance_proc_addr;
}

void RecordActualDevice(const VkPhysicalDeviceProperties&, const char*, const char*) {}

std::string EffectivePackageSha256() {
    return {};
}

const char* StatusJson() {
    return R"({"schema":"lsx4.vulkan-driver-axis.v1","requested":"system","effective":"system","fallbackReason":"non-android"})";
}

} // namespace Vulkan::DriverAxis

#endif
