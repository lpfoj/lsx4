import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = (ROOT / "android-app/app/src/main/java/app/lsx4/android/MainActivity.java").read_text(
    encoding="utf-8"
)
JAVA_BRIDGE = (ROOT / "android-app/app/src/main/java/app/lsx4/android/RuntimeBridge.java").read_text(
    encoding="utf-8"
)
JNI_BRIDGE = (ROOT / "android-app/app/src/main/cpp/runtime_bridge.cpp").read_text(
    encoding="utf-8"
)
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
MOBILE_H = (ROOT / "funnel-arm/src/video_core/mobile_gpu_optimization.h").read_text(
    encoding="utf-8"
)
RASTERIZER = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text(
    encoding="utf-8"
)
INSTANCE = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text(
    encoding="utf-8"
)
SCHEDULER = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp").read_text(
    encoding="utf-8"
)


class A750AndroidMemoryPressureContract(unittest.TestCase):
    def test_android_callback_reaches_optional_native_bridge(self) -> None:
        self.assertIn("public void onTrimMemory(int level)", MAIN)
        self.assertIn("public void onLowMemory()", MAIN)
        self.assertIn("reportAndroidMemoryPressure(level)", MAIN)
        self.assertIn("native int reportAndroidMemoryPressure(int level)", JAVA_BRIDGE)
        self.assertIn("g_report_android_memory_pressure", JNI_BRIDGE)
        self.assertIn("executor_lsx4_runtime_report_android_memory_pressure", RUNTIME)
        self.assertIn("ReportAndroidMemoryPressure(bounded)", RUNTIME)
        self.assertIn('R"("androidMemoryPressureCallbacks":)"', RUNTIME)
        self.assertIn('R"("androidMemoryPressureConsumed":)"', RUNTIME)

    def test_renderer_consumes_event_only_inside_exact_a750_pressure_routes(self) -> None:
        self.assertIn("ReportAndroidMemoryPressure(u32 level)", MOBILE_H)
        self.assertIn("ConsumeAndroidMemoryPressure()", MOBILE_H)
        self.assertIn(
            "proprietary_a750_vma_pressure || coordinated_turnip_a750_pressure",
            RASTERIZER,
        )
        self.assertIn("monitored_a750_memory_pressure", RASTERIZER)
        self.assertIn("? VideoCore::ConsumeAndroidMemoryPressure()", RASTERIZER)
        self.assertIn("android_memory_pressure_epoch", RASTERIZER)
        self.assertIn("action=nonblocking-retirement sampleMiB=%llu", RASTERIZER)
        self.assertIn("promoted=%u waited=0", RASTERIZER)

    def test_pressure_path_never_adds_a_gpu_wait(self) -> None:
        pressure_begin = RASTERIZER.index("Android's callback is an event")



        pressure_end = RASTERIZER.index(
            "if (ExecutorUsesExactA750TurnipLoadingTransitionDrain", pressure_begin
        )
        pressure_path = RASTERIZER[pressure_begin:pressure_end]
        self.assertNotIn("scheduler.Finish()", pressure_path)
        self.assertNotIn("waitIdle", pressure_path)
        self.assertNotIn("Wait", pressure_path)
        self.assertIn("buffer_cache.RunGarbageCollector", pressure_path)

    def test_recording_pressure_hot_path_uses_narrow_sample_gate(self) -> None:
        pressure = SCHEDULER.split(
            "void Scheduler::MaybePublishExactA750D9SampledRecordingPressureBoundary", 1
        )[1].split("void Scheduler::Finish", 1)[0]
        self.assertIn("AndroidProcessMemorySampleSerial()", pressure)
        self.assertIn("GetAndroidProcessMemoryStats()", pressure)
        self.assertNotIn("GetMobileGpuOptimizationStats()", pressure)
        self.assertIn("std::memory_order_release", (
            ROOT / "funnel-arm/src/video_core/mobile_gpu_optimization.cpp"
        ).read_text(encoding="utf-8"))

    def test_api34_plus_memory_info_monitor_is_off_render_thread_and_exact_route_owned(self) -> None:
        self.assertIn("Executors.newSingleThreadScheduledExecutor", MAIN)
        self.assertIn('new Thread(runnable, "LSX4MemoryWatch")', MAIN)
        self.assertIn("ANDROID_MEMORY_MONITOR_INTERVAL_MS = 2_000L", MAIN)
        self.assertIn("ANDROID_PROCESS_MEMORY_MONITOR_INTERVAL_MS = 10_000L", MAIN)
        self.assertIn("ANDROID_MEMORY_PRESSURE_REPUBLISH_INTERVAL_MS = 10_000L", MAIN)
        self.assertIn("ActivityManager.MemoryInfo", MAIN)
        self.assertIn("manager.getMemoryInfo(info)", MAIN)
        self.assertIn("RuntimeBridge.androidMemoryPressureMonitorRequested() != 1", MAIN)
        self.assertIn('!"adreno-750".equals(runtimeHardwareProfile)', MAIN)
        self.assertIn('"mesa-turnip".equals(runtimeDriverFamily)', MAIN)
        self.assertIn('"qualcomm-proprietary".equals(runtimeDriverFamily)', MAIN)
        self.assertIn("1L * 1024L * 1024L * 1024L", MAIN)
        self.assertIn("androidMemoryPressureLastPublishAtMs", MAIN)
        self.assertIn("reportAndroidMemoryInfo(", MAIN)
        self.assertIn("Debug.getMemoryInfo(processInfo)", MAIN)
        self.assertIn("boolean processSampleFresh", MAIN)
        self.assertIn("if (processSampleFresh)", MAIN)
        self.assertIn("long processFootprintBytes = totalPssBytes", MAIN)
        self.assertNotIn(
            "saturatedAdd(totalPssBytes, totalSwapBytes)", MAIN
        )
        self.assertIn('memoryStatKb(processInfo, "summary.graphics")', MAIN)
        self.assertIn("ANDROID_PROCESS_MEMORY_WARNING_BYTES", MAIN)
        self.assertIn("ANDROID_GRAPHICS_MEMORY_WARNING_BYTES", MAIN)
        self.assertIn("reportAndroidProcessMemoryInfo(", MAIN)
        self.assertIn("native int reportAndroidProcessMemoryInfo", JAVA_BRIDGE)
        self.assertIn("g_report_android_process_memory_info", JNI_BRIDGE)
        self.assertIn(
            "executor_lsx4_runtime_report_android_process_memory_info", RUNTIME
        )
        self.assertIn("ReportAndroidProcessMemoryInfo(", MOBILE_H)

        self.assertIn("SetAndroidMemoryPressureMonitorRequested(", INSTANCE)
        self.assertIn(
            "capability_profile.aggregate_memory_pressure.cache_pressure_active",
            INSTANCE,
        )
        self.assertIn("IsAndroidMemoryPressureMonitorRequested()", RUNTIME)
        self.assertIn('R"("androidMemoryInfoSamples":)"', RUNTIME)
        self.assertNotIn("ActivityManager", RASTERIZER)


if __name__ == "__main__":
    unittest.main()
