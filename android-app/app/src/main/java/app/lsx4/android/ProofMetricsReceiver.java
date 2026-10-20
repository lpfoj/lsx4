package app.lsx4.android;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Debug;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;


public final class ProofMetricsReceiver extends BroadcastReceiver {
    public static final String ACTION = "app.lsx4.android.CAPTURE_PROOF_METRICS";
    public static final String EXTRA_REQUEST_TOKEN = "proof_request_token";

    private static final String TAG = "LSX4ProofMetrics";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION.equals(intent.getAction())) {
            return;
        }
        String token = intent.getStringExtra(EXTRA_REQUEST_TOKEN);
        if (token == null || !token.matches("[A-Za-z0-9._-]{1,80}")) {
            Log.e(TAG, "snapshot rejected=invalid-token");
            return;
        }

        try {
            long bootBeforeNs = SystemClock.elapsedRealtimeNanos();
            long processStarttime = readProcessStarttime();
            String fexRaw = RuntimeBridge.fexProofMetrics();
            String runtimeRaw = RuntimeBridge.runtimeProofCounters();
            String gpuResidencyRaw = RuntimeBridge.gpuResidencyMetrics();
            JSONObject androidMemory = captureAndroidProcessMemory();
            long hudBeforeNs = SystemClock.elapsedRealtimeNanos();
            long[] hudValues = RuntimeBridge.runtimeHudStats();
            long hudAfterNs = SystemClock.elapsedRealtimeNanos();
            if (fexRaw == null || fexRaw.isEmpty()) {
                throw new IOException("FEX proof query unavailable in target process");
            }
            if (runtimeRaw == null || runtimeRaw.isEmpty()) {
                throw new IOException("runtime proof query unavailable in target process");
            }
            if (gpuResidencyRaw == null || gpuResidencyRaw.isEmpty()) {
                throw new IOException("GPU residency query unavailable in target process");
            }
            if (hudValues == null || hudValues.length < 11) {
                throw new IOException("runtime HUD counters unavailable in target process");
            }

            JSONObject hudCounters = new JSONObject();
            hudCounters.put("generatedBlocks", requireNonNegative(hudValues[0], "generatedBlocks"));
            hudCounters.put("drawCalls", requireNonNegative(hudValues[6], "drawCalls"));
            hudCounters.put("submitCalls", requireNonNegative(hudValues[8], "submitCalls"));
            hudCounters.put("presentCalls", requireNonNegative(hudValues[10], "presentCalls"));
            if (hudValues.length >= 99) {
                hudCounters.put("fexCodeCacheGeneration",
                        requireNonNegative(hudValues[93], "fexCodeCacheGeneration"));
                hudCounters.put("fexCodeCacheCurrentSegments",
                        requireNonNegative(hudValues[94], "fexCodeCacheCurrentSegments"));
                hudCounters.put("fexCodeCacheUsedBytes",
                        requireNonNegative(hudValues[95], "fexCodeCacheUsedBytes"));
                hudCounters.put("fexCodeCacheCapacityBytes",
                        requireNonNegative(hudValues[96], "fexCodeCacheCapacityBytes"));
                hudCounters.put("fexJitCompilations",
                        requireNonNegative(hudValues[97], "fexJitCompilations"));
                hudCounters.put("fexJitCompileTimeNs",
                        requireNonNegative(hudValues[98], "fexJitCompileTimeNs"));
            }
            if (hudValues.length >= 108) {
                hudCounters.put("fsrEasuDispatchFrames",
                        requireNonNegative(hudValues[56], "fsrEasuDispatchFrames"));
                hudCounters.put("fsrActive", hudValues[99]);
                hudCounters.put("rcasActive", hudValues[100]);
                hudCounters.put("fsrSourceWidth", hudValues[101]);
                hudCounters.put("fsrSourceHeight", hudValues[102]);
                hudCounters.put("fsrOutputWidth", hudValues[103]);
                hudCounters.put("fsrOutputHeight", hudValues[104]);
                hudCounters.put("fsrEnabled", hudValues[105]);
                hudCounters.put("rcasEnabled", hudValues[106]);
                hudCounters.put("fsrRcasDispatchFrames",
                        requireNonNegative(hudValues[107], "fsrRcasDispatchFrames"));
            }
            if (hudValues.length >= 114 && hudValues[108] == 1) {
                hudCounters.put("upscalerSchema", hudValues[108]);
                hudCounters.put("upscalerRequestedMode", hudValues[109]);
                hudCounters.put("upscalerEffectiveMode", hudValues[110]);
                hudCounters.put("upscalerDispatchFrames", hudValues[111]);
                hudCounters.put("upscalerFallbackReason", hudValues[112]);
                hudCounters.put("temporalNativeInputs", hudValues[113]);
            }
            if (hudValues.length >= 166 && hudValues[114] == 1) {
                JSONObject cpuTuning = new JSONObject();
                cpuTuning.put("schema", "lsx4-cpu-tuning-proof/v1");
                cpuTuning.put("identityUnsigned", Long.toUnsignedString(hudValues[115]));
                String[] effectiveNames = {
                        "preset", "executionMode", "fastMemory", "memoryManager",
                        "blockLinking", "persistentCache", "functionReplacements",
                        "guestClockPercent", "cycleSkip", "idleLoopSkip",
                        "waitLoopDetection", "spinLoopDetection", "maxGuestWorkers",
                        "fastEventTiming", "jitBlockSize", "fpAccuracy", "atomics",
                        "accurateCacheLineStores", "accurateDmaTiming", "memoryExceptions",
                        "icacheAccuracy", "ignoreBadAccesses", "jitFeatureMask",
                        "jitDisableMask",
                        "cpuGpuDualThread", "scheduler", "preferredHeavyThreads",
                        "subsystemMultithreading", "separateIoThread",
                        "separateAudioHleThread", "guestThreadDelayMode",
                        "guestThreadDelayUs", "syncOnIdleSkip",
                        "cpuGpuSync", "forceRealClockSync", "hleMode", "hleFastTimers",
                        "perGameProfile", "autoGameDb"
                };
                JSONObject effective = new JSONObject();
                for (int index = 0; index < effectiveNames.length; index++) {
                    effective.put(effectiveNames[index], hudValues[116 + index]);
                }
                String[] eventNames = {
                        "idleYields", "waitParks", "spinParks", "cyclesSkipped",
                        "badAccessesIgnored", "hleFastCalls", "hleAccurateCalls",
                        "dmaDelays", "guestThreadsDelayed", "workerStartsThrottled",
                        "cpuGpuSyncs"
                };
                JSONObject events = new JSONObject();
                for (int index = 0; index < eventNames.length; index++) {
                    events.put(eventNames[index], requireNonNegative(
                            hudValues[155 + index], eventNames[index]));
                }
                cpuTuning.put("effective", effective);
                cpuTuning.put("events", events);
                hudCounters.put("cpuTuning", cpuTuning);
            }
            JSONArray rawHudValues = new JSONArray();
            for (long value : hudValues) {
                rawHudValues.put(value);
            }
            JSONObject hud = new JSONObject();
            hud.put("schema", "lsx4-runtime-hud-counters/v1");
            hud.put("source", "runtimeHudStats-exact-cumulative");
            hud.put("pid", Process.myPid());
            hud.put("bootBeforeNs", hudBeforeNs);
            hud.put("bootAfterNs", hudAfterNs);
            hud.put("valueCount", hudValues.length);
            JSONObject hudIndices = new JSONObject()
                    .put("generatedBlocks", 0)
                    .put("drawCalls", 6)
                    .put("submitCalls", 8)
                    .put("presentCalls", 10);
            if (hudValues.length >= 99) {
                hudIndices.put("fexCodeCacheGeneration", 93)
                        .put("fexCodeCacheCurrentSegments", 94)
                        .put("fexCodeCacheUsedBytes", 95)
                        .put("fexCodeCacheCapacityBytes", 96)
                        .put("fexJitCompilations", 97)
                        .put("fexJitCompileTimeNs", 98);
            }
            if (hudValues.length >= 108) {
                hudIndices.put("fsrEasuDispatchFrames", 56)
                        .put("fsrActive", 99)
                        .put("rcasActive", 100)
                        .put("fsrSourceWidth", 101)
                        .put("fsrSourceHeight", 102)
                        .put("fsrOutputWidth", 103)
                        .put("fsrOutputHeight", 104)
                        .put("fsrEnabled", 105)
                        .put("rcasEnabled", 106)
                        .put("fsrRcasDispatchFrames", 107);
            }
            if (hudValues.length >= 114 && hudValues[108] == 1) {
                hudIndices.put("upscalerSchema", 108)
                        .put("upscalerRequestedMode", 109)
                        .put("upscalerEffectiveMode", 110)
                        .put("upscalerDispatchFrames", 111)
                        .put("upscalerFallbackReason", 112)
                        .put("temporalNativeInputs", 113);
            }
            if (hudValues.length >= 166 && hudValues[114] == 1) {
                hudIndices.put("cpuTuningSchema", 114)
                        .put("cpuTuningIdentity", 115)
                        .put("cpuTuningEffectiveBegin", 116)
                        .put("cpuTuningEffectiveEnd", 154)
                        .put("cpuTuningEventsBegin", 155)
                        .put("cpuTuningEventsEnd", 165);
            }
            hud.put("indices", hudIndices);
            hud.put("counters", hudCounters);
            hud.put("rawValues", rawHudValues);

            JSONObject payload = new JSONObject();
            payload.put("schema", "lsx4-proof-metrics-snapshot/v2");
            payload.put("requestToken", token);
            payload.put("pid", Process.myPid());
            payload.put("processStarttime", processStarttime);
            payload.put("bootBeforeNs", bootBeforeNs);
            payload.put("fex", new JSONObject(fexRaw));
            payload.put("runtime", new JSONObject(runtimeRaw));
            payload.put("gpuResidency", new JSONObject(gpuResidencyRaw));
            payload.put("androidMemory", androidMemory);
            payload.put("hud", hud);
            payload.put("bootAfterNs", SystemClock.elapsedRealtimeNanos());

            File directory = new File(
                    new File(context.getFilesDir(), "lsx4-home"),
                    "runtime/proof-metrics");
            publishWithoutOverwrite(
                    new File(directory, "snapshot-" + token + ".json"),
                    (payload.toString() + "\n").getBytes(StandardCharsets.UTF_8));
            Log.i(TAG, "snapshot token=" + token + " pid=" + Process.myPid()
                    + " processStarttime=" + processStarttime);
        } catch (Exception error) {
            Log.e(TAG, "snapshot token=" + token + " failed="
                    + error.getClass().getSimpleName() + " message="
                    + error.getMessage());
        }
    }

    private static long requireNonNegative(long value, String name) throws IOException {
        if (value < 0) {
            throw new IOException("negative runtime HUD counter: " + name);
        }
        return value;
    }

    private static JSONObject captureAndroidProcessMemory() throws IOException {
        long bootBeforeNs = SystemClock.elapsedRealtimeNanos();
        Debug.MemoryInfo memory = new Debug.MemoryInfo();
        Debug.getMemoryInfo(memory);
        JSONObject result = new JSONObject();
        try {
            result.put("schema", "lsx4-android-process-memory/v1");
            result.put("source", "android.os.Debug.MemoryInfo-explicit-proof-only");
            result.put("pid", Process.myPid());
            result.put("bootBeforeNs", bootBeforeNs);
            result.put("totalPssKb", memory.getTotalPss());
            result.put("totalPrivateDirtyKb", memory.getTotalPrivateDirty());
            result.put("totalSharedDirtyKb", memory.getTotalSharedDirty());
            result.put("totalSwappablePssKb", memory.getTotalSwappablePss());
            result.put("summaryJavaHeapKb", memoryStatKb(memory, "summary.java-heap"));
            result.put("summaryNativeHeapKb", memoryStatKb(memory, "summary.native-heap"));
            result.put("summaryCodeKb", memoryStatKb(memory, "summary.code"));
            result.put("summaryStackKb", memoryStatKb(memory, "summary.stack"));
            result.put("summaryGraphicsKb", memoryStatKb(memory, "summary.graphics"));
            result.put("summaryPrivateOtherKb",
                    memoryStatKb(memory, "summary.private-other"));
            result.put("summarySystemKb", memoryStatKb(memory, "summary.system"));
            result.put("summaryTotalPssKb", memoryStatKb(memory, "summary.total-pss"));
            result.put("summaryTotalSwapKb", memoryStatKb(memory, "summary.total-swap"));
            result.put("bootAfterNs", SystemClock.elapsedRealtimeNanos());
        } catch (Exception error) {
            throw new IOException("Android process memory JSON failed", error);
        }
        return result;
    }

    private static long memoryStatKb(Debug.MemoryInfo memory, String key) {
        String value = memory.getMemoryStat(key);
        if (value == null || value.isEmpty()) {
            return -1L;
        }
        try {
            return Long.parseLong(value);
        } catch (NumberFormatException ignored) {
            return -1L;
        }
    }

    private static long readProcessStarttime() throws IOException {



        String stat = new String(
                Files.readAllBytes(new File("/proc/self/stat").toPath()),
                StandardCharsets.UTF_8).trim();
        int commandEnd = stat.lastIndexOf(')');
        if (commandEnd < 0 || commandEnd + 2 >= stat.length()) {
            throw new IOException("malformed /proc/self/stat");
        }

        String[] suffix = stat.substring(commandEnd + 2).trim().split("\\s+");
        if (suffix.length <= 19) {
            throw new IOException("short /proc/self/stat");
        }
        try {
            return Long.parseUnsignedLong(suffix[19]);
        } catch (NumberFormatException error) {
            throw new IOException("invalid /proc/self/stat starttime", error);
        }
    }

    private static void publishWithoutOverwrite(File destination, byte[] bytes)
            throws IOException {
        File parent = destination.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs())) {
            throw new IOException("cannot create snapshot directory: " + parent);
        }
        if (destination.exists()) {
            throw new IOException("request token already exists");
        }
        File temporary = File.createTempFile(destination.getName(), ".stage", parent);
        try {
            try (FileOutputStream output = new FileOutputStream(temporary, false)) {
                output.write(bytes);
                output.getFD().sync();
            }
            try {
                Files.move(temporary.toPath(), destination.toPath(),
                        StandardCopyOption.ATOMIC_MOVE);
            } catch (AtomicMoveNotSupportedException unsupported) {
                Files.move(temporary.toPath(), destination.toPath());
            }
        } finally {
            if (temporary.exists() && !temporary.delete()) {
                Log.w(TAG, "staging file could not be removed: " + temporary);
            }
        }
    }
}
