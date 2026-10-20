from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"
PROFILE = (VIDEO / "vk_capability_profile.h").read_text(encoding="utf-8")
INSTANCE = (VIDEO / "vk_instance.cpp").read_text(encoding="utf-8")
PIPELINE_CACHE_H = (VIDEO / "vk_pipeline_cache.h").read_text(encoding="utf-8")
PIPELINE_CACHE = (VIDEO / "vk_pipeline_cache.cpp").read_text(encoding="utf-8")
GRAPHICS = (VIDEO / "vk_graphics_pipeline.cpp").read_text(encoding="utf-8")
COMPUTE = (VIDEO / "vk_compute_pipeline.cpp").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class A750DescriptorTopologyContractTest(unittest.TestCase):
    def test_marker_is_exact_a750_and_default_off(self) -> None:
        self.assertIn("diag-sm8650-a750-descriptor-topology", INSTANCE)
        self.assertIn("diagnostic_a750_descriptor_topology", PROFILE)
        self.assertIn(
            "(capability_profile.exact_proprietary_adreno750 ||\n"
            "         capability_profile.exact_mesa_turnip_adreno750)",
            INSTANCE,
        )
        self.assertIn(
            "static_assert(!CapabilityProfile{}.diagnostic_a750_descriptor_topology);",
            PROFILE,
        )

    def test_census_is_pipeline_creation_only_not_bind_or_submit(self) -> None:
        for source in (GRAPHICS, COMPUTE):
            self.assertIn(
                "if (instance.GetCapabilityProfile()."
                "diagnostic_a750_descriptor_topology)",
                source,
            )
            self.assertIn("RecordPipelineDescriptorTopology(", source)
        self.assertNotIn("RecordPipelineDescriptorTopology(", (
            VIDEO / "vk_pipeline_common.cpp"
        ).read_text(encoding="utf-8").split("void Pipeline::BindResources", 1)[1])
        self.assertNotIn("RecordPipelineDescriptorTopology(", (
            VIDEO / "vk_rasterizer.cpp"
        ).read_text(encoding="utf-8").split("void Rasterizer::OnSubmit", 1)[1])

    def test_snapshot_is_monotonic_and_proof_only(self) -> None:
        for token in (
            "descriptor_layouts",
            "push_descriptor_layouts",
            "legacy_descriptor_layouts",
            "descriptor_bindings",
            "max_descriptor_bindings",
            "max_push_descriptors",
            "max_update_after_bind_descriptors",
        ):
            self.assertIn(token, PIPELINE_CACHE_H)
            self.assertIn(token, PIPELINE_CACHE)
        self.assertIn("std::memory_order_relaxed", PIPELINE_CACHE)
        for key in (
            '"descriptorTopology"',
            '"descriptorLayouts"',
            '"pushDescriptorLayouts"',
            '"legacyDescriptorLayouts"',
            '"maxDescriptorBindings"',
            '"maxPushDescriptors"',
            '"maxUpdateAfterBindDescriptors"',
        ):
            self.assertIn(key, RUNTIME)

    def test_actual_descriptor_limits_are_reported(self) -> None:
        self.assertIn("maxPushDescriptors=%u", INSTANCE)
        self.assertIn("maxUpdateAfterBindDescriptors=%u", INSTANCE)
        self.assertIn("vk12_props.maxUpdateAfterBindDescriptorsInAllPools", INSTANCE)


if __name__ == "__main__":
    unittest.main()
