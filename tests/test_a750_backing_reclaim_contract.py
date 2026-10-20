from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
ADDRESS_H = (ROOT / "funnel-arm/src/core/address_space.h").read_text(encoding="utf-8")
ADDRESS_CPP = (ROOT / "funnel-arm/src/core/address_space.cpp").read_text(encoding="utf-8")
MEMORY_H = (ROOT / "funnel-arm/src/core/memory.h").read_text(encoding="utf-8")
MEMORY_CPP = (ROOT / "funnel-arm/src/core/memory.cpp").read_text(encoding="utf-8")
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")


class A750BackingReclaimContract(unittest.TestCase):
    def test_shared_backing_uses_remove_not_dontneed(self):
        self.assertIn("DiscardBackingRange(PAddr phys_addr, u64 size)", ADDRESS_H)
        discard = ADDRESS_CPP.split("bool AddressSpace::DiscardBackingRange", 1)[1]
        discard = discard.split("void* AddressSpace::Map", 1)[0]
        self.assertIn("MADV_REMOVE", discard)
        self.assertIn("_SC_PAGESIZE", discard)
        self.assertNotIn("MADV_DONTNEED", discard)

    def test_policy_is_exact_proprietary_a750_only(self):
        self.assertIn("bool discard_released_backing_ = false", MEMORY_H)
        self.assertIn(
            "instance.GetCapabilityProfile().exact_proprietary_adreno750",
            RASTERIZER,
        )
        self.assertIn("rasterizer_ != nullptr && discard_released_backing_", MEMORY_CPP)

    def test_direct_free_uses_half_open_intersections(self):
        free = MEMORY_CPP.split("s32 MemoryManager::Free", 1)[1]
        free = free.split("s32 MemoryManager::PoolCommit", 1)[0]
        self.assertIn("const PAddr release_end", free)
        self.assertIn("const PAddr overlap_begin = std::max", free)
        self.assertIn("const PAddr overlap_end = std::min", free)
        self.assertIn("cursor = overlap_end", free)
        self.assertNotIn("remaining_size", free)
        self.assertNotIn("std::min<u64>(phys_mapping.size - phys_offset, size)", free)

    def test_direct_discard_follows_alias_teardown_and_free_transition(self):
        free = MEMORY_CPP.split("s32 MemoryManager::Free", 1)[1]
        free = free.split("s32 MemoryManager::PoolCommit", 1)[0]
        self.assertLess(free.index("UnmapMemoryImpl(addr, size)"),
                        free.index("new_dmem_area.dma_type = PhysicalMemoryType::Free"))
        self.assertLess(free.index("new_dmem_area.dma_type = PhysicalMemoryType::Free"),
                        free.index('TryDiscardReleasedBacking(free_addr, free_size, "direct-free")'))

    def test_flexible_zero_fallback_follows_virtual_unmap(self):
        unmap = MEMORY_CPP.split("u64 MemoryManager::UnmapBytesFromEntry", 1)[1]
        unmap = unmap.split("s32 MemoryManager::UnmapMemoryImpl", 1)[0]
        self.assertLess(unmap.index("impl.Unmap(virtual_addr, size_in_vma)"),
                        unmap.index('TryDiscardReleasedBacking(phys_addr, release_size, "flexible-unmap")'))
        self.assertIn("if (!TryDiscardReleasedBacking", unmap)
        self.assertIn("std::memset(impl.BackingBase() + phys_addr, 0, release_size)", unmap)

    def test_pool_decommit_is_not_discarded(self):
        pool = MEMORY_CPP.split("s32 MemoryManager::PoolDecommit", 1)[1]
        pool = pool.split("s32 MemoryManager::UnmapMemory", 1)[0]
        self.assertNotIn("TryDiscardReleasedBacking", pool)


if __name__ == "__main__":
    unittest.main()
