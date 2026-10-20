import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CAPABILITY_PROFILE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_capability_profile.h"
).read_text(encoding="utf-8")
INSTANCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_instance.cpp"
).read_text(encoding="utf-8")
SCHEDULER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_scheduler.cpp"
).read_text(encoding="utf-8")
LINEAGE_CONSUMERS = [
    (ROOT / "funnel-arm" / relative).read_text(encoding="utf-8")
    for relative in (
        "src/video_core/buffer_cache/buffer.cpp",
        "src/video_core/buffer_cache/buffer_cache.cpp",
        "src/video_core/texture_cache/texture_cache.cpp",
        "src/video_core/renderer_vulkan/vk_pipeline_cache.cpp",
        "src/video_core/renderer_vulkan/vk_rasterizer.cpp",
        "src/video_core/renderer_vulkan/vk_scheduler.cpp",
    )
]


class A750D9LineageProfileParityContract(unittest.TestCase):
    def test_d19_and_d20_have_exact_a750_sha_identities(self) -> None:
        for token in (
            "MesaTurnipA750D19FormattedNoLogDriverVersion",
            "MesaTurnipA750D19FormattedNoLogSha256",
            "90a23dc8aa854c5c9bb1fe43531f81a27de7ee69cd5e4330f49ae7d6a6a2c176",
            "MobileDriverBuild::MesaTurnipA750D19FormattedNoLog",
            "MesaTurnipA750D20NoBoCensusDriverVersion",
            "MesaTurnipA750D20NoBoCensusSha256",
            "df53cdaefc249f142b3c35c8f4df696466d3191d9ca2deb60845210c67b0c68f",
            "MobileDriverBuild::MesaTurnipA750D20NoBoCensus",
        ):
            self.assertIn(token, CAPABILITY_PROFILE)
        self.assertIn(
            "Adreno840DeviceId, MesaTurnipA750D19FormattedNoLogDriverVersion",
            CAPABILITY_PROFILE,
        )
        self.assertIn(
            "Adreno840DeviceId, MesaTurnipA750D20NoBoCensusDriverVersion",
            CAPABILITY_PROFILE,
        )

    def test_d9_d19_and_d20_share_one_typed_lineage_policy(self) -> None:
        helper_start = CAPABILITY_PROFILE.index(
            "constexpr bool IsMesaTurnipA750D9LineageBuild"
        )
        helper = CAPABILITY_PROFILE[helper_start : helper_start + 500]
        self.assertIn(
            "MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats",
            helper,
        )
        self.assertIn("MobileDriverBuild::MesaTurnipA750D19FormattedNoLog", helper)
        self.assertIn("MobileDriverBuild::MesaTurnipA750D20NoBoCensus", helper)
        self.assertIn(
            "capability_profile.exact_mesa_turnip_a750_d9_lineage =", INSTANCE
        )
        for consumer in LINEAGE_CONSUMERS:
            self.assertNotIn(
                "MesaTurnipA750Da14d65e44Ib32RolloverBoStats", consumer
            )

    def test_rejected_submit_wait_experiment_is_absent(self) -> None:
        for source in (CAPABILITY_PROFILE, INSTANCE, SCHEDULER):
            self.assertNotIn("SubmitRetirementCheckpoint", source)
            self.assertNotIn("submit_retirement_checkpoint", source)
            self.assertNotIn(
                "run-sm8650-a750-d19-submit-retirement-checkpoint-ab", source
            )


if __name__ == "__main__":
    unittest.main()
