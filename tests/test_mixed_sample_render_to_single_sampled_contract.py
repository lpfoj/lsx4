import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class MixedSampleRenderToSingleSampledContractTests(unittest.TestCase):
    def test_inactive_registered_depth_buffer_does_not_collapse_color_samples(self):
        pipeline = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp").read_text()
        prepare = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text()
        for required in (
            "regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()",
            "regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid()",
            "const bool db_enabled = depth_enabled || stencil_enabled;",
            "key.z_format = db_enabled && regs.depth_buffer.DepthValid()",
            "key.stencil_format = db_enabled && regs.depth_buffer.StencilValid()",
        ):
            self.assertIn(required, pipeline)
        self.assertIn(
            "(regs.depth_control.depth_enable && regs.depth_buffer.DepthValid())",
            prepare,
        )
        self.assertIn(
            "(regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())",
            prepare,
        )

    def test_multisampled_render_targets_do_not_request_storage_usage_by_default(self):
        image = (ROOT / "funnel-arm/src/video_core/texture_cache/image.cpp").read_text()
        header = (ROOT / "funnel-arm/src/video_core/texture_cache/image.h").read_text()
        for required in (
            "initial_role == ImageBackingRole::RenderTarget",
            "info.num_samples > 1 && info.resources.levels == 1",
            "probe_samples(universal_usage, universal_features)",
            "probe_samples(render_target_usage, render_target_features)",
            "!(universal_samples & requested_samples)",
            "render_target_samples & requested_samples",
            "role_scoped_usage =",
            "format_features = role_scoped_usage ? FullImageUsageFormatFeatures(usage_flags)",
            "if (!role_scoped_usage || role == ImageBackingRole::Universal || !backing)",
            "[EXECUTOR_MULTISAMPLE_BACKING] phase=create",
            "[EXECUTOR_MULTISAMPLE_BACKING] phase=set",
        ):
            self.assertIn(required, image)
        self.assertIn("bool role_scoped_usage{};", header)

    def test_multisampled_role_expansion_rechecks_support_and_resolves_when_needed(self):
        image = (ROOT / "funnel-arm/src/video_core/texture_cache/image.cpp").read_text()
        for required in (
            "getImageFormatProperties2(expanded_format_info)",
            "expanded_properties.result == vk::Result::eSuccess",
            "expanded_supported_samples & current_samples",
            "SetBackingSamples(1, true);",
            "supported_samples = expanded_supported_samples;",
        ):
            self.assertIn(required, image)

    def test_extension_feature_is_a_capability_profile_route(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text()
        instance = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text()
        field = "multisampled_render_to_single_sampled"
        self.assertIn(field, profile)
        self.assertIn("PhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT", instance)
        self.assertIn("VK_EXT_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_EXTENSION_NAME", instance)
        self.assertIn("multisampledRenderToSingleSampled = multisampled_render_to_single_sampled", instance)
        self.assertIn(f"capability_profile.{field} =", instance)

    def test_mixed_depth_fallback_remains_until_per_format_extension_proof_exists(self):
        pipeline = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp").read_text()
        self.assertNotIn("implicit_single_sample_depth", pipeline)
        self.assertIn("!instance.IsMixedDepthSamplesSupported()", pipeline)
        self.assertIn("key.color_samples.fill(key.depth_samples);", pipeline)

    def test_extension_is_discovered_but_not_activated_without_per_format_proof(self):
        image = (ROOT / "funnel-arm/src/video_core/texture_cache/image.cpp").read_text()
        scheduler = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp").read_text()
        rasterizer = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text()
        self.assertNotIn("eMultisampledRenderToSingleSampledEXT", image)
        self.assertNotIn("MultisampledRenderToSingleSampledInfoEXT", scheduler)
        self.assertNotIn("implicit_rasterization_samples", rasterizer)

    def test_legacy_or_missing_feature_keeps_the_existing_fallback(self):
        pipeline = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp").read_text()
        self.assertIn("key.color_samples.fill(key.depth_samples);", pipeline)
        self.assertIn("key.num_samples = key.depth_samples;", pipeline)
        self.assertNotIn("implicit_single_sample_depth", pipeline)


if __name__ == "__main__":
    unittest.main()
