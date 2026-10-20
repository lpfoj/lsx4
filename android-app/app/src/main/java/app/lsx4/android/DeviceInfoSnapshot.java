package app.lsx4.android;

import android.app.ActivityManager;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Build;
import android.os.PowerManager;
import android.os.StatFs;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;


final class DeviceInfoSnapshot {
    static final class Line {
        final String label;
        final String value;

        Line(String label, String value) {
            this.label = label;
            this.value = value;
        }
    }

    private DeviceInfoSnapshot() {}

    static List<Line> collect(Context context) {
        List<Line> lines = new ArrayList<>();
        String model = (Build.MANUFACTURER + " " + Build.MODEL).trim();
        lines.add(new Line(context.getString(R.string.home_info_model), model));

        String soc = Build.HARDWARE;
        if (Build.VERSION.SDK_INT >= 31) {
            String socModel = Build.SOC_MODEL;
            if (socModel != null && !socModel.isEmpty()
                    && !Build.UNKNOWN.equalsIgnoreCase(socModel)) {
                soc = (Build.SOC_MANUFACTURER + " " + socModel).trim();
            }
        }
        lines.add(new Line(context.getString(R.string.home_info_soc), soc));

        int cores = Runtime.getRuntime().availableProcessors();
        long maxKhz = maxCpuFrequencyKhz(cores);
        String cpu = maxKhz > 0
                ? context.getString(R.string.home_info_cores_frequency, cores,
                        String.format(Locale.ROOT, "%.2f", maxKhz / 1_000_000.0))
                : context.getString(R.string.home_info_cores, cores);
        lines.add(new Line(context.getString(R.string.home_info_cpu), cpu));

        ActivityManager activityManager = context.getSystemService(ActivityManager.class);
        if (activityManager != null) {
            ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
            activityManager.getMemoryInfo(memory);
            lines.add(new Line(context.getString(R.string.home_info_ram),
                    context.getString(R.string.home_info_free_of,
                            gib(memory.availMem), gib(memory.totalMem))));
        }

        try {
            StatFs storage = new StatFs(context.getFilesDir().getAbsolutePath());
            lines.add(new Line(context.getString(R.string.home_info_storage),
                    context.getString(R.string.home_info_free_of,
                            gib(storage.getAvailableBytes()), gib(storage.getTotalBytes()))));
        } catch (IllegalArgumentException ignored) {
        }

        lines.add(new Line(context.getString(R.string.home_info_android),
                Build.VERSION.RELEASE + " · API " + Build.VERSION.SDK_INT));

        Intent battery = context.registerReceiver(null,
                new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (battery != null) {
            int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
            int scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
            int tenths = battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
            StringBuilder value = new StringBuilder();
            if (level >= 0 && scale > 0) {
                value.append(Math.round(level * 100f / scale)).append('%');
            }
            if (tenths != Integer.MIN_VALUE) {
                if (value.length() > 0) value.append(" · ");
                value.append(String.format(Locale.ROOT, "%.1f °C", tenths / 10f));
            }
            if (value.length() > 0) {
                lines.add(new Line(context.getString(R.string.home_info_battery),
                        value.toString()));
            }
        }

        if (Build.VERSION.SDK_INT >= 29) {
            PowerManager power = context.getSystemService(PowerManager.class);
            if (power != null) {
                lines.add(new Line(context.getString(R.string.home_info_thermal),
                        thermalLabel(context, power.getCurrentThermalStatus())));
            }
        }

        try {
            String version = context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0).versionName;
            lines.add(new Line(context.getString(R.string.home_info_app),
                    version == null ? "" : version));
        } catch (Exception ignored) {
        }
        return lines;
    }

    private static String gib(long bytes) {
        return String.format(Locale.ROOT, "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    }

    private static long maxCpuFrequencyKhz(int cores) {
        long best = 0;
        for (int cpu = 0; cpu < Math.max(cores, 1); cpu++) {
            File file = new File("/sys/devices/system/cpu/cpu" + cpu
                    + "/cpufreq/cpuinfo_max_freq");
            try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
                String line = reader.readLine();
                if (line != null) {
                    best = Math.max(best, Long.parseLong(line.trim()));
                }
            } catch (Exception ignored) {
            }
        }
        return best;
    }

    private static String thermalLabel(Context context, int status) {
        switch (status) {
        case PowerManager.THERMAL_STATUS_NONE:
            return context.getString(R.string.home_thermal_none);
        case PowerManager.THERMAL_STATUS_LIGHT:
            return context.getString(R.string.home_thermal_light);
        case PowerManager.THERMAL_STATUS_MODERATE:
            return context.getString(R.string.home_thermal_moderate);
        case PowerManager.THERMAL_STATUS_SEVERE:
            return context.getString(R.string.home_thermal_severe);
        default:
            return context.getString(R.string.home_thermal_critical);
        }
    }
}
