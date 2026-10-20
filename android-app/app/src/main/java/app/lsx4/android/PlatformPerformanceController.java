package app.lsx4.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class PlatformPerformanceController {
    private static final String TAG = "LSX4-PlatformPerf";
    private static final Object LOCK = new Object();
    private static final int VIVO_HINT_DURATION_MS = 30_000;
    private static final int VIVO_RENEWAL_DELAY_SECONDS = 20;
    private static final int VIVO_RETRY_DELAY_SECONDS = 2;
    private static final int VIVO_RELEASE_RETRY_LIMIT = 5;
    private static final int VIVO_PERFORMANCE_HINT = 4241;



    private static final int VIVO_PERFORMANCE_HINT_USER_DATA_2 = 2;
    private static final String USE_VIVO_PERFORMANCE_HINT_MARKER =
            "run-use-vivo-performance-hint-4241";
    private static final String USE_VIVO_MODERATE_CPU_FLOOR_MARKER =
            "run-use-vivo-moderate-cpu-floor";
    private static final String DISABLE_VIVO_CPU_FLOOR_MARKER =
            "run-disable-vivo-moderate-cpu-floor";




    private static final int[] VIVO_CPU_FLOOR_RESOURCES = {
            0x40800000, 2000,
            0x40800100, 2000,
            0x40800200, 2400
    };
    private static final Pattern PARCEL_WORD = Pattern.compile("\\b([0-9a-fA-F]{8})\\b");
    private static final ScheduledExecutorService ROOT_EXECUTOR =
            Executors.newSingleThreadScheduledExecutor(task -> {
        Thread thread = new Thread(task, "lsx4-root-perf");
        thread.setDaemon(true);
        return thread;
    });
    private static final Map<String, String> ORIGINAL_VALUES = new LinkedHashMap<>();

    private enum VivoMode {
        OFF("off"),
        MODERATE_CPU_FLOOR("moderate-cpu-floor"),
        PERFORMANCE_HINT_4241("performance-hint-4241");

        final String logName;

        VivoMode(String logName) {
            this.logName = logName;
        }
    }






    private static final class VivoLease {
        final int handle;
        final VivoMode mode;
        final Object manager;
        final Method releaseMethod;
        final String transport;
        final long expiresAtNanos;

        VivoLease(int handle, VivoMode mode, Object manager, Method releaseMethod,
                String transport) {
            this.handle = handle;
            this.mode = mode;
            this.manager = manager;
            this.releaseMethod = releaseMethod;
            this.transport = transport;
            this.expiresAtNanos = System.nanoTime()
                    + TimeUnit.MILLISECONDS.toNanos(VIVO_HINT_DURATION_MS);
        }
    }

    private static boolean active;
    private static boolean gameplayReported;
    private static boolean rootApplied;
    private static long vivoGeneration;
    private static VivoMode vivoDesiredMode = VivoMode.OFF;
    private static VivoLease vivoLease;
    private static final Map<Integer, VivoLease> PENDING_VIVO_RELEASES =
            new LinkedHashMap<>();
    private static ScheduledFuture<?> vivoReconciliation;

    private PlatformPerformanceController() {
    }

    static void start(Context context, boolean gameSurface) {
        start(context, gameSurface, ProcessSafePreferences.get(context));
    }

    static void start(Context context, boolean gameSurface,
                      SharedPreferences preferences) {
        if (!gameSurface) {
            return;
        }
        long generation;
        synchronized (LOCK) {
            if (active) {
                return;
            }
            active = true;
            gameplayReported = false;
            generation = ++vivoGeneration;
        }
        reportGameState(context, true, true);
        Context applicationContext = context.getApplicationContext();
        ROOT_EXECUTOR.execute(
                () -> reconcileVivoPerformanceMode(applicationContext, generation));
        if (preferences.getBoolean(SettingsActivity.K_OEM_ROOT_PERFORMANCE, false)) {
            ROOT_EXECUTOR.execute(() -> applyRootBoost(applicationContext));
        }
    }

    static void markGameplay(Context context) {
        synchronized (LOCK) {
            if (!active || gameplayReported) {
                return;
            }
            gameplayReported = true;
        }
        reportGameState(context, false, true);
    }

    static void stop(Context context) {
        VivoLease detachedLease;
        synchronized (LOCK) {
            if (!active && !rootApplied && vivoLease == null
                    && vivoReconciliation == null) {
                return;
            }
            active = false;
            gameplayReported = false;
            ++vivoGeneration;
            vivoDesiredMode = VivoMode.OFF;
            detachedLease = vivoLease;
            vivoLease = null;
            if (vivoReconciliation != null) {
                vivoReconciliation.cancel(false);
                vivoReconciliation = null;
            }
        }
        reportGameState(context, false, false);
        ROOT_EXECUTOR.execute(() -> {
            if (detachedLease != null) {
                Log.i(TAG, "vivo transition=stop from=" + detachedLease.mode.logName
                        + " to=off order=release-detached oldHandle="
                        + detachedLease.handle);
                if (!releaseVivoLease(detachedLease)) {
                    scheduleDetachedReleaseRetry(detachedLease, 1);
                }
            }
            restoreRootBoost();
        });
    }

    private static void reconcileVivoPerformanceMode(Context context, long generation) {
        String manufacturer = Build.MANUFACTURER;
        if (manufacturer == null || !manufacturer.equalsIgnoreCase("vivo")) {
            return;
        }
        VivoMode desired = readDesiredVivoMode(context);
        VivoLease current;
        synchronized (LOCK) {
            if (!active || generation != vivoGeneration) {
                return;
            }
            vivoReconciliation = null;
            vivoDesiredMode = desired;
            current = vivoLease;
        }

        if (desired == VivoMode.OFF) {
            detachAndReleaseForOff(context, generation, current);
            return;
        }

        if (current != null && current.mode == desired) {
            renewVivoLease(context, generation, desired, current);
        } else {
            switchVivoLease(context, generation, desired, current);
        }
    }

    private static void detachAndReleaseForOff(Context context, long generation,
            VivoLease current) {
        if (current != null) {
            boolean detached = false;
            synchronized (LOCK) {
                if (active && generation == vivoGeneration
                        && vivoDesiredMode == VivoMode.OFF && vivoLease == current) {
                    Log.i(TAG, "vivo transition=switch from=" + current.mode.logName
                            + " to=off order=detach-old oldHandle=" + current.handle);
                    vivoLease = null;
                    detached = true;
                }
            }
            if (detached) {
                Log.i(TAG, "vivo transition=switch from=" + current.mode.logName
                        + " to=off order=release-old oldHandle=" + current.handle);
                if (!releaseVivoLease(current)) {
                    if (!restoreLeaseAfterFailedRelease(context, generation, current,
                            "off-release-failed")) {
                        scheduleDetachedReleaseRetry(current, 1);
                    }
                    return;
                }
            }
        }
        Log.i(TAG, "vivo mode=off result=disabled-by-marker");
        scheduleVivoReconciliation(context, generation, VIVO_RETRY_DELAY_SECONDS,
                "policy-off-marker");
    }

    private static void renewVivoLease(Context context, long generation,
            VivoMode desired, VivoLease oldLease) {
        if (blockAcquireForPendingReleases(context, generation, "renew-pending-release")) {
            return;
        }
        Log.i(TAG, "vivo transition=renew mode=" + desired.logName
                + " order=acquire-replacement oldHandle=" + oldLease.handle);
        VivoLease replacement = acquireVivoLease(context, desired);
        if (replacement == null) {
            Log.w(TAG, "vivo mode=" + desired.logName + " transport=binder"
                    + " result=renewal-failed");
            scheduleVivoReconciliation(context, generation, VIVO_RETRY_DELAY_SECONDS,
                    "renewal-failed");
            return;
        }

        VivoMode observedDesired = readDesiredVivoMode(context);
        boolean committed = false;
        synchronized (LOCK) {
            if (active && generation == vivoGeneration
                    && vivoDesiredMode == desired && observedDesired == desired
                    && vivoLease == oldLease) {
                Log.i(TAG, "vivo transition=renew mode=" + desired.logName
                        + " order=swap-replacement oldHandle=" + oldLease.handle
                        + " newHandle=" + replacement.handle);
                vivoLease = replacement;
                scheduleVivoReconciliationLocked(context, generation,
                        VIVO_RENEWAL_DELAY_SECONDS, "ttl-renewal");
                committed = true;
            }
        }

        if (!committed) {
            if (!releaseVivoLease(replacement)) {
                scheduleDetachedReleaseRetry(replacement, 1);
            }
            scheduleVivoReconciliation(context, generation, VIVO_RETRY_DELAY_SECONDS,
                    "renewal-guard-changed");
            return;
        }

        Log.i(TAG, "vivo transition=renew mode=" + desired.logName
                + " order=release-old oldHandle=" + oldLease.handle
                + " newHandle=" + replacement.handle);
        if (!releaseVivoLease(oldLease)) {
            scheduleDetachedReleaseRetry(oldLease, 1);
        }
        Log.i(TAG, "vivo mode=" + desired.logName + " transport="
                + replacement.transport + " result=renewed handle=" + replacement.handle);
    }

    private static void switchVivoLease(Context context, long generation,
            VivoMode desired, VivoLease oldLease) {
        if (oldLease != null) {
            boolean detached = false;
            synchronized (LOCK) {
                if (active && generation == vivoGeneration
                        && vivoDesiredMode == desired && vivoLease == oldLease) {
                    Log.i(TAG, "vivo transition=switch from=" + oldLease.mode.logName
                            + " to=" + desired.logName + " order=detach-old oldHandle="
                            + oldLease.handle);
                    vivoLease = null;
                    detached = true;
                }
            }
            if (!detached) {
                scheduleVivoReconciliation(context, generation,
                        VIVO_RETRY_DELAY_SECONDS, "switch-detach-guard-changed");
                return;
            }
            Log.i(TAG, "vivo transition=switch from=" + oldLease.mode.logName
                    + " to=" + desired.logName + " order=release-old oldHandle="
                    + oldLease.handle);
            if (!releaseVivoLease(oldLease)) {
                if (!restoreLeaseAfterFailedRelease(context, generation, oldLease,
                        "switch-release-failed")) {
                    scheduleDetachedReleaseRetry(oldLease, 1);
                }
                return;
            }
        }

        if (blockAcquireForPendingReleases(context, generation,
                "switch-pending-release")) {
            return;
        }
        if (!isVivoTransitionCurrent(context, generation, desired)) {
            scheduleVivoReconciliation(context, generation, VIVO_RETRY_DELAY_SECONDS,
                    "switch-before-acquire-guard-changed");
            return;
        }

        Log.i(TAG, "vivo transition=switch from="
                + (oldLease == null ? "none" : oldLease.mode.logName)
                + " to=" + desired.logName + " order=acquire-desired oldHandle="
                + (oldLease == null ? 0 : oldLease.handle));
        VivoLease acquired = acquireVivoLease(context, desired);
        if (acquired != null && commitVivoLease(context, generation, desired,
                acquired, false)) {
            Log.i(TAG, "vivo transition=switch from="
                    + (oldLease == null ? "none" : oldLease.mode.logName)
                    + " to=" + desired.logName + " order=commit-desired oldHandle="
                    + (oldLease == null ? 0 : oldLease.handle)
                    + " newHandle=" + acquired.handle);
            Log.i(TAG, "vivo mode=" + desired.logName + " transport="
                    + acquired.transport + " result=acquired handle=" + acquired.handle);
            return;
        }
        if (acquired != null) {
            if (!releaseVivoLease(acquired)) {
                scheduleDetachedReleaseRetry(acquired, 1);
            }
            scheduleVivoReconciliation(context, generation, VIVO_RETRY_DELAY_SECONDS,
                    "switch-commit-guard-changed");
            return;
        }

        Log.w(TAG, "vivo mode=" + desired.logName
                + " transport=binder result=acquire-failed");
        Log.i(TAG, "vivo transition=switch requested=" + desired.logName
                + " fallback=moderate-cpu-floor order=acquire-fallback");
        VivoLease fallback = acquireVivoLease(context, VivoMode.MODERATE_CPU_FLOOR);
        if (fallback != null && commitVivoLease(context, generation, desired,
                fallback, true)) {
            Log.i(TAG, "vivo transition=switch requested=" + desired.logName
                    + " fallback=moderate-cpu-floor order=commit-fallback");
            Log.i(TAG, "vivo mode=moderate-cpu-floor transport=" + fallback.transport
                    + " result=fallback-acquired handle=" + fallback.handle);
            return;
        }
        if (fallback != null) {
            if (!releaseVivoLease(fallback)) {
                scheduleDetachedReleaseRetry(fallback, 1);
            }
        }
        scheduleVivoReconciliation(context, generation, VIVO_RETRY_DELAY_SECONDS,
                "switch-and-fallback-failed");
    }

    private static boolean commitVivoLease(Context context, long generation,
            VivoMode requested, VivoLease candidate, boolean fallback) {
        VivoMode observedDesired = readDesiredVivoMode(context);
        synchronized (LOCK) {
            if (!active || generation != vivoGeneration
                    || vivoDesiredMode != requested || observedDesired != requested
                    || vivoLease != null || !PENDING_VIVO_RELEASES.isEmpty()) {
                return false;
            }
            vivoLease = candidate;
            scheduleVivoReconciliationLocked(context, generation,
                    fallback ? VIVO_RETRY_DELAY_SECONDS : VIVO_RENEWAL_DELAY_SECONDS,
                    fallback ? "fallback-retry" : "ttl-renewal");
            return true;
        }
    }

    private static boolean isVivoTransitionCurrent(Context context, long generation,
            VivoMode expectedDesired) {
        VivoMode observedDesired = readDesiredVivoMode(context);
        synchronized (LOCK) {
            return active && generation == vivoGeneration
                    && vivoDesiredMode == expectedDesired
                    && observedDesired == expectedDesired && vivoLease == null
                    && PENDING_VIVO_RELEASES.isEmpty();
        }
    }

    private static boolean blockAcquireForPendingReleases(Context context,
            long generation, String reason) {
        int pendingCount;
        synchronized (LOCK) {
            pruneExpiredPendingReleasesLocked();
            pendingCount = PENDING_VIVO_RELEASES.size();
        }
        if (pendingCount == 0) {
            return false;
        }
        Log.w(TAG, "vivo result=acquire-blocked reason=" + reason
                + " pendingReleases=" + pendingCount);
        scheduleVivoReconciliation(context, generation, VIVO_RETRY_DELAY_SECONDS,
                reason);
        return true;
    }

    private static void pruneExpiredPendingReleasesLocked() {
        final long now = System.nanoTime();
        Iterator<Map.Entry<Integer, VivoLease>> iterator =
                PENDING_VIVO_RELEASES.entrySet().iterator();
        while (iterator.hasNext()) {
            VivoLease lease = iterator.next().getValue();
            if (now - lease.expiresAtNanos >= 0) {
                iterator.remove();
                Log.w(TAG, "vivo mode=" + lease.mode.logName + " transport="
                        + lease.transport + " result=release-pending-expired handle="
                        + lease.handle + " ttlMs=" + VIVO_HINT_DURATION_MS);
            }
        }
    }

    private static boolean restoreLeaseAfterFailedRelease(Context context, long generation,
            VivoLease lease, String reason) {
        synchronized (LOCK) {
            if (!active || generation != vivoGeneration || vivoLease != null) {
                return false;
            }


            vivoLease = lease;
            scheduleVivoReconciliationLocked(context, generation,
                    VIVO_RETRY_DELAY_SECONDS, reason);
            return true;
        }
    }

    private static VivoMode readDesiredVivoMode(Context context) {
        File runtimeRoot = new File(context.getFilesDir(), "lsx4-home");
        if (new File(runtimeRoot, DISABLE_VIVO_CPU_FLOOR_MARKER).isFile()) {
            return VivoMode.OFF;
        }
        if (new File(runtimeRoot, USE_VIVO_MODERATE_CPU_FLOOR_MARKER).isFile()) {
            return VivoMode.MODERATE_CPU_FLOOR;
        }





        if (new File(runtimeRoot, USE_VIVO_PERFORMANCE_HINT_MARKER).isFile()) {
            return VivoMode.PERFORMANCE_HINT_4241;
        }
        return VivoMode.PERFORMANCE_HINT_4241;
    }

    private static void scheduleVivoReconciliation(Context context, long generation,
            int delaySeconds, String reason) {
        synchronized (LOCK) {
            scheduleVivoReconciliationLocked(context, generation, delaySeconds, reason);
        }
    }

    private static void scheduleVivoReconciliationLocked(Context context, long generation,
            int delaySeconds, String reason) {
        if (!active || generation != vivoGeneration) {
            return;
        }
        if (vivoReconciliation != null) {
            vivoReconciliation.cancel(false);
        }
        vivoReconciliation = ROOT_EXECUTOR.schedule(
                () -> reconcileVivoPerformanceMode(context, generation),
                delaySeconds, TimeUnit.SECONDS);
        Log.i(TAG, "vivo reconcile reason=" + reason + " delaySeconds=" + delaySeconds
                + " generation=" + generation);
    }

    private static VivoLease acquireVivoLease(Context context, VivoMode mode) {
        int handle;
        if (mode == VivoMode.PERFORMANCE_HINT_4241) {
            handle = invokeVivoPerformanceHint(context);
        } else if (mode == VivoMode.MODERATE_CPU_FLOOR) {
            handle = invokeVivoCpuFloorLock();
        } else {
            return null;
        }
        if (handle <= 0) {
            return null;
        }
        return new VivoLease(handle, mode, null, null, "binder");
    }

    private static boolean releaseVivoLease(VivoLease lease) {
        if (lease == null || lease.handle <= 0) {
            return true;
        }
        if (lease.manager != null && lease.releaseMethod != null) {
            try {
                Object result = lease.releaseMethod.invoke(lease.manager, lease.handle);
                int releaseResult = result instanceof Number
                        ? ((Number) result).intValue() : -1;
                if (releaseResult >= 0) {
                    Log.i(TAG, "vivo mode=" + lease.mode.logName + " transport="
                            + lease.transport + " result=released handle=" + lease.handle
                            + " releaseResult=" + releaseResult);
                    return true;
                }
                Log.w(TAG, "vivo mode=" + lease.mode.logName + " transport="
                        + lease.transport + " result=release-failed handle=" + lease.handle
                        + " releaseResult=" + releaseResult);
                return false;
            } catch (Throwable error) {
                Log.w(TAG, "vivo mode=" + lease.mode.logName + " transport="
                        + lease.transport + " result=release-failed handle=" + lease.handle,
                        error);
                return false;
            }
        }
        int result = invokeVivoServiceRelease(lease.handle);
        if (result >= 0) {
            Log.i(TAG, "vivo mode=" + lease.mode.logName + " transport="
                    + lease.transport + " result=released handle=" + lease.handle
                    + " releaseResult=" + result);
            return true;
        }
        Log.w(TAG, "vivo mode=" + lease.mode.logName + " transport="
                + lease.transport + " result=release-failed handle=" + lease.handle
                + " releaseResult=" + result);
        return false;
    }

    private static void scheduleDetachedReleaseRetry(VivoLease lease, int attempt) {
        if (lease == null || lease.handle <= 0) {
            return;
        }
        synchronized (LOCK) {
            pruneExpiredPendingReleasesLocked();
            if (System.nanoTime() - lease.expiresAtNanos >= 0) {
                return;
            }
            PENDING_VIVO_RELEASES.put(lease.handle, lease);
        }
        if (attempt > VIVO_RELEASE_RETRY_LIMIT) {
            Log.e(TAG, "vivo mode=" + lease.mode.logName + " transport="
                    + lease.transport + " result=release-retry-exhausted handle="
                    + lease.handle + " ttlMs=" + VIVO_HINT_DURATION_MS);
            return;
        }
        Log.w(TAG, "vivo mode=" + lease.mode.logName + " transport="
                + lease.transport + " result=release-retry-scheduled handle="
                + lease.handle + " attempt=" + attempt);
        ROOT_EXECUTOR.schedule(() -> {
            if (releaseVivoLease(lease)) {
                synchronized (LOCK) {
                    if (PENDING_VIVO_RELEASES.get(lease.handle) == lease) {
                        PENDING_VIVO_RELEASES.remove(lease.handle);
                    }
                }
            } else {
                scheduleDetachedReleaseRetry(lease, attempt + 1);
            }
        }, VIVO_RETRY_DELAY_SECONDS, TimeUnit.SECONDS);
    }

    private static int invokeVivoPerformanceHint(Context context) {
        return invokeVivoService(new String[]{
                "/system/bin/service", "call", "vendor.vivoperfservice", "3",
                "i32", Integer.toString(VIVO_PERFORMANCE_HINT),
                "s16", context.getPackageName(),
                "i32", Integer.toString(VIVO_HINT_DURATION_MS),
                "i32", Integer.toString(VIVO_PERFORMANCE_HINT_USER_DATA_2),
                "i32", Integer.toString(android.os.Process.myTid())
        });
    }

    private static int invokeVivoCpuFloorLock() {
        String[] command = new String[8 + VIVO_CPU_FLOOR_RESOURCES.length * 2];
        int index = 0;
        command[index++] = "/system/bin/service";
        command[index++] = "call";
        command[index++] = "vendor.vivoperfservice";
        command[index++] = "4";
        command[index++] = "i32";
        command[index++] = Integer.toString(VIVO_HINT_DURATION_MS);
        command[index++] = "i32";
        command[index++] = Integer.toString(VIVO_CPU_FLOOR_RESOURCES.length);
        for (int value : VIVO_CPU_FLOOR_RESOURCES) {
            command[index++] = "i32";
            command[index++] = Integer.toString(value);
        }
        return invokeVivoService(command);
    }

    private static int invokeVivoServiceRelease(int handle) {
        return invokeVivoService(new String[]{
                "/system/bin/service", "call", "vendor.vivoperfservice", "2",
                "i32", Integer.toString(handle)
        });
    }

    private static int invokeVivoService(String[] command) {
        Process process = null;
        try {
            process = new ProcessBuilder(command).redirectErrorStream(true).start();
            if (!process.waitFor(2, TimeUnit.SECONDS)) {
                process.destroyForcibly();
                return -1;
            }
            String output = readProcessOutput(process.getInputStream());
            if (process.exitValue() != 0) {
                return -1;
            }
            Matcher matcher = PARCEL_WORD.matcher(output);
            if (!matcher.find()) {
                return -1;
            }
            long exception = Long.parseLong(matcher.group(1), 16);
            if (exception != 0) {
                return -1;
            }
            if (!matcher.find()) {
                return -1;
            }
            return (int) Long.parseLong(matcher.group(1), 16);
        } catch (Throwable ignored) {
            return -1;
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    private static String readProcessOutput(InputStream input) throws java.io.IOException {
        byte[] buffer = new byte[512];
        StringBuilder output = new StringBuilder();
        int read;
        while ((read = input.read(buffer)) >= 0) {
            output.append(new String(buffer, 0, read, StandardCharsets.UTF_8));
        }
        return output.toString();
    }

    private static void reportGameState(Context context, boolean loading, boolean gameplay) {
        try {
            AndroidApiCompat.reportGameState(context, loading, gameplay);
            Log.i(TAG, "gameState loading=" + loading + " gameplay=" + gameplay);
        } catch (Throwable error) {
            Log.w(TAG, "gameState unavailable", error);
        }
    }

    private static void applyRootBoost(Context context) {
        Map<String, String> requested = discoverPerformanceNodes();
        if (requested.isEmpty()) {
            Log.i(TAG, "rootBoost unavailable: no supported sysfs nodes");
            return;
        }
        synchronized (LOCK) {
            if (!active) {
                return;
            }
            ORIGINAL_VALUES.clear();
            ORIGINAL_VALUES.putAll(requested);
        }

        StringBuilder command = new StringBuilder("set -e;");
        int cpuPolicies = 0;
        for (Map.Entry<String, String> entry : requested.entrySet()) {
            String path = entry.getKey();
            if (path.endsWith("/scaling_governor")) {
                File available = new File(new File(path).getParentFile(),
                        "scaling_available_governors");
                String governors = readText(available.getAbsolutePath());
                if (!containsWord(governors, "performance")) {
                    continue;
                }
                appendWrite(command, path, "performance");
                cpuPolicies++;
            }
        }
        String gpuMin = "/sys/class/kgsl/kgsl-3d0/devfreq/min_freq";
        String gpuMax = "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq";
        boolean gpuRequested = requested.containsKey(gpuMin) && new File(gpuMax).isFile();
        if (gpuRequested) {
            command.append("cat ").append(gpuMax).append(" > ").append(gpuMin).append(';');
        }
        if (cpuPolicies == 0 && !gpuRequested) {
            synchronized (LOCK) {
                ORIGINAL_VALUES.clear();
            }
            Log.i(TAG, "rootBoost unavailable: performance governor and Adreno nodes absent");
            return;
        }

        int result = runSu(command.toString());
        boolean changed = false;
        for (String path : requested.keySet()) {
            String current = readText(path);
            if (path.endsWith("/scaling_governor")) {
                changed |= "performance".equals(current)
                        && !"performance".equals(requested.get(path));
            } else if (path.equals(gpuMin)) {
                changed |= current.equals(readText(gpuMax))
                        && !current.isEmpty() && !current.equals(requested.get(path));
            }
        }
        if (result != 0 && changed) {
            StringBuilder rollback = new StringBuilder("set -e;");
            for (Map.Entry<String, String> entry : requested.entrySet()) {
                appendWrite(rollback, entry.getKey(), entry.getValue());
            }
            int rollbackResult = runSu(rollback.toString());
            Log.w(TAG, "rootBoost partial apply rolled back result=" + rollbackResult);
            changed = rollbackResult != 0;
        }
        synchronized (LOCK) {
            rootApplied = changed;
            if (!changed) {
                ORIGINAL_VALUES.clear();
            }
        }
        Log.i(TAG, "rootBoost result=" + result + " applied=" + changed
                + " cpuPolicies=" + cpuPolicies + " adreno=" + gpuRequested);
        if (!changed && result == 0) {
            Log.w(TAG, "rootBoost command succeeded but kernel values did not change");
        }
    }

    private static void restoreRootBoost() {
        Map<String, String> restore;
        synchronized (LOCK) {
            if (!rootApplied || ORIGINAL_VALUES.isEmpty()) {
                return;
            }
            restore = new LinkedHashMap<>(ORIGINAL_VALUES);
        }
        StringBuilder command = new StringBuilder("set -e;");
        for (Map.Entry<String, String> entry : restore.entrySet()) {
            appendWrite(command, entry.getKey(), entry.getValue());
        }
        int result = runSu(command.toString());
        if (result == 0) {
            synchronized (LOCK) {
                rootApplied = false;
                ORIGINAL_VALUES.clear();
            }
        }
        Log.i(TAG, "rootBoost restoreResult=" + result + " nodes=" + restore.size());
    }

    private static Map<String, String> discoverPerformanceNodes() {
        Map<String, String> values = new LinkedHashMap<>();
        File root = new File("/sys/devices/system/cpu/cpufreq");
        File[] policies = root.listFiles(file -> file.isDirectory()
                && file.getName().startsWith("policy"));
        if (policies != null) {
            java.util.Arrays.sort(policies, (left, right) ->
                    left.getName().compareTo(right.getName()));
            for (File policy : policies) {
                String path = new File(policy, "scaling_governor").getAbsolutePath();
                String value = readText(path);
                if (!value.isEmpty()) {
                    values.put(path, value);
                }
            }
        }
        String gpuMin = "/sys/class/kgsl/kgsl-3d0/devfreq/min_freq";
        String currentGpuMin = readText(gpuMin);
        if (!currentGpuMin.isEmpty()) {
            values.put(gpuMin, currentGpuMin);
        }
        return values;
    }

    private static void appendWrite(StringBuilder command, String path, String value) {
        if (!path.matches("/sys/[A-Za-z0-9_./-]+")
                || !value.matches("[A-Za-z0-9_.-]+")) {
            return;
        }
        command.append("printf '%s' '").append(value).append("' > ")
                .append(path).append(';');
    }

    private static boolean containsWord(String values, String expected) {
        for (String value : values.split("\\s+")) {
            if (expected.equals(value)) {
                return true;
            }
        }
        return false;
    }

    private static String readText(String path) {
        try {
            return new String(Files.readAllBytes(new File(path).toPath()),
                    StandardCharsets.UTF_8).trim();
        } catch (Throwable ignored) {
            return "";
        }
    }

    private static int runSu(String command) {
        Process process = null;
        try {
            process = new ProcessBuilder("su", "-c", command)
                    .redirectErrorStream(true)
                    .start();
            if (!process.waitFor(8, TimeUnit.SECONDS)) {
                process.destroyForcibly();
                return -2;
            }
            return process.exitValue();
        } catch (Throwable error) {
            Log.w(TAG, "rootBoost unavailable", error);
            return -1;
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }
}
