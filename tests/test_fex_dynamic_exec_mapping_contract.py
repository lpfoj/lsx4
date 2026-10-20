import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MEMORY = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "memory.cpp"
).read_text(encoding="utf-8")
NATIVE = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


def section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class FexDynamicExecutableMappingContractTests(unittest.TestCase):
    def test_uses_v3_lifecycle_bridges_and_rejects_legacy_append_only_registrar(self):
        self.assertIn(
            "executor_lsx4_android_fex_publish_dynamic_executable_range",
            MEMORY,
        )
        self.assertIn(
            "executor_lsx4_android_fex_invalidate_dynamic_executable_range",
            MEMORY,
        )
        self.assertNotIn(
            'dlsym(RTLD_DEFAULT, "executor_fexcore_register_guest_executable_range")',
            MEMORY,
        )
        self.assertIn("executor_lsx4_android_fault_signal_owner() == 2", MEMORY)
        self.assertIn("executor_lsx4_android_fex_signal_api_active() != 0", MEMORY)
        self.assertIn("result=missing_bridge", MEMORY)

    def test_dynamic_cookie_is_fresh_process_monotonic_and_never_address_only(self):
        cookie = section(
            MEMORY,
            "static std::uint64_t ExecutorAllocateDynamicRangeCookieLocked()",
            "static bool ExecutorFexDynamicRangeActive()",
        )
        self.assertIn("ExecutorDynamicRangeCookieDomain", cookie)
        self.assertIn("g_dynamic_range_cookie_serial++", cookie)
        self.assertIn("cookie != 0", cookie)
        self.assertNotIn("span.address", cookie)
        self.assertNotIn("span.size", cookie)

    def test_partial_protection_change_removes_old_before_os_and_publishes_split_after(self):
        protect = section(
            MEMORY,
            "static s32 ExecutorHostMprotect(const VAddr",
            "static void* ExecutorHostMmap(const VAddr",
        )
        plan = protect.index("ExecutorReplaceHostRangeLocked")
        remove = protect.index("ExecutorRemoveExecutableRanges")
        os_change = protect.index("::mprotect", remove)
        publish = protect.index("ExecutorPublishExecutableRanges(next_spans", os_change)
        commit = protect.index("ExecutorEraseHostRangesLocked", publish)
        self.assertLess(plan, remove)
        self.assertLess(remove, os_change)
        self.assertLess(os_change, publish)
        self.assertLess(publish, commit)
        self.assertIn("ExecutorRestoreHostProtections", protect)
        self.assertIn("mprotect_publish_cleanup", protect)
        self.assertIn("mprotect_publish_rollback", protect)

    def test_unchanged_wx_mprotect_is_an_invalidate_only_barrier(self):
        protect = section(
            MEMORY,
            "static s32 ExecutorHostMprotect(const VAddr",
            "static void* ExecutorHostMmap(const VAddr",
        )
        unchanged = section(
            protect,
            "if (!protection_changes)",
            "auto next_spans",
        )
        self.assertIn("ExecutorRangeExecutable(protection)", unchanged)
        self.assertIn("ExecutorRangeWritable(protection)", unchanged)
        self.assertIn("ExecutorFexInvalidateTranslations", unchanged)
        self.assertNotIn("ExecutorFexRemoveRegistrations", unchanged)

    def test_split_plan_keeps_left_and_right_and_allocates_new_exec_cookies(self):
        split = section(
            MEMORY,
            "static std::vector<ExecutorHostMmapSpan> ExecutorReplaceHostRangeLocked",
            "static void ExecutorEraseHostRangesLocked",
        )
        self.assertIn("old.address < begin", split)
        self.assertIn("begin - old.address", split)
        self.assertIn("intersection_begin", split)
        self.assertIn("intersection_end", split)
        self.assertIn("old_end > end", split)
        self.assertIn("old_end - end", split)

        protect = section(
            MEMORY,
            "static s32 ExecutorHostMprotect(const VAddr",
            "static void* ExecutorHostMmap(const VAddr",
        )
        self.assertLess(
            protect.index("ExecutorReplaceHostRangeLocked"),
            protect.index("ExecutorAssignExecutableCookiesLocked(next_spans)"),
        )

    def test_executable_mmap_fails_closed_if_publication_fails(self):
        mmap = section(
            MEMORY,
            "static void* ExecutorHostMmap(const VAddr",
            "static s32 ExecutorHostMunmap(const VAddr",
        )
        publish = mmap.index("ExecutorPublishExecutableRanges(next_spans")
        cleanup = mmap.index("ExecutorRemovePublishedExecutableRanges", publish)
        unmap = mmap.index("::munmap(host_addr, size)", cleanup)
        failure = mmap.index("return MAP_FAILED", unmap)
        commit = mmap.index("ExecutorInsertHostRangesLocked(next_spans)", failure)
        self.assertLess(publish, cleanup)
        self.assertLess(cleanup, unmap)
        self.assertLess(unmap, failure)
        self.assertLess(failure, commit)

    def test_munmap_retires_old_and_publishes_survivors_before_memory_disappears(self):
        munmap = section(
            MEMORY,
            "static s32 ExecutorHostMunmap(const VAddr",
            "#endif\n\nvoid* PS4_SYSV_ABI posix_mmap",
        )
        remove = munmap.index("ExecutorRemoveExecutableRanges")
        publish = munmap.index("ExecutorPublishExecutableRanges(survivors", remove)
        os_unmap = munmap.index("::munmap", publish)
        commit = munmap.index("ExecutorEraseHostRangesLocked", os_unmap)
        self.assertLess(remove, publish)
        self.assertLess(publish, os_unmap)
        self.assertLess(os_unmap, commit)
        self.assertIn("munmap_os_cleanup", munmap)
        self.assertIn("munmap_os_rollback", munmap)

    def test_remove_uses_invalidate_plus_remove_flags_on_full_old_intervals(self):
        remove = section(
            MEMORY,
            "static bool ExecutorRemoveExecutableRanges",
            "static bool ExecutorPublishExecutableRanges",
        )
        self.assertIn("ExecutorFexInvalidateTranslations", remove)
        self.assertIn("ExecutorFexRemoveRegistrations", remove)
        self.assertIn("span,", remove)

    def test_native_v3_bridge_serializes_and_retries_only_stale_generation(self):
        publish = section(
            NATIVE,
            "executor_lsx4_android_fex_publish_dynamic_executable_range(",
            'extern "C" __attribute__((visibility("default"), used)) int\n'
            "executor_lsx4_android_fex_invalidate_dynamic_executable_range(",
        )
        self.assertIn("g_fex_executable_range_mutation_mutex", publish)
        self.assertIn("FexRangeMutationMaxAttempts", publish)
        self.assertIn("snapshot_executable_ranges_v5", publish)
        self.assertIn("bulk_register_executable_ranges_v5", publish)
        self.assertIn("status != LSX4_FEX_STATUS_INVALID_STATE", publish)
        self.assertIn("range_flags != expected_range_flags", publish)

    def test_native_remove_validates_exact_registered_cookie_geometry(self):
        invalidate = section(
            NATIVE,
            'extern "C" __attribute__((visibility("default"), used)) int\n'
            "executor_lsx4_android_fex_invalidate_dynamic_executable_range(",
            "int RegisterBox64ModuleSegments(void* module_ptr)",
        )
        self.assertIn("SnapshotFexExecutableRanges", invalidate)
        self.assertIn("range.range_cookie == range_cookie", invalidate)
        self.assertIn("range.address == address && range.size == size", invalidate)
        self.assertIn("LSX4_FEX_RANGE_REMOVE_REGISTRATIONS", invalidate)
        self.assertIn("invalidate_executable_ranges_v5", invalidate)
        self.assertIn("status != LSX4_FEX_STATUS_INVALID_STATE", invalidate)


if __name__ == "__main__":
    unittest.main()
