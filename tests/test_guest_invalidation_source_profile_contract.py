from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PAGE_MANAGER = (
    ROOT / "funnel-arm/src/video_core/page_manager.cpp"
).read_text(encoding="utf-8")
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
DRIVER = (ROOT / "tools/codex_bloodborne_pmu.sh").read_text(
    encoding="utf-8"
)


def _scope_between(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


PROFILE_RECORDER = _scope_between(
    PAGE_MANAGER,
    "void ExecutorRecordBulkInvalidationProfile(const u64 requested_pages,",
    "\nstruct FexWriteEpochSlot",
)
SAMPLE_BULK_GUEST_INVALIDATION = _scope_between(
    PAGE_MANAGER,
    "    SampleBulkGuestInvalidation(const size_t first_page,",
    "\n#ifdef __ANDROID__\n    void ClearHleCleanBufferWriteTicket",
)
PREPARE_BULK_GUEST_WRITE = _scope_between(
    PAGE_MANAGER,
    "    void PrepareBulkGuestWrite(const VAddr address, const u64 size) {",
    "\n    void CancelPreparedBulkGuestWrite",
)
PREPARE_PROFILE_HOT_PATH = PREPARE_BULK_GUEST_WRITE.split(
    "if (TryEnterUnwatchedBulkGuestWrite", 1
)[0]


class GuestInvalidationSourceProfileContractTests(unittest.TestCase):
    def test_profile_is_explicit_and_observe_only(self):
        self.assertIn("run-profile-guest-invalidation-sources", NATIVE)
        self.assertIn("EXECUTOR_PROFILE_GUEST_INVALIDATION_SOURCES", NATIVE)
        self.assertIn("behavior=observe-only", NATIVE)
        self.assertNotIn("run-profile-guest-invalidation-sources", NATIVE.split(
            "SetFullRate60HzTarget", 1
        )[0])

    def test_default_bulk_path_keeps_early_exit_and_full_range_invalidation(self):
        body = PREPARE_PROFILE_HOT_PATH
        self.assertIn("ExecutorShouldSampleGuestInvalidationSources()", body)
        self.assertEqual(body.count("SampleBulkGuestInvalidation("), 1)
        self.assertIn("if (profile_sources)", body)
        self.assertIn(
            "while (first_invalidated_page < page_end &&", body
        )
        self.assertIn("page_begin, page_limit - page_begin", body)
        for outlined_only_token in (
            "MaxSampledPages",
            "std::try_to_lock",
            "WriteWatcherSourceMask(page)",
            "ExecutorBulkInvalidationFirstBlocker::Incomplete",
        ):
            self.assertNotIn(outlined_only_token, body)
        self.assertNotIn("former inlined diagnostic", body)
        self.assertNotIn("#if 0", body)
        self.assertNotIn("EXECUTOR_NARROW", body)

    def test_profile_reports_bulk_range_density_and_fault_context(self):
        for field in (
            "bulkCalls=",
            "bulkInvalidations=",
            "bulkNoops=",
            "requestedPages=",
            "protectedPages=",
            "invalidatedPages=",
            "potentialSpanPages=",
            "spanSavingsPages=",
            "protectedRuns=",
            "singlePageCalls=",
            "twoToFourPageCalls=",
            "fiveToSixteenPageCalls=",
            "largePageCalls=",
            "allProtectedCalls=",
            "sparseCalls=",
            "syncWaitNs=",
            "maxSyncWaitNs=",
            "generatedBufferFaults=",
            "generatedTextureFaults=",
            "generatedOtherFaults=",
        ):
            self.assertIn(field, PROFILE_RECORDER)

    def test_profile_logs_sparse_cumulative_power_of_two_snapshots(self):
        self.assertIn("return ((++ordinal) & 1023u) == 1u;", PAGE_MANAGER)
        self.assertIn("if ((calls & (calls - 1)) != 0)", PROFILE_RECORDER)
        self.assertNotIn("(calls & 65535u)", PROFILE_RECORDER)

    def test_profile_estimates_bounds_but_keeps_actual_full_range(self):
        self.assertIn("sample.first_protected_page", SAMPLE_BULK_GUEST_INVALIDATION)
        self.assertIn("sample.last_protected_page", SAMPLE_BULK_GUEST_INVALIDATION)
        self.assertIn(
            "profile_sample->last_protected_page -\n"
            "                          profile_sample->first_protected_page + 1",
            PREPARE_PROFILE_HOT_PATH,
        )
        self.assertIn(
            "rasterizer->InvalidateMemoryFromGuestThread(\n"
            "                page_begin, page_limit - page_begin);",
            PREPARE_PROFILE_HOT_PATH,
        )
        self.assertNotIn(
            "potential_span_pages << PAGE_BITS", PREPARE_PROFILE_HOT_PATH
        )

    def test_eligibility_snapshot_is_sparse_bounded_and_nonblocking(self):
        body = SAMPLE_BULK_GUEST_INVALIDATION
        for token in (
            "static thread_local ExecutorBulkInvalidationProfileSample",
            "MaxSampledPages = 256",
            "std::unique_lock<LockType>",
            "std::try_to_lock",
            "chunk_lock.owns_lock()",
            "eligibility.incomplete_sample = true",
            "FindChunk(page)",
            "FindState(page)",
            "ActualWriteProtected(page)",
            "ActualReadProtected(page)",
            "WriteWatcherSourceMask(page)",
            "GpuDirty(page)",
            "GpuMapped(page)",
            "BaseHostPerms(page)",
            "GuestCpuAccessAllowed(page, true)",
            "BulkWriteActive(page)",
            "BulkPrepareReservation(page)",
            "DescriptorSnapshotConflicted(page)",
            "WriteFaultBypassed(page)",
            "FexDeferredWriteBypassed(page)",
            "FexBufferCaptureBlocked(page)",
            "Core::Memory::TryInstance()",
            "PhysicalAliasingMayExist()",
            "DebugVmaGeneration()",
        ):
            self.assertIn(token, body)
        self.assertNotIn("Core::Memory::Instance()", body)
        self.assertNotIn("Common::RangeLockGuard", body)
        self.assertNotIn("if (!page_protected)", body)
        self.assertNotIn("InvalidateMemory", body)
        self.assertNotIn("#if 0", body)

    def test_eligibility_uses_exclusive_first_blocker_order(self):
        body = SAMPLE_BULK_GUEST_INVALIDATION.split(
            "if (eligibility.incomplete_sample)", 1
        )[1]
        ordered = (
            "ExecutorBulkInvalidationFirstBlocker::Incomplete",
            "ExecutorBulkInvalidationFirstBlocker::Alias",
            "ExecutorBulkInvalidationFirstBlocker::Topology",
            "ExecutorBulkInvalidationFirstBlocker::Read",
            "ExecutorBulkInvalidationFirstBlocker::Special",
            "ExecutorBulkInvalidationFirstBlocker::GpuDirty",
            "ExecutorBulkInvalidationFirstBlocker::Source",
            "ExecutorBulkInvalidationFirstBlocker::Mapping",
            "ExecutorBulkInvalidationFirstBlocker::Busy",
            "ExecutorBulkInvalidationFirstBlocker::Eligible",
        )
        positions = [body.index(token) for token in ordered]
        self.assertEqual(positions, sorted(positions))

    def test_fex_deferred_eligibility_requires_buffer_only_source(self):
        body = SAMPLE_BULK_GUEST_INVALIDATION.split(
            "const bool page_source_blocked =", 1
        )[1].split("const bool page_mapping_blocked =", 1)[0]
        self.assertIn("page_protected && !source_buffer", body)

    def test_descriptor_conflict_is_a_transient_busy_blocker(self):
        body = SAMPLE_BULK_GUEST_INVALIDATION.split(
            "const bool page_special_blocked =", 1
        )[1].split("eligibility.read_blocker =", 1)[0]
        special, busy = body.split("const bool page_busy_blocked =", 1)
        self.assertNotIn("descriptor_conflict", special)
        self.assertIn("descriptor_conflict", busy)

    def test_eligibility_logging_exposes_reconcilable_invariants(self):
        for field in (
            "aliasBlockedCalls=",
            "incompleteSampleCalls=",
            "topologyBlockedCalls=",
            "readBlockedCalls=",
            "specialBlockedCalls=",
            "gpuDirtyBlockedCalls=",
            "sourceBlockedCalls=",
            "mappingBlockedCalls=",
            "busyBlockedCalls=",
            "eligibleCalls=",
            "classifiedInvalidations=",
            "sourceNonePages=",
            "sourceBufferPages=",
            "sourceTexturePages=",
            "sourceMixedPages=",
            "sourceMismatchPages=",
            "sourceClassifiedPages=",
            "eligibleSyncWaitNs=",
            "blockedSyncWaitNs=",
            "classifiedSyncWaitNs=",
        ):
            self.assertIn(field, PROFILE_RECORDER)

    def test_driver_controls_profile_marker_explicitly(self):
        self.assertIn("BB_PROFILE_INVALIDATION_SOURCES", DRIVER)
        self.assertIn("run-profile-guest-invalidation-sources", DRIVER)


if __name__ == "__main__":
    unittest.main()
