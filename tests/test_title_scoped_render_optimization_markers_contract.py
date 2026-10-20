from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (
    ROOT / "funnel-arm/src/common/title_scoped_optimization_marker.h"
).read_text(encoding="utf-8")
PIPELINE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
PAGE_MANAGER = (
    ROOT / "funnel-arm/src/video_core/page_manager.cpp"
).read_text(encoding="utf-8")
LIVERPOOL = (
    ROOT / "funnel-arm/src/video_core/amdgpu/liverpool.cpp"
).read_text(encoding="utf-8")
PORTABLE_SRT = (
    ROOT
    / "funnel-arm/src/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp"
).read_text(encoding="utf-8")


class TitleScopedRenderOptimizationMarkersContract(unittest.TestCase):
    def test_marker_path_is_bound_to_validated_current_title(self):
        self.assertIn("MemoryPatcher::g_game_serial", HEADER)
        self.assertIn("title-markers/%.*s/%.*s", HEADER)
        self.assertIn(
            "const std::string_view title, const std::string_view marker",
            HEADER,
        )
        self.assertIn("safe_component(title)", HEADER)
        self.assertIn("safe_component(marker)", HEADER)
        self.assertNotIn("run-enable-global", HEADER)

    def test_render_opt_ins_do_not_read_legacy_global_marker_files(self):
        marker_sources = {
            PIPELINE: (
                "run-enable-program-hot-flattened-lease",
                "run-enable-program-hot-occupancy-mask",
                "run-enable-program-hot-grouped-tls",
                "run-enable-program-flattened-scratch-recycle",
                "run-enable-borrowed-fetch-shader-result",
                "run-enable-program-hot-call-scope-tls-hoist",
                "run-enable-program-hot-legacy-tls-cold-split",
                "run-enable-shader-signature-word-prefilter",
                "run-enable-stable-srt-epoch-hit",
                "run-enable-program-hot-exact-multiversion",
                "run-enable-program-hot-mru-probe",
            ),
            PAGE_MANAGER: (
                "run-enable-direct-region-modified-range",
                "run-enable-single-page-descriptor-snapshot-fast-path",
                "run-enable-descriptor-snapshot-local-chunk-hoist",
            ),
            LIVERPOOL: ("run-enable-gpu-thread-token",),
            PORTABLE_SRT: (
                "run-enable-portable-srt-precomputed-layout",
                "run-enable-portable-srt-scratch-default-init",
            ),
        }
        for source, markers in marker_sources.items():
            for marker in markers:
                with self.subTest(marker=marker):
                    marker_at = source.index(f'"{marker}"')
                    route = source[max(0, marker_at - 180) : marker_at + 180]
                    self.assertIn(
                        "Common::TitleScopedOptimizationMarkerEnabled(", route
                    )

    def test_global_preserve_markers_remain_available_for_diagnostic_ab(self):
        for source in (PIPELINE, PAGE_MANAGER, LIVERPOOL, PORTABLE_SRT):
            with self.subTest(source_length=len(source)):
                self.assertIn(
                    "run-preserve-bloodborne-snapdragon-render-bundle", source
                )


if __name__ == "__main__":
    unittest.main()
