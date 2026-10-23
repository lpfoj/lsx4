// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleVulkanGraphics.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <vulkan/vulkan.h>
namespace Lsx4::Apple {
namespace {
void Check(VkResult r, const char *operation) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + ": " + std::to_string(r));
}
struct Buffer {
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  void *ptr{};
};
struct Texture {
  VkImage image{};
  VkDeviceMemory memory{};
  VkImageView view{};
};
struct Context {
  VkInstance instance{};
  VkDevice device{};
  VkDescriptorSetLayout dsl{};
  VkPipelineLayout layout{};
  VkDescriptorPool dp{};
  VkImage image{};
  VkDeviceMemory im{};
  VkImageView view{};
  VkRenderPass rp{};
  VkFramebuffer fb{};
  VkPipeline pipeline{};
  VkCommandPool cp{};
  VkQueryPool queries{};
  VkFence fence{};
  std::vector<Buffer> buffers;
  std::vector<VkShaderModule> modules;
  std::vector<Texture> textures;
  std::vector<VkSampler> samplers;
  ~Context() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (fence)
        vkDestroyFence(device, fence, nullptr);
      if (queries)
        vkDestroyQueryPool(device, queries, nullptr);
      if (cp)
        vkDestroyCommandPool(device, cp, nullptr);
      if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
      for (auto shader : modules)
        vkDestroyShaderModule(device, shader, nullptr);
      if (fb)
        vkDestroyFramebuffer(device, fb, nullptr);
      if (rp)
        vkDestroyRenderPass(device, rp, nullptr);
      if (view)
        vkDestroyImageView(device, view, nullptr);
      if (image)
        vkDestroyImage(device, image, nullptr);
      if (im)
        vkFreeMemory(device, im, nullptr);
      for (auto &texture : textures) {
        if (texture.view)
          vkDestroyImageView(device, texture.view, nullptr);
        if (texture.image)
          vkDestroyImage(device, texture.image, nullptr);
        if (texture.memory)
          vkFreeMemory(device, texture.memory, nullptr);
      }
      for (auto sampler : samplers)
        vkDestroySampler(device, sampler, nullptr);
      if (dp)
        vkDestroyDescriptorPool(device, dp, nullptr);
      if (layout)
        vkDestroyPipelineLayout(device, layout, nullptr);
      if (dsl)
        vkDestroyDescriptorSetLayout(device, dsl, nullptr);
      for (auto &b : buffers) {
        if (b.ptr)
          vkUnmapMemory(device, b.memory);
        if (b.buffer)
          vkDestroyBuffer(device, b.buffer, nullptr);
        if (b.memory)
          vkFreeMemory(device, b.memory, nullptr);
      }
      vkDestroyDevice(device, nullptr);
    }
    if (instance)
      vkDestroyInstance(instance, nullptr);
  }
};
} // namespace
static DepthClearDrawResult
RenderGraphics(const CompiledGraphicsShader &shaders,
               const DepthClearDrawState &state,
               const std::vector<std::vector<std::uint8_t>> &inputs,
               const ColorDrawState *colorState,
               const std::vector<std::vector<std::uint8_t>> &textureInputs,
               std::span<const std::uint8_t> initialColor,
               std::span<const std::uint8_t> indices) {
  const bool colorTarget = colorState != nullptr;
  if (shaders.bufferBindings.size() != shaders.buffers.size() ||
      textureInputs.size() != shaders.images.size() ||
      (!colorTarget &&
       (!shaders.images.empty() || !shaders.samplers.empty())) ||
      (colorTarget &&
       (colorState->writeMask > 15 || !colorState->writeMask ||
        initialColor.size() != std::uint64_t(state.width) * state.height * 4 ||
        indices.size() != std::uint64_t(state.vertexCount) * 2 ||
        !shaders.tessControl.empty())))
    throw std::runtime_error("Invalid graphics resource layout");
  constexpr std::uint64_t Budget = 128ull * 1024 * 1024;
  const bool tessellated = !shaders.tessControl.empty();
  if (!state.width || !state.height || state.width > 16384 ||
      state.height > 16384 ||
      std::uint64_t(state.width) * state.height * 4 > Budget ||
      !state.vertexCount || state.vertexCount > 1048576 ||
      !state.instanceCount || state.instanceCount > 65536 ||
      state.cullMode > 3 || state.depthCompare > 7 ||
      !std::isfinite(state.clearDepth) || state.clearDepth < 0 ||
      state.clearDepth > 1 || shaders.buffers.size() != inputs.size() ||
      inputs.size() > 40 || shaders.vertex.size() < 5 ||
      shaders.fragment.size() < 5 || shaders.vertex[0] != 0x07230203 ||
      shaders.fragment[0] != 0x07230203 ||
      tessellated != !shaders.tessEvaluation.empty())
    throw std::runtime_error("Invalid depth draw inputs");
  if (tessellated &&
      (shaders.tessControl.size() < 5 || shaders.tessEvaluation.size() < 5 ||
       shaders.tessControl[0] != 0x07230203 ||
       shaders.tessEvaluation[0] != 0x07230203))
    throw std::runtime_error("Invalid tessellation shaders");
  for (auto value : state.viewport)
    if (!std::isfinite(value))
      throw std::runtime_error("Nonfinite viewport");
  if (state.viewport[2] <= 0 || state.viewport[2] > 16384 ||
      state.viewport[3] <= 0 || state.viewport[3] > 16384 ||
      state.viewport[4] < 0 || state.viewport[5] > 1 ||
      state.viewport[4] > state.viewport[5] || state.scissor[0] > state.width ||
      state.scissor[1] > state.height ||
      state.scissor[2] > state.width - state.scissor[0] ||
      state.scissor[3] > state.height - state.scissor[1])
    throw std::runtime_error("Invalid viewport/scissor");
  std::uint64_t total = std::uint64_t(state.width) * state.height * 4;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    const auto &binding = shaders.buffers[i];
    if (binding.written || !binding.size || binding.size > Budget ||
        inputs[i].size() != binding.size || total > Budget - binding.size ||
        (!binding.inlineBytes.empty() && binding.inlineBytes != inputs[i]))
      throw std::runtime_error("Invalid readonly graphics buffer");
    total += binding.size;
  }
  for (unsigned i = 0; i < textureInputs.size(); ++i) {
    const auto &texture = shaders.images[i];
    if (!texture.width || !texture.height || texture.width > texture.pitch ||
        texture.pitch > 16384 || texture.height > 16384 ||
        texture.size != textureInputs[i].size() || texture.size > Budget ||
        total > Budget - texture.size)
      throw std::runtime_error("Invalid graphics texture bytes");
    total += texture.size;
  }
  if (colorTarget && (total > Budget - initialColor.size() ||
                      total + initialColor.size() > Budget - indices.size()))
    throw std::runtime_error("Color staging budget exceeded");
  std::set<std::uint32_t> occupied;
  auto binding = [&](std::uint32_t id) {
    if (!occupied.insert(id).second)
      throw std::runtime_error("Duplicate graphics binding");
  };
  for (auto id : shaders.bufferBindings)
    binding(id);
  for (auto image : shaders.images)
    binding(image.binding);
  for (auto sampler : shaders.samplers)
    binding(sampler.binding);
  Context context;
  context.buffers.reserve(inputs.size() + textureInputs.size() + 3);
  context.modules.reserve(4);
  context.textures.reserve(textureInputs.size());
  context.samplers.reserve(shaders.samplers.size());
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName = "LSX4 portable graphics";
  ai.apiVersion = VK_API_VERSION_1_0;
  unsigned extensionCount = 0;
  Check(
      vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr),
      "instance extensions count");
  std::vector<VkExtensionProperties> extensions(extensionCount);
  Check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
                                               extensions.data()),
        "instance extensions");
  const bool portability = std::ranges::any_of(extensions, [](const auto &e) {
    return !strcmp(e.extensionName,
                   VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
  });
  const char *ix[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
  VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ii.pApplicationInfo = &ai;
  ii.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
  ii.enabledExtensionCount = portability ? 1 : 0;
  ii.ppEnabledExtensionNames = ix;
  auto &instance = context.instance;
  Check(vkCreateInstance(&ii, nullptr, &instance), "instance");
  unsigned n = 0;
  Check(vkEnumeratePhysicalDevices(instance, &n, nullptr), "physical count");
  if (!n)
    throw std::runtime_error("No GPU");
  std::vector<VkPhysicalDevice> physicals(n);
  Check(vkEnumeratePhysicalDevices(instance, &n, physicals.data()), "physical");
  auto physical = physicals[0];
  VkPhysicalDeviceFeatures features;
  vkGetPhysicalDeviceFeatures(physical, &features);
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(physical, &properties);

  if (tessellated && !features.tessellationShader)
    throw std::runtime_error("RectList tessellation unavailable");
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
  std::vector<VkQueueFamilyProperties> families(n);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
  unsigned family = 0;
  while (family < n && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    ++family;
  if (family == n)
    throw std::runtime_error("No graphics queue");
  float priority = 1;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = family;
  qi.queueCount = 1;
  qi.pQueuePriorities = &priority;
  Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount,
                                             nullptr),
        "device extension count");
  extensions.resize(extensionCount);
  Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount,
                                             extensions.data()),
        "device extensions");
  const bool subset = std::ranges::any_of(extensions, [](const auto &e) {
    return !strcmp(e.extensionName, "VK_KHR_portability_subset");
  });
  const char *dx[] = {"VK_KHR_portability_subset"};
  VkPhysicalDeviceFeatures enabled{};
  enabled.tessellationShader = tessellated;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.enabledExtensionCount = subset ? 1 : 0;
  di.ppEnabledExtensionNames = dx;
  di.pEnabledFeatures = &enabled;
  auto &device = context.device;
  Check(vkCreateDevice(physical, &di, nullptr, &device), "device");
  VkQueue queue;
  vkGetDeviceQueue(device, family, 0, &queue);
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(physical, &mp);
  auto memory = [&](VkMemoryRequirements r, VkMemoryPropertyFlags flags) {
    unsigned t = 0;
    while (t < mp.memoryTypeCount &&
           (!(r.memoryTypeBits & (1u << t)) ||
            (mp.memoryTypes[t].propertyFlags & flags) != flags))
      ++t;
    if (t == mp.memoryTypeCount)
      throw std::runtime_error("No suitable memory");
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.allocationSize = r.size;
    a.memoryTypeIndex = t;
    VkDeviceMemory m;
    Check(vkAllocateMemory(device, &a, nullptr, &m), "allocate");
    return m;
  };
  auto &buffers = context.buffers;
  auto addBuffer = [&](size_t size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b.size = size;
    b.usage = usage;
    buffers.emplace_back();
    auto &out = buffers.back();
    Check(vkCreateBuffer(device, &b, nullptr, &out.buffer), "buffer");
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(device, out.buffer, &r);
    out.memory = memory(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    Check(vkBindBufferMemory(device, out.buffer, out.memory, 0), "bind buffer");
    Check(vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.ptr),
          "map");
    return out;
  };
  if (properties.limits.maxViewportDimensions[0] < 16384 ||
      properties.limits.maxViewportDimensions[1] < 16384 ||
      state.width > properties.limits.maxFramebufferWidth ||
      state.height > properties.limits.maxFramebufferHeight ||
      shaders.buffers.size() >
          properties.limits.maxPerStageDescriptorStorageBuffers ||
      shaders.buffers.size() >
          properties.limits.maxDescriptorSetStorageBuffers ||
      shaders.images.size() >
          properties.limits.maxPerStageDescriptorSampledImages ||
      shaders.images.size() > properties.limits.maxDescriptorSetSampledImages ||
      shaders.samplers.size() >
          properties.limits.maxPerStageDescriptorSamplers ||
      shaders.samplers.size() > properties.limits.maxDescriptorSetSamplers ||
      occupied.size() > properties.limits.maxPerStageResources ||
      properties.limits.maxPushConstantsSize < 128)
    throw std::runtime_error("Graphics device limits unavailable");
  std::vector<VkDescriptorSetLayoutBinding> lbs;
  std::vector<VkDescriptorBufferInfo> bis;
  for (unsigned i = 0; i < shaders.buffers.size(); ++i) {
    auto &b = shaders.buffers[i];
    const auto &bytes = inputs[i];
    if (b.size > properties.limits.maxStorageBufferRange ||
        bytes.size() != b.size)
      throw std::runtime_error("Buffer length mismatch");
    auto out = addBuffer(b.size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(out.ptr, bytes.data(), bytes.size());
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = out.memory;
    range.size = VK_WHOLE_SIZE;
    Check(vkFlushMappedMemoryRanges(device, 1, &range), "flush");
    lbs.push_back({shaders.bufferBindings[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr});
    bis.push_back({out.buffer, 0, b.size});
  }

  struct Upload {
    VkBuffer buffer;
    VkImage image;
    unsigned width, height;
    VkImageAspectFlags aspect;
  };
  std::vector<Upload> uploads;
  auto stage = [&](std::span<const std::uint8_t> bytes, VkImage image,
                   unsigned width, unsigned height, VkImageAspectFlags aspect) {
    auto b = addBuffer(bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(b.ptr, bytes.data(), bytes.size());
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = b.memory;
    range.size = VK_WHOLE_SIZE;
    Check(vkFlushMappedMemoryRanges(device, 1, &range), "flush image staging");
    uploads.push_back({b.buffer, image, width, height, aspect});
  };
  std::vector<VkDescriptorImageInfo> textureInfos, samplerInfos;
  textureInfos.reserve(shaders.images.size());
  samplerInfos.reserve(shaders.samplers.size());
  auto swizzle = [](unsigned value) -> VkComponentSwizzle {
    switch (value) {
    case 0:
      return VK_COMPONENT_SWIZZLE_ZERO;
    case 1:
      return VK_COMPONENT_SWIZZLE_ONE;
    case 4:
      return VK_COMPONENT_SWIZZLE_R;
    case 5:
      return VK_COMPONENT_SWIZZLE_G;
    case 6:
      return VK_COMPONENT_SWIZZLE_B;
    case 7:
      return VK_COMPONENT_SWIZZLE_A;
    default:
      throw std::runtime_error("Unsupported texture swizzle");
    }
  };
  for (unsigned i = 0; i < shaders.images.size(); ++i) {
    const auto &source = shaders.images[i];
    const auto format = source.format == SampledPixelFormat::Rgb565Unorm
                            ? VK_FORMAT_R5G6B5_UNORM_PACK16
                            : VK_FORMAT_R8G8B8A8_UNORM;
    const unsigned pixelBytes =
        source.format == SampledPixelFormat::Rgb565Unorm ? 2 : 4;
    if (source.size != std::uint64_t(source.pitch) * source.height * pixelBytes)
      throw std::runtime_error("Texture pitch/size mismatch");
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    const auto needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((properties.optimalTilingFeatures & needed) != needed)
      throw std::runtime_error("Texture sampling format unavailable");
    context.textures.emplace_back();
    auto &texture = context.textures.back();
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    create.imageType = VK_IMAGE_TYPE_2D;
    create.format = format;
    create.extent = {source.width, source.height, 1};
    create.mipLevels = 1;
    create.arrayLayers = 1;
    create.samples = VK_SAMPLE_COUNT_1_BIT;
    create.tiling = VK_IMAGE_TILING_OPTIMAL;
    create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Check(vkCreateImage(device, &create, nullptr, &texture.image),
          "sampled image");
    VkMemoryRequirements requirement;
    vkGetImageMemoryRequirements(device, texture.image, &requirement);
    texture.memory = memory(requirement, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkBindImageMemory(device, texture.image, texture.memory, 0),
          "bind sampled image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = texture.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.components = {swizzle(source.swizzle[0]), swizzle(source.swizzle[1]),
                       swizzle(source.swizzle[2]), swizzle(source.swizzle[3])};
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    Check(vkCreateImageView(device, &view, nullptr, &texture.view),
          "sampled view");
    std::vector<std::uint8_t> packed(std::size_t(source.width) * source.height *
                                     pixelBytes);
    for (unsigned y = 0; y < source.height; ++y)
      memcpy(packed.data() + std::size_t(y) * source.width * pixelBytes,
             textureInputs[i].data() +
                 std::size_t(y) * source.pitch * pixelBytes,
             source.width * pixelBytes);
    stage(packed, texture.image, source.width, source.height,
          VK_IMAGE_ASPECT_COLOR_BIT);
    textureInfos.push_back({VK_NULL_HANDLE, texture.view,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    lbs.push_back({source.binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
  }
  for (const auto &source : shaders.samplers) {
    auto clamp = [](unsigned value) -> VkSamplerAddressMode {
      switch (value) {
      case 0:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      case 1:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case 2:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      case 6:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      default:
        throw std::runtime_error("Unsupported sampler clamp");
      }
    };
    if (source.magFilter > 1 || source.minFilter > 1 || source.mipFilter > 2 ||
        source.border > 2 ||
        std::abs(source.lodBias) > properties.limits.maxSamplerLodBias)
      throw std::runtime_error("Invalid sampler state");
    VkSamplerCreateInfo create{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    create.magFilter = VkFilter(source.magFilter);
    create.minFilter = VkFilter(source.minFilter);
    create.mipmapMode = source.mipFilter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                              : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    create.addressModeU = clamp(source.clamp[0]);
    create.addressModeV = clamp(source.clamp[1]);
    create.addressModeW = clamp(source.clamp[2]);
    create.mipLodBias = source.lodBias;
    create.minLod = source.minLod;
    create.maxLod = source.mipFilter ? source.maxLod : 0;
    create.borderColor = VkBorderColor(source.border * 2);
    context.samplers.emplace_back();
    Check(vkCreateSampler(device, &create, nullptr, &context.samplers.back()),
          "sampler");
    samplerInfos.push_back(
        {context.samplers.back(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    lbs.push_back({source.binding, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo li{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = lbs.size();
  li.pBindings = lbs.data();
  auto &dsl = context.dsl;
  Check(vkCreateDescriptorSetLayout(device, &li, nullptr, &dsl),
        "descriptor layout");
  VkPushConstantRange push{
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &dsl;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &push;
  auto &layout = context.layout;
  Check(vkCreatePipelineLayout(device, &pli, nullptr, &layout),
        "pipeline layout");
  std::vector<VkDescriptorPoolSize> sizes;
  if (!inputs.empty())
    sizes.push_back(
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, unsigned(inputs.size())});
  if (!textureInfos.empty())
    sizes.push_back(
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, unsigned(textureInfos.size())});
  if (!samplerInfos.empty())
    sizes.push_back(
        {VK_DESCRIPTOR_TYPE_SAMPLER, unsigned(samplerInfos.size())});
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = sizes.size();
  dpi.pPoolSizes = sizes.data();
  auto &dp = context.dp;
  Check(vkCreateDescriptorPool(device, &dpi, nullptr, &dp), "descriptor pool");
  VkDescriptorSetAllocateInfo dai{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = dp;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &dsl;
  VkDescriptorSet ds;
  Check(vkAllocateDescriptorSets(device, &dai, &ds), "descriptor set");
  std::vector<VkWriteDescriptorSet> writes;
  for (unsigned i = 0; i < inputs.size(); ++i) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = ds;
    w.dstBinding = shaders.bufferBindings[i];
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bis[i];
    writes.push_back(w);
  }
  for (unsigned i = 0; i < textureInfos.size(); ++i) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = ds;
    w.dstBinding = shaders.images[i].binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &textureInfos[i];
    writes.push_back(w);
  }
  for (unsigned i = 0; i < samplerInfos.size(); ++i) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = ds;
    w.dstBinding = shaders.samplers[i].binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &samplerInfos[i];
    writes.push_back(w);
  }
  vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
  // This first command disables all color outputs and clears depth. D32
  // readback checks the attachment independently; an occlusion query checks
  // real raster work.
  const auto width = state.width, height = state.height;
  VkFormat format =
      colorTarget ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_D32_SFLOAT;
  const auto aspect =
      colorTarget ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
  if (!(fp.optimalTilingFeatures &
        (colorTarget ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
                     : VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)))
    throw std::runtime_error("D32 unsupported");
  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = format;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage =
      (colorTarget ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT
                   : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  auto &image = context.image;
  Check(vkCreateImage(device, &imageInfo, nullptr, &image), "depth image");
  VkMemoryRequirements ir;
  vkGetImageMemoryRequirements(device, image, &ir);
  auto &im = context.im;
  im = memory(ir, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  Check(vkBindImageMemory(device, image, im, 0), "bind depth");
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {aspect, 0, 1, 0, 1};
  auto &view = context.view;
  Check(vkCreateImageView(device, &vi, nullptr, &view), "depth view");
  VkAttachmentDescription ad{};
  ad.format = format;
  ad.samples = VK_SAMPLE_COUNT_1_BIT;
  ad.loadOp =
      colorTarget ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
  ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  ad.initialLayout = colorTarget ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                 : VK_IMAGE_LAYOUT_UNDEFINED;
  ad.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference ar{
      0, colorTarget ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                     : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sp{};
  sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sp.pDepthStencilAttachment = colorTarget ? nullptr : &ar;
  sp.colorAttachmentCount = colorTarget ? 1 : 0;
  sp.pColorAttachments = colorTarget ? &ar : nullptr;
  VkSubpassDependency dep{
      0,
      VK_SUBPASS_EXTERNAL,
      VkPipelineStageFlags(
          colorTarget ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                      : VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT),
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      colorTarget ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                  : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT,
      0};
  VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  ri.attachmentCount = 1;
  ri.pAttachments = &ad;
  ri.subpassCount = 1;
  ri.pSubpasses = &sp;
  ri.dependencyCount = 1;
  ri.pDependencies = &dep;
  auto &rp = context.rp;
  Check(vkCreateRenderPass(device, &ri, nullptr, &rp), "render pass");
  VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fi.renderPass = rp;
  fi.attachmentCount = 1;
  fi.pAttachments = &view;
  fi.width = width;
  fi.height = height;
  fi.layers = 1;
  auto &fb = context.fb;
  Check(vkCreateFramebuffer(device, &fi, nullptr, &fb), "framebuffer");
  auto &modules = context.modules;
  std::vector<VkPipelineShaderStageCreateInfo> stages;
  for (auto [flag, code] :
       {std::pair{VK_SHADER_STAGE_VERTEX_BIT, &shaders.vertex},
        std::pair{VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                  &shaders.tessControl},
        std::pair{VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                  &shaders.tessEvaluation},
        std::pair{VK_SHADER_STAGE_FRAGMENT_BIT, &shaders.fragment}}) {
    if (code->empty())
      continue;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = code->size() * 4;
    mi.pCode = code->data();
    VkShaderModule module;
    Check(vkCreateShaderModule(device, &mi, nullptr, &module), "shader module");
    modules.push_back(module);
    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = flag;
    stage.module = module;
    stage.pName = "main";
    stages.push_back(stage);
  }
  VkPipelineVertexInputStateCreateInfo vertex{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = tessellated ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                            : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineTessellationStateCreateInfo tess{
      VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
  tess.patchControlPoints = 3;
  VkViewport vp{state.viewport[0], state.viewport[1], state.viewport[2],
                state.viewport[3], state.viewport[4], state.viewport[5]};
  VkRect2D scissor{
      {std::int32_t(state.scissor[0]), std::int32_t(state.scissor[1])},
      {state.scissor[2], state.scissor[3]}};
  VkPipelineViewportStateCreateInfo vpi{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vpi.viewportCount = 1;
  vpi.pViewports = &vp;
  vpi.scissorCount = 1;
  vpi.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = state.cullMode;
  raster.frontFace = state.clockwise ? VK_FRONT_FACE_CLOCKWISE
                                     : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = colorTarget ? 0 : 1;
  depth.depthWriteEnable = 0;
  depth.depthCompareOp = VkCompareOp(state.depthCompare);
  VkPipelineColorBlendAttachmentState colorAttachment{};
  colorAttachment.colorWriteMask = colorTarget ? colorState->writeMask : 0;
  VkPipelineColorBlendStateCreateInfo color{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color.attachmentCount = colorTarget ? 1 : 0;
  color.pAttachments = colorTarget ? &colorAttachment : nullptr;
  VkGraphicsPipelineCreateInfo gp{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = stages.size();
  gp.pStages = stages.data();
  gp.pVertexInputState = &vertex;
  gp.pInputAssemblyState = &ia;
  gp.pTessellationState = tessellated ? &tess : nullptr;
  gp.pViewportState = &vpi;
  gp.pRasterizationState = &raster;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &depth;
  gp.pColorBlendState = &color;
  gp.layout = layout;
  gp.renderPass = rp;
  auto &pipeline = context.pipeline;
  Check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr,
                                  &pipeline),
        "graphics pipeline");
  auto readback =
      addBuffer(width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  memset(readback.ptr, 0xcd, width * height * 4);
  Buffer indexBuffer;
  if (colorTarget) {
    stage(initialColor, image, width, height, aspect);
    indexBuffer = addBuffer(indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    memcpy(indexBuffer.ptr, indices.data(), indices.size());
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = indexBuffer.memory;
    range.size = VK_WHOLE_SIZE;
    Check(vkFlushMappedMemoryRanges(device, 1, &range), "flush indices");
  }
  VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  ci.queueFamilyIndex = family;
  auto &cp = context.cp;
  Check(vkCreateCommandPool(device, &ci, nullptr, &cp), "command pool");
  VkCommandBufferAllocateInfo ca{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = cp;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  VkCommandBuffer cmd;
  Check(vkAllocateCommandBuffers(device, &ca, &cmd), "command buffer");
  VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  queryInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
  queryInfo.queryCount = 1;
  auto &queries = context.queries;
  Check(vkCreateQueryPool(device, &queryInfo, nullptr, &queries), "query pool");
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  Check(vkBeginCommandBuffer(cmd, &bi), "begin");
  vkCmdResetQueryPool(cmd, queries, 0, 1);
  VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  host.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  host.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                       0, 1, &host, 0, nullptr, 0, nullptr);

  for (const auto &upload : uploads) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = upload.image;
    barrier.subresourceRange = {upload.aspect, 0, 1, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {upload.aspect, 0, 0, 1};
    copy.imageExtent = {upload.width, upload.height, 1};
    vkCmdCopyBufferToImage(cmd, upload.buffer, upload.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    const bool target = upload.image == image;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = target ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = target ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                   : VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         target ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
  }
  VkClearValue clear{};
  clear.depthStencil.depth = state.clearDepth;
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = rp;
  begin.framebuffer = fb;
  begin.renderArea = {{0, 0}, {width, height}};
  begin.clearValueCount = 1;
  begin.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                          &ds, 0, nullptr);
  vkCmdPushConstants(cmd, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, 128, shaders.pushConstants.data());
  vkCmdBeginQuery(cmd, queries, 0, 0);
  if (colorTarget) {
    vkCmdBindIndexBuffer(cmd, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, state.vertexCount, state.instanceCount, 0, 0, 0);
  } else
    vkCmdDraw(cmd, state.vertexCount, state.instanceCount, 0, 0);
  vkCmdEndQuery(cmd, queries, 0);
  vkCmdEndRenderPass(cmd);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {aspect, 0, 0, 1};
  copy.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         readback.buffer, 1, &copy);
  VkMemoryBarrier out{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  out.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  out.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &out, 0, nullptr, 0,
                       nullptr);
  Check(vkEndCommandBuffer(cmd), "end");
  VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  auto &fence = context.fence;
  Check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "fence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  Check(vkQueueSubmit(queue, 1, &submit, fence), "submit");
  Check(vkWaitForFences(device, 1, &fence, 1, 5000000000ull), "wait");
  std::uint64_t passed = 0;
  Check(vkGetQueryPoolResults(
            device, queries, 0, 1, sizeof(passed), &passed, sizeof(passed),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
        "raster query");
  VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
  range.memory = readback.memory;
  range.size = VK_WHOLE_SIZE;
  Check(vkInvalidateMappedMemoryRanges(device, 1, &range), "depth readback");
  DepthClearDrawResult result;
  result.depth.resize(std::size_t(width) * height * 4);
  std::memcpy(result.depth.data(), readback.ptr, result.depth.size());
  result.deviceName = properties.deviceName;
  result.occlusionVisible = passed != 0;
  return result;
}

DepthClearDrawResult
RenderDepthClearDraw(const CompiledGraphicsShader &shaders,
                     const DepthClearDrawState &state,
                     const std::vector<std::vector<std::uint8_t>> &inputs) {
  return RenderGraphics(shaders, state, inputs, nullptr, {}, {}, {});
}
ColorDrawResult
RenderColorDraw(const CompiledGraphicsShader &shaders,
                const ColorDrawState &state,
                const std::vector<std::vector<std::uint8_t>> &buffers,
                const std::vector<std::vector<std::uint8_t>> &textures,
                std::span<const std::uint8_t> initialColor,
                std::span<const std::uint8_t> indices) {
  auto result = RenderGraphics(shaders, state.raster, buffers, &state, textures,
                               initialColor, indices);
  return {std::move(result.depth), std::move(result.deviceName),
          result.occlusionVisible};
}
} // namespace Lsx4::Apple
