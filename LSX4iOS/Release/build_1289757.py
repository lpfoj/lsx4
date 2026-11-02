#!/usr/bin/env python3
"""Rebuild the 9757 handoff using repository Swift/scheduler sources.

The inherited native runtime is linked from the original, locally retained
9564 inputs. This is deliberately not a claim of a clean full-runtime build.
No installation, phone control, upload, or update-feed writes are performed.
"""
import argparse
import hashlib
import json
import plistlib
import shutil
import struct
import subprocess
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def text(path):
    # The port preserves code; normalize line endings and whitespace-only lines.
    return b"\n".join(line if line.strip() else b"" for line in
                       path.read_bytes().replace(b"\r\n", b"\n").split(b"\n"))


def run(command, log):
    with log.open("w") as stream:
        subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True)


def section(data, segment, name):
    assert struct.unpack_from("<I", data)[0] == 0xFEEDFACF
    offset = 32
    for _ in range(struct.unpack_from("<I", data, 16)[0]):
        command, size = struct.unpack_from("<II", data, offset)
        if command == 0x19:
            for index in range(struct.unpack_from("<I", data, offset + 64)[0]):
                item = struct.unpack_from("<16s16sQQIIIIIIII", data, offset + 72 + 80 * index)
                if item[0].rstrip(b"\0") == name and item[1].rstrip(b"\0") == segment:
                    return item[2], item[4], item[3]
        offset += size
    raise ValueError((segment, name))


def verify_patch(original, replacement, expected_addresses):
    before, after = original.read_bytes(), replacement.read_bytes()
    assert len(before) == len(after), original
    address, start, size = section(before, b"__TEXT", b"__text")
    words = set()
    for index, (a, b) in enumerate(zip(before, after)):
        if a != b:
            assert start <= index < start + size, (original, index)
            words.add(address + ((index - start) // 4) * 4)
    assert words == set(expected_addresses), (original, sorted(words))
    return {"originalSHA256": sha(original), "replacementSHA256": sha(replacement),
            "changedInstructionAddresses": [hex(x) for x in sorted(words)],
            "allOtherBytesIdentical": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--handoff", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    handoff = args.handoff.resolve()
    tools = handoff.parent
    summary = json.loads((handoff / "build-summary.json").read_text())
    reference = Path(summary["app"])
    baseline = Path(summary["baseApp"])
    out = args.output.resolve()
    assert not out.exists(), "Choose a new output directory; existing evidence is preserved"
    out.mkdir(parents=True)
    assert sha(reference / "LSX4RuntimeProbe") == summary["hostSHA256"]
    run(["codesign", "--verify", "--deep", "--strict", str(reference)], out / "reference-signature.log")
    receipt = {"version": "128.9757", "sourceCommit": subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
        "referenceHostSHA256": summary["hostSHA256"], "installed": False,
        "published": False, "sourcePins": {}, "resources": {}, "nativePatches": {}}
    swift = json.loads((handoff / "scripts/swift-command.json").read_text())
    swift_out = out / "swift"
    swift_out.mkdir()
    source_names = []
    for index, value in enumerate(swift):
        if value.endswith(".swift"):
            source = repo / "App" / Path(value).name
            assert text(source) == text(Path(value)), source
            receipt["sourcePins"][str(source.relative_to(repo))] = sha(source)
            source_names.append(source.name)
            swift[index] = str(source)
        elif value.endswith("LSX4CrashCapture.h"):
            source = repo / "App/LSX4CrashCapture.h"
            assert text(source) == text(Path(value))
            swift[index] = str(source)
        elif "/ctl/swift/" in value:
            swift[index] = str(swift_out / Path(value).name)
    assert len(source_names) == 41
    run(swift, out / "swift-build.log")
    (out / "swift-command.json").write_text(json.dumps(swift, indent=2))

    scheduler_source = repo.parent / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp"
    assert text(scheduler_source) == text(tools / "ctl/vk_scheduler_mpb3.cpp")
    scheduler = json.loads((handoff / "scripts/sched-command.json").read_text())
    scheduler = [value.replace("iPhoneOS27.1.sdk", "iPhoneOS.sdk") for value in scheduler]
    scheduler[scheduler.index("-c") + 1] = str(scheduler_source)
    scheduler[scheduler.index("-o") + 1] = str(out / "vk_scheduler.o")
    run(scheduler, out / "scheduler-build.log")
    (out / "scheduler-command.json").write_text(json.dumps(scheduler, indent=2))
    receipt["sourcePins"][str(scheduler_source.relative_to(repo.parent))] = sha(scheduler_source)

    link = json.loads(Path(summary["linkCommand"]).read_text())
    replacements = {
        "ov": (next(x for x in link if x.endswith("/compile/liblsx4_console_overlay.a")),
               swift_out / "liblsx4_console_overlay.a"),
        "rs": (next(x for x in link if x.endswith("/draw-state-1289322/vk_rasterizer.o")), tools / "ctl/rs-nofan.o"),
        "vo": (next(x for x in link if x.endswith("/3491b8728e18327f/video-label-0.o")), tools / "ctl/vo-fix.o"),
        "bc": (next(x for x in link if x.endswith("/published-upload-1289325/buffer_cache.o")), tools / "ctl/bc-wfin.o"),
        "sched": (next(x for x in link if x.endswith("/sc156/vk_scheduler.cpp.o")), out / "vk_scheduler.o"),
    }
    for key, addresses in {"rs": [0xac04], "bc": [0x15d08],
                           "vo": [0x86b0, 0x86f4, 0x86f8, 0x86fc, 0x8700, 0x8710, 0x8714]}.items():
        original, replacement = replacements[key]
        receipt["nativePatches"][key] = verify_patch(Path(original), replacement, addresses)
    app = out / "LSX4RuntimeProbe.app"
    subprocess.run(["cp", "-cR", str(reference), str(app)], check=True)
    pins = {}
    for value in link:
        path = Path(value)
        if path.suffix in (".o", ".a") and path.is_file():
            pins[value] = sha(path)
    for old, new in replacements.values():
        assert link.count(old) == 1
        link = [str(new) if value == old else value for value in link]
    link = [value.replace("iPhoneOS27.1.sdk", "iPhoneOS.sdk") for value in link]
    link[link.index("-o") + 1] = str(app / "LSX4RuntimeProbe")
    run(link, out / "link.log")
    (out / "link-command.json").write_text(json.dumps(link, indent=2))
    for path in (repo / "Resources").rglob("*.json"):
        name = path.relative_to(repo / "Resources")
        if not (reference / name).is_file():
            continue
        assert path.read_bytes() == (reference / name).read_bytes(), name
        shutil.copyfile(path, app / name)
        receipt["resources"][str(name)] = sha(path)
    info = plistlib.loads((repo / "App/Info.plist").read_bytes())
    assert info == plistlib.loads((reference / "Info.plist").read_bytes())
    assert info["CFBundleVersion"] == "128.9757"
    (app / "Info.plist").write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
    # These locally saved commands contain signing identity paths, never credentials.
    work = Path(summary["linkCommand"]).parents[2]
    sign = json.loads((work / "bloodborne-60fps-20261028/page-mutex-trial/sign-private9565-command.json").read_text())
    sign[-1] = str(app)
    run(sign, out / "sign.log")
    run(["codesign", "--verify", "--deep", "--strict", str(app)], out / "verify.log")
    assert all(sha(Path(path)) == digest for path, digest in pins.items())
    receipt["frozenInputPins"] = pins
    receipt["replacements"] = {key: {"original": old, "replacement": str(new), "sha256": sha(new)}
                               for key, (old, new) in replacements.items()}
    receipt["app"] = str(app)
    receipt["hostSHA256"] = sha(app / "LSX4RuntimeProbe")
    receipt["deepSignatureVerified"] = True
    receipt["fullRuntimeRecompiled"] = False
    (out / "build-proof.json").write_text(json.dumps(receipt, indent=2))
    print(json.dumps({key: receipt[key] for key in ["app", "hostSHA256", "sourceCommit", "deepSignatureVerified"]}))


if __name__ == "__main__":
    main()
