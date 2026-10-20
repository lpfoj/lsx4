package app.lsx4.android;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ResultReceiver;
import android.os.SystemClock;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.atomic.AtomicBoolean;

public final class VulkanDriverProbeActivity extends Activity {
    private static final long CHILD_TIMEOUT_MS = 20_000L;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final AtomicBoolean completed = new AtomicBoolean(false);
    private ResultReceiver receiver;
    private String probeId = "";
    private long startedAtMs;

    @Override
    @SuppressWarnings("deprecation")
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        startedAtMs = SystemClock.elapsedRealtime();
        Intent intent = getIntent();
        probeId = intent == null ? ""
                : intent.getStringExtra(VulkanDriverProbe.EXTRA_PROBE_ID);
        receiver = intent == null ? null
                : intent.getParcelableExtra(VulkanDriverProbe.EXTRA_RECEIVER);
        if (probeId == null || !probeId.matches("[0-9a-fA-F-]{36}")
                || receiver == null) {
            finishAndTerminate();
            return;
        }

        try {
            JSONObject started = new JSONObject();
            started.put("schema", "lsx4.vulkan-driver-probe.v1");
            started.put("probeId", probeId);
            started.put("pid", android.os.Process.myPid());
            started.put("outcome", "started");
            writeJournal("started", started);
            Bundle signal = new Bundle();
            signal.putInt("pid", android.os.Process.myPid());
            receiver.send(VulkanDriverProbe.RESULT_STARTED, signal);
        } catch (Exception error) {
            complete("error", null, "Could not create the probe journal");
            return;
        }

        handler.postDelayed(() -> complete(
                "timeout", null, "Driver probe exceeded 20 seconds"), CHILD_TIMEOUT_MS);
        Thread worker = new Thread(this::runProbe, "lsx4-vulkan-driver-probe");
        worker.setDaemon(true);
        worker.start();
    }

    private void runProbe() {
        try {
            Intent intent = getIntent();
            String directory = intent.getStringExtra(
                    VulkanDriverProbe.EXTRA_DRIVER_DIRECTORY);
            String expectedSha = intent.getStringExtra(
                    VulkanDriverProbe.EXTRA_DRIVER_SHA256);
            VulkanDriverManager.DriverPackage candidate =
                    VulkanDriverManager.validateProbeTarget(this, directory, expectedSha);
            File nativeDirectory = new File(getApplicationInfo().nativeLibraryDir)
                    .getCanonicalFile();
            File runtime = new File(nativeDirectory, "liblsx4_executor_android.so")
                    .getCanonicalFile();
            if (!runtime.isFile() || runtime.length() <= 0L) {
                throw new IllegalStateException("Packaged PS4 runtime is unavailable");
            }
            File report = new File(new File(getFilesDir(),
                    VulkanDriverProbe.JOURNAL_DIRECTORY), probeId + ".status.json");
            String rawStatus = RuntimeBridge.probeVulkanDriverAxis(
                    runtime.getAbsolutePath(), nativeDirectory.getAbsolutePath(),
                    candidate.directory.getCanonicalPath(), report.getAbsolutePath(),
                    candidate.sha256);
            JSONObject status = new JSONObject(rawStatus == null ? "{}" : rawStatus);
            String identity = (status.optString("driverName", "") + " "
                    + status.optString("driverInfo", "")).toLowerCase(java.util.Locale.ROOT);
            boolean pass = "turnip".equals(status.optString("requested"))
                    && "turnip".equals(status.optString("effective"))
                    && "none".equals(status.optString("fallbackReason"))
                    && status.optBoolean("selectionLocked", false)
                    && status.optBoolean("mappingVerified", false)
                    && status.optBoolean("logicalDeviceProbePassed", false)
                    && (identity.contains("mesa") || identity.contains("turnip"));
            complete(pass ? "pass" : "rejected", status,
                    pass ? "none" : status.optString(
                            "fallbackReason", "probe-evidence-mismatch"));
        } catch (Throwable error) {
            complete("error", null, error.getMessage() == null
                    ? error.getClass().getSimpleName() : error.getMessage());
        }
    }

    private void complete(String outcome, JSONObject runtimeStatus, String reason) {
        if (!completed.compareAndSet(false, true)) {
            return;
        }
        handler.removeCallbacksAndMessages(null);
        try {
            JSONObject result = new JSONObject();
            result.put("schema", "lsx4.vulkan-driver-probe.v1");
            result.put("probeId", probeId);
            result.put("outcome", outcome);
            result.put("pid", android.os.Process.myPid());
            result.put("durationMs", Math.max(0L,
                    SystemClock.elapsedRealtime() - startedAtMs));
            result.put("reason", reason == null ? "" : reason);
            if (runtimeStatus != null) {
                result.put("runtimeStatus", runtimeStatus);
            }
            writeJournal("result", result);
            Bundle payload = new Bundle();
            payload.putString(VulkanDriverProbe.EXTRA_RESULT_JSON, result.toString());
            receiver.send(VulkanDriverProbe.RESULT_FINISHED, payload);
        } catch (Throwable ignored) {
        }
        finishAndTerminate();
    }

    private void writeJournal(String suffix, JSONObject value) throws Exception {
        File destination = VulkanDriverProbe.journalFile(this, probeId, suffix);
        File directory = destination.getParentFile();
        if (directory == null || (!directory.isDirectory() && !directory.mkdirs())) {
            throw new IllegalStateException("Could not create the probe journal directory");
        }
        File temporary = new File(directory, destination.getName() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            output.write((value.toString() + "\n").getBytes(StandardCharsets.UTF_8));
            output.getFD().sync();
        }
        try {
            Files.move(temporary.toPath(), destination.toPath(),
                    StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException ignored) {
            Files.move(temporary.toPath(), destination.toPath(),
                    StandardCopyOption.REPLACE_EXISTING);
        }
    }

    private void finishAndTerminate() {
        finish();
        handler.postDelayed(() -> android.os.Process.killProcess(
                android.os.Process.myPid()), 200L);
    }
}
