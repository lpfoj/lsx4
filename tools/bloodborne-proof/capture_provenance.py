#!/usr/bin/env python3
"""Read-only, fail-closed provenance capture for Bloodborne device proofs.

This program deliberately does not launch, stop, configure, or otherwise mutate
LSX4 or the device.  Every adb read is preceded by an AgentChat event and an
ownership check for ``device:vivo``.  A capture is accepted only when it matches
an already-written expected-state JSON file and the dynamic identity is stable
across two snapshots.

The expected-state document is intentionally strict.  Its top-level schema is
``lsx4-bloodborne-expected-state/v1`` and it contains these required objects:

``device``
    ``serial``, ``soc``, ``board`` and ``adbTarget``.
``package``
    ``name``, ``uid``, ``versionCode``, ``versionName``, ``codePaths``,
    ``baseApkSha256`` and ``signerSha256``.
``process``
    ``cmdline`` and ``foregroundComponent`` (the pid/starttime/uid are captured
    dynamically and must be identical before and after the read-only capture).
``libraries``
    Entries named ``executor``, ``adapter``, ``bridge``, ``fexCore`` and
    ``vulkanIcd``.  Each entry has a mapped ``path`` or ``basename`` and the
    expected ``sha256``.
``backend`` / ``vulkan``
    Backend contains the complete canonical ``status`` payload, its
    ``statusSha256``, and a ``required`` exact-field object.  Vulkan contains
    its status hash and required subset.  The status files are read twice.
    Fallback, unlocked, transient, or unmapped Vulkan state is rejected even if
    the caller accidentally expected it.
``settings``
    ``sha256`` of normalized Android SharedPreferences.
``patches``
    Installed-APK ``catalogSha256``, ``bundleSha256``, exact enabled state for
    every Bloodborne patch in ``states``, and ``sixtyFpsPlus=true``.
``markers``
    Complete ``allowlist`` and exact ``present`` set for marker-looking files in
    the native root.  Unknown run/diagnostic/patch markers are fatal.
``inventories``
    Exactly ``route``, ``input``, ``save``, ``pipelineCache`` and ``fexCache``.
    Each has ``source`` (``host`` or ``app``), ``roots`` and expected canonical
    ``sha256``.  Symlinks and special files are rejected.
``surface`` / ``display``
    Stable required BLAST component/display state and regular expressions with
    named capture groups.  The exact layer name and generation are captured
    dynamically and must remain identical only inside one provenance phase;
    a fresh-process generation is not frozen across A/B/A phases.
``environment``
    Per-artifact required regular expressions for the thermal/frequency/power
    snapshots.  These values are recorded, not used as an equality shortcut.
``collection``
    Read-only app-relative status/preferences/marker paths.

No partial directory is ever reused and every emitted artifact is covered by a
SHA-256 manifest.  ``provenance.json`` is therefore evidence, not a best-effort
telemetry summary.
"""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import io
import json
import os
import pathlib
import re
import stat
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile
from collections.abc import Mapping, Sequence
from typing import Any


EXPECTED_SCHEMA = "lsx4-bloodborne-expected-state/v1"
CAPTURE_SCHEMA = "lsx4-bloodborne-provenance/v1"
REQUIRED_INVENTORIES = {
    "route", "input", "save", "pipelineCache", "fexCache"
}
REQUIRED_LIBRARY_ROLES = {
    "executor", "adapter", "bridge", "fexCore", "vulkanIcd"
}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SAFE_REMOTE_RE = re.compile(r"^[A-Za-z0-9_./:+-]+$")
MARKER_NAME_RE = re.compile(r"^(?:run-|lsx4-bb-)[A-Za-z0-9_.+-]+$")





MARKER_ARTIFACT_NAME_RE = re.compile(
    r"^(?:run-|lsx4-bb-|diag-)[A-Za-z0-9_.+-]+$"
)
EXPLICIT_DIAGNOSTIC_MARKERS = frozenset({
    "diag-fex-proof-metrics-static",
    "diag-fex-proof-metrics-dynamic",
})
CANONICAL_MARKER_ROOTS = {
    "runtime": "files/lsx4-home",
    "patch": "files/lsx4-home/native",
}


class CaptureRejected(RuntimeError):
    """Raised when the capture cannot prove the declared state."""

    def __init__(self, reason: str, detail: str | None = None):
        self.reason = reason
        self.detail = detail
        super().__init__(reason if detail is None else f"{reason}: {detail}")


def reject_if(condition: bool, reason: str, detail: str | None = None) -> None:
    if condition:
        raise CaptureRejected(reason, detail)


def canonical_json(value: Any) -> bytes:
    return (json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ) + "\n").encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def canonical_sha256(value: Any) -> str:
    return sha256_bytes(canonical_json(value))


def require_sha256(value: Any, field: str) -> str:
    reject_if(not isinstance(value, str) or not SHA256_RE.fullmatch(value),
              "invalid-sha256", field)
    return value


def require_nonempty(value: Any, field: str) -> Any:
    reject_if(value is None, "expected-state-empty", field)
    if isinstance(value, str):
        reject_if(not value.strip() or value.strip().lower() in {"unknown", "unset", "n/a"},
                  "expected-state-empty", field)
    elif isinstance(value, (list, tuple, dict)):
        reject_if(not value, "expected-state-empty", field)
    return value


def reject_unknown_tree(value: Any, path: str = "expected") -> None:
    if value is None:
        raise CaptureRejected("expected-state-empty", path)
    if isinstance(value, str):
        require_nonempty(value, path)
        return
    if isinstance(value, Mapping):
        reject_if(not value, "expected-state-empty", path)
        for key, child in value.items():
            reject_if(not isinstance(key, str) or not key,
                      "expected-key-invalid", f"{path}:{key!r}")
            reject_unknown_tree(child, f"{path}.{key}")
        return
    if isinstance(value, (list, tuple)):
        reject_if(not value, "expected-state-empty", path)
        for index, child in enumerate(value):
            reject_unknown_tree(child, f"{path}[{index}]")


def nested(value: Mapping[str, Any], path: str) -> Any:
    current: Any = value
    for part in path.split("."):
        reject_if(not isinstance(current, Mapping) or part not in current,
                  "field-missing", path)
        current = current[part]
    return current


def require_exact_subset(actual: Mapping[str, Any], expected: Mapping[str, Any],
                         prefix: str) -> None:
    for name, wanted in expected.items():
        path = f"{prefix}.{name}" if prefix else name
        reject_if(name not in actual, "expected-field-missing", path)
        got = actual[name]
        if isinstance(wanted, Mapping):
            reject_if(not isinstance(got, Mapping), "expected-field-type-mismatch", path)
            require_exact_subset(got, wanted, path)
        else:
            reject_if(got != wanted, "expected-state-mismatch",
                      f"{path}: expected={wanted!r} actual={got!r}")


def validate_expected_state(expected: Mapping[str, Any]) -> None:
    reject_unknown_tree(expected)
    reject_if(expected.get("schema") != EXPECTED_SCHEMA, "expected-schema-invalid")
    for path in (
        "device.serial", "device.soc", "device.board", "device.adbTarget",
        "package.name", "package.uid", "package.versionCode", "package.versionName",
        "package.codePaths", "package.baseApkSha256", "package.signerSha256",
        "process.cmdline", "process.foregroundComponent",
        "backend.status", "backend.statusSha256", "backend.required",
        "vulkan.statusSha256", "vulkan.required",
        "settings.sha256", "patches.catalogSha256", "patches.bundleSha256",
        "patches.states", "markers.allowlist", "markers.present",
        "markers.presentSha256",
        "surface.requiredComponent", "surface.displayId", "surface.identityRegex",
        "display.extractRegex", "display.state", "environment.requiredRegexes",
        "collection.backendStatusPath", "collection.vulkanStatusPath",
        "collection.preferencesPath", "collection.markerRoots",
    ):
        require_nonempty(nested(expected, path), path)
    for path in (
        "package.baseApkSha256", "package.signerSha256", "backend.statusSha256",
        "vulkan.statusSha256", "settings.sha256", "patches.catalogSha256",
        "patches.bundleSha256", "markers.presentSha256",
    ):
        require_sha256(nested(expected, path), path)
    expected_backend_status = nested(expected, "backend.status")
    reject_if(not isinstance(expected_backend_status, Mapping),
              "expected-backend-status-payload-invalid")
    validate_backend(expected_backend_status)
    reject_if(canonical_sha256(expected_backend_status) !=
              nested(expected, "backend.statusSha256"),
              "expected-backend-status-payload-hash-mismatch")
    require_exact_subset(expected_backend_status,
                         nested(expected, "backend.required"),
                         "backend")
    libraries = nested(expected, "libraries")
    reject_if(set(libraries) != REQUIRED_LIBRARY_ROLES,
              "expected-library-roles-incomplete",
              repr(sorted(set(libraries) ^ REQUIRED_LIBRARY_ROLES)))
    for role, spec in libraries.items():
        reject_if(not isinstance(spec, Mapping), "library-spec-invalid", role)
        reject_if(not spec.get("path") and not spec.get("basename"),
                  "library-location-missing", role)
        require_sha256(spec.get("sha256"), f"libraries.{role}.sha256")
    inventories = nested(expected, "inventories")
    reject_if(set(inventories) != REQUIRED_INVENTORIES,
              "expected-inventories-incomplete",
              repr(sorted(set(inventories) ^ REQUIRED_INVENTORIES)))
    for name, spec in inventories.items():
        reject_if(not isinstance(spec, Mapping), "inventory-spec-invalid", name)
        reject_if(spec.get("source") not in {"host", "app"},
                  "inventory-source-invalid", name)
        roots = require_nonempty(spec.get("roots"), f"inventories.{name}.roots")
        reject_if(not all(isinstance(item, str) and item for item in roots),
                  "inventory-root-invalid", name)
        require_sha256(spec.get("sha256"), f"inventories.{name}.sha256")
    allowlist = nested(expected, "markers.allowlist")
    present = nested(expected, "markers.present")
    reject_if(len(set(allowlist)) != len(allowlist), "marker-allowlist-duplicate")
    reject_if(not set(present).issubset(set(allowlist)), "marker-present-not-allowed")
    reject_if(nested(expected, "patches.sixtyFpsPlus") is not True,
              "sixty-fps-plus-not-predeclared")
    for key, expression in nested(expected, "environment.requiredRegexes").items():
        require_nonempty(expression, f"environment.requiredRegexes.{key}")
        try:
            re.compile(expression)
        except re.error as error:
            raise CaptureRejected("environment-regex-invalid", f"{key}: {error}") from error
    for path in nested(expected, "collection").values():
        if isinstance(path, str):
            reject_if(not SAFE_REMOTE_RE.fullmatch(path) or path.startswith("/") or ".." in path,
                      "unsafe-app-relative-path", path)
    reject_if(nested(expected, "collection.markerRoots") != CANONICAL_MARKER_ROOTS,
              "marker-roots-not-canonical",
              str(nested(expected, "collection.markerRoots")))


@dataclasses.dataclass
class CommandResult:
    argv: list[str]
    returncode: int
    stdout: bytes
    stderr: bytes


class CommandRunner:
    def run(self, argv: Sequence[str], *, timeout: int = 60) -> CommandResult:
        try:
            result = subprocess.run(
                list(argv), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                check=False, timeout=timeout,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise CaptureRejected("command-execution-failed", f"{argv[0]}: {error}") from error
        return CommandResult(list(argv), result.returncode, result.stdout, result.stderr)


class ArtifactStore:
    def __init__(self, root: pathlib.Path):
        reject_if(root.exists(), "output-already-exists", str(root))
        root.mkdir(parents=True, exist_ok=False)
        self.root = root
        self.raw = root / "raw"
        self.raw.mkdir()
        self._hashes: dict[str, str] = {}

    def write(self, relative: str, payload: bytes) -> pathlib.Path:
        reject_if(not relative or relative.startswith(("/", "\\")) or ".." in pathlib.PurePosixPath(relative).parts,
                  "unsafe-artifact-path", relative)
        destination = self.root / pathlib.PurePosixPath(relative)
        destination.parent.mkdir(parents=True, exist_ok=True)
        try:
            with destination.open("xb") as stream:
                stream.write(payload)
        except FileExistsError as error:
            raise CaptureRejected("artifact-overwrite-refused", relative) from error
        self._hashes[relative] = sha256_bytes(payload)
        return destination

    def text(self, relative: str, value: str) -> pathlib.Path:
        return self.write(relative, value.encode("utf-8"))

    def json(self, relative: str, value: Any) -> pathlib.Path:
        return self.write(relative, canonical_json(value))

    def manifest(self) -> dict[str, str]:
        return dict(sorted(self._hashes.items()))

    def seal(self) -> None:
        for path in sorted(self.root.rglob("*"), reverse=True):
            if path.is_file():
                try:
                    path.chmod(stat.S_IREAD)
                except OSError:
                    pass


class AgentChatGate:
    def __init__(self, runner: CommandRunner, script: pathlib.Path, *, agent: str,
                 room: str, area: str = "device:vivo",
                 additional_areas: Sequence[str] = ()):
        self.runner = runner
        self.script = script
        self.agent = agent
        self.room = room
        self.area = area
        self.areas = (area, *tuple(additional_areas))

    def authorize(self, summary: str) -> None:
        base = [sys.executable, str(self.script)]
        event = self.runner.run(base + [
            "event", "--agent", self.agent, "--room", self.room,
            "--action", "inspect", "--target", self.area, "--summary", summary,
        ])
        reject_if(event.returncode != 0, "agentchat-event-failed",
                  event.stderr.decode("utf-8", "replace").strip())
        for area in self.areas:
            own = self.runner.run(base + [
                "own", "--agent", self.agent, "--room", self.room,
                "--area", area,
            ])
            reject_if(own.returncode != 0, "agentchat-ownership-denied",
                      (own.stdout + own.stderr).decode("utf-8", "replace").strip())


class AdbReader:
    """The only route through which this module invokes adb."""

    def __init__(self, runner: CommandRunner, gate: AgentChatGate,
                 store: ArtifactStore, adb: str, serial: str):
        self.runner = runner
        self.gate = gate
        self.store = store
        self.adb = adb
        self.serial = serial
        self.sequence = 0

    def read(self, label: str, arguments: Sequence[str], *, binary: bool = False,
             timeout: int = 60, allow_empty: bool = False) -> bytes:
        self.gate.authorize(f"read-only Bloodborne provenance: {label}")
        result = self.runner.run([self.adb, "-s", self.serial, *arguments], timeout=timeout)
        self.sequence += 1
        stem = f"raw/{self.sequence:03d}-{re.sub('[^A-Za-z0-9_.-]', '_', label)}"
        if not binary:
            self.store.write(stem + ".stdout.txt", result.stdout)
        self.store.write(stem + ".stderr.txt", result.stderr)
        reject_if(result.returncode != 0, "adb-read-failed",
                  f"{label}: {result.stderr.decode('utf-8', 'replace').strip()}")
        reject_if(not allow_empty and not result.stdout.strip(), "adb-read-empty", label)
        return result.stdout

    def text(self, label: str, arguments: Sequence[str], **kwargs: Any) -> str:
        return self.read(label, arguments, **kwargs).decode("utf-8", "strict").replace("\r\n", "\n")

    def app_text(self, package: str, label: str, path: str) -> str:
        validate_app_path(path)
        return self.text(label, ["exec-out", "run-as", package, "cat", path])


def adb_remote_shell_command(prefix: Sequence[str], script: str) -> list[str]:
    """Build an adb command whose complete script survives its transport.

    ``adb shell`` adds an outer Android-shell parse, so its script needs one
    POSIX quoting layer.  ``adb exec-out run-as ...`` preserves the ``sh -c``
    script argument; adding that same layer there makes the quotes literal and
    asks sh to execute the whole script as a filename.  Keep the distinction
    explicit and reject unknown transports.
    """

    reject_if(not script or "\x00" in script, "remote-shell-script-invalid")
    reject_if(not prefix or prefix[0] not in {"shell", "exec-out"},
              "remote-shell-prefix-invalid", repr(list(prefix)))
    if prefix[0] == "shell":
        script = "'" + script.replace("'", "'\\''") + "'"
    return [*prefix, "sh", "-c", script]


def validate_app_path(path: str) -> None:
    reject_if(not isinstance(path, str) or not SAFE_REMOTE_RE.fullmatch(path)
              or path.startswith("/") or ".." in path,
              "unsafe-app-relative-path", repr(path))


def parse_proc_stat(text: str) -> tuple[int, str, int]:
    line = text.strip()
    match = re.fullmatch(r"(\d+) \((.*)\) ([A-Za-z]) (.*)", line)
    reject_if(match is None, "proc-stat-malformed")
    assert match is not None
    tail = match.group(4).split()
    reject_if(len(tail) < 19, "proc-stat-truncated")
    try:
        starttime = int(tail[18])
    except ValueError as error:
        raise CaptureRejected("proc-starttime-invalid") from error
    reject_if(starttime <= 0, "proc-starttime-invalid")
    return int(match.group(1)), match.group(2), starttime


def parse_uid(status: str) -> int:
    match = re.search(r"^Uid:\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$", status,
                      flags=re.MULTILINE)
    reject_if(match is None, "proc-uid-missing")
    assert match is not None
    values = [int(item) for item in match.groups()]
    reject_if(len(set(values)) != 1, "proc-uid-inconsistent", repr(values))
    return values[0]


def parse_json_object(text: str, name: str) -> dict[str, Any]:
    try:
        value = json.loads(text)
    except json.JSONDecodeError as error:
        raise CaptureRejected("json-artifact-invalid", f"{name}: {error}") from error
    reject_if(not isinstance(value, dict) or not value, "json-artifact-empty", name)
    return value


def normalize_preferences(text: str) -> dict[str, Any]:
    try:
        root = ET.fromstring(text)
    except ET.ParseError as error:
        raise CaptureRejected("preferences-xml-invalid", str(error)) from error
    reject_if(root.tag != "map", "preferences-root-invalid", root.tag)
    result: dict[str, Any] = {}
    for child in root:
        name = child.attrib.get("name")
        reject_if(not name or name in result, "preferences-key-invalid", repr(name))
        if child.tag == "string":
            value: Any = child.text or ""
        elif child.tag == "boolean":
            raw = child.attrib.get("value")
            reject_if(raw not in {"true", "false"}, "preferences-boolean-invalid", name)
            value = raw == "true"
        elif child.tag in {"int", "long"}:
            try:
                value = int(child.attrib["value"])
            except (KeyError, ValueError) as error:
                raise CaptureRejected("preferences-integer-invalid", name) from error
        elif child.tag == "float":
            try:
                value = float(child.attrib["value"])
            except (KeyError, ValueError) as error:
                raise CaptureRejected("preferences-float-invalid", name) from error
        elif child.tag == "set":
            reject_if(any(item.tag != "string" for item in child),
                      "preferences-set-invalid", name)
            value = sorted((item.text or "") for item in child)
        else:
            raise CaptureRejected("preferences-type-unknown", f"{name}:{child.tag}")
        result[name] = {"type": child.tag, "value": value}
    reject_if(not result, "preferences-empty")
    return result


def preference_bool(preferences: Mapping[str, Any], key: str, default: bool) -> bool:
    item = preferences.get(key)
    if item is None:
        return default
    reject_if(not isinstance(item, Mapping) or item.get("type") != "boolean"
              or not isinstance(item.get("value"), bool),
              "patch-preference-not-boolean", key)
    return bool(item["value"])


def parse_apksigner(text: str) -> str:
    digests = re.findall(
        r"^(?:Signer #\d+ certificate|V\d+\.\d+ Signer: certificate) "
        r"SHA-256 digest:\s*"
        r"([0-9A-Fa-f:]{64,95})\s*$",
        text,
        flags=re.MULTILINE,
    )
    normalized = {item.replace(":", "").lower() for item in digests}
    reject_if(len(normalized) != 1
              or not SHA256_RE.fullmatch(next(iter(normalized), "")),
              "apk-signer-digest-unavailable", repr(sorted(normalized)))
    return next(iter(normalized))


def parse_pm_paths(text: str, package: str) -> list[str]:
    prefix = "package:"
    paths = [line[len(prefix):].strip() for line in text.splitlines()
             if line.startswith(prefix)]
    reject_if(not paths or any(not item.startswith("/") for item in paths),
              "package-code-paths-invalid", package)
    reject_if(len(set(paths)) != len(paths), "package-code-path-duplicate")
    reject_if(sum(path.endswith("/base.apk") for path in paths) != 1,
              "package-base-apk-ambiguous", repr(paths))
    return sorted(paths)


def parse_package_version(text: str) -> tuple[int, str]:
    codes = re.findall(r"\bversionCode=(\d+)\b", text)
    names = re.findall(r"^\s*versionName=([^\s]+)\s*$", text, flags=re.MULTILINE)
    reject_if(not codes or not names, "package-version-missing")
    reject_if(len(set(codes)) != 1 or len(set(names)) != 1,
              "package-version-ambiguous")
    return int(codes[0]), names[0]


def parse_package_uid(text: str) -> int:



    values = re.findall(r"^\s*(?:userId|appId)=(\d+)\s*$", text,
                        flags=re.MULTILINE)
    reject_if(not values or len(set(values)) != 1, "package-uid-missing-or-ambiguous")
    return int(values[0])


def mapped_paths(maps_text: str) -> set[str]:
    result: set[str] = set()
    for line in maps_text.splitlines():
        fields = line.split(None, 5)
        if len(fields) < 6:
            continue
        path = fields[5]
        if path.startswith("/"):
            result.add(path)
    reject_if(not result, "process-maps-empty")
    return result


def mapped_file_identity(maps_text: str, path: str) -> str:
    lines = [line.strip() for line in maps_text.splitlines()
             if len(line.split(None, 5)) >= 6 and line.split(None, 5)[5] == path]
    reject_if(not lines, "mapped-library-lines-missing", path)
    return canonical_sha256(lines)


def select_mapped_library(paths: set[str], spec: Mapping[str, Any], role: str) -> str:
    deleted_suffix = " (deleted)"
    if spec.get("path"):
        wanted = str(spec["path"])
        deleted = [path for path in paths
                   if path.endswith(deleted_suffix)
                   and path[:-len(deleted_suffix)] == wanted]
        candidates = [path for path in paths
                      if not path.endswith(deleted_suffix) and path == wanted]
    else:
        wanted = str(spec.get("basename", ""))
        deleted = [path for path in paths
                   if path.endswith(deleted_suffix)
                   and pathlib.PurePosixPath(path[:-len(deleted_suffix)]).name == wanted]
        candidates = [path for path in paths
                      if not path.endswith(deleted_suffix)
                      and pathlib.PurePosixPath(path).name == wanted]
    reject_if(bool(deleted), "mapped-library-deleted",
              f"{role}:{wanted}:{sorted(deleted)!r}")
    reject_if(len(candidates) != 1, "mapped-library-not-unique",
              f"{role}:{wanted}:{sorted(candidates)!r}")
    return candidates[0]


def parse_sha256sum(text: str, path: str) -> str:
    match = re.match(r"^([0-9a-fA-F]{64})\s+", text.strip())
    reject_if(match is None, "remote-sha256-invalid", path)
    assert match is not None
    return match.group(1).lower()


def local_inventory(roots: Sequence[str]) -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    for root_index, raw_root in enumerate(roots):
        root = pathlib.Path(raw_root)
        reject_if(root.is_symlink(), "inventory-symlink", str(root))
        reject_if(not root.exists(), "inventory-root-missing", str(root))
        base_label = f"root{root_index}"

        def add(path: pathlib.Path, relative: pathlib.PurePosixPath) -> None:
            info = path.lstat()
            reject_if(stat.S_ISLNK(info.st_mode), "inventory-symlink", str(path))
            if stat.S_ISDIR(info.st_mode):
                entries.append({"root": base_label, "path": relative.as_posix(), "type": "dir"})
                for child in sorted(path.iterdir(), key=lambda item: item.name):
                    add(child, relative / child.name)
            elif stat.S_ISREG(info.st_mode):
                digest = hashlib.sha256()
                with path.open("rb") as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(block)
                entries.append({
                    "root": base_label, "path": relative.as_posix(), "type": "file",
                    "size": info.st_size, "sha256": digest.hexdigest(),
                })
            else:
                raise CaptureRejected("inventory-special-file", str(path))

        add(root, pathlib.PurePosixPath(root.name))
    entries.sort(key=lambda item: (item["root"], item["path"]))
    reject_if(not entries, "inventory-empty")
    return {"entries": entries, "sha256": canonical_sha256(entries)}


def remote_inventory_script(roots: Sequence[str]) -> str:
    for root in roots:
        validate_app_path(root)
    quoted = " ".join("'" + root + "'" for root in roots)
    return (
        "set -eu; for root in " + quoted + "; do "
        "if [ ! -e \"$root\" ]; then printf 'M\\t%s\\n' \"$root\"; continue; fi; "
        "find \"$root\" -xdev -print | sort | while IFS= read -r p; do "
        "case \"$p\" in *\"$(printf '\\t')\"*) printf 'S\\tunsafe-name\\n'; continue;; esac; "
        "if [ -L \"$p\" ]; then printf 'L\\t%s\\n' \"$p\"; "
        "elif [ -d \"$p\" ]; then printf 'D\\t%s\\n' \"$p\"; "
        "elif [ -f \"$p\" ]; then size=$(stat -c %s \"$p\"); "
        "sha=$(sha256sum \"$p\" | awk '{print $1}'); "
        "printf 'F\\t%s\\t%s\\t%s\\n' \"$size\" \"$sha\" \"$p\"; "
        "else printf 'S\\t%s\\n' \"$p\"; fi; done; done"
    )


def remote_marker_inventory_script(marker_roots: Mapping[str, str]) -> str:
    """Inventory only direct marker-looking children, never the runtime tree."""
    reject_if(dict(marker_roots) != CANONICAL_MARKER_ROOTS,
              "marker-roots-not-canonical", repr(dict(marker_roots)))
    for marker_root in marker_roots.values():
        validate_app_path(marker_root)
    quoted = " ".join("'" + root + "'" for root in marker_roots.values())
    return (
        "set -eu; for root in " + quoted + "; do test -d \"$root\"; "
        "for p in \"$root\"/run-* \"$root\"/lsx4-bb-* \"$root\"/diag-*; do "
        "[ -e \"$p\" ] || [ -L \"$p\" ] || continue; "
        "case \"$p\" in *\"$(printf '\\t')\"*) printf 'S\\tunsafe-name\\n'; continue;; esac; "
        "if [ -L \"$p\" ]; then printf 'L\\t%s\\n' \"$p\"; "
        "elif [ -f \"$p\" ]; then size=$(stat -c %s \"$p\"); "
        "sha=$(sha256sum \"$p\" | awk '{print $1}'); "
        "printf 'F\\t%s\\t%s\\t%s\\n' \"$size\" \"$sha\" \"$p\"; "
        "else printf 'S\\t%s\\n' \"$p\"; fi; done; done"
    )


def parse_remote_inventory(text: str, roots: Sequence[str]) -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    for line in text.splitlines():
        parts = line.split("\t")
        if parts[0] in {"M", "L", "S"}:
            raise CaptureRejected({
                "M": "inventory-root-missing",
                "L": "inventory-symlink",
                "S": "inventory-special-file",
            }[parts[0]], "\t".join(parts[1:]))
        if parts[0] == "D" and len(parts) == 2:
            entries.append({"path": parts[1], "type": "dir"})
        elif parts[0] == "F" and len(parts) == 4:
            try:
                size = int(parts[1])
            except ValueError as error:
                raise CaptureRejected("inventory-size-invalid", line) from error
            require_sha256(parts[2].lower(), parts[3])
            entries.append({
                "path": parts[3], "type": "file", "size": size,
                "sha256": parts[2].lower(),
            })
        else:
            raise CaptureRejected("inventory-line-invalid", line)
    entries.sort(key=lambda item: item["path"])
    reject_if(not entries, "inventory-empty")
    for root in roots:
        reject_if(not any(item["path"] == root or item["path"].startswith(root + "/")
                          for item in entries), "inventory-root-uncovered", root)
    return {"entries": entries, "sha256": canonical_sha256(entries)}


def extract_patch_catalog(apk: bytes, store: ArtifactStore) -> tuple[dict[str, Any], bytes, bytes]:
    try:
        with zipfile.ZipFile(io.BytesIO(apk)) as archive:
            catalog = archive.read("assets/game_patches.json")
            bundle = archive.read("assets/patches/lsx4-bundled/Bloodborne.xml")
    except (zipfile.BadZipFile, KeyError) as error:
        raise CaptureRejected("installed-apk-patch-assets-missing", str(error)) from error
    store.write("raw/installed-game_patches.json", catalog)
    store.write("raw/installed-Bloodborne.xml", bundle)
    parsed = parse_json_object(catalog.decode("utf-8"), "installed game patch catalog")
    return parsed, catalog, bundle


def bloodborne_patch_states(catalog: Mapping[str, Any],
                            preferences: Mapping[str, Any]) -> tuple[dict[str, bool], dict[str, str]]:
    games = catalog.get("games")
    reject_if(not isinstance(games, list), "patch-catalog-games-missing")
    matches = [game for game in games if isinstance(game, Mapping)
               and game.get("name") == "Bloodborne"]
    reject_if(len(matches) != 1, "bloodborne-patch-catalog-ambiguous")
    patches = matches[0].get("patches")
    reject_if(not isinstance(patches, list) or not patches, "bloodborne-patches-empty")
    states: dict[str, bool] = {}
    markers: dict[str, str] = {}
    for patch in patches:
        reject_if(not isinstance(patch, Mapping), "patch-catalog-entry-invalid")
        patch_id = str(require_nonempty(patch.get("id"), "patch.id"))
        key = str(require_nonempty(patch.get("preferenceKey"), f"patch.{patch_id}.key"))
        reject_if(patch_id in states, "patch-id-duplicate", patch_id)
        inverted = patch.get("inverted", True)
        reject_if(not isinstance(inverted, bool), "patch-inverted-invalid", patch_id)
        stored = preference_bool(preferences, key, inverted)
        states[patch_id] = not stored if inverted else stored
        marker = patch.get("marker", "")
        if marker:
            reject_if(not isinstance(marker, str) or not MARKER_NAME_RE.fullmatch(marker),
                      "patch-marker-invalid", patch_id)
            markers[patch_id] = marker
    return dict(sorted(states.items())), markers


def validate_backend(status: Mapping[str, Any]) -> None:
    for field in ("schema", "requested", "effective", "fallbackReason"):
        require_nonempty(status.get(field), f"backend.{field}")
    reject_if(status.get("requested") != "fex-experimental"
              or status.get("effective") != "fex-experimental",
              "backend-not-fex")
    reject_if(status.get("fallbackReason") != "none", "backend-fallback-active")
    reject_if(status.get("supported") is not True, "backend-not-supported")
    reject_if(status.get("selectionLocked") is not True, "backend-selection-unlocked")
    if "source" in status:
        reject_if(any(token in str(status.get("source", "")).lower()
                      for token in ("default", "pending", "probe")),
                  "backend-status-transient", str(status.get("source")))


def validate_vulkan(status: Mapping[str, Any]) -> None:
    required = (
        "schema", "requested", "effective", "fallbackReason", "driverName",
        "driverInfo", "driverVersion", "apiVersion", "requestedPath",
        "requestedSha256", "requestedPackageSource", "path", "sha256",
        "packageSource",
    )
    for field in required:
        require_nonempty(status.get(field), f"vulkan.{field}")
    reject_if(status.get("effective") != status.get("requested"),
              "vulkan-route-mismatch")
    reject_if(status.get("requestedPath") != status.get("path"),
              "vulkan-selected-path-mismatch")
    reject_if(str(status.get("requestedSha256", "")).lower()
              != str(status.get("sha256", "")).lower(),
              "vulkan-selected-hash-mismatch")
    reject_if(status.get("fallbackReason") != "none", "vulkan-fallback-active")
    reject_if(status.get("selectionLocked") is not True, "vulkan-selection-unlocked")
    reject_if(status.get("mappingVerified") is not True, "vulkan-mapping-unverified")
    if "source" in status:
        reject_if(any(token in str(status.get("source", "")).lower()
                      for token in ("default", "pending", "probe")),
                  "vulkan-status-transient", str(status.get("source")))
    require_sha256(str(status.get("requestedSha256", "")).lower(),
                   "vulkan.requestedSha256")
    require_sha256(str(status.get("sha256", "")).lower(), "vulkan.sha256")
    if status.get("requested") == "turnip":
        identity = f"{status.get('driverName')} {status.get('driverInfo')}".lower()
        reject_if("mesa" not in identity and "turnip" not in identity,
                  "vulkan-turnip-properties-mismatch", identity)
        reject_if(status.get("packageSource") not in {"bundled", "imported"},
                  "vulkan-turnip-package-source-invalid")


def marker_entries(inventory: Mapping[str, Any],
                   marker_roots: Mapping[str, str]) -> list[str]:
    reject_if(dict(marker_roots) != CANONICAL_MARKER_ROOTS,
              "marker-roots-not-canonical", repr(dict(marker_roots)))
    result: list[str] = []
    for entry in inventory["entries"]:
        path = entry["path"]
        matches = [
            (kind, root) for kind, root in marker_roots.items()
            if pathlib.PurePosixPath(path).parent.as_posix() == root
        ]
        reject_if(len(matches) != 1, "marker-root-ambiguous", path)
        kind, _root = matches[0]
        name = pathlib.PurePosixPath(path).name
        reject_if(entry["type"] != "file", "runtime-marker-not-regular", path)
        if kind == "runtime":



            reject_if(name.startswith("lsx4-bb-") or
                      MARKER_ARTIFACT_NAME_RE.fullmatch(name) is None,
                      "marker-root-layout-invalid", path)
            result.append(name)
        else:
            reject_if(MARKER_ARTIFACT_NAME_RE.fullmatch(name) is None,
                      "marker-root-layout-invalid", path)
            result.append("native/" + name)
    return sorted(result)


def validate_patch_runtime_marker(logcat: str) -> str:
    requested = [line for line in logcat.splitlines()
                 if "[EXECUTOR_BUNDLED_PATCH] name=60 FPS++ " in line
                 and " requested=1 " in line and " versionMatch=1 " in line]
    reject_if(len(requested) != 1, "sixty-fps-runtime-marker-not-unique",
              str(len(requested)))
    match = re.search(r"eligibleLines=(\d+)", requested[0])
    reject_if(match is None or int(match.group(1)) <= 0,
              "sixty-fps-runtime-marker-no-lines")
    failures = [line for line in logcat.splitlines()
                if "60 FPS++" in line and any(token in line.lower() for token in (
                    "result=skipped", "result=failed", "write_failed",
                    "failed to apply", "address_out_of_image", "write_crosses_image",
                ))]
    reject_if(bool(failures), "sixty-fps-runtime-patch-failure", " | ".join(failures))
    return requested[0]


def validate_runtime_status_markers(logcat: str, backend: Mapping[str, Any],
                                    vulkan: Mapping[str, Any]) -> dict[str, str]:
    backend_prefix = "EXECUTOR_GUEST_CPU_BACKEND phase=initialized status="
    backend_values: list[tuple[str, dict[str, Any]]] = []
    vulkan_prefix = "[EXECUTOR_VULKAN_DRIVER_AXIS] "
    vulkan_values: list[tuple[str, dict[str, Any]]] = []
    for line in logcat.splitlines():
        if backend_prefix in line:
            payload = line.split(backend_prefix, 1)[1].strip()
            backend_values.append((line, parse_json_object(payload, "runtime backend marker")))
        if vulkan_prefix in line:
            payload = line.split(vulkan_prefix, 1)[1].strip()
            vulkan_values.append((line, parse_json_object(payload, "runtime Vulkan marker")))
    reject_if(not backend_values, "runtime-backend-marker-missing")
    reject_if(not vulkan_values, "runtime-vulkan-marker-missing")
    reject_if(backend_values[-1][1] != backend, "runtime-backend-marker-stale")
    reject_if(vulkan_values[-1][1] != vulkan, "runtime-vulkan-marker-stale")
    return {"backend": backend_values[-1][0], "vulkan": vulkan_values[-1][0]}


def extract_regex_state(text: str, expression: str, name: str) -> dict[str, str]:
    try:
        matches = list(re.finditer(expression, text, flags=re.MULTILINE | re.DOTALL))
    except re.error as error:
        raise CaptureRejected(f"{name}-regex-invalid", str(error)) from error
    reject_if(len(matches) != 1, f"{name}-identity-not-unique", str(len(matches)))
    state = matches[0].groupdict()
    reject_if(not state or any(not value for value in state.values()),
              f"{name}-identity-empty")
    return state


def validate_snapshot(expected: Mapping[str, Any], actual: Mapping[str, Any]) -> None:
    """Validate a canonical capture; exposed separately for negative-control tests."""
    validate_expected_state(expected)
    reject_if(actual.get("schema") != CAPTURE_SCHEMA, "capture-schema-invalid")
    device = nested(actual, "device")
    for field in ("serial", "soc", "board"):
        reject_if(device.get(field) != nested(expected, f"device.{field}"),
                  "device-state-mismatch", field)
    reject_if(device.get("bootIdPre") != device.get("bootIdPost"), "device-boot-changed")
    require_nonempty(device.get("bootIdPre"), "device.bootId")

    process = nested(actual, "process")
    for field in ("pid", "starttime", "cmdline", "uid", "foreground"):
        reject_if(process.get(f"{field}Pre") != process.get(f"{field}Post"),
                  "process-identity-changed", field)
    reject_if(process.get("cmdlinePre") != nested(expected, "process.cmdline"),
              "process-cmdline-mismatch")
    reject_if(process.get("uidPre") != nested(expected, "package.uid"),
              "process-package-uid-mismatch")
    reject_if(process.get("foregroundPre") != nested(expected, "process.foregroundComponent"),
              "foreground-component-mismatch")

    package = nested(actual, "package")
    for field in ("name", "uid", "versionCode", "versionName", "codePaths",
                  "baseApkSha256", "signerSha256"):
        reject_if(package.get(field) != nested(expected, f"package.{field}"),
                  "package-state-mismatch", field)

    backend_pre = nested(actual, "backend.pre")
    backend_post = nested(actual, "backend.post")
    reject_if(backend_pre != backend_post, "backend-status-changed")
    validate_backend(backend_pre)
    reject_if(canonical_sha256(backend_pre) != nested(expected, "backend.statusSha256"),
              "backend-status-hash-mismatch")
    reject_if(backend_pre != nested(expected, "backend.status"),
              "backend-status-payload-mismatch")
    require_exact_subset(backend_pre, nested(expected, "backend.required"), "backend")

    vulkan_pre = nested(actual, "vulkan.pre")
    vulkan_post = nested(actual, "vulkan.post")
    reject_if(vulkan_pre != vulkan_post, "vulkan-status-changed")
    validate_vulkan(vulkan_pre)
    reject_if(canonical_sha256(vulkan_pre) != nested(expected, "vulkan.statusSha256"),
              "vulkan-status-hash-mismatch")
    require_exact_subset(vulkan_pre, nested(expected, "vulkan.required"), "vulkan")

    settings = nested(actual, "settings")
    reject_if(settings.get("sha256") != nested(expected, "settings.sha256"),
              "settings-hash-mismatch")
    reject_if(settings.get("pre") != settings.get("post"), "settings-changed")

    libraries = nested(actual, "libraries")
    for role in REQUIRED_LIBRARY_ROLES:
        item = libraries.get(role)
        reject_if(not isinstance(item, Mapping), "library-capture-missing", role)
        spec = nested(expected, f"libraries.{role}")
        if spec.get("path"):
            reject_if(item.get("path") != spec["path"],
                      "library-path-mismatch", role)
        else:
            reject_if(pathlib.PurePosixPath(str(item.get("path", ""))).name
                      != spec.get("basename"), "library-basename-mismatch", role)
        reject_if(item.get("sha256") != nested(expected, f"libraries.{role}.sha256"),
                  "library-hash-mismatch", role)
        reject_if(item.get("mappedPre") is not True or item.get("mappedPost") is not True,
                  "library-map-confirmation-missing", role)
        reject_if(item.get("mapsSha256Pre") != item.get("mapsSha256Post"),
                  "library-mapping-changed", role)
    reject_if(vulkan_pre.get("path") != libraries["vulkanLoader"]["path"],
              "vulkan-status-path-not-mapped")
    reject_if(str(vulkan_pre.get("sha256")).lower()
              != libraries["vulkanLoader"]["sha256"],
              "vulkan-status-hash-not-loaded")
    reject_if(libraries["vulkanLoader"].get("mappedPre") is not True
              or libraries["vulkanLoader"].get("mappedPost") is not True,
              "vulkan-loader-map-confirmation-missing")
    reject_if(libraries["vulkanLoader"].get("mapsSha256Pre")
              != libraries["vulkanLoader"].get("mapsSha256Post"),
              "vulkan-loader-mapping-changed")
    reject_if(libraries["vulkanIcd"]["path"] != vulkan_pre.get("path")
              or libraries["vulkanIcd"]["sha256"]
              != str(vulkan_pre.get("sha256", "")).lower(),
              "actual-vulkan-icd-not-selected")

    patches = nested(actual, "patches")
    for field in ("catalogSha256", "bundleSha256", "states"):
        reject_if(patches.get(field) != nested(expected, f"patches.{field}"),
                  "patch-state-mismatch", field)
    reject_if(patches.get("states", {}).get("bloodborne_0109_60_fps_plus_plus") is not True,
              "sixty-fps-plus-not-enabled")
    require_nonempty(patches.get("runtimeMarker"), "patches.runtimeMarker")
    require_nonempty(patches.get("backendRuntimeMarker"), "patches.backendRuntimeMarker")
    require_nonempty(patches.get("vulkanRuntimeMarker"), "patches.vulkanRuntimeMarker")

    markers = nested(actual, "markers")
    actual_present = markers.get("present")
    expected_allow = set(nested(expected, "markers.allowlist"))
    reject_if(bool(set(actual_present) - expected_allow), "unknown-runtime-marker",
              repr(sorted(set(actual_present) - expected_allow)))
    reject_if(actual_present != sorted(nested(expected, "markers.present")),
              "runtime-marker-state-mismatch")
    reject_if(canonical_sha256(actual_present) != nested(expected, "markers.presentSha256"),
              "runtime-marker-hash-mismatch")
    reject_if(markers.get("preInventorySha256") != markers.get("postInventorySha256"),
              "runtime-marker-inventory-changed")

    inventories = nested(actual, "inventories")
    for name in REQUIRED_INVENTORIES:
        item = inventories.get(name, {})
        reject_if(item.get("pre") != item.get("post"),
                  "inventory-changed-during-capture", name)
        reject_if(item.get("sha256")
                  != nested(expected, f"inventories.{name}.sha256"),
                  "inventory-hash-mismatch", name)

    surface = nested(actual, "surface")
    reject_if(surface.get("pre") != surface.get("post"), "surface-generation-changed")
    layer_name = str(surface.get("pre", {}).get("layerName", ""))
    reject_if(str(nested(expected, "surface.requiredComponent")) not in layer_name,
              "surface-component-mismatch", layer_name)
    reject_if(surface.get("pre", {}).get("displayId") != str(nested(expected, "surface.displayId")),
              "surface-display-mismatch")
    reject_if(nested(actual, "display.state") != nested(expected, "display.state"),
              "display-state-mismatch")

    environment = nested(actual, "environment")
    for phase in ("start", "end"):
        phase_values = environment.get(phase)
        reject_if(not isinstance(phase_values, Mapping), "environment-phase-missing", phase)
        for name, expression in nested(expected, "environment.requiredRegexes").items():
            value = phase_values.get(name)
            reject_if(not isinstance(value, Mapping) or not value.get("sha256"),
                      "environment-artifact-missing", f"{phase}.{name}")
            require_sha256(value.get("sha256"), f"environment.{phase}.{name}.sha256")
            reject_if(name in {"cpuFreq", "gpuFreq"}
                      and "MISSING" in str(value.get("text", "")),
                      "environment-frequency-field-missing", f"{phase}.{name}")
            reject_if(re.search(expression, str(value.get("text", "")), re.MULTILINE) is None,
                      "environment-required-state-missing", f"{phase}.{name}")


def capture_process(adb: AdbReader, package: str, label: str,
                    expected_component: str, expected_cmdline: str) -> dict[str, Any]:
    pids = adb.text(f"{label}-pidof", ["shell", "pidof", expected_cmdline]).split()
    pids = sorted(set(pids))
    reject_if(len(pids) != 1 or not pids[0].isdigit(), "package-pid-not-unique", repr(pids))
    pid = int(pids[0])
    stat_text = adb.text(f"{label}-proc-stat", ["exec-out", "run-as", package,
                                                "cat", f"/proc/{pid}/stat"])
    parsed_pid, _comm, starttime = parse_proc_stat(stat_text)
    reject_if(parsed_pid != pid, "proc-pid-mismatch")
    cmdline = adb.read(f"{label}-proc-cmdline", ["exec-out", "run-as", package,
                                                   "cat", f"/proc/{pid}/cmdline"])
    cmdline_value = cmdline.rstrip(b"\x00").replace(b"\x00", b" ").decode("utf-8", "strict")
    status = adb.text(f"{label}-proc-status", ["exec-out", "run-as", package,
                                                  "cat", f"/proc/{pid}/status"])
    activity = adb.text(f"{label}-activity", ["shell", "dumpsys", "activity", "activities"])
    foreground_lines = [line.strip() for line in activity.splitlines()
                        if "mResumedActivity" in line or "topResumedActivity" in line]
    reject_if(not foreground_lines, "foreground-state-missing")
    reject_if(any(expected_component not in line for line in foreground_lines),
              "foreground-component-conflict", repr(foreground_lines))
    return {
        "pid": pid, "starttime": starttime, "cmdline": cmdline_value,
        "uid": parse_uid(status), "foreground": expected_component,
    }


def _requested_layer_name(line: str, required_component: str) -> tuple[str, str] | None:
    """Normalize one active ``SurfaceFlinger --list`` entry.

    Vivo's new-front-end output wraps the actual layer name in
    ``RequestedLayerState{...}`` and appends topology attributes after the
    generation.  TimeStats, in contrast, records only the inner name.  Keeping
    those two representations unnormalised made a healthy current layer look
    different from itself.
    """

    stripped = line.strip()
    prefix = "RequestedLayerState{"
    if not stripped.startswith(prefix) or not stripped.endswith("}"):
        return None
    body = stripped[len(prefix):-1]
    match = re.fullmatch(
        r"(?P<name>.+?#(?P<generation>[0-9]+))"
        r"(?P<attributes>(?:\s+(?:[A-Za-z][A-Za-z0-9]*=[^\s{}]+|"
        r"![A-Za-z][A-Za-z0-9]*))*)",
        body,
    )
    if match is None or required_component not in match.group("name"):
        return None
    return match.group("name"), match.group("generation")


def parse_active_surface_identity(listing: str, timestats: str, full: str,
                                  expected: Mapping[str, Any]) -> dict[str, str]:
    """Bind one current BLAST generation to the active composition hierarchy.

    ``--timestats -dump`` is intentionally historical: it retains sections for
    surfaces from earlier process lifetimes.  Historical sections are valid
    evidence, but they cannot select the current surface.  ``--list`` selects
    the single current RequestedLayerState and the full dump independently
    proves that the exact normalised name is visible in the active composition
    hierarchy.  Two current candidates or two active identities remain fatal.
    """

    required_component = str(expected["requiredComponent"])
    candidates = {
        parsed
        for line in listing.splitlines()
        if (parsed := _requested_layer_name(line, required_component)) is not None
    }
    reject_if(len(candidates) != 1, "surface-layer-not-unique",
              repr(sorted(name for name, _generation in candidates)))
    layer_name, generation = next(iter(candidates))

    timestats_identities = {
        line.split("=", 1)[1].strip()
        for line in timestats.splitlines()
        if line.strip().startswith("layerName = ") and required_component in line
    }
    reject_if(layer_name not in timestats_identities,
              "surface-current-layer-missing-from-timestats", layer_name)

    active_match = re.search(
        r"(?ms)^Active Layers - layers with client handles[^\r\n]*\r?\n"
        r"(?P<body>.*?)(?=^Offscreen Hierarchy\s*$)",
        full,
    )
    reject_if(active_match is None, "surface-active-hierarchy-missing")
    active_body = active_match.group("body")
    reject_if("Composition list (bottom to top)" not in active_body,
              "surface-active-composition-missing")

    active_identities: set[tuple[str, str]] = set()
    current_stack: str | None = None
    lines = active_body.splitlines()
    exact_layer = re.compile(
        r"^\s*Layer \[(?P<layerId>[0-9]+)\] " + re.escape(layer_name) + r"\s*$"
    )
    for index, line in enumerate(lines):
        stack_match = re.fullmatch(r"LayerStack=(?P<displayId>[0-9]+)", line.strip())
        if stack_match is not None:
            current_stack = stack_match.group("displayId")
            continue
        layer_match = exact_layer.fullmatch(line)
        if layer_match is None or current_stack is None:
            continue
        next_line = lines[index + 1] if index + 1 < len(lines) else ""
        if re.match(r"^\s+visible reason=", next_line) is None:
            continue
        active_identities.add((layer_match.group("layerId"), current_stack))

    reject_if(len(active_identities) != 1, "surface-active-identity-not-unique",
              repr(sorted(active_identities)))
    layer_id, display_id = next(iter(active_identities))
    reject_if(display_id != str(expected["displayId"]), "surface-display-mismatch",
              f"expected={expected['displayId']} actual={display_id}")

    evidence = (
        f"layerId={layer_id} generation={generation} displayId={display_id}"
    )
    identity = extract_regex_state(evidence, str(expected["identityRegex"]), "surface")
    parsed_identity = {
        "layerId": layer_id,
        "generation": generation,
        "displayId": display_id,
    }
    reject_if(any(identity.get(field) != value for field, value in parsed_identity.items()),
              "surface-identity-regex-mismatch", repr(identity))
    identity["layerName"] = layer_name
    return identity


def capture_surface(adb: AdbReader, label: str,
                    expected: Mapping[str, Any]) -> tuple[dict[str, str], str, str]:
    listing = adb.text(f"{label}-sf-list", ["shell", "dumpsys", "SurfaceFlinger", "--list"])
    timestats = adb.text(f"{label}-sf-timestats", [
        "shell", "dumpsys", "SurfaceFlinger", "--timestats", "-dump",
    ])
    full = adb.text(f"{label}-sf-full", ["shell", "dumpsys", "SurfaceFlinger"])
    identity = parse_active_surface_identity(listing, timestats, full, expected)
    return identity, listing + "\n" + timestats, full


def capture_environment(adb: AdbReader, label: str) -> dict[str, dict[str, str]]:
    cpu_freq_script = (
        "for p in /sys/devices/system/cpu/cpufreq/policy*; do "
        "[ -d \"$p\" ] || continue; printf '%s ' \"$p\"; "
        "for f in affected_cpus scaling_cur_freq scaling_min_freq scaling_max_freq; do "
        "printf '%s=' \"$f\"; cat \"$p/$f\" 2>/dev/null || printf MISSING; done; "
        "printf '\\n'; done"
    )
    gpu_freq_script = (
        "found=0; "
        "for f in "
        "/sys/class/kgsl/kgsl-3d0/gpuclk "
        "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq "
        "/sys/devices/platform/soc/3d00000.qcom,kgsl-3d0/devfreq/cur_freq; do "
        "if [ -r \"$f\" ]; then value=$(cat \"$f\" 2>/dev/null) || continue; "
        "printf 'source=kgsl-sysfs status=available path=%s value=%s\\n' "
        "\"$f\" \"$value\"; found=1; fi; done; "
        "for p in /sys/class/devfreq/*; do [ -d \"$p\" ] || continue; "
        "name=$(cat \"$p/name\" 2>/dev/null || true); "
        "case \"$name:$p\" in *kgsl*|*gpu*|*GPU*) "
        "if [ -r \"$p/cur_freq\" ]; then value=$(cat \"$p/cur_freq\" 2>/dev/null) || continue; "
        "printf 'source=kgsl-sysfs status=available path=%s name=%s value=%s\\n' "
        "\"$p/cur_freq\" \"$name\" \"$value\"; found=1; fi;; esac; done; "
        "if [ \"$found\" -eq 0 ]; then "
        "printf 'source=kgsl-sysfs status=unavailable reason=no-readable-frequency-node\\n'; fi"
    )
    commands = {
        "thermal": ["shell", "dumpsys", "thermalservice"],
        "battery": ["shell", "dumpsys", "battery"],
        "power": ["shell", "dumpsys", "power"],
        "cpuFreq": adb_remote_shell_command(["shell"], cpu_freq_script),
        "gpuFreq": adb_remote_shell_command(["shell"], gpu_freq_script),
    }
    result: dict[str, dict[str, str]] = {}
    for name, command in commands.items():
        text_value = adb.text(f"{label}-{name}", command)
        result[name] = {"text": text_value, "sha256": sha256_bytes(text_value.encode("utf-8"))}
    return result


def resolve_apksigner(value: str | None) -> str:
    if value:
        reject_if(not pathlib.Path(value).is_file(), "apksigner-not-found", value)
        return value
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    if not sdk and os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        candidate = pathlib.Path(os.environ["LOCALAPPDATA"]) / "Android" / "Sdk"
        if candidate.is_dir():
            sdk = str(candidate)
    reject_if(not sdk, "apksigner-not-found", "pass --apksigner")
    executable = "apksigner.bat" if os.name == "nt" else "apksigner"
    candidates = sorted(pathlib.Path(sdk).glob(f"build-tools/*/{executable}"), reverse=True)
    reject_if(not candidates, "apksigner-not-found", sdk)
    return str(candidates[0])


def capture_inventory_set(adb: AdbReader, store: ArtifactStore, package: str,
                          specs: Mapping[str, Any], phase: str) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, spec in specs.items():
        if spec["source"] == "host":
            inventory = local_inventory(spec["roots"])
            store.json(f"raw/inventory-{phase}-{name}.json", inventory["entries"])
        else:
            raw = adb.text(
                f"inventory-{phase}-{name}",
                adb_remote_shell_command(
                    ["exec-out", "run-as", package],
                    remote_inventory_script(spec["roots"]),
                ),




                timeout=300,
            )
            inventory = parse_remote_inventory(raw, spec["roots"])
        result[name] = inventory
    return result


def collect(expected: Mapping[str, Any], store: ArtifactStore, adb: AdbReader,
            runner: CommandRunner, apksigner: str) -> dict[str, Any]:
    package_name = str(nested(expected, "package.name"))
    collection = nested(expected, "collection")

    serial_pre = adb.text("pre-serial", ["shell", "getprop", "ro.serialno"]).strip()
    soc_pre = adb.text("pre-soc", ["shell", "getprop", "ro.soc.model"]).strip()
    board_pre = adb.text("pre-board", ["shell", "getprop", "ro.product.board"]).strip()
    boot_pre = adb.text("pre-boot-id", ["shell", "cat", "/proc/sys/kernel/random/boot_id"]).strip()
    process_pre = capture_process(
        adb, package_name, "pre", nested(expected, "process.foregroundComponent"),
        nested(expected, "process.cmdline"))
    maps_pre_text = adb.text("pre-maps", ["exec-out", "run-as", package_name,
                                           "cat", f"/proc/{process_pre['pid']}/maps"])
    maps_pre = mapped_paths(maps_pre_text)
    backend_pre = parse_json_object(adb.app_text(
        package_name, "pre-backend-status", collection["backendStatusPath"]), "backend pre")
    vulkan_pre = parse_json_object(adb.app_text(
        package_name, "pre-vulkan-status", collection["vulkanStatusPath"]), "vulkan pre")
    prefs_pre_text = adb.app_text(package_name, "pre-preferences", collection["preferencesPath"])
    prefs_pre = normalize_preferences(prefs_pre_text)
    surface_pre, _sf_list_pre, sf_full_pre = capture_surface(adb, "pre", nested(expected, "surface"))
    environment_start = capture_environment(adb, "start")

    pm_paths_text = adb.text("package-paths", ["shell", "pm", "path", package_name])
    code_paths = parse_pm_paths(pm_paths_text, package_name)
    pm_dump = adb.text("package-dump", ["shell", "dumpsys", "package", package_name])
    version_code, version_name = parse_package_version(pm_dump)
    package_uid = parse_package_uid(pm_dump)
    base_path = next(path for path in code_paths if path.endswith("/base.apk"))
    apk = adb.read("base-apk", ["exec-out", "cat", base_path], binary=True, timeout=300)
    apk_sha = sha256_bytes(apk)
    with tempfile.TemporaryDirectory(prefix="lsx4-proof-apk-") as temporary:
        apk_file = pathlib.Path(temporary) / "base.apk"
        apk_file.write_bytes(apk)
        signer_result = runner.run([apksigner, "verify", "--print-certs", str(apk_file)], timeout=120)
        store.write("raw/apksigner.stdout.txt", signer_result.stdout)
        store.write("raw/apksigner.stderr.txt", signer_result.stderr)
        reject_if(signer_result.returncode != 0, "apksigner-verification-failed")
        signer_sha = parse_apksigner(signer_result.stdout.decode("utf-8", "replace"))
    catalog, catalog_raw, bundle_raw = extract_patch_catalog(apk, store)
    del apk

    patch_states, patch_markers = bloodborne_patch_states(catalog, prefs_pre)
    reject_if(set(patch_states) != set(nested(expected, "patches.states")),
              "patch-catalog-state-coverage-mismatch")
    sixty_marker = patch_markers.get("bloodborne_0109_60_fps_plus_plus")
    reject_if(sixty_marker != "lsx4-bb-0109-60-fps-plus-plus",
              "sixty-fps-catalog-marker-mismatch", repr(sixty_marker))

    marker_raw = adb.text(
        "marker-inventory-pre",
        adb_remote_shell_command(
            ["exec-out", "run-as", package_name],
            remote_marker_inventory_script(collection["markerRoots"]),
        ),
    )
    marker_inventory_pre = parse_remote_inventory(
        marker_raw, list(collection["markerRoots"].values())
    )
    present_markers_pre = marker_entries(
        marker_inventory_pre, collection["markerRoots"])

    inventory_pre = capture_inventory_set(
        adb, store, package_name, nested(expected, "inventories"), "pre")

    logcat = adb.text("process-logcat", ["logcat", "-d",
                                          f"--pid={process_pre['pid']}", "-v", "epoch"])
    runtime_patch_marker = validate_patch_runtime_marker(logcat)
    runtime_status_markers = validate_runtime_status_markers(
        logcat, backend_pre, vulkan_pre)

    display_dump = adb.text("display-dump", ["shell", "dumpsys", "display"])
    wm_size = adb.text("wm-size", ["shell", "wm", "size"])
    wm_density = adb.text("wm-density", ["shell", "wm", "density"])
    display_source = display_dump + "\n" + wm_size + "\n" + wm_density
    display_state = extract_regex_state(
        display_source, nested(expected, "display.extractRegex"), "display")

    libraries: dict[str, Any] = {}
    for role, spec in nested(expected, "libraries").items():
        path = select_mapped_library(maps_pre, spec, role)
        digest_text = adb.text(f"library-sha-{role}", ["exec-out", "run-as", package_name,
                                                     "sha256sum", "--", path])
        libraries[role] = {
            "path": path, "sha256": parse_sha256sum(digest_text, path),
            "mappedPre": True, "mappedPost": False,
            "mapsSha256Pre": mapped_file_identity(maps_pre_text, path),
            "mapsSha256Post": "",
        }
    loader_path = str(vulkan_pre.get("path", ""))
    reject_if(loader_path not in maps_pre, "vulkan-status-path-not-mapped", loader_path)
    loader_sha_text = adb.text("vulkan-loader-sha", ["exec-out", "run-as", package_name,
                                                       "sha256sum", "--", loader_path])
    libraries["vulkanLoader"] = {
        "path": loader_path, "sha256": parse_sha256sum(loader_sha_text, loader_path),
        "mappedPre": True, "mappedPost": False,
        "mapsSha256Pre": mapped_file_identity(maps_pre_text, loader_path),
        "mapsSha256Post": "",
    }

    environment_end = capture_environment(adb, "end")
    marker_post_raw = adb.text(
        "marker-inventory-post",
        adb_remote_shell_command(
            ["exec-out", "run-as", package_name],
            remote_marker_inventory_script(collection["markerRoots"]),
        ),
    )
    marker_inventory_post = parse_remote_inventory(
        marker_post_raw, list(collection["markerRoots"].values())
    )
    present_markers_post = marker_entries(
        marker_inventory_post, collection["markerRoots"])
    inventory_post = capture_inventory_set(
        adb, store, package_name, nested(expected, "inventories"), "post")
    surface_post, _sf_list_post, sf_full_post = capture_surface(adb, "post", nested(expected, "surface"))
    prefs_post_text = adb.app_text(package_name, "post-preferences", collection["preferencesPath"])
    prefs_post = normalize_preferences(prefs_post_text)
    backend_post = parse_json_object(adb.app_text(
        package_name, "post-backend-status", collection["backendStatusPath"]), "backend post")
    vulkan_post = parse_json_object(adb.app_text(
        package_name, "post-vulkan-status", collection["vulkanStatusPath"]), "vulkan post")
    process_post = capture_process(
        adb, package_name, "post", nested(expected, "process.foregroundComponent"),
        nested(expected, "process.cmdline"))
    maps_post_text = adb.text("post-maps", ["exec-out", "run-as", package_name,
                                            "cat", f"/proc/{process_post['pid']}/maps"])
    maps_post = mapped_paths(maps_post_text)
    for role, item in libraries.items():
        item["mappedPost"] = item["path"] in maps_post
        if item["mappedPost"]:
            item["mapsSha256Post"] = mapped_file_identity(maps_post_text, item["path"])
    boot_post = adb.text("post-boot-id", ["shell", "cat", "/proc/sys/kernel/random/boot_id"]).strip()
    serial_post = adb.text("post-serial", ["shell", "getprop", "ro.serialno"]).strip()
    soc_post = adb.text("post-soc", ["shell", "getprop", "ro.soc.model"]).strip()
    board_post = adb.text("post-board", ["shell", "getprop", "ro.product.board"]).strip()

    reject_if(serial_pre != serial_post or soc_pre != soc_post or board_pre != board_post,
              "device-identity-changed")
    reject_if(sf_full_pre == sf_full_post and surface_pre != surface_post,
              "surface-parser-inconsistent")

    actual = {
        "schema": CAPTURE_SCHEMA,
        "capturedAtUnixNs": time.time_ns(),
        "expectedStateSha256": canonical_sha256(expected),
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
            "sha256": canonical_sha256(prefs_pre),
        },
        "patches": {
            "catalogSha256": sha256_bytes(catalog_raw),
            "bundleSha256": sha256_bytes(bundle_raw),
            "states": patch_states, "runtimeMarker": runtime_patch_marker,
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
            for name in REQUIRED_INVENTORIES
        },
        "surface": {"pre": surface_pre, "post": surface_post},
        "display": {"state": display_state},
        "environment": {"start": environment_start, "end": environment_end},
    }
    validate_snapshot(expected, actual)
    return actual


def load_expected(path: pathlib.Path) -> dict[str, Any]:
    reject_if(path.is_symlink() or not path.is_file(), "expected-file-invalid", str(path))
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CaptureRejected("expected-file-unreadable", str(error)) from error
    reject_if(not isinstance(value, dict), "expected-top-level-invalid")
    validate_expected_state(value)
    return value


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apksigner")
    parser.add_argument("--agentchat", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[3] / "agentchat" / "agentchat.py")
    parser.add_argument("--agent", default="codex")
    parser.add_argument("--room", default="увеличить fps")
    args = parser.parse_args(argv)

    store: ArtifactStore | None = None
    try:
        expected = load_expected(args.expected)
        reject_if(args.serial != nested(expected, "device.adbTarget"),
                  "adb-target-not-predeclared", args.serial)
        reject_if(not args.agentchat.is_file(), "agentchat-script-missing", str(args.agentchat))
        store = ArtifactStore(args.output)
        store.write("expected-state.json", canonical_json(expected))
        runner = CommandRunner()
        gate = AgentChatGate(
            runner, args.agentchat, agent=args.agent, room=args.room,
            additional_areas=(f"adb:{nested(expected, 'device.serial')}",),
        )
        adb = AdbReader(runner, gate, store, args.adb, args.serial)
        actual = collect(expected, store, adb, runner, resolve_apksigner(args.apksigner))
        store.json("provenance.json", actual)
        manifest = {"schema": "lsx4-bloodborne-artifact-manifest/v1",
                    "artifacts": store.manifest()}
        store.json("manifest.json", manifest)
        print(json.dumps({
            "accepted": True,
            "output": str(args.output.resolve()),
            "provenanceSha256": canonical_sha256(actual),
        }, ensure_ascii=False, sort_keys=True))
        store.seal()
        return 0
    except CaptureRejected as error:
        payload = {"accepted": False, "reason": error.reason, "detail": error.detail}
        if store is not None:
            try:
                store.json("REJECTED.json", payload)
                store.json("partial-manifest.json", {
                    "schema": "lsx4-bloodborne-artifact-manifest/v1",
                    "artifacts": store.manifest(),
                })
            except CaptureRejected:
                pass
        print(json.dumps(payload, ensure_ascii=False, sort_keys=True), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
