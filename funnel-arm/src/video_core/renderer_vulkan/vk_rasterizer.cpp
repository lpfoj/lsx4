// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/config.h"
#include "producer_readback_bridge.h"
#include "natural_boundary_submit_budget.h"
#include <cstdlib>
#include <cstdio>

#include "common/content_fingerprint.h"
#include "common/debug.h"
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include "common/memory_patcher.h"
#endif
#include "common/scope_exit.h"
#include "core/memory.h"
#include "core/libraries/videoout/video_out.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/render_wave_trace.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "video_core/invalidation_overlap_trace.h"
#include "video_core/readback_optimization.h"
#include "video_core/runtime_proof_counters.h"
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <cstdlib>
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <unistd.h>
#endif
#include <android/log.h>
#endif
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_render_area.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/vulkan_driver_optimization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <ctime>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <vk_mem_alloc.h>

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif


#if defined(LSX4_APPLE_RUNTIME)
#include "apple_compute_timing211.h"
#endif


#if defined(LSX4_APPLE_RUNTIME)
#include "apple_compute_split_bridge214.h"
#endif

namespace VideoCore {
extern thread_local bool claude_guest_readback_origin;
extern thread_local bool claude_guest_readback_async_issued;
extern std::atomic<u32> claude_hot_written;
extern std::atomic<u64> claude_hot_flushes;
} // namespace VideoCore

namespace Vulkan {

namespace {

enum class ExecutorDrawCpuStage : std::size_t {
    PreDraw,
    PipelineLookup,
    PrepareRenderState,
    PushUserData,
    BindBuffers,
    BindTextures,
    BindSamplers,
    UploadFlush,
    VertexIndex,
    BeginRendering,
    DescriptorBind,
    CommandBind,
    DrawEmit,
    PostDraw,
    Count,
};

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)

constexpr std::size_t ExecutorDrawCpuStageCount =
    static_cast<std::size_t>(ExecutorDrawCpuStage::Count);
constexpr u64 ExecutorDrawProfilePublishIntervalNs = 1'000'000'000ull;
constexpr u64 ExecutorDrawProfileSlowObtainNs = 250'000ull;

bool ExecutorDrawStageProfileEnabled() noexcept {
    // Marker-only and cached for the process lifetime. With the marker absent,
    // no clock, present counter, aggregate, file, or log operation is reached.
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-draw-stage-profile",
                 F_OK) == 0;
    return enabled;
}

struct ExecutorDrawCpuClockSample {
    u64 wall_ns{};
    u64 thread_cpu_ns{};
};

ExecutorDrawCpuClockSample ExecutorDrawCpuNow() noexcept {
    const u64 wall_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    timespec thread_cpu{};
    const bool cpu_clock_ok =
        ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &thread_cpu) == 0;
    const u64 thread_cpu_ns =
        cpu_clock_ok
            ? static_cast<u64>(thread_cpu.tv_sec) * 1'000'000'000ull +
                  static_cast<u64>(thread_cpu.tv_nsec)
            : 0;
    return {.wall_ns = wall_ns, .thread_cpu_ns = thread_cpu_ns};
}

u64 ExecutorDrawCpuDelta(const u64 end, const u64 begin) noexcept {
    return end >= begin ? end - begin : 0;
}

struct ExecutorDrawStageProfileTotals {
    std::array<u64, ExecutorDrawCpuStageCount> stage_calls{};
    std::array<u64, ExecutorDrawCpuStageCount> stage_wall_ns{};
    std::array<u64, ExecutorDrawCpuStageCount> stage_thread_cpu_ns{};
    std::array<u64, ExecutorDrawCpuStageCount> stage_max_wall_ns{};
    u64 window{};
    u64 window_begin_wall_ns{};
    u64 first_present_sequence{};
    u64 last_present_sequence{};
    bool has_present_sequence{};
    u64 frames{};
    u64 draws{};
    u64 indexed_draws{};
    u64 emitted_draws{};
    u64 filtered_draws{};
    u64 pipeline_misses{};
    u64 bind_failures{};
    u64 total_wall_ns{};
    u64 total_thread_cpu_ns{};
    u64 max_draw_wall_ns{};
    u64 obtain_calls{};
    u64 obtain_fast_calls{};
    u64 obtain_slow_calls{};
    u64 obtain_wall_ns{};
    u64 obtain_thread_cpu_ns{};
    u64 obtain_slow_wall_ns{};
    u64 obtain_slow_thread_cpu_ns{};
    u64 obtain_max_wall_ns{};
};

thread_local ExecutorDrawStageProfileTotals executor_draw_stage_profile_totals;

class ExecutorDrawStageProfile;
thread_local ExecutorDrawStageProfile* executor_active_draw_stage_profile{};

class ExecutorDrawStageProfile {
public:
    explicit ExecutorDrawStageProfile(const bool enabled, const bool indexed) noexcept
        : enabled_{enabled}, indexed_{indexed} {
        if (!enabled_) {
            return;
        }
        previous_ = executor_active_draw_stage_profile;
        executor_active_draw_stage_profile = this;
        present_sequence_ = ExecutorGetLivePresentCount();
        draw_begin_ = ExecutorDrawCpuNow();
    }

    ~ExecutorDrawStageProfile() {
        if (!enabled_) {
            return;
        }
        const auto draw_end = ExecutorDrawCpuNow();
        EndActiveStage(draw_end);
        executor_active_draw_stage_profile = previous_;
        Commit(draw_end);
    }

    ExecutorDrawStageProfile(const ExecutorDrawStageProfile&) = delete;
    ExecutorDrawStageProfile& operator=(const ExecutorDrawStageProfile&) = delete;

    void Begin(const ExecutorDrawCpuStage stage) noexcept {
        if (!enabled_) {
            return;
        }
        const auto now = ExecutorDrawCpuNow();
        EndActiveStage(now);
        active_stage_ = stage;
        active_stage_begin_ = now;
    }

    void End(const ExecutorDrawCpuStage stage) noexcept {
        if (!enabled_ || active_stage_ != stage) {
            return;
        }
        EndActiveStage(ExecutorDrawCpuNow());
    }

    void MarkFiltered() noexcept {
        filtered_ = true;
    }

    void MarkPipelineMiss() noexcept {
        pipeline_miss_ = true;
    }

    void MarkBindFailure() noexcept {
        bind_failure_ = true;
    }

    void MarkEmitted() noexcept {
        emitted_ = true;
    }

    void RecordObtain(const ExecutorDrawCpuClockSample& begin,
                      const ExecutorDrawCpuClockSample& end) noexcept {
        const u64 wall_ns = ExecutorDrawCpuDelta(end.wall_ns, begin.wall_ns);
        const u64 thread_cpu_ns =
            ExecutorDrawCpuDelta(end.thread_cpu_ns, begin.thread_cpu_ns);
        ++obtain_calls_;
        obtain_fast_calls_ += wall_ns < ExecutorDrawProfileSlowObtainNs;
        obtain_slow_calls_ += wall_ns >= ExecutorDrawProfileSlowObtainNs;
        obtain_wall_ns_ += wall_ns;
        obtain_thread_cpu_ns_ += thread_cpu_ns;
        if (wall_ns >= ExecutorDrawProfileSlowObtainNs) {
            obtain_slow_wall_ns_ += wall_ns;
            obtain_slow_thread_cpu_ns_ += thread_cpu_ns;
        }
        obtain_max_wall_ns_ = std::max(obtain_max_wall_ns_, wall_ns);
    }

private:
    void EndActiveStage(const ExecutorDrawCpuClockSample& now) noexcept {
        if (active_stage_ == ExecutorDrawCpuStage::Count) {
            return;
        }
        const auto index = static_cast<std::size_t>(active_stage_);
        const u64 wall_ns =
            ExecutorDrawCpuDelta(now.wall_ns, active_stage_begin_.wall_ns);
        const u64 thread_cpu_ns = ExecutorDrawCpuDelta(
            now.thread_cpu_ns, active_stage_begin_.thread_cpu_ns);
        ++stage_calls_[index];
        stage_wall_ns_[index] += wall_ns;
        stage_thread_cpu_ns_[index] += thread_cpu_ns;
        stage_max_wall_ns_[index] =
            std::max(stage_max_wall_ns_[index], wall_ns);
        active_stage_ = ExecutorDrawCpuStage::Count;
    }

    void Commit(const ExecutorDrawCpuClockSample& draw_end) const noexcept {
        auto& totals = executor_draw_stage_profile_totals;
        if (totals.window_begin_wall_ns == 0) {
            totals.window_begin_wall_ns = draw_begin_.wall_ns;
        }
        if (!totals.has_present_sequence) {
            totals.has_present_sequence = true;
            totals.first_present_sequence = present_sequence_;
            totals.last_present_sequence = present_sequence_;
            totals.frames = 1;
        } else if (present_sequence_ != totals.last_present_sequence) {
            totals.frames +=
                present_sequence_ > totals.last_present_sequence
                    ? present_sequence_ - totals.last_present_sequence
                    : 1;
            totals.last_present_sequence = present_sequence_;
        }

        for (std::size_t index = 0; index < ExecutorDrawCpuStageCount; ++index) {
            totals.stage_calls[index] += stage_calls_[index];
            totals.stage_wall_ns[index] += stage_wall_ns_[index];
            totals.stage_thread_cpu_ns[index] += stage_thread_cpu_ns_[index];
            totals.stage_max_wall_ns[index] =
                std::max(totals.stage_max_wall_ns[index], stage_max_wall_ns_[index]);
        }
        ++totals.draws;
        totals.indexed_draws += indexed_;
        totals.emitted_draws += emitted_;
        totals.filtered_draws += filtered_;
        totals.pipeline_misses += pipeline_miss_;
        totals.bind_failures += bind_failure_;
        const u64 draw_wall_ns =
            ExecutorDrawCpuDelta(draw_end.wall_ns, draw_begin_.wall_ns);
        totals.total_wall_ns += draw_wall_ns;
        totals.total_thread_cpu_ns += ExecutorDrawCpuDelta(
            draw_end.thread_cpu_ns, draw_begin_.thread_cpu_ns);
        totals.max_draw_wall_ns =
            std::max(totals.max_draw_wall_ns, draw_wall_ns);
        totals.obtain_calls += obtain_calls_;
        totals.obtain_fast_calls += obtain_fast_calls_;
        totals.obtain_slow_calls += obtain_slow_calls_;
        totals.obtain_wall_ns += obtain_wall_ns_;
        totals.obtain_thread_cpu_ns += obtain_thread_cpu_ns_;
        totals.obtain_slow_wall_ns += obtain_slow_wall_ns_;
        totals.obtain_slow_thread_cpu_ns += obtain_slow_thread_cpu_ns_;
        totals.obtain_max_wall_ns =
            std::max(totals.obtain_max_wall_ns, obtain_max_wall_ns_);

        if (draw_end.wall_ns - totals.window_begin_wall_ns <
            ExecutorDrawProfilePublishIntervalNs) {
            return;
        }
        Publish(draw_end.wall_ns);
    }

    static void Publish(const u64 publish_wall_ns) noexcept {
        auto& totals = executor_draw_stage_profile_totals;
        const double frames = static_cast<double>(std::max<u64>(totals.frames, 1));
        const auto ms_per_frame = [frames](const u64 ns) {
            return static_cast<double>(ns) / 1.0e6 / frames;
        };
        const auto ms = [](const u64 ns) {
            return static_cast<double>(ns) / 1.0e6;
        };
        const auto off_cpu = [](const u64 wall_ns, const u64 thread_cpu_ns) {
            return wall_ns > thread_cpu_ns ? wall_ns - thread_cpu_ns : 0;
        };
        u64 accounted_wall_ns{};
        u64 accounted_thread_cpu_ns{};
        for (std::size_t index = 0; index < ExecutorDrawCpuStageCount; ++index) {
            accounted_wall_ns += totals.stage_wall_ns[index];
            accounted_thread_cpu_ns += totals.stage_thread_cpu_ns[index];
        }
        const u64 other_wall_ns =
            totals.total_wall_ns > accounted_wall_ns
                ? totals.total_wall_ns - accounted_wall_ns
                : 0;
        const u64 other_thread_cpu_ns =
            totals.total_thread_cpu_ns > accounted_thread_cpu_ns
                ? totals.total_thread_cpu_ns - accounted_thread_cpu_ns
                : 0;

        std::array<char, 3584> line{};
        std::size_t used = 0;
        const auto append = [&]<typename... Args>(const char* format,
                                                  Args... args) {
            if (used >= line.size() - 1) {
                return;
            }
            const int written = std::snprintf(line.data() + used,
                                              line.size() - used, format,
                                              args...);
            if (written > 0) {
                used += std::min<std::size_t>(
                    static_cast<std::size_t>(written), line.size() - used - 1);
            }
        };

        append("schema=1 window=%llu intervalMs=%.3f presentFirst=%llu "
               "presentLast=%llu frames=%llu draws=%llu indexed=%llu "
               "emitted=%llu filtered=%llu pipelineMiss=%llu bindFail=%llu "
               "totalWallMsPerFrame=%.3f totalCpuMsPerFrame=%.3f "
               "totalOffCpuMsPerFrame=%.3f otherWallMsPerFrame=%.3f "
               "otherCpuMsPerFrame=%.3f otherOffCpuMsPerFrame=%.3f "
               "maxDrawWallMs=%.3f",
               static_cast<unsigned long long>(totals.window + 1),
               ms(publish_wall_ns - totals.window_begin_wall_ns),
               static_cast<unsigned long long>(totals.first_present_sequence),
               static_cast<unsigned long long>(totals.last_present_sequence),
               static_cast<unsigned long long>(totals.frames),
               static_cast<unsigned long long>(totals.draws),
               static_cast<unsigned long long>(totals.indexed_draws),
               static_cast<unsigned long long>(totals.emitted_draws),
               static_cast<unsigned long long>(totals.filtered_draws),
               static_cast<unsigned long long>(totals.pipeline_misses),
               static_cast<unsigned long long>(totals.bind_failures),
               ms_per_frame(totals.total_wall_ns),
               ms_per_frame(totals.total_thread_cpu_ns),
               ms_per_frame(off_cpu(totals.total_wall_ns,
                                    totals.total_thread_cpu_ns)),
               ms_per_frame(other_wall_ns),
               ms_per_frame(other_thread_cpu_ns),
               ms_per_frame(off_cpu(other_wall_ns, other_thread_cpu_ns)),
               ms(totals.max_draw_wall_ns));

        static constexpr std::array<const char*, ExecutorDrawCpuStageCount>
            StageNames{"preDraw", "pipeline", "prepare", "pushUserData",
                       "buffers", "textures", "samplers", "uploadFlush",
                       "vertexIndex", "beginRendering", "descriptorBind",
                       "commandBind", "drawEmit", "postDraw"};
        for (std::size_t index = 0; index < ExecutorDrawCpuStageCount; ++index) {
            append(" %sCalls=%llu %sWallMsPerFrame=%.3f "
                   "%sCpuMsPerFrame=%.3f %sOffCpuMsPerFrame=%.3f "
                   "%sMaxWallMs=%.3f",
                   StageNames[index],
                   static_cast<unsigned long long>(totals.stage_calls[index]),
                   StageNames[index], ms_per_frame(totals.stage_wall_ns[index]),
                   StageNames[index],
                   ms_per_frame(totals.stage_thread_cpu_ns[index]),
                   StageNames[index],
                   ms_per_frame(off_cpu(totals.stage_wall_ns[index],
                                        totals.stage_thread_cpu_ns[index])),
                   StageNames[index], ms(totals.stage_max_wall_ns[index]));
        }
        append(" obtainCalls=%llu obtainFast=%llu obtainSlow=%llu "
               "obtainSlowThresholdUs=%llu obtainWallMsPerFrame=%.3f "
               "obtainCpuMsPerFrame=%.3f obtainOffCpuMsPerFrame=%.3f "
               "obtainSlowWallMsPerFrame=%.3f "
               "obtainSlowOffCpuMsPerFrame=%.3f obtainMaxWallMs=%.3f",
               static_cast<unsigned long long>(totals.obtain_calls),
               static_cast<unsigned long long>(totals.obtain_fast_calls),
               static_cast<unsigned long long>(totals.obtain_slow_calls),
               static_cast<unsigned long long>(
                   ExecutorDrawProfileSlowObtainNs / 1'000),
               ms_per_frame(totals.obtain_wall_ns),
               ms_per_frame(totals.obtain_thread_cpu_ns),
               ms_per_frame(off_cpu(totals.obtain_wall_ns,
                                    totals.obtain_thread_cpu_ns)),
               ms_per_frame(totals.obtain_slow_wall_ns),
               ms_per_frame(off_cpu(totals.obtain_slow_wall_ns,
                                    totals.obtain_slow_thread_cpu_ns)),
               ms(totals.obtain_max_wall_ns));

        if (FILE* out = std::fopen(
                "/data/data/app.lsx4.android/files/runtime/"
                "draw-stage-profile.txt",
                "w")) {
            std::fputs(line.data(), out);
            std::fputc('\n', out);
            std::fclose(out);
        }
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_DRAW_STAGE_PROFILE] %s", line.data());

        const u64 next_window = totals.window + 1;
        totals = {};
        totals.window = next_window;
        totals.window_begin_wall_ns = publish_wall_ns;
    }

    bool enabled_{};
    bool indexed_{};
    bool emitted_{};
    bool filtered_{};
    bool pipeline_miss_{};
    bool bind_failure_{};
    u64 present_sequence_{};
    ExecutorDrawStageProfile* previous_{};
    ExecutorDrawCpuClockSample draw_begin_{};
    ExecutorDrawCpuStage active_stage_{ExecutorDrawCpuStage::Count};
    ExecutorDrawCpuClockSample active_stage_begin_{};
    std::array<u64, ExecutorDrawCpuStageCount> stage_calls_{};
    std::array<u64, ExecutorDrawCpuStageCount> stage_wall_ns_{};
    std::array<u64, ExecutorDrawCpuStageCount> stage_thread_cpu_ns_{};
    std::array<u64, ExecutorDrawCpuStageCount> stage_max_wall_ns_{};
    u64 obtain_calls_{};
    u64 obtain_fast_calls_{};
    u64 obtain_slow_calls_{};
    u64 obtain_wall_ns_{};
    u64 obtain_thread_cpu_ns_{};
    u64 obtain_slow_wall_ns_{};
    u64 obtain_slow_thread_cpu_ns_{};
    u64 obtain_max_wall_ns_{};
};

class ExecutorDrawStageScope {
public:
    explicit ExecutorDrawStageScope(const ExecutorDrawCpuStage stage) noexcept
        : profile_{executor_active_draw_stage_profile}, stage_{stage} {
        if (profile_ != nullptr) {
            profile_->Begin(stage_);
        }
    }

    ~ExecutorDrawStageScope() {
        End();
    }

    void End() noexcept {
        if (profile_ == nullptr) {
            return;
        }
        profile_->End(stage_);
        profile_ = nullptr;
    }

private:
    ExecutorDrawStageProfile* profile_{};
    ExecutorDrawCpuStage stage_{};
};

class ExecutorDrawObtainProfileScope {
public:
    ExecutorDrawObtainProfileScope() noexcept
        : profile_{executor_active_draw_stage_profile} {
        if (profile_ != nullptr) {
            begin_ = ExecutorDrawCpuNow();
        }
    }

    ~ExecutorDrawObtainProfileScope() {
        if (profile_ != nullptr) {
            profile_->RecordObtain(begin_, ExecutorDrawCpuNow());
        }
    }

private:
    ExecutorDrawStageProfile* profile_{};
    ExecutorDrawCpuClockSample begin_{};
};

#else

constexpr bool ExecutorDrawStageProfileEnabled() noexcept {
    return false;
}

class ExecutorDrawStageProfile {
public:
    explicit ExecutorDrawStageProfile(bool, bool) noexcept {}
    void Begin(ExecutorDrawCpuStage) noexcept {}
    void End(ExecutorDrawCpuStage) noexcept {}
    void MarkFiltered() noexcept {}
    void MarkPipelineMiss() noexcept {}
    void MarkBindFailure() noexcept {}
    void MarkEmitted() noexcept {}
};

class ExecutorDrawStageScope {
public:
    explicit ExecutorDrawStageScope(ExecutorDrawCpuStage) noexcept {}
    void End() noexcept {}
};

class ExecutorDrawObtainProfileScope {};

#endif

// Avoid initializing the full per-draw profiler state in the production hot
// path. The state is deliberately large (it contains four per-stage counter
// arrays), and constructing a disabled direct instance makes the compiler clear
// it on every draw. A disengaged optional initializes only tiny bookkeeping;
// when the diagnostic marker is enabled, the contained profiler and its TLS
// lifetime are identical to the previous direct instance.
class ExecutorOptionalDrawStageProfile {
public:
    explicit ExecutorOptionalDrawStageProfile(const bool enabled,
                                              const bool indexed) noexcept {
        if (enabled) {
            profile_.emplace(true, indexed);
        }
    }

    void Begin(const ExecutorDrawCpuStage stage) noexcept {
        if (profile_) {
            profile_->Begin(stage);
        }
    }

    void End(const ExecutorDrawCpuStage stage) noexcept {
        if (profile_) {
            profile_->End(stage);
        }
    }

    void MarkFiltered() noexcept {
        if (profile_) {
            profile_->MarkFiltered();
        }
    }

    void MarkPipelineMiss() noexcept {
        if (profile_) {
            profile_->MarkPipelineMiss();
        }
    }

    void MarkBindFailure() noexcept {
        if (profile_) {
            profile_->MarkBindFailure();
        }
    }

    void MarkEmitted() noexcept {
        if (profile_) {
            profile_->MarkEmitted();
        }
    }

private:
    std::optional<ExecutorDrawStageProfile> profile_;
};

template <typename Func>
decltype(auto) ExecutorDrawProfileObtain(Func&& func) {
    ExecutorDrawObtainProfileScope profile_scope;
    return std::forward<Func>(func)();
}

} // namespace

namespace {
thread_local bool executor_draining_guest_invalidations = false;
constexpr size_t ExecutorNestedGuestInvalidationCapacity = 4096;
constexpr auto ExecutorGuestInvalidationDrainWatchdogPeriod =
    std::chrono::milliseconds{100};
constexpr u32 ExecutorGuestInvalidationMaxWatchdogRescues = 1;
struct ExecutorNestedGuestInvalidationQueue {
    std::array<std::pair<VAddr, u64>,
               ExecutorNestedGuestInvalidationCapacity>
        ranges{};
    size_t count{};
};
thread_local ExecutorNestedGuestInvalidationQueue
    executor_nested_guest_invalidations;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
std::atomic<u64> executor_guest_invalidation_requests{0};
std::atomic<u64> executor_guest_invalidation_completions{0};
std::atomic<u64> executor_guest_invalidation_retries{0};
std::atomic<u64> executor_guest_invalidation_nested{0};
std::atomic<u64> executor_guest_invalidation_batches{0};
std::atomic<u64> executor_guest_invalidation_drain_rounds{0};
std::atomic<u64> executor_guest_invalidation_catchup_requests{0};
std::atomic<u64> executor_guest_invalidation_wait_ns{0};
std::atomic<u64> executor_guest_invalidation_wait_max_ns{0};
std::atomic<u64> executor_guest_invalidation_queue_max{0};
std::atomic<u64> executor_guest_invalidation_nested_overflows{0};
std::atomic<u64> executor_runtime_proof_invalid_mask{0};
std::atomic<u64> executor_runtime_proof_epoch{0};
std::atomic<u64> executor_runtime_proof_query_sequence{0};
std::atomic<bool> executor_runtime_proof_sequence_saturated{false};
std::atomic<bool> executor_runtime_proof_initialized{false};

constexpr u64 ExecutorRuntimeProofRequestedMask =
    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_REQUESTS |
    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_COMPLETIONS |
    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_WAIT_NS |
    LSX4_FUNNEL_RUNTIME_PROOF_OWNED_GENERATED_WRITE_FAULTS |
    LSX4_FUNNEL_RUNTIME_PROOF_FASTMEM_MISSES;
constexpr u64 ExecutorRuntimeProofSupportedMask =
    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_REQUESTS |
    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_COMPLETIONS |
    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_WAIT_NS |
    LSX4_FUNNEL_RUNTIME_PROOF_OWNED_GENERATED_WRITE_FAULTS;
static_assert((ExecutorRuntimeProofRequestedMask &
               LSX4_FUNNEL_RUNTIME_PROOF_FASTMEM_MISSES) != 0);
static_assert((ExecutorRuntimeProofSupportedMask &
               LSX4_FUNNEL_RUNTIME_PROOF_FASTMEM_MISSES) == 0);

bool ExecutorGuestInvalidationProfileEnabled() noexcept {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-guest-invalidation-profile",
                 F_OK) == 0;
    return enabled;
}

u64 ExecutorRuntimeProofCounterAdd(std::atomic<u64>& counter,
                                   const u64 delta,
                                   const u64 metric_bit) noexcept {
    const u64 previous = counter.fetch_add(delta, std::memory_order_relaxed);
    if (delta > ~u64{0} - previous) {
        executor_runtime_proof_invalid_mask.fetch_or(metric_bit,
                                                      std::memory_order_release);
    }
    return previous;
}

void ExecutorInitializeRuntimeProofCounters() noexcept {
    executor_runtime_proof_initialized.store(false, std::memory_order_release);
    const bool enabled = ExecutorGuestInvalidationProfileEnabled();
    executor_runtime_proof_epoch.store(0, std::memory_order_relaxed);
    executor_runtime_proof_query_sequence.store(0, std::memory_order_relaxed);
    executor_runtime_proof_sequence_saturated.store(false,
                                                     std::memory_order_relaxed);
    executor_runtime_proof_invalid_mask.store(0, std::memory_order_relaxed);
    if (!enabled) {
        return;
    }

    executor_guest_invalidation_requests.store(0, std::memory_order_relaxed);
    executor_guest_invalidation_completions.store(0, std::memory_order_relaxed);
    executor_guest_invalidation_retries.store(0, std::memory_order_relaxed);
    executor_guest_invalidation_nested.store(0, std::memory_order_relaxed);
    executor_guest_invalidation_batches.store(0, std::memory_order_relaxed);
    executor_guest_invalidation_drain_rounds.store(0, std::memory_order_relaxed);
    executor_guest_invalidation_catchup_requests.store(0,
                                                       std::memory_order_relaxed);
    executor_guest_invalidation_wait_ns.store(0, std::memory_order_relaxed);
    executor_guest_invalidation_wait_max_ns.store(0, std::memory_order_relaxed);
    executor_guest_invalidation_queue_max.store(0, std::memory_order_relaxed);
    executor_guest_invalidation_nested_overflows.store(0,
                                                       std::memory_order_relaxed);
    u64 epoch = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (epoch == 0) {
        epoch = 1;
    }
    executor_runtime_proof_epoch.store(epoch, std::memory_order_release);
    executor_runtime_proof_initialized.store(true, std::memory_order_release);
}

bool ExecutorVulkanOptimizationTelemetryEnabled() noexcept {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-vulkan-optimization-stats",
                 F_OK) == 0;
    return enabled;
}

bool ExecutorBoundedGuestInvalidationDrainEnabled() noexcept {
    static const bool enabled = [] {
        const char* value =
            std::getenv("EXECUTOR_BOUNDED_GUEST_INVALIDATION_DRAIN");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

bool ExecutorGpuContentProbeBuffersEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_GPU_CONTENT_PROBE_BUFFERS");
        return value == nullptr || value[0] == '\0' || value[0] != '0';
    }();
    return enabled;
}

bool ExecutorElideTurnipCompletionSidecars(
    const Vulkan::Instance& instance) noexcept {
    static const bool diagnostic_requested =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                  "run-diagnostic-turnip-elide-completion-sidecar",
                  F_OK) == 0;
    static const bool exact_a840_disabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-bisect-disable-a840-turnip-completion-sidecar-elision",
                 F_OK) == 0;
    const auto& compatibility = instance.GetCapabilityProfile();
    const bool exact_a840_default =
        compatibility.completion_sidecar.elide_by_default &&
        !exact_a840_disabled;
    if (!compatibility.completion_sidecar.diagnostic_allowed ||
        (!diagnostic_requested && !exact_a840_default)) {
        return false;
    }
    static std::atomic_flag logged = ATOMIC_FLAG_INIT;
    if (!logged.test_and_set(std::memory_order_relaxed)) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_TURNIP_COMPLETION_SIDECAR] enabled=1 mode=%s "
            "device=0x%x resourceHazards=preserved",
            exact_a840_default ? "a840-default" : "diagnostic",
            instance.GetDeviceID());
    }
    return true;
}

bool ExecutorComputeDispatchPhaseDiagnosticEnabled() noexcept {
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "diag-compute-dispatch-phase",
                 F_OK) == 0;
    return enabled;
}

void UpdateAtomicMaximum(std::atomic<u64>& value, const u64 candidate) noexcept {
    u64 previous = value.load(std::memory_order_relaxed);
    while (previous < candidate &&
           !value.compare_exchange_weak(previous, candidate,
                                        std::memory_order_relaxed)) {
    }
}
#endif
void DeferNestedGuestInvalidation(const VAddr addr, const u64 size) noexcept {
    if (size == 0) {
        return;
    }
    const VAddr end = size > std::numeric_limits<VAddr>::max() - addr
                          ? std::numeric_limits<VAddr>::max()
                          : addr + size;
    auto& queue = executor_nested_guest_invalidations;
    VAddr merged_begin = addr;
    VAddr merged_end = end;
    size_t output = 0;
    for (size_t i = 0; i < queue.count; ++i) {
        const auto [range_begin, range_size] = queue.ranges[i];
        const VAddr range_end =
            range_size > std::numeric_limits<VAddr>::max() - range_begin
                ? std::numeric_limits<VAddr>::max()
                : range_begin + range_size;
        if (merged_begin <= range_end && range_begin <= merged_end) {
            merged_begin = std::min(merged_begin, range_begin);
            merged_end = std::max(merged_end, range_end);
        } else {
            queue.ranges[output++] = queue.ranges[i];
        }
    }
    if (output < queue.ranges.size()) {
        queue.ranges[output++] = {merged_begin, merged_end - merged_begin};
        queue.count = output;
        return;
    }

    // Signal context cannot allocate. This queue is as large as the complete
    // epoch table and is emptied after every invalidation; exceeding it means
    // broken recursive progress. Widening disjoint pages here would violate
    // the exact-page cache contract and recreate intermittent atlas damage.
    queue.count = output;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    executor_guest_invalidation_nested_overflows.fetch_add(
        1, std::memory_order_relaxed);
#endif
    ASSERT_MSG(false, "Nested guest invalidation queue overflow");
}

template <typename Invalidate>
bool DrainNestedGuestInvalidations(Invalidate&& invalidate, bool result) {
    auto& queue = executor_nested_guest_invalidations;
    while (queue.count != 0) {
        const auto [address, size] = queue.ranges[--queue.count];
        result = invalidate(address, size) && result;
    }
    return result;
}
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {
bool ExecutorAndroidRendererProbesEnabled() {
    static const bool enabled =
        std::getenv("EXECUTOR_ANDROID_RENDERER_PROBES") != nullptr ||
        ::access("/data/data/app.lsx4.android/files/lsx4-home/diag-renderer-probes",
                 F_OK) == 0;
    return enabled;
}

// Before scaling an oversized offscreen target we must know what it is. Record
// format and pixel format of every bound colour attachment, keyed by extent, so
// the 4096-class surfaces can be identified instead of guessed at.
struct AttachClass {
    struct Row { std::uint32_t w, h, fmt, use; std::uint64_t binds; };
    Row rows[24]{};
    std::mutex lock;
    std::uint64_t n{0};
    void note(std::uint32_t w, std::uint32_t h, std::uint32_t fmt, std::uint32_t use) {
        std::scoped_lock lk{lock};
        ++n;
        for (auto& r : rows) {
            if (r.binds && r.w == w && r.h == h && r.fmt == fmt && r.use == use) { ++r.binds; goto dump; }
            if (!r.binds) { r = {w, h, fmt, use, 1}; goto dump; }
        }
    dump:
        if ((n % 5000) != 0) return;
        if (FILE* out = std::fopen(
                "/data/data/app.lsx4.android/files/runtime/attach-class.txt", "w")) {
            std::fprintf(out, "binds_total=%llu", (unsigned long long)n);
            std::fputc(10, out);
            for (const auto& r : rows) {
                if (!r.binds) continue;
                std::fprintf(out,
                             "%ux%u fmt=%u binds=%llu texture=%u storage=%u rt=%u depth=%u vo=%u",
                             r.w, r.h, r.fmt, (unsigned long long)r.binds,
                             (r.use >> 0) & 1u, (r.use >> 1) & 1u, (r.use >> 2) & 1u,
                             (r.use >> 3) & 1u, (r.use >> 4) & 1u);
                std::fputc(10, out);
            }
            std::fclose(out);
        }
    }
};
AttachClass g_attach_class;
}
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {
// Attachment size is not work: a huge shadow atlas is drawn one small viewport at
// a time. Count the pixels a draw can actually touch, grouped by attachment, so
// the decision about shadows rests on covered area rather than texture size.
struct ShadeArea {
    struct Row { std::uint32_t w, h; std::uint64_t draws; double px; };
    Row rows[16]{};
    std::mutex lock;
    std::uint64_t n{0};
    void note(std::uint32_t aw, std::uint32_t ah, double px) {
        std::scoped_lock lk{lock};
        ++n;
        for (auto& r : rows) {
            if (r.draws && r.w == aw && r.h == ah) { ++r.draws; r.px += px; goto dump; }
            if (!r.draws) { r = {aw, ah, 1, px}; goto dump; }
        }
    dump:
        if ((n % 20000) != 0) return;
        if (FILE* out = std::fopen(
                "/data/data/app.lsx4.android/files/runtime/shade-area.txt", "w")) {
            std::fprintf(out, "draws_total=%llu", (unsigned long long)n);
            std::fputc(10, out);
            for (const auto& r : rows) {
                if (!r.draws) continue;
                std::fprintf(out, "attach %ux%u draws=%llu covered_Mpx=%.1f",
                             r.w, r.h, (unsigned long long)r.draws, r.px / 1e6);
                std::fputc(10, out);
            }
            std::fclose(out);
        }
    }
};
ShadeArea g_shade_area;
}
#endif

bool Rasterizer::IsDeviceTerminal() const noexcept {
    return scheduler.IsDeviceLost();
}

void Rasterizer::MarkDeviceTerminal(vk::Result result, const char* where) noexcept {
    scheduler.MarkTerminal(result, where);
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
static u64 ExecutorVkPipelineHandle(vk::Pipeline pipeline) noexcept {
    return reinterpret_cast<u64>(static_cast<VkPipeline>(pipeline));
}

static const std::vector<u64>& ExecutorBisectSkippedPipelineHashes() noexcept {
    // Android's process environment is fixed before renderer construction. Parse the
    // diagnostic list once so ordinary draws never enter getenv/strtoull.
    static const std::vector<u64> hashes = [] {
        std::vector<u64> parsed;
        const char* cursor = std::getenv("EXECUTOR_BISECT_SKIP_PIPELINE_HASHES");
        if (cursor == nullptr) {
            return parsed;
        }
        while (*cursor != '\0') {
            while (*cursor == ' ' || *cursor == ',' || *cursor == ';' || *cursor == '\t') {
                ++cursor;
            }
            if (*cursor == '\0') {
                break;
            }
            char* end{};
            const u64 candidate = std::strtoull(cursor, &end, 0);
            if (end == cursor) {
                ++cursor;
                continue;
            }
            parsed.push_back(candidate);
            cursor = end;
        }
        std::sort(parsed.begin(), parsed.end());
        parsed.erase(std::unique(parsed.begin(), parsed.end()), parsed.end());
        return parsed;
    }();
    return hashes;
}

static bool ExecutorBisectSkipPipeline(u64 pipeline_hash) noexcept {
    const auto& hashes = ExecutorBisectSkippedPipelineHashes();
    return std::binary_search(hashes.begin(), hashes.end(), pipeline_hash);
}

static bool ExecutorUsesExactA750TurnipScopePostBindDynamicState(
    const Instance& instance) noexcept {
    static const bool exact_turnip_a750_d9_requested =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-sm8650-a750-turnip-post-bind-dynamic-ab",
                 F_OK) == 0;
    const auto& profile = instance.GetCapabilityProfile();
    const bool exact_turnip_a750_d9 =
        exact_turnip_a750_d9_requested &&
        profile.exact_mesa_turnip_a750_d9_lineage;
    return exact_turnip_a750_d9;
}

static bool ExecutorUsesExactA750TurnipLoadingTransitionDrain(
    const Instance& instance) noexcept {
    static const bool requested =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-sm8650-a750-turnip-loading-transition-drain-ab",
                 F_OK) == 0;
    const auto& profile = instance.GetCapabilityProfile();
    return requested && profile.exact_mesa_turnip_a750_d9_lineage &&
           profile.memory_policy.residency_retirement ==
               ResidencyRetirementMode::CoordinatedSustainedNonBlocking;
}

enum class ExecutorDmaCleanEpochDiagnosticAxis : u8 {
    Disabled,
    Sm8650A750,
    Sm8850A840,
};

static ExecutorDmaCleanEpochDiagnosticAxis
ExecutorBloodborneDmaCleanEpochDiagnosticAxis(
    const Instance& instance) noexcept {
    // The marker probe is resolved once. Marker-absent production runs never
    // touch counters or serials and retain the unconditional synchronization.
    static const bool sm8650_requested =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-sm8650-a750-bb-dma-clean-epoch-diag",
                 F_OK) == 0;
    static const bool sm8850_requested =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-sm8850-a840-bb-dma-clean-epoch-diag",
                 F_OK) == 0;
    if (MemoryPatcher::g_game_serial != "CUSA03173") {
        return ExecutorDmaCleanEpochDiagnosticAxis::Disabled;
    }
    const auto& profile = instance.GetCapabilityProfile();
    if (sm8650_requested && profile.exact_mesa_turnip_adreno750 &&
        profile.mobile_driver_build ==
            MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats) {
        return ExecutorDmaCleanEpochDiagnosticAxis::Sm8650A750;
    }
    if (sm8850_requested && profile.exact_mesa_turnip_adreno840) {
        return ExecutorDmaCleanEpochDiagnosticAxis::Sm8850A840;
    }
    return ExecutorDmaCleanEpochDiagnosticAxis::Disabled;
}

static bool ExecutorBloodborneEmptyPhysicalDrsCleanupFastNoop() noexcept {
    static const bool preserve_empty_off_cleanup =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-preserve-empty-physical-drs-draw-cleanup",
                 F_OK) == 0;
    return !preserve_empty_off_cleanup &&
           MemoryPatcher::g_game_serial == "CUSA03173";
}

constexpr bool ExecutorDmaCleanEpochWouldSkip(
    const bool baseline_valid, const bool prior_sync_failed,
    const bool pending_transfers, const bool fex_writer_activity,
    const u64 cpu_publication_serial,
    const u64 baseline_cpu_publication_serial, const u64 mapping_serial,
    const u64 baseline_mapping_serial, const u64 topology_serial,
    const u64 baseline_topology_serial, const u64 transfer_serial,
    const u64 baseline_transfer_serial) noexcept {
    return baseline_valid && !prior_sync_failed && !pending_transfers &&
           !fex_writer_activity &&
           cpu_publication_serial == baseline_cpu_publication_serial &&
           mapping_serial == baseline_mapping_serial &&
           topology_serial == baseline_topology_serial &&
           transfer_serial == baseline_transfer_serial;
}

static_assert(ExecutorDmaCleanEpochWouldSkip(true, false, false, false, 7, 7,
                                            11, 11, 13, 13, 17, 17));
static_assert(!ExecutorDmaCleanEpochWouldSkip(false, false, false, false, 7, 7,
                                             11, 11, 13, 13, 17, 17));
static_assert(!ExecutorDmaCleanEpochWouldSkip(true, false, false, false, 8, 7,
                                             11, 11, 13, 13, 17, 17));
static_assert(!ExecutorDmaCleanEpochWouldSkip(true, false, true, false, 7, 7,
                                             11, 11, 13, 13, 17, 17));
static_assert(!ExecutorDmaCleanEpochWouldSkip(true, false, false, true, 7, 7,
                                             11, 11, 13, 13, 17, 17));
static_assert(!ExecutorDmaCleanEpochWouldSkip(true, false, false, false, 7, 7,
                                             11, 11, 13, 13, 18, 17));

static bool ExecutorNeedsQualcommPostBindDynamicState(const Instance& instance) noexcept {
    static const bool disabled_for_bisection =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-bisect-disable-qualcomm-post-bind",
                 F_OK) == 0;
    const auto& profile = instance.GetCapabilityProfile();
    return !disabled_for_bisection &&
           (ExecutorUsesExactA750TurnipScopePostBindDynamicState(instance) ||
            (profile.HasQuirk(CapabilityQuirk::RequirePostBindDynamicState) &&
             !instance.IsExtendedDynamicState3Supported()));
}

static void ExecutorLogQualcommPostBindDynamicState() noexcept {
    static std::atomic_flag logged = ATOMIC_FLAG_INIT;
    if (!logged.test_and_set(std::memory_order_relaxed)) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_POST_BIND_PIPELINE_ORDER] enabled=1 "
            "order=prepare_all_then_pipeline_vertex_index_dynamic scope_cached "
            "direct_and_indirect");
    }
}

void Rasterizer::ExecutorBindGuestGraphicsPipeline(const GraphicsPipeline* pipeline,
                                                   const vk::CommandBuffer cmdbuf) {
    const bool needs_post_bind_dynamic = ExecutorNeedsQualcommPostBindDynamicState(instance);
    const bool exact_turnip_scope_replay =
        ExecutorUsesExactA750TurnipScopePostBindDynamicState(instance);
    auto& dynamic_state = scheduler.GetDynamicState();
    const auto handle = pipeline->Handle();
    const u64 recording_tick = scheduler.CurrentTick();
    const u64 rendering_epoch = scheduler.RenderingEpoch();
    const u64 pipeline_epoch =
        scheduler.PipelineBindEpoch(vk::PipelineBindPoint::eGraphics);
    const bool dedup_enabled =
        VideoCore::IsManagedVulkanPipelineBindDeduplication();
    const bool command_epoch_changed =
        !executor_guest_graphics_pipeline_valid ||
        executor_guest_graphics_cmdbuf != cmdbuf ||
        executor_guest_graphics_recording_tick != recording_tick;
    const bool rendering_epoch_changed =
        !executor_guest_graphics_pipeline_valid ||
        executor_guest_graphics_rendering_epoch != rendering_epoch;
    const bool needs_bind = !dedup_enabled ||
                            !executor_guest_graphics_pipeline_valid ||
                            executor_guest_graphics_cmdbuf != cmdbuf ||
                            executor_guest_graphics_pipeline_identity != pipeline->Identity() ||
                            executor_guest_graphics_recording_tick != recording_tick ||
                            (needs_post_bind_dynamic &&
                             executor_guest_graphics_rendering_epoch != rendering_epoch) ||
                            executor_guest_graphics_pipeline_epoch != pipeline_epoch;
    u64 post_pipeline_epoch = pipeline_epoch;
    if (needs_bind) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
        post_pipeline_epoch =
            scheduler.MarkPipelineBind(vk::PipelineBindPoint::eGraphics);
        // Qualcomm's proprietary no-EDS3 route requires a full replay after
        // every pipeline bind. Turnip EDS3 preserves dynamic state across
        // pipeline switches; on D9 it only needs replay after a new command
        // buffer or dynamic-rendering scope. Re-invalidating on every guest
        // pipeline switch creates millions of redundant vkCmdSet* commands
        // and eventually doubles KGSL driver-private memory.
        const bool replay_dynamic_state =
            needs_post_bind_dynamic &&
            (!exact_turnip_scope_replay || command_epoch_changed ||
             rendering_epoch_changed);
        if (replay_dynamic_state) {
            dynamic_state.Invalidate();
            ExecutorLogQualcommPostBindDynamicState();
        }
    }
    VideoCore::RecordVulkanPipelineBind(!needs_bind);

    executor_guest_graphics_cmdbuf = cmdbuf;
    executor_guest_graphics_pipeline_identity = pipeline->Identity();
    executor_guest_graphics_recording_tick = recording_tick;
    executor_guest_graphics_rendering_epoch = rendering_epoch;
    executor_guest_graphics_pipeline_epoch = post_pipeline_epoch;
    executor_guest_graphics_pipeline_valid = true;

    if (ExecutorVulkanOptimizationTelemetryEnabled()) {
        static thread_local u64 attempts{};
        static thread_local u64 hits{};
        ++attempts;
        hits += static_cast<u64>(!needs_bind);
        if ((attempts & 16383u) == 0 && !VideoCore::IsExecutorLiveQuiet()) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_VK_BIND_DEDUP] kind=graphicsPipeline attempts=%llu hits=%llu "
                "hitPct=%.1f",
                static_cast<unsigned long long>(attempts),
                static_cast<unsigned long long>(hits),
                100.0 * static_cast<double>(hits) / static_cast<double>(attempts));
        }
    }
}

void Rasterizer::ExecutorSealGuestGraphicsPipelineState(
    const GraphicsPipeline* pipeline, const vk::CommandBuffer cmdbuf) {
    if (!executor_guest_graphics_pipeline_valid ||
        executor_guest_graphics_cmdbuf != cmdbuf ||
        executor_guest_graphics_pipeline_identity != pipeline->Identity() ||
        executor_guest_graphics_recording_tick != scheduler.CurrentTick() ||
        (ExecutorNeedsQualcommPostBindDynamicState(instance) &&
         executor_guest_graphics_rendering_epoch != scheduler.RenderingEpoch()) ||
        executor_guest_graphics_pipeline_epoch !=
            scheduler.PipelineBindEpoch(vk::PipelineBindPoint::eGraphics)) {
        executor_guest_graphics_pipeline_valid = false;
    }
}

void Rasterizer::ExecutorBindGuestComputePipeline(const ComputePipeline* pipeline,
                                                  const vk::CommandBuffer cmdbuf) {
    const auto handle = pipeline->Handle();
    const u64 recording_tick = scheduler.CurrentTick();
    const u64 pipeline_epoch =
        scheduler.PipelineBindEpoch(vk::PipelineBindPoint::eCompute);
    const bool dedup_enabled =
        VideoCore::IsManagedVulkanPipelineBindDeduplication();
    const bool needs_bind = !dedup_enabled ||
                            !executor_guest_compute_pipeline_valid ||
                            executor_guest_compute_cmdbuf != cmdbuf ||
                            executor_guest_compute_pipeline_identity != pipeline->Identity() ||
                            executor_guest_compute_recording_tick != recording_tick ||
                            executor_guest_compute_pipeline_epoch != pipeline_epoch;
    u64 post_pipeline_epoch = pipeline_epoch;
    if (needs_bind) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        post_pipeline_epoch =
            scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
    }
    VideoCore::RecordVulkanPipelineBind(!needs_bind);

    executor_guest_compute_cmdbuf = cmdbuf;
    executor_guest_compute_pipeline_identity = pipeline->Identity();
    executor_guest_compute_recording_tick = recording_tick;
    executor_guest_compute_pipeline_epoch = post_pipeline_epoch;
    executor_guest_compute_pipeline_valid = true;

    if (ExecutorVulkanOptimizationTelemetryEnabled()) {
        static thread_local u64 attempts{};
        static thread_local u64 hits{};
        ++attempts;
        hits += static_cast<u64>(!needs_bind);
        if ((attempts & 16383u) == 0 && !VideoCore::IsExecutorLiveQuiet()) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_VK_BIND_DEDUP] kind=computePipeline attempts=%llu hits=%llu "
                "hitPct=%.1f",
                static_cast<unsigned long long>(attempts),
                static_cast<unsigned long long>(hits),
                100.0 * static_cast<double>(hits) / static_cast<double>(attempts));
        }
    }
}

struct ExecutorDepthWriteOracleRuntime {
    std::atomic<u64> epoch{0};
    std::atomic<u64> attachment_signature{0};
};

static ExecutorDepthWriteOracleRuntime& ExecutorGetDepthWriteOracleRuntime() noexcept {
    static ExecutorDepthWriteOracleRuntime runtime{};
    return runtime;
}

static bool ExecutorDepthWriteOracleMarkerPresent() noexcept {
    return ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                    "run-depth-write-oracle",
                    F_OK) == 0;
}

static bool ExecutorClaimDepthWriteOracle(u64 attachment_signature) noexcept {
    if (!ExecutorDepthWriteOracleMarkerPresent() || attachment_signature == 0) {
        return false;
    }
    const u64 epoch = AmdGpu::RenderWaveTrace::LateFrameCaptureEpoch();
    if (epoch == 0) {
        return false;
    }
    auto& runtime = ExecutorGetDepthWriteOracleRuntime();
    u64 previous = runtime.epoch.load(std::memory_order_acquire);
    while (previous != epoch) {
        if (runtime.epoch.compare_exchange_weak(previous, epoch, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            runtime.attachment_signature.store(attachment_signature, std::memory_order_release);
            return true;
        }
    }
    return false;
}

static bool ExecutorDepthWriteOracleActive(u64 attachment_signature) noexcept {
    if (!AmdGpu::RenderWaveTrace::LateFrameCaptureActive() ||
        !ExecutorDepthWriteOracleMarkerPresent() || attachment_signature == 0) {
        return false;
    }
    const auto& runtime = ExecutorGetDepthWriteOracleRuntime();
    const u64 epoch = AmdGpu::RenderWaveTrace::LateFrameCaptureEpoch();
    return epoch != 0 && runtime.epoch.load(std::memory_order_acquire) == epoch &&
           runtime.attachment_signature.load(std::memory_order_acquire) == attachment_signature;
}

struct ExecutorVkAttachmentSummary {
    u64 signature{};
    VAddr depth_address{};
    u32 color_count{};
};

template <typename ColorTargets, typename DepthTarget>
static ExecutorVkAttachmentSummary ExecutorSummarizeAttachments(
    const ColorTargets& color_targets, const DepthTarget& depth_target) noexcept {
    ExecutorVkAttachmentSummary summary{};
    Common::ContentFingerprint64 fingerprint{Common::FingerprintDomain::AttachmentSet};
    bool any_attachment = false;
    auto mix = [&](const u64 value) {
        fingerprint.UpdateLittleEndian(value);
    };
    for (u32 index = 0; index < color_targets.size(); ++index) {
        const auto& [image_id, desc] = color_targets[index];
        if (!image_id) {
            continue;
        }
        any_attachment = true;
        ++summary.color_count;
        const auto& info = desc.info;
        mix(0xc010000000000000ull | index);
        mix(info.guest_address);
        mix(info.guest_size);
        mix((static_cast<u64>(info.size.width) << 32) | info.size.height);
        mix(static_cast<u64>(info.pixel_format));
    }
    if (depth_target.first) {
        any_attachment = true;
        const auto& info = depth_target.second.info;
        summary.depth_address = info.guest_address;
        mix(0xd320000000000000ull);
        mix(info.guest_address);
        mix(info.guest_size);
        mix((static_cast<u64>(info.size.width) << 32) | info.size.height);
        mix(static_cast<u64>(info.pixel_format));
    }
    summary.signature = any_attachment ? fingerprint.Finish() : 0;
    return summary;
}

static bool ExecutorAlignedSsboScratchEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_ALIGNED_SSBO_SCRATCH");
        return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

static bool ExecutorTracePm4Enabled() noexcept {
    static const bool enabled = std::getenv("EXECUTOR_TRACE_PM4") != nullptr;
    return enabled;
}

static bool ExecutorTraceLiveWideEnabled() noexcept {
    static const bool enabled = std::getenv("EXECUTOR_TRACE_LIVE_WIDE") != nullptr;
    return enabled;
}

static bool ExecutorTraceLivePresentResourcesEnabled() noexcept {
    static const bool enabled =
        std::getenv("EXECUTOR_TRACE_LIVE_PRESENT_RT") != nullptr;
    return enabled;
}

static bool ExecutorAdaptiveVrsProfileEnabled() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool enabled =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-diagnostic-adaptive-vrs-profile",
                 F_OK) == 0;
    return enabled;
#else
    return false;
#endif
}

struct ExecutorGuestRangeFingerprint {
    u64 digest{};
    u64 clamped_size{};
    bool readable{};
};

static ExecutorGuestRangeFingerprint ExecutorFingerprintGuestRange(Core::MemoryManager* memory,
                                                                    VAddr address, u64 size) {
    ExecutorGuestRangeFingerprint result{};
    if (address == 0 || size == 0 || !memory->IsValidMapping(address, 1)) {
        return result;
    }
    result.clamped_size = memory->ClampRangeSize(address, size);
    if (result.clamped_size == 0) {
        return result;
    }
    result.readable = true;
    Common::ContentFingerprint64 fingerprint{Common::FingerprintDomain::GuestRangeSample};
    const auto hash_bytes = [&](const u8* bytes, const u64 count) {
        fingerprint.Update(
            std::span<const std::uint8_t>{bytes, static_cast<std::size_t>(count)});
    };
    const u64 head_size = std::min<u64>(result.clamped_size, 128);
    hash_bytes(reinterpret_cast<const u8*>(address), head_size);
    if (result.clamped_size > head_size) {
        const u64 tail_size = std::min<u64>(result.clamped_size - head_size, 128);
        hash_bytes(reinterpret_cast<const u8*>(address + result.clamped_size - tail_size),
                   tail_size);
    }
    fingerprint.UpdateLittleEndian(result.clamped_size);
    result.digest = fingerprint.Finish();
    return result;
}

static void ExecutorTraceDrawnColorTarget(
    const VideoCore::TextureCache::ImageDesc* color_target) {
    static const bool enabled = std::getenv("EXECUTOR_TRACE_VIDEOOUT_SOURCE") != nullptr;
    if (!enabled) {
        return;
    }

    const auto* info = color_target ? &color_target->info : nullptr;
    const u64 cb_address = info ? info->guest_address : 0;
    const u64 cb_size = info ? info->guest_size : 0;
    Libraries::VideoOut::ExecutorVideoOutBufferSnapshot videoout{};
    (void)Libraries::VideoOut::ExecutorGetVideoOutBufferSnapshot(cb_address, cb_size, &videoout);

    const u64 vo_address = videoout.valid ? videoout.address : 0;
    const u64 meta = info ? (static_cast<u64>(info->size.width) << 32) |
                                static_cast<u64>(info->size.height)
                          : 0;

    static std::atomic<u64> seen_draws{0};
    static std::atomic<u64> emitted{0};
    static std::atomic<u64> last_cb_address{~0ull};
    static std::atomic<u64> last_vo_address{~0ull};
    static std::atomic<u64> last_extent{~0ull};
    const u64 draw_sequence = seen_draws.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool changed = last_cb_address.exchange(cb_address, std::memory_order_relaxed) !=
                             cb_address ||
                         last_vo_address.exchange(vo_address, std::memory_order_relaxed) !=
                             vo_address ||
                         last_extent.exchange(meta, std::memory_order_relaxed) != meta;
    if (draw_sequence > 4 && !changed) {
        return;
    }
    const u64 log_sequence = emitted.fetch_add(1, std::memory_order_relaxed);
    if (log_sequence >= 32) {
        return;
    }

    const u64 cb_end = cb_address + cb_size;
    const u64 vo_end = vo_address + videoout.size_bytes;
    const bool exact = cb_address != 0 && cb_address == vo_address;
    const bool overlaps = cb_address != 0 && vo_address != 0 && cb_address < vo_end &&
                          vo_address < cb_end;
    const s64 delta = static_cast<s64>(cb_address - vo_address);
    const std::string cb_format =
        info ? vk::to_string(info->pixel_format) : std::string{"none"};

    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_DRAW_VIDEOOUT_MATCH] seq=%llu draw=%llu transition=%u "
        "cb0=0x%llx cbBytes=%llu cbExtent=%ux%u cbPitch=%u cbFormat=%s cbTiled=%u "
        "voValid=%u voCurrent=%d voEffective=%d voExactAny=%d voOverlapAny=%d "
        "voRegistered=%u voAddr=0x%llx "
        "voBytes=%llu voExtent=%ux%u voPitch=%u voFormat=0x%x voTiling=%u "
        "exact=%u overlap=%u delta=%lld",
        static_cast<unsigned long long>(log_sequence + 1),
        static_cast<unsigned long long>(draw_sequence), changed ? 1u : 0u,
        static_cast<unsigned long long>(cb_address),
        static_cast<unsigned long long>(cb_size), info ? info->size.width : 0,
        info ? info->size.height : 0, info ? info->pitch : 0, cb_format.c_str(),
        info && info->props.is_tiled ? 1u : 0u, videoout.valid ? 1u : 0u,
        videoout.requested_index, videoout.effective_index, videoout.exact_address_index,
        videoout.overlapping_address_index, videoout.registered_buffers,
        static_cast<unsigned long long>(vo_address),
        static_cast<unsigned long long>(videoout.size_bytes), videoout.width, videoout.height,
        videoout.pitch_in_pixel, videoout.pixel_format, videoout.tiling_mode, exact ? 1u : 0u,
        overlaps ? 1u : 0u, static_cast<long long>(delta));
}

static u64 ExecutorCountNonzeroSparse(Core::MemoryManager* memory, VAddr address, u64 size) {
    if (!memory || address == 0 || size == 0) {
        return 0;
    }
    std::array<u8, 4096> buffer{};
    u64 nonzero = 0;
    u64 remaining = std::min<u64>(size, 4ull << 20);
    VAddr cursor = address;
    while (remaining) {
        const u64 chunk = std::min<u64>(remaining, buffer.size());
        memory->CopySparseMemory(cursor, buffer.data(), chunk);
        for (u64 i = 0; i < chunk; ++i) {
            nonzero += buffer[i] != 0;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return nonzero;
}
#endif

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
struct Rasterizer::ExecutorPhysicalDrsCleanupControl {
    std::mutex mutex;
    Rasterizer* owner{};
};
#endif

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache,
                     page_manager} {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorInitializeRuntimeProofCounters();
    (void)ExecutorVulkanOptimizationTelemetryEnabled();
    VideoCore::InvalidationOverlapTrace::Initialize();
    memory->SetRasterizer(
        this, instance.GetCapabilityProfile().exact_proprietary_adreno750);
    page_manager.CompleteInitialGpuMappingRegistration();
    if (!Config::nullGpu()) {
        liverpool->BindRasterizer(this);
    }
#else
    if (!Config::nullGpu()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(
        this, instance.GetCapabilityProfile().exact_proprietary_adreno750);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    executor_physical_drs_cleanup_control =
        std::make_shared<ExecutorPhysicalDrsCleanupControl>();
    executor_physical_drs_cleanup_control->owner = this;
    VideoCore::SetManagedMobileGpuOptimizationTransitionCallback(
        &Rasterizer::ExecutorManagedMobileGpuTransition, this);
    // Qualcomm's mobile driver serializes frequent scaled-target resolves and
    // Bloodborne invalidates its main target between passes. Prefer native
    // 2x2 fragment shading there: it removes 75% of fragment invocations
    // without scratch images, blits, or driver-visible attachment churn.
    VideoCore::SetMobileGpuPhysicalDrsAvailable(
        !instance.GetCapabilityProfile().HasQuirk(
            CapabilityQuirk::DisablePhysicalDrsResolve));
#endif
}

Rasterizer::~Rasterizer() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    executor_runtime_proof_initialized.store(false, std::memory_order_release);
    VideoCore::PageManager::ShutdownFexDeferredWriteEpochs();
    // Deferred epoch batches retain this Rasterizer in queued callbacks. A
    // FIFO barrier is required even when shutdown found no READY slots, since
    // an older asynchronous batch can still be ahead of destruction.
    if (liverpool != nullptr && !liverpool->IsRendererTerminal()) {
        liverpool->SendCommand<true>([] {});
    }
    VideoCore::SetManagedMobileGpuOptimizationTransitionCallback(nullptr,
                                                                 nullptr);
    if (executor_physical_drs_cleanup_control != nullptr) {
        std::scoped_lock control_lock{
            executor_physical_drs_cleanup_control->mutex};
        executor_physical_drs_cleanup_control->owner = nullptr;
    }
    executor_physical_drs_shutting_down = true;
    ExecutorResolvePhysicalDrs("shutdown");
    VideoCore::SetMobileGpuPhysicalDrsAvailable(false);
#endif
    if (scheduler.HasPendingBufferTransfers()
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        || !executor_physical_drs_surfaces.empty() ||
           !executor_physical_drs_depth_surfaces.empty()
#endif
    ) {
        scheduler.Finish(Scheduler::ExecutorFinishReason::Rasterizer);
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const u64 shutdown_tick = scheduler.CurrentTick();
    if (shutdown_tick > 1 && !scheduler.IsDeviceLost()) {
        scheduler.WaitRetirement(shutdown_tick - 1);
    }
#endif
    if (liverpool != nullptr) {
        liverpool->BindRasterizer(nullptr);
    }
    if (memory != nullptr) {
        memory->SetRasterizer(nullptr);
    }
}

u32 Rasterizer::MaximumDeferredGuestSubmitBoundaries() const noexcept {
    return instance.GetCapabilityProfile()
        .command_submission.maximum_deferred_guest_submit_boundaries;
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
void Rasterizer::ExecutorTouchPhysicalDrsSurface(
    ExecutorPhysicalDrsSurface& surface) noexcept {
    surface.last_used_generation = executor_physical_drs_generation;
    surface.last_use_tick =
        std::max(surface.last_use_tick, scheduler.CurrentTick());
}

void Rasterizer::ExecutorTouchPhysicalDrsDepthSurface(
    ExecutorPhysicalDrsDepthSurface& surface) noexcept {
    surface.last_used_generation = executor_physical_drs_generation;
    surface.last_use_tick =
        std::max(surface.last_use_tick, scheduler.CurrentTick());
}

u64 Rasterizer::ExecutorPhysicalDrsImageAllocationBytes(
    const vk::ImageCreateInfo& image_ci) const noexcept {
    const vk::Device device = instance.GetDevice();
    if (instance.IsMaintenance4Supported()) {
        const vk::MemoryRequirements2 requirements =
            device.getImageMemoryRequirements(
            vk::DeviceImageMemoryRequirements{
                .pCreateInfo = &image_ci,
                .planeAspect = vk::ImageAspectFlagBits::eNone,
            });
        return static_cast<u64>(requirements.memoryRequirements.size);
    }

    // Maintenance4 is optional on the degraded Android path. An unbound image
    // object provides the same exact requirements without allocating device
    // memory, so admission still happens before VMA can consume the budget.
    vk::Image probe_image{};
    const vk::Result probe_result =
        device.createImage(&image_ci, nullptr, &probe_image);
    if (probe_result != vk::Result::eSuccess) {
        return 0;
    }
    const vk::MemoryRequirements requirements =
        device.getImageMemoryRequirements(probe_image);
    device.destroyImage(probe_image);
    return static_cast<u64>(requirements.size);
}

bool Rasterizer::ExecutorTryCreatePhysicalDrsImage(
    VideoCore::UniqueImage& image, const vk::ImageCreateInfo& image_ci,
    u64& allocation_bytes) {
    allocation_bytes = ExecutorPhysicalDrsImageAllocationBytes(image_ci);
    if (allocation_bytes == 0 ||
        !ExecutorRunPhysicalDrsResidencyGc(allocation_bytes)) {
        return false;
    }

    const vk::Result result = image.TryCreate(image_ci);
    if (result == vk::Result::eSuccess) {
        return true;
    }

    static std::atomic<u32> allocation_failure_logs{0};
    const u32 log_index =
        allocation_failure_logs.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 16) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_PHYSICAL_DRS_ALLOC] result=fallback error=%s bytes=%llu",
            vk::to_string(result).c_str(),
            static_cast<unsigned long long>(allocation_bytes));
    }
    return false;
}

void Rasterizer::ExecutorManagedMobileGpuTransition(
    void* const context, const bool enabled) noexcept {
    if (context != nullptr) {
        static_cast<Rasterizer*>(context)
            ->ExecutorNotifyManagedMobileGpuTransition(enabled);
    }
}

void Rasterizer::ExecutorNotifyManagedMobileGpuTransition(
    const bool enabled) noexcept {
    {
        std::scoped_lock drs_lock{executor_physical_drs_mutex};
        if (enabled || executor_physical_drs_shutting_down) {
            return;
        }
    }

    // The option setter may run without another guest submit. Reclaim
    // already-retired scratch immediately and schedule the rest against the
    // exact timeline tick that last referenced it.
    (void)ExecutorRunPhysicalDrsResidencyGc(0, true);
    ExecutorSchedulePhysicalDrsOffCleanup();
}

void Rasterizer::ExecutorSchedulePhysicalDrsOffCleanup() {
    std::scoped_lock drs_lock{executor_physical_drs_mutex};
    if (executor_physical_drs_shutting_down ||
        VideoCore::IsManagedMobileGpuOptimization() ||
        executor_physical_drs_off_cleanup_scheduled ||
        (executor_physical_drs_surfaces.empty() &&
         executor_physical_drs_depth_surfaces.empty()) ||
        executor_physical_drs_active ||
        executor_physical_drs_building) {
        return;
    }

    u64 retire_tick{};
    for (const auto& surface : executor_physical_drs_surfaces) {
        retire_tick = std::max(retire_tick, surface.last_use_tick);
    }
    for (const auto& surface : executor_physical_drs_depth_surfaces) {
        retire_tick = std::max(retire_tick, surface.last_use_tick);
    }

    if (retire_tick == 0 || scheduler.IsFree(retire_tick)) {
        (void)ExecutorRunPhysicalDrsResidencyGc(0, true);
        if (executor_physical_drs_surfaces.empty() &&
            executor_physical_drs_depth_surfaces.empty()) {
            return;
        }
        retire_tick = 0;
        for (const auto& surface : executor_physical_drs_surfaces) {
            retire_tick = std::max(retire_tick, surface.last_use_tick);
        }
        for (const auto& surface : executor_physical_drs_depth_surfaces) {
            retire_tick = std::max(retire_tick, surface.last_use_tick);
        }
        if (retire_tick == 0 || scheduler.IsFree(retire_tick)) {
            return;
        }
    }

    const auto control = executor_physical_drs_cleanup_control;
    if (control == nullptr) {
        return;
    }
    executor_physical_drs_off_cleanup_scheduled = true;
    scheduler.DeferPriorityOperationAt(
        retire_tick, [control] {
            std::scoped_lock control_lock{control->mutex};
            if (control->owner != nullptr) {
                control->owner->ExecutorCompletePhysicalDrsOffCleanup();
            }
        });
}

void Rasterizer::ExecutorCompletePhysicalDrsOffCleanup() {
    {
        std::scoped_lock drs_lock{executor_physical_drs_mutex};
        executor_physical_drs_off_cleanup_scheduled = false;
        if (executor_physical_drs_shutting_down ||
            VideoCore::IsManagedMobileGpuOptimization()) {
            return;
        }
    }
    (void)ExecutorRunPhysicalDrsResidencyGc(0, true);
    ExecutorSchedulePhysicalDrsOffCleanup();
}

u64 Rasterizer::ExecutorPhysicalDrsResidencyBudgetBytes() const noexcept {
    constexpr u64 FallbackBudget = 64_MB;
    constexpr u64 MaximumBudget = 192_MB;
    const auto mobile = VideoCore::GetMobileGpuOptimizationStats();
    const u64 device_budget =
        mobile.memory_budget_bytes != 0
            ? mobile.memory_budget_bytes
            : (instance.CanReportMemoryUsage()
                   ? static_cast<u64>(instance.GetTotalMemoryBudget())
                   : 0);
    if (device_budget == 0) {
        return FallbackBudget;
    }

    // The mobile texture cache starts collection at its managed residency
    // threshold. Scratch receives at most one quarter of that threshold and
    // is additionally capped to one eighth of the physical device budget.
    const u64 texture_trigger =
        texture_cache.ManagedResidencyTriggerBytes();
    const u64 proportional =
        texture_trigger != 0 ? texture_trigger / 4
                             : device_budget / 16;
    const u64 floor = std::min<u64>(
        32_MB, std::max<u64>(8_MB, device_budget / 64));
    const u64 ceiling = std::max(
        floor,
        std::min<u64>(MaximumBudget, device_budget / 8));
    u64 budget = std::clamp(proportional, floor, ceiling);

    // Yield scratch residency before the canonical texture pool reaches its
    // own pressure/critical thresholds.
    if (mobile.memory_used_bytes >=
        device_budget - device_budget / 10) {
        budget = std::max(floor, budget / 2);
    }
    if (mobile.memory_used_bytes >=
        device_budget - device_budget / 32) {
        budget = floor;
    }
    return budget;
}

void Rasterizer::ExecutorResetPhysicalDrsTransientBindings() noexcept {
    executor_physical_drs_active = false;
    executor_physical_drs_draw_active = false;
    executor_physical_drs_building = false;
    executor_physical_drs_surface = nullptr;
    executor_physical_drs_depth_surface = nullptr;
    executor_physical_drs_color_surfaces.fill(nullptr);
    executor_physical_drs_color_image_ids.fill({});
    executor_physical_drs_color_image_uids.fill(0);
    executor_physical_drs_color_guest_addresses.fill(0);
    executor_physical_drs_color_mask = 0;
    executor_physical_drs_image_id = {};
    executor_physical_drs_image_uid = 0;
    executor_physical_drs_depth_image_id = {};
    executor_physical_drs_depth_image_uid = 0;
    executor_physical_drs_guest_address = 0;
    executor_physical_drs_depth_guest_address = 0;
    executor_physical_drs_scale_percent = 100;
}

bool Rasterizer::ExecutorRunPhysicalDrsResidencyGc(
    const u64 reserve_bytes, const bool force_cleanup) {
    std::scoped_lock drs_lock{executor_physical_drs_mutex};
    if (force_cleanup) {
        // OFF is a true baseline: the resolved presentation alias must not pin
        // a scratch image after managed mobile rendering is disabled.
        executor_physical_drs_present_surface = nullptr;
        executor_physical_drs_present_image_id = {};
        executor_physical_drs_present_image_uid = 0;
    }

    const u64 budget =
        force_cleanup ? 0 : ExecutorPhysicalDrsResidencyBudgetBytes();
    executor_physical_drs_budget_bytes = budget;
    if (reserve_bytes > budget) {
        return false;
    }
    const u64 target_bytes = budget - reserve_bytes;
    if (executor_physical_drs_resident_bytes <= target_bytes &&
        !force_cleanup) {
        return true;
    }

    ++executor_physical_drs_gc_passes;
    u64 evictions{};
    bool blocked_by_gpu{};
    const bool selected_surfaces_are_live =
        executor_physical_drs_active ||
        executor_physical_drs_building;
    const auto color_protected =
        [&](const ExecutorPhysicalDrsSurface& surface) {
            if (executor_physical_drs_present_surface == &surface) {
                return true;
            }
            if (!selected_surfaces_are_live) {
                return false;
            }
            if (executor_physical_drs_surface == &surface) {
                return true;
            }
            return std::ranges::find(
                       executor_physical_drs_color_surfaces,
                       &surface) !=
                   executor_physical_drs_color_surfaces.end();
        };
    const auto depth_protected =
        [&](const ExecutorPhysicalDrsDepthSurface& surface) {
            return selected_surfaces_are_live &&
                   executor_physical_drs_depth_surface == &surface;
        };
    const auto old_enough =
        [&](const u64 generation, const u64 tick) {
            if (tick != 0 && !scheduler.IsFree(tick)) {
                blocked_by_gpu = true;
                return false;
            }
            // Reserve pressure may reuse any retired inactive node. Routine
            // aging waits two submits to avoid immediate scale-thrash.
            return force_cleanup || reserve_bytes != 0 ||
                   executor_physical_drs_generation - generation >= 2;
        };

    while (executor_physical_drs_resident_bytes > target_bytes) {
        auto color_candidate =
            executor_physical_drs_surfaces.end();
        auto depth_candidate =
            executor_physical_drs_depth_surfaces.end();
        u64 oldest_generation =
            std::numeric_limits<u64>::max();
        u64 oldest_tick = std::numeric_limits<u64>::max();

        for (auto iterator =
                 executor_physical_drs_surfaces.begin();
             iterator != executor_physical_drs_surfaces.end();
             ++iterator) {
            if (color_protected(*iterator) ||
                !old_enough(iterator->last_used_generation,
                            iterator->last_use_tick)) {
                continue;
            }
            if (std::pair{iterator->last_used_generation,
                          iterator->last_use_tick} <
                std::pair{oldest_generation, oldest_tick}) {
                oldest_generation = iterator->last_used_generation;
                oldest_tick = iterator->last_use_tick;
                color_candidate = iterator;
                depth_candidate =
                    executor_physical_drs_depth_surfaces.end();
            }
        }
        for (auto iterator =
                 executor_physical_drs_depth_surfaces.begin();
             iterator !=
                 executor_physical_drs_depth_surfaces.end();
             ++iterator) {
            if (depth_protected(*iterator) ||
                !old_enough(iterator->last_used_generation,
                            iterator->last_use_tick)) {
                continue;
            }
            if (std::pair{iterator->last_used_generation,
                          iterator->last_use_tick} <
                std::pair{oldest_generation, oldest_tick}) {
                oldest_generation = iterator->last_used_generation;
                oldest_tick = iterator->last_use_tick;
                depth_candidate = iterator;
                color_candidate =
                    executor_physical_drs_surfaces.end();
            }
        }

        if (color_candidate !=
            executor_physical_drs_surfaces.end()) {
            auto* const victim = &*color_candidate;
            if (executor_physical_drs_surface == victim) {
                executor_physical_drs_surface = nullptr;
            }
            if (executor_physical_drs_present_surface == victim) {
                executor_physical_drs_present_surface = nullptr;
                executor_physical_drs_present_image_id = {};
                executor_physical_drs_present_image_uid = 0;
            }
            for (auto*& selected :
                 executor_physical_drs_color_surfaces) {
                if (selected == victim) {
                    selected = nullptr;
                }
            }
            executor_physical_drs_resident_bytes -=
                std::min(
                    executor_physical_drs_resident_bytes,
                    color_candidate->allocation_bytes);
            executor_physical_drs_surfaces.erase(
                color_candidate);
            ++evictions;
            continue;
        }
        if (depth_candidate !=
            executor_physical_drs_depth_surfaces.end()) {
            if (executor_physical_drs_depth_surface ==
                &*depth_candidate) {
                executor_physical_drs_depth_surface = nullptr;
            }
            executor_physical_drs_resident_bytes -=
                std::min(
                    executor_physical_drs_resident_bytes,
                    depth_candidate->allocation_bytes);
            executor_physical_drs_depth_surfaces.erase(
                depth_candidate);
            ++evictions;
            continue;
        }
        break;
    }

    executor_physical_drs_gc_evictions += evictions;
    if (blocked_by_gpu &&
        executor_physical_drs_resident_bytes > target_bytes) {
        ++executor_physical_drs_gc_blocked;
    }
    VideoCore::RecordMobileGpuResidencyGc(evictions);

    const bool within_budget =
        executor_physical_drs_resident_bytes <= target_bytes;
    if (executor_physical_drs_surfaces.empty() &&
        executor_physical_drs_depth_surfaces.empty()) {
        executor_physical_drs_has_residency.store(false,
                                                  std::memory_order_release);
    }
    if (evictions != 0 || !within_budget ||
        executor_physical_drs_gc_passes <= 8 ||
        (executor_physical_drs_gc_passes & 127u) == 0u) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_PHYSICAL_DRS_RESIDENCY] budgetMiB=%llu "
            "residentMiB=%llu peakMiB=%llu reserveMiB=%llu "
            "colors=%zu depths=%zu evicted=%llu totalEvicted=%llu "
            "blocked=%u force=%u generation=%llu",
            static_cast<unsigned long long>(budget / 1_MB),
            static_cast<unsigned long long>(
                executor_physical_drs_resident_bytes / 1_MB),
            static_cast<unsigned long long>(
                executor_physical_drs_peak_bytes / 1_MB),
            static_cast<unsigned long long>(reserve_bytes / 1_MB),
            executor_physical_drs_surfaces.size(),
            executor_physical_drs_depth_surfaces.size(),
            static_cast<unsigned long long>(evictions),
            static_cast<unsigned long long>(
                executor_physical_drs_gc_evictions),
            blocked_by_gpu ? 1u : 0u,
            force_cleanup ? 1u : 0u,
            static_cast<unsigned long long>(
                executor_physical_drs_generation));
    }
    return within_budget;
}

bool Rasterizer::ExecutorPhysicalDrsPipelineCompatible(
    const GraphicsPipeline* pipeline) const {
    if (pipeline == nullptr || !pipeline->IsValid() ||
        !VideoCore::MobileGpuWantsPhysicalDrs() || attachment_feedback_loop ||
        Libraries::GnmDriver::ExecutorReplayActive() ||
        texture_cache.ExecutorGpuContentProbeEnabled() ||
        AmdGpu::RenderWaveTrace::LateFrameCaptureActive()) {
        return false;
    }

    const auto& key = pipeline->GetGraphicsKey();
    const auto* fragment = pipeline->TryGetStage(Shader::LogicalStage::Fragment);
    if (fragment == nullptr || key.num_samples != 1 ||
        fragment->has_storage_images || fragment->has_image_query ||
        fragment->loads.GetAny(Shader::IR::Attribute::FragCoord) ||
        fragment->uses_buffer_atomic_float_min_max ||
        fragment->uses_image_atomic_float_min_max ||
        fragment->uses_buffer_int64_atomics) {
        return false;
    }
    for (const auto& buffer : fragment->buffers) {
        if (buffer.is_written) {
            return false;
        }
    }
    return true;
}

void Rasterizer::ExecutorPreparePhysicalDrsDraw(const GraphicsPipeline* pipeline) {
    const bool managed_mobile_gpu =
        VideoCore::IsManagedMobileGpuOptimization();
    if (!managed_mobile_gpu &&
        ExecutorBloodborneEmptyPhysicalDrsCleanupFastNoop() &&
        !executor_physical_drs_has_residency.load(
            std::memory_order_acquire)) {
        // With no scratch surface ever published (or after GC retired the
        // last one), the legacy force-GC + cleanup scheduler is a strict
        // no-op. Avoid three recursive-mutex transactions on every draw.
        return;
    }
    std::scoped_lock drs_lock{executor_physical_drs_mutex};
    executor_physical_drs_draw_active = false;
    if (!managed_mobile_gpu) {
        if (executor_physical_drs_active) {
            ExecutorResolvePhysicalDrs("option_off");
        }
        (void)ExecutorRunPhysicalDrsResidencyGc(0, true);
        ExecutorSchedulePhysicalDrsOffCleanup();
        return;
    }
    if (!executor_physical_drs_active) {
        return;
    }

    bool keep_scaled_target = ExecutorPhysicalDrsPipelineCompatible(pipeline);
    if (keep_scaled_target) {
        const auto& key = pipeline->GetGraphicsKey();
        u32 current_color_mask = 0;
        for (u32 index = 0; index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
            if ((key.mrt_mask & (1u << index)) == 0 ||
                liverpool->regs.color_buffers[index].Address() == 0) {
                continue;
            }
            current_color_mask |= 1u << index;
            if ((executor_physical_drs_color_mask & (1u << index)) == 0 ||
                liverpool->regs.color_buffers[index].Address() !=
                    executor_physical_drs_color_guest_addresses[index]) {
                keep_scaled_target = false;
                break;
            }
        }
        keep_scaled_target &=
            current_color_mask == executor_physical_drs_color_mask;
    }
    if (keep_scaled_target) {
        const bool has_depth = liverpool->regs.depth_buffer.DepthValid() ||
                               liverpool->regs.depth_buffer.StencilValid();
        keep_scaled_target =
            has_depth == static_cast<bool>(executor_physical_drs_depth_image_id) &&
            (!has_depth ||
             liverpool->regs.depth_buffer.DepthAddress() ==
                 executor_physical_drs_depth_guest_address);
    }
    if (!keep_scaled_target) {
        ExecutorResolvePhysicalDrs("draw_hazard");
    }
}

void Rasterizer::ExecutorTransitionPhysicalDrsSurface(
    const vk::ImageLayout layout, const vk::AccessFlags2 access,
    const vk::PipelineStageFlags2 stage) {
    auto* surface = executor_physical_drs_surface;
    if (surface == nullptr) {
        return;
    }
    ExecutorTouchPhysicalDrsSurface(*surface);
    if (surface->layout == layout && surface->access == access &&
        surface->stage == stage) {
        return;
    }
    scheduler.EndRendering(Scheduler::EndRenderingReason::RasterizerBarrier);
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = surface->layout == vk::ImageLayout::eUndefined
                            ? vk::PipelineStageFlagBits2::eTopOfPipe
                            : surface->stage,
        .srcAccessMask = surface->layout == vk::ImageLayout::eUndefined
                             ? vk::AccessFlags2{}
                             : surface->access,
        .dstStageMask = stage,
        .dstAccessMask = access,
        .oldLayout = surface->layout,
        .newLayout = layout,
        .image = surface->image.image,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    scheduler.CommandBuffer().pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &barrier});
    surface->layout = layout;
    surface->access = access;
    surface->stage = stage;
}

void Rasterizer::ExecutorTransitionPhysicalDrsDepthSurface(
    const vk::ImageLayout layout, const vk::AccessFlags2 access,
    const vk::PipelineStageFlags2 stage) {
    auto* surface = executor_physical_drs_depth_surface;
    if (surface == nullptr) {
        return;
    }
    ExecutorTouchPhysicalDrsDepthSurface(*surface);
    if (surface->layout == layout && surface->access == access &&
        surface->stage == stage) {
        return;
    }
    scheduler.EndRendering(Scheduler::EndRenderingReason::RasterizerBarrier);
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = surface->layout == vk::ImageLayout::eUndefined
                            ? vk::PipelineStageFlagBits2::eTopOfPipe
                            : surface->stage,
        .srcAccessMask = surface->layout == vk::ImageLayout::eUndefined
                             ? vk::AccessFlags2{}
                             : surface->access,
        .dstStageMask = stage,
        .dstAccessMask = access,
        .oldLayout = surface->layout,
        .newLayout = layout,
        .image = surface->image.image,
        .subresourceRange{
            .aspectMask = surface->aspects,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    scheduler.CommandBuffer().pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &barrier});
    surface->layout = layout;
    surface->access = access;
    surface->stage = stage;
}

bool Rasterizer::ExecutorBeginPhysicalDrs(const GraphicsPipeline* pipeline,
                                          RenderState& state) {
    std::scoped_lock drs_lock{executor_physical_drs_mutex};
    executor_physical_drs_draw_active = false;
    const bool requested = VideoCore::MobileGpuWantsPhysicalDrs();
    u32 color_mask = 0;
    bool valid_color_descriptors = true;
    for (u32 index = 0; index < state.num_color_attachments; ++index) {
        if (!state.color_attachments[index].image_view) {
            continue;
        }
        color_mask |= 1u << index;
        valid_color_descriptors &= static_cast<bool>(cb_descs[index].first);
    }

    const bool compatible =
        requested && ExecutorPhysicalDrsPipelineCompatible(pipeline) &&
        valid_color_descriptors && (color_mask & 1u) != 0 &&
        state.num_layers == 1;
    if (!compatible) {
        if (executor_physical_drs_active) {
            ExecutorResolvePhysicalDrs("incompatible_draw");
        }
        VideoCore::RecordMobileGpuPhysicalDrsDecision(false, requested, 0, 0);
        return false;
    }

    auto& canonical = texture_cache.GetImage(cb_descs[0].first);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Attachment 0 is the one almost every pass binds; the loop below starts at 1.
    if (ExecutorAndroidRendererProbesEnabled()) {
        g_attach_class.note(canonical.info.size.width, canonical.info.size.height,
                            static_cast<std::uint32_t>(canonical.info.pixel_format),
                            std::uint32_t(canonical.usage.texture) |
                            (std::uint32_t(canonical.usage.storage) << 1) |
                            (std::uint32_t(canonical.usage.render_target) << 2) |
                            (std::uint32_t(canonical.usage.depth_target) << 3) |
                            (std::uint32_t(canonical.usage.vo_surface) << 4));
    }
#endif
    const auto& info = canonical.info;
    if (info.size.depth != 1 || info.resources.levels != 1 ||
        info.resources.layers != 1 || info.num_samples != 1 ||
        canonical.backing == nullptr || canonical.backing->num_samples != 1 ||
        info.props.is_depth || info.props.is_volume ||
        info.size.width != state.width || info.size.height != state.height) {
        if (executor_physical_drs_active) {
            ExecutorResolvePhysicalDrs("unsupported_target");
        }
        VideoCore::RecordMobileGpuPhysicalDrsDecision(false, true, 0, 0);
        return false;
    }
    std::array<VideoCore::Image*, AmdGpu::NUM_COLOR_BUFFERS>
        canonical_colors{};
    canonical_colors[0] = &canonical;
    for (u32 index = 1; index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
        if ((color_mask & (1u << index)) == 0) {
            continue;
        }
        auto& image = texture_cache.GetImage(cb_descs[index].first);
        const auto& color_info = image.info;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        // usage.texture is the decisive bit: a surface that is sampled later
        // cannot be physically scaled without breaking its UV mapping.
        if (ExecutorAndroidRendererProbesEnabled()) {
            g_attach_class.note(color_info.size.width, color_info.size.height,
                                static_cast<std::uint32_t>(color_info.pixel_format),
                                std::uint32_t(image.usage.texture) |
                                (std::uint32_t(image.usage.storage) << 1) |
                                (std::uint32_t(image.usage.render_target) << 2) |
                                (std::uint32_t(image.usage.depth_target) << 3) |
                                (std::uint32_t(image.usage.vo_surface) << 4));
        }
#endif
        if (color_info.size.depth != 1 ||
            color_info.resources.levels != 1 ||
            color_info.resources.layers != 1 ||
            color_info.num_samples != 1 || image.backing == nullptr ||
            image.backing->num_samples != 1 ||
            color_info.props.is_depth || color_info.props.is_volume ||
            color_info.size.width != state.width ||
            color_info.size.height != state.height) {
            if (executor_physical_drs_active) {
                ExecutorResolvePhysicalDrs("unsupported_mrt_target");
            }
            VideoCore::RecordMobileGpuPhysicalDrsDecision(
                false, true, 0, 0);
            return false;
        }
        canonical_colors[index] = &image;
    }

    const bool has_depth =
        state.depth_stencil_attachment.has_depth ||
        state.depth_stencil_attachment.has_stencil;
    VideoCore::Image* canonical_depth = nullptr;
    vk::ImageAspectFlags depth_aspects{};
    if (has_depth) {
        if (!db_desc.first) {
            if (executor_physical_drs_active) {
                ExecutorResolvePhysicalDrs("missing_depth_target");
            }
            VideoCore::RecordMobileGpuPhysicalDrsDecision(false, true, 0, 0);
            return false;
        }
        canonical_depth = &texture_cache.GetImage(db_desc.first);
        const auto& depth_info = canonical_depth->info;
        if (!depth_info.props.is_depth || depth_info.props.is_volume ||
            depth_info.size.depth != 1 || depth_info.resources.levels != 1 ||
            depth_info.resources.layers != 1 || depth_info.num_samples != 1 ||
            canonical_depth->backing == nullptr ||
            canonical_depth->backing->num_samples != 1 ||
            depth_info.size.width != state.width ||
            depth_info.size.height != state.height) {
            if (executor_physical_drs_active) {
                ExecutorResolvePhysicalDrs("unsupported_depth_target");
            }
            VideoCore::RecordMobileGpuPhysicalDrsDecision(false, true, 0, 0);
            return false;
        }
        if (state.depth_stencil_attachment.has_depth) {
            depth_aspects |= vk::ImageAspectFlagBits::eDepth;
        }
        if (state.depth_stencil_attachment.has_stencil) {
            depth_aspects |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    // The floor used to be 50%, which collapsed every mode below 450p into one
    // and threw away the headroom that matters under thermal throttling.
    const u32 scale = std::clamp(VideoCore::MobileGpuEffectiveScalePercent(),
                                 25u, 100u);
    u32 physical_width =
        std::max(1u, (static_cast<u32>(state.width) * scale + 99u) / 100u);
    u32 physical_height =
        std::max(1u, (static_cast<u32>(state.height) * scale + 99u) / 100u);
    if (VideoCore::IsManagedMobileFsrEnabled() && scale >= 60u) {
        // The mobile FSR quality floor is generic rather than title-specific.
        // It guarantees that the 720p performance preset reconstructs from at
        // least 960x544, while never enlarging a smaller guest attachment.
        physical_width = std::max(
            physical_width, std::min(static_cast<u32>(state.width), 960u));
        physical_height = std::max(
            physical_height, std::min(static_cast<u32>(state.height), 544u));
    }
    const vk::Extent2D physical_extent{
        .width = physical_width,
        .height = physical_height,
    };
    const vk::FormatFeatureFlags2 required_features =
        vk::FormatFeatureFlagBits2::eColorAttachment |
        vk::FormatFeatureFlagBits2::eBlitSrc |
        vk::FormatFeatureFlagBits2::eBlitDst |
        vk::FormatFeatureFlagBits2::eSampledImage;
    std::array<vk::Format, AmdGpu::NUM_COLOR_BUFFERS> physical_formats{};
    const vk::Format physical_format =
        instance.GetSupportedFormat(cb_descs[0].second.view_info.format,
                                    required_features);
    physical_formats[0] = physical_format;
    if (physical_format == vk::Format::eUndefined) {
        if (executor_physical_drs_active) {
            ExecutorResolvePhysicalDrs("undefined_format");
        }
        VideoCore::RecordMobileGpuPhysicalDrsDecision(false, true, 0, 0);
        return false;
    }
    const auto format_properties =
        instance.GetPhysicalDevice().getFormatProperties(physical_format);
    const vk::FormatFeatureFlags required_legacy =
        vk::FormatFeatureFlagBits::eColorAttachment |
        vk::FormatFeatureFlagBits::eBlitSrc |
        vk::FormatFeatureFlagBits::eBlitDst |
        vk::FormatFeatureFlagBits::eSampledImage;
    if ((format_properties.optimalTilingFeatures & required_legacy) !=
        required_legacy) {
        if (executor_physical_drs_active) {
            ExecutorResolvePhysicalDrs("unsupported_format");
        }
        VideoCore::RecordMobileGpuPhysicalDrsDecision(false, true, 0, 0);
        return false;
    }
    for (u32 index = 1; index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
        if ((color_mask & (1u << index)) == 0) {
            continue;
        }
        const vk::Format format =
            instance.GetSupportedFormat(
                cb_descs[index].second.view_info.format, required_features);
        if (format == vk::Format::eUndefined) {
            if (executor_physical_drs_active) {
                ExecutorResolvePhysicalDrs("undefined_mrt_format");
            }
            VideoCore::RecordMobileGpuPhysicalDrsDecision(
                false, true, 0, 0);
            return false;
        }
        const auto properties =
            instance.GetPhysicalDevice().getFormatProperties(format);
        if ((properties.optimalTilingFeatures & required_legacy) !=
            required_legacy) {
            if (executor_physical_drs_active) {
                ExecutorResolvePhysicalDrs("unsupported_mrt_format");
            }
            VideoCore::RecordMobileGpuPhysicalDrsDecision(
                false, true, 0, 0);
            return false;
        }
        physical_formats[index] = format;
    }

    vk::Format depth_format = vk::Format::eUndefined;
    if (has_depth) {
        depth_format = db_desc.second.view_info.format;
        const auto depth_properties =
            instance.GetPhysicalDevice().getFormatProperties(depth_format);
        const vk::FormatFeatureFlags depth_required =
            vk::FormatFeatureFlagBits::eDepthStencilAttachment |
            vk::FormatFeatureFlagBits::eBlitSrc |
            vk::FormatFeatureFlagBits::eBlitDst;
        if (depth_format == vk::Format::eUndefined ||
            (depth_properties.optimalTilingFeatures & depth_required) !=
                depth_required) {
            if (executor_physical_drs_active) {
                ExecutorResolvePhysicalDrs("unsupported_depth_format");
            }
            VideoCore::RecordMobileGpuPhysicalDrsDecision(false, true, 0, 0);
            return false;
        }
    }

    if (executor_physical_drs_present_image_id == cb_descs[0].first) {
        executor_physical_drs_present_surface = nullptr;
        executor_physical_drs_present_image_id = {};
        executor_physical_drs_present_image_uid = 0;
    }

    bool continuing =
        executor_physical_drs_active &&
        executor_physical_drs_color_mask == color_mask &&
        executor_physical_drs_image_id == cb_descs[0].first &&
        executor_physical_drs_image_uid == canonical.image_uid &&
        executor_physical_drs_depth_image_id ==
            (has_depth ? db_desc.first : VideoCore::ImageId{}) &&
        executor_physical_drs_depth_image_uid ==
            (has_depth ? canonical_depth->image_uid : 0) &&
        executor_physical_drs_scale_percent == scale &&
        executor_physical_drs_surface != nullptr &&
        executor_physical_drs_surface->format == physical_format &&
        executor_physical_drs_surface->extent == physical_extent &&
        (!has_depth ||
         (executor_physical_drs_depth_surface != nullptr &&
          executor_physical_drs_depth_surface->format == depth_format &&
          executor_physical_drs_depth_surface->extent == physical_extent &&
          executor_physical_drs_depth_surface->aspects == depth_aspects));
    for (u32 index = 0;
         continuing && index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
        if ((color_mask & (1u << index)) == 0) {
            continue;
        }
        const auto* surface =
            executor_physical_drs_color_surfaces[index];
        continuing =
            surface != nullptr &&
            executor_physical_drs_color_image_ids[index] ==
                cb_descs[index].first &&
            executor_physical_drs_color_image_uids[index] ==
                canonical_colors[index]->image_uid &&
            surface->format == physical_formats[index] &&
            surface->extent == physical_extent;
    }
    if (executor_physical_drs_active && !continuing) {
        ExecutorResolvePhysicalDrs("target_change");
    }

    if (!continuing) {
        executor_physical_drs_building = true;
        auto surface_it = std::ranges::find_if(
            executor_physical_drs_surfaces,
            [&](const ExecutorPhysicalDrsSurface& surface) {
                return surface.format == physical_format &&
                       surface.extent == physical_extent &&
                       surface.owner_image_id == cb_descs[0].first &&
                       surface.owner_image_uid == canonical.image_uid;
            });
        if (surface_it == executor_physical_drs_surfaces.end()) {
            ExecutorPhysicalDrsSurface surface{};
            surface.image =
                VideoCore::UniqueImage{instance.GetDevice(), instance.GetAllocator()};
            surface.format = physical_format;
            surface.extent = physical_extent;
            surface.owner_image_id = cb_descs[0].first;
            surface.owner_image_uid = canonical.image_uid;
            const vk::ImageCreateInfo image_ci{
                .imageType = vk::ImageType::e2D,
                .format = physical_format,
                .extent = {physical_extent.width, physical_extent.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eColorAttachment |
                         vk::ImageUsageFlagBits::eTransferSrc |
                         vk::ImageUsageFlagBits::eTransferDst |
                         vk::ImageUsageFlagBits::eSampled,
                .initialLayout = vk::ImageLayout::eUndefined,
            };
            if (!ExecutorTryCreatePhysicalDrsImage(
                    surface.image, image_ci, surface.allocation_bytes)) {
                ExecutorResetPhysicalDrsTransientBindings();
                VideoCore::RecordMobileGpuPhysicalDrsDecision(
                    false, true, 0, 0);
                return false;
            }
            auto [view_result, view] =
                instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
                    .image = surface.image.image,
                    .viewType = vk::ImageViewType::e2D,
                    .format = physical_format,
                    .subresourceRange{
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                });
            if (view_result != vk::Result::eSuccess) {
                ExecutorResetPhysicalDrsTransientBindings();
                VideoCore::RecordMobileGpuPhysicalDrsDecision(false, true, 0, 0);
                return false;
            }
            surface.view = std::move(view);
            executor_physical_drs_surfaces.emplace_back(std::move(surface));
            executor_physical_drs_has_residency.store(
                true, std::memory_order_release);
            surface_it = std::prev(executor_physical_drs_surfaces.end());
            executor_physical_drs_resident_bytes +=
                surface_it->allocation_bytes;
            executor_physical_drs_peak_bytes =
                std::max(executor_physical_drs_peak_bytes,
                         executor_physical_drs_resident_bytes);
        }
        executor_physical_drs_surface = &*surface_it;
        executor_physical_drs_color_surfaces.fill(nullptr);
        executor_physical_drs_color_surfaces[0] =
            executor_physical_drs_surface;

        for (u32 index = 1; index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
            if ((color_mask & (1u << index)) == 0) {
                continue;
            }
            auto color_surface_it = std::ranges::find_if(
                executor_physical_drs_surfaces,
                [&](const ExecutorPhysicalDrsSurface& surface) {
                    return surface.format == physical_formats[index] &&
                           surface.extent == physical_extent &&
                           surface.owner_image_id == cb_descs[index].first &&
                           surface.owner_image_uid ==
                               canonical_colors[index]->image_uid;
                });
            if (color_surface_it == executor_physical_drs_surfaces.end()) {
                ExecutorPhysicalDrsSurface surface{};
                surface.image = VideoCore::UniqueImage{
                    instance.GetDevice(), instance.GetAllocator()};
                surface.format = physical_formats[index];
                surface.extent = physical_extent;
                surface.owner_image_id = cb_descs[index].first;
                surface.owner_image_uid =
                    canonical_colors[index]->image_uid;
                const vk::ImageCreateInfo image_ci{
                    .imageType = vk::ImageType::e2D,
                    .format = physical_formats[index],
                    .extent = {physical_extent.width,
                               physical_extent.height, 1},
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = vk::SampleCountFlagBits::e1,
                    .tiling = vk::ImageTiling::eOptimal,
                    .usage = vk::ImageUsageFlagBits::eColorAttachment |
                             vk::ImageUsageFlagBits::eTransferSrc |
                             vk::ImageUsageFlagBits::eTransferDst |
                             vk::ImageUsageFlagBits::eSampled,
                    .initialLayout = vk::ImageLayout::eUndefined,
                };
                if (!ExecutorTryCreatePhysicalDrsImage(
                        surface.image, image_ci,
                        surface.allocation_bytes)) {
                    ExecutorResetPhysicalDrsTransientBindings();
                    VideoCore::RecordMobileGpuPhysicalDrsDecision(
                        false, true, 0, 0);
                    return false;
                }
                auto [view_result, view] =
                    instance.GetDevice().createImageViewUnique(
                        vk::ImageViewCreateInfo{
                            .image = surface.image.image,
                            .viewType = vk::ImageViewType::e2D,
                            .format = physical_formats[index],
                            .subresourceRange{
                                .aspectMask =
                                    vk::ImageAspectFlagBits::eColor,
                                .baseMipLevel = 0,
                                .levelCount = 1,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                            },
                        });
                if (view_result != vk::Result::eSuccess) {
                    ExecutorResetPhysicalDrsTransientBindings();
                    VideoCore::RecordMobileGpuPhysicalDrsDecision(
                        false, true, 0, 0);
                    return false;
                }
                surface.view = std::move(view);
                executor_physical_drs_surfaces.emplace_back(
                    std::move(surface));
                executor_physical_drs_has_residency.store(
                    true, std::memory_order_release);
                color_surface_it =
                    std::prev(executor_physical_drs_surfaces.end());
                executor_physical_drs_resident_bytes +=
                    color_surface_it->allocation_bytes;
                executor_physical_drs_peak_bytes =
                    std::max(executor_physical_drs_peak_bytes,
                             executor_physical_drs_resident_bytes);
            }
            executor_physical_drs_color_surfaces[index] =
                &*color_surface_it;
        }

        if (has_depth) {
            auto depth_surface_it = std::ranges::find_if(
                executor_physical_drs_depth_surfaces,
                [&](const ExecutorPhysicalDrsDepthSurface& surface) {
                    return surface.format == depth_format &&
                           surface.extent == physical_extent &&
                           surface.aspects == depth_aspects &&
                           surface.owner_image_id == db_desc.first &&
                           surface.owner_image_uid == canonical_depth->image_uid;
                });
            if (depth_surface_it == executor_physical_drs_depth_surfaces.end()) {
                ExecutorPhysicalDrsDepthSurface surface{};
                surface.image = VideoCore::UniqueImage{
                    instance.GetDevice(), instance.GetAllocator()};
                surface.format = depth_format;
                surface.extent = physical_extent;
                surface.aspects = depth_aspects;
                surface.owner_image_id = db_desc.first;
                surface.owner_image_uid = canonical_depth->image_uid;
                const vk::ImageCreateInfo image_ci{
                    .imageType = vk::ImageType::e2D,
                    .format = depth_format,
                    .extent = {physical_extent.width, physical_extent.height, 1},
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = vk::SampleCountFlagBits::e1,
                    .tiling = vk::ImageTiling::eOptimal,
                    .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                             vk::ImageUsageFlagBits::eTransferSrc |
                             vk::ImageUsageFlagBits::eTransferDst,
                    .initialLayout = vk::ImageLayout::eUndefined,
                };
                if (!ExecutorTryCreatePhysicalDrsImage(
                        surface.image, image_ci,
                        surface.allocation_bytes)) {
                    ExecutorResetPhysicalDrsTransientBindings();
                    VideoCore::RecordMobileGpuPhysicalDrsDecision(
                        false, true, 0, 0);
                    return false;
                }
                auto [view_result, view] =
                    instance.GetDevice().createImageViewUnique(
                        vk::ImageViewCreateInfo{
                            .image = surface.image.image,
                            .viewType = vk::ImageViewType::e2D,
                            .format = depth_format,
                            .subresourceRange{
                                .aspectMask = depth_aspects,
                                .baseMipLevel = 0,
                                .levelCount = 1,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                            },
                        });
                if (view_result != vk::Result::eSuccess) {
                    ExecutorResetPhysicalDrsTransientBindings();
                    VideoCore::RecordMobileGpuPhysicalDrsDecision(
                        false, true, 0, 0);
                    return false;
                }
                surface.view = std::move(view);
                executor_physical_drs_depth_surfaces.emplace_back(
                    std::move(surface));
                executor_physical_drs_has_residency.store(
                    true, std::memory_order_release);
                depth_surface_it =
                    std::prev(executor_physical_drs_depth_surfaces.end());
                executor_physical_drs_resident_bytes +=
                    depth_surface_it->allocation_bytes;
                executor_physical_drs_peak_bytes =
                    std::max(executor_physical_drs_peak_bytes,
                             executor_physical_drs_resident_bytes);
            }
            executor_physical_drs_depth_surface = &*depth_surface_it;
        } else {
            executor_physical_drs_depth_surface = nullptr;
        }

        scheduler.EndRendering(Scheduler::EndRenderingReason::RasterizerCopy);
        canonical.Transit(vk::ImageLayout::eTransferSrcOptimal,
                          vk::AccessFlagBits2::eTransferRead, {});
        ExecutorTransitionPhysicalDrsSurface(
            vk::ImageLayout::eTransferDstOptimal,
            vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eTransfer);
        const vk::ImageBlit region{
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .srcOffsets = std::array{
                vk::Offset3D{0, 0, 0},
                vk::Offset3D{static_cast<s32>(state.width),
                             static_cast<s32>(state.height), 1},
            },
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .dstOffsets = std::array{
                vk::Offset3D{0, 0, 0},
                vk::Offset3D{static_cast<s32>(physical_extent.width),
                             static_cast<s32>(physical_extent.height), 1},
            },
        };
        scheduler.CommandBuffer().blitImage(
            canonical.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
            executor_physical_drs_surface->image.image,
            vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        ExecutorTransitionPhysicalDrsSurface(
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits2::eColorAttachmentRead |
                vk::AccessFlagBits2::eColorAttachmentWrite,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput);

        for (u32 index = 1; index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
            if ((color_mask & (1u << index)) == 0) {
                continue;
            }
            auto& mrt = *canonical_colors[index];
            executor_physical_drs_surface =
                executor_physical_drs_color_surfaces[index];
            mrt.Transit(vk::ImageLayout::eTransferSrcOptimal,
                        vk::AccessFlagBits2::eTransferRead, {});
            ExecutorTransitionPhysicalDrsSurface(
                vk::ImageLayout::eTransferDstOptimal,
                vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eTransfer);
            const vk::ImageBlit mrt_region{
                .srcSubresource{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .srcOffsets = std::array{
                    vk::Offset3D{0, 0, 0},
                    vk::Offset3D{static_cast<s32>(state.width),
                                 static_cast<s32>(state.height), 1},
                },
                .dstSubresource{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .dstOffsets = std::array{
                    vk::Offset3D{0, 0, 0},
                    vk::Offset3D{
                        static_cast<s32>(physical_extent.width),
                        static_cast<s32>(physical_extent.height), 1},
                },
            };
            scheduler.CommandBuffer().blitImage(
                mrt.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                executor_physical_drs_surface->image.image,
                vk::ImageLayout::eTransferDstOptimal, mrt_region,
                vk::Filter::eNearest);
            ExecutorTransitionPhysicalDrsSurface(
                vk::ImageLayout::eColorAttachmentOptimal,
                vk::AccessFlagBits2::eColorAttachmentRead |
                    vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput);
        }
        executor_physical_drs_surface =
            executor_physical_drs_color_surfaces[0];

        if (has_depth) {
            canonical_depth->Transit(vk::ImageLayout::eTransferSrcOptimal,
                                     vk::AccessFlagBits2::eTransferRead, {});
            ExecutorTransitionPhysicalDrsDepthSurface(
                vk::ImageLayout::eTransferDstOptimal,
                vk::AccessFlagBits2::eTransferWrite,
                vk::PipelineStageFlagBits2::eTransfer);
            const auto blit_depth_aspect =
                [&](const vk::ImageAspectFlagBits aspect) {
                    const vk::ImageBlit depth_region{
                        .srcSubresource{
                            .aspectMask = aspect,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                        },
                        .srcOffsets = std::array{
                            vk::Offset3D{0, 0, 0},
                            vk::Offset3D{static_cast<s32>(state.width),
                                         static_cast<s32>(state.height), 1},
                        },
                        .dstSubresource{
                            .aspectMask = aspect,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                        },
                        .dstOffsets = std::array{
                            vk::Offset3D{0, 0, 0},
                            vk::Offset3D{
                                static_cast<s32>(physical_extent.width),
                                static_cast<s32>(physical_extent.height), 1},
                        },
                    };
                    scheduler.CommandBuffer().blitImage(
                        canonical_depth->GetImage(),
                        vk::ImageLayout::eTransferSrcOptimal,
                        executor_physical_drs_depth_surface->image.image,
                        vk::ImageLayout::eTransferDstOptimal, depth_region,
                        vk::Filter::eNearest);
                };
            if (depth_aspects & vk::ImageAspectFlagBits::eDepth) {
                blit_depth_aspect(vk::ImageAspectFlagBits::eDepth);
            }
            if (depth_aspects & vk::ImageAspectFlagBits::eStencil) {
                blit_depth_aspect(vk::ImageAspectFlagBits::eStencil);
            }
            ExecutorTransitionPhysicalDrsDepthSurface(
                vk::ImageLayout::eDepthStencilAttachmentOptimal,
                vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                    vk::PipelineStageFlagBits2::eLateFragmentTests);
        }

        executor_physical_drs_image_id = cb_descs[0].first;
        executor_physical_drs_image_uid = canonical.image_uid;
        executor_physical_drs_depth_image_id =
            has_depth ? db_desc.first : VideoCore::ImageId{};
        executor_physical_drs_depth_image_uid =
            has_depth ? canonical_depth->image_uid : 0;
        executor_physical_drs_guest_address = info.guest_address;
        executor_physical_drs_depth_guest_address =
            has_depth ? canonical_depth->info.guest_address : 0;
        executor_physical_drs_color_image_ids.fill({});
        executor_physical_drs_color_image_uids.fill(0);
        executor_physical_drs_color_guest_addresses.fill(0);
        executor_physical_drs_color_mask = color_mask;
        for (u32 index = 0; index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
            if ((color_mask & (1u << index)) == 0) {
                continue;
            }
            executor_physical_drs_color_image_ids[index] =
                cb_descs[index].first;
            executor_physical_drs_color_image_uids[index] =
                canonical_colors[index]->image_uid;
            executor_physical_drs_color_guest_addresses[index] =
                canonical_colors[index]->info.guest_address;
        }
        executor_physical_drs_scale_percent = scale;
        executor_physical_drs_active = true;
        executor_physical_drs_building = false;
    }

    for (u32 index = 0; index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
        if ((color_mask & (1u << index)) == 0) {
            continue;
        }
        ExecutorTouchPhysicalDrsSurface(
            *executor_physical_drs_color_surfaces[index]);
        state.color_attachments[index].image_view =
            *executor_physical_drs_color_surfaces[index]->view;
        state.color_attachments[index].image_layout =
            vk::ImageLayout::eColorAttachmentOptimal;
    }
    if (has_depth) {
        ExecutorTouchPhysicalDrsDepthSurface(
            *executor_physical_drs_depth_surface);
        state.depth_stencil_attachment.image_view =
            *executor_physical_drs_depth_surface->view;
        state.depth_stencil_attachment.image_layout =
            vk::ImageLayout::eDepthStencilAttachmentOptimal;
    }
    state.width = static_cast<u16>(physical_extent.width);
    state.height = static_cast<u16>(physical_extent.height);
    executor_physical_drs_draw_active = true;

    const u64 canonical_pixels =
        static_cast<u64>(info.size.width) * info.size.height;
    const u64 physical_pixels =
        static_cast<u64>(physical_extent.width) * physical_extent.height;
    VideoCore::RecordMobileGpuPhysicalDrsDecision(
        true, false, canonical_pixels, physical_pixels);
    return true;
}

bool Rasterizer::ExecutorResolvePhysicalDrs(const char* reason) {
    if (!VideoCore::IsManagedMobileGpuOptimization() &&
        ExecutorBloodborneEmptyPhysicalDrsCleanupFastNoop() &&
        !executor_physical_drs_has_residency.load(
            std::memory_order_acquire)) {
        return false;
    }
    std::scoped_lock drs_lock{executor_physical_drs_mutex};
    executor_physical_drs_draw_active = false;
    if (!executor_physical_drs_active ||
        executor_physical_drs_surface == nullptr ||
        !executor_physical_drs_image_id) {
        if (!VideoCore::IsManagedMobileGpuOptimization()) {
            (void)ExecutorRunPhysicalDrsResidencyGc(0, true);
            ExecutorSchedulePhysicalDrsOffCleanup();
        }
        return false;
    }

    scheduler.EndRendering(Scheduler::EndRenderingReason::RasterizerCopy);
    auto& canonical = texture_cache.GetImage(executor_physical_drs_image_id);
    if (canonical.image_uid == executor_physical_drs_image_uid) {
        ExecutorTransitionPhysicalDrsSurface(
            vk::ImageLayout::eTransferSrcOptimal,
            vk::AccessFlagBits2::eTransferRead,
            vk::PipelineStageFlagBits2::eTransfer);
        canonical.Transit(vk::ImageLayout::eTransferDstOptimal,
                          vk::AccessFlagBits2::eTransferWrite, {});
        const vk::ImageBlit region{
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .srcOffsets = std::array{
                vk::Offset3D{0, 0, 0},
                vk::Offset3D{
                    static_cast<s32>(executor_physical_drs_surface->extent.width),
                    static_cast<s32>(executor_physical_drs_surface->extent.height), 1},
            },
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .dstOffsets = std::array{
                vk::Offset3D{0, 0, 0},
                vk::Offset3D{static_cast<s32>(canonical.info.size.width),
                             static_cast<s32>(canonical.info.size.height), 1},
            },
        };
        scheduler.CommandBuffer().blitImage(
            executor_physical_drs_surface->image.image,
            vk::ImageLayout::eTransferSrcOptimal, canonical.GetImage(),
            vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        canonical.flags |= VideoCore::ImageFlagBits::GpuModified;
        executor_physical_drs_present_surface =
            executor_physical_drs_surface;
        executor_physical_drs_present_image_id =
            executor_physical_drs_image_id;
        executor_physical_drs_present_image_uid =
            executor_physical_drs_image_uid;
        VideoCore::RecordMobileGpuPhysicalDrsResolve();
    }

    for (u32 index = 1; index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
        if ((executor_physical_drs_color_mask & (1u << index)) == 0 ||
            executor_physical_drs_color_surfaces[index] == nullptr ||
            !executor_physical_drs_color_image_ids[index]) {
            continue;
        }
        auto& mrt =
            texture_cache.GetImage(
                executor_physical_drs_color_image_ids[index]);
        if (mrt.image_uid !=
            executor_physical_drs_color_image_uids[index]) {
            continue;
        }
        executor_physical_drs_surface =
            executor_physical_drs_color_surfaces[index];
        ExecutorTransitionPhysicalDrsSurface(
            vk::ImageLayout::eTransferSrcOptimal,
            vk::AccessFlagBits2::eTransferRead,
            vk::PipelineStageFlagBits2::eTransfer);
        mrt.Transit(vk::ImageLayout::eTransferDstOptimal,
                    vk::AccessFlagBits2::eTransferWrite, {});
        const vk::ImageBlit region{
            .srcSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .srcOffsets = std::array{
                vk::Offset3D{0, 0, 0},
                vk::Offset3D{
                    static_cast<s32>(
                        executor_physical_drs_surface->extent.width),
                    static_cast<s32>(
                        executor_physical_drs_surface->extent.height),
                    1},
            },
            .dstSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .dstOffsets = std::array{
                vk::Offset3D{0, 0, 0},
                vk::Offset3D{
                    static_cast<s32>(mrt.info.size.width),
                    static_cast<s32>(mrt.info.size.height), 1},
            },
        };
        scheduler.CommandBuffer().blitImage(
            executor_physical_drs_surface->image.image,
            vk::ImageLayout::eTransferSrcOptimal, mrt.GetImage(),
            vk::ImageLayout::eTransferDstOptimal, region,
            vk::Filter::eNearest);
        mrt.flags |= VideoCore::ImageFlagBits::GpuModified;
    }

    if (executor_physical_drs_depth_surface != nullptr &&
        executor_physical_drs_depth_image_id) {
        auto& canonical_depth =
            texture_cache.GetImage(executor_physical_drs_depth_image_id);
        if (canonical_depth.image_uid ==
            executor_physical_drs_depth_image_uid) {
            ExecutorTransitionPhysicalDrsDepthSurface(
                vk::ImageLayout::eTransferSrcOptimal,
                vk::AccessFlagBits2::eTransferRead,
                vk::PipelineStageFlagBits2::eTransfer);
            canonical_depth.Transit(vk::ImageLayout::eTransferDstOptimal,
                                    vk::AccessFlagBits2::eTransferWrite, {});
            const auto blit_depth_aspect =
                [&](const vk::ImageAspectFlagBits aspect) {
                    const vk::ImageBlit region{
                        .srcSubresource{
                            .aspectMask = aspect,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                        },
                        .srcOffsets = std::array{
                            vk::Offset3D{0, 0, 0},
                            vk::Offset3D{
                                static_cast<s32>(
                                    executor_physical_drs_depth_surface
                                        ->extent.width),
                                static_cast<s32>(
                                    executor_physical_drs_depth_surface
                                        ->extent.height),
                                1},
                        },
                        .dstSubresource{
                            .aspectMask = aspect,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                        },
                        .dstOffsets = std::array{
                            vk::Offset3D{0, 0, 0},
                            vk::Offset3D{
                                static_cast<s32>(
                                    canonical_depth.info.size.width),
                                static_cast<s32>(
                                    canonical_depth.info.size.height),
                                1},
                        },
                    };
                    scheduler.CommandBuffer().blitImage(
                        executor_physical_drs_depth_surface->image.image,
                        vk::ImageLayout::eTransferSrcOptimal,
                        canonical_depth.GetImage(),
                        vk::ImageLayout::eTransferDstOptimal, region,
                        vk::Filter::eNearest);
                };
            if (executor_physical_drs_depth_surface->aspects &
                vk::ImageAspectFlagBits::eDepth) {
                blit_depth_aspect(vk::ImageAspectFlagBits::eDepth);
            }
            if (executor_physical_drs_depth_surface->aspects &
                vk::ImageAspectFlagBits::eStencil) {
                blit_depth_aspect(vk::ImageAspectFlagBits::eStencil);
            }
            canonical_depth.flags |=
                VideoCore::ImageFlagBits::GpuModified;
        }
    }

    static std::atomic<u32> resolve_logs{0};
    const u32 log_index = resolve_logs.fetch_add(1, std::memory_order_relaxed);
    if (log_index < 16) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_PHYSICAL_DRS] phase=resolve reason=%s scale=%u "
            "guest=0x%llx",
            reason != nullptr ? reason : "unknown",
            executor_physical_drs_scale_percent,
            static_cast<unsigned long long>(
                executor_physical_drs_guest_address));
    }
    ExecutorResetPhysicalDrsTransientBindings();
    if (!VideoCore::IsManagedMobileGpuOptimization()) {
        (void)ExecutorRunPhysicalDrsResidencyGc(0, true);
        ExecutorSchedulePhysicalDrsOffCleanup();
    }
    return true;
}

bool Rasterizer::ExecutorAcquirePhysicalDrsPresentationSource(
    const VideoCore::ImageId image_id, vk::ImageView& image_view,
    vk::Extent2D& extent) {
    std::scoped_lock drs_lock{executor_physical_drs_mutex};
    if (!VideoCore::IsManagedMobileGpuOptimization()) {
        (void)ExecutorRunPhysicalDrsResidencyGc(0, true);
        ExecutorSchedulePhysicalDrsOffCleanup();
        return false;
    }
    if (executor_physical_drs_present_surface == nullptr ||
        executor_physical_drs_present_image_id != image_id || !image_id) {
        return false;
    }
    const auto& canonical = texture_cache.GetImage(image_id);
    auto* surface = executor_physical_drs_present_surface;
    if (canonical.image_uid != executor_physical_drs_present_image_uid ||
        surface->owner_image_id != image_id ||
        surface->owner_image_uid != canonical.image_uid ||
        !surface->view || !surface->image) {
        executor_physical_drs_present_surface = nullptr;
        executor_physical_drs_present_image_id = {};
        executor_physical_drs_present_image_uid = 0;
        return false;
    }
    ExecutorTouchPhysicalDrsSurface(*surface);

    if (surface->layout != vk::ImageLayout::eShaderReadOnlyOptimal ||
        surface->access != vk::AccessFlagBits2::eShaderRead) {
        scheduler.EndRendering(
            Scheduler::EndRenderingReason::RasterizerBarrier);
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = surface->layout == vk::ImageLayout::eUndefined
                                ? vk::PipelineStageFlagBits2::eTopOfPipe
                                : surface->stage,
            .srcAccessMask =
                surface->layout == vk::ImageLayout::eUndefined
                    ? vk::AccessFlags2{}
                    : surface->access,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader |
                            vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = surface->layout,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = surface->image.image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        scheduler.CommandBuffer().pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &barrier});
        surface->layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        surface->access = vk::AccessFlagBits2::eShaderRead;
        surface->stage = vk::PipelineStageFlagBits2::eComputeShader |
                         vk::PipelineStageFlagBits2::eFragmentShader;
    }
    image_view = *surface->view;
    extent = surface->extent;
    return true;
}

void Rasterizer::ExecutorGpuLatePassTransition(u64 next_attachment_signature, u64 next_sequence,
                                               const char* reason) {
    if (!AmdGpu::RenderWaveTrace::LateFrameCaptureActive()) {
        executor_gpu_late_pass_probe = {};
        return;
    }

    const u64 epoch = AmdGpu::RenderWaveTrace::LateFrameCaptureEpoch();
    if (executor_gpu_late_pass_probe.epoch != epoch) {
        executor_gpu_late_pass_probe = {};
        executor_gpu_late_pass_probe.epoch = epoch;
    }
    if (executor_gpu_late_pass_probe.armed &&
        executor_gpu_late_pass_probe.attachment_signature != next_attachment_signature) {
        ExecutorGpuLatePassFlush(next_attachment_signature, next_sequence, reason);
    }
}

void Rasterizer::ExecutorGpuLatePassArm(u64 attachment_signature, u64 selected_sequence,
                                        u64 fragment_hash) {
    if (!AmdGpu::RenderWaveTrace::LateFrameCaptureActive() || attachment_signature == 0) {
        return;
    }

    const u64 epoch = AmdGpu::RenderWaveTrace::LateFrameCaptureEpoch();
    if (executor_gpu_late_pass_probe.epoch != epoch) {
        executor_gpu_late_pass_probe = {};
        executor_gpu_late_pass_probe.epoch = epoch;
    }
    if (executor_gpu_late_pass_probe.armed) {
        return;
    }

    auto& pending = executor_gpu_late_pass_probe;
    const auto capture_identity = [&](VideoCore::ImageId image_id, VAddr desc_guest_address,
                                      const VideoCore::SubresourceRange& view_range) {
        ExecutorGpuLatePassProbeState::ImageIdentity identity{};
        if (!image_id) {
            return identity;
        }
        const auto& image = texture_cache.GetImage(image_id);
        identity.image_id = image_id;
        identity.desc_guest_address = desc_guest_address;
        identity.image_guest_address = image.info.guest_address;
        identity.image_uid = image.image_uid;
        identity.backing_identity = reinterpret_cast<uintptr_t>(image.backing);
        identity.image_handle =
            reinterpret_cast<u64>(static_cast<VkImage>(image.GetImage()));
        identity.view_range = view_range;
        return identity;
    };
    pending.attachment_signature = attachment_signature;
    pending.selected_sequence = selected_sequence;
    pending.fragment_hash = fragment_hash;
    pending.color_count = 0;
    for (const auto& color_desc : cb_descs) {
        if (!color_desc.first || pending.color_count >= pending.color_images.size()) {
            continue;
        }
        pending.color_images[pending.color_count++] =
            capture_identity(color_desc.first, color_desc.second.info.guest_address,
                             color_desc.second.view_info.range);
    }
    if (db_desc.first) {
        pending.depth_image =
            capture_identity(db_desc.first, db_desc.second.info.guest_address,
                             db_desc.second.view_info.range);
    }

    const auto& regs = liverpool->regs;
    pending.depth_render_control_raw = std::bit_cast<u32>(regs.depth_render_control);
    pending.depth_control_raw = std::bit_cast<u32>(regs.depth_control);
    pending.depth_clear_enable = regs.depth_render_control.depth_clear_enable;
    pending.depth_write_enable = regs.depth_control.depth_write_enable;
    pending.depth_effective_write =
        regs.depth_control.depth_write_enable && !regs.depth_render_control.depth_clear_enable;
    const auto& dynamic = scheduler.GetDynamicState();
    pending.scheduler_depth_test = dynamic.depth_test_enabled;
    pending.scheduler_depth_write = dynamic.depth_write_enabled;
    pending.scheduler_depth_compare = static_cast<u32>(dynamic.depth_compare_op);
    pending.depth_read_address = regs.depth_buffer.DepthAddress();
    pending.depth_write_address = regs.depth_buffer.DepthWriteAddress();
    pending.stencil_read_address = regs.depth_buffer.StencilAddress();
    pending.stencil_write_address = regs.depth_buffer.StencilWriteAddress();
    pending.htile_address = regs.depth_htile_data_base.GetAddress();
    pending.desc_stencil_address = db_desc.first ? db_desc.second.info.stencil_addr : 0;
    pending.desc_htile_address =
        db_desc.first ? db_desc.second.info.meta_info.htile_addr : 0;
    pending.armed = pending.color_count != 0 || static_cast<bool>(pending.depth_image.image_id);

    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GPU_PASS_END_ARM] epoch=%llu seq=%llu attach=0x%llx ps=0x%llx "
        "colors=%u depth=%u dbRender=0x%x dbControl=0x%x clear=%u write=%u "
        "effectiveWrite=%u schedTest=%u schedWrite=%u schedCompare=%u "
        "depthRead=0x%llx depthWrite=0x%llx stencilRead=0x%llx stencilWrite=0x%llx "
        "htile=0x%llx descStencil=0x%llx descHtile=0x%llx",
        static_cast<unsigned long long>(pending.epoch),
        static_cast<unsigned long long>(pending.selected_sequence),
        static_cast<unsigned long long>(pending.attachment_signature),
        static_cast<unsigned long long>(pending.fragment_hash), pending.color_count,
        pending.depth_image.image_id ? 1u : 0u, pending.depth_render_control_raw,
        pending.depth_control_raw, pending.depth_clear_enable, pending.depth_write_enable,
        pending.depth_effective_write, pending.scheduler_depth_test,
        pending.scheduler_depth_write, pending.scheduler_depth_compare,
        static_cast<unsigned long long>(pending.depth_read_address),
        static_cast<unsigned long long>(pending.depth_write_address),
        static_cast<unsigned long long>(pending.stencil_read_address),
        static_cast<unsigned long long>(pending.stencil_write_address),
        static_cast<unsigned long long>(pending.htile_address),
        static_cast<unsigned long long>(pending.desc_stencil_address),
        static_cast<unsigned long long>(pending.desc_htile_address));

    const auto log_identity = [&](const char* role, u32 index,
                                  const ExecutorGpuLatePassProbeState::ImageIdentity& identity) {
        if (!identity.image_id) {
            return;
        }
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_PASS_END_IMAGE] phase=arm role=%s index=%u imageId=%u "
            "descGuest=0x%llx imageGuest=0x%llx imageUid=%llu backing=0x%llx "
            "vkImage=0x%llx mip=%u+%u layer=%u+%u",
            role, index, identity.image_id.index,
            static_cast<unsigned long long>(identity.desc_guest_address),
            static_cast<unsigned long long>(identity.image_guest_address),
            static_cast<unsigned long long>(identity.image_uid),
            static_cast<unsigned long long>(identity.backing_identity),
            static_cast<unsigned long long>(identity.image_handle), identity.view_range.base.level,
            identity.view_range.extent.levels, identity.view_range.base.layer,
            identity.view_range.extent.layers);
    };
    for (u32 index = 0; index < pending.color_count; ++index) {
        log_identity("color", index, pending.color_images[index]);
    }
    log_identity("depth", 0, pending.depth_image);
}

void Rasterizer::ExecutorGpuLatePassFlush(u64 next_attachment_signature, u64 next_sequence,
                                          const char* reason) {
    if (!executor_gpu_late_pass_probe.armed) {
        return;
    }

    const auto completed = executor_gpu_late_pass_probe;
    executor_gpu_late_pass_probe = {};
    executor_gpu_late_pass_probe.epoch = completed.epoch;

    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GPU_PASS_END_FLUSH] epoch=%llu selectedSeq=%llu nextSeq=%llu "
        "attach=0x%llx nextAttach=0x%llx ps=0x%llx colors=%u depth=%u reason=%s",
        static_cast<unsigned long long>(completed.epoch),
        static_cast<unsigned long long>(completed.selected_sequence),
        static_cast<unsigned long long>(next_sequence),
        static_cast<unsigned long long>(completed.attachment_signature),
        static_cast<unsigned long long>(next_attachment_signature),
        static_cast<unsigned long long>(completed.fragment_hash), completed.color_count,
        completed.depth_image.image_id ? 1u : 0u, reason ? reason : "unknown");

    const auto log_current_identity = [&](const char* role, u32 index,
                                          const ExecutorGpuLatePassProbeState::ImageIdentity& captured) {
        if (!captured.image_id) {
            return;
        }
        const auto& image = texture_cache.GetImage(captured.image_id);
        const u64 backing_identity = reinterpret_cast<uintptr_t>(image.backing);
        const u64 image_handle =
            reinterpret_cast<u64>(static_cast<VkImage>(image.GetImage()));
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_PASS_END_IMAGE] phase=flush role=%s index=%u imageId=%u "
            "capturedUid=%llu currentUid=%llu capturedBacking=0x%llx currentBacking=0x%llx "
            "capturedVkImage=0x%llx currentVkImage=0x%llx same=%u",
            role, index, captured.image_id.index,
            static_cast<unsigned long long>(captured.image_uid),
            static_cast<unsigned long long>(image.image_uid),
            static_cast<unsigned long long>(captured.backing_identity),
            static_cast<unsigned long long>(backing_identity),
            static_cast<unsigned long long>(captured.image_handle),
            static_cast<unsigned long long>(image_handle),
            captured.image_uid == image.image_uid && captured.backing_identity == backing_identity &&
                    captured.image_handle == image_handle
                ? 1u
                : 0u);
    };

    for (u32 index = 0; index < completed.color_count; ++index) {
        log_current_identity("color", index, completed.color_images[index]);
        texture_cache.ExecutorGpuContentProbe(
            completed.color_images[index].image_id,
            VideoCore::TextureCache::ExecutorGpuContentRole::ColorPassEnd,
            static_cast<u32>(Shader::LogicalStage::Fragment), completed.fragment_hash,
            completed.selected_sequence);
    }
    if (completed.depth_image.image_id) {
        log_current_identity("depth", 0, completed.depth_image);
        texture_cache.ExecutorGpuContentProbe(
            completed.depth_image.image_id,
            VideoCore::TextureCache::ExecutorGpuContentRole::DepthPassEnd,
            static_cast<u32>(Shader::LogicalStage::Fragment), completed.fragment_hash,
            completed.selected_sequence);
    }
}
#endif

void Rasterizer::CpSync() {
    if (IsDeviceTerminal()) {
        return;
    }
    scheduler.FlushPendingBufferTransfers();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (VideoCore::IsManagedVulkanDriverOptimization()) {
        // PFP_SYNC_ME predates the resource-specific barriers now emitted by
        // DrawIndirect/DispatchIndirect. A global compute->indirect barrier is
        // still kept after a real dispatch, but repeated sync packets with no
        // compute producer must not terminate and reopen dynamic rendering.
        if (!executor_compute_work_since_cp_sync) {
            return;
        }
        executor_compute_work_since_cp_sync = false;
    }
    ++executor_completion_work_serial;
#endif
    if (!scheduler.HasRecordingCommandBuffer()) {
        return;
    }
    scheduler.EndRendering(Scheduler::EndRenderingReason::RasterizerBarrier);
    auto cmdbuf = scheduler.CommandBuffer();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlagBits::eByRegion, ib_barrier, {}, {});
}

void Rasterizer::CompletionWaitBarrier(const u64 dependency_tick) {
    const bool completion_inside_rendering = scheduler.IsRendering();
    const bool completion_after_pending_transfers =
        scheduler.HasPendingBufferTransfers();
    scheduler.FlushPendingBufferTransfers();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (scheduler.IsDeviceLost() || !scheduler.HasRecordingCommandBuffer()) {
        executor_completion_barrier_valid = false;
        return;
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ++executor_completion_barrier_calls;
    const u64 current_tick = scheduler.CurrentTick();
    const u64 current_epoch = scheduler.CommandRecordingEpoch();
    // Managed EOP/EOS labels are CPU-side timeline sidecars, not resource
    // accesses.  If the wait arrives inside one graphics scope, later
    // attachment work remains ordered by that scope and buffer/image hazards
    // still emit their normal resource-specific barriers.  If queued transfers
    // were just flushed, their all-commands pre/post memory barriers already
    // provide the same dependency.  Re-emitting an all-commands barrier here
    // ended dynamic rendering thousands of times per second on Adreno without
    // adding an uncovered resource hazard.
    const bool turnip_completion_sidecar_elision =
        ExecutorElideTurnipCompletionSidecars(instance);
    const bool sidecar_dependency_already_ordered =
        Config::managedReadbackBatching() &&
        VideoCore::IsManagedVulkanDriverOptimization() &&
        (completion_inside_rendering || completion_after_pending_transfers ||
         turnip_completion_sidecar_elision);
    if (sidecar_dependency_already_ordered) {
        VideoCore::RecordVulkanBarrierElision();
        ++executor_completion_barrier_elided;
        executor_completion_barrier_tick = current_tick;
        executor_completion_barrier_epoch = current_epoch;
        executor_completion_barrier_dependency_tick =
            std::max(executor_completion_barrier_dependency_tick,
                     dependency_tick);
        executor_completion_barrier_work_serial =
            executor_completion_work_serial;
        executor_completion_barrier_valid = true;
        return;
    }
    const bool barrier_is_redundant =
        VideoCore::IsManagedVulkanDriverOptimization() &&
        executor_completion_barrier_valid &&
        executor_completion_barrier_tick == current_tick &&
        executor_completion_barrier_epoch == current_epoch &&
        executor_completion_barrier_work_serial == executor_completion_work_serial &&
        dependency_tick <= executor_completion_barrier_dependency_tick;
    if (barrier_is_redundant) {
        VideoCore::RecordVulkanBarrierElision();
        ++executor_completion_barrier_elided;
        if ((executor_completion_barrier_calls & 4095u) == 0u) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_COMPLETION_BARRIER_COALESCE] calls=%llu emitted=%llu elided=%llu "
                "elidePct=%.2f tick=%llu epoch=%llu",
                static_cast<unsigned long long>(executor_completion_barrier_calls),
                static_cast<unsigned long long>(executor_completion_barrier_emitted),
                static_cast<unsigned long long>(executor_completion_barrier_elided),
                executor_completion_barrier_calls != 0
                    ? static_cast<double>(executor_completion_barrier_elided) * 100.0 /
                          static_cast<double>(executor_completion_barrier_calls)
                    : 0.0,
                static_cast<unsigned long long>(current_tick),
                static_cast<unsigned long long>(current_epoch));
        }
        return;
    }
#endif
    if (scheduler.TryCompletionBarrierInsideRendering()) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        ++executor_completion_barrier_emitted;
        executor_completion_barrier_tick = current_tick;
        executor_completion_barrier_epoch = scheduler.CommandRecordingEpoch();
        executor_completion_barrier_dependency_tick = dependency_tick;
        executor_completion_barrier_work_serial = executor_completion_work_serial;
        executor_completion_barrier_valid = true;
#endif
        return;
    }
    scheduler.EndRendering(Scheduler::EndRenderingReason::RasterizerBarrier);
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask =
            vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    u64 barrier_epoch{};
    scheduler.CommandBuffer(barrier_epoch).pipelineBarrier2(vk::DependencyInfo{
#else
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
#endif
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ++executor_completion_barrier_emitted;
    if (scheduler.CurrentTick() == current_tick) {
        executor_completion_barrier_tick = current_tick;
        executor_completion_barrier_epoch = barrier_epoch;
        executor_completion_barrier_dependency_tick = dependency_tick;
        executor_completion_barrier_work_serial = executor_completion_work_serial;
        executor_completion_barrier_valid = true;
    } else {
        executor_completion_barrier_valid = false;
    }
    if ((executor_completion_barrier_calls & 4095u) == 0u) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_COMPLETION_BARRIER_COALESCE] calls=%llu emitted=%llu elided=%llu "
            "elidePct=%.2f tick=%llu epoch=%llu",
            static_cast<unsigned long long>(executor_completion_barrier_calls),
            static_cast<unsigned long long>(executor_completion_barrier_emitted),
            static_cast<unsigned long long>(executor_completion_barrier_elided),
            executor_completion_barrier_calls != 0
                ? static_cast<double>(executor_completion_barrier_elided) * 100.0 /
                      static_cast<double>(executor_completion_barrier_calls)
                : 0.0,
            static_cast<unsigned long long>(current_tick),
            static_cast<unsigned long long>(barrier_epoch));
    }
#endif
}

bool Rasterizer::FilterDraw() {
    const auto& regs = liverpool->regs;
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }
    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = liverpool->regs;
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        static const bool kTracePrep =
            ExecutorTracePm4Enabled() ||
            (::access("/data/data/app.lsx4.android/files/lsx4-home/run-trace-draw", F_OK) == 0);
        if (kTracePrep)
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                                "[EXECUTOR_DRAW_TRACE] prep_cb cb=%u skip=%d colBufValid=%d mask=%u mrt=%d base=0x%llx",
                                cb, (int)skip_cb_binding, (int)(bool)col_buf, target_mask,
                                (int)((key.mrt_mask & (1 << cb)) != 0),
                                (unsigned long long)col_buf.Address());
#endif
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = liverpool->last_cb_extent[cb];
        std::construct_at(&desc, col_buf, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
        if (Libraries::GnmDriver::ExecutorGnmCaptureActive()) {
            Libraries::GnmDriver::ExecutorGnmCaptureRecordResource(
                5u, col_buf.Address(), static_cast<u64>(col_buf.GetColorSliceSize()) * col_buf.NumSlices());
        }
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = liverpool->last_db_extent;
        auto& [image_id, desc] = db_desc;
        std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                          htile_address, hint);
        image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
        if (Libraries::GnmDriver::ExecutorGnmCaptureActive()) {
            const auto& db = regs.depth_buffer;
            if (db.DepthValid()) {
                Libraries::GnmDriver::ExecutorGnmCaptureRecordResource(5u, db.DepthAddress(),
                                                                       db.GetDepthSliceSize());
            }
            if (db.StencilValid()) {
                Libraries::GnmDriver::ExecutorGnmCaptureRecordResource(5u, db.StencilAddress(),
                                                                       db.GetDepthSliceSize());
            }
            if (htile_address) {
                Libraries::GnmDriver::ExecutorGnmCaptureRecordResource(5u, htile_address, 0x40000u);
            }
        }
    } else {
        db_desc.first = {};
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.user_data[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.user_data[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = liverpool->regs.color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, liverpool->last_cb_extent[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto* image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (image_view == nullptr) {
        return;
    }
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    if (Config::getVkHostMarkersEnabled()) {
        ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                     col_buf.CmaskAddress()));
    }
    image.Clear(clear_value, desc.view_info.range);
    ScopeMarkerEnd();
}

extern "C" {
__attribute__((used)) std::atomic<std::uint64_t> lsx4_natural_publications{0};
}

// At entry, no descriptors/geometry for the next work item have been prepared.
// Existing closed render scopes only: never create a new attachment split.
namespace {
struct ClaudeRasterStats {
    bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_RASTER_STATS");
        return value && value[0] == '1';
    }();
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 ns[4]{};
    u64 n[4]{};
};
ClaudeRasterStats g_claude_raster;
struct ClaudeRasterTimer {
    int kind;
    std::chrono::steady_clock::time_point begin{};
    explicit ClaudeRasterTimer(int kind_) : kind{kind_} {
        if (g_claude_raster.enabled) begin = std::chrono::steady_clock::now();
    }
    ~ClaudeRasterTimer() {
        if (!g_claude_raster.enabled) return;
        const auto now = std::chrono::steady_clock::now();
        g_claude_raster.ns[kind] += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin).count());
        ++g_claude_raster.n[kind];
        const auto window = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - g_claude_raster.window)
                                .count();
        if (window < 2000) return;
        std::fprintf(stderr,
                     "[CLAUDE_RASTER] windowMs=%lld draw=%llux%.1fms drawIndirect=%llux%.1fms "
                     "dispatch=%llux%.1fms dispatchIndirect=%llux%.1fms\n",
                     static_cast<long long>(window), static_cast<unsigned long long>(g_claude_raster.n[0]),
                     g_claude_raster.ns[0] / 1e6, static_cast<unsigned long long>(g_claude_raster.n[1]),
                     g_claude_raster.ns[1] / 1e6, static_cast<unsigned long long>(g_claude_raster.n[2]),
                     g_claude_raster.ns[2] / 1e6, static_cast<unsigned long long>(g_claude_raster.n[3]),
                     g_claude_raster.ns[3] / 1e6);
        g_claude_raster.window = now;
        for (int i = 0; i < 4; ++i) {
            g_claude_raster.ns[i] = 0;
            g_claude_raster.n[i] = 0;
        }
    }
};
} // namespace

namespace {
struct ClaudeDispatchEntry {
    u64 address;
    u64 count;
    u64 groups;
    u32 local;
    u32 max_groups;
};
struct ClaudeDispatchCensus {
    bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_DISPATCH_CENSUS");
        return value && value[0] == '1';
    }();
    std::array<ClaudeDispatchEntry, 128> entries{};
    u64 other{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};
ClaudeDispatchCensus g_claude_census;
void ClaudeCensusNote(u64 address, u32 groups, u32 local) {
    auto& c = g_claude_census;
    bool placed = false;
    for (auto& e : c.entries) {
        if (e.address == address || e.address == 0) {
            e.address = address;
            ++e.count;
            e.groups += groups;
            e.local = local;
            e.max_groups = std::max(e.max_groups, groups);
            placed = true;
            break;
        }
    }
    if (!placed) ++c.other;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - c.window).count() < 2000) return;
    std::array<ClaudeDispatchEntry, 128> sorted = c.entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.count > b.count; });
    std::fprintf(stderr, "[CLAUDE_DISPATCH_CENSUS] other=%llu", static_cast<unsigned long long>(c.other));
    for (int i = 0; i < 12 && sorted[i].count != 0; ++i) {
        std::fprintf(stderr, " | %llx n=%llu groups=%llu max=%u local=%u",
                     static_cast<unsigned long long>(sorted[i].address),
                     static_cast<unsigned long long>(sorted[i].count),
                     static_cast<unsigned long long>(sorted[i].groups), sorted[i].max_groups,
                     sorted[i].local);
    }
    std::fprintf(stderr, "\n");
    c.entries = {};
    c.other = 0;
    c.window = now;
}
} // namespace

static bool ClaudeHotFlush(Rasterizer& rasterizer) {
    static const int mode = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_HOT_FLUSH");
        return value ? std::atoi(value) : 0;
    }();
    static const int min_items = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_HOT_FLUSH_MIN");
        return value ? std::max(1, std::atoi(value)) : 4;
    }();
    if (mode == 0 || VideoCore::claude_hot_written.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    VideoCore::claude_hot_written.store(0, std::memory_order_relaxed);
    if (mode == 2 && (static_cast<u64>(std::time(nullptr)) / 5u) % 2u != 0u) return false;
    struct LocalCount { Scheduler* owner{}; u64 tick{}; int count{}; };
    static thread_local LocalCount local;
    auto& scheduler = rasterizer.GetScheduler();
    if (local.owner != &scheduler) { local = {}; local.owner = &scheduler; }
    const u64 tick = scheduler.CurrentTick();
    if (tick != local.tick) { local.tick = tick; local.count = 0; }
    if (++local.count < min_items) {
        VideoCore::claude_hot_written.store(1, std::memory_order_relaxed);
        return false;
    }
    if (rasterizer.IsDeviceTerminal() || !scheduler.HasRecordingCommandBuffer() ||
        !scheduler.HasPendingWork()) {
        return false;
    }
    rasterizer.Flush();
    scheduler.PublishMemoryPressureBoundary();
    VideoCore::claude_hot_flushes.fetch_add(1, std::memory_order_relaxed);
    local.tick = scheduler.CurrentTick();
    local.count = 0;
    return true;
}

static bool ClaudeEagerSubmit(Rasterizer& rasterizer) {
    ClaudeHotFlush(rasterizer);
    static const int budget = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_EAGER_SUBMIT");
        return value ? std::atoi(value) : 0;
    }();
    if (budget <= 0) return false;
    static const bool ab = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_EAGER_AB");
        return value && value[0] == '1';
    }();
    static const bool force = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_EAGER_FORCE");
        return value && value[0] == '1';
    }();
    // Adaptive: past the minimum budget, publish only while the GPU has
    // retired every submitted tick (it is starving); keep batching otherwise.
    static const bool adaptive = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_EAGER_ADAPTIVE");
        return value && value[0] == '1';
    }();
    if (rasterizer.IsDeviceTerminal()) return true;
    struct LocalCount { Scheduler* owner{}; u64 tick{}; int count{}; };
    static thread_local LocalCount local;
    auto& scheduler = rasterizer.GetScheduler();
    if (local.owner != &scheduler) { local = {}; local.owner = &scheduler; }
    const u64 tick = scheduler.CurrentTick();
    if (tick != local.tick) { local.tick = tick; local.count = 0; }
    if (local.count < budget) { ++local.count; return true; }
    if (ab && (static_cast<u64>(std::time(nullptr)) / 5u) % 2u != 0u) return true;
    if (!scheduler.HasRecordingCommandBuffer() || !scheduler.HasPendingWork() ||
        (!force && scheduler.IsRendering())) return true;
    if (adaptive && tick > 1 && !scheduler.IsFree(tick - 1)) return true;
    rasterizer.Flush();
    scheduler.PublishMemoryPressureBoundary();
    local.tick = scheduler.CurrentTick();
    local.count = 1;
    return true;
}

} // namespace Vulkan
std::atomic<bool> lsx4_claude_cc_barrier_pending{false};
std::atomic<bool> lsx4_claude_cc_dispatched{false};
std::atomic<unsigned long long> lsx4_claude_cc_barriers{0};
namespace Vulkan {
static void ClaudeConcurrentBarrierBeforeDispatch(vk::CommandBuffer cmdbuf) {
    if (lsx4_claude_cc_barrier_pending.exchange(false, std::memory_order_relaxed)) {
        const vk::MemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader |
                            vk::PipelineStageFlagBits2::eDrawIndirect,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite |
                             vk::AccessFlagBits2::eIndirectCommandRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &barrier,
        });
        lsx4_claude_cc_barriers.fetch_add(1, std::memory_order_relaxed);
    }
    lsx4_claude_cc_dispatched.store(true, std::memory_order_relaxed);
}

static void PortablePublishNaturalBoundary(Rasterizer& rasterizer) {
    if (ClaudeEagerSubmit(rasterizer)) return;
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_NATURAL_BOUNDARY_SUBMIT");
        return value && value[0]=='1' && value[1]=='\0';
    }();
    if (!enabled || rasterizer.IsDeviceTerminal()) return;
    struct LocalBudget { Scheduler* owner{}; NaturalBoundarySubmitBudget budget; };
    static thread_local LocalBudget local;
    auto& scheduler = rasterizer.GetScheduler();
    if (local.owner != &scheduler) { local = {}; local.owner = &scheduler; }
    if (!scheduler.HasRecordingCommandBuffer() ||
        !local.budget.Observe(scheduler.CurrentTick(),scheduler.IsRendering(),
                              scheduler.HasPendingWork())) return;
    rasterizer.Flush();
    // Existing complete, queue-ordered, nonblocking publication. Pop only
    // retired callbacks; CPU readback retains its separate retirement wait.
    scheduler.PublishMemoryPressureBoundary();
    local.budget.Published(scheduler.CurrentTick());
    if (lsx4_natural_publications.fetch_add(1,std::memory_order_relaxed)==0)
        std::fprintf(stderr,"[PORTABLE_NATURAL_PUBLICATION] first=1 budget=128 activeRenderSplit=0\n");
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    PortablePublishNaturalBoundary(*this);
    ClaudeRasterTimer claude_raster_timer{0};
    if (!Config::managedGpuFastPath()) PortableGpu::BeginProducerBindings(&buffer_cache,&scheduler);
    if (IsDeviceTerminal()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    scheduler.MaybePublishExactA750D9SampledRecordingPressureBoundary("draw");
#endif
    ExecutorOptionalDrawStageProfile draw_stage_profile{
        ExecutorDrawStageProfileEnabled(), is_indexed};
    draw_stage_profile.Begin(ExecutorDrawCpuStage::PreDraw);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorAndroidRendererProbesEnabled()) {
        const auto& st = scheduler.GetRenderState();
        double px = 0.0;
        for (const auto& vp : liverpool->regs.viewports) {
            const double w = std::abs(vp.xscale) * 2.0, h = std::abs(vp.yscale) * 2.0;
            if (w > 0.0 && h > 0.0) { px = w * h; break; }
        }
        if (px <= 0.0) px = double(st.width) * double(st.height);
        g_shade_area.note(st.width, st.height, px);
    }
#endif
    RENDERER_TRACE;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ++executor_completion_work_serial;
    static const bool trace_actual_draw =
        ExecutorTraceLiveWideEnabled() ||
        ExecutorTracePm4Enabled() ||
        (::access("/data/data/app.lsx4.android/files/lsx4-home/run-trace-draw",
                  F_OK) == 0);
    if (trace_actual_draw) {
        LOG_INFO(Render_Vulkan,
                 "EXECUTOR_VK_ACTUAL_DRAW phase=rasterizer_draw indexed={} index_offset={}",
                 is_indexed ? "YES" : "NO", index_offset);
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_VK_ACTUAL_DRAW] phase=rasterizer_draw indexed=%u "
                            "indexOffset=%u",
                            is_indexed ? 1u : 0u, index_offset);
    }
#else
    LOG_INFO(Render_Vulkan, "EXECUTOR_VK_ACTUAL_DRAW phase=rasterizer_draw indexed={} index_offset={}",
             is_indexed ? "YES" : "NO", index_offset);
#endif

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        draw_stage_profile.End(ExecutorDrawCpuStage::PreDraw);
        draw_stage_profile.MarkFiltered();
        return;
    }
    draw_stage_profile.End(ExecutorDrawCpuStage::PreDraw);

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const int kBisectDrawLevel = [] {
        const char* e = std::getenv("EXECUTOR_BISECT_DRAW");
        const int lvl = e ? std::atoi(e) : 0;
        if (lvl > 0) {
            __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                                "[EXECUTOR_BISECT_DRAW] active level=%d", lvl);
        }
        return lvl;
    }();
    if (kBisectDrawLevel == 1) {
        return;
    }
#endif

    const auto& regs = liverpool->regs;
    if (Libraries::GnmDriver::ExecutorGnmCaptureActive()) {
        Libraries::GnmDriver::ExecutorGnmCaptureOnDraw();
        const auto rec_shader = [](const auto& prog, u32 stage) {
            const u64 addr = reinterpret_cast<u64>(prog.template Address<const u8*>());
            if (addr == 0) {
                return;
            }
            const auto p = AmdGpu::GetParams(prog);
            Libraries::GnmDriver::ExecutorGnmCaptureRecordShader(
                stage, p.hash, addr, static_cast<u32>(p.code.size() * sizeof(u32)));
        };
        rec_shader(regs.ps_program, 0u);
        rec_shader(regs.vs_program, 1u);
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool kTraceDraw =
        ExecutorTracePm4Enabled() ||
        (::access("/data/data/app.lsx4.android/files/lsx4-home/run-trace-draw", F_OK) == 0);
#define EXEC_DRAW_TRACE(stage)                                                                          \
    do {                                                                                               \
        if (kTraceDraw)                                                                                \
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_DRAW_TRACE] " stage);         \
    } while (0)
#else
#define EXEC_DRAW_TRACE(stage) ((void)0)
#endif
    EXEC_DRAW_TRACE("enter");
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_draw_diag = Libraries::GnmDriver::ExecutorReplayActive() ||
                                    ExecutorTraceLivePresentResourcesEnabled();
    if (Libraries::GnmDriver::ExecutorReplayActive()) {
        Core::Memory::Instance()->VerifyReplayCanaries("draw_enter");
    }
    if ((kTraceDraw && Libraries::GnmDriver::ExecutorReplayActive()) || executor_draw_diag) {
        const u64 ps_a = reinterpret_cast<u64>(regs.ps_program.Address<const u8*>());
        const u64 vs_a = reinterpret_cast<u64>(regs.vs_program.Address<const u8*>());
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_PIPELINE_INPUT] indexed=%d psAddr=0x%llx psReloc=%d vsAddr=0x%llx "
                            "vsReloc=%d numIndices=%u",
                            (int)is_indexed, (unsigned long long)ps_a,
                            (int)Libraries::GnmDriver::ExecutorReplayIsRelocated(ps_a),
                            (unsigned long long)vs_a,
                            (int)Libraries::GnmDriver::ExecutorReplayIsRelocated(vs_a),
                            regs.num_indices);
    }
#endif
    const bool single_quad_fan =
        regs.primitive_type == AmdGpu::PrimitiveType::QuadList && regs.num_indices == 4 &&
        regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    // MoltenVK has no triangle fans: each fan draw costs an index-conversion
    // compute pass and a Metal render pass restart. Draw the quad as two
    // listed triangles in the fan's vertex order (provoking vertices 1 and 2).
    static const int kQuadTriListMode = [] {
        const char* value = std::getenv("EXECUTOR_CLAUDE_QUAD_TRILIST");
        return value ? std::atoi(value) : 0;
    }();
    static constexpr u16 kQuadTriListIndices[6]{1, 2, 0, 2, 3, 0};
    bool quad_tri_list = false;
    vk::Buffer quad_index_buffer{};
    u64 quad_index_offset = 0;
    // Only non-indexed quads: an indexed quad's guest index buffer selects the
    // vertices and must stay bound.
    if (single_quad_fan && !is_indexed && kQuadTriListMode != 0) {
        quad_tri_list = kQuadTriListMode == 1 ||
                        (kQuadTriListMode == 2 &&
                         (static_cast<u64>(std::time(nullptr)) / 5u) % 2u == 0u);
        if (quad_tri_list) {
            auto& stream = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
            const auto [index_data, index_offset] = stream.Map(sizeof(kQuadTriListIndices), 4);
            if (index_data != nullptr) {
                std::memcpy(index_data, kQuadTriListIndices, sizeof(kQuadTriListIndices));
                stream.Commit();
                quad_index_buffer = stream.Handle();
                quad_index_offset = index_offset;
            } else {
                quad_tri_list = false;
            }
        }
    }
    draw_stage_profile.Begin(ExecutorDrawCpuStage::PipelineLookup);
    const GraphicsPipeline* pipeline =
        pipeline_cache.GetGraphicsPipeline(
            single_quad_fan
                ? std::optional{quad_tri_list ? AmdGpu::PrimitiveType::TriangleList
                                              : AmdGpu::PrimitiveType::TriangleFan}
                : std::nullopt);
    draw_stage_profile.End(ExecutorDrawCpuStage::PipelineLookup);
    if (!pipeline || !pipeline->IsValid()) {
        draw_stage_profile.MarkPipelineMiss();
        EXEC_DRAW_TRACE("no_pipeline_return");
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorBisectSkipPipeline(pipeline->GetPipelineHash())) {
        return;
    }
#endif
    EXEC_DRAW_TRACE("pipeline_ok");
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorPreparePhysicalDrsDraw(pipeline);
    if (kBisectDrawLevel == 2) {
        return;
    }
#endif

    draw_stage_profile.Begin(ExecutorDrawCpuStage::PrepareRenderState);
    PrepareRenderState(pipeline);
    draw_stage_profile.End(ExecutorDrawCpuStage::PrepareRenderState);
    EXEC_DRAW_TRACE("render_state_ok");
    auto resource_upload_batch = scheduler.BeginBufferUploadBatch();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    buffer_cache.BeginReadOnlyDescriptorSnapshotBatch();
    SCOPE_EXIT { buffer_cache.CancelReadOnlyDescriptorSnapshotBatch(); };
#endif
    if (!BindResources(pipeline)) {
        draw_stage_profile.MarkBindFailure();
        draw_stage_profile.Begin(ExecutorDrawCpuStage::UploadFlush);
        resource_upload_batch.Flush();
        draw_stage_profile.End(ExecutorDrawCpuStage::UploadFlush);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
#endif
        EXEC_DRAW_TRACE("bind_resources_return");
        return;
    }
    draw_stage_profile.Begin(ExecutorDrawCpuStage::UploadFlush);
    resource_upload_batch.Flush();
    draw_stage_profile.End(ExecutorDrawCpuStage::UploadFlush);
    EXEC_DRAW_TRACE("bind_resources_ok");
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (kBisectDrawLevel == 3) {
        buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
        return;
    }
#endif

    const bool executor_gpu_probe_enabled = texture_cache.ExecutorGpuContentProbeEnabled();
    const u64 executor_gpu_probe_draw =
        executor_gpu_probe_enabled ? ++executor_gpu_probe_draw_sequence : 0;
    const auto* executor_gpu_probe_fragment =
        pipeline->TryGetStage(Shader::LogicalStage::Fragment);
    const u64 executor_gpu_probe_fragment_hash =
        executor_gpu_probe_fragment ? executor_gpu_probe_fragment->pgm_hash : 0;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto executor_gpu_probe_attachments =
        executor_gpu_probe_enabled ? ExecutorSummarizeAttachments(cb_descs, db_desc)
                                   : ExecutorVkAttachmentSummary{};
    const u64 executor_gpu_probe_attachment_signature =
        executor_gpu_probe_attachments.signature;
    const bool executor_gpu_probe_late = AmdGpu::RenderWaveTrace::LateFrameCaptureActive();
#else
    const u64 executor_gpu_probe_attachment_signature = 0;
    const bool executor_gpu_probe_late = false;
#endif
    const u64 executor_gpu_probe_extent = executor_gpu_probe_enabled
        ? texture_cache.ExecutorGpuContentProbeExtentFilter() : 0;
    const bool executor_gpu_probe_extent_matches = executor_gpu_probe_extent &&
        executor_gpu_probe_fragment && std::ranges::any_of(
            executor_gpu_probe_fragment->images, [&](const auto& descriptor) {
                const auto sharp = descriptor.GetSharp(*executor_gpu_probe_fragment);
                if (!descriptor.is_written) texture_cache.ExecutorGpuContentProbeObserveImage(sharp);
                return !descriptor.is_written &&
                    static_cast<u64>(sharp.width + 1) == (executor_gpu_probe_extent >> 32) &&
                    static_cast<u64>(sharp.height + 1) == (executor_gpu_probe_extent & 0xffffffffu);
            });
    const bool executor_gpu_probe_selected =
        executor_gpu_probe_enabled &&
        texture_cache.ExecutorGpuContentProbeDrawSelected(executor_gpu_probe_draw,
                                                          executor_gpu_probe_fragment_hash,
                                                          executor_gpu_probe_attachment_signature,
                                                          executor_gpu_probe_attachments.color_count,
                                                          cb_descs[0].first
                                                              ? cb_descs[0].second.info.size.width
                                                              : 0,
                                                          cb_descs[0].first
                                                              ? cb_descs[0].second.info.size.height
                                                              : 0,
                                                          static_cast<bool>(db_desc.first),
                                                          regs.num_indices, false,
                                                          executor_gpu_probe_extent_matches);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_gpu_probe_buffers =
        executor_gpu_probe_selected && ExecutorGpuContentProbeBuffersEnabled();
#else
    const bool executor_gpu_probe_buffers = false;
#endif

    // Publish geometry ranges before attachment discovery and Image::Transit so
    // aliased buffer/image dependencies are visible to the transition path.
    auto geometry_upload_batch = scheduler.BeginBufferUploadBatch();
    draw_stage_profile.Begin(ExecutorDrawCpuStage::VertexIndex);
    const auto prepared_vertex_buffers =
        buffer_cache.PrepareVertexBuffers(*pipeline, buffer_barriers,
                                          executor_gpu_probe_buffers, executor_gpu_probe_draw);
    if (!prepared_vertex_buffers.valid) {
        draw_stage_profile.End(ExecutorDrawCpuStage::VertexIndex);
        draw_stage_profile.Begin(ExecutorDrawCpuStage::UploadFlush);
        geometry_upload_batch.Flush();
        draw_stage_profile.End(ExecutorDrawCpuStage::UploadFlush);
        buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
        draw_stage_profile.MarkBindFailure();
        EXEC_DRAW_TRACE("invalid_vertex_range_return");
        return;
    }
    EXEC_DRAW_TRACE("vertex_buffers_ok");
    VideoCore::BufferCache::PreparedIndexBuffer prepared_index_buffer{};
    if (is_indexed) {
        prepared_index_buffer =
            buffer_cache.PrepareIndexBuffer(index_offset, buffer_barriers,
                                            executor_gpu_probe_buffers, executor_gpu_probe_draw);
    }
    draw_stage_profile.End(ExecutorDrawCpuStage::VertexIndex);
    draw_stage_profile.Begin(ExecutorDrawCpuStage::UploadFlush);
    geometry_upload_batch.Flush();
    draw_stage_profile.End(ExecutorDrawCpuStage::UploadFlush);
    draw_stage_profile.Begin(ExecutorDrawCpuStage::BeginRendering);
    auto state = BeginRendering(pipeline);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_physical_drs_selected =
        ExecutorBeginPhysicalDrs(pipeline, state);
#endif
    UpdateAttachmentlessRenderArea(state);
    UpdateFragmentShadingRate(pipeline, state);
    draw_stage_profile.End(ExecutorDrawCpuStage::BeginRendering);
    EXEC_DRAW_TRACE("begin_rendering_ok");
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (Libraries::GnmDriver::ExecutorReplayActive()) {
        bool any_color = false;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            if (state.color_attachments[cb].image_view) {
                any_color = true;
                break;
            }
        }
        if (!any_color) {
            EXEC_DRAW_TRACE("skip_zprepass_replay");
            return;
        }
    }
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_gpu_probe_late) {
        ExecutorGpuLatePassTransition(executor_gpu_probe_attachment_signature,
                                      executor_gpu_probe_draw, "graphics_signature");
    }
#endif
    if (executor_gpu_probe_selected) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const auto& probe_key = pipeline->GetGraphicsKey();
        const auto& probe_vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
        const auto [probe_vertex_offset, probe_instance_offset] =
            GetDrawOffsets(regs, probe_vs_info, pipeline->GetFetchShader());
        const auto probe_stage_hash = [&](Shader::LogicalStage stage) {
            return probe_key.stage_hashes[static_cast<u32>(stage)];
        };
        const auto probe_is_list_topology = [](const AmdGpu::PrimitiveType type) {
            const auto topology = LiverpoolToVK::PrimitiveType(type);
            return topology == vk::PrimitiveTopology::ePointList ||
                   topology == vk::PrimitiveTopology::eLineList ||
                   topology == vk::PrimitiveTopology::eTriangleList ||
                   topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
                   topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
        };
        const auto probe_is_patch_topology = [](const AmdGpu::PrimitiveType type) {
            return type == AmdGpu::PrimitiveType::PatchPrimitive ||
                   type == AmdGpu::PrimitiveType::QuadList ||
                   type == AmdGpu::PrimitiveType::RectList;
        };
        const bool probe_restart_raw = (regs.enable_primitive_restart & 1) != 0;
        const bool probe_restart_effective =
            probe_restart_raw &&
            (instance.IsListRestartSupported() || !probe_is_list_topology(regs.primitive_type)) &&
            (instance.IsPatchListRestartSupported() ||
             !probe_is_patch_topology(regs.primitive_type));

        Vulkan::VertexInputs<vk::VertexInputAttributeDescription2EXT> probe_attributes;
        Vulkan::VertexInputs<vk::VertexInputBindingDescription2EXT> probe_bindings;
        Vulkan::VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> probe_divisors;
        Vulkan::VertexInputs<AmdGpu::Buffer> probe_guest_buffers;
        pipeline->GetVertexInputs(probe_attributes, probe_bindings, probe_divisors,
                                  probe_guest_buffers, regs.vgt_instance_step_rate_0,
                                  regs.vgt_instance_step_rate_1);
        const auto& probe_viewport = regs.viewports[0];
        const auto& probe_depth_bounds = regs.viewport_depths[0];
        const auto& probe_viewport_control = regs.viewport_control;
        const float probe_zoffset =
            probe_viewport_control.zoffset_enable ? probe_viewport.zoffset : 0.0f;
        const float probe_zscale =
            probe_viewport_control.zscale_enable ? probe_viewport.zscale : 1.0f;
        float probe_min_depth = probe_zoffset;
        float probe_max_depth = probe_zoffset + probe_zscale;
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            probe_min_depth = probe_zoffset - probe_zscale;
            probe_max_depth = probe_zoffset + probe_zscale;
        }
        const float probe_effective_min_depth =
            instance.IsDepthRangeUnrestrictedSupported() ? probe_min_depth
                                                          : std::max(probe_min_depth, 0.0f);
        const float probe_effective_max_depth =
            instance.IsDepthRangeUnrestrictedSupported() ? probe_max_depth
                                                          : std::min(probe_max_depth, 1.0f);
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_VERTEX_PROFILE] seq=%llu fs=0x%llx legacyAttrib=%u "
            "dynamicVertexInput=%u attributes=%zu bindings=%zu divisors=%zu",
            static_cast<unsigned long long>(executor_gpu_probe_draw),
            static_cast<unsigned long long>(executor_gpu_probe_fragment_hash),
            instance.IsLegacyVertexAttributesSupported() ? 1u : 0u,
            instance.IsVertexInputDynamicState() ? 1u : 0u, probe_attributes.size(),
            probe_bindings.size(), probe_divisors.size());

        u32 probe_global_buffer = 0;
        for (const auto* probe_stage : pipeline->GetStages()) {
            if (!probe_stage) {
                continue;
            }
            for (u32 stage_buffer = 0; stage_buffer < probe_stage->buffers.size();
                 ++stage_buffer, ++probe_global_buffer) {
                if (probe_global_buffer >= buffer_infos.size() ||
                    probe_global_buffer >= push_data.buf_offsets.size()) {
                    break;
                }
                const auto& desc = probe_stage->buffers[stage_buffer];
                const auto sharp = desc.GetSharp(*probe_stage);
                const auto& host = buffer_infos[probe_global_buffer];
                const VAddr sharp_address = static_cast<VAddr>(sharp.base_address);
                const u64 sharp_size = desc.IsSpecial() ? 0 : sharp.GetSize();
                const auto sharp_fingerprint =
                    ExecutorFingerprintGuestRange(memory, sharp_address, sharp_size);
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_GPU_BUFFER_BINDING] seq=%llu global=%u stage=%u stageHash=0x%llx "
                    "slot=%u sharpIdx=%u guest=0x%llx guestSize=%u storage=%u written=%u "
                    "hostOffset=0x%llx hostRange=0x%llx compensation=0x%x storageAlign=%llu "
                    "readable=%u clamped=%llu sampleDigest=0x%llx",
                    static_cast<unsigned long long>(executor_gpu_probe_draw), probe_global_buffer,
                    static_cast<unsigned>(probe_stage->l_stage),
                    static_cast<unsigned long long>(probe_stage->pgm_hash), stage_buffer,
                    static_cast<unsigned>(desc.sharp_idx),
                    static_cast<unsigned long long>(sharp.base_address),
                    static_cast<unsigned>(sharp.GetSize()), desc.IsStorage(sharp) ? 1u : 0u,
                    desc.is_written ? 1u : 0u,
                    static_cast<unsigned long long>(host.offset),
                    static_cast<unsigned long long>(host.range),
                    static_cast<unsigned>(push_data.buf_offsets[probe_global_buffer]),
                    static_cast<unsigned long long>(instance.StorageMinAlignment()),
                    sharp_fingerprint.readable ? 1u : 0u,
                    static_cast<unsigned long long>(sharp_fingerprint.clamped_size),
                    static_cast<unsigned long long>(sharp_fingerprint.digest));
                if (executor_gpu_probe_buffers && sharp.base_address != 0 && sharp.GetSize() != 0 &&
                    host.buffer != VK_NULL_HANDLE) {
                    const u64 exact_bound_offset =
                        host.offset + push_data.buf_offsets[probe_global_buffer];
                    const char* role =
                        probe_stage->l_stage == Shader::LogicalStage::Fragment
                            ? "fs_buffer"
                        : probe_stage->l_stage == Shader::LogicalStage::Vertex
                            ? "vs_buffer"
                            : "stage_buffer";
                    buffer_cache.ExecutorProbeBoundGpuBuffer(
                        static_cast<VAddr>(sharp.base_address), sharp.GetSize(), host.buffer,
                        exact_bound_offset, role, executor_gpu_probe_draw, stage_buffer);
                }
            }
        }
        for (u32 i = 0; i < probe_guest_buffers.size(); ++i) {
            const auto& buffer = probe_guest_buffers[i];
            const u64 size = buffer.GetSize();
            const auto fingerprint = ExecutorFingerprintGuestRange(
                memory, static_cast<VAddr>(buffer.base_address), size);
            const bool gpu_modified = size != 0 && buffer_cache.IsRegionGpuModified(
                                                       static_cast<VAddr>(buffer.base_address), size);
            const bool cpu_modified = size != 0 && buffer_cache.IsRegionCpuModified(
                                                       static_cast<VAddr>(buffer.base_address), size);
            const auto dst = buffer.DstSelect();
            const u32 location = i < probe_attributes.size() ? probe_attributes[i].location : ~0u;
            const u32 binding = i < probe_attributes.size() ? probe_attributes[i].binding : ~0u;
            const u32 host_format = i < probe_attributes.size()
                                        ? static_cast<u32>(probe_attributes[i].format)
                                        : 0u;
            const u32 divisor = i < probe_bindings.size() ? probe_bindings[i].divisor : 0u;
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_VERTEX_INPUT] seq=%llu slot=%u loc=%u bind=%u guest=0x%llx "
                "size=%llu clamped=%llu records=%u stride=%u dataFmt=%u numFmt=%u "
                "hostFmt=%u dst=%u,%u,%u,%u divisor=%u gpuDirty=%u cpuDirty=%u "
                "readable=%u sampleDigest=0x%llx",
                static_cast<unsigned long long>(executor_gpu_probe_draw), i, location, binding,
                static_cast<unsigned long long>(buffer.base_address),
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(fingerprint.clamped_size), buffer.num_records,
                buffer.GetStride(), static_cast<unsigned>(buffer.GetDataFmt()),
                static_cast<unsigned>(buffer.GetNumberFmt()), host_format,
                static_cast<unsigned>(dst.r), static_cast<unsigned>(dst.g),
                static_cast<unsigned>(dst.b), static_cast<unsigned>(dst.a), divisor,
                gpu_modified ? 1u : 0u, cpu_modified ? 1u : 0u,
                fingerprint.readable ? 1u : 0u,
                static_cast<unsigned long long>(fingerprint.digest));
            if (gpu_modified) {
                buffer_cache.ExecutorProbeGpuBuffer(
                    static_cast<VAddr>(buffer.base_address), static_cast<u32>(size),
                    "vertex", executor_gpu_probe_draw);
            }
        }
        for (u32 i = 0; i < probe_vs_info.buffers.size(); ++i) {
            const auto& desc = probe_vs_info.buffers[i];
            const auto buffer = desc.GetSharp(probe_vs_info);
            const bool special = desc.IsSpecial();
            const VAddr address = static_cast<VAddr>(buffer.base_address);
            const u64 size = special ? 0 : buffer.GetSize();
            const auto fingerprint = ExecutorFingerprintGuestRange(memory, address, size);
            const bool gpu_modified = size != 0 && buffer_cache.IsRegionGpuModified(address, size);
            const bool cpu_modified = size != 0 && buffer_cache.IsRegionCpuModified(address, size);
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_VS_BUFFER_INPUT] seq=%llu slot=%u sharpIdx=%u type=%u "
                "special=%u storage=%u written=%u formatted=%u guest=0x%llx size=%llu "
                "clamped=%llu records=%u stride=%u dataFmt=%u numFmt=%u gpuDirty=%u "
                "cpuDirty=%u readable=%u sampleDigest=0x%llx",
                static_cast<unsigned long long>(executor_gpu_probe_draw), i,
                static_cast<unsigned>(desc.sharp_idx), static_cast<unsigned>(desc.buffer_type),
                special ? 1u : 0u, desc.IsStorage(buffer) ? 1u : 0u,
                desc.is_written ? 1u : 0u, desc.is_formatted ? 1u : 0u,
                static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(fingerprint.clamped_size), buffer.num_records,
                buffer.GetStride(), static_cast<unsigned>(buffer.GetDataFmt()),
                static_cast<unsigned>(buffer.GetNumberFmt()), gpu_modified ? 1u : 0u,
                cpu_modified ? 1u : 0u, fingerprint.readable ? 1u : 0u,
                static_cast<unsigned long long>(fingerprint.digest));
            if (gpu_modified) {
                buffer_cache.ExecutorProbeGpuBuffer(address, static_cast<u32>(size),
                                                    "vs_buffer", executor_gpu_probe_draw);
            }
        }
        if (is_indexed) {
            const bool index16 =
                regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
            const u32 index_size = index16 ? sizeof(u16) : sizeof(u32);
            const VAddr index_address = regs.index_base_address.Address<VAddr>() +
                                        static_cast<u64>(index_offset) * index_size;
            const u64 index_bytes = static_cast<u64>(regs.num_indices) * index_size;
            const auto fingerprint =
                ExecutorFingerprintGuestRange(memory, index_address, index_bytes);
            const bool gpu_modified = index_bytes != 0 &&
                                      buffer_cache.IsRegionGpuModified(index_address, index_bytes);
            const bool cpu_modified = index_bytes != 0 &&
                                      buffer_cache.IsRegionCpuModified(index_address, index_bytes);
            u32 min_index = ~0u;
            u32 max_index = 0;
            const u32 scan_count = static_cast<u32>(std::min<u64>(
                regs.num_indices, fingerprint.clamped_size / index_size));
            if (fingerprint.readable) {
                for (u32 i = 0; i < scan_count; ++i) {
                    u32 value{};
                    if (index16) {
                        u16 value16{};
                        std::memcpy(&value16,
                                    reinterpret_cast<const void*>(index_address +
                                                                  static_cast<u64>(i) * 2),
                                    sizeof(value16));
                        value = value16;
                    } else {
                        std::memcpy(&value,
                                    reinterpret_cast<const void*>(index_address +
                                                                  static_cast<u64>(i) * 4),
                                    sizeof(value));
                    }
                    min_index = std::min(min_index, value);
                    max_index = std::max(max_index, value);
                }
            }
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_INDEX_INPUT] seq=%llu guest=0x%llx bytes=%llu clamped=%llu "
                "type=%u indexOffset=%u count=%u scanned=%u min=%u max=%u gpuDirty=%u "
                "cpuDirty=%u readable=%u sampleDigest=0x%llx",
                static_cast<unsigned long long>(executor_gpu_probe_draw),
                static_cast<unsigned long long>(index_address),
                static_cast<unsigned long long>(index_bytes),
                static_cast<unsigned long long>(fingerprint.clamped_size), index16 ? 16u : 32u,
                index_offset, regs.num_indices, scan_count,
                scan_count == 0 ? 0u : min_index, max_index, gpu_modified ? 1u : 0u,
                cpu_modified ? 1u : 0u, fingerprint.readable ? 1u : 0u,
                static_cast<unsigned long long>(fingerprint.digest));
        }
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_DRAW_STATE] seq=%llu fs=0x%llx tcs=0x%llx tes=0x%llx "
            "vs=0x%llx gs=0x%llx indexed=%u numIdx=%u "
            "instances=%u psMrtMask=0x%x keyMrtMask=0x%x keyWriteMask0=0x%x ctMask0=0x%x "
            "discard=%u colorMode=%u colorCtlRaw=0x%x rop3=%u keyLogicOp=%u "
            "blend0=%u blendRaw0=0x%x disableRop0=%u src=%u dst=%u "
            "vp=%.2f,%.2f,%.1f,%.1f "
            "scissor=%d,%d-%d,%d cb0=0x%llx idxReg=%u voff=%u ioff=%u prim=%u "
            "polyRaw=0x%x cull=%u front=%u depthRaw=0x%x depthEn=%u depthWrite=%u depthFunc=%u "
            "stageEnable=0x%x hsInCp=%u hsOutCp=%u rasterKill=%u keyDiscard=%u clipDis=%u "
            "vpCtl=%u%u%u%u gen=%u,%u-%u,%u win=%u,%u-%u,%u winOff=%d,%d offDis=%u "
            "vpscEn=%u vpsc0=%u,%u-%u,%u restart=%u/%u restartIdx=0x%x "
            "listRestart=%u patchRestart=%u robustBuf2=%u robustImg2=%u nullDesc=%u "
            "writes=%zu buffers=%zu images=%zu feedback=%u/%u depthUnrestricted=%u "
            "clipSpace=%u zCtl=%u%u zRaw=%.6f,%.6f zEffective=%.6f,%.6f "
            "zBounds=%.6f,%.6f "
            "gsMode=0x%x gsScenario=%u cut=%u onchip=%u esPass=%u suVtxRaw=0x%x",
            static_cast<unsigned long long>(executor_gpu_probe_draw),
            static_cast<unsigned long long>(executor_gpu_probe_fragment_hash),
            static_cast<unsigned long long>(
                probe_stage_hash(Shader::LogicalStage::TessellationControl)),
            static_cast<unsigned long long>(
                probe_stage_hash(Shader::LogicalStage::TessellationEval)),
            static_cast<unsigned long long>(probe_stage_hash(Shader::LogicalStage::Vertex)),
            static_cast<unsigned long long>(probe_stage_hash(Shader::LogicalStage::Geometry)),
            is_indexed ? 1u : 0u, regs.num_indices, regs.num_instances.NumInstances(),
            executor_gpu_probe_fragment ? executor_gpu_probe_fragment->mrt_mask : 0u,
            probe_key.mrt_mask, static_cast<unsigned>(probe_key.write_masks[0]),
            regs.color_target_mask.GetMask(0),
            executor_gpu_probe_fragment && executor_gpu_probe_fragment->has_discard ? 1u : 0u,
            static_cast<unsigned>(regs.color_control.mode),
            std::bit_cast<u32>(regs.color_control),
            static_cast<unsigned>(regs.color_control.rop3),
            static_cast<unsigned>(probe_key.logic_op),
            static_cast<unsigned>(regs.blend_control[0].enable),
            std::bit_cast<u32>(regs.blend_control[0]),
            static_cast<unsigned>(regs.blend_control[0].disable_rop3),
            static_cast<unsigned>(regs.blend_control[0].color_src_factor),
            static_cast<unsigned>(regs.blend_control[0].color_dst_factor),
            regs.viewports[0].xscale, regs.viewports[0].yscale, regs.viewports[0].xoffset,
            regs.viewports[0].yoffset, static_cast<int>(regs.screen_scissor.top_left_x),
            static_cast<int>(regs.screen_scissor.top_left_y),
            static_cast<int>(regs.screen_scissor.bottom_right_x),
            static_cast<int>(regs.screen_scissor.bottom_right_y),
            static_cast<unsigned long long>(regs.color_buffers[0].Address()),
            regs.index_offset, probe_vertex_offset, probe_instance_offset,
            static_cast<unsigned>(regs.primitive_type),
            std::bit_cast<u32>(regs.polygon_control),
            static_cast<unsigned>(regs.polygon_control.CullingMode()),
            static_cast<unsigned>(regs.polygon_control.front_face),
            std::bit_cast<u32>(regs.depth_control),
            static_cast<unsigned>(regs.depth_control.depth_enable),
            static_cast<unsigned>(regs.depth_control.depth_write_enable),
            static_cast<unsigned>(regs.depth_control.depth_func),
            static_cast<unsigned>(regs.stage_enable.raw),
            static_cast<unsigned>(regs.ls_hs_config.hs_input_control_points),
            static_cast<unsigned>(regs.ls_hs_config.hs_output_control_points),
            static_cast<unsigned>(regs.clipper_control.dx_rasterization_kill),
            static_cast<unsigned>(probe_key.rasterizer_discard_enable),
            static_cast<unsigned>(regs.clipper_control.clip_disable),
            static_cast<unsigned>(regs.viewport_control.xscale_enable),
            static_cast<unsigned>(regs.viewport_control.xoffset_enable),
            static_cast<unsigned>(regs.viewport_control.yscale_enable),
            static_cast<unsigned>(regs.viewport_control.yoffset_enable),
            static_cast<unsigned>(regs.generic_scissor.top_left_x),
            static_cast<unsigned>(regs.generic_scissor.top_left_y),
            static_cast<unsigned>(regs.generic_scissor.bottom_right_x),
            static_cast<unsigned>(regs.generic_scissor.bottom_right_y),
            static_cast<unsigned>(regs.window_scissor.top_left_x),
            static_cast<unsigned>(regs.window_scissor.top_left_y),
            static_cast<unsigned>(regs.window_scissor.bottom_right_x),
            static_cast<unsigned>(regs.window_scissor.bottom_right_y),
            static_cast<int>(regs.window_offset.window_x_offset),
            static_cast<int>(regs.window_offset.window_y_offset),
            static_cast<unsigned>(regs.window_scissor.window_offset_disable),
            static_cast<unsigned>(regs.mode_control.vport_scissor_enable),
            static_cast<unsigned>(regs.viewport_scissors[0].top_left_x),
            static_cast<unsigned>(regs.viewport_scissors[0].top_left_y),
            static_cast<unsigned>(regs.viewport_scissors[0].bottom_right_x),
            static_cast<unsigned>(regs.viewport_scissors[0].bottom_right_y),
            probe_restart_raw ? 1u : 0u, probe_restart_effective ? 1u : 0u,
            regs.primitive_restart_index, instance.IsListRestartSupported() ? 1u : 0u,
            instance.IsPatchListRestartSupported() ? 1u : 0u,
            instance.IsRobustBufferAccess2Supported() ? 1u : 0u,
            instance.IsRobustImageAccess2Supported() ? 1u : 0u,
            instance.IsNullDescriptorSupported() ? 1u : 0u, set_writes.size(), buffer_infos.size(),
            image_infos.size(), attachment_feedback_loop ? 1u : 0u,
            instance.IsAttachmentFeedbackLoopLayoutSupported() ? 1u : 0u,
            instance.IsDepthRangeUnrestrictedSupported() ? 1u : 0u,
            static_cast<unsigned>(regs.clipper_control.clip_space),
            static_cast<unsigned>(probe_viewport_control.zscale_enable),
            static_cast<unsigned>(probe_viewport_control.zoffset_enable), probe_min_depth,
            probe_max_depth, probe_effective_min_depth, probe_effective_max_depth,
            probe_depth_bounds.zmin, probe_depth_bounds.zmax,
            std::bit_cast<u32>(regs.vgt_gs_mode),
            static_cast<unsigned>(regs.vgt_gs_mode.mode), regs.vgt_gs_mode.cut_mode,
            regs.vgt_gs_mode.onchip, regs.vgt_gs_mode.es_passthru, regs.reg_array[0xA2F9]);
        const auto log_probe_image = [&](const char* role, VideoCore::ImageId image_id,
                                         u64 guest_address) {
            if (!image_id) {
                __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                    "[EXECUTOR_GPU_ATTACHMENT] seq=%llu role=%s imageId=0 "
                                    "guest=0x%llx",
                                    static_cast<unsigned long long>(executor_gpu_probe_draw), role,
                                    static_cast<unsigned long long>(guest_address));
                return;
            }
            const auto& image = texture_cache.GetImage(image_id);
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_ATTACHMENT] seq=%llu role=%s imageId=%u guest=0x%llx "
                "tracked=0x%llx format=%s extent=%ux%ux%u pitch=%u layers=%u bits=%u "
                "samples=%u layout=%u access=0x%llx depth=%u stencil=%u",
                static_cast<unsigned long long>(executor_gpu_probe_draw), role, image_id.index,
                static_cast<unsigned long long>(guest_address),
                static_cast<unsigned long long>(image.info.guest_address),
                vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
                image.info.size.height, image.info.size.depth, image.info.pitch,
                image.info.resources.layers, image.info.num_bits, image.info.num_samples,
                static_cast<u32>(image.backing->state.layout),
                static_cast<unsigned long long>(
                    static_cast<VkAccessFlags2>(image.backing->state.access_mask)),
                image.info.props.is_depth ? 1u : 0u,
                image.info.props.has_stencil ? 1u : 0u);
        };
        static constexpr std::array<const char*, AmdGpu::NUM_COLOR_BUFFERS> color_roles{
            "color0", "color1", "color2", "color3", "color4", "color5", "color6", "color7"};
        for (u32 color_index = 0; color_index < cb_descs.size(); ++color_index) {
            log_probe_image(color_roles[color_index], cb_descs[color_index].first,
                            regs.color_buffers[color_index].Address());
            const auto& color_key = probe_key.color_buffers[color_index];
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_MRT_STATE] seq=%llu index=%u bound=%u guest=0x%llx "
                "dataFmt=%u numFmt=%u conversion=%u exportFmt=%u writeMask=0x%x "
                "targetMask=0x%x samples=%u",
                static_cast<unsigned long long>(executor_gpu_probe_draw), color_index,
                cb_descs[color_index].first ? 1u : 0u,
                static_cast<unsigned long long>(regs.color_buffers[color_index].Address()),
                static_cast<unsigned>(color_key.data_format),
                static_cast<unsigned>(color_key.num_format),
                static_cast<unsigned>(color_key.num_conversion),
                static_cast<unsigned>(color_key.export_format),
                static_cast<unsigned>(probe_key.write_masks[color_index]),
                regs.color_target_mask.GetMask(color_index), probe_key.color_samples[color_index]);
        }
        log_probe_image("depth", db_desc.first, regs.depth_buffer.DepthAddress());
        for (const auto* probe_stage : pipeline->GetStages()) {
            if (!probe_stage) {
                continue;
            }
            Common::ContentFingerprint64 flat_fingerprint{
                Common::FingerprintDomain::ShaderUserData};
            u32 flat_nonzero = 0;
            const auto flat = probe_stage->FlattenedUserData();
            for (const u32 value : flat) {
                flat_fingerprint.UpdateLittleEndian(value);
                flat_nonzero += value != 0;
            }
            const u64 flat_digest = flat_fingerprint.Finish();
            const auto flat_at = [&](size_t index) {
                return index < flat.size() ? flat[index] : 0u;
            };
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_STAGE_SRT] seq=%llu stage=%u logical=%u hash=0x%llx "
                "flatDw=%zu flatNonzero=%u digest=0x%llx nodes=%zu copies=%zu "
                "flat0_15=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,"
                "%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                static_cast<unsigned long long>(executor_gpu_probe_draw),
                static_cast<unsigned>(probe_stage->stage),
                static_cast<unsigned>(probe_stage->l_stage),
                static_cast<unsigned long long>(probe_stage->pgm_hash), flat.size(), flat_nonzero,
                static_cast<unsigned long long>(flat_digest),
                probe_stage->srt_info.portable_nodes.size(),
                probe_stage->srt_info.portable_copies.size(), flat_at(0), flat_at(1), flat_at(2),
                flat_at(3), flat_at(4), flat_at(5), flat_at(6), flat_at(7), flat_at(8), flat_at(9),
                flat_at(10), flat_at(11), flat_at(12), flat_at(13), flat_at(14), flat_at(15));
        }
        if (executor_gpu_probe_fragment) {
            for (u32 image_index = 0;
                 image_index < executor_gpu_probe_fragment->images.size(); ++image_index) {
                const auto& image_desc = executor_gpu_probe_fragment->images[image_index];
                const auto image = image_desc.GetSharp(*executor_gpu_probe_fragment);
                const auto swizzle = image.DstSelect();
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_GPU_IMAGE_STATE] seq=%llu fs=0x%llx slot=%u sharp=%u "
                    "guest=0x%llx type=%u extent=%ux%ux%u pitch=%u dataFmt=%u numFmt=%u "
                    "dst=%u,%u,%u,%u levels=%u..%u array=%u..%u depth=%u written=%u "
                    "isDepth=%u",
                    static_cast<unsigned long long>(executor_gpu_probe_draw),
                    static_cast<unsigned long long>(executor_gpu_probe_fragment_hash),
                    image_index, image_desc.sharp_idx,
                    static_cast<unsigned long long>(image.Address()),
                    static_cast<unsigned>(image.GetType()), static_cast<unsigned>(image.width + 1),
                    static_cast<unsigned>(image.height + 1),
                    static_cast<unsigned>(image.depth + 1), image.Pitch(),
                    static_cast<unsigned>(image.GetDataFmt()),
                    static_cast<unsigned>(image.GetNumberFmt()),
                    static_cast<unsigned>(swizzle.r), static_cast<unsigned>(swizzle.g),
                    static_cast<unsigned>(swizzle.b), static_cast<unsigned>(swizzle.a),
                    static_cast<unsigned>(image.base_level),
                    static_cast<unsigned>(image.last_level),
                    static_cast<unsigned>(image.base_array),
                    static_cast<unsigned>(image.last_array),
                    static_cast<unsigned>(image.depth), image_desc.is_written ? 1u : 0u,
                    image_desc.is_depth ? 1u : 0u);
            }
            for (u32 sampler_index = 0;
                 sampler_index < executor_gpu_probe_fragment->samplers.size(); ++sampler_index) {
                const auto& sampler_desc = executor_gpu_probe_fragment->samplers[sampler_index];
                const auto sampler = sampler_desc.GetSharp(*executor_gpu_probe_fragment);
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_GPU_SAMPLER_STATE] seq=%llu fs=0x%llx slot=%u sharp=%u "
                    "assocImage=%u raw0=0x%llx raw1=0x%llx clamp=%u,%u,%u filter=%u,%u,%u "
                    "forceUnnorm=%u",
                    static_cast<unsigned long long>(executor_gpu_probe_draw),
                    static_cast<unsigned long long>(executor_gpu_probe_fragment_hash),
                    sampler_index, sampler_desc.sharp_idx, sampler_desc.associated_image,
                    static_cast<unsigned long long>(sampler.raw0),
                    static_cast<unsigned long long>(sampler.raw1),
                    static_cast<unsigned>(sampler.clamp_x.Value()),
                    static_cast<unsigned>(sampler.clamp_y.Value()),
                    static_cast<unsigned>(sampler.clamp_z.Value()),
                    static_cast<unsigned>(sampler.xy_mag_filter.Value()),
                    static_cast<unsigned>(sampler.xy_min_filter.Value()),
                    static_cast<unsigned>(sampler.mip_filter.Value()),
                    static_cast<unsigned>(sampler.force_unnormalized.Value()));
            }
        }
#endif
        if (!executor_gpu_probe_late) {
            for (const auto& color_desc : cb_descs) {
                if (!color_desc.first) {
                    continue;
                }
                texture_cache.ExecutorGpuContentProbe(
                    color_desc.first,
                    VideoCore::TextureCache::ExecutorGpuContentRole::ColorBeforeDraw,
                    static_cast<u32>(Shader::LogicalStage::Fragment),
                    executor_gpu_probe_fragment_hash, executor_gpu_probe_draw);
            }
            if (db_desc.first) {
                texture_cache.ExecutorGpuContentProbe(
                    db_desc.first,
                    VideoCore::TextureCache::ExecutorGpuContentRole::DepthBeforeDraw,
                    static_cast<u32>(Shader::LogicalStage::Fragment),
                    executor_gpu_probe_fragment_hash, executor_gpu_probe_draw);
            }
        }
    }

    draw_stage_profile.Begin(ExecutorDrawCpuStage::DescriptorBind);
    buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream).RetainCurrentAllocation();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);
    draw_stage_profile.End(ExecutorDrawCpuStage::DescriptorBind);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_post_bind_dynamic =
        ExecutorNeedsQualcommPostBindDynamicState(instance);
    if (!executor_post_bind_dynamic) {
        UpdateDynamicState(pipeline, is_indexed);
    }
#else
    UpdateDynamicState(pipeline, is_indexed);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_gpu_probe_selected && executor_gpu_probe_late) {
        ExecutorGpuLatePassArm(executor_gpu_probe_attachment_signature,
                               executor_gpu_probe_draw, executor_gpu_probe_fragment_hash);
    }
#endif
    // All draw-owned uploads were published above. Disjoint HLE copies may
    // remain queued until a real range dependency or submit boundary, but can
    // no longer end this scope while the draw is being bound. Preserve the
    // proven Qualcomm ordering for every ordinary draw.
    const bool executor_late_render_scope =
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        executor_physical_drs_selected;
#else
        false;
#endif
    if (!executor_late_render_scope) {
        draw_stage_profile.Begin(ExecutorDrawCpuStage::BeginRendering);
        scheduler.BeginRendering(state);
        draw_stage_profile.End(ExecutorDrawCpuStage::BeginRendering);
    }
    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool kForceRectListFirstVertexZero =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-force-rectlist-first-vertex-zero",
                 F_OK) == 0;
    if (kForceRectListFirstVertexZero && !is_indexed &&
        regs.primitive_type == AmdGpu::PrimitiveType::RectList && vertex_offset != 0) {
        static std::atomic<u32> forced_count{0};
        const u32 count = forced_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 32) {
            __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                                "[EXECUTOR_RECTLIST_FIRST_VERTEX_AB] n=%u raw=%u effective=0 "
                                "indexReg=%u",
                                count, vertex_offset, regs.index_offset);
        }
        vertex_offset = 0;
    }
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_physical_drs_selected) {
        std::scoped_lock drs_lock{executor_physical_drs_mutex};
        if (!executor_physical_drs_draw_active) {
            // Resource synchronization can resolve a scaled target while the
            // draw is being prepared. Never submit the already-built scratch
            // attachment state after that resolve; the following guest draw
            // will rebuild against the canonical target.
            ResetBindings();
            return;
        }
    }
#endif
    if (executor_late_render_scope) {
        // Record the render scope only after every operation that can flush
        // transfers or resolve a physical DRS target.
        draw_stage_profile.Begin(ExecutorDrawCpuStage::BeginRendering);
        scheduler.BeginRendering(state);
        draw_stage_profile.End(ExecutorDrawCpuStage::BeginRendering);
    }
    if (!scheduler.HasRecordingCommandBuffer()) {
        ResetBindings();
        return;
    }
    draw_stage_profile.Begin(ExecutorDrawCpuStage::CommandBind);
    const auto cmdbuf = scheduler.CommandBuffer();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorBindGuestGraphicsPipeline(pipeline, cmdbuf);
#else
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eGraphics);
#endif
    buffer_cache.BindPreparedVertexBuffers(prepared_vertex_buffers);
    if (is_indexed) {
        buffer_cache.BindPreparedIndexBuffer(prepared_index_buffer);
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_post_bind_dynamic) {
        UpdateDynamicState(pipeline, is_indexed);
    }
    ExecutorSealGuestGraphicsPipelineState(pipeline, cmdbuf);
#endif
    EXEC_DRAW_TRACE("before_draw_cmd");
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_depth_write_oracle_clear =
        executor_gpu_probe_selected && executor_gpu_probe_late && db_desc.first &&
        ExecutorClaimDepthWriteOracle(executor_gpu_probe_attachment_signature);
    const bool executor_depth_write_oracle =
        db_desc.first && ExecutorDepthWriteOracleActive(executor_gpu_probe_attachment_signature);
    if (executor_depth_write_oracle_clear) {
        vk::ClearValue clear_value{};
        clear_value.depthStencil = vk::ClearDepthStencilValue{0.25f, 0};
        const std::array clear_attachments{
            vk::ClearAttachment{vk::ImageAspectFlagBits::eDepth, 0, clear_value}};
        const std::array clear_rects{vk::ClearRect{
            vk::Rect2D{vk::Offset2D{0, 0}, vk::Extent2D{state.width, state.height}}, 0,
            state.num_layers}};
        cmdbuf.clearAttachments(clear_attachments, clear_rects);
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_DEPTH_WRITE_ORACLE] phase=armed epoch=%llu draw=%llu imageId=%u "
            "attach=0x%llx sentinel=0.25 extent=%ux%u layers=%u "
            "stateOrder=after_bind compare=always write=1 scope=complete_pass",
            static_cast<unsigned long long>(AmdGpu::RenderWaveTrace::LateFrameCaptureEpoch()),
            static_cast<unsigned long long>(executor_gpu_probe_draw), db_desc.first.index,
            static_cast<unsigned long long>(executor_gpu_probe_attachment_signature), state.width,
            state.height, state.num_layers);
    }
    if (executor_depth_write_oracle) {
        cmdbuf.setDepthTestEnable(true);
        cmdbuf.setDepthWriteEnable(true);
        cmdbuf.setDepthCompareOp(vk::CompareOp::eAlways);
        cmdbuf.setStencilTestEnable(false);
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(false);
        }
        cmdbuf.setDepthBiasEnable(false);
        // The oracle deliberately overwrites guest dynamic state. Publish the
        // invalidation before any diagnostic early return so a skipped draw
        // cannot leave the tracker authoritative for state that is no longer
        // in the command buffer.
        scheduler.GetDynamicState().Invalidate();
        static std::atomic<u32> replay_logs{0};
        const u32 replay_log = replay_logs.fetch_add(1, std::memory_order_relaxed);
        if (replay_log < 4) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_DEPTH_WRITE_ORACLE] phase=post_bind_replay epoch=%llu draw=%llu "
                "attach=0x%llx ordinal=%u",
                static_cast<unsigned long long>(AmdGpu::RenderWaveTrace::LateFrameCaptureEpoch()),
                static_cast<unsigned long long>(executor_gpu_probe_draw),
                static_cast<unsigned long long>(executor_gpu_probe_attachment_signature),
                replay_log + 1);
        }
    }
    if (executor_draw_diag) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_DRAW_STATE] mode=%s idx=%d numIdx=%u inst=%u colorMode=%u tmask0=%u depthValid=%d "
            "vp xs=%.2f ys=%.2f xo=%.1f yo=%.1f voff=%d scis=[%d,%d-%d,%d] gscis=[%d,%d-%d,%d] "
            "blend0 en=%u src=%u dst=%u dctl en=%u zfunc=%u",
            Libraries::GnmDriver::ExecutorReplayActive() ? "replay" : "live",
            is_indexed ? 1 : 0, regs.num_indices, regs.num_instances.NumInstances(),
            (unsigned)regs.color_control.mode, regs.color_target_mask.GetMask(0),
            regs.depth_buffer.DepthValid() ? 1 : 0, regs.viewports[0].xscale, regs.viewports[0].yscale,
            regs.viewports[0].xoffset, regs.viewports[0].yoffset, (int)vertex_offset,
            (int)regs.screen_scissor.top_left_x, (int)regs.screen_scissor.top_left_y,
            (int)regs.screen_scissor.bottom_right_x, (int)regs.screen_scissor.bottom_right_y,
            (int)regs.generic_scissor.top_left_x, (int)regs.generic_scissor.top_left_y,
            (int)regs.generic_scissor.bottom_right_x, (int)regs.generic_scissor.bottom_right_y,
            (unsigned)regs.blend_control[0].enable, (unsigned)regs.blend_control[0].color_src_factor,
            (unsigned)regs.blend_control[0].color_dst_factor, (unsigned)regs.depth_control.depth_enable,
            (unsigned)regs.depth_control.depth_func);
        const auto& gkey = pipeline->GetGraphicsKey();
        const auto* ps_info = pipeline->TryGetStage(Shader::LogicalStage::Fragment);
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_PS_INFO] mode=%s psMrtMask=0x%x hasDiscard=%d keyMrtMask=0x%x "
                            "wmask0=0x%x ctMask0=0x%x cb0base=0x%llx",
                            Libraries::GnmDriver::ExecutorReplayActive() ? "replay" : "live",
                            ps_info ? (unsigned)ps_info->mrt_mask : 0u,
                            ps_info && ps_info->has_discard ? 1 : 0,
                            (unsigned)gkey.mrt_mask, (unsigned)gkey.write_masks[0],
                            (unsigned)regs.color_target_mask.GetMask(0),
                            (unsigned long long)regs.color_buffers[0].Address());
    }
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (kBisectDrawLevel == 4) {
        ResetBindings();
        return;
    }
    static const u64 kBisectDrawMax = [] {
        const char* value = std::getenv("EXECUTOR_BISECT_DRAW_MAX");
        return value ? std::strtoull(value, nullptr, 10) : 0ull;
    }();
    if (kBisectDrawMax != 0) {
        static std::atomic<u64> bisect_draw_sequence{0};
        const u64 bisect_draw_sequence_value =
            bisect_draw_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (bisect_draw_sequence_value > kBisectDrawMax) {
            ResetBindings();
            return;
        }
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_BISECT_DRAW_MAX] execute=%llu max=%llu pipe=0x%llx "
                            "vs=0x%llx ps=0x%llx elements=%u cb0=0x%llx",
                            static_cast<unsigned long long>(bisect_draw_sequence_value),
                            static_cast<unsigned long long>(kBisectDrawMax),
                            static_cast<unsigned long long>(pipeline->GetPipelineHash()),
                            static_cast<unsigned long long>(pipeline->GetGraphicsKey().stage_hashes[
                                u32(Shader::LogicalStage::Vertex)]),
                            static_cast<unsigned long long>(pipeline->GetGraphicsKey().stage_hashes[
                                u32(Shader::LogicalStage::Fragment)]),
                            regs.num_indices,
                            static_cast<unsigned long long>(regs.color_buffers[0].Address()));
    }
#endif
    draw_stage_profile.End(ExecutorDrawCpuStage::CommandBind);
    draw_stage_profile.Begin(ExecutorDrawCpuStage::DrawEmit);
    if (Config::getVkHostMarkersEnabled()) {
        // Keep the failing GPU command identifiable without logging guest shader
        // source. This is disabled in ordinary runs, including performance tests.
        const auto& marker_key = pipeline->GetGraphicsKey();
        ScopedMarkerInsert(fmt::format(
            "Draw:pipe={:#x}:vs={:#x}:ps={:#x}:indexed={}:count={}:instances={}:cb0={:#x}:pass={}x{}x{}:colors={}:depth={}:stencil={}",
            pipeline->GetPipelineHash(),
            marker_key.stage_hashes[u32(Shader::LogicalStage::Vertex)],
            marker_key.stage_hashes[u32(Shader::LogicalStage::Fragment)], is_indexed,
            regs.num_indices, regs.num_instances.NumInstances(), regs.color_buffers[0].Address(),
            state.width, state.height, state.num_layers,
            std::count_if(state.color_attachments.begin(), state.color_attachments.end(),
                          [](const auto& attachment) { return bool(attachment.image_view); }),
            state.depth_stencil_attachment.has_depth, state.depth_stencil_attachment.has_stencil));
    }
    if (quad_tri_list) {
        buffer_cache.BindPreparedIndexBuffer(VideoCore::BufferCache::PreparedIndexBuffer{
            .buffer = quad_index_buffer,
            .offset = quad_index_offset,
            .type = vk::IndexType::eUint16,
        });
        cmdbuf.drawIndexed(6, regs.num_instances.NumInstances(), 0, s32(vertex_offset),
                           instance_offset);
    } else if (is_indexed) {
        cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }
    PortableGpu::CommitProducerBindings(&buffer_cache,&scheduler,scheduler.CurrentTick());
    draw_stage_profile.End(ExecutorDrawCpuStage::DrawEmit);
    draw_stage_profile.MarkEmitted();
    draw_stage_profile.Begin(ExecutorDrawCpuStage::PostDraw);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
#endif
    if (executor_gpu_probe_selected && !executor_gpu_probe_late) {
        for (const auto& color_desc : cb_descs) {
            if (!color_desc.first) {
                continue;
            }
            texture_cache.ExecutorGpuContentProbe(
                color_desc.first, VideoCore::TextureCache::ExecutorGpuContentRole::ColorAfterDraw,
                static_cast<u32>(Shader::LogicalStage::Fragment),
                executor_gpu_probe_fragment_hash, executor_gpu_probe_draw);
        }
    }
    if (executor_gpu_probe_selected && !executor_gpu_probe_late && db_desc.first) {
        texture_cache.ExecutorGpuContentProbe(
            db_desc.first, VideoCore::TextureCache::ExecutorGpuContentRole::DepthAfterDraw,
            static_cast<u32>(Shader::LogicalStage::Fragment), executor_gpu_probe_fragment_hash,
            executor_gpu_probe_draw);
    }
    if (executor_gpu_probe_selected && !executor_gpu_probe_late) {
        u32 sampled_probe_count = 0;
        for (const auto& probe : executor_gpu_probe_sampled_images) {
            if (sampled_probe_count++ >= 8) {
                break;
            }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            const auto& probe_image = texture_cache.GetImage(probe.image_id);
            const auto& mapping = probe.view_info.mapping;
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_IMAGE_VIEW_STATE] seq=%llu stage=%u hash=0x%llx imageId=%u "
                "imageFormat=%s requestedViewFormat=%s effectiveViewFormat=%s viewType=%u "
                "mapping=%u,%u,%u,%u mip=%u+%u layer=%u+%u storage=%u aspect=0x%x "
                "vkImage=0x%llx vkImageView=0x%llx layout=%u",
                static_cast<unsigned long long>(executor_gpu_probe_draw), probe.stage,
                static_cast<unsigned long long>(probe.stage_hash), probe.image_id.index,
                vk::to_string(probe.image_format).c_str(),
                vk::to_string(probe.view_info.format).c_str(),
                vk::to_string(probe.effective_view_format).c_str(),
                static_cast<unsigned>(probe.view_info.type),
                static_cast<unsigned>(mapping.r), static_cast<unsigned>(mapping.g),
                static_cast<unsigned>(mapping.b), static_cast<unsigned>(mapping.a),
                probe.view_info.range.base.level, probe.view_info.range.extent.levels,
                probe.view_info.range.base.layer, probe.view_info.range.extent.layers,
                probe.view_info.is_storage ? 1u : 0u,
                static_cast<unsigned>(static_cast<VkImageAspectFlags>(probe.image_aspect)),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(
                    static_cast<VkImage>(probe_image.GetImage()))),
                static_cast<unsigned long long>(probe.image_view_handle),
                static_cast<unsigned>(probe_image.backing->state.layout));
#endif
            texture_cache.ExecutorGpuContentProbe(
                probe.image_id,
                VideoCore::TextureCache::ExecutorGpuContentRole::SampledBeforeDraw,
                probe.stage, probe.stage_hash, executor_gpu_probe_draw);
        }
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!Config::managedGpuFastPath() || ExecutorVkJournalRequested()) {
        const auto& key = pipeline->GetGraphicsKey();
        const auto& color = cb_descs[0].second.info;
        const u64 image_view0 =
            image_infos.empty()
                ? 0
                : reinterpret_cast<u64>(
                      static_cast<VkImageView>(image_infos[0].imageView));
        const u64 image_view1 =
            image_infos.size() < 2
                ? 0
                : reinterpret_cast<u64>(
                      static_cast<VkImageView>(image_infos[1].imageView));
        const u64 journal_sequence = ExecutorVkJournalGraphicsDraw(
            &scheduler, scheduler.CurrentTick(), pipeline->GetPipelineHash(),
            ExecutorVkPipelineHandle(pipeline->Handle()),
            key.stage_hashes[u32(Shader::LogicalStage::Vertex)],
            key.stage_hashes[u32(Shader::LogicalStage::Fragment)],
            is_indexed ? ExecutorVkJournalDrawType::IndexedDirect
                       : ExecutorVkJournalDrawType::Direct,
            static_cast<u32>(key.prim_type), regs.num_indices,
            regs.num_instances.NumInstances(), 0, 0,
            image_view0, image_view1,
            cb_descs[0].first ? color.guest_address : 0,
            executor_gpu_probe_attachments.depth_address,
            executor_gpu_probe_attachments.signature,
            executor_gpu_probe_attachments.color_count,
            cb_descs[0].first ? color.size.width : 0, cb_descs[0].first ? color.size.height : 0,
            static_cast<u32>(pipeline->GetColorFormat0()),
            static_cast<u32>(pipeline->GetDepthFormat()),
            static_cast<u32>(set_writes.size()), static_cast<u32>(buffer_infos.size()),
            static_cast<u32>(image_infos.size()), executor_gpu_probe_enabled);
        scheduler.RecordGpuFaultDrawCheckpoint(
            journal_sequence, pipeline->GetPipelineHash(), regs.num_indices,
            is_indexed ? ExecutorVkJournalDrawType::IndexedDirect
                       : ExecutorVkJournalDrawType::Direct,
            static_cast<u32>(key.prim_type));
        if (executor_gpu_probe_selected) {
            ExecutorVkJournalArmContentProbe(&scheduler, journal_sequence,
                                             executor_gpu_probe_draw,
                                             executor_gpu_probe_fragment_hash);
        }
    }
    ++executor_unpublished_actual_draws;
    {
        const auto& key = pipeline->GetGraphicsKey();
        AmdGpu::RenderWaveTrace::RasterDraw(
            regs, key.stage_hashes[u32(Shader::LogicalStage::Vertex)],
            key.stage_hashes[u32(Shader::LogicalStage::Fragment)], pipeline->GetPipelineHash(),
            is_indexed, false);
    }
    if (!Config::managedGpuFastPath() || ExecutorVkJournalRequested()) {
        ExecutorTraceDrawnColorTarget(cb_descs[0].first ? &cb_descs[0].second : nullptr);
    }
    if (cb_image_uids[0] != 0) {
        texture_cache.ExecutorTrackLiveDrawnColorTarget(cb_descs[0].first,
                                                        cb_image_uids[0]);
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (trace_actual_draw) {
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_VK_ACTUAL_DRAW] phase=vk_cmd_draw indexed=%u numIdx=%u "
                            "instances=%u vertexOffset=%u instanceOffset=%u",
                            is_indexed ? 1u : 0u, regs.num_indices,
                            regs.num_instances.NumInstances(), vertex_offset, instance_offset);
    }
#endif
    ResetBindings();
    draw_stage_profile.End(ExecutorDrawCpuStage::PostDraw);
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    PortablePublishNaturalBoundary(*this);
    ClaudeRasterTimer claude_raster_timer{1};
    if (!Config::managedGpuFastPath()) PortableGpu::BeginProducerBindings(&buffer_cache,&scheduler);
    if (IsDeviceTerminal()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    scheduler.MaybePublishExactA750D9SampledRecordingPressureBoundary("draw-indirect");
#endif
    RENDERER_TRACE;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ++executor_completion_work_serial;
#endif

    scheduler.PopPendingOperations();

    if (!FilterDraw()) {
        return;
    }

    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline();
    if (!pipeline || !pipeline->IsValid()) {
        return;
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorBisectSkipPipeline(pipeline->GetPipelineHash())) {
        return;
    }
    ExecutorPreparePhysicalDrsDraw(pipeline);
#endif
    PrepareRenderState(pipeline);
    auto resource_upload_batch = scheduler.BeginBufferUploadBatch();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    buffer_cache.BeginReadOnlyDescriptorSnapshotBatch();
    SCOPE_EXIT { buffer_cache.CancelReadOnlyDescriptorSnapshotBatch(); };
#endif
    if (!BindResources(pipeline)) {
        resource_upload_batch.Flush();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
#endif
        return;
    }
    resource_upload_batch.Flush();

    auto geometry_upload_batch = scheduler.BeginBufferUploadBatch();

    const auto& [buffer, base] =
        buffer_cache.ObtainReadOnlyBuffer(
            arg_address + offset, stride * max_count,
            VideoCore::BufferCache::ReadOnlyBufferUsage::Indirect);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) =
            buffer_cache.ObtainReadOnlyBuffer(
                count_address, 4,
                VideoCore::BufferCache::ReadOnlyBufferUsage::Indirect);
    }

    const auto prepared_vertex_buffers =
        buffer_cache.PrepareVertexBuffers(*pipeline, buffer_barriers);
    if (!prepared_vertex_buffers.valid) {
        geometry_upload_batch.Flush();
        buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
        return;
    }
    VideoCore::BufferCache::PreparedIndexBuffer prepared_index_buffer{};
    if (is_indexed) {
        prepared_index_buffer = buffer_cache.PrepareIndexBuffer(0, buffer_barriers);
    }
    geometry_upload_batch.Flush();
    auto state = BeginRendering(pipeline);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_physical_drs_selected =
        ExecutorBeginPhysicalDrs(pipeline, state);
#endif
    UpdateAttachmentlessRenderArea(state);
    UpdateFragmentShadingRate(pipeline, state);
    const auto& regs = liverpool->regs;

    const bool executor_gpu_probe_enabled = texture_cache.ExecutorGpuContentProbeEnabled();
    const u64 executor_gpu_probe_draw =
        executor_gpu_probe_enabled ? ++executor_gpu_probe_draw_sequence : 0;
    const auto* executor_gpu_probe_fragment =
        pipeline->TryGetStage(Shader::LogicalStage::Fragment);
    const u64 executor_gpu_probe_fragment_hash =
        executor_gpu_probe_fragment ? executor_gpu_probe_fragment->pgm_hash : 0;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto executor_gpu_probe_attachments =
        executor_gpu_probe_enabled ? ExecutorSummarizeAttachments(cb_descs, db_desc)
                                   : ExecutorVkAttachmentSummary{};
    const u64 executor_gpu_probe_attachment_signature =
        executor_gpu_probe_attachments.signature;
    const bool executor_gpu_probe_late = AmdGpu::RenderWaveTrace::LateFrameCaptureActive();
    if (executor_gpu_probe_late) {
        ExecutorGpuLatePassTransition(executor_gpu_probe_attachment_signature,
                                      executor_gpu_probe_draw, "graphics_signature");
    }
#else
    const u64 executor_gpu_probe_attachment_signature = 0;
    const bool executor_gpu_probe_late = false;
#endif
    const u64 executor_gpu_probe_extent = executor_gpu_probe_enabled
        ? texture_cache.ExecutorGpuContentProbeExtentFilter() : 0;
    const bool executor_gpu_probe_extent_matches = executor_gpu_probe_extent &&
        executor_gpu_probe_fragment && std::ranges::any_of(
            executor_gpu_probe_fragment->images, [&](const auto& descriptor) {
                const auto sharp = descriptor.GetSharp(*executor_gpu_probe_fragment);
                if (!descriptor.is_written) texture_cache.ExecutorGpuContentProbeObserveImage(sharp);
                return !descriptor.is_written &&
                    static_cast<u64>(sharp.width + 1) == (executor_gpu_probe_extent >> 32) &&
                    static_cast<u64>(sharp.height + 1) == (executor_gpu_probe_extent & 0xffffffffu);
            });
    const bool executor_gpu_probe_selected =
        executor_gpu_probe_enabled &&
        texture_cache.ExecutorGpuContentProbeDrawSelected(executor_gpu_probe_draw,
                                                          executor_gpu_probe_fragment_hash,
                                                          executor_gpu_probe_attachment_signature,
                                                          executor_gpu_probe_attachments.color_count,
                                                          cb_descs[0].first
                                                              ? cb_descs[0].second.info.size.width
                                                              : 0,
                                                          cb_descs[0].first
                                                              ? cb_descs[0].second.info.size.height
                                                              : 0,
                                                          static_cast<bool>(db_desc.first), 0, true,
                                                          executor_gpu_probe_extent_matches);
    if (executor_gpu_probe_selected) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const auto& probe_key = pipeline->GetGraphicsKey();
        const auto probe_stage_hash = [&](Shader::LogicalStage stage) {
            return probe_key.stage_hashes[static_cast<u32>(stage)];
        };
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_DRAW_INDIRECT_STATE] seq=%llu fs=0x%llx tcs=0x%llx "
            "tes=0x%llx vs=0x%llx gs=0x%llx indexed=%u arg=0x%llx offset=%u "
            "stride=%u maxCount=%u count=0x%llx prim=%u stageEnable=0x%x cb0=0x%llx "
            "db=0x%llx writes=%zu buffers=%zu images=%zu",
            static_cast<unsigned long long>(executor_gpu_probe_draw),
            static_cast<unsigned long long>(executor_gpu_probe_fragment_hash),
            static_cast<unsigned long long>(
                probe_stage_hash(Shader::LogicalStage::TessellationControl)),
            static_cast<unsigned long long>(
                probe_stage_hash(Shader::LogicalStage::TessellationEval)),
            static_cast<unsigned long long>(probe_stage_hash(Shader::LogicalStage::Vertex)),
            static_cast<unsigned long long>(probe_stage_hash(Shader::LogicalStage::Geometry)),
            is_indexed ? 1u : 0u, static_cast<unsigned long long>(arg_address), offset, stride,
            max_count, static_cast<unsigned long long>(count_address),
            static_cast<unsigned>(regs.primitive_type), static_cast<unsigned>(regs.stage_enable.raw),
            static_cast<unsigned long long>(regs.color_buffers[0].Address()),
            static_cast<unsigned long long>(regs.depth_buffer.DepthAddress()), set_writes.size(),
            buffer_infos.size(), image_infos.size());
        for (const auto* probe_stage : pipeline->GetStages()) {
            if (!probe_stage) {
                continue;
            }
            Common::ContentFingerprint64 flat_fingerprint{
                Common::FingerprintDomain::ShaderUserData};
            u32 flat_nonzero = 0;
            const auto flat = probe_stage->FlattenedUserData();
            for (const u32 value : flat) {
                flat_fingerprint.UpdateLittleEndian(value);
                flat_nonzero += value != 0;
            }
            const u64 flat_digest = flat_fingerprint.Finish();
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_STAGE_SRT] seq=%llu indirect=1 stage=%u logical=%u "
                "hash=0x%llx flatDw=%zu flatNonzero=%u digest=0x%llx nodes=%zu copies=%zu",
                static_cast<unsigned long long>(executor_gpu_probe_draw),
                static_cast<unsigned>(probe_stage->stage),
                static_cast<unsigned>(probe_stage->l_stage),
                static_cast<unsigned long long>(probe_stage->pgm_hash),
                flat.size(), flat_nonzero,
                static_cast<unsigned long long>(flat_digest),
                probe_stage->srt_info.portable_nodes.size(),
                probe_stage->srt_info.portable_copies.size());
        }
#endif
        if (!executor_gpu_probe_late) {
            if (cb_descs[0].first) {
                texture_cache.ExecutorGpuContentProbe(
                    cb_descs[0].first,
                    VideoCore::TextureCache::ExecutorGpuContentRole::ColorBeforeDraw,
                    static_cast<u32>(Shader::LogicalStage::Fragment),
                    executor_gpu_probe_fragment_hash, executor_gpu_probe_draw);
            }
            if (db_desc.first) {
                texture_cache.ExecutorGpuContentProbe(
                    db_desc.first,
                    VideoCore::TextureCache::ExecutorGpuContentRole::DepthBeforeDraw,
                    static_cast<u32>(Shader::LogicalStage::Fragment),
                    executor_gpu_probe_fragment_hash, executor_gpu_probe_draw);
            }
        }
    }

    if (auto barrier = buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                          vk::PipelineStageFlagBits2::eDrawIndirect)) {
        buffer_barriers.emplace_back(*barrier);
    }
    if (count_buffer) {
        if (auto barrier = count_buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                                    vk::PipelineStageFlagBits2::eDrawIndirect)) {
            buffer_barriers.emplace_back(*barrier);
        }
    }

    buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream).RetainCurrentAllocation();

    pipeline->BindResources(set_writes, buffer_barriers, push_data);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_post_bind_dynamic =
        ExecutorNeedsQualcommPostBindDynamicState(instance);
    if (!executor_post_bind_dynamic) {
        UpdateDynamicState(pipeline, is_indexed);
    }
#else
    UpdateDynamicState(pipeline, is_indexed);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_gpu_probe_selected && executor_gpu_probe_late) {
        ExecutorGpuLatePassArm(executor_gpu_probe_attachment_signature,
                               executor_gpu_probe_draw, executor_gpu_probe_fragment_hash);
    }
    if (executor_physical_drs_selected) {
        std::scoped_lock drs_lock{executor_physical_drs_mutex};
        if (!executor_physical_drs_draw_active) {
            // Descriptor/image synchronization may resolve the scaled target
            // after its attachment state was built.  The direct-draw path
            // already rejects that stale transaction; indirect draws must do
            // the same or they can render into a scratch image whose layout
            // and canonical ownership were just retired.
            ResetBindings();
            return;
        }
    }
#endif
    scheduler.BeginRendering(state);
    if (!scheduler.HasRecordingCommandBuffer()) {
        ResetBindings();
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorBindGuestGraphicsPipeline(pipeline, cmdbuf);
#else
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eGraphics);
#endif
    buffer_cache.BindPreparedVertexBuffers(prepared_vertex_buffers);
    if (is_indexed) {
        buffer_cache.BindPreparedIndexBuffer(prepared_index_buffer);
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_post_bind_dynamic) {
        UpdateDynamicState(pipeline, is_indexed);
    }
    ExecutorSealGuestGraphicsPipelineState(pipeline, cmdbuf);
#endif

    if (Config::getVkHostMarkersEnabled()) {
        const auto& marker_key = pipeline->GetGraphicsKey();
        ScopedMarkerInsert(fmt::format(
            "DrawIndirect:pipe={:#x}:vs={:#x}:ps={:#x}:indexed={}:max={}:stride={}:cb0={:#x}:pass={}x{}x{}:colors={}:depth={}:stencil={}",
            pipeline->GetPipelineHash(),
            marker_key.stage_hashes[u32(Shader::LogicalStage::Vertex)],
            marker_key.stage_hashes[u32(Shader::LogicalStage::Fragment)], is_indexed,
            max_count, stride, regs.color_buffers[0].Address(),
            state.width, state.height, state.num_layers,
            std::count_if(state.color_attachments.begin(), state.color_attachments.end(),
                          [](const auto& attachment) { return bool(attachment.image_view); }),
            state.depth_stencil_attachment.has_depth, state.depth_stencil_attachment.has_stencil));
    }
    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_base, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
    }
    PortableGpu::CommitProducerBindings(&buffer_cache,&scheduler,scheduler.CurrentTick());
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
#endif
    if (executor_gpu_probe_selected && !executor_gpu_probe_late && cb_descs[0].first) {
        texture_cache.ExecutorGpuContentProbe(
            cb_descs[0].first, VideoCore::TextureCache::ExecutorGpuContentRole::ColorAfterDraw,
            static_cast<u32>(Shader::LogicalStage::Fragment),
            executor_gpu_probe_fragment_hash,
            executor_gpu_probe_draw);
    }
    if (executor_gpu_probe_selected && !executor_gpu_probe_late && db_desc.first) {
        texture_cache.ExecutorGpuContentProbe(
            db_desc.first, VideoCore::TextureCache::ExecutorGpuContentRole::DepthAfterDraw,
            static_cast<u32>(Shader::LogicalStage::Fragment), executor_gpu_probe_fragment_hash,
            executor_gpu_probe_draw);
    }
    if (executor_gpu_probe_selected && !executor_gpu_probe_late) {
        u32 sampled_probe_count = 0;
        for (const auto& probe : executor_gpu_probe_sampled_images) {
            if (sampled_probe_count++ >= 8) {
                break;
            }
            texture_cache.ExecutorGpuContentProbe(
                probe.image_id,
                VideoCore::TextureCache::ExecutorGpuContentRole::SampledBeforeDraw,
                probe.stage, probe.stage_hash, executor_gpu_probe_draw);
        }
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!Config::managedGpuFastPath() || ExecutorVkJournalRequested()) {
        const auto& key = pipeline->GetGraphicsKey();
        const auto& color = cb_descs[0].second.info;
        const u64 journal_sequence = ExecutorVkJournalGraphicsDraw(
            &scheduler, scheduler.CurrentTick(), pipeline->GetPipelineHash(),
            ExecutorVkPipelineHandle(pipeline->Handle()),
            key.stage_hashes[u32(Shader::LogicalStage::Vertex)],
            key.stage_hashes[u32(Shader::LogicalStage::Fragment)],
            is_indexed ? ExecutorVkJournalDrawType::IndexedIndirect
                       : ExecutorVkJournalDrawType::Indirect,
            static_cast<u32>(key.prim_type), 0, 0, max_count, stride,
            arg_address + offset, count_address,
            cb_descs[0].first ? color.guest_address : 0,
            executor_gpu_probe_attachments.depth_address,
            executor_gpu_probe_attachments.signature,
            executor_gpu_probe_attachments.color_count,
            cb_descs[0].first ? color.size.width : 0, cb_descs[0].first ? color.size.height : 0,
            static_cast<u32>(pipeline->GetColorFormat0()),
            static_cast<u32>(pipeline->GetDepthFormat()), static_cast<u32>(set_writes.size()),
            static_cast<u32>(buffer_infos.size()), static_cast<u32>(image_infos.size()),
            executor_gpu_probe_enabled);
        scheduler.RecordGpuFaultDrawCheckpoint(
            journal_sequence, pipeline->GetPipelineHash(), 0,
            is_indexed ? ExecutorVkJournalDrawType::IndexedIndirect
                       : ExecutorVkJournalDrawType::Indirect,
            static_cast<u32>(key.prim_type));
        if (executor_gpu_probe_selected) {
            ExecutorVkJournalArmContentProbe(&scheduler, journal_sequence,
                                             executor_gpu_probe_draw,
                                             executor_gpu_probe_fragment_hash);
        }
    }
    ++executor_unpublished_actual_draws;
    {
        const auto& key = pipeline->GetGraphicsKey();
        AmdGpu::RenderWaveTrace::RasterDraw(
            regs, key.stage_hashes[u32(Shader::LogicalStage::Vertex)],
            key.stage_hashes[u32(Shader::LogicalStage::Fragment)], pipeline->GetPipelineHash(),
            is_indexed, true);
    }
    if (!Config::managedGpuFastPath() || ExecutorVkJournalRequested()) {
        ExecutorTraceDrawnColorTarget(cb_descs[0].first ? &cb_descs[0].second : nullptr);
    }
    if (cb_image_uids[0] != 0) {
        texture_cache.ExecutorTrackLiveDrawnColorTarget(cb_descs[0].first,
                                                        cb_image_uids[0]);
    }
#endif

    ResetBindings();
}

void Rasterizer::DispatchDirect() {
#if defined(LSX4_APPLE_RUNTIME)
    std::optional<AppleComputeSplit::BridgeScope> apple_split_scope;
    if (AppleComputeSplit::BridgeSelected()) {
        const auto& grid = liverpool->GetCsRegs();
        apple_split_scope.emplace(grid.dim_x,grid.dim_y,grid.dim_z);
    }
#endif
    PortablePublishNaturalBoundary(*this);
    ClaudeRasterTimer claude_raster_timer{2};
    if (!Config::managedGpuFastPath()) PortableGpu::BeginProducerBindings(&buffer_cache,&scheduler);
    if (IsDeviceTerminal()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    scheduler.MaybePublishExactA750D9SampledRecordingPressureBoundary("dispatch-direct");
#endif
    RENDERER_TRACE;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorResolvePhysicalDrs("compute_direct");
#endif
    scheduler.PopPendingOperations();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ++executor_completion_work_serial;
    if (AmdGpu::RenderWaveTrace::LateFrameCaptureActive()) {
        ExecutorGpuLatePassTransition(0, executor_gpu_probe_dispatch_sequence + 1,
                                      "compute_direct");
    }
#endif

    const auto& cs_program = liverpool->GetCsRegs();
    if (g_claude_census.enabled) {
        ClaudeCensusNote(static_cast<u64>(cs_program.address) << 8,
                         cs_program.dim_x * cs_program.dim_y * cs_program.dim_z,
                         cs_program.num_thread_x.full * cs_program.num_thread_y.full *
                             cs_program.num_thread_z.full);
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool diagnose_dispatch_phase =
        ExecutorComputeDispatchPhaseDiagnosticEnabled();
    static std::atomic<u32> dispatch_diagnostic_sequence{};
    const u32 dispatch_diagnostic_id =
        diagnose_dispatch_phase
            ? dispatch_diagnostic_sequence.fetch_add(1, std::memory_order_relaxed) + 1
            : 0;
    const auto log_dispatch_phase = [&](const char* phase,
                                        const ComputePipeline* current_pipeline = nullptr) {
        if (!diagnose_dispatch_phase || dispatch_diagnostic_id > 256 ||
            (std::strcmp(phase, "before-bind-resources") != 0 &&
             std::strcmp(phase, "after-bind-resources") != 0)) {
            return;
        }
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_COMPUTE_DISPATCH_PHASE] id=%u phase=%s pgmBase=0x%llx "
            "dims=%ux%ux%u pipeline=0x%llx",
            dispatch_diagnostic_id, phase,
            static_cast<unsigned long long>(cs_program.address << 8),
            cs_program.dim_x, cs_program.dim_y, cs_program.dim_z,
            static_cast<unsigned long long>(
                current_pipeline ? current_pipeline->GetPipelineHash() : 0));
    };
    log_dispatch_phase("before-pipeline-lookup");
#else
    constexpr u32 dispatch_diagnostic_id = 0;
#endif
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("after-pipeline-lookup", pipeline);
#endif
    if (!pipeline || !pipeline->IsValid()) {
        return;
    }

    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (ExecuteShaderHLE(cs, liverpool->regs, cs_program, *this)) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("after-shader-hle", pipeline);
#endif

    auto buffer_upload_batch = scheduler.BeginBufferUploadBatch();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    buffer_cache.BeginReadOnlyDescriptorSnapshotBatch();
    SCOPE_EXIT { buffer_cache.CancelReadOnlyDescriptorSnapshotBatch(); };
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("before-bind-resources", pipeline);
#endif
    if (!BindResources(pipeline, dispatch_diagnostic_id)) {
        buffer_upload_batch.Flush();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
#endif
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("after-bind-resources", pipeline);
#endif
    buffer_upload_batch.Flush();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("after-upload-flush", pipeline);
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (texture_cache.ExecutorGpuContentProbeEnabled()) {
        const u64 probe_sequence = executor_gpu_probe_dispatch_sequence + 1;
        if (texture_cache.ExecutorGpuContentProbeDispatchSelected(probe_sequence)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_COMPUTE_DISPATCH] seq=%llu indirect=0 hash=0x%llx "
                "pipeline=0x%llx code=0x%llx dims=%ux%ux%u writes=%u buffers=%u images=%u "
                "storageImages=%u ud=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                static_cast<unsigned long long>(probe_sequence),
                static_cast<unsigned long long>(cs.pgm_hash),
                static_cast<unsigned long long>(pipeline->GetPipelineHash()),
                static_cast<unsigned long long>(cs_program.address << 8), cs_program.dim_x,
                cs_program.dim_y, cs_program.dim_z, static_cast<u32>(set_writes.size()),
                static_cast<u32>(buffer_infos.size()), static_cast<u32>(image_infos.size()),
                static_cast<u32>(executor_gpu_probe_storage_images.size()),
                cs_program.user_data[0], cs_program.user_data[1], cs_program.user_data[2],
                cs_program.user_data[3], cs_program.user_data[4], cs_program.user_data[5],
                cs_program.user_data[6], cs_program.user_data[7]);
        }
    }
#endif

    scheduler.EndRendering(Scheduler::EndRenderingReason::RasterizerDispatch);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("after-end-rendering", pipeline);
#endif
    buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream).RetainCurrentAllocation();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("before-descriptor-bind", pipeline);
#endif
    pipeline->BindResources(set_writes, buffer_barriers, push_data);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("after-descriptor-bind", pipeline);
#endif

    if (!scheduler.HasRecordingCommandBuffer()) {
        ResetBindings();
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("before-pipeline-bind", pipeline);
    ExecutorBindGuestComputePipeline(pipeline, cmdbuf);
    log_dispatch_phase("after-pipeline-bind", pipeline);
#else
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("before-dispatch", pipeline);
#endif
#if defined(LSX4_APPLE_RUNTIME)
    // Preserve resource/barrier/retirement bookkeeping around the Vulkan no-op.
    // The common capability profile supplies the host translation policy.
    if (instance.GetCapabilityProfile().ShouldSubmitDirectDispatch(
            cs_program.dim_x, cs_program.dim_y, cs_program.dim_z)) {
#endif
#if defined(LSX4_APPLE_RUNTIME)
    const u32 apple_compute_timing_index = AppleComputeTiming::Begin(
        instance,&scheduler,cmdbuf,scheduler.CurrentTick(),cs.pgm_hash,
        pipeline->GetPipelineHash(),cs_program.dim_x,cs_program.dim_y,cs_program.dim_z);
#endif
#if defined(LSX4_APPLE_RUNTIME)
    const u32 apple_split_factor = AppleComputeSplit::BridgeSelected() ? AppleComputeSplit::DirectFactor() : 1;
    if(apple_split_factor == 8) {
        static std::atomic_flag apple_split_used_first=ATOMIC_FLAG_INIT;
        if(!apple_split_used_first.test_and_set(std::memory_order_relaxed))
            std::fprintf(stderr,"[APPLE_COMPUTE_SPLIT_USE] firstDispatch=1 hash=0x%llx factor=8 guestInvocationCoverage=1 indirectUnchanged=1\n",(unsigned long long)cs.pgm_hash);
    }
    ClaudeConcurrentBarrierBeforeDispatch(cmdbuf);
    cmdbuf.dispatch(cs_program.dim_x * apple_split_factor, cs_program.dim_y, cs_program.dim_z);
#else
    cmdbuf.dispatch(cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
#endif
#if defined(LSX4_APPLE_RUNTIME)
    AppleComputeTiming::End(cmdbuf,apple_compute_timing_index);
#endif
    PortableGpu::CommitProducerBindings(&buffer_cache,&scheduler,scheduler.CurrentTick());
#if defined(LSX4_APPLE_RUNTIME)
    } else {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set(std::memory_order_relaxed)) {
            std::fprintf(stderr, "[APPLE_EMPTY_DISPATCH] skipped=1 dims=%ux%ux%u\n",
                         cs_program.dim_x, cs_program.dim_y, cs_program.dim_z);
        }
    }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("after-dispatch", pipeline);
    executor_compute_work_since_cp_sync = true;
    buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!Config::managedGpuFastPath()) {
        ExecutorVkJournalComputeDispatch(
            &scheduler, scheduler.CurrentTick(), pipeline->GetPipelineHash(),
            ExecutorVkPipelineHandle(pipeline->Handle()), pipeline->GetPipelineHash(), false,
            cs_program.dim_x, cs_program.dim_y, cs_program.dim_z, 0,
            static_cast<u32>(set_writes.size()), static_cast<u32>(buffer_infos.size()),
            static_cast<u32>(image_infos.size()),
            texture_cache.ExecutorGpuContentProbeEnabled());
    }
#endif

    if (texture_cache.ExecutorGpuContentProbeEnabled()) {
        const u64 executor_gpu_probe_dispatch = ++executor_gpu_probe_dispatch_sequence;
        if (texture_cache.ExecutorGpuContentProbeDispatchSelected(executor_gpu_probe_dispatch)) {
            for (const auto& probe : executor_gpu_probe_storage_images) {
                texture_cache.ExecutorGpuContentProbe(
                    probe.image_id,
                    VideoCore::TextureCache::ExecutorGpuContentRole::StorageAfterDispatch,
                    probe.stage, probe.stage_hash, executor_gpu_probe_dispatch);
            }
        }
    }

    ResetBindings();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_dispatch_phase("complete", pipeline);
#endif
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    PortablePublishNaturalBoundary(*this);
    ClaudeRasterTimer claude_raster_timer{3};
    if (!Config::managedGpuFastPath()) PortableGpu::BeginProducerBindings(&buffer_cache,&scheduler);
    if (IsDeviceTerminal()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    scheduler.MaybePublishExactA750D9SampledRecordingPressureBoundary("dispatch-indirect");
#endif
    RENDERER_TRACE;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorResolvePhysicalDrs("compute_indirect");
#endif
    scheduler.FlushPendingBufferTransfers();
    scheduler.PopPendingOperations();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ++executor_completion_work_serial;
    if (AmdGpu::RenderWaveTrace::LateFrameCaptureActive()) {
        ExecutorGpuLatePassTransition(0, executor_gpu_probe_dispatch_sequence + 1,
                                      "compute_indirect");
    }
#endif

    const auto& cs_program = liverpool->GetCsRegs();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline || !pipeline->IsValid()) {
        return;
    }
    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);

    auto buffer_upload_batch = scheduler.BeginBufferUploadBatch();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    buffer_cache.BeginReadOnlyDescriptorSnapshotBatch();
    SCOPE_EXIT { buffer_cache.CancelReadOnlyDescriptorSnapshotBatch(); };
#endif
    if (!BindResources(pipeline)) {
        buffer_upload_batch.Flush();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
#endif
        return;
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (texture_cache.ExecutorGpuContentProbeEnabled()) {
        const u64 probe_sequence = executor_gpu_probe_dispatch_sequence + 1;
        if (texture_cache.ExecutorGpuContentProbeDispatchSelected(probe_sequence)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_COMPUTE_DISPATCH] seq=%llu indirect=1 hash=0x%llx "
                "pipeline=0x%llx code=0x%llx args=0x%llx writes=%u buffers=%u images=%u "
                "storageImages=%u ud=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                static_cast<unsigned long long>(probe_sequence),
                static_cast<unsigned long long>(cs.pgm_hash),
                static_cast<unsigned long long>(pipeline->GetPipelineHash()),
                static_cast<unsigned long long>(cs_program.address << 8),
                static_cast<unsigned long long>(address + offset),
                static_cast<u32>(set_writes.size()), static_cast<u32>(buffer_infos.size()),
                static_cast<u32>(image_infos.size()),
                static_cast<u32>(executor_gpu_probe_storage_images.size()),
                cs_program.user_data[0], cs_program.user_data[1], cs_program.user_data[2],
                cs_program.user_data[3], cs_program.user_data[4], cs_program.user_data[5],
                cs_program.user_data[6], cs_program.user_data[7]);
        }
    }
#endif

    const auto [buffer, base] = buffer_cache.ObtainReadOnlyBuffer(
        address + offset, size,
        VideoCore::BufferCache::ReadOnlyBufferUsage::Indirect);
    buffer_upload_batch.Flush();

    if (auto barrier = buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                          vk::PipelineStageFlagBits2::eDrawIndirect)) {
        buffer_barriers.emplace_back(*barrier);
    }

    scheduler.EndRendering(Scheduler::EndRenderingReason::RasterizerDispatch);
    buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream).RetainCurrentAllocation();
    pipeline->BindResources(set_writes, buffer_barriers, push_data);

    if (!scheduler.HasRecordingCommandBuffer()) {
        ResetBindings();
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorBindGuestComputePipeline(pipeline, cmdbuf);
#else
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    scheduler.MarkPipelineBind(vk::PipelineBindPoint::eCompute);
#endif
    ClaudeConcurrentBarrierBeforeDispatch(cmdbuf);
    cmdbuf.dispatchIndirect(buffer->Handle(), base);
    PortableGpu::CommitProducerBindings(&buffer_cache,&scheduler,scheduler.CurrentTick());
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    executor_compute_work_since_cp_sync = true;
    buffer_cache.RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick());
#endif

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (!Config::managedGpuFastPath()) {
        ExecutorVkJournalComputeDispatch(
            &scheduler, scheduler.CurrentTick(), pipeline->GetPipelineHash(),
            ExecutorVkPipelineHandle(pipeline->Handle()), pipeline->GetPipelineHash(), true, 0, 0,
            0, address + offset, static_cast<u32>(set_writes.size()),
            static_cast<u32>(buffer_infos.size()), static_cast<u32>(image_infos.size()),
            texture_cache.ExecutorGpuContentProbeEnabled());
    }
#endif

    if (texture_cache.ExecutorGpuContentProbeEnabled()) {
        const u64 executor_gpu_probe_dispatch = ++executor_gpu_probe_dispatch_sequence;
        if (texture_cache.ExecutorGpuContentProbeDispatchSelected(executor_gpu_probe_dispatch)) {
            for (const auto& probe : executor_gpu_probe_storage_images) {
                texture_cache.ExecutorGpuContentProbe(
                    probe.image_id,
                    VideoCore::TextureCache::ExecutorGpuContentRole::StorageAfterDispatch,
                    probe.stage, probe.stage_hash, executor_gpu_probe_dispatch);
            }
        }
    }

    ResetBindings();
}

u64 Rasterizer::Flush() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorResolvePhysicalDrs("flush");
#endif
    const u64 current_tick = scheduler.CurrentTick();
    scheduler.Flush();
    return current_tick;
}

void Rasterizer::Finish() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorResolvePhysicalDrs("finish");
#endif
    scheduler.Finish(Scheduler::ExecutorFinishReason::Rasterizer);
}

void Rasterizer::OnSubmit() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool skip_empty_physical_drs_submit_cleanup =
        !VideoCore::IsManagedMobileGpuOptimization() &&
        ExecutorBloodborneEmptyPhysicalDrsCleanupFastNoop() &&
        !executor_physical_drs_has_residency.load(
            std::memory_order_acquire);
    if (!skip_empty_physical_drs_submit_cleanup) {
        std::scoped_lock drs_lock{executor_physical_drs_mutex};
        ++executor_physical_drs_generation;
        ExecutorResolvePhysicalDrs("submit");
    }
#endif
    scheduler.FlushPendingBufferTransfers();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_unpublished_actual_draws != 0) {
        Libraries::GnmDriver::ExecutorGnmRecordActualDraw(executor_unpublished_actual_draws);
        executor_unpublished_actual_draws = 0;
    }
#endif
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    buffer_cache.PrefetchReadbackWindows();
    if (Libraries::GnmDriver::ExecutorReplayActive() &&
        texture_cache.executor_replay_color_target) {
        texture_cache.ExecutorReplayReadbackColorTarget();
    }
#endif
    texture_cache.ProcessDownloadImages();
    const auto& memory_policy =
        instance.GetCapabilityProfile().memory_policy;
    const bool sustained_turnip_a750_retirement =
        memory_policy.residency_retirement ==
        ResidencyRetirementMode::CoordinatedSustainedNonBlocking;
    const auto& aggregate_pressure_policy =
        instance.GetCapabilityProfile().aggregate_memory_pressure;
    const bool coordinated_turnip_a750_pressure =
        aggregate_pressure_policy.coordinated_turnip_retirement;
    // GPU residency pressure is a property of the exact A750 proprietary ICD,
    // not of the selected CPU translator.  Coupling this route to FEX's signal
    // owner silently disabled all VMA sampling and non-blocking retirement for
    // JIT, allowing the same guest/Vulkan working set to reach Android LMKD.
    // The resolved policy remains false for A840, Mali and Turnip.
    const bool proprietary_a750_vma_pressure =
        aggregate_pressure_policy.proprietary_vma_sampling;
    const bool exact_a750_vma_pressure =
        proprietary_a750_vma_pressure || coordinated_turnip_a750_pressure;
    const bool exact_a750_combined_pressure_gc =
        exact_a750_vma_pressure &&
        aggregate_pressure_policy.combined_gc;
    bool run_residency_gc = true;
    bool force_memory_budget_sample = false;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Android's callback is an event, not a per-frame query. Consume it only
    // inside the two exact A750 pressure-managed routes so protected A840 and
    // Mali paths do not gain a load, collector pass, or policy change. The
    // proprietary ICD under-reports KGSL residency through VMA, so its OS
    // pressure event is required to start wait-free retirement early enough.
    // The aggregate policy is the single typed admission point for Android
    // process-footprint pressure.  CoordinatedNonBlocking Turnip must consume
    // it as well: VK_EXT_memory_budget can stay flat while driver-internal BOs
    // grow in Android PSS/graphics/swap.  A840, Mali and marker-free Turnip
    // never resolve exact_a750_vma_pressure and therefore remain unchanged.
    const bool monitored_a750_memory_pressure = exact_a750_vma_pressure;
    const u32 android_memory_pressure_level =
        monitored_a750_memory_pressure
            ? VideoCore::ConsumeAndroidMemoryPressure()
            : 0;
    constexpr u32 AndroidRunningLowMemoryLevel = 10;
    const bool android_memory_pressure_epoch =
        android_memory_pressure_level >= AndroidRunningLowMemoryLevel;
    const u64 guest_flip_count = ExecutorGetGuestFlipCount();
    const u64 now_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    constexpr u64 StalledPresentGcIntervalNs = 500'000'000;
    const bool first_gc = !executor_residency_gc_guest_flip_count_valid;
    const bool new_guest_flip =
        executor_residency_gc_guest_flip_count_valid &&
        guest_flip_count != executor_residency_gc_guest_flip_count;
    const bool stalled_guest_flip =
        executor_residency_gc_guest_flip_count_valid &&
        now_ns - executor_residency_gc_last_run_ns >=
            StalledPresentGcIntervalNs;
    run_residency_gc = first_gc || new_guest_flip || stalled_guest_flip ||
                       android_memory_pressure_epoch;
    force_memory_budget_sample = first_gc || stalled_guest_flip ||
                                 android_memory_pressure_epoch;
    if (run_residency_gc) {
        executor_residency_gc_guest_flip_count = guest_flip_count;
        executor_residency_gc_last_run_ns = now_ns;
        executor_residency_gc_guest_flip_count_valid = true;
    }
#endif

    // The proprietary A750 ICD reports no VK_EXT_memory_budget. Sampling VMA
    // heap usage is O(heap-count), so observe it at every submit, but retain
    // the established cache-GC cadence until aggregate pressure is real.
    // Turnip exposes VK_EXT_memory_budget, so its bounded route samples only
    // on the established flip/stall cadence. Proprietary A750 still needs the
    // VMA fallback every submit because no driver budget sample exists.
    const bool sample_residency =
        run_residency_gc || proprietary_a750_vma_pressure;
    if (sample_residency) {
        std::optional<size_t> sampled_device_memory;
        const auto sample_a750_vma_usage = [&]() {
            std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> heap_budgets{};
            vmaGetHeapBudgets(instance.GetAllocator(), heap_budgets.data());
            size_t usage = 0;
            for (const auto& heap : heap_budgets) {
                usage += static_cast<size_t>(heap.usage);
            }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            static std::atomic<u64> last_logged_vma_bucket{
                std::numeric_limits<u64>::max()};
            const u64 bucket = usage / 256_MB;
            const u64 previous = last_logged_vma_bucket.exchange(
                bucket, std::memory_order_relaxed);
            if (previous != bucket) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_A750_AGGREGATE_MEMORY] source=vma-heap-usage "
                    "blockMiB=%llu budgetMiB=%llu bucket=%llu",
                    static_cast<unsigned long long>(usage / 1_MB),
                    static_cast<unsigned long long>(
                        instance.GetTotalMemoryBudget() / 1_MB),
                    static_cast<unsigned long long>(bucket));
            }
#endif
            return usage;
        };
        if (instance.CanReportMemoryUsage() || exact_a750_vma_pressure) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            constexpr u64 MemoryBudgetSampleInterval = 8;
#else
            constexpr u64 MemoryBudgetSampleInterval = 16;
#endif
            if (proprietary_a750_vma_pressure || force_memory_budget_sample ||
                !memory_budget_sample_valid ||
                (memory_budget_submit_counter++ %
                 MemoryBudgetSampleInterval) == 0) {
                if (instance.CanReportMemoryUsage()) {
                    cached_device_memory_usage = instance.GetDeviceMemoryUsage();
                } else {
                    cached_device_memory_usage = sample_a750_vma_usage();
                }
                memory_budget_sample_valid = true;
            }
            sampled_device_memory = cached_device_memory_usage;
            VideoCore::ObserveMobileGpuMemory(
                static_cast<u64>(*sampled_device_memory),
                static_cast<u64>(instance.GetMemoryPressureUsageLimit()));
        }

        const u64 device_budget = instance.GetMemoryPressureUsageLimit();
        const u64 a750_soft_limit =
            exact_a750_combined_pressure_gc
                ? device_budget * 55 / 100
                : device_budget - device_budget / 4;
        bool exact_a750_pressure_epoch = android_memory_pressure_epoch;
        if (exact_a750_vma_pressure && sampled_device_memory &&
            device_budget != 0) {
            const u64 usage = static_cast<u64>(*sampled_device_memory);
            if (usage < a750_soft_limit) {
                executor_a750_aggregate_gc_latched = false;
                executor_a750_aggregate_gc_high_water_usage = 0;
                executor_a750_aggregate_last_sustained_gc_ns = 0;
            } else {
                constexpr u64 SustainedRetirementIntervalNs = 1'000'000'000;
                // The proprietary A750 route can remain above the pressure
                // threshold without growing another high-water step. Re-arm
                // the existing wait-free collectors at a bounded cadence so
                // Android does not keep swapping while the latch is set.
                const bool sustained_retirement_due =
                    (sustained_turnip_a750_retirement ||
                     proprietary_a750_vma_pressure) &&
                    (executor_a750_aggregate_last_sustained_gc_ns == 0 ||
                     now_ns - executor_a750_aggregate_last_sustained_gc_ns >=
                         SustainedRetirementIntervalNs);
                const bool high_water_grew =
                    usage > executor_a750_aggregate_gc_high_water_usage &&
                    usage - executor_a750_aggregate_gc_high_water_usage >=
                        128_MB;
                exact_a750_pressure_epoch = android_memory_pressure_epoch ||
                    !executor_a750_aggregate_gc_latched || high_water_grew ||
                    sustained_retirement_due;
                if (exact_a750_pressure_epoch) {
                    executor_a750_aggregate_gc_latched = true;
                    executor_a750_aggregate_gc_high_water_usage =
                        std::max(executor_a750_aggregate_gc_high_water_usage,
                                 usage);
                    if (sustained_turnip_a750_retirement ||
                        proprietary_a750_vma_pressure) {
                        executor_a750_aggregate_last_sustained_gc_ns = now_ns;
                    }
                    ++executor_a750_aggregate_gc_pressure_epoch_count;
                } else {
                    ++executor_a750_aggregate_gc_suppressed_count;
                }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                const bool log_pressure_epoch =
                    exact_a750_pressure_epoch ||
                    executor_a750_aggregate_gc_suppressed_count <= 8 ||
                    (executor_a750_aggregate_gc_suppressed_count % 256) == 0;
                if (log_pressure_epoch) {
                    __android_log_print(
                        ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_A750_AGGREGATE_GC] action=%s "
                        "usageMiB=%llu softMiB=%llu highWaterMiB=%llu "
                        "pressureEpoch=%llu suppressed=%llu",
                        exact_a750_pressure_epoch ? "pressure-epoch"
                                                  : "suppressed",
                        static_cast<unsigned long long>(usage / 1_MB),
                        static_cast<unsigned long long>(a750_soft_limit /
                                                        1_MB),
                        static_cast<unsigned long long>(
                            executor_a750_aggregate_gc_high_water_usage /
                            1_MB),
                        static_cast<unsigned long long>(
                            executor_a750_aggregate_gc_pressure_epoch_count),
                        static_cast<unsigned long long>(
                            executor_a750_aggregate_gc_suppressed_count));
                }
#endif
            }
        }
        std::optional<size_t> cache_pressure_sample = sampled_device_memory;
        if (android_memory_pressure_epoch && device_budget != 0) {
            // The proprietary ICD cannot report its real KGSL residency, so
            // Android process pressure remains its emergency proxy. Turnip
            // does report a device-memory sample: promoting unrelated process
            // PSS/swap to the full Vulkan budget made both caches enter the
            // emergency path and retire up to 128 images during Bloodborne's
            // loading screen. On A750 this produced missing glyphs followed by
            // an unhandled driver access. Preserve the real Turnip sample and
            // use process pressure only to request the wait-free publication
            // boundary below. A840 and Mali never enter this exact route.
            if (proprietary_a750_vma_pressure) {
                cache_pressure_sample = std::max(
                    cache_pressure_sample.value_or(0),
                    static_cast<size_t>(device_budget));
            }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_A750_ANDROID_MEMORY_PRESSURE] level=%u "
                "action=nonblocking-retirement sampleMiB=%llu promoted=%u waited=0",
                android_memory_pressure_level,
                static_cast<unsigned long long>(
                    cache_pressure_sample.value_or(0) / 1_MB),
                proprietary_a750_vma_pressure ? 1u : 0u);
#endif
        }
        if (exact_a750_vma_pressure && !exact_a750_pressure_epoch) {
            cache_pressure_sample.reset();
        }
        const bool run_cache_collectors =
            run_residency_gc || exact_a750_pressure_epoch;
        if (run_cache_collectors) {
            texture_cache.RunGarbageCollector(cache_pressure_sample,
                                               exact_a750_pressure_epoch);
            buffer_cache.RunGarbageCollector(cache_pressure_sample,
                                              exact_a750_pressure_epoch);
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        // Both snapshots are O(1) counters owned by this render thread. Keep
        // the proof receiver away from cache mutexes and container walks.
        const auto texture_residency =
            texture_cache.GetResidencyTelemetrySnapshot();
        const auto buffer_residency =
            buffer_cache.GetResidencyTelemetrySnapshot();
        PublishRendererCacheResidencyTelemetry(
            texture_residency.guest_bytes,
            texture_residency.image_entries,
            texture_residency.image_view_entries,
            texture_residency.sampler_entries,
            buffer_residency.resident_bytes,
            buffer_residency.utility_bytes,
            buffer_residency.buffer_entries);
        if (ExecutorUsesExactA750TurnipLoadingTransitionDrain(instance)) {
            const u64 cache_bytes = texture_residency.guest_bytes +
                                    buffer_residency.resident_bytes;
            executor_a750_loading_cache_peak_bytes =
                std::max(executor_a750_loading_cache_peak_bytes, cache_bytes);
            constexpr u64 MinimumLoadingPeakBytes = 1200_MB;
            constexpr u64 MinimumLoadingDropBytes = 256_MB;
            const bool large_loading_drop =
                executor_a750_loading_cache_peak_bytes >= MinimumLoadingPeakBytes &&
                cache_bytes <= executor_a750_loading_cache_peak_bytes -
                                   MinimumLoadingDropBytes;
            if (!executor_a750_loading_transition_drained &&
                android_memory_pressure_epoch && large_loading_drop) {
                const u64 known_before = scheduler.KnownGpuTick();
                const u64 pending_before = scheduler.PendingOperationCount();
                const auto begin = std::chrono::steady_clock::now();
                scheduler.PublishMemoryPressureBoundary();
                const u64 current_after_publish = scheduler.CurrentTick();
                const u64 published_tick =
                    current_after_publish > 1 ? current_after_publish - 1 : 0;
                const bool waited = published_tick > scheduler.KnownGpuTick();
                if (waited) {
                    scheduler.Wait(published_tick);
                }
                scheduler.PopPendingOperations(true);
                const u64 elapsed_ns = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - begin)
                        .count());
                executor_a750_loading_transition_drained = true;
                __android_log_print(
                    ANDROID_LOG_WARN, "LSX4Native",
                    "[EXECUTOR_A750_D9_LOADING_TRANSITION_DRAIN] peakMiB=%llu "
                    "currentMiB=%llu dropMiB=%llu published=%llu knownBefore=%llu "
                    "knownAfter=%llu pendingBefore=%llu pendingAfter=%llu waited=%u "
                    "elapsedMs=%.3f",
                    static_cast<unsigned long long>(
                        executor_a750_loading_cache_peak_bytes / 1_MB),
                    static_cast<unsigned long long>(cache_bytes / 1_MB),
                    static_cast<unsigned long long>(
                        (executor_a750_loading_cache_peak_bytes - cache_bytes) / 1_MB),
                    static_cast<unsigned long long>(published_tick),
                    static_cast<unsigned long long>(known_before),
                    static_cast<unsigned long long>(scheduler.KnownGpuTick()),
                    static_cast<unsigned long long>(pending_before),
                    static_cast<unsigned long long>(scheduler.PendingOperationCount()),
                    waited ? 1u : 0u,
                    static_cast<double>(elapsed_ns) / 1'000'000.0);
            }
        }
        const auto utility_rings =
            buffer_cache.GetUtilityRingTelemetrySnapshot();
        const auto copy_ring = [](const auto& source) {
            return UtilityRingUsageTelemetrySnapshot{
                .capacity_bytes = source.capacity_bytes,
                .requested_bytes = source.requested_bytes,
                .request_count = source.request_count,
                .max_request_bytes = source.max_request_bytes,
                .max_mapped_end = source.max_mapped_end,
                .wrap_count = source.wrap_count,
                .forced_wait_count = source.forced_wait_count,
                .failed_request_count = source.failed_request_count,
            };
        };
        PublishRendererUtilityRingTelemetry({
            .staging = copy_ring(utility_rings.staging),
            .stream = copy_ring(utility_rings.stream),
            .download = copy_ring(utility_rings.download),
            .device = copy_ring(utility_rings.device),
            .upload_reservation = {
                .transaction_count =
                    utility_rings.upload_reservation.transaction_count,
                .completed_transaction_count =
                    utility_rings.upload_reservation.completed_transaction_count,
                .failed_transaction_count =
                    utility_rings.upload_reservation.failed_transaction_count,
                .measurement_count =
                    utility_rings.upload_reservation.measurement_count,
                .retry_count = utility_rings.upload_reservation.retry_count,
                .full_span_candidate_bytes =
                    utility_rings.upload_reservation.full_span_candidate_bytes,
                .dirty_bytes = utility_rings.upload_reservation.dirty_bytes,
                .avoided_bytes = utility_rings.upload_reservation.avoided_bytes,
                .max_full_span_candidate_bytes =
                    utility_rings.upload_reservation.max_full_span_candidate_bytes,
                .max_dirty_bytes =
                    utility_rings.upload_reservation.max_dirty_bytes,
            },
        });
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        // One coordinator owns emergency publication after both cache mutexes
        // are released. Close the current command-buffer epoch so deferred
        // image and scratch owners can retire as the GPU advances, but never
        // wait for the GPU on a loading frame. Hysteresis prevents repeated
        // publication while residency remains in the same pressure band.
        const u64 a750_hard_limit =
            exact_a750_combined_pressure_gc
                ? device_budget * 65 / 100
                : device_budget - device_budget / 5;
        if (exact_a750_vma_pressure && sampled_device_memory &&
            device_budget != 0) {
            const u64 usage_before = *sampled_device_memory;
            if (usage_before < a750_soft_limit) {
                executor_a750_aggregate_drain_latched = false;
                executor_a750_aggregate_last_drain_usage = 0;
                executor_a750_aggregate_last_drain_ns = 0;
            }
            constexpr u64 SustainedDrainIntervalNs = 1'000'000'000;
            const bool sustained_drain_due =
                (sustained_turnip_a750_retirement ||
                 proprietary_a750_vma_pressure) &&
                (executor_a750_aggregate_last_drain_ns == 0 ||
                 now_ns - executor_a750_aggregate_last_drain_ns >=
                     SustainedDrainIntervalNs);
            const bool pressure_grew = android_memory_pressure_epoch ||
                !executor_a750_aggregate_drain_latched ||
                (usage_before > executor_a750_aggregate_last_drain_usage &&
                 usage_before - executor_a750_aggregate_last_drain_usage >=
                     128_MB) ||
                sustained_drain_due;
            const u64 pending_before = scheduler.PendingOperationCount();
            if ((usage_before >= a750_hard_limit ||
                 android_memory_pressure_epoch) &&
                pending_before != 0 &&
                pressure_grew) {
                const auto publish_begin = std::chrono::steady_clock::now();
                scheduler.PublishMemoryPressureBoundary();
                const u64 pending_after = scheduler.PendingOperationCount();
                const u64 publish_ns = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - publish_begin)
                        .count());
                executor_a750_aggregate_drain_latched = true;
                executor_a750_aggregate_last_drain_usage = usage_before;
                executor_a750_aggregate_last_drain_ns = now_ns;
                __android_log_print(
                    ANDROID_LOG_WARN, "LSX4Native",
                    "[EXECUTOR_A750_MEMORY_ADMISSION] action=publish-collect-completed "
                    "usageMiB=%llu budgetMiB=%llu pendingBefore=%llu "
                    "pendingAfter=%llu publishMs=%.3f waited=0",
                    static_cast<unsigned long long>(usage_before / 1_MB),
                    static_cast<unsigned long long>(device_budget / 1_MB),
                    static_cast<unsigned long long>(pending_before),
                    static_cast<unsigned long long>(pending_after),
                    static_cast<double>(publish_ns) / 1'000'000.0);
            }
        }

        if (run_cache_collectors) {
            const bool force_physical_drs_cleanup =
                !VideoCore::IsManagedMobileGpuOptimization();
            (void)ExecutorRunPhysicalDrsResidencyGc(
                0, force_physical_drs_cleanup);
            if (force_physical_drs_cleanup) {
                ExecutorSchedulePhysicalDrsOffCleanup();
            }
            VideoCore::MaybeLogReadbackClassification();
        }
#endif
    }
    pipeline_cache.AdvanceShaderIdentitySubmit();
}

void Rasterizer::DisableFexDeferredWriteEpochsForPhysicalAliasing() noexcept {
    VideoCore::PageManager::ShutdownFexDeferredWriteEpochs();
}

bool Rasterizer::BindResources(const Pipeline* pipeline,
                               const u32 compute_diagnostic_id) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool diagnose_compute_resources =
        compute_diagnostic_id != 0 && compute_diagnostic_id <= 256 &&
        ExecutorComputeDispatchPhaseDiagnosticEnabled();
    const auto log_compute_resource_phase =
        [&](const char* phase, const u32 stage_index = 0xffffffffu) {
            if (!diagnose_compute_resources) {
                return;
            }
            if (std::strcmp(phase, "before-snapshot-guard") != 0 &&
                std::strcmp(phase, "after-snapshot-guard") != 0 &&
                std::strcmp(phase, "before-dma-sync-sweep") != 0 &&
                std::strcmp(phase, "after-dma-sync-sweep") != 0) {
                return;
            }
            const auto& cs_program = liverpool->GetCsRegs();
            __android_log_print(
                ANDROID_LOG_WARN, "LSX4Native",
                "[EXECUTOR_COMPUTE_RESOURCE_PHASE] id=%u phase=%s "
                "pgmBase=0x%llx stage=%u setWrites=%u bufferInfos=%u "
                "imageInfos=%u barriers=%u",
                compute_diagnostic_id, phase,
                static_cast<unsigned long long>(cs_program.address << 8),
                stage_index, static_cast<u32>(set_writes.size()),
                static_cast<u32>(buffer_infos.size()),
                static_cast<u32>(image_infos.size()),
                static_cast<u32>(buffer_barriers.size()));
        };
    log_compute_resource_phase("entry");
#else
    (void)compute_diagnostic_id;
#endif

    if (IsComputeImageCopy(pipeline)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("classified-image-copy");
#endif
        return false;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_resource_phase("after-image-copy-check");
#endif
    if (IsComputeMetaClear(pipeline)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("classified-meta-clear");
#endif
        return false;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_resource_phase("after-meta-clear-check");
#endif
    if (IsComputeImageClear(pipeline)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("classified-image-clear");
#endif
        return false;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_resource_phase("after-image-clear-check");
#endif

    set_writes.clear();
    buffer_barriers.clear();
    buffer_infos.clear();
    image_infos.clear();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool exact_read_only_reuse_requested =
        VideoCore::IsManagedVulkanExactReadOnlyBufferBindReuse();
    if (exact_read_only_reuse_requested) {
        exact_read_only_buffer_bindings.clear();
    }
#endif
    executor_gpu_probe_sampled_images.clear();
    executor_gpu_probe_storage_images.clear();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_resource_phase("after-binding-reset");
#endif

    bool uses_dma = false;

    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(liverpool->regs);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_resource_phase("after-user-data");
#endif
    u32 stage_index = 0;
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            ++stage_index;
            continue;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("before-push-ud", stage_index);
#endif
        {
            ExecutorDrawStageScope push_user_data_scope{
                ExecutorDrawCpuStage::PushUserData};
            stage->PushUd(binding, push_data);
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("after-push-ud", stage_index);
        log_compute_resource_phase("before-bind-buffers", stage_index);
#endif
        BindBuffers(*stage, binding, push_data);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("after-bind-buffers", stage_index);
        log_compute_resource_phase("before-bind-textures", stage_index);
#endif
        BindTextures(*stage, binding);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("after-bind-textures", stage_index);
#endif
        uses_dma |= stage->uses_dma;
        ++stage_index;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_resource_phase(uses_dma ? "after-stages-dma" :
                                           "after-stages-no-dma");
#endif

    if (uses_dma) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("before-snapshot-guard");
#endif
        // MemoryManager mapping changes take unmap_mutex before entering the
        // rasterizer and eventually taking mapped_ranges_mutex. Preserve that
        // order here as well: taking SnapshotReadGuard below mapped_ranges_mutex
        // can deadlock command processing against a synchronous guest unmap.
        auto dma_snapshot_guard = memory->AcquireSnapshotReadGuard();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("after-snapshot-guard");
        log_compute_resource_phase("before-mapped-ranges-lock");
#endif
        std::vector<std::pair<VAddr, u64>> dma_mapped_ranges;
        {
            std::shared_lock lock{mapped_ranges_mutex};
            dma_mapped_ranges.reserve(mapped_ranges.iterative_size());
            for (const auto& range : mapped_ranges) {
                dma_mapped_ranges.emplace_back(
                    range.lower(), range.upper() - range.lower());
            }
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("after-mapped-ranges-lock");
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        const auto dma_clean_epoch_diag_axis =
            ExecutorBloodborneDmaCleanEpochDiagnosticAxis(instance);
        const bool dma_clean_epoch_diag_enabled =
            dma_clean_epoch_diag_axis !=
            ExecutorDmaCleanEpochDiagnosticAxis::Disabled;
        VideoCore::BufferCache::DmaCleanEpochDiagnosticSerials
            dma_clean_epoch_before{};
        u64 dma_clean_epoch_mapping_before{};
        u64 dma_clean_epoch_transfer_before{};
        bool dma_clean_epoch_pending_before{};
        bool dma_clean_epoch_fex_activity_before{};
        bool dma_clean_epoch_would_skip_candidate{};
        if (dma_clean_epoch_diag_enabled) {
            auto& diag = executor_dma_clean_epoch_diag;
            if (!diag.enabled.load(std::memory_order_acquire)) {
                buffer_cache.EnableDmaCleanEpochDiagnostic();
                diag.enabled.store(true, std::memory_order_release);
            }
            dma_clean_epoch_before =
                buffer_cache.GetDmaCleanEpochDiagnosticSerials();
            dma_clean_epoch_mapping_before =
                diag.mapping_serial.load(std::memory_order_acquire);
            dma_clean_epoch_transfer_before =
                scheduler.PendingBufferTransferRequestEpoch();
            dma_clean_epoch_pending_before =
                scheduler.HasPendingBufferTransfers();
            dma_clean_epoch_fex_activity_before =
                VideoCore::PageManager::GetFexDeferredWriteActivitySnapshot()
                    .HasActiveWriterOrArmer();
            ++diag.attempts;
            if (!diag.baseline_valid) {
                ++diag.reject_no_baseline;
            }
            if (diag.prior_sync_failed) {
                ++diag.reject_prior_sync_failed;
            }
            if (diag.baseline_valid &&
                dma_clean_epoch_before.cpu_publication !=
                    diag.baseline_cpu_publication_serial) {
                ++diag.reject_cpu_publication_changed;
            }
            if (diag.baseline_valid && dma_clean_epoch_mapping_before !=
                                           diag.baseline_mapping_serial) {
                ++diag.reject_mapping_changed;
            }
            if (diag.baseline_valid &&
                dma_clean_epoch_before.topology !=
                    diag.baseline_topology_serial) {
                ++diag.reject_topology_changed;
            }
            if (dma_clean_epoch_pending_before) {
                ++diag.reject_pending_transfers;
            }
            if (diag.baseline_valid && dma_clean_epoch_transfer_before !=
                                           diag.baseline_transfer_serial) {
                ++diag.reject_transfer_serial_changed;
            }
            if (dma_clean_epoch_fex_activity_before) {
                ++diag.reject_fex_writer_activity;
            }
            dma_clean_epoch_would_skip_candidate =
                ExecutorDmaCleanEpochWouldSkip(
                    diag.baseline_valid, diag.prior_sync_failed,
                    dma_clean_epoch_pending_before,
                    dma_clean_epoch_fex_activity_before,
                    dma_clean_epoch_before.cpu_publication,
                    diag.baseline_cpu_publication_serial,
                    dma_clean_epoch_mapping_before,
                    diag.baseline_mapping_serial,
                    dma_clean_epoch_before.topology,
                    diag.baseline_topology_serial,
                    dma_clean_epoch_transfer_before,
                    diag.baseline_transfer_serial);
        }
        const auto finish_dma_clean_epoch_diagnostic =
            [&](const bool full_sync_succeeded) {
                if (!dma_clean_epoch_diag_enabled) {
                    return;
                }
                auto& diag = executor_dma_clean_epoch_diag;
                const auto after =
                    buffer_cache.GetDmaCleanEpochDiagnosticSerials();
                const u64 mapping_after =
                    diag.mapping_serial.load(std::memory_order_acquire);
                const u64 transfer_after =
                    scheduler.PendingBufferTransferRequestEpoch();
                const bool pending_after =
                    scheduler.HasPendingBufferTransfers();
                const bool fex_activity_after =
                    VideoCore::PageManager::GetFexDeferredWriteActivitySnapshot()
                        .HasActiveWriterOrArmer();
                const bool serials_stable =
                    after.cpu_publication ==
                        dma_clean_epoch_before.cpu_publication &&
                    mapping_after == dma_clean_epoch_mapping_before &&
                    after.topology == dma_clean_epoch_before.topology &&
                    transfer_after == dma_clean_epoch_transfer_before;
                if (transfer_after != dma_clean_epoch_transfer_before) {
                    ++diag.transfer_serial_changes;
                }
                if (!full_sync_succeeded) {
                    ++diag.full_sync_failures;
                    diag.baseline_valid = false;
                    diag.prior_sync_failed = true;
                } else if (!serials_stable || pending_after ||
                           fex_activity_after) {
                    ++diag.unstable_sweeps;
                    diag.baseline_valid = false;
                    diag.prior_sync_failed = false;
                } else {
                    ++diag.stable_sweeps;
                    if (dma_clean_epoch_would_skip_candidate) {
                        ++diag.would_skip;
                    }
                    diag.baseline_cpu_publication_serial =
                        after.cpu_publication;
                    diag.baseline_mapping_serial = mapping_after;
                    diag.baseline_topology_serial = after.topology;
                    diag.baseline_transfer_serial = transfer_after;
                    diag.baseline_valid = true;
                    diag.prior_sync_failed = false;
                }
                if (diag.attempts < diag.next_log_attempt) {
                    return;
                }
                if (diag.next_log_attempt <=
                    std::numeric_limits<u64>::max() / 2) {
                    diag.next_log_attempt *= 2;
                }
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_BB_DMA_CLEAN_EPOCH_DIAG] "
                    "axis=%u attempts=%llu wouldSkip=%llu rejectNoBaseline=%llu "
                    "rejectCpuPublicationChanged=%llu "
                    "rejectMappingChanged=%llu rejectTopologyChanged=%llu "
                    "rejectTransferSerialChanged=%llu "
                    "rejectPendingTransfers=%llu rejectFexWriterActivity=%llu "
                    "rejectPriorSyncFailed=%llu "
                    "stableSweeps=%llu unstableSweeps=%llu "
                    "fullSyncFailures=%llu transferSerialChanges=%llu "
                    "cpuSerial=%llu mappingSerial=%llu topologySerial=%llu "
                    "transferSerial=%llu baselineValid=%u",
                    static_cast<unsigned>(dma_clean_epoch_diag_axis),
                    static_cast<unsigned long long>(diag.attempts),
                    static_cast<unsigned long long>(diag.would_skip),
                    static_cast<unsigned long long>(diag.reject_no_baseline),
                    static_cast<unsigned long long>(
                        diag.reject_cpu_publication_changed),
                    static_cast<unsigned long long>(
                        diag.reject_mapping_changed),
                    static_cast<unsigned long long>(
                        diag.reject_topology_changed),
                    static_cast<unsigned long long>(
                        diag.reject_transfer_serial_changed),
                    static_cast<unsigned long long>(
                        diag.reject_pending_transfers),
                    static_cast<unsigned long long>(
                        diag.reject_fex_writer_activity),
                    static_cast<unsigned long long>(
                        diag.reject_prior_sync_failed),
                    static_cast<unsigned long long>(diag.stable_sweeps),
                    static_cast<unsigned long long>(diag.unstable_sweeps),
                    static_cast<unsigned long long>(diag.full_sync_failures),
                    static_cast<unsigned long long>(
                        diag.transfer_serial_changes),
                    static_cast<unsigned long long>(after.cpu_publication),
                    static_cast<unsigned long long>(mapping_after),
                    static_cast<unsigned long long>(after.topology),
                    static_cast<unsigned long long>(transfer_after),
                    diag.baseline_valid ? 1u : 0u);
            };
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("before-dma-sync-sweep");
#endif
        u32 dma_sync_range_index = 0;
        for (const auto& [range_address, range_size] : dma_mapped_ranges) {
            if (!buffer_cache.SynchronizeBuffersInRange(
                    range_address, range_size, &dma_snapshot_guard,
                    compute_diagnostic_id, dma_sync_range_index)) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                finish_dma_clean_epoch_diagnostic(false);
#endif
                return false;
            }
            ++dma_sync_range_index;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        log_compute_resource_phase("after-dma-sync-sweep");
#endif
        fault_process_pending = true;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        finish_dma_clean_epoch_diagnostic(true);
#endif
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    log_compute_resource_phase("complete");
#endif
    return true;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    if (!info.has_bitwise_xor) {
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    if (instance.IsMaintenance8Supported() ||
        src_image.info.props.is_depth == dst_image.info.props.is_depth) {
        dst_image.CopyImage(src_image);
    } else {
        const auto& copy_buffer =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::DeviceLocal);
        dst_image.CopyImageWithBuffer(src_image, copy_buffer.Handle(), 0);
    }
    dst_image.flags |= VideoCore::ImageFlagBits::GpuModified;
    dst_image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    image1.Clear(clear, range);
    image1.flags |= VideoCore::ImageFlagBits::GpuModified;
    image1.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    ExecutorDrawStageScope draw_stage_scope{ExecutorDrawCpuStage::BindBuffers};
    buffer_bindings.clear();
    const bool require_storage_buffer_read_write_barrier =
        instance.GetCapabilityProfile().HasQuirk(
            CapabilityQuirk::RequireStorageBufferReadWriteBarrier);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool exact_read_only_reuse_requested =
        VideoCore::IsManagedVulkanExactReadOnlyBufferBindReuse();
    const bool exact_read_only_reuse =
        exact_read_only_reuse_requested &&
        !Libraries::GnmDriver::ExecutorReplayActive() &&
        !Libraries::GnmDriver::ExecutorGnmCaptureActive();
    const bool exec_live_resource_diag =
        ExecutorTraceLivePresentResourcesEnabled() &&
        !Libraries::GnmDriver::ExecutorReplayActive();
    static int exec_live_cbuf_log_budget = 96;
#endif

    for (const auto& desc : stage.buffers) {
        auto vsharp = desc.GetSharp(stage);
        if (Libraries::GnmDriver::ExecutorReplayActive() && !desc.IsSpecial() &&
            vsharp.base_address != 0) {
            const u64 nb = Libraries::GnmDriver::ExecutorReplayRelocate(vsharp.base_address);
            if (nb) {
                vsharp.base_address = nb;
            }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            const u64 cb = vsharp.base_address;
            u32 c0 = 0, c1 = 0, c2 = 0, c3 = 0;
            if (Libraries::GnmDriver::ExecutorReplayIsRelocated(cb)) {
                const u32* p = reinterpret_cast<const u32*>(cb);
                c0 = p[0]; c1 = p[1]; c2 = p[2]; c3 = p[3];
            }
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                                "[EXECUTOR_CBUF] stage=%u base=0x%llx size=%u stride=%u buf=%08x %08x "
                                "%08x %08x",
                                (unsigned)stage.stage, (unsigned long long)cb, (unsigned)vsharp.GetSize(),
                                (unsigned)vsharp.stride, c0, c1, c2, c3);
#endif
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (exec_live_resource_diag && exec_live_cbuf_log_budget > 0 && !desc.IsSpecial() &&
            vsharp.base_address != 0) {
            u32 c0 = 0, c1 = 0, c2 = 0, c3 = 0;
            const u64 cb = vsharp.base_address;
            const u32 sample_size = std::min<u32>(16u, vsharp.GetSize());
            const bool mapped = sample_size != 0 && memory->IsValidMapping(cb, sample_size);
            if (mapped) {
                u8 tmp[16]{};
                memory->CopySparseMemory(cb, tmp, sample_size);
                std::memcpy(&c0, tmp + 0, sizeof(c0));
                std::memcpy(&c1, tmp + 4, sizeof(c1));
                std::memcpy(&c2, tmp + 8, sizeof(c2));
                std::memcpy(&c3, tmp + 12, sizeof(c3));
            }
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                                "[EXECUTOR_LIVE_CBUF] stage=%u sharpIdx=%u type=%u base=0x%llx "
                                "size=%u stride=%u mapped=%d first=%08x %08x %08x %08x",
                                (unsigned)stage.stage, (unsigned)desc.sharp_idx,
                                (unsigned)desc.buffer_type, (unsigned long long)cb,
                                (unsigned)vsharp.GetSize(), (unsigned)vsharp.stride,
                                mapped ? 1 : 0, c0, c1, c2, c3);
            --exec_live_cbuf_log_budget;
        }
#endif
        if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
            const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
            const auto buffer_id =
                desc.is_written || desc.is_formatted
                    ? buffer_cache.FindBuffer(vsharp.base_address, static_cast<u32>(size))
                    : VideoCore::BufferId{};
            buffer_bindings.emplace_back(buffer_id, vsharp, size);
        } else {
            buffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp, 0);
        }
    }

    for (u32 i = 0; i < buffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp, size] = buffer_bindings[i];
        const auto& desc = stage.buffers[i];
        const bool is_storage = desc.IsStorage(vsharp);
        const u32 alignment =
            is_storage ? instance.StorageMinAlignment() : instance.UniformMinAlignment();
        const bool has_guest_range =
            !desc.IsSpecial() && vsharp.base_address != 0 && size != 0;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (exact_read_only_reuse && (desc.is_written || desc.is_formatted)) {
            // A write-capable binding can change access/barrier state for an
            // overlapping backing. Do not reuse any resolution across it.
            exact_read_only_buffer_bindings.clear();
        }
        const bool exact_reuse_candidate = exact_read_only_reuse && has_guest_range &&
                                           !desc.is_written && !desc.is_formatted;
        if (exact_reuse_candidate) {
            const auto cached = std::ranges::find_if(
                exact_read_only_buffer_bindings, [&](const auto& entry) {
                    return entry.guest_address == vsharp.base_address && entry.size == size &&
                           entry.is_storage == is_storage;
                });
            if (cached != exact_read_only_buffer_bindings.end()) {
                if (cached->add_push_offset) {
                    push_data.AddOffset(binding.buffer, cached->push_offset);
                }
                buffer_infos.emplace_back(cached->descriptor);
                set_writes.push_back({
                    .dstSet = VK_NULL_HANDLE,
                    .dstBinding = binding.unified++,
                    .dstArrayElement = 0,
                    .descriptorCount = 1,
                    .descriptorType = is_storage ? vk::DescriptorType::eStorageBuffer
                                                 : vk::DescriptorType::eUniformBuffer,
                    .pBufferInfo = &buffer_infos.back(),
                });
                ++binding.buffer;
                continue;
            }
        }
        u32 exact_push_offset = 0;
        bool exact_add_push_offset = false;
#endif
        if (!has_guest_range) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const auto flattened_user_data = stage.FlattenedUserData();
                const u32 ubo_size = flattened_user_data.size() * sizeof(u32);
                const u64 offset =
                    vk_buffer.Copy(flattened_user_data.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (Shader::IsBdaPageTableBufferType(desc.buffer_type)) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                const auto& bda_policy =
                    instance.GetCapabilityProfile().memory_policy.bda_page_table;
                const u64 segment_index =
                    Shader::BdaPageTableSegmentIndex(desc.buffer_type);
                const bool segmented =
                    bda_policy.mode == BdaPageTableMode::SegmentedSparse;
                const u64 descriptor_offset =
                    segmented ? segment_index * bda_policy.descriptor_segment_size : 0;
                const u64 descriptor_range =
                    segmented ? bda_policy.descriptor_segment_size
                              : bda_buffer->SizeBytes();
                ASSERT_MSG(descriptor_offset + descriptor_range <=
                               bda_buffer->SizeBytes(),
                           "BDA page-table segment exceeds logical buffer");
                buffer_infos.emplace_back(bda_buffer->Handle(), descriptor_offset,
                                          descriptor_range);
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                const auto& cs_program = liverpool->GetCsRegs();
                const u64 lds_size = static_cast<u64>(cs_program.SharedMemSize()) *
                                     static_cast<u64>(cs_program.NumWorkgroups());

                auto& utility_lds =
                    buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                std::unique_ptr<VideoCore::StreamBuffer> dedicated_lds;
                VideoCore::StreamBuffer* lds_buffer = &utility_lds;
                if (lds_size > utility_lds.SizeBytes()) {
                    dedicated_lds = std::make_unique<VideoCore::StreamBuffer>(
                        instance, scheduler, VideoCore::MemoryUsage::Stream, lds_size);
                    lds_buffer = dedicated_lds.get();
                }

                const auto [data, offset] = lds_buffer->Map(lds_size, alignment);
                ASSERT_MSG(data != nullptr,
                           "Failed to reserve {} bytes for shared-memory storage fallback",
                           lds_size);
                std::memset(data, 0, lds_size);
                lds_buffer->Commit();
                buffer_infos.emplace_back(lds_buffer->Handle(), offset, lds_size);
                if (auto barrier = lds_buffer->GetBarrier(
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        vk::PipelineStageFlagBits2::eComputeShader,
                        static_cast<u32>(offset))) {
                    buffer_barriers.emplace_back(*barrier);
                }
                if (dedicated_lds) {
                    scheduler.DeferOperation(
                        [buffer = std::move(dedicated_lds)]() mutable { buffer.reset(); });
                }
            } else if (instance.IsNullDescriptorSupported()) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                auto& null_buffer = buffer_cache.GetBuffer(VideoCore::NULL_BUFFER_ID);
                buffer_infos.emplace_back(null_buffer.Handle(), 0, VK_WHOLE_SIZE);
            }
        } else {
            const auto [vk_buffer, offset] = ExecutorDrawProfileObtain([&] {
                if (!desc.is_written && !desc.is_formatted) {
                    const auto usage =
                        is_storage ? VideoCore::BufferCache::ReadOnlyBufferUsage::Storage
                                   : VideoCore::BufferCache::ReadOnlyBufferUsage::Uniform;
                    return buffer_cache.ObtainReadOnlyBuffer(
                        vsharp.base_address, static_cast<u32>(size), usage, buffer_id);
                }
                return buffer_cache.ObtainBuffer(
                    vsharp.base_address, static_cast<u32>(size), desc.is_written,
                    desc.is_formatted, buffer_id);
            });
            const bool renamed_read_only =
                !desc.is_written && !desc.is_formatted &&
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                buffer_cache.IsReadOnlySnapshotBuffer(vk_buffer);
#else
                vk_buffer == &buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
#endif
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            ASSERT(adjust % 4 == 0);
            bool bound_aligned_scratch = false;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            const bool needs_aligned_scratch =
                is_storage && !desc.is_written && adjust != 0 &&
                ExecutorAlignedSsboScratchEnabled();
            if (needs_aligned_scratch) {
                auto& stream_scratch =
                    buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                auto& scratch = vk_buffer->Handle() == stream_scratch.Handle()
                                    ? buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Upload)
                                    : stream_scratch;
                const auto [scratch_data, scratch_offset] = scratch.Map(size, alignment);
                if (scratch_data != nullptr) {
                    scratch.Commit();
                    scheduler.EndRendering(Scheduler::EndRenderingReason::RasterizerCopy);
                    const auto cmdbuf = scheduler.CommandBuffer();

                    boost::container::small_vector<vk::BufferMemoryBarrier2, 2> pre_barriers;
                    if (auto barrier = vk_buffer->GetBarrier(
                            vk::AccessFlagBits2::eTransferRead,
                            vk::PipelineStageFlagBits2::eTransfer, offset)) {
                        pre_barriers.emplace_back(*barrier);
                    }
                    if (auto barrier = scratch.GetBarrier(
                            vk::AccessFlagBits2::eTransferWrite,
                            vk::PipelineStageFlagBits2::eTransfer,
                            static_cast<u32>(scratch_offset))) {
                        pre_barriers.emplace_back(*barrier);
                    }
                    if (!pre_barriers.empty()) {
                        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                            .bufferMemoryBarrierCount =
                                static_cast<u32>(pre_barriers.size()),
                            .pBufferMemoryBarriers = pre_barriers.data(),
                        });
                    }
                    cmdbuf.copyBuffer(vk_buffer->Handle(), scratch.Handle(),
                                      vk::BufferCopy{
                                          .srcOffset = offset,
                                          .dstOffset = scratch_offset,
                                          .size = size,
                                      });
                    if (auto barrier = scratch.GetBarrier(
                            vk::AccessFlagBits2::eShaderRead,
                            vk::PipelineStageFlagBits2::eAllCommands,
                            static_cast<u32>(scratch_offset))) {
                        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
                            .bufferMemoryBarrierCount = 1,
                            .pBufferMemoryBarriers = &*barrier,
                        });
                    }

                    buffer_infos.emplace_back(scratch.Handle(), scratch_offset, size);
                    bound_aligned_scratch = true;
                    if (ExecutorVulkanOptimizationTelemetryEnabled()) {
                        static std::atomic<u32> scratch_log_budget{64};
                        u32 remaining =
                            scratch_log_budget.load(std::memory_order_relaxed);
                        while (remaining != 0 &&
                               !scratch_log_budget.compare_exchange_weak(
                                   remaining, remaining - 1,
                                   std::memory_order_relaxed)) {
                        }
                        if (remaining != 0) {
                            __android_log_print(
                                ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_ALIGNED_SSBO_SCRATCH] binding=%u guest=0x%llx bytes=%llu "
                                "sourceOffset=0x%x originalAdjust=0x%x scratchOffset=0x%llx align=%u",
                                binding.buffer,
                                static_cast<unsigned long long>(vsharp.base_address),
                                static_cast<unsigned long long>(size), offset, adjust,
                                static_cast<unsigned long long>(scratch_offset),
                                alignment);
                        }
                    }
                }
            }
#endif
            if (!bound_aligned_scratch) {
                push_data.AddOffset(binding.buffer, adjust);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
                exact_push_offset = adjust;
                exact_add_push_offset = true;
#endif
                buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned, size + adjust);
                if (!renamed_read_only) {
                    const vk::AccessFlags2 shader_access =
                        desc.is_written
                            ? (require_storage_buffer_read_write_barrier && is_storage
                                   ? vk::AccessFlagBits2::eShaderRead |
                                         vk::AccessFlagBits2::eShaderWrite
                                   : vk::AccessFlagBits2::eShaderWrite)
                            : vk::AccessFlagBits2::eShaderRead;
                    if (auto barrier =
                            vk_buffer->GetBarrier(shader_access,
                                                  vk::PipelineStageFlagBits2::eAllCommands)) {
                        buffer_barriers.emplace_back(*barrier);
                    }
                }
            }
            if (desc.is_written) {
                // Upstream 1478b91: raw storage-buffer writes also invalidate
                // cached images that alias the same guest byte range.
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
            }
        }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (exact_reuse_candidate) {
            exact_read_only_buffer_bindings.push_back({
                .guest_address = vsharp.base_address,
                .size = size,
                .descriptor = buffer_infos.back(),
                .push_offset = exact_push_offset,
                .add_push_offset = exact_add_push_offset,
                .is_storage = is_storage,
            });
        }
#endif
        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = is_storage ? vk::DescriptorType::eStorageBuffer
                                         : vk::DescriptorType::eUniformBuffer,
            .pBufferInfo = &buffer_infos.back(),
        });
        ++binding.buffer;
    }
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    ExecutorDrawStageScope texture_stage_scope{ExecutorDrawCpuStage::BindTextures};
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool exec_live_resource_diag =
        ExecutorTraceLivePresentResourcesEnabled() &&
        !Libraries::GnmDriver::ExecutorReplayActive();
    static int exec_live_tsharp_log_budget = 96;
    static int exec_replay_tsharp_log_budget = 512;
    const auto resolve_physical_drs_alias = [&](const VideoCore::ImageId image_id) {
        if (!executor_physical_drs_active || !image_id) {
            return;
        }
        const auto& sampled = texture_cache.GetImage(image_id);
        bool aliases = false;
        for (u32 index = 0;
             !aliases && index < AmdGpu::NUM_COLOR_BUFFERS; ++index) {
            if ((executor_physical_drs_color_mask & (1u << index)) == 0 ||
                !executor_physical_drs_color_image_ids[index]) {
                continue;
            }
            const auto& color =
                texture_cache.GetImage(
                    executor_physical_drs_color_image_ids[index]);
            aliases =
                image_id ==
                    executor_physical_drs_color_image_ids[index] ||
                (color.image_uid ==
                     executor_physical_drs_color_image_uids[index] &&
                 sampled.Overlaps(color.info.guest_address,
                                  color.info.guest_size));
        }
        if (!aliases && executor_physical_drs_depth_image_id) {
            const auto& depth =
                texture_cache.GetImage(executor_physical_drs_depth_image_id);
            aliases =
                image_id == executor_physical_drs_depth_image_id ||
                (depth.image_uid == executor_physical_drs_depth_image_uid &&
                 sampled.Overlaps(depth.info.guest_address,
                                  depth.info.guest_size));
        }
        if (aliases) {
            ExecutorResolvePhysicalDrs("sampled_image_alias");
        }
    };
    std::optional<VideoCore::TextureCache::ExactSurfaceBindBatch>
        exact_surface_bind_batch;
    const bool exact_surface_bind_batch_allowed =
        VideoCore::IsManagedVulkanExactSurfaceBindBatch() &&
        !scheduler.HasPendingBufferTransfers() &&
        !executor_physical_drs_active && !exec_live_resource_diag &&
        !texture_cache.ExecutorGpuContentProbeEnabled() &&
        !ExecutorAndroidRendererProbesEnabled() &&
        !Libraries::GnmDriver::ExecutorReplayActive() &&
        !Libraries::GnmDriver::ExecutorGnmCaptureActive();
    if (exact_surface_bind_batch_allowed) {
        exact_surface_bind_batch.emplace(
            texture_cache.BeginExactSurfaceBindBatch());
    }
#endif

    u32 image_slot = 0;
    for (const auto& image_desc : stage.images) {
        auto tsharp = image_desc.GetSharp(stage);
        u64 old_byte = static_cast<u64>(tsharp.base_address) << 8;
        u64 new_byte = 0;
        bool reloc_hit = false;
        if (Libraries::GnmDriver::ExecutorReplayActive() && tsharp.base_address != 0) {
            new_byte = Libraries::GnmDriver::ExecutorReplayRelocate(old_byte);
            reloc_hit = new_byte != 0;
            if (new_byte) {
                tsharp.base_address = new_byte >> 8;
            }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                                "[EXECUTOR_TSHARP] sharpIdx=%u old=0x%llx new=0x%llx relocHit=%d fmt=%u "
                                "w=%u h=%u",
                                (unsigned)image_desc.sharp_idx, (unsigned long long)old_byte,
                                (unsigned long long)new_byte, new_byte ? 1 : 0,
                                static_cast<u32>(tsharp.GetDataFmt()),
                                (unsigned)tsharp.width, (unsigned)tsharp.height);
#endif
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (exec_live_resource_diag && exec_live_tsharp_log_budget > 0) {
            const u64 byte_addr = static_cast<u64>(tsharp.base_address) << 8;
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                                "[EXECUTOR_LIVE_TSHARP] stage=%u hash=0x%llx sharpIdx=%u base=0x%llx fmt=%u "
                                "w=%u h=%u pitch=%u mips=%u valid=%d",
                                (unsigned)stage.stage, (unsigned long long)stage.pgm_hash,
                                (unsigned)image_desc.sharp_idx,
                                (unsigned long long)byte_addr,
                                static_cast<u32>(tsharp.GetDataFmt()), (unsigned)tsharp.width,
                                (unsigned)tsharp.height, (unsigned)tsharp.pitch,
                                (unsigned)tsharp.NumLevels(), tsharp.Valid() ? 1 : 0);
            --exec_live_tsharp_log_budget;
        }
#endif
        if (texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        if (tsharp.Address() == 0 || tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (texture_cache.ExecutorGpuContentProbeEnabled() &&
                stage.stage == Shader::Stage::Compute && image_desc.is_written) {
                static std::atomic<u32> invalid_storage_logs{0};
                const u32 ordinal = invalid_storage_logs.fetch_add(1, std::memory_order_relaxed);
                if (ordinal < 96) {
                    __android_log_print(
                        ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_GPU_STORAGE_BIND] ordinal=%u status=invalid hash=0x%llx "
                        "sharpIdx=%u addr=0x%llx fmt=%u extent=%ux%ux%u pitch=%u levels=%u",
                        ordinal + 1, static_cast<unsigned long long>(stage.pgm_hash),
                        static_cast<u32>(image_desc.sharp_idx),
                        static_cast<unsigned long long>(tsharp.Address()),
                        static_cast<u32>(tsharp.GetDataFmt()), tsharp.width, tsharp.height,
                        tsharp.depth, tsharp.pitch, tsharp.NumLevels());
                }
            }
            if (Libraries::GnmDriver::ExecutorReplayActive() &&
                (exec_replay_tsharp_log_budget > 0 || image_desc.sharp_idx == 16)) {
                __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                                    "[EXECUTOR_BIND_IMAGE] imageSlot=%u sharpIdx=%u oldBase=0x%llx "
                                    "newBase=0x%llx relocHit=%d fmt=%u w=%u h=%u imageId=0 "
                                    "readbackNonzero=0 nonzeroBytes=0 invalidFmt=%u nullAddr=%u",
                                    (unsigned)image_slot, (unsigned)image_desc.sharp_idx,
                                    (unsigned long long)old_byte, (unsigned long long)new_byte,
                                    reloc_hit ? 1 : 0, static_cast<u32>(tsharp.GetDataFmt()),
                                    (unsigned)tsharp.width, (unsigned)tsharp.height,
                                    tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid ? 1u
                                                                                           : 0u,
                                    tsharp.Address() == 0 ? 1u : 0u);
                --exec_replay_tsharp_log_budget;
            }
#endif
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            image_descriptor_array_sizes.push_back(1);
            ++image_slot;
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);

        for (u32 mip_index = 0; mip_index < num_bindings; ++mip_index) {
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp, image_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += mip_index;
                desc.view_info.range.extent.levels = 1;
            }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (exact_surface_bind_batch &&
                exact_surface_bind_batch->IsActive()) {
                image_id = texture_cache.FindImageExactHot(
                    desc, *exact_surface_bind_batch);
                if (!image_id) {
                    // A miss can create, merge, expand or retire an image.
                    // End the short exact-only batch before entering that
                    // unchanged full path, and keep it disabled for the rest
                    // of this stage.
                    exact_surface_bind_batch->End();
                    image_id = texture_cache.FindImage(desc);
                }
            } else {
                image_id = texture_cache.FindImage(desc);
            }
#else
            image_id = texture_cache.FindImage(desc);
#endif
            auto* image = &texture_cache.GetImage(image_id);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (texture_cache.ExecutorGpuContentProbeEnabled() &&
                stage.stage == Shader::Stage::Compute && image_desc.is_written) {
                static std::atomic<u32> valid_storage_logs{0};
                const u32 ordinal = valid_storage_logs.fetch_add(1, std::memory_order_relaxed);
                if (ordinal < 96) {
                    std::array<u8, 32> cpu_bytes{};
                    memory->CopySparseMemory(tsharp.Address(), cpu_bytes.data(), cpu_bytes.size());
                    std::array<u64, 4> cpu_words{};
                    std::memcpy(cpu_words.data(), cpu_bytes.data(), cpu_bytes.size());
                    __android_log_print(
                        ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_GPU_STORAGE_BIND] ordinal=%u status=valid hash=0x%llx "
                        "sharpIdx=%u addr=0x%llx fmt=%u extent=%ux%ux%u pitch=%u levels=%u "
                        "imageId=%u flags=0x%x usage=%u,%u,%u cpu=%016llx,%016llx,%016llx,%016llx",
                        ordinal + 1, static_cast<unsigned long long>(stage.pgm_hash),
                        static_cast<u32>(image_desc.sharp_idx),
                        static_cast<unsigned long long>(tsharp.Address()),
                        static_cast<u32>(tsharp.GetDataFmt()), tsharp.width, tsharp.height,
                        tsharp.depth, tsharp.pitch, tsharp.NumLevels(), image_id.index,
                        static_cast<u32>(image->flags), image->usage.texture,
                        image->usage.storage, image->usage.render_target,
                        static_cast<unsigned long long>(cpu_words[0]),
                        static_cast<unsigned long long>(cpu_words[1]),
                        static_cast<unsigned long long>(cpu_words[2]),
                        static_cast<unsigned long long>(cpu_words[3]));
                }
            }

        // Normal binding path: runs for every pass, unlike the DRS-only helper
        // where this probe first sat and therefore never fired.
        if (ExecutorAndroidRendererProbesEnabled()) {
            g_attach_class.note(image->info.size.width, image->info.size.height,
                                static_cast<std::uint32_t>(image->info.pixel_format),
                                std::uint32_t(image->usage.texture) |
                                (std::uint32_t(image->usage.storage) << 1) |
                                (std::uint32_t(image->usage.render_target) << 2) |
                                (std::uint32_t(image->usage.depth_target) << 3) |
                                (std::uint32_t(image->usage.vo_surface) << 4));
        }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (exec_live_resource_diag && exec_live_tsharp_log_budget > 0) {
                __android_log_print(
                    ANDROID_LOG_INFO, "EXECUTOR",
                    "[EXECUTOR_LIVE_IMAGE_BIND] stage=%u hash=0x%llx slot=%u sharpIdx=%u "
                    "imageId=%u guest=0x%llx bytes=0x%x imageFmt=%u extent=%ux%ux%u "
                    "pitch=%u levels=%u layers=%u tile=%u flags=0x%x block=%u "
                    "usageTexture=%u usageStorage=%u usageRt=%u",
                    (unsigned)stage.stage, (unsigned long long)stage.pgm_hash,
                    (unsigned)image_slot, (unsigned)image_desc.sharp_idx,
                    (unsigned)image_id.index,
                    (unsigned long long)image->info.guest_address,
                    (unsigned)image->info.guest_size,
                    (unsigned)image->info.pixel_format,
                    (unsigned)image->info.size.width, (unsigned)image->info.size.height,
                    (unsigned)image->info.size.depth, (unsigned)image->info.pitch,
                    (unsigned)image->info.resources.levels,
                    (unsigned)image->info.resources.layers,
                    (unsigned)image->info.tile_mode, (unsigned)image->flags,
                    image->info.props.is_block ? 1u : 0u, image->usage.texture,
                    image->usage.storage, image->usage.render_target);
            }
#endif
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (Libraries::GnmDriver::ExecutorReplayActive() &&
                (exec_replay_tsharp_log_budget > 0 || image_desc.sharp_idx == 16)) {
                const u64 nonzero_bytes = ExecutorCountNonzeroSparse(
                    memory, image->info.guest_address, image->info.guest_size);
                __android_log_print(
                    ANDROID_LOG_INFO, "EXECUTOR",
                    "[EXECUTOR_BIND_IMAGE] imageSlot=%u sharpIdx=%u oldBase=0x%llx "
                    "newBase=0x%llx relocHit=%d fmt=%u w=%u h=%u imageId=%u "
                    "readbackNonzero=%d nonzeroBytes=%llu guest=0x%llx guestSize=%llu "
                    "pitch=%u levels=%u layers=%u type=%u baseLevel=%u lastLevel=%u "
                    "baseArray=%u lastArray=%u dstSel=%u,%u,%u,%u isArray=%d",
                    (unsigned)image_slot, (unsigned)image_desc.sharp_idx,
                    (unsigned long long)old_byte, (unsigned long long)new_byte,
                    reloc_hit ? 1 : 0, static_cast<u32>(tsharp.GetDataFmt()),
                    (unsigned)tsharp.width, (unsigned)tsharp.height,
                    static_cast<unsigned>(image_id.index), nonzero_bytes ? 1 : 0,
                    (unsigned long long)nonzero_bytes,
                    (unsigned long long)image->info.guest_address,
                    (unsigned long long)image->info.guest_size, (unsigned)image->info.pitch,
                    (unsigned)image->info.resources.levels,
                    (unsigned)image->info.resources.layers, (unsigned)tsharp.type,
                    (unsigned)tsharp.base_level, (unsigned)tsharp.last_level,
                    (unsigned)tsharp.base_array, (unsigned)tsharp.last_array,
                    (unsigned)tsharp.dst_sel_x, (unsigned)tsharp.dst_sel_y,
                    (unsigned)tsharp.dst_sel_z, (unsigned)tsharp.dst_sel_w,
                    image_desc.is_array ? 1 : 0);
                --exec_replay_tsharp_log_budget;
            }
#endif
            if (Libraries::GnmDriver::ExecutorGnmCaptureActive()) {
                Libraries::GnmDriver::ExecutorGnmCaptureRecordResource(
                    4u, image->info.guest_address, image->info.guest_size);
            }
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            resolve_physical_drs_alias(image_id);
#endif
            if (image->binding.is_bound) {
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
            ++image_slot;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // FindTexture owns the same mutex and must retain its original independent
    // refresh/lifetime transaction.  Never carry the exact lookup batch into
    // the view-resolution pass below.
    if (exact_surface_bind_batch) {
        exact_surface_bind_batch->End();
    }
#endif

    for (auto& [image_id, desc] : image_bindings) {
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            if (instance.IsNullDescriptorSupported()) {
                image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            } else {
                auto* null_image_view = texture_cache.FindTexture(VideoCore::NULL_IMAGE_ID, desc);
                ASSERT(null_image_view != nullptr);
                image_infos.emplace_back(VK_NULL_HANDLE, *null_image_view->image_view,
                                         vk::ImageLayout::eGeneral);
            }
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
            }

            auto* image_view = texture_cache.FindTexture(image_id, desc);
            if (image_view == nullptr) {
                image_id = texture_cache.FindImage(desc);
                if (image_id) {
                    image_view = texture_cache.FindTexture(image_id, desc);
                }
            }
            if (image_view == nullptr) {
                if (instance.IsNullDescriptorSupported()) {
                    image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE,
                                             vk::ImageLayout::eGeneral);
                    continue;
                }
                image_id = VideoCore::NULL_IMAGE_ID;
                image_view = texture_cache.FindTexture(image_id, desc);
                ASSERT(image_view != nullptr);
            }
            bound_images.emplace_back(image_id);
            auto& image = texture_cache.GetImage(image_id);
            if ((image.binding.force_general || image.binding.is_target) &&
                !image.info.props.is_depth) {
                image.Transit(instance.IsAttachmentFeedbackLoopLayoutSupported() &&
                                      image.binding.is_target
                                  ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                  : vk::ImageLayout::eGeneral,
                              vk::AccessFlagBits2::eShaderRead |
                                  (image.info.props.is_depth
                                       ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                                       : vk::AccessFlagBits2::eColorAttachmentWrite |
                                             vk::AccessFlagBits2::eColorAttachmentRead),
                              {});
            } else {
                if (is_storage) {
                    image.Transit(vk::ImageLayout::eGeneral,
                                  vk::AccessFlagBits2::eShaderRead |
                                      vk::AccessFlagBits2::eShaderWrite,
                                  desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    image.Transit(new_layout, vk::AccessFlagBits2::eShaderRead,
                                  desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            if (texture_cache.ExecutorGpuContentProbeEnabled()) {
                auto& probes = is_storage ? executor_gpu_probe_storage_images
                                          : executor_gpu_probe_sampled_images;
                bool already_recorded = false;
                for (const auto& probe : probes) {
                    if (probe.image_id == image_id &&
                        probe.stage == static_cast<u32>(stage.stage) &&
                        probe.stage_hash == stage.pgm_hash) {
                        already_recorded = true;
                        break;
                    }
                }
                if (!already_recorded) {
                    probes.push_back({
                        image_id,
                        static_cast<u32>(stage.stage),
                        stage.pgm_hash,
                        image_view->info,
                        image.info.pixel_format,
                        instance.GetSupportedFormat(image_view->info.format,
                                                    image.format_features),
                        image.aspect_mask,
                        reinterpret_cast<u64>(
                            static_cast<VkImageView>(*image_view->image_view)),
                    });
                }
            }

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view->image_view,
                                     image.backing->state.layout);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (const u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified,
            .dstArrayElement = 0,
            .descriptorCount = array_size,
            .descriptorType =
                is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage,
            .pImageInfo = &image_infos[image_info_idx],
        });

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    texture_stage_scope.End();
    ExecutorDrawStageScope sampler_stage_scope{ExecutorDrawCpuStage::BindSamplers};
    u32 sampler_slot = 0;
    for (const auto& sampler : stage.samplers) {
        auto ssharp = sampler.GetSharp(stage);
        if (sampler.disable_aniso) {
            const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (Libraries::GnmDriver::ExecutorReplayActive()) {
            const u32 associated_image = sampler.associated_image;
            const u32 associated_sharp =
                associated_image < stage.images.size() ? stage.images[associated_image].sharp_idx
                                                       : std::numeric_limits<u32>::max();
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                                "[EXECUTOR_BIND_SAMPLER] sampSlot=%u ssharpIdx=%u associatedImage=%u "
                                "associatedSharpIdx=%u clampX=%u clampY=%u clampZ=%u minFilter=%u "
                                "magFilter=%u mipFilter=%u filterMode=%u border=%u forceUnnorm=%u "
                                "minLod=%.2f maxLod=%.2f lodBias=%.2f disableAniso=%u raw0=0x%llx "
                                "raw1=0x%llx",
                                (unsigned)sampler_slot, (unsigned)sampler.sharp_idx,
                                (unsigned)associated_image, (unsigned)associated_sharp,
                                (unsigned)ssharp.clamp_x.Value(), (unsigned)ssharp.clamp_y.Value(),
                                (unsigned)ssharp.clamp_z.Value(),
                                (unsigned)ssharp.xy_min_filter.Value(),
                                (unsigned)ssharp.xy_mag_filter.Value(),
                                (unsigned)ssharp.mip_filter.Value(),
                                (unsigned)ssharp.filter_mode.Value(),
                                (unsigned)ssharp.border_color_type.Value(),
                                (unsigned)ssharp.force_unnormalized.Value(), ssharp.MinLod(),
                                ssharp.MaxLod(), ssharp.LodBias(), sampler.disable_aniso,
                                (unsigned long long)ssharp.raw0, (unsigned long long)ssharp.raw1);
        }
#endif
        const auto vk_sampler = texture_cache.GetSampler(ssharp, liverpool->regs.ta_bc_base, bool(sampler.is_depth));
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        set_writes.push_back({
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = binding.unified++,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampler,
            .pImageInfo = &image_infos.back(),
        });
        ++sampler_slot;
    }
}

void Rasterizer::UpdateAttachmentlessRenderArea(RenderState& state) const {
    const bool has_attachment = state.depth_stencil_attachment.image_view ||
        std::any_of(state.color_attachments.begin(), state.color_attachments.end(),
                    [](const auto& attachment) { return bool(attachment.image_view); });
    if (!has_attachment) {
        // Derive current scissors before sizing the attachmentless pass. Some
        // capability routes publish dynamic state only after pipeline binding;
        // using the previous draw's cached scissors here would clip valid work.
        UpdateViewportScissorState();
        const auto& scissors = scheduler.GetDynamicState().scissors;
        const auto extent = AttachmentlessRenderExtent(
            {scissors.data(), scissors.size()}, {state.width, state.height});
        state.width = static_cast<u16>(extent.width);
        state.height = static_cast<u16>(extent.height);
    }
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    cb_image_uids.fill(0);
    const auto& regs = liverpool->regs;
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state{};
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (ExecutorTracePm4Enabled())
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_DRAW_TRACE] br_enter numColor=%u", state.num_color_attachments);
#endif
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            continue;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorTracePm4Enabled())
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_DRAW_TRACE] br_getimage cb=%u", cb);
#endif
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorTracePm4Enabled())
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_DRAW_TRACE] br_before_updateimage cb=%u", cb);
#endif
        if (!texture_cache.UpdateImage(image_id)) {
            state.color_attachments[cb] = {};
            continue;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorTracePm4Enabled())
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_DRAW_TRACE] br_before_findrt cb=%u", cb);
#endif
        image = &texture_cache.GetImage(image_id);
        image->SetBackingSamples(key.color_samples[cb]);
        const auto* image_view = texture_cache.FindRenderTarget(image_id, desc);
        if (image_view == nullptr) {
            state.color_attachments[cb] = {};
            continue;
        }
        image = &texture_cache.GetImage(image_id);
        cb_image_uids[cb] = image->image_uid;
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (ExecutorTracePm4Enabled())
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_DRAW_TRACE] br_rt_ok cb=%u", cb);
#endif
        const auto slice = image_view->info.range.base.layer;
        const auto mip = image_view->info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            image->Transit(instance.IsAttachmentFeedbackLoopLayoutSupported()
                               ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                               : vk::ImageLayout::eGeneral,
                           vk::AccessFlagBits2::eColorAttachmentWrite, {});
            attachment_feedback_loop = true;
        } else {
            image->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                           vk::AccessFlagBits2::eColorAttachmentWrite |
                               vk::AccessFlagBits2::eColorAttachmentRead,
                           desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view->info.range.extent.layers);
        const bool exec_clear_probe =
            Libraries::GnmDriver::ExecutorReplayActive() && std::getenv("EXECUTOR_CLEAR_PROBE");
        const auto clear_value =
            exec_clear_probe
                ? vk::ClearValue{.color = {.float32 = {{1.0f, 0.0f, 0.0f, 1.0f}}}}
                : (is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{});
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view->image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.format = image_view->info.format;
        attachment.samples = LiverpoolToVK::NumSamples(key.color_samples[cb],
                                                         instance.GetColorSampleCounts());
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear || exec_clear_probe;
        image->usage.render_target = 1u;
        const bool executor_track_present_rt = Libraries::GnmDriver::ExecutorReplayActive();
        if (executor_track_present_rt && cb == 0) {
            texture_cache.executor_replay_color_target = image_id;
            {
                std::scoped_lock lk{texture_cache.executor_replay_candidates_mutex};
                texture_cache.executor_replay_color_candidates.push_back(image_id);
            }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_RT_CAND] mode=%s push id=%u count=%zu extent=%ux%u base=0x%llx",
                                Libraries::GnmDriver::ExecutorReplayActive() ? "replay" : "live",
                                image_id.index, texture_cache.executor_replay_color_candidates.size(),
                                image->info.size.width, image->info.size.height,
                                (unsigned long long)image->info.guest_address);
#endif
        }
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto* image_view = texture_cache.FindDepthTarget(image_id, desc);
        if (image_view == nullptr) {
            state.depth_stencil_attachment = {};
            if (state.num_layers == std::numeric_limits<u16>::max()) {
                state.num_layers = 1;
            }
            return state;
        }
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view->info.range.base.layer;
        const bool is_depth_clear = regs.depth_render_control.depth_clear_enable ||
                                    texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        const bool read_only_depth_stencil_layout =
            new_layout == vk::ImageLayout::eDepthStencilReadOnlyOptimal ||
            new_layout == vk::ImageLayout::eDepthReadOnlyOptimal;
        const bool precise_read_only_access =
            read_only_depth_stencil_layout &&
            instance.GetCapabilityProfile().HasQuirk(
                CapabilityQuirk::ReadOnlyDepthStencilAccess);
        const vk::AccessFlags2 depth_stencil_access =
            precise_read_only_access
                ? vk::AccessFlags2{vk::AccessFlagBits2::eDepthStencilAttachmentRead}
                : vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                      vk::AccessFlagBits2::eDepthStencilAttachmentRead;
        image.Transit(new_layout, depth_stencil_access, desc.view_info.range);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (Libraries::GnmDriver::ExecutorReplayActive()) __android_log_print(ANDROID_LOG_INFO,"EXECUTOR","[EXECUTOR_DRAW_TRACE] depth_after_transit");
#endif

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view->info.range.extent.layers);
        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view->image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.format = image_view->info.format;
        attachment.samples = LiverpoolToVK::NumSamples(key.depth_samples,
                                                         instance.GetDepthSampleCounts());
        attachment.clear_value = {};

        if (regs.depth_buffer.DepthValid()) {
            const bool depth_clear =
                is_depth_clear || Libraries::GnmDriver::ExecutorReplayActive();
            attachment.clear_value[0] = depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    return state;
}

void Rasterizer::Resolve() {
    if (IsDeviceTerminal()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorResolvePhysicalDrs("gnm_resolve");
#endif
    scheduler.FlushPendingBufferTransfers();
    const auto& mrt0_hint = liverpool->last_cb_extent[0];
    const auto& mrt1_hint = liverpool->last_cb_extent[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{liverpool->regs.color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{liverpool->regs.color_buffers[1], mrt1_hint};
    const auto mrt0_id = texture_cache.FindImage(mrt0_desc, true);
    const auto mrt1_id = texture_cache.FindImage(mrt1_desc, true);
    auto& mrt0_image = texture_cache.GetImage(mrt0_id);
    auto& mrt1_image = texture_cache.GetImage(mrt1_id);

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    static const bool trace_resolve_pair =
        std::getenv("EXECUTOR_TRACE_VIDEOOUT_SOURCE") != nullptr ||
        std::getenv("EXECUTOR_TRACE_RESOLVE_PAIR") != nullptr;
    static std::atomic<u64> resolve_sequence{0};
    const u64 resolve_seq = resolve_sequence.fetch_add(1, std::memory_order_relaxed);
    const bool log_resolve =
        trace_resolve_pair && (resolve_seq < 12 || (resolve_seq % 60) == 0);
    if (log_resolve) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_RESOLVE_PAIR] seq=%llu phase=before srcId=%u src=0x%llx "
            "srcExtent=%ux%u srcInfoSamples=%u srcBackingSamples=%u srcFlags=0x%x "
            "dstId=%u dst=0x%llx dstExtent=%ux%u dstInfoSamples=%u "
            "dstBackingSamples=%u dstFlags=0x%x dstUsageVo=%u dstUsageRt=%u "
            "srcAttribSamplesLog2=%u srcAttribFragmentsLog2=%u "
            "dstAttribSamplesLog2=%u dstAttribFragmentsLog2=%u depthSamples=%u",
            static_cast<unsigned long long>(resolve_seq), mrt0_id.index,
            static_cast<unsigned long long>(mrt0_image.info.guest_address),
            mrt0_image.info.size.width, mrt0_image.info.size.height, mrt0_image.info.num_samples,
            mrt0_image.backing ? mrt0_image.backing->num_samples : 0u,
            static_cast<u32>(mrt0_image.flags), mrt1_id.index,
            static_cast<unsigned long long>(mrt1_image.info.guest_address),
            mrt1_image.info.size.width, mrt1_image.info.size.height, mrt1_image.info.num_samples,
            mrt1_image.backing ? mrt1_image.backing->num_samples : 0u,
            static_cast<u32>(mrt1_image.flags), mrt1_image.usage.vo_surface,
            mrt1_image.usage.render_target,
            liverpool->regs.color_buffers[0].attrib.num_samples_log2,
            liverpool->regs.color_buffers[0].attrib.num_fragments_log2,
            liverpool->regs.color_buffers[1].attrib.num_samples_log2,
            liverpool->regs.color_buffers[1].attrib.num_fragments_log2,
            liverpool->regs.depth_buffer.NumSamples());
    }
#endif

    if (Config::getVkHostMarkersEnabled()) {
        ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                     liverpool->regs.color_buffers[0].Address(),
                                     liverpool->regs.color_buffers[1].Address()));
    }
    mrt1_image.Resolve(mrt0_image, mrt0_desc.view_info.range, mrt1_desc.view_info.range);
    ScopeMarkerEnd();
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    texture_cache.ExecutorTrackLiveDrawnColorTarget(mrt1_id,
                                                    mrt1_image.image_uid);
    texture_cache.ExecutorTrackLiveResolvedColorTarget(
        mrt0_id, mrt0_image.image_uid, mrt1_id, mrt1_image.image_uid);
    if (log_resolve) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_RESOLVE_PAIR] seq=%llu phase=recorded srcId=%u srcBackingSamples=%u "
            "dstId=%u dstBackingSamples=%u dstFlags=0x%x",
            static_cast<unsigned long long>(resolve_seq), mrt0_id.index,
            mrt0_image.backing ? mrt0_image.backing->num_samples : 0u, mrt1_id.index,
            mrt1_image.backing ? mrt1_image.backing->num_samples : 0u,
            static_cast<u32>(mrt1_image.flags));
        // A bounded, opt-in source/destination readback is intentionally adjacent to the
        // recorded resolve.  It proves whether corruption already exists in the render target
        // or is introduced by the 1x copy/resolve path; the regular VideoOut trace sentinel is
        // required, and only the first two pairs are downloaded.
        if (resolve_seq < 2) {
            texture_cache.ExecutorTraceVideoOutSource(mrt0_id, 0x20000u + resolve_seq * 2u, 0);
            texture_cache.ExecutorTraceVideoOutSource(mrt1_id,
                                                       0x20001u + resolve_seq * 2u, 0);
        }
    }
#endif
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    if (IsDeviceTerminal()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorResolvePhysicalDrs("depth_stencil_copy");
#endif
    scheduler.FlushPendingBufferTransfers();
    if (!scheduler.HasRecordingCommandBuffer()) {
        return;
    }
    auto& regs = liverpool->regs;

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), liverpool->last_db_extent, true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = liverpool->regs.depth_view.slice_start;
    sub_range.extent.layers = liverpool->regs.depth_view.NumSlices() - sub_range.base.layer;

    if (Config::getVkHostMarkersEnabled()) {
        ScopeMarkerBegin(fmt::format(
            "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}",
            regs.depth_buffer.DepthAddress(), regs.depth_buffer.StencilAddress(),
            regs.depth_buffer.DepthWriteAddress(), regs.depth_buffer.StencilWriteAddress()));
    }

    read_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       sub_range);
    write_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                        sub_range);

    auto aspect_mask = vk::ImageAspectFlags(0);
    if (is_depth) {
        aspect_mask |= vk::ImageAspectFlagBits::eDepth;
    }
    if (is_stencil) {
        aspect_mask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .dstOffset = {0, 0, 0},
        .extent = {write_image.info.size.width, write_image.info.size.height, 1},
    };
    scheduler.CommandBuffer().copyImage(read_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        write_image.GetImage(),
                                        vk::ImageLayout::eTransferDstOptimal, region);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    if (IsDeviceTerminal()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    scheduler.MaybePublishExactA750D9SampledRecordingPressureBoundary("fill-buffer");
    ++executor_completion_work_serial;
#endif
    buffer_cache.FillBuffer(address, num_bytes, value, is_gds);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds,
                            bool defer_gnm_dma) {
    if (IsDeviceTerminal()) {
        return;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    scheduler.MaybePublishExactA750D9SampledRecordingPressureBoundary("copy-buffer");
    ++executor_completion_work_serial;
#endif
    buffer_cache.CopyBuffer(dst, src, num_bytes, dst_gds, src_gds, defer_gnm_dma);
}

void Rasterizer::FlushPendingGnmDmaTransfers() {
    scheduler.FlushPendingGnmDmaTransfers();
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        return false;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorResolvePhysicalDrs("invalidate_memory");
#endif
    pipeline_cache.InvalidateShaderIdentityRange(addr, size);
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::InvalidateMemoryBeforeGuestWrite(VAddr addr, u64 size) {
    if (!IsMapped(addr, size)) {
        return false;
    }

    // Guest write faults are delivered before the faulting instruction is
    // retried. Preserve the current GPU authority for partially overwritten
    // images before the normal cache invalidation marks them CPU-dirty.
    texture_cache.PublishGpuImagesBeforeCpuWrite(addr, size);
    return InvalidateMemory(addr, size);
}

VideoCore::CpuOwnedWritePublishResult
Rasterizer::TryPublishHleCpuOwnedWrite(const VAddr addr, const u64 size) {
    return buffer_cache.TryPublishCpuOwnedWriteIfGpuClean(addr, size);
}

bool Rasterizer::BeginHleCleanBufferWrite(const VAddr addr,
                                          const u64 size) noexcept {
    return pipeline_cache.BeginHleCleanBufferWrite(addr, size);
}

void Rasterizer::EndHleCleanBufferWrite(const VAddr addr,
                                        const u64 size) noexcept {
    pipeline_cache.EndHleCleanBufferWrite(addr, size);
    // Normal-context, lock-free publication only.  This does not enqueue or
    // synchronously wait for a GpuComm command.
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    buffer_cache.RecordDmaCleanEpochCpuPublication();
#endif
    liverpool->NotifyMemoryPublicationForExecutor(addr, size);
}

void Rasterizer::PublishCapturedGuestBufferWritePages(
    const std::span<const VAddr> pages,
    const bool texture_cache_lock_held) {
    for (size_t index = 0; index < pages.size();) {
        const VAddr begin = pages[index];
        VAddr end = begin + VideoCore::TRACKER_BYTES_PER_PAGE;
        while (++index < pages.size() && pages[index] == end) {
            end += VideoCore::TRACKER_BYTES_PER_PAGE;
        }
        const u64 size = end - begin;
        if (!IsMapped(begin, size)) {
            continue;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        buffer_cache.RecordDmaCleanEpochCpuPublication();
#endif
        pipeline_cache.InvalidateShaderIdentityRange(begin, size);
        if (texture_cache_lock_held) {
            texture_cache.InvalidateMemoryLocked(begin, size);
        } else {
            texture_cache.InvalidateMemory(begin, size);
        }
        liverpool->NotifyMemoryPublicationForExecutor(begin, size);
    }
}

bool Rasterizer::IsGpuThread() const noexcept {
    return liverpool != nullptr && liverpool->IsGpuThread();
}

bool Rasterizer::InvalidateMemoryFromGuestThread(VAddr addr, u64 size) {
    return InvalidateMemoryFromGuestThread(addr, size, nullptr, nullptr, 0);
}

bool Rasterizer::InvalidateMemoryBeforeGuestWriteFromGuestThread(VAddr addr,
                                                                  u64 size) {
    return InvalidateMemoryBeforeGuestWriteFromGuestThread(
        addr, size, nullptr, nullptr, 0);
}

bool Rasterizer::FailPendingGuestInvalidationsIfTerminalLocked() noexcept {
    if (liverpool != nullptr && !liverpool->IsRendererTerminal()) {
        return false;
    }

    for (GuestInvalidationRequest* request : pending_guest_invalidations) {
        request->result = false;
        request->retry = false;
        request->complete = true;
    }
    pending_guest_invalidations.clear();
    guest_invalidation_drain_queued = false;
    guest_invalidation_handoff_pending = false;
    guest_invalidation_completion_cv.notify_all();
    return true;
}

bool Rasterizer::InvalidateMemoryFromGuestThread(
    VAddr addr, u64 size, GuestInvalidationValidator validator,
    void* validator_context, u64 validation_token) {
    return InvalidateMemoryFromGuestThreadImpl(
        addr, size, validator, validator_context, validation_token, false);
}

bool Rasterizer::InvalidateMemoryBeforeGuestWriteFromGuestThread(
    VAddr addr, u64 size, GuestInvalidationValidator validator,
    void* validator_context, u64 validation_token) {
    return InvalidateMemoryFromGuestThreadImpl(
        addr, size, validator, validator_context, validation_token, true);
}

bool Rasterizer::InvalidateMemoryFromGuestThreadImpl(
    VAddr addr, u64 size, GuestInvalidationValidator validator,
    void* validator_context, u64 validation_token,
    bool publish_gpu_images) {
    if (liverpool == nullptr || liverpool->IsRendererTerminal()) {
        return false;
    }

    // Invalidation may itself touch a protected guest alias while the drain is
    // running on GpuComm. Joining the active batch deadlocks the only drainer,
    // while recursively entering Vulkan from the signal handler can reacquire
    // the cache lock that the interrupted operation already owns. Publish the
    // range in TLS and consume it immediately after the interrupted invalidation
    // returns instead.
    if (executor_draining_guest_invalidations) {
        ASSERT_MSG(validator == nullptr,
                   "Validated guest invalidation cannot nest in its drainer");
        if (validator != nullptr) {
            return false;
        }
        DeferNestedGuestInvalidation(addr, size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        executor_guest_invalidation_nested.fetch_add(1, std::memory_order_relaxed);
#endif
        return true;
    }
    if (validator != nullptr &&
        !validator(validator_context, addr, size, validation_token)) {
        return false;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_guest_invalidation_profile_enabled =
        ExecutorGuestInvalidationProfileEnabled();
    const bool executor_guest_invalidation_overlap_enabled =
        VideoCore::InvalidationOverlapTrace::Enabled();
    const bool executor_guest_invalidation_timing_enabled =
        executor_guest_invalidation_profile_enabled ||
        executor_guest_invalidation_overlap_enabled;
    const auto wait_begin =
        executor_guest_invalidation_timing_enabled
                                ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    const auto overlap_trace_v2 =
        executor_guest_invalidation_overlap_enabled
            ? VideoCore::InvalidationOverlapTrace::BeginRequestV2(
                  wait_begin, ExecutorVkGpuTimelineCurrentFrameEpoch())
            : VideoCore::InvalidationOverlapTrace::RequestTokenV2{};
#endif
    GuestInvalidationRequest request{
        .addr = addr,
        .size = size,
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        .overlap_trace_v2 = overlap_trace_v2,
#endif
        .publish_gpu_images = publish_gpu_images,
        .validator = validator,
        .validator_context = validator_context,
        .validation_token = validation_token,
    };
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_guest_invalidation_profile_enabled) {
        ExecutorRuntimeProofCounterAdd(
            executor_guest_invalidation_requests, 1,
            LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_REQUESTS);
    }
#endif
    const bool gpu_thread_fault = IsGpuThread();
    for (;;) {
        request.complete = false;
        request.retry = false;
        request.result = false;
        bool queue_drain = false;
        u32 watchdog_rescues = 0;
        {
            std::scoped_lock lock{guest_invalidation_mutex};
            pending_guest_invalidations.push_back(&request);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
            if (executor_guest_invalidation_profile_enabled) {
                UpdateAtomicMaximum(executor_guest_invalidation_queue_max,
                                    pending_guest_invalidations.size());
            }
#endif
            if (!guest_invalidation_drain_queued) {
                guest_invalidation_drain_queued = true;
                queue_drain = true;
            }
        }
        if (gpu_thread_fault) {
            // A host access made by GpuComm can fault while a drain callback
            // queued by another worker is still waiting in GpuComm's own FIFO.
            // Waiting on this stack request would self-join the sole consumer
            // forever.  Drain the immutable snapshot inline; the older queued
            // callback will subsequently observe an empty queue.
            DrainGuestInvalidations();
        } else if (queue_drain) {
            liverpool->SendCommand<false>([this] { DrainGuestInvalidations(); });
        }
        std::unique_lock lock{guest_invalidation_mutex};
        const auto queue_guest_invalidation_handoff = [&] {
            if (!guest_invalidation_handoff_pending) {
                return false;
            }
            // The flag and retained pending queue are protected by the same
            // mutex. Exactly one non-Gpu waiter consumes the baton and queues
            // the next immutable snapshot through Liverpool's normal FIFO.
            guest_invalidation_handoff_pending = false;
            lock.unlock();
            liverpool->SendCommand<false>(
                [this] { DrainGuestInvalidations(); });
            lock.lock();
            return true;
        };
        if (gpu_thread_fault) {
            // DrainGuestInvalidations() is synchronous on GpuComm. Never enter
            // a futex wait from the sole consumer, even if this invariant is
            // accidentally broken by a later queueing change.
            ASSERT_MSG(request.complete,
                       "GpuComm guest invalidation did not complete inline");
        } else {
            if (request.validator == nullptr) {
                while (!request.complete) {
                    guest_invalidation_completion_cv.wait_for(
                        lock, ExecutorGuestInvalidationDrainWatchdogPeriod,
                        [this, &request] {
                            return request.complete ||
                                   guest_invalidation_handoff_pending;
                        });
                    if (FailPendingGuestInvalidationsIfTerminalLocked()) {
                        continue;
                    }
                    if (queue_guest_invalidation_handoff()) {
                        continue;
                    }
                    if (request.complete) {
                        continue;
                    }

                    // The request must still be in the shared pending queue.
                    // Once DrainGuestInvalidations swaps it into an active
                    // batch, another callback could complete and destroy this
                    // stack object before a duplicate callback dereferences it.
                    // Only the front waiter rescues a lost drain so a stalled
                    // queue produces one callback rather than one per waiter.
                    const bool rescue_drain =
                        watchdog_rescues <
                            ExecutorGuestInvalidationMaxWatchdogRescues &&
                        !pending_guest_invalidations.empty() &&
                        pending_guest_invalidations.front() == &request;
                    if (!rescue_drain) {
                        continue;
                    }
                    ++watchdog_rescues;
                    lock.unlock();
                    liverpool->SendCommand<false>(
                        [this] { DrainGuestInvalidations(); });
                    lock.lock();
                }
            } else {
                auto rescue_deadline =
                    std::chrono::steady_clock::now() +
                    ExecutorGuestInvalidationDrainWatchdogPeriod;
                while (!request.complete) {
                    guest_invalidation_completion_cv.wait_for(
                        lock, std::chrono::microseconds{50});
                    if (FailPendingGuestInvalidationsIfTerminalLocked()) {
                        continue;
                    }
                    // Queue the retained generation before a stale validated
                    // waiter is allowed to remove itself from pending. The
                    // already queued callback then drains the remaining peers
                    // or takes the idempotent empty path.
                    if (queue_guest_invalidation_handoff()) {
                        continue;
                    }
                    if (request.complete) {
                        continue;
                    }
                    lock.unlock();
                    const bool current = request.validator(
                        request.validator_context, request.addr, request.size,
                        request.validation_token);
                    lock.lock();
                    // The drain can publish a handoff while validation runs
                    // without the mutex. Relay it before a stale validator can
                    // erase the last retained request and leave the latch with
                    // no producer responsible for the follow-up callback.
                    if (FailPendingGuestInvalidationsIfTerminalLocked()) {
                        continue;
                    }
                    if (queue_guest_invalidation_handoff()) {
                        continue;
                    }
                    if (request.complete) {
                        continue;
                    }
                    if (!current) {
                        const auto it = std::ranges::find(
                            pending_guest_invalidations, &request);
                        if (it != pending_guest_invalidations.end()) {
                            pending_guest_invalidations.erase(it);
                            request.result = false;
                            request.complete = true;
                        }
                    }
                    if (request.complete) {
                        continue;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    const bool rescue_drain =
                        now >= rescue_deadline &&
                        watchdog_rescues <
                            ExecutorGuestInvalidationMaxWatchdogRescues &&
                        !pending_guest_invalidations.empty() &&
                        pending_guest_invalidations.front() == &request;
                    if (!rescue_drain) {
                        continue;
                    }
                    rescue_deadline =
                        now + ExecutorGuestInvalidationDrainWatchdogPeriod;
                    ++watchdog_rescues;
                    lock.unlock();
                    liverpool->SendCommand<false>(
                        [this] { DrainGuestInvalidations(); });
                    lock.lock();
                }
            }
        }
        if (!request.retry) {
            break;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (executor_guest_invalidation_profile_enabled) {
            executor_guest_invalidation_retries.fetch_add(1, std::memory_order_relaxed);
        }
#endif
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_guest_invalidation_timing_enabled) {
        const auto wait_end = std::chrono::steady_clock::now();
        const u64 wait_ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                wait_end - wait_begin)
                .count());
        if (overlap_trace_v2) {
            VideoCore::InvalidationOverlapTrace::EndRequestV2(
                overlap_trace_v2, wait_end, addr, size, request.result);
        } else if (executor_guest_invalidation_overlap_enabled) {
            VideoCore::InvalidationOverlapTrace::RecordWaitSpan(
                wait_begin, wait_end, addr, size, request.result);
        }
        if (executor_guest_invalidation_profile_enabled) {
            ExecutorRuntimeProofCounterAdd(
                executor_guest_invalidation_wait_ns, wait_ns,
                LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_WAIT_NS);
            UpdateAtomicMaximum(executor_guest_invalidation_wait_max_ns, wait_ns);
            const u64 completions =
                ExecutorRuntimeProofCounterAdd(
                    executor_guest_invalidation_completions, 1,
                    LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_COMPLETIONS) +
                1;
            if ((completions & 4095u) == 0 && !VideoCore::IsExecutorLiveQuiet()) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_GUEST_INVALIDATION] completed=%llu requests=%llu batches=%llu "
                    "retries=%llu nested=%llu rounds=%llu catchup=%llu queueMax=%llu "
                    "waitTotalMs=%.3f waitMaxMs=%.3f",
                    static_cast<unsigned long long>(completions),
                    static_cast<unsigned long long>(executor_guest_invalidation_requests.load(
                        std::memory_order_relaxed)),
                    static_cast<unsigned long long>(executor_guest_invalidation_batches.load(
                        std::memory_order_relaxed)),
                    static_cast<unsigned long long>(executor_guest_invalidation_retries.load(
                        std::memory_order_relaxed)),
                    static_cast<unsigned long long>(executor_guest_invalidation_nested.load(
                        std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        executor_guest_invalidation_drain_rounds.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        executor_guest_invalidation_catchup_requests.load(
                            std::memory_order_relaxed)),
                    static_cast<unsigned long long>(executor_guest_invalidation_queue_max.load(
                        std::memory_order_relaxed)),
                    static_cast<double>(executor_guest_invalidation_wait_ns.load(
                        std::memory_order_relaxed)) /
                        1.0e6,
                    static_cast<double>(executor_guest_invalidation_wait_max_ns.load(
                        std::memory_order_relaxed)) /
                        1.0e6);
            }
        }
    }
#endif
    return request.result;
}

bool Rasterizer::InvalidateMemoryBatchOnGpuThread(
    const std::span<const std::pair<VAddr, u64>> ranges,
    const bool publish_gpu_images) {
    if (ranges.empty()) {
        return true;
    }
    bool result = true;
    const bool previous_draining = executor_draining_guest_invalidations;
    executor_draining_guest_invalidations = true;
    SCOPE_EXIT {
        executor_draining_guest_invalidations = previous_draining;
    };
    for (const auto& [address, size] : ranges) {
        result = (publish_gpu_images
                      ? InvalidateMemoryBeforeGuestWrite(address, size)
                      : InvalidateMemory(address, size)) &&
                 result;
        result = DrainNestedGuestInvalidations(
            [this](const VAddr nested_address, const u64 nested_size) {
                return InvalidateMemory(nested_address, nested_size);
            },
            result);
    }
    return result;
}

bool Rasterizer::InvalidateMemoryBatchFromGuestThread(
    const std::span<const std::pair<VAddr, u64>> ranges) {
    if (ranges.empty()) {
        return true;
    }
    bool result = true;
    if (IsGpuThread()) {
        result = InvalidateMemoryBatchOnGpuThread(ranges);
    } else {
        liverpool->SendCommand<true>([this, ranges, &result] {
            result = InvalidateMemoryBatchOnGpuThread(ranges);
        });
    }
    return result;
}

bool Rasterizer::InvalidateMemoryBatchBeforeGuestWriteFromGuestThread(
    const std::span<const std::pair<VAddr, u64>> ranges) {
    if (ranges.empty()) {
        return true;
    }
    bool result = true;
    if (IsGpuThread()) {
        result = InvalidateMemoryBatchOnGpuThread(ranges, true);
    } else {
        liverpool->SendCommand<true>([this, ranges, &result] {
            result = InvalidateMemoryBatchOnGpuThread(ranges, true);
        });
    }
    return result;
}

void Rasterizer::EnqueueMemoryInvalidationBatchFromGuestThread(
    const std::span<const std::pair<VAddr, u64>> ranges) {
    if (ranges.empty()) {
        return;
    }
    if (IsGpuThread()) {
        (void)InvalidateMemoryBatchOnGpuThread(ranges);
        return;
    }
    std::vector<std::pair<VAddr, u64>> owned_ranges{ranges.begin(), ranges.end()};
    liverpool->SendCommand<false>(
        [this, ranges = std::move(owned_ranges)]() mutable {
            (void)InvalidateMemoryBatchOnGpuThread(ranges);
        });
}

void Rasterizer::DrainGuestInvalidations() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (VideoCore::InvalidationOverlapTrace::Enabled()) [[unlikely]] {
        const auto begin = std::chrono::steady_clock::now();
        DrainGuestInvalidationsImpl();
        const auto end = std::chrono::steady_clock::now();
        VideoCore::InvalidationOverlapTrace::RecordDrainSpanEnabled(begin,
                                                                    end);
        return;
    }
#endif
    DrainGuestInvalidationsImpl();
}

void Rasterizer::DrainGuestInvalidationsImpl() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool executor_guest_invalidation_profile_enabled =
        ExecutorGuestInvalidationProfileEnabled();
#endif
    const bool previous_draining = executor_draining_guest_invalidations;
    executor_draining_guest_invalidations = true;
    SCOPE_EXIT {
        executor_draining_guest_invalidations = previous_draining;
    };
    struct MergedRange {
        VAddr begin;
        VAddr end;
        size_t first_request;
        size_t request_count;
        bool publish_gpu_images;
    };

    using RequestBatch =
        boost::container::small_vector<GuestInvalidationRequest*, 16>;
    RequestBatch batch;
    {
        std::scoped_lock lock{guest_invalidation_mutex};
        if (FailPendingGuestInvalidationsIfTerminalLocked()) {
            return;
        }
        batch.swap(pending_guest_invalidations);
        // A queued callback (including a watchdog duplicate) now owns every
        // request in this snapshot, so an older unconsumed baton is obsolete.
        guest_invalidation_handoff_pending = false;
        if (batch.empty()) {
            guest_invalidation_drain_queued = false;
            return;
        }
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_guest_invalidation_profile_enabled) {
        executor_guest_invalidation_batches.fetch_add(1, std::memory_order_relaxed);
    }
#endif

    const auto process_batch = [&](const RequestBatch& current_batch) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        VideoCore::InvalidationOverlapTrace::DrainTokenV2 overlap_batch_v2{};
        if (VideoCore::InvalidationOverlapTrace::V2MayHaveOutstandingSpans()) [[unlikely]] {
            const VideoCore::InvalidationOverlapTrace::RequestTokenV2*
                first_captured_request = nullptr;
            u64 untracked_request_count = 0;
            for (const GuestInvalidationRequest* request : current_batch) {
                if (request->overlap_trace_v2) {
                    if (first_captured_request == nullptr) {
                        first_captured_request = &request->overlap_trace_v2;
                    }
                } else {
                    ++untracked_request_count;
                }
            }
            const auto drain_begin = std::chrono::steady_clock::now();
            overlap_batch_v2 =
                VideoCore::InvalidationOverlapTrace::BeginDrainBatchV2(
                    drain_begin, ExecutorVkGpuTimelineCurrentFrameEpoch(),
                    first_captured_request, untracked_request_count,
                    current_batch.size());
            if (overlap_batch_v2) {
                for (const GuestInvalidationRequest* request : current_batch) {
                    if (request->overlap_trace_v2) {
                        VideoCore::InvalidationOverlapTrace::
                            RecordRequestBatchMappingV2(
                                request->overlap_trace_v2, overlap_batch_v2,
                                drain_begin);
                    }
                }
            }
        }
        SCOPE_EXIT {
            if (overlap_batch_v2) {
                VideoCore::InvalidationOverlapTrace::EndDrainBatchV2(
                    overlap_batch_v2, std::chrono::steady_clock::now(),
                    ExecutorVkGpuTimelineCurrentFrameEpoch());
            }
        };
#endif
        if (liverpool == nullptr || liverpool->IsRendererTerminal()) {
            for (GuestInvalidationRequest* request : current_batch) {
                request->result = false;
                request->retry = false;
            }
            return;
        }
        if (current_batch.size() == 1) {
            GuestInvalidationRequest* const request = current_batch.front();
            const bool current =
                request->validator == nullptr ||
                request->validator(request->validator_context, request->addr,
                                   request->size,
                                   request->validation_token);
            bool result = current &&
                          (request->publish_gpu_images
                               ? InvalidateMemoryBeforeGuestWrite(
                                     request->addr, request->size)
                               : InvalidateMemory(request->addr,
                                                  request->size));
            if (current) {
                result = DrainNestedGuestInvalidations(
                    [this](const VAddr nested_address,
                           const u64 nested_size) {
                        return InvalidateMemory(nested_address, nested_size);
                    },
                    result);
            }
            request->result = result;
            return;
        }

        RequestBatch ordinary_batch;
        ordinary_batch.reserve(current_batch.size());
        for (GuestInvalidationRequest* request : current_batch) {
            if (request->validator == nullptr) {
                ordinary_batch.push_back(request);
                continue;
            }
            const bool current = request->validator(
                request->validator_context, request->addr, request->size,
                request->validation_token);
            bool result = current &&
                          (request->publish_gpu_images
                               ? InvalidateMemoryBeforeGuestWrite(
                                     request->addr, request->size)
                               : InvalidateMemory(request->addr,
                                                  request->size));
            if (current) {
                result = DrainNestedGuestInvalidations(
                    [this](const VAddr nested_address,
                           const u64 nested_size) {
                        return InvalidateMemory(nested_address, nested_size);
                    },
                    result);
            }
            request->result = result;
        }

        std::ranges::sort(
            ordinary_batch, {},
            [](const GuestInvalidationRequest* request) {
                return request->addr;
            });
        boost::container::small_vector<MergedRange, 16> ranges;
        ranges.reserve(ordinary_batch.size());
        for (size_t i = 0; i < ordinary_batch.size();) {
            const size_t first = i;
            const VAddr begin = ordinary_batch[i]->addr;
            const bool publish_gpu_images =
                ordinary_batch[i]->publish_gpu_images;
            VAddr end = ordinary_batch[i]->size >
                                std::numeric_limits<VAddr>::max() - begin
                            ? std::numeric_limits<VAddr>::max()
                            : begin + ordinary_batch[i]->size;
            ++i;
            while (i < ordinary_batch.size() &&
                   ordinary_batch[i]->addr <= end &&
                   ordinary_batch[i]->publish_gpu_images ==
                       publish_gpu_images) {
                const VAddr next_end =
                    ordinary_batch[i]->size >
                            std::numeric_limits<VAddr>::max() -
                                ordinary_batch[i]->addr
                        ? std::numeric_limits<VAddr>::max()
                        : ordinary_batch[i]->addr + ordinary_batch[i]->size;
                end = std::max(end, next_end);
                ++i;
            }
            ranges.push_back({
                .begin = begin,
                .end = end,
                .first_request = first,
                .request_count = i - first,
                .publish_gpu_images = publish_gpu_images,
            });
        }

        for (const MergedRange& range : ranges) {
            bool result =
                range.end > range.begin &&
                (range.publish_gpu_images
                     ? InvalidateMemoryBeforeGuestWrite(
                           range.begin, range.end - range.begin)
                     : InvalidateMemory(range.begin,
                                        range.end - range.begin));
            result = DrainNestedGuestInvalidations(
                [this](const VAddr nested_address, const u64 nested_size) {
                    return InvalidateMemory(nested_address, nested_size);
                },
                result);
            for (size_t i = 0; i < range.request_count; ++i) {
                ordinary_batch[range.first_request + i]->result = result;
            }
        }
    };

    static constexpr u32 MaxCatchupRounds = 8;
    static constexpr size_t MaxRequestsPerDrain = 64;
    static constexpr auto MaxCatchupTime = std::chrono::microseconds{250};
    const bool bounded_catchup = ExecutorBoundedGuestInvalidationDrainEnabled();
    const auto catchup_deadline =
        std::chrono::steady_clock::now() + MaxCatchupTime;
    RequestBatch completed_requests;
    completed_requests.reserve(MaxRequestsPerDrain);
    bool wake_multiple_requests = false;
    u32 rounds = 0;
    for (;;) {
        ++rounds;
        process_batch(batch);
        completed_requests.insert(completed_requests.end(), batch.begin(),
                                  batch.end());
        batch.clear();

        bool take_catchup = false;
        {
            std::scoped_lock lock{guest_invalidation_mutex};
            const bool renderer_terminal =
                liverpool == nullptr || liverpool->IsRendererTerminal();
            take_catchup =
                !renderer_terminal && bounded_catchup &&
                rounds < MaxCatchupRounds &&
                std::chrono::steady_clock::now() < catchup_deadline &&
                completed_requests.size() < MaxRequestsPerDrain &&
                !pending_guest_invalidations.empty() &&
                pending_guest_invalidations.size() <=
                    MaxRequestsPerDrain - completed_requests.size();
            if (take_catchup) {
                batch.swap(pending_guest_invalidations);
            } else {
                const bool handoff_late_arrivals =
                    !renderer_terminal && !bounded_catchup &&
                    !pending_guest_invalidations.empty();
                const size_t wake_count =
                    completed_requests.size() +
                    pending_guest_invalidations.size();
                for (GuestInvalidationRequest* request : completed_requests) {
                    request->complete = true;
                }
                // Producers processed in early rounds remain asleep until the
                // whole bounded drain is complete.  This prevents their writes
                // from racing a later overlapping invalidation after a
                // spurious condition-variable wake.
                if (handoff_late_arrivals) {
                    // The default synchronous path processes one immutable
                    // snapshot per callback. Retain arrivals published during
                    // that work and let one waiter queue their next FIFO
                    // callback. They stay asleep and cannot write until their
                    // actual invalidation completes; no catch-up work is added
                    // to this callback.
                    guest_invalidation_handoff_pending = true;
                } else {
                    // Preserve the opt-in bounded-drain experiment exactly: at
                    // its limit late arrivals retry through the old path.
                    // Terminal completion also remains fail-closed.
                    for (GuestInvalidationRequest* request :
                         pending_guest_invalidations) {
                        request->result = false;
                        request->retry = !renderer_terminal;
                        request->complete = true;
                    }
                    pending_guest_invalidations.clear();
                    guest_invalidation_drain_queued = false;
                    guest_invalidation_handoff_pending = false;
                }
                // A singleton drain has exactly one producer sleeping on the
                // shared condition variable. Wake it while the mutex still
                // excludes a newly published waiter from stealing the signal.
                // Batched completion remains a broadcast after unlock.
                if (wake_count == 1) {
                    guest_invalidation_completion_cv.notify_one();
                } else {
                    wake_multiple_requests = wake_count > 1;
                }
            }
        }
        if (!take_catchup) {
            break;
        }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (executor_guest_invalidation_profile_enabled) {
            executor_guest_invalidation_catchup_requests.fetch_add(
                batch.size(), std::memory_order_relaxed);
        }
#endif
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (executor_guest_invalidation_profile_enabled) {
        executor_guest_invalidation_drain_rounds.fetch_add(
            rounds, std::memory_order_relaxed);
    }
#endif
    if (wake_multiple_requests) {
        guest_invalidation_completion_cv.notify_all();
    }
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, VAddr completion_addr,
                            u64 completion_size) {
    if (!IsMapped(addr, size)) {
        return false;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorResolvePhysicalDrs("read_memory");
    if (completion_size == 0) {
        completion_addr = addr;
        completion_size = size;
    }
    const bool completion_sidecar_resolved =
        liverpool->ResolvePendingCompletionReadForExecutor(
            completion_addr, completion_size);
    if (completion_sidecar_resolved) {
        static std::atomic<u64> sidecar_resolved_reads{0};
        static std::atomic<u64> sidecar_readbacks_elided{0};
        const u64 resolved =
            sidecar_resolved_reads.fetch_add(1, std::memory_order_relaxed) + 1;
        // Completion labels are written directly to CPU backing by the
        // retirement sidecar.  If no independent GPU write remains for this
        // range, issuing another copyBuffer + timeline wait only downloads the
        // value that was just published and serializes the graphics queue.
        buffer_cache.ResolvePendingReadback(addr, size);
        if (!buffer_cache.IsRegionGpuModified(addr, size)) {
            const u64 elided =
                sidecar_readbacks_elided.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((resolved & 4095u) == 0) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_READBACK_SIDECAR_FASTPATH] resolved=%llu elided=%llu",
                    static_cast<unsigned long long>(resolved),
                    static_cast<unsigned long long>(elided));
            }
            return true;
        }
    }
#endif
    pipeline_cache.InvalidateShaderIdentityReadbackRange(addr, size);
    buffer_cache.ReadMemory(addr, size);
    pipeline_cache.InvalidateShaderIdentityReadbackRange(addr, size);
    return true;
}

bool Rasterizer::ReadMemoryFromGuestThread(VAddr addr, u64 size,
                                           VAddr completion_addr,
                                           u64 completion_size) {
    bool read = false;
    bool claude_async = false;
    const bool claude_guest = !liverpool->IsGpuThread();
    liverpool->SendCommand<true>([this, addr, size, completion_addr,
                                  completion_size, &read, &claude_async, claude_guest] {
        VideoCore::claude_guest_readback_origin = claude_guest;
        VideoCore::claude_guest_readback_async_issued = false;
        read = ReadMemory(addr, size, completion_addr, completion_size);
        claude_async = VideoCore::claude_guest_readback_async_issued;
        VideoCore::claude_guest_readback_origin = false;
        VideoCore::claude_guest_readback_async_issued = false;
    });
    if (claude_async) {
        buffer_cache.ResolvePendingReadback(addr, size);
        liverpool->SendCommand<true>([this, addr, size] {
            pipeline_cache.InvalidateShaderIdentityReadbackRange(addr, size);
        });
    }
    return read;
}

void Rasterizer::TrackPendingCompletionRead(VAddr addr, u64 size) {
    page_manager.TrackPendingCompletionRead(addr, size);
}

void Rasterizer::UntrackPendingCompletionRead(VAddr addr, u64 size) {
    page_manager.UntrackPendingCompletionRead(addr, size);
}

void Rasterizer::TrackMemoryWaitWrite(VAddr addr, u64 size) {
    page_manager.TrackMemoryWaitWrite(addr, size);
}

void Rasterizer::UntrackMemoryWaitWrite(VAddr addr, u64 size) {
    page_manager.UntrackMemoryWaitWrite(addr, size);
}

bool Rasterizer::ConsumeMemoryWaitWriteFault(VAddr addr) {
    if (!page_manager.ConsumeMemoryWaitWriteFault(addr)) {
        return false;
    }
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    buffer_cache.RecordDmaCleanEpochCpuPublication();
#endif
    liverpool->NotifyMemoryPublicationForExecutor(addr, 1);
    return true;
}

void Rasterizer::TrackJitPollWrite(VAddr addr, u64 size) {
    page_manager.TrackJitPollWrite(addr, size);
}

void Rasterizer::UntrackJitPollWrite(VAddr addr, u64 size) {
    page_manager.UntrackJitPollWrite(addr, size);
}

bool Rasterizer::ConsumeJitPollWriteFault(VAddr addr) {
    return page_manager.ConsumeJitPollWriteFault(addr);
}

void Rasterizer::SetJitPollWritePublishCallback(
    const VideoCore::JitPollWritePublishCallback callback) noexcept {
    page_manager.SetJitPollWritePublishCallback(callback);
}

bool Rasterizer::IsMemoryGpuModified(VAddr addr, u64 size) {
    return IsMapped(addr, size) && buffer_cache.IsRegionGpuModified(addr, size);
}

void Rasterizer::ProcessDownloadImages() {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool pending_transfers = scheduler.HasPendingBufferTransfers();
    const bool pending_downloads = texture_cache.HasPendingDownloadImages();
    const bool resolved_physical_drs = ExecutorResolvePhysicalDrs("download_images");
    if (!pending_transfers && !pending_downloads && !resolved_physical_drs) {
        return;
    }
    ++executor_completion_work_serial;
#endif
    if (scheduler.HasPendingBufferTransfers()) {
        scheduler.FlushPendingBufferTransfers();
    }
    if (texture_cache.HasPendingDownloadImages()) {
        texture_cache.ProcessDownloadImages();
    }
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    std::shared_lock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size,
                           Core::MemoryPermission host_permissions,
                           bool guest_cpu_read, bool guest_cpu_write) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (executor_dma_clean_epoch_diag.enabled.load(std::memory_order_acquire)) {
            executor_dma_clean_epoch_diag.mapping_serial.fetch_add(
                1, std::memory_order_release);
        }
#endif
    }
    page_manager.OnGpuMap(addr, size, host_permissions, guest_cpu_read,
                          guest_cpu_write);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    ExecutorResolvePhysicalDrs("unmap_memory");
#endif
    // Retire overlapping deferred guest writes while the cache objects and
    // mapping still exist. OnGpuUnmap performs the rare synchronous drain;
    // doing it after cache teardown loses the last authoritative invalidation.
    page_manager.OnGpuUnmap(addr, size);
    pipeline_cache.InvalidateShaderIdentityRange(addr, size);
    pipeline_cache.InvalidateSrtSnapshots();
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (executor_dma_clean_epoch_diag.enabled.load(std::memory_order_acquire)) {
            executor_dma_clean_epoch_diag.mapping_serial.fetch_add(
                1, std::memory_order_release);
        }
#endif
    }
}

bool Rasterizer::UpdateGuestProtection(
    VAddr addr, u64 size, Core::MemoryPermission host_permissions,
    bool guest_cpu_read, bool guest_cpu_write) {
    if (size == 0 || static_cast<u64>(addr) >
                         std::numeric_limits<u64>::max() - size) {
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(
        addr, addr + size);
    std::shared_lock lock{mapped_ranges_mutex};
    if (!boost::icl::contains(mapped_ranges, range)) {
        return false;
    }
    const bool changed = page_manager.OnGuestProtectionChanged(
        addr, size, host_permissions, guest_cpu_read, guest_cpu_write);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (changed &&
        executor_dma_clean_epoch_diag.enabled.load(std::memory_order_acquire)) {
        executor_dma_clean_epoch_diag.mapping_serial.fetch_add(
            1, std::memory_order_release);
    }
#endif
    return changed;
}

bool Rasterizer::BeginGuestProtectionUpdate(const VAddr addr, const u64 size) {
    return page_manager.BeginGuestProtectionChange(addr, size);
}

void Rasterizer::EndGuestProtectionUpdate(const bool active) noexcept {
    page_manager.EndGuestProtectionChange(active);
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    // ExecutorBindGuestGraphicsPipeline invalidates this cache at the required
    // Qualcomm replay boundary (every proprietary no-EDS3 bind, or each new
    // Turnip command/rendering epoch). Outside that boundary the pipeline and
    // dynamic state remain current and replaying every vkCmdSet* is redundant.
    const bool full_replay =
        !VideoCore::IsManagedVulkanStateDeduplication();
    if (full_replay) {
        dynamic_state.Invalidate();
    }
    VideoCore::RecordVulkanDynamicState(full_replay);
    dynamic_state.Commit(instance, scheduler.CommandBuffer());
}

void Rasterizer::UpdateFragmentShadingRate(const GraphicsPipeline* pipeline,
                                           const RenderState& state) const {
    vk::Extent2D rate{1, 1};
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    bool use_coarse_rate = false;
    const bool explicit_coarse =
        Config::managedCoarseFragmentShading();
    const bool adaptive_mobile =
        VideoCore::MobileGpuWantsCoarseShading();
    if ((explicit_coarse || adaptive_mobile) &&
        !executor_physical_drs_draw_active &&
        instance.IsCoarseFragmentShadingSupported() &&
        state.width >= 960 && state.height >= 540 &&
        !attachment_feedback_loop) {
        const auto& key = pipeline->GetGraphicsKey();
        const auto* fragment = pipeline->TryGetStage(Shader::LogicalStage::Fragment);
        bool has_color = false;
        for (u32 index = 0; index < state.num_color_attachments; ++index) {
            if (!state.color_attachments[index].image_view) {
                continue;
            }
            has_color = true;
        }
        bool has_buffer_writes = false;
        if (fragment != nullptr) {
            for (const auto& buffer : fragment->buffers) {
                has_buffer_writes |= buffer.is_written;
            }
        }
        // Fragment shading rate is defined for blended and depthless color
        // passes as well.  Those are the expensive lighting/post-processing
        // passes that dominate mobile GPUs, so keep them eligible while
        // excluding shaders whose per-fragment side effects or discard
        // semantics cannot be coarsened safely.
        use_coarse_rate =
            has_color && key.num_samples == 1 &&
            fragment != nullptr && !fragment->has_discard &&
            !fragment->has_storage_images && !has_buffer_writes &&
            !fragment->uses_buffer_atomic_float_min_max &&
            !fragment->uses_image_atomic_float_min_max &&
            !fragment->uses_buffer_int64_atomics;
        if (use_coarse_rate) {
            rate = explicit_coarse
                       ? instance.GetCoarseFragmentSize()
                       : instance.GetMobileFragmentSize();
        }
    }

    if (ExecutorAdaptiveVrsProfileEnabled()) {
        static thread_local u64 total_draws = 0;
        static thread_local u64 coarse_draws = 0;
        ++total_draws;
        coarse_draws += use_coarse_rate;
        if ((total_draws & 16383u) == 0) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_ADAPTIVE_VRS] draws=%llu coarse=%llu coarsePct=%.1f rate=%ux%u",
                static_cast<unsigned long long>(total_draws),
                static_cast<unsigned long long>(coarse_draws),
                static_cast<double>(coarse_draws) * 100.0 /
                    static_cast<double>(total_draws),
                explicit_coarse ? instance.GetCoarseFragmentSize().width
                                : instance.GetMobileFragmentSize().width,
                explicit_coarse ? instance.GetCoarseFragmentSize().height
                                : instance.GetMobileFragmentSize().height);
        }
    }
#endif
    VideoCore::RecordMobileGpuShadingDecision(rate.width > 1 || rate.height > 1);
    scheduler.GetDynamicState().SetFragmentShadingRate(rate);
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = liverpool->regs;

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    float pixel_center_offset = 0.f;
    if (instance.GetCapabilityProfile().emulate_integer_pixel_centers) {
        // Conservatively retain the original route if any attachment uses MSAA,
        // including inactive attachments. The offset is in guest pixels before DRS.
        const bool single_sample = regs.depth_buffer.NumSamples() == 1 &&
            std::all_of(std::begin(regs.color_buffers), std::end(regs.color_buffers),
                        [](const auto& color) { return color.NumSamples() == 1; });
        pixel_center_offset = GuestPixelCenterOffset(
            true, regs.reg_array[0xA2F9], single_sample, regs.IsClipDisabled());
    }
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }
        viewport.x += pixel_center_offset;
        viewport.y += pixel_center_offset;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (executor_physical_drs_draw_active) {
            const float physical_scale =
                static_cast<float>(executor_physical_drs_scale_percent) / 100.0f;
            viewport.x *= physical_scale;
            viewport.y *= physical_scale;
            viewport.width *= physical_scale;
            viewport.height *= physical_scale;
        }
#endif
        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        vk::Rect2D scissor{
            .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
            .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
        };
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
        if (executor_physical_drs_draw_active) {
            const double physical_scale =
                static_cast<double>(executor_physical_drs_scale_percent) / 100.0;
            const s32 right = scissor.offset.x +
                              static_cast<s32>(scissor.extent.width);
            const s32 bottom = scissor.offset.y +
                               static_cast<s32>(scissor.extent.height);
            const s32 scaled_left =
                static_cast<s32>(std::floor(scissor.offset.x * physical_scale));
            const s32 scaled_top =
                static_cast<s32>(std::floor(scissor.offset.y * physical_scale));
            const s32 scaled_right =
                static_cast<s32>(std::ceil(right * physical_scale));
            const s32 scaled_bottom =
                static_cast<s32>(std::ceil(bottom * physical_scale));
            scissor.offset = {scaled_left, scaled_top};
            scissor.extent = {
                static_cast<u32>(std::max(0, scaled_right - scaled_left)),
                static_cast<u32>(std::max(0, scaled_bottom - scaled_top)),
            };
        }
#endif
        scissors.push_back(scissor);
    }

    if (viewports.empty()) {
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        const auto front_reference = LiverpoolToVK::StencilReference(
            regs.depth_control.stencil_ref_func, depth_test_enabled, regs.depth_control.depth_func,
            regs.stencil_control.stencil_zpass_front,
            regs.stencil_control.stencil_zfail_front, front);
        const auto back_reference = regs.depth_control.backface_enable
                                        ? LiverpoolToVK::StencilReference(
                                              regs.depth_control.stencil_bf_func,
                                              depth_test_enabled, regs.depth_control.depth_func,
                                              regs.stencil_control.stencil_zpass_back,
                                              regs.stencil_control.stencil_zfail_back, back)
                                        : front_reference;
        dynamic_state.SetStencilReferences(front_reference, back_reference);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
#if !defined(__ANDROID__) && !defined(LSX4_APPLE_RUNTIME)
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
#endif
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = liverpool->regs;
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !Config::getVkGuestMarkersEnabled()) ||
        (!from_guest && !Config::getVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !Config::getVkGuestMarkersEnabled()) ||
        (!from_guest && !Config::getVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !Config::getVkGuestMarkersEnabled()) ||
        (!from_guest && !Config::getVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !Config::getVkGuestMarkersEnabled()) ||
        (!from_guest && !Config::getVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

std::int32_t ExecutorQueryRuntimeProofCounters(
    Lsx4FunnelRuntimeProofCountersV1* const output) noexcept {
    constexpr u64 QueryRequestedMask =
        LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_REQUESTS |
        LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_COMPLETIONS |
        LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_WAIT_NS |
        LSX4_FUNNEL_RUNTIME_PROOF_OWNED_GENERATED_WRITE_FAULTS |
        LSX4_FUNNEL_RUNTIME_PROOF_FASTMEM_MISSES;
    constexpr u64 QuerySupportedMask =
        LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_REQUESTS |
        LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_COMPLETIONS |
        LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_WAIT_NS |
        LSX4_FUNNEL_RUNTIME_PROOF_OWNED_GENERATED_WRITE_FAULTS;
    constexpr u64 QueryAttemptMask =
        LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_REQUESTS |
        LSX4_FUNNEL_RUNTIME_PROOF_OWNED_GENERATED_WRITE_FAULTS;
    constexpr std::size_t RequiredPrefix =
        offsetof(Lsx4FunnelRuntimeProofCountersV1,
                 guest_invalidation_requests);
    if (output == nullptr || output->struct_size < RequiredPrefix ||
        output->abi_version != LSX4_FUNNEL_RUNTIME_PROOF_COUNTERS_ABI_V1) {
        return LSX4_FUNNEL_RUNTIME_PROOF_STATUS_INVALID_ARGUMENT;
    }

    const std::uint32_t writable = static_cast<std::uint32_t>(
        std::min<std::size_t>(output->struct_size,
                              sizeof(Lsx4FunnelRuntimeProofCountersV1)));
    Lsx4FunnelRuntimeProofCountersV1 snapshot{};
    snapshot.struct_size = writable;
    snapshot.abi_version = LSX4_FUNNEL_RUNTIME_PROOF_COUNTERS_ABI_V1;
    snapshot.status = LSX4_FUNNEL_RUNTIME_PROOF_STATUS_OK;
    snapshot.requested_mask = QueryRequestedMask;

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool marker_enabled = ExecutorGuestInvalidationProfileEnabled();
    snapshot.mode = marker_enabled
                        ? LSX4_FUNNEL_RUNTIME_PROOF_COUNTERS_DYNAMIC_DIAGNOSTIC
                        : LSX4_FUNNEL_RUNTIME_PROOF_COUNTERS_DISABLED;
    if (!marker_enabled) {
        snapshot.unavailable_mask = QueryRequestedMask;
        std::memcpy(output, &snapshot, writable);
        return snapshot.status;
    }

    snapshot.capture_flags =
        LSX4_FUNNEL_RUNTIME_PROOF_CAPTURE_RELAXED_MULTI_FIELD |
        LSX4_FUNNEL_RUNTIME_PROOF_CAPTURE_WAIT_CLOCKS;
    const bool generated_write_fault_telemetry_enabled =
        !VideoCore::IsExecutorLiveQuiet();
    if (generated_write_fault_telemetry_enabled) {
        snapshot.capture_flags |=
            LSX4_FUNNEL_RUNTIME_PROOF_CAPTURE_WRITE_FAULT_COUNTER;
    }
    if (!executor_runtime_proof_initialized.load(std::memory_order_acquire)) {
        snapshot.status = LSX4_FUNNEL_RUNTIME_PROOF_STATUS_INVALID_STATE;
        snapshot.unavailable_mask = QueryRequestedMask;
        std::memcpy(output, &snapshot, writable);
        return snapshot.status;
    }

    const u64 epoch_before =
        executor_runtime_proof_epoch.load(std::memory_order_acquire);
    snapshot.guest_invalidation_requests =
        executor_guest_invalidation_requests.load(std::memory_order_relaxed);
    snapshot.guest_invalidation_completions =
        executor_guest_invalidation_completions.load(std::memory_order_relaxed);
    snapshot.guest_invalidation_wait_ns =
        executor_guest_invalidation_wait_ns.load(std::memory_order_relaxed);
    const auto page_fault_stats =
        VideoCore::PageManager::GetOwnedPageFaultPcStats();
    snapshot.owned_generated_code_write_faults =
        page_fault_stats.generated_code_write;
    const u64 overflow_invalid =
        executor_runtime_proof_invalid_mask.load(std::memory_order_acquire) |
        (generated_write_fault_telemetry_enabled &&
                 page_fault_stats.generated_code_write_saturated
             ? LSX4_FUNNEL_RUNTIME_PROOF_OWNED_GENERATED_WRITE_FAULTS
             : 0);
    const u64 source_unavailable =
        generated_write_fault_telemetry_enabled
            ? 0
            : LSX4_FUNNEL_RUNTIME_PROOF_OWNED_GENERATED_WRITE_FAULTS;
    const u64 sequence =
        executor_runtime_proof_query_sequence.fetch_add(
            1, std::memory_order_relaxed) +
        1;
    if (sequence == 0) {
        executor_runtime_proof_sequence_saturated.store(
            true, std::memory_order_release);
    }
    const bool sequence_saturated =
        executor_runtime_proof_sequence_saturated.load(
            std::memory_order_acquire);
    const u64 epoch_after =
        executor_runtime_proof_epoch.load(std::memory_order_acquire);
    const bool initialized_after =
        executor_runtime_proof_initialized.load(std::memory_order_acquire);

    if (epoch_before == 0 || epoch_before != epoch_after ||
        !initialized_after || sequence_saturated) {
        snapshot.status = LSX4_FUNNEL_RUNTIME_PROOF_STATUS_INVALID_STATE;
        snapshot.saturated = sequence_saturated;
        snapshot.unavailable_mask = QueryRequestedMask;
        snapshot.guest_invalidation_requests = 0;
        snapshot.guest_invalidation_completions = 0;
        snapshot.guest_invalidation_wait_ns = 0;
        snapshot.owned_generated_code_write_faults = 0;
        std::memcpy(output, &snapshot, writable);
        return snapshot.status;
    }

    snapshot.epoch = epoch_before;
    snapshot.sequence = sequence;
    snapshot.valid_mask =
        QuerySupportedMask & ~overflow_invalid & ~source_unavailable;
    snapshot.unavailable_mask =
        QueryRequestedMask & ~snapshot.valid_mask;
    snapshot.exact_mask = snapshot.valid_mask;
    snapshot.sampled_mask = 0;
    snapshot.static_mask = 0;
    snapshot.dynamic_mask = snapshot.valid_mask;
    snapshot.perturbed_mask = snapshot.valid_mask;
    snapshot.attempted_not_retired_mask =
        QueryAttemptMask & snapshot.valid_mask;
    snapshot.saturated = overflow_invalid != 0;
    if ((snapshot.valid_mask &
         LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_REQUESTS) == 0) {
        snapshot.guest_invalidation_requests = 0;
    }
    if ((snapshot.valid_mask &
         LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_COMPLETIONS) == 0) {
        snapshot.guest_invalidation_completions = 0;
    }
    if ((snapshot.valid_mask &
         LSX4_FUNNEL_RUNTIME_PROOF_GUEST_INVALIDATION_WAIT_NS) == 0) {
        snapshot.guest_invalidation_wait_ns = 0;
    }
    if ((snapshot.valid_mask &
         LSX4_FUNNEL_RUNTIME_PROOF_OWNED_GENERATED_WRITE_FAULTS) == 0) {
        snapshot.owned_generated_code_write_faults = 0;
    }
#else
    snapshot.mode = LSX4_FUNNEL_RUNTIME_PROOF_COUNTERS_DISABLED;
    snapshot.unavailable_mask = QueryRequestedMask;
#endif

    std::memcpy(output, &snapshot, writable);
    return snapshot.status;
}

}

extern "C" std::int32_t executor_funnel_runtime_proof_counters_query_v1(
    Lsx4FunnelRuntimeProofCountersV1* const counters) noexcept {
    return Vulkan::ExecutorQueryRuntimeProofCounters(counters);
}
