// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleVertexFetch.h"
#include "AppleGcnValidation.h"
#include <optional>
namespace Lsx4::Apple {
namespace {
using namespace Shader::Gcn;
GcnInst Decode(std::span<const std::uint32_t> words) {
  if (words.empty())
    throw std::runtime_error("Missing GCN instruction");
  std::array<std::uint32_t, 4> padded{};
  std::copy_n(words.begin(), std::min(words.size(), padded.size()),
              padded.begin());
  GcnDecodeContext decoder;
  GcnCodeSlice slice(padded.data(), padded.data() + padded.size());
  auto ins = decoder.decodeInstruction(slice);
  if (!ins.length || ins.length % 4 || ins.length > words.size_bytes())
    throw std::runtime_error("Truncated GCN instruction");
  return ins;
}
void Move(std::vector<std::uint32_t> &output, unsigned destination,
          unsigned source) {
  const std::array<std::uint32_t, 1> word{0xbe800300u | (destination << 16) |
                                          source};
  const auto decoded = Decode(word);
  if (decoded.opcode != Opcode::S_MOV_B32 ||
      decoded.dst[0].code != destination || decoded.src[0].code != source)
    throw std::runtime_error("GCN scalar move encoding changed");
  output.push_back(word[0]);
}
} // namespace
ResolvedVertexFetch ResolveVertexFetch(const GpuRegisters &values,
                                       std::span<const std::uint32_t> code,
                                       const GpuMemoryReader &read) {
  if (code.empty() || code.size() > 16384)
    throw std::runtime_error("Invalid vertex shader size");
  ResolvedVertexFetch result;
  result.registers = values;
  auto reg = [&](unsigned at) {
    auto p = values.find(at);
    return p == values.end() ? 0 : p->second;
  };
  const unsigned users = (reg(0x2c4b) >> 1) & 31;
  if (users > 16)
    throw std::runtime_error("Invalid vertex user registers");
  std::optional<std::size_t> callOffset;
  for (std::size_t offset = 0; offset < code.size();) {
    const auto ins = Decode(code.subspan(offset));
    if (ins.opcode == Opcode::S_SWAPPC_B64) {
      if (callOffset)
        throw std::runtime_error(
            "Multiple vertex fetch calls require an adapter");
      callOffset = offset;
    }
    offset += ins.length / 4;
  }
  if (!callOffset) {
    ValidateDirectGcn(code);
    result.code.assign(code.begin(), code.end());
    return result;
  }
  for (std::size_t offset = 0; offset < *callOffset;) {
    const auto ins = Decode(code.subspan(offset));
    if (ins.opcode != Opcode::S_MOV_B32 ||
        ins.dst[0].field != OperandField::VccHi)
      throw std::runtime_error("Vertex fetch is not a simple entry prologue");
    result.code.insert(result.code.end(), code.begin() + offset,
                       code.begin() + offset + ins.length / 4);
    offset += ins.length / 4;
  }
  const auto call = Decode(code.subspan(*callOffset));
  const unsigned source = call.src[0].code;
  if (source + 1 >= users || call.dst[0].code != 0 ||
      call.src[0].field != OperandField::ScalarGPR || call.dst[0].field != OperandField::ScalarGPR)
    throw std::runtime_error("Unsupported fetch pointer/return registers");
  const std::uint64_t fetchAddress =
      std::uint64_t(reg(0x2c4c + source)) |
      (std::uint64_t(reg(0x2c4d + source)) << 32);
  if (!fetchAddress || fetchAddress > UINT64_MAX - 4096)
    throw std::runtime_error("Invalid fetch address");
  struct Load {
    unsigned destination{}, slot{};
    std::array<std::uint32_t, 4> descriptor{};
  };
  std::vector<Load> loads;
  std::vector<std::uint32_t> fetchCode;
  const unsigned firstSlot = (users + 3) & ~3u;
  bool returned = false;
  for (unsigned offset = 0; offset < 4096;) {
    std::array<std::uint32_t, 4> words{};
    if (!read(fetchAddress + offset,
              {reinterpret_cast<std::uint8_t *>(words.data()), 4}))
      throw std::runtime_error("Fetch code GPU read failed");
    // Decode using padding to determine the complete instruction length,
    // then require and decode every actual byte before using it.
    auto ins = Decode(words);
    if (ins.length > 16 || ins.length > 4096-offset ||
        !read(fetchAddress + offset,
              {reinterpret_cast<std::uint8_t *>(words.data()), ins.length}))
      throw std::runtime_error("Truncated fetch code GPU range");
    ins = Decode(std::span(words).first(ins.length / 4));
    offset += ins.length;
    result.fetchBytes = offset;
    if (ins.opcode == Opcode::S_SETPC_B64) {
      if (ins.src[0].code != 0)
        throw std::runtime_error("Unsupported fetch return");
      returned = true;
      break;
    }
    if (ins.opcode == Opcode::S_LOAD_DWORDX4) {
      const unsigned base = ins.src[0].code * 2, dst = ins.dst[0].code,
                     slot = firstSlot + loads.size() * 4;
      if (!ins.control.smrd.imm || base + 1 >= users || dst < users ||
          dst + 4 > 16 || slot + 4 > 16)
        throw std::runtime_error(
            "Fetch descriptor load needs a larger SRT adapter");
      if(dst!=slot && dst<slot+4 && slot<dst+4)
        throw std::runtime_error("Fetch descriptor move overlaps its source");
      for (const auto &previous : loads)
        if (previous.destination < slot + 4 && slot < previous.destination + 4)
          throw std::runtime_error(
              "Fetch scalar writes overlap a future hoisted descriptor");
      const std::uint64_t pointer = std::uint64_t(reg(0x2c4c + base)) |
                                    (std::uint64_t(reg(0x2c4d + base)) << 32);
      const auto offsetBytes = std::uint64_t(ins.control.smrd.offset) * 4;
      if (!pointer || pointer > UINT64_MAX - offsetBytes - 16)
        throw std::runtime_error("Invalid fetch descriptor pointer");
      Load load{dst, slot};
      if (!read(pointer + offsetBytes,
                {reinterpret_cast<std::uint8_t *>(load.descriptor.data()), 16}))
        throw std::runtime_error("Fetch descriptor GPU read failed");
      for (unsigned i = 0; i < 4; ++i) {
        result.registers[0x2c4c + slot + i] = load.descriptor[i];
        if (dst + i != slot + i)
          Move(fetchCode, dst + i, slot + i);
      }
      loads.push_back(load);
    } else {
      const bool bufferLoad = ins.opcode == Opcode::BUFFER_LOAD_FORMAT_X ||
                              ins.opcode == Opcode::BUFFER_LOAD_FORMAT_XY ||
                              ins.opcode == Opcode::BUFFER_LOAD_FORMAT_XYZ ||
                              ins.opcode == Opcode::BUFFER_LOAD_FORMAT_XYZW;
      if (ins.opcode != Opcode::S_WAITCNT && !bufferLoad)
        throw std::runtime_error("Unsupported fetch prologue instruction");
      if (bufferLoad &&
          (loads.empty() || ins.src[2].code * 4 != loads.back().destination ||
           ins.control.mubuf.lds || ins.control.mubuf.tfe))
        throw std::runtime_error("Unsupported fetch buffer source");
      fetchCode.insert(fetchCode.end(), words.begin(),
                       words.begin() + ins.length / 4);
    }
  }
  if (!returned || loads.empty())
    throw std::runtime_error(
        "Fetch prologue has no bounded return/descriptors");
  result.code.insert(result.code.end(), fetchCode.begin(), fetchCode.end());
  const auto suffix = *callOffset + call.length / 4;
  for (std::size_t offset = suffix; offset < code.size();) {
    const auto ins = Decode(code.subspan(offset));
    if (ins.category != InstCategory::VectorALU &&
        ins.category != InstCategory::Export &&
        ins.opcode != Opcode::S_WAITCNT && ins.opcode != Opcode::S_ENDPGM)
      throw std::runtime_error(
          "Vertex shader after fetch requires a general call adapter");
    // The common inline-fetch route does not expose a native return PC.
    for (unsigned i = 0; i < ins.src_count; ++i)
      if (ins.src[i].field == OperandField::ScalarGPR && ins.src[i].code < 2)
        throw std::runtime_error(
            "Vertex shader consumes fetch return registers");
    result.code.insert(result.code.end(), code.begin() + offset,
                       code.begin() + offset + ins.length / 4);
    offset += ins.length / 4;
  }
  result.descriptorLoads = loads.size();
  const unsigned expanded = firstSlot + loads.size() * 4;
  result.registers[0x2c4b] = (reg(0x2c4b) & ~62u) | (expanded << 1);
  ValidateDirectGcn(result.code);
  return result;
}
} // namespace Lsx4::Apple
