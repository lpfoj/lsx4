import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")
PROFILE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h"
).read_text(encoding="utf-8")


class TurnipCompletionSidecarAbContractTests(unittest.TestCase):
    def test_a840_default_and_diagnostic_ab_are_turnip_only(self):
        self.assertIn(
            "run-diagnostic-turnip-elide-completion-sidecar", RASTERIZER
        )
        self.assertIn(
            "run-bisect-disable-a840-turnip-completion-sidecar-elision",
            RASTERIZER,
        )
        self.assertIn("MobileDriverFamily::MesaTurnip", PROFILE)
        self.assertIn("CapabilityQuirk::ElideCpuCompletionSidecarBarrier",
                      PROFILE)
        self.assertIn('exact_a840_default ? "a840-default" : "diagnostic"',
                      RASTERIZER)
        self.assertIn("turnip_completion_sidecar_elision", RASTERIZER)

    def test_default_is_capability_scoped_to_turnip_a840(self):
        self.assertIn("IsExactMesaTurnipAdreno840", PROFILE)
        self.assertIn("ExactMesaTurnipAdreno840RuntimeQuirks", PROFILE)
        self.assertIn("MobileDriverFamily::MesaTurnip", PROFILE)
        self.assertIn("Adreno840DeviceId", PROFILE)
        self.assertIn("Adreno750DeviceId) == 0", PROFILE)
        self.assertIn("ElideCpuCompletionSidecarBarrier", PROFILE)

    def test_default_resource_hazard_paths_remain_present(self):
        self.assertIn("scheduler.FlushPendingBufferTransfers();", RASTERIZER)
        self.assertIn("scheduler.TryCompletionBarrierInsideRendering()", RASTERIZER)
        self.assertIn("scheduler.EndRendering(", RASTERIZER)
        self.assertIn("vk::PipelineStageFlagBits2::eAllCommands", RASTERIZER)


if __name__ == "__main__":
    unittest.main()
