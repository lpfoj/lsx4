import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
PTHREAD = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "threads"
    / "pthread.cpp"
).read_text(encoding="utf-8")
LOADER = (ROOT / "src" / "executor" / "fex_backend_loader.cpp").read_text(
    encoding="utf-8"
)


def section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class FexThreadReleaseContractTests(unittest.TestCase):
    def test_native_export_is_inactive_noop_and_calls_required_v3_slot(self):
        release = section(
            NATIVE,
            "executor_lsx4_android_release_guest_thread",
            'extern "C" void* executor_lsx4_android_run_guest_thread',
        )
        inactive = release.index("if (!RuntimeGuestFexActive())")
        active_api = release.index("Executor::Fex::ActiveApi()")
        backend_call = release.index("api->release_guest_thread")
        self.assertLess(inactive, active_api)
        self.assertLess(active_api, backend_call)
        self.assertIn("return LSX4_FEX_STATUS_OK", release[:active_api])
        self.assertIn("api == nullptr || api->release_guest_thread == nullptr", release)

        required = section(LOADER, "bool HasRequiredFunctions", "Lsx4FexCpuidResultV6")
        self.assertIn("api.release_guest_thread != nullptr", required)

    def test_release_identity_is_stable_and_retry_safe(self):
        release = section(
            NATIVE,
            "executor_lsx4_android_release_guest_thread",
            'extern "C" void* executor_lsx4_android_run_guest_thread',
        )
        self.assertIn("MakeFexThreadIdentity(guest_thread, pthread_handle)", release)
        self.assertIn("identity.host_tid = 0", release)
        self.assertIn("identity.pthread_handle", release)
        self.assertIn("return status", release)

    def test_finalize_releases_after_destructors_and_before_collection_or_tid_recycle(self):
        prepare = section(PTHREAD, "static void PrepareThreadExit", "static Pthread* FinalizeThreadState")
        finalize = section(PTHREAD, "static Pthread* FinalizeThreadState", "static void ExitThread")
        self.assertIn("RunRegisteredThreadDtors()", prepare)

        tls_cleanup = finalize.index("_thread_cleanupspecific()")
        release = finalize.index("executor_lsx4_android_release_guest_thread(")
        collect = finalize.index("thread_state->TryCollect(curthread)")
        tid_recycle = finalize.index("curthread->tid.store(TidTerminated)")
        self.assertLess(tls_cleanup, release)
        self.assertLess(release, collect)
        self.assertLess(release, tid_recycle)

        normal_exit = section(PTHREAD, "void PS4_SYSV_ABI posix_pthread_exit", "static int JoinThread")
        self.assertLess(normal_exit.index("PrepareThreadExit"), normal_exit.index("ExitThread()"))

    def test_release_failure_quarantines_identity_without_crashing_joiners(self):
        finalize = section(PTHREAD, "static Pthread* FinalizeThreadState", "static void ExitThread")
        failure = finalize.index("if (fex_release_status != 0)")
        quarantine = finalize.index("quarantine_fex_identity = true", failure)
        ref_hold = finalize.index(
            "curthread->refcount = std::max(curthread->refcount, 1)", quarantine
        )
        collect = finalize.index("thread_state->TryCollect(curthread)", ref_hold)
        terminated = finalize.index("curthread->tid.store(TidTerminated)", collect)
        self.assertLess(failure, quarantine)
        self.assertLess(quarantine, ref_hold)
        self.assertLess(ref_hold, collect)
        self.assertLess(collect, terminated)
        self.assertIn("action=retain_identity_no_recycle", finalize)
        self.assertNotIn("std::abort", finalize)
        self.assertNotIn("UNREACHABLE", finalize)

    def test_guest_exit_staging_uses_immutable_registered_wrapper_identity(self):
        metadata = section(
            NATIVE,
            "ExecutorHleTargetMetadata BuildExecutorHleTargetMetadata",
            "const ExecutorHleTargetMetadata& GetExecutorHleTargetMetadata",
        )
        wrapper = metadata.index(
            "HOST_CALL(Libraries::Kernel::posix_pthread_exit)"
        )
        classify = metadata.index("native_function == pthread_exit_wrapper")
        nid_fallback = metadata.index(
            'HleNameMatchesAnyNid(name, {"FJrT5LuUBAU", "3kg7rT0NQIs"})'
        )
        self.assertLess(wrapper, classify)
        self.assertLess(classify, nid_fallback)
        self.assertNotIn("GetAndroidX64HleNameForNative", metadata[classify:nid_fallback])


if __name__ == "__main__":
    unittest.main()
