import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCHER = (ROOT / "funnel-arm/src/common/memory_patcher.cpp").read_text(
    encoding="utf-8"
)


class BundledBloodbornePatchNativeContractTests(unittest.TestCase):
    def test_marker_activation_is_limited_to_the_apk_owned_bundle(self):
        body = PATCHER.split("void ApplyPatchesFromXML", 1)[1].split(
            "void OnGameLoaded", 1
        )[0]
        self.assertIn('path.parent_path().filename() == "lsx4-bundled"', body)
        self.assertIn('path.filename() == "Bloodborne.xml"', body)
        self.assertIn('attribute("LSX4Marker")', body)
        self.assertIn("trusted_bundled_patch && !marker.empty()", body)

    def test_content_version_is_accepted_for_updated_eboot_layouts(self):
        body = PATCHER.split("void ApplyPatchesFromXML", 1)[1].split(
            "void OnGameLoaded", 1
        )[0]
        self.assertIn('GetString("APP_VER")', body)
        self.assertIn('GetString("VERSION")', body)
        self.assertIn("metadata_app_version == app_version", body)
        self.assertIn("metadata_app_version == content_version", body)

    def test_bundled_file_is_rejected_for_an_undeclared_title(self):
        body = PATCHER.split("void ApplyPatchesFromXML", 1)[1].split(
            "void OnGameLoaded", 1
        )[0]
        self.assertIn('patchXML.child("TitleID").children("ID")', body)
        self.assertIn("title_id.child_value()", body)
        self.assertIn("== g_game_serial", body)
        self.assertIn("reason=title_mismatch", body)

    def test_native_layer_rejects_conflicting_patch_groups(self):
        body = PATCHER.split("void ApplyPatchesFromXML", 1)[1].split(
            "void OnGameLoaded", 1
        )[0]
        self.assertIn('attribute("LSX4Group")', body)
        self.assertIn("enabled_groups[group] != 1", body)
        self.assertIn("reason=exclusive_group_conflict", body)

    def test_native_layer_rejects_explicit_performance_patch_conflicts(self):
        body = PATCHER.split("void ApplyPatchesFromXML", 1)[1].split(
            "void OnGameLoaded", 1
        )[0]
        self.assertIn('attribute("LSX4Conflicts")', body)
        self.assertIn("enabled_markers.find(candidate)", body)
        self.assertIn("reason=explicit_patch_conflict", body)

    def test_direct_patch_writes_are_bounded_by_the_eboot_image(self):
        body = PATCHER.split("void PatchMemory", 1)[1].split(
            "uintptr_t PatternScan", 1
        )[0]
        self.assertIn("parsed < 0x400000", body)
        self.assertIn("offset > g_eboot_image_size", body)
        self.assertIn("bytePatch.size() > g_eboot_image_size - direct_image_offset", body)
        self.assertIn("[EXECUTOR_PATCH_RANGE_GUARD]", body)

    def test_empty_or_malformed_patch_bytes_are_rejected(self):
        body = PATCHER.split("void PatchMemory", 1)[1].split(
            "uintptr_t PatternScan", 1
        )[0]
        self.assertIn("valueStr.empty() || valueStr.size() % 2 != 0", body)
        self.assertIn("parsed > 0xff", body)


if __name__ == "__main__":
    unittest.main()
