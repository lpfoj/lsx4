#!/usr/bin/env python3
"""Adapt the shared ELF assembly's object-format notation to Mach-O.

Instructions and register contracts remain in the original source. Unknown
directives fail in the assembler; this is not a second copy of the JIT stubs.
"""
import argparse
import re
from pathlib import Path


def convert(source):
    symbols = set(re.findall(r"^\s*\.global\s+(\w+)\s*$", source, re.M))
    symbols.update(re.findall(r"^\s*bl\s+(\w+)\s*$", source, re.M))
    symbols.update(re.findall(r"^\s*adrp\s+\w+,\s*(\w+)\s*$", source, re.M))
    if not symbols:
        raise ValueError("No shared assembly symbols found")
    result = []
    for line in source.splitlines():
        stripped = line.strip()
        if stripped.startswith((".type ", ".size ")):
            continue
        if stripped == '.section .note.GNU-stack,"",%progbits':
            continue
        if stripped.startswith(".hidden "):
            line = line.replace(".hidden ", ".private_extern ")
        if re.match(r"\s*adrp\s", line):
            line = re.sub(r",\s*(\w+)\s*$", r", \1@PAGE", line)
        line = re.sub(r":lo12:(\w+)", r"\1@PAGEOFF", line)
        for symbol in symbols:
            line = re.sub(r"(?<![\w.])" + re.escape(symbol) + r"(?!\w)", "_" + symbol, line)
        result.append(line)
    return "// Generated from the shared JIT assembly. Do not edit.\n" + "\n".join(result) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.write_text(convert(args.source.read_text()))
