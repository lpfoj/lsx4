from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.h"
).read_text(encoding="utf-8")
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")


class ProgramFlattenedScratchRecycleContractTests(unittest.TestCase):
    def test_candidate_is_default_off_and_marker_gated(self):
        self.assertIn("run-enable-program-flattened-scratch-recycle", SOURCE)
        self.assertIn("ExecutorProgramFlattenedScratchRecycleEnabled", SOURCE)

    def test_recycle_uses_exact_vector_swap(self):
        self.assertIn("selected.flattened_ud_buf.swap(info.flattened_ud_buf)", HEADER)
        self.assertIn(
            "selected.flattened_ud_buf = std::move(info.flattened_ud_buf)", HEADER
        )

    def test_all_get_program_activation_paths_receive_the_gate(self):
        self.assertGreaterEqual(
            SOURCE.count("program_flattened_scratch_recycle_enabled"), 8
        )
        self.assertIn("ExecutorRecordProgramFlattenedScratchRecycle", SOURCE)

    def test_runtime_proof_keeps_correctness_contract_explicit(self):
        self.assertIn("payloadUnchanged=1", SOURCE)
        self.assertIn("allocationRecycled=1", SOURCE)
        self.assertIn("proofsUnchanged=1", SOURCE)


if __name__ == "__main__":
    unittest.main()
