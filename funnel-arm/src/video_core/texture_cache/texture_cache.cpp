// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <filesystem>
#include <xxhash.h>

#include "common/assert.h"
#include "common/config.h"
#include "common/hash.h"
#include "common/content_fingerprint.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
#include <android/log.h>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#endif
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/amdgpu/pm4_diff_trace.h"
#include "video_core/amdgpu/render_wave_trace.h"
#include "video_core/page_manager.h"
#include "video_core/mobile_gpu_optimization.h"
#include "video_core/readback_optimization.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
extern "C" int executor_lsx4_runtime_present_guest_frame(
    const void* source, std::uint32_t width, std::uint32_t height,
    std::uint32_t stride_bytes, std::uint32_t pixel_format, std::uint32_t tiling_mode);
#endif

namespace VideoCore {

#if defined(__ANDROID__)
static void ExecutorDumpRtPpm(const char* path, const u8* pixels, u32 width, u32 height, u32 stride,
                              u32 bpp) {
    if (!path || !pixels || width == 0 || height == 0 || bpp < 4) {
        return;
    }
    FILE* fp = std::fopen(path, "wb");
    if (!fp) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_RT_DUMP] failed_open path=%s", path);
        return;
    }
    std::fprintf(fp, "P6\n%u %u\n255\n", width, height);
    for (u32 y = 0; y < height; ++y) {
        const u8* row = pixels + static_cast<u64>(y) * stride;
        for (u32 x = 0; x < width; ++x) {
            const u8 rgb[3] = {row[static_cast<u64>(x) * bpp + 0],
                               row[static_cast<u64>(x) * bpp + 1],
                               row[static_cast<u64>(x) * bpp + 2]};
            std::fwrite(rgb, 1, sizeof(rgb), fp);
        }
    }
    std::fclose(fp);
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_RT_DUMP] wrote path=%s extent=%ux%u stride=%u", path, width,
                        height, stride);
}
#endif

static constexpr u64 PageShift = 12;
static constexpr u64 NumFramesBeforeRemoval = 32;

static ImageBackingRole BackingRoleFor(
    const TextureCache::BindingType binding) noexcept {
    switch (binding) {
    case TextureCache::BindingType::Texture:
        return ImageBackingRole::Texture;
    case TextureCache::BindingType::Storage:
        return ImageBackingRole::Storage;
    case TextureCache::BindingType::RenderTarget:
        return ImageBackingRole::RenderTarget;
    case TextureCache::BindingType::DepthTarget:
        return ImageBackingRole::DepthTarget;
    case TextureCache::BindingType::VideoOut:
        return ImageBackingRole::VideoOut;
    }
    UNREACHABLE();
}

enum class GuestBackingHashMode : u8 {
    None,
    Sample,
    FullMip,
};

struct GuestBackingHashPlan {
    GuestBackingHashMode mode{GuestBackingHashMode::None};
    u64 read_size{};
    u64 sample_size{}; // Complete surface coverage; no truncation at 4 GiB.
    bool sample_hash{};
    bool mip_hash{};
};

[[nodiscard]] static GuestBackingHashPlan BuildGuestBackingHashPlan(
    const Image& image) {
    const bool gpu_dirty = True(image.flags & ImageFlagBits::GpuDirty);
    const bool sample_hash =
        True(image.flags & ImageFlagBits::MaybeCpuDirty) &&
        False(image.flags & ImageFlagBits::CpuDirty) && !gpu_dirty;
    // GpuDirty means a guest-backed Vulkan buffer is the newer authority.
    // RefreshImage uploads that buffer directly and deliberately skips the
    // CPU mip hashes; publishing it back to CPU first is redundant.
    const bool mip_hash =
        True(image.flags & ImageFlagBits::GpuModified) &&
        !gpu_dirty;
    // A transparent/static first tile is not evidence that streaming has
    // left the rest of the surface unchanged. Use the same full range as the
    // existing initial image.hash. GPU-authoritative buffers still skip CPU
    // hashing through the unchanged !gpu_dirty admission above.
    const u64 sample_size = sample_hash ? image.info.guest_size : 0;
    if (mip_hash) {
        return {
            .mode = GuestBackingHashMode::FullMip,
            .read_size = image.info.guest_size,
            .sample_size = sample_size,
            .sample_hash = sample_hash,
            .mip_hash = true,
        };
    }
    if (sample_hash) {
        return {
            .mode = GuestBackingHashMode::Sample,
            .read_size = sample_size,
            .sample_size = sample_size,
            .sample_hash = true,
            .mip_hash = false,
        };
    }
    return {};
}

#ifdef __ANDROID__
static void TraceGuestBackingHashPlan(const VAddr address,
                                      const u64 surface_size,
                                      const u32 flags,
                                      const GuestBackingHashPlan& plan) {
    static const bool enabled = [] {
        const char* value = std::getenv("EXECUTOR_READBACK_CONTRACT_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (!enabled) {
        return;
    }
    static std::atomic<u64> calls{0};
    static std::atomic<u64> none_calls{0};
    static std::atomic<u64> sample_calls{0};
    static std::atomic<u64> full_calls{0};
    static std::atomic<u64> bypassed_surface_bytes{0};
    const u64 ordinal = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool none = plan.mode == GuestBackingHashMode::None;
    const bool sample = plan.mode == GuestBackingHashMode::Sample;
    const u64 none_total =
        none_calls.fetch_add(none ? 1u : 0u, std::memory_order_relaxed) +
        (none ? 1u : 0u);
    const u64 sample_total =
        sample_calls.fetch_add(sample ? 1u : 0u,
                               std::memory_order_relaxed) +
        (sample ? 1u : 0u);
    const u64 full_total =
        full_calls.fetch_add(!none && !sample ? 1u : 0u,
                             std::memory_order_relaxed) +
        (!none && !sample ? 1u : 0u);
    const u64 bypassed_total =
        bypassed_surface_bytes.fetch_add(none ? surface_size : 0u,
                                         std::memory_order_relaxed) +
        (none ? surface_size : 0u);
    if (ordinal <= 64 || (ordinal & (ordinal - 1)) == 0) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_TEXTURE_HASH_PLAN] n=%llu addr=0x%llx surfaceBytes=%llu "
            "flags=0x%x mode=%u readBytes=%llu sampleHash=%u mipHash=%u "
            "noneTotal=%llu sampleTotal=%llu fullTotal=%llu "
            "bypassedSurfaceBytes=%llu",
            static_cast<unsigned long long>(ordinal),
            static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(surface_size), flags,
            static_cast<unsigned>(plan.mode),
            static_cast<unsigned long long>(plan.read_size),
            plan.sample_hash ? 1u : 0u, plan.mip_hash ? 1u : 0u,
            static_cast<unsigned long long>(none_total),
            static_cast<unsigned long long>(sample_total),
            static_cast<unsigned long long>(full_total),
            static_cast<unsigned long long>(bypassed_total));
    }
}

static bool ExecutorUsesExactA750TurnipImageObjectGc(
    const Vulkan::Instance& instance) noexcept {
    static const bool requested = [] {
        constexpr std::array<const char*, 2> markers{
            "/data/data/app.lsx4.android/files/lsx4-home/"
            "run-sm8650-a750-turnip-image-object-gc-ab",
            "/data/user/0/app.lsx4.android/files/lsx4-home/"
            "run-sm8650-a750-turnip-image-object-gc-ab",
        };
        for (const char* const path : markers) {
            if (std::FILE* const marker = std::fopen(path, "rb")) {
                std::fclose(marker);
                return true;
            }
        }
        return false;
    }();
    const auto& profile = instance.GetCapabilityProfile();
    return requested && profile.exact_mesa_turnip_a750_d9_lineage;
}

#endif

static u64 ExactSurfaceIdentity(const ImageInfo& info) {
    Common::ContentFingerprint64 fingerprint{
        Common::FingerprintDomain::AttachmentSet};
    const auto mix = [&](const auto value) {
        fingerprint.UpdateLittleEndian(static_cast<u64>(value));
    };
    mix(info.guest_address);
    mix(info.guest_size);
    mix(info.size.width);
    mix(info.size.height);
    mix(info.size.depth);
    mix(info.pitch);
    mix(info.resources.levels);
    mix(info.resources.layers);
    mix(info.num_samples);
    mix(info.num_bits);
    mix(info.pixel_format);
    mix(info.type);
    mix(info.tile_mode);
    return fingerprint.Finish();
}

static u64 ExactSurfacePrehashProbeKey(const ImageInfo& info) {
    // This is only a direct-mapped-table selector.  A hit still has to prove
    // the slot generation, registration state and every exact surface field.
    // Keep the selector deliberately cheaper than ExactSurfaceIdentity so the
    // common hit can avoid constructing and updating a content fingerprint.
    return (info.guest_address >> 8u) ^ (info.guest_address >> 32u) ^
           (static_cast<u64>(info.guest_size) << 1u);
}

TextureCache::TextureCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                           AmdGpu::Liverpool* liverpool_, BufferCache& buffer_cache_,
                           PageManager& tracker_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      buffer_cache{buffer_cache_}, tracker{tracker_}, blit_helper{instance, scheduler},
      tile_manager{instance, scheduler, buffer_cache.GetUtilityBuffer(MemoryUsage::Stream)},
      page_table{instance_.GetCapabilityProfile().optimization_a750_host_metadata_compaction
                     ? 16u
                     : 8192u} {
#if defined(__ANDROID__)
    const bool exact_sm8650_d9_default =
        Common::IsSnapdragon8Gen3Host() &&
        instance.GetCapabilityProfile().exact_mesa_turnip_a750_d9_lineage;
    const bool exact_surface_hot_requested =
        ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                 "run-enable-exact-surface-hot-cache",
                 F_OK) == 0;
    if (exact_surface_hot_requested || exact_sm8650_d9_default) {
        exact_surface_hot_entries.reset(
            new (std::nothrow) ExactSurfaceHotEntry[ExactSurfaceHotEntryCount]{});
        exact_surface_hot_cache_enabled = exact_surface_hot_entries != nullptr;
        const bool exact_surface_prehash_requested =
            ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                     "run-enable-exact-surface-prehash-probe",
                     F_OK) == 0;
        exact_surface_prehash_probe_enabled =
            exact_surface_hot_cache_enabled &&
            (exact_surface_prehash_requested || exact_sm8650_d9_default);
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_EXACT_SURFACE_HOT_CACHE] enabled=%u entries=%zu "
            "prehashProbe=%u requested=%u prehashRequested=%u "
            "sm8650ExactD9Default=%u "
            "proof=identity+slot+uid+registered+full-info",
            exact_surface_hot_cache_enabled ? 1u : 0u,
            ExactSurfaceHotEntryCount,
            exact_surface_prehash_probe_enabled ? 1u : 0u,
            exact_surface_hot_requested ? 1u : 0u,
            exact_surface_prehash_requested ? 1u : 0u,
            exact_sm8650_d9_default ? 1u : 0u);
    }
#endif
    if (tracker.HleCleanBufferFastPathEnabled()) {
        hle_residency_lease_control =
            std::make_shared<HleResidencyLeaseControl>();
        hle_residency_lease_control->owner = &tracker;
    }
    const auto null_id = GetNullImage(vk::Format::eR8G8B8A8Unorm);
    ASSERT(null_id.index == NULL_IMAGE_ID.index);
    RefreshManagedResidencyPolicy(true);
}

void TextureCache::RefreshManagedResidencyPolicy(const bool force) {
    const u32 max_samplers = instance.GetMaxSamplerAllocationCount();
#ifdef __ANDROID__
    const bool managed_mobile_gpu =
        VideoCore::IsManagedMobileGpuOptimization();
#else
    constexpr bool managed_mobile_gpu = false;
#endif
    if (!force && residency_policy_initialized &&
        residency_policy_mobile == managed_mobile_gpu) {
        return;
    }
    residency_policy_initialized = true;
    residency_policy_mobile = managed_mobile_gpu;

    trigger_gc_samplers = managed_mobile_gpu ? max_samplers * 2 / 3
                                             : max_samplers * 3 / 4;
    pressure_gc_samplers = managed_mobile_gpu ? max_samplers * 4 / 5
                                              : max_samplers * 7 / 8;
    critical_gc_samplers = managed_mobile_gpu ? max_samplers * 9 / 10
                                              : max_samplers * 15 / 16;

    if (!instance.CanReportMemoryUsage()) {
        trigger_gc_memory = 0;
        pressure_gc_memory = DEFAULT_PRESSURE_GC_MEMORY;
        critical_gc_memory = DEFAULT_CRITICAL_GC_MEMORY;
        return;
    }

    const s64 device_local_memory = static_cast<s64>(instance.GetTotalMemoryBudget());
#ifdef __ANDROID__
    const auto adaptive_threshold = [device_local_memory](
                                        const s64 numerator,
                                        const s64 denominator,
                                        const s64 floor,
                                        const s64 ceiling) {
        const s64 proportional = device_local_memory * numerator / denominator;
        return static_cast<u64>(
            std::min(device_local_memory, std::clamp(proportional, floor, ceiling)));
    };
    trigger_gc_memory = managed_mobile_gpu
                            ? adaptive_threshold(7, 20, 512_MB, 1_GB + 256_MB)
                            : adaptive_threshold(9, 20, 768_MB, 1_GB + 512_MB);
    pressure_gc_memory = std::min<u64>(
        device_local_memory,
        std::max<u64>(
            trigger_gc_memory + 256_MB,
            managed_mobile_gpu
                ? adaptive_threshold(11, 20, 768_MB, 2_GB)
                : adaptive_threshold(3, 5, 1_GB, 2_GB + 256_MB)));
    critical_gc_memory = std::min<u64>(
        device_local_memory,
        std::max<u64>(
            pressure_gc_memory + 256_MB,
            managed_mobile_gpu
                ? adaptive_threshold(7, 10, 1_GB + 256_MB, 2_GB + 512_MB)
                : adaptive_threshold(3, 4, 1_GB + 512_MB, 3_GB)));
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_TEXTURE_GC_POLICY] budgetMiB=%lld triggerMiB=%llu pressureMiB=%llu "
        "criticalMiB=%llu",
        static_cast<long long>(device_local_memory / 1_MB),
        static_cast<unsigned long long>(trigger_gc_memory / 1_MB),
        static_cast<unsigned long long>(pressure_gc_memory / 1_MB),
        static_cast<unsigned long long>(critical_gc_memory / 1_MB));
#else
    const s64 min_spacing_expected = device_local_memory - 1_GB;
    const s64 min_spacing_critical = device_local_memory - 512_MB;
    const s64 mem_threshold = std::min<s64>(device_local_memory, TARGET_GC_THRESHOLD);
    const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
    const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
    pressure_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_expected, min_spacing_expected),
                      DEFAULT_PRESSURE_GC_MEMORY));
    critical_gc_memory = static_cast<u64>(
        std::max<u64>(std::min(device_local_memory - min_vacancy_critical, min_spacing_critical),
                      DEFAULT_CRITICAL_GC_MEMORY));
    trigger_gc_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
#endif
}

TextureCache::~TextureCache() {
    // Scheduler callbacks can be discarded or outlive TextureCache during
    // terminal teardown.  Prevent their lease destructors from dereferencing
    // this object; PageManager itself is destroyed later by Rasterizer member
    // order, and a retained count is conservative until then.
    if (hle_residency_lease_control) {
        std::unique_lock lock{hle_residency_lease_control->mutex};
        hle_residency_lease_control->closing = true;
        hle_residency_lease_control->cv.wait(lock, [this] {
            return hle_residency_lease_control->running == 0;
        });
        hle_residency_lease_control->owner = nullptr;
    }
}

TextureCache::HleResidencyLease::~HleResidencyLease() {
    Release();
}

void TextureCache::HleResidencyLease::Release() noexcept {
    if (!held || !control) {
        return;
    }
    std::scoped_lock lock{control->mutex};
    if (control->owner != nullptr) {
        control->owner->UntrackTextureResidency(address, size);
    }
    held = false;
}

ImageId TextureCache::GetNullImage(const vk::Format format) {
    const auto existing_image = null_images.find(format);
    if (existing_image != null_images.end()) {
        return existing_image->second;
    }

    ImageInfo info{};
    info.pixel_format = format;
    info.type = AmdGpu::ImageType::Color2D;
    info.tile_mode = AmdGpu::TileMode::Thin1DThin;
    info.num_bits = 32;
    info.UpdateSize();

    const ImageId null_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views, info);
    auto& image = slot_images[null_id];
    Vulkan::SetObjectName(instance.GetDevice(), image.GetImage(),
                          fmt::format("Null Image ({})", vk::to_string(format)));

    if (null_id.index == NULL_IMAGE_ID.index) {
        const vk::ClearValue zero = {
            .color = {.uint32 = std::array<u32, 4>{0, 0, 0, 0}},
        };
        image.Clear(zero, VideoCore::SubresourceRange{.extent = image.info.resources});
        image.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead, {});
    }

    image.flags = ImageFlagBits::Empty;
    image.track_addr = image.info.guest_address;
    image.track_addr_end = image.info.guest_address + image.info.guest_size;

    null_images.emplace(format, null_id);
    return null_id;
}

void TextureCache::ProcessDownloadImages() {
    for (auto it = download_images.begin(); it != download_images.end();) {
        if (DownloadImageMemory(*it, true, ImageDownloadClassification::LinearImage)) {
            it = download_images.erase(it);
        } else {
            ++it;
        }
    }
}

bool TextureCache::DownloadImageMemory(ImageId image_id, bool sync,
                                       ImageDownloadClassification classification) {
    Image& image = slot_images[image_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        return true;
    }
    auto& download_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
    const u32 linear_download_size =
        image.info.pitch * image.info.size.height * image.info.size.depth *
        image.info.resources.layers * (image.info.num_bits / 8);
    ASSERT(linear_download_size <= image.info.guest_size);
    const u32 download_size =
        image.info.IsTiled() ? image.info.guest_size : linear_download_size;
#ifdef __ANDROID__
    const bool classify_readback = IsReadbackClassificationEnabled();
    const auto classify_total_start = classify_readback ? std::chrono::steady_clock::now()
                                                        : std::chrono::steady_clock::time_point{};
    u64 classify_map_wait_ns = 0;
    SCOPE_EXIT {
        if (classify_readback) {
            const u64 total_ns =
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - classify_total_start)
                                     .count());
            const auto source = classification == ImageDownloadClassification::GarbageCollection
                                    ? TextureReadbackClassification::GarbageCollection
                                    : TextureReadbackClassification::LinearImage;
            RecordTextureReadbackClassification(source, download_size, classify_map_wait_ns,
                                                total_ns);
        }
    };
    const auto classify_map_start = classify_readback ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
#endif
    u8* download = nullptr;
    u64 offset = 0;
    std::unique_ptr<Buffer> overflow_download;
    if (download_size <= download_buffer.SizeBytes()) {
        std::tie(download, offset) = download_buffer.Map(download_size);
    }
    if (download == nullptr) {
        overflow_download = std::make_unique<Buffer>(
            std::nothrow, instance, scheduler, MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, download_size);
        if (overflow_download->IsValid() &&
            overflow_download->mapped_data.size() >= download_size) {
            download = overflow_download->mapped_data.data();
            offset = 0;
#ifdef __ANDROID__
            static std::atomic<u32> overflow_log_count{};
            const u32 ordinal =
                overflow_log_count.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 16) {
                __android_log_print(
                    ANDROID_LOG_WARN, "LSX4Native",
                    "[EXECUTOR_TEXTURE_READBACK_OVERFLOW] ordinal=%u bytes=%u "
                    "ringBytes=%llu sync=%u classification=%u",
                    ordinal + 1, download_size,
                    static_cast<unsigned long long>(download_buffer.SizeBytes()),
                    sync ? 1u : 0u, static_cast<u32>(classification));
            }
#endif
        } else {
            overflow_download.reset();
        }
    }
#ifdef __ANDROID__
    if (classify_readback) {
        classify_map_wait_ns =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - classify_map_start)
                                 .count());
    }
#endif
    if (download == nullptr) {
#ifdef __ANDROID__
        static std::atomic<u32> allocation_failure_log_count{};
        const u32 ordinal =
            allocation_failure_log_count.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 16) {
            __android_log_print(
                ANDROID_LOG_ERROR, "LSX4Native",
                "[EXECUTOR_TEXTURE_READBACK_DROP] ordinal=%u reason=map-and-overflow-failed "
                "bytes=%u ringBytes=%llu sync=%u classification=%u",
                ordinal + 1, download_size,
                static_cast<unsigned long long>(download_buffer.SizeBytes()),
                sync ? 1u : 0u, static_cast<u32>(classification));
        }
#endif
        // Keep GpuModified set so a later readback can retry. Recording a
        // copy after Map() failed writes outside the utility ring and can
        // reset the Vulkan context.
        return false;
    }
    Buffer* const download_target =
        overflow_download ? overflow_download.get() : &download_buffer;
    if (!overflow_download) {
        download_buffer.Commit();
    }
    std::array<vk::BufferImageCopy, 1> image_downloads{{
        {
        .bufferOffset = 0,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource =
            {
                .aspectMask = image.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                        : vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, image.info.size.depth},
        },
    }};
    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::ImageDownload);
    tile_manager.TileImage(image, image_downloads, download_target->Handle(), offset,
                           linear_download_size);

    if (sync) {
        scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::TextureReadback);
        const bool wrote = Core::Memory::Instance()->TryWriteBacking(
            std::bit_cast<u8*>(image.info.guest_address), download, download_size);
        if (!wrote) {
            static std::atomic<u32> failure_count{0};
            const u32 failure = failure_count.fetch_add(1, std::memory_order_relaxed);
            if (failure < 16) {
                LOG_ERROR(Render_Vulkan,
                          "GPU image readback could not write guest backing: addr={:#x}, "
                          "size={:#x}, sync=true, occurrence={}",
                          image.info.guest_address, download_size, failure + 1);
            }
            return false;
        }
    } else {
        // The image may be unregistered and destroyed immediately after this
        // operation is queued by GC.  Hold an independent residency lease
        // until the deferred GPU->guest write has either completed or failed.
        // Allocate the lifetime carrier before incrementing the sidecar so an
        // allocation failure cannot strand a count without an owner.
        std::shared_ptr<HleResidencyLease> residency_lease;
        if (tracker.HleCleanBufferFastPathEnabled()) {
            residency_lease = std::make_shared<HleResidencyLease>();
            residency_lease->control = hle_residency_lease_control;
            residency_lease->address = image.info.guest_address;
            residency_lease->size = download_size;
            residency_lease->held =
                tracker.TrackTextureResidency(image.info.guest_address,
                                              download_size);
            if (!residency_lease->held) {
                residency_lease.reset();
            }
        }
        auto write_backing =
            [device_addr = image.info.guest_address, download, download_size,
             overflow_download = std::move(overflow_download)] {
                // A dedicated overflow allocation must outlive the submitted
                // copy. The move-only deferred callback releases it only
                // after the GPU completion associated with this operation.
                (void)overflow_download;
                const bool wrote = Core::Memory::Instance()->TryWriteBacking(
                    std::bit_cast<u8*>(device_addr), download, download_size);
                if (!wrote) {
                    static std::atomic<u32> failure_count{0};
                    const u32 failure =
                        failure_count.fetch_add(1, std::memory_order_relaxed);
                    if (failure < 16) {
                        LOG_ERROR(Render_Vulkan,
                                  "Deferred GPU image readback could not write guest backing: "
                                  "addr={:#x}, size={:#x}, occurrence={}",
                                  device_addr, download_size, failure + 1);
                    }
                }
            };
        if (residency_lease == nullptr) {
            // Preserve the original marker-off callback: no lease-control
            // capture, mutex, or running-counter traffic on the normal path.
            scheduler.DeferPriorityOperation(std::move(write_backing));
        } else {
            scheduler.DeferPriorityOperation(
                [write_backing = std::move(write_backing), residency_lease,
                 lease_control = hle_residency_lease_control] {
                bool closing = false;
                {
                    std::scoped_lock lock{lease_control->mutex};
                    closing = lease_control->closing;
                    if (!closing) {
                        ++lease_control->running;
                    }
                }
                if (closing) {
                    if (residency_lease != nullptr) {
                        residency_lease->Release();
                    }
                    return;
                }
                SCOPE_EXIT {
                    if (residency_lease != nullptr) {
                        residency_lease->Release();
                    }
                    {
                        std::scoped_lock lock{lease_control->mutex};
                        ASSERT(lease_control->running != 0);
                        --lease_control->running;
                    }
                    lease_control->cv.notify_all();
                };
                write_backing();
            });
        }
    }
    return true;
}

#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
namespace {

struct ExecutorGpuContentProbeConfig {
    bool enabled{};
    u32 budget{12};
    u64 max_bytes{16ull * 1024 * 1024};
    u64 draw_first{1};
    u64 draw_count{1};
    u64 draw_stride{1};
    u64 draw_hash{};
    u64 draw_hash1{};
    u64 draw_hash2{};
    u64 draw_hash3{};
    u64 draw_trigger_hash{};
    u32 sampled_width{};
    u32 sampled_height{};
    const char* dump_directory{};
    u64 dispatch_first{1};
    u64 dispatch_count{4};
};

struct ExecutorGpuLateProbeState {
    std::mutex mutex;
    u64 epoch{};
    std::array<u64, 4> attachment_signatures{};
    u32 attachment_count{};
    u32 dispatch_count{};
    u32 issued{};
    u32 pass_end_issued{};
    u32 display_width{};
    u32 display_height{};
};

ExecutorGpuLateProbeState& ExecutorGpuLateProbeRuntime() {
    static ExecutorGpuLateProbeState state;
    return state;
}

void ExecutorGpuLateProbeResetLocked(ExecutorGpuLateProbeState& state, u64 epoch) {
    if (state.epoch == epoch) {
        return;
    }
    state.epoch = epoch;
    state.attachment_signatures.fill(0);
    state.attachment_count = 0;
    state.dispatch_count = 0;
    state.issued = 0;
    state.pass_end_issued = 0;
    state.display_width = 0;
    state.display_height = 0;
    Libraries::VideoOut::ExecutorVideoOutBufferSnapshot video_out{};
    if (Libraries::VideoOut::ExecutorGetVideoOutBufferSnapshot(0, 0, &video_out) &&
        video_out.valid) {
        state.display_width = video_out.width;
        state.display_height = video_out.height;
    }
}

u64 ExecutorGpuProbeEnvU64(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    char* end{};
    const u64 parsed = std::strtoull(value, &end, 0);
    return end != value ? parsed : fallback;
}

const ExecutorGpuContentProbeConfig& ExecutorGpuProbeConfig() {
    static const ExecutorGpuContentProbeConfig config = [] {
        ExecutorGpuContentProbeConfig value{};
        const char* marker = std::getenv("EXECUTOR_GPU_CONTENT_PROBE");
        value.enabled = marker && *marker && std::strcmp(marker, "0") != 0;
        value.budget = static_cast<u32>(
            std::clamp<u64>(ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_BUDGET", 12),
                            1, 32));
        value.max_bytes = std::clamp<u64>(
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_MAX_BYTES",
                                   16ull * 1024 * 1024),
            4096, 64ull * 1024 * 1024);
        value.draw_first =
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DRAW_FIRST", 1);
        value.draw_count =
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DRAW_COUNT", 1);
        value.draw_stride = std::max<u64>(
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DRAW_STRIDE", 1), 1);
        value.draw_hash =
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DRAW_HASH", 0);
        value.draw_hash1 =
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DRAW_HASH1", 0);
        value.draw_hash2 =
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DRAW_HASH2", 0);
        value.draw_hash3 =
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DRAW_HASH3", 0);
        value.draw_trigger_hash =
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DRAW_TRIGGER_HASH", 0);
        value.sampled_width = static_cast<u32>(std::min<u64>(
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_SAMPLE_WIDTH", 0), 16384));
        value.sampled_height = static_cast<u32>(std::min<u64>(
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_SAMPLE_HEIGHT", 0), 16384));
        value.dump_directory = std::getenv("EXECUTOR_GPU_CONTENT_DUMP_DIR");
        value.dispatch_first =
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DISPATCH_FIRST", 1);
        value.dispatch_count =
            ExecutorGpuProbeEnvU64("EXECUTOR_GPU_CONTENT_PROBE_DISPATCH_COUNT", 4);
        return value;
    }();
    return config;
}

bool ExecutorGpuProbeSequenceSelected(u64 sequence, u64 first, u64 count, u64 stride = 1) {
    if (count == 0 || sequence < first || stride == 0) {
        return false;
    }
    const u64 delta = sequence - first;
    return delta % stride == 0 && delta / stride < count;
}

const char* ExecutorGpuProbeRoleName(TextureCache::ExecutorGpuContentRole role) {
    switch (role) {
    case TextureCache::ExecutorGpuContentRole::SampledBeforeDraw:
        return "sampled_pre_draw";
    case TextureCache::ExecutorGpuContentRole::ColorBeforeDraw:
        return "color_pre_draw";
    case TextureCache::ExecutorGpuContentRole::DepthBeforeDraw:
        return "depth_pre_draw";
    case TextureCache::ExecutorGpuContentRole::StorageAfterDispatch:
        return "storage_post_dispatch";
    case TextureCache::ExecutorGpuContentRole::ColorAfterDraw:
        return "color_post_draw";
    case TextureCache::ExecutorGpuContentRole::DepthAfterDraw:
        return "depth_post_draw";
    case TextureCache::ExecutorGpuContentRole::ColorPassEnd:
        return "color_pass_end";
    case TextureCache::ExecutorGpuContentRole::DepthPassEnd:
        return "depth_pass_end";
    }
    return "unknown";
}

}
#endif

bool TextureCache::ExecutorGpuContentProbeEnabled() const {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& config = ExecutorGpuProbeConfig();
    if (config.enabled) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_CONTENT_PROBE_CONFIG] budget=%u maxBytes=%llu "
                "drawFirst=%llu drawCount=%llu drawStride=%llu "
                "drawHashes=0x%llx,0x%llx,0x%llx,0x%llx triggerHash=0x%llx "
                "dispatchFirst=%llu "
                "dispatchCount=%llu",
                config.budget, static_cast<unsigned long long>(config.max_bytes),
                static_cast<unsigned long long>(config.draw_first),
                static_cast<unsigned long long>(config.draw_count),
                static_cast<unsigned long long>(config.draw_stride),
                static_cast<unsigned long long>(config.draw_hash),
                static_cast<unsigned long long>(config.draw_hash1),
                static_cast<unsigned long long>(config.draw_hash2),
                static_cast<unsigned long long>(config.draw_hash3),
                static_cast<unsigned long long>(config.draw_trigger_hash),
                static_cast<unsigned long long>(config.dispatch_first),
                static_cast<unsigned long long>(config.dispatch_count));
        }
    }
    return config.enabled || AmdGpu::RenderWaveTrace::LateFrameCaptureActive() ||
           AmdGpu::Pm4DiffTrace::Active();
#else
    return false;
#endif
}

void TextureCache::ExecutorGpuContentProbeObserveImage(const AmdGpu::Image& image) const {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& config = ExecutorGpuProbeConfig();
    if (!config.enabled || !config.sampled_width || !config.sampled_height) return;
    // Bounded descriptor inventory explains an unmatched extent without dumping
    // arbitrary textures or instrumenting production draws.
    static std::mutex mutex;
    static std::array<VAddr, 64> addresses{};
    static std::size_t count{};
    std::scoped_lock lock{mutex};
    if (count == addresses.size() ||
        std::find(addresses.begin(), addresses.begin() + count, image.Address()) !=
            addresses.begin() + count) return;
    addresses[count++] = image.Address();
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GPU_SAMPLED_INVENTORY] ordinal=%zu addr=0x%llx extent=%ux%u "
        "pitch=%u dataFmt=%u numFmt=%u tileMode=%u",
        count, static_cast<unsigned long long>(image.Address()),
        static_cast<unsigned>(image.width + 1), static_cast<unsigned>(image.height + 1),
        image.Pitch(), static_cast<unsigned>(image.GetDataFmt()),
        static_cast<unsigned>(image.GetNumberFmt()), static_cast<unsigned>(image.GetTileMode()));
#else
    (void)image;
#endif
}

u64 TextureCache::ExecutorGpuContentProbeExtentFilter() const {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& config = ExecutorGpuProbeConfig();
    if (config.enabled && config.sampled_width && config.sampled_height) {
        return (static_cast<u64>(config.sampled_width) << 32) | config.sampled_height;
    }
#endif
    return 0;
}

bool TextureCache::ExecutorGpuContentProbeDrawSelected(
    u64 sequence, u64 fragment_hash, u64 attachment_signature,
    u32 color_attachment_count, u32 color_width, u32 color_height, bool has_depth,
    u32 element_count, bool indirect, bool sampled_extent_matches) const {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& config = ExecutorGpuProbeConfig();
    if (AmdGpu::Pm4DiffTrace::Active()) {
        return true;
    }
    if (AmdGpu::RenderWaveTrace::LateFrameCaptureActive()) {
        auto& runtime = ExecutorGpuLateProbeRuntime();
        std::scoped_lock lock{runtime.mutex};
        ExecutorGpuLateProbeResetLocked(runtime, AmdGpu::RenderWaveTrace::LateFrameCaptureEpoch());
        const bool scene_scale =
            color_width != 0 && color_height != 0 &&
            (runtime.display_width == 0 || color_width * 2 >= runtime.display_width) &&
            (runtime.display_height == 0 || color_height * 2 >= runtime.display_height);
        const bool substantial_geometry = indirect || element_count >= 64;
        if (fragment_hash == 0 || attachment_signature == 0 || color_attachment_count == 0 ||
            !has_depth || !scene_scale || !substantial_geometry) {
            return false;
        }
        const u64 identity = attachment_signature != 0 ? attachment_signature : ~u64{0};
        if (std::ranges::find(runtime.attachment_signatures.begin(),
                             runtime.attachment_signatures.begin() + runtime.attachment_count,
                             identity) != runtime.attachment_signatures.begin() +
                                             runtime.attachment_count) {
            return false;
        }
        if (runtime.attachment_count >= runtime.attachment_signatures.size()) {
            return false;
        }
        runtime.attachment_signatures[runtime.attachment_count++] = identity;
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_FRAME_ATTACHMENT_SELECT] epoch=%llu group=%u seq=%llu "
            "attach=0x%llx ps=0x%llx colors=%u extent=%ux%u depth=%u elements=%u indirect=%u",
            static_cast<unsigned long long>(runtime.epoch), runtime.attachment_count,
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(attachment_signature),
            static_cast<unsigned long long>(fragment_hash), color_attachment_count, color_width,
            color_height, has_depth ? 1u : 0u, element_count, indirect ? 1u : 0u);
        return true;
    }
    if (!config.enabled) {
        return false;
    }
    if (config.sampled_width && config.sampled_height) {
        if (!sampled_extent_matches) return false;
        static std::atomic<u64> matching_draws{};
        const auto occurrence = matching_draws.fetch_add(1, std::memory_order_relaxed) + 1;
        return ExecutorGpuProbeSequenceSelected(occurrence, config.draw_first,
                                                config.draw_count, config.draw_stride);
    }
    const std::array hashes{config.draw_hash, config.draw_hash1, config.draw_hash2,
                            config.draw_hash3};
    if (config.draw_trigger_hash != 0) {
        static std::atomic<bool> trigger_seen{false};
        if (fragment_hash == config.draw_trigger_hash) {
            trigger_seen.store(true, std::memory_order_release);
        }
        if (!trigger_seen.load(std::memory_order_acquire)) {
            return false;
        }
    }
    const bool has_hash_filter = std::ranges::any_of(hashes, [](u64 hash) { return hash != 0; });
    if (!has_hash_filter) {
        return ExecutorGpuProbeSequenceSelected(sequence, config.draw_first, config.draw_count,
                                                config.draw_stride);
    }
    if (fragment_hash == 0 ||
        std::ranges::find_if(hashes, [fragment_hash](u64 hash) {
            return hash != 0 && hash == fragment_hash;
        }) == hashes.end()) {
        return false;
    }
    const auto hash_it = std::ranges::find(hashes, fragment_hash);
    const auto hash_index = static_cast<std::size_t>(std::distance(hashes.begin(), hash_it));
    static std::array<std::atomic<u64>, 4> matching_sequences{};
    const u64 occurrence =
        matching_sequences[hash_index].fetch_add(1, std::memory_order_relaxed) + 1;
    return ExecutorGpuProbeSequenceSelected(occurrence, config.draw_first, config.draw_count,
                                            config.draw_stride);
#else
    (void)sequence;
    (void)fragment_hash;
    (void)attachment_signature;
    (void)color_attachment_count;
    (void)color_width;
    (void)color_height;
    (void)has_depth;
    (void)element_count;
    (void)indirect;
    (void)sampled_extent_matches;
    return false;
#endif
}

bool TextureCache::ExecutorGpuContentProbeDispatchSelected(u64 sequence) const {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& config = ExecutorGpuProbeConfig();
    if (AmdGpu::RenderWaveTrace::LateFrameCaptureActive()) {
        auto& runtime = ExecutorGpuLateProbeRuntime();
        std::scoped_lock lock{runtime.mutex};
        ExecutorGpuLateProbeResetLocked(runtime, AmdGpu::RenderWaveTrace::LateFrameCaptureEpoch());
        return runtime.dispatch_count++ < 2;
    }
    return config.enabled && ExecutorGpuProbeSequenceSelected(
                                 sequence, config.dispatch_first, config.dispatch_count);
#else
    (void)sequence;
    return false;
#endif
}

void TextureCache::ExecutorGpuContentProbe(ImageId image_id, ExecutorGpuContentRole role,
                                           u32 stage, u64 stage_hash, u64 sequence) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& config = ExecutorGpuProbeConfig();
    const bool late_capture = AmdGpu::RenderWaveTrace::LateFrameCaptureActive();
    const bool pm4_capture = AmdGpu::Pm4DiffTrace::Active();
    if ((!config.enabled && !late_capture && !pm4_capture) || !image_id) {
        return;
    }
    if (pm4_capture && !config.enabled && !late_capture &&
        role != ExecutorGpuContentRole::SampledBeforeDraw) {
        return;
    }

    const Image& candidate = slot_images[image_id];
    if (config.sampled_width && config.sampled_height &&
        (role != ExecutorGpuContentRole::SampledBeforeDraw || candidate.usage.render_target ||
         candidate.info.size.width != config.sampled_width ||
         candidate.info.size.height != config.sampled_height)) {
        return;
    }
    static std::atomic<u32> startup_issued{0};
    u32 ordinal{};
    const bool pass_end_role = role == ExecutorGpuContentRole::ColorPassEnd ||
                               role == ExecutorGpuContentRole::DepthPassEnd;
    const u32 effective_budget = pm4_capture ? 128u
                                 : late_capture ? (pass_end_role ? 64u : 32u)
                                                : config.budget;
    if (late_capture) {
        auto& runtime = ExecutorGpuLateProbeRuntime();
        std::scoped_lock lock{runtime.mutex};
        ExecutorGpuLateProbeResetLocked(runtime, AmdGpu::RenderWaveTrace::LateFrameCaptureEpoch());
        ordinal = pass_end_role ? runtime.pass_end_issued++ : runtime.issued++;
    } else {
        ordinal = startup_issued.fetch_add(1, std::memory_order_relaxed);
    }
    if (ordinal >= effective_budget) {
        return;
    }

    Image& image = slot_images[image_id];
    const char* role_name = ExecutorGpuProbeRoleName(role);
    const u32 bpp = image.info.num_bits / 8u;
    const u32 width = image.info.size.width;
    const u32 height = image.info.size.height;
    const u32 depth = std::max(image.info.size.depth, 1u);
    const u32 layers = std::max(image.info.resources.layers, 1u);
    const u64 layer_stride = static_cast<u64>(image.info.pitch) * height * depth * bpp;
    const u64 download_size = layer_stride * layers;
    const bool unsupported = image.info.props.is_block || bpp == 0 ||
                             image.info.num_bits % 8u != 0 || image.info.num_samples != 1 ||
                             width == 0 || height == 0 || image.info.pitch < width ||
                             download_size == 0 || download_size > config.max_bytes ||
                             image.backing->state.layout == vk::ImageLayout::eUndefined;
    if (unsupported) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_GPU_CONTENT_PROBE] ordinal=%u role=%s seq=%llu stage=%u hash=0x%llx "
            "imageId=%u status=skip format=%s extent=%ux%ux%u pitch=%u layers=%u bits=%u "
            "samples=%u depth=%u block=%u bytes=%llu maxBytes=%llu layout=%u",
            ordinal + 1, role_name, static_cast<unsigned long long>(sequence), stage,
            static_cast<unsigned long long>(stage_hash), image_id.index,
            vk::to_string(image.info.pixel_format).c_str(), width, height, depth, image.info.pitch,
            layers, image.info.num_bits, image.info.num_samples,
            image.info.props.is_depth ? 1u : 0u, image.info.props.is_block ? 1u : 0u,
            static_cast<unsigned long long>(download_size),
            static_cast<unsigned long long>(config.max_bytes),
            static_cast<u32>(image.backing->state.layout));
        return;
    }

    const Image::State restore_state = image.backing->state;
    auto& download_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
    const auto [download, offset] = download_buffer.Map(download_size);
    download_buffer.Commit();
    const vk::ImageAspectFlags probe_aspect = image.info.props.is_depth
                                                  ? vk::ImageAspectFlagBits::eDepth
                                                  : vk::ImageAspectFlagBits::eColor;
    const vk::BufferImageCopy region = {
        .bufferOffset = offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = height,
        .imageSubresource = {.aspectMask = probe_aspect,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = layers},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, depth},
    };

    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::ImageDownload);
    const auto cmdbuf = scheduler.CommandBuffer();
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
                  cmdbuf);
    cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                             download_buffer.Handle(), region);
    const auto restore_barriers = image.GetBarriers(
        restore_state.layout, restore_state.access_mask, restore_state.pl_stage, {});
    if (!restore_barriers.empty()) {
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .imageMemoryBarrierCount = static_cast<u32>(restore_barriers.size()),
            .pImageMemoryBarriers = restore_barriers.data(),
        });
    }
    scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::TextureProbe);

    const auto dump_bytes = [&](const char* suffix, const void* data, u64 size) {
        if (!config.enabled || !config.dump_directory || !*config.dump_directory ||
            !data || !size || size > config.max_bytes) return;
        // Explicit, bounded diagnostic output only; the host owns this directory.
        const auto path = std::filesystem::path(config.dump_directory) /
            fmt::format("texture-{}-{}.{}", image_id.index, ordinal + 1, suffix);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_CONTENT_DUMP] imageId=%u ordinal=%u suffix=%s bytes=%llu ok=%u",
            image_id.index, ordinal + 1, suffix, static_cast<unsigned long long>(size),
            file.good() ? 1u : 0u);
    };
    const auto* bytes = reinterpret_cast<const u8*>(download);
    dump_bytes("gpu", bytes, download_size);
    const u64 content_hash = XXH3_64bits(bytes, download_size);
    u64 nonzero = 0;
    for (u64 i = 0; i < download_size; ++i) {
        nonzero += bytes[i] != 0;
    }
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_GPU_CONTENT_PROBE] ordinal=%u role=%s seq=%llu stage=%u hash=0x%llx "
        "imageId=%u status=ok format=%s extent=%ux%ux%u pitch=%u layers=%u bpp=%u "
        "bytes=%llu contentHash=0x%llx nonzero=%llu layout=%u access=0x%llx flags=0x%x "
        "usageTexture=%u usageStorage=%u usageRt=%u",
        ordinal + 1, role_name, static_cast<unsigned long long>(sequence), stage,
        static_cast<unsigned long long>(stage_hash), image_id.index,
        vk::to_string(image.info.pixel_format).c_str(), width, height, depth, image.info.pitch,
        layers, bpp, static_cast<unsigned long long>(download_size),
        static_cast<unsigned long long>(content_hash), static_cast<unsigned long long>(nonzero),
        static_cast<u32>(restore_state.layout),
        static_cast<unsigned long long>(static_cast<VkAccessFlags2>(restore_state.access_mask)),
        static_cast<u32>(image.flags), image.usage.texture, image.usage.storage,
        image.usage.render_target);

    // Explicit content probes compare sampled CPU backing with the GPU copy.
    // Keep this bounded by the same diagnostic budget; never read back a render
    // target here because its authoritative content may exist only on the GPU.
    if (role == ExecutorGpuContentRole::SampledBeforeDraw &&
        !image.usage.render_target && image.info.guest_size != 0 &&
        image.info.guest_size <= config.max_bytes) {
        buffer_cache.ExecutorProbeGpuBuffer(image.info.guest_address + image.info.guest_size / 2,
                                               65536, "sampled_texture_source", sequence);
        std::vector<u8> backing(image.info.guest_size);
        const bool readable = Core::Memory::Instance()->TryCopyBackingMemory(
            image.info.guest_address, backing.data(), backing.size());
        if (readable) dump_bytes("backing", backing.data(), backing.size());
        const auto backing_nonzero = readable
            ? std::ranges::count_if(backing, [](u8 value) { return value != 0; }) : 0;
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_CONTENT_BACKING] ordinal=%u imageId=%u addr=0x%llx "
            "bytes=%zu readable=%u hash=0x%llx nonzero=%llu tiled=%u tileMode=%u",
            ordinal + 1, image_id.index,
            static_cast<unsigned long long>(image.info.guest_address), backing.size(),
            readable ? 1u : 0u,
            static_cast<unsigned long long>(readable ? XXH3_64bits(backing.data(), backing.size()) : 0),
            static_cast<unsigned long long>(backing_nonzero), image.info.props.is_tiled ? 1u : 0u,
            static_cast<unsigned>(image.info.tile_mode));
    }

    if (image.info.props.is_depth && bpp >= sizeof(u32)) {
        constexpr u32 Sentinel025Bits = 0x3e800000u;
        constexpr u32 OneBits = 0x3f800000u;
        u64 sentinel_025_count = 0;
        u64 one_count = 0;
        u64 other_count = 0;
        u32 other_min = std::numeric_limits<u32>::max();
        u32 other_max = 0;
        for (u32 layer = 0; layer < layers; ++layer) {
            const u8* const layer_bytes = bytes + static_cast<u64>(layer) * layer_stride;
            for (u32 y = 0; y < height; ++y) {
                const u8* const row =
                    layer_bytes + static_cast<u64>(y) * image.info.pitch * bpp;
                for (u32 x = 0; x < width; ++x) {
                    u32 value{};
                    std::memcpy(&value, row + static_cast<u64>(x) * bpp, sizeof(value));
                    if (value == Sentinel025Bits) {
                        ++sentinel_025_count;
                    } else if (value == OneBits) {
                        ++one_count;
                    } else {
                        ++other_count;
                        other_min = std::min(other_min, value);
                        other_max = std::max(other_max, value);
                    }
                }
            }
        }
        if (other_count == 0) {
            other_min = 0;
        }
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_CONTENT_DEPTH_D32] ordinal=%u role=%s seq=%llu hash=0x%llx "
            "imageId=%u pixels=%llu sentinel025=%llu one=%llu other=%llu "
            "otherBitsMin=0x%08x otherBitsMax=0x%08x",
            ordinal + 1, role_name, static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(stage_hash), image_id.index,
            static_cast<unsigned long long>(static_cast<u64>(width) * height * layers),
            static_cast<unsigned long long>(sentinel_025_count),
            static_cast<unsigned long long>(one_count),
            static_cast<unsigned long long>(other_count), other_min, other_max);
    }

    if (bpp == 4 && !image.info.props.is_depth) {
        std::array<u8, 4> channel_min{255, 255, 255, 255};
        std::array<u8, 4> channel_max{};
        std::array<u64, 4> channel_sum{};
        u64 rg_neutral = 0;
        u64 rg_saturated = 0;
        u64 horizontal_l1 = 0;
        u64 vertical_l1 = 0;
        const u64 pixel_count = static_cast<u64>(width) * height * layers;
        for (u32 layer = 0; layer < layers; ++layer) {
            const u8* const layer_bytes = bytes + static_cast<u64>(layer) * layer_stride;
            for (u32 y = 0; y < height; ++y) {
                const u8* const row = layer_bytes + static_cast<u64>(y) * image.info.pitch * bpp;
                const u8* const previous_row =
                    y == 0 ? nullptr
                           : layer_bytes + static_cast<u64>(y - 1) * image.info.pitch * bpp;
                for (u32 x = 0; x < width; ++x) {
                    const u8* const pixel = row + static_cast<u64>(x) * bpp;
                    for (u32 channel = 0; channel < 4; ++channel) {
                        channel_min[channel] = std::min(channel_min[channel], pixel[channel]);
                        channel_max[channel] = std::max(channel_max[channel], pixel[channel]);
                        channel_sum[channel] += pixel[channel];
                        if (x != 0) {
                            horizontal_l1 += std::abs(static_cast<int>(pixel[channel]) -
                                                      static_cast<int>((pixel - bpp)[channel]));
                        }
                        if (previous_row) {
                            vertical_l1 +=
                                std::abs(static_cast<int>(pixel[channel]) -
                                         static_cast<int>(previous_row[x * bpp + channel]));
                        }
                    }
                    const bool neutral_r = pixel[0] >= 127 && pixel[0] <= 129;
                    const bool neutral_g = pixel[1] >= 127 && pixel[1] <= 129;
                    rg_neutral += neutral_r && neutral_g;
                    rg_saturated += pixel[0] <= 1 || pixel[0] >= 254 || pixel[1] <= 1 ||
                                    pixel[1] >= 254;
                }
            }
        }
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_GPU_CONTENT_RGBA8] ordinal=%u role=%s seq=%llu hash=0x%llx "
            "imageId=%u pixels=%llu min=%u,%u,%u,%u max=%u,%u,%u,%u "
            "mean=%.3f,%.3f,%.3f,%.3f rgNeutral=%llu rgSaturated=%llu "
            "horizontalL1=%llu verticalL1=%llu",
            ordinal + 1, role_name, static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(stage_hash), image_id.index,
            static_cast<unsigned long long>(pixel_count), channel_min[0], channel_min[1],
            channel_min[2], channel_min[3], channel_max[0], channel_max[1], channel_max[2],
            channel_max[3], static_cast<double>(channel_sum[0]) / pixel_count,
            static_cast<double>(channel_sum[1]) / pixel_count,
            static_cast<double>(channel_sum[2]) / pixel_count,
            static_cast<double>(channel_sum[3]) / pixel_count,
            static_cast<unsigned long long>(rg_neutral),
            static_cast<unsigned long long>(rg_saturated),
            static_cast<unsigned long long>(horizontal_l1),
            static_cast<unsigned long long>(vertical_l1));
    }

    const u32 logged_layers = std::min(layers, 2u);
    const std::array<u32, 5> xs{0u, width / 4, width / 2, (width * 3) / 4, width - 1};
    const std::array<u32, 5> ys{0u, height / 4, height / 2, (height * 3) / 4, height - 1};
    for (u32 layer = 0; layer < logged_layers; ++layer) {
        for (const u32 y : ys) {
            std::array<u64, 5> samples{};
            for (u32 sample = 0; sample < xs.size(); ++sample) {
                const u64 sample_offset =
                    static_cast<u64>(layer) * layer_stride +
                    (static_cast<u64>(y) * image.info.pitch + xs[sample]) * bpp;
                const u32 sample_bytes = std::min(bpp, 8u);
                std::memcpy(&samples[sample], bytes + sample_offset, sample_bytes);
            }
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_GPU_CONTENT_GRID] ordinal=%u role=%s seq=%llu stage=%u hash=0x%llx "
                "imageId=%u layer=%u y=%u bpp=%u x0=%u:0x%016llx x1=%u:0x%016llx "
                "x2=%u:0x%016llx x3=%u:0x%016llx x4=%u:0x%016llx",
                ordinal + 1, role_name, static_cast<unsigned long long>(sequence), stage,
                static_cast<unsigned long long>(stage_hash), image_id.index, layer, y, bpp,
                xs[0], static_cast<unsigned long long>(samples[0]), xs[1],
                static_cast<unsigned long long>(samples[1]), xs[2],
                static_cast<unsigned long long>(samples[2]), xs[3],
                static_cast<unsigned long long>(samples[3]), xs[4],
                static_cast<unsigned long long>(samples[4]));
        }
    }
#else
    (void)image_id;
    (void)role;
    (void)stage;
    (void)stage_hash;
    (void)sequence;
#endif
}

void TextureCache::ExecutorTraceVideoOutSource(ImageId image_id, u64 sequence,
                                               u32 flags_before_refresh) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    Image& image = slot_images[image_id];
    const u32 bpp = std::max<u32>(image.info.num_bits / 8u, 1u);
    const u64 download_size = static_cast<u64>(image.info.pitch) * image.info.size.height * bpp;
    if (download_size == 0 || download_size > (64ull * 1024 * 1024)) {
        __android_log_print(
            ANDROID_LOG_WARN, "LSX4Native",
            "[EXECUTOR_VIDEOOUT_SOURCE] seq=%llu id=%u skip=bad_size addr=0x%llx "
            "extent=%ux%u pitch=%u bpp=%u bytes=%llu flagsBefore=0x%x flagsAfter=0x%x",
            static_cast<unsigned long long>(sequence), image_id.index,
            static_cast<unsigned long long>(image.info.guest_address), image.info.size.width,
            image.info.size.height, image.info.pitch, bpp,
            static_cast<unsigned long long>(download_size), flags_before_refresh,
            static_cast<u32>(image.flags));
        return;
    }

    std::vector<u8> cpu_bytes(download_size);
    Core::Memory::Instance()->CopySparseMemory(image.info.guest_address, cpu_bytes.data(),
                                               download_size);
    const u64 cpu_hash = XXH3_64bits(cpu_bytes.data(), cpu_bytes.size());
    u64 cpu_nonzero = 0;
    for (const u8 value : cpu_bytes) {
        cpu_nonzero += value != 0;
    }

    auto& download_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
    const auto [download, offset] = download_buffer.Map(download_size);
    download_buffer.Commit();
    const vk::BufferImageCopy region = {
        .bufferOffset = offset,
        .bufferRowLength = image.info.pitch,
        .bufferImageHeight = image.info.size.height,
        .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .mipLevel = 0,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {image.info.size.width, image.info.size.height, 1},
    };
    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::ImageDownload);
    const auto cmdbuf = scheduler.CommandBuffer();
    image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
    cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                             download_buffer.Handle(), region);
    scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::TextureProbe);

    const auto* gpu_bytes = reinterpret_cast<const u8*>(download);
    const u64 gpu_hash = XXH3_64bits(gpu_bytes, download_size);
    AmdGpu::Pm4DiffTrace::TryArmFromVideoOutSource(
        sequence, gpu_hash, image.info.guest_address);
    std::array<u64, 4> gpu_nonzero{};
    for (u64 offset_px = 0; offset_px < download_size; ++offset_px) {
        gpu_nonzero[offset_px & 3] += gpu_bytes[offset_px] != 0;
    }
    const u64 middle = (download_size / 2) & ~u64(15);

    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_VIDEOOUT_SOURCE] seq=%llu id=%u addr=0x%llx format=%s extent=%ux%u "
        "pitch=%u bpp=%u tiled=%u bytes=%llu flagsBefore=0x%x flagsAfter=0x%x "
        "usageVo=%u usageRt=%u bound=%u target=%u cpuHash=0x%llx cpuNz=%llu "
        "gpuHash=0x%llx gpuNz=%llu,%llu,%llu,%llu mid=%02x%02x%02x%02x",
        static_cast<unsigned long long>(sequence), image_id.index,
        static_cast<unsigned long long>(image.info.guest_address),
        vk::to_string(image.info.pixel_format).c_str(), image.info.size.width,
        image.info.size.height, image.info.pitch, bpp, image.info.props.is_tiled ? 1u : 0u,
        static_cast<unsigned long long>(download_size), flags_before_refresh,
        static_cast<u32>(image.flags), image.usage.vo_surface, image.usage.render_target,
        image.binding.is_bound, image.binding.is_target,
        static_cast<unsigned long long>(cpu_hash), static_cast<unsigned long long>(cpu_nonzero),
        static_cast<unsigned long long>(gpu_hash),
        static_cast<unsigned long long>(gpu_nonzero[0]),
        static_cast<unsigned long long>(gpu_nonzero[1]),
        static_cast<unsigned long long>(gpu_nonzero[2]),
        static_cast<unsigned long long>(gpu_nonzero[3]), gpu_bytes[middle + 0],
        gpu_bytes[middle + 1], gpu_bytes[middle + 2], gpu_bytes[middle + 3]);

#if defined(__ANDROID__)
    static std::mutex dump_mutex;
    static std::array<u64, 8> dumped_source_addresses{};
    if (bpp >= 4 && (gpu_nonzero[0] + gpu_nonzero[1] + gpu_nonzero[2]) != 0) {
        std::scoped_lock lock{dump_mutex};
        const u64 source_address = image.info.guest_address;
        static std::atomic<u32> live_dump_count{};
        const bool live_dump_requested =
            ::access("/data/data/app.lsx4.android/files/lsx4-home/"
                     "run-trace-videoout-source-live-dump",
                     F_OK) == 0;
        const u32 live_dump_ordinal = live_dump_requested
                                          ? live_dump_count.fetch_add(
                                                1, std::memory_order_relaxed)
                                          : std::numeric_limits<u32>::max();
        if (live_dump_ordinal < 8) {
            char live_path[256]{};
            std::snprintf(
                live_path, sizeof(live_path),
                "/data/user/0/app.lsx4.android/files/lsx4-home/"
                "live-videoout-source-seq-%06llu-%016llx.ppm",
                static_cast<unsigned long long>(sequence),
                static_cast<unsigned long long>(source_address));
            ExecutorDumpRtPpm(live_path, gpu_bytes, image.info.size.width,
                              image.info.size.height, image.info.pitch * bpp, bpp);
        }
        const bool already_dumped =
            std::ranges::find(dumped_source_addresses, source_address) !=
            dumped_source_addresses.end();
        const auto free_slot = std::ranges::find(dumped_source_addresses, 0);
        if (!already_dumped && free_slot != dumped_source_addresses.end()) {
            *free_slot = source_address;
            char path[256]{};
            std::snprintf(
                path, sizeof(path),
                "/data/user/0/app.lsx4.android/files/lsx4-home/"
                "live-videoout-source-%016llx.ppm",
                static_cast<unsigned long long>(source_address));
            ExecutorDumpRtPpm(path, gpu_bytes, image.info.size.width, image.info.size.height,
                              image.info.pitch * bpp, bpp);
        }
    }
#endif
#else
    (void)image_id;
    (void)sequence;
    (void)flags_before_refresh;
#endif
}

void TextureCache::ExecutorTrackLiveDrawnColorTarget(ImageId image_id, u64 image_uid) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const auto& capability_profile = instance.GetCapabilityProfile();
    // Slot ids are recyclable cache indices. A frame candidate therefore has no stable identity
    // without the image generation that was current when the draw was recorded. Keep the
    // generation-aware route as a shared correctness invariant; the old A750 marker remains
    // diagnostic-only and must not decide whether stale candidates can reach the presenter.
    const bool uid_guard = true;
    const bool slot0_tracking = capability_profile.diagnostic_a750_videoout_slot0_tracking;
    if (uid_guard) {
        if (image_uid == 0) {
            static std::atomic<u32> invalid_uid_logs{};
            const u32 ordinal = invalid_uid_logs.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 8) {
                __android_log_print(
                    ANDROID_LOG_WARN, "LSX4Native",
                    "[EXECUTOR_A750_VIDEOOUT_CANDIDATE_UID_DROP] phase=track reason=empty "
                    "ordinal=%u id=%u uid=%llu",
                    ordinal + 1, image_id.index,
                    static_cast<unsigned long long>(image_uid));
            }
            return;
        }

        if (executor_live_uid_last_color_uid.exchange(image_uid, std::memory_order_acq_rel) ==
            image_uid) {
            return;
        }

        // DeleteImage already owns the cache mutex and then takes the candidate mutex.  Use the
        // same ordering here so a SlotId cannot be recycled between UID validation and enqueue.
        std::unique_lock cache_lock{mutex};
        std::scoped_lock candidates_lock{executor_replay_candidates_mutex};
        const bool allocated = slot_images.is_allocated(image_id);
        const bool registered =
            allocated && True(slot_images[image_id].flags & ImageFlagBits::Registered);
        const u64 current_uid = allocated ? slot_images[image_id].image_uid : 0;
        if (!allocated || !registered || current_uid != image_uid) {
            u64 expected_uid = image_uid;
            executor_live_uid_last_color_uid.compare_exchange_strong(
                expected_uid, 0, std::memory_order_release, std::memory_order_relaxed);
            static std::atomic<u32> stale_uid_logs{};
            const u32 ordinal = stale_uid_logs.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 16) {
                __android_log_print(
                    ANDROID_LOG_WARN, "LSX4Native",
                    "[EXECUTOR_A750_VIDEOOUT_CANDIDATE_UID_DROP] phase=track reason=stale "
                    "ordinal=%u id=%u expectedUid=%llu allocated=%u registered=%u "
                    "currentUid=%llu",
                    ordinal + 1, image_id.index,
                    static_cast<unsigned long long>(image_uid), allocated ? 1u : 0u,
                    registered ? 1u : 0u, static_cast<unsigned long long>(current_uid));
            }
            return;
        }

        const ExecutorLiveColorCandidateUid candidate{image_id, image_uid};
        if (executor_live_drawn_color_candidates_uid.empty() ||
            executor_live_drawn_color_candidates_uid.back().image_id != candidate.image_id ||
            executor_live_drawn_color_candidates_uid.back().image_uid != candidate.image_uid) {
            executor_live_drawn_color_candidates_uid.push_back(candidate);
            static std::atomic<u32> generation_enqueue_logs{};
            const u32 ordinal =
                generation_enqueue_logs.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 8) {
                const auto& image = slot_images[image_id];
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_VIDEOOUT_GENERATION_GUARD] phase=enqueue ordinal=%u "
                    "id=%u uid=%llu flags=0x%x usageVo=%u usageRt=%u addr=0x%llx",
                    ordinal + 1, image_id.index,
                    static_cast<unsigned long long>(image_uid),
                    static_cast<u32>(image.flags), image.usage.vo_surface,
                    image.usage.render_target,
                    static_cast<unsigned long long>(image.info.guest_address));
            }
        }
        return;
    }

    if (!image_id) {
        return;
    }
    if (instance.UseLegacyRenderPass()) {
        static std::atomic<u32> legacy_track_logs{};
        const u32 ordinal = legacy_track_logs.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 8) {
            const auto& image = GetImage(image_id);
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_MALI_DRAW_TARGET] event=track ordinal=%u id=%u "
                                "extent=%ux%u addr=0x%llx vo=%u rt=%u",
                                ordinal + 1, image_id.index, image.info.size.width,
                                image.info.size.height,
                                static_cast<unsigned long long>(image.info.guest_address),
                                image.usage.vo_surface, image.usage.render_target);
        }
    }
    const u32 image_index = image_id.index;
    if (slot0_tracking) {
        // SlotId index zero is valid, so reserve atomic zero only as the reset sentinel by storing
        // index+1.  This path deliberately keeps the established ImageId-only queue and therefore
        // isolates the slot-zero cadence bug from the generation-aware UID experiment above.
        const u32 tracking_key = image_index + 1;
        if (executor_live_last_color_target.exchange(tracking_key,
                                                     std::memory_order_relaxed) == tracking_key) {
            return;
        }
    } else if (executor_live_last_color_target.exchange(image_index,
                                                        std::memory_order_relaxed) == image_index) {
        return;
    }
    std::scoped_lock lk{executor_replay_candidates_mutex};
    if (executor_live_drawn_color_candidates.empty() ||
        executor_live_drawn_color_candidates.back() != image_id) {
        executor_live_drawn_color_candidates.push_back(image_id);
        if (slot0_tracking && image_index == 0) {
            static std::atomic<u32> slot0_track_logs{};
            const u32 ordinal = slot0_track_logs.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 16) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_A750_VIDEOOUT_SLOT0_TRACKING] phase=enqueue ordinal=%u id=0",
                    ordinal + 1);
            }
        }
    }
#else
    (void)image_id;
    (void)image_uid;
#endif
}

void TextureCache::ExecutorTrackLiveResolvedColorTarget(ImageId source_id, u64 source_uid,
                                                        ImageId destination_id,
                                                        u64 destination_uid) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    if (source_uid == 0 || destination_uid == 0) {
        return;
    }
    std::unique_lock cache_lock{mutex};
    if (!slot_images.is_allocated(source_id) || !slot_images.is_allocated(destination_id)) {
        return;
    }
    const auto& source = slot_images[source_id];
    const auto& destination = slot_images[destination_id];
    if (source.image_uid != source_uid || destination.image_uid != destination_uid ||
        False(source.flags & ImageFlagBits::Registered) ||
        False(destination.flags & ImageFlagBits::Registered)) {
        return;
    }
    const ExecutorLiveResolvePairUid pair{
        source_id, source_uid, destination_id, destination_uid};
    std::scoped_lock candidates_lock{executor_replay_candidates_mutex};
    if (executor_live_resolve_pairs_uid.empty() ||
        executor_live_resolve_pairs_uid.back().source_id != pair.source_id ||
        executor_live_resolve_pairs_uid.back().source_uid != pair.source_uid ||
        executor_live_resolve_pairs_uid.back().destination_id != pair.destination_id ||
        executor_live_resolve_pairs_uid.back().destination_uid != pair.destination_uid) {
        executor_live_resolve_pairs_uid.push_back(pair);
    }
#else
    (void)source_id;
    (void)source_uid;
    (void)destination_id;
    (void)destination_uid;
#endif
}

ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget(u32 width, u32 height,
                                                           ImageId preferred) {
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    const bool uid_guard = true;
    std::vector<ImageId> candidates;
    std::vector<ExecutorLiveResolvePairUid> resolve_pairs;
    std::unique_lock<std::mutex> cache_lock{mutex, std::defer_lock};
    if (uid_guard) {
        cache_lock.lock();
        std::vector<ExecutorLiveColorCandidateUid> guarded_candidates;
        {
            std::scoped_lock candidates_lock{executor_replay_candidates_mutex};
            guarded_candidates.swap(executor_live_drawn_color_candidates_uid);
            resolve_pairs.swap(executor_live_resolve_pairs_uid);
            executor_live_uid_last_color_uid.store(0, std::memory_order_release);
        }

        u32 invalid_candidates = 0;
        for (const auto& candidate : guarded_candidates) {
            if (!slot_images.is_allocated(candidate.image_id)) {
                ++invalid_candidates;
                continue;
            }
            const auto& image = slot_images[candidate.image_id];
            if (False(image.flags & ImageFlagBits::Registered) ||
                image.image_uid != candidate.image_uid) {
                ++invalid_candidates;
                continue;
            }
            candidates.push_back(candidate.image_id);
            static std::atomic<u32> generation_consume_logs{};
            const u32 ordinal =
                generation_consume_logs.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 8) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_VIDEOOUT_GENERATION_GUARD] phase=consume ordinal=%u "
                    "id=%u uid=%llu flags=0x%x usageVo=%u usageRt=%u addr=0x%llx",
                    ordinal + 1, candidate.image_id.index,
                    static_cast<unsigned long long>(candidate.image_uid),
                    static_cast<u32>(image.flags), image.usage.vo_surface,
                    image.usage.render_target,
                    static_cast<unsigned long long>(image.info.guest_address));
            }
        }
        if (invalid_candidates != 0) {
            static std::atomic<u32> invalid_consume_logs{};
            const u32 ordinal = invalid_consume_logs.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 16) {
                __android_log_print(
                    ANDROID_LOG_WARN, "LSX4Native",
                    "[EXECUTOR_A750_VIDEOOUT_CANDIDATE_UID_DROP] phase=consume ordinal=%u "
                    "invalid=%u total=%zu",
                    ordinal + 1, invalid_candidates, guarded_candidates.size());
            }
        }
    } else {
        // Marker-off behavior is intentionally the established ImageId-only queue.
        std::scoped_lock candidates_lock{executor_replay_candidates_mutex};
        candidates.swap(executor_live_drawn_color_candidates);
        executor_live_last_color_target.store(0, std::memory_order_relaxed);
    }

    if (instance.UseLegacyRenderPass()) {
        static std::atomic<u32> legacy_consume_logs{};
        const u32 ordinal = legacy_consume_logs.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 8) {
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_MALI_DRAW_TARGET] event=consume ordinal=%u "
                                "candidates=%zu preferred=%u",
                                ordinal + 1, candidates.size(), preferred.index);
        }
    }

    if (width == 0 || height == 0) {
        return {};
    }

    const auto candidate_valid = [&](const ImageId id) {
        if (uid_guard) {
            return slot_images.is_allocated(id) &&
                   True(slot_images[id].flags & ImageFlagBits::Registered);
        }
        return id && slot_images.is_allocated(id);
    };
    const bool preferred_valid = candidate_valid(preferred);
    const Image* preferred_image = preferred_valid ? &GetImage(preferred) : nullptr;
    const u32 tile_aligned_height = (height + 15u) & ~15u;

    const auto is_authoritative_render_target_alias =
        [&](const Image& image, const bool allow_tile_aligned_height) {
            if (preferred_image == nullptr || image.info.props.is_depth ||
                !image.usage.render_target || image.usage.vo_surface ||
                False(image.flags & ImageFlagBits::Registered) ||
                False(image.flags & ImageFlagBits::GpuModified)) {
                return false;
            }

            const auto& candidate_info = image.info;
            const auto& preferred_info = preferred_image->info;
            const bool extent_matches =
                candidate_info.size.width == preferred_info.size.width &&
                candidate_info.size.depth == preferred_info.size.depth &&
                (allow_tile_aligned_height
                     ? preferred_info.size.height == height &&
                           candidate_info.size.height == tile_aligned_height
                     : candidate_info.size.height == preferred_info.size.height);
            const bool allocation_size_matches =
                allow_tile_aligned_height
                    ? candidate_info.guest_size >= preferred_info.guest_size
                    : candidate_info.guest_size == preferred_info.guest_size;
            return candidate_info.guest_address == preferred_info.guest_address &&
                   allocation_size_matches && extent_matches &&
                   candidate_info.pitch == preferred_info.pitch &&
                   candidate_info.resources == preferred_info.resources &&
                   candidate_info.num_samples == preferred_info.num_samples &&
                   candidate_info.num_bits == preferred_info.num_bits &&
                   candidate_info.type == preferred_info.type &&
                   candidate_info.tile_mode == preferred_info.tile_mode &&
                   IsVulkanFormatCompatible(candidate_info.pixel_format,
                                            preferred_info.pixel_format);
        };

    static std::atomic<bool> traced_live_candidate_set{false};
    if (!uid_guard && std::getenv("EXECUTOR_TRACE_VIDEOOUT_SOURCE") != nullptr &&
        !traced_live_candidate_set.exchange(true, std::memory_order_relaxed)) {
        std::array<u32, 16> traced_ids{};
        std::size_t traced_count = 0;
        for (const ImageId id : candidates) {
            if (!candidate_valid(id) ||
                std::ranges::find(traced_ids.begin(), traced_ids.begin() + traced_count, id.index) !=
                    traced_ids.begin() + traced_count) {
                continue;
            }
            traced_ids[traced_count++] = id.index;
            ExecutorTraceVideoOutSource(id, 0x10000u + traced_count, 0);
            if (traced_count == traced_ids.size()) {
                break;
            }
        }
    }

    // A resolve destination can have a distinct cache identity from the registered VideoOut image
    // even though it names the exact same guest allocation. The recorded source/destination UID
    // lineage makes that destination authoritative. Present the destination so the precise resolve
    // subresource is retained; never substitute the whole source backing (which may be MSAA-sized).
    if (preferred_image) {
        for (auto it = resolve_pairs.rbegin(); it != resolve_pairs.rend(); ++it) {
            if (!slot_images.is_allocated(it->source_id) ||
                !slot_images.is_allocated(it->destination_id)) {
                continue;
            }
            const auto& source = slot_images[it->source_id];
            const auto& destination = slot_images[it->destination_id];
            if (source.image_uid != it->source_uid ||
                destination.image_uid != it->destination_uid ||
                False(source.flags & ImageFlagBits::Registered) ||
                False(destination.flags & ImageFlagBits::Registered) ||
                False(source.flags & ImageFlagBits::GpuModified) ||
                False(destination.flags & ImageFlagBits::GpuModified) ||
                !source.usage.render_target || source.info.props.is_depth ||
                destination.info.props.is_depth || destination.backing == nullptr ||
                destination.info.guest_address != preferred_image->info.guest_address ||
                destination.info.guest_size != preferred_image->info.guest_size ||
                destination.info.size != preferred_image->info.size ||
                destination.info.pitch != preferred_image->info.pitch ||
                destination.info.resources != preferred_image->info.resources ||
                destination.info.num_bits != preferred_image->info.num_bits ||
                destination.info.type != preferred_image->info.type ||
                destination.info.tile_mode != preferred_image->info.tile_mode ||
                !IsVulkanFormatCompatible(destination.info.pixel_format,
                                          preferred_image->info.pixel_format)) {
                continue;
            }
            static std::atomic<u32> resolve_source_bypass_logs{};
            const u32 ordinal =
                resolve_source_bypass_logs.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 8) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_VIDEOOUT_RESOLVE_SOURCE] ordinal=%u source=%u/%llu "
                    "destination=%u/%llu scanout=%u sourceBackingSamples=%u "
                    "destinationBackingSamples=%u action=present-destination",
                    ordinal + 1, it->source_id.index,
                    static_cast<unsigned long long>(it->source_uid),
                    it->destination_id.index,
                    static_cast<unsigned long long>(it->destination_uid), preferred.index,
                    source.backing ? source.backing->num_samples : 0u,
                    destination.backing->num_samples);
            }
            return it->destination_id;
        }
    }

    if (preferred_image && tile_aligned_height > height) {
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            if (!candidate_valid(*it) || *it == preferred) {
                continue;
            }
            const auto& image = GetImage(*it);
            if (image.info.size.width == width &&
                is_authoritative_render_target_alias(image, true)) {
                return *it;
            }
        }
    }

    if (preferred_image) {
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            if (!candidate_valid(*it) || *it == preferred) {
                continue;
            }
            const auto& image = GetImage(*it);
            const bool authoritative =
                is_authoritative_render_target_alias(image, false);
            if (image.info.guest_address == preferred_image->info.guest_address) {
                static std::atomic<u32> alias_verdict_logs{};
                const u32 ordinal =
                    alias_verdict_logs.fetch_add(1, std::memory_order_relaxed);
                if (ordinal < 8) {
                    __android_log_print(
                        ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_VIDEOOUT_GENERATION_GUARD] phase=alias-verdict "
                        "ordinal=%u id=%u flags=0x%x usageVo=%u usageRt=%u "
                        "size=%u extent=%ux%ux%u pitch=%u levels=%u layers=%u "
                        "samples=%u bits=%u type=%llu tile=%u format=%u accepted=%u",
                        ordinal + 1, it->index, static_cast<u32>(image.flags),
                        image.usage.vo_surface, image.usage.render_target,
                        image.info.guest_size, image.info.size.width,
                        image.info.size.height, image.info.size.depth,
                        image.info.pitch, image.info.resources.levels,
                        image.info.resources.layers, image.info.num_samples,
                        image.info.num_bits,
                        static_cast<unsigned long long>(image.info.type),
                        static_cast<u32>(image.info.tile_mode),
                        static_cast<u32>(image.info.pixel_format),
                        authoritative ? 1u : 0u);
                }
            }
            if (authoritative) {
                return *it;
            }
        }

        // Some games render the current frame to a separate linear allocation whose height is
        // rounded up to the hardware tile granularity, then flip a 16-row-cropped SRGB scanout.
        // Requiring the render target to alias the scanout address loses that live frame and
        // presents the cleared VideoOut image instead. Keep this fallback much narrower than the
        // old same-size heuristic: the candidate must have been drawn in this flip, remain a
        // registered GPU-modified color target, and match the scanout's observable row layout
        // except for the exact tile-height payload and a compatible UNORM/SRGB format.
        const auto& preferred_info = preferred_image->info;
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
            if (!candidate_valid(*it) || *it == preferred) {
                continue;
            }
            const auto& image = GetImage(*it);
            const auto& candidate_info = image.info;
            const bool detached_tile_aligned_live_target =
                !candidate_info.props.is_depth && image.usage.render_target &&
                !image.usage.vo_surface &&
                True(image.flags & ImageFlagBits::Registered) &&
                True(image.flags & ImageFlagBits::GpuModified) &&
                candidate_info.guest_address != preferred_info.guest_address &&
                candidate_info.size.width == preferred_info.size.width &&
                candidate_info.size.height == tile_aligned_height &&
                candidate_info.pitch == preferred_info.pitch &&
                candidate_info.num_bits == preferred_info.num_bits &&
                IsVideoOutUnormSrgbPair(candidate_info.pixel_format,
                                       preferred_info.pixel_format) &&
                IsVulkanFormatCompatible(candidate_info.pixel_format,
                                         preferred_info.pixel_format);
            if (!detached_tile_aligned_live_target) {
                continue;
            }
            static std::atomic<u32> detached_tile_aligned_logs{};
            const u32 ordinal =
                detached_tile_aligned_logs.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 8) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_VIDEOOUT_TILE_ALIGNED_LIVE_TARGET] ordinal=%u "
                    "candidate=%u scanout=%u candidateAddr=0x%llx scanoutAddr=0x%llx "
                    "extent=%ux%u cropHeight=%u format=%u scanoutFormat=%u",
                    ordinal + 1, it->index, preferred.index,
                    static_cast<unsigned long long>(candidate_info.guest_address),
                    static_cast<unsigned long long>(preferred_info.guest_address),
                    candidate_info.size.width, candidate_info.size.height, height,
                    static_cast<u32>(candidate_info.pixel_format),
                    static_cast<u32>(preferred_info.pixel_format));
            }
            return *it;
        }
    }

    // A GPU-authored, tile-height render target is the PS4 frame source even when the
    // registered 1080-row VideoOut image was also touched during this flip. Prefer the
    // narrowly proven render target above before accepting that scanout descriptor;
    // otherwise a cleared scanout hides the completed 1088-to-1080 cropped frame.
    if (preferred_valid &&
        std::ranges::find(candidates, preferred) != candidates.end()) {
        return preferred;
    }

    // The registered VideoOut descriptor remains the fallback authority, but a current
    // same-allocation render-target alias above may contain the only GPU-written copy of this
    // flip (commonly UNORM rendering into an SRGB scanout allocation). UpdateImage cannot
    // resolve between sibling VkImages through guest memory, so reject unrelated candidates
    // only after that compatible alias has had a chance to win.
    if (preferred_valid && !instance.UseLegacyRenderPass()) {
        return {};
    }

    ImageId exact_best{};
    int exact_best_score = std::numeric_limits<int>::min();
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (!candidate_valid(*it)) {
            continue;
        }
        const auto& image = GetImage(*it);
        if (!image.info.props.is_depth && image.info.size.width == width &&
            image.info.size.height == height) {
            int score = image.usage.vo_surface ? 8 : 0;
            if (preferred_image) {
                score += IsVulkanFormatCompatible(image.info.pixel_format,
                                                   preferred_image->info.pixel_format)
                    ? 4
                    : 0;
                score += image.info.type == preferred_image->info.type ? 2 : 0;
            }
            if (score > exact_best_score) {
                exact_best = *it;
                exact_best_score = score;
            }
        }
    }
    if (exact_best) {
        return exact_best;
    }

    ImageId best{};
    u64 best_area = 0;
    u32 best_width = 0;
    u32 best_height = 0;
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (!candidate_valid(*it)) {
            continue;
        }
        const auto& image = GetImage(*it);
        if (image.info.props.is_depth) {
            continue;
        }
        const u32 candidate_width = image.info.size.width;
        const u32 candidate_height = image.info.size.height;
        if (candidate_width < width / 2 || candidate_height < height / 2) {
            continue;
        }
        const u64 lhs = static_cast<u64>(candidate_width) * height;
        const u64 rhs = static_cast<u64>(candidate_height) * width;
        const u64 difference = lhs > rhs ? lhs - rhs : rhs - lhs;
        const u64 aspect_scale = std::max(lhs, rhs);
        if (difference > std::max<u64>(1, aspect_scale / 100)) {
            continue;
        }
        const u64 area = static_cast<u64>(candidate_width) * candidate_height;
        if (area > best_area) {
            best = *it;
            best_area = area;
            best_width = candidate_width;
            best_height = candidate_height;
        }
    }
    if (best) {
        static std::atomic<bool> logged_dynamic_source{false};
        if (!logged_dynamic_source.exchange(true, std::memory_order_relaxed)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_FRAME_SOURCE_BRIDGE] scanout=%ux%u source=%ux%u target=%u candidates=%zu",
                width, height, best_width, best_height, best.index, candidates.size());
        }
    }
    return best;
#else
    (void)width;
    (void)height;
    (void)preferred;
    return {};
#endif
}

void TextureCache::ExecutorReplayReadbackColorTarget() {
#ifdef __ANDROID__
    std::vector<ImageId> candidates;
    {
        std::scoped_lock lk{executor_replay_candidates_mutex};
        candidates = executor_replay_color_candidates;
    }
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_RT_READBACK] enter candidates=%zu fallback=%u",
                        candidates.size(), executor_replay_color_target.index);
    if (candidates.empty() && executor_replay_color_target) {
        candidates.push_back(executor_replay_color_target);
    }
    if (candidates.empty()) {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_RT_READBACK] skip reason=no_candidates");
        return;
    }
    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::ImageDownload);

    struct RtStats {
        ImageId id{};
        u32 w{}, h{};
        u32 pitch{}, bpp{};
        u64 nzc[4]{};
        u32 unique_rgb{};
        u8 mid[8]{};
        std::vector<u8> rgba;
        bool ok{false};
    };
    auto readback = [&](ImageId id) -> RtStats {
        RtStats st{};
        st.id = id;
        Image& image = slot_images[id];
        const u32 bpp = std::max<u32>(image.info.num_bits / 8u, 1u);
        const u32 w = image.info.size.width;
        const u32 h = image.info.size.height;
        const u64 download_size = static_cast<u64>(image.info.pitch) * h * bpp;
        if (download_size == 0 || download_size > (64ull * 1024 * 1024)) {
            __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                                "[EXECUTOR_RT_READBACK] skip_candidate id=%u reason=bad_size "
                                "extent=%ux%u pitch=%u bpp=%u size=%llu bits=%u",
                                id.index, w, h, image.info.pitch, bpp,
                                static_cast<unsigned long long>(download_size),
                                image.info.num_bits);
            return st;
        }
        auto& download_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
        const auto [download, offset] = download_buffer.Map(download_size);
        download_buffer.Commit();
        const vk::BufferImageCopy region = {
            .bufferOffset = offset,
            .bufferRowLength = image.info.pitch,
            .bufferImageHeight = h,
            .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                 .mipLevel = 0,
                                 .baseArrayLayer = 0,
                                 .layerCount = 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {w, h, 1},
        };
        const auto cmdbuf = scheduler.CommandBuffer();
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
        cmdbuf.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                 download_buffer.Handle(), region);
        scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::TextureProbe);
        const u8* px = reinterpret_cast<const u8*>(download);
        const u64 scan = std::min<u64>(download_size, 8ull * 1024 * 1024);
        for (u64 i = 0; i < scan; ++i) {
            if (px[i] != 0) {
                ++st.nzc[i & 3];
            }
        }
        const u64 mid = (download_size / 2) & ~u64(15);
        for (int k = 0; k < 8; ++k) {
            st.mid[k] = px[mid + k];
        }
        if (bpp >= 4 && w > 0 && h > 0) {
            std::array<u32, 256> seen{};
            u32 seen_count = 0;
            constexpr u32 GridX = 64;
            constexpr u32 GridY = 36;
            for (u32 gy = 0; gy < GridY; ++gy) {
                const u32 y = std::min<u32>((static_cast<u64>(gy) * h) / GridY, h - 1);
                for (u32 gx = 0; gx < GridX; ++gx) {
                    const u32 x = std::min<u32>((static_cast<u64>(gx) * w) / GridX, w - 1);
                    const u64 off = (static_cast<u64>(y) * image.info.pitch + x) * bpp;
                    if (off + 2 >= download_size) {
                        continue;
                    }
                    const u32 rgb = static_cast<u32>(px[off]) |
                                    (static_cast<u32>(px[off + 1]) << 8) |
                                    (static_cast<u32>(px[off + 2]) << 16);
                    if (rgb == 0) {
                        continue;
                    }
                    bool known = false;
                    for (u32 i = 0; i < seen_count; ++i) {
                        if (seen[i] == rgb) {
                            known = true;
                            break;
                        }
                    }
                    if (!known) {
                        seen[seen_count++] = rgb;
                        if (seen_count == seen.size()) {
                            break;
                        }
                    }
                }
                if (seen_count == seen.size()) {
                    break;
                }
            }
            st.unique_rgb = seen_count;
        }
        st.w = w;
        st.h = h;
        st.pitch = image.info.pitch;
        st.bpp = bpp;
        if (bpp >= 4) {
            st.rgba.assign(px, px + download_size);
            for (u64 off = 3; off < st.rgba.size(); off += bpp) {
                st.rgba[off] = 0xff;
            }
        }
        st.ok = true;
        return st;
    };

    ImageId best{};
    u64 best_score = 0;
    RtStats best_stats{};
    u32 order = 0;
    FILE* candidate_meta =
        std::fopen("/data/user/0/app.lsx4.android/files/lsx4-home/live-rt-candidates.txt",
                   "wb");
    for (const ImageId id : candidates) {
        ++order;
        const RtStats st = readback(id);
        if (!st.ok) {
            continue;
        }
        const u32 rgb_channels = (st.nzc[0] > 0) + (st.nzc[1] > 0) + (st.nzc[2] > 0);
        const u64 area = static_cast<u64>(st.w) * st.h;
        const bool full_res = st.w >= 1280 && st.h >= 720;
        const u64 rgb_bytes = st.nzc[0] + st.nzc[1] + st.nzc[2];
        const u64 score = (full_res ? (1ull << 62) : 0) |
                          (static_cast<u64>(rgb_channels) << 58) |
                          (static_cast<u64>(st.unique_rgb) << 48) |
                          std::min<u64>(rgb_bytes, (1ull << 48) - 1);
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_RT_READBACK] cand extent=%ux%u rgbCh=%u nzR=%llu nzG=%llu "
                            "nzB=%llu nzA=%llu uniqueRGB=%u order=%u id=%u midpx=%02x%02x%02x%02x "
                            "score=%llu",
                            st.w, st.h, rgb_channels, (unsigned long long)st.nzc[0],
                            (unsigned long long)st.nzc[1], (unsigned long long)st.nzc[2],
                            (unsigned long long)st.nzc[3], st.unique_rgb, order, id.index,
                            st.mid[0], st.mid[1], st.mid[2], st.mid[3],
                            (unsigned long long)score);
        if (st.bpp >= 4 && !st.rgba.empty()) {
            char path[192]{};
            std::snprintf(path, sizeof(path),
                          "/data/user/0/app.lsx4.android/files/lsx4-home/"
                          "live-rt-cand-%02u-id%u.ppm",
                          order, id.index);
            ExecutorDumpRtPpm(path, st.rgba.data(), st.w, st.h, st.pitch * st.bpp, st.bpp);
        }
        if (candidate_meta) {
            std::fprintf(candidate_meta,
                         "order=%u id=%u extent=%ux%u pitch=%u bpp=%u rgbCh=%u "
                         "nzR=%llu nzG=%llu nzB=%llu nzA=%llu uniqueRGB=%u "
                         "midpx=%02x%02x%02x%02x score=%llu\n",
                         order, id.index, st.w, st.h, st.pitch, st.bpp, rgb_channels,
                         (unsigned long long)st.nzc[0], (unsigned long long)st.nzc[1],
                         (unsigned long long)st.nzc[2], (unsigned long long)st.nzc[3],
                         st.unique_rgb, st.mid[0], st.mid[1], st.mid[2], st.mid[3],
                         (unsigned long long)score);
            std::fflush(candidate_meta);
        }
        if (score >= best_score) {
            best_score = score;
            best = id;
            best_stats = st;
        }
    }
    if (candidate_meta) {
        std::fclose(candidate_meta);
    }
    if (best) {
        executor_replay_color_target = best;
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_RT_READBACK] CHOSEN extent=%ux%u uniqueRGB=%u "
                            "midpx=%02x%02x%02x%02x",
                            best_stats.w, best_stats.h, best_stats.unique_rgb, best_stats.mid[0],
                            best_stats.mid[1], best_stats.mid[2], best_stats.mid[3]);
        if (best_stats.bpp >= 4 && !best_stats.rgba.empty()) {
            ExecutorDumpRtPpm(
                "/data/user/0/app.lsx4.android/files/lsx4-home/live-rt-chosen.ppm",
                best_stats.rgba.data(), best_stats.w, best_stats.h,
                best_stats.pitch * best_stats.bpp, best_stats.bpp);
        }
        if (std::getenv("EXECUTOR_LIVE_CPU_PRESENT_RT") != nullptr && best_stats.bpp >= 4 &&
            !best_stats.rgba.empty()) {
            const u32 stride = best_stats.pitch * best_stats.bpp;
            const int present_rc = executor_lsx4_runtime_present_guest_frame(
                best_stats.rgba.data(), best_stats.w, best_stats.h, stride, 1u, 1u);
            __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                                "[EXECUTOR_RT_CPU_PRESENT] rc=%d extent=%ux%u stride=%u "
                                "bytes=%zu uniqueRGB=%u midpx=%02x%02x%02x%02x",
                                present_rc, best_stats.w, best_stats.h, stride,
                                best_stats.rgba.size(), best_stats.unique_rgb, best_stats.mid[0],
                                best_stats.mid[1], best_stats.mid[2], best_stats.mid[3]);
        }
    } else {
        __android_log_print(ANDROID_LOG_WARN, "LSX4Native",
                            "[EXECUTOR_RT_READBACK] no_valid_candidate count=%zu",
                            candidates.size());
    }
    {
        std::scoped_lock lk{executor_replay_candidates_mutex};
        executor_replay_color_candidates.clear();
    }
#endif
}

void TextureCache::MarkAsMaybeDirty(ImageId image_id, Image& image) {
    if (image.hash == 0) {
        const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
        image.hash = XXH3_64bits(addr, image.info.guest_size);
    }
    image.flags |= ImageFlagBits::MaybeCpuDirty;
    UntrackImage(image_id);
}

void TextureCache::InvalidateMemory(VAddr addr, size_t size) {
    std::scoped_lock lock{mutex};
    InvalidateMemoryLocked(addr, size);
}

void TextureCache::PublishGpuImagesBeforeCpuWrite(VAddr addr, size_t size) {
    if (size == 0 || addr > std::numeric_limits<VAddr>::max() - size) {
        return;
    }

    boost::container::small_vector<std::pair<VAddr, u64>, 4> published_ranges;
    const VAddr write_end = addr + size;
    {
        std::scoped_lock lock{mutex};
        const auto pages_start = PageManager::GetPageAddr(addr);
        const auto pages_end = PageManager::GetNextPageAddr(write_end - 1);
        ForEachImageInRegion(
            pages_start, pages_end - pages_start,
            [&](ImageId image_id, Image& image) {
                const VAddr image_begin = image.info.guest_address;
                const u64 image_size = image.info.guest_size;
                const VAddr image_end = image_begin + image_size;
                const bool fully_overwritten =
                    addr <= image_begin && write_end >= image_end;
                if (fully_overwritten || !image.SafeToDownload()) {
                    return;
                }

                // This path is entered synchronously from the write-protection
                // fault, before the guest instruction is retried. Tiling the
                // current VkImage back to guest memory therefore preserves the
                // GPU-authored bytes; the pending CPU write then changes only its
                // intended page/range and the ordinary refresh may safely upload
                // the complete allocation later.
                if (DownloadImageMemory(
                        image_id, true,
                        ImageDownloadClassification::LinearImage)) {
                    published_ranges.emplace_back(image_begin, image_size);
                }
            });
    }

    // TryWriteBacking bypasses guest write faults, so the image readback above
    // does not by itself invalidate an overlapping cached VkBuffer. Publish the
    // complete reconstructed backing to BufferCache before the faulting store is
    // retried. Otherwise a later tiled upload can combine the one dirty CPU page
    // with stale zeroes from the old buffer and destroy every untouched texel.
    // Keep the texture-cache mutex out of this call: BufferCache may synchronously
    // retire a newer buffer owner before accepting the CPU backing.
    for (const auto& [published_addr, published_size] : published_ranges) {
        buffer_cache.InvalidateMemory(published_addr, published_size);
    }
}

void TextureCache::InvalidateMemoryLocked(VAddr addr, size_t size) {
    const auto pages_start = PageManager::GetPageAddr(addr);
    const auto pages_end = PageManager::GetNextPageAddr(addr + size - 1);
    ForEachImageInRegion(pages_start, pages_end - pages_start, [&](ImageId image_id, Image& image) {
        const auto image_begin = image.info.guest_address;
        const auto image_end = image.info.guest_address + image.info.guest_size;
        if (image.Overlaps(addr, size)) {
            image.flags |= ImageFlagBits::CpuDirty;
            UntrackImage(image_id);
        } else if (pages_end < image_end) {
            UntrackImageHead(image_id);
        } else if (image_begin < pages_start) {
            UntrackImageTail(image_id);
        } else {
            MarkAsMaybeDirty(image_id, image);
        }
    });
}

void TextureCache::InvalidateMemoryFromGPU(VAddr address, size_t max_size) {
    std::scoped_lock lock{mutex};
    ForEachImageInRegion(address, max_size, [&](ImageId image_id, Image& image) {
        if (image.info.guest_address != address) {
            return;
        }
        image.flags |= ImageFlagBits::GpuDirty;
    });
}

void TextureCache::UnmapMemory(VAddr cpu_addr, size_t size) {
    std::scoped_lock lk{mutex};

    ImageIds deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        FreeImage(id);
    }
}

ImageId TextureCache::ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                          ImageId cache_image_id) {
    auto& cache_image = slot_images[cache_image_id];

    if (!cache_image.info.props.is_depth && !requested_info.props.is_depth) {
        return {};
    }

    const bool stencil_match =
        requested_info.props.has_stencil == cache_image.info.props.has_stencil;
    const bool bpp_match = requested_info.num_bits == cache_image.info.num_bits;

    bool recreate = cache_image.info.resources < requested_info.resources;

    switch (binding) {
    case BindingType::Texture:
        recreate |= requested_info.props.is_depth && !cache_image.info.props.is_depth;
        break;
    case BindingType::Storage:
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::RenderTarget:
        ASSERT(!requested_info.props.is_depth);
        recreate |= cache_image.info.props.is_depth;
        break;
    case BindingType::DepthTarget:
        recreate |= !cache_image.info.props.is_depth;

        recreate |= cache_image.info.props.is_depth && !(stencil_match && bpp_match);
        break;
    default:
        break;
    }

    if (recreate) {
        auto new_info = requested_info;
        new_info.resources = std::max(requested_info.resources, cache_image.info.resources);
        const auto new_image_id =
            slot_images.insert(instance, scheduler, blit_helper, slot_image_views,
                               new_info, BackingRoleFor(binding));
        RegisterImage(new_image_id);

        auto& new_image = slot_images[new_image_id];
        new_image.usage = cache_image.usage;
        new_image.flags &= ~ImageFlagBits::Dirty;
        new_image.info.meta_info.htile_clear_mask = 0;

        if (cache_image.info.num_samples == 1 && new_info.num_samples == 1) {
            if (instance.IsMaintenance8Supported()) {
                new_image.CopyImage(cache_image);
            } else {
                const auto& copy_buffer = buffer_cache.GetUtilityBuffer(MemoryUsage::DeviceLocal);
                new_image.CopyImageWithBuffer(cache_image, copy_buffer.Handle(), 0);
            }
        } else if (cache_image.info.num_samples == 1 && new_info.props.is_depth &&
                   new_info.num_samples > 1) {
            cache_image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::AccessFlagBits2::eShaderRead, {});
            new_image.Transit(vk::ImageLayout::eDepthAttachmentOptimal,
                              vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {});
            blit_helper.ReinterpretColorAsMsDepth(
                new_info.size.width, new_info.size.height, new_info.num_samples,
                cache_image.info.pixel_format, new_info.pixel_format, cache_image.GetImage(),
                new_image.GetImage());
        } else {
            LOG_WARNING(Render_Vulkan, "Unimplemented depth overlap copy");
        }

        FreeImage(cache_image_id);
        return new_image_id;
    }

    return cache_image_id;
}

std::tuple<ImageId, int, int> TextureCache::ResolveOverlap(const ImageInfo& image_info,
                                                           BindingType binding,
                                                           ImageId cache_image_id,
                                                           ImageId merged_image_id,
                                                           std::unique_lock<std::mutex>& lock) {
    auto& cache_image = slot_images[cache_image_id];
    const bool safe_to_delete =
        scheduler.CurrentTick() - cache_image.tick_accessed_last > NumFramesBeforeRemoval;

    if (image_info.guest_address == cache_image.info.guest_address) {
        const u32 lhs_block_size = image_info.num_bits * image_info.num_samples;
        const u32 rhs_block_size = cache_image.info.num_bits * cache_image.info.num_samples;
        if (image_info.BlockDim() != cache_image.info.BlockDim() ||
            lhs_block_size != rhs_block_size) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        if (const auto depth_image_id = ResolveDepthOverlap(image_info, binding, cache_image_id)) {
            return {depth_image_id, -1, -1};
        }

        if (image_info.props.is_block && !cache_image.info.props.is_block) {
            return {ExpandImage(image_info, binding, cache_image_id, lock), -1, -1};
        }

        if (image_info.guest_size == cache_image.info.guest_size &&
            (image_info.type == AmdGpu::ImageType::Color3D ||
             cache_image.info.type == AmdGpu::ImageType::Color3D)) {
            return {ExpandImage(image_info, binding, cache_image_id, lock), -1, -1};
        }

        if (image_info.pixel_format != cache_image.info.pixel_format ||
            image_info.guest_size <= cache_image.info.guest_size) {
            auto result_id = merged_image_id ? merged_image_id : cache_image_id;
            const auto& result_image = slot_images[result_id];
            const bool is_compatible =
                IsVulkanFormatCompatible(result_image.info.pixel_format, image_info.pixel_format);
            return {is_compatible ? result_id : ImageId{}, -1, -1};
        }

        if (image_info.type == cache_image.info.type &&
            image_info.resources > cache_image.info.resources) {
            return {ExpandImage(image_info, binding, cache_image_id, lock), -1, -1};
        }

        if (image_info.tile_mode != cache_image.info.tile_mode) {
            if (safe_to_delete) {
                FreeImage(cache_image_id);
            }
            return {merged_image_id, -1, -1};
        }

        u64 expected_size =
            (static_cast<u64>(image_info.size.width) * static_cast<u64>(image_info.size.height) *
             static_cast<u64>(image_info.size.depth) * static_cast<u64>(image_info.num_bits) / 8);
        LOG_ERROR(Render_Vulkan,
                  "Unresolvable image overlap with equal memory address:\n"
                  "=== OLD IMAGE (cached) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "  Last accessed:  tick {}\n"
                  "  Safe to delete: {}\n"
                  "\n"
                  "=== NEW IMAGE (requested) ===\n"
                  "  Address:        {:#x}\n"
                  "  Size:           {:#x} bytes\n"
                  "  Format:         {}\n"
                  "  Type:           {}\n"
                  "  Width:          {}\n"
                  "  Height:         {}\n"
                  "  Depth:          {}\n"
                  "  Pitch:          {}\n"
                  "  Mip levels:     {}\n"
                  "  Array layers:   {}\n"
                  "  Samples:        {}\n"
                  "  Tile mode:      {:#x}\n"
                  "  Block size:     {} bits\n"
                  "  Is block-comp:  {}\n"
                  "  Guest size:     {:#x}\n"
                  "\n"
                  "=== COMPARISON ===\n"
                  "  Same format:           {}\n"
                  "  Same type:             {}\n"
                  "  Same tile mode:        {}\n"
                  "  Same block size:       {}\n"
                  "  Same BlockDim:         {}\n"
                  "  Same pitch:            {}\n"
                  "  Old resources <= new:  {} (old: {}, new: {})\n"
                  "  Old size <= new size:  {}\n"
                  "  Expected size (calc):  {} bytes\n"
                  "  Size ratio (new/expected): {:.2f}x\n"
                  "  Size ratio (new/old):  {:.2f}x\n"
                  "  Old vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  New vs expected diff:  {} bytes ({:+.2f}%)\n"
                  "  Merged image ID:       {}\n"
                  "  Binding type:          {}\n"
                  "  Current tick:          {}\n"
                  "  Age (ticks since last access): {}",

                  cache_image.info.guest_address, cache_image.info.guest_size,
                  vk::to_string(cache_image.info.pixel_format),
                  static_cast<int>(cache_image.info.type), cache_image.info.size.width,
                  cache_image.info.size.height, cache_image.info.size.depth, cache_image.info.pitch,
                  cache_image.info.resources.levels, cache_image.info.resources.layers,
                  cache_image.info.num_samples, static_cast<u32>(cache_image.info.tile_mode),
                  cache_image.info.num_bits, cache_image.info.props.is_block,
                  cache_image.info.guest_size, cache_image.tick_accessed_last, safe_to_delete,

                  image_info.guest_address, image_info.guest_size,
                  vk::to_string(image_info.pixel_format), static_cast<int>(image_info.type),
                  image_info.size.width, image_info.size.height, image_info.size.depth,
                  image_info.pitch, image_info.resources.levels, image_info.resources.layers,
                  image_info.num_samples, static_cast<u32>(image_info.tile_mode),
                  image_info.num_bits, image_info.props.is_block, image_info.guest_size,

                  (image_info.pixel_format == cache_image.info.pixel_format),
                  (image_info.type == cache_image.info.type),
                  (image_info.tile_mode == cache_image.info.tile_mode),
                  (image_info.num_bits == cache_image.info.num_bits),
                  (image_info.BlockDim() == cache_image.info.BlockDim()),
                  (image_info.pitch == cache_image.info.pitch),
                  (cache_image.info.resources <= image_info.resources),
                  cache_image.info.resources.levels, image_info.resources.levels,
                  (cache_image.info.guest_size <= image_info.guest_size), expected_size,

                  static_cast<double>(image_info.guest_size) / expected_size,
                  static_cast<double>(image_info.guest_size) / cache_image.info.guest_size,

                  static_cast<s64>(cache_image.info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(cache_image.info.guest_size) / expected_size - 1.0) * 100.0,

                  static_cast<s64>(image_info.guest_size) - static_cast<s64>(expected_size),
                  (static_cast<double>(image_info.guest_size) / expected_size - 1.0) * 100.0,

                  merged_image_id.index, static_cast<int>(binding), scheduler.CurrentTick(),
                  scheduler.CurrentTick() - cache_image.tick_accessed_last);

        UNREACHABLE_MSG("Encountered unresolvable image overlap with equal memory address.");
    }

    if (image_info.guest_address > cache_image.info.guest_address) {
        if (auto mip = image_info.MipOf(cache_image.info); mip >= 0) {
            if (auto slice = image_info.SliceOf(cache_image.info, mip); slice >= 0) {
                return {cache_image_id, mip, slice};
            }
        }

        if (safe_to_delete) {
            FreeImage(cache_image_id);
        }

        return {{}, -1, -1};
    } else {
        if (auto mip = cache_image.info.MipOf(image_info); mip >= 0) {
            if (auto slice = cache_image.info.SliceOf(image_info, mip); slice >= 0) {
                if (cache_image.binding.is_target) {
                    cache_image.binding.needs_rebind = 1u;
                    if (merged_image_id) {
                        GetImage(merged_image_id).binding.is_target = 1u;
                    }

                    FreeImage(cache_image_id);
                    return {merged_image_id, -1, -1};
                }

                if (merged_image_id) {
                    auto& merged_image = slot_images[merged_image_id];
                    merged_image.CopyMip(cache_image, mip, slice);
                    FreeImage(cache_image_id);
                }
            }
        }
    }

    return {merged_image_id, -1, -1};
}

ImageId TextureCache::ExpandImage(const ImageInfo& info, BindingType binding,
                                  ImageId image_id,
                                  std::unique_lock<std::mutex>& lock) {
    if (!slot_images.is_allocated(image_id)) {
        return {};
    }
    const auto& initial_src_image = slot_images[image_id];
    if (False(initial_src_image.flags & ImageFlagBits::Registered)) {
        return {};
    }
    const u64 source_uid = initial_src_image.image_uid;
    const VAddr source_address = initial_src_image.info.guest_address;
    const u64 source_size = initial_src_image.info.guest_size;

    const auto new_image_id =
        slot_images.insert(instance, scheduler, blit_helper, slot_image_views,
                           info, BackingRoleFor(binding));
    const u64 new_image_uid = slot_images[new_image_id].image_uid;
    RegisterImage(new_image_id);

    if (!RefreshImageOrdered(new_image_id, lock) ||
        !slot_images.is_allocated(image_id) ||
        !slot_images.is_allocated(new_image_id) ||
        slot_images[image_id].image_uid != source_uid ||
        False(slot_images[image_id].flags & ImageFlagBits::Registered) ||
        slot_images[image_id].info.guest_address != source_address ||
        slot_images[image_id].info.guest_size != source_size ||
        slot_images[new_image_id].image_uid != new_image_uid ||
        False(slot_images[new_image_id].flags & ImageFlagBits::Registered)) {
        if (slot_images.is_allocated(new_image_id) &&
            slot_images[new_image_id].image_uid == new_image_uid &&
            True(slot_images[new_image_id].flags & ImageFlagBits::Registered)) {
            FreeImage(new_image_id);
        }
        return {};
    }
    auto& refreshed_src_image = slot_images[image_id];
    auto& refreshed_new_image = slot_images[new_image_id];
    refreshed_new_image.CopyImage(refreshed_src_image);

    if (refreshed_src_image.binding.is_bound || refreshed_src_image.binding.is_target) {
        refreshed_src_image.binding.needs_rebind = 1u;
    }

    FreeImage(image_id);

    TrackImage(new_image_id);
    refreshed_new_image.flags &= ~ImageFlagBits::Dirty;
    return new_image_id;
}

TextureCache::ExactSurfaceBindBatch::ExactSurfaceBindBatch(TextureCache& cache)
    : owner{&cache}, lock{cache.mutex} {}

TextureCache::ExactSurfaceBindBatch TextureCache::BeginExactSurfaceBindBatch() {
    return ExactSurfaceBindBatch{*this};
}

ImageId TextureCache::FindExactSurfaceHotImageLocked(const ImageInfo& info) {
#ifdef __ANDROID__
    if (!VideoCore::IsManagedMobileGpuOptimization() ||
        !exact_surface_hot_cache_enabled) {
        return {};
    }
    // The cheap prehash is only a set selector.  Identity is still accepted
    // exclusively after slot liveness, UID, registration and every ImageInfo
    // field used by the original exact-cache path have been revalidated.
    const u64 hot_probe_key = exact_surface_prehash_probe_enabled
                                  ? ExactSurfacePrehashProbeKey(info)
                                  : ExactSurfaceIdentity(info);
    auto& hot = exact_surface_hot_entries[
        hot_probe_key & (ExactSurfaceHotEntryCount - 1)];
    if (!hot.valid || hot.identity != hot_probe_key ||
        !slot_images.is_allocated(hot.image_id)) {
        return {};
    }
    auto& hot_image = slot_images[hot.image_id];
    const auto& hot_info = hot_image.info;
    const bool exact_match =
        hot_image.image_uid == hot.image_uid &&
        True(hot_image.flags & ImageFlagBits::Registered) &&
        hot_info.guest_address == info.guest_address &&
        hot_info.guest_size == info.guest_size && hot_info.size == info.size &&
        hot_info.pitch == info.pitch && hot_info.resources == info.resources &&
        hot_info.num_samples == info.num_samples &&
        hot_info.num_bits == info.num_bits &&
        hot_info.pixel_format == info.pixel_format && hot_info.type == info.type &&
        hot_info.tile_mode == info.tile_mode;
    if (!exact_match) {
        return {};
    }
    hot_image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(hot_image);
    VideoCore::RecordMobileGpuSurfaceReuse(hot_image.info.guest_size);
    static std::atomic<bool> first_hot_hit_logged{false};
    if (!first_hot_hit_logged.exchange(true, std::memory_order_relaxed)) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_EXACT_SURFACE_HOT_CACHE] firstHit=1 "
            "identityProof=%u uidProof=1 fullInfoProof=1 fingerprintSkipped=%u",
            exact_surface_prehash_probe_enabled ? 0u : 1u,
            exact_surface_prehash_probe_enabled ? 1u : 0u);
    }
    return hot.image_id;
#else
    (void)info;
    return {};
#endif
}

ImageId TextureCache::FindImageExactHot(ImageDesc& desc,
                                        ExactSurfaceBindBatch& batch) {
    ASSERT(batch.owner == this && batch.lock.owns_lock());
    const ImageId image_id = FindExactSurfaceHotImageLocked(desc.info);
    if (image_id) {
        slot_images[image_id].EnsureBackingRole(BackingRoleFor(desc.type));
    }
#ifdef __ANDROID__
    if (image_id && ++batch.exact_hits == 2 &&
        !exact_surface_bind_batch_proof_logged) {
        exact_surface_bind_batch_proof_logged = true;
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_EXACT_SURFACE_BIND_BATCH] firstBatch=1 exactHits=2 "
            "mutexPairsSaved=1 fullInfoProof=1 fallbackOnMiss=1 "
            "pendingTransferGate=1 drsReplayDiagnosticGate=1");
        if (std::FILE* proof = std::fopen(
                "/data/data/app.lsx4.android/files/runtime/"
                "exact-surface-bind-batch.txt",
                "w")) {
            std::fputs("firstBatch=1 exactHits=2 mutexPairsSaved=1 "
                       "fullInfoProof=1 fallbackOnMiss=1 "
                       "pendingTransferGate=1 drsReplayDiagnosticGate=1\n",
                       proof);
            std::fclose(proof);
        }
    }
#else
    (void)batch;
#endif
    return image_id;
}

ImageId TextureCache::FindImage(ImageDesc& desc, bool exact_fmt) {
    const auto& info = desc.info;

    if (info.guest_address == 0) [[unlikely]] {
        return GetNullImage(info.pixel_format);
    }

    scheduler.FlushPendingBufferTransfersForRange(info.guest_address,
                                                  info.guest_size);
    std::unique_lock lock{mutex};
#ifdef __ANDROID__
    if (VideoCore::IsManagedMobileGpuOptimization()) {
        if (const ImageId hot_image_id = FindExactSurfaceHotImageLocked(info)) {
            slot_images[hot_image_id].EnsureBackingRole(
                BackingRoleFor(desc.type));
            return hot_image_id;
        }
        // D75 is a second, default-off probe layered over D63.  Its table key
        // is intentionally cheap, but it is never accepted as identity: the
        // complete ImageInfo contract below remains the authority.  A miss
        // falls through to the unchanged exact fingerprint/index path.
        const u64 hot_probe_key = exact_surface_prehash_probe_enabled
                                      ? ExactSurfacePrehashProbeKey(info)
                                      : ExactSurfaceIdentity(info);
        const u64 exact_identity = exact_surface_prehash_probe_enabled
                                       ? ExactSurfaceIdentity(info)
                                       : hot_probe_key;
        const auto indexed = exact_surface_index.find(exact_identity);
        if (indexed != exact_surface_index.end()) {
            for (const ImageId indexed_id : indexed->second) {
                if (!slot_images.is_allocated(indexed_id)) {
                    continue;
                }
                auto& indexed_image = slot_images[indexed_id];
                const auto& indexed_info = indexed_image.info;
                if (False(indexed_image.flags & ImageFlagBits::Registered) ||
                    indexed_info.guest_address != info.guest_address ||
                    indexed_info.guest_size != info.guest_size ||
                    indexed_info.size != info.size ||
                    indexed_info.pitch != info.pitch ||
                    indexed_info.resources != info.resources ||
                    indexed_info.num_samples != info.num_samples ||
                    indexed_info.num_bits != info.num_bits ||
                    indexed_info.pixel_format != info.pixel_format ||
                    indexed_info.type != info.type ||
                    indexed_info.tile_mode != info.tile_mode) {
                    continue;
                }
                indexed_image.tick_accessed_last = scheduler.CurrentTick();
                TouchImage(indexed_image);
                VideoCore::RecordMobileGpuSurfaceReuse(
                    indexed_image.info.guest_size);
                if (exact_surface_hot_cache_enabled) {
                    auto& hot = exact_surface_hot_entries[
                        hot_probe_key & (ExactSurfaceHotEntryCount - 1)];
                    hot.identity = hot_probe_key;
                    hot.image_id = indexed_id;
                    hot.image_uid = indexed_image.image_uid;
                    hot.valid = true;
                }
                indexed_image.EnsureBackingRole(BackingRoleFor(desc.type));
                return indexed_id;
            }
        }
    }
#endif
    struct ImageCandidate {
        ImageId id{};
        u64 uid{};
    };
    boost::container::small_vector<ImageCandidate, 16> image_candidates;
    ForEachImageInRegion(info.guest_address, info.guest_size,
                         [&](ImageId candidate_id, Image& candidate_image) {
                             image_candidates.push_back(
                                 {candidate_id, candidate_image.image_uid});
                         });

    ImageId image_id{};
    u64 image_uid{};
    bool exact_surface_reuse = false;

    for (const auto& candidate : image_candidates) {
        const ImageId cache_id = candidate.id;
        if (!slot_images.is_allocated(cache_id) ||
            slot_images[cache_id].image_uid != candidate.uid ||
            False(slot_images[cache_id].flags & ImageFlagBits::Registered)) {
            continue;
        }
        auto& cache_image = slot_images[cache_id];
        if (cache_image.info.guest_address != info.guest_address) {
            continue;
        }
        if (cache_image.info.guest_size != info.guest_size) {
            continue;
        }
        if (cache_image.info.size != info.size) {
            continue;
        }
        if (!IsVulkanFormatCompatible(cache_image.info.pixel_format, info.pixel_format) ||
            (cache_image.info.type != info.type && info.size != Extent3D{1, 1, 1})) {
            continue;
        }
        if (exact_fmt && info.pixel_format != cache_image.info.pixel_format) {
            continue;
        }
        image_id = cache_id;
        image_uid = candidate.uid;
        exact_surface_reuse = true;
    }

    int view_mip{-1};
    int view_slice{-1};
    if (!image_id) {
        for (const auto& candidate : image_candidates) {
            const ImageId cache_id = candidate.id;
            view_mip = -1;
            view_slice = -1;

            if (!slot_images.is_allocated(cache_id) ||
                slot_images[cache_id].image_uid != candidate.uid ||
                False(slot_images[cache_id].flags & ImageFlagBits::Registered)) {
                continue;
            }
            if (image_id &&
                (!slot_images.is_allocated(image_id) ||
                 slot_images[image_id].image_uid != image_uid ||
                 False(slot_images[image_id].flags & ImageFlagBits::Registered))) {
                image_id = {};
                image_uid = 0;
            }

            const ImageInfo merged_info = image_id ? slot_images[image_id].info : info;
            auto [overlap_image_id, overlap_view_mip, overlap_view_slice] =
                ResolveOverlap(merged_info, desc.type, cache_id, image_id,
                               lock);
            if (overlap_image_id && slot_images.is_allocated(overlap_image_id)) {
                image_id = overlap_image_id;
                image_uid = slot_images[overlap_image_id].image_uid;
                view_mip = overlap_view_mip;
                view_slice = overlap_view_slice;
            }
        }
    }

    if (image_id) {
        if (!slot_images.is_allocated(image_id) ||
            slot_images[image_id].image_uid != image_uid ||
            False(slot_images[image_id].flags & ImageFlagBits::Registered)) {
            image_id = {};
            exact_surface_reuse = false;
        }
    }
    if (image_id) {
        Image& image_resolved = slot_images[image_id];
        if (exact_fmt && info.pixel_format != image_resolved.info.pixel_format) {
            image_id = {};
        } else if (image_resolved.info.resources < info.resources) {
            FreeImage(image_id);
            image_id = {};
            LOG_WARNING(Render_Vulkan, "Image overlap resolve failed");
        }
    }
    if (!image_id) {
        image_id = slot_images.insert(instance, scheduler, blit_helper,
                                      slot_image_views, info,
                                      BackingRoleFor(desc.type));
        RegisterImage(image_id);
    }

    Image& image = slot_images[image_id];
    image.EnsureBackingRole(BackingRoleFor(desc.type));
    if (exact_surface_reuse) {
        VideoCore::RecordMobileGpuSurfaceReuse(image.info.guest_size);
    }
    image.tick_accessed_last = scheduler.CurrentTick();
    TouchImage(image);
    if (view_mip > 0) {
        desc.view_info.range.base.level = view_mip;
    }
    if (view_slice > 0) {
        desc.view_info.range.base.layer = view_slice;
    }

    return image_id;
}

ImageId TextureCache::FindImageFromRange(VAddr address, size_t size, bool ensure_valid) {
    std::scoped_lock lock{mutex};
    ImageIds image_ids;
    ForEachImageInRegion(address, size, [&](ImageId image_id, Image& image) {
        if (False(image.flags & ImageFlagBits::Registered)) {
            return;
        }
        if (image.info.guest_address != address) {
            return;
        }
        if (ensure_valid && !image.SafeToDownload()) {
            return;
        }
        image_ids.push_back(image_id);
    });
    if (image_ids.size() == 1) {
        return image_ids.back();
    }
    if (!image_ids.empty()) {
        for (s32 i = 0; i < image_ids.size(); ++i) {
            Image& image = slot_images[image_ids[i]];
            if (image.info.guest_size == size) {
                return image_ids[i];
            }
        }
        LOG_WARNING(Render_Vulkan,
                    "Failed to find exact image match for copy addr={:#x}, size={:#x}", address,
                    size);
    }
    return {};
}

ImageView* TextureCache::FindTexture(ImageId image_id, const ImageDesc& desc) {
    std::unique_lock lock{mutex};
    if (!slot_images.is_allocated(image_id)) {
        return nullptr;
    }
    Image& initial_image = slot_images[image_id];
    const u64 image_uid = initial_image.image_uid;
    const bool registration_required = initial_image.info.guest_address != 0;
    if (registration_required &&
        False(initial_image.flags & ImageFlagBits::Registered)) {
        return nullptr;
    }
    if (desc.type == BindingType::Storage) {
        initial_image.flags |= ImageFlagBits::GpuModified;
        if (Config::readbackLinearImages() && !initial_image.info.props.is_tiled &&
            initial_image.info.guest_address != 0) {
            download_images.emplace(image_id);
        }
    }
    TrackImage(image_id);
    TouchImage(initial_image);
    if (!RefreshImageOrdered(image_id, lock) ||
        !slot_images.is_allocated(image_id) ||
        slot_images[image_id].image_uid != image_uid ||
        (registration_required &&
         False(slot_images[image_id].flags & ImageFlagBits::Registered))) {
        return nullptr;
    }
    TrackImage(image_id);
    return &slot_images[image_id].FindView(desc.view_info);
}

ImageView* TextureCache::FindRenderTarget(ImageId image_id, const ImageDesc& desc) {
    std::unique_lock lock{mutex};
    if (!slot_images.is_allocated(image_id)) {
        return nullptr;
    }
    Image& initial_image = slot_images[image_id];
    const u64 image_uid = initial_image.image_uid;
    if (False(initial_image.flags & ImageFlagBits::Registered)) {
        return nullptr;
    }
    initial_image.flags |= ImageFlagBits::GpuModified;
    if (Config::readbackLinearImages() && !initial_image.info.props.is_tiled) {
        download_images.emplace(image_id);
    }
    initial_image.usage.render_target = 1u;
    TrackImage(image_id);
    TouchImage(initial_image);
    if (!RefreshImageOrdered(image_id, lock) ||
        !slot_images.is_allocated(image_id) ||
        slot_images[image_id].image_uid != image_uid ||
        False(slot_images[image_id].flags & ImageFlagBits::Registered)) {
        return nullptr;
    }
    TrackImage(image_id);
    Image& image = slot_images[image_id];

    if (desc.info.meta_info.cmask_addr) {
        TrackImageMetaResidency(image, desc.info.meta_info.cmask_addr);
        surface_metas.emplace(desc.info.meta_info.cmask_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::CMask});
        image.info.meta_info.cmask_addr = desc.info.meta_info.cmask_addr;
    }

    if (desc.info.meta_info.fmask_addr) {
        TrackImageMetaResidency(image, desc.info.meta_info.fmask_addr);
        surface_metas.emplace(desc.info.meta_info.fmask_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::FMask});
        image.info.meta_info.fmask_addr = desc.info.meta_info.fmask_addr;
    }

    return &image.FindView(desc.view_info, false);
}

ImageView* TextureCache::FindDepthTarget(ImageId image_id, const ImageDesc& desc) {
#ifdef __ANDROID__
#define FDT_MARK(s) do { if (Libraries::GnmDriver::ExecutorReplayActive()) \
    __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_FDT] " s); } while(0)
#else
#define FDT_MARK(s) ((void)0)
#endif
    std::unique_lock lock{mutex};
    if (!slot_images.is_allocated(image_id)) {
        return nullptr;
    }
    Image& initial_image = slot_images[image_id];
    const u64 image_uid = initial_image.image_uid;
    if (False(initial_image.flags & ImageFlagBits::Registered)) {
        return nullptr;
    }
    initial_image.flags |= ImageFlagBits::GpuModified;
    initial_image.usage.depth_target = 1u;
#ifdef __ANDROID__
    if (Libraries::GnmDriver::ExecutorReplayActive())
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                            "[EXECUTOR_FDT] before_updateimage depthAddr=0x%llx size=%u reloc=%d "
                            "stencilAddr=0x%llx htile=0x%llx",
                            (unsigned long long)initial_image.info.guest_address,
                            initial_image.info.guest_size,
                            Libraries::GnmDriver::ExecutorReplayIsRelocated(initial_image.info.guest_address) ? 1
                                                                                                      : 0,
                            (unsigned long long)desc.info.stencil_addr,
                            (unsigned long long)desc.info.meta_info.htile_addr);
#endif
    if (!Libraries::GnmDriver::ExecutorReplayActive()) {
        TrackImage(image_id);
        TouchImage(initial_image);
        if (!RefreshImageOrdered(image_id, lock) ||
            !slot_images.is_allocated(image_id) ||
            slot_images[image_id].image_uid != image_uid ||
            False(slot_images[image_id].flags & ImageFlagBits::Registered)) {
            return nullptr;
        }
        TrackImage(image_id);
    }
    FDT_MARK("after_updateimage");
    Image& image = slot_images[image_id];

    if (desc.info.meta_info.htile_addr) {
        TrackImageMetaResidency(image, desc.info.meta_info.htile_addr);
        surface_metas.emplace(desc.info.meta_info.htile_addr,
                              MetaDataInfo{.type = MetaDataInfo::Type::HTile,
                                           .clear_mask = image.info.meta_info.htile_clear_mask});
        image.info.meta_info.htile_addr = desc.info.meta_info.htile_addr;
    }

    FDT_MARK("before_stencil");
    if (desc.info.stencil_addr != 0 && !Libraries::GnmDriver::ExecutorReplayActive()) {
        FDT_MARK("stencil_path");
        ImageId stencil_id{};
        ForEachImageInRegion(desc.info.stencil_addr, desc.info.stencil_size,
                             [&](ImageId image_id, Image& image) {
                                 if (image.info.guest_address == desc.info.stencil_addr) {
                                     stencil_id = image_id;
                                 }
                             });
        if (!stencil_id) {
            ImageInfo info{};
            info.guest_address = desc.info.stencil_addr;
            info.guest_size = desc.info.stencil_size;
            info.size = desc.info.size;
            stencil_id =
                slot_images.insert(instance, scheduler, blit_helper,
                                   slot_image_views, info,
                                   ImageBackingRole::DepthTarget);
            RegisterImage(stencil_id);
        }
        Image& stencil_image = slot_images[stencil_id];
        TouchImage(stencil_image);
        stencil_image.AssociateDepth(image_id, image.image_uid);
    }

    FDT_MARK("before_findview");
    auto& v = image.FindView(desc.view_info, false);
    FDT_MARK("after_findview");
    return &v;
}

bool TextureCache::UpdateImage(const ImageId image_id) {
    std::unique_lock lock{mutex};
    if (!slot_images.is_allocated(image_id)) {
        return false;
    }
    Image& image = slot_images[image_id];
    const u64 image_uid = image.image_uid;
    const bool registration_required = image.info.guest_address != 0;
    if (registration_required &&
        False(image.flags & ImageFlagBits::Registered)) {
        return false;
    }
    TrackImage(image_id);
    TouchImage(image);
    if (!RefreshImageOrdered(image_id, lock) ||
        !slot_images.is_allocated(image_id) ||
        slot_images[image_id].image_uid != image_uid ||
        (registration_required &&
         False(slot_images[image_id].flags & ImageFlagBits::Registered))) {
        return false;
    }
    TrackImage(image_id);
    return true;
}

bool TextureCache::RefreshImageOrdered(
    const ImageId image_id, std::unique_lock<std::mutex>& lock) {
    ASSERT(lock.owns_lock());
    if (!slot_images.is_allocated(image_id)) {
        return false;
    }
    Image& initial_image = slot_images[image_id];
    if (False(initial_image.flags & ImageFlagBits::Dirty) ||
        initial_image.info.num_samples > 1) {
        return true;
    }
    const u64 image_uid = initial_image.image_uid;
    const VAddr guest_address = initial_image.info.guest_address;
    const u64 guest_size = initial_image.info.guest_size;
    const u32 initial_flags = static_cast<u32>(initial_image.flags);
    const GuestBackingHashPlan initial_hash_plan =
        BuildGuestBackingHashPlan(initial_image);
    const u64 hash_read_size = initial_hash_plan.read_size;
    const bool registration_required = guest_address != 0;
    if (registration_required &&
        False(initial_image.flags & ImageFlagBits::Registered)) {
        return false;
    }

    lock.unlock();
#ifdef __ANDROID__
    TraceGuestBackingHashPlan(guest_address, guest_size, initial_flags,
                              initial_hash_plan);
    // RefreshImage hashes the guest backing while both the texture-cache lock
    // and a shared MemoryManager snapshot guard are held.  A demand read fault
    // from that hash would run the GpuComm readback inline; its retirement must
    // take MemoryManager's exclusive lock to publish the downloaded bytes.
    // Resolve only the range RefreshImage will actually hash. GpuDirty uses
    // the newer Vulkan buffer directly and does no CPU mip hash, so forcing a
    // full GPU->CPU publication there only creates a readback/re-protect loop.
    if (hash_read_size != 0 &&
        !PageManager::PrepareTextureGuestReadProtectedRuns(guest_address,
                                                           hash_read_size)) {
        lock.lock();
        return false;
    }
#endif
    auto* const guest_memory = Core::Memory::Instance();
    std::optional<Core::MemoryManager::SnapshotReadGuard> snapshot_guard;
    if (guest_memory != nullptr) {
        snapshot_guard.emplace(guest_memory->AcquireSnapshotReadGuard());
    }
#ifdef __ANDROID__
    // Close the small interval between the locked PageManager postcheck and
    // snapshot-guard acquisition.  If a non-Gpu presenter raced a new pending
    // readback into the range, drop the guard and retry on a later refresh;
    // never enter the hash with a read-protected page.
    if (hash_read_size != 0 &&
        !PageManager::IsFexGuestMemoryReadReady(guest_address,
                                                hash_read_size)) {
        snapshot_guard.reset();
        lock.lock();
        return false;
    }
#endif
    const bool snapshot_boundary =
        snapshot_guard.has_value() &&
        PageManager::BeginFexBufferSnapshotBoundary(guest_address,
                                                    guest_size);
    lock.lock();
    SCOPE_EXIT {
        PageManager::EndFexBufferSnapshotBoundary(snapshot_boundary);
    };

    if (!slot_images.is_allocated(image_id)) {
        return false;
    }
    Image& image = slot_images[image_id];
    if (image.image_uid != image_uid ||
        (registration_required &&
         False(image.flags & ImageFlagBits::Registered)) ||
        image.info.guest_address != guest_address ||
        image.info.guest_size != guest_size) {
        return false;
    }
    // The image was deliberately unlocked around the preflight. If another
    // thread introduced a stronger hash requirement meanwhile, retry from a
    // fresh plan rather than reading an unprepared part of guest backing.
    if (BuildGuestBackingHashPlan(image).read_size > hash_read_size) {
        return false;
    }
    return RefreshImage(image, snapshot_guard ? &*snapshot_guard : nullptr);
}

bool TextureCache::RefreshImage(
    Image& image,
    Core::MemoryManager::SnapshotReadGuard* external_snapshot_guard) {
    scheduler.FlushPendingBufferTransfersForRange(image.info.guest_address,
                                                  image.info.guest_size);
#ifdef __ANDROID__
    if (Libraries::GnmDriver::ExecutorReplayActive()) {
        const u32* src = reinterpret_cast<const u32*>(image.info.guest_address);
        u32 s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        if (Libraries::GnmDriver::ExecutorReplayIsRelocated(image.info.guest_address)) {
            s0 = src[0]; s1 = src[1]; s2 = src[2]; s3 = src[3];
        }
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                            "[EXECUTOR_RI] enter dirty=%d depth=%d addr=0x%llx size=%u src=%08x %08x %08x %08x",
                            (int)True(image.flags & ImageFlagBits::Dirty), image.info.props.is_depth ? 1 : 0,
                            (unsigned long long)image.info.guest_address, image.info.guest_size, s0, s1, s2,
                            s3);
    }
#endif
    if (False(image.flags & ImageFlagBits::Dirty) || image.info.num_samples > 1) {
        return true;
    }
#ifdef __ANDROID__
    if (Libraries::GnmDriver::ExecutorReplayActive() &&
        (image.usage.render_target || image.usage.depth_target ||
         True(image.flags & ImageFlagBits::GpuModified))) {
        image.flags &= ~ImageFlagBits::Dirty;
        return true;
    }
#endif
#ifdef __ANDROID__
    if (Libraries::GnmDriver::ExecutorReplayActive()) {
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                            "[EXECUTOR_REFRESH_IMG] guest_addr=0x%llx guest_size=%llu tiled=%d "
                            "w=%u h=%u mips=%u layers=%u relocHit=%d",
                            (unsigned long long)image.info.guest_address,
                            (unsigned long long)image.info.guest_size, image.info.props.is_tiled ? 1 : 0,
                            image.info.size.width, image.info.size.height, image.info.resources.levels,
                            image.info.resources.layers,
                            Libraries::GnmDriver::ExecutorReplayIsRelocated(image.info.guest_address) ? 1
                                                                                                     : 0);
        Libraries::GnmDriver::ExecutorReplayCheckAddr(4u, image.info.guest_address,
                                                      image.info.guest_size);
    }
#endif

    RENDERER_TRACE;
    TRACE_HINT(fmt::format("{:x}:{:x}", image.info.guest_address, image.info.guest_size));

    const GuestBackingHashPlan hash_plan = BuildGuestBackingHashPlan(image);
    if (hash_plan.sample_hash) {
        const auto addr = std::bit_cast<u8*>(image.info.guest_address);
        const u64 hash = XXH3_64bits(addr, hash_plan.sample_size);
        if (image.hash == hash) {
            image.flags &= ~ImageFlagBits::MaybeCpuDirty;
            return true;
        }
        image.hash = hash;
    }

    const u32 num_layers = image.info.resources.layers;
    const u32 num_mips = image.info.resources.levels;

    boost::container::small_vector<vk::BufferImageCopy, 14> image_copies;
    for (u32 m = 0; m < num_mips; m++) {
        const u32 width = std::max(image.info.size.width >> m, 1u);
        const u32 height = std::max(image.info.size.height >> m, 1u);
        const u32 depth =
            image.info.props.is_volume ? std::max(image.info.size.depth >> m, 1u) : 1u;
        const auto [mip_size, mip_pitch, mip_height, mip_offset] = image.info.mips_layout[m];

        if (hash_plan.mip_hash) {
            const u8* addr = std::bit_cast<u8*>(image.info.guest_address);
            const u64 hash = XXH3_64bits(addr + mip_offset, mip_size);
            if (image.mip_hashes[m] == hash) {
                continue;
            }
            image.mip_hashes[m] = hash;
        }

        const u32 extent_width = mip_pitch ? std::min(mip_pitch, width) : width;
        const u32 extent_height = mip_height ? std::min(mip_height, height) : height;
        image_copies.push_back({
            .bufferOffset = mip_offset,
            .bufferRowLength = mip_pitch,
            .bufferImageHeight = mip_height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = m,
                .baseArrayLayer = 0,
                .layerCount = num_layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent_width, extent_height, depth},
        });
    }

    if (image_copies.empty()) {
        image.flags &= ~ImageFlagBits::Dirty;
        return true;
    }

    scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::TextureCacheRefresh);

    if (Libraries::GnmDriver::ExecutorGnmCaptureActive()) {
        Libraries::GnmDriver::ExecutorGnmCaptureRecordResource(4u, image.info.guest_address,
                                                               image.info.guest_size);
    }

#ifdef __ANDROID__
    const bool exec_ri_trace = Libraries::GnmDriver::ExecutorReplayActive();
    if (exec_ri_trace)
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_RI] before_obtain copies=%zu",
                            image_copies.size());
#endif
    const auto [in_buffer, in_offset] =
        buffer_cache.ObtainBufferForImage(image.info.guest_address,
                                          image.info.guest_size,
                                          external_snapshot_guard);
    if (in_buffer == nullptr) {
        // Snapshot acquisition failed closed. Keep the image Dirty so a later
        // refresh can retry; never feed the 16-byte null buffer to detiling.
        return false;
    }
#ifdef __ANDROID__
    if (exec_ri_trace)
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_RI] after_obtain in_offset=%u",
                            (unsigned)in_offset);
#endif
    const auto source_access = image.info.props.is_tiled
                                   ? vk::AccessFlagBits2::eShaderRead
                                   : vk::AccessFlagBits2::eTransferRead;
    const auto source_stage = image.info.props.is_tiled
                                  ? vk::PipelineStageFlagBits2::eComputeShader
                                  : vk::PipelineStageFlagBits2::eTransfer;
    if (auto barrier = in_buffer->GetBarrier(source_access, source_stage)) {
        scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = 1,
            .pBufferMemoryBarriers = &barrier.value(),
        });
    }

    auto detile =
        tile_manager.DetileImage(in_buffer->Handle(), in_offset, image.info);
#ifdef __ANDROID__
    if (exec_ri_trace)
        __android_log_print(ANDROID_LOG_INFO, "EXECUTOR", "[EXECUTOR_RI] after_detile");
#endif
    for (auto& copy : image_copies) {
        copy.bufferOffset += detile.offset;
    }

    image.Upload(image_copies, detile.buffer, detile.offset);
    tile_manager.RetireDetileResult(std::move(detile));

#ifdef __ANDROID__
    if (Libraries::GnmDriver::ExecutorReplayActive() && !image.info.props.is_depth &&
        image.info.size.width >= 64 && image.info.guest_size <= (64ull << 20)) {
        auto& dl = buffer_cache.GetUtilityBuffer(MemoryUsage::Download);
        const u32 bpp = std::max<u32>(image.info.num_bits / 8u, 1u);
        const u64 dsize = static_cast<u64>(image.info.pitch) * image.info.size.height * bpp;
        if (dsize > 0 && dsize <= (64ull << 20)) {
            const auto [dptr, doff] = dl.Map(dsize);
            dl.Commit();
            const vk::BufferImageCopy region{
                .bufferOffset = doff,
                .bufferRowLength = image.info.pitch,
                .bufferImageHeight = image.info.size.height,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {image.info.size.width, image.info.size.height, 1}};
            scheduler.EndRendering(Vulkan::Scheduler::EndRenderingReason::ImageDownload);
            const auto cb = scheduler.CommandBuffer();
            image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
            cb.copyImageToBuffer(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, dl.Handle(),
                                 region);
            scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::TextureProbe);
            const u8* px = reinterpret_cast<const u8*>(dptr);
            u64 nz = 0;
            const u64 scan = std::min<u64>(dsize, 4ull << 20);
            for (u64 i = 0; i < scan; ++i)
                if (px[i]) ++nz;
            __android_log_print(ANDROID_LOG_INFO, "EXECUTOR",
                                "[EXECUTOR_TEX_READBACK] addr=0x%llx %ux%u bits=%u nonzeroBytes=%llu "
                                "first=%08x %08x",
                                (unsigned long long)image.info.guest_address, image.info.size.width,
                                image.info.size.height, image.info.num_bits, (unsigned long long)nz,
                                ((const u32*)px)[0], ((const u32*)px)[1]);
        }
    }
#endif
    return true;
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base) {
    return GetSampler(sampler, border_color_base,
                      sampler.depth_compare_func != AmdGpu::DepthCompare::Never);
}

vk::Sampler TextureCache::GetSampler(const AmdGpu::Sampler& sampler,
                                     AmdGpu::BorderColorBuffer border_color_base,
                                     const bool is_depth) {
    // Plain and comparison reads of the same S# must own distinct VkSamplers.
    const u64 hash = HashCombine(XXH3_64bits(&sampler, sizeof(sampler)), u64(is_depth));

    std::scoped_lock lock{samplers_mutex};
    const auto [it, new_sampler] = samplers.try_emplace(hash, instance, sampler, border_color_base, is_depth);
    if (new_sampler) {
        samplers.at(hash).lru_id = sampler_lru_cache.Insert(hash, gc_tick);
    } else {
        sampler_lru_cache.Touch(it->second.lru_id, gc_tick);
    }
    return it->second.Handle();
}

void TextureCache::TrackImageMetaResidency(Image& image,
                                           const VAddr address) {
    if (!tracker.HleCleanBufferFastPathEnabled() || address == 0 ||
        std::ranges::any_of(
                            image.hle_meta_residencies,
                            [address](const auto& range) {
                                return range.source_address == address;
                            })) {
        return;
    }
    // CMask/FMask/HTile Addrlib layouts may align/swizzle before the programmed
    // base and their exact GFX7 extent is not represented by ImageInfo. Cover
    // one 256-KiB alignment unit before the base and at least one after it,
    // plus the full surface footprint, rather than trusting the incomplete
    // cmask/fmask tile_max model.
    constexpr u64 GuestAddressLimit = u64{1} << 40;
    if (address >= GuestAddressLimit) {
        tracker.DisableHleCleanBufferFastPathFailClosed();
        return;
    }
    constexpr u64 MetaAlignment = 256_KB;
    const VAddr residency_begin = Common::AlignDown(address, MetaAlignment);
    const u64 forward_extent =
        std::max<u64>(image.info.guest_size, MetaAlignment);
    const VAddr residency_end =
        address > GuestAddressLimit -
                      std::min(forward_extent, GuestAddressLimit)
            ? GuestAddressLimit
            : std::min(GuestAddressLimit, address + forward_extent);
    const u64 residency_size = residency_end - residency_begin;
    // Reserve vector storage before incrementing the PageManager count. A
    // small_vector growth failure therefore cannot leak an unowned lease.
    image.hle_meta_residencies.push_back(
        {.source_address = address,
         .address = residency_begin,
         .size = residency_size});
    if (!tracker.TrackTextureResidency(residency_begin, residency_size)) {
        image.hle_meta_residencies.pop_back();
    }
}

void TextureCache::UntrackImageMetaResidencies(Image& image) {
    for (const auto& range : image.hle_meta_residencies) {
        tracker.UntrackTextureResidency(range.address, range.size);
    }
    image.hle_meta_residencies.clear();
}

void TextureCache::RegisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    bool residency_committed = false;
    SCOPE_EXIT {
        if (!residency_committed) {
            if (image.hle_main_residency_tracked) {
                tracker.UntrackTextureResidency(image.info.guest_address,
                                                image.info.guest_size);
                image.hle_main_residency_tracked = false;
            }
            UntrackImageMetaResidencies(image);
        }
    };
    // Publish residency before either the Registered flag or page-table
    // entry becomes visible.  A clean-buffer HLE writer which already owns a
    // bulk lease makes this call wait; otherwise its admission sees the
    // resident sidecar and fails closed without taking TextureCache::mutex.
    image.hle_main_residency_tracked =
        tracker.TrackTextureResidency(image.info.guest_address,
                                      image.info.guest_size);
    TrackImageMetaResidency(image, image.info.meta_info.cmask_addr);
    TrackImageMetaResidency(image, image.info.meta_info.fmask_addr);
    TrackImageMetaResidency(image, image.info.meta_info.htile_addr);
    image.flags |= ImageFlagBits::Registered;
    total_used_memory += Common::AlignUp(image.info.guest_size, 1024);
    image.lru_id = lru_cache.Insert(image_id, gc_tick);
    exact_surface_index[ExactSurfaceIdentity(image.info)].push_back(image_id);
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id](u64 page) { page_table[page].push_back(image_id); });
    residency_committed = true;
}

void TextureCache::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already unregistered image");
    image.flags &= ~ImageFlagBits::Registered;
    const auto exact_key = ExactSurfaceIdentity(image.info);
    auto exact_it = exact_surface_index.find(exact_key);
    if (exact_it != exact_surface_index.end()) {
        auto& exact_ids = exact_surface_index[exact_key];
        auto id_it = std::ranges::find(exact_ids, image_id);
        if (id_it != exact_ids.end()) {
            exact_ids.erase(id_it);
        }
        if (exact_ids.empty()) {
            exact_surface_index.erase(exact_key);
        }
    }
    lru_cache.Free(image.lru_id);
    total_used_memory -= Common::AlignUp(image.info.guest_size, 1024);
    bool all_page_references_removed = true;
    ForEachPage(image.info.guest_address, image.info.guest_size,
                [this, image_id,
                 &all_page_references_removed](u64 page) {
        const auto page_it = page_table.find(page);
        if (page_it == nullptr) {
            all_page_references_removed = false;
            UNREACHABLE_MSG("Unregistering unregistered page=0x{:x}", page << PageShift);
            return;
        }
        auto& image_ids = *page_it;
        const auto vector_it = std::ranges::find(image_ids, image_id);
        if (vector_it == image_ids.end()) {
            all_page_references_removed = false;
            ASSERT_MSG(false, "Unregistering unregistered image in page=0x{:x}", page << PageShift);
            return;
        }
        image_ids.erase(vector_it);
        if (std::ranges::find(image_ids, image_id) != image_ids.end()) {
            all_page_references_removed = false;
        }
    });
    // A missing/duplicate entry means the page table no longer proves that
    // every reference was removed.  Leak the marker-only count deliberately:
    // releasing it would turn an existing bookkeeping inconsistency into a
    // fail-open guest write.
    if (all_page_references_removed && image.hle_main_residency_tracked) {
        tracker.UntrackTextureResidency(image.info.guest_address,
                                        image.info.guest_size);
        image.hle_main_residency_tracked = false;
    }
}

void TextureCache::TrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_begin == image.track_addr && image_end == image.track_addr_end) {
        return;
    }

    if (!image.IsTracked()) {
        image.track_addr = image_begin;
        image.track_addr_end = image_end;
        tracker.UpdateTexturePageWatchers<1>(image_begin, image.info.guest_size);
    } else {
        if (image_begin < image.track_addr) {
            TrackImageHead(image_id);
        }
        if (image.track_addr_end < image_end) {
            TrackImageTail(image_id);
        }
    }
}

void TextureCache::TrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_begin = image.info.guest_address;
    if (image_begin == image.track_addr) {
        return;
    }
    ASSERT(image.track_addr != 0 && image_begin < image.track_addr);
    const auto size = image.track_addr - image_begin;
    image.track_addr = image_begin;
    tracker.UpdateTexturePageWatchers<1>(image_begin, size);
}

void TextureCache::TrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!(image.flags & ImageFlagBits::Registered)) {
        return;
    }
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (image_end == image.track_addr_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0 && image.track_addr_end < image_end);
    const auto addr = image.track_addr_end;
    const auto size = image_end - image.track_addr_end;
    image.track_addr_end = image_end;
    tracker.UpdateTexturePageWatchers<1>(addr, size);
}

void TextureCache::UntrackImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    if (!image.IsTracked()) {
        return;
    }
    const auto addr = image.track_addr;
    const auto size = image.track_addr_end - image.track_addr;
    image.track_addr = 0;
    image.track_addr_end = 0;
    if (size != 0) {
        tracker.UpdateTexturePageWatchers<false>(addr, size);
    }
}

void TextureCache::UntrackImageHead(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_begin = image.info.guest_address;
    if (!image.IsTracked() || image_begin < image.track_addr) {
        return;
    }
    const auto addr = tracker.GetNextPageAddr(image_begin);
    const auto size = addr - image_begin;
    image.track_addr = addr;
    if (image.track_addr == image.track_addr_end) {
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdateTexturePageWatchers<false>(image_begin, size);
}

void TextureCache::UntrackImageTail(ImageId image_id) {
    auto& image = slot_images[image_id];
    const auto image_end = image.info.guest_address + image.info.guest_size;
    if (!image.IsTracked() || image.track_addr_end < image_end) {
        return;
    }
    ASSERT(image.track_addr_end != 0);
    const auto addr = tracker.GetPageAddr(image_end);
    const auto size = image_end - addr;
    image.track_addr_end = addr;
    if (image.track_addr == image.track_addr_end) {
        MarkAsMaybeDirty(image_id, image);
    }
    tracker.UpdateTexturePageWatchers<false>(addr, size);
}

void TextureCache::GarbageCollectImages(
    const std::optional<size_t> sampled_device_memory,
    const bool force_pressure_scan) {
    std::scoped_lock lock{mutex};
    // The device sample includes this cache, BufferCache, staging rings and
    // driver-private allocations. Treating it as TextureCache residency made
    // every cache independently enter pressure policy at the same threshold.
    // total_used_memory is maintained incrementally by RegisterImage and
    // UnregisterImage, so ordinary GC stays cache-owned and O(1) per flip.
    // Retain the device sample solely as a true near-budget emergency signal.
    const u64 device_budget = instance.GetMemoryPressureUsageLimit();
    const auto& aggregate_pressure_policy =
        instance.GetCapabilityProfile().aggregate_memory_pressure;
    const bool exact_a750_aggregate_pressure =
        aggregate_pressure_policy.cache_pressure_active;
    const u64 emergency_headroom = exact_a750_aggregate_pressure
                                       ? device_budget / 5
                                       : std::min<u64>(512_MB, device_budget / 8);
    const bool device_emergency =
        sampled_device_memory && device_budget != 0 &&
        *sampled_device_memory >= device_budget - emergency_headroom;
#ifdef __ANDROID__
    // On the exact Turnip D9/A750 build, VkImage/KGSL object overhead is the
    // limiting resource before texture bytes are. Bloodborne's gameplay
    // transition can publish roughly one thousand small images while their
    // guest payload remains below the ordinary 512 MiB byte trigger. Keep a
    // separate marker-gated object watermark so stale LRU images are retired
    // before Android LMKD observes several GiB of driver-private graphics PSS.
    // The conservative age and bounded batches are intentionally gentler than
    // the existing byte-emergency path, which remains unchanged.
    const bool exact_a750_image_object_gc =
        ExecutorUsesExactA750TurnipImageObjectGc(instance);
    constexpr size_t ImageObjectTrigger = 512;
    constexpr size_t ImageObjectPressure = 768;
    constexpr size_t ImageObjectCritical = 1024;
    const bool image_object_trigger =
        exact_a750_image_object_gc && slot_images.size() >= ImageObjectTrigger;
    const bool image_object_pressure =
        exact_a750_image_object_gc && slot_images.size() >= ImageObjectPressure;
    const bool image_object_critical =
        exact_a750_image_object_gc && slot_images.size() >= ImageObjectCritical;
    const bool protect_all_image_identities =
        exact_a750_image_object_gc && image_object_trigger;
    const bool trace_exact_a750_residency =
        instance.GetCapabilityProfile().diagnostic_a750_residency_telemetry;
    const bool trace_residency_sample =
        trace_exact_a750_residency &&
        (gc_tick < 16 || (gc_tick & 31u) == 0u);
    u64 trace_registered_backing_bytes = 0;
    u64 trace_retiring_backing_bytes = 0;
    size_t trace_backing_count = 0;
    const auto sample_trace_backings = [&] {
        trace_registered_backing_bytes = 0;
        trace_retiring_backing_bytes = 0;
        trace_backing_count = 0;
        for (const Image& resident_image : slot_images) {
            const u64 backing_bytes = resident_image.AllocatedBackingBytes();
            trace_backing_count += resident_image.backing_images.size();
            if (True(resident_image.flags & ImageFlagBits::Registered)) {
                trace_registered_backing_bytes += backing_bytes;
            } else {
                trace_retiring_backing_bytes += backing_bytes;
            }
        }
    };
    if (exact_a750_image_object_gc && image_object_critical) {
        // One-shot census for the exact D9/A750 route.  Deleting any of these
        // logical cache entries has proven unsafe for Bloodborne: the scene is
        // initially correct and loses geometry when deferred VkImage owners
        // retire.  Count the actual population instead so pressure can be
        // removed at image creation without invalidating guest identities.
        static bool image_object_census_logged = false;
        if (!image_object_census_logged) {
            image_object_census_logged = true;
            std::unordered_map<VAddr, u32> address_counts;
            std::unordered_map<u32, u32> format_counts;
            std::array<u32, 32> role_counts{};
            size_t registered = 0;
            size_t one_by_one = 0;
            size_t up_to_4k = 0;
            size_t up_to_64k = 0;
            size_t at_least_1m = 0;
            u64 guest_bytes = 0;
            u64 backing_bytes = 0;
            for (const Image& resident_image : slot_images) {
                if (False(resident_image.flags & ImageFlagBits::Registered)) {
                    continue;
                }
                ++registered;
                const auto& resident_info = resident_image.info;
                if (resident_info.guest_address != 0) {
                    ++address_counts[resident_info.guest_address];
                }
                ++format_counts[static_cast<u32>(resident_info.pixel_format)];
                const u32 role = resident_image.usage.texture |
                                 (resident_image.usage.storage << 1) |
                                 (resident_image.usage.render_target << 2) |
                                 (resident_image.usage.depth_target << 3) |
                                 (resident_image.usage.vo_surface << 4);
                ++role_counts[role];
                one_by_one += resident_info.size.width == 1 &&
                              resident_info.size.height == 1 &&
                              resident_info.size.depth == 1;
                up_to_4k += resident_info.guest_size <= 4_KB;
                up_to_64k += resident_info.guest_size <= 64_KB;
                at_least_1m += resident_info.guest_size >= 1_MB;
                guest_bytes += resident_info.guest_size;
                backing_bytes += resident_image.AllocatedBackingBytes();
            }
            size_t duplicate_groups = 0;
            size_t duplicate_entries = 0;
            u32 maximum_address_copies = 0;
            std::vector<std::pair<VAddr, u32>> duplicate_addresses;
            for (const auto& [address, count] : address_counts) {
                maximum_address_copies = std::max(maximum_address_copies, count);
                if (count > 1) {
                    ++duplicate_groups;
                    duplicate_entries += count - 1;
                    duplicate_addresses.emplace_back(address, count);
                }
            }
            std::ranges::sort(duplicate_addresses, std::greater{},
                              &std::pair<VAddr, u32>::second);
            std::vector<std::pair<u32, u32>> populated_formats;
            populated_formats.reserve(format_counts.size());
            for (const auto& entry : format_counts) {
                populated_formats.push_back(entry);
            }
            std::ranges::sort(populated_formats, std::greater{},
                              &std::pair<u32, u32>::second);
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_A750_IMAGE_OBJECT_CENSUS] registered=%zu slots=%zu "
                "uniqueAddresses=%zu duplicateGroups=%zu duplicateEntries=%zu "
                "maxAddressCopies=%u oneByOne=%zu le4KiB=%zu le64KiB=%zu "
                "ge1MiB=%zu guestMiB=%llu backingMiB=%llu "
                "rolesNone=%u texture=%u storage=%u textureStorage=%u rt=%u "
                "depth=%u vo=%u",
                registered, slot_images.size(), address_counts.size(),
                duplicate_groups, duplicate_entries, maximum_address_copies,
                one_by_one, up_to_4k, up_to_64k, at_least_1m,
                static_cast<unsigned long long>(guest_bytes / 1_MB),
                static_cast<unsigned long long>(backing_bytes / 1_MB),
                role_counts[0], role_counts[1], role_counts[2], role_counts[3],
                role_counts[4], role_counts[8], role_counts[16]);
            for (size_t i = 0; i < std::min<size_t>(8, duplicate_addresses.size()); ++i) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_A750_IMAGE_DUPLICATE_ADDRESS] ordinal=%zu "
                    "address=0x%llx copies=%u",
                    i + 1,
                    static_cast<unsigned long long>(duplicate_addresses[i].first),
                    duplicate_addresses[i].second);
            }
            for (size_t i = 0; i < std::min<size_t>(8, populated_formats.size()); ++i) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_A750_IMAGE_FORMAT_CENSUS] ordinal=%zu format=%u count=%u",
                    i + 1, populated_formats[i].first,
                    populated_formats[i].second);
            }
        }
    }
#else
    constexpr bool image_object_trigger = false;
    constexpr bool image_object_pressure = false;
    constexpr bool image_object_critical = false;
#endif
    if (total_used_memory < trigger_gc_memory && !device_emergency &&
        !image_object_trigger) {
#ifdef __ANDROID__
        if (trace_residency_sample) {
            sample_trace_backings();
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_A750_IMAGE_RESIDENCY] tick=%llu state=below-trigger "
                "deviceMiB=%llu budgetMiB=%llu imageMiB=%llu images=%zu "
                "hostRegisteredMiB=%llu hostRetiringMiB=%llu backings=%zu "
                "triggerMiB=%llu pressureMiB=%llu criticalMiB=%llu emergency=0",
                static_cast<unsigned long long>(gc_tick),
                static_cast<unsigned long long>(sampled_device_memory.value_or(0) / 1_MB),
                static_cast<unsigned long long>(device_budget / 1_MB),
                static_cast<unsigned long long>(total_used_memory / 1_MB),
                slot_images.size(),
                static_cast<unsigned long long>(trace_registered_backing_bytes / 1_MB),
                static_cast<unsigned long long>(trace_retiring_backing_bytes / 1_MB),
                trace_backing_count,
                static_cast<unsigned long long>(trigger_gc_memory / 1_MB),
                static_cast<unsigned long long>(pressure_gc_memory / 1_MB),
                static_cast<unsigned long long>(critical_gc_memory / 1_MB));
        }
#endif
        return;
    }
#ifdef __ANDROID__
    // Exact D9 keeps every image candidate alive once the object watermark is
    // active. Walking and TouchImage-ing the whole protected LRU therefore
    // cannot reclaim a byte. Skip only that proven no-victim state; a real
    // byte-critical or device-emergency signal keeps the established path.
    const bool protected_gc_candidate =
        instance.GetCapabilityProfile()
                .optimization_a750_protected_gc_fast_noop_ab &&
        protect_all_image_identities && !force_pressure_scan &&
        !device_emergency && total_used_memory < critical_gc_memory;
    const bool protected_gc_audit_scan =
        protected_gc_candidate && (gc_tick & 127u) == 0u;
    const bool protected_gc_fast_noop =
        protected_gc_candidate && !protected_gc_audit_scan;
    if (protected_gc_fast_noop) {
        VideoCore::RecordMobileGpuTextureResidencyGcNoVictimSkip();
        if (trace_residency_sample) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_A750_IMAGE_RESIDENCY] tick=%llu "
                "state=no-victim-skip imageMiB=%llu images=%zu emergency=0",
                static_cast<unsigned long long>(gc_tick),
                static_cast<unsigned long long>(total_used_memory / 1_MB),
                slot_images.size());
        }
        return;
    }
#endif
    bool pressured = false;
    bool aggresive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;
    size_t deleted_images = 0;
#ifdef __ANDROID__
    size_t object_gc_protected_images = 0;
#endif

    const auto configure = [&](bool allow_aggressive) {
        pressured = device_emergency || total_used_memory >= pressure_gc_memory ||
                    image_object_pressure;
        aggresive = allow_aggressive &&
                    (device_emergency ||
                     total_used_memory >= critical_gc_memory ||
                     image_object_critical);
#ifdef __ANDROID__
        if (protect_all_image_identities &&
            !device_emergency && total_used_memory < critical_gc_memory) {
            ticks_to_destroy = aggresive ? 32 : pressured ? 48 : 64;
            num_deletions = aggresive ? 32 : pressured ? 16 : 8;
        } else {
            ticks_to_destroy = aggresive ? 16 : pressured ? 32 : 64;
            num_deletions = aggregate_pressure_policy.coordinated_turnip_retirement
                                ? aggresive ? 128 : pressured ? 64 : 16
                                : aggresive ? 48 : pressured ? 24 : 8;
        }
#else
        ticks_to_destroy = aggresive ? 160 : pressured ? 80 : 16;
        num_deletions = aggresive ? 40 : pressured ? 20 : 10;
#endif
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
    };
    const auto clean_up = [&](ImageId image_id) {
        if (num_deletions == 0) {
            return true;
        }
        auto& image = slot_images[image_id];
        const bool download = image.SafeToDownload();
#ifdef __ANDROID__
        const bool gpu_authored = True(image.flags & ImageFlagBits::GpuModified);
        const bool persistent_attachment_role = image.usage.render_target ||
                                                image.usage.depth_target ||
                                                image.usage.vo_surface;
        const u64 current_gpu_tick = scheduler.CurrentTick();
        const u64 known_gpu_tick = scheduler.KnownGpuTick();
        const bool access_may_be_in_flight =
            image.tick_accessed_last >= known_gpu_tick;
        // Bloodborne keeps guest texture descriptors live across the loading
        // transition even after the texture cache's ordinary LRU has gone
        // cold. Retiring those sampled inputs produced a correct first frame
        // followed by a white/missing-material scene when their deferred
        // VkImageView owners finally retired. Restrict the experiment to old
        // storage results whose complete image payload can first be tiled
        // back to guest memory by the established asynchronous readback path.
        const bool object_gc_protected = protect_all_image_identities;
        if (protect_all_image_identities) {
            static std::atomic<u32> candidate_logs{};
            const u32 ordinal =
                candidate_logs.fetch_add(1, std::memory_order_relaxed);
            if (ordinal < 64) {
                __android_log_print(
                    ANDROID_LOG_INFO, "LSX4Native",
                    "[EXECUTOR_A750_IMAGE_OBJECT_CANDIDATE] ordinal=%u action=%s "
                    "id=%u uid=%llu addr=0x%llx bytes=%llu extent=%ux%ux%u "
                    "flags=0x%x usage=%u,%u,%u,%u,%u bound=%u target=%u "
                    "safeDownload=%u gpuAuthored=%u current=%llu known=%llu "
                    "lastAccess=%llu",
                    ordinal + 1, object_gc_protected ? "protect" : "delete",
                    image_id.index,
                    static_cast<unsigned long long>(image.image_uid),
                    static_cast<unsigned long long>(image.info.guest_address),
                    static_cast<unsigned long long>(image.info.guest_size),
                    image.info.size.width, image.info.size.height,
                    image.info.size.depth, static_cast<u32>(image.flags),
                    image.usage.texture, image.usage.storage,
                    image.usage.render_target, image.usage.depth_target,
                    image.usage.vo_surface, image.binding.is_bound,
                    image.binding.is_target, download ? 1u : 0u,
                    gpu_authored ? 1u : 0u,
                    static_cast<unsigned long long>(current_gpu_tick),
                    static_cast<unsigned long long>(known_gpu_tick),
                    static_cast<unsigned long long>(image.tick_accessed_last));
            }
        }
        if (object_gc_protected) {
            // No ImageId class is safe to retire on this route yet.  Sampled
            // inputs and readback-capable storage results both produced the
            // same delayed scene corruption. Keep identities intact while the
            // creation census drives a pressure fix at the Vulkan object
            // source rather than masking it with logical cache eviction.
            TouchImage(image);
            ++object_gc_protected_images;
            return false;
        }
#endif
        if (download && !pressured) {
            return false;
        }
        if (download) {
            DownloadImageMemory(image_id, false, ImageDownloadClassification::GarbageCollection);
        }
        --num_deletions;
        ++deleted_images;
        FreeImage(image_id);
        if (!device_emergency && total_used_memory < critical_gc_memory) {
            if (aggresive) {
                num_deletions >>= 2;
                aggresive = false;
                return false;
            }
            if (pressured && total_used_memory < pressure_gc_memory) {
                num_deletions >>= 1;
                pressured = false;
            }
        }
        return false;
    };

    configure(false);
    lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);

    if (device_emergency || total_used_memory >= critical_gc_memory ||
        image_object_critical) {
        configure(true);
        lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
        if (deleted_images != 0 && !exact_a750_aggregate_pressure) {
            // Destruction is deferred until the owning submit retires. Drain
            // the established generic path under critical pressure. Exact
            // proprietary A750 coordinates TextureCache, BufferCache and
            // transient scratch retirement in Rasterizer::OnSubmit, outside
            // both cache mutexes.
#ifdef __ANDROID__
            const bool classify_finish = IsReadbackClassificationEnabled();
            const auto classify_finish_start = classify_finish
                                                   ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
#endif
            scheduler.Finish(Vulkan::Scheduler::ExecutorFinishReason::TextureReadback);
#ifdef __ANDROID__
            if (classify_finish) {
                RecordTextureGcCriticalFinishClassification(
                    static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - classify_finish_start)
                                         .count()));
            }
#endif
        }
    }
#ifdef __ANDROID__
    ASSERT_MSG(!protected_gc_audit_scan || deleted_images == 0,
               "A750 protected texture GC audit found an evictable identity");
    VideoCore::RecordMobileGpuTextureResidencyGc(
        deleted_images, force_pressure_scan, protected_gc_audit_scan);
#else
    VideoCore::RecordMobileGpuTextureResidencyGc(deleted_images, false, false);
#endif
#ifdef __ANDROID__
    if (trace_residency_sample) {
        sample_trace_backings();
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_A750_IMAGE_RESIDENCY] tick=%llu state=collect "
            "deviceMiB=%llu budgetMiB=%llu imageMiB=%llu images=%zu "
            "hostRegisteredMiB=%llu hostRetiringMiB=%llu backings=%zu "
            "deleted=%zu pressure=%u aggressive=%u emergency=%u objectGc=%u "
            "objectTrigger=%u objectPressure=%u objectCritical=%u protected=%zu",
            static_cast<unsigned long long>(gc_tick),
            static_cast<unsigned long long>(sampled_device_memory.value_or(0) / 1_MB),
            static_cast<unsigned long long>(device_budget / 1_MB),
            static_cast<unsigned long long>(total_used_memory / 1_MB),
            slot_images.size(),
            static_cast<unsigned long long>(trace_registered_backing_bytes / 1_MB),
            static_cast<unsigned long long>(trace_retiring_backing_bytes / 1_MB),
            trace_backing_count, deleted_images, pressured ? 1u : 0u,
            aggresive ? 1u : 0u, device_emergency ? 1u : 0u,
            exact_a750_image_object_gc ? 1u : 0u,
            image_object_trigger ? 1u : 0u, image_object_pressure ? 1u : 0u,
            image_object_critical ? 1u : 0u, object_gc_protected_images);
    }
#endif
}

void TextureCache::GarbageCollectSamplers() {
    std::scoped_lock lock{samplers_mutex};
    total_used_samplers = samplers.size();
    if (total_used_samplers < trigger_gc_samplers) {
        return;
    }

    bool pressured = false;
    bool aggressive = false;
    u64 ticks_to_destroy = 0;
    size_t num_deletions = 0;
    size_t scheduled_deletions = 0;
    const auto configure = [&](bool allow_aggressive) {
        total_used_samplers = samplers.size();
        pressured = total_used_samplers >= pressure_gc_samplers;
        aggressive = allow_aggressive && total_used_samplers >= critical_gc_samplers;
        ticks_to_destroy = aggressive ? 160 : pressured ? 80 : 16;
        ticks_to_destroy = std::min(ticks_to_destroy, gc_tick);
        num_deletions = aggressive ? 40 : pressured ? 20 : 10;
    };
    const auto clean_up = [&](u64 hash) {
        if (num_deletions == 0) {
            return true;
        }
        --num_deletions;
        auto sampler_it = samplers.find(hash);
        if (sampler_it == samplers.end()) {
            return false;
        }
        const size_t lru_id = sampler_it->second.lru_id;
        // Descriptor sets recorded in the current command buffer retain the raw VkSampler. GC
        // runs from OnSubmit before that buffer is submitted, so erasing the UniqueSampler here
        // violates Vulkan lifetime. Keep ownership until this recording epoch retires, matching
        // buffers, images and pipelines.
        auto retired_sampler = std::make_unique<Sampler>(std::move(sampler_it.value()));
        samplers.erase(sampler_it);
        sampler_lru_cache.Free(lru_id);
        scheduler.DeferOperation(
            [sampler = std::move(retired_sampler)]() mutable { sampler.reset(); });
        ++scheduled_deletions;
        return false;
    };

    configure(false);
    sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    if (samplers.size() >= critical_gc_samplers) {
        configure(true);
        sampler_lru_cache.ForEachItemBelow(gc_tick - ticks_to_destroy, clean_up);
    }
#ifdef __ANDROID__
    if (scheduled_deletions != 0) {
        static std::atomic<u32> sampler_retirement_logs{};
        const u32 ordinal = sampler_retirement_logs.fetch_add(1, std::memory_order_relaxed);
        if (ordinal < 16) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_SAMPLER_RETIREMENT] ordinal=%u scheduled=%zu remaining=%zu "
                "trigger=%llu pressure=%llu critical=%llu current=%llu knownGpu=%llu",
                ordinal + 1, scheduled_deletions, samplers.size(),
                static_cast<unsigned long long>(trigger_gc_samplers),
                static_cast<unsigned long long>(pressure_gc_samplers),
                static_cast<unsigned long long>(critical_gc_samplers),
                static_cast<unsigned long long>(scheduler.CurrentTick()),
                static_cast<unsigned long long>(scheduler.KnownGpuTick()));
        }
    }
#endif
}

void TextureCache::RunGarbageCollector(
    const std::optional<size_t> sampled_device_memory,
    const bool force_pressure_scan) {
    RefreshManagedResidencyPolicy();
    SCOPE_EXIT {
        ++gc_tick;
    };
    GarbageCollectImages(sampled_device_memory, force_pressure_scan);
    GarbageCollectSamplers();
}

void TextureCache::TouchImage(const Image& image) {
    lru_cache.Touch(image.lru_id, gc_tick);
}

void TextureCache::DeleteImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(!image.IsTracked(), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");
    download_images.erase(image_id);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    {
        std::scoped_lock lk{executor_replay_candidates_mutex};
        std::erase(executor_live_drawn_color_candidates, image_id);
        const bool slot0_tracking =
            instance.GetCapabilityProfile().diagnostic_a750_videoout_slot0_tracking;
        u32 expected = slot0_tracking ? image_id.index + 1 : image_id.index;
        executor_live_last_color_target.compare_exchange_strong(
            expected, 0, std::memory_order_relaxed, std::memory_order_relaxed);
        std::erase_if(executor_live_drawn_color_candidates_uid,
                      [image_id](const ExecutorLiveColorCandidateUid& candidate) {
                          return candidate.image_id == image_id;
                      });
        u64 expected_uid = image.image_uid;
        executor_live_uid_last_color_uid.compare_exchange_strong(
            expected_uid, 0, std::memory_order_release, std::memory_order_relaxed);
        std::erase_if(executor_live_resolve_pairs_uid,
                      [image_id](const ExecutorLiveResolvePairUid& pair) {
                          return pair.source_id == image_id ||
                                 pair.destination_id == image_id;
                      });
    }
#endif

    const auto& meta_info = image.info.meta_info;
    if (image.hle_meta_residencies.empty()) {
        // Marker-off path preserves the established bookkeeping exactly.
        if (meta_info.cmask_addr) {
            surface_metas.erase(meta_info.cmask_addr);
        }
        if (meta_info.fmask_addr) {
            surface_metas.erase(meta_info.fmask_addr);
        }
        if (meta_info.htile_addr) {
            surface_metas.erase(meta_info.htile_addr);
        }
    } else {
        // Late reassociation can leave more metadata keys than the three
        // current ImageInfo fields. Erase every recorded key only when no
        // other live image retains a sidecar for it; shared keys keep their
        // map entry and remain covered by the other image's count.
        for (const auto& range : image.hle_meta_residencies) {
            bool used_by_other_image = false;
            for (const Image& other : slot_images) {
                if (&other == &image) {
                    continue;
                }
                if (std::ranges::any_of(
                        other.hle_meta_residencies,
                        [&range](const auto& other_range) {
                            return other_range.source_address ==
                                   range.source_address;
                        })) {
                    used_by_other_image = true;
                    break;
                }
            }
            if (!used_by_other_image) {
                surface_metas.erase(range.source_address);
            }
        }
    }
    // Metadata publication is gone before its conservative sidecar lease.
    // This balances both addresses present at RegisterImage and addresses
    // attached later by FindRenderTarget/FindDepthTarget.
    UntrackImageMetaResidencies(image);

    scheduler.DeferOperation([this, image_id] {
        Image& image = slot_images[image_id];
        for (auto& backing : image.backing_images) {
            for (const ImageViewId image_view_id : backing.image_view_ids) {
                slot_image_views.erase(image_view_id);
            }
        }
        slot_images.erase(image_id);
    });
}

}
