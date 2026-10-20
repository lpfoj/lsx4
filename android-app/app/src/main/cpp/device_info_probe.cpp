

#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <vulkan/vulkan.h>
#include <sys/system_properties.h>
#include <string>
#include <vector>
#include <utility>

using Facts = std::vector<std::pair<std::string, std::string>>;

static std::string version(uint32_t v) {
    return std::to_string(VK_VERSION_MAJOR(v)) + "." +
           std::to_string(VK_VERSION_MINOR(v)) + "." + std::to_string(VK_VERSION_PATCH(v));
}

static void append(std::string& list, const char* entry) {
    if (!list.empty()) list += '\n';
    list += entry;
}

static void glFacts(Facts& out) {
    struct EglScope {
        EGLDisplay display = EGL_NO_DISPLAY;
        EGLSurface surface = EGL_NO_SURFACE;
        EGLContext context = EGL_NO_CONTEXT;
        bool initialized = false;
        ~EglScope() {
            if (!initialized) return;
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
            eglTerminate(display);
            eglReleaseThread();
        }
    } egl;
    egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl.display == EGL_NO_DISPLAY || !eglInitialize(egl.display, nullptr, nullptr)) return;
    egl.initialized = true;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return;
    EGLConfig config;
    EGLint count = 0;
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    if (!eglChooseConfig(egl.display, attributes, &config, 1, &count) || count < 1) return;
    const EGLint size[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    egl.surface = eglCreatePbufferSurface(egl.display, config, size);
    const EGLint context3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    egl.context = eglCreateContext(egl.display, config, EGL_NO_CONTEXT, context3);
    bool es3 = egl.context != EGL_NO_CONTEXT;
    if (!es3) {
        const EGLint context2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        egl.context = eglCreateContext(egl.display, config, EGL_NO_CONTEXT, context2);
    }
    if (egl.surface == EGL_NO_SURFACE || egl.context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context)) return;
    for (auto entry : {std::pair<const char*, GLenum>{"gl.renderer", GL_RENDERER},
                       {"gl.vendor", GL_VENDOR}, {"gl.version", GL_VERSION}}) {
        const auto* value = glGetString(entry.second);
        if (value) out.emplace_back(entry.first, reinterpret_cast<const char*>(value));
    }
    std::string extensions;
    if (es3) {
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n && i < 8192; ++i) {
            const auto* ext = glGetStringi(GL_EXTENSIONS, i);
            if (ext) append(extensions, reinterpret_cast<const char*>(ext));
        }
    } else {
        const auto* ext = glGetString(GL_EXTENSIONS);
        if (ext) {
            extensions = reinterpret_cast<const char*>(ext);
            for (auto& c : extensions) if (c == ' ') c = '\n';
        }
    }
    out.emplace_back("gl.extensions", extensions);
}

static void vkFacts(Facts& out) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "LSX4 device information";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    struct InstanceScope {
        VkInstance value = VK_NULL_HANDLE;
        ~InstanceScope() { if (value) vkDestroyInstance(value, nullptr); }
    } instance;
    if (vkCreateInstance(&create, nullptr, &instance.value) != VK_SUCCESS) return;
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(instance.value, &count, nullptr) != VK_SUCCESS ||
        count == 0 || count > 64) return;
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance.value, &count, devices.data()) != VK_SUCCESS) return;

    auto device = devices.front();
    VkPhysicalDeviceProperties p{};
    VkPhysicalDeviceFeatures f{};
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceProperties(device, &p);
    vkGetPhysicalDeviceFeatures(device, &f);
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    out.emplace_back("vk.available", "1");
    out.emplace_back("vk.device", p.deviceName);
    out.emplace_back("vk.api", version(p.apiVersion));

    out.emplace_back("vk.driver", std::to_string(p.driverVersion));
    const char* types[] = {"Other", "Integrated GPU", "Discrete GPU", "Virtual GPU", "CPU"};
    out.emplace_back("vk.type", p.deviceType <= VK_PHYSICAL_DEVICE_TYPE_CPU ? types[p.deviceType] : "Other");
    out.emplace_back("vk.push", std::to_string(p.limits.maxPushConstantsSize));
    out.emplace_back("vk.allocations", std::to_string(p.limits.maxMemoryAllocationCount));
    out.emplace_back("vk.descriptors", std::to_string(p.limits.maxBoundDescriptorSets));
    out.emplace_back("vk.compute", std::to_string(p.limits.maxComputeWorkGroupSize[0]) + " × " +
        std::to_string(p.limits.maxComputeWorkGroupSize[1]) + " × " +
        std::to_string(p.limits.maxComputeWorkGroupSize[2]));
    uint64_t deviceBytes = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            deviceBytes += memory.memoryHeaps[i].size;
    out.emplace_back("vk.memory", std::to_string(deviceBytes / (1024 * 1024)) + " MiB");
    std::string supported, unsupported;
#define FEATURE(name) append(f.name ? supported : unsupported, #name)
    FEATURE(robustBufferAccess); FEATURE(fullDrawIndexUint32); FEATURE(imageCubeArray);
    FEATURE(independentBlend); FEATURE(geometryShader); FEATURE(tessellationShader);
    FEATURE(sampleRateShading); FEATURE(dualSrcBlend); FEATURE(logicOp); FEATURE(multiDrawIndirect);
    FEATURE(drawIndirectFirstInstance); FEATURE(depthClamp); FEATURE(depthBiasClamp);
    FEATURE(fillModeNonSolid); FEATURE(depthBounds); FEATURE(wideLines); FEATURE(largePoints);
    FEATURE(alphaToOne); FEATURE(multiViewport); FEATURE(samplerAnisotropy);
    FEATURE(textureCompressionETC2); FEATURE(textureCompressionASTC_LDR); FEATURE(textureCompressionBC);
    FEATURE(occlusionQueryPrecise); FEATURE(pipelineStatisticsQuery); FEATURE(vertexPipelineStoresAndAtomics);
    FEATURE(fragmentStoresAndAtomics); FEATURE(shaderTessellationAndGeometryPointSize);
    FEATURE(shaderImageGatherExtended); FEATURE(shaderStorageImageExtendedFormats);
    FEATURE(shaderStorageImageMultisample); FEATURE(shaderStorageImageReadWithoutFormat);
    FEATURE(shaderStorageImageWriteWithoutFormat); FEATURE(shaderUniformBufferArrayDynamicIndexing);
    FEATURE(shaderSampledImageArrayDynamicIndexing); FEATURE(shaderStorageBufferArrayDynamicIndexing);
    FEATURE(shaderStorageImageArrayDynamicIndexing); FEATURE(shaderClipDistance); FEATURE(shaderCullDistance);
    FEATURE(shaderFloat64); FEATURE(shaderInt64); FEATURE(shaderInt16); FEATURE(shaderResourceResidency);
    FEATURE(shaderResourceMinLod); FEATURE(sparseBinding); FEATURE(sparseResidencyBuffer);
    FEATURE(sparseResidencyImage2D); FEATURE(sparseResidencyImage3D); FEATURE(sparseResidency2Samples);
    FEATURE(sparseResidency4Samples); FEATURE(sparseResidency8Samples); FEATURE(sparseResidency16Samples);
    FEATURE(sparseResidencyAliased); FEATURE(variableMultisampleRate); FEATURE(inheritedQueries);
#undef FEATURE
    out.emplace_back("vk.supported", supported);
    out.emplace_back("vk.unsupported", unsupported);
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &n, nullptr) != VK_SUCCESS || n > 8192) return;
    std::vector<VkExtensionProperties> extensions(n);
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &n, extensions.data()) != VK_SUCCESS) return;
    std::string list, missing;
    for (uint32_t i = 0; i < n; ++i) append(list, extensions[i].extensionName);

    for (const char* wanted : {
"VK_KHR_swapchain",
        "VK_KHR_maintenance1",
        "VK_KHR_maintenance2",
        "VK_KHR_maintenance3",
        "VK_KHR_maintenance4",
        "VK_KHR_shader_draw_parameters",
        "VK_KHR_push_descriptor",
        "VK_KHR_descriptor_update_template",
        "VK_KHR_create_renderpass2",
        "VK_KHR_depth_stencil_resolve",
        "VK_KHR_draw_indirect_count",
        "VK_KHR_driver_properties",
        "VK_KHR_image_format_list",
        "VK_KHR_imageless_framebuffer",
        "VK_KHR_sampler_mirror_clamp_to_edge",
        "VK_KHR_timeline_semaphore",
        "VK_KHR_uniform_buffer_standard_layout",
        "VK_KHR_shader_float_controls",
        "VK_KHR_shader_float16_int8",
        "VK_KHR_16bit_storage",
        "VK_KHR_8bit_storage",
        "VK_KHR_storage_buffer_storage_class",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
        "VK_KHR_bind_memory2",
        "VK_KHR_external_memory",
        "VK_KHR_external_semaphore",
        "VK_KHR_external_fence",
        "VK_KHR_spirv_1_4",
        "VK_KHR_shader_subgroup_extended_types",
        "VK_KHR_vulkan_memory_model",
        "VK_KHR_buffer_device_address",
        "VK_KHR_separate_depth_stencil_layouts",
        "VK_KHR_dynamic_rendering",
        "VK_KHR_synchronization2",
        "VK_KHR_copy_commands2",
        "VK_EXT_robustness2",
        "VK_EXT_extended_dynamic_state",
        "VK_EXT_extended_dynamic_state2",
        "VK_EXT_vertex_attribute_divisor",
        "VK_EXT_transform_feedback",
        "VK_EXT_conditional_rendering",
        "VK_EXT_depth_clip_enable",
        "VK_EXT_custom_border_color",
        "VK_EXT_shader_demote_to_helper_invocation",
        "VK_EXT_4444_formats",
        "VK_EXT_shader_stencil_export",
        "VK_EXT_sampler_filter_minmax",
        "VK_EXT_scalar_block_layout",
        "VK_EXT_host_query_reset",
        "VK_EXT_subgroup_size_control",
        "VK_EXT_shader_viewport_index_layer",
        "VK_EXT_memory_budget",
        "VK_EXT_memory_priority",
        "VK_EXT_descriptor_indexing",
        "VK_EXT_primitive_topology_list_restart",
        "VK_EXT_provoking_vertex",
        "VK_EXT_line_rasterization"
    }) {
        bool found = false;
        for (uint32_t i = 0; i < n; ++i) if (std::string(extensions[i].extensionName) == wanted) found = true;
        if (!found) append(missing, wanted);
    }
    out.emplace_back("vk.extensions", list);
    out.emplace_back("vk.missing", missing);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_app_lsx4_android_DeviceGraphicsInfo_query(JNIEnv* env, jclass) {
    Facts facts;
    for (auto property : {std::pair<const char*, const char*>{"boot.locked", "ro.boot.flash.locked"},
                          {"boot.verified", "ro.boot.verifiedbootstate"}}) {
        char value[PROP_VALUE_MAX]{};
        if (__system_property_get(property.second, value) > 0)
            facts.emplace_back(property.first, value);
    }
    try {
        glFacts(facts);
        vkFacts(facts);
    } catch (const std::exception&) {

    }
    auto stringClass = env->FindClass("java/lang/String");
    if (!stringClass) return nullptr;
    auto result = env->NewObjectArray(static_cast<jsize>(facts.size() * 2), stringClass, nullptr);
    env->DeleteLocalRef(stringClass);
    if (!result) return nullptr;
    jsize i = 0;
    for (const auto& entry : facts) {
        for (const auto* text : {&entry.first, &entry.second}) {
            auto value = env->NewStringUTF(text->c_str());
            if (!value) return nullptr;
            env->SetObjectArrayElement(result, i++, value);
            env->DeleteLocalRef(value);
        }
    }
    return result;
}
