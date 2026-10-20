package app.lsx4.android;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;


final class ElfVulkanDriverValidator {
    private static final int ELF_HEADER_SIZE = 64;
    private static final int PROGRAM_HEADER_SIZE = 56;
    private static final int SECTION_HEADER_SIZE = 64;
    private static final int SYMBOL_SIZE = 24;
    private static final int DYNAMIC_SIZE = 16;
    private static final int PT_LOAD = 1;
    private static final int SHT_STRTAB = 3;
    private static final int SHT_DYNAMIC = 6;
    private static final int SHT_DYNSYM = 11;
    private static final long HARDWARE_MODULE_TAG = 0x48574d54L;
    private static final int HWVULKAN_MODULE_API_VERSION_0_1 = 0x0001;
    private static final int MIN_HMI_SIZE = 248;
    private static final int MAX_TABLE_ENTRIES = 1_000_000;

    private static final class LoadSegment {
        final long offset;
        final long virtualAddress;
        final long fileSize;

        LoadSegment(long offset, long virtualAddress, long fileSize) {
            this.offset = offset;
            this.virtualAddress = virtualAddress;
            this.fileSize = fileSize;
        }
    }

    private static final class Section {
        final int type;
        final long offset;
        final long size;
        final int link;
        final long entrySize;

        Section(int type, long offset, long size, int link, long entrySize) {
            this.type = type;
            this.offset = offset;
            this.size = size;
            this.link = link;
            this.entrySize = entrySize;
        }
    }

    private ElfVulkanDriverValidator() {
    }

    static void validate(File library) throws IOException {
        try (RandomAccessFile input = new RandomAccessFile(library, "r")) {
            long length = input.length();
            requireRange(0, ELF_HEADER_SIZE, length, "ELF header");
            if (u8(input, 0) != 0x7f || u8(input, 1) != 'E'
                    || u8(input, 2) != 'L' || u8(input, 3) != 'F'
                    || u8(input, 4) != 2 || u8(input, 5) != 1
                    || u16(input, 16) != 3 || u16(input, 18) != 0xb7) {
                throw new IOException("Driver is not an ELF64 little-endian AArch64 DSO");
            }

            long programOffset = nonNegative(u64(input, 32), "program header offset");
            long sectionOffset = nonNegative(u64(input, 40), "section header offset");
            int programEntrySize = u16(input, 54);
            int programCount = u16(input, 56);
            int sectionEntrySize = u16(input, 58);
            int sectionCount = u16(input, 60);
            if (programEntrySize < PROGRAM_HEADER_SIZE || programCount <= 0
                    || programCount > 4096) {
                throw new IOException("Invalid ELF program header table");
            }
            if (sectionEntrySize < SECTION_HEADER_SIZE || sectionCount <= 0
                    || sectionCount > 16384) {
                throw new IOException("Invalid ELF section header table");
            }
            requireTable(programOffset, programEntrySize, programCount, length,
                    "program header table");
            requireTable(sectionOffset, sectionEntrySize, sectionCount, length,
                    "section header table");

            List<LoadSegment> loads = new ArrayList<>();
            for (int index = 0; index < programCount; ++index) {
                long base = programOffset + (long) index * programEntrySize;
                if (u32(input, base) != PT_LOAD) {
                    continue;
                }
                long offset = nonNegative(u64(input, base + 8), "segment offset");
                long virtualAddress = nonNegative(u64(input, base + 16),
                        "segment virtual address");
                long fileSize = nonNegative(u64(input, base + 32), "segment size");
                requireRange(offset, fileSize, length, "load segment");
                loads.add(new LoadSegment(offset, virtualAddress, fileSize));
            }
            if (loads.isEmpty()) {
                throw new IOException("ELF has no file-backed load segment");
            }

            Section[] sections = new Section[sectionCount];
            for (int index = 0; index < sectionCount; ++index) {
                long base = sectionOffset + (long) index * sectionEntrySize;
                int type = (int) u32(input, base + 4);
                long offset = nonNegative(u64(input, base + 24), "section offset");
                long size = nonNegative(u64(input, base + 32), "section size");
                int link = (int) u32(input, base + 40);
                long entrySize = nonNegative(u64(input, base + 56), "section entry size");
                if (type != 8) {
                    requireRange(offset, size, length, "section");
                }
                sections[index] = new Section(type, offset, size, link, entrySize);
            }

            validateHmi(input, length, sections, loads);
            validateDependencies(input, sections);
        }
    }

    private static void validateHmi(RandomAccessFile input, long length, Section[] sections,
                                    List<LoadSegment> loads) throws IOException {
        int hmiCount = 0;
        long hmiVirtualAddress = 0;
        long hmiSize = 0;
        for (Section symbols : sections) {
            if (symbols.type != SHT_DYNSYM) {
                continue;
            }
            if (symbols.link <= 0 || symbols.link >= sections.length
                    || sections[symbols.link].type != SHT_STRTAB
                    || symbols.entrySize < SYMBOL_SIZE
                    || symbols.size % symbols.entrySize != 0) {
                throw new IOException("Malformed ELF dynamic symbol table");
            }
            long count = symbols.size / symbols.entrySize;
            if (count > MAX_TABLE_ENTRIES) {
                throw new IOException("ELF dynamic symbol table is too large");
            }
            Section strings = sections[symbols.link];
            for (long index = 0; index < count; ++index) {
                long base = symbols.offset + index * symbols.entrySize;
                long nameOffset = u32(input, base);
                if (nameOffset >= strings.size
                        || !"HMI".equals(readString(input, strings, nameOffset))) {
                    continue;
                }
                int info = u8(input, base + 4);
                int other = u8(input, base + 5);
                int sectionIndex = u16(input, base + 6);
                int binding = info >>> 4;
                int type = info & 0x0f;
                int visibility = other & 0x03;
                long value = nonNegative(u64(input, base + 8), "HMI address");
                long size = nonNegative(u64(input, base + 16), "HMI size");
                if (sectionIndex == 0 || (binding != 1 && binding != 2) || type != 1
                        || (visibility != 0 && visibility != 3) || size < MIN_HMI_SIZE) {
                    throw new IOException("HMI is not a valid exported Android HAL module");
                }
                hmiCount++;
                hmiVirtualAddress = value;
                hmiSize = size;
            }
        }
        if (hmiCount != 1) {
            throw new IOException("Driver must define exactly one exported HMI module");
        }

        long hmiOffset = -1;
        for (LoadSegment load : loads) {
            if (hmiVirtualAddress < load.virtualAddress) {
                continue;
            }
            long delta = hmiVirtualAddress - load.virtualAddress;
            if (delta <= load.fileSize && hmiSize <= load.fileSize - delta) {
                hmiOffset = load.offset + delta;
                break;
            }
        }
        if (hmiOffset < 0) {
            throw new IOException("HMI is not fully file-backed");
        }
        requireRange(hmiOffset, hmiSize, length, "HMI");
        long tag = u32(input, hmiOffset);
        int moduleApi = u16(input, hmiOffset + 4);
        int halApi = u16(input, hmiOffset + 6);
        if (tag != HARDWARE_MODULE_TAG || moduleApi != HWVULKAN_MODULE_API_VERSION_0_1
                || (halApi != 0x0000 && halApi != 0x0100)) {
            throw new IOException("HMI has an incompatible Android Vulkan HAL header");
        }
    }

    private static void validateDependencies(RandomAccessFile input, Section[] sections)
            throws IOException {
        boolean foundDynamic = false;
        for (Section dynamic : sections) {
            if (dynamic.type != SHT_DYNAMIC) {
                continue;
            }
            foundDynamic = true;
            if (dynamic.link <= 0 || dynamic.link >= sections.length
                    || sections[dynamic.link].type != SHT_STRTAB
                    || dynamic.entrySize < DYNAMIC_SIZE
                    || dynamic.size % dynamic.entrySize != 0) {
                throw new IOException("Malformed ELF dynamic dependency table");
            }
            long count = dynamic.size / dynamic.entrySize;
            if (count > MAX_TABLE_ENTRIES) {
                throw new IOException("ELF dynamic dependency table is too large");
            }
            Section strings = sections[dynamic.link];
            int neededCount = 0;
            for (long index = 0; index < count; ++index) {
                long base = dynamic.offset + index * dynamic.entrySize;
                long tag = u64(input, base);
                if (tag == 0) {
                    break;
                }
                if (tag != 1) {
                    continue;
                }
                if (++neededCount > 128) {
                    throw new IOException("Driver has too many shared-library dependencies");
                }
                long nameOffset = nonNegative(u64(input, base + 8),
                        "dependency name offset");
                String name = readString(input, strings, nameOffset);
                String lower = name.toLowerCase(Locale.ROOT);
                if (name.isEmpty() || name.length() > 128
                        || !name.matches("[A-Za-z0-9._+\\-]+")
                        || lower.contains(".so.") || lower.startsWith("ld-linux")
                        || lower.contains("x11") || lower.contains("xcb")
                        || lower.contains("wayland") || lower.contains("glibc")) {
                    throw new IOException("Unsupported driver dependency: " + name);
                }
            }
        }
        if (!foundDynamic) {
            throw new IOException("ELF has no dynamic dependency table");
        }
    }

    private static String readString(RandomAccessFile input, Section strings, long relative)
            throws IOException {
        if (relative < 0 || relative >= strings.size) {
            throw new IOException("ELF string offset is outside its table");
        }
        long maximum = Math.min(strings.size - relative, 512);
        StringBuilder value = new StringBuilder();
        input.seek(strings.offset + relative);
        for (long index = 0; index < maximum; ++index) {
            int character = input.read();
            if (character < 0) {
                break;
            }
            if (character == 0) {
                return value.toString();
            }
            if (character < 0x20 || character > 0x7e) {
                throw new IOException("ELF string table contains an invalid name");
            }
            value.append((char) character);
        }
        throw new IOException("ELF string is unterminated or too long");
    }

    private static void requireTable(long offset, long entrySize, long count, long length,
                                     String label) throws IOException {
        if (entrySize <= 0 || count < 0 || count > Long.MAX_VALUE / entrySize) {
            throw new IOException("Invalid " + label);
        }
        requireRange(offset, entrySize * count, length, label);
    }

    private static void requireRange(long offset, long size, long length, String label)
            throws IOException {
        if (offset < 0 || size < 0 || offset > length || size > length - offset) {
            throw new IOException(label + " lies outside the driver file");
        }
    }

    private static long nonNegative(long value, String label) throws IOException {
        if (value < 0) {
            throw new IOException("Unsupported 64-bit " + label);
        }
        return value;
    }

    private static int u8(RandomAccessFile input, long offset) throws IOException {
        input.seek(offset);
        int value = input.read();
        if (value < 0) {
            throw new IOException("Unexpected end of ELF");
        }
        return value;
    }

    private static int u16(RandomAccessFile input, long offset) throws IOException {
        return u8(input, offset) | (u8(input, offset + 1) << 8);
    }

    private static long u32(RandomAccessFile input, long offset) throws IOException {
        return (long) u16(input, offset) | ((long) u16(input, offset + 2) << 16);
    }

    private static long u64(RandomAccessFile input, long offset) throws IOException {
        return u32(input, offset) | (u32(input, offset + 4) << 32);
    }
}
