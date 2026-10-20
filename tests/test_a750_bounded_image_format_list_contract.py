import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PROFILE = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
    encoding="utf-8"
)
INSTANCE = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text(
    encoding="utf-8"
)
IMAGE = (ROOT / "funnel-arm/src/video_core/texture_cache/image.cpp").read_text(
    encoding="utf-8"
)
COMPATIBILITY = (
    ROOT / "funnel-arm/src/video_core/texture_cache/host_compatibility.cpp"
).read_text(encoding="utf-8")


class A750BoundedImageFormatListContractTest(unittest.TestCase):
    def test_route_is_default_off_and_exact_a750_driver_family_only(self) -> None:
        self.assertIn("opt-sm8650-a750-bounded-image-format-list", INSTANCE)
        self.assertIn("optimization_a750_bounded_image_format_list", PROFILE)
        self.assertIn(
            "static_assert(!CapabilityProfile{}.optimization_a750_bounded_image_format_list);",
            PROFILE,
        )
        self.assertIn(
            "a750_bounded_image_format_list_requested &&\n"
            "        (capability_profile.exact_proprietary_adreno750 ||\n"
            "         capability_profile.exact_mesa_turnip_adreno750)",
            INSTANCE,
        )
        self.assertIn("exactProprietaryA750=%u exactTurnipA750=%u", INSTANCE)

    def test_format_list_is_filtered_to_device_mapped_guest_formats(self) -> None:
        self.assertIn("VulkanCompatibleViewFormats", COMPATIBILITY)
        self.assertIn("VIEW_FORMAT_TABLE", COMPATIBILITY)
        self.assertIn("LiverpoolToVK::SurfaceFormats()", IMAGE)
        self.assertIn("LiverpoolToVK::DepthFormats()", IMAGE)
        self.assertIn("instance->GetSupportedFormat(guest_view_format, format_features)", IMAGE)
        self.assertIn("std::ranges::find(formats, host_view_format)", IMAGE)
        self.assertIn("vk::ImageFormatListCreateInfo", IMAGE)
        self.assertIn("CreateTextureCacheImage", IMAGE)
        self.assertIn("image.image_ci.pNext = nullptr", IMAGE)

    def test_candidate_does_not_enter_draw_submit_or_present_paths(self) -> None:
        for relative in (
            "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp",
            "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp",
            "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn("optimization_a750_bounded_image_format_list", source)


if __name__ == "__main__":
    unittest.main()
