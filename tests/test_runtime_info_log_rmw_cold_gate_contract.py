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


class RuntimeInfoLogRmwColdGateContractTests(unittest.TestCase):
    def test_candidate_is_default_off_marker_gated(self):
        gate = SOURCE.split(
            "bool ExecutorRuntimeInfoLogRmwColdGateEnabled", 1
        )[1].split("}", 1)[0]
        self.assertIn("run-enable-runtime-info-log-rmw-cold-gate", gate)
        self.assertIn("static const bool enabled", gate)

    def test_repeat_hit_load_precedes_the_preserved_exchange(self):
        body = SOURCE.split(
            "static std::atomic<bool> first_runtime_info_identity_hit_logged", 1
        )[1].split("const bool cpu_stable", 1)[0]
        load = body.index(
            "first_runtime_info_identity_hit_logged.load(std::memory_order_relaxed)"
        )
        exchange = body.index(
            "first_runtime_info_identity_hit_logged.exchange("
        )
        self.assertLess(load, exchange)
        self.assertIn("!runtime_info_log_already_published &&", body)
        self.assertEqual(body.count("first_runtime_info_identity_hit_logged.exchange("), 1)

    def test_first_winner_and_exact_identity_proof_remain(self):
        get_program = SOURCE.split(
            "std::optional<PipelineCache::Result> PipelineCache::GetProgram", 1
        )[1]
        for token in (
            "candidate.owner == this",
            "candidate.program == program.get()",
            "candidate.snapshot_key == snapshot_key",
            "candidate.module_generation == program->module_generation",
            "candidate.runtime_info_identity == runtime_info_identity",
            "candidate.binding_start == binding_start",
        ):
            self.assertIn(token, get_program)
        self.assertIn("[EXECUTOR_RUNTIME_INFO_IDENTITY] firstHit=1", get_program)
        self.assertIn("runtime-info-identity-hit.txt", get_program)
        self.assertIn("coldGate=%u repeatRmwSkipped=%u", get_program)

    def test_same_gate_cold_splits_the_second_get_program_proof_rmw(self):
        mru = SOURCE.split(
            "static std::atomic<bool> first_reordered_hit_logged", 1
        )[1].split("program_hot_mru_ways[program_hot_set_index]", 1)[0]
        load = mru.index("first_reordered_hit_logged.load(std::memory_order_relaxed)")
        exchange = mru.index("first_reordered_hit_logged.exchange(")
        self.assertLess(load, exchange)
        self.assertIn("!reordered_log_already_published &&", mru)
        self.assertIn("[EXECUTOR_PROGRAM_HOT_MRU] firstReorderedHit=1", mru)
        self.assertIn("coldGate=%u", mru)
        self.assertIn("repeatRmwSkipped=%u", mru)

    def test_candidate_does_not_change_cache_or_srt_validation(self):
        gate_body = SOURCE.split(
            "const bool runtime_info_log_rmw_cold_gate_enabled", 1
        )[1].split("const bool cpu_stable", 1)[0]
        self.assertNotIn("candidate.valid =", gate_body)
        self.assertNotIn("source_matches =", gate_body)
        for token in (
            "AreSrtMemoryRangesCpuStableSince",
            "AreSrtMemoryRangesGpuClean",
            "DoSrtSourceBytesMatch",
            "CaptureCpuInvalidationSequence",
        ):
            self.assertIn(token, SOURCE)


if __name__ == "__main__":
    unittest.main()
