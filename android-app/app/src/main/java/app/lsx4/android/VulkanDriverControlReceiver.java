package app.lsx4.android;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;


public final class VulkanDriverControlReceiver extends BroadcastReceiver {
    static final String ACTION_SELECT_IMPORTED =
            "app.lsx4.android.SELECT_IMPORTED_VULKAN_DRIVER";
    private static final String TAG = "LSX4DriverControl";
    private static final String RESULT_SCHEMA = "lsx4.vulkan-driver-control.v1";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION_SELECT_IMPORTED.equals(intent.getAction())) {
            return;
        }
        PendingResult pendingResult = goAsync();
        Context appContext = context.getApplicationContext();
        String token = sanitizeToken(intent.getStringExtra("request_token"));
        String sha256 = sanitizeSha256(intent.getStringExtra("sha256"));
        Thread worker = new Thread(() -> {
            JSONObject result = new JSONObject();
            try {
                result.put("schema", RESULT_SCHEMA);
                result.put("requestToken", token);
                result.put("sha256", sha256);
                if (token.isEmpty()) {
                    throw new IllegalArgumentException("Missing or invalid request_token");
                }
                if (sha256.isEmpty()) {
                    throw new IllegalArgumentException("sha256 must contain 64 hexadecimal digits");
                }
                String selectionId = VulkanDriverManager.SELECTION_IMPORTED_PREFIX + sha256;
                VulkanDriverManager.selectDriver(
                        appContext, SettingsActivity.prefs(appContext), selectionId);
                String selected = VulkanDriverManager.readSelectionId(
                        appContext, SettingsActivity.prefs(appContext));
                if (!selectionId.equals(selected)) {
                    throw new IllegalStateException(
                            "Driver selection did not persist: " + selected);
                }
                result.put("success", true);
                result.put("selectionId", selected);
                Log.i(TAG, "Selected verified imported Vulkan driver sha=" + sha256);
            } catch (Exception error) {
                try {
                    result.put("success", false);
                    result.put("error", friendlyMessage(error));
                } catch (Exception ignored) {
                }
                Log.e(TAG, "Imported Vulkan driver selection failed", error);
            } finally {
                if (!token.isEmpty()) {
                    writeResult(appContext, token, result);
                }
                pendingResult.finish();
            }
        }, "lsx4-driver-control");
        worker.setDaemon(true);
        worker.start();
    }

    private static String sanitizeSha256(String raw) {
        String value = raw == null ? "" : raw.trim().toLowerCase(Locale.ROOT);
        return value.matches("[0-9a-f]{64}") ? value : "";
    }

    private static String sanitizeToken(String raw) {
        String value = raw == null ? "" : raw.trim();
        return value.length() <= 64 && value.matches("[A-Za-z0-9._-]+") ? value : "";
    }

    private static String friendlyMessage(Exception error) {
        String message = error.getMessage();
        return message == null || message.trim().isEmpty()
                ? error.getClass().getSimpleName() : message.trim();
    }

    private static void writeResult(Context context, String token, JSONObject result) {
        File directory = new File(context.getFilesDir(), "vulkan-drivers");
        File destination = new File(directory, "control-result-" + token + ".json");
        File temporary = new File(directory, "control-result-" + token + ".tmp");
        try {
            if (!directory.isDirectory() && !directory.mkdirs()) {
                throw new IllegalStateException("Could not create Vulkan driver directory");
            }
            byte[] bytes = (result.toString() + "\n").getBytes(StandardCharsets.UTF_8);
            try (FileOutputStream output = new FileOutputStream(temporary, false)) {
                output.write(bytes);
                output.flush();
                output.getFD().sync();
            }
            if (destination.exists() && !destination.delete()) {
                throw new IllegalStateException("Could not replace prior control result");
            }
            if (!temporary.renameTo(destination)) {
                throw new IllegalStateException("Could not publish control result");
            }
        } catch (Exception error) {
            Log.e(TAG, "Could not write Vulkan driver control result", error);
            if (temporary.exists() && !temporary.delete()) {
                Log.w(TAG, "Could not remove temporary control result " + temporary);
            }
        }
    }
}
