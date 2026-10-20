#include <adrenotools/driver.h>

#include <dlfcn.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s HOOK_LIBRARY_DIR CUSTOM_DRIVER_DIR DRIVER_SONAME\n",
                     argv[0]);
        return 2;
    }

    void* loader = adrenotools_open_libvulkan(
        RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, argv[1], argv[2], argv[3],
        nullptr, nullptr);
    if (loader == nullptr) {
        std::fprintf(stderr, "ADRENOTOOLS_SMOKE phase=open-loader result=fail\n");
        return 3;
    }

    auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(loader, "vkGetInstanceProcAddr"));
    if (get_instance_proc_addr == nullptr) {
        std::fprintf(stderr, "ADRENOTOOLS_SMOKE phase=gipa result=fail reason=%s\n", dlerror());
        return 4;
    }

    const auto enumerate_instance_version =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    const auto enumerate_instance_extensions =
        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
    const auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
        get_instance_proc_addr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (enumerate_instance_extensions == nullptr || create_instance == nullptr) {
        std::fprintf(stderr, "ADRENOTOOLS_SMOKE phase=global-functions result=fail\n");
        return 5;
    }

    uint32_t api_version = VK_API_VERSION_1_0;
    if (enumerate_instance_version != nullptr &&
        enumerate_instance_version(&api_version) != VK_SUCCESS) {
        std::fprintf(stderr, "ADRENOTOOLS_SMOKE phase=instance-version result=fail\n");
        return 6;
    }

    uint32_t extension_count = 0;
    VkResult result = enumerate_instance_extensions(nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions(extension_count);
    if (result == VK_SUCCESS && extension_count != 0) {
        result = enumerate_instance_extensions(nullptr, &extension_count, extensions.data());
        extensions.resize(extension_count);
    }
    const bool has_surface = result == VK_SUCCESS &&
                             HasExtension(extensions, VK_KHR_SURFACE_EXTENSION_NAME);
    const bool has_android_surface =
        result == VK_SUCCESS &&
        HasExtension(extensions, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    if (!has_surface || !has_android_surface) {
        std::fprintf(stderr,
                     "ADRENOTOOLS_SMOKE phase=instance-extensions result=fail surface=%d "
                     "androidSurface=%d count=%u vkResult=%d\n",
                     has_surface, has_android_surface, extension_count, result);
        return 7;
    }

    const char* enabled_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application_info{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lsx4-adrenotools-smoke",
        .applicationVersion = 1,
        .pEngineName = "lsx4-smoke",
        .engineVersion = 1,
        .apiVersion = api_version,
    };
    const VkInstanceCreateInfo instance_info{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = enabled_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    result = create_instance(&instance_info, nullptr, &instance);
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        std::fprintf(stderr,
                     "ADRENOTOOLS_SMOKE phase=create-instance result=fail vkResult=%d\n",
                     result);
        return 8;
    }

    const auto enumerate_physical_devices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_instance_proc_addr(
            instance, "vkEnumeratePhysicalDevices"));
    const auto get_physical_device_properties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceProperties"));
    const auto get_physical_device_properties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceProperties2"));
    const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
        get_instance_proc_addr(instance, "vkDestroyInstance"));
    if (enumerate_physical_devices == nullptr || get_physical_device_properties == nullptr ||
        destroy_instance == nullptr) {
        std::fprintf(stderr, "ADRENOTOOLS_SMOKE phase=instance-functions result=fail\n");
        return 9;
    }

    uint32_t physical_device_count = 0;
    result = enumerate_physical_devices(instance, &physical_device_count, nullptr);
    if (result != VK_SUCCESS || physical_device_count == 0) {
        std::fprintf(stderr,
                     "ADRENOTOOLS_SMOKE phase=physical-devices result=fail vkResult=%d "
                     "count=%u\n",
                     result, physical_device_count);
        destroy_instance(instance, nullptr);
        return 10;
    }
    std::vector<VkPhysicalDevice> physical_devices(physical_device_count);
    result = enumerate_physical_devices(instance, &physical_device_count,
                                        physical_devices.data());
    if (result != VK_SUCCESS || physical_device_count == 0) {
        std::fprintf(stderr,
                     "ADRENOTOOLS_SMOKE phase=physical-device-list result=fail vkResult=%d "
                     "count=%u\n",
                     result, physical_device_count);
        destroy_instance(instance, nullptr);
        return 11;
    }

    VkPhysicalDeviceProperties properties{};
    get_physical_device_properties(physical_devices[0], &properties);
    VkPhysicalDeviceDriverProperties driver_properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    if (get_physical_device_properties2 != nullptr) {
        VkPhysicalDeviceProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driver_properties,
        };
        get_physical_device_properties2(physical_devices[0], &properties2);
    }

    std::printf(
        "ADRENOTOOLS_SMOKE result=pass api=%u.%u.%u instanceExtensions=%u "
        "physicalDevices=%u deviceName=%s driverName=%s driverInfo=%s "
        "driverVersion=%u vendorId=0x%04x deviceId=0x%04x\n",
        VK_VERSION_MAJOR(api_version), VK_VERSION_MINOR(api_version),
        VK_VERSION_PATCH(api_version), extension_count, physical_device_count,
        properties.deviceName, driver_properties.driverName, driver_properties.driverInfo,
        properties.driverVersion, properties.vendorID, properties.deviceID);

    destroy_instance(instance, nullptr);
    return 0;
}
