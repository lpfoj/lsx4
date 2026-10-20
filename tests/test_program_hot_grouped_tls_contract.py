from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")


class ProgramHotGroupedTlsContractTests(unittest.TestCase):
    def test_route_is_default_off_and_marker_gated(self):
        self.assertIn("run-enable-program-hot-grouped-tls", SOURCE)
        self.assertIn("ExecutorProgramHotGroupedTlsEnabled", SOURCE)

    def test_grouped_state_contains_the_complete_existing_cache_state(self):
        for field in (
            "ProgramHotSets sets",
            "ProgramHotByteArray mru_ways",
            "ProgramHotByteArray valid_way_masks",
            "u64 lookups",
            "u64 hits",
            "u64 clock",
            "u64 exact_revalidations",
            "u64 exact_revalidation_hits",
            "u64 retained_content_versions",
        ):
            self.assertIn(field, SOURCE)

    def test_on_path_resolves_one_grouped_state(self):
        self.assertIn("static thread_local ProgramHotThreadState state", SOURCE)
        self.assertIn("grouped_state = &ExecutorProgramHotThreadState()", SOURCE)
        self.assertIn("ExecutorProgramHotThreadStateRefs(*grouped_state)", SOURCE)
        self.assertIn(".sets = &state.sets", SOURCE)
        self.assertIn(
            ".retained_content_versions = &state.retained_content_versions", SOURCE
        )

    def test_same_apk_off_control_preserves_legacy_layout(self):
        self.assertIn("static thread_local ProgramHotSets legacy_sets", SOURCE)
        self.assertIn("static thread_local u64 legacy_lookups", SOURCE)
        self.assertIn("static thread_local u64 legacy_retained_content_versions", SOURCE)

    def test_runtime_proof_does_not_claim_weakened_identity_or_bytes(self):
        self.assertIn("cacheFieldsUnchanged=1", SOURCE)
        self.assertIn("identityProofUnchanged=1", SOURCE)
        self.assertIn("byteProofUnchanged=1", SOURCE)


if __name__ == "__main__":
    unittest.main()
