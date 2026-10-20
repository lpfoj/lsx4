from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
INFO = (ROOT / "funnel-arm/src/shader_recompiler/info.h").read_text(encoding="utf-8")
CACHE_H = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.h"
).read_text(encoding="utf-8")
CACHE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")
SERIALIZATION = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp"
).read_text(encoding="utf-8")
GRADLE = (ROOT / "android-app/app/build.gradle").read_text(encoding="utf-8")


class ProgramHotFlattenedLeaseContractTests(unittest.TestCase):
    def test_candidate_is_explicitly_gated_and_proves_first_use(self):
        self.assertIn("run-enable-program-hot-flattened-lease", CACHE)
        self.assertIn("ExecutorProgramHotFlattenedLeaseEnabled()", CACHE)
        self.assertIn("program-hot-flattened-lease.txt", CACHE)
        self.assertIn("copySkipped=1 immutableLease=1", CACHE)

    def test_info_exposes_one_effective_immutable_payload(self):
        self.assertIn(
            "std::shared_ptr<const std::vector<u32>> flattened_ud_lease", INFO
        )
        self.assertIn("FlattenedUserData() const noexcept", INFO)
        self.assertIn("flattened_ud_lease.reset();", INFO)
        read_sharp = INFO.split("inline T ReadUdSharp", 1)[1].split(
            "template <typename T>\n    T ReadUdReg", 1
        )[0]
        self.assertIn("FlattenedUserData()", read_sharp)

    def test_program_hot_hit_uses_lease_but_keeps_existing_fallback(self):
        self.assertIn("ActivateCachedPermutLease", CACHE_H)
        hit = CACHE.split("if (program_hot_hit != nullptr)", 1)[1].split(
            "const auto remember_program_hot", 1
        )[0]
        self.assertIn("ExecutorProgramHotFlattenedLeaseEnabled()", hit)
        self.assertIn("ActivateCachedPermutLease", hit)
        self.assertIn("ActivateCachedPermut(", hit)
        self.assertIn("ActivatePermut(", hit)

    def test_consumers_and_persistence_use_the_effective_payload(self):
        self.assertIn("stage.FlattenedUserData()", RASTERIZER)
        self.assertIn("probe_stage->FlattenedUserData()", RASTERIZER)
        self.assertIn("info.Write(FlattenedUserData());", SERIALIZATION)
        self.assertIn("flattened_ud_lease.reset();", SERIALIZATION)
        snapshot = CACHE.split("const auto store_snapshot", 1)[1].split(
            "auto [it_pgm, new_program]", 1
        )[0]
        self.assertIn("info.FlattenedUserData()", snapshot)

    def test_apk_freshness_gate_tracks_all_native_sources_in_candidate(self):
        for source in (
            "vk_pipeline_cache.cpp",
            "vk_pipeline_cache.h",
            "vk_rasterizer.cpp",
            "shader_recompiler/info.h",
        ):
            self.assertIn(source, GRADLE)


if __name__ == "__main__":
    unittest.main()
