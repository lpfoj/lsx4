// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleGraphicsShader.h"
#include "AppleGcnValidation.h"
#include "AppleSpirvGraphics.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/texture_cache/tile.h"
#include <cmath>
namespace Lsx4::Apple {
GpuRegisters DefaultGpuRegisters() {
  auto regs = std::make_unique<AmdGpu::Regs>();
  regs->SetDefaults();
  GpuRegisters result;
  for (unsigned i = 0; i < regs->NumRegs; ++i)
    if (regs->reg_array[i])
      result[i] = regs->reg_array[i];
  return result;
}
namespace {
std::unique_ptr<AmdGpu::Regs> ReadRegisters(const GpuRegisters &values) {
  auto regs = std::make_unique<AmdGpu::Regs>();
  std::vector<u32> words(AmdGpu::Regs::NumRegs);
  for (auto [key, value] : values) {
    if (key >= regs->NumRegs)
      throw std::runtime_error("Graphics register outside range");
    words[key] = value;
  }
  // Copy the complete object representation before reading typed fields.
  // Writing reg_array and then reading a different union view in this local
  // function allowed Clang to reuse fields from value initialization.
  std::memcpy(regs.get(), words.data(), sizeof(*regs));
  return regs;
}
void FillRasterState(const AmdGpu::Regs *regs, DepthClearDrawState &out) {
  const auto &vp = regs->viewports[0];
  const auto &control = regs->viewport_control;
  const float zoffset = control.zoffset_enable ? vp.zoffset : 0.f,
              zscale = control.zscale_enable ? vp.zscale : 1.f;
  out.viewport[4] = std::max(0.f, regs->clipper_control.clip_space ==
                                          AmdGpu::ClipSpace::MinusWToW
                                      ? zoffset - zscale
                                      : zoffset);
  out.viewport[5] = std::min(1.f, zoffset + zscale);
  if (regs->IsClipDisabled()) {
    out.viewport[0] = 0;
    out.viewport[1] = 0;
    out.viewport[2] = 16384;
    out.viewport[3] = 16384;
  } else {
    const float xoffset = control.xoffset_enable ? vp.xoffset : 0.f,
                xscale = control.xscale_enable ? vp.xscale : 1.f;
    const float yoffset = control.yoffset_enable ? vp.yoffset : 0.f,
                yscale = control.yscale_enable ? vp.yscale : 1.f;
    out.viewport[0] = xoffset - xscale;
    out.viewport[1] = yoffset - yscale;
    out.viewport[2] = xscale * 2;
    out.viewport[3] = yscale * 2;
  }
  const bool enableOffset = !regs->window_scissor.window_offset_disable;
  const auto xo = enableOffset ? regs->window_offset.window_x_offset : 0,
             yo = enableOffset ? regs->window_offset.window_y_offset : 0;
  int left = std::max({0, int(regs->screen_scissor.top_left_x),
                       int(regs->window_scissor.top_left_x) + xo,
                       int(regs->generic_scissor.top_left_x) + xo});
  int top = std::max({0, int(regs->screen_scissor.top_left_y),
                      int(regs->window_scissor.top_left_y) + yo,
                      int(regs->generic_scissor.top_left_y) + yo});
  int right =
      std::min({int(out.width), int(regs->screen_scissor.bottom_right_x),
                int(regs->window_scissor.bottom_right_x) + xo,
                int(regs->generic_scissor.bottom_right_x) + xo});
  int bottom =
      std::min({int(out.height), int(regs->screen_scissor.bottom_right_y),
                int(regs->window_scissor.bottom_right_y) + yo,
                int(regs->generic_scissor.bottom_right_y) + yo});
  if (regs->mode_control.vport_scissor_enable) {
    const auto &scissor = regs->viewport_scissors[0];
    left = std::max(left, int(scissor.top_left_x));
    top = std::max(top, int(scissor.top_left_y));
    right = std::min(right, int(scissor.bottom_right_x));
    bottom = std::min(bottom, int(scissor.bottom_right_y));
  }
  left = std::min(left, int(out.width));
  top = std::min(top, int(out.height));
  right = std::max(left, right);
  bottom = std::max(top, bottom);
  out.scissor = {unsigned(left), unsigned(top), unsigned(right - left),
                 unsigned(bottom - top)};
  out.clearDepth = regs->depth_clear;
  out.cullMode = unsigned(regs->polygon_control.CullingMode());
  out.clockwise =
      regs->polygon_control.front_face == AmdGpu::FrontFace::Clockwise;
  out.depthCompare = unsigned(regs->depth_control.depth_func);
  out.instanceCount = regs->num_instances.NumInstances();
}

} // namespace
CompiledGraphicsShader CompileGraphicsShader(const GraphicsShaderInput &input) {
  auto regs = ReadRegisters(input.registers);
  if (regs->num_interp > 32 || regs->reg_array[0xa207] ||
      regs->stage_enable.raw || regs->clipper_control.user_clip_plane_enable)
    throw std::runtime_error(
        "Additional graphics stages/outputs require an adapter");
  if (regs->primitive_type != AmdGpu::PrimitiveType::TriangleList &&
      regs->primitive_type != AmdGpu::PrimitiveType::RectList)
    throw std::runtime_error("Unsupported graphics primitive");
  CompiledGraphicsShader result;
  Shader::PushData push{};
  Shader::Backend::Bindings bindings{};
  push.xoffset =
      regs->viewport_control.xoffset_enable ? regs->viewports[0].xoffset : 0.f;
  push.xscale =
      regs->viewport_control.xscale_enable ? regs->viewports[0].xscale : 1.f;
  push.yoffset =
      regs->viewport_control.yoffset_enable ? regs->viewports[0].yoffset : 0.f;
  push.yscale =
      regs->viewport_control.yscale_enable ? regs->viewports[0].yscale : 1.f;
  if (!std::isfinite(push.xoffset) || !std::isfinite(push.yoffset) ||
      !std::isfinite(push.xscale) || !std::isfinite(push.yscale))
    throw std::runtime_error("Nonfinite graphics viewport");
  for (bool fragment : {false, true}) {
    const auto code = fragment ? input.fragmentCode : input.vertexCode;
    const auto &pgm = fragment ? regs->ps_program : regs->vs_program;
    if (code.empty() || code.size() > 16384 ||
        pgm.settings.num_user_regs > 16 || pgm.settings.scratch_en ||
        pgm.settings.oc_lds_en)
      throw std::runtime_error("Unsupported graphics program requirements");
    ValidateDirectGcn(code);
    const auto stage =
        fragment ? Shader::Stage::Fragment : Shader::Stage::Vertex;
    const auto logical = fragment ? Shader::LogicalStage::Fragment
                                  : Shader::LogicalStage::Vertex;
    Shader::ShaderParams params{std::span<const u32, 16>(pgm.user_data), code,
                                0};
    Shader::Info info(stage, logical, params);
    info.pgm_base = pgm.Address<u64>();
    union Holder {
      Shader::RuntimeInfo runtime;
      Holder() {}
      ~Holder() {}
    } holder;
    auto &rt = holder.runtime;
    rt.Initialize(stage);
    rt.num_user_data = pgm.settings.num_user_regs;
    rt.num_input_vgprs = pgm.settings.vgpr_comp_cnt;
    rt.num_allocated_vgprs = pgm.NumVgprs();
    rt.fp_denorm_mode32 = pgm.settings.fp_denorm_mode32;
    rt.fp_round_mode32 = pgm.settings.fp_round_mode32;
    if (fragment) {
      rt.fs_info.en_flags = regs->ps_input_ena;
      rt.fs_info.addr_flags = regs->ps_input_addr;
      rt.fs_info.num_inputs = regs->num_interp;
      rt.fs_info.z_export_format = regs->z_export_format;
      rt.fs_info.mrtz_mask =
          regs->depth_shader_control.z_export_enable |
          ((regs->depth_shader_control.stencil_op_val_export_enable |
            regs->depth_shader_control.stencil_test_val_export_enable)
           << 1) |
          (regs->depth_shader_control.mask_export_enable << 2) |
          (regs->depth_shader_control.coverage_to_mask_enable << 3);
      for (unsigned i = 0; i < regs->num_interp; ++i) {
        const auto &v = regs->ps_inputs[i];
        rt.fs_info.inputs[i] = {u8(v.input_offset), bool(v.use_default),
                                bool(v.flat_shade), u8(v.default_value)};
      }
      for (unsigned i = 0; i < Shader::MaxColorBuffers; ++i) {
        const auto &c = regs->color_buffers[i];
        if (!c || !regs->color_target_mask.GetMask(i))
          continue;
        if (c.info.format != 10 || c.info.number_type != 0)
          throw std::runtime_error("Graphics color format requires an adapter");
        auto &out = rt.fs_info.color_buffers[i];
        out.data_format = c.GetDataFmt();
        out.num_format = c.GetNumberFmt();
        out.num_conversion = c.GetNumberConversion();
        out.export_format = regs->color_export_format.GetFormat(i);
        out.swizzle = c.Swizzle();
      }
    } else {
      rt.vs_info.step_rate_0 = regs->vgt_instance_step_rate_0;
      rt.vs_info.step_rate_1 = regs->vgt_instance_step_rate_1;
      rt.vs_info.clip_disable = regs->IsClipDisabled();
      rt.vs_info.emulate_depth_negative_one_to_one =
          regs->clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
      rt.vs_info.tess_emulated_primitive =
          regs->primitive_type == AmdGpu::PrimitiveType::RectList;
    }
    Shader::Profile profile{};
    profile.subgroup_size = 32;
    profile.max_ubo_size = 16384;
    profile.max_shared_memory_size = 16384;
    profile.max_viewport_width = 16384;
    profile.max_viewport_height = 16384;
    // Use the shared hardware-interpolation route; no barycentric extension is
    // enabled.
    profile.needs_manual_interpolation = false;
    Shader::Pools pools;
    auto program = Shader::TranslateProgram(code, pools, info, rt, profile);
    if (info.translation_failed || !info.srt_info.portable_nodes.empty() ||
        !info.fmasks.empty())
      throw std::runtime_error("Unsupported graphics resources or translation");
    if (bindings.user_data + info.ud_mask.NumRegs() >
        Shader::NUM_USER_DATA_REGS)
      throw std::runtime_error("Graphics push constants exceed portable limit");
    // The Vulkan 1.0 route keeps a positive viewport. Reflect clip-space Y
    // for a negative guest Y scale, preserving the same framebuffer mapping.
    // This uses the shared IR and does not require an optional Vulkan route.
    if (!fragment && !regs->IsClipDisabled() && push.yscale < 0) {
      if (regs->primitive_type != AmdGpu::PrimitiveType::TriangleList)
        throw std::runtime_error(
            "Negative-height tessellation viewport requires an adapter");
      for (auto block : program.blocks)
        for (auto it = block->begin(); it != block->end(); ++it) {
          if (it->GetOpcode() != Shader::IR::Opcode::SetAttribute ||
              it->Arg(0).Attribute() != Shader::IR::Attribute::Position0 ||
              it->Arg(2).U32() != 1)
            continue;
          Shader::IR::IREmitter emit(*block, it);
          it->SetArg(1, emit.FPNeg(Shader::IR::F32{it->Arg(1)}));
        }
      result.reflectedViewportY = true;
    }
    auto pushed = bindings;
    auto resourceBinding = bindings.unified;
    auto &spirv = fragment ? result.fragment : result.vertex;
    spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, program, bindings);
    if (!fragment)
      spirv = ZeroBaseVertex(spirv);
    info.PushUd(pushed, push);
    for (const auto &resource : info.buffers) {
      if (resource.is_written)
        throw std::runtime_error("Graphics storage writes require an adapter");
      ComputeBufferBinding binding;
      if (resource.buffer_type == Shader::BufferType::Flatbuf) {
        auto flat = info.FlattenedUserData();
        binding.size = flat.size_bytes();
        binding.inlineBytes.resize(binding.size);
        std::memcpy(binding.inlineBytes.data(), flat.data(), binding.size);
      } else if (resource.buffer_type == Shader::BufferType::Guest) {
        auto sharp = resource.GetSharp(info);
        binding.address = sharp.base_address;
        binding.size = std::uint64_t(sharp.num_records) *
                       (sharp.stride ? sharp.stride : 1);
        if (!sharp.Valid() || !binding.size || binding.size > UINT32_MAX)
          throw std::runtime_error("Invalid graphics buffer descriptor");
      } else
        throw std::runtime_error("Unsupported special graphics buffer");
      result.bufferBindings.push_back(resourceBinding++);
      result.buffers.push_back(std::move(binding));
    }
    for (const auto &resource : info.images) {
      const auto sharp = resource.GetSharp(info);
      if (!fragment || resource.is_depth || resource.is_atomic ||
          resource.is_array || resource.is_written || resource.is_r128 ||
          resource.NumBindings(info) != 1 || !sharp || !sharp.Valid() ||
          sharp.GetType() != AmdGpu::ImageType::Color2D || sharp.base_level ||
          sharp.last_level || sharp.base_array || sharp.last_array ||
          sharp.depth || sharp.pow2pad || sharp.alt_tile_mode ||
          sharp.compression_en || sharp.interlaced || sharp.min_lod ||
          sharp.GetNumberFmt() != AmdGpu::NumberFormat::Unorm)
        throw std::runtime_error("Image requires a general texture adapter");
      GraphicsImageBinding out;
      out.binding = resourceBinding++;
      out.address = sharp.Address();
      out.width = sharp.width + 1;
      out.height = sharp.height + 1;
      out.pitch = sharp.Pitch();
      out.tileMode = unsigned(sharp.GetTileMode());
      unsigned pixelBytes = 0;
      if (sharp.GetDataFmt() == AmdGpu::DataFormat::Format8_8_8_8) {
        out.format = SampledPixelFormat::Rgba8Unorm;
        pixelBytes = 4;
      } else if (sharp.GetDataFmt() == AmdGpu::DataFormat::Format5_6_5) {
        out.format = SampledPixelFormat::Rgb565Unorm;
        pixelBytes = 2;
      } else
        throw std::runtime_error(
            "Sampled texture pixel format requires an adapter");
      if (out.width > out.pitch || out.pitch > 16384 || out.height > 16384 ||
          (out.tileMode != 8 && out.tileMode != 31))
        throw std::runtime_error("Sampled texture layout requires an adapter");
      out.size = std::uint64_t(out.pitch) * out.height * pixelBytes;
      if (out.size > 64ull * 1024 * 1024)
        throw std::runtime_error("Texture staging budget exceeded");
      const auto mapping = sharp.DstSelect();
      out.swizzle = {unsigned(mapping.r), unsigned(mapping.g),
                     unsigned(mapping.b), unsigned(mapping.a)};
      result.images.push_back(out);
    }
    for (const auto &resource : info.samplers) {
      const auto sharp = resource.GetSharp(info);
      if (!fragment || sharp.force_unnormalized ||
          sharp.filter_mode != AmdGpu::FilterMode::Blend ||
          sharp.xy_min_filter > AmdGpu::Filter::Bilinear ||
          sharp.xy_mag_filter > AmdGpu::Filter::Bilinear ||
          sharp.mip_filter > AmdGpu::MipFilter::Linear ||
          sharp.border_color_type == AmdGpu::BorderColor::Custom ||
          sharp.lod_bias_sec || sharp.force_degamma)
        throw std::runtime_error(
            "Sampler requires an additional filtering adapter");
      GraphicsSamplerBinding out;
      out.binding = resourceBinding++;
      out.minFilter = unsigned(sharp.xy_min_filter.Value());
      out.magFilter = unsigned(sharp.xy_mag_filter.Value());
      out.mipFilter = unsigned(sharp.mip_filter.Value());
      out.clamp = {unsigned(sharp.clamp_x.Value()),
                   unsigned(sharp.clamp_y.Value()),
                   unsigned(sharp.clamp_z.Value())};
      for (auto clamp : out.clamp)
        if (clamp != 0 && clamp != 1 && clamp != 2 && clamp != 6)
          throw std::runtime_error(
              "Sampler border/mirror mode requires an adapter");
      out.border = unsigned(sharp.border_color_type.Value());
      out.minLod = sharp.MinLod();
      out.maxLod = sharp.MaxLod();
      out.lodBias = sharp.LodBias();
      result.samplers.push_back(out);
    }
    if (resourceBinding != bindings.unified)
      throw std::runtime_error("Graphics binding accounting mismatch");
    if (fragment && regs->primitive_type == AmdGpu::PrimitiveType::RectList) {
      using namespace Shader::Backend::SPIRV;
      result.tessControl = EmitAuxilaryTessShader(
          AuxShaderType::RectListTCS, rt.fs_info, profile.supported_spirv);
      result.tessEvaluation = EmitAuxilaryTessShader(
          AuxShaderType::PassthroughTES, rt.fs_info, profile.supported_spirv);
    }
  }
  std::memcpy(result.pushConstants.data(), &push, sizeof(push));
  return result;
}
} // namespace Lsx4::Apple
namespace Lsx4::Apple {
GuestDepthClearDraw BuildDepthClearDraw(const GpuRegisters &values,
                                        std::uint32_t hint) {
  auto regs = ReadRegisters(values);
  const auto &db = regs->depth_buffer;
  if (regs->reg_array[0xa08e] || regs->reg_array[0xa000] != 1 ||
      !regs->depth_control.depth_enable || regs->depth_control.stencil_enable ||
      regs->depth_control.depth_bounds_enable ||
      db.z_info.format != AmdGpu::DepthBuffer::ZFormat::Z32Float ||
      db.NumSamples() != 1 || !db.DepthValid() ||
      db.DepthAddress() != db.DepthWriteAddress() || regs->reg_array[0xa002] ||
      regs->polygon_control.PolyMode() != AmdGpu::PolygonMode::Fill ||
      regs->polygon_control.NeedsBias() ||
      regs->polygon_control.enable_window_offset || regs->index_offset ||
      regs->clipper_control.dx_rasterization_kill ||
      regs->aa_config.NumSamples() != 1)
    throw std::runtime_error(
        "Graphics attachments/state require a general draw adapter");
  for (unsigned i = 1; i < AmdGpu::NUM_VIEWPORTS; ++i)
    if (regs->viewports[i].xscale != 0)
      throw std::runtime_error("Multiple viewports require an adapter");
  GuestDepthClearDraw result;
  auto &out = result.state;
  result.address = db.DepthAddress();
  result.size = db.GetDepthSliceSize();
  result.pitch = db.Pitch();
  result.allocationHeight = db.Height();
  result.tileMode = unsigned(db.GetTileMode());
  out.width = hint ? hint & 65535 : result.pitch;
  out.height = hint ? hint >> 16 : result.allocationHeight;
  if (!out.width || !out.height || out.width > result.pitch ||
      out.height > result.allocationHeight ||
      result.size !=
          std::uint64_t(result.pitch) * result.allocationHeight * 4 ||
      result.size > 128ull * 1024 * 1024)
    throw std::runtime_error("Invalid depth surface extent");
  FillRasterState(regs.get(), out);
  return result;
}
} // namespace Lsx4::Apple

namespace Lsx4::Apple {
GuestColorDraw BuildColorDraw(const GpuRegisters &values) {
  auto regs = ReadRegisters(values);
  const auto &cb = regs->color_buffers[0];
  if (!cb || regs->color_target_mask.raw > 15 || !regs->color_target_mask.raw ||
      regs->depth_control.depth_enable || regs->depth_control.stencil_enable ||
      regs->depth_control.depth_bounds_enable || regs->reg_array[0xa000] ||
      regs->primitive_type != AmdGpu::PrimitiveType::TriangleList ||
      regs->color_control.mode != AmdGpu::ColorControl::OperationMode::Normal ||
      regs->color_control.rop3 != AmdGpu::ColorControl::LogicOp::Copy ||
      regs->blend_control[0].enable ||
      regs->polygon_control.PolyMode() != AmdGpu::PolygonMode::Fill ||
      regs->polygon_control.NeedsBias() ||
      regs->polygon_control.enable_window_offset || regs->index_offset ||
      regs->clipper_control.dx_rasterization_kill ||
      regs->aa_config.NumSamples() != 1 || cb.info.format != 10 ||
      cb.info.number_type != 0 || cb.NumSamples() != 1 ||
      cb.attrib.num_samples_log2 || cb.BaseSlice() || cb.NumSlices() != 1 ||
      cb.info.endian != AmdGpu::ColorBuffer::EndianSwap::None ||
      cb.info.fast_clear || cb.info.compression || cb.info.dcc_enable ||
      cb.info.alt_tile_mode || cb.attrib.force_dst_alpha_1)
    throw std::runtime_error(
        "Color attachments/state require a general draw adapter");
  for (unsigned i = 1; i < AmdGpu::NUM_VIEWPORTS; ++i)
    if (regs->viewports[i].xscale != 0)
      throw std::runtime_error("Multiple viewports require an adapter");
  GuestColorDraw result;
  result.address = cb.Address();
  result.size = cb.GetColorSliceSize();
  result.pitch = cb.Pitch();
  result.allocationHeight = cb.Height();
  result.tileMode = unsigned(cb.GetTileMode());
  auto &out = result.state.raster;
  out.width = result.pitch;
  out.height = result.allocationHeight;
  if (!out.width || !out.height || out.width > 16384 || out.height > 16384 ||
      result.size != std::uint64_t(out.width) * out.height * 4 ||
      result.size > 64ull * 1024 * 1024)
    throw std::runtime_error("Invalid color surface extent");
  FillRasterState(regs.get(), out);
  result.state.writeMask = regs->color_target_mask.raw;
  if (out.viewport[3] < 0) {
    out.viewport[1] += out.viewport[3];
    out.viewport[3] = -out.viewport[3];
  }
  return result;
}
} // namespace Lsx4::Apple

namespace Lsx4::Apple {
DisplaySurfaceGeometry DisplaySurface(std::uint32_t width,std::uint32_t height,std::uint32_t pitch,bool tiled) {
    if(!width || !height || width>pitch || pitch>16384 || height>16384)throw std::runtime_error("Invalid VideoOut extent");
    DisplaySurfaceGeometry result;result.tileMode=tiled?10:8;
    if(tiled)std::tie(result.pitch,result.height,result.size)=VideoCore::ImageSizeMacroTiled((width+127)&~127u,height,1,32,1,AmdGpu::TileMode::Display2DThin,0,false);
    else {result.pitch=pitch;result.height=height;result.size=std::uint64_t(pitch)*height*4;}
    if(result.size>64ull*1024*1024)throw std::runtime_error("VideoOut staging budget exceeded");
    return result;
}
}
