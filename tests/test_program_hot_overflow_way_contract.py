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


class ProgramHotOverflowWayContractTests(unittest.TestCase):
    def test_candidate_is_default_off_and_requires_grouped_tls(self):
        gate = SOURCE.split(
            "bool ExecutorProgramHotOverflowWayEnabled", 1
        )[1].split("bool ExecutorSrtVictimOwnerCountGateEnabled", 1)[0]
        self.assertIn("run-enable-program-hot-overflow-way", gate)
        self.assertIn("ExecutorProgramHotGroupedTlsEnabled()", gate)
        self.assertIn("return selected && grouped;", gate)

    def test_marker_off_keeps_overflow_storage_unallocated(self):
        setup = SOURCE.split(
            "ProgramHotOverflowWay* program_hot_overflow_way", 1
        )[1].split("auto& program_hot_sets", 1)[0]
        self.assertIn("ExecutorProgramHotOverflowWayEnabled()", setup)
        self.assertIn("new (std::nothrow) ProgramHotOverflowWay{}", setup)
        self.assertNotIn("else", setup)

    def test_overflow_entry_uses_the_same_exact_identity_and_source_proofs(self):
        loop = SOURCE.split(
            "for (size_t probe = 0; probe < program_hot_probe_count; ++probe)", 1
        )[1].split("ProgramHotEntry* const program_hot_victim", 1)[0]
        self.assertIn("candidate.owner == this", loop)
        self.assertIn("candidate.program == program.get()", loop)
        self.assertIn("candidate.snapshot_key == snapshot_key", loop)
        self.assertIn("candidate.module_generation == program->module_generation", loop)
        self.assertIn("candidate.runtime_info_identity == runtime_info_identity", loop)
        self.assertIn("candidate.binding_start == binding_start", loop)
        self.assertIn("AreSrtMemoryRangesCpuStableSince", loop)
        self.assertIn("AreSrtMemoryRangesGpuClean", loop)
        self.assertIn("DoSrtSourceBytesMatch", loop)

    def test_overflow_hit_runs_the_existing_activation_and_binding_path(self):
        hit = SOURCE.split("if (program_hot_hit != nullptr)", 1)[1].split(
            "const auto remember_program_hot", 1
        )[0]
        self.assertIn("program->ActivateCachedPermutLease", hit)
        self.assertIn("program->ActivateCachedPermut", hit)
        self.assertIn("program->ActivatePermut", hit)
        self.assertIn("binding = program_hot_hit->binding_end", hit)
        self.assertIn("return program_hot_hit->result", hit)

    def test_base_occupancy_mask_never_shifts_by_overflow_index(self):
        loop = SOURCE.split(
            "for (size_t probe = 0; probe < program_hot_probe_count; ++probe)", 1
        )[1].split("ProgramHotEntry* const program_hot_victim", 1)[0]
        self.assertIn("candidate_way < ProgramHotWayCount", loop)
        remember = SOURCE.split("const auto remember_program_hot", 1)[1].split(
            "#endif", 1
        )[0]
        self.assertIn("program_hot_victim_way < ProgramHotWayCount", remember)

    def test_runtime_witness_names_every_reused_proof(self):
        witness = SOURCE.split("firstOverflowHit=1", 1)[1].split(";", 1)[0]
        for proof in (
            "exactOwner=1",
            "exactProgram=1",
            "exactSnapshot=1",
            "exactModuleGeneration=1",
            "exactRuntimeInfo=1",
            "exactBinding=1",
            "cpuEpochProof=1",
            "gpuCleanProof=1",
            "sourceByteProof=1",
        ):
            self.assertIn(proof, witness)


if __name__ == "__main__":
    unittest.main()
