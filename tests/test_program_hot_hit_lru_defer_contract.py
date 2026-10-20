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


class ProgramHotHitLruDeferContractTests(unittest.TestCase):
    def test_candidate_is_default_off_and_emits_selection_proof(self):
        gate = SOURCE.split(
            "bool ExecutorProgramHotHitLruDeferEnabled", 1
        )[1].split("bool ExecutorSrtVictimOwnerCountGateEnabled", 1)[0]
        self.assertIn("run-enable-program-hot-hit-lru-defer", gate)
        self.assertIn("static const bool enabled", gate)
        self.assertIn("selected=1 hitVictimUnused=1 missLruExact=1", gate)

    def test_old_lru_position_is_preserved_when_candidate_is_off(self):
        loop = SOURCE.split(
            "for (size_t probe = 0; probe < program_hot_probe_count; ++probe)", 1
        )[1].split("ProgramHotEntry* const program_hot_victim", 1)[0]
        legacy = loop.split(
            "if (!program_hot_hit_lru_defer_enabled)", 1
        )[1].split("const bool identity", 1)[0]
        self.assertIn("consider_program_hot_lru();", legacy)
        self.assertLess(
            loop.index("if (!program_hot_hit_lru_defer_enabled)"),
            loop.index("const bool identity"),
        )

    def test_identity_miss_still_updates_lru_before_continue(self):
        loop = SOURCE.split(
            "for (size_t probe = 0; probe < program_hot_probe_count; ++probe)", 1
        )[1].split("ProgramHotEntry* const program_hot_victim", 1)[0]
        miss = loop.split("if (!identity)", 1)[1].split(
            "static std::atomic<bool> first_runtime_info_identity_hit_logged", 1
        )[0]
        self.assertIn("if (program_hot_hit_lru_defer_enabled)", miss)
        self.assertLess(miss.index("consider_program_hot_lru();"), miss.index("continue;"))

    def test_source_hit_breaks_before_deferred_lru_work(self):
        loop = SOURCE.split(
            "for (size_t probe = 0; probe < program_hot_probe_count; ++probe)", 1
        )[1].split("ProgramHotEntry* const program_hot_victim", 1)[0]
        hit = loop.rsplit("if (source_matches)", 1)[1]
        self.assertLess(hit.index("break;"), hit.index("consider_program_hot_lru();"))

    def test_source_miss_updates_lru_before_replacement_policy(self):
        loop = SOURCE.split(
            "for (size_t probe = 0; probe < program_hot_probe_count; ++probe)", 1
        )[1].split("ProgramHotEntry* const program_hot_victim", 1)[0]
        tail = loop.rsplit("if (source_matches)", 1)[1]
        self.assertLess(
            tail.index("consider_program_hot_lru();"),
            tail.index("if (!program_hot_exact_multiversion_enabled)"),
        )

    def test_victim_selection_contract_is_unchanged(self):
        victim = SOURCE.split(
            "ProgramHotEntry* const program_hot_victim", 1
        )[1].split("program_hot_srt_profile.Stop();", 1)[0]
        self.assertIn(
            "program_hot_invalid != nullptr ? program_hot_invalid : program_hot_lru",
            victim,
        )


if __name__ == "__main__":
    unittest.main()
