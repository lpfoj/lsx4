from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "funnel-arm" / "src" / "video_core"
TEXTURE = (VIDEO / "texture_cache" / "texture_cache.cpp").read_text(encoding="utf-8")
BUFFER = (VIDEO / "buffer_cache" / "buffer_cache.cpp").read_text(encoding="utf-8")
PROFILE = (VIDEO / "renderer_vulkan" / "vk_capability_profile.h").read_text(
    encoding="utf-8"
)
INSTANCE = (VIDEO / "renderer_vulkan" / "vk_instance.cpp").read_text(
    encoding="utf-8"
)
RASTERIZER = (VIDEO / "renderer_vulkan" / "vk_rasterizer.cpp").read_text(
    encoding="utf-8"
)
MOBILE_H = (VIDEO / "mobile_gpu_optimization.h").read_text(encoding="utf-8")
MOBILE_CPP = (VIDEO / "mobile_gpu_optimization.cpp").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
MEASURE = (ROOT / "scripts" / "measure-bloodborne-gameplay-vivo.ps1").read_text(
    encoding="utf-8"
)


class A750NoVictimGcFastNoopContract(unittest.TestCase):
    def test_candidate_is_default_off_and_resolved_once_for_exact_lineage(self) -> None:
        self.assertIn("run-sm8650-a750-d9-protected-gc-fast-noop-ab", INSTANCE)
        assignment = INSTANCE.split(
            "capability_profile.optimization_a750_protected_gc_fast_noop_ab =", 1
        )[1].split(";", 1)[0]
        self.assertIn("ExecutorA750ProtectedGcFastNoopRequested()", assignment)
        self.assertIn("exact_mesa_turnip_a750_d9_lineage", assignment)
        self.assertIn(
            "static_assert(!CapabilityProfile{}.optimization_a750_protected_gc_fast_noop_ab)",
            PROFILE,
        )
        for source in (TEXTURE, BUFFER):
            self.assertNotIn("run-sm8650-a750-d9-protected-gc-fast-noop-ab", source)

    def test_texture_skip_requires_existing_hard_protection_and_no_emergency(self) -> None:
        method = TEXTURE.split("void TextureCache::GarbageCollectImages", 1)[1].split(
            "void TextureCache::GarbageCollectSamplers", 1
        )[0]
        fast = method.split("const bool protected_gc_candidate", 1)[1].split(
            "bool pressured", 1
        )[0]
        self.assertIn("protect_all_image_identities", fast)
        self.assertIn("!force_pressure_scan", fast)
        self.assertIn("!device_emergency", fast)
        self.assertIn("total_used_memory < critical_gc_memory", fast)
        self.assertIn("RecordMobileGpuTextureResidencyGcNoVictimSkip", fast)
        self.assertIn("protected_gc_audit_scan", fast)
        self.assertIn("return;", fast)
        self.assertIn(
            "const bool object_gc_protected = protect_all_image_identities", method
        )
        self.assertIn("!protected_gc_audit_scan || deleted_images == 0", method)

    def test_buffer_skip_requires_bda_identity_and_keeps_transfer_flush(self) -> None:
        method = BUFFER.split("void BufferCache::RunGarbageCollector", 1)[1].split(
            "void BufferCache::TouchBuffer", 1
        )[0]
        self.assertLess(
            method.index("scheduler.FlushPendingBufferTransfers()"),
            method.index("const bool protected_gc_fast_noop"),
        )
        fast = method.split("const bool protected_gc_candidate", 1)[1].split(
            "const bool aggressive", 1
        )[0]
        self.assertIn("protect_a750_bda_identity", fast)
        self.assertIn("!force_pressure_scan", fast)
        self.assertIn("!aggregate_emergency", fast)
        self.assertIn("resident_buffer_bytes < critical_gc_memory", fast)
        self.assertIn("RecordMobileGpuBufferResidencyGcNoVictimSkip", fast)
        self.assertIn("protected_gc_audit_scan", fast)
        self.assertIn("!protected_gc_audit_scan || clean_evicted == 0", method)

    def test_rasterizer_forces_legacy_scan_for_every_pressure_epoch(self) -> None:
        self.assertIn(
            "texture_cache.RunGarbageCollector(cache_pressure_sample,\n"
            "                                               exact_a750_pressure_epoch)",
            RASTERIZER,
        )
        self.assertIn(
            "buffer_cache.RunGarbageCollector(cache_pressure_sample,\n"
            "                                              exact_a750_pressure_epoch)",
            RASTERIZER,
        )

    def test_gc_tick_and_emergency_collectors_are_not_removed(self) -> None:
        self.assertIn("SCOPE_EXIT {\n        ++gc_tick;", BUFFER)
        self.assertIn("SCOPE_EXIT {\n        ++gc_tick;", TEXTURE)
        self.assertIn("device_emergency || total_used_memory >= critical_gc_memory", TEXTURE)
        self.assertIn("aggregate_emergency || resident_buffer_bytes >= critical_gc_memory", BUFFER)
        texture_run = TEXTURE.split("void TextureCache::RunGarbageCollector", 1)[1]
        self.assertLess(
            texture_run.index("GarbageCollectImages"),
            texture_run.index("GarbageCollectSamplers"),
        )

    def test_skip_counter_is_exported_and_measured(self) -> None:
        for source in (MOBILE_H, MOBILE_CPP):
            self.assertIn("residency_gc_no_victim_skips", source)
        for source in (RUNTIME, MEASURE):
            self.assertIn("residencyGcNoVictimSkips", source)
        for name in (
            "textureGcPasses",
            "textureGcNoVictimSkips",
            "textureGcForcedPasses",
            "textureGcAuditPasses",
            "bufferGcPasses",
            "bufferGcNoVictimSkips",
            "bufferGcForcedPasses",
            "bufferGcAuditPasses",
        ):
            self.assertIn(name, RUNTIME)
            self.assertIn(name, MEASURE)
        self.assertIn("RecordMobileGpuTextureResidencyGcNoVictimSkip", MOBILE_H)
        self.assertIn("RecordMobileGpuBufferResidencyGcNoVictimSkip", MOBILE_H)
        self.assertIn("noVictimSkipsPerPresent", MEASURE)
        self.assertIn("attemptsDelta", MEASURE)


if __name__ == "__main__":
    unittest.main()
