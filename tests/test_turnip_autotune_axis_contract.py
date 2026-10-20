from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRIVER_AXIS = (
    ROOT
    / "funnel-arm/src/video_core/renderer_vulkan/vk_driver_axis.cpp"
).read_text(encoding="utf-8")


def function_body(name: str) -> str:
    match = re.search(
        rf"(?:bool|void|PFN_vkGetInstanceProcAddr)\s+{name}\([^{{]*\)\s*\{{",
        DRIVER_AXIS,
    )
    if match is None:
        raise AssertionError(f"missing function {name}")

    depth = 1
    cursor = match.end()
    while cursor < len(DRIVER_AXIS) and depth:
        if DRIVER_AXIS[cursor] == "{":
            depth += 1
        elif DRIVER_AXIS[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise AssertionError(f"unterminated function {name}")
    return DRIVER_AXIS[match.start() : cursor]


class TurnipAutotuneAxisContractTests(unittest.TestCase):
    def test_marker_is_default_off_and_strictly_bounded(self) -> None:
        reader = function_body("ReadTurnipAutotuneMarkerLocked")
        self.assertIn('TurnipAutotuneMarkerName = "turnip-autotune-algo"', DRIVER_AXIS)
        self.assertIn("TurnipAutotuneMarkerMaxBytes = 64", DRIVER_AXIS)
        self.assertIn("marker_size == 0", reader)
        self.assertIn("marker_size > TurnipAutotuneMarkerMaxBytes", reader)
        self.assertIn("if (path.empty() || !IsRegularFile(path))", reader)
        self.assertIn("return false;", reader)

    def test_only_reviewed_algorithms_are_accepted(self) -> None:
        declaration = DRIVER_AXIS.split("TurnipAutotuneAlgorithms{", 1)[1].split(
            "};", 1
        )[0]
        self.assertEqual(
            re.findall(r'std::string_view\{"([a-z_]+)"\}', declaration),
            ["bandwidth", "prefer_sysmem", "profiled_imm"],
        )
        reader = function_body("ReadTurnipAutotuneMarkerLocked")
        self.assertIn("IsTurnipAutotuneAlgorithm(contents)", reader)
        self.assertIn('contents.ends_with("\\r\\n")', reader)
        self.assertIn("contents.ends_with('\\n')", reader)

    def test_environment_is_applied_only_after_bundle_validation_and_before_dlopen(self) -> None:
        selection = function_body("SelectGetInstanceProcAddr")
        order = [
            selection.index('"loader-bundle-incomplete"'),
            selection.index("ApplyTurnipAutotuneMarkerLocked();"),
            selection.index("ApplyTurnipDebugOptionsLocked();"),
            selection.index("dlopen(LibraryPath(AdrenoToolsName)"),
            selection.index("PreflightLocked(custom_gipa, disposable_probe)"),
        ]
        self.assertEqual(order, sorted(order))
        system_return = selection.index("if (!state.requested_turnip)")
        marker_apply = selection.index("ApplyTurnipAutotuneMarkerLocked();")
        self.assertLess(system_return, marker_apply)

    def test_every_fallback_restores_the_previous_environment(self) -> None:
        fallback = function_body("FallbackLocked")
        restore = function_body("RestoreTurnipAutotuneEnvironmentLocked")
        self.assertLess(
            fallback.index("RestoreTurnipAutotuneEnvironmentLocked();"),
            fallback.index('state.effective = "system"'),
        )
        self.assertIn("turnip_autotune_previous_environment_present", restore)
        self.assertIn("setenv(TurnipAutotuneEnvironmentName.data()", restore)
        self.assertIn("unsetenv(TurnipAutotuneEnvironmentName.data())", restore)
        self.assertIn('state.turnip_autotune_source = "restore-failed"', restore)

    def test_status_reports_effective_marker_state(self) -> None:
        status = function_body("RefreshStatusLocked")
        self.assertIn('{"turnipAutotuneAlgo", state.turnip_autotune_algo}', status)
        self.assertIn('{"turnipAutotuneSource", state.turnip_autotune_source}', status)
        self.assertIn(
            '{"turnipAutotuneMarkerApplied", state.turnip_autotune_marker_applied}',
            status,
        )


if __name__ == "__main__":
    unittest.main()
