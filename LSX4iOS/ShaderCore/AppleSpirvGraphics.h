// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <spirv/unified1/spirv.hpp11>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace Lsx4::Apple {
// The synchronous route only issues vertexOffset=0. The shared emitter always
// declares BaseVertex and DrawParameters, even for this fixed-zero case.
// Specialize that one input to a private zero so the module remains Vulkan 1.0
// and does not require an unenabled shader_draw_parameters extension.
inline std::vector<std::uint32_t>
ZeroBaseVertex(std::span<const std::uint32_t> source) {
  using namespace spv;
  if (source.size() < 5 || source[0] != 0x07230203 ||
      source[3] > UINT32_MAX - 2)
    throw std::runtime_error("Invalid vertex SPIR-V header");
  std::vector<std::span<const std::uint32_t>> instructions;
  std::uint32_t variable = 0, pointer = 0, pointee = 0;
  for (std::size_t at = 5; at < source.size();) {
    const auto count = source[at] >> 16;
    if (!count || count > source.size() - at)
      throw std::runtime_error("Invalid SPIR-V instruction length");
    auto words = source.subspan(at, count);
    instructions.push_back(words);
    at += count;
    if (Op(words[0] & 65535) == Op::OpDecorate && count >= 4 &&
        words[2] == unsigned(Decoration::BuiltIn)) {
      if (words[3] == unsigned(BuiltIn::BaseVertex)) {
        if (variable)
          throw std::runtime_error("Multiple BaseVertex inputs");
        variable = words[1];
      } else if (words[3] == unsigned(BuiltIn::BaseInstance) ||
                 words[3] == unsigned(BuiltIn::DrawIndex))
        throw std::runtime_error("Other draw parameters require an adapter");
    }
  }
  if (!variable)
    throw std::runtime_error("Shared vertex BaseVertex declaration changed");
  for (auto words : instructions)
    if (Op(words[0] & 65535) == Op::OpVariable && words.size() >= 4 &&
        words[2] == variable) {
      if (words.size() != 4 || words[3] != unsigned(StorageClass::Input))
        throw std::runtime_error("Invalid BaseVertex input");
      pointer = words[1];
    }
  for (auto words : instructions)
    if (Op(words[0] & 65535) == Op::OpTypePointer && words.size() == 4 &&
        words[1] == pointer) {
      if (words[2] != unsigned(StorageClass::Input))
        throw std::runtime_error("Invalid BaseVertex pointer");
      pointee = words[3];
    }
  bool unsigned32 = false;
  for (auto words : instructions)
    if (Op(words[0] & 65535) == Op::OpTypeInt && words.size() == 4 &&
        words[1] == pointee)
      unsigned32 = words[2] == 32 && words[3] == 0;
  if (!unsigned32)
    throw std::runtime_error("BaseVertex is not u32");
  const auto privatePointer = source[3], zero = source[3] + 1;
  std::vector<std::uint32_t> output(source.begin(), source.begin() + 5);
  output[3] += 2;
  for (auto words : instructions) {
    const auto opcode = Op(words[0] & 65535);
    if (opcode == Op::OpCapability && words.size() == 2 &&
        words[1] == unsigned(Capability::DrawParameters))
      continue;
    if (opcode == Op::OpExtension) {
      const std::string_view text(
          reinterpret_cast<const char *>(words.data() + 1),
          (words.size() - 1) * 4);
      constexpr std::string_view extension = "SPV_KHR_shader_draw_parameters";
      if (text.starts_with(extension) && text.size() > extension.size() &&
          text[extension.size()] == '\0')
        continue;
    }
    if (opcode == Op::OpDecorate && words.size() >= 3 && words[1] == variable) {
      if (words.size() != 4 || words[2] != unsigned(Decoration::BuiltIn) ||
          words[3] != unsigned(BuiltIn::BaseVertex))
        throw std::runtime_error("Unexpected BaseVertex decoration");
      continue;
    }
    if (opcode == Op::OpVariable && words.size() >= 3 && words[2] == variable) {
      output.insert(output.end(),
                    {(4u << 16) | unsigned(Op::OpTypePointer), privatePointer,
                     unsigned(StorageClass::Private), pointee,
                     (4u << 16) | unsigned(Op::OpConstant), pointee, zero, 0,
                     (5u << 16) | unsigned(Op::OpVariable), privatePointer,
                     variable, unsigned(StorageClass::Private), zero});
      continue;
    }
    if (opcode == Op::OpEntryPoint) {
      std::size_t end = 3;
      bool terminated = false;
      for (; end < words.size(); ++end) {
        const auto word = words[end];
        if (!(word & 255) || !(word & 65280) || !(word & 16711680) ||
            !(word & 4278190080u)) {
          ++end;
          terminated = true;
          break;
        }
      }
      if (!terminated)
        throw std::runtime_error("Invalid SPIR-V entry point name");
      std::vector<std::uint32_t> entry(words.begin(), words.begin() + end);
      for (; end < words.size(); ++end)
        if (words[end] != variable)
          entry.push_back(words[end]);
      entry[0] = (std::uint32_t(entry.size()) << 16) | unsigned(opcode);
      output.insert(output.end(), entry.begin(), entry.end());
      continue;
    }
    output.insert(output.end(), words.begin(), words.end());
  }
  return output;
}
} // namespace Lsx4::Apple
