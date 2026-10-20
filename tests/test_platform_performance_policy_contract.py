import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROLLER = (
    ROOT
    / "android-app"
    / "app"
    / "src"
    / "main"
    / "java"
    / "app"
    / "lsx4"
    / "android"
    / "PlatformPerformanceController.java"
).read_text(encoding="utf-8")


def method_body(signature: str, next_signature: str) -> str:
    return CONTROLLER.split(signature, 1)[1].split(next_signature, 1)[0]


class PlatformPerformancePolicyContractTests(unittest.TestCase):
    def test_default_restores_early_4241_and_moderate_is_diagnostic_only(self):
        self.assertIn('"run-use-vivo-performance-hint-4241"', CONTROLLER)
        self.assertIn('"run-use-vivo-moderate-cpu-floor"', CONTROLLER)
        self.assertIn('"run-disable-vivo-moderate-cpu-floor"', CONTROLLER)
        self.assertRegex(
            CONTROLLER,
            r"enum VivoMode\s*\{\s*OFF\(\"off\"\),\s*"
            r"MODERATE_CPU_FLOOR\(\"moderate-cpu-floor\"\),\s*"
            r"PERFORMANCE_HINT_4241\(\"performance-hint-4241\"\)",
        )

        desired = method_body(
            "private static VivoMode readDesiredVivoMode(Context context)",
            "private static void scheduleVivoReconciliation",
        )
        disabled = desired.index("DISABLE_VIVO_CPU_FLOOR_MARKER")
        moderate = desired.index("USE_VIVO_MODERATE_CPU_FLOOR_MARKER")
        legacy_hint = desired.index("USE_VIVO_PERFORMANCE_HINT_MARKER")
        default = desired.rindex("return VivoMode.PERFORMANCE_HINT_4241")
        self.assertLess(disabled, moderate)
        self.assertLess(moderate, legacy_hint)
        self.assertLess(legacy_hint, default)
        self.assertIn("return VivoMode.OFF", desired[disabled:moderate])
        self.assertIn(
            "return VivoMode.MODERATE_CPU_FLOOR", desired[moderate:legacy_hint]
        )
        self.assertIn(
            "return VivoMode.PERFORMANCE_HINT_4241", desired[legacy_hint:default]
        )

    def test_moderate_policy_is_the_measured_cpu_floor_only(self):
        initializer = re.search(
            r"VIVO_CPU_FLOOR_RESOURCES\s*=\s*\{(?P<body>.*?)\};",
            CONTROLLER,
            re.DOTALL,
        )
        self.assertIsNotNone(initializer)
        values = [
            int(token, 0)
            for token in re.findall(
                r"0x[0-9a-fA-F]+|\b\d+\b", initializer.group("body")
            )
        ]
        self.assertEqual(
            values,
            [0x40800000, 2000, 0x40800100, 2000, 0x40800200, 2400],
        )
        self.assertNotIn("0xf0100000", CONTROLLER.lower())
        self.assertNotIn("0x40804000", CONTROLLER.lower())
        self.assertNotIn("0x40804100", CONTROLLER.lower())
        self.assertNotIn("0x40804200", CONTROLLER.lower())

    def test_binder_abis_are_exact_and_release_is_transaction_two(self):
        self.assertIn("VIVO_HINT_DURATION_MS = 30_000", CONTROLLER)
        self.assertIn("VIVO_PERFORMANCE_HINT = 4241", CONTROLLER)
        self.assertIn("VIVO_PERFORMANCE_HINT_USER_DATA_2 = 2", CONTROLLER)

        generic = method_body(
            "private static int invokeVivoPerformanceHint(Context context)",
            "private static int invokeVivoCpuFloorLock()",
        )
        ordered_generic_arguments = [
            '"vendor.vivoperfservice", "3"',
            '"i32", Integer.toString(VIVO_PERFORMANCE_HINT)',
            '"s16", context.getPackageName()',
            '"i32", Integer.toString(VIVO_HINT_DURATION_MS)',
            '"i32", Integer.toString(VIVO_PERFORMANCE_HINT_USER_DATA_2)',
            '"i32", Integer.toString(android.os.Process.myTid())',
        ]
        positions = [generic.index(argument) for argument in ordered_generic_arguments]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("});", generic)

        moderate = method_body(
            "private static int invokeVivoCpuFloorLock()",
            "private static int invokeVivoServiceRelease",
        )
        self.assertIn('command[index++] = "4";', moderate)
        self.assertIn(
            "Integer.toString(VIVO_CPU_FLOOR_RESOURCES.length)", moderate
        )
        self.assertIn("for (int value : VIVO_CPU_FLOOR_RESOURCES)", moderate)
        self.assertIn("return invokeVivoService(command);", moderate)

        release = method_body(
            "private static int invokeVivoServiceRelease(int handle)",
            "private static int invokeVivoService(String[] command)",
        )
        self.assertIn('"vendor.vivoperfservice", "2"', release)
        self.assertIn('"i32", Integer.toString(handle)', release)

        parser = method_body(
            "private static int invokeVivoService(String[] command)",
            "private static String readProcessOutput",
        )
        self.assertIn("long exception = Long.parseLong", parser)
        self.assertIn("if (exception != 0)", parser)
        self.assertGreaterEqual(parser.count("if (!matcher.find())"), 2)
        self.assertIn(
            "return (int) Long.parseLong(matcher.group(1), 16)", parser
        )

    def test_immutable_lease_owns_release_transport_and_handle(self):
        lease = CONTROLLER.split("private static final class VivoLease", 1)[1].split(
            "private static boolean active", 1
        )[0]
        for declaration in (
            "final int handle;",
            "final VivoMode mode;",
            "final Object manager;",
            "final Method releaseMethod;",
            "final String transport;",
            "final long expiresAtNanos;",
        ):
            self.assertIn(declaration, lease)

        self.assertIn("private static VivoLease vivoLease;", CONTROLLER)
        self.assertNotIn("private static Object vivoPerfManager", CONTROLLER)
        self.assertNotIn("private static Method vivoReleaseMethod", CONTROLLER)
        self.assertNotIn("private static int vivoPerfHandle", CONTROLLER)

        release = method_body(
            "private static boolean releaseVivoLease(VivoLease lease)",
            "private static void scheduleDetachedReleaseRetry",
        )
        self.assertIn("lease.releaseMethod.invoke(lease.manager, lease.handle)", release)
        self.assertIn("invokeVivoServiceRelease(lease.handle)", release)
        self.assertIn("lease.transport", release)
        self.assertIn("if (releaseResult >= 0)", release)
        self.assertIn("if (result >= 0)", release)
        self.assertIn('result=release-failed handle=', release)

        retry = method_body(
            "private static void scheduleDetachedReleaseRetry",
            "private static int invokeVivoPerformanceHint",
        )
        self.assertIn("VIVO_RELEASE_RETRY_LIMIT", retry)
        self.assertIn("ROOT_EXECUTOR.schedule", retry)
        self.assertIn("if (releaseVivoLease(lease))", retry)
        self.assertIn("} else {", retry)

    def test_pending_releases_block_every_new_lease_until_release_or_ttl(self):
        self.assertIn(
            "private static final Map<Integer, VivoLease> PENDING_VIVO_RELEASES",
            CONTROLLER,
        )
        blocker = method_body(
            "private static boolean blockAcquireForPendingReleases",
            "private static void pruneExpiredPendingReleasesLocked",
        )
        self.assertIn("pruneExpiredPendingReleasesLocked()", blocker)
        self.assertIn("PENDING_VIVO_RELEASES.size()", blocker)
        self.assertIn('result=acquire-blocked', blocker)
        self.assertIn("scheduleVivoReconciliation", blocker)

        retry = method_body(
            "private static void scheduleDetachedReleaseRetry",
            "private static int invokeVivoPerformanceHint",
        )
        registered = retry.index("PENDING_VIVO_RELEASES.put")
        scheduled = retry.index("ROOT_EXECUTOR.schedule")
        removed = retry.index("PENDING_VIVO_RELEASES.remove")
        self.assertLess(registered, scheduled)
        self.assertLess(scheduled, removed)

        renew = method_body(
            "private static void renewVivoLease(Context context, long generation,",
            "private static void switchVivoLease",
        )
        self.assertLess(
            renew.index("blockAcquireForPendingReleases"),
            renew.index("VivoLease replacement = acquireVivoLease"),
        )
        switch = method_body(
            "private static void switchVivoLease(Context context, long generation,",
            "private static boolean commitVivoLease",
        )
        self.assertLess(
            switch.index("blockAcquireForPendingReleases"),
            switch.index("VivoLease acquired = acquireVivoLease(context, desired)"),
        )
        self.assertIn("!PENDING_VIVO_RELEASES.isEmpty()", CONTROLLER)

    def test_same_mode_renewal_acquires_swaps_then_releases(self):
        renew = method_body(
            "private static void renewVivoLease(Context context, long generation,",
            "private static void switchVivoLease",
        )
        acquire = renew.index("VivoLease replacement = acquireVivoLease")
        swap = renew.index("vivoLease = replacement")
        release = renew.index("releaseVivoLease(oldLease)")
        self.assertLess(acquire, swap)
        self.assertLess(swap, release)
        self.assertIn("vivoLease == oldLease", renew)
        self.assertIn("observedDesired == desired", renew)
        self.assertIn("VIVO_RENEWAL_DELAY_SECONDS", renew)

        failed = renew[
            renew.index("if (replacement == null)") :
            renew.index("VivoMode observedDesired")
        ]
        self.assertNotIn("vivoLease = null", failed)
        self.assertIn("VIVO_RETRY_DELAY_SECONDS", failed)
        self.assertIn('"renewal-failed"', failed)

    def test_cross_mode_switch_releases_before_acquire_and_falls_back(self):
        switch = method_body(
            "private static void switchVivoLease(Context context, long generation,",
            "private static boolean commitVivoLease",
        )
        detach = switch.index("vivoLease = null")
        release = switch.index("releaseVivoLease(oldLease)")
        acquire = switch.index("VivoLease acquired = acquireVivoLease(context, desired)")
        self.assertLess(detach, release)
        self.assertLess(release, acquire)
        self.assertIn("vivoDesiredMode == desired", switch[:detach])
        self.assertIn("if (!releaseVivoLease(oldLease))", switch[:acquire])
        self.assertIn("restoreLeaseAfterFailedRelease", switch[:acquire])
        self.assertIn("return;", switch[release:acquire])

        fallback_order = switch.index("order=acquire-fallback")
        fallback_acquire = switch.index(
            "acquireVivoLease(context, VivoMode.MODERATE_CPU_FLOOR)"
        )
        fallback_commit = switch.index("order=commit-fallback")
        self.assertLess(acquire, fallback_order)
        self.assertLess(fallback_order, fallback_acquire)
        self.assertLess(fallback_acquire, fallback_commit)
        self.assertIn("VIVO_RETRY_DELAY_SECONDS", switch)

        commit = method_body(
            "private static boolean commitVivoLease(Context context, long generation,",
            "private static boolean isVivoTransitionCurrent",
        )
        for guard in (
            "!active",
            "generation != vivoGeneration",
            "vivoDesiredMode != requested",
            "observedDesired != requested",
            "vivoLease != null",
        ):
            self.assertIn(guard, commit)
        self.assertIn("fallback ? VIVO_RETRY_DELAY_SECONDS", commit)

    def test_off_marker_keeps_reconciliation_alive(self):
        off = method_body(
            "private static void detachAndReleaseForOff",
            "private static void renewVivoLease",
        )
        self.assertIn("vivoLease = null", off)
        self.assertIn("releaseVivoLease(current)", off)
        self.assertIn("scheduleVivoReconciliation", off)
        self.assertIn("VIVO_RETRY_DELAY_SECONDS", off)
        self.assertIn('"policy-off-marker"', off)

        schedule = method_body(
            "private static void scheduleVivoReconciliationLocked",
            "private static VivoLease acquireVivoLease",
        )
        self.assertIn("ROOT_EXECUTOR.schedule", schedule)
        self.assertIn("active", schedule)
        self.assertIn("generation != vivoGeneration", schedule)

    def test_stop_detaches_and_cancels_before_async_release(self):
        stop = method_body(
            "static void stop(Context context)",
            "private static void reconcileVivoPerformanceMode",
        )
        synchronized_end = stop.index("reportGameState(context, false, false)")
        synchronous = stop[:synchronized_end]
        asynchronous = stop[synchronized_end:]
        self.assertIn("++vivoGeneration", synchronous)
        self.assertIn("detachedLease = vivoLease", synchronous)
        self.assertIn("vivoLease = null", synchronous)
        self.assertIn("vivoReconciliation.cancel(false)", synchronous)
        self.assertIn("vivoReconciliation = null", synchronous)
        self.assertNotIn("releaseVivoLease", synchronous)
        self.assertIn("ROOT_EXECUTOR.execute", asynchronous)
        self.assertIn("releaseVivoLease(detachedLease)", asynchronous)

        start = method_body(
            "static void start(Context context, boolean gameSurface)",
            "static void markGameplay",
        )
        self.assertIn("generation = ++vivoGeneration", start)
        self.assertIn("ROOT_EXECUTOR.execute", start)
        self.assertIn("reconcileVivoPerformanceMode", start)


if __name__ == "__main__":
    unittest.main()
