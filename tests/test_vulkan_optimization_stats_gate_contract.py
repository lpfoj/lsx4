import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/vulkan_driver_optimization.cpp"
).read_text(encoding="utf-8")
HEADER = (
    ROOT / "funnel-arm/src/video_core/vulkan_driver_optimization.h"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def function_body(source: str, name: str) -> str:
    match = re.search(rf"void {name}\([^)]*\) noexcept \{{", source)
    if match is None:
        raise AssertionError(f"missing definition for {name}")

    opening = source.index("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated definition for {name}")


class VulkanOptimizationStatsGateContractTests(unittest.TestCase):
    def test_gate_is_cached_explicit_marker_only_and_default_off(self):
        marker_support = between(
            SOURCE,
            "#ifdef __ANDROID__\nbool BisectionMarkerAbsent",
            "struct LocalStats",
        )
        gate = between(
            SOURCE,
            "bool VulkanDriverOptimizationStatsEnabled() noexcept",
            "struct LocalStats",
        )
        self.assertIn("#ifdef __ANDROID__", gate)
        self.assertIn("static const bool enabled", gate)
        self.assertIn(
            '!BisectionMarkerAbsent("run-diagnostic-vulkan-optimization-stats")',
            gate,
        )
        self.assertNotIn("getenv", gate)
        self.assertNotIn("IsManagedVulkanDriverOptimization", gate)
        self.assertIn("return true;", gate)
        self.assertIn("::access", marker_support)
        self.assertNotIn("getenv", marker_support)
        self.assertEqual(
            SOURCE.count("run-diagnostic-vulkan-optimization-stats"), 1
        )

    def test_every_record_api_returns_before_tls_mutation_or_flush(self):
        declarations = re.findall(r"void (RecordVulkan\w+)\([^;]*\) noexcept;", HEADER)
        self.assertTrue(declarations)
        self.assertEqual(len(declarations), len(set(declarations)))

        definitions = re.findall(r"void (RecordVulkan\w+)\([^)]*\) noexcept \{", SOURCE)
        self.assertEqual(set(declarations), set(definitions))

        guard = "if (!VulkanDriverOptimizationStatsEnabled())"
        for name in declarations:
            with self.subTest(api=name):
                body = function_body(SOURCE, name)
                self.assertTrue(body.lstrip().startswith(guard))
                guard_index = body.index(guard)
                return_index = body.index("return;", guard_index)
                tls_index = body.index("ThreadStats()")
                mutation_index = min(
                    index
                    for token in ("++stats.", "stats.")
                    if (index := body.find(token, tls_index)) >= 0
                )
                flush_index = body.index("MaybeFlushThreadStats(stats)")
                self.assertLess(guard_index, return_index)
                self.assertLess(return_index, tls_index)
                self.assertLess(tls_index, mutation_index)
                self.assertLess(mutation_index, flush_index)

    def test_gate_does_not_change_optimization_decisions_or_stats_readback(self):
        decision_section = between(
            SOURCE,
            "void SetManagedVulkanDriverOptimization",
            "void RecordVulkanPipelineBind",
        )
        self.assertNotIn("VulkanDriverOptimizationStatsEnabled", decision_section)

        readback_section = between(
            SOURCE,
            "VulkanDriverOptimizationStats GetVulkanDriverOptimizationStats",
            "void ResetVulkanDriverOptimizationStats",
        )
        reset_section = SOURCE.split(
            "void ResetVulkanDriverOptimizationStats", 1
        )[1]
        self.assertNotIn("VulkanDriverOptimizationStatsEnabled", readback_section)
        self.assertNotIn("VulkanDriverOptimizationStatsEnabled", reset_section)


if __name__ == "__main__":
    unittest.main()
