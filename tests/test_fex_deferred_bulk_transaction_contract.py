import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PAGE_MANAGER_H = (
    ROOT / "funnel-arm" / "src" / "video_core" / "page_manager.h"
).read_text(encoding="utf-8")
PAGE_MANAGER_CPP = (
    ROOT / "funnel-arm" / "src" / "video_core" / "page_manager.cpp"
).read_text(encoding="utf-8")


class FexDeferredBulkTransactionContractTests(unittest.TestCase):
    def test_public_transaction_is_explicit_and_has_no_sync_fallback(self):
        self.assertIn(
            "TryBeginFexDeferredBulkGuestWrite(\n"
            "        VAddr address, u64 size, bool require_read_ready = false) noexcept",
            PAGE_MANAGER_H,
        )
        begin = PAGE_MANAGER_CPP.split(
            "bool PageManager::TryBeginFexDeferredBulkGuestWrite", 1
        )[1].split("void PageManager::EnterBulkGuestWrite", 1)[0]

        self.assertIn("const u64 captured_epoch", begin)
        self.assertIn("SCOPE_EXIT", begin)
        self.assertIn("AbortCurrentFexBulkLeaseAfter(captured_epoch)", begin)
        self.assertIn("TryPrepareFexDeferredBulkGuestWrite", begin)
        self.assertIn("TryEnterFexDeferredBulkGuestWriteActivity", begin)
        self.assertIn("TryAdmitCurrentUntrackedFexBulkWriterOnce", begin)
        self.assertIn("TryEnterFexDirectReadyBulkGuestWriteActivity", begin)
        self.assertGreaterEqual(begin.count("require_read_ready"), 3)
        self.assertIn("ExecutorFexBulkLeasePhase::Entered", begin)
        self.assertIn("committed = true", begin)

        self.assertLess(
            begin.index("TryPrepareFexDeferredBulkGuestWrite"),
            begin.index("TryEnterFexDeferredBulkGuestWriteActivity"),
        )
        self.assertLess(
            begin.index("executor_fex_bulk_activity_deferred = true"),
            begin.index("ExecutorFexBulkLeasePhase::Entered"),
        )
        self.assertLess(
            begin.index("ExecutorFexBulkLeasePhase::Entered"),
            begin.index("committed = true"),
        )

        self.assertNotIn("Impl::owner->PrepareBulkGuestWrite", begin)
        self.assertNotIn("PageManager::PrepareBulkGuestWrite", begin)
        self.assertNotIn("EnterBulkGuestWrite(address", begin)
        self.assertNotIn("sleep_for", begin)
        self.assertNotIn("yield", begin)

    def test_read_ready_validation_precedes_deferred_page_side_effects(self):
        prepare = PAGE_MANAGER_CPP.split(
            "bool TryPrepareFexDeferredBulkGuestWrite", 1
        )[1].split("bool TryEnterFexDeferredBulkGuestWriteActivity", 1)[0]
        first_validation = prepare.index(
            "require_read_ready && ActualReadProtected(page)"
        )
        acquire_epoch = prepare.index("AcquireFexBulkPageEpoch")
        open_bypass = prepare.index("SetFexDeferredWriteBypass(page, true)")
        self.assertLess(first_validation, acquire_epoch)
        self.assertLess(first_validation, open_bypass)
        self.assertGreaterEqual(
            prepare.count("require_read_ready && ActualReadProtected(page)"), 2
        )

        enter = PAGE_MANAGER_CPP.split(
            "bool TryEnterFexDeferredBulkGuestWriteActivity", 1
        )[1].split("bool TryEnterFexDirectReadyBulkGuestWriteActivity", 1)[0]
        self.assertIn("require_read_ready && ActualReadProtected(page)", enter)
        self.assertLess(
            enter.index("require_read_ready && ActualReadProtected(page)"),
            enter.index("UpdateBulkWriterCount(page, true)"),
        )

        direct = PAGE_MANAGER_CPP.split(
            "bool TryEnterFexDirectReadyBulkGuestWriteActivity", 1
        )[1].split("void CompleteFexDeferredBulkGuestWrite", 1)[0]
        self.assertIn("require_read_ready && ActualReadProtected(page)", direct)
        self.assertIn("WriteWatcherHazard(page)", direct)
        self.assertIn("ActualWriteProtected(page)", direct)
        self.assertIn("WriteFaultBypassed(page)", direct)
        self.assertIn("FexDeferredWriteBypassed(page)", direct)
        self.assertIn("BulkPrepareReservation(page) != 0", direct)
        self.assertLess(
            direct.index("require_read_ready && ActualReadProtected(page)"),
            direct.index("UpdateBulkWriterCount(page, true)"),
        )
        self.assertNotIn("Protect(", direct)
        self.assertNotIn("rasterizer", direct)
        self.assertNotIn("sleep_for", direct)
        self.assertNotIn("yield", direct)

        read_ready = PAGE_MANAGER_CPP.split(
            "bool PageManager::IsFexGuestMemoryReadReady", 1
        )[1].split("void PageManager::EnterBulkGuestWrite", 1)[0]
        self.assertIn("Impl::owner != nullptr", read_ready)
        self.assertIn("Impl::owner->IsFexGuestMemoryReadReady", read_ready)
        internal_read_ready = PAGE_MANAGER_CPP.split(
            "bool IsFexGuestMemoryReadReady", 1
        )[1].split("void CompleteFexDeferredBulkGuestWrite", 1)[0]
        self.assertIn("Common::RangeLockGuard", internal_read_ready)
        self.assertIn("ActualReadProtected(page)", internal_read_ready)
        self.assertNotIn("Protect(", internal_read_ready)
        self.assertNotIn("rasterizer", internal_read_ready)
        self.assertNotIn("UpdateBulkWriterCount", internal_read_ready)
        self.assertNotIn("sleep_for", internal_read_ready)
        self.assertNotIn("yield", internal_read_ready)

    def test_epoch_enabled_query_is_atomic_and_side_effect_free(self):
        self.assertIn("FexDeferredWriteEpochEnabled() noexcept", PAGE_MANAGER_H)
        query = PAGE_MANAGER_CPP.split(
            "bool PageManager::FexDeferredWriteEpochEnabled() noexcept", 1
        )[1].split("bool PageManager::IsFexGuestMemoryReadReady", 1)[0]
        self.assertIn("enabled.load(std::memory_order_acquire)", query)
        self.assertNotIn("store(", query)
        self.assertNotIn("sleep_for", query)
        self.assertNotIn("yield", query)

    def test_sm8650_bulk_wrappers_use_deferred_transaction_before_sync(self):
        gate = PAGE_MANAGER_CPP.split(
            "bool ExecutorFexDeferredBulkGuestWritesEnabled()", 1
        )[1].split("bool ExecutorShouldLogReadbackOrdinal", 1)[0]
        self.assertIn("EXECUTOR_FEX_DEFERRED_BULK_GUEST_WRITES", gate)
        self.assertIn("ExecutorFexBufferCaptureAckEnabled()", gate)

        wrappers = PAGE_MANAGER_CPP.split(
            "void PageManager::PrepareBulkGuestWrite(", 1
        )[1].split("bool PageManager::TryPrepareFexDeferredBulkGuestWrite", 1)[0]
        self.assertEqual(
            wrappers.count("ExecutorFexDeferredBulkGuestWritesEnabled()"), 2
        )
        self.assertEqual(
            wrappers.count("TryPrepareFexDeferredBulkGuestWrite"), 2
        )
        self.assertLess(
            wrappers.index("TryPrepareFexDeferredBulkGuestWrite"),
            wrappers.index("Impl::owner->PrepareBulkGuestWrite"),
        )

    def test_faulted_buffer_capture_retry_is_bounded_before_handoff(self):
        capture = PAGE_MANAGER_CPP.split(
            "bool PageManager::BeginFexDeferredBufferCapture", 1
        )[1].split("void PageManager::EndFexDeferredBufferCapture", 1)[0]

        faulted = capture.split("if (must_retry)", 1)[1].split(
            "if (!has_ready || unsupported)", 1
        )[0]

        self.assertIn("FexBufferCaptureFaultedRetryLimit = 32", PAGE_MANAGER_CPP)
        self.assertIn(
            "if (++faulted_retries < FexBufferCaptureFaultedRetryLimit)",
            faulted,
        )
        self.assertIn("std::this_thread::yield();", faulted)
        self.assertEqual(faulted.count("continue;"), 1)
        self.assertIn("buffer_capture_faulted_fallbacks.fetch_add", faulted)
        self.assertIn("first_faulted_token", faulted)
        self.assertIn("first_faulted_owner_tid", faulted)
        self.assertIn("reopen_gates();", faulted)
        self.assertIn("coordinator_lock.unlock();", faulted)
        self.assertIn("return false;", faulted)
        self.assertLess(
            faulted.index("if (++faulted_retries"), faulted.index("continue;")
        )
        self.assertLess(faulted.index("continue;"), faulted.index("return false;"))

    def test_exact_epoch_page_index_is_optional_and_fail_closed(self):
        gate = PAGE_MANAGER_CPP.split(
            "bool ExecutorExactFexEpochPageIndexEnabled()", 1
        )[1].split("bool ExecutorFexDeferredBulkGuestWritesEnabled", 1)[0]
        self.assertIn("run-enable-exact-fex-epoch-page-index", gate)

        add = PAGE_MANAGER_CPP.split(
            "void AddFexPendingPageRef", 1
        )[1].split("void RemoveFexPendingPageRef", 1)[0]
        self.assertIn("const u64 token", add)
        self.assertIn("previous == 0 ? token : 0", add)

        remove = PAGE_MANAGER_CPP.split(
            "void RemoveFexPendingPageRef", 1
        )[1].split("bool EnterFexWriteEpochArming", 1)[0]
        self.assertIn("if (previous <= 2)", remove)
        self.assertIn("pending_page_tokens", remove)
        self.assertIn(".store(0, std::memory_order_release)", remove)

        capture = PAGE_MANAGER_CPP.split(
            "bool PageManager::BeginFexDeferredBufferCapture", 1
        )[1].split("void PageManager::EndFexDeferredBufferCapture", 1)[0]
        self.assertIn("refs != 1 || token == 0", capture)
        self.assertIn("slot.token.load(std::memory_order_acquire) != token", capture)
        self.assertIn("slot.address != page", capture)
        self.assertIn("exact_indexed = false", capture)
        self.assertIn("for (const FexWriteEpochSlot& slot : epochs.slots)", capture)
        self.assertLess(
            capture.index("exact_indexed = false"),
            capture.index("for (const FexWriteEpochSlot& slot : epochs.slots)"),
        )

    def test_buffer_capture_noop_check_uses_effective_permissions(self):
        guard = PAGE_MANAGER_CPP.split(
            "bool GuardFexBufferCapturePage", 1
        )[1].split("void EndFexBufferCapture", 1)[0]
        end = PAGE_MANAGER_CPP.split(
            "void EndFexBufferCapture", 1
        )[1].split("bool FaultProtectionActive", 1)[0]

        self.assertIn("EffectivePerms(page, capture_perms)", guard)
        self.assertIn("EffectivePerms(page, restored_perms)", end)
        for body in (guard, end):
            self.assertIn("ActualReadProtected(page) != desired_read_protected", body)
            self.assertIn("ActualWriteProtected(page) != desired_write_protected", body)
            self.assertEqual(body.count("Protect("), 1)

    def test_bulk_epoch_gate_admission_is_single_shot(self):
        admission = PAGE_MANAGER_CPP.split(
            "bool TryEnterFexBulkEpochArming() noexcept", 1
        )[1].split("void LeaveFexBulkEpochArming", 1)[0]

        self.assertEqual(admission.count("active_bulk_armers.fetch_add"), 1)
        self.assertEqual(admission.count("active_bulk_armers.fetch_sub"), 1)
        self.assertGreaterEqual(admission.count("bulk_gate_open.load"), 2)
        self.assertGreaterEqual(admission.count("epochs.enabled.load"), 2)
        self.assertNotIn("while (", admission)
        self.assertNotIn("for (", admission)
        self.assertNotIn("sleep_for", admission)
        self.assertNotIn("yield", admission)

        direct_admission = PAGE_MANAGER_CPP.split(
            "bool TryAdmitCurrentUntrackedFexBulkWriterOnce", 1
        )[1].split("void ReleaseCurrentFexBulkWriterAdmission", 1)[0]
        self.assertEqual(
            direct_admission.count("active_untracked_bulk_writers.fetch_add"), 1
        )
        self.assertEqual(
            direct_admission.count("active_untracked_bulk_writers.fetch_sub"), 1
        )
        self.assertEqual(
            direct_admission.count("active_bulk_writers.fetch_add"), 1
        )
        self.assertEqual(
            direct_admission.count("active_bulk_writers.fetch_sub"), 1
        )
        self.assertNotIn("while (", direct_admission)
        self.assertNotIn("for (", direct_admission)
        self.assertNotIn("sleep_for", direct_admission)
        self.assertNotIn("yield", direct_admission)

    def test_prepare_transfers_armer_admission_to_writer_or_rolls_back(self):
        prepare = PAGE_MANAGER_CPP.split(
            "bool TryPrepareFexDeferredBulkGuestWrite", 1
        )[1].split("bool TryEnterFexDeferredBulkGuestWriteActivity", 1)[0]

        self.assertIn("!TryEnterFexBulkEpochArming()", prepare)
        self.assertIn("SCOPE_EXIT { LeaveFexBulkEpochArming(); }", prepare)
        self.assertIn("executor_fex_bulk_writer_admitted = true", prepare)
        self.assertIn("epochs.active_bulk_writers.fetch_add", prepare)
        self.assertLess(
            prepare.index("executor_fex_bulk_writer_admitted = true"),
            prepare.index("return true"),
        )

    def test_abort_releases_activity_ticket_and_admission(self):
        abort = PAGE_MANAGER_CPP.split(
            "void PageManager::AbortCurrentFexBulkLeaseAfter", 1
        )[1].split("OwnedPageFaultPcStats PageManager::GetOwnedPageFaultPcStats", 1)[0]

        self.assertIn("SetBulkGuestWriteActivity", abort)
        self.assertIn("PublishAndClearCurrentFexDeferredBulkTicket()", abort)
        self.assertIn("ReleaseCurrentFexBulkWriterAdmission()", abort)
        self.assertIn("ExecutorFexBulkLeasePhase::Idle", abort)
        self.assertLess(
            abort.index("SetBulkGuestWriteActivity"),
            abort.index("PublishAndClearCurrentFexDeferredBulkTicket()"),
        )
        self.assertLess(
            abort.index("PublishAndClearCurrentFexDeferredBulkTicket()"),
            abort.index("ReleaseCurrentFexBulkWriterAdmission()"),
        )


if __name__ == "__main__":
    unittest.main()
