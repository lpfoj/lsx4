// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleVulkanProbe.h"
#include "AppleShaderProbe.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>
namespace Lsx4::Apple {
namespace {
void Check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
struct Resources {
    VkInstance instance{};VkDevice device{};VkImage image{};VkBuffer buffer{};
    VkDeviceMemory imageMemory{},bufferMemory{};VkCommandPool pool{};VkFence fence{};
    VkShaderModule shader{};VkPipelineLayout pipelineLayout{};VkPipeline pipeline{};
    void* mapped{};
    ~Resources() {
        if(device) {
            vkDeviceWaitIdle(device);
            if(mapped)vkUnmapMemory(device,bufferMemory);
            if(fence)vkDestroyFence(device,fence,nullptr);
            if(pool)vkDestroyCommandPool(device,pool,nullptr);
            if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
            if(pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
            if(shader)vkDestroyShaderModule(device,shader,nullptr);
            if(image)vkDestroyImage(device,image,nullptr);
            if(buffer)vkDestroyBuffer(device,buffer,nullptr);
            if(imageMemory)vkFreeMemory(device,imageMemory,nullptr);
            if(bufferMemory)vkFreeMemory(device,bufferMemory,nullptr);
            vkDestroyDevice(device,nullptr);
        }
        if(instance)vkDestroyInstance(instance,nullptr);
    }
};
bool Has(const std::vector<VkExtensionProperties>& extensions,const char* name) {
    return std::ranges::any_of(extensions,[&](const auto& value){return std::strcmp(value.extensionName,name)==0;});
}
}
VulkanProbeResult ProbeVulkanTransfer() {
    VulkanProbeResult result;Resources resources;
    try {
        std::uint32_t count=0;Check(vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr),"instance extension count");
        std::vector<VkExtensionProperties> extensions(count);
        Check(vkEnumerateInstanceExtensionProperties(nullptr,&count,extensions.data()),"instance extensions");
        std::vector<const char*> enabled;
        const bool portability=Has(extensions,VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        if(portability)enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName="LSX4 iOS GPU verification";app.apiVersion=VK_API_VERSION_1_0;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&app;
        create.enabledExtensionCount=enabled.size();create.ppEnabledExtensionNames=enabled.data();
        create.flags=portability?VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR:0;
        Check(vkCreateInstance(&create,nullptr,&resources.instance),"create instance");
        Check(vkEnumeratePhysicalDevices(resources.instance,&count,nullptr),"physical device count");
        if(!count)throw std::runtime_error("No Vulkan physical device");
        std::vector<VkPhysicalDevice> physicals(count);
        Check(vkEnumeratePhysicalDevices(resources.instance,&count,physicals.data()),"physical devices");
        const auto physical=physicals.front();VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical,&properties);
        result.device=properties.deviceName;result.apiVersion=properties.apiVersion;
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
        std::uint32_t family=0;
        constexpr auto queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;
        while(family<count && (families[family].queueFlags&queueFlags)!=queueFlags)++family;
        if(family==count)throw std::runtime_error("No graphics/compute queue");
        Check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr),"device extension count");
        extensions.resize(count);
        Check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,extensions.data()),"device extensions");
        enabled.clear();
        if(Has(extensions,"VK_KHR_portability_subset"))enabled.push_back("VK_KHR_portability_subset");
        const float priority=1;
        VkDeviceQueueCreateInfo queueCreate{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueCreate.queueFamilyIndex=family;queueCreate.queueCount=1;queueCreate.pQueuePriorities=&priority;
        VkDeviceCreateInfo deviceCreate{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceCreate.queueCreateInfoCount=1;deviceCreate.pQueueCreateInfos=&queueCreate;
        deviceCreate.enabledExtensionCount=enabled.size();deviceCreate.ppEnabledExtensionNames=enabled.data();
        Check(vkCreateDevice(physical,&deviceCreate,nullptr,&resources.device),"create device");
        VkQueue queue;vkGetDeviceQueue(resources.device,family,0,&queue);
        const auto spirv=CompileShaderProbe();result.shaderWords=spirv.size();
        VkShaderModuleCreateInfo shaderCreate{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderCreate.codeSize=spirv.size()*4;shaderCreate.pCode=spirv.data();
        Check(vkCreateShaderModule(resources.device,&shaderCreate,nullptr,&resources.shader),"create translated shader");
        VkPipelineLayoutCreateInfo layoutCreate{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,128};
        layoutCreate.pushConstantRangeCount=1;layoutCreate.pPushConstantRanges=&push;
        Check(vkCreatePipelineLayout(resources.device,&layoutCreate,nullptr,&resources.pipelineLayout),"create compute layout");
        VkComputePipelineCreateInfo pipelineCreate{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineCreate.layout=resources.pipelineLayout;
        pipelineCreate.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineCreate.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipelineCreate.stage.module=resources.shader;pipelineCreate.stage.pName="main";
        Check(vkCreateComputePipelines(resources.device,VK_NULL_HANDLE,1,&pipelineCreate,nullptr,&resources.pipeline),"compile translated shader pipeline");
        VkFormatProperties format;vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&format);
        // Vulkan 1.0 guarantees transfer support for color attachment formats.
        if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))throw std::runtime_error("RGBA8 unavailable");
        VkImageCreateInfo imageCreate{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageCreate.imageType=VK_IMAGE_TYPE_2D;imageCreate.format=VK_FORMAT_R8G8B8A8_UNORM;
        imageCreate.extent={4,4,1};imageCreate.mipLevels=1;imageCreate.arrayLayers=1;
        imageCreate.samples=VK_SAMPLE_COUNT_1_BIT;imageCreate.tiling=VK_IMAGE_TILING_OPTIMAL;
        imageCreate.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateImage(resources.device,&imageCreate,nullptr,&resources.image),"create image");
        VkPhysicalDeviceMemoryProperties memoryProperties;vkGetPhysicalDeviceMemoryProperties(physical,&memoryProperties);
        auto allocate=[&](const VkMemoryRequirements& requirements,VkMemoryPropertyFlags required,VkDeviceMemory& allocation) {
            std::uint32_t index=0;
            while(index<memoryProperties.memoryTypeCount &&
                  (!(requirements.memoryTypeBits&(1u<<index)) ||
                   (memoryProperties.memoryTypes[index].propertyFlags&required)!=required))++index;
            if(index==memoryProperties.memoryTypeCount)throw std::runtime_error("No suitable Vulkan memory type");
            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};info.allocationSize=requirements.size;info.memoryTypeIndex=index;
            Check(vkAllocateMemory(resources.device,&info,nullptr,&allocation),"allocate memory");
        };
        VkMemoryRequirements requirements;vkGetImageMemoryRequirements(resources.device,resources.image,&requirements);
        allocate(requirements,0,resources.imageMemory);
        Check(vkBindImageMemory(resources.device,resources.image,resources.imageMemory,0),"bind image");
        VkBufferCreateInfo bufferCreate{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferCreate.size=64;bufferCreate.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateBuffer(resources.device,&bufferCreate,nullptr,&resources.buffer),"create staging buffer");
        vkGetBufferMemoryRequirements(resources.device,resources.buffer,&requirements);
        allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,resources.bufferMemory);
        Check(vkBindBufferMemory(resources.device,resources.buffer,resources.bufferMemory,0),"bind staging buffer");
        VkCommandPoolCreateInfo poolCreate{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolCreate.queueFamilyIndex=family;
        Check(vkCreateCommandPool(resources.device,&poolCreate,nullptr,&resources.pool),"create command pool");
        VkCommandBufferAllocateInfo commandAllocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandAllocate.commandPool=resources.pool;commandAllocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commandAllocate.commandBufferCount=1;
        VkCommandBuffer command;Check(vkAllocateCommandBuffers(resources.device,&commandAllocate,&command),"allocate command buffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(vkBeginCommandBuffer(command,&begin),"begin command buffer");
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,resources.pipeline);
        vkCmdDispatch(command,1,1,1);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image=resources.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        const VkClearColorValue color{{17.f/255.f,34.f/255.f,51.f/255.f,1.f}};
        vkCmdClearColorImage(command,resources.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={4,4,1};
        vkCmdCopyImageToBuffer(command,resources.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,resources.buffer,1,&copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=resources.buffer;host.size=VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        Check(vkEndCommandBuffer(command),"end command buffer");
        VkFenceCreateInfo fenceCreate{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(vkCreateFence(resources.device,&fenceCreate,nullptr,&resources.fence),"create fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        Check(vkQueueSubmit(queue,1,&submit,resources.fence),"submit GPU work");
        Check(vkWaitForFences(resources.device,1,&resources.fence,VK_TRUE,5000000000ull),"wait for GPU work");
        result.shaderExecuted=true;
        Check(vkMapMemory(resources.device,resources.bufferMemory,0,VK_WHOLE_SIZE,0,&resources.mapped),"map readback");
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=resources.bufferMemory;range.size=VK_WHOLE_SIZE;
        Check(vkInvalidateMappedMemoryRanges(resources.device,1,&range),"invalidate readback");
        const auto* bytes=static_cast<const std::uint8_t*>(resources.mapped);
        constexpr std::array<std::uint8_t,4> expected{17,34,51,255};
        for(unsigned i=0;i<16;++i) {
            if(!std::equal(expected.begin(),expected.end(),bytes+i*4))throw std::runtime_error("GPU readback pixel mismatch");
            ++result.verifiedPixels;
        }
        result.passed=true;
    }catch(const std::exception& error){result.error=error.what();}
    return result;
}
}
