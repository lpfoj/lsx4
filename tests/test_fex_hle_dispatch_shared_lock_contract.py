import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
MUTEX = (
    ROOT / "funnel-arm/src/core/libraries/kernel/threads/mutex.cpp"
).read_text(encoding="utf-8")
PTHREAD = (
    ROOT / "funnel-arm/src/core/libraries/kernel/threads/pthread.h"
).read_text(encoding="utf-8")


class FexHleDispatchSharedLockContractTests(unittest.TestCase):
    def test_runtime_gate_is_exact_sm8850_a840_with_same_binary_control(self):
        gate = RUNTIME.split("preserve_hle_dispatch_exclusive_lock_marker", 1)[1].split(
            "const bool maxinst_512_requested", 1
        )[0]
        for required in (
            'run-preserve-fex-hle-dispatch-exclusive-lock',
            'g_guest_cpu_backend_hardware_profile == "adreno-840"',
            "Common::IsSnapdragon8Gen5Host()",
            'setenv("EXECUTOR_FEX_HLE_DISPATCH_SHARED_LOCK", "1", 1)',
            'unsetenv("EXECUTOR_FEX_HLE_DISPATCH_SHARED_LOCK")',
            "scope=sm8850-a840",
            "signalMutators=exclusive",
        ):
            self.assertIn(required, gate)
        self.assertNotIn("g_guest_cpu_backend_launch_title_id", gate)
        self.assertNotIn("CUSA03173", gate)

    def test_only_owned_state_readers_share_while_signal_mutators_are_exclusive(self):
        self.assertIn("static std::shared_mutex g_mono_suspend_pending_signal_lock", MUTEX)
        helper = MUTEX.split("WithFexHleDispatchStateLock", 1)[1].split(
            "static std::mutex g_mono_suspend_abba_resume_lock", 1
        )[0]
        self.assertIn("std::shared_lock lock(g_mono_suspend_pending_signal_lock)", helper)
        self.assertIn("std::scoped_lock lock(g_mono_suspend_pending_signal_lock)", helper)

        polling = MUTEX.split("ExecutorAlertableMutexPollingModeFor", 1)[1].split(
            "RunPendingMonoSuspendSigusr1AtSafePoint", 1
        )[0]
        self.assertIn("WithFexHleDispatchStateLock", polling)
        enter = MUTEX.split("executor_live_mono_fex_hle_dispatch_enter", 1)[1].split(
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread", 1
        )[0]
        leave = MUTEX.split("executor_live_mono_fex_hle_dispatch_leave", 1)[1].split(
            "RecordMutexWaitSnapshot", 1
        )[0]
        self.assertIn("WithFexHleDispatchStateLock", enter)
        self.assertIn("WithFexHleDispatchStateLock", leave)

        mark = MUTEX.split("static void MarkMonoSuspendSigusr1Pending", 1)[1].split(
            "executor_live_mono_mark_pending_sigusr1_for_thread", 1
        )[0]
        sender = MUTEX.split(
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread", 1
        )[1].split("executor_live_mono_fex_hle_dispatch_leave", 1)[0]
        self.assertIn("std::scoped_lock lock(g_mono_suspend_pending_signal_lock)", mark)
        self.assertIn("std::scoped_lock lock(g_mono_suspend_pending_signal_lock)", sender)
        self.assertNotIn("std::shared_lock", mark)
        self.assertNotIn("std::shared_lock", sender)

    def test_a840_target_owned_state_lock_avoids_cross_thread_serialization(self):
        gate = RUNTIME.split("preserve_hle_dispatch_exclusive_lock_marker", 1)[1].split(
            "const bool maxinst_512_requested", 1
        )[0]
        for required in (
            'run-preserve-fex-hle-global-state-lock',
            'setenv("EXECUTOR_FEX_HLE_PER_THREAD_STATE_LOCK", "1", 1)',
            'unsetenv("EXECUTOR_FEX_HLE_PER_THREAD_STATE_LOCK")',
            "hle_dispatch_shared_lock_enabled && !preserve_hle_global_state_lock",
            "scope=sm8850-a840",
            "setMutation=global-then-target",
        ):
            self.assertIn(required, gate)
        self.assertNotIn("g_guest_cpu_backend_launch_title_id", gate)
        self.assertIn("std::mutex fex_hle_state_lock;", PTHREAD)

        helper = MUTEX.split("WithFexHleDispatchStateLock", 1)[1].split(
            "static std::mutex g_mono_suspend_abba_resume_lock", 1
        )[0]
        self.assertIn("std::scoped_lock lock(target->fex_hle_state_lock)", helper)
        self.assertIn("std::shared_lock lock(g_mono_suspend_pending_signal_lock)", helper)

        signal_lock = MUTEX.split("LockFexHleTargetStateForSignal", 1)[1].split(
            "HasMonoSuspendSigusr1PendingLocked", 1
        )[0]
        self.assertIn("std::defer_lock", signal_lock)
        self.assertIn("state_lock.lock()", signal_lock)




        mark = MUTEX.split("static void MarkMonoSuspendSigusr1Pending", 1)[1].split(
            "executor_live_mono_mark_pending_sigusr1_for_thread", 1
        )[0]
        self.assertLess(
            mark.index("g_mono_suspend_pending_signal_lock"),
            mark.index("LockFexHleTargetStateForSignal"),
        )

    def test_a840_pending_mirror_removes_only_hot_membership_lookup(self):
        gate = RUNTIME.split("preserve_hle_dispatch_exclusive_lock_marker", 1)[1].split(
            "const bool maxinst_512_requested", 1
        )[0]
        for required in (
            'run-preserve-fex-hle-pending-set-lookup',
            'setenv("EXECUTOR_FEX_HLE_PENDING_MIRROR", "1", 1)',
            'unsetenv("EXECUTOR_FEX_HLE_PENDING_MIRROR")',
            "hle_dispatch_shared_lock_enabled && !preserve_hle_pending_set_lookup",
        ):
            self.assertIn(required, gate)
        self.assertIn("bool fex_hle_sigusr1_pending{};", PTHREAD)

        enter = MUTEX.split("executor_live_mono_fex_hle_dispatch_enter", 1)[1].split(
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread", 1
        )[0]
        leave = MUTEX.split("executor_live_mono_fex_hle_dispatch_leave", 1)[1].split(
            "RecordMutexWaitSnapshot", 1
        )[0]
        self.assertIn("HasMonoSuspendSigusr1PendingLocked(target)", enter)
        self.assertIn("HasMonoSuspendSigusr1PendingLocked(target)", leave)
        self.assertNotIn("g_mono_suspend_pending_sigusr1.contains(target)", enter)
        self.assertNotIn("g_mono_suspend_pending_sigusr1.contains(target)", leave)




        self.assertEqual(MUTEX.count("g_mono_suspend_pending_sigusr1.insert(target)"), 2)
        self.assertEqual(MUTEX.count("SetFexHleSigusr1PendingLocked(target);"), 2)
        self.assertEqual(MUTEX.count("ClearFexHleSigusr1PendingLocked(target);"), 4)

    def test_a840_atomic_state_is_title_agnostic_and_same_binary_reversible(self):
        gate = RUNTIME.split("preserve_hle_dispatch_exclusive_lock_marker", 1)[1].split(
            "const bool maxinst_512_requested", 1
        )[0]
        for required in (
            'run-preserve-fex-hle-target-mutex',
            'setenv("EXECUTOR_FEX_HLE_ATOMIC_STATE", "1", 1)',
            'unsetenv("EXECUTOR_FEX_HLE_ATOMIC_STATE")',
            "hle_per_thread_state_lock_enabled && !preserve_hle_target_mutex",
            "scope=sm8850-a840",
            "canonicalSet=exclusive",
        ):
            self.assertIn(required, gate)
        self.assertNotIn("g_guest_cpu_backend_launch_title_id", gate)
        self.assertNotIn("CUSA03173", gate)

        self.assertIn("std::atomic<u32> fex_hle_atomic_depth{0};", PTHREAD)
        self.assertIn("std::atomic<u32> fex_hle_atomic_flags{0};", PTHREAD)
        self.assertIn("alignas(4)", PTHREAD)
        atomic_contract = MUTEX.split("ExecutorFexHleAtomicStateEnabled", 1)[1].split(
            "static std::mutex g_mono_suspend_abba_resume_lock", 1
        )[0]
        for required in (
            "FexHleSigusr1PendingBit",
            "FexHleSigusr1DeliveryBit",
            "std::atomic<u32>::is_always_lock_free",
            "fetch_or(FexHleSigusr1PendingBit",
            "fetch_and(~FexHleSigusr1PendingBit",
        ):
            self.assertIn(required, atomic_contract)

        enter = MUTEX.split("executor_live_mono_fex_hle_dispatch_enter", 1)[1].split(
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread", 1
        )[0]
        leave = MUTEX.split("executor_live_mono_fex_hle_dispatch_leave", 1)[1].split(
            "RecordMutexWaitSnapshot", 1
        )[0]
        for hot_path in (enter, leave):
            self.assertIn("compare_exchange_weak", hot_path)
            self.assertIn("std::memory_order_acq_rel", hot_path)
        self.assertIn("fex_hle_atomic_depth.fetch_add", enter)
        self.assertIn("flags | FexHleSigusr1DeliveryBit", enter)
        self.assertIn("flags | FexHleSigusr1DeliveryBit", leave)
        self.assertIn("fex_hle_atomic_depth.fetch_sub", leave)
        self.assertIn("Finish never clears Pending", leave)

        sender = MUTEX.split(
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread", 1
        )[1].split("executor_live_mono_fex_hle_dispatch_leave", 1)[0]
        self.assertLess(
            sender.index("g_mono_suspend_pending_sigusr1.insert(target)"),
            sender.index("SetFexHleSigusr1PendingLocked(target)"),
        )
        self.assertIn("std::scoped_lock lock(g_mono_suspend_pending_signal_lock)", sender)


if __name__ == "__main__":
    unittest.main()
