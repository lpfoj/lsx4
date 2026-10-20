import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MUTEX = ROOT / "funnel-arm" / "src" / "common" / "shared_first_mutex.h"
LEGACY_RECURSIVE_LOCK = (
    ROOT / "funnel-arm" / "src" / "common" / "recursive_lock.cpp"
)
RASTERIZER_H = (
    ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan" / "vk_rasterizer.h"
)
RASTERIZER_CPP = (
    ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan" / "vk_rasterizer.cpp"
)
GRADLE = ROOT / "android-app" / "app" / "build.gradle"


class RecursiveSharedFirstMutexContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mutex = MUTEX.read_text(encoding="utf-8")
        cls.legacy_recursive_lock = LEGACY_RECURSIVE_LOCK.read_text(encoding="utf-8")
        cls.rasterizer_h = RASTERIZER_H.read_text(encoding="utf-8")
        cls.rasterizer_cpp = RASTERIZER_CPP.read_text(encoding="utf-8")
        cls.gradle = GRADLE.read_text(encoding="utf-8")

    def test_common_recursive_lock_keeps_the_isaac_safe_legacy_registry(self):
        self.assertIn(
            "thread_local std::unordered_map<void*, RecursiveLockState> "
            "g_recursive_locks",
            self.legacy_recursive_lock,
        )
        self.assertNotIn("InlineRecursiveLock", self.legacy_recursive_lock)
        self.assertNotIn("inline_owners", self.legacy_recursive_lock)

    def test_reader_recursion_is_owned_by_the_mutex_not_tls(self):
        section = self.mutex.split("class RecursiveSharedFirstMutex", 1)[1]
        self.assertIn("std::this_thread::get_id()", section)
        self.assertIn("IncrementReaderOwnerLocked", section)
        self.assertIn("DecrementReaderOwnerLocked", section)
        self.assertNotIn("thread_local", section)

    def test_writer_waits_for_outermost_reader_release(self):
        section = self.mutex.split("class RecursiveSharedFirstMutex", 1)[1]
        self.assertIn("!writer_active && readers == 0", section)
        self.assertIn("if (--readers == 0)", section)
        self.assertIn("if (--state.recursion != 0)", section)

    def test_reader_ownership_has_exact_overflow_fallback(self):
        section = self.mutex.split("class RecursiveSharedFirstMutex", 1)[1]
        self.assertIn("InlineReaderCapacity = 16", section)
        self.assertIn("overflow_owners.try_emplace", section)
        self.assertIn("overflow_owners.erase(it)", section)

    def test_only_mapped_ranges_uses_the_recursive_mutex(self):
        self.assertIn("RecursiveSharedFirstMutex mapped_ranges_mutex", self.rasterizer_h)
        combined = self.rasterizer_h + self.rasterizer_cpp
        self.assertEqual(combined.count("std::shared_lock lock{mapped_ranges_mutex}"), 4)
        self.assertNotIn("RecursiveSharedLock lock{mapped_ranges_mutex}", combined)

    def test_apk_freshness_gate_tracks_the_complete_mutex_change_surface(self):
        prepare = self.gradle.split(
            'def prepareRuntimeJniLibs = tasks.register("prepareRuntimeJniLibs")', 1
        )[1].split("def registerFexPackagingVerifier", 1)[0]
        for source in (
            'rootProject.file("../funnel-arm/src/common/recursive_lock.cpp")',
            'rootProject.file("../funnel-arm/src/common/shared_first_mutex.h")',
            'rootProject.file("../funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp")',
            'rootProject.file("../funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.h")',
        ):
            self.assertIn(source, prepare)
        self.assertIn(
            "ps4Runtime.lastModified() < newestPs4Source.lastModified()", prepare
        )


if __name__ == "__main__":
    unittest.main()
