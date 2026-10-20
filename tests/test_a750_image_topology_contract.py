from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "funnel-arm" / "src" / "video_core"
VULKAN = VIDEO / "renderer_vulkan"
PROFILE = (VULKAN / "vk_capability_profile.h").read_text(encoding="utf-8")
INSTANCE = (VULKAN / "vk_instance.cpp").read_text(encoding="utf-8")
PIPELINE_H = (VULKAN / "vk_pipeline_cache.h").read_text(encoding="utf-8")
PIPELINE = (VULKAN / "vk_pipeline_cache.cpp").read_text(encoding="utf-8")
IMAGE = (VIDEO / "texture_cache" / "image.cpp").read_text(encoding="utf-8")
IMAGE_VIEW = (VIDEO / "texture_cache" / "image_view.cpp").read_text(encoding="utf-8")
RASTERIZER = (VULKAN / "vk_rasterizer.cpp").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class A750ImageTopologyContractTest(unittest.TestCase):
    def test_marker_is_default_off_and_exact_a750_only(self) -> None:
        self.assertIn("diag-sm8650-a750-image-topology", INSTANCE)
        self.assertIn("diagnostic_a750_image_topology", PROFILE)
        self.assertIn(
            "static_assert(!CapabilityProfile{}.diagnostic_a750_image_topology);",
            PROFILE,
        )
        self.assertIn(
            "(capability_profile.exact_proprietary_adreno750 ||\n"
            "         capability_profile.exact_mesa_turnip_adreno750)",
            INSTANCE,
        )

    def test_census_is_resource_creation_only(self) -> None:
        self.assertIn("RecordRendererImageCreationTopology(", IMAGE)
        create_call = (
            "CreateTextureCacheImage(backing->image, image_ci, bounded_view_formats);"
        )
        self.assertIn(create_call, IMAGE)
        self.assertLess(
            IMAGE.index(create_call),
            IMAGE.index("RecordRendererImageCreationTopology("),
        )
        self.assertIn("RecordRendererImageViewTopology(", IMAGE_VIEW)
        self.assertIn("createImageViewUnique(image_view_ci)", IMAGE_VIEW)
        self.assertIn("EXECUTOR_A750_IMAGE_FORMAT_PAIR", PIPELINE)
        self.assertIn("reinterpret_index <= 32", PIPELINE)
        self.assertNotIn("RecordRendererImageCreationTopology(", RASTERIZER)
        self.assertNotIn("RecordRendererImageViewTopology(", RASTERIZER)
        common = (VULKAN / "vk_pipeline_common.cpp").read_text(encoding="utf-8")
        bind = common.split("void Pipeline::BindResources", 1)[1]
        self.assertNotIn("RecordRendererImage", bind)

    def test_snapshot_separates_mutability_reinterpretation_and_bytes(self) -> None:
        for token in (
            "image_creations",
            "mutable_image_creations",
            "extended_usage_image_creations",
            "color_storage_image_creations",
            "depth_image_creations",
            "block_image_creations",
            "image_allocation_bytes",
            "max_image_allocation_bytes",
            "bounded_format_list_image_creations",
            "bounded_format_list_entries",
            "max_bounded_format_list_entries",
            "image_view_creations",
            "reinterpreted_image_view_creations",
            "storage_image_view_creations",
        ):
            self.assertIn(token, PIPELINE_H)
            self.assertIn(token, PIPELINE)
        for token in (
            '"imageTopology"',
            '"imageCreations"',
            '"mutableImageCreations"',
            '"extendedUsageImageCreations"',
            '"colorStorageImageCreations"',
            '"imageAllocationBytes"',
            '"boundedFormatListImageCreations"',
            '"boundedFormatListEntries"',
            '"maxBoundedFormatListEntries"',
            '"imageViewCreations"',
            '"reinterpretedImageViewCreations"',
            '"storageImageViewCreations"',
        ):
            self.assertIn(token, RUNTIME)

    def test_c16_diagnostic_keeps_default_policy_and_never_waits(self) -> None:
        self.assertIn(
            "vk::ImageCreateFlagBits::eMutableFormat |\n"
            "                               vk::ImageCreateFlagBits::eExtendedUsage",
            IMAGE,
        )
        self.assertIn(
            "!instance->GetCapabilityProfile().optimization_a750_bounded_image_format_list",
            IMAGE,
        )
        self.assertNotIn("waitIdle", IMAGE)


if __name__ == "__main__":
    unittest.main()
