import copy
import importlib.util
import json
import pathlib
import stat
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = (
    ROOT / "tools" / "bloodborne-proof" / "generate_expected_state_candidate.py"
)
SPEC = importlib.util.spec_from_file_location(
    "bloodborne_expected_state_candidate", MODULE_PATH
)
candidate = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = candidate
SPEC.loader.exec_module(candidate)
provenance = candidate.provenance


HASH_A = "a" * 64
HASH_B = "b" * 64
HASH_C = "c" * 64


def backend_status():
    return {
        "schema": "lsx4.guest-cpu-backend.v1",
        "platform": "ps4",
        "requested": "fex-experimental",
        "effective": "fex-experimental",
        "supported": True,
        "fallbackReason": "none",
        "adapterAbi": "lsx4-fex-backend-v3",
        "mappedEntryAbi": "lsx4-fex-mapped-entry-v3",
        "cacheNamespace": "fex-2608-ps4-v3-test-lookup-throughput-sm-swtso",
        "configureFlags": 0,
        "providedCapabilities": 8191,
        "requiredCapabilities": 4095,
        "forbiddenCapabilities": 0,
        "strongMemoryModel": (
            "FEX software TSO (hardware TSO when enabled by the host OS)"
        ),
        "faultSignalOwner": "unselected",
        "hostIsaFeatures": 63,
        "hostOsFeatures": 1,
        "hostSveVectorBytes": 64,
        "selectionLocked": True,
    }


def vulkan_status():
    path = "/data/user/0/app.lsx4.android/files/vulkan/libvulkan_freedreno.so"
    return {
        "schema": "lsx4.vulkan-driver-axis.v1",
        "requested": "turnip",
        "effective": "turnip",
        "fallbackReason": "none",
        "driverName": "Mesa Turnip",
        "driverInfo": "Mesa Turnip 26.2.1",
        "driverVersion": candidate.REQUIRED_VULKAN_DRIVER_VERSION,
        "apiVersion": "1.3.300",
        "requestedPath": path,
        "requestedSha256": HASH_B,
        "requestedPackageSource": "imported",
        "path": path,
        "sha256": HASH_B,
        "packageSource": "imported",
        "mappingVerified": True,
        "selectionLocked": True,
    }


def declaration():
    return {
        "schema": candidate.DECLARATION_SCHEMA,
        "device": {
            "serial": candidate.PHYSICAL_SERIAL,
            "adbTarget": "adb-10AG263F1B001HM-test._adb-tls-connect._tcp",
            "soc": "SM8850",
            "board": "canoe",
        },
        "package": {"name": candidate.PACKAGE_NAME},
        "process": {
            "cmdline": "app.lsx4.android:game",
            "foregroundComponent": "app.lsx4.android/.GameActivity",
        },
        "libraries": {
            "executor": {"basename": "liblsx4_executor_android.so"},
            "adapter": {"basename": "liblsx4_fex_backend_v3.so"},
            "bridge": {"basename": "liblsx4_fex2608_context_bridge.so"},
            "fexCore": {"basename": "libFEXCore.so"},
            "vulkanIcd": {"basename": "libvulkan_freedreno.so"},
        },
        "backend": {
            "required": {
                "requested": "fex-experimental",
                "effective": "fex-experimental",
                "supported": True,
                "fallbackReason": "none",
                "selectionLocked": True,
            }
        },
        "vulkan": {
            "required": {
                "requested": "turnip",
                "effective": "turnip",
                "fallbackReason": "none",
                "driverInfo": "Mesa Turnip 26.2.1",
                "driverVersion": candidate.REQUIRED_VULKAN_DRIVER_VERSION,
                "packageSource": "imported",
                "selectionLocked": True,
                "mappingVerified": True,
            }
        },
        "patches": {
            "requiredStates": {
                candidate.REQUIRED_PATCH_ID: True,
            }
        },
        "markers": {
            "allowlist": [
                candidate.REQUIRED_PATCH_MARKER,
                "run-disable-ssao",
                "run-enable-fex-multiblock",
                "run-enable-fex-buffer-capture-ack",
                "run-enable-fex-deferred-write-epoch",
                "run-enable-bounded-guest-invalidation-drain",
                "run-enable-fex-relaxed-memcpyset-tso-ab",
                "run-enable-fex-relaxed-vector-tso-ab",
                "run-enable-hle-clean-buffer-fastpath",
                "diag-fex-proof-metrics-static",
                "diag-fex-proof-metrics-dynamic",
            ],
            "requiredPresent": [candidate.REQUIRED_PATCH_MARKER],
            "forbidden": [
                "run-enable-fex-multiblock",
                "run-enable-fex-buffer-capture-ack",
                "run-enable-fex-deferred-write-epoch",
                "run-enable-bounded-guest-invalidation-drain",
                "run-enable-fex-relaxed-memcpyset-tso-ab",
                "run-enable-fex-relaxed-vector-tso-ab",
                "run-enable-hle-clean-buffer-fastpath",
                "diag-fex-proof-metrics-static",
                "diag-fex-proof-metrics-dynamic",
            ],
        },
        "inventories": {
            "route": {"source": "host", "roots": ["route.json"]},
            "input": {"source": "host", "roots": ["input.json"]},
            "save": {"source": "app", "roots": ["files/lsx4-home/native/savedata"]},
            "pipelineCache": {
                "source": "app", "roots": ["files/lsx4-home/native/cache"],
            },
            "fexCache": {"source": "app", "roots": ["files/lsx4-home/cache"]},
        },
        "surface": {
            "requiredComponent": (
                "SurfaceView[app.lsx4.android/"
                "app.lsx4.android.GameActivity](BLAST)"
            ),
            "displayId": "0",
            "identityRegex": (
                r"layerId=(?P<layerId>\d+).*generation=(?P<generation>\d+)"
                r".*displayId=(?P<displayId>\d+)"
            ),
        },
        "display": {
            "extractRegex": (
                r"mode=(?P<mode>\d+x\d+) refresh=(?P<refreshHz>[0-9.]+)"
            ),
            "requiredState": {"mode": "3168x1440", "refreshHz": "60.0"},
        },
        "environment": {
            "requiredRegexes": {
                "thermal": "Temperature",
                "battery": "level:",
                "power": "mWakefulness=Awake",
                "cpuFreq": "policy0",
                "gpuFreq": "kgsl",
            }
        },
        "collection": {
            "backendStatusPath": "files/guest-cpu-backend-status.json",
            "vulkanStatusPath": "files/vulkan-driver-status.json",
            "preferencesPath": "shared_prefs/lsx4_settings.xml",
            "markerRoots": copy.deepcopy(provenance.CANONICAL_MARKER_ROOTS),
        },
    }


def observed_capture():
    declared = declaration()
    backend = backend_status()
    vulkan = vulkan_status()
    settings = {
        "audio_enabled": {"type": "boolean", "value": True},
        candidate.REQUIRED_PATCH_ID: {"type": "boolean", "value": True},
    }
    library_paths = {
        "executor": "/data/app/lib/liblsx4_executor_android.so",
        "adapter": "/data/app/lib/liblsx4_fex_backend_v3.so",
        "bridge": "/data/app/lib/liblsx4_fex2608_context_bridge.so",
        "fexCore": "/data/app/lib/libFEXCore.so",
        "vulkanIcd": vulkan["path"],
    }
    libraries = {
        role: {
            "path": path,
            "sha256": HASH_B if role == "vulkanIcd" else HASH_A,
            "mappedPre": True,
            "mappedPost": True,
            "mapsSha256Pre": HASH_C,
            "mapsSha256Post": HASH_C,
        }
        for role, path in library_paths.items()
    }
    libraries["vulkanLoader"] = {
        "path": vulkan["path"],
        "sha256": HASH_B,
        "mappedPre": True,
        "mappedPost": True,
        "mapsSha256Pre": HASH_C,
        "mapsSha256Post": HASH_C,
    }
    process = {
        "pidPre": 400,
        "pidPost": 400,
        "starttimePre": 9001,
        "starttimePost": 9001,
        "cmdlinePre": declared["process"]["cmdline"],
        "cmdlinePost": declared["process"]["cmdline"],
        "uidPre": 10358,
        "uidPost": 10358,
        "foregroundPre": declared["process"]["foregroundComponent"],
        "foregroundPost": declared["process"]["foregroundComponent"],
    }
    layer = {
        "layerId": "7",
        "generation": "12",
        "displayId": "0",
        "layerName": declared["surface"]["requiredComponent"] + "#12",
    }
    environment_text = {
        "thermal": "Temperature{status=0}",
        "battery": "level: 75",
        "power": "mWakefulness=Awake",
        "cpuFreq": "/sys/policy0 scaling_cur_freq=123",
        "gpuFreq": "/sys/kgsl cur_freq=456",
    }
    present = sorted([
        candidate.REQUIRED_PATCH_MARKER,
        "run-disable-ssao",
    ])
    inventories = {
        name: {
            "pre": [{"path": name, "type": "dir"}],
            "post": [{"path": name, "type": "dir"}],
            "sha256": digest,
        }
        for name, digest in zip(
            sorted(provenance.REQUIRED_INVENTORIES),
            (HASH_A, HASH_B, HASH_C, "d" * 64, "e" * 64),
        )
    }
    return {
        "schema": provenance.CAPTURE_SCHEMA,
        "capturedAtUnixNs": 123,
        "device": {
            "serial": candidate.PHYSICAL_SERIAL,
            "soc": "SM8850",
            "board": "canoe",
            "bootIdPre": "boot-1",
            "bootIdPost": "boot-1",
        },
        "package": {
            "name": candidate.PACKAGE_NAME,
            "uid": 10358,
            "versionCode": 42,
            "versionName": "1.2.3",
            "codePaths": ["/data/app/example/base.apk"],
            "baseApkSha256": HASH_A,
            "signerSha256": HASH_C,
        },
        "process": process,
        "libraries": libraries,
        "backend": {"pre": backend, "post": copy.deepcopy(backend)},
        "vulkan": {"pre": vulkan, "post": copy.deepcopy(vulkan)},
        "settings": {
            "pre": settings,
            "post": copy.deepcopy(settings),
            "sha256": provenance.canonical_sha256(settings),
        },
        "patches": {
            "catalogSha256": HASH_A,
            "bundleSha256": HASH_B,
            "states": {candidate.REQUIRED_PATCH_ID: True},
            "runtimeMarker": "[EXECUTOR_BUNDLED_PATCH] name=60 FPS++ requested=1",
            "backendRuntimeMarker": "EXECUTOR_GUEST_CPU_BACKEND phase=initialized status={}",
            "vulkanRuntimeMarker": "[EXECUTOR_VULKAN_DRIVER_AXIS] {}",
        },
        "markers": {
            "present": present,
            "presentPost": list(present),
            "preInventorySha256": HASH_A,
            "postInventorySha256": HASH_A,
        },
        "inventories": inventories,
        "surface": {"pre": layer, "post": copy.deepcopy(layer)},
        "display": {"state": copy.deepcopy(declared["display"]["requiredState"])},
        "environment": {
            phase: {
                name: {
                    "text": text,
                    "sha256": provenance.sha256_bytes(text.encode("utf-8")),
                }
                for name, text in environment_text.items()
            }
            for phase in ("start", "end")
        },
    }


class CandidateBuilderTests(unittest.TestCase):
    def test_builds_valid_nested_expected_state_but_never_an_accepted_verdict(self):
        actual = observed_capture()
        expected = candidate.build_candidate_expected(declaration(), actual)
        provenance.validate_snapshot(expected, actual)
        self.assertEqual(expected["backend"]["status"], backend_status())
        self.assertEqual(
            expected["backend"]["statusSha256"],
            provenance.canonical_sha256(expected["backend"]["status"]),
        )

        wrapper = candidate.build_candidate_wrapper(expected, actual)
        self.assertEqual(wrapper["schema"], candidate.CANDIDATE_SCHEMA)
        self.assertIs(wrapper["accepted"], False)
        self.assertIs(wrapper["candidateOnly"], True)
        self.assertIs(wrapper["verdictAllowed"], False)
        self.assertEqual(wrapper["reason"], "manual-freeze-required")
        self.assertEqual(
            wrapper["expectedStateCandidateSha256"],
            provenance.canonical_sha256(expected),
        )
        with self.assertRaisesRegex(provenance.CaptureRejected,
                                    "expected-schema-invalid"):
            provenance.validate_expected_state(wrapper)

    def test_candidate_freezes_component_not_surface_generation(self):
        expected = candidate.build_candidate_expected(declaration(), observed_capture())
        self.assertIn("requiredComponent", expected["surface"])
        self.assertNotIn("layerName", expected["surface"])
        self.assertNotIn("generation", expected["surface"])

    def test_rejects_changed_observation_instead_of_blessing_it(self):
        actual = observed_capture()
        actual["settings"]["post"]["audio_enabled"]["value"] = False
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "candidate-settings-changed"):
            candidate.build_candidate_expected(declaration(), actual)

    def test_rejects_missing_and_forbidden_markers(self):
        for mutation, reason in (
            (lambda markers: markers.remove(candidate.REQUIRED_PATCH_MARKER),
             "candidate-required-marker-missing"),
            (lambda markers: markers.append("run-enable-fex-multiblock"),
             "candidate-forbidden-marker-present"),
        ):
            with self.subTest(reason=reason):
                actual = observed_capture()
                mutation(actual["markers"]["present"])
                actual["markers"]["present"].sort()
                actual["markers"]["presentPost"] = list(actual["markers"]["present"])
                with self.assertRaisesRegex(candidate.CaptureRejected, reason):
                    candidate.build_candidate_expected(declaration(), actual)

    def test_rejects_fallback_and_display_drift(self):
        actual = observed_capture()
        for phase in ("pre", "post"):
            actual["vulkan"][phase]["effective"] = "system"
            actual["vulkan"][phase]["fallbackReason"] = "init-failed"
        with self.assertRaises(candidate.CaptureRejected):
            candidate.build_candidate_expected(declaration(), actual)

        actual = observed_capture()
        actual["display"]["state"]["refreshHz"] = "120.0"
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "candidate-display-state-mismatch"):
            candidate.build_candidate_expected(declaration(), actual)

    def test_writes_hash_manifest_and_can_be_sealed(self):
        declared = declaration()
        actual = observed_capture()
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "candidate"
            store = provenance.ArtifactStore(root)
            wrapper = candidate.write_success_artifacts(store, declared, actual)
            manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["schema"], candidate.MANIFEST_SCHEMA)
            self.assertEqual(set(manifest["artifacts"]), {
                "candidate.json", "declaration.json", "observed-capture.json",
            })
            self.assertEqual(
                manifest["artifacts"]["candidate.json"],
                provenance.sha256_bytes(provenance.canonical_json(wrapper)),
            )
            store.seal()
            self.assertFalse((root / "candidate.json").stat().st_mode & stat.S_IWRITE)


class DeclarationGuardTests(unittest.TestCase):
    def test_declaration_is_closed_to_unknown_fields(self):
        declared = declaration()
        declared["device"]["observedLater"] = True
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "declaration-keys-invalid"):
            candidate.validate_declaration(declared)

    def test_declaration_is_locked_to_physical_vivo_and_turnip_2621(self):
        declared = declaration()
        declared["device"]["serial"] = "another-device"
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "declaration-device-not-vivo"):
            candidate.validate_declaration(declared)

        declared = declaration()
        declared["vulkan"]["required"]["driverVersion"] = "109060096"
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "declaration-vulkan-not-clean-turnip-26.2.1"):
            candidate.validate_declaration(declared)

        declared = declaration()
        declared["vulkan"]["required"]["driverInfo"] = "Mesa Turnip 26.2.0"
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "declaration-vulkan-semver-mismatch"):
            candidate.validate_declaration(declared)

        declared = declaration()
        declared["device"]["adbTarget"] = "emulator-5554"
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "declaration-adb-target-not-vivo"):
            candidate.validate_declaration(declared)

    def test_declaration_requires_all_libraries_and_inventories(self):
        for section, key, reason in (
            ("libraries", "bridge", "declaration-library-roles-incomplete"),
            ("inventories", "save", "declaration-inventories-incomplete"),
        ):
            with self.subTest(section=section):
                declared = declaration()
                del declared[section][key]
                with self.assertRaisesRegex(candidate.CaptureRejected, reason):
                    candidate.validate_declaration(declared)

    def test_marker_policy_requires_forbidden_axes_to_be_known(self):
        declared = declaration()
        declared["markers"]["allowlist"].remove("run-enable-fex-multiblock")
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "declaration-forbidden-marker-not-known"):
            candidate.validate_declaration(declared)

    def test_patch_marker_must_use_real_native_qualified_layout(self):
        declared = declaration()
        declared["markers"]["requiredPresent"] = [
            candidate.REQUIRED_PATCH_MARKER_BASENAME
        ]
        declared["markers"]["allowlist"].append(
            candidate.REQUIRED_PATCH_MARKER_BASENAME
        )
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "declaration-marker-invalid"):
            candidate.validate_declaration(declared)

        declared = declaration()
        declared["collection"]["markerRoots"]["patch"] = "files/lsx4-home"
        with self.assertRaisesRegex(candidate.CaptureRejected,
                                    "declaration-marker-roots-not-canonical"):
            candidate.validate_declaration(declared)

        declared = declaration()
        declared["markers"]["forbidden"].remove("run-enable-fex-multiblock")
        with self.assertRaisesRegex(
                candidate.CaptureRejected,
                "declaration-mandatory-forbidden-marker-missing"):
            candidate.validate_declaration(declared)


class ReadOnlyOwnershipTests(unittest.TestCase):
    class FakeRunner:
        def __init__(self):
            self.argv = []

        def run(self, argv, *, timeout=60):
            self.argv.append(list(argv))
            return provenance.CommandResult(list(argv), 0, b"ok\n", b"")

    def test_each_adb_read_acquires_both_ownership_areas_first(self):
        runner = self.FakeRunner()
        gate = candidate.DualAgentChatGate(
            runner, pathlib.Path("agentchat.py"), agent="codex", room="увеличить fps"
        )
        with tempfile.TemporaryDirectory() as temporary:
            store = provenance.ArtifactStore(pathlib.Path(temporary) / "capture")
            adb = provenance.AdbReader(
                runner, gate, store, "adb", declaration()["device"]["adbTarget"]
            )
            self.assertEqual(adb.text("probe", ["shell", "getprop", "ro.serialno"]),
                             "ok\n")

        self.assertEqual(len(runner.argv), 5)
        self.assertEqual([argv[2] for argv in runner.argv[:4]],
                         ["event", "own", "event", "own"])
        self.assertIn(candidate.DEVICE_OWNERSHIP_AREA, runner.argv[0])
        self.assertIn(candidate.DEVICE_OWNERSHIP_AREA, runner.argv[1])
        self.assertIn(candidate.ADB_OWNERSHIP_AREA, runner.argv[2])
        self.assertIn(candidate.ADB_OWNERSHIP_AREA, runner.argv[3])
        self.assertEqual(runner.argv[4][:3], [
            "adb", "-s", declaration()["device"]["adbTarget"],
        ])

    def test_source_exposes_no_device_mutation_or_direct_acceptance_switch(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        for forbidden in (
            "force-stop", " am start", "monkey", "input tap", "input swipe",
            "settings put", "setprop", "reboot", " install ", " rm ", " touch ",
            "--promote",
        ):
            self.assertNotIn(forbidden, source)
        self.assertNotIn('"accepted": True', source)


if __name__ == "__main__":
    unittest.main()
