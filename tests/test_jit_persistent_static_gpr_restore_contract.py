import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CORE = (
    ROOT / "src/executor/dynamic_translation/retiring_execution_core.cpp"
).read_text(encoding="utf-8")


class JitPersistentStaticGprRestoreContractTest(unittest.TestCase):
    def test_restore_accepts_only_explicit_static_gpr_metadata(self):
        restore = CORE.split(
            "bool NativeSegmentDepot::RestorePersistentNativeBlock(", 1
        )[1].split(
            "Arm64BlockEntry NativeSegmentDepot::EmitReturnImmediateBlock", 1
        )[0]

        for token in (
            "static_gpr_segment_index >= segments.size()",
            "static_gpr_entry_offset >=",
            "(static_gpr_entry_offset & 3u) != 0",
            "committed_segments[static_gpr_segment_index] +",
            "static_gpr_entry_offset",
        ):
            self.assertIn(token, restore)

        for forbidden in (
            "effective_static_gpr_segment_index",
            "effective_static_gpr_entry_offset",
            "kMovX21Zero",
            "alternate_first",
        ):
            self.assertNotIn(forbidden, restore)

    def test_missing_static_entry_uses_publication_adapter(self):
        publish = CORE.split("void PublishEdgeEntryVariants(", 1)[1].split(
            "enum class GuestByteMatchResult", 1
        )[0]
        direct = publish.index("destination->static_gpr_entry != nullptr")
        fallback = publish.index("destination->resident_entry != nullptr", direct)
        adapter = publish.index("&Lsx4JitStaticEdgeToResident", fallback)
        publication = publish.index(
            "edge.static_source_patch_target.store(patch_target", adapter
        )
        self.assertLess(direct, fallback)
        self.assertLess(fallback, adapter)
        self.assertLess(adapter, publication)


if __name__ == "__main__":
    unittest.main()
