#!/usr/bin/env python3

import argparse
import json
import struct
from pathlib import Path


MAGIC = 0x4C53583441373530
HEADER_FIELDS = (
    "magic",
    "version",
    "struct_size",
    "sequence",
    "committed_sequence",
    "tid",
    "caller",
    "command_buffer",
    "dependency_flags",
    "memory_barrier_count",
    "buffer_barrier_count",
    "image_barrier_count",
    "pnext",
    "anomaly",
    "captured_buffer_count",
    "captured_image_count",
)
BUFFER_FIELDS = (
    "handle",
    "src_stage",
    "src_access",
    "dst_stage",
    "dst_access",
    "offset",
    "size",
)
IMAGE_FIELDS = (
    "handle",
    "src_stage",
    "src_access",
    "dst_stage",
    "dst_access",
    "old_layout",
    "new_layout",
    "aspect",
    "base_mip",
    "levels",
    "base_layer",
    "layers",
    "reserved",
)
CAPTURE_SIZE = 768


def decode_capture(data: bytes) -> dict:
    if len(data) < CAPTURE_SIZE:
        raise ValueError(f"capture is truncated: {len(data)} < {CAPTURE_SIZE}")
    offset = 0
    header_values = struct.unpack_from("<16Q", data, offset)
    offset += struct.calcsize("<16Q")
    result = dict(zip(HEADER_FIELDS, header_values))
    if result["magic"] != MAGIC:
        raise ValueError(f"bad magic: 0x{result['magic']:x}")
    if result["version"] != 1 or result["struct_size"] != CAPTURE_SIZE:
        raise ValueError(
            f"unsupported capture version/size: {result['version']}/{result['struct_size']}"
        )
    result["committed"] = result["sequence"] == result["committed_sequence"]
    result["buffers"] = []
    for index in range(4):
        values = struct.unpack_from("<7Q", data, offset)
        offset += struct.calcsize("<7Q")
        if index < result["captured_buffer_count"]:
            result["buffers"].append(dict(zip(BUFFER_FIELDS, values)))
    result["images"] = []
    for index in range(4):
        values = struct.unpack_from("<13Q", data, offset)
        offset += struct.calcsize("<13Q")
        if index < result["captured_image_count"]:
            result["images"].append(dict(zip(IMAGE_FIELDS, values)))
    for key in ("magic", "caller", "command_buffer", "dependency_flags", "pnext", "anomaly"):
        result[f"{key}_hex"] = f"0x{result[key]:x}"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode exact-A750 Vulkan barrier capture")
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    print(json.dumps(decode_capture(args.capture.read_bytes()), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
