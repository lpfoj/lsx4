from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "shader_recompiler"
    / "ir"
    / "passes"
    / "srt.h"
).read_text(encoding="utf-8")
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "shader_recompiler"
    / "ir"
    / "passes"
    / "flatten_extended_userdata_pass.cpp"
).read_text(encoding="utf-8")
SERIALIZATION = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_serialization.cpp"
).read_text(encoding="utf-8")


class PortableSrtPrecomputedLayoutContractTests(unittest.TestCase):
    def test_candidate_is_default_off_and_marker_gated(self):
        self.assertIn("run-enable-portable-srt-precomputed-layout", SOURCE)
        self.assertIn("ExecutorPortableSrtPrecomputedLayoutEnabled", SOURCE)
        prepare = SOURCE.split(
            "void PersistentSrtInfo::PreparePortableLayout()", 1
        )[1].split("PFN_SrtWalker RegisterWalkerCode", 1)[0]
        self.assertLess(
            prepare.index("if (!ExecutorPortableSrtPrecomputedLayoutEnabled())"),
            prepare.index("portable_node_span_dw.assign"),
        )

    def test_derived_layout_is_not_added_to_archive_format(self):
        self.assertIn("portable_node_span_dw", HEADER)
        self.assertIn("portable_node_bulk_offsets", HEADER)
        self.assertNotIn("srt.Write(portable_node_span_dw)", SERIALIZATION)
        self.assertNotIn("srt.Write(portable_node_bulk_offsets)", SERIALIZATION)

    def test_layout_is_prepared_for_new_and_deserialized_recipes(self):
        self.assertIn("info.srt_info.PreparePortableLayout();", SOURCE)
        self.assertIn("PreparePortableLayout();", SERIALIZATION)

    def test_legacy_per_draw_validation_and_layout_remain_available(self):
        self.assertIn("if (!use_precomputed_layout)", SOURCE)
        self.assertIn("legacy_node_span_dw", SOURCE)
        self.assertIn("legacy_node_bulk_offsets", SOURCE)

    def test_runtime_memory_semantics_remain_on_shared_path(self):
        self.assertIn("memoryReadsUnchanged=1", SOURCE)
        self.assertEqual(SOURCE.count("memory_reader(pointer_address, &ptr, sizeof(ptr))"), 1)
        self.assertEqual(SOURCE.count("memory_reader(source_address, &v, sizeof(v))"), 1)


if __name__ == "__main__":
    unittest.main()
