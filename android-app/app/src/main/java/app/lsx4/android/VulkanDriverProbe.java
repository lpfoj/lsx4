package app.lsx4.android;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ResultReceiver;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

final class VulkanDriverProbe {
    static final int RESULT_STARTED = 1;
    static final int RESULT_FINISHED = 2;
    static final String EXTRA_PROBE_ID = "lsx4_driver_probe_id";
    static final String EXTRA_DRIVER_DIRECTORY = "lsx4_driver_probe_directory";
    static final String EXTRA_DRIVER_SHA256 = "lsx4_driver_probe_sha256";
    static final String EXTRA_RECEIVER = "lsx4_driver_probe_receiver";
    static final String EXTRA_RESULT_JSON = "lsx4_driver_probe_result";
    static final String JOURNAL_DIRECTORY = "vulkan-driver-probes";
    private static final long PARENT_TIMEOUT_MS = 25_000L;

    interface Callback {
        void onCompleted(ProbeResult result);
    }

    static final class ProbeResult {
        final String outcome;
        final String runtimeStatus;
        final String reason;

        ProbeResult(String outcome, String runtimeStatus, String reason) {
            this.outcome = outcome;
            this.runtimeStatus = runtimeStatus;
            this.reason = reason;
        }

        boolean passed() {
            return "pass".equals(outcome);
        }
    }

    private VulkanDriverProbe() {
    }

    static void start(Activity owner, VulkanDriverManager.DriverPackage driver,
                      Callback callback) {
        Handler handler = new Handler(Looper.getMainLooper());
        AtomicBoolean completed = new AtomicBoolean(false);
        String probeId = UUID.randomUUID().toString();
        int[] childPid = {-1};
        Runnable timeout = () -> {
            if (!completed.compareAndSet(false, true)) {
                return;
            }
            JSONObject journal = readJournal(owner, probeId, "result");
            if (journal != null) {
                callback.onCompleted(parseResult(journal));
                return;
            }
            boolean started = readJournal(owner, probeId, "started") != null;
            boolean alive = childPid[0] > 0 && isExpectedProbeProcess(owner, childPid[0]);
            if (alive) {
                android.os.Process.killProcess(childPid[0]);
            }
            callback.onCompleted(new ProbeResult(
                    started && !alive ? "crash" : "timeout", "",
                    started && !alive
                            ? "Driver probe process exited without a result"
                            : "Driver probe timed out"));
        };
        ResultReceiver receiver = new ResultReceiver(handler) {
            @Override
            protected void onReceiveResult(int resultCode, Bundle resultData) {
                if (resultCode == RESULT_STARTED) {
                    childPid[0] = resultData == null ? -1 : resultData.getInt("pid", -1);
                    return;
                }
                if (resultCode != RESULT_FINISHED
                        || !completed.compareAndSet(false, true)) {
                    return;
                }
                handler.removeCallbacks(timeout);
                String raw = resultData == null ? ""
                        : resultData.getString(EXTRA_RESULT_JSON, "");
                try {
                    callback.onCompleted(parseResult(new JSONObject(raw)));
                } catch (Exception error) {
                    callback.onCompleted(new ProbeResult("error", "",
                            "Malformed driver probe response"));
                }
            }
        };

        Intent intent = new Intent(owner, VulkanDriverProbeActivity.class);
        intent.putExtra(EXTRA_PROBE_ID, probeId);
        intent.putExtra(EXTRA_DRIVER_DIRECTORY, driver.directory.getAbsolutePath());
        intent.putExtra(EXTRA_DRIVER_SHA256, driver.sha256);
        intent.putExtra(EXTRA_RECEIVER, receiver);
        try {
            owner.startActivity(intent);
            handler.postDelayed(timeout, PARENT_TIMEOUT_MS);
        } catch (Throwable error) {
            completed.set(true);
            callback.onCompleted(new ProbeResult("error", "",
                    error.getMessage() == null
                            ? "Could not start driver probe" : error.getMessage()));
        }
    }

    static File journalFile(Activity context, String probeId, String suffix) {
        return new File(new File(context.getFilesDir(), JOURNAL_DIRECTORY),
                probeId + "." + suffix + ".json");
    }

    private static JSONObject readJournal(Activity context, String probeId, String suffix) {
        if (probeId == null || !probeId.matches("[0-9a-fA-F-]{36}")) {
            return null;
        }
        File file = journalFile(context, probeId, suffix);
        if (!file.isFile() || file.length() <= 0L || file.length() > 128 * 1024L) {
            return null;
        }
        try (FileInputStream input = new FileInputStream(file)) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            int total = 0;
            for (int count = input.read(buffer); count >= 0; count = input.read(buffer)) {
                if (count <= 0) {
                    continue;
                }
                if (total > 128 * 1024 - count) {
                    return null;
                }
                output.write(buffer, 0, count);
                total += count;
            }
            return new JSONObject(new String(output.toByteArray(), StandardCharsets.UTF_8));
        } catch (Exception ignored) {
            return null;
        }
    }

    private static ProbeResult parseResult(JSONObject result) {
        JSONObject runtime = result.optJSONObject("runtimeStatus");
        return new ProbeResult(result.optString("outcome", "error"),
                runtime == null ? "" : runtime.toString(),
                result.optString("reason", "Driver probe failed"));
    }

    private static boolean isExpectedProbeProcess(Activity context, int pid) {
        if (pid <= 0) {
            return false;
        }
        try {
            String cmdline = readSmallFile(new File("/proc/" + pid + "/cmdline"));
            int nul = cmdline.indexOf('\0');
            if (nul >= 0) {
                cmdline = cmdline.substring(0, nul);
            }
            if (!(context.getPackageName() + ":driverprobe").equals(cmdline)) {
                return false;
            }
            String status = readSmallFile(new File("/proc/" + pid + "/status"));
            String expected = "Uid:\t" + android.os.Process.myUid() + "\t";
            return status.contains(expected);
        } catch (Exception ignored) {
            return false;
        }
    }

    private static String readSmallFile(File file) throws Exception {
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[32 * 1024];
            int count = input.read(buffer);
            return count <= 0 ? "" : new String(buffer, 0, count,
                    StandardCharsets.UTF_8);
        }
    }
}
