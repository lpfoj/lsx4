// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestImage.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <bit>
#include <map>

namespace Lsx4::Apple {
namespace {
[[noreturn]] void Invalid(const char* reason) { throw std::runtime_error(reason); }
void Range(std::uint64_t offset, std::uint64_t size, std::size_t length) {
    if (offset > length || size > length - offset) Invalid("Truncated executable segment or header");
}
std::uint64_t Read(std::span<const std::uint8_t> file, std::uint64_t offset, int bytes) {
    Range(offset, bytes, file.size());
    std::uint64_t result = 0;
    for (int i = 0; i < bytes; ++i) result |= std::uint64_t(file[offset + i]) << (8 * i);
    return result;
}
struct ContainerSegment { std::uint64_t flags, offset, bytes, memory; };
struct Program { std::uint32_t type, flags; std::uint64_t offset, address, bytes, memory, alignment; };
}

GuestImage GuestImage::Parse(std::span<const std::uint8_t> file) {
    GuestImage image;
    image.self = Read(file, 0, 4) == 0x1d3d154f;
    std::uint64_t elf = 0;
    std::vector<ContainerSegment> container;
    if (image.self) {
        const auto count = Read(file, 24, 2);
        if (!count || count > 4096) Invalid("Invalid SELF segment count");
        Range(32, count * 32, file.size());
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto offset = 32 + i * 32;
            ContainerSegment segment{Read(file, offset, 8), Read(file, offset + 8, 8),
                Read(file, offset + 16, 8), Read(file, offset + 24, 8)};
            if (segment.flags & 2) Invalid("Encrypted SELF is not supported; use your decrypted dump");
            if (segment.flags & 8) Invalid("Compressed SELF segment is not supported yet");
            Range(segment.offset, segment.bytes, file.size());
            container.push_back(segment);
        }
        elf = 32 + count * 32;
    }
    Range(elf, 64, file.size());
    if (Read(file, elf, 4) != 0x464c457f || Read(file, elf + 4, 1) != 2 ||
        Read(file, elf + 5, 1) != 1 || Read(file, elf + 6, 1) != 1 ||
        Read(file, elf + 18, 2) != 62 || Read(file, elf + 20, 4) != 1)
        Invalid("Expected little-endian x86-64 ELF version 1");
    const auto type = Read(file, elf + 16, 2);
    image.type = static_cast<std::uint16_t>(type);
    if (type != 2 && type != 3 && type != 0xfe00 && type != 0xfe10 && type != 0xfe18)
        Invalid("ELF is not an executable or shared module");
    image.entry = Read(file, elf + 24, 8);
    const auto phoff = Read(file, elf + 32, 8);
    const auto phnum = Read(file, elf + 56, 2);
    if (Read(file, elf + 52, 2) != 64 || Read(file, elf + 54, 2) != 56 || !phnum || phnum > 4096)
        Invalid("Invalid ELF program header table");
    if (phoff > file.size() - elf) Invalid("Invalid ELF table offset");
    Range(elf + phoff, phnum * 56, file.size());
    for (const auto& segment : container)
        if ((segment.flags & 0x800) && ((segment.flags >> 20) & 0xfff) >= phnum)
            Invalid("SELF references an absent program segment");
    std::vector<Program> programs;
    for (std::uint64_t i = 0; i < phnum; ++i) {
        const auto p = elf + phoff + i * 56;
        programs.push_back({static_cast<std::uint32_t>(Read(file,p,4)),
            static_cast<std::uint32_t>(Read(file,p+4,4)), Read(file,p+8,8), Read(file,p+16,8),
            Read(file,p+32,8), Read(file,p+40,8), Read(file,p+48,8)});
    }
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& p = programs[i];
        if (p.type != 1 && p.type != 0x61000010) continue; // PT_LOAD / PT_SCE_RELRO
        if (p.bytes > p.memory || p.memory > 512 * 1024 * 1024 - total ||
            p.address > std::numeric_limits<std::uint64_t>::max() - p.memory)
            Invalid("Invalid or oversized guest memory range");
        if (p.alignment > 1 && ((p.alignment & (p.alignment - 1)) ||
            p.address % p.alignment != p.offset % p.alignment)) Invalid("Invalid segment alignment");
        total += p.memory;
        if (!p.memory) continue;
        std::uint64_t source = p.offset;
        if (image.self && p.bytes) {
            const ContainerSegment* match = nullptr;
            for (const auto& segment : container) {
                if ((segment.flags & 0x800) && ((segment.flags >> 20) & 0xfff) == i) {
                    if (match) Invalid("Duplicate SELF program segment");
                    match = &segment;
                }
            }
            if (!match || p.bytes > match->bytes || p.bytes > match->memory)
                Invalid("Missing or short SELF load segment");
            source = match->offset;
        }
        Range(source, p.bytes, file.size());
        GuestSegment loaded{p.address, p.flags, std::vector<std::uint8_t>(p.memory, 0)};
        std::copy_n(file.data() + source, p.bytes, loaded.bytes.data());
        image.segments.push_back(std::move(loaded));
    }
    std::sort(image.segments.begin(), image.segments.end(), [](const auto& a, const auto& b) {return a.address < b.address;});
    for (std::size_t i = 1; i < image.segments.size(); ++i)
        if (image.segments[i-1].address + image.segments[i-1].bytes.size() > image.segments[i].address)
            Invalid("Overlapping guest load segments");
    if (image.segments.empty()) Invalid("No loadable guest segments");
    bool entryValid = false;
    for (const auto& s : image.segments)
        if ((s.flags & 1) && image.entry >= s.address && image.entry - s.address < s.bytes.size()) entryValid = true;
    if (!entryValid && type != 0xfe18) Invalid("Entry point is outside executable guest memory");

    // Program offsets in a SELF are offsets in the embedded ELF, not offsets
    // in the outer file. Dynamic records can be subranges of a blocked segment.
    auto contents = [&](std::uint64_t offset, std::uint64_t bytes) {
        if (!image.self || !bytes) {
            Range(offset, bytes, file.size());
            return file.subspan(offset, bytes);
        }
        for (const auto& c : container) {
            if (!(c.flags & 0x800)) continue;
            const auto& p = programs[(c.flags >> 20) & 0xfff];
            if (offset < p.offset || offset - p.offset > p.bytes) continue;
            const auto delta = offset - p.offset;
            if (bytes > p.bytes - delta || delta > c.bytes || bytes > c.bytes - delta) continue;
            Range(c.offset + delta, bytes, file.size());
            return file.subspan(c.offset + delta, bytes);
        }
        Invalid("SELF has no backing for dynamic data");
    };
    std::span<const std::uint8_t> dynamic, dynlib;
    for (const auto& p : programs) {
        if (p.type == 2) {
            if (!dynamic.empty()) Invalid("Duplicate dynamic program segment");
            dynamic = contents(p.offset, p.bytes);
        } else if (p.type == 0x61000000) {
            if (!dynlib.empty()) Invalid("Duplicate dynamic data segment");
            dynlib = contents(p.offset, p.bytes);
        } else if (p.type == 0x61000001) image.processParameters = p.address;
        else if (p.type==7 && p.memory) {
            if(p.bytes>p.memory || p.memory>64*1024*1024 ||
               (p.alignment>1 && (p.alignment&(p.alignment-1)))) Invalid("Invalid guest TLS layout");
            auto data=contents(p.offset,p.bytes);
            image.tlsInitial.assign(data.begin(),data.end());
            image.tlsBytes=p.memory;image.tlsAlignment=std::max<std::uint64_t>(p.alignment,1);
        }
    }
    if (!dynamic.empty()) {
        if (dynamic.size() % 16 || dynlib.empty()) Invalid("Unsupported or malformed dynamic layout");
        std::vector<std::pair<std::uint64_t,std::uint64_t>> entries;
        std::map<std::uint64_t,std::uint64_t> values;
        bool ended = false;
        for (std::size_t offset=0;offset<dynamic.size();offset+=16) {
            const auto tag=Read(dynamic,offset,8), value=Read(dynamic,offset+8,8);
            if (!tag) {ended=true;break;}
            entries.emplace_back(tag,value);values[tag]=value;
        }
        if (!ended) Invalid("Dynamic table has no terminator");
        auto table = [&](std::uint64_t pointerTag, std::uint64_t sizeTag) {
            const auto length=values[sizeTag], offset=values[pointerTag];
            Range(offset,length,dynlib.size());
            return dynlib.subspan(offset,length);
        };
        const auto strings=table(0x61000035,0x61000037);
        auto name = [&](std::uint64_t offset) {
            if (offset >= strings.size()) Invalid("Invalid dynamic string offset");
            const auto begin=strings.begin()+offset;
            const auto end=std::find(begin,strings.end(),0);
            if (end==strings.end()) Invalid("Unterminated dynamic string");
            return std::string(begin,end);
        };
        const auto symbols=table(0x61000039,0x6100003f);
        if (symbols.size()%24 || (!symbols.empty() && values[0x6100003b]!=24)) Invalid("Invalid symbol table stride");
        for (std::size_t offset=0;offset<symbols.size();offset+=24) {
            image.symbols.push_back({name(Read(symbols,offset,4)),Read(symbols,offset+8,8),
                Read(symbols,offset+16,8),static_cast<std::uint16_t>(Read(symbols,offset+6,2)),
                static_cast<std::uint8_t>(Read(symbols,offset+4,1))});
        }
        auto relocations = [&](std::uint64_t pointerTag,std::uint64_t sizeTag) {
            const auto relas=table(pointerTag,sizeTag);
            if (relas.size()%24) Invalid("Invalid relocation table stride");
            for (std::size_t offset=0;offset<relas.size();offset+=24) {
                const auto info=Read(relas,offset+8,8);
                const auto symbol=static_cast<std::uint32_t>(info>>32);
                if (symbol && symbol>=image.symbols.size()) Invalid("Relocation symbol is out of range");
                image.relocations.push_back({Read(relas,offset,8),static_cast<std::uint32_t>(info),
                    symbol,std::bit_cast<std::int64_t>(Read(relas,offset+16,8))});
            }
        };
        if (values[0x61000031] && values[0x61000033]!=24) Invalid("Unsupported RELA entry size");
        if (values[0x6100002d] && values[0x6100002b]!=7) Invalid("Only RELA PLT relocations are supported");
        relocations(0x6100002f,0x61000031);
        relocations(0x61000029,0x6100002d);
        image.hasInit=values.contains(12); image.init=values[12];
        for (auto [tag,value] : entries) {
            if (tag==1) image.needed.push_back(name(value));
            if (tag==0x6100000f || tag==0x61000015) {
                auto& imports = tag==0x6100000f ? image.importedModules : image.importedLibraries;
                imports.push_back({static_cast<std::uint16_t>(value>>48),name(value&0xffffffff)});
            }
            if (tag==0x6100000d || tag==0x61000013) {
                auto& exports = tag==0x6100000d ? image.exportedModules : image.exportedLibraries;
                exports.push_back({static_cast<std::uint16_t>(value>>48),name(value&0xffffffff)});
            }
        }
    }
    return image;
}

std::span<const std::uint8_t> GuestImage::BytesAt(std::uint64_t address) const {
    for (const auto& s : segments)
        if (address >= s.address && address - s.address < s.bytes.size())
            return std::span<const std::uint8_t>(s.bytes).subspan(address - s.address);
    return {};
}

std::span<std::uint8_t> GuestImage::MutableBytesAt(std::uint64_t address) {
    for (auto& s : segments)
        if (address>=s.address && address-s.address<s.bytes.size())
            return std::span<std::uint8_t>(s.bytes).subspan(address-s.address);
    return {};
}

void GuestImage::Relocate(std::uint64_t base,
    const std::function<std::optional<std::uint64_t>(const GuestSymbol&)>& resolver,
    std::uint64_t tlsModuleID) {
    if (relocated_) Invalid("Module was already relocated");
    struct Patch {std::uint64_t offset,value;};
    std::vector<Patch> patches;
    // Resolve and validate everything before writing any guest byte.
    for (const auto& r : relocations) {
        if (r.type==0) continue;
        if (BytesAt(r.offset).size()<8) Invalid("Relocation target is outside loaded memory");
        std::uint64_t value=0;
        if (r.type==8) value=base+static_cast<std::uint64_t>(r.addend);
        else if(r.type==16) value=tlsModuleID; // R_X86_64_DTPMOD64
        else if(r.type==17) { // R_X86_64_DTPOFF64, offset in module's TLS image
            if(r.symbol>=symbols.size() || !symbols[r.symbol].section) Invalid("Unsupported external TLS relocation");
            value=symbols[r.symbol].value+static_cast<std::uint64_t>(r.addend);
        }
        else if (r.type==1 || r.type==6 || r.type==7) {
            if (r.symbol>=symbols.size()) Invalid("Missing relocation symbol");
            const auto& symbol=symbols[r.symbol];
            std::optional<std::uint64_t> address;
            if (symbol.section==0xfff1) address=symbol.value; // SHN_ABS
            else if (symbol.section) address=base+symbol.value;
            else if (resolver) address=resolver(symbol);
            if (!address && (symbol.info>>4)==2) address=0; // unresolved weak ELF symbol
            if (!address) throw std::runtime_error("Unresolved import: "+symbol.name);
            value=*address+(r.type==1 ? static_cast<std::uint64_t>(r.addend) : 0);
        } else throw std::runtime_error("Unsupported relocation type: "+std::to_string(r.type));
        patches.push_back({r.offset,value});
    }
    for (const auto& p : patches) {
        auto bytes=MutableBytesAt(p.offset);
        for (int i=0;i<8;++i) bytes[i]=static_cast<std::uint8_t>(p.value>>(i*8));
    }
    relocated_=true;
}
}
