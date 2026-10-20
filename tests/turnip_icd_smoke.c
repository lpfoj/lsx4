#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

typedef VkResult(VKAPI_PTR* PfnIcdNegotiate)(uint32_t* supported_version);

static bool HasExtension(const VkExtensionProperties* properties, uint32_t count,
                         const char* name) {
    for (uint32_t index = 0; index < count; ++index) {
        if (strcmp(properties[index].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s /absolute/path/to/turnip.so\n", argv[0]);
        return 2;
    }

    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "TURNIP_SMOKE phase=dlopen result=fail reason=%s\n", dlerror());
        return 3;
    }

    PfnIcdNegotiate negotiate =
        (PfnIcdNegotiate)dlsym(library, "vk_icdNegotiateLoaderICDInterfaceVersion");
    PFN_vkGetInstanceProcAddr get_instance_proc_addr =
        (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    const char* entrypoint = "vkGetInstanceProcAddr";
    bool direct_loader = get_instance_proc_addr != NULL;
    if (get_instance_proc_addr == NULL) {
        get_instance_proc_addr =
            (PFN_vkGetInstanceProcAddr)dlsym(library, "vk_icdGetInstanceProcAddr");
        entrypoint = "vk_icdGetInstanceProcAddr";
    }
    if (get_instance_proc_addr == NULL || (!direct_loader && negotiate == NULL)) {
        fprintf(stderr,
                "TURNIP_SMOKE phase=entrypoints result=fail gipa=%d negotiate=%d\n",
                get_instance_proc_addr != NULL, negotiate != NULL);
        dlclose(library);
        return 4;
    }

    uint32_t interface_version = 0;
    if (negotiate != NULL) {
        interface_version = 7;
        const VkResult negotiate_result = negotiate(&interface_version);
        if (negotiate_result != VK_SUCCESS) {
            fprintf(stderr,
                    "TURNIP_SMOKE phase=negotiate result=fail vkResult=%d version=%u\n",
                    negotiate_result, interface_version);
            dlclose(library);
            return 5;
        }
    }

    PFN_vkEnumerateInstanceVersion enumerate_instance_version =
        (PFN_vkEnumerateInstanceVersion)get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t api_version = VK_API_VERSION_1_0;
    if (enumerate_instance_version != NULL &&
        enumerate_instance_version(&api_version) != VK_SUCCESS) {
        fprintf(stderr, "TURNIP_SMOKE phase=instance-version result=fail\n");
        dlclose(library);
        return 6;
    }

    PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions =
        (PFN_vkEnumerateInstanceExtensionProperties)get_instance_proc_addr(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)get_instance_proc_addr(VK_NULL_HANDLE, "vkCreateInstance");
    if (enumerate_instance_extensions == NULL || create_instance == NULL) {
        fprintf(stderr, "TURNIP_SMOKE phase=global-functions result=fail\n");
        dlclose(library);
        return 7;
    }

    uint32_t extension_count = 0;
    VkResult result = enumerate_instance_extensions(NULL, &extension_count, NULL);
    VkExtensionProperties* extensions = NULL;
    if (result == VK_SUCCESS && extension_count != 0) {
        extensions = calloc(extension_count, sizeof(*extensions));
        if (extensions == NULL) {
            dlclose(library);
            return 8;
        }
        result = enumerate_instance_extensions(NULL, &extension_count, extensions);
    }
    const bool has_surface = result == VK_SUCCESS &&
                             HasExtension(extensions, extension_count,
                                          VK_KHR_SURFACE_EXTENSION_NAME);
    const bool has_android_surface = result == VK_SUCCESS &&
                                     HasExtension(extensions, extension_count,
                                                  VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    for (uint32_t index = 0; result == VK_SUCCESS && index < extension_count; ++index) {
        printf("TURNIP_SMOKE instanceExtension[%u]=%s version=%u\n", index,
               extensions[index].extensionName, extensions[index].specVersion);
    }
    if (!has_surface || !has_android_surface) {
        fprintf(stderr,
                "TURNIP_SMOKE phase=instance-extensions result=fail surface=%d "
                "androidSurface=%d count=%u\n",
                has_surface, has_android_surface, extension_count);
        free(extensions);
        dlclose(library);
        return 9;
    }

    const char* enabled_extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
    };
    const VkApplicationInfo application_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lsx4-turnip-icd-smoke",
        .applicationVersion = 1,
        .pEngineName = "lsx4-smoke",
        .engineVersion = 1,
        .apiVersion = api_version,
    };
    const VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application_info,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = enabled_extensions,
    };
    VkInstance instance = VK_NULL_HANDLE;
    result = create_instance(&instance_info, NULL, &instance);
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        fprintf(stderr, "TURNIP_SMOKE phase=create-instance result=fail vkResult=%d\n",
                result);
        free(extensions);
        dlclose(library);
        return 10;
    }

    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices =
        (PFN_vkEnumeratePhysicalDevices)get_instance_proc_addr(
            instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties =
        (PFN_vkGetPhysicalDeviceProperties)get_instance_proc_addr(
            instance, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance destroy_instance =
        (PFN_vkDestroyInstance)get_instance_proc_addr(instance, "vkDestroyInstance");
    uint32_t physical_device_count = 0;
    result = enumerate_physical_devices != NULL
                 ? enumerate_physical_devices(instance, &physical_device_count, NULL)
                 : VK_ERROR_INITIALIZATION_FAILED;
    if (result != VK_SUCCESS || physical_device_count == 0 ||
        get_physical_device_properties == NULL || destroy_instance == NULL) {
        fprintf(stderr,
                "TURNIP_SMOKE phase=physical-devices result=fail vkResult=%d count=%u\n",
                result, physical_device_count);
        if (destroy_instance != NULL) {
            destroy_instance(instance, NULL);
        }
        free(extensions);
        dlclose(library);
        return 11;
    }

    VkPhysicalDevice* physical_devices =
        calloc(physical_device_count, sizeof(*physical_devices));
    if (physical_devices == NULL) {
        destroy_instance(instance, NULL);
        free(extensions);
        dlclose(library);
        return 12;
    }
    result = enumerate_physical_devices(instance, &physical_device_count, physical_devices);
    if (result != VK_SUCCESS || physical_device_count == 0) {
        fprintf(stderr,
                "TURNIP_SMOKE phase=physical-device-list result=fail vkResult=%d count=%u\n",
                result, physical_device_count);
        free(physical_devices);
        destroy_instance(instance, NULL);
        free(extensions);
        dlclose(library);
        return 13;
    }

    VkPhysicalDeviceProperties properties = {0};
    get_physical_device_properties(physical_devices[0], &properties);
    printf("TURNIP_SMOKE result=pass entrypoint=%s interface=%u api=%u.%u.%u "
           "instanceExtensions=%u physicalDevices=%u deviceName=%s "
           "driverVersion=%u vendorId=0x%04x deviceId=0x%04x\n",
           entrypoint, interface_version, VK_VERSION_MAJOR(api_version),
           VK_VERSION_MINOR(api_version), VK_VERSION_PATCH(api_version), extension_count,
           physical_device_count, properties.deviceName, properties.driverVersion,
           properties.vendorID, properties.deviceID);

    free(physical_devices);
    destroy_instance(instance, NULL);
    free(extensions);
    dlclose(library);
    return 0;
}
