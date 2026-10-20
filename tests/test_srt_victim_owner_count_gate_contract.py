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


class SrtVictimOwnerCountGateContractTests(unittest.TestCase):
    def test_candidate_is_android_marker_gated_and_default_off_elsewhere(self):
        self.assertIn("run-enable-srt-victim-owner-count-gate", SOURCE)
        self.assertIn("ExecutorSrtVictimOwnerCountGateEnabled", SOURCE)
        non_android = SOURCE.split(
            "constexpr bool ExecutorSrtVictimOwnerCountGateEnabled", 1
        )[1].split("}", 1)[0]
        self.assertIn("return false;", non_android)

    def test_owner_count_is_program_local_and_keys_keep_exact_owner_identity(self):
        program = HEADER.split("struct Program {", 1)[1].split(
            "struct ProgramHotEntry", 1
        )[0]
        self.assertIn("size_t srt_victim_snapshot_count{};", program)
        key = HEADER.split("struct SrtVictimKey {", 1)[1].split(
            "struct SrtVictimKeyHash", 1
        )[0]
        self.assertIn("Program* program{};", key)
        self.assertIn("u64 identity_key{};", key)

    def test_primary_lookup_precedes_the_zero_owner_gate(self):
        get_program = SOURCE.split(
            "std::optional<PipelineCache::Result> PipelineCache::GetProgram", 1
        )[1]
        primary = get_program.index("program->FindSpecializationSnapshot(snapshot_key)")
        gate = get_program.index("program->srt_victim_snapshot_count == 0")
        victim = get_program.index("FindSrtVictimSnapshot(*program, snapshot_key)")
        self.assertLess(primary, gate)
        self.assertLess(gate, victim)
        gate_body = get_program[gate:victim]
        self.assertIn("++srt_snapshot_victim_owner_skips", gate_body)
        self.assertNotIn("source_matches", gate_body)

    def test_exact_count_tracks_insert_evict_and_global_invalidation(self):
        retain = SOURCE.split(
            "PipelineCache::RetainSrtVictimSnapshot", 1
        )[1].split("void PipelineCache::InvalidateSrtSnapshots", 1)[0]
        self.assertIn("--candidate.program->srt_victim_snapshot_count", retain)
        self.assertIn("++program.srt_victim_snapshot_count", retain)
        self.assertLess(
            retain.index("--candidate.program->srt_victim_snapshot_count"),
            retain.index("srt_snapshot_victims.erase(it)"),
        )
        self.assertLess(
            retain.index("srt_snapshot_victim_clock.push_back(key)"),
            retain.index("++program.srt_victim_snapshot_count"),
        )
        invalidate = SOURCE.split(
            "void PipelineCache::InvalidateSrtSnapshots", 1
        )[1].split("bool PipelineCache::WasCpuRangeInvalidatedSinceLocked", 1)[0]
        self.assertIn("srt_snapshot_victims.clear();", invalidate)
        self.assertIn("program->srt_victim_snapshot_count = 0;", invalidate)

    def test_existing_victim_hit_and_validation_paths_are_unchanged(self):
        find = SOURCE.split(
            "PipelineCache::FindSrtVictimSnapshot", 1
        )[1].split("PipelineCache::RetainSrtVictimSnapshot", 1)[0]
        for token in (
            "srt_snapshot_victims.find(key)",
            "!entry.snapshot || !entry.snapshot->valid",
            "entry.referenced = true",
        ):
            self.assertIn(token, find)
        for token in (
            "AreSrtMemoryRangesCpuStableSince",
            "AreSrtMemoryRangesGpuClean",
            "DoSrtSourceBytesMatch",
            "snapshot.runtime_info_identity == runtime_info_identity",
            "snapshot.specialization_user_data == specialization_user_data",
        ):
            self.assertIn(token, SOURCE)


if __name__ == "__main__":
    unittest.main()
