import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
NATIVE_RUNTIME = ROOT / "src" / "executor" / "native_runtime_api.cpp"


class FexHleFramePoolMemoryLeaseContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = NATIVE_RUNTIME.read_text(encoding="utf-8")

    def test_hle_frame_pools_are_registered_as_mutable_sideband_only(self):
        self.assertIn(
            "g_box64_external_guest_mutable_ranges", self.source
        )
        funnel = self.source.split(
            '"funnel_hle_frame_pool"', 1
        )[0].rsplit("RegisterBox64ExternalGuestMutableRange", 1)[1]
        box64 = self.source.split(
            '"box64_hle_frame_pool"', 1
        )[0].rsplit("RegisterBox64ExternalGuestMutableRange", 1)[1]
        self.assertIn("geometry.base", funnel)
        self.assertIn("geometry.size", funnel)
        self.assertIn("pool.base", box64)
        self.assertIn("ExecutorBox64HleSafePoolSize", box64)

        registration = self.source.split(
            "void RegisterBox64ExternalGuestMutableRange", 1
        )[1].split("void RegisterBox64GuestGcSidebandRange", 1)[0]
        self.assertNotIn("RegisterBox64GuestStackRange", registration)
        self.assertNotIn("RegisterBox64GuestReadableRange", registration)
        self.assertNotIn("MarkBox64GuestGcRootsDirty", registration)
        self.assertNotIn("PublishJitRegisteredFullPages", registration)

    def test_no_host_reentry_helpers_can_lease_mutable_sideband_without_gpu_epoch(self):
        acquire = self.source.split(
            "int32_t ExecutorFexAcquireGuestMemoryLease", 1
        )[1].split("void ExecutorFexReleaseGuestMemoryLease", 1)[0]
        start = acquire.index(
            "if (no_host_reentry) {\n        std::unique_lock mutable_lock(g_box64_guest_stack_lock);"
        )
        end = acquire.index("if (!write) {", start)
        mutable = acquire[start:end]
        self.assertIn("if (no_host_reentry)", mutable)
        self.assertNotIn("FexDeferredWriteEpochEnabled", mutable)
        self.assertIn("IsActiveHleGuestReadLeaseRange", mutable)
        self.assertIn("IsExternalGuestMutableRangeLocked", mutable)
        self.assertIn(
            "std::unique_lock mutable_lock(g_box64_guest_stack_lock)", mutable
        )
        self.assertIn("std::move(mutable_lock)", mutable)
        self.assertNotIn("VideoCore::PageManager", mutable)
        self.assertNotIn("bulk_write_active", mutable)

    def test_typed_hle_stack_arguments_can_read_a_leased_pool_slot(self):
        helper = self.source.split(
            "bool IsActiveHleGuestReadLeaseRange", 1
        )[1].split('extern "C" bool ExecutorJitIsReadableGuestRange', 1)[0]
        self.assertIn("g_active_hle_guest_read_lease", helper)
        self.assertIn("g_box64_external_guest_mutable_ranges", helper)
        self.assertIn("JitRangeContained", helper)
        self.assertIn("std::lock_guard", helper)
        self.assertNotIn("RegisterBox64GuestReadableRange", helper)
        self.assertNotIn("MarkBox64GuestGcRootsDirty", helper)

        read = self.source.split(
            'extern "C" bool ExecutorJitReadGuestBytes(', 1
        )[1].split(
            'extern "C" bool ExecutorJitReadGuestBytesStable', 1
        )[0]
        self.assertIn("TryReadActiveHleGuestStackLease", read)
        self.assertIn("TryReadExternalGuestMutableRange", read)

    def test_aerolib_readability_probe_admits_only_registered_mutable_ranges(self):
        self.assertIn(
            "IsActiveHleGuestReadLeaseRange(host_address, size) ||",
            self.source,
        )
        self.assertIn(
            "IsExternalGuestMutableRange(host_address, size);",
            self.source,
        )
        membership = self.source.split(
            "bool IsActiveHleGuestReadLeaseRange", 1
        )[1].split(
            "bool TryResolveExternalHleFrameSlot", 1
        )[0]
        self.assertIn("g_active_hle_guest_read_lease", membership)
        self.assertIn("JitRangeContained", membership)

        readable = self.source.split(
            'extern "C" bool ExecutorJitIsReadableGuestRange', 1
        )[1].split(
            'extern "C" bool ExecutorJitReadGuestBytes', 1
        )[0]
        self.assertNotIn("g_box64_guest_gc_sideband_ranges", readable)
        self.assertNotIn("RegisterBox64GuestReadableRange", readable)

        exact = self.source.split(
            "bool IsExternalGuestMutableRangeLocked", 1
        )[1].split("bool TryReadExternalGuestMutableRange", 1)[0]
        self.assertIn("g_box64_external_guest_mutable_ranges", exact)
        self.assertIn("JitRangeContained(address, size, candidate.base", exact)
        self.assertNotIn("ValidateOwnedFrame", exact)

    def test_pool_registration_is_geometry_not_blanket_authority(self):
        resolver = self.source.split(
            "bool TryResolveExternalHleFrameSlot", 1
        )[1].split('extern "C" bool ExecutorJitIsReadableGuestRange', 1)[0]
        self.assertIn("Lsx4::FexHleLease::PoolSize", resolver)
        self.assertIn("Lsx4::FexHleLease::SlotSize", resolver)

        scoped = self.source.split(
            "struct ScopedExecutorHleGuestReadLease", 1
        )[1].split("struct ScopedExecutorHleBoundary", 1)[0]
        self.assertIn("TryResolveExternalHleFrameSlot", scoped)
        self.assertIn(".base = slot_base", scoped)
        self.assertIn(".size = Lsx4::FexHleLease::SlotSize", scoped)


if __name__ == "__main__":
    unittest.main()
