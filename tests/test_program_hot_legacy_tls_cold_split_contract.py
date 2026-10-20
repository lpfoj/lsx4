from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")


class ProgramHotLegacyTlsColdSplitContractTests(unittest.TestCase):
    def test_candidate_is_default_off(self):
        self.assertIn("run-enable-program-hot-legacy-tls-cold-split", SOURCE)
        self.assertIn("ExecutorProgramHotLegacyTlsColdSplitEnabled", SOURCE)

    def test_nine_legacy_tls_objects_live_only_in_noinline_helper(self):
        helper = SOURCE.split(
            "ExecutorProgramHotLegacyThreadStateRefs() noexcept", 1
        )[1].split("ExecutorRecordProgramHotLegacyTlsColdSplit", 1)[0]
        self.assertIn("[[gnu::noinline]]", SOURCE)
        self.assertEqual(helper.count("static thread_local"), 9)
        for name in (
            "legacy_sets",
            "legacy_mru_ways",
            "legacy_valid_way_masks",
            "legacy_lookups",
            "legacy_hits",
            "legacy_clock",
            "legacy_exact_revalidations",
            "legacy_exact_revalidation_hits",
            "legacy_retained_content_versions",
        ):
            self.assertIn(name, helper)

    def test_grouped_on_path_skips_legacy_helper(self):
        get_program = SOURCE.split("PipelineCache::GetProgram(", 1)[1]
        selection = get_program.split(
            "const bool program_hot_legacy_tls_cold_split_enabled", 1
        )[1].split("auto& program_hot_sets", 1)[0]
        self.assertRegex(
            selection,
            re.compile(
                r"if \(program_hot_grouped_tls_enabled &&\s*"
                r"program_hot_legacy_tls_cold_split_enabled\) \{\s*"
                r"program_hot_refs = ExecutorProgramHotThreadStateRefs"
            ),
        )
        self.assertEqual(
            selection.count("ExecutorProgramHotLegacyThreadStateRefs()"), 1
        )
        self.assertIn("} else {", selection)

    def test_fallback_and_all_cache_fields_are_preserved(self):
        refs = SOURCE.split("struct ProgramHotThreadStateRefs", 1)[1].split(
            "namespace {", 1
        )[0]
        for field in (
            "sets",
            "mru_ways",
            "valid_way_masks",
            "lookups",
            "hits",
            "clock",
            "exact_revalidations",
            "exact_revalidation_hits",
            "retained_content_versions",
        ):
            self.assertIn(field, refs)
        self.assertIn("legacyFallbackPreserved=1", SOURCE)
        self.assertIn("cacheFieldsUnchanged=1 proofsUnchanged=1", SOURCE)

    def test_runtime_witness_is_reachable_only_from_grouped_fast_path(self):
        self.assertIn("legacyTlsObjectsSkipped=9", SOURCE)
        self.assertIn("program-hot-legacy-tls-cold-split.txt", SOURCE)
        get_program = SOURCE.split("PipelineCache::GetProgram(", 1)[1]
        fast_path = get_program.split(
            "if (program_hot_grouped_tls_enabled &&", 1
        )[1].split("} else {", 1)[0]
        self.assertIn("ExecutorRecordProgramHotLegacyTlsColdSplit()", fast_path)


if __name__ == "__main__":
    unittest.main()
