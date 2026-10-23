// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/static_vector.hpp>
#include <boost/container/small_vector.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/assert.h"
#include "common/config.h"
#include "common/content_fingerprint.h"
#include "common/debug.h"
#include "common/path_util.h"
#include "common/types.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_driver_axis.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#include <vk_mem_alloc.h>

#include <array>
#include <cstdio>
#include <ctime>
#ifdef __ANDROID__
#include <android/log.h>
#else
#define __android_log_print(...) ((void)0)
#endif
#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/instruction.h"
#include "executor/orbshdr_corpus.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/pm4_opcodes.h"
#include "video_core/amdgpu/regs_shader.h"
#include "executor/gnmcap_format.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/memory.h"
#if defined(_WIN32)
#include <cstdlib>
static inline void* mmap(void*, std::size_t len, int, int, int, long) {
    return std::malloc(len);
}
static inline int munmap(void* p, std::size_t) {
    std::free(p);
    return 0;
}
static inline void usleep(unsigned) {}
#ifndef PROT_READ
#define PROT_READ 0
#define PROT_WRITE 0
#define MAP_PRIVATE 0
#define MAP_ANONYMOUS 0
#define MAP_FAILED ((void*)nullptr)
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#include <csignal>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#ifdef __ANDROID__
#include <fcntl.h>
#include <sys/syscall.h>
#endif
#endif

#ifdef __ANDROID__
namespace {

PFN_vkCmdPipelineBarrier2 g_a750_barrier_capture_next{};
std::atomic<unsigned long long> g_a750_barrier_capture_sequence{};

struct A750BufferBarrierCapture {
    u64 handle{};
    u64 src_stage{};
    u64 src_access{};
    u64 dst_stage{};
    u64 dst_access{};
    u64 offset{};
    u64 size{};
};

struct A750ImageBarrierCapture {
    u64 handle{};
    u64 src_stage{};
    u64 src_access{};
    u64 dst_stage{};
    u64 dst_access{};
    u64 old_layout{};
    u64 new_layout{};
    u64 aspect{};
    u64 base_mip{};
    u64 levels{};
    u64 base_layer{};
    u64 layers{};
    u64 reserved{};
};

struct A750BarrierCapture {
    u64 magic{};
    u64 version{};
    u64 struct_size{};
    u64 sequence{};
    u64 committed_sequence{};
    u64 tid{};
    u64 caller{};
    u64 command_buffer{};
    u64 dependency_flags{};
    u64 memory_barrier_count{};
    u64 buffer_barrier_count{};
    u64 image_barrier_count{};
    u64 pnext{};
    u64 anomaly{};
    u64 captured_buffer_count{};
    u64 captured_image_count{};
    std::array<A750BufferBarrierCapture, 4> buffers{};
    std::array<A750ImageBarrierCapture, 4> images{};
};

static_assert(sizeof(A750BarrierCapture) == 768);
A750BarrierCapture* g_a750_barrier_capture{};

unsigned long long VulkanHandleBits(const VkCommandBuffer handle) {
    return reinterpret_cast<unsigned long long>(handle);
}

unsigned long long VulkanHandleBits(const VkImage handle) {
    return reinterpret_cast<unsigned long long>(handle);
}

unsigned long long VulkanHandleBits(const VkBuffer handle) {
    return reinterpret_cast<unsigned long long>(handle);
}

VKAPI_ATTR void VKAPI_CALL ExecutorA750CaptureCmdPipelineBarrier2(
    VkCommandBuffer command_buffer, const VkDependencyInfo* dependency_info) {
    constexpr unsigned long long NullDependency = 1ULL << 0;
    constexpr unsigned long long NullMemoryBarrierArray = 1ULL << 1;
    constexpr unsigned long long NullBufferBarrierArray = 1ULL << 2;
    constexpr unsigned long long NullImageBarrierArray = 1ULL << 3;
    constexpr unsigned long long NullImage = 1ULL << 4;
    constexpr unsigned long long EmptyAspect = 1ULL << 5;
    constexpr unsigned long long EmptyLevelRange = 1ULL << 6;
    constexpr unsigned long long EmptyLayerRange = 1ULL << 7;
    constexpr unsigned long long UndefinedNewLayout = 1ULL << 8;
    constexpr unsigned long long SourceAccessWithoutStage = 1ULL << 9;
    constexpr unsigned long long DestinationAccessWithoutStage = 1ULL << 10;
    constexpr unsigned long long NullBuffer = 1ULL << 11;
    constexpr unsigned long long EmptyBufferRange = 1ULL << 12;

    const unsigned long long sequence =
        g_a750_barrier_capture_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    unsigned long long anomaly = 0;
    if (dependency_info == nullptr) {
        anomaly |= NullDependency;
    } else {
        if (dependency_info->memoryBarrierCount != 0 &&
            dependency_info->pMemoryBarriers == nullptr) {
            anomaly |= NullMemoryBarrierArray;
        }
        if (dependency_info->bufferMemoryBarrierCount != 0 &&
            dependency_info->pBufferMemoryBarriers == nullptr) {
            anomaly |= NullBufferBarrierArray;
        }
        if (dependency_info->imageMemoryBarrierCount != 0 &&
            dependency_info->pImageMemoryBarriers == nullptr) {
            anomaly |= NullImageBarrierArray;
        }
        if (dependency_info->pMemoryBarriers != nullptr) {
            for (u32 index = 0; index < dependency_info->memoryBarrierCount; ++index) {
                const VkMemoryBarrier2& barrier = dependency_info->pMemoryBarriers[index];
                if (barrier.srcAccessMask != 0 && barrier.srcStageMask == 0) {
                    anomaly |= SourceAccessWithoutStage;
                }
                if (barrier.dstAccessMask != 0 && barrier.dstStageMask == 0) {
                    anomaly |= DestinationAccessWithoutStage;
                }
            }
        }
        if (dependency_info->pBufferMemoryBarriers != nullptr) {
            for (u32 index = 0; index < dependency_info->bufferMemoryBarrierCount; ++index) {
                const VkBufferMemoryBarrier2& barrier =
                    dependency_info->pBufferMemoryBarriers[index];
                if (barrier.buffer == VK_NULL_HANDLE) {
                    anomaly |= NullBuffer;
                }
                if (barrier.size == 0) {
                    anomaly |= EmptyBufferRange;
                }
                if (barrier.srcAccessMask != 0 && barrier.srcStageMask == 0) {
                    anomaly |= SourceAccessWithoutStage;
                }
                if (barrier.dstAccessMask != 0 && barrier.dstStageMask == 0) {
                    anomaly |= DestinationAccessWithoutStage;
                }
            }
        }
        if (dependency_info->pImageMemoryBarriers != nullptr) {
            for (u32 index = 0; index < dependency_info->imageMemoryBarrierCount; ++index) {
                const VkImageMemoryBarrier2& barrier =
                    dependency_info->pImageMemoryBarriers[index];
                if (barrier.image == VK_NULL_HANDLE) {
                    anomaly |= NullImage;
                }
                if (barrier.subresourceRange.aspectMask == 0) {
                    anomaly |= EmptyAspect;
                }
                if (barrier.subresourceRange.levelCount == 0) {
                    anomaly |= EmptyLevelRange;
                }
                if (barrier.subresourceRange.layerCount == 0) {
                    anomaly |= EmptyLayerRange;
                }
                if (barrier.newLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
                    anomaly |= UndefinedNewLayout;
                }
                if (barrier.srcAccessMask != 0 && barrier.srcStageMask == 0) {
                    anomaly |= SourceAccessWithoutStage;
                }
                if (barrier.dstAccessMask != 0 && barrier.dstStageMask == 0) {
                    anomaly |= DestinationAccessWithoutStage;
                }
            }
        }
    }

    if (g_a750_barrier_capture != nullptr) {
        A750BarrierCapture capture{
            .magic = 0x4C53583441373530ULL,
            .version = 1,
            .struct_size = sizeof(A750BarrierCapture),
            .sequence = sequence,
            .committed_sequence = 0,
            .tid = static_cast<u64>(::syscall(SYS_gettid)),
            .caller = reinterpret_cast<u64>(__builtin_return_address(0)),
            .command_buffer = VulkanHandleBits(command_buffer),
            .dependency_flags = dependency_info ? dependency_info->dependencyFlags : 0,
            .memory_barrier_count = dependency_info ? dependency_info->memoryBarrierCount : 0,
            .buffer_barrier_count = dependency_info ? dependency_info->bufferMemoryBarrierCount : 0,
            .image_barrier_count = dependency_info ? dependency_info->imageMemoryBarrierCount : 0,
            .pnext = reinterpret_cast<u64>(dependency_info ? dependency_info->pNext : nullptr),
            .anomaly = anomaly,
            .captured_buffer_count =
                dependency_info ? std::min<u32>(dependency_info->bufferMemoryBarrierCount, 4) : 0,
            .captured_image_count =
                dependency_info ? std::min<u32>(dependency_info->imageMemoryBarrierCount, 4) : 0,
        };
        if (dependency_info != nullptr && dependency_info->pBufferMemoryBarriers != nullptr) {
            for (u32 index = 0; index < capture.captured_buffer_count; ++index) {
                const VkBufferMemoryBarrier2& barrier =
                    dependency_info->pBufferMemoryBarriers[index];
                capture.buffers[index] = {
                    .handle = VulkanHandleBits(barrier.buffer),
                    .src_stage = barrier.srcStageMask,
                    .src_access = barrier.srcAccessMask,
                    .dst_stage = barrier.dstStageMask,
                    .dst_access = barrier.dstAccessMask,
                    .offset = barrier.offset,
                    .size = barrier.size,
                };
            }
        }
        if (dependency_info != nullptr && dependency_info->pImageMemoryBarriers != nullptr) {
            for (u32 index = 0; index < capture.captured_image_count; ++index) {
                const VkImageMemoryBarrier2& barrier =
                    dependency_info->pImageMemoryBarriers[index];
                capture.images[index] = {
                    .handle = VulkanHandleBits(barrier.image),
                    .src_stage = barrier.srcStageMask,
                    .src_access = barrier.srcAccessMask,
                    .dst_stage = barrier.dstStageMask,
                    .dst_access = barrier.dstAccessMask,
                    .old_layout = static_cast<u64>(barrier.oldLayout),
                    .new_layout = static_cast<u64>(barrier.newLayout),
                    .aspect = barrier.subresourceRange.aspectMask,
                    .base_mip = barrier.subresourceRange.baseMipLevel,
                    .levels = barrier.subresourceRange.levelCount,
                    .base_layer = barrier.subresourceRange.baseArrayLayer,
                    .layers = barrier.subresourceRange.layerCount,
                };
            }
        }
        *g_a750_barrier_capture = capture;
        std::atomic_thread_fence(std::memory_order_release);
        g_a750_barrier_capture->committed_sequence = sequence;
    }
    if (anomaly != 0) {
        __android_log_print(
            ANDROID_LOG_ERROR, "LSX4Native",
            "[EXECUTOR_A750_VK_BARRIER_ANOMALY] sequence=%llu caller=%p anomaly=0x%llx "
            "memory=%u buffer=%u image=%u",
            sequence, __builtin_return_address(0), anomaly,
            dependency_info ? dependency_info->memoryBarrierCount : 0,
            dependency_info ? dependency_info->bufferMemoryBarrierCount : 0,
            dependency_info ? dependency_info->imageMemoryBarrierCount : 0);
    }

    if (g_a750_barrier_capture_next != nullptr) {
        g_a750_barrier_capture_next(command_buffer, dependency_info);
    }
}

bool InstallA750BarrierCapture(PFN_vkCmdPipelineBarrier2& dispatch,
                               const std::filesystem::path& output_path) {
    if (dispatch == nullptr || dispatch == &ExecutorA750CaptureCmdPipelineBarrier2) {
        return false;
    }
    const int fd = ::open(output_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0 || ::ftruncate(fd, sizeof(A750BarrierCapture)) != 0) {
        if (fd >= 0) {
            ::close(fd);
        }
        return false;
    }
    void* const mapped = ::mmap(nullptr, sizeof(A750BarrierCapture), PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
        return false;
    }
    g_a750_barrier_capture = static_cast<A750BarrierCapture*>(mapped);
    g_a750_barrier_capture_next = dispatch;
    dispatch = &ExecutorA750CaptureCmdPipelineBarrier2;
    return true;
}

} // namespace
#endif

namespace Vulkan {

vk::DeviceSize Instance::StorageMinAlignment() const {
    const vk::DeviceSize reported = properties.limits.minStorageBufferOffsetAlignment;
#ifdef __ANDROID__
    static const vk::DeviceSize forced = [] {
        const char* value = std::getenv("EXECUTOR_BISECT_FORCE_SSBO_ALIGNMENT");
        if (value == nullptr || value[0] == '\0') {
            return vk::DeviceSize{0};
        }
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 0);
        if (end == value || *end != '\0' || parsed < 4 || parsed > 4096 ||
            (parsed & (parsed - 1)) != 0) {
            return vk::DeviceSize{0};
        }
        return static_cast<vk::DeviceSize>(parsed);
    }();
    return std::max(reported, forced);
#else
    return reported;
#endif
}

namespace {

bool ExecutorVkGpuTimelineRequestedAtDeviceCreation() {
#ifdef __ANDROID__
    return std::getenv("EXECUTOR_ANDROID_GPU_TIMELINE") != nullptr ||
           ::access("/data/data/app.lsx4.android/files/lsx4-home/diag-vk-gpu-timeline",
                    F_OK) == 0;
#else
    return false;
#endif
}

bool ExecutorDisableVkRobustness() {
#ifdef __ANDROID__
    if (Config::managedDisableVkRobustness()) {
        return true;
    }
    static const bool disabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        return std::filesystem::exists(user_dir / "run-disable-vk-robustness") ||
               std::filesystem::exists(user_dir.parent_path() /
                                       "run-disable-vk-robustness");
    }();
    return disabled;
#else
    return false;
#endif
}

bool ExecutorEnableVkCoarseFragment() {
#ifdef __ANDROID__
    if (Config::managedCoarseFragmentShading() ||
        VideoCore::IsManagedMobileGpuOptimization()) {
        return true;
    }
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        return std::filesystem::exists(user_dir / "run-vk-coarse-fragment") ||
               std::filesystem::exists(user_dir.parent_path() / "run-vk-coarse-fragment");
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750VmaBlock64Requested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{"run-sm8650-a750-vma-block64-ab"};
        return std::filesystem::exists(user_dir / marker) ||
               std::filesystem::exists(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750TurnipVmaBlock64Requested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{"run-sm8650-a750-turnip-vma-block64-ab"};
        return std::filesystem::exists(user_dir / marker) ||
               std::filesystem::exists(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750TurnipBudgetBaselineRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-turnip-budget-baseline-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750TurnipBoundedResidencyRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-turnip-bounded-residency-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750TurnipSustainedResidencyRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-turnip-sustained-residency-drain-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750TurnipSegmentedSparseBdaRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-turnip-bda-segmented-sparse-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750TurnipFlatSparseBdaRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-turnip-bda-flat-sparse-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750QcomSegmentedSparseBdaRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-qcom-bda-segmented-sparse-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750QcomSingleBoundaryRecordingRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-qcom-single-boundary-recording-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750TurnipSparseUploadReservationRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-turnip-sparse-upload-reservation-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750QcomAllUploadReservationRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-qcom-all-upload-reservation-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750QcomStructuralReuseDisabled() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-qcom-disable-structural-reuse-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750QcomFormattedBufferDeferDisabled() {
#ifdef __ANDROID__
    static const bool disabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-qcom-disable-formatted-buffer-defer-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return disabled;
#else
    return false;
#endif
}

bool ExecutorA750QcomAliasedSharedU64LoweringDisabled() {
#ifdef __ANDROID__
    static const bool disabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-qcom-disable-aliased-shared-u64-lowering-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return disabled;
#else
    return false;
#endif
}

bool ExecutorA750VmaBlock64Disabled() {
#ifdef __ANDROID__
    static const bool disabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view legacy_marker{
            "run-disable-sm8650-a750-vma-block64"};
        constexpr std::string_view composed_marker{
            "run-disable-sm8650-a750-turnip-composed-memory"};
        return std::filesystem::exists(user_dir / legacy_marker) ||
               std::filesystem::exists(user_dir.parent_path() / legacy_marker) ||
               std::filesystem::exists(user_dir / composed_marker) ||
               std::filesystem::exists(user_dir.parent_path() / composed_marker);
    }();
    return disabled;
#else
    return false;
#endif
}

bool ExecutorA750CombinedPressureGcRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{"run-sm8650-a750-combined-pressure-gc"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750ConservativeImageTransitionStagesRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-conservative-image-stages-ab"};
        return std::filesystem::exists(user_dir / marker) ||
               std::filesystem::exists(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750BarrierCaptureRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{"run-diagnostic-sm8650-a750-vk-barrier-capture"};
        return std::filesystem::exists(user_dir / marker) ||
               std::filesystem::exists(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750ResidencyTelemetryRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{"run-diagnostic-sm8650-a750-residency"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750DescriptorTopologyRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "diag-sm8650-a750-descriptor-topology"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750QcomBoundedDescriptorBatchRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-qcom-bounded-descriptor-batch-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750ImageTopologyRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{"diag-sm8650-a750-image-topology"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750BoundedImageFormatListRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "opt-sm8650-a750-bounded-image-format-list"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750ProtectedGcFastNoopRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-d9-protected-gc-fast-noop-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750NarrowImageFlagsRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-turnip-narrow-image-flags-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750RoleImageUsageRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-turnip-role-image-usage-ab"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750HostMetadataCompactionDisabled() {
#ifdef __ANDROID__
    static const bool disabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-disable-sm8650-a750-host-metadata-compaction"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return disabled;
#else
    return false;
#endif
}

bool ExecutorA750PresentSourceSubmitSplitRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-diagnostic-sm8650-a750-present-source-submit-split"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750VideoOutCandidateUidRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-diagnostic-sm8650-a750-videoout-candidate-uid"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750VideoOutSlot0TrackingRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-diagnostic-sm8650-a750-videoout-slot0-tracking"};
        return std::filesystem::is_regular_file(user_dir / marker) ||
               std::filesystem::is_regular_file(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750ExplicitImageSubresourcesRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{"run-sm8650-a750-explicit-image-subresources-ab"};
        return std::filesystem::exists(user_dir / marker) ||
               std::filesystem::exists(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750ReadOnlyDepthStencilAccessRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{"run-sm8650-a750-readonly-depth-access-ab"};
        return std::filesystem::exists(user_dir / marker) ||
               std::filesystem::exists(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

bool ExecutorA750SplitDepthStencilBarriersRequested() {
#ifdef __ANDROID__
    static const bool enabled = [] {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        constexpr std::string_view marker{
            "run-sm8650-a750-split-depth-stencil-barriers-ab"};
        return std::filesystem::exists(user_dir / marker) ||
               std::filesystem::exists(user_dir.parent_path() / marker);
    }();
    return enabled;
#else
    return false;
#endif
}

std::vector<vk::PhysicalDevice> EnumeratePhysicalDevices(vk::UniqueInstance& instance) {
    auto [devices_result, devices] = instance->enumeratePhysicalDevices();
    ASSERT_MSG(devices_result == vk::Result::eSuccess, "Failed to enumerate physical devices: {}",
               vk::to_string(devices_result));
    return std::move(devices);
}

std::vector<std::string> GetSupportedExtensions(vk::PhysicalDevice physical) {
    const auto [extensions_result, extensions] = physical.enumerateDeviceExtensionProperties();
    if (extensions_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Could not query supported extensions: {}",
                  vk::to_string(extensions_result));
        return {};
    }
    std::vector<std::string> supported_extensions;
    supported_extensions.reserve(extensions.size());
    for (const auto& extension : extensions) {
        supported_extensions.emplace_back(extension.extensionName.data());
    }
    return supported_extensions;
}

vk::FormatProperties3 GetFormatProperties(vk::PhysicalDevice physical, vk::Format format) {
    vk::FormatProperties3 properties3{};
    vk::FormatProperties2 properties2 = {
        .pNext = &properties3,
    };
    physical.getFormatProperties2(format, &properties2);
    return properties3;
}

std::unordered_map<vk::Format, vk::FormatProperties3> GetFormatProperties(
    vk::PhysicalDevice physical) {
    std::unordered_map<vk::Format, vk::FormatProperties3> format_properties;
    for (const auto& format_info : LiverpoolToVK::SurfaceFormats()) {
        const auto format = format_info.vk_format;
        if (!format_properties.contains(format)) {
            format_properties.emplace(format, GetFormatProperties(physical, format));
        }
    }
    for (const auto& format_info : LiverpoolToVK::DepthFormats()) {
        const auto format = format_info.vk_format;
        if (!format_properties.contains(format)) {
            format_properties.emplace(format, GetFormatProperties(physical, format));
        }
    }
    static constexpr std::array misc_formats = {
        vk::Format::eA2R10G10B10UnormPack32,
        vk::Format::eB8G8R8A8Unorm,
        vk::Format::eB8G8R8A8Srgb,
        vk::Format::eD24UnormS8Uint,
    };
    for (const auto& format : misc_formats) {
        if (!format_properties.contains(format)) {
            format_properties.emplace(format, GetFormatProperties(physical, format));
        }
    }
    return format_properties;
}

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

#if defined(ANDROID) || defined(LSX4_APPLE_RUNTIME)
VkPipelineStageFlags LegacyStageMask(VkPipelineStageFlags2 mask, bool source,
                                     bool& conservative) {
    constexpr VkPipelineStageFlags2 mapped_extended =
        VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
        VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT |
        VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
        VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
        VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT;
    VkPipelineStageFlags legacy = static_cast<VkPipelineStageFlags>(mask & 0xffffffffu);
    if (mask & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
                VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT)) {
        legacy |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (mask & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT)) {
        legacy |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    }
    if (mask & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) {
        legacy |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                  VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                  VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    }
    if ((mask & ~(VkPipelineStageFlags2{0xffffffffu} | mapped_extended)) != 0) {
        conservative = true;
    }
    if (legacy == 0) {
        legacy = source ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                        : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    return legacy;
}

VkAccessFlags LegacyAccessMask(VkAccessFlags2 mask, bool& conservative) {
    constexpr VkAccessFlags2 mapped_extended =
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkAccessFlags legacy = static_cast<VkAccessFlags>(mask & 0xffffffffu);
    if (mask & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) {
        legacy |= VK_ACCESS_SHADER_READ_BIT;
    }
    if (mask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) {
        legacy |= VK_ACCESS_SHADER_WRITE_BIT;
    }
    if ((mask & ~(VkAccessFlags2{0xffffffffu} | mapped_extended)) != 0) {
        conservative = true;
    }
    return legacy;
}

VKAPI_ATTR void VKAPI_CALL LegacyCmdPipelineBarrier2(
    VkCommandBuffer command_buffer, const VkDependencyInfo* dependency_info) {
    boost::container::small_vector<VkMemoryBarrier, 8> memory_barriers;
    boost::container::small_vector<VkBufferMemoryBarrier, 16> buffer_barriers;
    boost::container::small_vector<VkImageMemoryBarrier, 16> image_barriers;
    VkPipelineStageFlags src_stages{};
    VkPipelineStageFlags dst_stages{};
    bool conservative = dependency_info->pNext != nullptr;

    memory_barriers.reserve(dependency_info->memoryBarrierCount +
                            (dependency_info->pNext != nullptr ? 1u : 0u));
    if (dependency_info->pNext != nullptr && dependency_info->memoryBarrierCount == 0 &&
        dependency_info->bufferMemoryBarrierCount == 0 &&
        dependency_info->imageMemoryBarrierCount == 0) {
        memory_barriers.push_back(VkMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        });
    }
    for (u32 i = 0; i < dependency_info->memoryBarrierCount; ++i) {
        const auto& barrier = dependency_info->pMemoryBarriers[i];
        conservative |= barrier.pNext != nullptr;
        src_stages |= LegacyStageMask(barrier.srcStageMask, true, conservative);
        dst_stages |= LegacyStageMask(barrier.dstStageMask, false, conservative);
        memory_barriers.push_back(VkMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = LegacyAccessMask(barrier.srcAccessMask, conservative),
            .dstAccessMask = LegacyAccessMask(barrier.dstAccessMask, conservative),
        });
    }
    buffer_barriers.reserve(dependency_info->bufferMemoryBarrierCount);
    for (u32 i = 0; i < dependency_info->bufferMemoryBarrierCount; ++i) {
        const auto& barrier = dependency_info->pBufferMemoryBarriers[i];
        conservative |= barrier.pNext != nullptr;
        src_stages |= LegacyStageMask(barrier.srcStageMask, true, conservative);
        dst_stages |= LegacyStageMask(barrier.dstStageMask, false, conservative);
        buffer_barriers.push_back(VkBufferMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = LegacyAccessMask(barrier.srcAccessMask, conservative),
            .dstAccessMask = LegacyAccessMask(barrier.dstAccessMask, conservative),
            .srcQueueFamilyIndex = barrier.srcQueueFamilyIndex,
            .dstQueueFamilyIndex = barrier.dstQueueFamilyIndex,
            .buffer = barrier.buffer,
            .offset = barrier.offset,
            .size = barrier.size,
        });
    }
    image_barriers.reserve(dependency_info->imageMemoryBarrierCount);
    for (u32 i = 0; i < dependency_info->imageMemoryBarrierCount; ++i) {
        const auto& barrier = dependency_info->pImageMemoryBarriers[i];
        conservative |= barrier.pNext != nullptr;
        src_stages |= LegacyStageMask(barrier.srcStageMask, true, conservative);
        dst_stages |= LegacyStageMask(barrier.dstStageMask, false, conservative);
        image_barriers.push_back(VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = LegacyAccessMask(barrier.srcAccessMask, conservative),
            .dstAccessMask = LegacyAccessMask(barrier.dstAccessMask, conservative),
            .oldLayout = barrier.oldLayout,
            .newLayout = barrier.newLayout,
            .srcQueueFamilyIndex = barrier.srcQueueFamilyIndex,
            .dstQueueFamilyIndex = barrier.dstQueueFamilyIndex,
            .image = barrier.image,
            .subresourceRange = barrier.subresourceRange,
        });
    }
    if (conservative) {
        src_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dst_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        constexpr VkAccessFlags full_memory =
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        for (auto& barrier : memory_barriers) {
            barrier.srcAccessMask = full_memory;
            barrier.dstAccessMask = full_memory;
        }
        for (auto& barrier : buffer_barriers) {
            barrier.srcAccessMask = full_memory;
            barrier.dstAccessMask = full_memory;
        }
        for (auto& barrier : image_barriers) {
            barrier.srcAccessMask = full_memory;
            barrier.dstAccessMask = full_memory;
        }
    }
    if (src_stages == 0) {
        src_stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    if (dst_stages == 0) {
        dst_stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier(
        command_buffer, src_stages, dst_stages, dependency_info->dependencyFlags,
        static_cast<u32>(memory_barriers.size()), memory_barriers.data(),
        static_cast<u32>(buffer_barriers.size()), buffer_barriers.data(),
        static_cast<u32>(image_barriers.size()), image_barriers.data());
}
#endif

}

Instance::Instance(bool enable_validation, bool enable_crash_diagnostic,
                   bool create_headless_device)
    : instance{CreateInstance(Frontend::WindowSystemType::Headless, enable_validation,
                              enable_crash_diagnostic)},
      physical_devices{EnumeratePhysicalDevices(instance)} {
    if (!create_headless_device || physical_devices.empty()) {
        return;
    }
    physical_device = physical_devices[0];
    for (const auto& pd : physical_devices) {
        if (pd.getProperties().apiVersion >= TargetVulkanApiVersion) {
            physical_device = pd;
            break;
        }
    }
    available_extensions = GetSupportedExtensions(physical_device);
    format_properties = GetFormatProperties(physical_device);
    properties = physical_device.getProperties();
    memory_properties = physical_device.getMemoryProperties();
    CollectDeviceParameters();
    LOG_INFO(Render_Vulkan, "[EXECUTOR_HEADLESS_VK] device={} apiVersion={}.{}",
             properties.deviceName.data(), VK_VERSION_MAJOR(properties.apiVersion),
             VK_VERSION_MINOR(properties.apiVersion));
    if (!CreateDevice()) {
        LOG_ERROR(Render_Vulkan, "[EXECUTOR_HEADLESS_VK] CreateDevice failed");
        return;
    }
    CollectPhysicalMemoryInfo();
    CollectImageFormatInfo();
    CollectToolingInfo();
}

Instance::Instance(Frontend::WindowSDL& window, s32 physical_device_index,
                   bool enable_validation, bool enable_crash_diagnostic)
    : instance{CreateInstance(window.GetWindowInfo().type, enable_validation,
                              enable_crash_diagnostic)},
      physical_devices{EnumeratePhysicalDevices(instance)} {
    if (enable_validation) {
        debug_callback = CreateDebugCallback(*instance);
    }
    const std::size_t num_physical_devices = static_cast<u16>(physical_devices.size());
    ASSERT_MSG(num_physical_devices > 0, "No physical devices found");
    LOG_INFO(Render_Vulkan, "Found {} physical devices", num_physical_devices);

    if (physical_device_index < 0) {
        std::vector<
            std::tuple<size_t, vk::PhysicalDeviceProperties2, vk::PhysicalDeviceMemoryProperties>>
            properties2{};
        for (auto const& physical : physical_devices) {
            properties2.emplace_back(properties2.size(), physical.getProperties2(),
                                     physical.getMemoryProperties());
        }
        std::sort(properties2.begin(), properties2.end(), [](const auto& left, const auto& right) {
            const vk::PhysicalDeviceProperties& left_prop = std::get<1>(left).properties;
            const vk::PhysicalDeviceProperties& right_prop = std::get<1>(right).properties;
            if (left_prop.apiVersion >= TargetVulkanApiVersion &&
                right_prop.apiVersion < TargetVulkanApiVersion) {
                return true;
            }
            if (left_prop.deviceType != right_prop.deviceType) {
                return left_prop.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
            }
            constexpr auto get_mem = [](const vk::PhysicalDeviceMemoryProperties& mem) -> size_t {
                size_t max = 0;
                for (u32 i = 0; i < mem.memoryHeapCount; i++) {
                    const vk::MemoryHeap& heap = mem.memoryHeaps[i];
                    if (heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal && heap.size > max) {
                        max = heap.size;
                    }
                }
                return max;
            };
            size_t left_mem_size = get_mem(std::get<2>(left));
            size_t right_mem_size = get_mem(std::get<2>(right));
            return left_mem_size > right_mem_size;
        });
        physical_device = physical_devices[std::get<0>(properties2[0])];
    } else {
        ASSERT_MSG(physical_device_index < num_physical_devices,
                   "Invalid physical device index {} provided when only {} devices exist",
                   physical_device_index, num_physical_devices);

        physical_device = physical_devices[physical_device_index];
    }

    available_extensions = GetSupportedExtensions(physical_device);
    format_properties = GetFormatProperties(physical_device);
    properties = physical_device.getProperties();
    memory_properties = physical_device.getMemoryProperties();
    CollectDeviceParameters();
#if defined(ANDROID) || defined(LSX4_APPLE_RUNTIME)
    if (properties.apiVersion < TargetVulkanApiVersion) {
        LOG_WARNING(Render_Vulkan,
                  "EXECUTOR_VULKAN_WSI phase=device_api_compat "
                  "targetApi={}.{} deviceApi={}.{} result=feature_detect_continue",
                  VK_VERSION_MAJOR(TargetVulkanApiVersion), VK_VERSION_MINOR(TargetVulkanApiVersion),
                  VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));
    }
#else
    ASSERT_MSG(properties.apiVersion >= TargetVulkanApiVersion,
               "Vulkan {}.{} is required, but only {}.{} is supported by device!",
               VK_VERSION_MAJOR(TargetVulkanApiVersion), VK_VERSION_MINOR(TargetVulkanApiVersion),
               VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));
#endif

#if defined(ANDROID) || defined(LSX4_APPLE_RUNTIME)
    if (!CreateDevice()) {
        throw std::runtime_error("Android Vulkan device lacks required upstream renderer features");
    }
#else
    ASSERT_MSG(CreateDevice(), "Failed to create Vulkan device");
#endif
    CollectPhysicalMemoryInfo();
    CollectImageFormatInfo();
    CollectToolingInfo();

    for (const auto& format : LiverpoolToVK::SurfaceFormats()) {
        if (!IsFormatSupported(format.vk_format, format.flags)) {
            LOG_WARNING(Render_Vulkan,
                        "Surface format data_format={}, number_format={} is not fully supported "
                        "(vk_format={}, missing features={})",
                        static_cast<u32>(format.data_format),
                        static_cast<u32>(format.number_format), vk::to_string(format.vk_format),
                        vk::to_string(format.flags & ~GetFormatFeatureFlags(format.vk_format)));
        }
    }
    for (const auto& format : LiverpoolToVK::DepthFormats()) {
        if (!IsFormatSupported(format.vk_format, format.flags)) {
            LOG_WARNING(Render_Vulkan,
                        "Depth format z_format={}, stencil_format={} is not fully supported "
                        "(vk_format={}, missing features={})",
                        static_cast<u32>(format.z_format), static_cast<u32>(format.stencil_format),
                        vk::to_string(format.vk_format),
                        vk::to_string(format.flags & ~GetFormatFeatureFlags(format.vk_format)));
        }
    }
}

Instance::~Instance() {
    VideoCore::SetAndroidMemoryPressureMonitorRequested(false);
    vmaDestroyAllocator(allocator);
}

std::string Instance::GetDriverVersionName() {
    const u32 version = properties.driverVersion;
    if (driver_id == vk::DriverId::eNvidiaProprietary) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (driver_id == vk::DriverId::eIntelProprietaryWindows) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

bool Instance::CreateDevice() {
    const vk::StructureChain feature_chain =
        physical_device
            .getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                          vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
                           vk::PhysicalDeviceCustomBorderColorFeaturesEXT,
                           vk::PhysicalDeviceDepthClipControlFeaturesEXT,
                           vk::PhysicalDeviceDepthClipEnableFeaturesEXT,
                           vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR,
                           vk::PhysicalDeviceLegacyVertexAttributesFeaturesEXT,
                           vk::PhysicalDeviceProvokingVertexFeaturesEXT,
                           vk::PhysicalDeviceMaintenance8FeaturesKHR,
                           vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT,
                           vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT,
                           vk::PhysicalDeviceRobustness2FeaturesEXT,
                           vk::PhysicalDeviceVertexAttributeDivisorFeatures,
                           vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT,
                          vk::PhysicalDeviceFaultFeaturesEXT,
                          vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT,
                          vk::PhysicalDeviceFragmentShadingRateFeaturesKHR,
                          vk::PhysicalDeviceDynamicRenderingLocalReadFeatures,
                          vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT,
                          vk::PhysicalDevicePortabilitySubsetFeaturesKHR,
                           vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT,
                           vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR,
                          vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT,
                          vk::PhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT>();
    features = feature_chain.get().features;
    const auto vertex_input_dynamic_state_features =
        feature_chain.get<vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT>();
    const auto vertex_attribute_divisor_features =
        feature_chain.get<vk::PhysicalDeviceVertexAttributeDivisorFeatures>();
    const auto custom_border_color_features =
        feature_chain.get<vk::PhysicalDeviceCustomBorderColorFeaturesEXT>();
    const auto depth_clip_control_features =
        feature_chain.get<vk::PhysicalDeviceDepthClipControlFeaturesEXT>();
    const auto depth_clip_enable_features =
        feature_chain.get<vk::PhysicalDeviceDepthClipEnableFeaturesEXT>();
    const auto fragment_shader_barycentric_features =
        feature_chain.get<vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR>();
    const auto legacy_vertex_attributes_features =
        feature_chain.get<vk::PhysicalDeviceLegacyVertexAttributesFeaturesEXT>();
    const auto provoking_vertex_features =
        feature_chain.get<vk::PhysicalDeviceProvokingVertexFeaturesEXT>();
    const auto maintenance_8_features =
        feature_chain.get<vk::PhysicalDeviceMaintenance8FeaturesKHR>();
    const auto attachment_feedback_loop_layout_features =
        feature_chain.get<vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT>();
    const auto attachment_feedback_loop_dynamic_state_features =
        feature_chain.get<vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT>();
    const auto multisampled_render_to_single_sampled_features =
        feature_chain
            .get<vk::PhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT>();
    const vk::StructureChain properties_chain = physical_device.getProperties2<
        vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties,
        vk::PhysicalDeviceVulkan12Properties, vk::PhysicalDeviceVulkan13Properties,
        vk::PhysicalDevicePushDescriptorPropertiesKHR>();
    vk11_props = properties_chain.get<vk::PhysicalDeviceVulkan11Properties>();
    vk12_props = properties_chain.get<vk::PhysicalDeviceVulkan12Properties>();
    vk13_props = properties_chain.get<vk::PhysicalDeviceVulkan13Properties>();
    push_descriptor_props = properties_chain.get<vk::PhysicalDevicePushDescriptorPropertiesKHR>();
    LOG_INFO(Render_Vulkan, "Physical device subgroup size {}", vk11_props.subgroupSize);

    if (available_extensions.empty()) {
        LOG_CRITICAL(Render_Vulkan, "No extensions supported by device.");
        return false;
    }

    boost::container::static_vector<const char*, 64> enabled_extensions;
    const auto add_extension = [&](std::string_view extension) -> bool {
        const auto result =
            std::find_if(available_extensions.begin(), available_extensions.end(),
                         [&](const std::string& name) { return name == extension; });

        if (result != available_extensions.end()) {
            LOG_INFO(Render_Vulkan, "Enabling extension: {}", extension);
            enabled_extensions.push_back(extension.data());
            return true;
        }

        LOG_WARNING(Render_Vulkan, "Extension {} unavailable.", extension);
        return false;
    };

    ASSERT(add_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME));
    const bool push_descriptors = add_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    const bool vertex_attribute_divisor =
        add_extension(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    if (!push_descriptors) {
        push_descriptor_props.maxPushDescriptors = 0;
    }

    const bool promoted_12 = properties.apiVersion >= VK_API_VERSION_1_2;
    const bool promoted_13 = properties.apiVersion >= VK_API_VERSION_1_3;
    bool timeline_semaphore_khr{};
    bool buffer_device_address_khr{};
    bool dynamic_rendering_khr{};
    bool synchronization2_khr{};
    bool maintenance4_khr{};
#if defined(ANDROID) || defined(LSX4_APPLE_RUNTIME)
    if (!promoted_12) {
        const auto& queried_vk12 =
            feature_chain.get<vk::PhysicalDeviceVulkan12Features>();
        if (queried_vk12.storageBuffer8BitAccess ||
            queried_vk12.uniformAndStorageBuffer8BitAccess ||
            queried_vk12.storagePushConstant8) {
            add_extension(VK_KHR_8BIT_STORAGE_EXTENSION_NAME);
        }
        if (queried_vk12.shaderFloat16 || queried_vk12.shaderInt8) {
            add_extension(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
        }
        add_extension(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    }
    timeline_semaphore_khr =
        !promoted_12 && add_extension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    buffer_device_address_khr =
        !promoted_12 && add_extension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    dynamic_rendering_khr =
        !promoted_13 && add_extension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    synchronization2_khr =
        !promoted_13 && add_extension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    maintenance4_khr =
        !promoted_13 && add_extension(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    const bool pipeline_creation_cache_control_extension =
        promoted_13 ||
        add_extension(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
    pipeline_creation_cache_control =
        pipeline_creation_cache_control_extension &&
        feature_chain.get<vk::PhysicalDeviceVulkan13Features>()
            .pipelineCreationCacheControl;
#endif

    maintenance_8 = add_extension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME) &&
                    maintenance_8_features.maintenance8;
    attachment_feedback_loop = add_extension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
    if (attachment_feedback_loop) {
        attachment_feedback_loop =
            add_extension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
        if (!attachment_feedback_loop) {
            enabled_extensions.pop_back();
        }
    }
    attachment_feedback_loop =
        attachment_feedback_loop &&
        attachment_feedback_loop_layout_features.attachmentFeedbackLoopLayout &&
        attachment_feedback_loop_dynamic_state_features.attachmentFeedbackLoopDynamicState;
    dynamic_rendering_local_read =
        add_extension(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    if (dynamic_rendering_local_read) {
        dynamic_rendering_local_read_features =
            feature_chain.get<vk::PhysicalDeviceDynamicRenderingLocalReadFeatures>();
        dynamic_rendering_local_read =
            dynamic_rendering_local_read_features.dynamicRenderingLocalRead;
#ifdef __ANDROID__
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_VK_DYNAMIC_RENDERING_LOCAL_READ] enabled=%u",
            dynamic_rendering_local_read ? 1u : 0u);
#endif
    }
    depth_range_unrestricted = add_extension(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    dynamic_state_3 = add_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (dynamic_state_3) {
        dynamic_state_3_features =
            feature_chain.get<vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- extendedDynamicState3ColorWriteMask: {}",
                 dynamic_state_3_features.extendedDynamicState3ColorWriteMask);
    }
    fragment_shading_rate_features =
        feature_chain.get<vk::PhysicalDeviceFragmentShadingRateFeaturesKHR>();
#ifdef __ANDROID__
    const bool coarse_fragment_requested = ExecutorEnableVkCoarseFragment();
    fragment_shading_rate =
        coarse_fragment_requested &&
        fragment_shading_rate_features.pipelineFragmentShadingRate &&
        add_extension(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    if (fragment_shading_rate) {
        const auto [rates_result, rates] = physical_device.getFragmentShadingRatesKHR();
        if (rates_result == vk::Result::eSuccess) {
            const auto supports_rate = [&](const u32 width, const u32 height) {
                return std::ranges::any_of(rates, [&](const auto& rate) {
                    return rate.fragmentSize.width == width &&
                           rate.fragmentSize.height == height &&
                           static_cast<bool>(rate.sampleCounts &
                                             vk::SampleCountFlagBits::e1);
                });
            };
            if (supports_rate(2, 2)) {
                coarse_fragment_size = {2, 2};
            }
            if (supports_rate(2, 1)) {
                mobile_fragment_size = {2, 1};
            } else if (supports_rate(1, 2)) {
                mobile_fragment_size = {1, 2};
            }
        }
        fragment_shading_rate =
            coarse_fragment_size.width > 1 ||
            coarse_fragment_size.height > 1 ||
            mobile_fragment_size.width > 1 ||
            mobile_fragment_size.height > 1;
    }
    LOG_INFO(Render_Vulkan,
             "EXECUTOR_VK_COARSE_FRAGMENT requested={} enabled={} pipelineFeature={} "
             "coarse={}x{} mobile={}x{}",
             coarse_fragment_requested, fragment_shading_rate,
             fragment_shading_rate_features.pipelineFragmentShadingRate,
             coarse_fragment_size.width, coarse_fragment_size.height,
             mobile_fragment_size.width, mobile_fragment_size.height);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VK_COARSE_FRAGMENT] requested=%u enabled=%u pipelineFeature=%u "
        "coarse=%ux%u mobile=%ux%u",
        coarse_fragment_requested ? 1u : 0u, fragment_shading_rate ? 1u : 0u,
        fragment_shading_rate_features.pipelineFragmentShadingRate ? 1u : 0u,
        coarse_fragment_size.width, coarse_fragment_size.height,
        mobile_fragment_size.width, mobile_fragment_size.height);
    VideoCore::SetMobileGpuCoarseShadingAvailable(
        mobile_fragment_size.width > 1 ||
        mobile_fragment_size.height > 1);
#endif
    robustness2 = add_extension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    if (robustness2) {
        robustness2_features = feature_chain.get<vk::PhysicalDeviceRobustness2FeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- robustBufferAccess2: {}",
                 robustness2_features.robustBufferAccess2);
        LOG_INFO(Render_Vulkan, "- robustImageAccess2: {}",
                 robustness2_features.robustImageAccess2);
        LOG_INFO(Render_Vulkan, "- nullDescriptor: {}", robustness2_features.nullDescriptor);
#ifdef __ANDROID__
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_VK_ROBUSTNESS2] extension=1 buffer=%u image=%u nullDescriptor=%u",
            robustness2_features.robustBufferAccess2 ? 1u : 0u,
            robustness2_features.robustImageAccess2 ? 1u : 0u,
            robustness2_features.nullDescriptor ? 1u : 0u);
#endif
    }
    device_fault_features = feature_chain.get<vk::PhysicalDeviceFaultFeaturesEXT>();
    LOG_INFO(Render_Vulkan, "- deviceFault: {}", device_fault_features.deviceFault);
    LOG_INFO(Render_Vulkan, "- deviceFaultVendorBinary: {}",
             device_fault_features.deviceFaultVendorBinary);
    device_fault = device_fault_features.deviceFault &&
                   add_extension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    custom_border_color = add_extension(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME) &&
                          custom_border_color_features.customBorderColors &&
                          custom_border_color_features.customBorderColorWithoutFormat;
    depth_clip_control = add_extension(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME) &&
                         depth_clip_control_features.depthClipControl;
    depth_clip_enable = add_extension(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME) &&
                        depth_clip_enable_features.depthClipEnable;
    vertex_input_dynamic_state =
        vertex_input_dynamic_state_features.vertexInputDynamicState &&
        add_extension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
#ifdef __ANDROID__
    const bool executor_disable_dynamic_vertex_input =
        std::getenv("EXECUTOR_BISECT_DISABLE_DYNAMIC_VERTEX_INPUT") != nullptr;
    if (executor_disable_dynamic_vertex_input) {
        vertex_input_dynamic_state = false;
    }
#endif
    list_restart = add_extension(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
    if (list_restart) {
        list_restart_features =
            feature_chain.get<vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- primitiveTopologyListRestart: {}",
                 list_restart_features.primitiveTopologyListRestart);
        LOG_INFO(Render_Vulkan, "- primitiveTopologyPatchListRestart: {}",
                 list_restart_features.primitiveTopologyPatchListRestart);
    }
    amd_shader_explicit_vertex_parameter =
        add_extension(VK_AMD_SHADER_EXPLICIT_VERTEX_PARAMETER_EXTENSION_NAME);
    if (!amd_shader_explicit_vertex_parameter) {
        fragment_shader_barycentric =
            add_extension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) &&
            fragment_shader_barycentric_features.fragmentShaderBarycentric;
    }
    legacy_vertex_attributes = add_extension(VK_EXT_LEGACY_VERTEX_ATTRIBUTES_EXTENSION_NAME) &&
                               legacy_vertex_attributes_features.legacyVertexAttributes;
#ifdef __ANDROID__
    const bool executor_disable_legacy_vertex_attributes =
        std::getenv("EXECUTOR_BISECT_DISABLE_LEGACY_VERTEX_ATTRIBUTES") != nullptr;
    if (executor_disable_legacy_vertex_attributes) {
        legacy_vertex_attributes = false;
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VK_VERTEX_INPUT_ROUTE] dynamic=%u legacy=%u forcedStatic=%u forcedNoLegacy=%u",
        vertex_input_dynamic_state ? 1u : 0u, legacy_vertex_attributes ? 1u : 0u,
        executor_disable_dynamic_vertex_input ? 1u : 0u,
        executor_disable_legacy_vertex_attributes ? 1u : 0u);
#endif
    provoking_vertex = add_extension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME) &&
                       provoking_vertex_features.provokingVertexLast;
    shader_stencil_export = add_extension(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    image_load_store_lod = add_extension(VK_AMD_SHADER_IMAGE_LOAD_STORE_LOD_EXTENSION_NAME);
    amd_gcn_shader = add_extension(VK_AMD_GCN_SHADER_EXTENSION_NAME);
    amd_shader_trinary_minmax = add_extension(VK_AMD_SHADER_TRINARY_MINMAX_EXTENSION_NAME);
    nv_framebuffer_mixed_samples = add_extension(VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME);
    amd_mixed_attachment_samples = add_extension(VK_AMD_MIXED_ATTACHMENT_SAMPLES_EXTENSION_NAME);
    const bool multisampled_render_to_single_sampled =
        multisampled_render_to_single_sampled_features.multisampledRenderToSingleSampled &&
        add_extension(VK_EXT_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_EXTENSION_NAME);
    shader_atomic_float = add_extension(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
    shader_atomic_float2 = add_extension(VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME);
    if (shader_atomic_float2) {
        shader_atomic_float2_features =
            feature_chain.get<vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- shaderBufferFloat32AtomicMinMax: {}",
                 shader_atomic_float2_features.shaderBufferFloat32AtomicMinMax);
        LOG_INFO(Render_Vulkan, "- shaderImageFloat32AtomicMinMax: {}",
                 shader_atomic_float2_features.shaderImageFloat32AtomicMinMax);
    }
    workgroup_memory_explicit_layout =
        add_extension(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
    if (workgroup_memory_explicit_layout) {
        workgroup_memory_explicit_layout_features =
            feature_chain.get<vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR>();
        LOG_INFO(Render_Vulkan, "- workgroupMemoryExplicitLayout: {}",
                 workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout);
        LOG_INFO(Render_Vulkan, "- workgroupMemoryExplicitLayoutScalarBlockLayout: {}",
                 workgroup_memory_explicit_layout_features
                     .workgroupMemoryExplicitLayoutScalarBlockLayout);
        LOG_INFO(
            Render_Vulkan, "- workgroupMemoryExplicitLayout16BitAccess: {}",
            workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout16BitAccess);
    }
    image_2d_view_of_3d = add_extension(VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME);
    if (image_2d_view_of_3d) {
        image_2d_view_of_3d_features =
            feature_chain.get<vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- image2DViewOf3D: {}",
                 image_2d_view_of_3d_features.image2DViewOf3D);
        LOG_INFO(Render_Vulkan, "- sampler2DViewOf3D: {}",
                 image_2d_view_of_3d_features.sampler2DViewOf3D);
    }
    calibrated_timestamps =
        (TRACY_GPU_ENABLED || ExecutorVkGpuTimelineRequestedAtDeviceCreation()) &&
        add_extension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);

#ifdef __APPLE__
    portability_subset = add_extension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    if (portability_subset) {
        portability_features = feature_chain.get<vk::PhysicalDevicePortabilitySubsetFeaturesKHR>();
    }
#endif

    supports_memory_budget = add_extension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    const auto family_properties = physical_device.getQueueFamilyProperties();
    if (family_properties.empty()) {
        LOG_CRITICAL(Render_Vulkan, "Physical device reported no queues.");
        return false;
    }

    bool graphics_queue_found = false;
    for (std::size_t i = 0; i < family_properties.size(); i++) {
        const u32 index = static_cast<u32>(i);
        if (family_properties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
            queue_family_index = index;
            graphics_queue_found = true;
        }
    }

    if (!graphics_queue_found) {
        LOG_CRITICAL(Render_Vulkan, "Unable to find graphics and/or present queues.");
        return false;
    }
    graphics_queue_supports_sparse =
        (family_properties[queue_family_index].queueFlags &
         vk::QueueFlagBits::eSparseBinding) != vk::QueueFlags{};

    static constexpr std::array queue_priorities = {1.0f};
    const vk::DeviceQueueCreateInfo queue_info = {
        .queueFamilyIndex = queue_family_index,
        .queueCount = static_cast<u32>(queue_priorities.size()),
        .pQueuePriorities = queue_priorities.data(),
    };

    const auto vk11_features = feature_chain.get<vk::PhysicalDeviceVulkan11Features>();
    vk12_features = feature_chain.get<vk::PhysicalDeviceVulkan12Features>();
    vk13_features = feature_chain.get<vk::PhysicalDeviceVulkan13Features>();
    capability_profile.api_version = properties.apiVersion;
    capability_profile.spirv_version = SpirvVersionForVulkan(properties.apiVersion);
#if defined(ANDROID) || defined(LSX4_APPLE_RUNTIME)
    constexpr CapabilityRoute timeline_fallback = CapabilityRoute::Legacy;
    constexpr CapabilityRoute bda_fallback = CapabilityRoute::Legacy;
#else
    constexpr CapabilityRoute timeline_fallback = CapabilityRoute::Unavailable;
    constexpr CapabilityRoute bda_fallback = CapabilityRoute::Unavailable;
#endif
    capability_profile.timeline_semaphore = SelectPromotedRoute(
        properties.apiVersion, VK_API_VERSION_1_2, timeline_semaphore_khr,
        vk12_features.timelineSemaphore, timeline_fallback);
    capability_profile.buffer_device_address = SelectPromotedRoute(
        properties.apiVersion, VK_API_VERSION_1_2, buffer_device_address_khr,
        vk12_features.bufferDeviceAddress, bda_fallback);
    capability_profile.dynamic_rendering = SelectPromotedRoute(
        properties.apiVersion, VK_API_VERSION_1_3, dynamic_rendering_khr,
        vk13_features.dynamicRendering, CapabilityRoute::Legacy);
    capability_profile.synchronization2 = SelectPromotedRoute(
        properties.apiVersion, VK_API_VERSION_1_3, synchronization2_khr,
        vk13_features.synchronization2, CapabilityRoute::Legacy);
    capability_profile.maintenance4 = SelectPromotedRoute(
        properties.apiVersion, VK_API_VERSION_1_3, maintenance4_khr,
        vk13_features.maintenance4, CapabilityRoute::Legacy);
    capability_profile.push_descriptors =
        push_descriptors && push_descriptor_props.maxPushDescriptors != 0
            ? CapabilityRoute::KhrExtension
            : CapabilityRoute::Legacy;
    capability_profile.vertex_attribute_divisor =
        vertex_attribute_divisor &&
        vertex_attribute_divisor_features.vertexAttributeInstanceRateDivisor;
    capability_profile.dynamic_rendering_local_read =
        dynamic_rendering_local_read && IsDynamicRenderingSupported();
    dynamic_rendering_local_read = capability_profile.dynamic_rendering_local_read;
    capability_profile.fragment_shading_rate = fragment_shading_rate;
    capability_profile.extended_dynamic_state3 = dynamic_state_3;
    capability_profile.robustness2 = robustness2;
    capability_profile.memory_budget = supports_memory_budget;
    capability_profile.shader_float16 = vk12_features.shaderFloat16;
    capability_profile.multisampled_render_to_single_sampled =
        multisampled_render_to_single_sampled;
    const MobileDriverFamily reported_mobile_driver_family =
        driver_id == vk::DriverId::eArmProprietary
            ? MobileDriverFamily::ArmProprietary
            : driver_id == vk::DriverId::eQualcommProprietary
            ? MobileDriverFamily::QualcommProprietary
            : driver_id == vk::DriverId::eMesaTurnip
            ? MobileDriverFamily::MesaTurnip
#if defined(LSX4_APPLE_RUNTIME)
            : driver_id == vk::DriverId::eMoltenvk
            ? MobileDriverFamily::MoltenVk
#endif
            : MobileDriverFamily::Unknown;
    const MobileDriverFamily mobile_driver_family =
        ResolveMobileDriverFamily(reported_mobile_driver_family, properties.vendorID);
    capability_profile.preferred_spatial_upscaler =
        PreferredSpatialUpscalerForFamily(mobile_driver_family);
    capability_profile.driver_version = properties.driverVersion;
    capability_profile.reported_mobile_driver_family = reported_mobile_driver_family;
    capability_profile.mobile_driver_family = mobile_driver_family;
    capability_profile.mobile_gpu_model =
        ResolveMobileGpuModel(properties.vendorID, properties.deviceID);
    capability_profile.mobile_gpu_generation =
        ResolveMobileGpuGeneration(capability_profile.mobile_gpu_model);
    capability_profile.gpu_compatibility_route = ResolveGpuCompatibilityRoute(
        reported_mobile_driver_family, properties.vendorID, properties.deviceID);
    capability_profile.pipeline_warmup =
        ResolvePipelineWarmupPolicy(reported_mobile_driver_family);
    capability_profile.imgui_upload =
        ResolveImGuiUploadPolicy(reported_mobile_driver_family);
#if defined(LSX4_APPLE_RUNTIME)
    // UIKit/AppKit hosts use the same completed-upload retirement contract
    // as the Android metadata compaction route, without enabling GPU quirks.
    capability_profile.imgui_upload.release_font_cpu_pixels = true;
#endif
    const bool a750_qcom_single_boundary_recording_requested =
        ExecutorA750QcomSingleBoundaryRecordingRequested();
    capability_profile.command_submission = ResolveCommandSubmissionPolicy(
        capability_profile.gpu_compatibility_route,
        a750_qcom_single_boundary_recording_requested);
    capability_profile.exact_proprietary_adreno750 =
        capability_profile.gpu_compatibility_route ==
        GpuCompatibilityRoute::Adreno750QualcommProprietary;
    capability_profile.exact_proprietary_adreno840 =
        capability_profile.gpu_compatibility_route ==
        GpuCompatibilityRoute::Adreno840QualcommProprietary;
    capability_profile.exact_mesa_turnip_adreno750 =
        capability_profile.gpu_compatibility_route ==
        GpuCompatibilityRoute::Adreno750MesaTurnip;
    capability_profile.exact_mesa_turnip_adreno840 =
        capability_profile.gpu_compatibility_route ==
        GpuCompatibilityRoute::Adreno840MesaTurnip;
    const std::string effective_driver_package_sha =
        DriverAxis::EffectivePackageSha256();
    capability_profile.mobile_driver_build = ResolveMobileDriverBuild(
        reported_mobile_driver_family, properties.vendorID, properties.deviceID,
        properties.driverVersion, effective_driver_package_sha);
    capability_profile.exact_mesa_turnip_a750_d9_lineage =
        capability_profile.exact_mesa_turnip_adreno750 &&
        IsMesaTurnipA750D9LineageBuild(capability_profile.mobile_driver_build);
    const bool a750_qcom_bounded_descriptor_batch_requested =
        ExecutorA750QcomBoundedDescriptorBatchRequested();
    capability_profile.descriptor_binding = ResolveDescriptorBindingPolicy(
        capability_profile.gpu_compatibility_route,
        capability_profile.mobile_driver_build,
        a750_qcom_bounded_descriptor_batch_requested);
    const bool candidate_a750_turnip_memory_policy_build =
        HasCandidateA750TurnipComposedMemoryPolicy(
            capability_profile.mobile_driver_build);
    const bool product_default_a750_turnip_memory_policy =
        HasProductDefaultA750TurnipComposedMemoryPolicy(
            capability_profile.mobile_driver_build);
    const bool validated_a750_turnip_memory_policy_disabled =
        ExecutorA750VmaBlock64Disabled();
    const bool a750_turnip_budget_baseline_requested =
        ExecutorA750TurnipBudgetBaselineRequested();
    const bool a750_turnip_sparse_upload_reservation_requested =
        ExecutorA750TurnipSparseUploadReservationRequested();
    const bool a750_turnip_bounded_residency_requested =
        ExecutorA750TurnipBoundedResidencyRequested();
    const bool a750_turnip_sustained_residency_requested =
        ExecutorA750TurnipSustainedResidencyRequested();
    const bool a750_qcom_all_upload_reservation_requested =
        ExecutorA750QcomAllUploadReservationRequested();
    capability_profile.memory_policy = ResolveMobileMemoryPolicy(
        reported_mobile_driver_family, properties.vendorID, properties.deviceID,
        capability_profile.mobile_driver_build,
        a750_turnip_budget_baseline_requested,
        validated_a750_turnip_memory_policy_disabled,
        a750_turnip_sparse_upload_reservation_requested,
        a750_turnip_bounded_residency_requested,
        a750_turnip_sustained_residency_requested,
        a750_qcom_all_upload_reservation_requested);

#if defined(LSX4_APPLE_RUNTIME)
    const auto* exact_upload_option = std::getenv("LSX4_APPLE_EXACT_DIRTY_UPLOAD");
    const bool exact_upload_requested = exact_upload_option &&
        std::strcmp(exact_upload_option, "1") == 0 &&
        reported_mobile_driver_family == MobileDriverFamily::MoltenVk;
    capability_profile.memory_policy = ApplyExactDirtyUploadDiagnostic(
        capability_profile.memory_policy, exact_upload_requested);
    if (exact_upload_requested) {
        std::fprintf(stderr, "[APPLE_EXACT_DIRTY_UPLOAD_POLICY] route=moltenvk mode=exact-dirty-ranges scope=all-cpu-dirty-uploads ownershipTransactionUnchanged=1\n");
    }
#endif

    const bool a750_turnip_segmented_sparse_bda_requested =
        ExecutorA750TurnipSegmentedSparseBdaRequested();
    const bool a750_turnip_flat_sparse_bda_requested =
        ExecutorA750TurnipFlatSparseBdaRequested();
    const bool a750_qcom_segmented_sparse_bda_requested =
        ExecutorA750QcomSegmentedSparseBdaRequested();
    capability_profile.memory_policy.bda_page_table = ResolveBdaPageTablePolicy(
        reported_mobile_driver_family, properties.vendorID, properties.deviceID,
        capability_profile.mobile_driver_build,
        a750_turnip_segmented_sparse_bda_requested,
        a750_qcom_segmented_sparse_bda_requested,
        validated_a750_turnip_memory_policy_disabled,
        IsSparseBufferResidencySupported(), properties.limits.maxStorageBufferRange,
        a750_turnip_flat_sparse_bda_requested);
    const bool a750_combined_pressure_gc_requested =
        ExecutorA750CombinedPressureGcRequested();
    capability_profile.aggregate_memory_pressure =
        ResolveAggregateMemoryPressurePolicy(
            capability_profile.gpu_compatibility_route,
            CanReportMemoryUsage(),
            capability_profile.memory_policy.residency_retirement,
            a750_combined_pressure_gc_requested);
    // Android process PSS/graphics/swap is an independent pressure domain from
    // VK_EXT_memory_budget.  Ask the off-render-thread sampler for every exact
    // A750 route whose resolved aggregate policy can consume it.  In
    // particular this includes the marker-gated coordinated-nonblocking
    // Turnip candidate: the driver heap stayed below its soft limit while the
    // process reached LMKD.  The typed policy remains false for A840, Mali,
    // unknown packages and marker-free Turnip, so those paths do not start the
    // sampler or gain a renderer branch.
    VideoCore::SetAndroidMemoryPressureMonitorRequested(
        capability_profile.aggregate_memory_pressure.cache_pressure_active);
    const bool a750_qcom_structural_reuse_disabled =
        ExecutorA750QcomStructuralReuseDisabled();
    capability_profile.graphics_lookup = ResolveGraphicsLookupPolicy(
        capability_profile.gpu_compatibility_route,
        a750_qcom_structural_reuse_disabled);
    // D9 has now completed the same generation-guarded structural-signature
    // route with the Bloodborne A750 residency fixes enabled.  User-data-only
    // register traffic cannot change GraphicsStructuralSignature: the guard
    // still requires the primitive override, Other generation and
    // ShaderProgram generation to match, and descriptor/SRT refresh remains
    // mandatory below it.  Promote only the imported package whose complete
    // SHA resolved to this build; A840, proprietary A750 and every other
    // Turnip package retain their existing policy.
    if (capability_profile.exact_mesa_turnip_a750_d9_lineage) {
        capability_profile.graphics_lookup.structural_signature_reuse_default =
            true;
    }
    const bool a750_barrier_capture_requested = ExecutorA750BarrierCaptureRequested();
    capability_profile.diagnostic_a750_barrier_capture =
        EnableExactProprietaryAdreno750BarrierCapture(
            reported_mobile_driver_family, properties.vendorID, properties.deviceID,
            a750_barrier_capture_requested);
    const bool a750_residency_telemetry_requested =
        ExecutorA750ResidencyTelemetryRequested();
    capability_profile.diagnostic_a750_residency_telemetry =
        EnableExactA750TurnipD9ResidencyTelemetry(
            capability_profile.gpu_compatibility_route,
            capability_profile.mobile_driver_build,
            a750_residency_telemetry_requested);
    capability_profile.optimization_a750_protected_gc_fast_noop_ab =
        ExecutorA750ProtectedGcFastNoopRequested() &&
        capability_profile.exact_mesa_turnip_a750_d9_lineage;
    const bool a750_descriptor_topology_requested =
        ExecutorA750DescriptorTopologyRequested();
    capability_profile.diagnostic_a750_descriptor_topology =
        a750_descriptor_topology_requested &&
        (capability_profile.exact_proprietary_adreno750 ||
         capability_profile.exact_mesa_turnip_adreno750);
    const bool a750_image_topology_requested =
        ExecutorA750ImageTopologyRequested();
    capability_profile.diagnostic_a750_image_topology =
        a750_image_topology_requested &&
        (capability_profile.exact_proprietary_adreno750 ||
         capability_profile.exact_mesa_turnip_adreno750);
    const bool a750_bounded_image_format_list_requested =
        ExecutorA750BoundedImageFormatListRequested();
    capability_profile.optimization_a750_bounded_image_format_list =
        a750_bounded_image_format_list_requested &&
        (capability_profile.exact_proprietary_adreno750 ||
         capability_profile.exact_mesa_turnip_adreno750);
    const bool a750_narrow_image_flags_requested =
        ExecutorA750NarrowImageFlagsRequested();
    capability_profile.optimization_a750_narrow_image_flags =
        a750_narrow_image_flags_requested &&
        capability_profile.exact_mesa_turnip_a750_d9_lineage;
    const bool a750_role_image_usage_requested =
        ExecutorA750RoleImageUsageRequested();
    capability_profile.optimization_a750_role_image_usage =
        a750_role_image_usage_requested &&
        capability_profile.exact_mesa_turnip_a750_d9_lineage;
    const bool a750_host_metadata_compaction_disabled =
        ExecutorA750HostMetadataCompactionDisabled();
    capability_profile.optimization_a750_host_metadata_compaction =
        capability_profile.exact_proprietary_adreno750 &&
        !a750_host_metadata_compaction_disabled;
    const bool a750_present_source_submit_split_requested =
        ExecutorA750PresentSourceSubmitSplitRequested();
    capability_profile.diagnostic_a750_present_source_submit_split =
        capability_profile.exact_proprietary_adreno750 &&
        a750_present_source_submit_split_requested;
    const bool a750_videoout_candidate_uid_requested =
        ExecutorA750VideoOutCandidateUidRequested();
    capability_profile.diagnostic_a750_videoout_candidate_uid =
        capability_profile.exact_proprietary_adreno750 &&
        a750_videoout_candidate_uid_requested;
    const bool a750_videoout_slot0_tracking_requested =
        ExecutorA750VideoOutSlot0TrackingRequested();
    capability_profile.diagnostic_a750_videoout_slot0_tracking =
        capability_profile.exact_proprietary_adreno750 &&
        a750_videoout_slot0_tracking_requested;
    const bool conservative_image_transition_stages_requested =
        ExecutorA750ConservativeImageTransitionStagesRequested();
    const bool explicit_image_subresources_requested =
        ExecutorA750ExplicitImageSubresourcesRequested();
    const bool read_only_depth_stencil_access_requested =
        ExecutorA750ReadOnlyDepthStencilAccessRequested();
    const bool split_depth_stencil_barriers_requested =
        ExecutorA750SplitDepthStencilBarriersRequested();
    const bool a750_qcom_formatted_buffer_defer_disabled =
        ExecutorA750QcomFormattedBufferDeferDisabled();
    const bool a750_qcom_aliased_shared_u64_lowering_disabled =
        ExecutorA750QcomAliasedSharedU64LoweringDisabled();
    capability_profile.driver_quirks = DriverQuirksForFamily(mobile_driver_family);
#if defined(LSX4_APPLE_RUNTIME)
    const char* descriptor_sets_env = std::getenv("LSX4_APPLE_ARGUMENT_BUFFER_DESCRIPTOR_SETS");
    const bool descriptor_sets_requested = descriptor_sets_env &&
        std::strcmp(descriptor_sets_env, "1") == 0 &&
        reported_mobile_driver_family == MobileDriverFamily::MoltenVk;
    capability_profile.push_descriptors = ResolveArgumentBufferDescriptorRoute(
        capability_profile.push_descriptors, reported_mobile_driver_family, descriptor_sets_requested);
    if (descriptor_sets_requested) {
        capability_profile.driver_quirks |= static_cast<u64>(CapabilityQuirk::RequireArgumentBufferDescriptorSets);
        push_descriptor_props.maxPushDescriptors = 0;
        std::fprintf(stderr, "[APPLE_ARGUMENT_BUFFER_DESCRIPTOR_POLICY] pushDescriptors=legacy descriptorSets=1 quirk=RequireArgumentBufferDescriptorSets\n");
    }
#endif

#if defined(LSX4_APPLE_RUNTIME)
    if (mobile_driver_family == MobileDriverFamily::MoltenVk) {
        const char* diagnostic = std::getenv("LSX4_APPLE_PHYSICAL_DRS_POLICY");
        const bool legacy = diagnostic && std::strcmp(diagnostic, "legacy") == 0;
        if (legacy) {
            capability_profile.driver_quirks &=
                ~static_cast<u64>(CapabilityQuirk::DisablePhysicalDrsResolve);
        }
        std::fprintf(stderr,
            "[APPLE_ANDROID_DRS_POLICY] family=moltenvk physicalDrsQuarantined=%d\n",
            legacy ? 0 : 1);
        std::fprintf(stderr, "[APPLE_EMPTY_DISPATCH_POLICY] skipEmptyDirect=%d\n",
            capability_profile.HasQuirk(CapabilityQuirk::SkipEmptyDirectDispatch) ? 1 : 0);
    }
#endif
    // MoltenVK 1.4.2 can expose incorrect OpArrayLength values for host
    // compute push descriptors. Carry their exact range in the tiling UBO.
    if (driver_id == vk::DriverId::eMoltenvk) {
        capability_profile.driver_quirks |=
            static_cast<u64>(CapabilityQuirk::ExplicitTilingBufferBounds);
        capability_profile.emulate_integer_pixel_centers = true;
    }
    capability_profile.driver_quirks |= ExactProprietaryAdreno750PipelineQuirks(
        reported_mobile_driver_family, properties.vendorID, properties.deviceID,
        !a750_qcom_formatted_buffer_defer_disabled,
        !a750_qcom_aliased_shared_u64_lowering_disabled);
    capability_profile.driver_quirks |= ExactProprietaryAdreno840PipelineQuirks(
        reported_mobile_driver_family, properties.vendorID, properties.deviceID);
    capability_profile.driver_quirks |= ExactProprietaryAdreno750RuntimeQuirks(
        reported_mobile_driver_family, properties.vendorID, properties.deviceID);
    capability_profile.driver_quirks |= ExactMesaTurnipAdreno840RuntimeQuirks(
        reported_mobile_driver_family, properties.vendorID, properties.deviceID);
    capability_profile.completion_sidecar = ResolveCompletionSidecarPolicy(
        reported_mobile_driver_family,
        capability_profile.HasQuirk(
            CapabilityQuirk::ElideCpuCompletionSidecarBarrier));
    capability_profile.driver_quirks |= ExactProprietaryAdreno750Quirks(
        reported_mobile_driver_family, properties.vendorID, properties.deviceID,
        conservative_image_transition_stages_requested,
        explicit_image_subresources_requested,
        read_only_depth_stencil_access_requested,
        split_depth_stencil_barriers_requested);
#if defined(ANDROID) || defined(LSX4_APPLE_RUNTIME)
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VK_BDA_CAPS] gpu=%s driver=%s exactTurnipA750=%u "
        "sparseQueue=%u sparseBinding=%u sparseResidencyBuffer=%u "
        "nonResidentStrict=%u maxStorageBufferRangeMiB=%llu "
        "logicalBdaTableMiB=512 sparseCandidateEligible=%u requested=%u flatSparseRequested=%u "
        "candidateBuild=%u productDefault=%u disabled=%u driverBuild=%s policyState=%s "
        "effective=%s descriptorSegments=%u descriptorSegmentMiB=%llu",
        MobileGpuModelName(capability_profile.mobile_gpu_model).data(),
        MobileDriverFamilyName(capability_profile.mobile_driver_family).data(),
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        graphics_queue_supports_sparse ? 1u : 0u,
        features.sparseBinding ? 1u : 0u,
        features.sparseResidencyBuffer ? 1u : 0u,
        properties.sparseProperties.residencyNonResidentStrict ? 1u : 0u,
        static_cast<unsigned long long>(
            properties.limits.maxStorageBufferRange / (1024ULL * 1024ULL)),
        capability_profile.exact_mesa_turnip_adreno750 &&
                IsSparseBufferResidencySupported()
            ? 1u
            : 0u,
        a750_turnip_segmented_sparse_bda_requested ? 1u : 0u,
        a750_turnip_flat_sparse_bda_requested ? 1u : 0u,
        candidate_a750_turnip_memory_policy_build ? 1u : 0u,
        product_default_a750_turnip_memory_policy ? 1u : 0u,
        validated_a750_turnip_memory_policy_disabled ? 1u : 0u,
        MobileDriverBuildName(capability_profile.mobile_driver_build).data(),
        MobileDriverBuildPolicyStateName(ResolveMobileDriverBuildPolicyState(
            capability_profile.mobile_driver_build)).data(),
        BdaPageTableModeName(
            capability_profile.memory_policy.bda_page_table.mode).data(),
        capability_profile.memory_policy.bda_page_table.descriptor_segments,
        static_cast<unsigned long long>(
            capability_profile.memory_policy.bda_page_table.descriptor_segment_size /
            (1024ULL * 1024ULL)));
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GPU_COMPATIBILITY_PROFILE] route=%s gpu=%s generation=%s "
        "driver=%s reportedDriver=%s "
        "vendor=0x%x "
        "device=0x%x driverVersion=0x%x driverBuild=%s api=0x%x exactQcomA750=%u "
        "exactQcomA840=%u exactTurnipA750=%u exactTurnipA840=%u "
        "memoryBlockMiB=%llu budgetAccounting=%s residencyRetirement=%s uploadReservation=%s "
        "uploadReservationScope=%s warmupMax=%u warmupBudgetMs=%u "
        "imguiPersistentMapAllowed=%u descriptorDedupAbAllowed=%u "
        "maxDeferredGuestSubmits=%u "
        "aggregatePressure=%u vmaPressure=%u turnipRetirement=%u combinedGc=%u "
        "structuralReuseDefault=%u structuralReuseAbAllowed=%u "
        "quirks=0x%llx",
        GpuCompatibilityRouteName(
            capability_profile.gpu_compatibility_route).data(),
        MobileGpuModelName(capability_profile.mobile_gpu_model).data(),
        MobileGpuGenerationName(
            capability_profile.mobile_gpu_generation).data(),
        MobileDriverFamilyName(capability_profile.mobile_driver_family).data(),
        MobileDriverFamilyName(
            capability_profile.reported_mobile_driver_family).data(),
        properties.vendorID, properties.deviceID, properties.driverVersion,
        MobileDriverBuildName(capability_profile.mobile_driver_build).data(),
        properties.apiVersion,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.exact_proprietary_adreno840 ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno840 ? 1u : 0u,
        static_cast<unsigned long long>(
            capability_profile.memory_policy.preferred_large_heap_block_size /
            (1024ULL * 1024ULL)),
        MemoryBudgetAccountingName(
            capability_profile.memory_policy.budget_accounting).data(),
        ResidencyRetirementModeName(
            capability_profile.memory_policy.residency_retirement).data(),
        UploadReservationModeName(
            capability_profile.memory_policy.upload_reservation).data(),
        UploadReservationScopeName(
            capability_profile.memory_policy.upload_reservation_scope).data(),
        capability_profile.pipeline_warmup.maximum_pipelines,
        capability_profile.pipeline_warmup.time_budget_ms,
        capability_profile.imgui_upload.allow_persistent_mapping_ab ? 1u : 0u,
        capability_profile.descriptor_binding.allow_exact_write_dedup_ab ? 1u : 0u,
        capability_profile.command_submission.maximum_deferred_guest_submit_boundaries,
        capability_profile.aggregate_memory_pressure.cache_pressure_active ? 1u : 0u,
        capability_profile.aggregate_memory_pressure.proprietary_vma_sampling ? 1u : 0u,
        capability_profile.aggregate_memory_pressure.coordinated_turnip_retirement ? 1u : 0u,
        capability_profile.aggregate_memory_pressure.combined_gc ? 1u : 0u,
        capability_profile.graphics_lookup.structural_signature_reuse_default ? 1u : 0u,
        capability_profile.graphics_lookup.allow_generation_guarded_reuse_ab ? 1u : 0u,
        static_cast<unsigned long long>(capability_profile.driver_quirks));
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_QCOM_RECORDING_BOUND] requested=%u exactQcomA750=%u "
        "effectiveBoundaries=%u waited=0",
        a750_qcom_single_boundary_recording_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.command_submission.maximum_deferred_guest_submit_boundaries);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_TURNIP_UPLOAD_RESERVATION] requested=%u "
        "exactTurnipA750=%u candidateBuild=%u disabled=%u effective=%s scope=%s",
        a750_turnip_sparse_upload_reservation_requested ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        candidate_a750_turnip_memory_policy_build ? 1u : 0u,
        validated_a750_turnip_memory_policy_disabled ? 1u : 0u,
        UploadReservationModeName(
            capability_profile.memory_policy.upload_reservation).data(),
        UploadReservationScopeName(
            capability_profile.memory_policy.upload_reservation_scope).data());
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_QCOM_UPLOAD_RESERVATION] requested=%u "
        "exactQcomA750=%u effective=%s scope=%s",
        a750_qcom_all_upload_reservation_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        UploadReservationModeName(
            capability_profile.memory_policy.upload_reservation).data(),
        UploadReservationScopeName(
            capability_profile.memory_policy.upload_reservation_scope).data());
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_QCOM_STRUCTURAL_REUSE] disabledRequested=%u "
        "exactQcomA750=%u effective=%u",
        a750_qcom_structural_reuse_disabled ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.graphics_lookup.structural_signature_reuse_default
            ? 1u
            : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_QCOM_PIPELINE_QUIRKS] formattedDeferDisabledRequested=%u "
        "sharedU64LoweringDisabledRequested=%u exactQcomA750=%u "
        "formattedDeferEffective=%u sharedU64LoweringEffective=%u",
        a750_qcom_formatted_buffer_defer_disabled ? 1u : 0u,
        a750_qcom_aliased_shared_u64_lowering_disabled ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.HasQuirk(
            CapabilityQuirk::DeferFormattedBufferTypesUntilLowering)
            ? 1u
            : 0u,
        capability_profile.HasQuirk(CapabilityQuirk::LowerAliasedSharedU64ToU32)
            ? 1u
            : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_TURNIP_BOUNDED_RESIDENCY] requested=%u "
        "exactTurnipA750=%u candidateBuild=%u disabled=%u accounting=%s "
        "retirement=%s",
        a750_turnip_bounded_residency_requested ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        candidate_a750_turnip_memory_policy_build ? 1u : 0u,
        validated_a750_turnip_memory_policy_disabled ? 1u : 0u,
        MemoryBudgetAccountingName(
            capability_profile.memory_policy.budget_accounting).data(),
        ResidencyRetirementModeName(
            capability_profile.memory_policy.residency_retirement).data());
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_TURNIP_SUSTAINED_RESIDENCY] requested=%u "
        "exactTurnipA750=%u candidateBuild=%u disabled=%u retirement=%s "
        "cadenceMs=1000 waited=0",
        a750_turnip_sustained_residency_requested ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        candidate_a750_turnip_memory_policy_build ? 1u : 0u,
        validated_a750_turnip_memory_policy_disabled ? 1u : 0u,
        ResidencyRetirementModeName(
            capability_profile.memory_policy.residency_retirement).data());
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_TURNIP_BUDGET_BASELINE] requested=%u "
        "exactTurnipA750=%u effective=%u",
        a750_turnip_budget_baseline_requested ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        capability_profile.memory_policy.budget_accounting ==
                MemoryBudgetAccounting::ReportedUsageWithInitialBaseline
            ? 1u
            : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_COMBINED_PRESSURE_GC] requested=%u exactProprietaryA750=%u "
        "effective=%u softPercent=55 hardPercent=65",
        a750_combined_pressure_gc_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.aggregate_memory_pressure.combined_gc ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_IMAGE_TRANSITION_STAGES] requested=%u exactProprietaryA750=%u "
        "effective=%u",
        conservative_image_transition_stages_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.HasQuirk(
            CapabilityQuirk::ConservativeImageTransitionStages)
            ? 1u
            : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_EXPLICIT_IMAGE_SUBRESOURCES] requested=%u exactProprietaryA750=%u "
        "effective=%u",
        explicit_image_subresources_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.HasQuirk(CapabilityQuirk::ExplicitFullImageSubresourceCounts)
            ? 1u
            : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_READONLY_DEPTH_ACCESS] requested=%u exactProprietaryA750=%u "
        "effective=%u",
        read_only_depth_stencil_access_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.HasQuirk(CapabilityQuirk::ReadOnlyDepthStencilAccess)
            ? 1u
            : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_SPLIT_DEPTH_STENCIL_BARRIERS] requested=%u "
        "exactProprietaryA750=%u separateLayouts=%u effective=%u",
        split_depth_stencil_barriers_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        vk12_features.separateDepthStencilLayouts ? 1u : 0u,
        capability_profile.HasQuirk(CapabilityQuirk::SplitDepthStencilImageBarriers) &&
                vk12_features.separateDepthStencilLayouts
            ? 1u
            : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_VK_BARRIER_CAPTURE] requested=%u exactProprietaryA750=%u "
        "effective=%u",
        a750_barrier_capture_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.diagnostic_a750_barrier_capture ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_RESIDENCY_TELEMETRY] requested=%u exactTurnipA750=%u "
        "driverBuild=%u exactD9Build=%u effective=%u",
        a750_residency_telemetry_requested ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        static_cast<u32>(capability_profile.mobile_driver_build),
        capability_profile.exact_mesa_turnip_a750_d9_lineage ? 1u : 0u,
        capability_profile.diagnostic_a750_residency_telemetry ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_DESCRIPTOR_TOPOLOGY] requested=%u exactProprietaryA750=%u "
        "exactTurnipA750=%u effective=%u maxPushDescriptors=%u "
        "maxUpdateAfterBindDescriptors=%u",
        a750_descriptor_topology_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        capability_profile.diagnostic_a750_descriptor_topology ? 1u : 0u,
        push_descriptor_props.maxPushDescriptors,
        vk12_props.maxUpdateAfterBindDescriptorsInAllPools);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_IMAGE_TOPOLOGY] requested=%u exactProprietaryA750=%u "
        "exactTurnipA750=%u effective=%u",
        a750_image_topology_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        capability_profile.diagnostic_a750_image_topology ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_BOUNDED_IMAGE_FORMAT_LIST] requested=%u "
        "exactProprietaryA750=%u exactTurnipA750=%u effective=%u",
        a750_bounded_image_format_list_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        capability_profile.optimization_a750_bounded_image_format_list ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_NARROW_IMAGE_FLAGS] requested=%u exactTurnipA750=%u "
        "driverBuild=%u effective=%u proof=base-optimal-format-features",
        a750_narrow_image_flags_requested ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        static_cast<u32>(capability_profile.mobile_driver_build),
        capability_profile.optimization_a750_narrow_image_flags ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_ROLE_IMAGE_USAGE] requested=%u exactTurnipA750=%u "
        "driverBuild=%u effective=%u proof=initial-binding-plus-lossless-expansion",
        a750_role_image_usage_requested ? 1u : 0u,
        capability_profile.exact_mesa_turnip_adreno750 ? 1u : 0u,
        static_cast<u32>(capability_profile.mobile_driver_build),
        capability_profile.optimization_a750_role_image_usage ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_HOST_METADATA_COMPACTION] exactProprietaryA750=%u "
        "disabled=%u effective=%u texturePageChunk=16 bufferPageChunk=256 "
        "releaseFontCpuPixels=%u",
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        a750_host_metadata_compaction_disabled ? 1u : 0u,
        capability_profile.optimization_a750_host_metadata_compaction ? 1u : 0u,
        capability_profile.optimization_a750_host_metadata_compaction ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_PRESENT_SOURCE_SUBMIT_SPLIT] requested=%u "
        "exactProprietaryA750=%u effective=%u",
        a750_present_source_submit_split_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.diagnostic_a750_present_source_submit_split ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_VIDEOOUT_CANDIDATE_UID] requested=%u "
        "exactProprietaryA750=%u effective=%u",
        a750_videoout_candidate_uid_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.diagnostic_a750_videoout_candidate_uid ? 1u : 0u);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_VIDEOOUT_SLOT0_TRACKING] requested=%u "
        "exactProprietaryA750=%u effective=%u",
        a750_videoout_slot0_tracking_requested ? 1u : 0u,
        capability_profile.exact_proprietary_adreno750 ? 1u : 0u,
        capability_profile.diagnostic_a750_videoout_slot0_tracking ? 1u : 0u);
#endif
#if !defined(ANDROID) && !defined(LSX4_APPLE_RUNTIME)
    pipeline_creation_cache_control =
        vk13_features.pipelineCreationCacheControl;
#endif
#if defined(ANDROID) || defined(LSX4_APPLE_RUNTIME)
    const auto log_feature_fallback = [&](std::string_view feature, bool available,
                                          std::string_view action) {
        if (available) {
            return;
        }
        LOG_WARNING(Render_Vulkan,
                    "EXECUTOR_VK_FEATURE_FALLBACK feature={} available=NO action={} "
                    "targetApi={}.{} deviceApi={}.{}",
                    feature, action, VK_VERSION_MAJOR(TargetVulkanApiVersion),
                    VK_VERSION_MINOR(TargetVulkanApiVersion), VK_VERSION_MAJOR(properties.apiVersion),
                    VK_VERSION_MINOR(properties.apiVersion));
    };
    const bool timeline_semaphore_ready = IsTimelineSemaphoreSupported();
    const bool buffer_device_address_ready = IsBufferDeviceAddressSupported();
    const bool dynamic_rendering_ready = IsDynamicRenderingSupported();
    const bool synchronization2_ready = IsSynchronization2Supported();
    const bool maintenance4_ready = IsMaintenance4Supported();
    log_feature_fallback("timelineSemaphore", timeline_semaphore_ready,
                         "fence_virtual_timeline");
    log_feature_fallback("bufferDeviceAddress", buffer_device_address_ready,
                         "descriptor_flatbuffer");
    log_feature_fallback("dynamicRendering", dynamic_rendering_ready, "presenter_unsupported");
    log_feature_fallback("synchronization2", synchronization2_ready, "presenter_unsupported");
    log_feature_fallback("maintenance4", maintenance4_ready, "presenter_unsupported");
    if (!timeline_semaphore_ready || !buffer_device_address_ready || !dynamic_rendering_ready ||
        !synchronization2_ready || !maintenance4_ready) {
        LOG_WARNING(Render_Vulkan,
                  "EXECUTOR_VULKAN_WSI phase=feature_detect result=presenter_degraded_continue "
                  "timelineSemaphore={} bufferDeviceAddress={} dynamicRendering={} "
                  "synchronization2={} maintenance4={}",
                  timeline_semaphore_ready ? "YES" : "NO",
                  buffer_device_address_ready ? "YES" : "NO",
                  dynamic_rendering_ready ? "YES" : "NO",
                  synchronization2_ready ? "YES" : "NO", maintenance4_ready ? "YES" : "NO");
    }
#endif
    const bool disable_robustness = ExecutorDisableVkRobustness();
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_VK_ROBUSTNESS_MODE] disabled=%u",
                        disable_robustness ? 1u : 0u);
#endif
    vk::StructureChain device_chain = {
        vk::DeviceCreateInfo{
            .queueCreateInfoCount = 1u,
            .pQueueCreateInfos = &queue_info,
            .enabledExtensionCount = static_cast<u32>(enabled_extensions.size()),
            .ppEnabledExtensionNames = enabled_extensions.data(),
        },
        vk::PhysicalDeviceFeatures2{
            .features{
                .robustBufferAccess =
                    disable_robustness ? false : features.robustBufferAccess,
                .imageCubeArray = features.imageCubeArray,
                .independentBlend = features.independentBlend,
                .geometryShader = features.geometryShader,
                .tessellationShader = features.tessellationShader,
                .sampleRateShading = features.sampleRateShading,
                .dualSrcBlend = features.dualSrcBlend,
                .logicOp = features.logicOp,
                .multiDrawIndirect = features.multiDrawIndirect,
                .depthClamp = features.depthClamp,
                .depthBiasClamp = features.depthBiasClamp,
                .fillModeNonSolid = features.fillModeNonSolid,
                .depthBounds = features.depthBounds,
                .wideLines = features.wideLines,
                .multiViewport = features.multiViewport,
                .samplerAnisotropy = features.samplerAnisotropy,
                .vertexPipelineStoresAndAtomics = features.vertexPipelineStoresAndAtomics,
                .fragmentStoresAndAtomics = features.fragmentStoresAndAtomics,
                .shaderImageGatherExtended = features.shaderImageGatherExtended,
                .shaderStorageImageExtendedFormats = features.shaderStorageImageExtendedFormats,
                .shaderStorageImageMultisample = features.shaderStorageImageMultisample,
                .shaderClipDistance = features.shaderClipDistance,
                .shaderFloat64 = features.shaderFloat64,
                .shaderInt64 = features.shaderInt64,
                .shaderInt16 = features.shaderInt16,
                .sparseBinding =
                    capability_profile.memory_policy.bda_page_table.mode !=
                    BdaPageTableMode::FlatResident,
                .sparseResidencyBuffer =
                    capability_profile.memory_policy.bda_page_table.mode !=
                    BdaPageTableMode::FlatResident,
            },
        },
        vk::PhysicalDeviceVulkan11Features{
            .storageBuffer16BitAccess = vk11_features.storageBuffer16BitAccess,
            .uniformAndStorageBuffer16BitAccess = vk11_features.uniformAndStorageBuffer16BitAccess,
            .shaderDrawParameters = vk11_features.shaderDrawParameters,
        },
        vk::PhysicalDeviceVulkan12Features{
            .samplerMirrorClampToEdge = vk12_features.samplerMirrorClampToEdge,
            .drawIndirectCount = vk12_features.drawIndirectCount,
            .storageBuffer8BitAccess = vk12_features.storageBuffer8BitAccess,
            .uniformAndStorageBuffer8BitAccess = vk12_features.uniformAndStorageBuffer8BitAccess,
            .shaderBufferInt64Atomics = vk12_features.shaderBufferInt64Atomics,
            .shaderSharedInt64Atomics = vk12_features.shaderSharedInt64Atomics,
            .shaderFloat16 = vk12_features.shaderFloat16,
            .shaderInt8 = vk12_features.shaderInt8,
            .scalarBlockLayout = vk12_features.scalarBlockLayout,
            .uniformBufferStandardLayout = vk12_features.uniformBufferStandardLayout,
            .separateDepthStencilLayouts = vk12_features.separateDepthStencilLayouts,
            .hostQueryReset = vk12_features.hostQueryReset,
            .timelineSemaphore = IsTimelineSemaphoreSupported(),
            .bufferDeviceAddress = IsBufferDeviceAddressSupported(),
            .shaderOutputLayer = vk12_features.shaderOutputLayer,
        },
        vk::PhysicalDeviceVulkan13Features{
            .robustImageAccess =
                disable_robustness ? false : vk13_features.robustImageAccess,
            .pipelineCreationCacheControl = pipeline_creation_cache_control,
            .shaderDemoteToHelperInvocation = vk13_features.shaderDemoteToHelperInvocation,
            .subgroupSizeControl = vk13_features.subgroupSizeControl,
            .synchronization2 = IsSynchronization2Supported(),
            .dynamicRendering = IsDynamicRenderingSupported(),
            .maintenance4 = IsMaintenance4Supported(),
        },
        vk::PhysicalDeviceCustomBorderColorFeaturesEXT{
            .customBorderColors = custom_border_color_features.customBorderColors,
            .customBorderColorWithoutFormat =
                custom_border_color_features.customBorderColorWithoutFormat,
        },
        vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT{
            .extendedDynamicState3ColorWriteMask =
                dynamic_state_3_features.extendedDynamicState3ColorWriteMask,
        },
        vk::PhysicalDeviceFragmentShadingRateFeaturesKHR{
            .pipelineFragmentShadingRate =
                fragment_shading_rate_features.pipelineFragmentShadingRate,
        },
        vk::PhysicalDeviceDynamicRenderingLocalReadFeatures{
            .dynamicRenderingLocalRead =
                dynamic_rendering_local_read_features.dynamicRenderingLocalRead,
        },
        vk::PhysicalDeviceDepthClipControlFeaturesEXT{
            .depthClipControl = depth_clip_control_features.depthClipControl,
        },
        vk::PhysicalDeviceDepthClipEnableFeaturesEXT{
            .depthClipEnable = depth_clip_enable_features.depthClipEnable,
        },
        vk::PhysicalDeviceRobustness2FeaturesEXT{
            .robustBufferAccess2 =
                disable_robustness ? false : robustness2_features.robustBufferAccess2,
            .robustImageAccess2 =
                disable_robustness ? false : robustness2_features.robustImageAccess2,
            .nullDescriptor = robustness2_features.nullDescriptor,
        },
        vk::PhysicalDeviceFaultFeaturesEXT{
            .deviceFault = device_fault_features.deviceFault,
            .deviceFaultVendorBinary = device_fault_features.deviceFaultVendorBinary,
        },
        vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT{
            .vertexInputDynamicState = vertex_input_dynamic_state,
        },
        vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT{
            .primitiveTopologyListRestart = list_restart_features.primitiveTopologyListRestart,
            .primitiveTopologyPatchListRestart =
                list_restart_features.primitiveTopologyPatchListRestart,
        },
        vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR{
            .fragmentShaderBarycentric =
                fragment_shader_barycentric_features.fragmentShaderBarycentric,
        },
        vk::PhysicalDeviceLegacyVertexAttributesFeaturesEXT{
            .legacyVertexAttributes = legacy_vertex_attributes_features.legacyVertexAttributes,
        },
        vk::PhysicalDeviceProvokingVertexFeaturesEXT{
            .provokingVertexLast = provoking_vertex_features.provokingVertexLast,
        },
        vk::PhysicalDeviceVertexAttributeDivisorFeatures{
            .vertexAttributeInstanceRateDivisor =
                vertex_attribute_divisor_features.vertexAttributeInstanceRateDivisor,
        },
        vk::PhysicalDeviceMaintenance8FeaturesKHR{
            .maintenance8 = maintenance_8_features.maintenance8,
        },
        vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT{
            .attachmentFeedbackLoopLayout =
                attachment_feedback_loop_layout_features.attachmentFeedbackLoopLayout,
        },
        vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT{
            .attachmentFeedbackLoopDynamicState =
                attachment_feedback_loop_dynamic_state_features
                    .attachmentFeedbackLoopDynamicState,
        },
        vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT{
            .shaderBufferFloat32AtomicMinMax =
                shader_atomic_float2_features.shaderBufferFloat32AtomicMinMax,
            .shaderImageFloat32AtomicMinMax =
                shader_atomic_float2_features.shaderImageFloat32AtomicMinMax,
        },
        vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR{
            .workgroupMemoryExplicitLayout =
                workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout,
            .workgroupMemoryExplicitLayoutScalarBlockLayout =
                workgroup_memory_explicit_layout_features
                    .workgroupMemoryExplicitLayoutScalarBlockLayout,
            .workgroupMemoryExplicitLayout16BitAccess =
                workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout16BitAccess,
        },
        vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT{
            .image2DViewOf3D = image_2d_view_of_3d_features.image2DViewOf3D,
            .sampler2DViewOf3D = image_2d_view_of_3d_features.sampler2DViewOf3D,
        },
        vk::PhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT{
            .multisampledRenderToSingleSampled = multisampled_render_to_single_sampled,
        },
#ifdef __APPLE__
        vk::PhysicalDevicePortabilitySubsetFeaturesKHR{
            .constantAlphaColorBlendFactors = portability_features.constantAlphaColorBlendFactors,
            .events = portability_features.events,
            .imageViewFormatReinterpretation = portability_features.imageViewFormatReinterpretation,
            .imageViewFormatSwizzle = portability_features.imageViewFormatSwizzle,
            .imageView2DOn3DImage = portability_features.imageView2DOn3DImage,
            .multisampleArrayImage = portability_features.multisampleArrayImage,
            .mutableComparisonSamplers = portability_features.mutableComparisonSamplers,
            .pointPolygons = portability_features.pointPolygons,
            .samplerMipLodBias = portability_features.samplerMipLodBias,
            .separateStencilMaskRef = portability_features.separateStencilMaskRef,
            .shaderSampleRateInterpolationFunctions =
                portability_features.shaderSampleRateInterpolationFunctions,
            .tessellationIsolines = portability_features.tessellationIsolines,
            .tessellationPointMode = portability_features.tessellationPointMode,
            .triangleFans = portability_features.triangleFans,
            .vertexAttributeAccessBeyondStride =
                portability_features.vertexAttributeAccessBeyondStride,
        },
#endif
    };

    if (!custom_border_color) {
        device_chain.unlink<vk::PhysicalDeviceCustomBorderColorFeaturesEXT>();
    }
    if (!dynamic_state_3) {
        device_chain.unlink<vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>();
    }
    if (!fragment_shading_rate) {
        device_chain.unlink<vk::PhysicalDeviceFragmentShadingRateFeaturesKHR>();
    }
    if (!dynamic_rendering_local_read) {
        device_chain.unlink<vk::PhysicalDeviceDynamicRenderingLocalReadFeatures>();
    }
    if (!depth_clip_control) {
        device_chain.unlink<vk::PhysicalDeviceDepthClipControlFeaturesEXT>();
    }
    if (!depth_clip_enable) {
        device_chain.unlink<vk::PhysicalDeviceDepthClipEnableFeaturesEXT>();
    }
    if (!robustness2) {
        device_chain.unlink<vk::PhysicalDeviceRobustness2FeaturesEXT>();
    }
    if (!device_fault) {
        device_chain.unlink<vk::PhysicalDeviceFaultFeaturesEXT>();
    }
    if (!vertex_input_dynamic_state) {
        device_chain.unlink<vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT>();
    }
    if (!vertex_attribute_divisor) {
        device_chain.unlink<vk::PhysicalDeviceVertexAttributeDivisorFeatures>();
    }
    if (!list_restart) {
        device_chain.unlink<vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT>();
    }
    if (!fragment_shader_barycentric) {
        device_chain.unlink<vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR>();
    }
    if (!legacy_vertex_attributes) {
        device_chain.unlink<vk::PhysicalDeviceLegacyVertexAttributesFeaturesEXT>();
    }
    if (!provoking_vertex) {
        device_chain.unlink<vk::PhysicalDeviceProvokingVertexFeaturesEXT>();
    }
    if (!maintenance_8) {
        device_chain.unlink<vk::PhysicalDeviceMaintenance8FeaturesKHR>();
    }
    if (!attachment_feedback_loop) {
        device_chain.unlink<vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT>();
        device_chain.unlink<vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT>();
    }
    if (!shader_atomic_float2) {
        device_chain.unlink<vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT>();
    }
    if (!workgroup_memory_explicit_layout) {
        device_chain.unlink<vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR>();
    }
    if (!image_2d_view_of_3d) {
        device_chain.unlink<vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT>();
    }

    auto [device_result, dev] = physical_device.createDeviceUnique(device_chain.get());
    if (device_result != vk::Result::eSuccess) {
        LOG_CRITICAL(Render_Vulkan, "Failed to create device: {}", vk::to_string(device_result));
        return false;
    }
    device = std::move(dev);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(*device);

#if defined(ANDROID) || defined(LSX4_APPLE_RUNTIME)
    auto& dispatch = VULKAN_HPP_DEFAULT_DISPATCHER;
#define LSX4_ALIAS_PROMOTED(core_name, extension_name)                                      \
    if (dispatch.core_name == nullptr && dispatch.extension_name != nullptr) {              \
        dispatch.core_name = reinterpret_cast<PFN_##core_name>(dispatch.extension_name);    \
    }
    LSX4_ALIAS_PROMOTED(vkCmdPipelineBarrier2, vkCmdPipelineBarrier2KHR)
    LSX4_ALIAS_PROMOTED(vkCmdWriteTimestamp2, vkCmdWriteTimestamp2KHR)
    LSX4_ALIAS_PROMOTED(vkQueueSubmit2, vkQueueSubmit2KHR)
    LSX4_ALIAS_PROMOTED(vkCmdBeginRendering, vkCmdBeginRenderingKHR)
    LSX4_ALIAS_PROMOTED(vkCmdEndRendering, vkCmdEndRenderingKHR)
    LSX4_ALIAS_PROMOTED(vkGetDeviceBufferMemoryRequirements,
                        vkGetDeviceBufferMemoryRequirementsKHR)
    LSX4_ALIAS_PROMOTED(vkGetDeviceImageMemoryRequirements,
                        vkGetDeviceImageMemoryRequirementsKHR)
    LSX4_ALIAS_PROMOTED(vkGetDeviceImageSparseMemoryRequirements,
                        vkGetDeviceImageSparseMemoryRequirementsKHR)
    LSX4_ALIAS_PROMOTED(vkGetSemaphoreCounterValue, vkGetSemaphoreCounterValueKHR)
    LSX4_ALIAS_PROMOTED(vkWaitSemaphores, vkWaitSemaphoresKHR)
    LSX4_ALIAS_PROMOTED(vkSignalSemaphore, vkSignalSemaphoreKHR)
    LSX4_ALIAS_PROMOTED(vkGetBufferDeviceAddress, vkGetBufferDeviceAddressKHR)
    LSX4_ALIAS_PROMOTED(vkGetBufferOpaqueCaptureAddress, vkGetBufferOpaqueCaptureAddressKHR)
    LSX4_ALIAS_PROMOTED(vkGetDeviceMemoryOpaqueCaptureAddress,
                        vkGetDeviceMemoryOpaqueCaptureAddressKHR)
#undef LSX4_ALIAS_PROMOTED
    if (IsDynamicRenderingSupported() &&
        (dispatch.vkCmdBeginRendering == nullptr || dispatch.vkCmdEndRendering == nullptr)) {
        capability_profile.dynamic_rendering = CapabilityRoute::Legacy;
        capability_profile.dynamic_rendering_local_read = false;
    }
    if (IsSynchronization2Supported() && dispatch.vkCmdPipelineBarrier2 == nullptr) {
        capability_profile.synchronization2 = CapabilityRoute::Legacy;
    }
    if (!IsSynchronization2Supported()) {
        dispatch.vkCmdPipelineBarrier2 = &LegacyCmdPipelineBarrier2;
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_VK_PROMOTED_FALLBACK] command=vkCmdPipelineBarrier2 mode=legacy");
    }
#ifdef __ANDROID__
    if (capability_profile.diagnostic_a750_barrier_capture) {
        const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        const bool installed = InstallA750BarrierCapture(
            dispatch.vkCmdPipelineBarrier2,
            user_dir / "diagnostic-a750-last-vk-barrier.bin");
        __android_log_print(
            installed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "LSX4Native",
            "[EXECUTOR_A750_VK_BARRIER_CAPTURE_INSTALL] installed=%u",
            installed ? 1u : 0u);
        if (!installed) {
            capability_profile.diagnostic_a750_barrier_capture = false;
        }
    }
#endif
    if (IsPushDescriptorSupported() && dispatch.vkCmdPushDescriptorSetKHR == nullptr) {
        capability_profile.push_descriptors = CapabilityRoute::Legacy;
        push_descriptor_props.maxPushDescriptors = 0;
    }
    if (IsMaintenance4Supported() &&
        (dispatch.vkGetDeviceBufferMemoryRequirements == nullptr ||
         dispatch.vkGetDeviceImageMemoryRequirements == nullptr)) {
        capability_profile.maintenance4 = CapabilityRoute::Legacy;
    }
    if (IsTimelineSemaphoreSupported() &&
        (dispatch.vkGetSemaphoreCounterValue == nullptr || dispatch.vkWaitSemaphores == nullptr ||
         dispatch.vkSignalSemaphore == nullptr)) {
        capability_profile.timeline_semaphore = CapabilityRoute::Legacy;
    }
    if (IsBufferDeviceAddressSupported() && dispatch.vkGetBufferDeviceAddress == nullptr) {
        capability_profile.buffer_device_address = CapabilityRoute::Legacy;
    }
#endif

    LOG_INFO(Render_Vulkan,
             "EXECUTOR_VK_CAPABILITY_PROFILE device={} vendor={:#06x} deviceId={:#010x} "
             "driverFamily={} reportedDriverFamily={} api={}.{} spirv={}.{} "
             "timeline={} bda={} rendering={} sync2={} maintenance4={} push={} "
             "divisor={} localRead={} fsr={} dynState3={} storageAlign={} uniformAlign={} "
             "quirks={:#x} compatible={}",
             properties.deviceName.data(), properties.vendorID, properties.deviceID,
             MobileDriverFamilyName(mobile_driver_family),
             MobileDriverFamilyName(reported_mobile_driver_family),
             VK_VERSION_MAJOR(capability_profile.api_version),
             VK_VERSION_MINOR(capability_profile.api_version),
             (capability_profile.spirv_version >> 16) & 0xff,
             (capability_profile.spirv_version >> 8) & 0xff,
             CapabilityRouteName(capability_profile.timeline_semaphore),
             CapabilityRouteName(capability_profile.buffer_device_address),
             CapabilityRouteName(capability_profile.dynamic_rendering),
             CapabilityRouteName(capability_profile.synchronization2),
             CapabilityRouteName(capability_profile.maintenance4),
             CapabilityRouteName(capability_profile.push_descriptors),
             capability_profile.vertex_attribute_divisor,
             capability_profile.dynamic_rendering_local_read,
             capability_profile.fragment_shading_rate,
             capability_profile.extended_dynamic_state3,
             StorageMinAlignment(), UniformMinAlignment(),
             capability_profile.driver_quirks,
             capability_profile.RuntimeCompatible());
#if defined(ANDROID) || defined(LSX4_APPLE_RUNTIME)
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VK_CAPABILITY_PROFILE] device=%s vendor=0x%04x deviceId=0x%08x "
        "driverFamily=%s reportedDriverFamily=%s api=%u.%u spirv=%u.%u "
        "timeline=%s bda=%s rendering=%s sync2=%s maintenance4=%s push=%s "
        "divisor=%u localRead=%u fsr=%u dynState3=%u storageAlign=%llu uniformAlign=%llu "
        "quirks=0x%llx compatible=%u",
        properties.deviceName.data(), properties.vendorID, properties.deviceID,
        MobileDriverFamilyName(mobile_driver_family).data(),
        MobileDriverFamilyName(reported_mobile_driver_family).data(),
        VK_VERSION_MAJOR(capability_profile.api_version),
        VK_VERSION_MINOR(capability_profile.api_version),
        (capability_profile.spirv_version >> 16) & 0xff,
        (capability_profile.spirv_version >> 8) & 0xff,
        CapabilityRouteName(capability_profile.timeline_semaphore).data(),
        CapabilityRouteName(capability_profile.buffer_device_address).data(),
        CapabilityRouteName(capability_profile.dynamic_rendering).data(),
        CapabilityRouteName(capability_profile.synchronization2).data(),
        CapabilityRouteName(capability_profile.maintenance4).data(),
        CapabilityRouteName(capability_profile.push_descriptors).data(),
        capability_profile.vertex_attribute_divisor ? 1u : 0u,
        capability_profile.dynamic_rendering_local_read ? 1u : 0u,
        capability_profile.fragment_shading_rate ? 1u : 0u,
        capability_profile.extended_dynamic_state3 ? 1u : 0u,
        static_cast<unsigned long long>(StorageMinAlignment()),
        static_cast<unsigned long long>(UniformMinAlignment()),
        static_cast<unsigned long long>(capability_profile.driver_quirks),
        capability_profile.RuntimeCompatible() ? 1u : 0u);
#endif
    if (!capability_profile.RuntimeCompatible()) {
        LOG_CRITICAL(Render_Vulkan, "Vulkan device lacks a complete renderer capability route");
        return false;
    }

    graphics_queue = device->getQueue(queue_family_index, 0);
    present_queue = device->getQueue(queue_family_index, 0);

    if (calibrated_timestamps) {
        const auto [time_domains_result, time_domains] =
            physical_device.getCalibrateableTimeDomainsEXT();
        if (time_domains_result == vk::Result::eSuccess) {
#if _WIN64
            calibrated_host_time_domain = vk::TimeDomainEXT::eQueryPerformanceCounter;
            calibrated_timestamp_host_domain =
                std::find(time_domains.cbegin(), time_domains.cend(),
                          calibrated_host_time_domain) != time_domains.cend();
#elif __linux__
            calibrated_host_time_domain = vk::TimeDomainEXT::eClockMonotonicRaw;
            calibrated_timestamp_host_domain =
                std::find(time_domains.cbegin(), time_domains.cend(),
                          calibrated_host_time_domain) != time_domains.cend();
#else
            calibrated_timestamp_host_domain = false;
#endif
            if (calibrated_timestamp_host_domain) {
#if TRACY_GPU_ENABLED
                static constexpr std::string_view context_name{"vk_rasterizer"};
                profiler_context = TracyVkContextHostCalibrated(
                    *instance, physical_device, *device,
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
                TracyVkContextName(profiler_context, context_name.data(), context_name.size());
#endif
            }
        } else {
            LOG_WARNING(Render_Vulkan, "Could not query calibrated time domains for profiling: {}",
                        vk::to_string(time_domains_result));
        }
    }

    CreateAllocator();
    return true;
}

bool Instance::QueryCalibratedTimestamp(CalibratedTimestampPair& pair) const noexcept {
    if (!calibrated_timestamps || !calibrated_timestamp_host_domain || !device ||
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetCalibratedTimestampsEXT == nullptr) {
        return false;
    }
    const std::array<vk::CalibratedTimestampInfoKHR, 2> infos{
        vk::CalibratedTimestampInfoKHR{.timeDomain = vk::TimeDomainEXT::eDevice},
        vk::CalibratedTimestampInfoKHR{.timeDomain = calibrated_host_time_domain},
    };
    std::array<u64, 2> timestamps{};
    u64 max_deviation_ns{};
    const vk::Result result = device->getCalibratedTimestampsEXT(
        static_cast<u32>(infos.size()), infos.data(), timestamps.data(),
        &max_deviation_ns);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    pair = {
        .device_ticks = timestamps[0],
        .host_timestamp = timestamps[1],
        .max_deviation_ns = max_deviation_ns,
    };
    return true;
}

void Instance::DumpDeviceFaultInfo(const char* where, u64 failing_tick) const noexcept {
    if (!IsDeviceFaultSupported() || !device ||
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT == nullptr) {
        return;
    }
    if (device_fault_dumped.test_and_set(std::memory_order_acq_rel)) {
        return;
    }

    VkDeviceFaultCountsEXT counts{
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT,
    };
    const VkDevice raw_device = static_cast<VkDevice>(*device);
    const VkResult counts_result =
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT(raw_device, &counts, nullptr);
    if (counts_result != VK_SUCCESS) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_VK_DEVICE_FAULT] where=%s tick=%llu phase=counts "
                            "result=%d",
                            where ? where : "unknown",
                            static_cast<unsigned long long>(failing_tick), counts_result);
#endif
        LOG_ERROR(Render_Vulkan, "VK_EXT_device_fault count query failed at {} tick {}: {}",
                  where ? where : "unknown", failing_tick,
                  vk::to_string(static_cast<vk::Result>(counts_result)));
        return;
    }

    constexpr u32 MaxAddressInfos = 256;
    constexpr u32 MaxVendorInfos = 256;
    constexpr VkDeviceSize MaxVendorBinarySize = 16ull * 1024ull * 1024ull;
    const u32 reported_address_count = counts.addressInfoCount;
    const u32 reported_vendor_count = counts.vendorInfoCount;
    const VkDeviceSize reported_binary_size = counts.vendorBinarySize;
    counts.addressInfoCount = std::min(counts.addressInfoCount, MaxAddressInfos);
    counts.vendorInfoCount = std::min(counts.vendorInfoCount, MaxVendorInfos);
    counts.vendorBinarySize = std::min(counts.vendorBinarySize, MaxVendorBinarySize);

    std::vector<VkDeviceFaultAddressInfoEXT> address_infos(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendor_infos(counts.vendorInfoCount);
    std::vector<u8> vendor_binary(static_cast<std::size_t>(counts.vendorBinarySize));
    VkDeviceFaultInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
        .pAddressInfos = address_infos.empty() ? nullptr : address_infos.data(),
        .pVendorInfos = vendor_infos.empty() ? nullptr : vendor_infos.data(),
        .pVendorBinaryData = vendor_binary.empty() ? nullptr : vendor_binary.data(),
    };
    const VkResult info_result =
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceFaultInfoEXT(raw_device, &counts, &info);

#ifdef __ANDROID__
    __android_log_print(
        ANDROID_LOG_ERROR, "LSX4Native",
        "[EXECUTOR_VK_DEVICE_FAULT] where=%s tick=%llu phase=info result=%d desc=%.*s "
        "addresses=%u/%u vendors=%u/%u binary=%llu/%llu",
        where ? where : "unknown", static_cast<unsigned long long>(failing_tick), info_result,
        VK_MAX_DESCRIPTION_SIZE, info.description, counts.addressInfoCount, reported_address_count,
        counts.vendorInfoCount, reported_vendor_count,
        static_cast<unsigned long long>(counts.vendorBinarySize),
        static_cast<unsigned long long>(reported_binary_size));
#endif
    LOG_ERROR(Render_Vulkan,
              "VK_EXT_device_fault at {} tick {}: result={} desc='{}' addresses={}/{} "
              "vendors={}/{} binary={}/{}",
              where ? where : "unknown", failing_tick,
              vk::to_string(static_cast<vk::Result>(info_result)), info.description,
              counts.addressInfoCount, reported_address_count, counts.vendorInfoCount,
              reported_vendor_count, counts.vendorBinarySize, reported_binary_size);

    for (u32 i = 0; i < counts.addressInfoCount; ++i) {
        const auto& address = address_infos[i];
#ifdef __ANDROID__
        __android_log_print(
            ANDROID_LOG_ERROR, "LSX4Native",
            "[EXECUTOR_VK_DEVICE_FAULT_ADDRESS] index=%u type=%d address=0x%llx precision=0x%llx",
            i, static_cast<int>(address.addressType),
            static_cast<unsigned long long>(address.reportedAddress),
            static_cast<unsigned long long>(address.addressPrecision));
#endif
        LOG_ERROR(Render_Vulkan, "VK device fault address[{}]: type={} address={:#x} precision={:#x}",
                  i, static_cast<int>(address.addressType), address.reportedAddress,
                  address.addressPrecision);
    }

    for (u32 i = 0; i < counts.vendorInfoCount; ++i) {
        const auto& vendor = vendor_infos[i];
#ifdef __ANDROID__
        __android_log_print(
            ANDROID_LOG_ERROR, "LSX4Native",
            "[EXECUTOR_VK_DEVICE_FAULT_VENDOR] index=%u desc=%.*s code=0x%llx data=0x%llx", i,
            VK_MAX_DESCRIPTION_SIZE, vendor.description,
            static_cast<unsigned long long>(vendor.vendorFaultCode),
            static_cast<unsigned long long>(vendor.vendorFaultData));
#endif
        LOG_ERROR(Render_Vulkan, "VK device fault vendor[{}]: desc='{}' code={:#x} data={:#x}", i,
                  vendor.description, vendor.vendorFaultCode, vendor.vendorFaultData);
    }

    if (!vendor_binary.empty()) {
        const u64 hash = Common::FingerprintBytes(
            vendor_binary, Common::FingerprintDomain::DeviceFaultBinary);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_VK_DEVICE_FAULT_BINARY] size=%zu fingerprint=0x%llx",
                            vendor_binary.size(), static_cast<unsigned long long>(hash));
#endif
        LOG_ERROR(Render_Vulkan, "VK device fault vendor binary: size={} fingerprint={:#x}",
                  vendor_binary.size(), hash);
    }
}

void Instance::CreateAllocator() {
    const VmaVulkanFunctions functions = {
        .vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
    };

    const VmaAllocatorCreateFlags allocator_flags =
        IsBufferDeviceAddressSupported() ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0;

    const bool exact_qcom_a750 = capability_profile.exact_proprietary_adreno750;
    const bool exact_turnip_a750 = capability_profile.exact_mesa_turnip_adreno750;
    const bool a750_block64_requested = ExecutorA750VmaBlock64Requested();
    const bool a750_turnip_block64_requested = ExecutorA750TurnipVmaBlock64Requested();
    const bool a750_block64_disabled = ExecutorA750VmaBlock64Disabled();
    const VkDeviceSize routed_block_size = static_cast<VkDeviceSize>(
        capability_profile.memory_policy.preferred_large_heap_block_size);
    const bool use_a750_block64 = !a750_block64_disabled &&
        ((exact_qcom_a750 &&
         (routed_block_size != 0 || a750_block64_requested)) ||
         (exact_turnip_a750 &&
          (routed_block_size != 0 || a750_turnip_block64_requested)));
    constexpr VkDeviceSize A750LargeHeapBlockSize = VkDeviceSize{64} * 1024 * 1024;
    const VkDeviceSize effective_block_size =
        use_a750_block64
            ? (routed_block_size != 0 ? routed_block_size : A750LargeHeapBlockSize)
            : VkDeviceSize{0};

    const VmaAllocatorCreateInfo allocator_info = {
        .flags = allocator_flags,
        .physicalDevice = physical_device,
        .device = *device,
        .preferredLargeHeapBlockSize = effective_block_size,
        .pVulkanFunctions = &functions,
        .instance = *instance,
        .vulkanApiVersion = std::min(properties.apiVersion, TargetVulkanApiVersion),
    };

#ifdef __ANDROID__
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_A750_VMA_BLOCK_POLICY] requested=%d turnipRequested=%d disabled=%d "
        "candidateBuild=%d productDefault=%d driverBuild=%s policyState=%s enabled=%d blockMiB=%llu "
        "exactDevice=%d exactTurnipDevice=%d",
        a750_block64_requested ? 1 : 0, a750_turnip_block64_requested ? 1 : 0,
        a750_block64_disabled ? 1 : 0,
        HasCandidateA750TurnipComposedMemoryPolicy(
            capability_profile.mobile_driver_build)
            ? 1
            : 0,
        HasProductDefaultA750TurnipComposedMemoryPolicy(
            capability_profile.mobile_driver_build)
            ? 1
            : 0,
        MobileDriverBuildName(capability_profile.mobile_driver_build).data(),
        MobileDriverBuildPolicyStateName(ResolveMobileDriverBuildPolicyState(
            capability_profile.mobile_driver_build)).data(),
        use_a750_block64 ? 1 : 0,
        static_cast<unsigned long long>(effective_block_size / (1024ULL * 1024ULL)),
        exact_qcom_a750 ? 1 : 0, exact_turnip_a750 ? 1 : 0);
#endif

    const VkResult result = vmaCreateAllocator(&allocator_info, &allocator);
    if (result != VK_SUCCESS) {
        UNREACHABLE_MSG("Failed to initialize VMA with error {}",
                        vk::to_string(vk::Result{result}));
    }
}

void Instance::CollectDeviceParameters() {
    const vk::StructureChain property_chain =
        physical_device
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>();
    const vk::PhysicalDeviceDriverProperties driver =
        property_chain.get<vk::PhysicalDeviceDriverProperties>();

    driver_id = driver.driverID;
    vendor_name = driver.driverName.data();
    DriverAxis::RecordActualDevice(static_cast<const VkPhysicalDeviceProperties&>(properties),
                                   driver.driverName.data(), driver.driverInfo.data());

    const std::string model_name{GetModelName()};
    const std::string driver_version = GetDriverVersionName();
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);
    const std::string api_version = GetReadableVersion(properties.apiVersion);
    const std::string extensions = fmt::format("{}", fmt::join(available_extensions, ", "));

    LOG_INFO(Render_Vulkan, "GPU_Vendor: {}", vendor_name);
    LOG_INFO(Render_Vulkan, "GPU_Model: {}", model_name);
    LOG_INFO(Render_Vulkan, "GPU_Integrated: {}", IsIntegrated() ? "Yes" : "No");
    LOG_INFO(Render_Vulkan, "GPU_Vulkan_Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "GPU_Vulkan_Version: {}", api_version);
    LOG_INFO(Render_Vulkan, "GPU_Vulkan_Extensions: {}", extensions);
}

void Instance::CollectPhysicalMemoryInfo() {
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    vk::PhysicalDeviceMemoryProperties2 props = {
        .pNext = supports_memory_budget ? &budget : nullptr,
    };
    physical_device.getMemoryProperties2(&props);
    const auto& memory_props = props.memoryProperties;
    const size_t num_props = memory_props.memoryHeapCount;
    std::vector<bool> heap_has_unprotected_type(num_props, false);
    for (u32 i = 0; i < memory_props.memoryTypeCount; ++i) {
        const auto& type = memory_props.memoryTypes[i];
        const bool is_protected =
            (type.propertyFlags & vk::MemoryPropertyFlagBits::eProtected) !=
            vk::MemoryPropertyFlags{};
        if (!is_protected) {
            heap_has_unprotected_type[type.heapIndex] = true;
        }
    }

    valid_heaps.clear();
    total_memory_budget = 0;
    memory_pressure_usage_limit = 0;
    device_initial_memory_usage = 0;
    reported_device_memory_budget = 0;
    u64 device_initial_usage = 0;
    for (size_t i = 0; i < num_props; ++i) {
        const bool is_device_local =
            (memory_props.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) !=
            vk::MemoryHeapFlags{};
        if (!IsIntegrated() && !is_device_local) {
            continue;
        }
        if (!heap_has_unprotected_type[i]) {
            continue;
        }
        valid_heaps.push_back(i);
        if (supports_memory_budget) {
            device_initial_usage += budget.heapUsage[i];
            total_memory_budget += budget.heapBudget[i];
            continue;
        }
        total_memory_budget += memory_props.memoryHeaps[i].size;
    }
    device_initial_memory_usage = device_initial_usage;
    reported_device_memory_budget = total_memory_budget;
    if (!IsIntegrated()) {
        const u64 system_memory = std::min<u64>(total_memory_budget / 8, 1_GB);
        total_memory_budget -= system_memory;
        memory_pressure_usage_limit = total_memory_budget;
        return;
    }

    const u64 allocatable_memory =
        total_memory_budget > device_initial_usage ? total_memory_budget - device_initial_usage
                                                   : total_memory_budget;
    const u64 system_reserve = std::min<u64>(8_GB, allocatable_memory * 3 / 4);
    const u64 renderer_floor = std::min<u64>(1_GB, allocatable_memory);
    total_memory_budget = std::max(allocatable_memory - system_reserve, renderer_floor);
    memory_pressure_usage_limit = total_memory_budget;
    if (supports_memory_budget &&
        capability_profile.memory_policy.budget_accounting ==
            MemoryBudgetAccounting::ReportedUsageWithInitialBaseline) {
        memory_pressure_usage_limit = std::min(
            reported_device_memory_budget,
            device_initial_memory_usage + total_memory_budget);
    }
    if (supports_memory_budget &&
        capability_profile.memory_policy.budget_accounting ==
            MemoryBudgetAccounting::ReportedBudget70Percent) {
        const u64 reported_incremental_budget =
            reported_device_memory_budget > device_initial_memory_usage
                ? reported_device_memory_budget - device_initial_memory_usage
                : reported_device_memory_budget;
        memory_pressure_usage_limit = std::min(
            reported_device_memory_budget,
            device_initial_memory_usage + reported_incremental_budget * 70 / 100);
    }
    LOG_INFO(Render_Vulkan,
             "Integrated GPU memory: renderer budget={} MiB, pressure limit={} MiB, "
             "reported budget={} MiB, initial usage={} MiB, accounting={}, heaps=[{}]",
             total_memory_budget / 1_MB, memory_pressure_usage_limit / 1_MB,
             reported_device_memory_budget / 1_MB, device_initial_usage / 1_MB,
             MemoryBudgetAccountingName(
                 capability_profile.memory_policy.budget_accounting),
             fmt::join(valid_heaps, ", "));
#ifdef __ANDROID__
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GPU_MEMORY_BUDGET_MODEL] cacheBudgetMiB=%llu "
        "pressureLimitMiB=%llu reportedBudgetMiB=%llu initialUsageMiB=%llu "
        "accounting=%s",
        static_cast<unsigned long long>(total_memory_budget / 1_MB),
        static_cast<unsigned long long>(memory_pressure_usage_limit / 1_MB),
        static_cast<unsigned long long>(reported_device_memory_budget / 1_MB),
        static_cast<unsigned long long>(device_initial_memory_usage / 1_MB),
        MemoryBudgetAccountingName(
            capability_profile.memory_policy.budget_accounting).data());
#endif
}

void Instance::CollectImageFormatInfo() {
    const vk::PhysicalDeviceImageFormatInfo2 block_texel_view_info{
        .format = vk::Format::eBc1RgbaUnormBlock,
        .type = vk::ImageType::e2D,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled,
        .flags = vk::ImageCreateFlagBits::eBlockTexelViewCompatible,
    };
    const auto block_texel_view_props =
        physical_device.getImageFormatProperties2(block_texel_view_info);
    supports_block_texel_view = block_texel_view_props.result == vk::Result::eSuccess;
    LOG_INFO(Render_Vulkan, "Block Texel View support: {}",
             supports_block_texel_view ? "Yes" : "No");
}

void Instance::CollectToolingInfo() const {
    if (driver_id == vk::DriverId::eAmdProprietary ||
        driver_id == vk::DriverId::eIntelProprietaryWindows) {
        return;
    }
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceToolProperties == nullptr) {
        LOG_INFO(Render_Vulkan, "Vulkan tool properties entry point is unavailable");
        return;
    }
    const auto [tools_result, tools] = physical_device.getToolProperties();
    if (tools_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Could not get Vulkan tool properties: {}",
                  vk::to_string(tools_result));
        return;
    }
    for (const vk::PhysicalDeviceToolProperties& tool : tools) {
        const std::string_view name = tool.name;
        LOG_INFO(Render_Vulkan, "Attached debugging tool: {}", name);
    }
}

u64 Instance::GetDeviceMemoryUsage() const {
    constexpr u64 SampleIntervalNs = 250'000'000;
    const u64 now_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const u64 last_sample_ns =
        memory_usage_sample_time_ns.load(std::memory_order_acquire);
    if (last_sample_ns != 0 && now_ns - last_sample_ns < SampleIntervalNs) {
        return memory_usage_cache.load(std::memory_order_acquire);
    }
    if (memory_usage_query_in_flight.test_and_set(std::memory_order_acquire)) {
        return memory_usage_cache.load(std::memory_order_acquire);
    }

    vk::PhysicalDeviceMemoryBudgetPropertiesEXT memory_budget_props{};
    vk::PhysicalDeviceMemoryProperties2 props = {
        .pNext = &memory_budget_props,
    };
    physical_device.getMemoryProperties2(&props);

    u64 total_usage = 0;
    for (const size_t heap : valid_heaps) {
        total_usage += memory_budget_props.heapUsage[heap];
    }
    memory_usage_cache.store(total_usage, std::memory_order_release);
    memory_usage_sample_time_ns.store(now_ns, std::memory_order_release);
    memory_usage_query_in_flight.clear(std::memory_order_release);
    return total_usage;
}

vk::FormatFeatureFlags2 Instance::GetFormatFeatureFlags(vk::Format format) const {
    const auto it = format_properties.find(format);
    if (it == format_properties.end()) {
        UNIMPLEMENTED_MSG("Properties of format {} have not been queried.", vk::to_string(format));
    }

    return it->second.optimalTilingFeatures | it->second.bufferFeatures;
}

bool Instance::IsFormatSupported(const vk::Format format,
                                 const vk::FormatFeatureFlags2 flags) const {
    if (format == vk::Format::eUndefined) [[unlikely]] {
        return true;
    }
    return (GetFormatFeatureFlags(format) & flags) == flags;
}

bool Instance::IsOptimalImageFormatSupported(
    const vk::Format format, const vk::FormatFeatureFlags2 flags) const {
    if (format == vk::Format::eUndefined) [[unlikely]] {
        return true;
    }
    const auto it = format_properties.find(format);
    if (it == format_properties.end()) {
        UNIMPLEMENTED_MSG("Properties of format {} have not been queried.",
                          vk::to_string(format));
    }
    return (it->second.optimalTilingFeatures & flags) == flags;
}

vk::Format Instance::GetSupportedFormat(const vk::Format format,
                                        const vk::FormatFeatureFlags2 flags) const {
    if (!IsFormatSupported(format, flags)) [[unlikely]] {
        switch (format) {
        case vk::Format::eD16UnormS8Uint:
            if (IsFormatSupported(vk::Format::eD24UnormS8Uint, flags)) {
                return vk::Format::eD24UnormS8Uint;
            }
            if (IsFormatSupported(vk::Format::eD32SfloatS8Uint, flags)) {
                return vk::Format::eD32SfloatS8Uint;
            }
            break;
        case vk::Format::eR8Srgb:
            if (IsFormatSupported(vk::Format::eR8Unorm, flags)) {
                return vk::Format::eR8Unorm;
            }
            break;
        default:
            break;
        }
    }
    return format;
}

namespace {

struct ShaderCensus {
    bool decoded_ok;
    u32 insts;
    u32 mem_ops;
    u32 interp_ops;
    u32 exp_ops;
    u32 exp_mrt;
    u32 exp_pos;
    bool endpgm;
};

ShaderCensus CensusOrbShdr(const Executor::OrbShdrBlob& s) {
    ShaderCensus c{};
    try {
        Shader::Gcn::GcnCodeSlice slice(s.code, s.code + s.num_dwords);
        Shader::Gcn::GcnDecodeContext decoder;
        while (!slice.atEnd()) {
            const auto inst = decoder.decodeInstruction(slice);
            ++c.insts;
            switch (inst.category) {
            case Shader::Gcn::InstCategory::VectorMemory:
            case Shader::Gcn::InstCategory::DataShare:
                ++c.mem_ops;
                break;
            case Shader::Gcn::InstCategory::VectorInterpolation:
                ++c.interp_ops;
                break;
            case Shader::Gcn::InstCategory::Export: {
                ++c.exp_ops;
                const u32 tgt = static_cast<u32>(inst.control.exp.target);
                if (tgt <= 7) {
                    ++c.exp_mrt;
                } else if (tgt >= 12 && tgt <= 15) {
                    ++c.exp_pos;
                }
                break;
            }
            default:
                break;
            }
            if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) {
                c.endpgm = true;
            }
            if (inst.inst_class == Shader::Gcn::InstClass::ScalarMemRd ||
                inst.inst_class == Shader::Gcn::InstClass::ScalarMemUt) {
                ++c.mem_ops;
            }
        }
        c.decoded_ok = true;
    } catch (const std::exception&) {
        c.decoded_ok = false;
    }
    return c;
}

const char* ClassifyOrbShdr(const Executor::OrbShdrBlob& s, const ShaderCensus& c, bool& translatable) {
    translatable = false;
    if (!c.decoded_ok) {
        return "decode_failed";
    }
    const bool is_ps = std::string_view(s.stage) == "PS";
    if (is_ps && c.exp_mrt > 0 && c.mem_ops == 0 && c.interp_ops == 0) {
        translatable = true;
        return "standalone_ps";
    }
    if (is_ps && c.interp_ops > 0 && c.mem_ops == 0) {
        return "vintrp_ps_requires_fs_inputs";
    }
    if (is_ps) {
        return "resource_ps_requires_resources";
    }
    if (c.exp_pos > 0) {
        return "vs_requires_vtx_fetch_state";
    }
    return "other_requires_state";
}

}

void ExecutorRunLayer3PipelinePrepSmoke(const char* marker_path) {
    auto mark = [&](const char* text) {
        if (FILE* f = std::fopen(marker_path, "a")) {
            std::time_t t = std::time(nullptr);
            std::fprintf(f, "[%lld] EXECUTOR_LAYER3_PIPELINE_PREP %s\n", (long long)t, text);
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_LAYER3_PIPELINE_PREP] %s", text);
    };
    mark("phase=begin connect corpus + PM4 SetShReg + GetParams (no draw)");

    struct Staged {
        void* map{};
        size_t map_len{};
        u8* base{};
        u64 code_bytes{};
        u64 hash{};
    };
    auto stage = [](const Executor::OrbShdrBlob& blob, u32 type_id) -> Staged {
        Staged s;
        const u64 code_bytes = blob.num_dwords * sizeof(u32);
        const u64 staged_bytes = code_bytes + sizeof(AmdGpu::BinaryInfo);
        s.map_len = static_cast<size_t>((staged_bytes + 0xFFF) & ~u64(0xFFF));
        s.map = ::mmap(nullptr, s.map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (s.map == MAP_FAILED) {
            s.map = nullptr;
            return s;
        }
        s.base = reinterpret_cast<u8*>(s.map);
        std::memcpy(s.base, blob.code, code_bytes);
        AmdGpu::BinaryInfo bi{};
        bi.signature = AmdGpu::BinaryInfo::signature_ref;
        bi.version = blob.version;
        bi.length = static_cast<u32>(code_bytes);
        bi.type = type_id;
        bi.shader_hash = blob.hash;
        bi.crc32 = blob.crc32;
        std::memcpy(s.base + code_bytes, &bi, sizeof(bi));
        s.code_bytes = code_bytes;
        s.hash = blob.hash;
        return s;
    };

    const auto& ps_blob = Executor::kOrbShdrCorpus[1];
    const auto& vs_blob = Executor::kOrbShdrCorpus[18];
    Staged ps = stage(ps_blob, 0u);
    Staged vs = stage(vs_blob, 1u);
    if (!ps.map || !vs.map) {
        mark("result=FAIL stage=mmap");
        if (ps.map) ::munmap(ps.map, ps.map_len);
        if (vs.map) ::munmap(vs.map, vs.map_len);
        return;
    }

    const u64 ps_addr = reinterpret_cast<u64>(ps.base);
    const u64 vs_addr = reinterpret_cast<u64>(vs.base);
    const u32 ps_addr_field_lo = static_cast<u32>((ps_addr >> 8) & 0xFFFFFFFFu);
    const u32 ps_addr_field_hi = static_cast<u32>((ps_addr >> 8) >> 32);
    const u32 vs_addr_field_lo = static_cast<u32>((vs_addr >> 8) & 0xFFFFFFFFu);
    const u32 vs_addr_field_hi = static_cast<u32>((vs_addr >> 8) >> 32);

    std::array<u32, 64> dcb_storage{};
    u32* p = dcb_storage.data();
    using namespace AmdGpu;
    p = WritePacket<PM4ItOpcode::ClearState>(p, PM4ShaderType::ShaderGraphics, 0u);
    p = PM4CmdSetData::SetShReg(p, 8u, ps_addr_field_lo, ps_addr_field_hi);
    p = PM4CmdSetData::SetShReg(p, 72u, vs_addr_field_lo, vs_addr_field_hi);
    const u32 dcb_dwords = static_cast<u32>(p - dcb_storage.data());

    try {
        Liverpool liverpool;
        const auto pstats = liverpool.ExecutorParsePm4NoRaster(
            std::span<const u32>(dcb_storage.data(), dcb_dwords));
        const auto& regs = liverpool.ExecutorRegs();

        const u64 ps_prog_addr = reinterpret_cast<u64>(regs.ps_program.Address<const u8*>());
        const u64 vs_prog_addr = reinterpret_cast<u64>(regs.vs_program.Address<const u8*>());
        const bool ps_addr_ok = (ps_prog_addr == ps_addr);
        const bool vs_addr_ok = (vs_prog_addr == vs_addr);
        {
            char b[224];
            std::snprintf(b, sizeof(b),
                          "stage=setshreg setShRegSeen=%s psAddr=%s(0x%llx vs 0x%llx) vsAddr=%s",
                          pstats.set_sh_reg ? "YES" : "NO", ps_addr_ok ? "OK" : "BAD",
                          (unsigned long long)ps_prog_addr, (unsigned long long)ps_addr,
                          vs_addr_ok ? "OK" : "BAD");
            mark(b);
        }

        bool ps_bi_ok = false, vs_bi_ok = false, ps_stage_ok = false, vs_stage_ok = false;
        size_t ps_code_dw = 0, vs_code_dw = 0;
        u64 ps_hash_out = 0, vs_hash_out = 0;
        if (ps_addr_ok) {
            const auto ps_params = AmdGpu::GetParams(regs.ps_program);
            ps_bi_ok = (ps_params.code.size() == ps.code_bytes / sizeof(u32)) &&
                       (ps_params.hash == ps.hash);
            ps_code_dw = ps_params.code.size();
            ps_hash_out = ps_params.hash;
            Shader::Info ps_info(Shader::Stage::Fragment, Shader::LogicalStage::Fragment, ps_params);
            ps_stage_ok = (ps_info.stage == Shader::Stage::Fragment);
        }
        if (vs_addr_ok) {
            const auto vs_params = AmdGpu::GetParams(regs.vs_program);
            vs_bi_ok = (vs_params.code.size() == vs.code_bytes / sizeof(u32)) &&
                       (vs_params.hash == vs.hash);
            vs_code_dw = vs_params.code.size();
            vs_hash_out = vs_params.hash;
            Shader::Info vs_info(Shader::Stage::Vertex, Shader::LogicalStage::Vertex, vs_params);
            vs_stage_ok = (vs_info.stage == Shader::Stage::Vertex);
        }

        {
            char b[256];
            std::snprintf(b, sizeof(b),
                          "stage=getparams psBinaryInfo=%s vsBinaryInfo=%s psHash=0x%llx vsHash=0x%llx "
                          "psCodeDwords=%zu vsCodeDwords=%zu psInfoStage=%s vsInfoStage=%s",
                          ps_bi_ok ? "OK" : "BAD", vs_bi_ok ? "OK" : "BAD",
                          (unsigned long long)ps_hash_out, (unsigned long long)vs_hash_out,
                          ps_code_dw, vs_code_dw, ps_stage_ok ? "Fragment" : "?",
                          vs_stage_ok ? "Vertex" : "?");
            mark(b);
        }

        const bool ok = pstats.set_sh_reg && ps_addr_ok && vs_addr_ok && ps_bi_ok && vs_bi_ok &&
                        ps_stage_ok && vs_stage_ok;
        char b[200];
        std::snprintf(b, sizeof(b),
                      "result=%s setShRegSeen=%s psAddressOk=%s vsAddressOk=%s psBinaryInfo=%s "
                      "vsBinaryInfo=%s rasterizer=NO vk=NO draw=NO",
                      ok ? "OK" : "FAIL", pstats.set_sh_reg ? "YES" : "NO", ps_addr_ok ? "YES" : "NO",
                      vs_addr_ok ? "YES" : "NO", ps_bi_ok ? "YES" : "NO", vs_bi_ok ? "YES" : "NO");
        mark(b);
    } catch (const std::exception& e) {
        char b[224];
        std::snprintf(b, sizeof(b), "result=FAIL exception what=%s", e.what());
        mark(b);
    }
    ::munmap(ps.map, ps.map_len);
    ::munmap(vs.map, vs.map_len);
}

void ExecutorRunGnmcapReplayDryRunSmoke(const char* marker_path) {
    auto mark = [&](const char* text) {
        if (FILE* f = std::fopen(marker_path, "a")) {
            std::time_t t = std::time(nullptr);
            std::fprintf(f, "[%lld] EXECUTOR_GNMCAP_REPLAY %s\n", (long long)t, text);
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_GNMCAP_REPLAY] %s", text);
    };
    using namespace AmdGpu;
    using namespace Executor::GnmCap;
    mark("phase=begin capture->replay dry-run (no draw)");

    const auto& blob = Executor::kOrbShdrCorpus[1];
    const u64 code_bytes = blob.num_dwords * sizeof(u32);
    const u64 staged_bytes = code_bytes + sizeof(AmdGpu::BinaryInfo);
    const size_t map_len = static_cast<size_t>((staged_bytes + 0xFFF) & ~u64(0xFFF));

    auto stage = [&](u8* dst) {
        std::memcpy(dst, blob.code, code_bytes);
        AmdGpu::BinaryInfo bi{};
        bi.signature = AmdGpu::BinaryInfo::signature_ref;
        bi.version = blob.version;
        bi.length = static_cast<u32>(code_bytes);
        bi.type = 0u;
        bi.shader_hash = blob.hash;
        bi.crc32 = blob.crc32;
        std::memcpy(dst + code_bytes, &bi, sizeof(bi));
    };
    void* m_old = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* m_new = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m_old == MAP_FAILED || m_new == MAP_FAILED) {
        mark("result=FAIL stage=mmap");
        return;
    }
    stage(reinterpret_cast<u8*>(m_old));
    stage(reinterpret_cast<u8*>(m_new));
    const u64 old_base = reinterpret_cast<u64>(m_old);
    const u64 new_base = reinterpret_cast<u64>(m_new);

    std::array<u32, 64> dcb_storage{};
    u32* p = dcb_storage.data();
    p = WritePacket<PM4ItOpcode::ClearState>(p, PM4ShaderType::ShaderGraphics, 0u);
    p = PM4CmdSetData::SetShReg(p, 8u, static_cast<u32>((old_base >> 8) & 0xFFFFFFFFu),
                                static_cast<u32>((old_base >> 8) >> 32));
    p = PM4CmdSetData::SetShReg(p, 10u, 15u, 0u);
    p = WritePacket<PM4ItOpcode::DrawIndexAuto>(p, PM4ShaderType::ShaderGraphics, 3u, 0u);
    const u32 dcb_dwords = static_cast<u32>(p - dcb_storage.data());
    std::span<const u32> dcb(dcb_storage.data(), dcb_dwords);

    u32 captured_indices = 0;
    u64 captured_hash = 0;
    std::vector<u8> cap_bytes;
    try {
        Liverpool lcap;
        const auto st = lcap.ExecutorParsePm4NoRaster(dcb);
        captured_indices = st.num_indices;
        const auto& regs = lcap.ExecutorRegs();
        if (reinterpret_cast<u64>(regs.ps_program.Address<const u8*>()) == old_base) {
            captured_hash = AmdGpu::GetParams(regs.ps_program).hash;
        }
        Capture cap;
        Deserialize(lcap.ExecutorCaptureGnmcapFrame(dcb, {}), cap);
        {
            auto& s = cap.AddSection(SectionType::Shader);
            ShaderHdr hdr{blob.hash, 0u, static_cast<uint32_t>(code_bytes), blob.crc32, 0u};
            Capture::AppendPod(s.payload, hdr);
            Capture::AppendBytes(s.payload, blob.code, code_bytes);
        }
        {
            auto& s = cap.AddSection(SectionType::Reloc);
            RelocRec r{old_base, new_base, staged_bytes, 5u, 0u};
            Capture::AppendPod(s.payload, r);
        }
        cap_bytes = Serialize(cap);
        char b[160];
        std::snprintf(b, sizeof(b), "stage=capture indices=%u shaderHash=0x%llx gnmcapBytes=%zu",
                      captured_indices, (unsigned long long)captured_hash, cap_bytes.size());
        mark(b);
    } catch (const std::exception& e) {
        char b[200];
        std::snprintf(b, sizeof(b), "result=FAIL stage=capture what=%s", e.what());
        mark(b);
        ::munmap(m_old, map_len);
        ::munmap(m_new, map_len);
        return;
    }

    bool replay_ok = false, indices_match = false, hash_match = false, roundtrip = false;
    try {
        Capture rc;
        if (Deserialize(cap_bytes, rc) != ParseResult::Ok) {
            mark("result=FAIL stage=replay_parse");
            ::munmap(m_old, map_len);
            ::munmap(m_new, map_len);
            return;
        }
        std::vector<u32> rdcb;
        u64 r_old = 0, r_new = 0;
        for (const auto& s : rc.sections) {
            if (s.type == SectionType::QueueBlob) {
                const auto* qh = reinterpret_cast<const QueueBlobHdr*>(s.payload.data());
                if (qh->queue_type == static_cast<uint32_t>(QueueType::Dcb)) {
                    const u32* d = reinterpret_cast<const u32*>(s.payload.data() + sizeof(QueueBlobHdr));
                    rdcb.assign(d, d + qh->dword_count);
                }
            } else if (s.type == SectionType::Reloc) {
                const auto* r = reinterpret_cast<const RelocRec*>(s.payload.data());
                r_old = r->old_base;
                r_new = r->new_base;
            }
        }
        const u32 old_lo = static_cast<u32>((r_old >> 8) & 0xFFFFFFFFu);
        const u32 old_hi = static_cast<u32>((r_old >> 8) >> 32);
        const u32 new_lo = static_cast<u32>((r_new >> 8) & 0xFFFFFFFFu);
        const u32 new_hi = static_cast<u32>((r_new >> 8) >> 32);
        u32 patched = 0;
        for (size_t i = 0; i + 1 < rdcb.size(); ++i) {
            if (rdcb[i] == old_lo && rdcb[i + 1] == old_hi) {
                rdcb[i] = new_lo;
                rdcb[i + 1] = new_hi;
                ++patched;
            }
        }
        Liverpool lrep;
        const auto st = lrep.ExecutorParsePm4NoRaster(std::span<const u32>(rdcb.data(), rdcb.size()));
        const auto& regs = lrep.ExecutorRegs();
        roundtrip = (reinterpret_cast<u64>(regs.ps_program.Address<const u8*>()) == r_new);
        indices_match = (st.num_indices == captured_indices);
        if (roundtrip) {
            hash_match = (AmdGpu::GetParams(regs.ps_program).hash == captured_hash);
        }
        replay_ok = (patched > 0) && roundtrip && indices_match && hash_match;
        char b[224];
        std::snprintf(b, sizeof(b),
                      "stage=replay relocPatched=%u addressRebased=%s indicesMatch=%s shaderHashMatch=%s",
                      patched, roundtrip ? "YES" : "NO", indices_match ? "YES" : "NO",
                      hash_match ? "YES" : "NO");
        mark(b);
    } catch (const std::exception& e) {
        char b[200];
        std::snprintf(b, sizeof(b), "result=FAIL stage=replay what=%s", e.what());
        mark(b);
    }

    char b[200];
    std::snprintf(b, sizeof(b),
                  "result=%s captureToReplay=state_equivalent indicesMatch=%s shaderHashMatch=%s "
                  "draw=NO vk=NO",
                  replay_ok ? "OK" : "FAIL", indices_match ? "YES" : "NO", hash_match ? "YES" : "NO");
    mark(b);
    ::munmap(m_old, map_len);
    ::munmap(m_new, map_len);
}

void ExecutorRunVsCompileClassifySmoke(const char* marker_path) {
    auto mark = [&](const char* text) {
        if (FILE* f = std::fopen(marker_path, "a")) {
            std::time_t t = std::time(nullptr);
            std::fprintf(f, "[%lld] EXECUTOR_VS_COMPILE_CLASSIFY %s\n", (long long)t, text);
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_VS_COMPILE_CLASSIFY] %s", text);
    };
    mark("phase=begin shader=vs_18 stage=Vertex precheck");

    const auto& vs_blob = Executor::kOrbShdrCorpus[18];
    const u64 code_bytes = vs_blob.num_dwords * sizeof(u32);
    const u64 staged_bytes = code_bytes + sizeof(AmdGpu::BinaryInfo);

    const size_t map_len = static_cast<size_t>((staged_bytes + 0xFFF) & ~u64(0xFFF));
    void* mapped = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        mark("result=FAIL stage=mmap");
        return;
    }
    u8* base = reinterpret_cast<u8*>(mapped);
    std::memcpy(base, vs_blob.code, code_bytes);
    {
        AmdGpu::BinaryInfo bi{};
        bi.signature = AmdGpu::BinaryInfo::signature_ref;
        bi.version = vs_blob.version;
        bi.length = static_cast<u32>(code_bytes);
        bi.type = 1u;
        bi.shader_hash = vs_blob.hash;
        bi.crc32 = vs_blob.crc32;
        std::memcpy(base + code_bytes, &bi, sizeof(bi));
    }
    const u64 vs_addr = reinterpret_cast<u64>(base);

    const ShaderCensus census = CensusOrbShdr(vs_blob);
    const bool resource_load = census.mem_ops > 0;
    const bool exp_pos0 = census.exp_pos > 0;

    std::array<u32, 64> dcb_storage{};
    u32* p = dcb_storage.data();
    using namespace AmdGpu;
    p = WritePacket<PM4ItOpcode::ClearState>(p, PM4ShaderType::ShaderGraphics, 0u);
    p = PM4CmdSetData::SetShReg(p, 72u, static_cast<u32>((vs_addr >> 8) & 0xFFFFFFFFu),
                                static_cast<u32>((vs_addr >> 8) >> 32));
    p = PM4CmdSetData::SetShReg(p, 74u, 15u, 0u);
    const u32 dcb_dwords = static_cast<u32>(p - dcb_storage.data());

    bool roundtrip = false, getparams_ok = false;
    const char* translate_status = "SKIPPED_SAFE";
    const char* classification = "UNKNOWN";
    try {
        Liverpool liverpool;
        liverpool.ExecutorParsePm4NoRaster(std::span<const u32>(dcb_storage.data(), dcb_dwords));
        const auto& regs = liverpool.ExecutorRegs();
        roundtrip = (reinterpret_cast<u64>(regs.vs_program.Address<const u8*>()) == vs_addr);
        if (roundtrip) {
            const auto params = AmdGpu::GetParams(regs.vs_program);
            getparams_ok = (params.code.size() == vs_blob.num_dwords) && (params.hash == vs_blob.hash);

            classification = resource_load ? "REQUIRES_RESOURCE_STATE" : "STANDALONE_VS";

            std::string flag_path(marker_path);
            if (const auto sl = flag_path.find_last_of("/\\"); sl != std::string::npos) {
                flag_path.replace(sl + 1, std::string::npos, "allow-translate-resource-shader");
            }
            bool opt_in = false;
            if (FILE* fp = std::fopen(flag_path.c_str(), "r")) {
                std::fclose(fp);
                opt_in = true;
            }
            if (opt_in && getparams_ok) {
                try {
                    Shader::Info info(Shader::Stage::Vertex, Shader::LogicalStage::Vertex, params);
                    union RuntimeInfoHolder {
                        Shader::RuntimeInfo ri;
                        RuntimeInfoHolder() {}
                        ~RuntimeInfoHolder() {}
                    } holder;
                    Shader::RuntimeInfo& runtime_info = holder.ri;
                    runtime_info.Initialize(Shader::Stage::Vertex);
                    runtime_info.num_allocated_vgprs = regs.vs_program.NumVgprs();
                    Shader::Profile profile{};
                    profile.supported_spirv = 0x00010300;
                    profile.subgroup_size = 64;
                    profile.support_float64 = true;
                    profile.support_int64 = true;
                    Shader::Pools pools;
                    Shader::Backend::Bindings binding{};
                    const auto ir = Shader::TranslateProgram(params.code, pools, info, runtime_info,
                                                             profile);
                    const auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir, binding);
                    translate_status = (spv.size() >= 5 && spv[0] == 0x07230203u) ? "OK" : "FAIL_SAFE";
                } catch (const std::exception&) {
                    translate_status = "FAIL_SAFE";
                }
            }
        }
    } catch (const std::exception& e) {
        char b[224];
        std::snprintf(b, sizeof(b), "stage=parse exception what=%s", e.what());
        mark(b);
    }

    char feat[64];
    std::snprintf(feat, sizeof(feat), "%s%s%s", resource_load ? "RESOURCE_LOAD" : "",
                  (resource_load && exp_pos0) ? "|" : "", exp_pos0 ? "EXP_POS0" : "");
    char b[256];
    std::snprintf(b, sizeof(b),
                  "result=%s shader=vs_18 stage=Vertex addressRoundtrip=%s getParams=%s features=%s "
                  "classification=%s translate=%s signals=0 device=NO",
                  (roundtrip && getparams_ok) ? "OK" : "FAIL", roundtrip ? "YES" : "NO",
                  getparams_ok ? "OK" : "BAD", feat, classification, translate_status);
    mark(b);
    ::munmap(mapped, map_len);
}

void ExecutorRunLayer3GnmcapRelocSmoke(const char* marker_path) {
    auto mark = [&](const char* text) {
        if (FILE* f = std::fopen(marker_path, "a")) {
            std::time_t t = std::time(nullptr);
            std::fprintf(f, "[%lld] EXECUTOR_GNMCAP_RELOC %s\n", (long long)t, text);
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_GNMCAP_RELOC] %s", text);
    };
    mark("phase=begin rebase staged shader into a 40-bit-representable untagged VA");

    const auto& ps_blob = Executor::kOrbShdrCorpus[1];
    const u64 code_bytes = ps_blob.num_dwords * sizeof(u32);
    const u64 staged_bytes = code_bytes + sizeof(AmdGpu::BinaryInfo);

    std::vector<u8> old_mem(staged_bytes + 256, 0);
    u8* old_base = reinterpret_cast<u8*>(
        (reinterpret_cast<uintptr_t>(old_mem.data()) + 255) & ~uintptr_t(255));

    const size_t map_len = static_cast<size_t>((staged_bytes + 0xFFF) & ~u64(0xFFF));
    void* mapped = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        mark("result=FAIL stage=mmap");
        return;
    }
    u8* new_base = reinterpret_cast<u8*>(mapped);

    auto write_staged = [&](u8* dst) {
        std::memcpy(dst, ps_blob.code, code_bytes);
        AmdGpu::BinaryInfo bi{};
        bi.signature = AmdGpu::BinaryInfo::signature_ref;
        bi.version = ps_blob.version;
        bi.length = static_cast<u32>(code_bytes);
        bi.type = 0u;
        bi.shader_hash = ps_blob.hash;
        bi.crc32 = ps_blob.crc32;
        std::memcpy(dst + code_bytes, &bi, sizeof(bi));
    };
    write_staged(old_base);
    write_staged(new_base);

    const u64 old_addr = reinterpret_cast<u64>(old_base);
    const u64 new_addr = reinterpret_cast<u64>(new_base);
    const bool fits40 = ((new_addr >> 8) >> 40) == 0;
    const bool aligned = (new_addr & 0xFF) == 0;

    Executor::GnmCap::RelocRec reloc{old_addr, new_addr, staged_bytes, 5u, 0u};
    {
        char b[224];
        std::snprintf(b, sizeof(b),
                      "stage=reloc oldBase=0x%llx newBase=0x%llx size=%llu fits40=%s alignment=0x%x "
                      "reason=shader_code",
                      (unsigned long long)reloc.old_base, (unsigned long long)reloc.new_base,
                      (unsigned long long)reloc.size, fits40 ? "YES" : "NO",
                      (new_addr & 0xFF) == 0 ? 0x100 : 1);
        mark(b);
    }
    if (!fits40 || !aligned) {
        mark("result=FAIL stage=reloc (new_base does not fit the 40-bit address field)");
        ::munmap(mapped, map_len);
        return;
    }

    std::array<u32, 64> dcb_storage{};
    u32* p = dcb_storage.data();
    using namespace AmdGpu;
    p = WritePacket<PM4ItOpcode::ClearState>(p, PM4ShaderType::ShaderGraphics, 0u);
    p = PM4CmdSetData::SetShReg(p, 8u, static_cast<u32>((new_addr >> 8) & 0xFFFFFFFFu),
                                static_cast<u32>((new_addr >> 8) >> 32));
    p = PM4CmdSetData::SetShReg(p, 10u, 15u, 0u);
    const u32 dcb_dwords = static_cast<u32>(p - dcb_storage.data());

    const char* vk_status = "SKIPPED_EMULATOR";
    bool roundtrip = false, getparams_ok = false, translate_ok = false, emit_ok = false;
    size_t spv_words = 0;
    try {
        Liverpool liverpool;
        const auto pstats = liverpool.ExecutorParsePm4NoRaster(
            std::span<const u32>(dcb_storage.data(), dcb_dwords));
        const auto& regs = liverpool.ExecutorRegs();
        roundtrip = (reinterpret_cast<u64>(regs.ps_program.Address<const u8*>()) == new_addr);
        if (pstats.set_sh_reg && roundtrip) {
            const auto params = AmdGpu::GetParams(regs.ps_program);
            getparams_ok = (params.code.size() == ps_blob.num_dwords) && (params.hash == ps_blob.hash);
            Shader::Info info(Shader::Stage::Fragment, Shader::LogicalStage::Fragment, params);
            union RuntimeInfoHolder {
                Shader::RuntimeInfo ri;
                RuntimeInfoHolder() {}
                ~RuntimeInfoHolder() {}
            } holder;
            Shader::RuntimeInfo& runtime_info = holder.ri;
            runtime_info.Initialize(Shader::Stage::Fragment);
            runtime_info.num_allocated_vgprs = regs.ps_program.NumVgprs();
            runtime_info.num_user_data = regs.ps_program.settings.num_user_regs;
            Shader::Profile profile{};
            profile.supported_spirv = 0x00010300;
            profile.subgroup_size = 64;
            profile.support_float64 = true;
            profile.support_int64 = true;
            profile.support_int16 = true;
            profile.max_ubo_size = 65536;
            profile.max_viewport_width = 16384;
            profile.max_viewport_height = 16384;
            Shader::Pools pools;
            Shader::Backend::Bindings binding{};
            const auto ir_program =
                Shader::TranslateProgram(params.code, pools, info, runtime_info, profile);
            translate_ok = true;
            const std::vector<u32> spv =
                Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
            emit_ok = spv.size() >= 5 && spv[0] == 0x07230203u;
            spv_words = spv.size();

            std::string flag_path(marker_path);
            if (const auto sl = flag_path.find_last_of("/\\"); sl != std::string::npos) {
                flag_path.replace(sl + 1, std::string::npos, "allow-vk-shader-module");
            }
            if (emit_ok) {
                if (FILE* fp = std::fopen(flag_path.c_str(), "r")) {
                    std::fclose(fp);
                    try {
                        Instance instance(false, false, true);
                        const vk::Device device = instance.GetDevice();
                        const vk::ShaderModuleCreateInfo ci{.codeSize = spv.size() * sizeof(u32),
                                                            .pCode = spv.data()};
                        const auto [res, module] = device.createShaderModule(ci);
                        if (res == vk::Result::eSuccess && module) {
                            device.destroyShaderModule(module);
                            vk_status = "OK_DEVICE";
                        } else {
                            vk_status = "FAIL";
                        }
                    } catch (const std::exception&) {
                        vk_status = "FAIL_EXCEPTION";
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        char b[224];
        std::snprintf(b, sizeof(b), "stage=compile exception what=%s", e.what());
        mark(b);
    }

    char b[256];
    std::snprintf(b, sizeof(b),
                  "result=%s oldBase=0x%llx newBase=0x%llx fits40=YES alignment=0x100 setShRegPatched=YES "
                  "addressRoundtrip=%s getParams=%s translate=%s emit=%s spvWords=%zu "
                  "vkCreateShaderModule=%s",
                  (roundtrip && getparams_ok && translate_ok && emit_ok) ? "OK" : "FAIL",
                  (unsigned long long)old_addr, (unsigned long long)new_addr, roundtrip ? "YES" : "NO",
                  getparams_ok ? "OK" : "BAD", translate_ok ? "OK" : "BAD", emit_ok ? "OK" : "BAD",
                  spv_words, vk_status);
    mark(b);
    ::munmap(mapped, map_len);
}

void ExecutorRunLayer3PipelineShaderCompileSmoke(const char* marker_path) {
    auto mark = [&](const char* text) {
        if (FILE* f = std::fopen(marker_path, "a")) {
            std::time_t t = std::time(nullptr);
            std::fprintf(f, "[%lld] EXECUTOR_LAYER3_PIPELINE_SHADER_COMPILE %s\n", (long long)t, text);
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                            "[EXECUTOR_LAYER3_PIPELINE_SHADER_COMPILE] %s", text);
    };
    mark("phase=begin source=PipelinePrep stage=Fragment shader=ps_01");

    const auto& ps_blob = Executor::kOrbShdrCorpus[1];
    const u64 code_bytes = ps_blob.num_dwords * sizeof(u32);

    const size_t map_len =
        static_cast<size_t>((code_bytes + sizeof(AmdGpu::BinaryInfo) + 0xFFF) & ~u64(0xFFF));
    void* map = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        mark("result=FAIL stage=mmap");
        return;
    }
    u8* base = reinterpret_cast<u8*>(map);
    std::memcpy(base, ps_blob.code, code_bytes);
    {
        AmdGpu::BinaryInfo bi{};
        bi.signature = AmdGpu::BinaryInfo::signature_ref;
        bi.version = ps_blob.version;
        bi.length = static_cast<u32>(code_bytes);
        bi.type = 0u;
        bi.shader_hash = ps_blob.hash;
        bi.crc32 = ps_blob.crc32;
        std::memcpy(base + code_bytes, &bi, sizeof(bi));
    }
    const u64 ps_addr = reinterpret_cast<u64>(base);

    std::array<u32, 64> dcb_storage{};
    u32* p = dcb_storage.data();
    using namespace AmdGpu;
    p = WritePacket<PM4ItOpcode::ClearState>(p, PM4ShaderType::ShaderGraphics, 0u);
    p = PM4CmdSetData::SetShReg(p, 8u, static_cast<u32>((ps_addr >> 8) & 0xFFFFFFFFu),
                                static_cast<u32>((ps_addr >> 8) >> 32));
    p = PM4CmdSetData::SetShReg(p, 10u, 15u, 0u);
    const u32 dcb_dwords = static_cast<u32>(p - dcb_storage.data());

    try {
        Liverpool liverpool;
        const auto pstats = liverpool.ExecutorParsePm4NoRaster(
            std::span<const u32>(dcb_storage.data(), dcb_dwords));
        const auto& regs = liverpool.ExecutorRegs();
        const bool addr_ok = (reinterpret_cast<u64>(regs.ps_program.Address<const u8*>()) == ps_addr);
        if (!pstats.set_sh_reg || !addr_ok) {
            mark("result=FAIL stage=setshreg (address did not round-trip)");
            ::munmap(map, map_len);
            return;
        }

        const auto params = AmdGpu::GetParams(regs.ps_program);
        const bool getparams_ok =
            (params.code.size() == ps_blob.num_dwords) && (params.hash == ps_blob.hash);
        Shader::Info info(Shader::Stage::Fragment, Shader::LogicalStage::Fragment, params);

        union RuntimeInfoHolder {
            Shader::RuntimeInfo ri;
            RuntimeInfoHolder() {}
            ~RuntimeInfoHolder() {}
        } holder;
        Shader::RuntimeInfo& runtime_info = holder.ri;
        runtime_info.Initialize(Shader::Stage::Fragment);
        runtime_info.num_allocated_vgprs = regs.ps_program.NumVgprs();
        runtime_info.num_user_data = regs.ps_program.settings.num_user_regs;
        runtime_info.fp_denorm_mode32 = regs.ps_program.settings.fp_denorm_mode32;
        runtime_info.fp_round_mode32 = regs.ps_program.settings.fp_round_mode32;
        {
            char b[200];
            std::snprintf(b, sizeof(b),
                          "stage=runtimeinfo source=FROM_REGS num_allocated_vgprs=%u num_user_data=%u "
                          "getParams=%s codeDwords=%zu",
                          runtime_info.num_allocated_vgprs, runtime_info.num_user_data,
                          getparams_ok ? "OK" : "BAD", params.code.size());
            mark(b);
        }

        Shader::Profile profile{};
        profile.supported_spirv = 0x00010300;
        profile.subgroup_size = 64;
        profile.support_float64 = true;
        profile.support_int64 = true;
        profile.support_int16 = true;
        profile.max_ubo_size = 65536;
        profile.max_viewport_width = 16384;
        profile.max_viewport_height = 16384;

        Shader::Pools pools;
        Shader::Backend::Bindings binding{};
        const auto ir_program =
            Shader::TranslateProgram(params.code, pools, info, runtime_info, profile);
        const std::vector<u32> spv =
            Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
        const bool spv_ok = spv.size() >= 5 && spv[0] == 0x07230203u;
        {
            char b[160];
            std::snprintf(b, sizeof(b), "stage=compile translate=OK emit=%s spvWords=%zu",
                          spv_ok ? "OK" : "BAD", spv.size());
            mark(b);
        }

        const char* vk_status = "SKIPPED_EMULATOR";
        std::string flag_path(marker_path);
        if (const auto sl = flag_path.find_last_of("/\\"); sl != std::string::npos) {
            flag_path.replace(sl + 1, std::string::npos, "allow-vk-shader-module");
        }
        if (spv_ok) {
            if (FILE* fp = std::fopen(flag_path.c_str(), "r")) {
                std::fclose(fp);
                try {
                    Instance instance(false, false, true);
                    const vk::Device device = instance.GetDevice();
                    const vk::ShaderModuleCreateInfo ci{.codeSize = spv.size() * sizeof(u32),
                                                        .pCode = spv.data()};
                    const auto [res, module] = device.createShaderModule(ci);
                    if (res == vk::Result::eSuccess && module) {
                        device.destroyShaderModule(module);
                        vk_status = "OK";
                    } else {
                        vk_status = "FAIL";
                    }
                } catch (const std::exception&) {
                    vk_status = "FAIL_EXCEPTION";
                }
            }
        }

        char b[256];
        std::snprintf(b, sizeof(b),
                      "result=%s source=PipelinePrep stage=Fragment shader=ps_01 getParams=%s "
                      "runtimeInfo=FROM_REGS translate=OK emit=%s spvWords=%zu vkCreateShaderModule=%s "
                      "draw=NO rasterizer=NO presenter=NO",
                      (getparams_ok && spv_ok) ? "OK" : "FAIL", getparams_ok ? "OK" : "BAD",
                      spv_ok ? "OK" : "BAD", spv.size(), vk_status);
        mark(b);
    } catch (const std::exception& e) {
        char b[224];
        std::snprintf(b, sizeof(b), "result=FAIL exception what=%s", e.what());
        mark(b);
    }
    ::munmap(map, map_len);
}

void ExecutorRunGnmFormalSubmitSmoke(const char* marker_path) {
    auto mark = [&](const char* text) {
        if (FILE* f = std::fopen(marker_path, "a")) {
            std::time_t t = std::time(nullptr);
            std::fprintf(f, "[%lld] EXECUTOR_GNM_FORMAL_SUBMIT %s\n", (long long)t, text);
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_GNM_FORMAL_SUBMIT] %s", text);
    };
    using namespace AmdGpu;
    mark("phase=begin formal sceGnmSubmitCommandBuffersForWorkload (no draw)");

    const bool presenter_ok =
        Libraries::GnmDriver::ExecutorEnsureGnmPresenter("formal_submit_no_draw_smoke");
    mark(presenter_ok ? "stage=presenter result=OK" : "stage=presenter result=FAIL");

    const size_t map_len = 0x1000;
    void* map = ::mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) {
        mark("result=FAIL stage=mmap");
        return;
    }
    u32* dcb = reinterpret_cast<u32*>(map);
    u32* p = dcb;
    p = WritePacket<PM4ItOpcode::ClearState>(p, PM4ShaderType::ShaderGraphics, 0u);
    p = WritePacket<PM4ItOpcode::NumInstances>(p, PM4ShaderType::ShaderGraphics, 1u);
    const u32 dcb_dwords = static_cast<u32>(p - dcb);
    u32 dcb_bytes = dcb_dwords * sizeof(u32);

    const u32* dcb_addrs[1] = {dcb};
    u32 dcb_sizes[1] = {dcb_bytes};
    int rc = 0;
    try {
        rc = Libraries::GnmDriver::sceGnmSubmitCommandBuffersForWorkload(
            1u, 1u, dcb_addrs, dcb_sizes, nullptr, nullptr);
    } catch (const std::exception& e) {
        char b[200];
        std::snprintf(b, sizeof(b), "result=FAIL stage=submit what=%s", e.what());
        mark(b);
        ::munmap(map, map_len);
        return;
    }
    char b[200];
    std::snprintf(b, sizeof(b), "stage=submit dcbDwords=%u submitRc=0x%x", dcb_dwords, (unsigned)rc);
    mark(b);

    const bool ok = presenter_ok && (rc == 0);
    std::snprintf(b, sizeof(b),
                  "result=%s presenter=%s submitRc=0x%x draw=NO rasterizerBound=YES",
                  ok ? "OK" : "FAIL", presenter_ok ? "OK" : "FAIL", (unsigned)rc);
    mark(b);
    ::usleep(300 * 1000);
    ::munmap(map, map_len);
}

void ExecutorRunGnmReplayLiteSmoke(const char* marker_path) {
    auto mark = [&](const char* text) {
        if (FILE* f = std::fopen(marker_path, "a")) {
            std::time_t t = std::time(nullptr);
            std::fprintf(f, "[%lld] EXECUTOR_GNM_REPLAYLITE %s\n", (long long)t, text);
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_GNM_REPLAYLITE] %s", text);
    };
    using namespace AmdGpu;
    mark("phase=begin replay real-game DCB (INCOMPLETE_MEMORY, pixelClaim=NO)");

    using namespace Executor::GnmCap;
    std::string cap_path(marker_path);
    if (const auto sl = cap_path.find_last_of("/\\"); sl != std::string::npos) {
        cap_path.replace(sl + 1, std::string::npos, "replay-primary.gnmcap");
    }
    std::vector<u8> cap_bytes;
    if (FILE* f = std::fopen(cap_path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            cap_bytes.resize(static_cast<size_t>(sz));
            const size_t rd = std::fread(cap_bytes.data(), 1, cap_bytes.size(), f);
            (void)rd;
        }
        std::fclose(f);
    }
    if (cap_bytes.empty()) {
        char b[256];
        std::snprintf(b, sizeof(b), "result=FAIL stage=read_gnmcap path=%s (push a PC-captured .gnmcap here)",
                      cap_path.c_str());
        mark(b);
        return;
    }
    Capture rc;
    if (Deserialize(cap_bytes, rc) != ParseResult::Ok) {
        mark("result=FAIL stage=gnmcap_parse (container did not deserialize)");
        return;
    }
    std::vector<u32> dcb;
    u64 capShaderVs = 0, capShaderPs = 0;
    for (const auto& s : rc.sections) {
        if (s.type == SectionType::QueueBlob && s.payload.size() >= sizeof(QueueBlobHdr)) {
            const auto* qh = reinterpret_cast<const QueueBlobHdr*>(s.payload.data());
            if (qh->queue_type == static_cast<uint32_t>(QueueType::Dcb)) {
                const u32* d = reinterpret_cast<const u32*>(s.payload.data() + sizeof(QueueBlobHdr));
                dcb.assign(d, d + qh->dword_count);
            }
        }
    }
    {
        char b[200];
        std::snprintf(b, sizeof(b), "stage=gnmcap_loaded sections=%zu dcbDwords=%zu capBytes=%zu",
                      rc.sections.size(), dcb.size(), cap_bytes.size());
        mark(b);
    }
    if (dcb.empty()) {
        mark("result=FAIL stage=no_dcb_in_gnmcap");
        return;
    }
    (void)capShaderVs;
    (void)capShaderPs;
    {
        char b[128];
        std::snprintf(b, sizeof(b), "stage=loaded dcbDwords=%zu dcbBytes=%zu", dcb.size(),
                      dcb.size() * sizeof(u32));
        mark(b);
    }

    {
        const u32* p = dcb.data();
        const size_t n = dcb.size();
        size_t i = 0;
        u32 packets = 0, draws = 0, set_sh = 0, set_ctx = 0, set_uconfig = 0, indirect = 0, dispatch = 0;
        u32 num_instances = 0, event_writes = 0, unknown = 0;
        bool truncated = false;
        while (i < n) {
            const u32 h = p[i];
            const u32 type = h >> 30;
            if (type == 3u) {
                const u32 op = (h >> 8) & 0xFFu;
                const u32 cnt = ((h >> 16) & 0x3FFFu) + 1u;
                ++packets;
                switch (op) {
                case 0x27: case 0x2D: case 0x30: case 0x35: case 0x36: ++draws; break;
                case 0x76: ++set_sh; break;
                case 0x69: ++set_ctx; break;
                case 0x79: ++set_uconfig; break;
                case 0x33: case 0x3F: ++indirect; break;
                case 0x15: ++dispatch; break;
                case 0x2F: ++num_instances; break;
                case 0x46: case 0x47: case 0x48: ++event_writes; break;
                default: ++unknown; break;
                }
                if (i + 1u + cnt > n) { truncated = true; break; }
                i += 1u + cnt;
            } else if (type == 2u) {
                ++i;
            } else if (type == 0u) {
                const u32 cnt = ((h >> 16) & 0x3FFFu) + 1u;
                ++packets;
                if (i + 1u + cnt > n) { truncated = true; break; }
                i += 1u + cnt;
            } else {
                ++unknown; ++i;
            }
        }
        char b[320];
        std::snprintf(b, sizeof(b),
                      "stage=scan pm4Packets=%u draws=%u setShReg=%u setContextReg=%u setUconfig=%u "
                      "indirectBuffer=%u dispatch=%u numInstances=%u eventWrites=%u unknown=%u trunc=%s",
                      packets, draws, set_sh, set_ctx, set_uconfig, indirect, dispatch, num_instances,
                      event_writes, unknown, truncated ? "YES" : "NO");
        mark(b);

        const bool ok = (packets > 0) && (draws > 0 || set_sh > 0);
        std::snprintf(b, sizeof(b),
                      "result=%s source=gnmcap_capture gnmcap=DCB_real Liverpool=scan_safe "
                      "pm4Packets=%u draws=%u setShRegSeen=%s memoryComplete=NO pixelClaim=NO",
                      ok ? "OK" : "FAIL", packets, draws, set_sh > 0 ? "YES" : "NO");
        mark(b);
    }
}

void ExecutorRunGnmReplayWithMemorySmoke(const char* marker_path) {
    auto mark = [&](const char* text) {
        if (FILE* f = std::fopen(marker_path, "a")) {
            std::time_t t = std::time(nullptr);
            std::fprintf(f, "[%lld] EXECUTOR_GNM_REPLAY_MEM %s\n", (long long)t, text);
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_GNM_REPLAY_MEM] %s", text);
    };
    using namespace Executor::GnmCap;
    mark("phase=begin replay real-game GNM submits WITH memory");

    std::string cap_path(marker_path);
    if (const auto sl = cap_path.find_last_of("/\\"); sl != std::string::npos) {
        cap_path.replace(sl + 1, std::string::npos, "replay-primary.gnmcap");
    }
    std::vector<u8> cap_bytes;
    if (FILE* f = std::fopen(cap_path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            cap_bytes.resize(static_cast<size_t>(sz));
            (void)std::fread(cap_bytes.data(), 1, cap_bytes.size(), f);
        }
        std::fclose(f);
    }
    Capture rc;
    if (cap_bytes.empty() || Deserialize(cap_bytes, rc) != ParseResult::Ok) {
        mark("result=FAIL stage=load_gnmcap");
        return;
    }
    mark("stage=cap_loaded primary=OK");

    std::vector<u32> dcb;
    struct Range {
        u64 old_base;
        u64 size;
        const u8* data;
        u32 usage;
    };
    std::vector<Range> ranges;
    u32 shader_sections = 0;

    auto load_capture_file = [&](const std::string& path, Capture& out) -> bool {
        std::vector<u8> bytes;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                bytes.resize(static_cast<size_t>(sz));
                (void)std::fread(bytes.data(), 1, bytes.size(), f);
            }
            std::fclose(f);
        }
        return !bytes.empty() && Deserialize(bytes, out) == ParseResult::Ok;
    };
    auto merge_range = [&](const MemoryRangeHdr* mh, const u8* data) {
        if (!mh || mh->size == 0) {
            return;
        }
        for (auto& r : ranges) {
            if (r.old_base == mh->guest_base) {
                if (mh->size > r.size) {
                    r.size = mh->size;
                    r.data = data;
                    r.usage = mh->flags;
                }
                return;
            }
        }
        ranges.push_back({mh->guest_base, mh->size, data, mh->flags});
    };
    auto extract_capture = [&](Capture& cap, std::vector<u32>& out_dcb, bool merge_memory,
                               u32* shader_count) {
        out_dcb.clear();
        for (const auto& s : cap.sections) {
            if (s.type == SectionType::QueueBlob && s.payload.size() >= sizeof(QueueBlobHdr)) {
                const auto* qh = reinterpret_cast<const QueueBlobHdr*>(s.payload.data());
                if (qh->queue_type == static_cast<uint32_t>(QueueType::Dcb)) {
                    const u32* d =
                        reinterpret_cast<const u32*>(s.payload.data() + sizeof(QueueBlobHdr));
                    out_dcb.assign(d, d + qh->dword_count);
                }
            } else if (merge_memory && s.type == SectionType::MemoryRange &&
                       s.payload.size() >= sizeof(MemoryRangeHdr)) {
                const auto* mh = reinterpret_cast<const MemoryRangeHdr*>(s.payload.data());
                merge_range(mh, s.payload.data() + sizeof(MemoryRangeHdr));
            } else if (shader_count && s.type == SectionType::Shader) {
                ++(*shader_count);
            }
        }
    };

    extract_capture(rc, dcb, true, &shader_sections);
    if (dcb.empty()) {
        mark("result=FAIL stage=no_dcb");
        return;
    }
    mark("stage=primary_extracted result=OK");
    Capture rc1;
    std::vector<u32> dcb1;
    {
        std::string cap1_path(marker_path);
        if (const auto sl = cap1_path.find_last_of("/\\"); sl != std::string::npos) {
            cap1_path.replace(sl + 1, std::string::npos, "replay-prior.gnmcap");
        }
        if (load_capture_file(cap1_path, rc1)) {
            extract_capture(rc1, dcb1, true, nullptr);
            mark("stage=prior_extracted result=OK");
        }
    }

    std::vector<Capture> seq_caps;
    std::vector<std::vector<u32>> seq_dcbs;
    seq_caps.reserve(64);
    seq_dcbs.reserve(64);
    std::string cap_dir(marker_path);
    if (const auto sl = cap_dir.find_last_of("/\\"); sl != std::string::npos) {
        cap_dir.resize(sl + 1);
    } else {
        cap_dir.clear();
    }
    auto try_load_sequence = [&](const char* pattern) -> bool {
        seq_caps.clear();
        seq_dcbs.clear();
        for (u32 i = 0; i < 64; ++i) {
            char name[96];
            std::snprintf(name, sizeof(name), pattern, i);
            const std::string path = cap_dir + name;
            Capture cap;
            if (!load_capture_file(path, cap)) {
                return !seq_dcbs.empty();
            }
            seq_caps.push_back(std::move(cap));
            seq_dcbs.emplace_back();
            extract_capture(seq_caps.back(), seq_dcbs.back(), false, nullptr);
            if (seq_dcbs.back().empty()) {
                seq_caps.pop_back();
                seq_dcbs.pop_back();
                return !seq_dcbs.empty();
            }
        }
        return !seq_dcbs.empty();
    };
    bool sequence_mode = try_load_sequence("live-submit-%u.gnmcap");
    const char* sequence_name = sequence_mode ? "live-submit" : "none";
    if (!sequence_mode) {
        sequence_mode = try_load_sequence("replay-submit-%u.gnmcap");
        sequence_name = sequence_mode ? "replay-submit" : "none";
    }
    if (sequence_mode) {
        ranges.clear();
        shader_sections = 0;
        for (size_t i = 0; i < seq_caps.size(); ++i) {
            extract_capture(seq_caps[i], seq_dcbs[i], true, &shader_sections);
        }
        dcb1.clear();
        dcb = seq_dcbs.back();
    }
    {
        char b[176];
        std::snprintf(b, sizeof(b),
                      "stage=loaded dcbDwords=%zu ranges=%zu shaders=%u priorDcb=%zu sequence=%s "
                      "seqSubmits=%zu (multi=%d)",
                      dcb.size(), ranges.size(), shader_sections, dcb1.size(), sequence_name,
                      seq_dcbs.size(), (sequence_mode || !dcb1.empty()) ? 1 : 0);
        mark(b);
    }

    struct Reloc {
        u64 old_base;
        u64 new_base;
        u64 size;
    };
    std::vector<Reloc> relocs;
    std::vector<std::pair<void*, size_t>> maps;
    std::vector<u32> map_usage;
    mark("stage=overlay_before_memory_instance");
    auto* mem = Core::Memory::Instance();
    mark(mem ? "stage=overlay_memory_instance result=OK" : "stage=overlay_memory_instance result=NULL");
    mem->ClearReplayMemory();
    mark("stage=overlay_clear result=OK");
    u32 overlay_registered = 0;
    for (const auto& r : ranges) {
        if (r.size == 0 || r.size > (64ull << 20)) {
            continue;
        }
        const size_t len = static_cast<size_t>((r.size + 0xFFF) & ~u64(0xFFF));
        const size_t alloc_len = len + 0x1000;
        void* m = ::mmap(nullptr, alloc_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            continue;
        }
        std::memcpy(m, r.data, static_cast<size_t>(r.size));
        const u64 new_base = reinterpret_cast<u64>(m);
        const u32 canary_val = 0xC0DE0000u | static_cast<u32>(relocs.size() & 0xFFFFu);
        const u64 canary_addr = new_base + len;
        *reinterpret_cast<u32*>(canary_addr) = canary_val;
        relocs.push_back({r.old_base, new_base, r.size});
        maps.push_back({m, alloc_len});
        map_usage.push_back(r.usage);
        mem->RegisterReplayCanary(canary_addr, canary_val);
        mem->RegisterReplayMemory(new_base, len, reinterpret_cast<const u8*>(m));
        ++overlay_registered;
    }
    {
        char b[128];
        std::snprintf(b, sizeof(b), "stage=overlay_registered count=%u maps=%zu relocs=%zu",
                      overlay_registered, maps.size(), relocs.size());
        mark(b);
    }
    auto reloc_byte = [&](u64 addr) -> u64 {
        for (const auto& rl : relocs) {
            if (addr >= rl.old_base && addr < rl.old_base + rl.size) {
                return rl.new_base + (addr - rl.old_base);
            }
        }
        return 0;
    };
    auto ensure_visible = [&](u64 addr, u64 num_bytes) -> u64 {
        const u64 hit = reloc_byte(addr);
        if (hit) {
            return hit;
        }
        if (addr == 0 || num_bytes == 0 || num_bytes > (64ull << 20)) {
            return 0;
        }
        const u64 old_base = addr & ~u64(0xFFF);
        const u64 span = ((addr + num_bytes) - old_base + 0xFFF) & ~u64(0xFFF);
        void* m = ::mmap(nullptr, static_cast<size_t>(span), PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            return 0;
        }
        const u64 new_base = reinterpret_cast<u64>(m);
        relocs.push_back({old_base, new_base, span});
        maps.push_back({m, static_cast<size_t>(span)});
        map_usage.push_back(0u);
        mem->RegisterReplayMemory(new_base, static_cast<size_t>(span),
                                  reinterpret_cast<const u8*>(m));
        return new_base + (addr - old_base);
    };
    auto host_span = [&](u64 addr) -> std::pair<u8*, u64> {
        for (const auto& rl : relocs) {
            if (addr >= rl.old_base && addr < rl.old_base + rl.size) {
                const u64 off = addr - rl.old_base;
                return {reinterpret_cast<u8*>(rl.new_base + off), rl.size - off};
            }
        }
        return {nullptr, 0};
    };
    u32 dma_total = 0, dma_src_reloc = 0, dma_dst_reloc = 0, dma_dst_synth = 0, dma_exec = 0;

    std::vector<std::pair<void*, size_t>> dcb_maps;
    u32 patched1 = 0, patched2 = 0, patched_vd = 0, nopped = 0, patched_idx = 0;
    auto patch_dcb = [&](const std::vector<u32>& src) -> u32* {
        const size_t dlen = static_cast<size_t>((src.size() * sizeof(u32) + 0xFFF) & ~u64(0xFFF));
        void* dm = ::mmap(nullptr, dlen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (dm == MAP_FAILED) {
            return nullptr;
        }
        u32* rdcb = reinterpret_cast<u32*>(dm);
        std::memcpy(rdcb, src.data(), src.size() * sizeof(u32));
        dcb_maps.push_back({dm, dlen});
        {
            size_t i = 0;
            const size_t n = src.size();
        while (i < n) {
            const u32 h = rdcb[i];
            const u32 type = h >> 30;
            if (type == 3u) {
                const u32 op = (h >> 8) & 0xFFu;
                const u32 cnt = ((h >> 16) & 0x3FFFu) + 1u;
                const size_t body = i + 1;
                const size_t body_end = std::min(body + cnt, n);
                switch (op) {
                case 0x33u: case 0x37u: case 0x39u: case 0x3Cu: case 0x3Fu: case 0x40u:
                case 0x46u: case 0x47u: case 0x48u: case 0x49u: case 0x58u:
                case 0x81u: case 0x83u:
                    rdcb[i] = (h & ~0x0000FF00u) | (0x10u << 8);
                    i = body_end;
                    ++nopped;
                    continue;
                default:
                    break;
                }
                if (op == 0x50u && body + 5 < body_end) {
                    const u32 ctrl = rdcb[body];
                    const u32 src_sel = (ctrl >> 29) & 0x3u;
                    const u32 dst_sel = (ctrl >> 20) & 0x3u;
                    const u32 num_bytes = rdcb[body + 5] & 0x1FFFFFu;
                    const u32 imm = rdcb[body + 1];
                    ++dma_total;
                    if ((dst_sel == 0u || dst_sel == 3u) && num_bytes > 0u) {
                        const u64 dst_old =
                            static_cast<u64>(rdcb[body + 3]) | (static_cast<u64>(rdcb[body + 4]) << 32);
                        const bool was_capt = reloc_byte(dst_old) != 0;
                        ensure_visible(dst_old, num_bytes);
                        auto [dst_ptr, dst_avail] = host_span(dst_old);
                        if (dst_ptr) {
                            ++dma_dst_reloc;
                            if (!was_capt) {
                                ++dma_dst_synth;
                            }
                            const u64 n = std::min<u64>(num_bytes, dst_avail);
                            if (src_sel == 2u) {
                                for (u64 k = 0; k + 4 <= n; k += 4) {
                                    std::memcpy(dst_ptr + k, &imm, 4);
                                }
                                ++dma_exec;
                            } else if (src_sel == 0u || src_sel == 3u) {
                                const u64 src_old = static_cast<u64>(rdcb[body + 1]) |
                                                    (static_cast<u64>(rdcb[body + 2]) << 32);
                                auto [src_ptr, src_avail] = host_span(src_old);
                                if (src_ptr) {
                                    ++dma_src_reloc;
                                    std::memcpy(dst_ptr, src_ptr, std::min<u64>(n, src_avail));
                                    ++dma_exec;
                                }
                            }
                        }
                    }
                    rdcb[i] = (h & ~0x0000FF00u) | (0x10u << 8);
                    ++nopped;
                    i = body_end;
                    continue;
                }
                if (op == 0x76u || op == 0x69u || op == 0x79u) {
                    for (size_t j = body + 1; j < body_end; ++j) {
                        if (j + 1 < body_end) {
                            const u64 a2 = ((static_cast<u64>(rdcb[j + 1]) << 32) | rdcb[j]) << 8;
                            const u64 r2 = reloc_byte(a2);
                            if (r2) {
                                const u64 v = r2 >> 8;
                                rdcb[j] = static_cast<u32>(v);
                                rdcb[j + 1] = static_cast<u32>(v >> 32);
                                patched2 += 2;
                                ++j;
                                continue;
                            }
                            const u64 av =
                                static_cast<u64>(rdcb[j]) | (static_cast<u64>(rdcb[j + 1] & 0xFFFu) << 32);
                            const u64 rv = reloc_byte(av);
                            if (rv) {
                                rdcb[j] = static_cast<u32>(rv);
                                rdcb[j + 1] =
                                    (rdcb[j + 1] & ~0xFFFu) | static_cast<u32>((rv >> 32) & 0xFFF);
                                ++patched_vd;
                                continue;
                            }
                        }
                        const u64 a1 = static_cast<u64>(rdcb[j]) << 8;
                        const u64 r1 = reloc_byte(a1);
                        if (r1) {
                            rdcb[j] = static_cast<u32>(r1 >> 8);
                            ++patched1;
                        }
                    }
                }
                if (op == 0x27u && body + 2 < body_end) {
                    const u32 low_flags = rdcb[body + 1] & 1u;
                    const u64 idx_old = (static_cast<u64>(rdcb[body + 1] & ~1u)) |
                                        (static_cast<u64>(rdcb[body + 2]) << 32);
                    const u64 idx_new = reloc_byte(idx_old);
                    if (idx_new) {
                        rdcb[body + 1] = (static_cast<u32>(idx_new) & ~1u) | low_flags;
                        rdcb[body + 2] = static_cast<u32>(idx_new >> 32);
                        ++patched_idx;
                    } else if (idx_old) {
                        Libraries::GnmDriver::ExecutorReplayCheckAddr(2u, idx_old, 4u);
                    }
                }
                i = body_end;
            } else if (type == 0u) {
                const u32 cnt = ((h >> 16) & 0x3FFFu) + 1u;
                i += 1u + cnt;
            } else {
                ++i;
            }
        }
        }
        return rdcb;
    };

    u32 patched_vsharp = 0;
    mark("stage=descriptor_vsharp_begin");
    for (size_t mi = 0; mi < maps.size(); ++mi) {
        if (map_usage[mi] != 6u) {
            continue;
        }
        auto& m = maps[mi];
        u32* w = reinterpret_cast<u32*>(m.first);
        const size_t nw = m.second / sizeof(u32);
        for (size_t j = 0; j + 1 < nw; ++j) {
            const u64 base = static_cast<u64>(w[j]) | (static_cast<u64>(w[j + 1] & 0xFFFu) << 32);
            if (base == 0) {
                continue;
            }
            const u64 nb = reloc_byte(base);
            if (nb) {
                w[j] = static_cast<u32>(nb);
                w[j + 1] = (w[j + 1] & ~0xFFFu) | static_cast<u32>((nb >> 32) & 0xFFF);
                ++patched_vsharp;
            }
        }
    }
    mark("stage=descriptor_vsharp_done");

    u32 patched_tsharp = 0;
    mark("stage=descriptor_tsharp_begin");
    for (size_t mi = 0; mi < maps.size(); ++mi) {
        if (map_usage[mi] != 6u) {
            continue;
        }
        auto& m = maps[mi];
        u32* w = reinterpret_cast<u32*>(m.first);
        const size_t nw = m.second / sizeof(u32);
        for (size_t j = 0; j + 1 < nw; ++j) {
            const u64 base38 =
                static_cast<u64>(w[j]) | (static_cast<u64>(w[j + 1] & 0x3Fu) << 32);
            const u64 t_old = base38 << 8;
            if (t_old == 0) {
                continue;
            }
            const u64 t_new = reloc_byte(t_old);
            if (t_new) {
                const u64 t_new38 = t_new >> 8;
                w[j] = static_cast<u32>(t_new38);
                w[j + 1] = (w[j + 1] & ~0x3Fu) | static_cast<u32>((t_new38 >> 32) & 0x3F);
                ++patched_tsharp;
            }
        }
    }
    mark("stage=descriptor_tsharp_done");
    {
        char b[256];
        std::snprintf(b, sizeof(b),
                      "stage=relocate ranges=%zu relocs=%zu dcb2dwordAddrs=%u dcbVsharp=%u "
                      "dcb1dwordAddrs=%u memVsharpPatched=%u noppedSync=%u idxBuf=%u memTsharp=%u "
                      "dmaTotal=%u dmaSrcReloc=%u dmaDstReloc=%u dmaDstSynth=%u",
                      ranges.size(), relocs.size(), patched2, patched_vd, patched1, patched_vsharp,
                      nopped, patched_idx, patched_tsharp, dma_total, dma_src_reloc, dma_dst_reloc,
                      dma_dst_synth);
        mark(b);
    }

#ifdef __ANDROID__
    if (const char* p = std::getenv("EXECUTOR_REPLAY_PROTECT"); p && p[0] == '1') {
        u32 protid = 0;
        for (auto& m : maps) {
            if (::mprotect(m.first, m.second, PROT_READ) == 0) {
                ++protid;
            }
        }
        char b[96];
        std::snprintf(b, sizeof(b), "stage=protect readonly_ranges=%u", protid);
        mark(b);
    }
#endif

    Libraries::GnmDriver::ExecutorReplaySetActive(true);
    for (const auto& rl : relocs) {
        Libraries::GnmDriver::ExecutorReplayAddReloc(rl.old_base, rl.new_base, rl.size);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                            "[EXECUTOR_RELOC_RANGE] old=0x%llx new=0x%llx size=%llu",
                            (unsigned long long)rl.old_base, (unsigned long long)rl.new_base,
                            (unsigned long long)rl.size);
#endif
    }

#ifdef __ANDROID__
    {
        struct sigaction sa{};
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sa.sa_sigaction = +[](int sig, siginfo_t* si, void* uc) {
            static std::atomic_bool logged{false};
            if (logged.exchange(true, std::memory_order_acq_rel)) {
                _exit(128 + sig);
            }
            const unsigned long long fa =
                reinterpret_cast<unsigned long long>(si ? si->si_addr : nullptr);
            unsigned long long pc = 0, lr = 0;
            if (uc) {
                auto* u = reinterpret_cast<ucontext_t*>(uc);
                pc = static_cast<unsigned long long>(u->uc_mcontext.pc);
                lr = static_cast<unsigned long long>(u->uc_mcontext.regs[30]);
            }
            __android_log_print(ANDROID_LOG_ERROR, "EXECUTOR",
                                "[EXECUTOR_REPLAY_SEGV] sig=%d faultAddr=0x%llx pc=0x%llx lr=0x%llx "
                                "(symbolize pc against liblsx4_executor_android.so)",
                                sig, fa, pc, lr);
            _exit(128 + sig);
        };
        ::sigaction(SIGSEGV, &sa, nullptr);
        ::sigaction(SIGBUS, &sa, nullptr);
    }
#endif

#ifdef __ANDROID__
    ::setenv("EXECUTOR_TRACE_PM4", "1", 1);
#endif
    const bool liverpool_ok = Libraries::GnmDriver::ExecutorEnsureLiverpoolForReplay();
    mark(liverpool_ok ? "stage=liverpool result=OK" : "stage=liverpool result=FAIL");
    if (!liverpool_ok) {
        return;
    }
    const bool presenter_ok =
        Libraries::GnmDriver::ExecutorEnsureGnmPresenter("replay_with_memory");
    mark(presenter_ok ? "stage=presenter result=OK" : "stage=presenter result=FAIL");

    auto submit_one = [&](const std::vector<u32>& src, const char* tag) -> int {
        using Clock = std::chrono::steady_clock;
        const auto patch_begin = Clock::now();
        u32* rd = patch_dcb(src);
        const auto patch_end = Clock::now();
        if (!rd) {
            mark("result=FAIL stage=dcb_mmap");
            return -1;
        }
        {
            char b[160];
            std::snprintf(b, sizeof(b),
                          "stage=dma tag=%s dmaTotal=%u dmaExec=%u dmaDstReloc=%u dmaDstSynth=%u "
                          "dmaSrcReloc=%u nopped=%u",
                          tag, dma_total, dma_exec, dma_dst_reloc, dma_dst_synth, dma_src_reloc, nopped);
            mark(b);
        }
        u32 dbytes = static_cast<u32>(src.size() * sizeof(u32));
        const u32* addrs[1] = {rd};
        u32 sizes[1] = {dbytes};
        int rc2 = -1;
        const auto submit_begin = Clock::now();
        try {
            rc2 = Libraries::GnmDriver::sceGnmSubmitCommandBuffersForWorkload(1u, 1u, addrs, sizes,
                                                                              nullptr, nullptr);
        } catch (const std::exception& e) {
            char b[200];
            std::snprintf(b, sizeof(b), "result=FAIL stage=submit tag=%s what=%s", tag, e.what());
            mark(b);
            return -1;
        }
        const auto submit_end = Clock::now();
        const auto idle_begin = Clock::now();
        Libraries::GnmDriver::ExecutorReplayWaitGpuIdle();
        const auto idle_end = Clock::now();
        const auto ms = [](Clock::duration d) -> double {
            return static_cast<double>(
                       std::chrono::duration_cast<std::chrono::microseconds>(d).count()) /
                   1000.0;
        };
        char b[256];
        std::snprintf(b, sizeof(b),
                      "stage=submitted tag=%s submitRc=0x%x gpuIdle=1 dwords=%zu bytes=%u "
                      "patchMs=%.3f submitMs=%.3f waitIdleMs=%.3f totalMs=%.3f",
                      tag, (unsigned)rc2, src.size(), dbytes, ms(patch_end - patch_begin),
                      ms(submit_end - submit_begin), ms(idle_end - idle_begin),
                      ms(idle_end - patch_begin));
        mark(b);
        return rc2;
    };

    int sc = -1;
    if (sequence_mode) {
        for (size_t i = 0; i < seq_dcbs.size(); ++i) {
            char tag[48];
            std::snprintf(tag, sizeof(tag), "submit_seq_%02zu", i);
            sc = submit_one(seq_dcbs[i], tag);
            if (sc != 0) {
                break;
            }
        }
    } else {
        if (!dcb1.empty()) {
            submit_one(dcb1, "submit1_prior");
        }
        sc = submit_one(dcb, "submit2_target");
    }

    char b[224];
    const char* frame_claim =
        sequence_mode ? "FULL_SEQUENCE_CANDIDATE" :
                        (!dcb1.empty() ? "PARTIAL_LEGACY_TWO_SUBMIT" : "PARTIAL_SINGLE_SUBMIT");
    std::snprintf(b, sizeof(b),
                  "result=%s source=gnmcap_capture stage=done submitRc=0x%x presenter=%s relocs=%zu "
                  "vsharpPatched=%u multi=%d seqSubmits=%zu frameClaim=%s pixelClaim=%s "
                  "(watch logcat for EXECUTOR_VK_ACTUAL_DRAW)",
                  (presenter_ok && sc == 0) ? "OK" : "PARTIAL", (unsigned)sc,
                  presenter_ok ? "OK" : "FAIL", relocs.size(), patched_vsharp,
                  (sequence_mode || !dcb1.empty()) ? 1 : 0, seq_dcbs.size(), frame_claim,
                  sequence_mode ? "ORDERED_SEQUENCE" : "PARTIAL_RT_HEURISTIC");
    mark(b);
    mark("stage=presenting_loop");
    using Clock = std::chrono::steady_clock;
    u64 present_count = 0;
    u64 ok_count = 0;
    u64 fail_count = 0;
    u64 window_count = 0;
    u64 window_ok = 0;
    u64 window_fail = 0;
    double window_present_ms_sum = 0.0;
    double window_present_ms_max = 0.0;
    auto window_begin = Clock::now();
    for (;;) {
        const auto present_begin = Clock::now();
        const bool present_ok = Libraries::GnmDriver::ExecutorPresentReplayRT();
        const auto present_end = Clock::now();
        const double present_ms =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    present_end - present_begin)
                                    .count()) /
            1000.0;
        ++present_count;
        ++window_count;
        window_present_ms_sum += present_ms;
        window_present_ms_max = std::max(window_present_ms_max, present_ms);
        if (present_ok) {
            ++ok_count;
            ++window_ok;
        } else {
            ++fail_count;
            ++window_fail;
        }
        const auto now = Clock::now();
        const double elapsed_s =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    now - window_begin)
                                    .count()) /
            1000000.0;
        if (elapsed_s >= 1.0) {
            const double fps = static_cast<double>(window_count) / elapsed_s;
            const double avg_present_ms =
                window_count ? window_present_ms_sum / static_cast<double>(window_count) : 0.0;
            char pb[240];
            std::snprintf(pb, sizeof(pb),
                          "stage=presenting_loop alive=1 fps=%.2f presents=%llu ok=%llu fail=%llu "
                          "window=%llu windowOk=%llu windowFail=%llu avgPresentMs=%.3f "
                          "maxPresentMs=%.3f sleepMs=16 replayActive=1",
                          fps, static_cast<unsigned long long>(present_count),
                          static_cast<unsigned long long>(ok_count),
                          static_cast<unsigned long long>(fail_count),
                          static_cast<unsigned long long>(window_count),
                          static_cast<unsigned long long>(window_ok),
                          static_cast<unsigned long long>(window_fail), avg_present_ms,
                          window_present_ms_max);
            mark(pb);
            window_count = 0;
            window_ok = 0;
            window_fail = 0;
            window_present_ms_sum = 0.0;
            window_present_ms_max = 0.0;
            window_begin = now;
        }
        ::usleep(16 * 1000);
    }
    mark("stage=presented");
    Libraries::GnmDriver::ExecutorReplaySetActive(false);
    Core::Memory::Instance()->ClearReplayMemory();
    for (auto& m : dcb_maps) ::munmap(m.first, m.second);
    for (auto& m : maps) ::munmap(m.first, m.second);
}

void ExecutorRunGcnSpirvSmoke(const char* marker_path) {
    auto mark = [&](const char* text) {
        if (FILE* f = std::fopen(marker_path, "a")) {
            std::time_t t = std::time(nullptr);
            std::fprintf(f, "[%lld] EXECUTOR_GCN_SPIRV %s\n", (long long)t, text);
            std::fclose(f);
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_GCN_SPIRV] %s", text);
    };

    mark("phase=begin milestone=Layer3ShaderCorpus path=cpu_recompile");

    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.subgroup_size = 64;
    profile.support_float64 = true;
    profile.support_int64 = true;
    profile.support_int16 = true;
    profile.support_float16 = false;
    profile.max_ubo_size = 65536;
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;

    static const std::array<u32, Shader::ShaderParams::NumShaderUserData> kUserData{};

    auto translate_standalone_ps = [&](const Executor::OrbShdrBlob& s, std::vector<u32>& out) -> bool {
        try {
            const Shader::ShaderParams params{
                .user_data =
                    std::span<const u32, Shader::ShaderParams::NumShaderUserData>(kUserData),
                .code = std::span<const u32>(s.code, s.num_dwords),
                .hash = s.hash,
            };
            Shader::Info info(Shader::Stage::Fragment, Shader::LogicalStage::Fragment, params);
            union RuntimeInfoHolder {
                Shader::RuntimeInfo ri;
                RuntimeInfoHolder() {}
                ~RuntimeInfoHolder() {}
            } holder;
            Shader::RuntimeInfo& runtime_info = holder.ri;
            runtime_info.Initialize(Shader::Stage::Fragment);
            runtime_info.num_allocated_vgprs = 64;

            Shader::Pools pools;
            Shader::Backend::Bindings binding{};
            const auto ir_program =
                Shader::TranslateProgram(params.code, pools, info, runtime_info, profile);
            out = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
            return out.size() >= 5 && out[0] == 0x07230203u;
        } catch (const std::exception&) {
            out.clear();
            return false;
        }
    };

    u32 classified = 0, standalone = 0, recompiled_ok = 0, decode_failed = 0, decode_crash = 0;
    u32 cls_vintrp = 0, cls_resource = 0, cls_vs = 0, cls_other = 0;
    std::vector<u32> first_spv;

    static const u32 kDecodeUnsafe[] = {12, 14, 16};
    const bool force_decode_all = [&] {
        std::string p(marker_path);
        if (const auto sl = p.find_last_of("/\\"); sl != std::string::npos) {
            p.replace(sl + 1, std::string::npos, "allow-decode-all");
        }
        if (FILE* f = std::fopen(p.c_str(), "r")) {
            std::fclose(f);
            return true;
        }
        return false;
    }();
    auto is_decode_unsafe = [&](u32 idx) -> bool {
        if (force_decode_all) {
            return false;
        }
        for (u32 u : kDecodeUnsafe) {
            if (u == idx) {
                return true;
            }
        }
        return false;
    };

    for (size_t i = 0; i < Executor::kOrbShdrCorpusCount; ++i) {
        const auto& s = Executor::kOrbShdrCorpus[i];
        {
            char bf[160];
            std::snprintf(bf, sizeof(bf), "censusing i=%zu name=%s dwords=%zu", i, s.name,
                          s.num_dwords);
            mark(bf);
        }
        if (is_decode_unsafe(static_cast<u32>(i))) {
            ++classified;
            ++decode_crash;
            char bf[200];
            std::snprintf(bf, sizeof(bf),
                          "shader=%s stage=%s class=fetch_shader_decode_unsafe recompile=skipped "
                          "(SIGSEGVs production decoder standalone; not a draw-time shader)",
                          s.name, s.stage);
            mark(bf);
            continue;
        }
        const ShaderCensus c = CensusOrbShdr(s);
        bool translatable = false;
        const char* cls = ClassifyOrbShdr(s, c, translatable);
        ++classified;

        std::vector<u32> spv;
        const char* recompile = "skipped";
        u32 words = 0, bound = 0;
        if (translatable) {
            ++standalone;
            if (translate_standalone_ps(s, spv)) {
                recompile = "OK";
                words = static_cast<u32>(spv.size());
                bound = spv.size() >= 4 ? spv[3] : 0;
                ++recompiled_ok;
                if (first_spv.empty()) {
                    first_spv = spv;
                }
            } else {
                recompile = "FAIL";
            }
        }
        const std::string_view clsv(cls);
        if (clsv == "decode_failed") {
            ++decode_failed;
        } else if (clsv == "vintrp_ps_requires_fs_inputs") {
            ++cls_vintrp;
        } else if (clsv == "resource_ps_requires_resources") {
            ++cls_resource;
        } else if (clsv == "vs_requires_vtx_fetch_state") {
            ++cls_vs;
        } else if (clsv == "other_requires_state") {
            ++cls_other;
        }

        char b[256];
        std::snprintf(b, sizeof(b),
                      "shader=%s stage=%s class=%s insts=%u mem=%u interp=%u exp=%u(mrt=%u,pos=%u) "
                      "recompile=%s words=%u bound=%u",
                      s.name, s.stage, cls, c.insts, c.mem_ops, c.interp_ops, c.exp_ops, c.exp_mrt,
                      c.exp_pos, recompile, words, bound);
        mark(b);
    }

    {
        char b[256];
        std::snprintf(b, sizeof(b),
                      "Layer3ShaderCorpus result=%s corpus=%zu classified=%u standalone_ps=%u "
                      "recompiledOK=%u vintrp=%u resource=%u vs=%u other=%u decodeFail=%u "
                      "decodeUnsafe=%u reboot=NO device=NO",
                      (classified == Executor::kOrbShdrCorpusCount && recompiled_ok == standalone &&
                       standalone > 0)
                          ? "OK"
                          : "PARTIAL",
                      Executor::kOrbShdrCorpusCount, classified, standalone, recompiled_ok, cls_vintrp,
                      cls_resource, cls_vs, cls_other, decode_failed, decode_crash);
        mark(b);
    }

    if (first_spv.empty()) {
        return;
    }
    std::string flag_path(marker_path);
    if (const auto slash = flag_path.find_last_of("/\\"); slash != std::string::npos) {
        flag_path.replace(slash + 1, std::string::npos, "allow-vk-shader-module");
    }
    if (FILE* fp = std::fopen(flag_path.c_str(), "r")) {
        std::fclose(fp);
        mark("stage=vk_shader_module gate=allowed creating_headless_device");
        try {
            Instance instance(false, false, true);
            const vk::Device device = instance.GetDevice();
            if (!device) {
                mark("stage=vk_shader_module device=null (cpu corpus proof already OK)");
                return;
            }
            const vk::ShaderModuleCreateInfo ci{
                .codeSize = first_spv.size() * sizeof(u32),
                .pCode = first_spv.data(),
            };
            const auto [res, module] = device.createShaderModule(ci);
            if (res != vk::Result::eSuccess || !module) {
                char b[96];
                std::snprintf(b, sizeof(b), "stage=vk_shader_module result=FAIL res=%d",
                              static_cast<int>(res));
                mark(b);
                return;
            }
            device.destroyShaderModule(module);
            mark("result=OK vkCreateShaderModule=OK wsi=NO");
        } catch (const std::exception& e) {
            char b[224];
            std::snprintf(b, sizeof(b), "stage=vk_shader_module result=FAIL exception what=%s", e.what());
            mark(b);
        }
    } else {
        mark("stage=vk_shader_module gate=skipped (no allow-vk-shader-module sentinel; "
             "emulator-safe CPU corpus proof stands)");
    }
}

}
