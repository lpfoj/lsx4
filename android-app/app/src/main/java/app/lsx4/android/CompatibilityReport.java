package app.lsx4.android;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;


final class CompatibilityReport {
    static final int MAX_BYTES = 4096;

    static JSONObject create(Context context, String title, String titleId, String platform,
                             GameCompatibilityStatus status, String[] graphics) throws JSONException {
        if (status == GameCompatibilityStatus.UNCLASSIFIED) throw new IllegalArgumentException("No result");
        Map<String, String> facts = new HashMap<>();
        if (graphics != null) for (int i = 0; i + 1 < graphics.length; i += 2) {
            facts.put(graphics[i], graphics[i + 1]);
        }
        JSONObject device = new JSONObject();
        device.put("manufacturer", known(Build.MANUFACTURER));
        device.put("model", known(Build.MODEL));
        device.put("android_version", known(Build.VERSION.RELEASE));
        device.put("android_api", Build.VERSION.SDK_INT);
        device.put("soc", Build.VERSION.SDK_INT >= 31 ? known(Build.SOC_MODEL) : JSONObject.NULL);
        device.put("cpu_abi", Build.SUPPORTED_ABIS.length > 0 ? known(Build.SUPPORTED_ABIS[0]) : JSONObject.NULL);
        device.put("gpu_renderer", known(facts.get("gl.renderer")));
        device.put("gpu_vendor", known(facts.get("gl.vendor")));
        device.put("vulkan_device", known(facts.get("vk.device")));
        device.put("vulkan_api_version", known(facts.get("vk.api")));
        device.put("vulkan_driver_version", known(facts.get("vk.driver")));
        Object ram = JSONObject.NULL;
        try {
            ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
            if (manager != null) {
                manager.getMemoryInfo(memory);
                if (memory.totalMem > 0) ram = memory.totalMem / (1024 * 1024);
            }
        } catch (RuntimeException unavailable) { }
        device.put("ram_total_mib", ram);
        return new JSONObject().put("schema_version", 1)
                .put("client_report_id", UUID.randomUUID().toString())
                .put("game", new JSONObject().put("title", title).put("title_id", titleId)
                        .put("platform", platform))
                .put("result", status == GameCompatibilityStatus.SUPPORTED ? "supported" : "unsupported")
                .put("app_version", BuildConfig.VERSION_NAME).put("device", device);
    }

    private static Object known(String value) {
        return value == null || value.trim().isEmpty() || "unknown".equalsIgnoreCase(value)
                ? JSONObject.NULL : value;
    }

    private CompatibilityReport() { }
}
