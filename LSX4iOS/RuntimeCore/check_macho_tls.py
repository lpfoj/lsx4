#!/usr/bin/env python3
"""Reject unexpectedly large per-thread storage in the linked Apple runtime."""
import argparse
import json
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--limit", type=int, default=2 * 1024 * 1024)
    args = parser.parse_args()
    data = args.binary.read_bytes()
    if data[:4] != b"\xcf\xfa\xed\xfe":
        raise SystemExit("Expected a thin 64-bit Mach-O runtime")
    offset = 32
    sections = []
    for _ in range(struct.unpack_from("<I", data, 16)[0]):
        command, size = struct.unpack_from("<II", data, offset)
        if command == 0x19:  # LC_SEGMENT_64
            for index in range(struct.unpack_from("<I", data, offset + 64)[0]):
                section = offset + 72 + 80 * index
                flags = struct.unpack_from("<I", data, section + 64)[0]
                if flags & 0xFF in (0x11, 0x12):  # TLS regular / zero-fill
                    address, length = struct.unpack_from("<QQ", data, section + 32)
                    sections.append((address, length))
        offset += size
    # dyld copies/zeros one image TLS template per accessing thread. Count
    # alignment gaps too, rather than just adding the section payload sizes.
    span = (max(a + n for a, n in sections) - min(a for a, _ in sections)) if sections else 0
    passed = span <= args.limit
    print(json.dumps({"machOTlsBytesPerThread": span, "limit": args.limit, "passed": passed}))
    if not passed:
        raise SystemExit("Move large Apple thread-owned payloads out of the native TLS template")


if __name__ == "__main__":
    main()
