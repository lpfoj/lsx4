import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MEMORY_H = (ROOT / "funnel-arm/src/core/memory.h").read_text(encoding="utf-8")
MEMORY_CPP = (ROOT / "funnel-arm/src/core/memory.cpp").read_text(encoding="utf-8")


class ThreadLocalSnapshotPlanLeaseContractTests(unittest.TestCase):
    def test_candidate_is_independently_default_off_and_observable(self):
        gate = MEMORY_CPP.split(
            "bool ThreadLocalSnapshotPlanLeaseEnabled", 1
        )[1].split("void RecordThreadLocalSnapshotPlanLeaseHit", 1)[0]
        self.assertIn("run-enable-thread-local-snapshot-plan-lease", gate)
        self.assertIn("::access", gate)
        self.assertIn("return false", gate)
        proof = MEMORY_CPP.split(
            "void RecordThreadLocalSnapshotPlanLeaseHit", 1
        )[1].split("void RecordAliasFreeSnapshotNegativeCacheHit", 1)[0]
        self.assertIn("firstHit=1", proof)
        self.assertIn("vectorCopy=0 atomicRefcount=0", proof)
        self.assertIn("collisionFallback=1", proof)
        self.assertIn("thread-local-snapshot-plan-lease.txt", proof)

    def test_hit_key_preserves_manager_topology_and_exact_range(self):
        build = MEMORY_CPP.split(
            "bool MemoryManager::BuildSnapshotWritePlan", 1
        )[1].split("bool MemoryManager::IsWritableMapping", 1)[0]
        for token in (
            "snapshot_plan_cache_identity",
            "vma_generation.load(std::memory_order_acquire)",
            "virtual_addr == virtual_addr",
            "size == size",
            "replay_ranges.empty()",
            "static constexpr size_t CacheCapacity = 1024",
        ):
            self.assertIn(token, build)
        hit = build.split("if (thread_local_lease_entry->valid", 1)[1].split(
            "SharedCacheEntry*", 1
        )[0]
        self.assertIn("++thread_local_lease_entry->active_leases", hit)
        self.assertIn("plan.thread_local_cached_data", hit)
        self.assertNotIn("plan.cached_data =", hit)
        self.assertNotIn("owned_write_ranges =", hit)

    def test_active_collision_cannot_overwrite_immutable_plan(self):
        build = MEMORY_CPP.split(
            "bool MemoryManager::BuildSnapshotWritePlan", 1
        )[1].split("bool MemoryManager::IsWritableMapping", 1)[0]
        store = build.split("if (thread_local_lease_entry != nullptr", 1)[1]
        self.assertIn("thread_local_lease_entry->active_leases == 0", store)
        self.assertLess(
            store.index("thread_local_lease_entry->valid = false"),
            store.index("thread_local_lease_entry->data.write_ranges"),
        )
        self.assertLess(
            store.index("thread_local_lease_entry->active_leases = 1"),
            store.index("thread_local_lease_entry->valid = true"),
        )

    def test_plan_move_transfers_exactly_one_non_atomic_lease(self):
        for token in (
            "~SnapshotWritePlan();",
            "SnapshotWritePlan(SnapshotWritePlan&& other) noexcept;",
            "const SnapshotWritePlanData* thread_local_cached_data{}",
            "u32* thread_local_cache_leases{}",
        ):
            self.assertIn(token, MEMORY_H)
        move = MEMORY_CPP.split(
            "MemoryManager::SnapshotWritePlan::operator=(SnapshotWritePlan&& other)", 1
        )[1].split("void MemoryManager::SnapshotWritePlan::Reset", 1)[0]
        self.assertIn(
            "std::exchange(other.thread_local_cached_data, nullptr)", move
        )
        self.assertIn(
            "std::exchange(other.thread_local_cache_leases, nullptr)", move
        )
        reset = MEMORY_CPP.split(
            "void MemoryManager::SnapshotWritePlan::Reset", 1
        )[1].split("#ifdef __ANDROID__", 1)[0]
        self.assertIn("--*thread_local_cache_leases", reset)
        self.assertIn("thread_local_cached_data = nullptr", reset)

    def test_old_shared_cache_and_uncached_fallback_remain(self):
        build = MEMORY_CPP.split(
            "bool MemoryManager::BuildSnapshotWritePlan", 1
        )[1].split("bool MemoryManager::IsWritableMapping", 1)[0]
        self.assertIn("ImmutableSnapshotPlanCacheEnabled()", build)
        self.assertIn("!thread_local_lease_cacheable", build)
        self.assertIn("BuildSnapshotWriteRangesUncached", build)
        self.assertIn("std::make_shared<SnapshotWritePlanData>()", build)
        self.assertIn("active_leases == 0", build)


if __name__ == "__main__":
    unittest.main()
