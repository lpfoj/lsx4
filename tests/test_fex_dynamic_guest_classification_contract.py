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
LINKER = (ROOT / "funnel-arm" / "src" / "core" / "linker.cpp").read_text(
    encoding="utf-8"
)


def section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class FexDynamicGuestClassificationContractTests(unittest.TestCase):
    def test_module_pc_lookup_uses_signal_safe_append_only_snapshot(self):
        lookup = section(
            LINKER,
            "Module* Linker::FindByAddress(VAddr address)",
            "void Linker::Relocate(Module* module)",
        )
        self.assertIn(
            "m_module_address_snapshot.load(std::memory_order_acquire)", lookup
        )
        self.assertIn("for (Module* const module : snapshot->modules)", lookup)
        self.assertNotIn("scoped_lock", lookup)

        publish = section(
            LINKER,
            "void Linker::PublishModuleAddressSnapshotLocked()",
            "Module* Linker::FindByAddress(VAddr address)",
        )
        self.assertIn("snapshot->modules.reserve(m_modules.size())", publish)
        self.assertIn(
            "m_module_address_snapshot.store(published, std::memory_order_release)",
            publish,
        )

    def test_interval_query_is_locked_live_and_executable_only(self):
        query = section(
            MEMORY,
            "executor_lsx4_android_is_live_dynamic_guest_executable_address(",
            "static bool ExecutorHostMmapContains",
        )
        self.assertIn("std::scoped_lock lock{g_host_mmap_mutex}", query)
        self.assertIn("g_host_mmap_ranges.upper_bound(address)", query)
        self.assertIn("ExecutorRangeEnd", query)
        self.assertIn("address >= end", query)
        self.assertIn("ExecutorRangeExecutable(range.protection)", query)
        self.assertIn("range.executable_cookie != 0", query)
        self.assertNotIn("ExecutorRangeWritable(range.protection)", query)

    def test_query_observes_only_committed_interval_generations(self):
        protect = section(
            MEMORY,
            "static s32 ExecutorHostMprotect(const VAddr",
            "static void* ExecutorHostMmap(const VAddr",
        )
        self.assertIn("std::scoped_lock lock{g_host_mmap_mutex}", protect)
        self.assertLess(
            protect.index("ExecutorPublishExecutableRanges(next_spans"),
            protect.index("ExecutorEraseHostRangesLocked", protect.index("ExecutorPublishExecutableRanges(next_spans")),
        )

        unmap = section(
            MEMORY,
            "static s32 ExecutorHostMunmap(const VAddr",
            "#endif\n\nvoid* PS4_SYSV_ABI posix_mmap",
        )
        self.assertIn("std::scoped_lock lock{g_host_mmap_mutex}", unmap)
        self.assertLess(unmap.index("::munmap"), unmap.index("ExecutorEraseHostRangesLocked"))

    def test_canonical_classifier_accepts_module_rx_or_live_dynamic_exec(self):
        classify = section(
            NATIVE,
            "bool IsGuestExecutableAddress(const std::uint64_t address,",
            "struct X86InstructionDecode",
        )
        self.assertIn("FindGuestModuleByAddress(address)", classify)
        self.assertIn("ModuleContainsExecutableRange(*module, address, 1)", classify)
        self.assertIn("RuntimeGuestJitActive() || RuntimeGuestFexActive()", classify)
        self.assertIn(
            "executor_lsx4_android_is_live_dynamic_guest_executable_address(address) == 1",
            classify,
        )
        self.assertIn("*module_out = nullptr", classify)

    def test_thread_route_and_runner_share_canonical_classifier(self):
        route = section(
            NATIVE,
            "executor_lsx4_android_should_suppress_guest_thread(",
            "executor_lsx4_android_note_suppressed_guest_thread(",
        )
        self.assertIn("RuntimeGuestFexActive()", route)
        self.assertIn("IsGuestExecutableAddress(start_address)", route)
        self.assertNotIn("FindByAddress", route)

        runner = section(
            NATIVE,
            "executor_lsx4_android_run_guest_thread(",
            "int RunFexGuestModuleLifecycle",
        )
        self.assertIn("IsGuestExecutableAddress(start_address, &module)", runner)
        self.assertIn("module != nullptr &&", runner)
        self.assertIn("!BuildMappedEntryRequestFromModule(*module", runner)
        self.assertIn("module == nullptr && !RuntimeGuestJitActive()", runner)
        self.assertIn(".guest_function = start_address", runner)
        self.assertIn('"<dynamic-executable>"', runner)

    def test_once_and_callback_gates_use_live_executable_membership(self):
        once = section(
            NATIVE,
            "executor_lsx4_android_run_guest_once(",
            "executor_lsx4_android_run_guest_callback(",
        )
        self.assertIn("IsGuestExecutableAddress(init_address, &module)", once)
        self.assertIn("RunFexGuestCallbackN(init_address", once)
        self.assertIn("if (module == nullptr)", once)

        callback = section(
            NATIVE,
            "executor_lsx4_android_run_guest_callback(void* callback",
            "executor_lsx4_android_run_guest_callback6(",
        )
        self.assertIn("IsGuestExecutableAddress(callback_address, &module)", callback)
        self.assertIn("RunFexGuestCallbackN(", callback)
        self.assertIn("if (module == nullptr)", callback)

        callback6 = section(
            NATIVE,
            "executor_lsx4_android_run_guest_callback6(",
            "executor_lsx4_android_run_guest_callback4(",
        )
        self.assertIn("IsGuestExecutableAddress(callback_address, &module)", callback6)
        self.assertIn("RunFexGuestCallbackN(", callback6)

    def test_tls_cleanup_and_regular_comparators_share_the_same_gate(self):
        guest_address = section(
            NATIVE,
            "extern \"C\" bool executor_lsx4_android_is_guest_address",
            "static std::string InlineQwordAsciiPreview",
        )
        self.assertIn("IsGuestExecutableAddress", guest_address)

        signal = section(
            NATIVE,
            "executor_lsx4_android_run_guest_signal_handler(",
            "executor_lsx4_android_run_guest_comparator(",
        )
        self.assertIn("RunFexGuestCallback3", signal)
        self.assertNotIn("IsGuestExecutableAddress(handler)", signal)
        self.assertIn("host signal context", signal)

        comparator = section(
            NATIVE,
            "executor_lsx4_android_run_guest_comparator(",
            "executor_lsx4_android_get_current_guest_regs(",
        )
        self.assertIn("IsGuestExecutableAddress(func)", comparator)
        self.assertIn("RunFexGuestCallback3", comparator)


if __name__ == "__main__":
    unittest.main()
