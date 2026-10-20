#!/usr/bin/env python3

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


SCHEMA_VERSION = 1
EXPECTED_ELF_CLASS = "ELF64"
EXPECTED_ELF_DATA = "2's complement, little endian"
EXPECTED_ELF_MACHINE = "AArch64"
EXPECTED_SONAMES = {
    "fexcore": "libFEXCore.so",
    "adapter": "liblsx4_fex_backend_v3.so",
    "bridge": "liblsx4_fex2608_context_bridge.so",
}
EXPECTED_EXPORTS = {
    "adapter": {"lsx4_fex_backend_get_api_v3"},
    "bridge": {
        "lsx4_fex2608_create_context_v1",
        "lsx4_fex2608_get_runtime_bridge_api_v2",
    },
}
SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cmake",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".inl",
    ".in",
    ".json",
    ".py",
    ".s",
    ".S",
    ".asm",
    ".sh",
    ".toml",
    ".yaml",
    ".yml",
}
IGNORED_SOURCE_PARTS = {
    ".git",
    ".cache",
    "__pycache__",
    "build",
    "Build",
    "CMakeFiles",
}


class VerificationError(RuntimeError):
    pass


@dataclass(frozen=True)
class Toolchain:
    ndk_root: Path
    readelf: Path
    nm: Path
    clang: Path
    libcxx: Path
    system_library_dir: Path


def _canonical(path: Path | str) -> Path:
    return Path(path).expanduser().resolve(strict=True)


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _run(command: Sequence[str | os.PathLike[str]], *, text: bool = True) -> str | bytes:
    completed = subprocess.run(
        [os.fspath(item) for item in command],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=text,
        encoding="utf-8" if text else None,
        errors="replace" if text else None,
    )
    if completed.returncode != 0:
        stderr = completed.stderr if text else completed.stderr.decode("utf-8", "replace")
        raise VerificationError(
            f"command failed ({completed.returncode}): {' '.join(map(os.fspath, command))}\n{stderr.strip()}"
        )
    return completed.stdout


def _tool_executable(bin_dir: Path, name: str) -> Path:
    candidates = (bin_dir / f"{name}.exe", bin_dir / name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise VerificationError(f"missing NDK tool {name} under {bin_dir}")


def discover_toolchain(ndk_root: Path, api: int) -> Toolchain:
    ndk_root = _canonical(ndk_root)
    prebuilt_root = ndk_root / "toolchains" / "llvm" / "prebuilt"
    prebuilts = sorted(path for path in prebuilt_root.iterdir() if path.is_dir())
    if len(prebuilts) != 1:
        raise VerificationError(
            f"expected exactly one LLVM prebuilt under {prebuilt_root}, found {len(prebuilts)}"
        )
    prebuilt = prebuilts[0]
    bin_dir = prebuilt / "bin"
    sysroot_lib = prebuilt / "sysroot" / "usr" / "lib" / "aarch64-linux-android"
    system_library_dir = sysroot_lib / str(api)
    libcxx = sysroot_lib / "libc++_shared.so"
    if not system_library_dir.is_dir() or not libcxx.is_file():
        raise VerificationError(f"NDK does not provide arm64 API {api} sysroot libraries")
    return Toolchain(
        ndk_root=ndk_root,
        readelf=_tool_executable(bin_dir, "llvm-readelf"),
        nm=_tool_executable(bin_dir, "llvm-nm"),
        clang=_tool_executable(bin_dir, "clang"),
        libcxx=libcxx.resolve(),
        system_library_dir=system_library_dir.resolve(),
    )


def _include_source_file(path: Path) -> bool:
    if any(part in IGNORED_SOURCE_PARTS for part in path.parts):
        return False
    return path.name == "CMakeLists.txt" or path.suffix in SOURCE_SUFFIXES


def source_tree_state(root: Path) -> dict[str, object]:
    root = _canonical(root)
    entries: list[tuple[str, Path]] = []
    if root.is_file():
        if not _include_source_file(Path(root.name)):
            raise VerificationError(f"source input is not a supported build file: {root}")
        entries.append((root.name, root))
    else:
        for path in root.rglob("*"):
            if path.is_file() and _include_source_file(path.relative_to(root)):
                entries.append((path.relative_to(root).as_posix(), path))
    entries.sort(key=lambda item: item[0])
    digest = hashlib.sha256()
    newest_mtime_ns = 0
    for relative, path in entries:
        file_digest = sha256_file(path)
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(file_digest.encode("ascii"))
        digest.update(b"\n")
        newest_mtime_ns = max(newest_mtime_ns, path.stat().st_mtime_ns)
    return {
        "root": os.fspath(root),
        "file_count": len(entries),
        "sha256": digest.hexdigest(),
        "newest_mtime_ns": newest_mtime_ns,
    }


def git_state(root: Path) -> dict[str, object]:
    root = _canonical(root)
    git_probe = root if root.is_dir() else root.parent
    git_root = Path(
        str(_run(["git", "-C", git_probe, "rev-parse", "--show-toplevel"])).strip()
    ).resolve()
    relative_root = root.relative_to(git_root).as_posix()
    head = str(_run(["git", "-C", git_root, "rev-parse", "HEAD"])).strip()
    diff = _run(
        ["git", "-C", git_root, "diff", "--binary", "HEAD", "--", relative_root],
        text=False,
    )
    untracked_raw = str(
        _run(
            [
                "git",
                "-C",
                git_root,
                "ls-files",
                "--others",
                "--exclude-standard",
                "-z",
                "--",
                relative_root,
            ]
        )
    )
    untracked_paths = sorted(item for item in untracked_raw.split("\0") if item)
    untracked_digest = hashlib.sha256()
    for relative in untracked_paths:
        path = git_root / relative
        if not path.is_file() or not _include_source_file(Path(relative)):
            continue
        untracked_digest.update(relative.replace("\\", "/").encode("utf-8"))
        untracked_digest.update(b"\0")
        untracked_digest.update(sha256_file(path).encode("ascii"))
        untracked_digest.update(b"\n")
    return {
        "head": head,
        "tracked_diff_sha256": _sha256_bytes(diff),
        "untracked_source_sha256": untracked_digest.hexdigest(),
    }


def source_state(name: str, root: Path, *, git: bool) -> dict[str, object]:
    state: dict[str, object] = {"name": name, **source_tree_state(root)}
    if git:
        state["git"] = git_state(root)
    return state


def parse_elf_header(output: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in output.splitlines():
        match = re.match(r"\s*(Class|Data|Type|Machine):\s*(.+?)\s*$", line)
        if match:
            fields[match.group(1).lower()] = match.group(2)
    return fields


def parse_dynamic(output: str) -> tuple[list[str], str | None]:
    needed = re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", output)
    soname_match = re.search(r"\(SONAME\).*?\[([^\]]+)\]", output)
    return needed, soname_match.group(1) if soname_match else None


def parse_build_id(output: str) -> str:
    match = re.search(r"Build ID:\s*([0-9A-Fa-f]+)", output)
    if not match:
        raise VerificationError("ELF has no GNU build ID")
    return match.group(1).lower()


def parse_android_api_note(output: str) -> int:
    match = re.search(r"NT_ANDROID_TYPE_IDENT.*?description data:\s*([0-9A-Fa-f ]+)", output, re.S)
    if not match:
        raise VerificationError("ELF has no .note.android.ident API level")
    octets = bytes(int(value, 16) for value in match.group(1).split())
    if len(octets) < 4:
        raise VerificationError("malformed .note.android.ident payload")
    return int.from_bytes(octets[:4], "little")


def _normalize_symbol(symbol: str) -> str:
    return symbol.split("@", 1)[0]


def parse_nm(output: str) -> tuple[set[str], set[str], set[str]]:
    defined: set[str] = set()
    undefined: set[str] = set()
    weak_undefined: set[str] = set()
    for line in output.splitlines():
        fields = line.strip().split(maxsplit=2)
        if len(fields) == 2:
            symbol_type, symbol = fields
        elif len(fields) == 3:
            _, symbol_type, symbol = fields
        else:
            continue
        symbol = _normalize_symbol(symbol.strip())
        if symbol_type == "U":
            undefined.add(symbol)
        elif symbol_type in {"w", "v"}:
            weak_undefined.add(symbol)
        elif symbol_type.isalpha() and symbol_type.upper() != "U":
            defined.add(symbol)
    return defined, undefined, weak_undefined


def _elf_symbols(toolchain: Toolchain, path: Path) -> tuple[set[str], set[str], set[str]]:
    output = str(_run([toolchain.nm, "-D", path]))
    return parse_nm(output)


def inspect_elf(
    toolchain: Toolchain,
    path: Path,
    *,
    expected_api: int,
    exact_api: bool = True,
) -> dict[str, object]:
    path = _canonical(path)
    header = parse_elf_header(str(_run([toolchain.readelf, "-h", path])))
    if header.get("class") != EXPECTED_ELF_CLASS:
        raise VerificationError(f"{path.name}: expected {EXPECTED_ELF_CLASS}, got {header.get('class')}")
    if header.get("data") != EXPECTED_ELF_DATA:
        raise VerificationError(f"{path.name}: expected little-endian ELF, got {header.get('data')}")
    if header.get("machine") != EXPECTED_ELF_MACHINE:
        raise VerificationError(f"{path.name}: expected AArch64, got {header.get('machine')}")
    if not header.get("type", "").startswith("DYN"):
        raise VerificationError(f"{path.name}: expected ET_DYN, got {header.get('type')}")

    dynamic_output = str(_run([toolchain.readelf, "-d", path]))
    needed, soname = parse_dynamic(dynamic_output)
    notes = str(_run([toolchain.readelf, "-n", path]))
    api = parse_android_api_note(notes)
    if exact_api and api != expected_api:
        raise VerificationError(f"{path.name}: expected Android API note {expected_api}, got {api}")
    if not exact_api and api > expected_api:
        raise VerificationError(
            f"{path.name}: requires Android API {api}, target is API {expected_api}"
        )
    defined, undefined, weak_undefined = _elf_symbols(toolchain, path)
    return {
        "path": os.fspath(path),
        "size": path.stat().st_size,
        "sha256": sha256_file(path),
        "build_id": parse_build_id(notes),
        "android_api": api,
        "elf_class": header["class"],
        "machine": header["machine"],
        "soname": soname,
        "needed": needed,
        "defined_symbol_count": len(defined),
        "undefined_symbol_count": len(undefined),
        "weak_undefined_symbol_count": len(weak_undefined),
        "mtime_ns": path.stat().st_mtime_ns,
    }


def _provider_paths(
    toolchain: Toolchain,
    artifacts: dict[str, Path],
    *,
    libcxx_override: Path | None = None,
) -> dict[str, Path]:
    providers: dict[str, Path] = {path.name: path for path in artifacts.values()}
    libcxx = _canonical(libcxx_override) if libcxx_override is not None else toolchain.libcxx
    providers[libcxx.name] = libcxx
    for name in ("libc.so", "libdl.so", "libm.so", "liblog.so", "libandroid.so"):
        candidate = toolchain.system_library_dir / name
        if candidate.is_file():
            providers[name] = candidate.resolve()
    return providers


def verify_symbol_closure(
    toolchain: Toolchain,
    artifacts: dict[str, Path],
    *,
    libcxx_override: Path | None = None,
) -> dict[str, object]:
    providers = _provider_paths(toolchain, artifacts, libcxx_override=libcxx_override)
    metadata: dict[Path, tuple[list[str], set[str], set[str]]] = {}

    def load(path: Path) -> tuple[list[str], set[str], set[str]]:
        path = path.resolve()
        cached = metadata.get(path)
        if cached is not None:
            return cached
        needed, _ = parse_dynamic(str(_run([toolchain.readelf, "-d", path])))
        defined, undefined, _ = _elf_symbols(toolchain, path)
        result = (needed, defined, undefined)
        metadata[path] = result
        return result

    details: dict[str, object] = {}
    for role, root in artifacts.items():
        queue = [root.resolve()]
        visited: set[Path] = set()
        reachable_definitions: set[str] = set()
        root_undefined: set[str] = set()
        resolved_sonames: set[str] = set()
        while queue:
            current = queue.pop()
            if current in visited:
                continue
            visited.add(current)
            needed, defined, undefined = load(current)
            reachable_definitions.update(defined)
            if current == root.resolve():
                root_undefined = undefined
            for soname in needed:
                provider = providers.get(soname)
                if provider is None:
                    raise VerificationError(f"{root.name}: missing provider for DT_NEEDED {soname}")
                resolved_sonames.add(soname)
                queue.append(provider)
        unresolved = sorted(root_undefined - reachable_definitions)
        if unresolved:
            preview = ", ".join(unresolved[:12])
            raise VerificationError(
                f"{root.name}: {len(unresolved)} strong undefined dynamic symbols are not closed: {preview}"
            )
        details[role] = {
            "reachable_provider_count": len(visited),
            "resolved_needed": sorted(resolved_sonames),
            "strong_undefined_closed": len(root_undefined),
        }
    return details


def _version(command: Sequence[str | os.PathLike[str]]) -> str:
    output = str(_run(command)).splitlines()
    return output[0].strip() if output else ""


def toolchain_state(toolchain: Toolchain, cmake: Path, ninja: Path) -> dict[str, object]:
    source_properties = toolchain.ndk_root / "source.properties"
    android_toolchain = toolchain.ndk_root / "build" / "cmake" / "android.toolchain.cmake"
    cmake = _canonical(cmake)
    ninja = _canonical(ninja)
    properties_text = source_properties.read_text(encoding="utf-8", errors="replace")
    revision_match = re.search(r"^Pkg\.Revision\s*=\s*(\S+)\s*$", properties_text, re.M)
    return {
        "ndk_root": os.fspath(toolchain.ndk_root),
        "ndk_revision": revision_match.group(1) if revision_match else "unknown",
        "ndk_source_properties_sha256": sha256_file(source_properties),
        "clang": _version([toolchain.clang, "--version"]),
        "clang_sha256": sha256_file(toolchain.clang),
        "cmake": _version([cmake, "--version"]),
        "cmake_sha256": sha256_file(cmake),
        "ninja": _version([ninja, "--version"]),
        "ninja_sha256": sha256_file(ninja),
        "android_toolchain_sha256": sha256_file(android_toolchain),
        "llvm_readelf_sha256": sha256_file(toolchain.readelf),
        "llvm_nm_sha256": sha256_file(toolchain.nm),
    }


def _validate_roles(artifacts: dict[str, Path]) -> None:
    if set(artifacts) != set(EXPECTED_SONAMES):
        raise VerificationError(f"artifact roles must be exactly {sorted(EXPECTED_SONAMES)}")
    for role, expected_soname in EXPECTED_SONAMES.items():
        if artifacts[role].name != expected_soname:
            raise VerificationError(
                f"{role}: expected filename {expected_soname}, got {artifacts[role].name}"
            )


def create_manifest(
    *,
    artifacts: dict[str, Path],
    source_specs: Sequence[tuple[str, Path, bool]],
    ndk_root: Path,
    cmake: Path,
    ninja: Path,
    api: int,
    abi_header: Path,
    android_smokes: Sequence[Path],
    check_freshness: bool,
) -> dict[str, object]:
    artifacts = {role: _canonical(path) for role, path in artifacts.items()}
    _validate_roles(artifacts)
    toolchain = discover_toolchain(ndk_root, api)
    source_states = [source_state(name, root, git=git) for name, root, git in source_specs]
    newest_source_mtime = max(int(state["newest_mtime_ns"]) for state in source_states)

    artifact_states: dict[str, object] = {}
    for role, path in artifacts.items():
        state = inspect_elf(toolchain, path, expected_api=api)
        expected_soname = EXPECTED_SONAMES[role]
        if state["soname"] != expected_soname:
            raise VerificationError(
                f"{path.name}: expected SONAME {expected_soname}, got {state['soname']}"
            )
        defined, _, _ = _elf_symbols(toolchain, path)
        missing_exports = sorted(EXPECTED_EXPORTS.get(role, set()) - defined)
        if missing_exports:
            raise VerificationError(f"{path.name}: missing required exports {missing_exports}")
        if check_freshness and int(state["mtime_ns"]) < newest_source_mtime:
            raise VerificationError(f"{path.name}: artifact is older than a build input")
        artifact_states[role] = state

    smoke_states: list[dict[str, object]] = []
    for smoke in android_smokes:
        smoke = _canonical(smoke)
        state = inspect_elf(toolchain, smoke, expected_api=api)
        if check_freshness and int(state["mtime_ns"]) < newest_source_mtime:
            raise VerificationError(f"{smoke.name}: Android smoke binary is older than a build input")
        smoke_states.append(state)

    closure = verify_symbol_closure(toolchain, artifacts)
    abi_header = _canonical(abi_header)
    libcxx_notes = str(_run([toolchain.readelf, "-n", toolchain.libcxx]))
    libcxx_api = parse_android_api_note(libcxx_notes)
    if libcxx_api > api:
        raise VerificationError(
            f"libc++_shared.so requires Android API {libcxx_api}, target is API {api}"
        )
    for state in artifact_states.values():
        state.pop("mtime_ns", None)
    for state in smoke_states:
        state.pop("mtime_ns", None)
    for state in source_states:
        state.pop("newest_mtime_ns", None)
    return {
        "schema_version": SCHEMA_VERSION,
        "target": {"android_api": api, "abi": "arm64-v8a", "machine": EXPECTED_ELF_MACHINE},
        "artifacts": artifact_states,
        "android_smokes": smoke_states,
        "runtime_dependencies": {
            "libc++_shared.so": {
                "path": os.fspath(toolchain.libcxx),
                "size": toolchain.libcxx.stat().st_size,
                "sha256": sha256_file(toolchain.libcxx),
                "build_id": parse_build_id(libcxx_notes),
                "android_api": libcxx_api,
            }
        },
        "abi": {"path": os.fspath(abi_header), "sha256": sha256_file(abi_header)},
        "sources": source_states,
        "symbol_closure": closure,
        "toolchain": toolchain_state(toolchain, _canonical(cmake), _canonical(ninja)),
    }


def _manifest_comparable(manifest: dict[str, object]) -> dict[str, object]:
    clone = json.loads(json.dumps(manifest))
    for state in clone.get("artifacts", {}).values():
        state.pop("mtime_ns", None)
    for state in clone.get("android_smokes", []):
        state.pop("mtime_ns", None)
    for source in clone.get("sources", []):
        source.pop("newest_mtime_ns", None)
    return clone


def create_source_snapshot(
    source_specs: Sequence[tuple[str, Path, bool]],
) -> dict[str, object]:
    states = [source_state(name, root, git=git) for name, root, git in source_specs]
    for state in states:
        state.pop("newest_mtime_ns", None)
    return {"schema_version": SCHEMA_VERSION, "sources": states}


def verify_source_snapshot(path: Path) -> dict[str, object]:
    path = _canonical(path)
    saved = json.loads(path.read_text(encoding="utf-8"))
    if saved.get("schema_version") != SCHEMA_VERSION:
        raise VerificationError(f"unsupported source snapshot schema {saved.get('schema_version')}")
    source_specs = [
        (state["name"], Path(state["root"]), "git" in state)
        for state in saved["sources"]
    ]
    regenerated = create_source_snapshot(source_specs)
    if saved != regenerated:
        difference = _first_difference(saved, regenerated)
        raise VerificationError(f"build inputs changed while the runtime was being built: {difference}")
    return regenerated


def _first_difference(left: object, right: object, path: str = "$") -> str:
    if type(left) is not type(right):
        return f"{path}: type {type(left).__name__} != {type(right).__name__}"
    if isinstance(left, dict):
        left_keys = set(left)
        right_keys = set(right)
        if left_keys != right_keys:
            return f"{path}: keys {sorted(left_keys)} != {sorted(right_keys)}"
        for key in sorted(left_keys):
            difference = _first_difference(left[key], right[key], f"{path}.{key}")
            if difference:
                return difference
        return ""
    if isinstance(left, list):
        if len(left) != len(right):
            return f"{path}: length {len(left)} != {len(right)}"
        for index, (left_item, right_item) in enumerate(zip(left, right)):
            difference = _first_difference(left_item, right_item, f"{path}[{index}]")
            if difference:
                return difference
        return ""
    if left != right:
        return f"{path}: {left!r} != {right!r}"
    return ""


def verify_manifest(path: Path, *, check_freshness: bool) -> dict[str, object]:
    path = _canonical(path)
    saved = json.loads(path.read_text(encoding="utf-8"))
    if saved.get("schema_version") != SCHEMA_VERSION:
        raise VerificationError(f"unsupported manifest schema {saved.get('schema_version')}")
    artifacts = {
        role: Path(state["path"])
        for role, state in saved["artifacts"].items()
    }
    source_specs = [
        (state["name"], Path(state["root"]), "git" in state)
        for state in saved["sources"]
    ]
    regenerated = create_manifest(
        artifacts=artifacts,
        source_specs=source_specs,
        ndk_root=Path(saved["toolchain"]["ndk_root"]),
        cmake=_find_tool_from_version(saved["toolchain"]["cmake"], "cmake", path),
        ninja=_find_tool_from_version(saved["toolchain"]["ninja"], "ninja", path),
        api=int(saved["target"]["android_api"]),
        abi_header=Path(saved["abi"]["path"]),
        android_smokes=[Path(state["path"]) for state in saved.get("android_smokes", [])],
        check_freshness=check_freshness,
    )
    saved_comparable = _manifest_comparable(saved)
    regenerated_comparable = _manifest_comparable(regenerated)
    if saved_comparable != regenerated_comparable:
        difference = _first_difference(saved_comparable, regenerated_comparable)
        raise VerificationError(
            "manifest provenance or artifact content no longer matches the filesystem: " + difference
        )
    return regenerated


def verify_packaged_libcxx(manifest: dict[str, object], path: Path) -> dict[str, object]:
    path = _canonical(path)
    if path.name != "libc++_shared.so":
        raise VerificationError(f"packaged C++ runtime has the wrong filename: {path.name}")
    api = int(manifest["target"]["android_api"])
    toolchain = discover_toolchain(Path(manifest["toolchain"]["ndk_root"]), api)
    state = inspect_elf(toolchain, path, expected_api=api, exact_api=False)
    if state["soname"] != "libc++_shared.so":
        raise VerificationError(
            f"packaged C++ runtime has wrong SONAME {state['soname']}"
        )
    expected = manifest["runtime_dependencies"]["libc++_shared.so"]
    if path.stat().st_size != int(expected["size"]):
        raise VerificationError(
            "packaged libc++_shared.so size does not match the attested NDK runtime"
        )
    if sha256_file(path) != str(expected["sha256"]):
        raise VerificationError(
            "packaged libc++_shared.so hash does not match the attested NDK runtime"
        )
    artifacts = {
        role: _canonical(artifact["path"])
        for role, artifact in manifest["artifacts"].items()
    }
    verify_symbol_closure(toolchain, artifacts, libcxx_override=path)
    return state


def verify_packaged_artifact_copies(manifest: dict[str, object], directory: Path) -> None:
    directory = _canonical(directory)
    if not directory.is_dir():
        raise VerificationError(f"packaged native-library directory is not a directory: {directory}")
    for role, expected in manifest["artifacts"].items():
        source_name = Path(expected["path"]).name
        packaged = directory / source_name
        if not packaged.is_file():
            raise VerificationError(f"packaged directory is missing {source_name} ({role})")
        if packaged.stat().st_size != int(expected["size"]):
            raise VerificationError(f"packaged {source_name} has the wrong size")
        actual_hash = sha256_file(packaged)
        if actual_hash != expected["sha256"]:
            raise VerificationError(
                f"packaged {source_name} hash mismatch: expected {expected['sha256']}, got {actual_hash}"
            )


def _find_tool_from_version(version: str, name: str, manifest_path: Path) -> Path:
    tool_paths = manifest_path.parent / "tool-paths.json"
    if tool_paths.is_file():
        value = json.loads(tool_paths.read_text(encoding="utf-8")).get(name)
        if value:
            return _canonical(value)
    resolved = shutil_which(name)
    if resolved:
        return Path(resolved).resolve()
    ndk_root = None
    try:
        saved = json.loads(manifest_path.read_text(encoding="utf-8"))
        ndk_root = Path(saved["toolchain"]["ndk_root"])
    except (KeyError, OSError, TypeError, json.JSONDecodeError):
        pass
    if ndk_root is not None:
        sdk_root = ndk_root.parent.parent
        if name == "cmake":
            for candidate in sorted((sdk_root / "cmake").glob("*/bin/cmake.exe"), reverse=True):
                if candidate.is_file() and _version([candidate, "--version"]) == version:
                    return candidate.resolve()
        if name == "ninja":
            for candidate in sorted((sdk_root / "cmake").glob("*/bin/ninja.exe"), reverse=True):
                if candidate.is_file() and _version([candidate, "--version"]) == version:
                    return candidate.resolve()
    raise VerificationError(f"cannot locate {name} matching manifest toolchain")


def shutil_which(name: str) -> str | None:
    path_value = os.environ.get("PATH", "")
    suffixes = (".exe", "") if os.name == "nt" else ("",)
    for directory in path_value.split(os.pathsep):
        for suffix in suffixes:
            candidate = Path(directory) / f"{name}{suffix}"
            if candidate.is_file():
                return os.fspath(candidate)
    return None


def _write_json(path: Path, value: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    serialized = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    path.write_text(serialized, encoding="utf-8", newline="\n")


def _parse_source_spec(value: str) -> tuple[str, Path, bool]:
    fields = value.split("=", 2)
    if len(fields) != 3 or fields[2] not in {"git", "tree"}:
        raise argparse.ArgumentTypeError("source must be NAME=PATH=git or NAME=PATH=tree")
    return fields[0], Path(fields[1]), fields[2] == "git"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Verify and attest LSX4 FEX Android runtime artifacts")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--manifest", type=Path, help="verify an existing manifest")
    mode.add_argument("--write-manifest", type=Path, help="write a new deterministic manifest")
    mode.add_argument("--source-snapshot", type=Path, help="verify a build-input snapshot")
    mode.add_argument("--write-source-snapshot", type=Path, help="write a build-input snapshot")
    parser.add_argument("--fexcore", type=Path)
    parser.add_argument("--adapter", type=Path)
    parser.add_argument("--bridge", type=Path)
    parser.add_argument("--android-smoke", action="append", type=Path, default=[])
    parser.add_argument("--source", action="append", type=_parse_source_spec, default=[])
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--cmake", type=Path)
    parser.add_argument("--ninja", type=Path)
    parser.add_argument("--abi-header", type=Path)
    parser.add_argument(
        "--packaged-libcxx",
        type=Path,
        help="libc++_shared.so that will actually be copied into the APK",
    )
    parser.add_argument(
        "--packaged-dir",
        type=Path,
        help="directory containing the three FEX DSOs after the packaging copy step",
    )
    parser.add_argument("--api", type=int, default=26)
    parser.add_argument(
        "--skip-freshness",
        action="store_true",
        help="diagnostic-only: do not permit this option in APK packaging",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.source_snapshot:
            snapshot = verify_source_snapshot(args.source_snapshot)
            print(f"FEX_ANDROID_SOURCE_SNAPSHOT_OK sources={len(snapshot['sources'])}")
            return 0
        if args.write_source_snapshot:
            if not args.source:
                raise VerificationError("--write-source-snapshot requires at least one --source")
            snapshot = create_source_snapshot(args.source)
            _write_json(args.write_source_snapshot, snapshot)
            print(f"FEX_ANDROID_SOURCE_SNAPSHOT_OK sources={len(snapshot['sources'])}")
            return 0
        if args.manifest:
            manifest = verify_manifest(args.manifest, check_freshness=not args.skip_freshness)
            if args.packaged_dir:
                verify_packaged_artifact_copies(manifest, args.packaged_dir)
            if args.packaged_libcxx:
                verify_packaged_libcxx(manifest, args.packaged_libcxx)
        else:
            required = {
                "--fexcore": args.fexcore,
                "--adapter": args.adapter,
                "--bridge": args.bridge,
                "--source": args.source,
                "--ndk": args.ndk,
                "--cmake": args.cmake,
                "--ninja": args.ninja,
                "--abi-header": args.abi_header,
            }
            missing = [name for name, value in required.items() if not value]
            if missing:
                raise VerificationError(f"missing arguments for manifest creation: {', '.join(missing)}")
            manifest = create_manifest(
                artifacts={
                    "fexcore": args.fexcore,
                    "adapter": args.adapter,
                    "bridge": args.bridge,
                },
                source_specs=args.source,
                ndk_root=args.ndk,
                cmake=args.cmake,
                ninja=args.ninja,
                api=args.api,
                abi_header=args.abi_header,
                android_smokes=args.android_smoke,
                check_freshness=not args.skip_freshness,
            )
            _write_json(args.write_manifest, manifest)
            tool_paths = {
                "cmake": os.fspath(_canonical(args.cmake)),
                "ninja": os.fspath(_canonical(args.ninja)),
            }
            _write_json(args.write_manifest.parent / "tool-paths.json", tool_paths)
        print(
            "FEX_ANDROID_ARTIFACTS_OK "
            f"schema={manifest['schema_version']} api={manifest['target']['android_api']} "
            f"artifacts={len(manifest['artifacts'])}"
        )
        return 0
    except (OSError, VerificationError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"FEX_ANDROID_ARTIFACTS_ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
