// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/dynamic_translation/artifact_repository.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <span>
#include <string_view>
#include <unordered_set>

namespace {

using Executor::Jit::JitIrCacheIdentity;
using Executor::Jit::JitIrCacheLimits;

std::uint64_t ReadLe64(const std::span<const std::uint8_t, 8> bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8u);
    }
    return value;
}

bool ReadIdentity(const std::filesystem::path& path, JitIrCacheIdentity& identity) {
    std::array<std::uint8_t, 56> header{};
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(header.data()), header.size())) {
        return false;
    }
    constexpr std::array<std::uint8_t, 8> magic{'E', 'X', 'B', 'I', 'R', 'C', '0', '1'};
    if (!std::equal(magic.begin(), magic.end(), header.begin())) {
        return false;
    }
    identity.jit_abi_version = ReadLe64(std::span<const std::uint8_t, 8>{header.data() + 16, 8});
    std::copy_n(header.begin() + 24, identity.content_key.size(), identity.content_key.begin());
    return true;
}

bool ParseAddress(const std::string_view text, std::uint64_t& value) {
    std::string_view digits = text;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2);
    }
    const auto [end, error] = std::from_chars(
        digits.data(), digits.data() + digits.size(), value, 16);
    return error == std::errc{} && end == digits.data() + digits.size();
}

void DumpRecord(const Executor::Jit::JitIrCacheRecord& record,
                const std::filesystem::path& output_directory) {
    std::cout << "record rip=0x" << std::hex << record.guest_rip
              << " start=0x" << record.block.start_rip
              << " end=0x" << record.block.end_rip << std::dec
              << " ops=" << record.block.instructions.size()
              << " validationBytes=" << record.block.validation_bytes.size()
              << " nativeFlags=0x" << std::hex << record.native_flags << std::dec
              << " entrySegment=" << record.entry_segment_index
              << " directSegment=" << record.direct_segment_index
              << " staticGprSegment=" << record.static_gpr_segment_index
              << " staticGprOffset=" << record.static_gpr_entry_offset
              << " segments=" << record.native_segments.size() << '\n';

    for (std::size_t index = 0; index < record.block.instructions.size(); ++index) {
        const auto& instruction = record.block.instructions[index];
        std::cout << "  ir[" << index << "] rip=0x" << std::hex
                  << instruction.guest_rip << std::dec
                  << " len=" << static_cast<unsigned>(instruction.length)
                  << " mnemonic=" << instruction.mnemonic
                  << " category=" << static_cast<unsigned>(instruction.category)
                  << " operands=" << static_cast<unsigned>(instruction.operand_count)
                  << " terminates=" << instruction.terminates_block
                  << " initialNative=" << instruction.eligible_for_initial_native
                  << " bytes=";
        for (std::size_t byte = 0; byte < instruction.length; ++byte) {
            std::cout << std::hex << std::setfill('0') << std::setw(2)
                      << static_cast<unsigned>(instruction.bytes[byte]);
        }
        std::cout << std::dec << '\n';
    }

    std::error_code error;
    std::filesystem::create_directories(output_directory, error);
    for (std::size_t index = 0; index < record.native_segments.size(); ++index) {
        const auto& segment = record.native_segments[index];
        const auto name = "jit-" + [&] {
            std::ostringstream stream;
            stream << std::hex << record.guest_rip << "-seg" << index << ".bin";
            return stream.str();
        }();
        const auto path = output_directory / name;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(segment.bytes.data()),
                     static_cast<std::streamsize>(segment.bytes.size()));
        std::cout << "  segment[" << index << "] bytes=" << segment.bytes.size()
                  << " relocs=" << segment.relocs.size()
                  << " output=" << path.string() << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: jit_cache_inspect <cache-dir> <output-dir> <guest-rip>...\n";
        return 2;
    }
    std::unordered_set<std::uint64_t> targets;
    for (int index = 3; index < argc; ++index) {
        std::uint64_t address = 0;
        if (!ParseAddress(argv[index], address)) {
            std::cerr << "invalid guest address: " << argv[index] << '\n';
            return 3;
        }
        targets.insert(address);
    }

    JitIrCacheLimits limits{};
    limits.max_file_bytes = 1024ull * 1024ull * 1024ull;
    limits.max_records = 2'000'000;
    limits.max_total_instructions = 16'000'000;
    limits.max_total_native_bytes = 1024ull * 1024ull * 1024ull;
    std::size_t shards = 0;
    std::size_t records = 0;
    std::size_t found = 0;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(argv[1], error), end;
         !error && iterator != end && found < targets.size(); iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || iterator->path().extension() != ".bbir") {
            continue;
        }
        JitIrCacheIdentity identity{};
        if (!ReadIdentity(iterator->path(), identity)) {
            continue;
        }
        const auto result = Executor::Jit::LoadJitIrCacheFile(
            iterator->path(), identity, limits);
        ++shards;
        if (!result.ok()) {
            std::cerr << "skip " << iterator->path() << ": " << result.detail << '\n';
            continue;
        }
        records += result.records.size();
        for (const auto& record : result.records) {
            if (!targets.contains(record.guest_rip)) {
                continue;
            }
            std::cout << "shard=" << iterator->path().string() << '\n';
            DumpRecord(record, argv[2]);
            targets.erase(record.guest_rip);
            ++found;
        }
    }
    std::cout << "summary shards=" << shards << " records=" << records
              << " found=" << found << " missing=" << targets.size() << '\n';
    return targets.empty() ? 0 : 4;
}
