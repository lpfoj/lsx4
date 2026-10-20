package app.lsx4.android;

import android.app.ActivityManager;
import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.io.RandomAccessFile;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TimeZone;
import java.util.UUID;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

import javax.net.ssl.HttpsURLConnection;

final class RemoteDiagnostics {
    interface Callback {
        void complete(boolean success, String message);
    }

    static final String PREF_ENABLED = "remote_diagnostics_enabled";
    static final String PREF_LAST_REPORT = "remote_diagnostics_last_report";
    static final String PREF_LAST_ERROR = "remote_diagnostics_last_error";

    private static final String TAG = "LSX4.Diagnostics";
    private static final long MAX_LOG_BYTES = 4L * 1024L * 1024L;
    private static final long MAX_ZIP_INPUT_BYTES = 2L * 1024L * 1024L;
    private static final long STARTUP_UPLOAD_SECONDS = 8L;
    private static final long PERIODIC_UPLOAD_SECONDS = 20L;
    private static final int MAX_PENDING_REPORTS = 5;
    private static final Object LOCK = new Object();
    private static volatile State state;
    private static volatile String titleId = "";
    private static volatile String platform = "";

    private RemoteDiagnostics() {
    }

    static boolean isConfigured() {
        return BuildConfig.LSX4_DIAGNOSTICS_URL.startsWith("https://")
                && !BuildConfig.LSX4_DIAGNOSTICS_UPLOAD_TOKEN.isEmpty();
    }

    static boolean isEnabled(Context context) {
        return isConfigured() && SettingsActivity.prefs(context).getBoolean(
                PREF_ENABLED, false);
    }

    static void initialize(Context context) {
        if (!isEnabled(context)) {
            return;
        }
        synchronized (LOCK) {
            if (state != null) {
                return;
            }
            State created = new State(context.getApplicationContext());
            state = created;
            created.start();
        }
    }

    static void setEnabled(Context context, boolean enabled) {
        SettingsActivity.prefs(context).edit().putBoolean(PREF_ENABLED, enabled).apply();
        if (enabled) {
            initialize(context);
            return;
        }
        synchronized (LOCK) {
            State current = state;
            state = null;
            if (current != null) {
                current.stop();
            }
        }
    }

    static void setGameContext(String gamePath, String gamePlatform) {
        platform = gamePlatform == null ? "" : safe(gamePlatform, 16);
        titleId = findTitleId(gamePath);
    }

    static void requestImmediateUpload(Context context, Callback callback) {
        if (!isConfigured()) {
            callback.complete(false, "Remote diagnostics are not configured in this build");
            return;
        }
        if (!isEnabled(context)) {
            setEnabled(context, true);
        }
        State current = state;
        if (current == null) {
            callback.complete(false, "Diagnostics collector did not start");
            return;
        }
        current.executor.execute(() -> {
            UploadResult result = current.snapshotAndUpload("manual", true);
            callback.complete(result.success, result.message);
        });
    }

    private static final class State {
        final Context context;
        final String processName;
        final String sessionId;
        final String installId;
        final File processDir;
        final File pendingDir;
        final File logFile;
        final File previousLogFile;
        final ScheduledExecutorService executor;
        final Object writerLock = new Object();
        volatile boolean running = true;
        Process logcat;
        BufferedWriter logWriter;
        int linesSinceFlush;
        Thread.UncaughtExceptionHandler previousExceptionHandler;

        State(Context context) {
            this.context = context;
            processName = currentProcessName(context);
            sessionId = timestamp() + "-" + UUID.randomUUID().toString().substring(0, 8);
            installId = installId(context);
            File root = new File(context.getFilesDir(), "remote-diagnostics");
            processDir = new File(root, safe(processName, 48));
            pendingDir = new File(processDir, "pending");
            processDir.mkdirs();
            pendingDir.mkdirs();
            logFile = new File(processDir, sessionId + ".log");
            previousLogFile = new File(processDir, sessionId + ".previous.log");
            executor = Executors.newSingleThreadScheduledExecutor(runnable -> {
                Thread thread = new Thread(runnable, "lsx4-diag-upload");
                thread.setDaemon(true);
                return thread;
            });
        }

        void start() {
            append("session-start process=" + processName + " pid="
                    + android.os.Process.myPid() + " build=" + BuildConfig.VERSION_NAME);
            installExceptionHandler();
            startLogcat();
            executor.schedule(this::uploadPreviousSession, 2, TimeUnit.SECONDS);
            executor.schedule(() -> snapshotAndUpload("startup", false),
                    STARTUP_UPLOAD_SECONDS, TimeUnit.SECONDS);
            executor.scheduleAtFixedRate(
                    () -> snapshotAndUpload("periodic", false),
                    STARTUP_UPLOAD_SECONDS + PERIODIC_UPLOAD_SECONDS,
                    PERIODIC_UPLOAD_SECONDS, TimeUnit.SECONDS);
        }

        void stop() {
            running = false;
            executor.shutdownNow();
            if (logcat != null) {
                logcat.destroy();
            }
            synchronized (writerLock) {
                closeWriter();
            }
        }

        void installExceptionHandler() {
            previousExceptionHandler = Thread.getDefaultUncaughtExceptionHandler();
            Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
                File crash = new File(processDir, "last-java-crash.txt");
                try (PrintWriter writer = new PrintWriter(new FileWriter(crash, false))) {
                    writer.println("time=" + timestamp());
                    writer.println("thread=" + thread.getName());
                    error.printStackTrace(writer);
                } catch (Exception ignored) {
                }
                append("uncaught-exception thread=" + thread.getName() + " error=" + error);
                Thread.UncaughtExceptionHandler previous = previousExceptionHandler;
                if (previous != null) {
                    previous.uncaughtException(thread, error);
                }
            });
        }

        void startLogcat() {
            Thread collector = new Thread(() -> {
                try {
                    logcat = new ProcessBuilder(
                            "logcat", "-v", "threadtime", "-T", "1",
                            "LSX4:V", "LSX4Native:V", "LSX4AudioProbe:V",
                            "LSX4.Diagnostics:V",
                            "AndroidRuntime:E", "libc:F", "DEBUG:F", "*:S")
                            .redirectErrorStream(true)
                            .start();
                    try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                            logcat.getInputStream(), StandardCharsets.UTF_8))) {
                        String line;
                        while (running && (line = reader.readLine()) != null) {
                            append(line);
                        }
                    }
                } catch (Exception error) {
                    append("logcat-collector-error=" + error);
                }
            }, "lsx4-diag-logcat");
            collector.setDaemon(true);
            collector.start();
        }

        void append(String line) {
            synchronized (writerLock) {
                try {
                    if (logWriter == null) {
                        logWriter = new BufferedWriter(new FileWriter(logFile, true), 32 * 1024);
                    }
                    logWriter.write(line == null ? "" : line);
                    logWriter.newLine();
                    if (++linesSinceFlush >= 8) {
                        logWriter.flush();
                        linesSinceFlush = 0;
                    }
                    if (logFile.length() > MAX_LOG_BYTES) {
                        closeWriter();
                        previousLogFile.delete();
                        logFile.renameTo(previousLogFile);
                    }
                } catch (Exception ignored) {
                }
            }
        }

        void closeWriter() {
            if (logWriter == null) {
                return;
            }
            try {
                logWriter.flush();
                logWriter.close();
            } catch (Exception ignored) {
            }
            logWriter = null;
            linesSinceFlush = 0;
        }

        void uploadPreviousSession() {
            File[] logs = processDir.listFiles((directory, name) ->
                    name.endsWith(".log") && !name.equals(logFile.getName())
                            && !name.equals(previousLogFile.getName()));
            File crash = new File(processDir, "last-java-crash.txt");
            if ((logs == null || logs.length == 0) && !crash.isFile()) {
                retryPending();
                return;
            }
            UploadResult result = snapshotAndUpload("previous-session", false);
            if (result.success) {
                if (logs != null) {
                    for (File old : logs) {
                        old.delete();
                    }
                }
                crash.delete();
            }
        }

        UploadResult snapshotAndUpload(String reason, boolean allProcesses) {
            synchronized (writerLock) {
                if (logWriter != null) {
                    try {
                        logWriter.flush();
                    } catch (Exception ignored) {
                    }
                }
            }
            retryPending();
            File bundle;
            try {
                bundle = createBundle(reason, allProcesses);
            } catch (Exception error) {
                return remember(false, "Cannot build diagnostics: " + error.getMessage());
            }
            UploadResult result = upload(bundle, reason);
            if (result.success) {
                bundle.delete();
            }
            trimPending();
            return remember(result.success, result.message);
        }

        UploadResult remember(boolean success, String message) {
            SharedPreferences.Editor editor = SettingsActivity.prefs(context).edit();
            if (success) {
                editor.putString(PREF_LAST_REPORT, message).remove(PREF_LAST_ERROR);
            } else {
                editor.putString(PREF_LAST_ERROR, message);
            }
            editor.apply();
            return new UploadResult(success, message);
        }

        File createBundle(String reason, boolean allProcesses) throws Exception {
            File bundle = new File(pendingDir,
                    timestamp() + "-" + safe(reason, 24) + "-" + UUID.randomUUID()
                            .toString().substring(0, 8) + ".zip");
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(bundle))) {
                zip.setLevel(Deflater.BEST_SPEED);
                addBytes(zip, "report.json", reportJson(reason).toString(2)
                        .getBytes(StandardCharsets.UTF_8));
                addFile(zip, logFile, "logs/current.log", MAX_ZIP_INPUT_BYTES);
                addFile(zip, previousLogFile, "logs/previous.log", MAX_ZIP_INPUT_BYTES);
                addFile(zip, new File(processDir, "last-java-crash.txt"),
                        "logs/last-java-crash.txt", 512 * 1024L);
                addStaleSessionLogs(zip);
                if (allProcesses) {
                    addOtherProcessLogs(zip);
                }
                addFile(zip, new File(context.getFilesDir(), "jit-status.json"),
                        "runtime/jit-status.json", 1024 * 1024L);
                addFile(zip, new File(context.getFilesDir(),
                                "guest-cpu-backend-status.json"),
                        "runtime/guest-cpu-backend-status.json", 256 * 1024L);
                addFile(zip, new File(context.getFilesDir(), "runtime-config.json"),
                        "runtime/runtime-config.json", 512 * 1024L);
                addFile(zip, new File(context.getFilesDir(), "executor-runtime-death.log"),
                        "runtime/executor-runtime-death.log", 1024 * 1024L);
                addFile(zip, new File(context.getFilesDir(), "executor-jit-watchdog.log"),
                        "runtime/executor-jit-watchdog.log", 1024 * 1024L);
                addTextFile(zip, new File("/proc/self/status"), "proc/self-status.txt", 256 * 1024L);
                addTextFile(zip, new File("/proc/self/smaps_rollup"),
                        "proc/self-smaps-rollup.txt", 256 * 1024L);
                addTextFile(zip, new File("/proc/self/oom_score"),
                        "proc/self-oom-score.txt", 16 * 1024L);
                addTextFile(zip, new File("/proc/self/oom_score_adj"),
                        "proc/self-oom-score-adj.txt", 16 * 1024L);
                addTextFile(zip, new File("/proc/pressure/memory"),
                        "proc/memory-pressure.txt", 64 * 1024L);
                addTextFile(zip, new File("/proc/self/maps"), "proc/self-maps.txt", 1024 * 1024L);
                addTextFile(zip, new File("/proc/cpuinfo"), "proc/cpuinfo.txt", 512 * 1024L);
                addTextFile(zip, new File("/proc/meminfo"), "proc/meminfo.txt", 256 * 1024L);
                addRuntimeLogs(zip);
            }
            return bundle;
        }

        void addStaleSessionLogs(ZipOutputStream zip) throws Exception {
            File[] logs = processDir.listFiles((directory, name) -> name.endsWith(".log")
                    && !name.equals(logFile.getName())
                    && !name.equals(previousLogFile.getName()));
            if (logs == null) {
                return;
            }
            java.util.Arrays.sort(logs, (left, right) ->
                    Long.compare(right.lastModified(), left.lastModified()));
            for (int i = 0; i < Math.min(2, logs.length); ++i) {
                addFile(zip, logs[i], "logs/stale-" + i + ".log", MAX_ZIP_INPUT_BYTES);
            }
        }

        void addOtherProcessLogs(ZipOutputStream zip) throws Exception {
            File root = processDir.getParentFile();
            File[] directories = root == null ? null : root.listFiles(File::isDirectory);
            if (directories == null) {
                return;
            }
            for (File directory : directories) {
                if (directory.equals(processDir)) {
                    continue;
                }
                File[] logs = directory.listFiles((parent, name) -> name.endsWith(".log"));
                if (logs == null) {
                    continue;
                }
                int start = Math.max(0, logs.length - 2);
                for (int i = start; i < logs.length; ++i) {
                    addFile(zip, logs[i], "other/" + safe(directory.getName(), 48) + "/"
                            + safe(logs[i].getName(), 80), MAX_ZIP_INPUT_BYTES);
                }
            }
        }

        void addRuntimeLogs(ZipOutputStream zip) throws Exception {
            File nativeLog = new File(context.getFilesDir(), "lsx4-home/native/log");
            File[] files = nativeLog.listFiles(file -> file.isFile()
                    && (file.getName().endsWith(".log") || file.getName().endsWith(".txt")
                    || file.getName().endsWith(".json")));
            if (files == null) {
                return;
            }
            int count = 0;
            for (int i = files.length - 1; i >= 0 && count < 8; --i) {
                addFile(zip, files[i], "runtime/native-log/" + safe(files[i].getName(), 96),
                        512 * 1024L);
                count++;
            }
        }

        JSONObject reportJson(String reason) throws Exception {
            JSONObject report = new JSONObject();
            report.put("schema", 1);
            report.put("timestamp", timestamp());
            report.put("reason", reason);
            report.put("install_id", installId);
            report.put("session_id", sessionId);
            report.put("process", processName);
            report.put("pid", android.os.Process.myPid());
            report.put("package", context.getPackageName());
            report.put("version_name", BuildConfig.VERSION_NAME);
            report.put("version_code", BuildConfig.VERSION_CODE);
            report.put("title_id", titleId);
            report.put("platform", platform);
            report.put("manufacturer", Build.MANUFACTURER);
            report.put("model", Build.MODEL);
            report.put("device", Build.DEVICE);
            report.put("sdk", Build.VERSION.SDK_INT);
            report.put("release", Build.VERSION.RELEASE);
            report.put("fingerprint", Build.FINGERPRINT);
            report.put("available_bytes", context.getFilesDir().getUsableSpace());
            ActivityManager manager = (ActivityManager) context.getSystemService(
                    Context.ACTIVITY_SERVICE);
            if (manager != null) {
                ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
                manager.getMemoryInfo(memory);
                JSONObject memoryJson = new JSONObject();
                memoryJson.put("available", memory.availMem);
                memoryJson.put("threshold", memory.threshold);
                memoryJson.put("low", memory.lowMemory);
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN) {
                    memoryJson.put("total", memory.totalMem);
                }
                Runtime runtime = Runtime.getRuntime();
                memoryJson.put("java_used", runtime.totalMemory() - runtime.freeMemory());
                memoryJson.put("java_committed", runtime.totalMemory());
                memoryJson.put("java_max", runtime.maxMemory());
                report.put("memory", memoryJson);
            }
            report.put("settings", settingsJson(context));
            return report;
        }

        void retryPending() {
            File[] pending = pendingDir.listFiles((directory, name) -> name.endsWith(".zip"));
            if (pending == null) {
                return;
            }
            for (File file : pending) {
                UploadResult result = upload(file, "retry");
                if (result.success) {
                    file.delete();
                } else {
                    break;
                }
            }
        }

        void trimPending() {
            File[] pending = pendingDir.listFiles((directory, name) -> name.endsWith(".zip"));
            if (pending == null || pending.length <= MAX_PENDING_REPORTS) {
                return;
            }
            java.util.Arrays.sort(pending, (left, right) ->
                    Long.compare(right.lastModified(), left.lastModified()));
            for (int i = MAX_PENDING_REPORTS; i < pending.length; ++i) {
                pending[i].delete();
            }
        }

        UploadResult upload(File bundle, String reason) {
            if (!bundle.isFile() || bundle.length() <= 0) {
                return new UploadResult(false, "Diagnostics bundle is empty");
            }
            HttpsURLConnection connection = null;
            try {
                connection = (HttpsURLConnection) new URL(
                        BuildConfig.LSX4_DIAGNOSTICS_URL).openConnection();
                connection.setConnectTimeout(15_000);
                connection.setReadTimeout(30_000);
                connection.setRequestMethod("POST");
                connection.setDoOutput(true);
                connection.setFixedLengthStreamingMode(bundle.length());
                connection.setRequestProperty("Content-Type", "application/zip");
                connection.setRequestProperty("Authorization", "Bearer "
                        + BuildConfig.LSX4_DIAGNOSTICS_UPLOAD_TOKEN);
                connection.setRequestProperty("X-LSX4-Install", installId);
                connection.setRequestProperty("X-LSX4-Process", safe(processName, 48));
                connection.setRequestProperty("X-LSX4-Session", sessionId);
                connection.setRequestProperty("X-LSX4-Reason", safe(reason, 24));
                connection.setRequestProperty("X-LSX4-Build", BuildConfig.VERSION_NAME + "-"
                        + BuildConfig.VERSION_CODE);
                connection.setRequestProperty("X-LSX4-Device",
                        safe(Build.MANUFACTURER + "-" + Build.MODEL + "-api"
                                + Build.VERSION.SDK_INT, 160));
                try (InputStream input = new FileInputStream(bundle);
                     OutputStream output = connection.getOutputStream()) {
                    copy(input, output);
                }
                int status = connection.getResponseCode();
                InputStream response = status >= 200 && status < 300
                        ? connection.getInputStream() : connection.getErrorStream();
                String body = readString(response, 64 * 1024L);
                if (status < 200 || status >= 300) {
                    return new UploadResult(false, "Upload HTTP " + status + ": " + body);
                }
                String reportId = new JSONObject(body).optString("report_id", "uploaded");
                Log.i(TAG, "Remote diagnostics uploaded report=" + reportId);
                return new UploadResult(true, reportId);
            } catch (Exception error) {
                Log.w(TAG, "Remote diagnostics upload failed", error);
                return new UploadResult(false, error.getClass().getSimpleName() + ": "
                        + String.valueOf(error.getMessage()));
            } finally {
                if (connection != null) {
                    connection.disconnect();
                }
            }
        }
    }

    private static JSONObject settingsJson(Context context) throws Exception {
        JSONObject output = new JSONObject();
        for (Map.Entry<String, ?> entry : SettingsActivity.prefs(context).getAll().entrySet()) {
            Object value = entry.getValue();
            if (value instanceof Set) {
                JSONArray array = new JSONArray();
                for (Object item : (Set<?>) value) {
                    array.put(String.valueOf(item));
                }
                output.put(entry.getKey(), array);
            } else {
                output.put(entry.getKey(), value);
            }
        }
        return output;
    }

    private static void addTextFile(ZipOutputStream zip, File file, String name, long limit)
            throws Exception {
        try (InputStream input = new FileInputStream(file)) {
            zip.putNextEntry(new ZipEntry(name));
            byte[] buffer = new byte[64 * 1024];
            long remaining = limit;
            int count;
            while (remaining > 0 && (count = input.read(
                    buffer, 0, (int) Math.min(buffer.length, remaining))) >= 0) {
                zip.write(buffer, 0, count);
                remaining -= count;
            }
            zip.closeEntry();
        } catch (Exception ignored) {
        }
    }

    private static void addFile(ZipOutputStream zip, File file, String name, long limit)
            throws Exception {
        if (file == null || !file.isFile() || file.length() <= 0) {
            return;
        }
        ZipEntry entry = new ZipEntry(name);
        entry.setTime(file.lastModified());
        zip.putNextEntry(entry);
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            long remaining = Math.min(input.length(), limit);
            input.seek(Math.max(0L, input.length() - remaining));
            byte[] buffer = new byte[64 * 1024];
            while (remaining > 0) {
                int read = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
                if (read < 0) {
                    break;
                }
                zip.write(buffer, 0, read);
                remaining -= read;
            }
        }
        zip.closeEntry();
    }

    private static void addBytes(ZipOutputStream zip, String name, byte[] bytes) throws Exception {
        zip.putNextEntry(new ZipEntry(name));
        try (InputStream input = new ByteArrayInputStream(bytes)) {
            copy(input, zip);
        }
        zip.closeEntry();
    }

    private static void copy(InputStream input, OutputStream output) throws Exception {
        byte[] buffer = new byte[128 * 1024];
        int count;
        while ((count = input.read(buffer)) >= 0) {
            output.write(buffer, 0, count);
        }
    }

    private static String readString(InputStream input, long limit) throws Exception {
        if (input == null) {
            return "";
        }
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        long remaining = limit;
        int count;
        while (remaining > 0 && (count = input.read(
                buffer, 0, (int) Math.min(buffer.length, remaining))) >= 0) {
            output.write(buffer, 0, count);
            remaining -= count;
        }
        return output.toString(StandardCharsets.UTF_8.name());
    }

    private static String installId(Context context) {
        try {
            String androidId = Settings.Secure.getString(
                    context.getContentResolver(), Settings.Secure.ANDROID_ID);
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] value = digest.digest((context.getPackageName() + ":" + androidId)
                    .getBytes(StandardCharsets.UTF_8));
            StringBuilder result = new StringBuilder();
            for (int i = 0; i < 12; ++i) {
                result.append(String.format(Locale.US, "%02x", value[i]));
            }
            return result.toString();
        } catch (Exception ignored) {
            return "anonymous";
        }
    }

    private static String currentProcessName(Context context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            String name = Application.getProcessName();
            if (name != null && !name.isEmpty()) {
                return name.replace(context.getPackageName(), "app");
            }
        }
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/self/cmdline"))) {
            String name = reader.readLine();
            return name == null ? "process" : name.replace(context.getPackageName(), "app");
        } catch (Exception ignored) {
            return "process";
        }
    }

    private static String findTitleId(String path) {
        if (path == null) {
            return "";
        }
        java.util.regex.Matcher matcher = java.util.regex.Pattern
                .compile("(?i)(CUSA|PPSA)[0-9]{5}").matcher(path);
        return matcher.find() ? matcher.group().toUpperCase(Locale.US) : "";
    }

    private static String safe(String value, int limit) {
        if (value == null) {
            return "";
        }
        String cleaned = value.replaceAll("[^A-Za-z0-9_.:-]+", "-");
        return cleaned.substring(0, Math.min(cleaned.length(), limit));
    }

    private static String timestamp() {
        SimpleDateFormat format = new SimpleDateFormat("yyyyMMdd'T'HHmmss.SSS'Z'", Locale.US);
        format.setTimeZone(TimeZone.getTimeZone("UTC"));
        return format.format(new Date());
    }

    private static final class UploadResult {
        final boolean success;
        final String message;

        UploadResult(boolean success, String message) {
            this.success = success;
            this.message = message;
        }
    }
}
