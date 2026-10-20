import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MEMORY_H = ROOT / "funnel-arm" / "src" / "core" / "memory.h"
MEMORY_CPP = ROOT / "funnel-arm" / "src" / "core" / "memory.cpp"
BUFFER_CPP = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "buffer_cache"
    / "buffer_cache.cpp"
)
RUNTIME_CPP = ROOT / "src" / "executor" / "native_runtime_api.cpp"


class ImmutableSnapshotPlanContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.memory_h = MEMORY_H.read_text(encoding="utf-8")
        cls.memory_cpp = MEMORY_CPP.read_text(encoding="utf-8")
        cls.buffer_cpp = BUFFER_CPP.read_text(encoding="utf-8")
        cls.runtime_cpp = RUNTIME_CPP.read_text(encoding="utf-8")

    def test_candidate_is_explicit_default_off_and_replay_is_not_cached(self):
        self.assertIn('"run-enable-immutable-snapshot-plan"', self.runtime_cpp)
        self.assertIn(
            'setenv("EXECUTOR_IMMUTABLE_SNAPSHOT_PLAN", "1", 1)',
            self.runtime_cpp,
        )
        self.assertIn(
            'unsetenv("EXECUTOR_IMMUTABLE_SNAPSHOT_PLAN")', self.runtime_cpp
        )
        gate = self.memory_cpp.split(
            "bool ImmutableSnapshotPlanCacheEnabled", 1
        )[1].split("RasterizerMemoryProtection", 1)[0]
        self.assertIn('std::getenv("EXECUTOR_IMMUTABLE_SNAPSHOT_PLAN")', gate)
        self.assertIn("return false", gate)

        build = self.memory_cpp.split(
            "bool MemoryManager::BuildSnapshotWritePlan", 1
        )[1].split("bool MemoryManager::IsWritableMapping", 1)[0]
        self.assertIn("ImmutableSnapshotPlanCacheEnabled()", build)
        self.assertIn("replay_ranges.empty()", build)

    def test_cache_identity_and_topology_generation_are_complete(self):
        build = self.memory_cpp.split(
            "bool MemoryManager::BuildSnapshotWritePlan", 1
        )[1].split("bool MemoryManager::IsWritableMapping", 1)[0]
        for token in (
            "snapshot_plan_cache_identity",
            "vma_generation.load(std::memory_order_acquire)",
            "virtual_addr",
            "size",
            "static constexpr size_t CacheCapacity = 1024",
            "static constexpr size_t MaxCachedRangeFanout = 16",
        ):
            self.assertIn(token, build)
        self.assertIn("next_snapshot_plan_cache_identity.fetch_add", self.memory_cpp)
        self.assertIn("BuildSnapshotWriteRangesUncached", build)
        self.assertLess(
            build.index("cache_entry->data"),
            build.index("BuildSnapshotWriteRangesUncached"),
        )

    def test_hit_returns_immutable_spans_without_vector_copy(self):
        for token in (
            "std::span<const SnapshotRange> WriteRanges()",
            "std::span<const SnapshotRange> AliasRanges()",
            "std::span<const SnapshotRange> SiblingAliasRanges()",
            "std::shared_ptr<const SnapshotWritePlanData> cached_data",
        ):
            self.assertIn(token, self.memory_h)

        build = self.memory_cpp.split(
            "bool MemoryManager::BuildSnapshotWritePlan", 1
        )[1].split("bool MemoryManager::IsWritableMapping", 1)[0]
        hit = build.split("if (cache_entry->owner_identity", 1)[1].split(
            "BuildSnapshotWriteRangesUncached", 1
        )[0]
        self.assertIn("plan.cached_data = cache_entry->data", hit)
        self.assertNotIn("owned_write_ranges =", hit)
        self.assertNotIn("owned_alias_ranges =", hit)

        self.assertGreaterEqual(
            self.buffer_cpp.count("BuildSnapshotWritePlan")
            + self.buffer_cpp.count("BuildSnapshotSiblingPlan"),
            6,
        )
        self.assertIn(
            "BuildSnapshotWritePlan(guard, virtual_addr, size, plan)",
            self.memory_cpp.split("MemoryManager::BuildSnapshotSiblingPlan", 1)[1],
        )
        self.assertNotIn("BuildSnapshotWriteRanges(", self.buffer_cpp)
        self.assertIn("snapshot_plan.WriteRanges()", self.buffer_cpp)
        self.assertIn("snapshot_plan.AliasRanges()", self.buffer_cpp)
        self.assertIn("snapshot_plan.SiblingAliasRanges()", self.buffer_cpp)

    def test_primary_alias_exclusion_is_precomputed_once(self):
        build = self.memory_cpp.split(
            "bool MemoryManager::BuildSnapshotWritePlan", 1
        )[1].split("bool MemoryManager::IsWritableMapping", 1)[0]
        ordered = (
            "const VAddr request_end = virtual_addr + size",
            "if (range_addr < virtual_addr)",
            "left_end - range_addr",
            "if (request_end < range_end)",
            "range_end - right_begin",
            "data->sibling_alias_ranges",
        )
        for token in ordered:
            self.assertIn(token, build)
        for before, after in zip(ordered, ordered[1:]):
            self.assertLess(build.index(before), build.index(after))
        self.assertNotIn("exclude_primary_alias", self.buffer_cpp)


if __name__ == "__main__":
    unittest.main()
