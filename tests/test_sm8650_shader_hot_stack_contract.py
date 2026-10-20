from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PIPELINE_CACHE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")


class Sm8650ShaderHotStackContract(unittest.TestCase):
    def test_a840_default_is_structural_while_payload_reuse_remains_a750_only(self):
        constructor = PIPELINE_CACHE[
            PIPELINE_CACHE.index("PipelineCache::PipelineCache(") :
            PIPELINE_CACHE.index("const char* const hle_clean_buffer_guard_flag")
        ]
        self.assertIn("capability_profile.exact_mesa_turnip_a750_d9_lineage", constructor)
        self.assertIn("capability_profile.exact_mesa_turnip_adreno840", constructor)
        self.assertIn(
            "exact_snapdragon_turnip_structural =\n"
            "        exact_sm8650_turnip_d9 || exact_sm8850_turnip",
            constructor,
        )
        self.assertIn(
            "stable_srt_epoch_hit_requested || exact_sm8650_turnip_d9",
            constructor,
        )
        self.assertIn(
            "program_hot_exact_multiversion_requested || exact_sm8650_turnip_d9",
            constructor,
        )
        self.assertIn(
            "program_hot_mru_probe_requested || exact_snapdragon_turnip_structural",
            constructor,
        )
        self.assertNotIn("exact_proprietary_adreno750", constructor)

    def test_all_four_routes_retain_independent_manual_markers(self):
        constructor = PIPELINE_CACHE[
            PIPELINE_CACHE.index("PipelineCache::PipelineCache(") :
            PIPELINE_CACHE.index("const char* const hle_clean_buffer_guard_flag")
        ]
        for marker in (
            "run-enable-shader-signature-word-prefilter",
            "run-enable-stable-srt-epoch-hit",
            "run-enable-program-hot-exact-multiversion",
            "run-enable-program-hot-mru-probe",
        ):
            self.assertIn(marker, constructor)
        self.assertEqual(constructor.count("|| exact_snapdragon_turnip_structural"), 2)
        self.assertEqual(constructor.count("|| exact_sm8650_turnip_d9"), 2)
        self.assertIn("preserve_strong_snapdragon_bundle", constructor)

    def test_signature_prefilter_keeps_full_tail_comparison(self):
        regs_shader = (
            ROOT
            / "funnel-arm"
            / "src"
            / "video_core"
            / "amdgpu"
            / "regs_shader.h"
        ).read_text(encoding="utf-8")
        prefilter_start = regs_shader.index("if (word_signature_prefilter)")
        prefilter_end = regs_shader.index(
            "\n    for (size_t offset = 0; offset < ShaderBinarySearchBytes;",
            prefilter_start,
        )
        prefilter = regs_shader[prefilter_start:prefilter_end]
        self.assertIn("candidate_prefix != signature_prefix", prefilter)
        self.assertIn("std::memcmp", prefilter)
        self.assertIn("BinaryInfo::signature_ref.size() - sizeof(u32)", prefilter)

    def test_exact_content_and_epoch_proofs_are_unchanged(self):
        self.assertIn(
            "CanUseStableSrtEpochHit(stable_srt_epoch_hit_enabled,",
            PIPELINE_CACHE,
        )
        self.assertIn(
            "CanUseProgramHotExactRevalidation(\n"
            "                       program_hot_exact_multiversion_enabled, identity,",
            PIPELINE_CACHE,
        )
        self.assertIn("publish_sequence == validation_sequence", PIPELINE_CACHE)
        self.assertIn("AreSrtMemoryRangesCpuStableSince(", PIPELINE_CACHE)
        self.assertIn("AreSrtMemoryRangesGpuClean(candidate.srt_ranges)", PIPELINE_CACHE)
        self.assertIn("DoSrtSourceBytesMatch(candidate.srt_ranges,", PIPELINE_CACHE)

    def test_mru_changes_probe_order_but_not_candidate_proofs(self):
        self.assertIn("program_hot_probe_order[program_hot_probe_count++]", PIPELINE_CACHE)
        self.assertIn("candidate.owner == this", PIPELINE_CACHE)
        self.assertIn("candidate.program == program.get()", PIPELINE_CACHE)
        self.assertIn("candidate.snapshot_key == snapshot_key", PIPELINE_CACHE)
        self.assertIn(
            "candidate.module_generation == program->module_generation", PIPELINE_CACHE
        )
        self.assertIn("candidate.runtime_info_identity == runtime_info_identity", PIPELINE_CACHE)
        self.assertIn("candidate.binding_start == binding_start", PIPELINE_CACHE)

    def test_runtime_marker_records_selected_exact_stack(self):
        self.assertIn("[EXECUTOR_SNAPDRAGON_SHADER_HOT_STACK] enabled=1", PIPELINE_CACHE)
        self.assertIn("exactA750D9=%u exactA840Turnip=%u", PIPELINE_CACHE)
        self.assertIn("signatureWordPrefilter=1", PIPELINE_CACHE)
        self.assertIn("programHotMultiversion=%u", PIPELINE_CACHE)
        self.assertIn("stableSrtEpoch=%u", PIPELINE_CACHE)
        self.assertIn("identityProof=exact generationProof=exact", PIPELINE_CACHE)
        self.assertIn("cpuStableProof=exact", PIPELINE_CACHE)
        self.assertIn("gpuCleanProof=exact", PIPELINE_CACHE)


if __name__ == "__main__":
    unittest.main()
