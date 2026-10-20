from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
PRESENTER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_presenter.cpp"
).read_text(encoding="utf-8")
RASTERIZER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_rasterizer.cpp"
).read_text(encoding="utf-8")
TEXTURE_CACHE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "texture_cache"
    / "texture_cache.cpp"
).read_text(encoding="utf-8")


class A750VideoOutExactIdentityContractTest(unittest.TestCase):
    def test_registration_and_prepare_use_exact_scanout_identity_on_a750(self):
        self.assertGreaterEqual(
            PRESENTER.count(
                "instance.GetCapabilityProfile().exact_proprietary_adreno750"
            ),
            2,
        )
        self.assertGreaterEqual(
            PRESENTER.count("desc, exact_videoout_identity"),
            2,
        )

    def test_resolve_publishes_the_exact_live_image_generation(self):
        resolve = re.search(
            r"void Rasterizer::Resolve\(\).*?\n\}", RASTERIZER, re.DOTALL
        )
        self.assertIsNotNone(resolve)
        body = resolve.group(0)
        self.assertIn("FindImage(mrt1_desc, true)", body)
        self.assertIn("ExecutorTrackLiveDrawnColorTarget(mrt1_id", body)
        self.assertIn("mrt1_image.image_uid", body)
        self.assertNotIn("scheduler.Finish", body)

    def test_registered_videoout_remains_authoritative_for_normal_present(self):
        lookup = re.search(
            r"ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget\(.*?\n\}",
            TEXTURE_CACHE,
            re.DOTALL,
        )
        self.assertIsNotNone(lookup)
        body = lookup.group(0)
        self.assertIn("preferred_valid && !instance.UseLegacyRenderPass()", body)
        self.assertIn("image.usage.vo_surface", body)
        self.assertIn("IsVulkanFormatCompatible", body)
        for forbidden in ("Finish(", "Wait(", "copyImage", "ExpandImage", "RefreshImage"):
            self.assertNotIn(forbidden, body)

    def test_a750_gc_avoids_the_generic_critical_finish(self):
        gc = re.search(
            r"void TextureCache::GarbageCollectImages\(.*?\n\}",
            TEXTURE_CACHE,
            re.DOTALL,
        )
        self.assertIsNotNone(gc)
        body = gc.group(0)
        self.assertIn("deleted_images != 0 && !exact_a750_aggregate_pressure", body)
        self.assertIn("scheduler.Finish", body)
        self.assertIn("outside\n            // both cache mutexes", body)

    def test_recreated_registered_surface_recovers_videoout_role(self):
        prepare = re.search(
            r"Frame\* Presenter::PrepareFrame\(.*?Frame\* frame = GetRenderFrame\(\);",
            PRESENTER,
            re.DOTALL,
        )
        self.assertIsNotNone(prepare)
        self.assertIn(
            "texture_cache.GetImage(image_id).usage.vo_surface = 1u",
            prepare.group(0),
        )

    def test_identity_fix_adds_no_wait_or_finish(self):
        registration = re.search(
            r"VideoCore::Image& Presenter::RegisterVideoOutSurface\(.*?\n\}",
            PRESENTER,
            re.DOTALL,
        )
        self.assertIsNotNone(registration)
        for forbidden in ("Wait", "Finish", "Flush", "pipelineBarrier"):
            self.assertNotIn(forbidden, registration.group(0))


if __name__ == "__main__":
    unittest.main()
