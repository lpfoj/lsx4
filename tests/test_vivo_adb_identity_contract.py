import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
INSTALLER = (ROOT / "scripts" / "install-vivo-apk.ps1").read_text(encoding="utf-8")
PMU = (ROOT / "tools" / "codex_bloodborne_pmu.sh").read_text(encoding="utf-8")
HARNESS = (ROOT / "tools" / "codex_bloodborne_hle_clean_ab.sh").read_text(
    encoding="utf-8"
)

EXPECTED = "10AG263F1B001HM"
WIRELESS_ALIAS = "adb-10AG263F1B001HM-2MNzzP._adb-tls-connect._tcp"
WIRELESS_PATTERN = re.compile(
    r"^adb-10AG263F1B001HM-[A-Za-z0-9]+(?: \([0-9]+\))?"
    r"\._adb-tls-connect\._tcp$"
)


class VivoAdbIdentityContractTests(unittest.TestCase):
    def test_only_exact_serial_or_strict_wireless_alias_is_accepted(self):
        self.assertTrue(WIRELESS_PATTERN.fullmatch(WIRELESS_ALIAS))
        for rejected in (
            "emulator-5554",
            "10AG263F1B001HM.evil",
            "adb-10AG263F1B001HM-._adb-tls-connect._tcp",
            "adb-10AG263F1B001HM-2MNzzP._adb-tls-connect._tcp.evil",
            "adb-OTHER-2MNzzP._adb-tls-connect._tcp",
            "adb-10AG263F1B001HM-2MNzzP._adb-tls-pairing._tcp",
            "adb-10AG263F1B001HM-2MNzzP;touch-x._adb-tls-connect._tcp",
        ):
            self.assertFalse(WIRELESS_PATTERN.fullmatch(rejected), rejected)

        powershell_pattern = (
            "^adb-10AG263F1B001HM-[A-Za-z0-9]+(?: \\([0-9]+\\))?"
            "\\._adb-tls-connect\\._tcp$"
        )
        bash_pattern = (
            "^adb-10AG263F1B001HM-[A-Za-z0-9]+[.]_adb-tls-connect[.]_tcp$"
        )
        self.assertIn(powershell_pattern, INSTALLER)
        self.assertIn(bash_pattern, PMU)
        self.assertIn(bash_pattern, HARNESS)

    def test_installer_keeps_public_device_id_exact_and_uses_optional_transport(self):
        for token in (
            "[Parameter(Mandatory = $true)]\n    [string]$DeviceId",
            "$DeviceId -cne $expectedDeviceId",
            "$adbTarget = if ([string]::IsNullOrWhiteSpace($env:ADB_SERIAL))",
            "$DeviceId\n} else {\n    $env:ADB_SERIAL",
            "ADB_SERIAL is not an approved transport",
        ):
            self.assertIn(token, INSTALLER)
        self.assertIn("'-s', \"`\"$adbTarget`\"\", 'install'", INSTALLER)
        self.assertNotIn("'-s', $DeviceId, 'install'", INSTALLER)

    def test_installer_proves_hardware_identity_before_install_or_ui_mutation(self):
        top_level = INSTALLER.split("function Get-NodeByResourceId", 1)[0]
        for token in (
            "Invoke-AdbDeviceAction",
            "'adb-verify-ro-serialno'",
            "@('shell', 'getprop', 'ro.serialno')",
            "$reportedSerial -cne $expectedDeviceId",
            "Dedicated Vivo identity mismatch",
        ):
            self.assertIn(token, top_level)
        self.assertLess(
            INSTALLER.index("$reportedSerial ="),
            INSTALLER.index("try {\n    Assert-AgentChatDeviceLease -Action 'adb-install-apk'"),
        )

        direct_adb = [
            line.strip()
            for line in INSTALLER.splitlines()
            if "& $adb " in line
        ]
        self.assertEqual(
            direct_adb,
            ["$output = @(& $adb -s $adbTarget @Arguments 2>$null)"],
        )
        wrapper = INSTALLER.split("function Invoke-AdbDeviceAction", 1)[1].split(
            "$reportedSerial =", 1
        )[0]
        self.assertLess(
            wrapper.index("Assert-AgentChatDeviceLease"), wrapper.index("& $adb -s")
        )
        install = INSTALLER.split("try {", 1)[1].split("$deadline =", 1)[0]
        self.assertLess(
            install.index("Assert-AgentChatDeviceLease"),
            install.index("Start-Process -FilePath $adb"),
        )

    def test_agentchat_event_then_ownership_guards_every_raw_adb_route(self):
        installer_guard = INSTALLER.split("function Assert-AgentChatDeviceLease", 1)[
            1
        ].split("function Invoke-AdbDeviceAction", 1)[0]
        self.assertIn("agentchat.py", INSTALLER)
        self.assertLess(installer_guard.index(" event "), installer_guard.index(" own "))
        self.assertIn("--target $agentTarget", installer_guard)
        self.assertIn("--area $agentTarget", installer_guard)
        self.assertIn('$agentAdbTarget = "adb:$expectedDeviceId"', INSTALLER)
        self.assertIn("--area $agentAdbTarget", installer_guard)

        for source in (PMU, HARNESS):
            direct = [
                line.strip()
                for line in source.splitlines()
                if re.match(r"^\s*adb(?:\s|$)", line)
            ]
            self.assertEqual(
                direct, ['adb -s "$device" "$@"', 'adb -s "$device" "$@" &']
            )
            guard = source.split("guard_device_action()", 1)[1].split(
                "adb_device()", 1
            )[0]
            self.assertLess(guard.index(" event "), guard.index(" own "))
            self.assertIn("device:vivo", source)

        combined = INSTALLER + PMU + HARNESS
        self.assertNotIn("kill-server", combined)
        self.assertNotIn("emulator-5554", combined)

    def test_pmu_and_harness_prove_ro_serial_before_device_mutation(self):
        for source in (PMU, HARNESS):
            verifier = source.split("verify_vivo_device_identity()", 1)[1].split(
                "host_path()", 1
            )[0]
            self.assertIn("adb_device shell getprop ro.serialno", verifier)
            self.assertIn('"$reported_serial" != "$expected_device"', verifier)

        pmu_main = PMU.rsplit("verify_canonical_fex_host || exit 51", 1)[1]
        identity = pmu_main.index("verify_vivo_device_identity || exit 69")
        self.assertLess(identity, pmu_main.index("trap cleanup EXIT"))
        self.assertLess(identity, pmu_main.index("stop_game || exit 65"))

        harness_main = HARNESS.rsplit('cd "$repo_root"', 1)[1]
        self.assertLess(
            harness_main.index("verify_vivo_device_identity"),
            harness_main.index("device_session_started=1"),
        )
        self.assertLess(
            harness_main.index("device_session_started=1"),
            harness_main.index("prepare_save_seed"),
        )


if __name__ == "__main__":
    unittest.main()
