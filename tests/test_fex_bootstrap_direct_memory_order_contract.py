import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NATIVE_RUNTIME = (
    ROOT / "src" / "executor" / "native_runtime_api.cpp"
).read_text(encoding="utf-8")
KERNEL_MEMORY = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "memory.cpp"
).read_text(encoding="utf-8")


def allocate_first_fit(free_ranges, size, alignment):
    for index, (begin, end) in enumerate(free_ranges):
        aligned = (begin + alignment - 1) & -alignment
        allocation_end = aligned + size
        if allocation_end > end:
            continue

        replacement = []
        if begin < aligned:
            replacement.append((begin, aligned))
        if allocation_end < end:
            replacement.append((allocation_end, end))
        free_ranges[index : index + 1] = replacement
        return aligned
    return None


def reserve_exact(free_ranges, begin, size):
    end = begin + size
    for index, (free_begin, free_end) in enumerate(free_ranges):
        if begin < free_begin or end > free_end:
            continue

        replacement = []
        if free_begin < begin:
            replacement.append((free_begin, begin))
        if end < free_end:
            replacement.append((end, free_end))
        free_ranges[index : index + 1] = replacement
        return True
    return False


class FexBootstrapDirectMemoryOrderContractTests(unittest.TestCase):
    def test_high_tail_gnm_preserves_alienation_late_pool(self):
        total_direct_memory = 0x120000000
        libc = (0x70000000, 0x200000)
        gnm = (0x10000, 0x10000)
        gpu_regions = (
            (0x40000000, 0x40000),
            (0x46000000, 0x40000),
            (0x14000000, 0x40000),
        )
        late_pool = (0x15F00000, 0x4000)

        fragmented = [(0, total_direct_memory)]
        self.assertEqual(allocate_first_fit(fragmented, *gnm), 0)
        self.assertEqual(allocate_first_fit(fragmented, *libc), 0x200000)
        for size, alignment in gpu_regions:
            self.assertIsNotNone(
                allocate_first_fit(fragmented, size, alignment)
            )
        self.assertIsNone(allocate_first_fit(fragmented, *late_pool))

        known_ordered = [(0, total_direct_memory)]
        self.assertTrue(
            reserve_exact(known_ordered, libc[0], gnm[0])
        )
        self.assertEqual(allocate_first_fit(known_ordered, *libc), 0)
        for size, alignment in gpu_regions:
            self.assertIsNotNone(
                allocate_first_fit(known_ordered, size, alignment)
            )
        ordered_late_pool = allocate_first_fit(known_ordered, *late_pool)
        self.assertEqual(ordered_late_pool, 0x10A040000)
        self.assertEqual(
            total_direct_memory - (ordered_late_pool + late_pool[0]),
            0xC0000,
        )

        high_tail = total_direct_memory - gnm[0]
        preserved = [(0, total_direct_memory)]
        self.assertTrue(reserve_exact(preserved, high_tail, gnm[0]))
        self.assertEqual(allocate_first_fit(preserved, *libc), 0)
        for size, alignment in gpu_regions:
            self.assertIsNotNone(allocate_first_fit(preserved, size, alignment))
        late_pool_address = allocate_first_fit(preserved, *late_pool)
        self.assertEqual(late_pool_address, 0x10A000000)
        self.assertEqual(
            high_tail - (late_pool_address + late_pool[0]), 0xF0000
        )

    def test_title_specific_0x70000000_would_fragment_bloodborne(self):
        total_direct_memory = 0x120000000
        gnm_size = 0x10000
        bloodborne_regions = (
            (0x07400000, 0x200000),
            (0x20C00000, 0x4000),
            (0x11400000, 0x4000),
            (0x09800000, 0x10000),
            (0x8EF00000, 0x10000),
            (0x37600000, 0x10000),
            (0x00A00000, 0x4000),
            (0x00600000, 0x4000),
            (0x00E00000, 0x4000),
            (0x00500000, 0x10000),
            (0x00800000, 0x10000),
            (0x00900000, 0x4000),
            (0x12800000, 0x4000),
            (0x00020000, 0x10000),
        )

        title_specific = [(0, total_direct_memory)]
        self.assertTrue(
            reserve_exact(title_specific, 0x70000000, gnm_size)
        )
        title_specific_results = [
            allocate_first_fit(title_specific, size, alignment)
            for size, alignment in bloodborne_regions
        ]
        self.assertIsNone(title_specific_results[5])

        high_tail = total_direct_memory - gnm_size
        preserved = [(0, total_direct_memory)]
        self.assertTrue(reserve_exact(preserved, high_tail, gnm_size))
        preserved_results = [
            allocate_first_fit(preserved, size, alignment)
            for size, alignment in bloodborne_regions
        ]
        self.assertNotIn(None, preserved_results)

    def test_fex_reserves_gnm_before_deferred_lifecycle_drain(self):
        process_bootstrap = NATIVE_RUNTIME.split(
            "bool EnsureMappedEntryProcessBootstrap", 1
        )[1].split(
            'extern "C" int executor_lsx4_android_run_guest_once', 1
        )[0]
        self.assertIn(
            "preserve_deferred_guest_libc_first_fit", process_bootstrap
        )
        self.assertIn("RuntimeGuestFexActive()", process_bootstrap)
        self.assertIn("FaultSignalOwner::Fex", process_bootstrap)
        self.assertIn(
            "Core::AeroLib::ExecutorUseRealLibcAlloc()",
            process_bootstrap,
        )
        self.assertIn(
            "GnmDriverPhysicalPlacement::HighTail", process_bootstrap
        )
        self.assertIn(
            "GnmDriverPhysicalPlacement::FirstFit", process_bootstrap
        )
        reserve = process_bootstrap.index(
            "EnsureMappedEntryGnmDriverBootstrap"
        )
        finalize = process_bootstrap.index(
            "FinalizeFexBootstrapLifecycleCollection"
        )
        self.assertLess(reserve, finalize)

        activation = NATIVE_RUNTIME.split(
            "std::string lifecycle_error;", 1
        )[1].split("Lsx4FexMappedEntryResultV5 fex_result", 1)[0]
        self.assertIn("DrainDeferredFexBootstrapLifecycles", activation)
        self.assertNotIn("EnsureMappedEntryGnmDriverBootstrap", activation)
        self.assertNotIn("gnm_driver_post_lifecycle", activation)

    def test_gnm_bootstrap_publishes_done_only_after_successful_map(self):
        helper = NATIVE_RUNTIME.split(
            "bool EnsureMappedEntryGnmDriverBootstrap", 1
        )[1].split(
            "bool EnsureMappedEntryProcessBootstrap", 1
        )[0]
        lock = helper.index("const std::lock_guard gnm_lock(gnm_mutex)")
        allocate = helper.index("sceKernelAllocateDirectMemory")
        mapped = helper.index("sceKernelMapNamedDirectMemory")
        publish = helper.index("gnm_done = true")
        self.assertLess(lock, allocate)
        self.assertLess(allocate, mapped)
        self.assertLess(mapped, publish)
        self.assertIn(
            "Common::AlignDown(total_direct_size - GnmDriverDmemSize",
            helper,
        )
        self.assertIn("search_start + GnmDriverDmemSize", helper)
        self.assertIn("static_cast<s64>(search_end)", helper)
        self.assertIn('"high-tail"', helper)
        self.assertIn(
            "static_cast<u64>(phys_addr) != search_start", helper
        )
        map_failure = helper.split("if (map_result != 0)", 1)[1].split(
            "gnm_done = true", 1
        )[0]
        self.assertIn("release_failed_gnm_allocation()", map_failure)
        self.assertIn("GnmDriverVirtualAddress", map_failure)
        self.assertIn("sceKernelMunmap", map_failure)
        self.assertNotIn("compare_exchange", helper)

    def test_module_preload_publishes_done_only_after_success(self):
        preload = NATIVE_RUNTIME.split(
            "bool EnsureMappedEntryModulePreload", 1
        )[1].split(
            "bool EnsureMappedEntryGnmDriverBootstrap", 1
        )[0]
        lock = preload.index(
            "const std::lock_guard preload_lock(preload_mutex)"
        )
        libc = preload.index("LoadAndStartModule")
        sysmodule = preload.index(
            "sceSysmodulePreloadModuleForLibkernel"
        )
        publish = preload.index("preload_done = true")
        self.assertLess(lock, libc)
        self.assertLess(libc, sysmodule)
        self.assertLess(sysmodule, publish)
        self.assertNotIn("compare_exchange", preload)

    def test_direct_memory_failure_diagnostic_is_failure_only(self):
        allocate = KERNEL_MEMORY.split(
            "s32 PS4_SYSV_ABI sceKernelAllocateDirectMemory", 1
        )[1].split(
            "s32 PS4_SYSV_ABI sceKernelAllocateMainDirectMemory", 1
        )[0]
        failure = allocate.split("if (phys_addr == -1)", 1)[1].split(
            "*physAddrOut", 1
        )[0]
        self.assertIn("[EXECUTOR_DMEM_ALLOC_FAIL]", failure)
        self.assertIn("ORBIS_KERNEL_ERROR_EAGAIN", failure)
        self.assertNotIn("[EXECUTOR_DMEM_ALLOC_FAIL]", allocate.split(
            "*physAddrOut", 1
        )[1])


if __name__ == "__main__":
    unittest.main()
