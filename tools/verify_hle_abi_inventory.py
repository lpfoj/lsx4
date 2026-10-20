#!/usr/bin/env python3
"""Validate every compiled Android x86-64 HLE ABI descriptor in an ELF.

The production registration templates emit one local 304-byte descriptor
object per distinct C++ function type.  This checker deliberately reads the
ELF symbol table instead of maintaining a second source-level registry, so
LIB_FUNCTION, overrides, dynamic typed registrations, FreeType and raw
variadic registrations are covered by the same gate as the shipped binary.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import pathlib
import struct
import sys
from typing import Iterable


ELF_HEADER_SIZE = 64
SECTION_HEADER_SIZE = 64
SYMBOL_SIZE = 24
DESCRIPTOR_SIZE = 304
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_PROGBITS = 1
SHN_UNDEF = 0
SHN_XINDEX = 0xFFFF
SHF_WRITE = 1
SHF_ALLOC = 2

TYPED_MARKER = "RegisterAndroidX64TypedHleTarget"
RAW_MARKER = "RegisterAndroidX64RawHleTarget"
DESCRIPTOR_SUFFIX = "E10descriptor"

CLASS_NAMES = {
    0: "NO_CLASS",
    1: "INTEGER",
    2: "SSE",
    3: "SSEUP",
    4: "X87",
    5: "X87UP",
    6: "COMPLEX_X87",
    7: "MEMORY",
    0xFF: "UNSUPPORTED",
}


@dataclasses.dataclass(frozen=True)
class Section:
    name_offset: int
    kind: int
    flags: int
    address: int
    offset: int
    size: int
    link: int
    info: int
    alignment: int
    entry_size: int


@dataclasses.dataclass(frozen=True)
class Symbol:
    name: str
    section_index: int
    value: int
    size: int


@dataclasses.dataclass(frozen=True)
class Argument:
    size: int
    alignment: int
    first_class: int
    second_class: int
    eightbyte_count: int


@dataclasses.dataclass(frozen=True)
class Descriptor:
    symbol: Symbol
    descriptor_id: int
    signature: str
    arguments: tuple[Argument, ...]
    stack_argument_bytes: int
    call_flags: int
    gpr_argument_count: int
    sse_argument_count: int
    return_classes: tuple[int, int]
    return_eightbyte_count: int
    sret_flags: int
    return_size: int
    supported: bool


class ElfError(RuntimeError):
    pass


class ElfFile:
    def __init__(self, path: pathlib.Path):
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) < ELF_HEADER_SIZE or self.data[:4] != b"\x7fELF":
            raise ElfError(f"{path}: not an ELF file")
        if self.data[4] != 2 or self.data[5] != 1:
            raise ElfError(f"{path}: only ELF64 little-endian is supported")

        section_offset = struct.unpack_from("<Q", self.data, 40)[0]
        section_entry_size = struct.unpack_from("<H", self.data, 58)[0]
        section_count = struct.unpack_from("<H", self.data, 60)[0]
        self.section_name_index = struct.unpack_from("<H", self.data, 62)[0]
        if section_entry_size != SECTION_HEADER_SIZE or section_count == 0:
            raise ElfError(f"{path}: unsupported or missing section table")
        if section_offset + section_entry_size * section_count > len(self.data):
            raise ElfError(f"{path}: truncated section table")

        self.sections: list[Section] = []
        for index in range(section_count):
            fields = struct.unpack_from(
                "<IIQQQQIIQQ",
                self.data,
                section_offset + index * section_entry_size,
            )
            self.sections.append(Section(*fields))
        if self.section_name_index >= len(self.sections):
            raise ElfError(f"{path}: invalid section-name table")

        self.relative_relocations: dict[int, int] = {}
        for section in self.sections:
            if section.kind != SHT_RELA:
                continue
            if section.entry_size not in (0, 24):
                raise ElfError(f"{path}: unexpected RELA entry size")
            relocations = self.data[section.offset : section.offset + section.size]
            for offset in range(0, len(relocations), 24):
                if offset + 24 > len(relocations):
                    raise ElfError(f"{path}: truncated RELA section")
                target, info, addend = struct.unpack_from("<QQq", relocations, offset)


                if info >> 32 == 0 and (info & 0xFFFFFFFF) == 1027:
                    self.relative_relocations[target] = addend

    def section_bytes(self, index: int) -> bytes:
        if index >= len(self.sections):
            raise ElfError(f"{self.path}: invalid section index {index}")
        section = self.sections[index]
        end = section.offset + section.size
        if end > len(self.data):
            raise ElfError(f"{self.path}: truncated section {index}")
        return self.data[section.offset:end]

    @staticmethod
    def read_c_string(data: bytes, offset: int) -> str:
        if offset < 0 or offset >= len(data):
            return ""
        end = data.find(b"\0", offset)
        if end < 0:
            end = len(data)
        return data[offset:end].decode("utf-8", errors="replace")

    def read_virtual_c_string(self, address: int) -> str:
        for section in self.sections:
            if section.address <= address < section.address + section.size:
                relative = address - section.address
                start = section.offset + relative
                end = self.data.find(b"\0", start, section.offset + section.size)
                if end < 0:
                    end = section.offset + section.size
                return self.data[start:end].decode("utf-8", errors="replace")
        return ""

    def relocated_u64(self, field_address: int, stored_value: int) -> int:
        return self.relative_relocations.get(field_address, stored_value)

    def symbols(self) -> Iterable[Symbol]:
        for section in self.sections:
            if section.kind != SHT_SYMTAB:
                continue
            if section.entry_size not in (0, SYMBOL_SIZE):
                raise ElfError(f"{self.path}: unexpected symbol entry size")
            if section.link >= len(self.sections):
                raise ElfError(f"{self.path}: invalid symbol string table")
            symbols = self.data[section.offset : section.offset + section.size]
            strings = self.section_bytes(section.link)
            for offset in range(0, len(symbols), SYMBOL_SIZE):
                if offset + SYMBOL_SIZE > len(symbols):
                    raise ElfError(f"{self.path}: truncated symbol table")
                name_offset, _info, _other, section_index, value, size = (
                    struct.unpack_from("<IBBHQQ", symbols, offset)
                )
                if section_index in (SHN_UNDEF, SHN_XINDEX):
                    continue
                yield Symbol(
                    self.read_c_string(strings, name_offset),
                    section_index,
                    value,
                    size,
                )

    def symbol_bytes(self, symbol: Symbol) -> bytes:
        if symbol.section_index >= len(self.sections):
            raise ElfError(f"{self.path}: invalid symbol section")
        section = self.sections[symbol.section_index]
        relative = symbol.value - section.address
        if relative < 0 or relative + symbol.size > section.size:
            raise ElfError(f"{self.path}: symbol outside its section: {symbol.name}")
        start = section.offset + relative
        return self.data[start : start + symbol.size]


def is_descriptor_symbol(symbol: Symbol) -> bool:
    return (
        symbol.size == DESCRIPTOR_SIZE
        and symbol.name.endswith(DESCRIPTOR_SUFFIX)
        and (TYPED_MARKER in symbol.name or RAW_MARKER in symbol.name)
    )


def parse_descriptor(elf: ElfFile, symbol: Symbol) -> Descriptor:
    data = elf.symbol_bytes(symbol)
    descriptor_id, signature_address, _invoke = struct.unpack_from("<QQQ", data, 0)
    signature_address = elf.relocated_u64(symbol.value + 8, signature_address)
    argument_count, stack_bytes, call_flags = struct.unpack_from("<III", data, 280)
    if argument_count > 32:
        raise ElfError(f"{symbol.name}: argument_count={argument_count}")
    arguments: list[Argument] = []
    for index in range(argument_count):
        size, alignment, first, second, count, _reserved = struct.unpack_from(
            "<HHBBBB", data, 24 + index * 8
        )
        arguments.append(Argument(size, alignment, first, second, count))
    return Descriptor(
        symbol=symbol,
        descriptor_id=descriptor_id,
        signature=elf.read_virtual_c_string(signature_address),
        arguments=tuple(arguments),
        stack_argument_bytes=stack_bytes,
        call_flags=call_flags,
        gpr_argument_count=data[292],
        sse_argument_count=data[293],
        return_classes=(data[294], data[295]),
        return_eightbyte_count=data[296],
        sret_flags=data[297],
        return_size=struct.unpack_from("<H", data, 298)[0],
        supported=data[300] != 0,
    )


def fnv1a_byte(value: int, byte: int) -> int:
    return ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF


def hash_integer(value: int, integer: int, byte_count: int) -> int:
    for shift in range(0, byte_count * 8, 8):
        value = fnv1a_byte(value, (integer >> shift) & 0xFF)
    return value


def canonical_descriptor_id(descriptor: Descriptor) -> int:
    value = 1469598103934665603
    for byte in b"LSX4HLE\x03":
        value = fnv1a_byte(value, byte)
    value = hash_integer(value, len(descriptor.arguments), 4)
    value = hash_integer(value, descriptor.stack_argument_bytes, 4)
    value = hash_integer(value, descriptor.call_flags, 4)
    value = fnv1a_byte(value, descriptor.gpr_argument_count)
    value = fnv1a_byte(value, descriptor.sse_argument_count)
    value = fnv1a_byte(value, descriptor.return_classes[0])
    value = fnv1a_byte(value, descriptor.return_classes[1])
    value = fnv1a_byte(value, descriptor.return_eightbyte_count)
    value = fnv1a_byte(value, descriptor.sret_flags)
    value = hash_integer(value, descriptor.return_size, 2)
    for argument in descriptor.arguments:
        value = hash_integer(value, argument.size, 2)
        value = hash_integer(value, argument.alignment, 2)
        value = fnv1a_byte(value, argument.first_class)
        value = fnv1a_byte(value, argument.second_class)
        value = fnv1a_byte(value, argument.eightbyte_count)
    return value or 1


def select_elf(path: pathlib.Path) -> ElfFile:
    candidates = [pathlib.Path(str(path) + ".symbols"), path]
    errors: list[str] = []
    for candidate in candidates:
        if not candidate.is_file():
            continue
        try:
            return ElfFile(candidate)
        except ElfError as error:
            errors.append(str(error))
    raise ElfError("; ".join(errors) or f"{path}: file not found")


def scan_descriptors(elf: ElfFile) -> list[Descriptor]:
    """Find descriptors after ThinLTO has discarded all local symbols.

    Descriptor IDs are hashes of every wire-layout field, so matching the
    stored ID against a recomputed ID is a much stronger signature than a
    byte-pattern heuristic.  Production descriptors live in writable,
    allocated PROGBITS because their pointer fields require relocations.
    """

    descriptors: list[Descriptor] = []
    for section_index, section in enumerate(elf.sections):
        if (
            section.kind != SHT_PROGBITS
            or not section.flags & SHF_ALLOC
            or not section.flags & SHF_WRITE
            or section.size < DESCRIPTOR_SIZE
        ):
            continue
        data = elf.section_bytes(section_index)
        for offset in range(0, len(data) - DESCRIPTOR_SIZE + 1, 8):
            candidate = data[offset : offset + DESCRIPTOR_SIZE]
            address = section.address + offset
            argument_count = struct.unpack_from("<I", candidate, 280)[0]
            if (
                struct.unpack_from("<Q", candidate, 0)[0] == 0
                or address + 8 not in elf.relative_relocations
                or argument_count > 32
                or candidate[300] not in (0, 1)
                or candidate[292] > 6
                or candidate[293] > 8
            ):
                continue
            symbol = Symbol(
                name=f"descriptor@0x{address:x}",
                section_index=section_index,
                value=address,
                size=DESCRIPTOR_SIZE,
            )
            descriptor = parse_descriptor(elf, symbol)
            if (
                descriptor.descriptor_id != 0
                and descriptor.descriptor_id == canonical_descriptor_id(descriptor)
            ):
                descriptors.append(descriptor)
    return descriptors


def discover_descriptors(elf: ElfFile) -> list[Descriptor]:
    symbols = sorted(
        (symbol for symbol in elf.symbols() if is_descriptor_symbol(symbol)),
        key=lambda symbol: (symbol.value, symbol.name),
    )
    if symbols:
        return [parse_descriptor(elf, symbol) for symbol in symbols]
    return scan_descriptors(elf)


def validate(
    descriptors: list[Descriptor], expected_count: int | None, require_no_memory: bool
) -> tuple[list[str], collections.Counter[int]]:
    errors: list[str] = []
    classes: collections.Counter[int] = collections.Counter()
    if expected_count is not None and len(descriptors) != expected_count:
        errors.append(
            f"descriptor count is {len(descriptors)}, expected {expected_count}"
        )
    seen_addresses: set[int] = set()
    for descriptor in descriptors:
        label = descriptor.signature or descriptor.symbol.name
        if descriptor.symbol.value in seen_addresses:
            errors.append(f"duplicate descriptor address: {label}")
        seen_addresses.add(descriptor.symbol.value)
        if not descriptor.supported:
            errors.append(f"unsupported descriptor: {label}")
        if not descriptor.signature:
            errors.append(f"missing signature: {descriptor.symbol.name}")
        if descriptor.descriptor_id == 0:
            errors.append(f"zero descriptor id: {label}")
        expected_id = canonical_descriptor_id(descriptor)
        if descriptor.descriptor_id != expected_id:
            errors.append(
                f"non-canonical descriptor id: {label}: "
                f"0x{descriptor.descriptor_id:016x} != 0x{expected_id:016x}"
            )
        if descriptor.stack_argument_bytes % 8 != 0:
            errors.append(f"unaligned stack byte count: {label}")
        if descriptor.gpr_argument_count > 6 or descriptor.sse_argument_count > 8:
            errors.append(f"impossible register count: {label}")

        all_classes = [*descriptor.return_classes]
        for argument in descriptor.arguments:
            all_classes.extend((argument.first_class, argument.second_class))
            if argument.size == 0:
                errors.append(f"zero-sized argument: {label}")
            if (
                argument.alignment == 0
                or argument.alignment & (argument.alignment - 1)
                or argument.alignment > 16
            ):
                errors.append(
                    f"unsupported argument alignment {argument.alignment}: {label}"
                )
            if argument.eightbyte_count > 2:
                errors.append(f"too many argument eightbytes: {label}")
            if require_no_memory and 7 in (
                argument.first_class,
                argument.second_class,
            ):
                errors.append(f"unexpected MEMORY argument: {label}")

        for abi_class in all_classes:
            classes[abi_class] += 1
            if abi_class not in CLASS_NAMES:
                errors.append(f"unknown ABI class {abi_class}: {label}")
            elif abi_class == 0xFF:
                errors.append(f"UNSUPPORTED ABI class: {label}")
        if descriptor.return_eightbyte_count > 2:
            errors.append(f"too many return eightbytes: {label}")
    return errors, classes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=pathlib.Path)
    parser.add_argument("--expected-count", type=int)
    parser.add_argument("--require-no-memory", action="store_true")
    args = parser.parse_args()

    try:
        elf = select_elf(args.elf)
        descriptors = discover_descriptors(elf)
        errors, classes = validate(
            descriptors, args.expected_count, args.require_no_memory
        )
    except (ElfError, OSError, struct.error) as error:
        print(f"HLE_DESCRIPTOR_GATE ERROR {error}", file=sys.stderr)
        return 2

    variadic = sum(bool(descriptor.call_flags & 1) for descriptor in descriptors)
    fixed_fp = sum(
        2 in descriptor.return_classes
        or any(
            2 in (argument.first_class, argument.second_class)
            for argument in descriptor.arguments
        )
        for descriptor in descriptors
    )
    memory_args = sum(
        7 in (argument.first_class, argument.second_class)
        for descriptor in descriptors
        for argument in descriptor.arguments
    )
    sseup = classes[3]
    max_alignment = max(
        (
            argument.alignment
            for descriptor in descriptors
            for argument in descriptor.arguments
        ),
        default=0,
    )
    supported = sum(descriptor.supported for descriptor in descriptors)
    print(
        "HLE_DESCRIPTOR_GATE "
        f"elf={elf.path} total={len(descriptors)} supported={supported} "
        f"unsupported={len(descriptors) - supported} variadic={variadic} "
        f"fixed_fp={fixed_fp} memory_args={memory_args} sseup={sseup} "
        f"max_align={max_alignment}"
    )
    print(
        "HLE_DESCRIPTOR_CLASSES "
        + " ".join(
            f"{CLASS_NAMES.get(abi_class, str(abi_class))}={count}"
            for abi_class, count in sorted(classes.items())
        )
    )
    if errors:
        for error in errors[:50]:
            print(f"HLE_DESCRIPTOR_GATE FAIL {error}", file=sys.stderr)
        if len(errors) > 50:
            print(
                f"HLE_DESCRIPTOR_GATE FAIL ... and {len(errors) - 50} more",
                file=sys.stderr,
            )
        return 1
    print("HLE_DESCRIPTOR_GATE PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
