import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class VideoOutTileAlignedSourceContractTests(unittest.TestCase):
    def test_registered_surface_accepts_only_its_tile_aligned_alias(self):
        source = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text()
        self.assertIn("if (preferred_image && tile_aligned_height > height) {", source)
        self.assertNotIn("if (!preferred_valid && tile_aligned_height > height) {", source)
        self.assertIn(
            "candidate_info.guest_address == preferred_info.guest_address", source
        )

    def test_detached_tile_aligned_live_target_is_strictly_workload_bound(self):
        source = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text()
        function = source.split(
            "ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget", 1
        )[1].split("void TextureCache::ExecutorReplayReadbackColorTarget", 1)[0]
        fallback = function.split(
            "const bool detached_tile_aligned_live_target", 1
        )[1].split("static std::atomic<u32> detached_tile_aligned_logs", 1)[0]
        for required in (
            "image.usage.render_target",
            "image.usage.vo_surface",
            "ImageFlagBits::Registered",
            "ImageFlagBits::GpuModified",
            "candidate_info.guest_address != preferred_info.guest_address",
            "candidate_info.size.height == tile_aligned_height",
            "candidate_info.pitch == preferred_info.pitch",
            "candidate_info.num_bits == preferred_info.num_bits",
            "IsVulkanFormatCompatible",
        ):
            self.assertIn(required, fallback)
        self.assertIn("[EXECUTOR_VIDEOOUT_TILE_ALIGNED_LIVE_TARGET]", function)

    def test_presenter_crops_tile_aligned_height_to_videoout_height(self):
        source = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp").read_text()
        self.assertIn("image_size.height == tile_aligned_height", source)
        self.assertIn("image_size.height = attribute.attrib.height", source)

    def test_same_allocation_alias_precedes_registered_videoout_fallback(self):
        source = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text()
        function = source.split(
            "ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget", 1
        )[1].split("void TextureCache::ExecutorReplayReadbackColorTarget", 1)[0]
        authority = function.index(
            "if (preferred_valid && !instance.UseLegacyRenderPass())"
        )
        preferred_candidate_fallback = function.index(
            "if (preferred_valid &&\n"
            "        std::ranges::find(candidates, preferred) != candidates.end())"
        )
        compatible_alias_fallback = function.index("if (preferred_image) {")
        self.assertLess(compatible_alias_fallback, preferred_candidate_fallback)
        self.assertLess(preferred_candidate_fallback, authority)
        self.assertIn("same-allocation render-target alias above", function)

    def test_detached_tile_aligned_target_precedes_touched_scanout(self):
        source = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text()
        function = source.split(
            "ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget", 1
        )[1].split("void TextureCache::ExecutorReplayReadbackColorTarget", 1)[0]
        detached_target = function.index(
            "const bool detached_tile_aligned_live_target"
        )
        preferred_candidate_fallback = function.index(
            "if (preferred_valid &&\n"
            "        std::ranges::find(candidates, preferred) != candidates.end())"
        )
        self.assertLess(detached_target, preferred_candidate_fallback)

    def test_same_address_without_gpu_written_render_target_role_is_rejected(self):
        source = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text()
        function = source.split(
            "ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget", 1
        )[1].split("void TextureCache::ExecutorReplayReadbackColorTarget", 1)[0]
        alias_contract = function.split(
            "const auto is_authoritative_render_target_alias", 1
        )[1].split("static std::atomic<bool> traced_live_candidate_set", 1)[0]
        for required in (
            "image.usage.render_target",
            "image.usage.vo_surface",
            "ImageFlagBits::Registered",
            "ImageFlagBits::GpuModified",
            "candidate_info.guest_size == preferred_info.guest_size",
            "candidate_info.pitch == preferred_info.pitch",
            "candidate_info.resources == preferred_info.resources",
            "candidate_info.num_samples == preferred_info.num_samples",
            "candidate_info.num_bits == preferred_info.num_bits",
            "candidate_info.type == preferred_info.type",
            "candidate_info.tile_mode == preferred_info.tile_mode",
        ):
            self.assertIn(required, alias_contract)
        self.assertNotIn(
            "if (image.info.guest_address == preferred_image->info.guest_address &&",
            function,
        )
        self.assertIn("if (authoritative) {", function)

    def test_live_candidate_identity_is_generation_aware_on_all_android_paths(self):
        texture = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text()
        rasterizer = (
            ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
        ).read_text()
        track = texture.split(
            "void TextureCache::ExecutorTrackLiveDrawnColorTarget", 1
        )[1].split("ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget", 1)[0]
        take = texture.split(
            "ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget", 1
        )[1].split("void TextureCache::ExecutorReplayReadbackColorTarget", 1)[0]
        delete = texture.split("void TextureCache::DeleteImage", 1)[1].split(
            "const auto& meta_info", 1
        )[0]
        self.assertIn("const bool uid_guard = true;", track)
        self.assertIn("const bool uid_guard = true;", take)
        self.assertIn("current_uid != image_uid", track)
        self.assertIn("image.image_uid != candidate.image_uid", take)
        self.assertIn("std::erase_if(executor_live_drawn_color_candidates_uid", delete)
        self.assertIn("cb_image_uids.fill(0);", rasterizer)
        self.assertIn("cb_image_uids[cb] = image->image_uid;", rasterizer)

    def test_resolve_destination_is_lineage_bound_and_source_is_not_presented(self):
        header = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.h").read_text()
        texture = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text()
        rasterizer = (
            ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
        ).read_text()
        self.assertIn("struct ExecutorLiveResolvePairUid", header)
        self.assertIn("ExecutorTrackLiveResolvedColorTarget", header)
        self.assertIn("ExecutorTrackLiveResolvedColorTarget(", rasterizer)
        take = texture.split(
            "ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget", 1
        )[1].split("void TextureCache::ExecutorReplayReadbackColorTarget", 1)[0]
        for required in (
            "source.image_uid != it->source_uid",
            "destination.image_uid != it->destination_uid",
            "destination.info.guest_address != preferred_image->info.guest_address",
            "source.usage.render_target",
            "[EXECUTOR_VIDEOOUT_RESOLVE_SOURCE]",
            "action=present-destination",
            "return it->destination_id;",
        ):
            self.assertIn(required, take)
        self.assertNotIn("action=present-source", take)
        self.assertNotIn("return it->source_id;", take)


if __name__ == "__main__":
    unittest.main()
