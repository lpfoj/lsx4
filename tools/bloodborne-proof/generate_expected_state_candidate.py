#!/usr/bin/env python3
"""Collect a read-only, candidate-only Bloodborne expected-state document.

This is deliberately not an expected-state promotion tool.  It records two
stable observations, verifies them against a strict operator declaration, and
emits a wrapper that cannot be consumed by the proof campaign as an accepted
expected state.  A human must review and separately freeze the nested
``expectedStateCandidate`` before any campaign may use it.

Every adb invocation is routed through :class:`capture_provenance.AdbReader`.
Its gate acquires both ``device:vivo`` and the physical-device ownership area
``adb:10AG263F1B001HM`` immediately before each read.  No device mutation,
application launch, setting change, cache deletion, or input injection exists
in this program.
"""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import pathlib
import re
import sys
import tempfile
import time
from collections.abc import Mapping, Sequence
from typing import Any


def _load_provenance() -> Any:
    """Load the sibling module both as a script and through importlib tests."""
    module_name = "bloodborne_candidate_capture_provenance"
    existing = sys.modules.get(module_name)
    if existing is not None:
        return existing
    path = pathlib.Path(__file__).with_name("capture_provenance.py")
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load provenance helper: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


provenance = _load_provenance()

DECLARATION_SCHEMA = "lsx4-bloodborne-expected-state-bootstrap-declaration/v1"
CANDIDATE_SCHEMA = "lsx4-bloodborne-expected-state-candidate/v1"
MANIFEST_SCHEMA = "lsx4-bloodborne-candidate-artifact-manifest/v1"
PHYSICAL_SERIAL = "10AG263F1B001HM"
DEVICE_OWNERSHIP_AREA = "device:vivo"
ADB_OWNERSHIP_AREA = f"adb:{PHYSICAL_SERIAL}"
PACKAGE_NAME = "app.lsx4.android"
REQUIRED_PATCH_ID = "bloodborne_0109_60_fps_plus_plus"
REQUIRED_PATCH_MARKER_BASENAME = "lsx4-bb-0109-60-fps-plus-plus"
REQUIRED_PATCH_MARKER = "native/" + REQUIRED_PATCH_MARKER_BASENAME
REQUIRED_MESA_VERSION = "26.2.1"
REQUIRED_VULKAN_DRIVER_VERSION = "109060097"
ADB_TARGET_RE = re.compile(
    rf"^adb-{re.escape(PHYSICAL_SERIAL)}-[A-Za-z0-9_-]+"
    r"\._adb-tls-connect\._tcp(?: \([0-9]+\))?$"
)
MANDATORY_FORBIDDEN_MARKERS = frozenset({
    "run-enable-fex-multiblock",
    "run-enable-fex-buffer-capture-ack",
    "run-enable-fex-deferred-write-epoch",
    "run-enable-bounded-guest-invalidation-drain",
    "run-enable-fex-relaxed-memcpyset-tso-ab",
    "run-enable-fex-relaxed-vector-tso-ab",
    "run-enable-hle-clean-buffer-fastpath",
    "diag-fex-proof-metrics-static",
    "diag-fex-proof-metrics-dynamic",
})
REQUIRED_BACKEND_FIELDS = {
    "requested", "effective", "supported", "fallbackReason", "selectionLocked"
}
REQUIRED_VULKAN_FIELDS = {
    "requested", "effective", "fallbackReason", "driverInfo", "driverVersion",
    "packageSource", "selectionLocked", "mappingVerified",
}
COLLECTION_FIELDS = {
    "backendStatusPath", "vulkanStatusPath", "preferencesPath", "markerRoots"
}
MANUAL_REVIEW = [
    "Confirm the APK version, signer, base APK hash, and every loaded library hash.",
    "Confirm FEX is requested/effective without fallback and matches the intended build.",
    "Confirm the selected mapped Vulkan ICD is Mesa Turnip 26.2.1 without fallback.",
    "Confirm 60 FPS++ and every other Bloodborne patch state are intentional.",
    "Confirm the complete marker set; diagnostic and quarantined markers must stay absent.",
    "Confirm route, input, save, pipeline-cache, and FEX-cache inventories are canonical.",
    "Confirm the foreground Hunter's Dream flowered-path scene and display state independently.",
    "Freeze the nested expectedStateCandidate into a separate reviewed expected-state file.",
    "Run ordinary provenance capture twice against that frozen file before a campaign.",
]


CaptureRejected = provenance.CaptureRejected


def reject_if(condition: bool, reason: str, detail: str | None = None) -> None:
    provenance.reject_if(condition, reason, detail)


def require_mapping(value: Any, path: str) -> Mapping[str, Any]:
    reject_if(not isinstance(value, Mapping), "declaration-object-required", path)
    return value


def require_exact_keys(value: Any, required: set[str], path: str) -> Mapping[str, Any]:
    mapping = require_mapping(value, path)
    actual = set(mapping)
    reject_if(actual != required, "declaration-keys-invalid",
              f"{path}: missing={sorted(required - actual)!r} "
              f"unknown={sorted(actual - required)!r}")
    return mapping


def require_string(value: Any, path: str) -> str:
    reject_if(not isinstance(value, str) or not value.strip(),
              "declaration-string-invalid", path)
    return value


def require_string_list(value: Any, path: str, *, allow_empty: bool = False) -> list[str]:
    reject_if(not isinstance(value, list) or (not value and not allow_empty),
              "declaration-list-invalid", path)
    reject_if(any(not isinstance(item, str) or not item for item in value),
              "declaration-list-invalid", path)
    reject_if(len(set(value)) != len(value), "declaration-list-duplicate", path)
    return list(value)


def require_regex(value: Any, path: str) -> str:
    expression = require_string(value, path)
    try:
        compiled = re.compile(expression)
    except re.error as error:
        raise CaptureRejected("declaration-regex-invalid", f"{path}: {error}") from error
    reject_if(not compiled.groupindex, "declaration-regex-has-no-named-groups", path)
    return expression


def validate_library_declarations(value: Any) -> None:
    libraries = require_mapping(value, "libraries")
    reject_if(set(libraries) != provenance.REQUIRED_LIBRARY_ROLES,
              "declaration-library-roles-incomplete",
              repr(sorted(set(libraries) ^ provenance.REQUIRED_LIBRARY_ROLES)))
    for role, raw_spec in libraries.items():
        spec = require_exact_keys(raw_spec, {"basename"}, f"libraries.{role}")
        basename = require_string(spec["basename"], f"libraries.{role}.basename")
        reject_if(pathlib.PurePosixPath(basename).name != basename or "/" in basename,
                  "declaration-library-basename-invalid", role)


def validate_inventory_declarations(value: Any) -> None:
    inventories = require_mapping(value, "inventories")
    reject_if(set(inventories) != provenance.REQUIRED_INVENTORIES,
              "declaration-inventories-incomplete",
              repr(sorted(set(inventories) ^ provenance.REQUIRED_INVENTORIES)))
    for name, raw_spec in inventories.items():
        spec = require_exact_keys(raw_spec, {"source", "roots"}, f"inventories.{name}")
        reject_if(spec["source"] not in {"host", "app"},
                  "declaration-inventory-source-invalid", name)
        roots = require_string_list(spec["roots"], f"inventories.{name}.roots")
        if spec["source"] == "app":
            for root in roots:
                provenance.validate_app_path(root)


def validate_marker_name(name: str, path: str) -> None:
    if name.startswith("native/"):
        basename = name[len("native/"):]
        reject_if(provenance.MARKER_ARTIFACT_NAME_RE.fullmatch(basename) is None,
                  "declaration-marker-invalid", f"{path}:{name}")
        return
    reject_if(name.startswith("lsx4-bb-") or
              provenance.MARKER_ARTIFACT_NAME_RE.fullmatch(name) is None,
              "declaration-marker-invalid", f"{path}:{name}")


def validate_declaration(declaration: Mapping[str, Any]) -> None:
    top = require_exact_keys(declaration, {
        "schema", "device", "package", "process", "libraries", "backend",
        "vulkan", "patches", "markers", "inventories", "surface", "display",
        "environment", "collection",
    }, "declaration")
    reject_if(top["schema"] != DECLARATION_SCHEMA, "declaration-schema-invalid")

    device = require_exact_keys(top["device"], {"serial", "adbTarget", "soc", "board"},
                                "device")
    reject_if(device["serial"] != PHYSICAL_SERIAL, "declaration-device-not-vivo",
              str(device["serial"]))
    for name in ("adbTarget", "soc", "board"):
        require_string(device[name], f"device.{name}")
    reject_if(ADB_TARGET_RE.fullmatch(str(device["adbTarget"])) is None,
              "declaration-adb-target-not-vivo", str(device["adbTarget"]))

    package = require_exact_keys(top["package"], {"name"}, "package")
    reject_if(package["name"] != PACKAGE_NAME, "declaration-package-invalid",
              str(package["name"]))
    process = require_exact_keys(top["process"], {"cmdline", "foregroundComponent"},
                                 "process")
    require_string(process["cmdline"], "process.cmdline")
    component = require_string(process["foregroundComponent"],
                               "process.foregroundComponent")
    reject_if(not component.startswith(PACKAGE_NAME + "/"),
              "declaration-component-invalid", component)

    validate_library_declarations(top["libraries"])

    backend = require_exact_keys(top["backend"], {"required"}, "backend")
    required_backend = require_exact_keys(backend["required"], REQUIRED_BACKEND_FIELDS,
                                          "backend.required")
    reject_if(required_backend["requested"] != "fex-experimental"
              or required_backend["effective"] != "fex-experimental"
              or required_backend["supported"] is not True
              or required_backend["fallbackReason"] != "none"
              or required_backend["selectionLocked"] is not True,
              "declaration-backend-not-clean-fex")

    vulkan = require_exact_keys(top["vulkan"], {"required"}, "vulkan")
    required_vulkan = require_exact_keys(vulkan["required"], REQUIRED_VULKAN_FIELDS,
                                         "vulkan.required")
    reject_if(required_vulkan["requested"] != "turnip"
              or required_vulkan["effective"] != "turnip"
              or required_vulkan["fallbackReason"] != "none"




              or required_vulkan["driverVersion"] != REQUIRED_VULKAN_DRIVER_VERSION
              or required_vulkan["packageSource"] not in {"bundled", "imported"}
              or required_vulkan["selectionLocked"] is not True
              or required_vulkan["mappingVerified"] is not True,
              "declaration-vulkan-not-clean-turnip-26.2.1")
    driver_info = require_string(required_vulkan["driverInfo"],
                                 "vulkan.required.driverInfo").lower()
    reject_if("mesa" not in driver_info and "turnip" not in driver_info,
              "declaration-vulkan-identity-invalid", driver_info)
    reject_if(re.search(rf"(?<![0-9.]){re.escape(REQUIRED_MESA_VERSION)}(?![0-9.])",
                        driver_info) is None,
              "declaration-vulkan-semver-mismatch", driver_info)

    patches = require_exact_keys(top["patches"], {"requiredStates"}, "patches")
    required_states = require_mapping(patches["requiredStates"], "patches.requiredStates")
    reject_if(not required_states, "declaration-patch-states-empty")
    reject_if(any(not isinstance(name, str) or not name
                  or not isinstance(enabled, bool)
                  for name, enabled in required_states.items()),
              "declaration-patch-state-invalid")
    reject_if(required_states.get(REQUIRED_PATCH_ID) is not True,
              "declaration-sixty-fps-plus-required")

    markers = require_exact_keys(top["markers"],
                                 {"allowlist", "requiredPresent", "forbidden"}, "markers")
    allowlist = require_string_list(markers["allowlist"], "markers.allowlist")
    required_present = require_string_list(markers["requiredPresent"],
                                           "markers.requiredPresent")
    forbidden = require_string_list(markers["forbidden"], "markers.forbidden")
    for list_name, names in (("allowlist", allowlist),
                             ("requiredPresent", required_present),
                             ("forbidden", forbidden)):
        for name in names:
            validate_marker_name(name, f"markers.{list_name}")
    reject_if(not set(required_present).issubset(set(allowlist)),
              "declaration-required-marker-not-allowed")




    reject_if(not set(forbidden).issubset(set(allowlist)),
              "declaration-forbidden-marker-not-known",
              repr(sorted(set(forbidden) - set(allowlist))))
    reject_if(REQUIRED_PATCH_MARKER not in required_present,
              "declaration-sixty-fps-marker-required")
    reject_if(not MANDATORY_FORBIDDEN_MARKERS.issubset(set(forbidden)),
              "declaration-mandatory-forbidden-marker-missing",
              repr(sorted(MANDATORY_FORBIDDEN_MARKERS - set(forbidden))))

    validate_inventory_declarations(top["inventories"])

    surface = require_exact_keys(top["surface"],
                                 {"requiredComponent", "displayId", "identityRegex"},
                                 "surface")
    require_string(surface["requiredComponent"], "surface.requiredComponent")
    require_string(surface["displayId"], "surface.displayId")
    expression = require_regex(surface["identityRegex"], "surface.identityRegex")
    reject_if(set(re.compile(expression).groupindex) != {"layerId", "generation", "displayId"},
              "declaration-surface-regex-groups-invalid")

    display = require_exact_keys(top["display"], {"extractRegex", "requiredState"}, "display")
    display_expression = require_regex(display["extractRegex"], "display.extractRegex")
    required_display = require_mapping(display["requiredState"], "display.requiredState")
    reject_if(not required_display, "declaration-display-state-empty")
    reject_if(set(required_display) != set(re.compile(display_expression).groupindex),
              "declaration-display-groups-mismatch")
    for name, value in required_display.items():
        require_string(value, f"display.requiredState.{name}")

    environment = require_exact_keys(top["environment"], {"requiredRegexes"}, "environment")
    regexes = require_mapping(environment["requiredRegexes"],
                              "environment.requiredRegexes")
    reject_if(set(regexes) != {"thermal", "battery", "power", "cpuFreq", "gpuFreq"},
              "declaration-environment-keys-invalid")
    for name, value in regexes.items():
        expression = require_string(value, f"environment.requiredRegexes.{name}")
        try:
            re.compile(expression)
        except re.error as error:
            raise CaptureRejected("declaration-regex-invalid",
                                  f"environment.{name}: {error}") from error

    collection = require_exact_keys(top["collection"], COLLECTION_FIELDS, "collection")
    for name, value in collection.items():
        if name == "markerRoots":
            continue
        path = require_string(value, f"collection.{name}")
        provenance.validate_app_path(path)
    marker_roots = require_mapping(collection["markerRoots"],
                                   "collection.markerRoots")
    reject_if(dict(marker_roots) != provenance.CANONICAL_MARKER_ROOTS,
              "declaration-marker-roots-not-canonical")


class DualAgentChatGate:
    """Acquire both device aliases immediately before every adb read."""

    def __init__(self, runner: Any, script: pathlib.Path, *, agent: str, room: str):
        self._gates = tuple(
            provenance.AgentChatGate(
                runner, script, agent=agent, room=room, area=area
            )
            for area in (DEVICE_OWNERSHIP_AREA, ADB_OWNERSHIP_AREA)
        )

    def authorize(self, summary: str) -> None:
        for gate in self._gates:
            gate.authorize(summary)


def _require_stable_pair(actual: Mapping[str, Any], path: str) -> None:
    before = provenance.nested(actual, f"{path}.pre")
    after = provenance.nested(actual, f"{path}.post")
    reject_if(before != after, "candidate-observation-changed", path)


def build_candidate_expected(declaration: Mapping[str, Any],
                             actual: Mapping[str, Any]) -> dict[str, Any]:
    """Build and validate a nested expected-state candidate from an A/A capture."""
    validate_declaration(declaration)
    reject_if(actual.get("schema") != provenance.CAPTURE_SCHEMA,
              "candidate-capture-schema-invalid")

    device = provenance.nested(actual, "device")
    declared_device = declaration["device"]
    for field in ("serial", "soc", "board"):
        reject_if(device.get(field) != declared_device[field],
                  "candidate-device-mismatch", field)
    reject_if(device.get("bootIdPre") != device.get("bootIdPost"),
              "candidate-device-boot-changed")

    package = provenance.nested(actual, "package")
    reject_if(package.get("name") != declaration["package"]["name"],
              "candidate-package-mismatch")
    process = provenance.nested(actual, "process")
    for field in ("pid", "starttime", "cmdline", "uid", "foreground"):
        reject_if(process.get(f"{field}Pre") != process.get(f"{field}Post"),
                  "candidate-process-changed", field)
    reject_if(process.get("cmdlinePre") != declaration["process"]["cmdline"],
              "candidate-process-cmdline-mismatch")
    reject_if(process.get("foregroundPre")
              != declaration["process"]["foregroundComponent"],
              "candidate-foreground-mismatch")
    reject_if(process.get("uidPre") != package.get("uid"),
              "candidate-package-uid-mismatch")

    for name in ("backend", "vulkan"):
        _require_stable_pair(actual, name)
    backend = provenance.nested(actual, "backend.pre")
    vulkan = provenance.nested(actual, "vulkan.pre")
    provenance.validate_backend(backend)
    provenance.validate_vulkan(vulkan)
    provenance.require_exact_subset(backend, declaration["backend"]["required"], "backend")
    provenance.require_exact_subset(vulkan, declaration["vulkan"]["required"], "vulkan")

    settings = provenance.nested(actual, "settings")
    reject_if(settings.get("pre") != settings.get("post"),
              "candidate-settings-changed")
    settings_sha = provenance.canonical_sha256(settings.get("pre"))
    reject_if(settings.get("sha256") != settings_sha,
              "candidate-settings-hash-invalid")

    observed_libraries = provenance.nested(actual, "libraries")
    expected_libraries: dict[str, dict[str, str]] = {}
    for role in provenance.REQUIRED_LIBRARY_ROLES:
        item = require_mapping(observed_libraries.get(role), f"actual.libraries.{role}")
        basename = declaration["libraries"][role]["basename"]
        reject_if(pathlib.PurePosixPath(str(item.get("path", ""))).name != basename,
                  "candidate-library-basename-mismatch", role)
        digest = provenance.require_sha256(item.get("sha256"), f"libraries.{role}.sha256")
        expected_libraries[role] = {"basename": basename, "sha256": digest}
    loader = require_mapping(observed_libraries.get("vulkanLoader"),
                             "actual.libraries.vulkanLoader")
    reject_if(loader.get("path") != vulkan.get("path")
              or loader.get("sha256") != str(vulkan.get("sha256", "")).lower(),
              "candidate-vulkan-loader-mismatch")
    reject_if(observed_libraries["vulkanIcd"].get("path") != vulkan.get("path"),
              "candidate-vulkan-icd-not-selected")

    patches = provenance.nested(actual, "patches")
    states = require_mapping(patches.get("states"), "actual.patches.states")
    for patch_id, enabled in declaration["patches"]["requiredStates"].items():
        reject_if(states.get(patch_id) is not enabled,
                  "candidate-required-patch-state-mismatch", patch_id)
    reject_if(states.get(REQUIRED_PATCH_ID) is not True,
              "candidate-sixty-fps-plus-not-enabled")
    for field in ("runtimeMarker", "backendRuntimeMarker", "vulkanRuntimeMarker"):
        provenance.require_nonempty(patches.get(field), f"patches.{field}")

    markers = provenance.nested(actual, "markers")
    present = markers.get("present")
    reject_if(not isinstance(present, list) or present != sorted(set(present)),
              "candidate-marker-list-invalid")
    reject_if(markers.get("presentPost") != present
              or markers.get("preInventorySha256")
              != markers.get("postInventorySha256"),
              "candidate-marker-inventory-changed")
    allowlist = declaration["markers"]["allowlist"]
    required_present = set(declaration["markers"]["requiredPresent"])
    forbidden = set(declaration["markers"]["forbidden"])
    reject_if(bool(set(present) - set(allowlist)), "candidate-marker-not-allowed",
              repr(sorted(set(present) - set(allowlist))))
    reject_if(not required_present.issubset(set(present)),
              "candidate-required-marker-missing",
              repr(sorted(required_present - set(present))))
    reject_if(bool(forbidden & set(present)), "candidate-forbidden-marker-present",
              repr(sorted(forbidden & set(present))))

    expected_inventories: dict[str, dict[str, Any]] = {}
    actual_inventories = provenance.nested(actual, "inventories")
    for name in provenance.REQUIRED_INVENTORIES:
        item = require_mapping(actual_inventories.get(name), f"actual.inventories.{name}")
        reject_if(item.get("pre") != item.get("post"),
                  "candidate-inventory-changed", name)
        digest = provenance.require_sha256(item.get("sha256"),
                                           f"inventories.{name}.sha256")
        spec = declaration["inventories"][name]
        expected_inventories[name] = {
            "source": spec["source"], "roots": list(spec["roots"]), "sha256": digest,
        }

    surface = provenance.nested(actual, "surface")
    reject_if(surface.get("pre") != surface.get("post"),
              "candidate-surface-generation-changed")
    surface_pre = require_mapping(surface.get("pre"), "actual.surface.pre")
    reject_if(declaration["surface"]["requiredComponent"]
              not in str(surface_pre.get("layerName", "")),
              "candidate-surface-component-mismatch")
    reject_if(str(surface_pre.get("displayId")) != declaration["surface"]["displayId"],
              "candidate-surface-display-mismatch")

    display_state = provenance.nested(actual, "display.state")
    reject_if(display_state != declaration["display"]["requiredState"],
              "candidate-display-state-mismatch")

    expected = {
        "schema": provenance.EXPECTED_SCHEMA,
        "device": {
            "serial": declared_device["serial"],
            "soc": declared_device["soc"],
            "board": declared_device["board"],
            "adbTarget": declared_device["adbTarget"],
        },
        "package": {
            field: copy.deepcopy(package[field])
            for field in ("name", "uid", "versionCode", "versionName", "codePaths",
                          "baseApkSha256", "signerSha256")
        },
        "process": {
            "cmdline": declaration["process"]["cmdline"],
            "foregroundComponent": declaration["process"]["foregroundComponent"],
        },
        "libraries": expected_libraries,
        "backend": {



            "status": copy.deepcopy(backend),
            "statusSha256": provenance.canonical_sha256(backend),
            "required": copy.deepcopy(declaration["backend"]["required"]),
        },
        "vulkan": {
            "statusSha256": provenance.canonical_sha256(vulkan),
            "required": copy.deepcopy(declaration["vulkan"]["required"]),
        },
        "settings": {"sha256": settings_sha},
        "patches": {
            "catalogSha256": provenance.require_sha256(
                patches.get("catalogSha256"), "patches.catalogSha256"),
            "bundleSha256": provenance.require_sha256(
                patches.get("bundleSha256"), "patches.bundleSha256"),
            "states": dict(sorted(states.items())),
            "sixtyFpsPlus": True,
        },
        "markers": {
            "allowlist": list(allowlist),
            "present": list(present),
            "presentSha256": provenance.canonical_sha256(present),
        },
        "inventories": expected_inventories,
        "surface": copy.deepcopy(declaration["surface"]),
        "display": {
            "extractRegex": declaration["display"]["extractRegex"],
            "state": copy.deepcopy(display_state),
        },
        "environment": copy.deepcopy(declaration["environment"]),
        "collection": copy.deepcopy(declaration["collection"]),
    }
    provenance.validate_expected_state(expected)
    validation_capture = copy.deepcopy(actual)
    validation_capture["expectedStateSha256"] = provenance.canonical_sha256(expected)
    provenance.validate_snapshot(expected, validation_capture)
    return expected


def build_candidate_wrapper(expected: Mapping[str, Any],
                            actual: Mapping[str, Any]) -> dict[str, Any]:
    provenance.validate_expected_state(expected)
    return {
        "schema": CANDIDATE_SCHEMA,
        "accepted": False,
        "candidateOnly": True,
        "collectionSucceeded": True,
        "verdictAllowed": False,
        "reason": "manual-freeze-required",
        "expectedStateCandidateSha256": provenance.canonical_sha256(expected),
        "observedCaptureSha256": provenance.canonical_sha256(actual),
        "expectedStateCandidate": copy.deepcopy(expected),
        "manualReview": list(MANUAL_REVIEW),
    }


def collect_observed(declaration: Mapping[str, Any], store: Any, adb: Any,
                     runner: Any, apksigner: str) -> dict[str, Any]:
    """Perform only read operations and return the ordinary capture shape."""
    package_name = declaration["package"]["name"]
    collection = declaration["collection"]
    process_decl = declaration["process"]

    serial_pre = adb.text("pre-serial", ["shell", "getprop", "ro.serialno"]).strip()
    reject_if(serial_pre != declaration["device"]["serial"],
              "candidate-device-mismatch", "serial")
    soc_pre = adb.text("pre-soc", ["shell", "getprop", "ro.soc.model"]).strip()
    reject_if(soc_pre != declaration["device"]["soc"],
              "candidate-device-mismatch", "soc")
    board_pre = adb.text("pre-board", ["shell", "getprop", "ro.product.board"]).strip()
    reject_if(board_pre != declaration["device"]["board"],
              "candidate-device-mismatch", "board")
    boot_pre = adb.text("pre-boot-id", ["shell", "cat", "/proc/sys/kernel/random/boot_id"]).strip()
    process_pre = provenance.capture_process(
        adb, package_name, "pre", process_decl["foregroundComponent"],
        process_decl["cmdline"])
    maps_pre_text = adb.text("pre-maps", ["exec-out", "run-as", package_name,
                                           "cat", f"/proc/{process_pre['pid']}/maps"])
    maps_pre = provenance.mapped_paths(maps_pre_text)
    backend_pre = provenance.parse_json_object(adb.app_text(
        package_name, "pre-backend-status", collection["backendStatusPath"]),
        "backend pre")
    vulkan_pre = provenance.parse_json_object(adb.app_text(
        package_name, "pre-vulkan-status", collection["vulkanStatusPath"]),
        "vulkan pre")
    provenance.validate_backend(backend_pre)
    provenance.validate_vulkan(vulkan_pre)
    provenance.require_exact_subset(backend_pre, declaration["backend"]["required"],
                                    "backend")
    provenance.require_exact_subset(vulkan_pre, declaration["vulkan"]["required"],
                                    "vulkan")
    prefs_pre = provenance.normalize_preferences(adb.app_text(
        package_name, "pre-preferences", collection["preferencesPath"]))
    surface_pre, _surface_list_pre, surface_full_pre = provenance.capture_surface(
        adb, "pre", declaration["surface"])
    environment_start = provenance.capture_environment(adb, "start")

    code_paths = provenance.parse_pm_paths(
        adb.text("package-paths", ["shell", "pm", "path", package_name]), package_name)
    package_dump = adb.text("package-dump", ["shell", "dumpsys", "package", package_name])
    version_code, version_name = provenance.parse_package_version(package_dump)
    package_uid = provenance.parse_package_uid(package_dump)
    base_path = next(path for path in code_paths if path.endswith("/base.apk"))
    apk = adb.read("base-apk", ["exec-out", "cat", base_path], binary=True, timeout=300)
    apk_sha = provenance.sha256_bytes(apk)
    with tempfile.TemporaryDirectory(prefix="lsx4-candidate-apk-") as temporary:
        apk_file = pathlib.Path(temporary) / "base.apk"
        apk_file.write_bytes(apk)
        signer_result = runner.run([apksigner, "verify", "--print-certs", str(apk_file)],
                                   timeout=120)
        store.write("raw/apksigner.stdout.txt", signer_result.stdout)
        store.write("raw/apksigner.stderr.txt", signer_result.stderr)
        reject_if(signer_result.returncode != 0, "apksigner-verification-failed")
        signer_sha = provenance.parse_apksigner(
            signer_result.stdout.decode("utf-8", "replace"))
    catalog, catalog_raw, bundle_raw = provenance.extract_patch_catalog(apk, store)
    patch_states, patch_markers = provenance.bloodborne_patch_states(catalog, prefs_pre)
    reject_if(patch_markers.get(REQUIRED_PATCH_ID) != REQUIRED_PATCH_MARKER_BASENAME,
              "sixty-fps-catalog-marker-mismatch")

    marker_raw = adb.text(
        "marker-inventory-pre",
        provenance.adb_remote_shell_command(
            ["exec-out", "run-as", package_name],
            provenance.remote_marker_inventory_script(collection["markerRoots"]),
        ),
    )
    marker_inventory_pre = provenance.parse_remote_inventory(
        marker_raw, list(collection["markerRoots"].values()))
    present_markers_pre = provenance.marker_entries(
        marker_inventory_pre, collection["markerRoots"])
    inventory_pre = provenance.capture_inventory_set(
        adb, store, package_name, declaration["inventories"], "pre")

    logcat = adb.text("process-logcat", ["logcat", "-d",
                                          f"--pid={process_pre['pid']}", "-v", "epoch"])
    runtime_patch_marker = provenance.validate_patch_runtime_marker(logcat)
    runtime_status_markers = provenance.validate_runtime_status_markers(
        logcat, backend_pre, vulkan_pre)

    display_source = "\n".join((
        adb.text("display-dump", ["shell", "dumpsys", "display"]),
        adb.text("wm-size", ["shell", "wm", "size"]),
        adb.text("wm-density", ["shell", "wm", "density"]),
    ))
    display_state = provenance.extract_regex_state(
        display_source, declaration["display"]["extractRegex"], "display")

    libraries: dict[str, Any] = {}
    for role, spec in declaration["libraries"].items():
        path = provenance.select_mapped_library(maps_pre, spec, role)
        digest = provenance.parse_sha256sum(adb.text(
            f"library-sha-{role}", ["exec-out", "run-as", package_name,
                                    "sha256sum", "--", path]), path)
        libraries[role] = {
            "path": path, "sha256": digest, "mappedPre": True, "mappedPost": False,
            "mapsSha256Pre": provenance.mapped_file_identity(maps_pre_text, path),
            "mapsSha256Post": "",
        }
    loader_path = str(vulkan_pre.get("path", ""))
    reject_if(loader_path not in maps_pre, "vulkan-status-path-not-mapped", loader_path)
    loader_sha = provenance.parse_sha256sum(adb.text(
        "vulkan-loader-sha", ["exec-out", "run-as", package_name,
                              "sha256sum", "--", loader_path]), loader_path)
    libraries["vulkanLoader"] = {
        "path": loader_path, "sha256": loader_sha,
        "mappedPre": True, "mappedPost": False,
        "mapsSha256Pre": provenance.mapped_file_identity(maps_pre_text, loader_path),
        "mapsSha256Post": "",
    }

    environment_end = provenance.capture_environment(adb, "end")
    marker_post_raw = adb.text(
        "marker-inventory-post",
        provenance.adb_remote_shell_command(
            ["exec-out", "run-as", package_name],
            provenance.remote_marker_inventory_script(collection["markerRoots"]),
        ),
    )
    marker_inventory_post = provenance.parse_remote_inventory(
        marker_post_raw, list(collection["markerRoots"].values()))
    present_markers_post = provenance.marker_entries(
        marker_inventory_post, collection["markerRoots"])
    inventory_post = provenance.capture_inventory_set(
        adb, store, package_name, declaration["inventories"], "post")
    surface_post, _surface_list_post, surface_full_post = provenance.capture_surface(
        adb, "post", declaration["surface"])
    prefs_post = provenance.normalize_preferences(adb.app_text(
        package_name, "post-preferences", collection["preferencesPath"]))
    backend_post = provenance.parse_json_object(adb.app_text(
        package_name, "post-backend-status", collection["backendStatusPath"]),
        "backend post")
    vulkan_post = provenance.parse_json_object(adb.app_text(
        package_name, "post-vulkan-status", collection["vulkanStatusPath"]),
        "vulkan post")
    process_post = provenance.capture_process(
        adb, package_name, "post", process_decl["foregroundComponent"],
        process_decl["cmdline"])
    maps_post_text = adb.text("post-maps", ["exec-out", "run-as", package_name,
                                            "cat", f"/proc/{process_post['pid']}/maps"])
    maps_post = provenance.mapped_paths(maps_post_text)
    for item in libraries.values():
        item["mappedPost"] = item["path"] in maps_post
        if item["mappedPost"]:
            item["mapsSha256Post"] = provenance.mapped_file_identity(
                maps_post_text, item["path"])
    boot_post = adb.text("post-boot-id", ["shell", "cat", "/proc/sys/kernel/random/boot_id"]).strip()
    serial_post = adb.text("post-serial", ["shell", "getprop", "ro.serialno"]).strip()
    soc_post = adb.text("post-soc", ["shell", "getprop", "ro.soc.model"]).strip()
    board_post = adb.text("post-board", ["shell", "getprop", "ro.product.board"]).strip()

    reject_if(serial_pre != serial_post or soc_pre != soc_post or board_pre != board_post,
              "device-identity-changed")
    reject_if(surface_full_pre == surface_full_post and surface_pre != surface_post,
              "surface-parser-inconsistent")

    return {
        "schema": provenance.CAPTURE_SCHEMA,
        "capturedAtUnixNs": time.time_ns(),
        "device": {
            "serial": serial_pre, "soc": soc_pre, "board": board_pre,
            "bootIdPre": boot_pre, "bootIdPost": boot_post,
        },
        "package": {
            "name": package_name, "uid": package_uid, "versionCode": version_code,
            "versionName": version_name, "codePaths": code_paths,
            "baseApkSha256": apk_sha, "signerSha256": signer_sha,
        },
        "process": {
            **{f"{key}Pre": value for key, value in process_pre.items()},
            **{f"{key}Post": value for key, value in process_post.items()},
        },
        "libraries": libraries,
        "backend": {"pre": backend_pre, "post": backend_post},
        "vulkan": {"pre": vulkan_pre, "post": vulkan_post},
        "settings": {
            "pre": prefs_pre, "post": prefs_post,
            "sha256": provenance.canonical_sha256(prefs_pre),
        },
        "patches": {
            "catalogSha256": provenance.sha256_bytes(catalog_raw),
            "bundleSha256": provenance.sha256_bytes(bundle_raw),
            "states": patch_states,
            "runtimeMarker": runtime_patch_marker,
            "backendRuntimeMarker": runtime_status_markers["backend"],
            "vulkanRuntimeMarker": runtime_status_markers["vulkan"],
        },
        "markers": {
            "present": present_markers_pre,
            "presentPost": present_markers_post,
            "preInventorySha256": marker_inventory_pre["sha256"],
            "postInventorySha256": marker_inventory_post["sha256"],
        },
        "inventories": {
            name: {
                "pre": inventory_pre[name]["entries"],
                "post": inventory_post[name]["entries"],
                "sha256": inventory_pre[name]["sha256"],
            }
            for name in provenance.REQUIRED_INVENTORIES
        },
        "surface": {"pre": surface_pre, "post": surface_post},
        "display": {"state": display_state},
        "environment": {"start": environment_start, "end": environment_end},
    }


def load_declaration(path: pathlib.Path) -> dict[str, Any]:
    reject_if(path.is_symlink() or not path.is_file(),
              "declaration-file-invalid", str(path))
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CaptureRejected("declaration-file-unreadable", str(error)) from error
    reject_if(not isinstance(value, dict), "declaration-top-level-invalid")
    validate_declaration(value)
    return value


def write_success_artifacts(store: Any, declaration: Mapping[str, Any],
                            actual: Mapping[str, Any]) -> dict[str, Any]:
    expected = build_candidate_expected(declaration, actual)
    wrapper = build_candidate_wrapper(expected, actual)
    store.json("declaration.json", declaration)
    store.json("observed-capture.json", actual)
    store.json("candidate.json", wrapper)
    store.json("manifest.json", {
        "schema": MANIFEST_SCHEMA,
        "artifacts": store.manifest(),
    })
    return wrapper


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--declaration", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--apksigner")
    parser.add_argument(
        "--agentchat", type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[3] / "agentchat" / "agentchat.py")
    parser.add_argument("--agent", default="codex")
    parser.add_argument("--room", default="увеличить fps")
    args = parser.parse_args(argv)

    store = None
    try:
        declaration = load_declaration(args.declaration)
        reject_if(not args.agentchat.is_file(), "agentchat-script-missing",
                  str(args.agentchat))
        store = provenance.ArtifactStore(args.output)
        runner = provenance.CommandRunner()
        gate = DualAgentChatGate(
            runner, args.agentchat, agent=args.agent, room=args.room)
        adb = provenance.AdbReader(
            runner, gate, store, args.adb, declaration["device"]["adbTarget"])
        actual = collect_observed(
            declaration, store, adb, runner,
            provenance.resolve_apksigner(args.apksigner))
        wrapper = write_success_artifacts(store, declaration, actual)
        store.seal()
        print(json.dumps({
            "accepted": False,
            "candidateOnly": True,
            "verdictAllowed": False,
            "reason": "manual-freeze-required",
            "output": str(args.output.resolve()),
            "candidateSha256": wrapper["expectedStateCandidateSha256"],
        }, ensure_ascii=False, sort_keys=True))
        return 0
    except CaptureRejected as error:
        payload = {
            "accepted": False,
            "candidateOnly": True,
            "verdictAllowed": False,
            "reason": error.reason,
            "detail": error.detail,
        }
        if store is not None:
            try:
                store.json("REJECTED.json", payload)
                store.json("partial-manifest.json", {
                    "schema": MANIFEST_SCHEMA,
                    "artifacts": store.manifest(),
                })
                store.seal()
            except CaptureRejected:
                pass
        print(json.dumps(payload, ensure_ascii=False, sort_keys=True), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
