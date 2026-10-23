// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleVulkanCompute.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <numeric>
#include <stdexcept>
namespace Lsx4::Apple {
namespace {
void Check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
struct Allocation {
    std::uint64_t base{},end{};std::vector<std::uint8_t> bytes;
    VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};
};
struct Context {
    VkInstance instance{};VkDevice device{};VkCommandPool commands{};VkFence fence{};
    VkShaderModule shader{};VkPipelineLayout layout{};VkPipeline pipeline{};
    VkDescriptorSetLayout descriptors{};VkDescriptorPool descriptorPool{};
    std::vector<Allocation> allocations;
    ~Context() {
        if(device) {
            vkDeviceWaitIdle(device);
            if(fence)vkDestroyFence(device,fence,nullptr);
            if(commands)vkDestroyCommandPool(device,commands,nullptr);
            if(pipeline)vkDestroyPipeline(device,pipeline,nullptr);
            if(layout)vkDestroyPipelineLayout(device,layout,nullptr);
            if(shader)vkDestroyShaderModule(device,shader,nullptr);
            if(descriptorPool)vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            if(descriptors)vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
            for(auto& allocation:allocations) {
                if(allocation.mapped)vkUnmapMemory(device,allocation.memory);
                if(allocation.buffer)vkDestroyBuffer(device,allocation.buffer,nullptr);
                if(allocation.memory)vkFreeMemory(device,allocation.memory,nullptr);
            }
            vkDestroyDevice(device,nullptr);
        }
        if(instance)vkDestroyInstance(instance,nullptr);
    }
};
bool Has(const std::vector<VkExtensionProperties>& extensions,const char* name) {
    return std::ranges::any_of(extensions,[&](const auto& value){return std::strcmp(value.extensionName,name)==0;});
}
}
std::string DispatchVulkanCompute(const CompiledComputeShader& compiled,
    std::array<std::uint32_t,3> groups,std::vector<std::vector<std::uint8_t>>& buffers) {
    constexpr std::uint64_t MemoryLimit=128ull*1024*1024;
    const auto count=compiled.buffers.size();
    if(count!=buffers.size() || count>40 || compiled.spirv.size()<5 || compiled.spirv[0]!=0x07230203)
        throw std::runtime_error("Invalid compute dispatch input");
    Context context;
    std::vector<std::size_t> parent(count);std::iota(parent.begin(),parent.end(),0);
    auto root=[&](std::size_t i){while(parent[i]!=i)i=parent[i];return i;};
    auto guest=[&](std::size_t i){return compiled.buffers[i].inlineBytes.empty();};
    auto begin=[&](std::size_t i){return guest(i)?compiled.buffers[i].address:0;};
    auto end=[&](std::size_t i){return (guest(i)?compiled.buffers[i].address:0)+compiled.buffers[i].size;};
    for(std::size_t i=0;i<count;++i) {
        const auto& binding=compiled.buffers[i];
        if(!binding.size || binding.size>MemoryLimit || binding.size!=buffers[i].size() ||
           binding.address>UINT64_MAX-binding.size || (!guest(i) && buffers[i]!=binding.inlineBytes))
            throw std::runtime_error("Invalid compute buffer bytes");
        for(std::size_t j=0;j<i;++j)if(guest(i) && guest(j) && begin(i)<end(j) && begin(j)<end(i)) {
            const auto overlapBegin=std::max(binding.address,compiled.buffers[j].address);
            const auto overlapEnd=std::min(end(i),end(j));
            if(overlapBegin<overlapEnd && !std::equal(buffers[i].begin()+overlapBegin-binding.address,
                buffers[i].begin()+overlapEnd-binding.address,buffers[j].begin()+overlapBegin-compiled.buffers[j].address))
                throw std::runtime_error("Inconsistent aliased compute buffer snapshot");
            parent[root(i)]=root(j);
        }
    }
    std::map<std::size_t,std::size_t> mapping;std::vector<std::size_t> bindingAllocation(count);
    for(std::size_t i=0;i<count;++i) {
        const auto [position,inserted]=mapping.emplace(root(i),context.allocations.size());
        if(inserted)context.allocations.push_back({begin(i),end(i)});
        auto& allocation=context.allocations[position->second];
        allocation.base=std::min(allocation.base,begin(i));allocation.end=std::max(allocation.end,end(i));
        bindingAllocation[i]=position->second;
    }
    std::uint64_t total=0;
    for(auto& allocation:context.allocations) {
        const auto size=allocation.end-allocation.base;
        if(size>MemoryLimit || total>MemoryLimit-size)throw std::runtime_error("Compute staging memory budget exceeded");
        allocation.bytes.resize(size);total+=size;
    }
    for(std::size_t i=0;i<count;++i) {
        auto& allocation=context.allocations[bindingAllocation[i]];
        const auto offset=guest(i)?compiled.buffers[i].address-allocation.base:0;
        if(offset%256)throw std::runtime_error("Unaligned overlapping compute descriptors require a shared memory route");
        std::copy(buffers[i].begin(),buffers[i].end(),allocation.bytes.begin()+offset);
    }
    std::uint32_t number=0;Check(vkEnumerateInstanceExtensionProperties(nullptr,&number,nullptr),"instance extensions count");
    std::vector<VkExtensionProperties> extensions(number);
    Check(vkEnumerateInstanceExtensionProperties(nullptr,&number,extensions.data()),"instance extensions");
    std::vector<const char*> enabled;
    const bool portability=Has(extensions,VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if(portability)enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="LSX4 compute";app.apiVersion=VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};instance.pApplicationInfo=&app;
    instance.flags=portability?VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR:0;
    instance.enabledExtensionCount=enabled.size();instance.ppEnabledExtensionNames=enabled.data();
    Check(vkCreateInstance(&instance,nullptr,&context.instance),"create instance");
    Check(vkEnumeratePhysicalDevices(context.instance,&number,nullptr),"physical device count");
    if(!number)throw std::runtime_error("No Vulkan GPU");std::vector<VkPhysicalDevice> physicals(number);
    Check(vkEnumeratePhysicalDevices(context.instance,&number,physicals.data()),"physical devices");
    const auto physical=physicals.front();VkPhysicalDeviceProperties properties;vkGetPhysicalDeviceProperties(physical,&properties);
    if(properties.limits.minStorageBufferOffsetAlignment>256 || count>properties.limits.maxPerStageDescriptorStorageBuffers)
        throw std::runtime_error("Compute descriptor limits unavailable");
    for(unsigned i=0;i<3;++i)if(!groups[i] || groups[i]>properties.limits.maxComputeWorkGroupCount[i])
        throw std::runtime_error("Compute group count exceeds device limit");
    vkGetPhysicalDeviceQueueFamilyProperties(physical,&number,nullptr);
    std::vector<VkQueueFamilyProperties> families(number);vkGetPhysicalDeviceQueueFamilyProperties(physical,&number,families.data());
    std::uint32_t family=0;while(family<number && !(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;
    if(family==number)throw std::runtime_error("No compute queue");
    Check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&number,nullptr),"device extensions count");
    extensions.resize(number);Check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&number,extensions.data()),"device extensions");
    enabled.clear();if(Has(extensions,"VK_KHR_portability_subset"))enabled.push_back("VK_KHR_portability_subset");
    const float priority=1;VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex=family;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device.queueCreateInfoCount=1;device.pQueueCreateInfos=&queueInfo;
    device.enabledExtensionCount=enabled.size();device.ppEnabledExtensionNames=enabled.data();
    Check(vkCreateDevice(physical,&device,nullptr,&context.device),"create compute device");
    VkQueue queue;vkGetDeviceQueue(context.device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties memoryProperties;vkGetPhysicalDeviceMemoryProperties(physical,&memoryProperties);
    for(auto& allocation:context.allocations) {
        if(allocation.bytes.size()>properties.limits.maxStorageBufferRange)throw std::runtime_error("Compute buffer range exceeds device limit");
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=allocation.bytes.size();info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        Check(vkCreateBuffer(context.device,&info,nullptr,&allocation.buffer),"create compute buffer");
        VkMemoryRequirements requirement;vkGetBufferMemoryRequirements(context.device,allocation.buffer,&requirement);
        unsigned type=0;while(type<memoryProperties.memoryTypeCount && (!(requirement.memoryTypeBits&(1u<<type)) ||
            !(memoryProperties.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))++type;
        if(type==memoryProperties.memoryTypeCount)throw std::runtime_error("No host-visible compute memory");
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};memory.allocationSize=requirement.size;memory.memoryTypeIndex=type;
        Check(vkAllocateMemory(context.device,&memory,nullptr,&allocation.memory),"allocate compute buffer");
        Check(vkBindBufferMemory(context.device,allocation.buffer,allocation.memory,0),"bind compute buffer");
        Check(vkMapMemory(context.device,allocation.memory,0,VK_WHOLE_SIZE,0,&allocation.mapped),"map compute buffer");
        std::memcpy(allocation.mapped,allocation.bytes.data(),allocation.bytes.size());
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=allocation.memory;range.size=VK_WHOLE_SIZE;
        Check(vkFlushMappedMemoryRanges(context.device,1,&range),"publish compute buffer");
    }
    std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
    for(unsigned i=0;i<count;++i)layoutBindings.push_back({i,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount=count;setInfo.pBindings=layoutBindings.data();
    Check(vkCreateDescriptorSetLayout(context.device,&setInfo,nullptr,&context.descriptors),"create compute descriptors");
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,128};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&context.descriptors;
    layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
    Check(vkCreatePipelineLayout(context.device,&layout,nullptr,&context.layout),"create compute layout");
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=compiled.spirv.size()*4;module.pCode=compiled.spirv.data();
    Check(vkCreateShaderModule(context.device,&module,nullptr,&context.shader),"create compute shader");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipeline.layout=context.layout;
    pipeline.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};pipeline.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module=context.shader;pipeline.stage.pName="main";
    Check(vkCreateComputePipelines(context.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&context.pipeline),"create compute pipeline");
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,std::uint32_t(std::max<std::size_t>(1,count))};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&poolSize;
    Check(vkCreateDescriptorPool(context.device,&pool,nullptr,&context.descriptorPool),"create descriptor pool");
    VkDescriptorSetAllocateInfo setAllocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};setAllocate.descriptorPool=context.descriptorPool;
    setAllocate.descriptorSetCount=1;setAllocate.pSetLayouts=&context.descriptors;VkDescriptorSet descriptor;
    Check(vkAllocateDescriptorSets(context.device,&setAllocate,&descriptor),"allocate descriptors");
    std::vector<VkDescriptorBufferInfo> bufferInfos(count);std::vector<VkWriteDescriptorSet> writes(count);
    for(unsigned i=0;i<count;++i) {
        const auto& allocation=context.allocations[bindingAllocation[i]];
        bufferInfos[i]={allocation.buffer,begin(i)-allocation.base,compiled.buffers[i].size};
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=descriptor;writes[i].dstBinding=i;
        writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&bufferInfos[i];
    }
    vkUpdateDescriptorSets(context.device,count,writes.data(),0,nullptr);
    VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};commands.queueFamilyIndex=family;
    Check(vkCreateCommandPool(context.device,&commands,nullptr,&context.commands),"create command pool");
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=context.commands;
    allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;VkCommandBuffer command;
    Check(vkAllocateCommandBuffers(context.device,&allocate,&command),"allocate compute command");
    VkCommandBufferBeginInfo start{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};start.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(command,&start),"begin compute command");
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,context.pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&descriptor,0,nullptr);
    vkCmdPushConstants(command,context.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,128,compiled.pushConstants.data());
    vkCmdDispatch(command,groups[0],groups[1],groups[2]);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    Check(vkEndCommandBuffer(command),"end compute command");
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};Check(vkCreateFence(context.device,&fence,nullptr,&context.fence),"create compute fence");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
    Check(vkQueueSubmit(queue,1,&submit,context.fence),"submit compute");
    Check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,5000000000ull),"wait compute");
    for(auto& allocation:context.allocations) {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=allocation.memory;range.size=VK_WHOLE_SIZE;
        Check(vkInvalidateMappedMemoryRanges(context.device,1,&range),"read compute result");
    }
    auto result=buffers;
    for(unsigned i=0;i<count;++i)if(compiled.buffers[i].written) {
        const auto& allocation=context.allocations[bindingAllocation[i]];
        const auto offset=guest(i)?compiled.buffers[i].address-allocation.base:0;
        std::memcpy(result[i].data(),static_cast<const std::uint8_t*>(allocation.mapped)+offset,result[i].size());
    }
    buffers=std::move(result);return properties.deviceName;
}
}
