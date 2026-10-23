// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "core/memory.h"
#include "shader_recompiler/resource.h"
#include "video_core/multi_level_page_table.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"

namespace AmdGpu {
struct Liverpool;
}

namespace VideoCore {

class BufferCache;
class PageManager;

class TextureCache {
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct ExactSurfaceHotEntry {
        u64 identity{};
        ImageId image_id{};
        u64 image_uid{};
        bool valid{};
    };

    static constexpr size_t ExactSurfaceHotEntryCount = 4096;
    static_assert((ExactSurfaceHotEntryCount & (ExactSurfaceHotEntryCount - 1)) == 0);

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 10;
        static constexpr size_t PageBits = 20;
    };
    using PageTable = MultiLevelPageTable<Traits>;

public:
    struct ResidencyTelemetrySnapshot {
        u64 guest_bytes{};
        u64 image_entries{};
        u64 image_view_entries{};
        u64 sampler_entries{};
    };

    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    enum class ExecutorGpuContentRole : u32 {
        SampledBeforeDraw,
        ColorBeforeDraw,
        DepthBeforeDraw,
        StorageAfterDispatch,
        ColorAfterDraw,
        DepthAfterDraw,
        ColorPassEnd,
        DepthPassEnd,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

    class ExactSurfaceBindBatch {
    public:
        ExactSurfaceBindBatch(const ExactSurfaceBindBatch&) = delete;
        ExactSurfaceBindBatch& operator=(const ExactSurfaceBindBatch&) = delete;
        ExactSurfaceBindBatch(ExactSurfaceBindBatch&&) noexcept = default;
        ExactSurfaceBindBatch& operator=(ExactSurfaceBindBatch&&) noexcept = default;

        [[nodiscard]] bool IsActive() const noexcept {
            return owner != nullptr && lock.owns_lock();
        }

        void End() noexcept {
            if (lock.owns_lock()) {
                lock.unlock();
            }
            owner = nullptr;
        }

    private:
        friend class TextureCache;
        explicit ExactSurfaceBindBatch(TextureCache& cache);

        TextureCache* owner{};
        std::unique_lock<std::mutex> lock{};
        u32 exact_hits{};
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache, PageManager& tracker);
    ~TextureCache();

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    void InvalidateMemory(VAddr addr, size_t size);

    // A protected guest write is observed before the faulting CPU instruction
    // is retried. Publish GPU-authored images first when the write covers only
    // part of their guest allocation, so the later CPU upload cannot replace
    // untouched GPU texels with stale backing memory.
    void PublishGpuImagesBeforeCpuWrite(VAddr addr, size_t size);

    // Caller already owns TextureCache::mutex (for example RefreshImage).
    void InvalidateMemoryLocked(VAddr addr, size_t size);

    void InvalidateMemoryFromGPU(VAddr address, size_t max_size);

    void UnmapMemory(VAddr cpu_addr, size_t size);

    void ProcessDownloadImages();

    [[nodiscard]] bool HasPendingDownloadImages() const noexcept {
        return !download_images.empty();
    }

    ImageId executor_replay_color_target{};
    std::vector<ImageId> executor_replay_color_candidates;
    std::vector<ImageId> executor_live_drawn_color_candidates;
    struct ExecutorLiveColorCandidateUid {
        ImageId image_id{};
        u64 image_uid{};
    };
    struct ExecutorLiveResolvePairUid {
        ImageId source_id{};
        u64 source_uid{};
        ImageId destination_id{};
        u64 destination_uid{};
    };
    std::vector<ExecutorLiveColorCandidateUid> executor_live_drawn_color_candidates_uid;
    std::vector<ExecutorLiveResolvePairUid> executor_live_resolve_pairs_uid;
    std::atomic<u32> executor_live_last_color_target{};
    std::atomic<u64> executor_live_uid_last_color_uid{};
    std::mutex executor_replay_candidates_mutex;
    void ExecutorReplayReadbackColorTarget();
    void ExecutorTrackLiveDrawnColorTarget(ImageId image_id, u64 image_uid);
    void ExecutorTrackLiveResolvedColorTarget(ImageId source_id, u64 source_uid,
                                              ImageId destination_id, u64 destination_uid);
    [[nodiscard]] ImageId ExecutorTakeLiveFullResolutionTarget(u32 width, u32 height,
                                                               ImageId preferred = {});

    void ExecutorTraceVideoOutSource(ImageId image_id, u64 sequence, u32 flags_before_refresh);

    [[nodiscard]] bool ExecutorGpuContentProbeEnabled() const;
    [[nodiscard]] u64 ExecutorGpuContentProbeExtentFilter() const;
    void ExecutorGpuContentProbeObserveImage(const AmdGpu::Image& image) const;
    [[nodiscard]] bool ExecutorGpuContentProbeDrawSelected(
        u64 sequence, u64 fragment_hash, u64 attachment_signature,
        u32 color_attachment_count, u32 color_width, u32 color_height, bool has_depth,
        u32 element_count, bool indirect, bool sampled_extent_matches = false) const;
    [[nodiscard]] bool ExecutorGpuContentProbeDispatchSelected(u64 sequence) const;
    void ExecutorGpuContentProbe(ImageId image_id, ExecutorGpuContentRole role, u32 stage,
                                 u64 stage_hash, u64 sequence);

    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    [[nodiscard]] ExactSurfaceBindBatch BeginExactSurfaceBindBatch();

    [[nodiscard]] ImageId FindImageExactHot(ImageDesc& desc,
                                            ExactSurfaceBindBatch& batch);

    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    [[nodiscard]] ImageView* FindTexture(ImageId image_id, const ImageDesc& desc);

    [[nodiscard]] ImageView* FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    [[nodiscard]] ImageView* FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    [[nodiscard]] bool UpdateImage(ImageId image_id);

    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id,
                                                               std::unique_lock<std::mutex>& lock);

    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, BindingType binding,
                                      ImageId image_id,
                                      std::unique_lock<std::mutex>& lock);

    [[nodiscard]] bool RefreshImage(
        Image& image,
        Core::MemoryManager::SnapshotReadGuard* external_snapshot_guard = nullptr);
    [[nodiscard]] bool RefreshImageOrdered(
        ImageId image_id, std::unique_lock<std::mutex>& lock);

    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base);

    [[nodiscard]] Image& GetImage(ImageId id) {
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    [[nodiscard]] ImageView& GetImageView(ImageId id) {
        return slot_image_views[id];
    }

    ImageId GetAssociatedDepth(Image& image) {
        if (!image.depth_id) {
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        image.DisassociateDepth();
        return {};
    }

    bool IsMeta(VAddr address) const {
        return surface_metas.contains(address);
    }

    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            return true;
        }
        return false;
    }

    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    void RunGarbageCollector(std::optional<size_t> sampled_device_memory = std::nullopt,
                             bool force_pressure_scan = false);

    [[nodiscard]] ResidencyTelemetrySnapshot
    GetResidencyTelemetrySnapshot() const noexcept {
        return {
            .guest_bytes = total_used_memory,
            .image_entries = static_cast<u64>(slot_images.size()),
            .image_view_entries = static_cast<u64>(slot_image_views.size()),
            .sampler_entries = static_cast<u64>(samplers.size()),
        };
    }

    /// Device-local residency threshold shared with small renderer-owned
    /// auxiliary pools. This keeps those pools inside the same mobile memory
    /// policy instead of each inventing an unrelated absolute limit.
    [[nodiscard]] u64 ManagedResidencyTriggerBytes() const noexcept {
        return trigger_gc_memory;
    }

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = slot_images[image_id];
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            slot_images[image_id].flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 page_end = (addr + size - 1) >> Traits::PageBits;
        for (u64 page = addr >> Traits::PageBits; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    ImageId GetNullImage(vk::Format format);

    enum class ImageDownloadClassification : u8 {
        LinearImage,
        GarbageCollection,
    };

    bool DownloadImageMemory(ImageId image_id, bool sync,
                             ImageDownloadClassification classification);

    void DownloadedImagesThread(const std::stop_token& token);

    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    void RegisterImage(ImageId image);

    void UnregisterImage(ImageId image);

    void TrackImageMetaResidency(Image& image, VAddr address);

    void UntrackImageMetaResidencies(Image& image);

    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);

    void DeleteImage(ImageId image_id);

    void TouchImage(const Image& image);

    void FreeImage(ImageId image_id) {
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id);
    }

    void GarbageCollectImages(std::optional<size_t> sampled_device_memory,
                              bool force_pressure_scan);
    void GarbageCollectSamplers();
    void RefreshManagedResidencyPolicy(bool force = false);

    [[nodiscard]] ImageId FindExactSurfaceHotImageLocked(const ImageInfo& info);

private:
    struct HleResidencyLeaseControl {
        std::mutex mutex;
        std::condition_variable cv;
        PageManager* owner{};
        u32 running{};
        bool closing{};
    };
    struct HleResidencyLease {
        std::shared_ptr<HleResidencyLeaseControl> control;
        VAddr address{};
        u64 size{};
        bool held{};

        ~HleResidencyLease();
        void Release() noexcept;
    };

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    std::shared_ptr<HleResidencyLeaseControl> hle_residency_lease_control;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    tsl::robin_map<vk::Format, ImageId> null_images;
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    tsl::robin_map<u64, ImageIds> exact_surface_index;
    std::unique_ptr<ExactSurfaceHotEntry[]> exact_surface_hot_entries;
    bool exact_surface_hot_cache_enabled = false;
    bool exact_surface_prehash_probe_enabled = false;
    bool exact_surface_bind_batch_proof_logged = false;
    PageTable page_table;
    std::mutex mutex;
    std::mutex samplers_mutex;
    bool residency_policy_initialized = false;
    bool residency_policy_mobile = false;
    struct MetaDataInfo {
        enum class Type {
            CMask,
            FMask,
            HTile,
        };
        Type type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
};

}
