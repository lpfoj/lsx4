import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
EXCEPTION = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "threads"
    / "exception.cpp"
).read_text(encoding="utf-8")
MUTEX = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "threads"
    / "mutex.cpp"
).read_text(encoding="utf-8")
PTHREAD = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "threads"
    / "pthread.h"
).read_text(encoding="utf-8")
SEMAPHORE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "threads"
    / "semaphore.cpp"
).read_text(encoding="utf-8")


def section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class FexHleMonoSafepointContractTests(unittest.TestCase):
    def test_only_committed_fex_publishes_target_hle_state(self):
        scoped = section(
            NATIVE,
            "class ScopedFexHleDispatchDepth",
            "class ScopedFexBulkLeaseAbort",
        )
        for proof in (
            "g_fex_hle_signal_safepoints_required.load(std::memory_order_acquire)",
            "RuntimeGuestFexActive()",
            "FaultSignalOwner::Fex",
            "Executor::Fex::ActiveApiForSignal() != nullptr",
            "Libraries::Kernel::Pthread* const current_guest_thread",
            "depth(&g_fex_hle_dispatch_depth)",
            "executor_live_mono_fex_hle_dispatch_enter(current_guest_thread)",
        ):
            self.assertIn(proof, scoped)
        self.assertLess(
            scoped.index("g_fex_hle_signal_safepoints_required.load"),
            scoped.index("RuntimeGuestFexActive()"),
        )
        self.assertLess(
            scoped.index("executor_live_mono_fex_hle_dispatch_leave"),
            scoped.index("Lsx4::FexHleLease::LeaveDepth"),
        )
        self.assertIn("Lsx4::FexHleLease::LeaveDepth(*depth)", scoped)


        self.assertEqual(
            NATIVE.count("ScopedFexHleDispatchDepth dispatch_depth(current_guest_thread)"),
            4,
        )

    def test_mono_capability_is_armed_before_media_module_initializers(self):
        media = section(
            NATIVE,
            "int LoadAndroidAppMediaModulesForLive",
            "bool WriteTextFile",
        )
        arm = media.index('RequireFexHleSignalSafepoints("media-module-scan"')
        load_loop = media.index("for (const auto& path : candidates)", arm)
        self.assertLess(arm, load_loop)
        self.assertIn("IsMonoPs4ModulePath(path)", media[:load_loop])

    def test_dynamic_mono_module_and_sigusr1_registration_arm_capability(self):
        module_registration = section(
            NATIVE,
            "int RegisterBox64ModuleSegments(void* module_ptr) {",
            "ExecutorBox64MappedEntryRequest request{}",
        )
        self.assertIn("IsMonoPs4ModulePath(module->file)", module_registration)
        self.assertIn(
            'RequireFexHleSignalSafepoints("module-registration"',
            module_registration,
        )

        signal_registration = section(
            NATIVE,
            "executor_lsx4_android_register_guest_signal_handler",
            "executor_lsx4_android_queue_guest_signal",
        )
        self.assertIn("status == LSX4_FEX_STATUS_OK", signal_registration)
        self.assertIn("signum == OrbisSigUsr1", signal_registration)
        self.assertIn("handler > 1", signal_registration)
        self.assertIn(
            'RequireFexHleSignalSafepoints("signal-registration")',
            signal_registration,
        )
        self.assertIn(
            'RequireFexHleSignalSafepoints("signal-action-v3-registration")',
            signal_registration,
        )

    def test_committed_fex_mono_kernel_route_precedes_backend_enqueue(self):
        pthread_kill = section(
            EXCEPTION,
            "s32 PS4_SYSV_ABI posix_pthread_kill",
            "static constexpr std::array<int, 33> signals",
        )
        route = pthread_kill.index("const bool committed_fex_mono_kernel_pending")
        synthetic = pthread_kill.index("const bool allow_synthetic_mono_ack")
        enqueue = pthread_kill.index("queue_guest_signal_enter")
        self.assertLess(route, synthetic)
        self.assertLess(route, enqueue)
        route_body = pthread_kill[route:synthetic]
        self.assertIn("ExecutorFexSignalApiActive()", route_body)
        self.assertIn("HandlerKinds[sig] == 3", route_body)
        self.assertIn(
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread(thread) == 1",
            route_body,
        )
        self.assertIn("return ORBIS_OK", route_body)
        self.assertNotIn("named_mono_thread &&", route_body)
        self.assertNotIn("synthetic_suspend_ack", route_body)
        self.assertNotIn("queue_guest_signal", route_body)

    def test_sender_mark_and_both_dispatch_state_paths_are_linearized(self):
        enter = section(
            MUTEX,
            "executor_live_mono_fex_hle_dispatch_enter",
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread",
        )
        mark = section(
            MUTEX,
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread",
            'extern "C" int executor_live_mono_fex_hle_dispatch_leave',
        )
        leave = section(
            MUTEX,
            'extern "C" int executor_live_mono_fex_hle_dispatch_leave',
            "static void RecordMutexWaitSnapshot",
        )
        self.assertIn("g_mono_suspend_pending_signal_lock", mark)
        self.assertIn("target->fex_hle_dispatch_depth", mark)
        self.assertIn("target->fex_hle_atomic_depth.load", mark)
        self.assertIn("target->fex_hle_atomic_flags.load", mark)
        self.assertIn("g_mono_suspend_pending_sigusr1.insert(target)", mark)
        self.assertIn("fex_hle_sigusr1_delivery_in_progress", mark)
        self.assertNotIn("dispatch_depth == 0", mark)

        self.assertIn("WithFexHleDispatchStateLock(target", enter)
        self.assertIn("fex_hle_atomic_depth.fetch_add", enter)
        self.assertIn("fex_hle_atomic_flags.compare_exchange_weak", enter)
        increment = enter.index("++target->fex_hle_dispatch_depth")
        pending = enter.index("HasMonoSuspendSigusr1PendingLocked(target)")
        safe_point = enter.index("RunPendingMonoSuspendSigusr1AtSafePoint")
        self.assertLess(increment, pending)
        self.assertLess(pending, safe_point)
        self.assertIn("fex_hle_sigusr1_delivery_in_progress = true", enter)

        self.assertIn("WithFexHleDispatchStateLock(target", leave)
        self.assertIn("fex_hle_atomic_depth.compare_exchange_weak", leave)
        self.assertIn("fex_hle_atomic_flags.compare_exchange_weak", leave)
        pending = leave.index("HasMonoSuspendSigusr1PendingLocked(target)")
        zero = leave.index("target->fex_hle_dispatch_depth = 0")
        safe_point = leave.index("RunPendingMonoSuspendSigusr1AtSafePoint")
        final_decrement = leave.index("--target->fex_hle_dispatch_depth", safe_point)
        self.assertLess(pending, zero)
        self.assertLess(pending, safe_point)
        self.assertLess(safe_point, final_decrement)
        self.assertIn("target->fex_hle_dispatch_depth > 1", leave)

    def test_delivery_preserves_one_followup_standard_signal(self):
        self.assertIn("u32 fex_hle_dispatch_depth{}", PTHREAD)
        self.assertIn("bool fex_hle_sigusr1_pending{}", PTHREAD)
        self.assertIn("bool fex_hle_sigusr1_delivery_in_progress{}", PTHREAD)
        mark = section(
            MUTEX,
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread",
            'extern "C" int executor_live_mono_fex_hle_dispatch_leave',
        )
        self.assertIn("delivery_in_progress", mark)
        self.assertIn("coalesced_with_pending = !inserted", mark)
        self.assertIn("SetFexHleSigusr1PendingLocked(target)", mark)
        self.assertNotIn("if (!coalesced_with_delivery)", mark)
        self.assertIn("return 1", mark)

    def test_entry_redrains_signal_published_while_handler_returns(self):
        enter = section(
            MUTEX,
            "executor_live_mono_fex_hle_dispatch_enter",
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread",
        )
        first_safe_point = enter.index("RunPendingMonoSuspendSigusr1AtSafePoint")
        clear_delivery = enter.index(
            "~FexHleSigusr1DeliveryBit", first_safe_point
        )
        recheck_pending = enter.index(
            "fex_hle_atomic_flags.load(std::memory_order_acquire)",
            clear_delivery,
        )
        reclaim_delivery = enter.index(
            "fex_hle_atomic_flags.compare_exchange_weak", recheck_pending
        )
        redrain = enter.index(
            "RunPendingMonoSuspendSigusr1AtSafePoint(target, nullptr)",
            reclaim_delivery,
        )
        self.assertLess(first_safe_point, clear_delivery)
        self.assertLess(clear_delivery, recheck_pending)
        self.assertLess(recheck_pending, reclaim_delivery)
        self.assertLess(reclaim_delivery, redrain)
        self.assertIn("redrain_pending_signal", enter)
        self.assertNotIn("CUSA", enter[first_safe_point:redrain])

    def test_every_safe_point_owns_delivery_across_guest_handler(self):
        raw = section(
            MUTEX,
            "RunConsumedMonoSuspendSigusr1AtSafePoint",
            "static MonoSignalSafePointResult RunPendingMonoSuspendSigusr1AtSafePoint",
        )
        owned = section(
            MUTEX,
            "static MonoSignalSafePointResult RunPendingMonoSuspendSigusr1AtSafePoint",
            'extern "C" void executor_live_mono_run_pending_signal_safe_point',
        )
        consume = owned.index("ConsumeMonoSuspendSigusr1PendingForCurrent")
        publish_delivery = owned.index("FexHleSigusr1DeliveryBit", consume)
        run_handler = owned.index(
            "RunConsumedMonoSuspendSigusr1AtSafePoint", publish_delivery
        )
        clear_delivery = owned.index("~FexHleSigusr1DeliveryBit", run_handler)
        followup = owned.index("followup_pending", clear_delivery)
        self.assertLess(consume, publish_delivery)
        self.assertLess(publish_delivery, run_handler)
        self.assertLess(run_handler, clear_delivery)
        self.assertLess(clear_delivery, followup)
        self.assertIn("fex_hle_sigusr1_delivery_in_progress = true", owned)
        self.assertIn("fex_hle_sigusr1_delivery_in_progress = false", owned)
        self.assertIn("RunConsumedMonoSuspendSigusr1AtSafePoint", owned)
        self.assertNotIn("ConsumeMonoSuspendSigusr1PendingForCurrent", raw)
        self.assertNotIn("CUSA", owned)

    def test_atomic_leave_redrains_signal_published_in_close_window(self):
        leave = section(
            MUTEX,
            'extern "C" int executor_live_mono_fex_hle_dispatch_leave',
            "static void RecordMutexWaitSnapshot",
        )
        close_window = leave.index(
            "sender may therefore publish a standard PS4 signal"
        )
        clear_delivery = leave.rindex(
            "~FexHleSigusr1DeliveryBit", 0, close_window
        )
        publish_zero = leave.index("fex_hle_atomic_depth.fetch_sub", clear_delivery)
        recheck_pending = leave.index(
            "fex_hle_atomic_flags.load(std::memory_order_acquire)", publish_zero
        )
        reclaim_zero = leave.index(
            "fex_hle_atomic_depth.compare_exchange_strong", recheck_pending
        )
        redrain = leave.index(
            "RunPendingMonoSuspendSigusr1AtSafePoint(target, nullptr)",
            reclaim_zero,
        )
        self.assertLess(clear_delivery, publish_zero)
        self.assertLess(publish_zero, recheck_pending)
        self.assertLess(recheck_pending, reclaim_zero)
        self.assertLess(reclaim_zero, redrain)
        self.assertIn("expected_zero, 1", leave[reclaim_zero:redrain])
        self.assertNotIn("CUSA", leave[close_window:redrain])

    def test_deferred_handler_waits_for_a_later_safe_point(self):
        enter = section(
            MUTEX,
            "executor_live_mono_fex_hle_dispatch_enter",
            "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread",
        )
        leave = section(
            MUTEX,
            'extern "C" int executor_live_mono_fex_hle_dispatch_leave',
            "static void RecordMutexWaitSnapshot",
        )

        enter_safe_point = enter.index("RunPendingMonoSuspendSigusr1AtSafePoint")
        enter_clear = enter.index("~FexHleSigusr1DeliveryBit", enter_safe_point)
        enter_gate = enter.index("if (safe_point_result !=", enter_clear)
        enter_recheck = enter.index("u32 flags =", enter_gate)
        self.assertLess(enter_clear, enter_gate)
        self.assertLess(enter_gate, enter_recheck)
        self.assertIn("same unsafe context", enter[enter_gate:enter_recheck])
        self.assertGreaterEqual(enter.count("safe_point_result !="), 2)

        leave_safe_point = leave.index("RunPendingMonoSuspendSigusr1AtSafePoint")
        leave_clear = leave.index("~FexHleSigusr1DeliveryBit", leave_safe_point)
        leave_depth_zero = leave.index("fex_hle_atomic_depth.fetch_sub", leave_clear)
        leave_gate = leave.index("if (safe_point_result !=", leave_depth_zero)
        leave_recheck = leave.index("u32 flags =", leave_gate)
        self.assertLess(leave_depth_zero, leave_gate)
        self.assertLess(leave_gate, leave_recheck)
        self.assertIn("next first HLE entry", leave[leave_gate:leave_recheck])
        self.assertGreaterEqual(leave.count("safe_point_result !="), 2)

    def test_pending_signal_wakes_generic_pthread_wait_without_binary_overpost(self):
        notify = section(
            SEMAPHORE,
            'extern "C" void executor_live_mono_notify_pending_signal_waiter',
            "#endif",
        )
        self.assertIn('target->PostWake("mono-pending-signal")', notify)
        self.assertIn("NotifyPendingSignal(target)", notify)
        self.assertIn("NotifyPthreadSemPendingSignalWaiter(target)", notify)

        post = section(PTHREAD, "void PostWake", "void ClearWake")
        self.assertIn("wake_sema_mutex", post)
        self.assertIn("if (wake_sema_posted)", post)
        self.assertIn("wake_sema_posted = true", post)
        self.assertIn("wake_sema.release()", post)

    def test_contended_fex_hle_mutex_is_target_thread_alertable(self):
        polling = section(
            MUTEX,
            "enum class ExecutorAlertableMutexPollingMode",
            "enum class MonoSignalSafePointResult",
        )
        self.assertIn("Pthread* const current_thread", polling)
        self.assertIn("g_mono_suspend_pending_signal_lock", polling)
        self.assertIn("current_thread->fex_hle_dispatch_depth != 0", polling)
        self.assertIn("ExecutorAlertableMutexPollingMode::CommittedFexHle", polling)
        self.assertIn("ExecutorAlertableMutexPollingMode::ExplicitDiagnostic", polling)

        mutex_wait = section(
            MUTEX,
            "s32 PthreadMutex::Lock",
            "s32 PthreadMutex::Unlock",
        )
        direct = mutex_wait.index(
            "if (alertable_mode == ExecutorAlertableMutexPollingMode::None)"
        )
        direct_lock = mutex_wait.index("m_lock.lock()", direct)
        fast_try = mutex_wait.index("else if (!m_lock.try_lock())", direct_lock)
        production = mutex_wait.index(
            "alertable_mode == ExecutorAlertableMutexPollingMode::CommittedFexHle",
            fast_try,
        )
        alertable = mutex_wait.index("while (!m_lock.try_lock_for", production)
        safe_point = mutex_wait.index(
            "RunPendingMonoSuspendSigusr1AtSafePoint(curthread, guest_cell)",
            alertable,
        )
        diagnostic = mutex_wait.index("const auto wait_start", safe_point)
        self.assertLess(direct, direct_lock)
        self.assertLess(direct_lock, fast_try)
        self.assertLess(fast_try, production)
        self.assertLess(production, alertable)
        self.assertLess(alertable, safe_point)
        self.assertLess(safe_point, diagnostic)

        production_loop = mutex_wait[production:diagnostic]
        for diagnostic_only in (
            "RecordMutexWaitSnapshot",
            "SignalMonoSuspendForUnityPreloadBlockingLock",
            "EXECUTOR_MUTEX_WAIT_STUCK",
            "std::this_thread::yield",
        ):
            self.assertNotIn(diagnostic_only, production_loop)

    def test_contract_is_backend_not_title_or_hardware_specific(self):
        seams = "\n".join(
            (
                section(
                    NATIVE,
                    "class ScopedFexHleDispatchDepth",
                    "class ScopedFexBulkLeaseAbort",
                ),
                section(
                    EXCEPTION,
                    "const bool committed_fex_mono_kernel_pending",
                    "const bool allow_synthetic_mono_ack",
                ),
                section(
                    MUTEX,
                    "executor_live_mono_try_mark_pending_sigusr1_for_fex_hle_thread",
                    "static void RecordMutexWaitSnapshot",
                ),
            )
        )
        for forbidden in (
            "CUSA",
            "SM8650",
            "A750",
            "A840",
            "Adreno",
            "Mali",
            "Snapdragon",
        ):
            self.assertNotIn(forbidden, seams)


if __name__ == "__main__":
    unittest.main()
