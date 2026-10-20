import hashlib
import json
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET


REPO_ROOT = Path(__file__).resolve().parents[1]
ASSETS = REPO_ROOT / "android-app/app/src/main/assets"
CATALOG_PATH = ASSETS / "game_patches.json"
PATCH_PATH = ASSETS / "patches/lsx4-bundled/Bloodborne.xml"
FILES_PATH = ASSETS / "patches/lsx4-bundled/files.json"
JAVA_ROOT = REPO_ROOT / "android-app/app/src/main/java/app/lsx4/android"

PINNED_SOURCE_COMMIT = "5a659cc8b259dbcd805c5e5b0b2328e8593158a9"
PINNED_SOURCE_SHA256 = (
    "c67caaca934cc5e85c06e349386bdae78026c46e37b02e6907505ee9ae5334a3"
)

EXPECTED_LABELS = [
    "Performance Patch (perf increase)",
    "Disable DoF",
    "Disable SSAO",
    "Enable Screen Space Reflections (READ NOTE)",
    "50% Text scale",
    "FMOD Crash Fix",
    "Model LOD 2 (Lowest)",
    "Model LOD 1 (Lower)",
    "Model LOD -2 (Highest)",
    "Increased Graphics Heap Sizes",
    "Intel 12th Gen+ SFX workaround",
    "Intel Black Tonemap Fix",
    "Disable HTTP Requests",
    "DS1-like physics",
    "Lower object/corpse physics",
    "30 FPS++",
    "60FPS (no deltatime)",
    "60 FPS++",
    "90 FPS++",
]




EXPECTED_UPSTREAM_METADATA_HASHES = {
    "Performance Patch (perf increase)": "e8a404f1a790a5ab861133f1e76fb93a1b96f65a82915be51afd0ff76e9887c6",
    "Disable DoF": "7139b912344378f1c18af4c7e65be8fc1f31fbb827595895d9487df6e5cbe480",
    "Disable SSAO": "40f34fcb8f710c676819d3960c1092fa456f5457a28563c93a9b4cae01e73587",
    "Enable Screen Space Reflections (READ NOTE)": "7385f890e18f454ff39dda751515689a2de2e19e19bcf4b3981dc091e2bc623c",
    "50% Text scale": "91cd3f460e2405f12de91ac6d5bcf1f3ae96a26dbfe1bc1bdbb48932c98c96f4",
    "FMOD Crash Fix": "f24afface8fd6586939c50d364a82224c99e2cf4c61e74d493253e0561e98745",
    "Model LOD 2 (Lowest)": "b17fcf397122630d1baccdc1c39f89011a15f1ee123e45a43d519d5d3811490c",
    "Model LOD 1 (Lower)": "1c36a7504995c16448f2411c5f3d36374a280c622f477a41a6abf8efefd250a5",
    "Model LOD -2 (Highest)": "c90ca31bc818945c149159bdddd802bca3b3d11629ef2d813a1a02903aab8e27",
    "Increased Graphics Heap Sizes": "47281ae9dfc6b77cc5f8d4805d040dd35aa8f032a3b519518c085a364182b30e",
    "Intel 12th Gen+ SFX workaround": "aa5ebbbd5289df06694f26bc30c93d77ec54f4b452d98556fda0127d57bdb14d",
    "Intel Black Tonemap Fix": "0e230ec082f5a09dade97e3b472f307343f76ae1a325aed78310817c58385d9a",
    "Disable HTTP Requests": "e18c3c91ec2019544c501c7098adf7b3c1bfb4b6bbf947024b9cd3f7338f4175",
    "DS1-like physics": "2ae537c2610aee0451602e2666112d56c50e8749dfea45e1e6b7cf8f77dab1ad",
    "Lower object/corpse physics": "1cef1a3ccb74e3a21b17ec466ef4179a02bc6deea888541327f2d1066222aebf",
    "30 FPS++": "02b1ea87a2c6d053c57423ecaff44fb790116c844bf00168328b918812c1c39e",
    "60FPS (no deltatime)": "7f31f904c8bc9562361178f666a530d643f4f4d5224e8aecde7c5c0f3ef63af4",
    "60 FPS++": "3027ee3c315ab6a60e5f814e2bff8d5906b396999efcbe1a7da03b7227153d2e",
    "90 FPS++": "dbb056183d3a352e232cbd696d84d6aa28268be6fe43f1400c203a051f1ab9df",
}

EXPECTED_GROUPS = {
    "Model LOD 2 (Lowest)": "model_lod",
    "Model LOD 1 (Lower)": "model_lod",
    "Model LOD -2 (Highest)": "model_lod",
    "DS1-like physics": "physics",
    "Lower object/corpse physics": "physics",
    "30 FPS++": "fps",
    "60FPS (no deltatime)": "fps",
    "60 FPS++": "fps",
    "90 FPS++": "fps",
}

EXPECTED_PERFORMANCE_CONFLICT_IDS = [
    "bloodborne_0109_enable_ssr",
    "bloodborne_0109_model_lod_2",
    "bloodborne_0109_model_lod_1",
    "bloodborne_0109_model_lod_minus_2",
    "bloodborne_0109_30_fps_plus_plus",
    "bloodborne_0109_60_fps_no_deltatime",
    "bloodborne_0109_60_fps_plus_plus",
    "bloodborne_0109_90_fps_plus_plus",
]

EXPECTED_TITLE_IDS = [
    "CUSA00207",
    "CUSA00208",
    "CUSA00299",
    "CUSA00900",
    "CUSA01363",
    "CUSA03014",
    "CUSA03023",
    "CUSA03173",
]


def upstream_payload_digest(metadata):
    upstream_attributes = {
        key: value
        for key, value in metadata.attrib.items()
        if not key.startswith("LSX4")
    }
    canonical = {
        "metadata": dict(sorted(upstream_attributes.items())),
        "lines": [
            dict(sorted(line.attrib.items()))
            for line in metadata.find("PatchList").findall("Line")
        ],
    }
    encoded = json.dumps(
        canonical, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


class AndroidBloodborneBundledPatchContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        cls.game = cls.catalog["games"][0]
        cls.ui_patches = cls.game["patches"]
        cls.bundled = [patch for patch in cls.ui_patches if "marker" in patch]
        cls.xml_text = PATCH_PATH.read_text(encoding="utf-8")
        cls.xml_root = ET.fromstring(cls.xml_text)
        cls.metadata = cls.xml_root.findall("Metadata")

    def test_exact_nineteen_bundled_entries_are_default_off(self):
        self.assertEqual(self.catalog["schemaVersion"], 2)
        self.assertEqual(len(self.ui_patches), 23)
        self.assertEqual([patch["label"] for patch in self.bundled], EXPECTED_LABELS)
        self.assertEqual(len(self.bundled), 19)
        self.assertTrue(all(patch.get("inverted") is False for patch in self.bundled))
        self.assertEqual(
            len({patch["preferenceKey"] for patch in self.ui_patches}),
            len(self.ui_patches),
        )
        self.assertEqual(len({patch["marker"] for patch in self.bundled}), 19)

    def test_native_graphics_toggles_reuse_preferences_without_duplicate_xml(self):
        by_label = {patch.get("label"): patch for patch in self.bundled}
        self.assertEqual(
            (by_label["Disable DoF"]["preferenceKey"], by_label["Disable DoF"]["marker"]),
            ("disable_depth_of_field", "run-disable-dof"),
        )
        self.assertEqual(
            (
                by_label["Disable SSAO"]["preferenceKey"],
                by_label["Disable SSAO"]["marker"],
            ),
            ("disable_ssao", "run-disable-ssao"),
        )
        self.assertNotIn("ssao", {patch["id"] for patch in self.ui_patches})
        self.assertNotIn("depth_of_field", {patch["id"] for patch in self.ui_patches})
        legacy = {patch["id"]: patch for patch in self.ui_patches if "marker" not in patch}
        self.assertEqual(
            set(legacy),
            {
                "dynamic_shadows",
                "motion_blur",
                "bloodborne_0109_disable_chromatic_aberration",
                "bloodborne_0109_disable_aa",
            },
        )
        self.assertTrue(legacy["dynamic_shadows"]["inverted"])
        self.assertTrue(legacy["motion_blur"]["inverted"])
        self.assertEqual(
            legacy["bloodborne_0109_disable_chromatic_aberration"],
            {
                "id": "bloodborne_0109_disable_chromatic_aberration",
                "label": "Disable Chromatic Aberration",
                "preferenceKey": "disable_chromatic_aberration",
                "inverted": False,
            },
        )
        self.assertEqual(
            legacy["bloodborne_0109_disable_aa"],
            {
                "id": "bloodborne_0109_disable_aa",
                "label": "Disable AA",
                "preferenceKey": "disable_anti_aliasing",
                "inverted": False,
            },
        )

    def test_exclusive_groups_match_ui_and_xml(self):
        catalog_groups = {
            patch["label"]: patch.get("group")
            for patch in self.bundled
            if patch.get("group")
        }
        xml_groups = {
            node.get("Name"): node.get("LSX4Group")
            for node in self.metadata
            if node.get("LSX4Group")
        }
        self.assertEqual(catalog_groups, EXPECTED_GROUPS)
        self.assertEqual(xml_groups, EXPECTED_GROUPS)
        self.assertEqual(
            {group: list(catalog_groups.values()).count(group) for group in set(catalog_groups.values())},
            {"model_lod": 3, "physics": 2, "fps": 4},
        )

    def test_performance_patch_conflicts_are_symmetric_and_native_enforced(self):
        by_id = {patch["id"]: patch for patch in self.ui_patches}
        performance = by_id["bloodborne_0109_performance_patch"]
        self.assertEqual(performance["conflicts"], EXPECTED_PERFORMANCE_CONFLICT_IDS)
        marker_by_id = {patch["id"]: patch.get("marker", "") for patch in self.ui_patches}
        performance_xml = self.metadata[0]
        self.assertEqual(
            performance_xml.get("LSX4Conflicts").split(","),
            [marker_by_id[patch_id] for patch_id in EXPECTED_PERFORMANCE_CONFLICT_IDS],
        )

        catalog_java = (JAVA_ROOT / "GamePatchCatalog.java").read_text(encoding="utf-8")
        settings_java = (JAVA_ROOT / "SettingsActivity.java").read_text(encoding="utf-8")
        self.assertIn("boolean conflictsWith(Patch peer)", catalog_java)
        self.assertIn("conflicts.contains(peer.id) || peer.conflicts.contains(id)", catalog_java)
        self.assertGreaterEqual(settings_java.count("patchesConflict(patch, peer)"), 2)
        self.assertIn("patchesConflict(patch, selected)", settings_java)

    def test_bundled_xml_is_exact_pinned_payload_subset(self):
        self.assertIn(PINNED_SOURCE_COMMIT, self.xml_text)
        self.assertEqual([node.get("Name") for node in self.metadata], EXPECTED_LABELS)
        self.assertEqual(len(self.metadata), 19)
        self.assertTrue(all(node.get("AppVer") == "01.09" for node in self.metadata))
        self.assertEqual(
            sum(len(node.find("PatchList").findall("Line")) for node in self.metadata),
            900,
        )
        actual_hashes = {
            node.get("Name"): upstream_payload_digest(node) for node in self.metadata
        }
        self.assertEqual(actual_hashes, EXPECTED_UPSTREAM_METADATA_HASHES)

    def test_performance_bundle_precedes_focused_overrides(self):
        names = [node.get("Name") for node in self.metadata]
        self.assertEqual(names[0], "Performance Patch (perf increase)")
        for focused in (
            "Enable Screen Space Reflections (READ NOTE)",
            "Model LOD 2 (Lowest)",
            "Model LOD 1 (Lower)",
            "Model LOD -2 (Highest)",
            "30 FPS++",
            "60FPS (no deltatime)",
            "60 FPS++",
            "90 FPS++",
        ):
            self.assertGreater(names.index(focused), names.index(names[0]))

    def test_catalog_xml_and_files_manifest_are_one_to_one(self):
        xml_markers = {
            node.get("Name"): node.get("LSX4Marker") for node in self.metadata
        }
        catalog_markers = {
            patch["label"]: patch["marker"] for patch in self.bundled
        }
        self.assertEqual(xml_markers, catalog_markers)
        xml_ids = [node.text for node in self.xml_root.find("TitleID").findall("ID")]
        files_manifest = json.loads(FILES_PATH.read_text(encoding="utf-8"))
        self.assertEqual(self.game["titleIds"], EXPECTED_TITLE_IDS)
        self.assertEqual(xml_ids, EXPECTED_TITLE_IDS)
        self.assertEqual(files_manifest, {"Bloodborne.xml": EXPECTED_TITLE_IDS})

    def test_android_ui_cross_process_and_atomic_materialization_contract(self):
        catalog_java = (JAVA_ROOT / "GamePatchCatalog.java").read_text(encoding="utf-8")
        settings_java = (JAVA_ROOT / "SettingsActivity.java").read_text(encoding="utf-8")
        main_java = (JAVA_ROOT / "MainActivity.java").read_text(encoding="utf-8")

        for field in (
            "final String label;",
            "final String marker;",
            "final String group;",
            "final List<String> conflicts;",
        ):
            self.assertIn(field, catalog_java)
        self.assertIn("left.group.equals(right.group)", settings_java)
        self.assertIn("MainActivity.materializeBundledGamePatches(this)", settings_java)
        self.assertIn("if (!editor.commit())", settings_java)

        self.assertIn('"native/patches/lsx4-bundled"', main_java)
        self.assertIn("copyAssetAtomically(context", main_java)
        self.assertIn("File.createTempFile", main_java)
        self.assertIn("StandardCopyOption.ATOMIC_MOVE", main_java)
        self.assertIn("output.getFD().sync()", main_java)
        self.assertIn("for (String key : crossProcessSettingKeys())", main_java)
        self.assertGreaterEqual(
            main_java.count("materializeBundledGamePatches(this);"), 3
        )

    def test_materializer_resolves_all_conflicts_in_catalog_order(self):
        main_java = (JAVA_ROOT / "MainActivity.java").read_text(encoding="utf-8")
        method = main_java.split(
            "static void materializeBundledGamePatches(Context context) {", 1
        )[1].split("private static void copyAssetAtomically(", 1)[0]

        self.assertIn(
            "List<GamePatchCatalog.Patch> activePatches = new ArrayList<>();",
            method,
        )
        self.assertIn("activeGroups.contains(patch.group)", method)
        self.assertIn("patch.conflictsWith(active)", method)
        self.assertIn("activePatches.add(patch)", method)
        self.assertLess(
            method.index("activePatches.add(patch)"),
            method.index("cleanup.putBoolean(patch.preferenceKey, patch.inverted)"),
        )
        self.assertIn('"explicit=" + explicitConflict.id', method)


if __name__ == "__main__":
    unittest.main()
