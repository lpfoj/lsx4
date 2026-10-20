import importlib.util
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "bloodborne-proof" / "capture_provenance.py"
SPEC = importlib.util.spec_from_file_location("bloodborne_adb_shell", MODULE_PATH)
provenance = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = provenance
SPEC.loader.exec_module(provenance)


class BloodborneAdbShellQuotingTests(unittest.TestCase):
    def test_entire_script_is_one_outer_shell_quoted_argument(self):
        script = "for p in /sys/policy*; do printf '%s\\n' \"$p\"; done"
        argv = provenance.adb_remote_shell_command(["shell"], script)
        self.assertEqual(argv[:3], ["shell", "sh", "-c"])
        self.assertEqual(len(argv), 4)
        self.assertTrue(argv[3].startswith("'") and argv[3].endswith("'"))
        self.assertIn("'\\''%s\\n'\\''", argv[3])

    def test_run_as_prefix_is_preserved(self):
        argv = provenance.adb_remote_shell_command(
            ["exec-out", "run-as", "app.lsx4.android"], "printf ok"
        )
        self.assertEqual(argv[:-3], ["exec-out", "run-as", "app.lsx4.android"])
        self.assertEqual(argv[-3:-1], ["sh", "-c"])
        self.assertEqual(argv[-1], "printf ok")

    def test_empty_or_nul_script_is_rejected(self):
        for script in ("", "printf ok\x00printf bad"):
            with self.subTest(script=repr(script)), self.assertRaisesRegex(
                provenance.CaptureRejected, "remote-shell-script-invalid"
            ):
                provenance.adb_remote_shell_command(["shell"], script)

    def test_unknown_remote_transport_is_rejected(self):
        with self.assertRaisesRegex(
            provenance.CaptureRejected, "remote-shell-prefix-invalid"
        ):
            provenance.adb_remote_shell_command(["push"], "printf ok")

    def test_gpu_frequency_unavailability_is_explicit_not_a_proxy(self):
        source = pathlib.Path(MODULE_PATH).read_text(encoding="utf-8")
        self.assertIn(
            "source=kgsl-sysfs status=unavailable "
            "reason=no-readable-frequency-node",
            source,
        )
        self.assertNotIn("dumpsys gpu |", source)

    def test_candidate_does_not_bypass_the_quoting_helper(self):
        source = (
            ROOT / "tools" / "bloodborne-proof" /
            "generate_expected_state_candidate.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn('"run-as", package_name, "sh", "-c"', source)
        self.assertGreaterEqual(source.count("provenance.adb_remote_shell_command"), 2)

    def test_exact_device_inventory_has_a_bounded_non_generic_timeout(self):
        source = pathlib.Path(MODULE_PATH).read_text(encoding="utf-8")
        inventory_body = source.split("def capture_inventory_set", 1)[1].split(
            "def collect", 1
        )[0]
        self.assertIn("timeout=300", inventory_body)


class BloodbornePackageUidTests(unittest.TestCase):
    def test_android_16_app_id_is_the_package_uid(self):
        dump = (
            "Package [app.lsx4.android] (18ce75f):\n"
            "  appId=10358\n"
            "  installerPackageUid=-1\n"
        )
        self.assertEqual(provenance.parse_package_uid(dump), 10358)

    def test_old_user_id_and_new_app_id_must_agree(self):
        self.assertEqual(
            provenance.parse_package_uid("userId=10358\nappId=10358\n"),
            10358,
        )
        with self.assertRaisesRegex(
            provenance.CaptureRejected, "package-uid-missing-or-ambiguous"
        ):
            provenance.parse_package_uid("userId=10358\nappId=10359\n")

    def test_unrelated_uid_fields_are_not_accepted(self):
        with self.assertRaisesRegex(
            provenance.CaptureRejected, "package-uid-missing-or-ambiguous"
        ):
            provenance.parse_package_uid("installerPackageUid=10358\n")


class BloodborneRemoteInventoryTests(unittest.TestCase):
    def test_valid_directory_and_file_lines_parse_without_key_error(self):
        digest = "a" * 64
        result = provenance.parse_remote_inventory(
            f"D\tfiles/root\nF\t3\t{digest}\tfiles/root/item\n",
            ["files/root"],
        )
        self.assertEqual(len(result["entries"]), 2)

    def test_unknown_line_is_a_capture_rejection_not_key_error(self):
        with self.assertRaisesRegex(
            provenance.CaptureRejected, "inventory-line-invalid"
        ):
            provenance.parse_remote_inventory("shell diagnostic\n", ["files/root"])


class BloodborneApkSignerTests(unittest.TestCase):
    DIGEST_A = "a" * 64
    DIGEST_B = "b" * 64

    def test_current_v3_apksigner_format_is_accepted(self):
        text = f"V3.0 Signer: certificate SHA-256 digest: {self.DIGEST_A}\n"
        self.assertEqual(provenance.parse_apksigner(text), self.DIGEST_A)

    def test_same_certificate_repeated_by_signing_schemes_is_one_identity(self):
        text = (
            f"Signer #1 certificate SHA-256 digest: {self.DIGEST_A}\n"
            f"V3.0 Signer: certificate SHA-256 digest: {self.DIGEST_A}\n"
        )
        self.assertEqual(provenance.parse_apksigner(text), self.DIGEST_A)

    def test_distinct_signers_are_rejected(self):
        text = (
            f"Signer #1 certificate SHA-256 digest: {self.DIGEST_A}\n"
            f"V3.0 Signer: certificate SHA-256 digest: {self.DIGEST_B}\n"
        )
        with self.assertRaisesRegex(
            provenance.CaptureRejected, "apk-signer-digest-unavailable"
        ):
            provenance.parse_apksigner(text)


if __name__ == "__main__":
    unittest.main()
