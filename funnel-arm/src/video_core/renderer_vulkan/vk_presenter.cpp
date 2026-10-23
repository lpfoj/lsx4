// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#endif

#include "common/assert.h"
#include "common/config.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "common/scope_exit.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/libraries/system/systemservice.h"
#include "imgui/renderer/imgui_core.h"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "sdl_window.h"
#include "video_core/invalidation_overlap_trace.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_legacy_sync.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image.h"
#include "executor/android_performance_hint.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <imgui.h>
#include <mutex>
#include <vk_mem_alloc.h>
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <unistd.h>
#endif

namespace Vulkan {

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {
std::atomic<std::uint64_t> g_present_imgui_ns{0};

bool ExecutorAndroidRendererProbesEnabled() {
    static const bool enabled =
        std::getenv("EXECUTOR_ANDROID_RENDERER_PROBES") != nullptr ||
        ::access("/data/data/app.lsx4.android/files/lsx4-home/diag-renderer-probes",
                 F_OK) == 0;
    return enabled;
}

bool ExecutorPresenterSwapchainGradientEnabled() {
    static const bool enabled =
        std::getenv("EXECUTOR_DIAGNOSTIC_PRESENTER_SWAPCHAIN_GRADIENT") != nullptr;
    return enabled;
}

// Proof-only fault injection for the historical 40:38 present-fence stall.
// The marker is absent in production and the cached false path performs no
// clock read, file access or atomic operation.  When explicitly armed before
// process start, exactly eight 250 ms timeout slices are injected once, after
// which the real Vulkan wait resumes.  This exercises the same ownership and
// keep-waiting path as a driver eTimeout without manufacturing a Vulkan error.
bool ExecutorInjectPresentFenceTimeout() {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-vk-inject-present-fence-timeouts",
                 F_OK) == 0;
    if (!enabled) {
        return false;
    }
    static std::atomic<u32> remaining{8};
    u32 observed = remaining.load(std::memory_order_relaxed);
    while (observed != 0) {
        if (remaining.compare_exchange_weak(observed, observed - 1,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
            const u32 slice = 8 - observed + 1;
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_VK_FENCE_STALL_INJECT] slice=%u of=8 delayMs=250 "
                "result=eTimeout proofOnly=1",
                slice);
            usleep(250'000);
            return true;
        }
    }
    return false;
}

enum class PresenterCpuStage : std::size_t {
    Acquire,
    CommandStart,
    Record,
    Flush,
    QueuePresent,
    Count,
};

enum class PresenterCpuRoute : std::size_t {
    Unknown,
    HostUi,
    DirectComposition,
    Gradient,
    Count,
};

constexpr std::size_t PresenterCpuStageCount =
    static_cast<std::size_t>(PresenterCpuStage::Count);
constexpr std::size_t PresenterCpuRouteCount =
    static_cast<std::size_t>(PresenterCpuRoute::Count);

bool ExecutorPresentStageProfileEnabled() {
    // This is intentionally marker-only and cached for the lifetime of the process. With the
    // marker absent, the predictable stage guards never read the clock or touch aggregate state.
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-present-stage-profile",
                 F_OK) == 0;
    return enabled;
}

u64 ExecutorPresentStageNowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

struct PresenterCpuStageTotals {
    std::mutex mutex;
    std::array<u64, PresenterCpuStageCount> stage_ns{};
    std::array<u64, PresenterCpuRouteCount> route_samples{};
    u64 total_ns{};
    u64 samples{};
    u64 queue_present_calls{};
    u64 queue_present_ok{};
};

PresenterCpuStageTotals& ExecutorPresentStageTotals() {
    static PresenterCpuStageTotals totals;
    return totals;
}

class PresenterCpuStageProfile {
public:
    explicit PresenterCpuStageProfile(const bool enabled) : enabled_{enabled} {
        if (enabled_) {
            frame_begin_ns_ = ExecutorPresentStageNowNs();
        }
    }

    ~PresenterCpuStageProfile() {
        if (!enabled_) {
            return;
        }
        const u64 frame_end_ns = ExecutorPresentStageNowNs();
        EndActiveStage(frame_end_ns);
        Commit(frame_end_ns - frame_begin_ns_);
    }

    void SetRoute(const PresenterCpuRoute route) noexcept {
        route_ = route;
    }

    void Begin(const PresenterCpuStage stage) {
        if (!enabled_) {
            return;
        }
        const u64 now_ns = ExecutorPresentStageNowNs();
        EndActiveStage(now_ns);
        active_stage_ = stage;
        active_stage_begin_ns_ = now_ns;
    }

    void End(const PresenterCpuStage stage) {
        if (!enabled_ || active_stage_ != stage) {
            return;
        }
        EndActiveStage(ExecutorPresentStageNowNs());
    }

    void RecordQueuePresentResult(const bool ok) noexcept {
        if (!enabled_) {
            return;
        }
        queue_present_called_ = true;
        queue_present_ok_ = ok;
    }

private:
    void EndActiveStage(const u64 now_ns) noexcept {
        if (active_stage_ == PresenterCpuStage::Count) {
            return;
        }
        const auto index = static_cast<std::size_t>(active_stage_);
        stage_ns_[index] += now_ns - active_stage_begin_ns_;
        active_stage_ = PresenterCpuStage::Count;
    }

    void Commit(const u64 frame_ns) const {
        // Calls which left before selecting a real composition route or before calling
        // vkQueuePresent are not frames. Excluding them prevents startup/recovery activity from
        // diluting the stage averages.
        if (route_ == PresenterCpuRoute::Unknown || !queue_present_called_) {
            return;
        }

        auto& totals = ExecutorPresentStageTotals();
        std::scoped_lock lock{totals.mutex};
        for (std::size_t index = 0; index < PresenterCpuStageCount; ++index) {
            totals.stage_ns[index] += stage_ns_[index];
        }
        totals.total_ns += frame_ns;
        ++totals.route_samples[static_cast<std::size_t>(route_)];
        ++totals.queue_present_calls;
        if (queue_present_ok_) {
            ++totals.queue_present_ok;
        }

        const u64 samples = ++totals.samples;
        constexpr u64 PublishInterval = 120;
        if (samples < PublishInterval) {
            return;
        }

        u64 accounted_ns{};
        for (std::size_t index = 0; index < PresenterCpuStageCount; ++index) {
            accounted_ns += totals.stage_ns[index];
        }
        const u64 total_ns = totals.total_ns;
        const u64 other_ns = total_ns > accounted_ns ? total_ns - accounted_ns : 0;
        const auto stage_ms = [&](const PresenterCpuStage stage) {
            return static_cast<double>(totals.stage_ns[static_cast<std::size_t>(stage)]) /
                   static_cast<double>(samples) / 1.0e6;
        };
        const double total_ms = static_cast<double>(total_ns) /
                                static_cast<double>(samples) / 1.0e6;
        const double other_ms = static_cast<double>(other_ns) /
                                static_cast<double>(samples) / 1.0e6;
        const u64 queue_present_calls = totals.queue_present_calls;
        const u64 queue_present_ok = totals.queue_present_ok;
        const u64 route_unknown =
            totals.route_samples[static_cast<std::size_t>(PresenterCpuRoute::Unknown)];
        const u64 route_host_ui =
            totals.route_samples[static_cast<std::size_t>(PresenterCpuRoute::HostUi)];
        const u64 route_direct =
            totals.route_samples[static_cast<std::size_t>(PresenterCpuRoute::DirectComposition)];
        const u64 route_gradient =
            totals.route_samples[static_cast<std::size_t>(PresenterCpuRoute::Gradient)];

        std::array<char, 768> line{};
        std::snprintf(
            line.data(), line.size(),
            "frames=%llu queue_present_calls=%llu queue_present_ok=%llu "
            "route_host_ui=%llu route_direct=%llu route_gradient=%llu route_unknown=%llu "
            "total_avg_ms=%.3f acquire_avg_ms=%.3f command_start_avg_ms=%.3f "
            "record_avg_ms=%.3f flush_avg_ms=%.3f queue_present_avg_ms=%.3f "
            "other_avg_ms=%.3f",
            static_cast<unsigned long long>(samples),
            static_cast<unsigned long long>(queue_present_calls),
            static_cast<unsigned long long>(queue_present_ok),
            static_cast<unsigned long long>(route_host_ui),
            static_cast<unsigned long long>(route_direct),
            static_cast<unsigned long long>(route_gradient),
            static_cast<unsigned long long>(route_unknown), total_ms,
            stage_ms(PresenterCpuStage::Acquire),
            stage_ms(PresenterCpuStage::CommandStart), stage_ms(PresenterCpuStage::Record),
            stage_ms(PresenterCpuStage::Flush), stage_ms(PresenterCpuStage::QueuePresent),
            other_ms);
        if (FILE* out = std::fopen(
                "/data/data/app.lsx4.android/files/runtime/present-stage-profile.txt", "w")) {
            std::fputs(line.data(), out);
            std::fputc('\n', out);
            std::fclose(out);
        }
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_PRESENT_STAGE_PROFILE] %s", line.data());

        // Publish non-overlapping windows. The next valid frame starts from a completely clean
        // aggregate, so title/menu samples cannot remain in later gameplay reports.
        totals.stage_ns.fill(0);
        totals.route_samples.fill(0);
        totals.total_ns = 0;
        totals.samples = 0;
        totals.queue_present_calls = 0;
        totals.queue_present_ok = 0;
    }

    bool enabled_{};
    bool queue_present_called_{};
    bool queue_present_ok_{};
    PresenterCpuRoute route_{PresenterCpuRoute::Unknown};
    PresenterCpuStage active_stage_{PresenterCpuStage::Count};
    u64 frame_begin_ns_{};
    u64 active_stage_begin_ns_{};
    std::array<u64, PresenterCpuStageCount> stage_ns_{};
};
}
#else
namespace {
enum class PresenterCpuStage : std::size_t {
    Acquire,
    CommandStart,
    Record,
    Flush,
    QueuePresent,
};

enum class PresenterCpuRoute : std::size_t {
    Unknown,
    HostUi,
    DirectComposition,
    Gradient,
};

constexpr bool ExecutorPresentStageProfileEnabled() noexcept {
    return false;
}

class PresenterCpuStageProfile {
public:
    explicit PresenterCpuStageProfile(bool) noexcept {}
    void SetRoute(PresenterCpuRoute) noexcept {}
    void Begin(PresenterCpuStage) noexcept {}
    void End(PresenterCpuStage) noexcept {}
    void RecordQueuePresentResult(bool) noexcept {}
};
}
#endif

namespace {
std::atomic<u64> g_executor_live_present_count{0};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
std::atomic<u64> g_executor_guest_flip_count{0};
#endif
}

u64 ExecutorGetLivePresentCount() noexcept {
    return g_executor_live_present_count.load(std::memory_order_relaxed);
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
u64 ExecutorGetGuestFlipCount() noexcept {
    return g_executor_guest_flip_count.load(std::memory_order_relaxed);
}

void ExecutorNotifyGuestFlip() noexcept {
    g_executor_guest_flip_count.fetch_add(1, std::memory_order_relaxed);
}
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
static void ExecutorLiveDirectPresentStats(bool present_ok, u32 src_width, u32 src_height,
                                           u32 dst_width, u32 dst_height, u32 target) {
    // The live present counter and ADPF maintenance are runtime inputs.  The
    // rate window below is diagnostic-only and must not add a clock read plus
    // a contended mutex to every production present.
    if (present_ok) {
        g_executor_live_present_count.fetch_add(1, std::memory_order_relaxed);
        Executor::AndroidPerformanceHint::MaintainSessionThreads();
    }
    if (VideoCore::IsExecutorLiveQuiet()) {
        return;
    }

    using Clock = std::chrono::steady_clock;
    static std::mutex stats_mutex;
    static auto start = Clock::now();
    static auto last = start;
    static u64 total = 0;
    static u64 ok = 0;
    static u64 fail = 0;
    static u64 last_total = 0;
    static u64 last_ok = 0;
    static u64 last_fail = 0;

    const auto now = Clock::now();
    std::scoped_lock lock{stats_mutex};
    ++total;
    if (present_ok) {
        ++ok;
    } else {
        ++fail;
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last);
    if (elapsed_ms.count() < 1000) {
        return;
    }

    const double window_sec = static_cast<double>(elapsed_ms.count()) / 1000.0;
    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_LIVE_FPS] source=direct_present elapsedMs=%lld fps=%.2f okRate=%.2f "
        "failRate=%.2f presents=%llu ok=%llu fail=%llu src=%ux%u dst=%ux%u target=%u",
        static_cast<long long>(total_ms),
        static_cast<double>(total - last_total) / window_sec,
        static_cast<double>(ok - last_ok) / window_sec,
        static_cast<double>(fail - last_fail) / window_sec,
        static_cast<unsigned long long>(total), static_cast<unsigned long long>(ok),
        static_cast<unsigned long long>(fail), src_width, src_height, dst_width, dst_height,
        target);

    last = now;
    last_total = total;
    last_ok = ok;
    last_fail = fail;
}
#endif

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, Config::getGpuId(), Config::vkValidationEnabled(),
               Config::getVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, GpuTimelineSchedulerKind::GuestDraw},
      present_scheduler{instance, GpuTimelineSchedulerKind::Present},
      flip_scheduler{instance, GpuTimelineSchedulerKind::RepeatFlip},
      swapchain{instance, window},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Android's fixed-size Surface is the presentation contract. Starting the
    // frame pool at the desktop 1920x1080 default and then shrinking it from
    // the first ImGui content-area measurement left an old-size image in the
    // repeat/present ring. It showed up as an intermittent smaller, stale
    // frame with an uncleared strip on the right and bottom. Keep every frame
    // slot and every upscaler output at the swapchain extent from the first frame.
    const vk::Extent2D initial_extent = swapchain.GetExtent();
    expected_frame_width = initial_extent.width;
    expected_frame_height = initial_extent.height;
#endif

    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        frame.in_free_queue = true;
        free_queue.push(&frame);
    }

    fsr_settings.enable = Config::getFsrEnabled();
    fsr_settings.mode = Config::getUpscalerMode();
    fsr_settings.use_rcas = Config::getRcasEnabled();
    fsr_settings.rcas_attenuation = static_cast<float>(Config::getRcasAttenuation() / 1000.f);
    fsr_settings.sharp_bilinear_strength =
        static_cast<float>(Config::getSharpBilinearStrength() / 1000.f);
    fsr_settings.bicubic_kernel = static_cast<u32>(Config::getBicubicKernel());
    fsr_settings.sgsr1_edge_sharpness =
        static_cast<float>(Config::getSgsr1EdgeSharpness() / 1000.f);
    fsr_settings.nis_sharpness =
        static_cast<float>(Config::getNisSharpness() / 1000.f);

    if (instance.UseHostUiComposition()) {
        fsr_pass.Create(instance, instance.GetAllocator(), num_images);
        sgsr2_pass.Create(instance, instance.GetAllocator(), num_images);
        arm_asr_pass.Create(instance, instance.GetAllocator(), num_images);
        pp_pass.Create(instance, swapchain.GetSurfaceFormat().format, num_images);
    }

    if (instance.UseHostUiComposition()) {
        ImGui::Layer::AddLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
    }
}

Presenter::~Presenter() {
    if (instance.UseHostUiComposition()) {
        ImGui::Layer::RemoveLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
    }
    draw_scheduler.Finish(Scheduler::ExecutorFinishReason::PresenterShutdown);
    arm_asr_pass.Destroy();
    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
    if (instance.UseHostUiComposition()) {
        ImGui::Core::Shutdown(device);
    }
}

VideoCore::Image& Presenter::RegisterVideoOutSurface(
    const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
    vo_buffers_addr.emplace_back(cpu_address);
    auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
    // The proprietary A750 driver exposes a deterministic failure when the second display
    // buffer is registered after an UNORM render-target alias already exists at the same guest
    // address: compatible-format reuse loses the VideoOut identity and every second flip is
    // presented from that alias. Keep the established route on other devices, while requiring
    // the exact scanout descriptor for the affected capability profile. This changes neither
    // submission order nor synchronization.
    const bool exact_videoout_identity =
        instance.GetCapabilityProfile().exact_proprietary_adreno750;
    const auto image_id = texture_cache.FindImage(desc, exact_videoout_identity);
    auto& image = texture_cache.GetImage(image_id);
    image.usage.vo_surface = 1u;
    executor_videoout_width.store(image.info.size.width, std::memory_order_release);
    executor_videoout_height.store(image.info.size.height, std::memory_order_release);
    return image;
}

bool Presenter::IsDeviceLost() const noexcept {
    return device_lost.load(std::memory_order_acquire) || draw_scheduler.IsDeviceLost() ||
           present_scheduler.IsDeviceLost() || flip_scheduler.IsDeviceLost();
}

void Presenter::MarkDeviceLost(vk::Result result, const char* where) noexcept {
    if (result != vk::Result::eErrorDeviceLost) {
        return;
    }

    MarkTerminal(result, where);
}

void Presenter::MarkTerminal(vk::Result result, const char* where) noexcept {

    bool expected = false;
    if (!device_lost.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        return;
    }

    LOG_ERROR(Render_Vulkan, "[EXECUTOR_VK_TERMINAL] where={} result={} terminal=1", where,
              vk::to_string(result));
    draw_scheduler.MarkTerminal(result, "presenter_draw");
    present_scheduler.MarkTerminal(result, "presenter_present");
    flip_scheduler.MarkTerminal(result, "presenter_flip");
    free_cv.notify_all();
    frame_cv.notify_all();
}

bool Presenter::WaitForFence(vk::Fence fence, const char* where) noexcept {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    constexpr u64 WaitSlice = 250'000'000ULL;
    u32 timeout_slices = 0;
    for (;;) {
        const vk::Result result = ExecutorInjectPresentFenceTimeout()
                                      ? vk::Result::eTimeout
                                      : instance.GetDevice().waitForFences(fence, false, WaitSlice);
        if (result == vk::Result::eSuccess) {
            return true;
        }
        if (result == vk::Result::eTimeout) {
            ++timeout_slices;
            if (timeout_slices == 8 || (timeout_slices > 8 &&
                                        (timeout_slices & (timeout_slices - 1)) == 0)) {
                __android_log_print(
                    ANDROID_LOG_ERROR, "LSX4Native",
                    "[EXECUTOR_VK_FENCE_STALL] where=%s slices=%u elapsedMs=%llu "
                    "action=keep-waiting",
                    where, timeout_slices,
                    static_cast<unsigned long long>(timeout_slices) * 250ULL);
            }
            // A timeout only means that the fence has not signalled yet. It is
            // not a Vulkan device error and must not poison every scheduler via
            // the process-wide MasterSemaphore terminal bit. In particular, a
            // transient present stall used to make Liverpool reject the queued
            // guest-invalidation drain, leaving stack-owned requests asleep
            // forever. Keep ownership of the frame and wait until the fence is
            // safe to recycle, or until a real Vulkan error is reported.
            continue;
        }
        MarkTerminal(result, where);
        return false;
    }
#else
    const vk::Result result = instance.GetDevice().waitForFences(
        fence, false, std::numeric_limits<u64>::max());
    if (result == vk::Result::eSuccess) {
        return true;
    }
    if (result == vk::Result::eErrorDeviceLost) {
        MarkDeviceLost(result, where);
    } else {
        LOG_ERROR(Render_Vulkan, "Failed waiting for present fence: {}", vk::to_string(result));
    }
    return false;
#endif
}

bool Presenter::WaitForFrameReady(Frame* frame, const char* where) noexcept {
    if (frame->ready_semaphore || frame->ready_tick == 0) {
        return true;
    }
    if (frame->ready_master == nullptr) {
        LOG_ERROR(Render_Vulkan, "Legacy frame dependency has no master at {}", where);
        MarkTerminal(vk::Result::eErrorUnknown, where);
        return false;
    }
    frame->ready_master->Wait(frame->ready_tick);
    if (frame->ready_master->IsDeviceLost()) {
        MarkTerminal(vk::Result::eErrorDeviceLost, where);
        return false;
    }
    return true;
}

bool Presenter::AcquireSwapchainImage(const char* where) {
    if (swapchain.AcquireNextImage()) {
        return true;
    }
    if (swapchain.IsTerminal()) {
        MarkTerminal(swapchain.TerminalResult(), where);
        return false;
    }
    if (!swapchain.NeedsRecreation()) {
        return false;
    }
    swapchain.Recreate(window.GetWidth(), window.GetHeight());
    if (swapchain.AcquireNextImage()) {
        return true;
    }
    if (swapchain.IsTerminal()) {
        MarkTerminal(swapchain.TerminalResult(), where);
    }
    return false;
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    pp_pass.ReleaseFrame(frame->id);
    if (frame->imgui_texture) {
        ImGui::Vulkan::RemoveTexture(frame->imgui_texture);
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    if (instance.UseHostUiComposition()) {
        frame->imgui_texture =
            ImGui::Vulkan::AddTexture(view, vk::ImageLayout::eShaderReadOnlyOptimal);
    }
    frame->is_hdr = swapchain.GetHDR();
}

bool Presenter::HasReclaimableFrameLocked() const {
    return !free_queue.empty() ||
           (last_submit_frame != nullptr && last_submit_frame->repeat_readers == 0);
}

void Presenter::QueueFreeFrameLocked(Frame* frame) {
    ASSERT_MSG(frame != nullptr, "Cannot recycle a null present frame");
    ASSERT_MSG(frame != last_submit_frame,
               "Present frame {} is both retained and free", frame->id);
    ASSERT_MSG(frame->repeat_readers == 0,
               "Present frame {} recycled with {} repeat readers", frame->id,
               frame->repeat_readers);
    ASSERT_MSG(!frame->recycle_after_repeat,
               "Present frame {} queued while deferred recycle is active", frame->id);
    ASSERT_MSG(!frame->in_free_queue,
               "Present frame {} entered the free queue twice", frame->id);
    frame->in_free_queue = true;
    free_queue.push(frame);
}

void Presenter::RecycleFrame(Frame* frame) {
    {
        std::scoped_lock lock{free_mutex};
        QueueFreeFrameLocked(frame);
    }
    free_cv.notify_one();
}

void Presenter::PublishLastFrame(Frame* frame) {
    bool released_previous{};
    {
        std::scoped_lock lock{free_mutex};
        ASSERT_MSG(frame != nullptr, "Cannot retain a null present frame");
        ASSERT_MSG(frame != last_submit_frame,
                   "Present frame {} was published twice", frame->id);
        ASSERT_MSG(!frame->in_free_queue,
                   "Present frame {} published while free", frame->id);
        ASSERT_MSG(frame->repeat_readers == 0 && !frame->recycle_after_repeat,
                   "Present frame {} published with stale repeat ownership", frame->id);

        Frame* const previous = last_submit_frame;
        last_submit_frame = frame;
        if (previous != nullptr) {
            ASSERT_MSG(!previous->in_free_queue,
                       "Retained present frame {} was also free", previous->id);
            if (previous->repeat_readers == 0) {
                QueueFreeFrameLocked(previous);
                released_previous = true;
            } else {
                ASSERT_MSG(!previous->recycle_after_repeat,
                           "Present frame {} already awaits repeat release", previous->id);
                previous->recycle_after_repeat = true;
            }
        }
    }
    if (released_previous) {
        free_cv.notify_one();
    }
}

void Presenter::ReleaseRepeatFrame(Frame* frame) {
    bool frame_available{};
    {
        std::scoped_lock lock{free_mutex};
        ASSERT_MSG(frame != nullptr, "Cannot release a null repeated frame");
        ASSERT_MSG(frame->repeat_readers != 0,
                   "Present frame {} repeat ownership underflow", frame->id);
        --frame->repeat_readers;
        if (frame->repeat_readers == 0) {
            if (frame != last_submit_frame) {
                ASSERT_MSG(frame->recycle_after_repeat,
                           "Detached repeated frame {} has no recycle owner", frame->id);
                frame->recycle_after_repeat = false;
                QueueFreeFrameLocked(frame);
            }
            // Wake a producer even when this is still the retained frame: it may reclaim the
            // frame under pool pressure after repeat readers reach zero.
            frame_available = true;
        }
    }
    if (frame_available) {
        free_cv.notify_one();
    }
}

Frame* Presenter::PrepareLastFrame() {
    if (IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "prepare_last_frame");
        return nullptr;
    }

    Frame* frame{};
    {
        std::scoped_lock lock{free_mutex};
        frame = last_submit_frame;
        if (frame == nullptr) {
            return nullptr;
        }
        ASSERT_MSG(!frame->in_free_queue && !frame->recycle_after_repeat,
                   "Retained present frame {} has conflicting ownership", frame->id);
        ++frame->repeat_readers;
    }

    if (!WaitForFence(frame->present_done, "prepare_last_frame_fence")) {
        ReleaseRepeatFrame(frame);
        return nullptr;
    }

    auto& scheduler = flip_scheduler;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    scheduler.SetGpuTimelineFrameEpoch(frame->gpu_timeline_epoch);
#endif
    scheduler.EndRendering(Scheduler::EndRenderingReason::Presenter);
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_master = scheduler.GetMasterSemaphore();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    if (scheduler.IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "prepare_last_frame_submit");
        ReleaseRepeatFrame(frame);
        return nullptr;
    }
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    if (IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "prepare_frame");
        return nullptr;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const u64 mobile_gpu_now_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const u64 mobile_gpu_interval_ns =
        mobile_gpu_last_frame_ns == 0 ? 0 : mobile_gpu_now_ns - mobile_gpu_last_frame_ns;
    mobile_gpu_last_frame_ns = mobile_gpu_now_ns;
    VideoCore::ObserveMobileGpuFrame(
        mobile_gpu_interval_ns, draw_scheduler.CurrentTick(),
        draw_scheduler.KnownGpuTick());
#endif

    auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
    const bool exact_videoout_identity =
        instance.GetCapabilityProfile().exact_proprietary_adreno750;
    auto image_id = texture_cache.FindImage(desc, exact_videoout_identity);
    if (exact_videoout_identity) {
        // An exact VideoOut image may have been recreated after overlap resolution or GC.
        // Restore its role from the still-live registration instead of allowing it to be
        // classified as a transient render-target alias on the next flip.
        texture_cache.GetImage(image_id).usage.vo_surface = 1u;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto registered_image_id = image_id;
    bool using_live_drawn_target = false;
    const auto live_target = texture_cache.ExecutorTakeLiveFullResolutionTarget(
        attribute.attrib.width, attribute.attrib.height, registered_image_id);
    if (live_target) {
        image_id = live_target;
        using_live_drawn_target = true;
    }
#endif
    auto* source_image_ptr = &texture_cache.GetImage(image_id);
    auto& source_image = *source_image_ptr;
    const bool source_view_descriptor_mismatch =
        source_image.info.pixel_format != desc.info.pixel_format ||
        source_image.info.type != desc.info.type ||
        source_image.info.resources != desc.info.resources;
    const bool source_is_distinct_rt_alias =
        source_image.usage.render_target && !source_image.usage.vo_surface;
    bool use_canonical_source_view =
        source_view_descriptor_mismatch || source_is_distinct_rt_alias;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    use_canonical_source_view |= using_live_drawn_target;
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& registered_image = texture_cache.GetImage(registered_image_id);
    static std::atomic<u32> frame_source_log_count{0};
    const u32 frame_source_log =
        frame_source_log_count.fetch_add(1, std::memory_order_relaxed);
    if (frame_source_log < 8) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_PREPARE_FRAME_SOURCE] ordinal=%u bridge=%u scanoutImage=%u "
            "scanoutFmt=%u scanoutType=%llu image=%u scanout=%ux%u source=%ux%u "
            "sourceAddr=0x%llx sourceBytes=0x%llx sourceFmt=%u sourceType=%llu "
            "sourceTile=%u sourceBits=%u sourceUsageVo=%u sourceUsageRt=%u "
            "canonicalView=%u descriptorMismatch=%u rtAlias=%u",
            frame_source_log + 1, using_live_drawn_target ? 1u : 0u,
            registered_image_id.index, static_cast<u32>(registered_image.info.pixel_format),
            static_cast<unsigned long long>(registered_image.info.type), image_id.index,
            attribute.attrib.width, attribute.attrib.height, source_image.info.size.width,
            source_image.info.size.height,
            static_cast<unsigned long long>(source_image.info.guest_address),
            static_cast<unsigned long long>(source_image.info.guest_size),
            static_cast<u32>(source_image.info.pixel_format),
            static_cast<unsigned long long>(source_image.info.type),
            static_cast<u32>(source_image.info.tile_mode), source_image.info.num_bits,
            source_image.usage.vo_surface, source_image.usage.render_target,
            use_canonical_source_view ? 1u : 0u,
            source_view_descriptor_mismatch ? 1u : 0u,
            source_is_distinct_rt_alias ? 1u : 0u);
    }
#endif
    const u32 flags_before_refresh = static_cast<u32>(source_image.flags);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!using_live_drawn_target) {
        if (!texture_cache.UpdateImage(image_id)) {
            return PrepareLastFrame();
        }
    }
#else
    if (!texture_cache.UpdateImage(image_id)) {
        return PrepareLastFrame();
    }
#endif
    source_image_ptr = &texture_cache.GetImage(image_id);
    auto& refreshed_source_image = *source_image_ptr;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& refreshed_registered_image = texture_cache.GetImage(registered_image_id);
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool trace_videoout_source =
        std::getenv("EXECUTOR_TRACE_VIDEOOUT_SOURCE") != nullptr ||
        std::getenv("EXECUTOR_TRACE_LIVE_WIDE") != nullptr;
    if (trace_videoout_source) {
        static std::atomic<u64> videoout_sequence{0};
        const u64 sequence = videoout_sequence.fetch_add(1, std::memory_order_relaxed);
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_VIDEOOUT_FRAME_SOURCE] seq=%llu image=%u registered=%u "
            "addr=0x%llx registeredAddr=0x%llx flagsBefore=0x%x flagsAfter=0x%x "
            "usageVo=%u usageRt=%u canonicalView=%u rtAlias=%u",
            static_cast<unsigned long long>(sequence), image_id.index,
            registered_image_id.index,
            static_cast<unsigned long long>(refreshed_source_image.info.guest_address),
            static_cast<unsigned long long>(refreshed_registered_image.info.guest_address),
            flags_before_refresh, static_cast<u32>(refreshed_source_image.flags),
            refreshed_source_image.usage.vo_surface,
            refreshed_source_image.usage.render_target,
            use_canonical_source_view ? 1u : 0u,
            source_is_distinct_rt_alias ? 1u : 0u);
        if (sequence < 12 || (sequence % 60) < 3) {
            texture_cache.ExecutorTraceVideoOutSource(image_id, sequence, flags_before_refresh);
        }
    }
#endif

    Frame* frame = GetRenderFrame();
    if (frame == nullptr) {
        return nullptr;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool presenter_journal =
        instance.GetCapabilityProfile().exact_proprietary_adreno750 &&
        ExecutorVkJournalRequested();
    const auto journal_prepare = [&](const ExecutorVkJournalPresenterStage stage,
                                     const vk::Extent2D source,
                                     const u32 flags = 0) {
        if (presenter_journal) {
            ExecutorVkJournalPresenter(
                &draw_scheduler, draw_scheduler.CurrentTick(), stage, frame->id,
                source.width, source.height, frame->width, frame->height, flags);
        }
    };
#endif
    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const bool direct_mobile_composition = !instance.UseHostUiComposition();
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = direct_mobile_composition ? vk::PipelineStageFlagBits2::eTopOfPipe
                                                  : vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = direct_mobile_composition ? vk::AccessFlags2{}
                                                   : vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = direct_mobile_composition ? vk::PipelineStageFlagBits2::eTransfer
                                                  : vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = direct_mobile_composition ? vk::AccessFlagBits2::eTransferWrite
                                                   : vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = direct_mobile_composition ? vk::ImageLayout::eTransferDstOptimal
                                               : vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering(Scheduler::EndRenderingReason::Presenter);
    auto cmdbuf = draw_scheduler.CommandBuffer();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    journal_prepare(ExecutorVkJournalPresenterStage::PrepareSourceBegin,
                    {refreshed_source_image.info.size.width,
                     refreshed_source_image.info.size.height},
                    direct_mobile_composition ? 1u : 0u);
#endif
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    if (use_canonical_source_view) {
        view_info.type = refreshed_source_image.info.type;
        const vk::Format scanout_format = GetFrameViewFormat(attribute.attrib.pixel_format);
        view_info.format = VideoCore::IsVulkanFormatCompatible(refreshed_source_image.info.pixel_format,
                                                                scanout_format)
            ? scanout_format
            : refreshed_source_image.info.pixel_format;
        view_info.range.extent = refreshed_source_image.info.resources;
    } else {
        view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    }
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    auto& image = texture_cache.GetImage(image_id);
    auto image_view = *image.FindView(view_info).image_view;
    image.Transit(direct_mobile_composition ? vk::ImageLayout::eTransferSrcOptimal
                                            : vk::ImageLayout::eShaderReadOnlyOptimal,
                  direct_mobile_composition ? vk::AccessFlagBits2::eTransferRead
                                            : vk::AccessFlagBits2::eShaderRead,
                  {}, cmdbuf);

    vk::Extent2D image_size = {image.info.size.width, image.info.size.height};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // The configured 960x544 mobile baseline is already the final internal
    // resolution. Do not let adaptive DRS halve the complete VideoOut image
    // (including UI and font glyphs) a second time.
    constexpr u64 MobileBaselinePixels = 960ull * 544ull;
    VideoCore::SetMobileGpuPhysicalDrsResolutionFloor(
        static_cast<u64>(image_size.width) * image_size.height <=
        MobileBaselinePixels);
    // A physically scaled render target remains available after its
    // guest-visible canonical resolve.  Feed that smaller source directly to
    // FSR so dynamic resolution saves fragment work and does not get expanded
    // once by a nearest blit before the real upscaler sees it.
    vk::ImageView physical_drs_view{};
    vk::Extent2D physical_drs_extent{};
    const bool using_physical_drs_source = !direct_mobile_composition &&
        rasterizer->ExecutorAcquirePhysicalDrsPresentationSource(
            image_id, physical_drs_view, physical_drs_extent);
    if (using_physical_drs_source) {
        image_view = physical_drs_view;
        image_size = physical_drs_extent;
    }
    const u32 tile_aligned_height = (attribute.attrib.height + 15u) & ~15u;
    if (!using_physical_drs_source && using_live_drawn_target &&
        image_size.width == attribute.attrib.width &&
        image_size.height == tile_aligned_height &&
        tile_aligned_height > attribute.attrib.height) {
        image_size.height = attribute.attrib.height;
    }
#endif
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    journal_prepare(ExecutorVkJournalPresenterStage::PrepareSourceReady, image_size,
                    using_physical_drs_source ? 2u : 0u);
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Diagnostic only: isolate guest/source transitions from the unjournaled
    // FSR/post-process work. Keep the source transition in the first submit,
    // then reacquire a fresh recording command buffer for composition.
    u64 a750_present_source_tick{};
    const bool split_a750_present_source =
        !direct_mobile_composition &&
        instance.GetCapabilityProfile().diagnostic_a750_present_source_submit_split;
    if (split_a750_present_source) {
        a750_present_source_tick = draw_scheduler.CurrentTick();
        SubmitInfo source_info{};
        draw_scheduler.Flush(source_info);
        if (draw_scheduler.IsDeviceLost()) {
            MarkDeviceLost(vk::Result::eErrorDeviceLost,
                           "prepare_frame_source_split");
            return nullptr;
        }
        cmdbuf = draw_scheduler.CommandBuffer();
    }
#endif

    if (direct_mobile_composition) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        DebugState.is_using_fsr = false;
        VideoCore::RecordMobileGpuFsrDecision(false, false, image_size.width,
                                              image_size.height, frame->width,
                                              frame->height);
        VideoCore::RecordMobileGpuUpscalerDecision(
            static_cast<u32>(fsr_settings.mode),
            static_cast<u32>(Config::UpscalerMode::Off), false, 0, false);
        journal_prepare(ExecutorVkJournalPresenterStage::PrepareDirectBlitBegin,
                        image_size);
#endif
        const auto blit = MakeImageBlitStretch(static_cast<s32>(image_size.width),
                                               static_cast<s32>(image_size.height),
                                               static_cast<s32>(frame->width),
                                               static_cast<s32>(frame->height));
        cmdbuf.blitImage(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, frame->image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        const vk::ImageMemoryBarrier2 post_barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe,
            .dstAccessMask = {},
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = frame->image,
            .subresourceRange = frame_subresources,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &post_barrier,
        });
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        journal_prepare(ExecutorVkJournalPresenterStage::PrepareDirectBlitEnd,
                        image_size);
#endif
    } else {
        const Config::UpscalerMode requested_upscaler = fsr_settings.mode;
        u32 upscaler_fallback_reason = 0;
        bool temporal_native_inputs = false;
        auto effective_fsr_settings = fsr_settings;
        effective_fsr_settings.enable =
            VideoCore::IsManagedMobileFsrEnabled() &&
            (effective_fsr_settings.enable ||
             VideoCore::MobileGpuWantsFsr(image_size.width, image_size.height,
                                          frame->width, frame->height));
        effective_fsr_settings.use_rcas =
            effective_fsr_settings.enable && effective_fsr_settings.use_rcas &&
            VideoCore::IsManagedMobileRcasEnabled();
        if (!effective_fsr_settings.enable) {
            effective_fsr_settings.mode = Config::UpscalerMode::Off;
        } else if (effective_fsr_settings.mode == Config::UpscalerMode::Auto) {
            effective_fsr_settings.mode =
                instance.GetCapabilityProfile().preferred_spatial_upscaler ==
                        PreferredSpatialUpscaler::Sgsr1
                    ? Config::UpscalerMode::Sgsr1
                    : Config::UpscalerMode::Fsr1;
        }
        // SGSR1 is defined for display-referred LDR input. Preserve the proven
        // HDR EASU path until a matching SGSR HDR transfer is available.
        if (frame->is_hdr && effective_fsr_settings.mode == Config::UpscalerMode::Sgsr1) {
            effective_fsr_settings.mode = Config::UpscalerMode::Fsr1;
            upscaler_fallback_reason = 4;
        }
        effective_fsr_settings.use_rcas =
            effective_fsr_settings.mode == Config::UpscalerMode::Fsr1 &&
            effective_fsr_settings.use_rcas;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        journal_prepare(ExecutorVkJournalPresenterStage::PrepareFsrBegin, image_size,
                        effective_fsr_settings.enable ? 1u : 0u);
#endif
        const bool genuine_upscale = effective_fsr_settings.enable &&
            image_size.width < frame->width && image_size.height < frame->height;
        if (!genuine_upscale) {
            effective_fsr_settings.mode = Config::UpscalerMode::Off;
        }
        if (effective_fsr_settings.mode == Config::UpscalerMode::Sgsr2) {
            image_view = sgsr2_pass.Render(
                draw_scheduler, cmdbuf, image_view, image_size,
                {frame->width, frame->height},
                HostPasses::Sgsr2Pass::TemporalInputs{},
                std::clamp(1.0f - Config::getSgsr2TemporalStability() / 1000.0f,
                           0.05f, 1.0f));
            temporal_native_inputs = sgsr2_pass.HasNativeTemporalInputs();
        } else if (effective_fsr_settings.mode == Config::UpscalerMode::ArmAsr &&
                   arm_asr_pass.IsSupported()) {
            const auto temporal = sgsr2_pass.PrepareForExternalTemporal(
                draw_scheduler, cmdbuf, image_view, image_size,
                {frame->width, frame->height},
                HostPasses::Sgsr2Pass::TemporalInputs{});
            const vk::ImageView arm_output = arm_asr_pass.Render(
                draw_scheduler, cmdbuf, image_size,
                {frame->width, frame->height}, temporal, 0.f, 0.f,
                frame->is_hdr, Config::getArmAsrSharpening(),
                Config::getArmAsrSharpness() / 1000.0f);
            if (arm_output) {
                image_view = arm_output;
                temporal_native_inputs = temporal.native;
            } else {
                effective_fsr_settings.mode = Config::UpscalerMode::Fsr1;
                upscaler_fallback_reason = 3;
                image_view = fsr_pass.Render(
                    draw_scheduler, cmdbuf, image_view, image_size,
                    {frame->width, frame->height}, effective_fsr_settings,
                    frame->is_hdr);
            }
        } else {
            if (effective_fsr_settings.mode == Config::UpscalerMode::ArmAsr) {
                effective_fsr_settings.mode = Config::UpscalerMode::Fsr1;
                upscaler_fallback_reason = 1;
            }
            image_view = fsr_pass.Render(draw_scheduler, cmdbuf, image_view, image_size,
                                         {frame->width, frame->height},
                                         effective_fsr_settings, frame->is_hdr);
            if (fsr_pass.LastFallbackReason() != 0) {
                upscaler_fallback_reason = fsr_pass.LastFallbackReason();
                effective_fsr_settings.mode = fsr_pass.LastEffectiveMode();
            }
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        VideoCore::RecordMobileGpuUpscalerDecision(
            static_cast<u32>(requested_upscaler),
            static_cast<u32>(effective_fsr_settings.mode), genuine_upscale,
            upscaler_fallback_reason, temporal_native_inputs);
        journal_prepare(ExecutorVkJournalPresenterStage::PrepareFsrEnd, image_size,
                        effective_fsr_settings.enable ? 1u : 0u);
        journal_prepare(ExecutorVkJournalPresenterStage::PreparePostProcessBegin,
                        image_size, frame->is_hdr ? 1u : 0u);
#endif
        pp_pass.Render(draw_scheduler, cmdbuf, image_view, image_size, *frame, pp_settings);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        journal_prepare(ExecutorVkJournalPresenterStage::PreparePostProcessEnd,
                        image_size, frame->is_hdr ? 1u : 0u);
#endif
    }

    DebugState.game_resolution = {image_size.width, image_size.height};
    DebugState.output_resolution = {frame->width, frame->height};

    frame->ready_semaphore = draw_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_master = draw_scheduler.GetMasterSemaphore();
    frame->ready_tick = draw_scheduler.CurrentTick();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    journal_prepare(ExecutorVkJournalPresenterStage::PrepareSubmit, image_size);
#endif
    SubmitInfo info{};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (a750_present_source_tick != 0) {
        info.AddWait(draw_scheduler.GetMasterSemaphore()->Handle(),
                     a750_present_source_tick,
                     vk::PipelineStageFlagBits::eAllCommands);
    }
#endif
    draw_scheduler.Flush(info);
    if (draw_scheduler.IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "prepare_frame_submit");
        return nullptr;
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    Frame* frame = GetRenderFrame();
    if (frame == nullptr) {
        return nullptr;
    }

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering(Scheduler::EndRenderingReason::Presenter);

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    if (!instance.UseHostUiComposition()) {
        const auto pre_barrier = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe,
            .srcAccessMask = {},
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = frame->image,
            .subresourceRange = simple_subresource,
        };
        const auto post_barrier = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = frame->image,
            .subresourceRange = simple_subresource,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &pre_barrier,
        });
        cmdbuf.clearColorImage(frame->image, vk::ImageLayout::eTransferDstOptimal,
                               vk::ClearColorValue{}, simple_subresource);
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &post_barrier,
        });

        frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
        frame->ready_master = scheduler.GetMasterSemaphore();
        frame->ready_tick = scheduler.CurrentTick();
        SubmitInfo info{};
        scheduler.Flush(info);
        if (scheduler.IsDeviceLost()) {
            MarkDeviceLost(vk::Result::eErrorDeviceLost, "prepare_blank_frame_submit");
            return nullptr;
        }
        return frame;
    }
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    PipelineBarrier(scheduler, cmdbuf, vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    if (instance.IsDynamicRenderingSupported()) {
        cmdbuf.beginRendering(rendering_info);
        cmdbuf.endRendering();
    } else {
        const vk::AttachmentDescription color_attachment{
            .format = swapchain.GetSurfaceFormat().format,
            .samples = vk::SampleCountFlagBits::e1,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
            .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
            .initialLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
        };
        const vk::AttachmentReference color_reference{
            .attachment = 0,
            .layout = vk::ImageLayout::eColorAttachmentOptimal,
        };
        const vk::SubpassDescription subpass{
            .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_reference,
        };
        const vk::Device device = instance.GetDevice();
        const auto render_pass =
            Check<"create blank frame legacy render pass">(device.createRenderPass({
                .attachmentCount = 1,
                .pAttachments = &color_attachment,
                .subpassCount = 1,
                .pSubpasses = &subpass,
            }));
        const auto framebuffer =
            Check<"create blank frame legacy framebuffer">(device.createFramebuffer({
                .renderPass = render_pass,
                .attachmentCount = 1,
                .pAttachments = &frame->image_view,
                .width = frame->width,
                .height = frame->height,
                .layers = 1,
            }));
        const vk::ClearValue clear_value{
            .color = vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}},
        };
        cmdbuf.beginRenderPass(
            vk::RenderPassBeginInfo{
                .renderPass = render_pass,
                .framebuffer = framebuffer,
                .renderArea = {.extent = {frame->width, frame->height}},
                .clearValueCount = 1,
                .pClearValues = &clear_value,
            },
            vk::SubpassContents::eInline);
        cmdbuf.endRenderPass();
        scheduler.DeferOperation([device, framebuffer, render_pass] {
            device.destroyFramebuffer(framebuffer);
            device.destroyRenderPass(render_pass);
        });
    }

    PipelineBarrier(scheduler, cmdbuf, vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_master = scheduler.GetMasterSemaphore();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    if (scheduler.IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "prepare_blank_frame_submit");
        return nullptr;
    }
    return frame;
}

bool Presenter::ExecutorPresentReplayRT() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "replay_present");
        return false;
    }
    static u32 replay_present_log_counter = 0;
    const bool live_present = std::getenv("EXECUTOR_LIVE_PRESENT_RT") != nullptr;
    const bool log_this_present =
        replay_present_log_counter == 0 || (replay_present_log_counter % 60) == 0;
    ++replay_present_log_counter;
    if (log_this_present && !live_present) {
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_PRESENT] replay candidates=%zu target=%u",
                            texture_cache.executor_replay_color_candidates.size(),
                            texture_cache.executor_replay_color_target.index);
    }
    VideoCore::ImageId image_id{};
    if (live_present) {
        const u32 videoout_width = executor_videoout_width.load(std::memory_order_acquire);
        const u32 videoout_height = executor_videoout_height.load(std::memory_order_acquire);
        image_id =
            texture_cache.ExecutorTakeLiveFullResolutionTarget(videoout_width, videoout_height);
        if (!image_id) {
            return false;
        }
    } else {
        if (!texture_cache.executor_replay_color_candidates.empty()) {
            texture_cache.ExecutorReplayReadbackColorTarget();
        }
        image_id = texture_cache.executor_replay_color_target;
    }
    if (!image_id) {
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_PRESENT] no replay color target");
        return false;
    }
    auto& src = texture_cache.GetImage(image_id);
    if (live_present) {
        std::scoped_lock present_lock{executor_present_mutex};
        if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
        if (!AcquireSwapchainImage("direct_present_acquire")) {
            __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                                "[EXECUTOR_PRESENT] live direct skip=acquire_failed");
            return false;
        }

        auto& scheduler = draw_scheduler;
        scheduler.EndRendering(Scheduler::EndRenderingReason::Presenter);
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::Image swapchain_image = swapchain.Image();
        constexpr vk::ImageSubresourceRange subresource{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        };
        const vk::ImageMemoryBarrier pre_barrier{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = subresource,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

        src.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
                    cmdbuf);
        const vk::Extent2D dst_extent = swapchain.GetExtent();
        const auto blit = MakeImageBlitStretch(static_cast<s32>(src.info.size.width),
                                               static_cast<s32>(src.info.size.height),
                                               static_cast<s32>(dst_extent.width),
                                               static_cast<s32>(dst_extent.height));
        cmdbuf.blitImage(src.GetImage(), vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);

        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = subresource,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eBottomOfPipe,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        SubmitInfo info{};
        info.AddWait(swapchain.GetImageAcquiredSemaphore(), 1,
                     vk::PipelineStageFlagBits::eTransfer);
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        scheduler.Flush(info);
        if (scheduler.IsDeviceLost()) {
            MarkDeviceLost(vk::Result::eErrorDeviceLost, "direct_present_submit");
            return false;
        }

        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        u64 queue_present_begin_ns{};
        const u64 queue_present_sequence =
            ExecutorVkGpuTimelineQueuePresentBegin(queue_present_begin_ns);
        const bool present_ok = swapchain.Present();
        ExecutorVkGpuTimelineQueuePresentEnd(queue_present_sequence,
                                             queue_present_begin_ns, present_ok);
        if (swapchain.IsTerminal()) {
            MarkTerminal(swapchain.TerminalResult(), "direct_queue_present");
        } else if (!present_ok) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        } else {
            executor_live_direct_present_active.store(true, std::memory_order_release);
        }
        ExecutorLiveDirectPresentStats(present_ok, src.info.size.width, src.info.size.height,
                                       dst_extent.width, dst_extent.height, image_id.index);
        if (log_this_present) {
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_PRESENT_DIRECT] ok=%d src=%ux%u dst=%ux%u target=%u",
                                present_ok ? 1 : 0, src.info.size.width, src.info.size.height,
                                dst_extent.width, dst_extent.height, image_id.index);
        }
        return present_ok;
    }
    Frame* frame = GetRenderFrame();
    if (frame == nullptr) {
        return false;
    }

    draw_scheduler.EndRendering(Scheduler::EndRenderingReason::Presenter);
    const auto cmdbuf = draw_scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange frame_subresources{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier{
        .srcAccessMask = vk::AccessFlagBits::eNone,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = frame->image,
        .subresourceRange = frame_subresources,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                           vk::PipelineStageFlagBits::eColorAttachmentOutput,
                           vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

    VideoCore::ImageViewInfo view_info{};
    view_info.type = src.info.type;
    view_info.format = src.info.pixel_format;
    view_info.range.extent = src.info.resources;
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    const auto& src_view = src.FindView(view_info);
    if (!src_view.image_view) {
        __android_log_print(ANDROID_LOG_ERROR, "LSX4Native",
                            "[EXECUTOR_PRESENT] no replay image view target=%u extent=%ux%u",
                            image_id.index, src.info.size.width, src.info.size.height);
        return false;
    }
    const auto image_view = *src_view.image_view;
    src.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {});

    const vk::Extent2D image_size = {src.info.size.width, src.info.size.height};
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);
    pp_pass.Render(draw_scheduler, cmdbuf, image_view, image_size, *frame, pp_settings);

    frame->ready_semaphore = draw_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_master = draw_scheduler.GetMasterSemaphore();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    if (draw_scheduler.IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "replay_present_submit");
        return false;
    }
    if (log_this_present) {
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_PRESENT] presenting replay RT opaque-alpha %ux%u -> frame %ux%u",
                            src.info.size.width, src.info.size.height, frame->width, frame->height);
    }
    Present(frame);
    return true;
#else
    return false;
#endif
}

void Presenter::ExecutorRecyclePreparedFrame(Frame* frame) {
    if (!frame) {
        return;
    }
    if (!WaitForFrameReady(frame, "recycle_frame_wait")) {
        return;
    }
    RecycleFrame(frame);
}

void Presenter::Present(Frame* frame, bool is_reusing_frame) {
    if (frame == nullptr) {
        return;
    }
    SCOPE_EXIT {
        if (is_reusing_frame) {
            ReleaseRepeatFrame(frame);
        }
    };
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    present_scheduler.SetGpuTimelineFrameEpoch(frame->gpu_timeline_epoch);
    const bool present_probes_enabled = ExecutorAndroidRendererProbesEnabled();
    // The presentation path was only ever estimated from a resolution delta.
    // Time it directly so the plan works off a measured number.
    struct PresentProbe {
        bool enabled;
        std::chrono::steady_clock::time_point t0{};
        explicit PresentProbe(const bool enabled_) : enabled(enabled_) {
            if (enabled) {
                t0 = std::chrono::steady_clock::now();
            }
        }
        ~PresentProbe() {
            if (!enabled) {
                return;
            }
            static std::atomic<std::uint64_t> total_ns{0}, imgui_ns{0}, calls{0};
            const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            const auto n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto sum = total_ns.fetch_add(dt, std::memory_order_relaxed) + dt;
            const auto ig = imgui_ns.load(std::memory_order_relaxed)
                          + g_present_imgui_ns.exchange(0, std::memory_order_relaxed);
            imgui_ns.store(ig, std::memory_order_relaxed);
            if ((n % 120) == 0) {
                if (FILE* out = std::fopen(
                        "/data/data/app.lsx4.android/files/runtime/present-probe.txt", "w")) {
                    std::fprintf(out, "frames=%llu avg_ms=%.3f imgui_avg_ms=%.3f\n",
                                 (unsigned long long)n, double(sum) / n / 1e6,
                                 double(ig) / n / 1e6);
                    std::fclose(out);
                }
            }
        }
    } present_probe{present_probes_enabled};
#endif
    PresenterCpuStageProfile present_stage_profile{ExecutorPresentStageProfileEnabled()};
    if (IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "present");
        return;
    }

    std::unique_lock present_lock{executor_present_mutex};
    if (!is_reusing_frame && std::getenv("EXECUTOR_LIVE_PRESENT_RT") != nullptr &&
        ExecutorLiveDirectPresentActive()) {
        present_lock.unlock();
        ExecutorRecyclePreparedFrame(frame);
        return;
    }

    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            PublishLastFrame(frame);
        }
    };

    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    present_stage_profile.Begin(PresenterCpuStage::Acquire);
    const bool acquired_swapchain_image = AcquireSwapchainImage("videoout_acquire");
    present_stage_profile.End(PresenterCpuStage::Acquire);
    if (!acquired_swapchain_image) {
        LOG_WARNING(Render_Vulkan, "Skipping frame!");
        free_frame();
        return;
    }

    present_stage_profile.Begin(PresenterCpuStage::CommandStart);
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    if (reset_result != vk::Result::eSuccess) [[unlikely]] {
        if (reset_result == vk::Result::eErrorDeviceLost) {
            MarkDeviceLost(reset_result, "present_reset_fence");
        } else {
            LOG_ERROR(Render_Vulkan, "Unexpected error resetting present done fence: {}",
                      vk::to_string(reset_result));
        }
        free_frame();
        return;
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorPresenterSwapchainGradientEnabled() &&
        instance.IsDynamicRenderingSupported()) {
        present_stage_profile.SetRoute(PresenterCpuRoute::Gradient);
        auto& scheduler = present_scheduler;
        scheduler.EndRendering(Scheduler::EndRenderingReason::Presenter);

        const vk::Image swapchain_image = swapchain.Image();
        const vk::ImageView swapchain_image_view = swapchain.ImageView();
        const vk::Extent2D extent = swapchain.GetExtent();
        constexpr vk::ImageSubresourceRange subresource{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        };
        const vk::ImageMemoryBarrier pre_barrier{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = subresource,
        };
        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = subresource,
        };

        const vk::RenderingAttachmentInfo color_attachment{
            .imageView = swapchain_image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        const vk::RenderingInfo rendering_info{
            .renderArea = {.extent = extent},
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_attachment,
        };

        const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
        present_stage_profile.End(PresenterCpuStage::CommandStart);
        present_stage_profile.Begin(PresenterCpuStage::Record);
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);
        cmdbuf.beginRendering(rendering_info);

        // Prime strip count avoids accidentally aligning this diagnostic with
        // Adreno's tile periods.  Exercise both axes in one frame: the left
        // half varies along Y and the right half varies along X.  A swapped or
        // incorrectly offset UBWC data/meta layout therefore cannot hide
        // behind identical scanlines or columns.
        constexpr u32 NumGradientStrips = 127;
        const u32 split_x = extent.width / 2;
        const u32 right_width = extent.width - split_x;
        for (u32 index = 0; index < NumGradientStrips; ++index) {
            const u32 y0 = static_cast<u32>(
                (static_cast<u64>(index) * extent.height) / NumGradientStrips);
            const u32 y1 = static_cast<u32>(
                (static_cast<u64>(index + 1) * extent.height) / NumGradientStrips);
            const u32 x0 = split_x + static_cast<u32>(
                                           (static_cast<u64>(index) * right_width) /
                                           NumGradientStrips);
            const u32 x1 = split_x + static_cast<u32>(
                                           (static_cast<u64>(index + 1) * right_width) /
                                           NumGradientStrips);
            if (y0 == y1 || x0 == x1 || split_x == 0 || right_width == 0) {
                continue;
            }
            const float value = static_cast<float>(index) /
                                static_cast<float>(NumGradientStrips - 1);
            const std::array clear_attachments{
                vk::ClearAttachment{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .colorAttachment = 0,
                    .clearValue = vk::ClearValue{
                        .color = vk::ClearColorValue{
                            std::array<float, 4>{value, value, value, 1.0f}},
                    },
                },
            };
            const std::array clear_rects{
                vk::ClearRect{
                    .rect = {{0, static_cast<s32>(y0)}, {split_x, y1 - y0}},
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                vk::ClearRect{
                    .rect = {{static_cast<s32>(x0), 0}, {x1 - x0, extent.height}},
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
            cmdbuf.clearAttachments(clear_attachments, clear_rects);
        }

        cmdbuf.endRendering();
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eBottomOfPipe,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        present_stage_profile.End(PresenterCpuStage::Record);
        present_stage_profile.Begin(PresenterCpuStage::Flush);
        SubmitInfo info{};
        info.AddWait(swapchain.GetImageAcquiredSemaphore(), 1,
                     vk::PipelineStageFlagBits::eColorAttachmentOutput);
        // The diagnostic never reads frame->image, but the frame cannot be retained/recycled
        // until its producer has finished writing it.
        if (!WaitForFrameReady(frame, "present_gradient_frame_ready")) {
            free_frame();
            return;
        }
        if (frame->ready_semaphore) {
            info.AddWait(frame->ready_semaphore, frame->ready_tick,
                         vk::PipelineStageFlagBits::eAllCommands);
        }
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        info.AddSignal(frame->present_done);
        scheduler.Flush(info);
        present_stage_profile.End(PresenterCpuStage::Flush);
        if (scheduler.IsDeviceLost()) {
            MarkDeviceLost(vk::Result::eErrorDeviceLost, "present_gradient_submit");
            free_frame();
            return;
        }

        bool present_ok{};
        {
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            u64 queue_present_begin_ns{};
            const u64 queue_present_sequence =
                ExecutorVkGpuTimelineQueuePresentBegin(queue_present_begin_ns);
            present_stage_profile.Begin(PresenterCpuStage::QueuePresent);
            const auto overlap_present_v2 =
                VideoCore::InvalidationOverlapTrace::BeginPresentV2(
                    frame->gpu_timeline_epoch,
                    frame->overlap_trace_frame_token);
            present_ok = swapchain.Present();
            VideoCore::InvalidationOverlapTrace::EndPresentV2(
                overlap_present_v2, present_ok);
            present_stage_profile.End(PresenterCpuStage::QueuePresent);
            present_stage_profile.RecordQueuePresentResult(present_ok);
            ExecutorVkGpuTimelineQueuePresentEnd(queue_present_sequence,
                                                 queue_present_begin_ns, present_ok);
            if (!present_ok && !swapchain.IsTerminal()) {
                swapchain.Recreate(window.GetWidth(), window.GetHeight());
            }
        }
        if (!present_ok) {
            if (swapchain.IsTerminal()) {
                MarkTerminal(swapchain.TerminalResult(), "present_gradient_queue_present");
            }
        } else {
            g_executor_live_present_count.fetch_add(1, std::memory_order_relaxed);
            Executor::AndroidPerformanceHint::MaintainSessionThreads();
        }

        static bool logged{};
        if (!logged) {
            logged = true;
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_PRESENTER_SWAPCHAIN_GRADIENT] enabled=1 route=dynamic "
                "axis=xy layout=split-left-y-right-x strips=%u extent=%ux%u",
                NumGradientStrips, extent.width, extent.height);
        }
        free_frame();
        if (!is_reusing_frame) {
            DebugState.IncFlipFrameNum();
        }
        return;
    }
    if (ExecutorPresenterSwapchainGradientEnabled() &&
        !instance.IsDynamicRenderingSupported()) {
        static bool logged_unsupported{};
        if (!logged_unsupported) {
            logged_unsupported = true;
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_PRESENTER_SWAPCHAIN_GRADIENT] enabled=0 "
                "reason=dynamic-rendering-unsupported fallback=normal-presenter");
        }
    }

    if (!instance.UseHostUiComposition()) {
        present_stage_profile.SetRoute(PresenterCpuRoute::DirectComposition);
        const vk::Image swapchain_image = swapchain.Image();
        const vk::Extent2D dst_extent = swapchain.GetExtent();
        constexpr vk::ImageSubresourceRange subresource{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .levelCount = 1,
            .layerCount = 1,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange = subresource,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = subresource,
            },
        };
        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = subresource,
        };

        auto& scheduler = present_scheduler;
        const vk::CommandBuffer cmdbuf = scheduler.CommandBuffer();
        present_stage_profile.End(PresenterCpuStage::CommandStart);
        present_stage_profile.Begin(PresenterCpuStage::Record);
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);
        const auto blit = MakeImageBlitFit(static_cast<s32>(frame->width),
                                           static_cast<s32>(frame->height),
                                           static_cast<s32>(dst_extent.width),
                                           static_cast<s32>(dst_extent.height));
        cmdbuf.blitImage(frame->image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eBottomOfPipe,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        present_stage_profile.End(PresenterCpuStage::Record);
        present_stage_profile.Begin(PresenterCpuStage::Flush);
        SubmitInfo info{};
        info.AddWait(swapchain.GetImageAcquiredSemaphore(), 1,
                     vk::PipelineStageFlagBits::eTransfer);
        if (!WaitForFrameReady(frame, "mali_direct_present_ready")) {
            free_frame();
            return;
        }
        if (frame->ready_semaphore) {
            info.AddWait(frame->ready_semaphore, frame->ready_tick,
                         vk::PipelineStageFlagBits::eTransfer);
        }
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        info.AddSignal(frame->present_done);
        scheduler.Flush(info);
        present_stage_profile.End(PresenterCpuStage::Flush);
        if (scheduler.IsDeviceLost()) {
            MarkDeviceLost(vk::Result::eErrorDeviceLost, "mali_direct_present_submit");
            free_frame();
            return;
        }

        {
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            u64 queue_present_begin_ns{};
            const u64 queue_present_sequence =
                ExecutorVkGpuTimelineQueuePresentBegin(queue_present_begin_ns);
            present_stage_profile.Begin(PresenterCpuStage::QueuePresent);
            const auto overlap_present_v2 =
                VideoCore::InvalidationOverlapTrace::BeginPresentV2(
                    frame->gpu_timeline_epoch,
                    frame->overlap_trace_frame_token);
            const bool present_ok = swapchain.Present();
            VideoCore::InvalidationOverlapTrace::EndPresentV2(
                overlap_present_v2, present_ok);
            present_stage_profile.End(PresenterCpuStage::QueuePresent);
            present_stage_profile.RecordQueuePresentResult(present_ok);
            ExecutorVkGpuTimelineQueuePresentEnd(queue_present_sequence,
                                                 queue_present_begin_ns, present_ok);
            if (!present_ok && !swapchain.IsTerminal()) {
                swapchain.Recreate(window.GetWidth(), window.GetHeight());
            }
        }
        static bool logged{};
        if (!logged) {
            logged = true;
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_MALI_DIRECT_COMPOSITION] enabled=1 src=%ux%u dst=%ux%u",
                                frame->width, frame->height, dst_extent.width, dst_extent.height);
        }
        free_frame();
        return;
    }
#endif

    present_stage_profile.SetRoute(PresenterCpuRoute::HostUi);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    std::chrono::steady_clock::time_point imgui_t0{};
    if (present_probes_enabled) {
        imgui_t0 = std::chrono::steady_clock::now();
    }
#endif
    ImGuiID dockId = ImGui::Core::NewFrame(is_reusing_frame);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (present_probes_enabled) {
        g_present_imgui_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - imgui_t0).count(),
            std::memory_order_relaxed);
    }
#endif

    const vk::Image swapchain_image = swapchain.Image();
    const vk::ImageView swapchain_image_view = swapchain.ImageView();

    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool host_ui_presenter_journal =
        instance.GetCapabilityProfile().exact_proprietary_adreno750 &&
        ExecutorVkJournalRequested();
    const auto journal_host_ui = [&](const ExecutorVkJournalPresenterStage stage) {
        if (host_ui_presenter_journal) {
            ExecutorVkJournalPresenter(
                &scheduler, scheduler.CurrentTick(), stage, frame->id,
                frame->width, frame->height, swapchain.GetExtent().width,
                swapchain.GetExtent().height);
        }
    };
    journal_host_ui(ExecutorVkJournalPresenterStage::HostUiBegin);
#endif
    present_stage_profile.End(PresenterCpuStage::CommandStart);
    present_stage_profile.Begin(PresenterCpuStage::Record);

    if (Config::getVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        auto* profiler_ctx = instance.GetProfilerContext();
        TracyVkNamedZoneC(profiler_ctx, renderer_gpu_zone, cmdbuf, "Host frame",
                          MarkersPalette::GpuMarkerColor, profiler_ctx != nullptr);

        const vk::Extent2D extent = swapchain.GetExtent();
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eShaderRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput |
                                   vk::PipelineStageFlagBits::eFragmentShader,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_Once);
            if (ImGui::Begin("Display##game_display", nullptr, ImGuiWindowFlags_NoNav)) {
                auto game_texture = frame->imgui_texture;
                auto game_width = frame->width;
                auto game_height = frame->height;

                if (Libraries::SystemService::IsSplashVisible()) {
                    if (!splash_img.has_value()) {
                        splash_img.emplace();
                        auto splash_path = Common::ElfInfo::Instance().GetSplashPath();
                        if (!splash_path.empty()) {
                            splash_img = ImGui::RefCountedTexture::DecodePngFile(splash_path);
                        }
                    }
                    if (auto& splash_image = this->splash_img.value()) {
                        auto [im_id, width, height] = splash_image.GetTexture();
                        game_texture = im_id;
                        game_width = width;
                        game_height = height;
                    }
                }

                ImVec2 contentArea = ImGui::GetContentRegionAvail();
                SetExpectedGameSize((s32)contentArea.x, (s32)contentArea.y);

                const auto imgRect =
                    FitImage(game_width, game_height, (s32)contentArea.x, (s32)contentArea.y);
                ImVec2 offset{
                    static_cast<float>(imgRect.offset.x),
                    static_cast<float>(imgRect.offset.y),
                };
                ImVec2 size{
                    static_cast<float>(imgRect.extent.width),
                    static_cast<float>(imgRect.extent.height),
                };

                ImGui::SetCursorPos(ImGui::GetCursorStartPos() + offset);
                ImGui::Image(game_texture, size);

                if (Config::nullGpu()) {
                    Core::Devtools::Layer::DrawNullGpuNotice();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
        }
        ImGui::Core::Render(cmdbuf, swapchain_image_view, swapchain.GetExtent(), scheduler);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        journal_host_ui(ExecutorVkJournalPresenterStage::HostUiRenderEnd);
#endif

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        if (profiler_ctx) {
            TracyVkCollect(profiler_ctx, cmdbuf);
        }
    }

    if (Config::getVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    present_stage_profile.End(PresenterCpuStage::Record);
    present_stage_profile.Begin(PresenterCpuStage::Flush);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    journal_host_ui(ExecutorVkJournalPresenterStage::HostUiSubmit);
#endif
    SubmitInfo info{};
    // The acquired image is first touched as the ImGui colour attachment.
    // Waiting at AllCommands stalls unrelated producer work at the queue head
    // on Turnip; the Vulkan WSI semaphore only needs to gate that first image
    // access.
    info.AddWait(swapchain.GetImageAcquiredSemaphore(), 1,
                 vk::PipelineStageFlagBits::eColorAttachmentOutput);
    if (!WaitForFrameReady(frame, "present_frame_ready")) {
        free_frame();
        return;
    }
    if (frame->ready_semaphore) {
        // The guest frame is consumed by ImGui as a sampled texture. Keep its
        // timeline dependency at the first consumer stage instead of blocking
        // the entire presentation command buffer.
        info.AddWait(frame->ready_semaphore, frame->ready_tick,
                     vk::PipelineStageFlagBits::eFragmentShader);
    }
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);
    present_stage_profile.End(PresenterCpuStage::Flush);
    if (scheduler.IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "present_submit");
        free_frame();
        return;
    }

    std::scoped_lock submit_lock{Scheduler::submit_mutex};
    u64 queue_present_begin_ns{};
    const u64 queue_present_sequence =
        ExecutorVkGpuTimelineQueuePresentBegin(queue_present_begin_ns);
    present_stage_profile.Begin(PresenterCpuStage::QueuePresent);
    const auto overlap_present_v2 =
        VideoCore::InvalidationOverlapTrace::BeginPresentV2(
            frame->gpu_timeline_epoch, frame->overlap_trace_frame_token);
    const bool present_ok = swapchain.Present();
    VideoCore::InvalidationOverlapTrace::EndPresentV2(overlap_present_v2,
                                                      present_ok);
    present_stage_profile.End(PresenterCpuStage::QueuePresent);
    present_stage_profile.RecordQueuePresentResult(present_ok);
    ExecutorVkGpuTimelineQueuePresentEnd(queue_present_sequence,
                                         queue_present_begin_ns, present_ok);
    if (!present_ok) {
        if (swapchain.IsTerminal()) {
            MarkTerminal(swapchain.TerminalResult(), "videoout_queue_present");
        } else {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    } else {
        g_executor_live_present_count.fetch_add(1, std::memory_order_relaxed);
        Executor::AndroidPerformanceHint::MaintainSessionThreads();
    }

    free_frame();
    if (!is_reusing_frame) {
        DebugState.IncFlipFrameNum();
    }
}

Frame* Presenter::GetRenderFrame() {
    if (IsDeviceLost()) {
        MarkDeviceLost(vk::Result::eErrorDeviceLost, "get_render_frame");
        return nullptr;
    }

    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        while (!IsDeviceLost() && !HasReclaimableFrameLocked()) {
            free_cv.wait_for(lock, std::chrono::milliseconds{250});
        }
#else
        free_cv.wait(lock,
                     [this] { return IsDeviceLost() || HasReclaimableFrameLocked(); });
#endif
        if (IsDeviceLost()) {
            lock.unlock();
            MarkDeviceLost(vk::Result::eErrorDeviceLost, "get_render_frame_queue");
            return nullptr;
        }
        if (!free_queue.empty()) {
            frame = free_queue.front();
            free_queue.pop();
            ASSERT_MSG(frame->in_free_queue,
                       "Present frame {} left a queue it did not own", frame->id);
            frame->in_free_queue = false;
        } else {
            frame = last_submit_frame;
            ASSERT_MSG(frame != nullptr && frame->repeat_readers == 0,
                       "Attempted to reclaim an actively repeated present frame");
            ASSERT_MSG(!frame->in_free_queue && !frame->recycle_after_repeat,
                       "Retained present frame {} has conflicting ownership", frame->id);
            last_submit_frame = nullptr;
        }
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size());
    }

    if (!WaitForFence(frame->present_done, "render_frame_fence")) {
        if (!IsDeviceLost()) {
            RecycleFrame(frame);
        }
        return nullptr;
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // SurfaceView already performs the requested fit/crop/stretch operation.
    // Feeding ImGui's logical content area back into the native frame pool
    // creates a second resolution domain and makes frame-slot reuse visible.
    // A fixed extent also prevents FSR images from being invalidated while an
    // older present/repeat submission can still sample them.
    const vk::Extent2D surface_extent = swapchain.GetExtent();
    expected_frame_width = surface_extent.width;
    expected_frame_height = surface_extent.height;
    return;
#endif
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

}
