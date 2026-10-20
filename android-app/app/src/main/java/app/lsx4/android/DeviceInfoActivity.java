package app.lsx4.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.os.StatFs;
import android.provider.Settings;
import android.view.KeyEvent;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.GridLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;


public final class DeviceInfoActivity extends Activity {
    static final String EXTRA_THEME = "device_info_theme";
    private static final int[] CATEGORIES = {
        R.string.system_info_category_system, R.string.system_info_category_gpu,
        R.string.system_info_category_vulkan, R.string.system_info_category_temperatures,
        R.string.system_info_category_cpu, R.string.system_info_category_memory
    };

    private static final class Row {
        final int category;
        final String label, value;
        final boolean expandable;
        final int progress;
        final int color;
        Row(int category, String label, String value, boolean expandable) {
            this(category, label, value, expandable, -1, 0xffaaddff);
        }
        Row(int category, String label, String value, boolean expandable, int progress) {
            this(category, label, value, expandable, progress, 0xffaaddff);
        }
        Row(int category, String label, String value, boolean expandable, int progress, int color) {
            this.category = category;
            this.label = label;
            this.value = value;
            this.expandable = expandable;
            this.progress = progress;
            this.color = color;
        }
    }

    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final List<TextView> tabs = new ArrayList<>();
    private List<Row> rows = new ArrayList<>();
    private LinearLayout content;
    private ScrollView scroll;
    private TextView timestamp;
    private TextView refresh;
    private TextView categoryTitle;
    private DeviceInfoBackground background;
    private int category;
    private int accent = 0xff88ccff;
    private int generation;
    private volatile boolean closed;
    private boolean portrait;
    private MenuSoundEngine sounds;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        portrait = getResources().getConfiguration().orientation
                != android.content.res.Configuration.ORIENTATION_LANDSCAPE;
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR);
        int theme = Math.max(0, Math.min(3, getIntent().getIntExtra(EXTRA_THEME, 0)));
        accent = PlayStationHomeView.THEMES[theme].primary;
        sounds = new MenuSoundEngine(this, getSharedPreferences("lsx4_library_layout", MODE_PRIVATE));
        if (state != null) category = Math.max(0, Math.min(5, state.getInt("category")));


        FrameLayout scene = new FrameLayout(this);
        background = new DeviceInfoBackground(this);
        background.setTheme(theme);
        scene.addView(background, new FrameLayout.LayoutParams(-1, -1));
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(portrait ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        root.setPadding(dp(portrait ? 12 : 32), dp(24), dp(portrait ? 12 : 32), dp(24));
        ScrollView sidebar = new ScrollView(this);
        sidebar.setFillViewport(true);
        LinearLayout navigation = new LinearLayout(this);
        navigation.setOrientation(LinearLayout.VERTICAL);
        navigation.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        if (portrait) navigation.addView(actions);
        TextView back = action("‹  " + getString(R.string.back));
        back.setOnClickListener(v -> finish());
        (portrait ? actions : navigation).addView(back);
        GridLayout portraitTabs = new GridLayout(this);
        portraitTabs.setColumnCount(3);
        if (portrait) navigation.addView(portraitTabs, new LinearLayout.LayoutParams(-1, -2));
        String[] icons = {"▣", "◉", "▱", "♨", "▦", "▤"};
        for (int i = 0; i < CATEGORIES.length; i++) {
            final int selection = i;
            TextView tab = action(icons[i] + "   " + getString(CATEGORIES[i]));
            tab.setTextSize(portrait ? 13 : 16);
            tab.setOnClickListener(v -> selectCategory(selection));
            tabs.add(tab);
            if (portrait) {
                GridLayout.LayoutParams tabLayout = new GridLayout.LayoutParams(
                        GridLayout.spec(i / 3), GridLayout.spec(i % 3, 1f));
                tabLayout.width = 0; tabLayout.height = -2;
                portraitTabs.addView(tab, tabLayout);
            } else navigation.addView(tab, new LinearLayout.LayoutParams(-1, -2));
        }
        if (portrait) {
            root.addView(navigation, new LinearLayout.LayoutParams(-1, -2));
        } else {
            sidebar.addView(navigation, new ScrollView.LayoutParams(-1, -1));
            root.addView(sidebar, new LinearLayout.LayoutParams(dp(200), -1));
        }
        LinearLayout details = new LinearLayout(this);
        details.setPadding(dp(portrait ? 0 : 32), dp(portrait ? 12 : 0), 0, 0);
        details.setOrientation(LinearLayout.VERTICAL);
        LinearLayout header = new LinearLayout(this);
        categoryTitle = text(getString(CATEGORIES[category]), 28, 0xff88ddff);
        categoryTitle.setShadowLayer(dp(8), 0, 0, 0x884488ff);
        header.addView(categoryTitle, new LinearLayout.LayoutParams(0, -2, 1));
        refresh = action(getString(R.string.device_info_refresh));
        refresh.setOnClickListener(v -> loadSnapshot());
        header.addView(refresh);
        details.addView(header);
        timestamp = text(getString(R.string.device_info_loading), 11, 0xff6688aa);
        details.addView(timestamp);
        scroll = new ScrollView(this);
        content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(content);
        details.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        root.addView(details, new LinearLayout.LayoutParams(portrait ? -1 : 0, portrait ? 0 : -1, 1));
        scene.addView(root, new FrameLayout.LayoutParams(-1, -1));
        setContentView(scene);
        selectCategory(category);
        loadSnapshot();
    }

    @Override protected void onResume() {
        super.onResume();
        if (background != null) background.setActive(true);
        if (sounds != null) sounds.setAmbientActive(true);
    }

    @Override protected void onPause() {
        if (background != null) background.setActive(false);
        if (sounds != null) sounds.setAmbientActive(false);
        super.onPause();
    }

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus) return;
        if (Build.VERSION.SDK_INT >= 30) {
            getWindow().getInsetsController().hide(WindowInsets.Type.systemBars());
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BUTTON_B) { finish(); return true; }
        if (keyCode == KeyEvent.KEYCODE_BUTTON_L1 || keyCode == KeyEvent.KEYCODE_BUTTON_R1) {
            selectCategory((category + (keyCode == KeyEvent.KEYCODE_BUTTON_L1 ? 5 : 1)) % 6);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        state.putInt("category", category);
        super.onSaveInstanceState(state);
    }

    @Override protected void onDestroy() {
        closed = true;
        if (background != null) background.setActive(false);
        generation++;
        worker.shutdownNow();
        if (sounds != null) sounds.release();
        super.onDestroy();
    }

    private void loadSnapshot() {
        final int request = ++generation;
        refresh.setEnabled(false);
        timestamp.setText(R.string.device_info_loading);
        worker.execute(() -> {
            List<Row> collected = new ArrayList<>();
            try {
                collectSystem(collected);
                collectCpu(collected);
                collectMemory(collected);
                collectThermal(collected);
                if (!closed) collectGraphics(collected, DeviceGraphicsInfo.collect());
            } catch (RuntimeException error) {
                add(collected, 0, R.string.device_info_error, unknown());
            }
            final String capturedAt = DateFormat.getTimeInstance().format(new Date());
            runOnUiThread(() -> {
                if (closed || isFinishing() || isDestroyed() || request != generation) return;
                rows = collected;
                timestamp.setText(getString(R.string.device_info_snapshot, capturedAt));
                refresh.setEnabled(true);
                selectCategory(category);
            });
        });
    }

    private void selectCategory(int selected) {
        category = selected;
        categoryTitle.setText(CATEGORIES[selected]);
        for (int i = 0; i < tabs.size(); i++) {
            tabs.get(i).setTextColor(i == selected ? 0xff88ddff : 0xff7799bb);
            android.graphics.drawable.GradientDrawable highlight = new android.graphics.drawable.GradientDrawable();
            highlight.setCornerRadius(dp(8));
            highlight.setColor(i == selected ? 0x662244aa : 0x00000000);
            tabs.get(i).setBackground(highlight);
            tabs.get(i).setSelected(i == selected);
        }
        content.removeAllViews();
        for (Row row : rows) {
            if (row.category != selected) continue;
            TextView value = text(row.value, 14, row.color);
            if (row.category == 3 && row.value.endsWith(" °C")) {
                try {
                    double degrees = Double.parseDouble(row.value.replace(" °C", ""));
                    value.setTextColor(degrees > 70 ? 0xffff6666 : degrees > 55 ? 0xffffaa44
                        : degrees > 40 ? 0xffffdd44 : degrees > 25 ? 0xff44ddaa : 0xff44aaff);
                } catch (NumberFormatException ignored) { }
            }
            value.setTextIsSelectable(true);
            if (!row.expandable && row.label.equals(row.value)) {
                content.addView(value, new LinearLayout.LayoutParams(-1, -2));
                continue;
            }
            if (row.expandable) {
                TextView toggle = action("▸ " + row.label);
                value.setVisibility(View.GONE);
                toggle.setOnClickListener(v -> {
                    boolean expanded = value.getVisibility() != View.VISIBLE;
                    value.setVisibility(expanded ? View.VISIBLE : View.GONE);
                    toggle.setText((expanded ? "▾ " : "▸ ") + row.label);
                });
                content.addView(toggle, new LinearLayout.LayoutParams(-1, -2));
                content.addView(value, new LinearLayout.LayoutParams(-1, -2));
            } else {
                LinearLayout info = new LinearLayout(this);
                info.setGravity(Gravity.CENTER_VERTICAL);
                info.addView(text(row.label, 14, 0xff6688aa), new LinearLayout.LayoutParams(0, -2, 0.42f));
                value.setGravity(Gravity.END);
                info.addView(value, new LinearLayout.LayoutParams(0, -2, 0.58f));
                content.addView(info, new LinearLayout.LayoutParams(-1, -2));
            }
            if (row.progress >= 0) {
                ProgressBar usage = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
                usage.setMax(100);
                usage.setProgress(row.progress);
                usage.setProgressTintList(android.content.res.ColorStateList.valueOf(0xff44aaff));
                LinearLayout.LayoutParams bar = new LinearLayout.LayoutParams(-1, dp(8));
                bar.topMargin = dp(8);
                bar.bottomMargin = dp(8);
                content.addView(usage, bar);
            }
        }
        scroll.scrollTo(0, 0);
    }

    private void collectSystem(List<Row> out) {
        add(out, 0, R.string.system_info_device_label, Build.MANUFACTURER + " " + Build.MODEL);
        add(out, 0, R.string.system_info_android_label, Build.VERSION.RELEASE + " · API " + Build.VERSION.SDK_INT);
        add(out, 0, R.string.system_info_build_label, Build.DISPLAY);
        add(out, 0, R.string.system_info_kernel_label, System.getProperty("os.version"));
        add(out, 0, R.string.system_info_board_label, Build.BOARD);
        add(out, 0, R.string.system_info_hardware_label, Build.HARDWARE);
        add(out, 0, R.string.system_info_abis_label, String.join(", ", Build.SUPPORTED_ABIS));
        add(out, 0, R.string.device_info_security_patch, Build.VERSION.SECURITY_PATCH);
        add(out, 0, R.string.system_info_root_status, getString(R.string.device_info_root_unknown));
        String enforce = read("/sys/fs/selinux/enforce");
        add(out, 0, R.string.system_info_selinux,
            "1".equals(enforce) ? "Enforcing" : "0".equals(enforce) ? "Permissive" : unknown());
        add(out, 0, R.string.system_info_usb_debugging, globalSetting(Settings.Global.ADB_ENABLED));
        add(out, 0, R.string.system_info_developer_options,
            globalSetting(Settings.Global.DEVELOPMENT_SETTINGS_ENABLED));
        boolean emulator = Build.FINGERPRINT.startsWith("generic") || Build.MODEL.contains("Emulator")
            || Build.HARDWARE.equals("goldfish") || Build.HARDWARE.equals("ranchu");
        add(out, 0, R.string.system_info_environment,
            emulator ? getString(R.string.system_info_emulator_detected) : Build.PRODUCT);
    }

    private void collectCpu(List<Row> out) {
        String cpu = Build.HARDWARE;
        if (Build.VERSION.SDK_INT >= 31) cpu = Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL;
        add(out, 4, R.string.system_info_processor_label, cpu);
        int cores = Runtime.getRuntime().availableProcessors();
        File[] files = new File("/sys/devices/system/cpu").listFiles(
            file -> file.getName().matches("cpu[0-9]+"));
        if (files != null && files.length > 0) cores = files.length;
        cores = Math.min(cores, 256);
        add(out, 4, R.string.system_info_cores_label, Integer.toString(cores));
        add(out, 4, R.string.system_info_architecture_label, String.join(", ", Build.SUPPORTED_ABIS));
        add(out, 4, R.string.system_info_64bit_label, yesNo(Build.SUPPORTED_64_BIT_ABIS.length > 0));
        StringBuilder frequencies = new StringBuilder();
        long max = 0;
        for (int i = 0; i < cores; i++) {
            String base = "/sys/devices/system/cpu/cpu" + i + "/cpufreq/";
            long current = number(read(base + "scaling_cur_freq"));
            long maximum = number(read(base + "cpuinfo_max_freq"));
            max = Math.max(max, maximum);
            frequencies.append("CPU ").append(i).append(": ").append(mhz(current))
                .append(" / ").append(mhz(maximum)).append('\n');
        }
        add(out, 4, R.string.system_info_max_frequency_label, mhz(max));
        add(out, 4, R.string.device_info_cpu_frequencies, frequencies.toString().trim());
        String features = null;
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/cpuinfo"))) {
            for (int i = 0; i < 4096; i++) {
                String line = reader.readLine();
                if (line == null) break;
                if (line.startsWith("Features") || line.startsWith("flags")) {
                    int colon = line.indexOf(':');
                    if (colon >= 0) features = line.substring(colon + 1).trim();
                    break;
                }
            }
        } catch (Exception inaccessible) {   }
        list(out, 4, R.string.system_info_cpu_features_count,
            features == null ? null : features.replace(' ', '\n'));
    }

    private void collectMemory(List<Row> out) {
        ActivityManager manager = getSystemService(ActivityManager.class);
        if (manager != null) {
            ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
            manager.getMemoryInfo(memory);
            add(out, 5, R.string.system_info_total_ram_label, gib(memory.totalMem));
            add(out, 5, R.string.system_info_available_label, gib(memory.availMem));
            add(out, 5, R.string.system_info_used_label, gib(Math.max(0, memory.totalMem - memory.availMem)));
            int usage = memory.totalMem > 0
                ? (int)(100 * Math.max(0, memory.totalMem - memory.availMem) / memory.totalMem) : 0;
            out.add(new Row(5, getString(R.string.system_info_memory_label),
                getString(R.string.system_info_usage_percent_used, usage), false, usage));
            add(out, 5, R.string.system_info_low_memory_warning, yesNo(memory.lowMemory));
            add(out, 5, R.string.device_info_memory_threshold, gib(memory.threshold));
        }
        StatFs storage = new StatFs(getFilesDir().getAbsolutePath());
        add(out, 5, R.string.home_info_storage, getString(R.string.home_info_free_of,
            gib(storage.getAvailableBytes()), gib(storage.getTotalBytes())));
    }

    private void collectThermal(List<Row> out) {
        Intent battery = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        int temp = battery == null ? Integer.MIN_VALUE : battery.getIntExtra(
            BatteryManager.EXTRA_TEMPERATURE, Integer.MIN_VALUE);
        add(out, 3, R.string.system_info_battery_label, temp == Integer.MIN_VALUE ? unknown() : celsius(temp / 10.0));
        if (Build.VERSION.SDK_INT >= 29) {
            PowerManager power = getSystemService(PowerManager.class);
            if (power != null) {
                String[] status = {getString(R.string.home_thermal_none), getString(R.string.home_thermal_light),
                    getString(R.string.home_thermal_moderate), getString(R.string.home_thermal_severe),
                    getString(R.string.home_thermal_critical)};
                add(out, 3, R.string.home_info_thermal,
                    status[Math.max(0, Math.min(4, power.getCurrentThermalStatus()))]);
            }
        }
        File[] zones = new File("/sys/class/thermal").listFiles(
            file -> file.getName().startsWith("thermal_zone"));
        StringBuilder all = new StringBuilder();
        String cpu = "", gpu = "", skin = "";
        if (zones != null) for (int i = 0; i < zones.length && i < 512; i++) {
            String type = read(new File(zones[i], "type").getPath());
            String raw = read(new File(zones[i], "temp").getPath());
            if (raw.isEmpty()) continue;
            try {
                double degrees = Double.parseDouble(raw);
                if (Math.abs(degrees) > 200) degrees /= 1000.0;
                if (degrees < -40 || degrees > 150) continue;
                String value = celsius(degrees);
                if (all.length() > 0) all.append('\n');
                all.append(type.isEmpty() ? zones[i].getName() : type).append(": ").append(value);
                String lower = type.toLowerCase(Locale.ROOT);
                if (cpu.isEmpty() && (lower.contains("cpu") || lower.contains("soc"))) cpu = value;
                if (gpu.isEmpty() && lower.contains("gpu")) gpu = value;
                if (skin.isEmpty() && (lower.contains("skin") || lower.contains("shell"))) skin = value;
            } catch (NumberFormatException ignored) { }
        }
        add(out, 3, R.string.system_info_category_cpu, cpu);
        add(out, 3, R.string.system_info_category_gpu, gpu);
        add(out, 3, R.string.system_info_skin_label, skin);
        list(out, 3, R.string.system_info_all_thermal_zones_count,
            all.length() == 0 ? null : all.toString());
    }

    private void collectGraphics(List<Row> out, String[] values) {
        Map<String, String> facts = new HashMap<>();
        if (values != null) for (int i = 0; i + 1 < values.length; i += 2) facts.put(values[i], values[i + 1]);
        String locked = facts.get("boot.locked");
        add(out, 0, R.string.system_info_bootloader, "1".equals(locked)
            ? getString(R.string.system_info_locked) : "0".equals(locked)
            ? getString(R.string.system_info_unlocked) : unknown());
        add(out, 0, R.string.system_info_verified_boot, facts.get("boot.verified"));
        add(out, 1, R.string.system_info_category_gpu, getString(R.string.device_info_system_driver));
        add(out, 1, R.string.system_info_renderer_label, facts.get("gl.renderer"));
        add(out, 1, R.string.system_info_vendor_label, facts.get("gl.vendor"));
        add(out, 1, R.string.system_info_gl_version_label, facts.get("gl.version"));
        add(out, 1, R.string.device_info_device_local_heap, facts.get("vk.memory"));
        list(out, 1, R.string.system_info_gl_extensions_count, facts.get("gl.extensions"));
        add(out, 2, R.string.system_info_category_vulkan, getString(R.string.device_info_system_driver));
        add(out, 2, R.string.system_info_available_label, facts.containsKey("vk.available")
            ? yesNo(true) : unknown());
        String[] keys = {"vk.api", "vk.driver", "vk.device", "vk.type", "vk.push", "vk.allocations",
            "vk.descriptors", "vk.compute"};
        int[] labels = {R.string.system_info_api_version_label, R.string.system_info_driver_version_label,
            R.string.system_info_device_label, R.string.system_info_type_label,
            R.string.system_info_push_constants_label, R.string.system_info_memory_allocations_label,
            R.string.system_info_descriptor_sets_label, R.string.system_info_compute_work_group_label};
        for (int i = 0; i < keys.length; i++) add(out, 2, labels[i], facts.get(keys[i]));
        add(out, 2, R.string.device_info_features_note, getString(R.string.device_info_features_note));
        list(out, 2, R.string.system_info_supported_features_count, facts.get("vk.supported"));
        list(out, 2, R.string.system_info_unsupported_features_count, facts.get("vk.unsupported"));
        out.add(new Row(2, getString(R.string.menu_shader_features), shaderFeatures(facts), true));
        list(out, 2, R.string.system_info_vulkan_extensions_count, facts.get("vk.extensions"));
        add(out, 2, R.string.device_info_missing_note, getString(R.string.device_info_missing_note));
        list(out, 2, R.string.system_info_missing_extensions_count, facts.get("vk.missing"));
    }

    private void add(List<Row> out, int category, int label, String value) {
        out.add(new Row(category, getString(label), value == null || value.trim().isEmpty() ? unknown() : value, false));
    }

    private String shaderFeatures(Map<String, String> facts) {
        StringBuilder result = new StringBuilder();
        for (String key : new String[]{"vk.supported", "vk.unsupported"}) {
            String values = facts.get(key);
            if (values == null) continue;
            for (String feature : values.split("\n")) {
                if (!(feature.startsWith("shader") || feature.equals("geometryShader")
                        || feature.equals("tessellationShader"))) continue;
                if (result.length() > 0) result.append('\n');
                result.append("vk.supported".equals(key) ? "✓ " : "× ").append(feature);
            }
        }
        return result.length() == 0 ? unknown() : result.toString();
    }
    private void list(List<Row> out, int category, int label, String value) {
        int count = value == null || value.isEmpty() ? 0 : value.split("\n").length;
        int color = label == R.string.system_info_supported_features_count ? 0xff44ddaa
            : label == R.string.system_info_unsupported_features_count ? 0xffff8866 : 0xff8899aa;
        out.add(new Row(category, getString(label, count),
            value == null ? unknown() : count == 0 ? getString(R.string.device_info_none) : value,
            true, -1, color));
    }
    private String globalSetting(String key) {
        try { return yesNo(Settings.Global.getInt(getContentResolver(), key) != 0); }
        catch (Exception inaccessible) { return unknown(); }
    }
    private String unknown() { return getString(R.string.device_info_unavailable); }
    private String yesNo(boolean value) { return getString(value ? R.string.system_info_yes : R.string.system_info_no); }
    private String mhz(long khz) { return khz <= 0 ? unknown() : String.format(Locale.ROOT, "%.0f MHz", khz / 1000.0); }
    private static String gib(long bytes) { return String.format(Locale.ROOT, "%.2f GiB", bytes / 1073741824.0); }
    private static String celsius(double degrees) { return String.format(Locale.ROOT, "%.1f °C", degrees); }
    private static long number(String value) { try { return Long.parseLong(value); } catch (NumberFormatException e) { return 0; } }
    private static String read(String path) {
        try (BufferedReader reader = new BufferedReader(new FileReader(path))) {
            String value = reader.readLine();
            return value == null ? "" : value.trim();
        } catch (Exception inaccessible) { return ""; }
    }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private TextView text(String value, int size, int color) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setPadding(dp(6), dp(6), dp(6), dp(6));
        return view;
    }
    private TextView action(String value) {
        TextView action = text(value, 14, accent);
        action.setPadding(dp(12), dp(10), dp(12), dp(10));
        action.setFocusable(true);
        action.setClickable(true);
        action.setBackgroundResource(android.R.drawable.list_selector_background);
        return action;
    }
}
