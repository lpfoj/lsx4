import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "bloodborne-proof" / "capture_provenance.py"
SPEC = importlib.util.spec_from_file_location("bloodborne_proof_provenance", MODULE_PATH)
provenance = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = provenance
SPEC.loader.exec_module(provenance)


HASH_A = "a" * 64
HASH_B = "b" * 64
HASH_C = "c" * 64
PACKAGE = "app.lsx4.android"


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
    return {
        "schema": "lsx4.vulkan-driver-axis.v1",
        "requested": "turnip",
        "effective": "turnip",
        "fallbackReason": "none",
        "driverName": "Mesa Turnip",
        "driverInfo": "Mesa 26.2.1",
        "driverVersion": "26.2.1",
        "apiVersion": "1.3.300",
        "requestedPath": "/data/user/0/app.lsx4.android/files/vulkan/libvulkan_freedreno.so",
        "requestedSha256": HASH_B,
        "requestedPackageSource": "imported",
        "path": "/data/user/0/app.lsx4.android/files/vulkan/libvulkan_freedreno.so",
        "sha256": HASH_B,
        "packageSource": "imported",
        "mappingVerified": True,
        "selectionLocked": True,
    }


def preferences():
    return {
        "audio_enabled": {"type": "boolean", "value": True},
        "bloodborne_0109_60_fps_plus_plus": {"type": "boolean", "value": True},
    }


def expected_state():
    backend = backend_status()
    vulkan = vulkan_status()
    settings_sha = provenance.canonical_sha256(preferences())
    return {
        "schema": provenance.EXPECTED_SCHEMA,
        "device": {
            "serial": "10AG263F1B001HM",
            "soc": "SM8850",
            "board": "canoe",
            "adbTarget": "adb-vivo._adb-tls-connect._tcp",
        },
        "package": {
            "name": PACKAGE,
            "uid": 10358,
            "versionCode": 42,
            "versionName": "1.2.3",
            "codePaths": ["/data/app/example/base.apk"],
            "baseApkSha256": HASH_A,
            "signerSha256": HASH_C,
        },
        "process": {
            "cmdline": "app.lsx4.android:game",
            "foregroundComponent": "app.lsx4.android/.GameActivity",
        },
        "libraries": {
            "executor": {"basename": "liblsx4_executor_android.so", "sha256": HASH_A},
            "adapter": {"basename": "liblsx4_fex_backend_v3.so", "sha256": HASH_A},
            "bridge": {"basename": "liblsx4_fex2608_context_bridge.so", "sha256": HASH_A},
            "fexCore": {"basename": "libFEXCore.so", "sha256": HASH_A},
            "vulkanIcd": {
                "path": "/data/user/0/app.lsx4.android/files/vulkan/libvulkan_freedreno.so",
                "sha256": HASH_B,
            },
        },
        "backend": {
            "status": copy.deepcopy(backend),
            "statusSha256": provenance.canonical_sha256(backend),
            "required": {
                "requested": "fex-experimental",
                "effective": "fex-experimental",
                "fallbackReason": "none",
                "selectionLocked": True,
            },
        },
        "vulkan": {
            "statusSha256": provenance.canonical_sha256(vulkan),
            "required": {
                "requested": "turnip",
                "effective": "turnip",
                "fallbackReason": "none",
                "driverInfo": "Mesa 26.2.1",
                "path": vulkan["path"],
                "sha256": HASH_B,
                "selectionLocked": True,
                "mappingVerified": True,
            },
        },
        "settings": {"sha256": settings_sha},
        "patches": {
            "catalogSha256": HASH_A,
            "bundleSha256": HASH_B,
            "states": {"bloodborne_0109_60_fps_plus_plus": True},
            "sixtyFpsPlus": True,
        },
        "markers": {
            "allowlist": [
                "native/lsx4-bb-0109-60-fps-plus-plus",
                "run-disable-ssao",
            ],
            "present": [
                "native/lsx4-bb-0109-60-fps-plus-plus",
                "run-disable-ssao",
            ],
            "presentSha256": provenance.canonical_sha256([
                "native/lsx4-bb-0109-60-fps-plus-plus",
                "run-disable-ssao",
            ]),
        },
        "inventories": {
            "route": {"source": "host", "roots": ["route.json"], "sha256": HASH_A},
            "input": {"source": "host", "roots": ["input.json"], "sha256": HASH_B},
            "save": {"source": "app", "roots": ["files/save"], "sha256": HASH_C},
            "pipelineCache": {
                "source": "app", "roots": ["files/pipeline"], "sha256": HASH_A,
            },
            "fexCache": {"source": "app", "roots": ["files/fex"], "sha256": HASH_B},
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
            "extractRegex": r"mode=(?P<mode>\d+x\d+) refresh=(?P<refreshHz>[0-9.]+)",
            "state": {"mode": "3168x1440", "refreshHz": "60.0"},
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


def valid_capture():
    expected = expected_state()
    backend = backend_status()
    vulkan = vulkan_status()
    environment_text = {
        "thermal": "Temperature{status=0}",
        "battery": "level: 75",
        "power": "mWakefulness=Awake",
        "cpuFreq": "/sys/policy0 scaling_cur_freq=123",
        "gpuFreq": "/sys/kgsl cur_freq=456",
    }
    libraries = {}
    paths = {
        "executor": "/data/app/lib/liblsx4_executor_android.so",
        "adapter": "/data/app/lib/liblsx4_fex_backend_v3.so",
        "bridge": "/data/app/lib/liblsx4_fex2608_context_bridge.so",
        "fexCore": "/data/app/lib/libFEXCore.so",
        "vulkanIcd": vulkan["path"],
    }
    for role, path in paths.items():
        libraries[role] = {
            "path": path,
            "sha256": expected["libraries"][role]["sha256"],
            "mappedPre": True,
            "mappedPost": True,
            "mapsSha256Pre": HASH_A,
            "mapsSha256Post": HASH_A,
        }
    libraries["vulkanLoader"] = {
        "path": vulkan["path"], "sha256": HASH_B,
        "mappedPre": True, "mappedPost": True,
        "mapsSha256Pre": HASH_A, "mapsSha256Post": HASH_A,
    }
    process = {
        "pidPre": 400, "pidPost": 400,
        "starttimePre": 9001, "starttimePost": 9001,
        "cmdlinePre": "app.lsx4.android:game",
        "cmdlinePost": "app.lsx4.android:game",
        "uidPre": 10358, "uidPost": 10358,
        "foregroundPre": "app.lsx4.android/.GameActivity",
        "foregroundPost": "app.lsx4.android/.GameActivity",
    }
    surface = {
        "layerId": "7", "generation": "12", "displayId": "0",
        "layerName": expected["surface"]["requiredComponent"] + "#12",
    }
    return expected, {
        "schema": provenance.CAPTURE_SCHEMA,
        "device": {
            "serial": "10AG263F1B001HM", "soc": "SM8850", "board": "canoe",
            "bootIdPre": "boot-1", "bootIdPost": "boot-1",
        },
        "package": copy.deepcopy(expected["package"]),
        "process": process,
        "libraries": libraries,
        "backend": {"pre": backend, "post": copy.deepcopy(backend)},
        "vulkan": {"pre": vulkan, "post": copy.deepcopy(vulkan)},
        "settings": {
            "pre": preferences(), "post": preferences(),
            "sha256": expected["settings"]["sha256"],
        },
        "patches": {
            "catalogSha256": HASH_A, "bundleSha256": HASH_B,
            "states": {"bloodborne_0109_60_fps_plus_plus": True},
            "runtimeMarker": "[EXECUTOR_BUNDLED_PATCH] name=60 FPS++ requested=1",
            "backendRuntimeMarker": "EXECUTOR_GUEST_CPU_BACKEND phase=initialized status={}",
            "vulkanRuntimeMarker": "[EXECUTOR_VULKAN_DRIVER_AXIS] {}",
        },
        "markers": {
            "present": sorted(expected["markers"]["present"]),
            "presentPost": sorted(expected["markers"]["present"]),
            "preInventorySha256": HASH_A,
            "postInventorySha256": HASH_A,
        },
        "inventories": {
            name: {"pre": [{"path": name}], "post": [{"path": name}],
                   "sha256": spec["sha256"]}
            for name, spec in expected["inventories"].items()
        },
        "surface": {"pre": surface, "post": copy.deepcopy(surface)},
        "display": {"state": copy.deepcopy(expected["display"]["state"])},
        "environment": {
            phase: {
                name: {"text": text, "sha256": provenance.sha256_bytes(text.encode())}
                for name, text in environment_text.items()
            }
            for phase in ("start", "end")
        },
    }


class SnapshotValidationTests(unittest.TestCase):
    def test_accepts_complete_stable_expected_state(self):
        expected, actual = valid_capture()
        provenance.validate_snapshot(expected, actual)

    def test_accepts_exact_relaxed_memory_axis_backend_status(self):
        for axis, flags, suffix, model in (
            ("MemcpySetTSO", 2, "-ab-mtso0-sm-swtso",
             "FEX software TSO (MemcpySetTSO relaxed A/B)"),
            ("VectorTSO", 1, "-ab-vtso0-sm-swtso",
             "FEX software TSO (VectorTSO relaxed A/B)"),
        ):
            with self.subTest(axis=axis):
                expected, actual = valid_capture()
                status = copy.deepcopy(expected["backend"]["status"])
                status.update({
                    "configureFlags": flags,
                    "providedCapabilities": 8187,
                    "requiredCapabilities": 8187,
                    "forbiddenCapabilities": 4,
                    "strongMemoryModel": model,
                    "cacheNamespace": (
                        status["cacheNamespace"][:-len("-sm-swtso")] + suffix
                    ),
                })
                expected["backend"]["status"] = copy.deepcopy(status)
                expected["backend"]["statusSha256"] = (
                    provenance.canonical_sha256(status)
                )
                actual["backend"] = {
                    "pre": copy.deepcopy(status),
                    "post": copy.deepcopy(status),
                }
                provenance.validate_snapshot(expected, actual)

    def test_relaxed_memory_axis_rejects_wrong_flag_suffix_caps_or_model(self):
        expected, actual = valid_capture()
        status = copy.deepcopy(expected["backend"]["status"])
        status.update({
            "configureFlags": 2,
            "providedCapabilities": 8187,
            "requiredCapabilities": 8187,
            "forbiddenCapabilities": 4,
            "strongMemoryModel": "FEX software TSO (MemcpySetTSO relaxed A/B)",
            "cacheNamespace": (
                status["cacheNamespace"][:-len("-sm-swtso")] +
                "-ab-mtso0-sm-swtso"
            ),
        })
        expected["backend"]["status"] = copy.deepcopy(status)
        expected["backend"]["statusSha256"] = provenance.canonical_sha256(status)
        mutations = {
            "flag": lambda value: value.update(configureFlags=1),
            "suffix": lambda value: value.update(cacheNamespace="wrong-cache"),
            "provided-caps": lambda value: value.update(
                providedCapabilities=8191),
            "required-caps": lambda value: value.update(
                requiredCapabilities=4095),
            "forbidden-caps": lambda value: value.update(
                forbiddenCapabilities=0),
            "model": lambda value: value.update(strongMemoryModel="wrong-model"),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                wrong = copy.deepcopy(status)
                mutate(wrong)
                actual["backend"] = {
                    "pre": copy.deepcopy(wrong),
                    "post": copy.deepcopy(wrong),
                }
                with self.assertRaisesRegex(
                    provenance.CaptureRejected, "backend-status-hash-mismatch"
                ):
                    provenance.validate_snapshot(expected, actual)

    def test_rejects_backend_payload_hash_disagreement_in_expected_state(self):
        expected = expected_state()
        expected["backend"]["status"]["configureFlags"] = 2
        with self.assertRaisesRegex(
            provenance.CaptureRejected,
            "expected-backend-status-payload-hash-mismatch",
        ):
            provenance.validate_expected_state(expected)

    def test_rejects_unknown_or_empty_predeclared_state(self):
        for value in ("unknown", ""):
            with self.subTest(value=value):
                expected = expected_state()
                expected["vulkan"]["required"]["driverInfo"] = value
                with self.assertRaisesRegex(provenance.CaptureRejected,
                                            "expected-state-empty"):
                    provenance.validate_expected_state(expected)

    def test_rejects_pid_reuse_by_starttime(self):
        expected, actual = valid_capture()
        actual["process"]["starttimePost"] += 1
        with self.assertRaisesRegex(provenance.CaptureRejected, "process-identity-changed"):
            provenance.validate_snapshot(expected, actual)

    def test_rejects_boot_change(self):
        expected, actual = valid_capture()
        actual["device"]["bootIdPost"] = "boot-2"
        with self.assertRaisesRegex(provenance.CaptureRejected, "device-boot-changed"):
            provenance.validate_snapshot(expected, actual)

    def test_rejects_vulkan_fallback_even_if_pre_and_post_match(self):
        expected, actual = valid_capture()
        for phase in ("pre", "post"):
            actual["vulkan"][phase]["effective"] = "system"
            actual["vulkan"][phase]["fallbackReason"] = "driver-init-failed"
        with self.assertRaisesRegex(provenance.CaptureRejected, "vulkan-route-mismatch"):
            provenance.validate_snapshot(expected, actual)

    def test_rejects_transient_unlocked_vulkan_status(self):
        expected, actual = valid_capture()
        for phase in ("pre", "post"):
            actual["vulkan"][phase]["selectionLocked"] = False
        with self.assertRaisesRegex(provenance.CaptureRejected, "vulkan-selection-unlocked"):
            provenance.validate_snapshot(expected, actual)

    def test_rejects_unknown_runtime_marker(self):
        expected, actual = valid_capture()
        actual["markers"]["present"].append("run-diagnostic-unexpected-tax")
        actual["markers"]["present"].sort()
        with self.assertRaisesRegex(provenance.CaptureRejected, "unknown-runtime-marker"):
            provenance.validate_snapshot(expected, actual)

    def test_rejects_mapped_but_unselected_icd(self):
        expected, actual = valid_capture()
        path = "/vendor/lib64/hw/vulkan.adreno.so"
        expected["libraries"]["vulkanIcd"]["path"] = path
        actual["libraries"]["vulkanIcd"]["path"] = path
        with self.assertRaisesRegex(provenance.CaptureRejected,
                                    "actual-vulkan-icd-not-selected"):
            provenance.validate_snapshot(expected, actual)

    def test_rejects_library_settings_cache_and_route_hash_drift(self):
        mutations = {
            "library": lambda value: value["libraries"]["fexCore"].update(sha256=HASH_C),
            "settings": lambda value: value["settings"].update(sha256=HASH_C),
            "cache": lambda value: value["inventories"]["pipelineCache"].update(sha256=HASH_C),
            "route": lambda value: value["inventories"]["route"].update(sha256=HASH_C),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                expected, actual = valid_capture()
                mutate(actual)
                with self.assertRaises(provenance.CaptureRejected):
                    provenance.validate_snapshot(expected, actual)

    def test_rejects_settings_change_during_capture(self):
        expected, actual = valid_capture()
        actual["settings"]["post"]["audio_enabled"]["value"] = False
        with self.assertRaisesRegex(provenance.CaptureRejected, "settings-changed"):
            provenance.validate_snapshot(expected, actual)

    def test_rejects_surface_component_drift(self):
        expected, actual = valid_capture()
        for phase in ("pre", "post"):
            actual["surface"][phase]["layerName"] = (
                "SurfaceView[app.lsx4.android/app.lsx4.android.MainActivity](BLAST)#12"
            )
        with self.assertRaisesRegex(provenance.CaptureRejected,
                                    "surface-component-mismatch"):
            provenance.validate_snapshot(expected, actual)

    def test_rejects_surface_generation_drift_within_phase(self):
        expected, actual = valid_capture()
        actual["surface"]["post"]["generation"] = "13"
        actual["surface"]["post"]["layerName"] = (
            expected["surface"]["requiredComponent"] + "#13"
        )
        with self.assertRaisesRegex(provenance.CaptureRejected,
                                    "surface-generation-changed"):
            provenance.validate_snapshot(expected, actual)

    def test_accepts_fresh_surface_generation_in_independent_phase(self):
        expected, first = valid_capture()
        provenance.validate_snapshot(expected, first)

        _ignored_expected, second = valid_capture()
        for boundary in ("pre", "post"):
            second["surface"][boundary]["generation"] = "13"
            second["surface"][boundary]["layerName"] = (
                expected["surface"]["requiredComponent"] + "#13"
            )
        provenance.validate_snapshot(expected, second)


class ParsingAndInventoryTests(unittest.TestCase):
    def test_unrelated_deleted_memfd_does_not_poison_library_proof(self):
        paths = provenance.mapped_paths(
            "1000-2000 r-xp 00000000 00:00 0 /memfd:jit-zygote-cache (deleted)\n"
            "2000-3000 r-xp 00000000 00:00 0 /data/app/libwanted.so\n"
        )
        self.assertEqual(
            provenance.select_mapped_library(
                paths, {"basename": "libwanted.so"}, "executor"),
            "/data/app/libwanted.so",
        )
        with self.assertRaisesRegex(provenance.CaptureRejected,
                                    "mapped-library-deleted"):
            provenance.select_mapped_library(
                {"/data/app/libwanted.so (deleted)"},
                {"basename": "libwanted.so"}, "executor")

    def test_marker_inventory_keeps_exact_renamed_artifacts_for_allowlist(self):
        inventory = {"entries": [
            {"path": "files/lsx4-home/diag-fex-proof-metrics-static",
             "type": "file"},
            {"path": "files/lsx4-home/run-disable-ssao", "type": "file"},
            {"path": "files/lsx4-home/native/lsx4-bb-0109-60-fps-plus-plus",
             "type": "file"},
        ]}
        self.assertEqual(
            provenance.marker_entries(inventory, provenance.CANONICAL_MARKER_ROOTS),
            ["diag-fex-proof-metrics-static",
             "native/lsx4-bb-0109-60-fps-plus-plus", "run-disable-ssao"],
        )
        inventory["entries"].extend([
            {"path": "files/lsx4-home/diag-unreviewed.diag-off", "type": "file"},
            {"path": "files/lsx4-home/native/run-disable-dof", "type": "file"},
            {"path": "files/lsx4-home/native/diag-audio-old.wav", "type": "file"},
        ])
        self.assertEqual(
            provenance.marker_entries(inventory, provenance.CANONICAL_MARKER_ROOTS),
            ["diag-fex-proof-metrics-static", "diag-unreviewed.diag-off",
             "native/diag-audio-old.wav",
             "native/lsx4-bb-0109-60-fps-plus-plus",
             "native/run-disable-dof", "run-disable-ssao"],
        )

    def test_marker_inventory_rejects_real_marker_in_wrong_root(self):
        wrong = {"entries": [{
            "path": "files/lsx4-home/lsx4-bb-0109-60-fps-plus-plus",
            "type": "file",
        }]}
        with self.assertRaisesRegex(provenance.CaptureRejected,
                                   "marker-root-layout-invalid"):
            provenance.marker_entries(wrong, provenance.CANONICAL_MARKER_ROOTS)

    def test_marker_inventory_command_never_walks_cache_tree(self):
        script = provenance.remote_marker_inventory_script(
            provenance.CANONICAL_MARKER_ROOTS)
        self.assertNotIn("find ", script)
        self.assertIn('"$root"/run-*', script)
        self.assertIn('"$root"/diag-*', script)
        self.assertIn("'files/lsx4-home/native'", script)

    def test_expected_state_rejects_noncanonical_marker_root(self):
        expected = expected_state()
        expected["collection"]["markerRoots"]["patch"] = (
            "files/lsx4-home/not-native"
        )
        with self.assertRaisesRegex(provenance.CaptureRejected,
                                   "marker-roots-not-canonical"):
            provenance.validate_expected_state(expected)

    def test_normalizes_preferences_independent_of_xml_order(self):
        first = provenance.normalize_preferences(
            '<map><boolean name="b" value="true"/><int name="a" value="2"/></map>'
        )
        second = provenance.normalize_preferences(
            '<map><int name="a" value="2"/><boolean name="b" value="true"/></map>'
        )
        self.assertEqual(provenance.canonical_sha256(first),
                         provenance.canonical_sha256(second))

    def test_local_inventory_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "root"
            root.mkdir()
            target = root / "target"
            target.write_text("data", encoding="utf-8")
            link = root / "link"
            try:
                link.symlink_to(target)
            except OSError as error:
                self.skipTest(f"symlink unavailable: {error}")
            with self.assertRaisesRegex(provenance.CaptureRejected, "inventory-symlink"):
                provenance.local_inventory([str(root)])

    def test_remote_inventory_rejects_special_and_missing(self):
        for text in ("S\tfiles/cache/socket\n", "M\tfiles/cache\n",
                     "L\tfiles/cache/link\n"):
            with self.subTest(text=text):
                with self.assertRaises(provenance.CaptureRejected):
                    provenance.parse_remote_inventory(text, ["files/cache"])

    def test_runtime_marker_requires_success_and_no_failure(self):
        good = ("I LSX4Native: [EXECUTOR_BUNDLED_PATCH] name=60 FPS++ group=fps "
                "requested=1 version=01.09 appVersion=01.09 contentVersion=01.09 "
                "versionMatch=1 eligibleLines=17")
        self.assertEqual(provenance.validate_patch_runtime_marker(good), good)
        with self.assertRaises(provenance.CaptureRejected):
            provenance.validate_patch_runtime_marker(
                good + "\nE LSX4Native: Failed to apply patch: 60 FPS++"
            )

    def test_status_files_do_not_pass_without_current_pid_runtime_markers(self):
        with self.assertRaisesRegex(provenance.CaptureRejected,
                                    "runtime-backend-marker-missing"):
            provenance.validate_runtime_status_markers(
                "unrelated current-pid log", backend_status(), vulkan_status()
            )

    def test_runtime_driver_properties_must_equal_status(self):
        backend = backend_status()
        runtime_vulkan = vulkan_status()
        stale_file = copy.deepcopy(runtime_vulkan)
        stale_file["driverInfo"] = "Mesa 26.1.0 stale file"
        log = (
            "EXECUTOR_GUEST_CPU_BACKEND phase=initialized status="
            + json.dumps(backend, separators=(",", ":"))
            + "\n[EXECUTOR_VULKAN_DRIVER_AXIS] "
            + json.dumps(runtime_vulkan, separators=(",", ":"))
        )
        with self.assertRaisesRegex(provenance.CaptureRejected,
                                    "runtime-vulkan-marker-stale"):
            provenance.validate_runtime_status_markers(log, backend, stale_file)


class SubprocessGuardTests(unittest.TestCase):
    @mock.patch.object(provenance.subprocess, "run")
    def test_every_adb_read_is_event_then_owned(self, run):
        run.side_effect = [
            subprocess.CompletedProcess([], 0, b"event seq=1\n", b""),
            subprocess.CompletedProcess([], 0, b"owned by codex\n", b""),
            subprocess.CompletedProcess([], 0, b"owned by codex\n", b""),
            subprocess.CompletedProcess([], 0, b"10AG263F1B001HM\n", b""),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary) / "capture"
            store = provenance.ArtifactStore(output)
            runner = provenance.CommandRunner()
            gate = provenance.AgentChatGate(
                runner, pathlib.Path("agentchat.py"), agent="codex",
                room="увеличить fps",
                additional_areas=("adb:10AG263F1B001HM",),
            )
            adb = provenance.AdbReader(
                runner, gate, store, "adb", "adb-vivo._adb-tls-connect._tcp"
            )
            self.assertEqual(
                adb.text("serial", ["shell", "getprop", "ro.serialno"]).strip(),
                "10AG263F1B001HM",
            )
        argv = [call.args[0] for call in run.call_args_list]
        self.assertEqual(argv[0][2], "event")
        self.assertIn("device:vivo", argv[0])
        self.assertEqual(argv[1][2], "own")
        self.assertIn("device:vivo", argv[1])
        self.assertEqual(argv[2][2], "own")
        self.assertIn("adb:10AG263F1B001HM", argv[2])
        self.assertEqual(argv[3][:3], ["adb", "-s", "adb-vivo._adb-tls-connect._tcp"])

    def test_source_contains_no_device_mutation_route(self):
        source = MODULE_PATH.read_text(encoding="utf-8")
        for forbidden in (
            "force-stop", " am start", "monkey", "input tap", "input swipe",
            "settings put", "setprop", "reboot", " install ", " rm ", " touch ",
        ):
            self.assertNotIn(forbidden, source)


if __name__ == "__main__":
    unittest.main()
