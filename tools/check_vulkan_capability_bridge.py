#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


PROMOTED_RAW_CALL = re.compile(
    r"\bvkCmd(?:BeginRendering|EndRendering|PipelineBarrier2|PushDescriptorSetKHR)(?:KHR)?\s*\("
)
MOBILE_DRIVER_ENUM = re.compile(
    r"vk::DriverId::e(?:(?:Arm|Qualcomm)Proprietary|MesaTurnip)"
)
MOBILE_VENDOR_LITERAL = re.compile(r"0x(?:13b5|5143)\b", re.IGNORECASE)

METHOD_ALLOWLIST = {
    re.compile(r"\.beginRendering\s*\("): {
        "video_core/renderer_vulkan/vk_scheduler.cpp",
        "video_core/renderer_vulkan/vk_presenter.cpp",
        "video_core/renderer_vulkan/host_passes/pp_pass.cpp",
    },
    re.compile(r"\.endRendering\s*\("): {
        "video_core/renderer_vulkan/vk_scheduler.cpp",
        "video_core/renderer_vulkan/vk_presenter.cpp",
        "video_core/renderer_vulkan/host_passes/pp_pass.cpp",
    },
    re.compile(r"\.pushDescriptorSetKHR\s*\("): {
        "video_core/renderer_vulkan/vk_scheduler.cpp",
        "video_core/renderer_vulkan/vk_pipeline_common.cpp",
        "video_core/renderer_vulkan/host_passes/fsr_pass.cpp",
        "video_core/renderer_vulkan/host_passes/pp_pass.cpp",
    },
}

REQUIRED_TOKENS = {
    "shader_recompiler/backend/spirv/emit_spirv_quad_rect.cpp": (
        "supported_spirv",
        "std::clamp(supported_spirv",
    ),
    "video_core/renderer_vulkan/vk_capability_profile.h": (
        "SelectPromotedRoute",
        "SpirvVersionForVulkan",
        "DriverQuirksForFamily",
        "IsExactProprietaryAdreno750",
        "MobileDriverFamilyFromVendorId",
        "ResolveMobileGpuGeneration",
        "ResolveAggregateMemoryPressurePolicy",
        "ResolveGraphicsLookupPolicy",
        "GpuCompatibilityRoute",
        "RuntimeCompatible",
    ),
    "video_core/renderer_vulkan/vk_instance.cpp": (
        "capability_profile.driver_quirks = DriverQuirksForFamily",
        "ResolveGpuCompatibilityRoute(",
        "GpuCompatibilityRoute::Adreno750QualcommProprietary",
        "ExecutorA750VmaBlock64Requested",
        "LSX4_ALIAS_PROMOTED(vkCmdPipelineBarrier2, vkCmdPipelineBarrier2KHR)",
        "dispatch.vkCmdPipelineBarrier2 = &LegacyCmdPipelineBarrier2",
        "EXECUTOR_VK_CAPABILITY_PROFILE",
    ),
    "video_core/renderer_vulkan/vk_graphics_pipeline.cpp": (
        "GetCapabilityProfile().spirv_version",
        "ensure_auxiliary_spirv",
        "IsTessellationSupported()",
    ),
    "video_core/renderer_vulkan/vk_pipeline_serialization.cpp": (
        "GetCapabilityProfile().pipeline_warmup",
        "warmup_policy.maximum_pipelines",
        "warmup_policy.time_budget_ms",
        "warmup_policy.allow_background_warmup",
    ),
    "video_core/renderer_vulkan/vk_pipeline_common.cpp": (
        "GetCapabilityProfile()",
        "descriptor_binding.allow_exact_write_dedup_ab",
    ),
    "video_core/renderer_vulkan/vk_pipeline_cache.cpp": (
        "GetCapabilityProfile().graphics_lookup",
        "graphics_lookup_policy.structural_signature_reuse_default",
        "graphics_lookup_policy.allow_generation_guarded_reuse_ab",
    ),
    "imgui/renderer/imgui_core.cpp": (
        "GetCapabilityProfile().imgui_upload.allow_persistent_mapping_ab",
    ),
    "imgui/renderer/imgui_impl_vulkan.cpp": (
        "info.allow_persistent_mapping_ab",
        "profileAllowed=%d",
    ),
    "video_core/renderer_vulkan/vk_scheduler.cpp": (
        "instance.UseLegacyRenderPass()",
        "instance.MaxPushDescriptors()",
        "PrepareSubmitFence",
        "UsesTimelineSemaphore",
    ),
    "video_core/renderer_vulkan/vk_rasterizer.cpp": (
        "completion_sidecar.diagnostic_allowed",
        "completion_sidecar.elide_by_default",
        "aggregate_pressure_policy.proprietary_vma_sampling",
        "aggregate_pressure_policy.coordinated_turnip_retirement",
    ),
    "video_core/renderer_vulkan/vk_master_semaphore.cpp": (
        "EXECUTOR_VK_TIMELINE_FALLBACK",
        "RefreshLegacyLocked",
        "PrepareSubmitFence",
    ),
    "video_core/buffer_cache/buffer_cache.cpp": (
        "instance.IsBufferDeviceAddressSupported()",
        "vk::BufferUsageFlagBits::eShaderDeviceAddress",
        "aggregate_pressure_policy.cache_pressure_active",
        "aggregate_pressure_policy.combined_gc",
    ),
    "video_core/texture_cache/texture_cache.cpp": (
        "aggregate_pressure_policy.cache_pressure_active",
        "aggregate_pressure_policy.coordinated_turnip_retirement",
    ),
    "video_core/renderer_vulkan/host_passes/pp_pass.cpp": (
        "uses_dynamic_rendering",
        "uses_push_descriptors",
    ),
    "video_core/renderer_vulkan/host_passes/fsr_pass.cpp": (
        "uses_push_descriptors",
    ),
}

FORBIDDEN_TOKENS = {
    "video_core/renderer_vulkan/vk_capability_profile.h": (
        "ForceLegacySynchronization2",
        "ExactDriverQuirksForDevice",
    ),
    "video_core/renderer_vulkan/vk_instance.cpp": (
        "CapabilityQuirk::ForceLegacySynchronization2",
        "name=force-legacy-synchronization2",
    ),
}


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def has_suffix(path: Path, suffix: str) -> bool:
    return path.as_posix().endswith(suffix)


def source_files(funnel_root: Path, project_root: Path) -> list[Path]:
    roots = [
        funnel_root / "video_core",
        project_root / "src" / "executor" / "ps5_desktop",
    ]
    files: list[Path] = []
    for root in roots:
        if root.is_dir():
            for extension in ("*.cpp", "*.h", "*.cc", "*.hpp"):
                files.extend(root.rglob(extension))
    return sorted(set(files))


def main() -> int:
    parser = argparse.ArgumentParser(description="Enforce the LSX4 Vulkan capability bridge")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--funnel-root", type=Path)
    parser.add_argument("--allow-historical-funnel", action="store_true")
    args = parser.parse_args()

    project_root = args.root.resolve()
    funnel_root = (args.funnel_root or project_root / "funnel-arm" / "src").resolve()
    profile_header = funnel_root / "video_core" / "renderer_vulkan" / "vk_capability_profile.h"
    if not profile_header.is_file():
        if args.allow_historical_funnel:
            print("Vulkan capability guard: historical Funnel tree, bridge checks skipped")
            return 0
        print(f"Vulkan capability guard: missing {profile_header}", file=sys.stderr)
        return 1

    errors: list[str] = []
    for path in source_files(funnel_root, project_root):
        text = path.read_text(encoding="utf-8", errors="replace")

        for match in PROMOTED_RAW_CALL.finditer(text):
            errors.append(
                f"{path}:{line_number(text, match.start())}: raw promoted Vulkan command; "
                "route through the capability bridge"
            )

        if not has_suffix(path, "video_core/renderer_vulkan/vk_instance.cpp"):
            for match in MOBILE_DRIVER_ENUM.finditer(text):
                errors.append(
                    f"{path}:{line_number(text, match.start())}: mobile driver branch outside "
                    "capability profile construction"
                )

        if not has_suffix(path, "video_core/renderer_vulkan/vk_capability_profile.h"):
            for match in MOBILE_VENDOR_LITERAL.finditer(text):
                errors.append(
                    f"{path}:{line_number(text, match.start())}: mobile vendor literal outside "
                    "the centralized profile"
                )

        for pattern, allowed_suffixes in METHOD_ALLOWLIST.items():
            if any(has_suffix(path, suffix) for suffix in allowed_suffixes):
                continue
            for match in pattern.finditer(text):
                errors.append(
                    f"{path}:{line_number(text, match.start())}: promoted method outside its "
                    "route-aware allowlist"
                )

    for suffix, tokens in REQUIRED_TOKENS.items():
        is_funnel_source = suffix.startswith(("video_core/", "shader_recompiler/", "imgui/"))
        path = funnel_root / suffix if is_funnel_source else project_root / suffix
        if not path.is_file():
            errors.append(f"{path}: required bridge file is missing")
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in tokens:
            if token not in text:
                errors.append(f"{path}: required bridge contract is missing: {token}")

    for suffix, tokens in FORBIDDEN_TOKENS.items():
        path = funnel_root / suffix
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in tokens:
            if token in text:
                errors.append(f"{path}: rejected A750 legacy-sync2 contract remains: {token}")

    auxiliary_shader = (
        funnel_root / "shader_recompiler" / "backend" / "spirv" / "emit_spirv_quad_rect.cpp"
    )
    if auxiliary_shader.is_file():
        auxiliary_text = auxiliary_shader.read_text(encoding="utf-8", errors="replace")
        if "Sirit::Module{SPIRV_VERSION_1_5}" in auxiliary_text:
            errors.append(
                f"{auxiliary_shader}: auxiliary shaders must use the capability-profile "
                "SPIR-V target"
            )

    profile_text = profile_header.read_text(encoding="utf-8", errors="replace")
    enum_match = re.search(
        r"enum class CapabilityQuirk[^\{]*\{(?P<body>.*?)\};", profile_text, re.DOTALL
    )
    documentation = project_root / "docs" / "ANDROID_VULKAN_CAPABILITY_BRIDGE.md"
    doc_text = documentation.read_text(encoding="utf-8", errors="replace") if documentation.is_file() else ""
    if enum_match is None:
        errors.append(f"{profile_header}: CapabilityQuirk enum is missing")
    else:
        quirk_names = re.findall(r"^\s*([A-Za-z][A-Za-z0-9_]*)\s*=", enum_match.group("body"), re.MULTILINE)
        for quirk in quirk_names:
            if f"`{quirk}`" not in doc_text:
                errors.append(f"{documentation}: undocumented CapabilityQuirk::{quirk}")

    if errors:
        print("Vulkan capability guard failed:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    print("Vulkan capability guard: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
