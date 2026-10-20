import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MEMORY_H = (ROOT / "funnel-arm/src/core/memory.h").read_text(encoding="utf-8")
MEMORY_CPP = (ROOT / "funnel-arm/src/core/memory.cpp").read_text(encoding="utf-8")
BUFFER_CACHE = (
    ROOT / "funnel-arm/src/video_core/buffer_cache/buffer_cache.cpp"
).read_text(encoding="utf-8")


class AliasFreeSnapshotNegativeCacheContractTests(unittest.TestCase):
    def test_candidate_is_independently_default_off(self):
        gate = MEMORY_CPP.split(
            "bool AliasFreeSnapshotNegativeCacheEnabled", 1
        )[1].split("void RecordAliasFreeSnapshotNegativeCacheHit", 1)[0]
        self.assertIn("run-enable-alias-free-snapshot-negative-cache", gate)
        self.assertIn("::access", gate)
        self.assertIn("return false", gate)
        self.assertNotIn("EXECUTOR_IMMUTABLE_SNAPSHOT_PLAN", gate)

    def test_cache_identity_covers_manager_generation_range_and_size(self):
        body = MEMORY_CPP.split("MemoryManager::BuildSnapshotSiblingPlan", 1)[1]
        for proof in (
            "owner_identity == snapshot_plan_cache_identity",
            "generation == generation",
            "virtual_addr == virtual_addr",
            "size == size",
            "replay_ranges.empty()",
        ):
            self.assertIn(proof, body)

    def test_only_proven_alias_free_results_are_cached(self):
        body = MEMORY_CPP.split("MemoryManager::BuildSnapshotSiblingPlan", 1)[1]
        self.assertIn("BuildSnapshotWritePlan(guard, virtual_addr, size, plan)", body)
        self.assertIn("!plan.SiblingAliasRanges().empty()", body)
        self.assertLess(
            body.index("!plan.SiblingAliasRanges().empty()"),
            body.index("*cache_entry = AliasFreeEntry"),
        )
        self.assertIn("SnapshotSiblingPlanResult::HasSiblingAliases", body)
        self.assertIn("SnapshotSiblingPlanResult::AliasFree", body)

    def test_alias_cases_and_failures_keep_the_full_old_plan(self):
        self.assertIn("enum class SnapshotSiblingPlanResult", MEMORY_H)
        resolve = BUFFER_CACHE.split("bool BufferCache::ResolveSnapshotDependencies", 1)[1]
        resolve = resolve.split("bool BufferCache::ResolveSnapshotGpuRange", 1)[0]
        self.assertIn("BuildSnapshotSiblingPlan", resolve)
        self.assertIn("SnapshotSiblingPlanResult::Failed", resolve)
        self.assertIn("SnapshotSiblingPlanResult::AliasFree", resolve)
        self.assertIn("snapshot_plan.SiblingAliasRanges()", resolve)
        self.assertIn("snapshot_plan.AliasRanges()", resolve)

    def test_initial_alias_upload_check_only_skips_proven_empty_siblings(self):
        sync = BUFFER_CACHE.split("BufferCache::SynchronizeBuffer", 1)[1]
        self.assertIn("BuildSnapshotSiblingPlan", sync)
        guarded = sync.split("BuildSnapshotSiblingPlan", 1)[1].split(
            "const bool upload_needed", 1
        )[0]
        self.assertIn("SnapshotSiblingPlanResult::Failed", guarded)
        self.assertIn("SnapshotSiblingPlanResult::HasSiblingAliases", guarded)
        self.assertIn("AreSnapshotRangesDirty", guarded)

    def test_direct_runtime_witness_is_non_periodic(self):
        proof = MEMORY_CPP.split(
            "void RecordAliasFreeSnapshotNegativeCacheHit", 1
        )[1].split("struct RasterizerMemoryProtection", 1)[0]
        self.assertIn("static std::atomic_flag recorded", proof)
        self.assertIn("firstHit=1", proof)
        self.assertIn("noSiblingAliases=1", proof)
        self.assertIn("fullFallback=1", proof)
        self.assertIn("alias-free-snapshot-negative-cache.txt", proof)


if __name__ == "__main__":
    unittest.main()
