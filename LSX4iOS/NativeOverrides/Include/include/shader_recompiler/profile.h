// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader {

enum class CodegenQuirk : u8 {
    BrokenSpirvClamp = 1U << 0,
    DeferFormattedBufferTypesUntilLowering = 1U << 1,
    LowerAliasedSharedU64ToU32 = 1U << 2,
    LowerNon32BitSharedToU32 = 1U << 3,
    SegmentedBdaPageTable = 1U << 4,
    // Exact zero reads/discarded writes are required by guest compute buffers.
    RequireExactComputeBufferBounds = 1U << 5,
};

struct Profile {
    u64 max_ubo_size{};
    u32 max_viewport_width{};
    u32 max_viewport_height{};
    u32 max_shared_memory_size{};
    u32 supported_spirv{0x00010000};
    u32 subgroup_size{};
    bool support_int8{};
    bool support_int16{};
    bool support_int64{};
    bool support_float16{};
    bool support_float64{};
    bool support_fp32_denorm_preserve{};
    bool support_fp32_denorm_flush{};
    bool support_fp32_round_to_zero{};
    bool support_legacy_vertex_attributes{};
    bool supports_image_load_store_lod{};
    bool supports_native_cube_calc{};
    bool supports_trinary_minmax{};
    bool supports_robust_buffer_access{};
    bool supports_buffer_fp32_atomic_min_max{};
    bool supports_image_fp32_atomic_min_max{};
    bool supports_buffer_int64_atomics{};
    bool supports_shared_int64_atomics{};
    bool supports_workgroup_explicit_memory_layout{};
    bool supports_amd_shader_explicit_vertex_parameter{};
    bool supports_fragment_shader_barycentric{};
    bool has_incomplete_fragment_shader_barycentric{};
    bool has_broken_spirv_float_controls{};
    // This byte replaces the former has_broken_spirv_clamp bool. Keeping the
    // field one byte wide preserves the serialized Profile layout, while the
    // bit mask gives device-scoped code generation workarounds distinct cache
    // identities without invalidating unaffected devices.
    u8 codegen_quirks{};
    bool lower_left_origin_mode{};
    bool needs_manual_interpolation{};
    bool needs_lds_barriers{};
    bool needs_buffer_offsets{};
    bool needs_unorm_fixup{};
    bool needs_clip_distance_emulation{};
    bool supports_shader_stencil_export{};
    bool supports_buffer_device_address{};

    [[nodiscard]] constexpr bool HasCodegenQuirk(const CodegenQuirk quirk) const {
        return (codegen_quirks & static_cast<u8>(quirk)) != 0;
    }

    bool operator==(const Profile&) const = default;
};

static_assert(sizeof(CodegenQuirk) == sizeof(u8));
// Profile is serialized as raw bytes. Replacing the former bool with the u8
// mask must not move fields or invalidate zero-mask caches on other devices.
static_assert(sizeof(Profile) == 64);
static_assert(static_cast<u8>(CodegenQuirk::BrokenSpirvClamp) == 1U);
static_assert(static_cast<u8>(CodegenQuirk::DeferFormattedBufferTypesUntilLowering) == 2U);
static_assert(static_cast<u8>(CodegenQuirk::LowerAliasedSharedU64ToU32) == 4U);
static_assert(static_cast<u8>(CodegenQuirk::LowerNon32BitSharedToU32) == 8U);
static_assert(static_cast<u8>(CodegenQuirk::SegmentedBdaPageTable) == 16U);
static_assert(Profile{}.codegen_quirks == 0U);
static_assert(Profile{.codegen_quirks =
                          static_cast<u8>(CodegenQuirk::BrokenSpirvClamp)}
                  .HasCodegenQuirk(CodegenQuirk::BrokenSpirvClamp));
static_assert(!Profile{.codegen_quirks =
                           static_cast<u8>(CodegenQuirk::BrokenSpirvClamp)}
                   .HasCodegenQuirk(CodegenQuirk::DeferFormattedBufferTypesUntilLowering));

}
