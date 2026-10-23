// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "AppleComputeShader.h"
#include <map>
namespace Lsx4::Apple {
using GpuRegisters = std::map<std::uint32_t, std::uint32_t>;
GpuRegisters DefaultGpuRegisters();
struct DepthClearDrawState {
  std::uint32_t width{}, height{}, vertexCount{}, instanceCount{};
  std::array<float, 6> viewport{};
  std::array<std::uint32_t, 4> scissor{};
  float clearDepth{};
  std::uint32_t cullMode{}, depthCompare{};
  bool clockwise{};
};
struct GuestDepthClearDraw {
  DepthClearDrawState state;
  std::uint64_t address{}, size{};
  std::uint32_t pitch{}, allocationHeight{}, tileMode{};
};
GuestDepthClearDraw BuildDepthClearDraw(const GpuRegisters &registers,
                                        std::uint32_t extentHint);
struct ColorDrawState {
  DepthClearDrawState raster;
  std::uint32_t writeMask{};
};
struct GuestColorDraw {
  ColorDrawState state;
  std::uint64_t address{}, size{};
  std::uint32_t pitch{}, allocationHeight{}, tileMode{};
};
GuestColorDraw BuildColorDraw(const GpuRegisters &registers);
enum class SampledPixelFormat { Rgba8Unorm, Rgb565Unorm };
struct GraphicsImageBinding {
  std::uint32_t binding{}, width{}, height{}, pitch{}, tileMode{};
  std::uint64_t address{}, size{};
  SampledPixelFormat format{};
  std::array<std::uint32_t, 4> swizzle{};
};
struct GraphicsSamplerBinding {
  std::uint32_t binding{}, minFilter{}, magFilter{}, mipFilter{}, border{};
  std::array<std::uint32_t, 3> clamp{};
  float minLod{}, maxLod{}, lodBias{};
};
struct DisplaySurfaceGeometry {
  std::uint32_t pitch{},height{},tileMode{};
  std::uint64_t size{};
};
DisplaySurfaceGeometry DisplaySurface(std::uint32_t width,std::uint32_t height,std::uint32_t pitch,bool tiled);
struct GraphicsShaderInput {
  const GpuRegisters &registers;
  std::span<const std::uint32_t> vertexCode, fragmentCode;
};
struct CompiledGraphicsShader {
  std::vector<std::uint32_t> vertex, fragment, tessControl, tessEvaluation;
  std::vector<ComputeBufferBinding> buffers;
  std::vector<std::uint32_t> bufferBindings;
  std::vector<GraphicsImageBinding> images;
  std::vector<GraphicsSamplerBinding> samplers;
  bool reflectedViewportY{};
  std::array<std::uint8_t, 128> pushConstants{};
};
// Bounded direct-descriptor route. Fetch/SRT, extra stages and shader memory
// writes are rejected before any guest memory is handed to the shared compiler.
CompiledGraphicsShader CompileGraphicsShader(const GraphicsShaderInput &input);
} // namespace Lsx4::Apple
