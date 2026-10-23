// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
}

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

void RecordPipelineDescriptorTopology(
    bool uses_push_descriptors, u32 descriptor_bindings,
    u32 max_push_descriptors,
    u32 max_update_after_bind_descriptors) noexcept;
void RecordRendererImageCreationTopology(
    bool mutable_format, bool extended_usage, bool color_storage,
    bool depth, bool block, u64 allocation_bytes,
    u32 bounded_format_list_entries) noexcept;
void RecordRendererImageViewTopology(bool format_reinterpreted,
                                     bool storage_view, u32 base_format,
                                     u32 view_format) noexcept;

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    vk::Pipeline Handle() const noexcept {
        return *pipeline;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return static_cast<bool>(pipeline);
    }

    [[nodiscard]] vk::Result BuildResult() const noexcept {
        return build_result;
    }

    [[nodiscard]] bool WasCompileRequired() const noexcept {
        return build_result == vk::Result::ePipelineCompileRequired ||
               build_result == vk::Result::ePipelineCompileRequiredEXT;
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::LogicalStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::LogicalStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    const Shader::Info* TryGetStage(Shader::LogicalStage stage) const noexcept {
        return stages[u32(stage)];
    }

    // Serialized background warm-up owns metadata-only Shader::Info objects:
    // their persistent resource layout is valid, but runtime user-data spans
    // are intentionally empty.  Once a foreground lookup has refreshed the
    // canonical Program::info, point the already-compiled pipeline at that
    // live object.  This is a pointer publication on the GPU command thread;
    // it does not introduce guest/host or Vulkan synchronization.
    void RebindStageInfo(Shader::LogicalStage stage,
                         const Shader::Info* info) noexcept {
        auto& current = stages[u32(stage)];
        if (current != info) [[unlikely]] {
            current = info;
        }
    }

    bool IsCompute() const {
        return is_compute;
    }

    [[nodiscard]] u64 Identity() const noexcept {
        return identity;
    }

    using DescriptorWrites = boost::container::small_vector<vk::WriteDescriptorSet, 16>;
    using BufferBarriers = boost::container::small_vector<vk::BufferMemoryBarrier2, 16>;

    void BindResources(DescriptorWrites& set_writes, const BufferBarriers& buffer_barriers,
                       const Shader::PushData& push_data) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    const u64 identity;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    vk::Result build_result{vk::Result::eNotReady};
    bool uses_push_descriptors{};
    bool is_compute;
};

}
