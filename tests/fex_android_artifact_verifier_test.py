from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
VERIFIER_PATH = REPO_ROOT / "tools" / "fex" / "verify_android_fex_artifacts.py"
SPEC = importlib.util.spec_from_file_location("verify_android_fex_artifacts", VERIFIER_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFIER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFIER
SPEC.loader.exec_module(VERIFIER)


class FexAndroidArtifactVerifierTest(unittest.TestCase):
    def test_parses_elf_header_and_dynamic_contract(self) -> None:
        header = VERIFIER.parse_elf_header(
            """
              Class:                             ELF64
              Data:                              2's complement, little endian
              Type:                              DYN (Shared object file)
              Machine:                           AArch64
            """
        )
        self.assertEqual(header["class"], "ELF64")
        self.assertEqual(header["machine"], "AArch64")
        needed, soname = VERIFIER.parse_dynamic(
            """
              0x0000000000000001 (NEEDED) Shared library: [libFEXCore.so]
              0x0000000000000001 (NEEDED) Shared library: [libc.so]
              0x000000000000000e (SONAME) Library soname: [libbridge.so]
            """
        )
        self.assertEqual(needed, ["libFEXCore.so", "libc.so"])
        self.assertEqual(soname, "libbridge.so")

    def test_parses_android_api_build_id_and_symbol_versions(self) -> None:
        notes = """
          Android 0x00000084 NT_ANDROID_TYPE_IDENT
           description data: 1a 00 00 00 72 32 39 00
          GNU 0x00000014 NT_GNU_BUILD_ID
            Build ID: 0123456789ABCDEF
        """
        self.assertEqual(VERIFIER.parse_android_api_note(notes), 26)
        self.assertEqual(VERIFIER.parse_build_id(notes), "0123456789abcdef")
        defined, undefined, weak = VERIFIER.parse_nm(
            """
            0000000000000010 T exported@@LIB
                             U required@LIBC
                             w optional@LIBC
            """
        )
        self.assertEqual(defined, {"exported"})
        self.assertEqual(undefined, {"required"})
        self.assertEqual(weak, {"optional"})

    def test_source_tree_fingerprint_is_sorted_and_content_sensitive(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "z.cpp").write_text("int z;\n", encoding="utf-8")
            (root / "a.h").write_text("#pragma once\n", encoding="utf-8")
            (root / "ignored.md").write_text("ignored\n", encoding="utf-8")
            first = VERIFIER.source_tree_state(root)
            second = VERIFIER.source_tree_state(root)
            self.assertEqual(first["file_count"], 2)
            self.assertEqual(first["sha256"], second["sha256"])
            (root / "z.cpp").write_text("int z = 1;\n", encoding="utf-8")
            changed = VERIFIER.source_tree_state(root)
            self.assertNotEqual(first["sha256"], changed["sha256"])
            single = VERIFIER.source_tree_state(root / "a.h")
            self.assertEqual(single["file_count"], 1)

    def test_manifest_comparison_ignores_only_timestamps(self) -> None:
        first = {
            "artifacts": {"fexcore": {"sha256": "a", "mtime_ns": 1}},
            "android_smokes": [{"sha256": "b", "mtime_ns": 2}],
            "sources": [{"sha256": "c", "newest_mtime_ns": 3}],
        }
        second = {
            "artifacts": {"fexcore": {"sha256": "a", "mtime_ns": 10}},
            "android_smokes": [{"sha256": "b", "mtime_ns": 20}],
            "sources": [{"sha256": "c", "newest_mtime_ns": 30}],
        }
        self.assertEqual(
            VERIFIER._manifest_comparable(first),
            VERIFIER._manifest_comparable(second),
        )
        second["artifacts"]["fexcore"]["sha256"] = "different"
        self.assertNotEqual(
            VERIFIER._manifest_comparable(first),
            VERIFIER._manifest_comparable(second),
        )

    def test_packaged_libcxx_is_checked_as_the_actual_symbol_provider(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            runtime = Path(temporary) / "libc++_shared.so"
            runtime.write_bytes(b"runtime")
            manifest = {
                "target": {"android_api": 26},
                "toolchain": {"ndk_root": temporary},
                "artifacts": {"fexcore": {"path": str(runtime)}},
            }
            with (
                mock.patch.object(VERIFIER, "discover_toolchain", return_value=object()),
                mock.patch.object(
                    VERIFIER,
                    "inspect_elf",
                    return_value={"soname": "libc++_shared.so"},
                ),
                mock.patch.object(VERIFIER, "verify_symbol_closure") as closure,
            ):
                VERIFIER.verify_packaged_libcxx(manifest, runtime)
            closure.assert_called_once()

    def test_packaged_artifact_copy_must_match_attested_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            library = directory / "libFEXCore.so"
            library.write_bytes(b"fex-core")
            manifest = {
                "artifacts": {
                    "fexcore": {
                        "path": str(library),
                        "size": library.stat().st_size,
                        "sha256": VERIFIER.sha256_file(library),
                    }
                }
            }
            VERIFIER.verify_packaged_artifact_copies(manifest, directory)
            library.write_bytes(b"tampered")
            with self.assertRaises(VERIFIER.VerificationError):
                VERIFIER.verify_packaged_artifact_copies(manifest, directory)

    def test_source_snapshot_rejects_mid_build_edits(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "input.cpp"
            source.write_text("int value;\n", encoding="utf-8")
            snapshot_path = root / "snapshot.json"
            snapshot = VERIFIER.create_source_snapshot([("input", source, False)])
            VERIFIER._write_json(snapshot_path, snapshot)
            VERIFIER.verify_source_snapshot(snapshot_path)
            source.write_text("int value = 1;\n", encoding="utf-8")
            with self.assertRaises(VERIFIER.VerificationError):
                VERIFIER.verify_source_snapshot(snapshot_path)


if __name__ == "__main__":
    unittest.main()
