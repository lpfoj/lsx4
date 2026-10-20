from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PLATFORM_H = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_platform.h"
).read_text(encoding="utf-8")
PLATFORM_CPP = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_platform.cpp"
).read_text(encoding="utf-8")
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class VulkanDebugNameGateContractTests(unittest.TestCase):
    def test_object_name_gate_precedes_formatting_and_driver_call(self):
        direct = between(
            PLATFORM_H,
            "void SetObjectName(vk::Device device, const HandleType& handle, "
            "std::string_view debug_name)",
            "template <VulkanHandleType HandleType, typename... Args>",
        )
        self.assertLess(
            direct.index("!ObjectNamesEnabled()"),
            direct.index("vkSetDebugUtilsObjectNameEXT"),
        )
        self.assertLess(
            direct.index("!ObjectNamesEnabled()"),
            direct.index("device.setDebugUtilsObjectNameEXT"),
        )

        formatted = between(
            PLATFORM_H,
            "void SetObjectName(vk::Device device, const HandleType& handle, "
            "const char* format",
            "template <StringLiteral msg",
        )
        self.assertLess(
            formatted.index("if (!ObjectNamesEnabled())"),
            formatted.index("fmt::vformat"),
        )

    def test_android_gate_is_positive_opt_in_and_desktop_stays_enabled(self):
        gate = between(
            PLATFORM_CPP,
            "bool ObjectNamesEnabled() noexcept",
            "static const char* const VALIDATION_LAYER_NAME",
        )
        self.assertIn("#if defined(ANDROID)", gate)
        self.assertIn("run-diagnostic-vulkan-object-names", gate)
        self.assertIn("Config::vkValidationEnabled()", gate)
        self.assertIn("Config::getVkHostMarkersEnabled()", gate)
        self.assertIn("Config::getVkGuestMarkersEnabled()", gate)
        self.assertIn("#else", gate)
        self.assertIn("return true;", gate)

    def test_hot_marker_strings_are_not_formatted_when_host_markers_are_off(self):
        cases = (
            (
                "void Rasterizer::EliminateFastClear()",
                "void Rasterizer::Draw(",
                "EliminateFastClear:MRT=",
                "image.Clear(clear_value, desc.view_info.range);",
            ),
            (
                "void Rasterizer::Resolve()",
                "void Rasterizer::DepthStencilCopy(",
                "Resolve:MRT0=",
                "mrt1_image.Resolve(",
            ),
            (
                "void Rasterizer::DepthStencilCopy(",
                "void Rasterizer::FillBuffer(",
                "DepthStencilCopy:DR=",
                "scheduler.CommandBuffer().copyImage(",
            ),
        )
        for start, end, label, command in cases:
            with self.subTest(label=label):
                body = between(RASTERIZER, start, end)
                gate = body.index("if (Config::getVkHostMarkersEnabled())")
                marker_format = body.index(label)
                command_call = body.index(command)
                self.assertLess(gate, marker_format)
                self.assertLess(marker_format, command_call)
                self.assertIn("}\n", body[marker_format:command_call])


if __name__ == "__main__":
    unittest.main()
