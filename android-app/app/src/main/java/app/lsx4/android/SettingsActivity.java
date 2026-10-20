package app.lsx4.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.InsetDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class SettingsActivity extends Activity {
    public static final String PREFS = "lsx4_settings";
    private static final String EXTRA_SCREEN = "settings_screen";
    private static final int REQUEST_IMPORT_NGS2_MODULE = 4301;
    private static final int REQUEST_IMPORT_TURNIP_DRIVER = 4302;
    private static final String NGS2_MODULE_NAME = "libSceNgs2.sprx";
    private static final long MAX_SYSTEM_MODULE_BYTES = 128L * 1024L * 1024L;
    private boolean turnipImportInProgress;
    private boolean turnipSelectionInProgress;
    private boolean vulkanDriverAxisSupported;
    private Spinner vulkanDriverSpinnerControl;
    private View vulkanDriverImportControl;
    private int triggerCaptureSlot = -1;
    private final TextView[] triggerCaptureButtons = new TextView[2];
    private String gameTitleId = "";
    private String gameTitle = "";
    private SharedPreferences editingPreferences;

    public static final String K_RES_MODE = "res_mode";
    public static final String K_UPSCALED_OUTPUT_RES_MODE = "upscaled_output_res_mode";
    private static final String K_LEGACY_FSR_OUTPUT_RES_MODE = "fsr_output_res_mode";

    @Deprecated
    public static final String K_FSR_OUTPUT_RES_MODE = K_UPSCALED_OUTPUT_RES_MODE;
    public static final String K_DISPLAY_SCALING_MODE = "display_scaling_mode";
    public static final String K_FSR_ENABLED = "managed_fsr_enabled";
    public static final String K_UPSCALER_MODE = "upscaler_mode";
    public static final String K_RCAS_ENABLED = "managed_rcas_enabled";
    public static final String K_SHARP_BILINEAR_STRENGTH =
            "upscaler_sharp_bilinear_strength";
    public static final String K_BICUBIC_KERNEL = "upscaler_bicubic_kernel";
    public static final String K_FSR1_RCAS_SHARPNESS = "upscaler_fsr1_rcas_sharpness";
    public static final String K_SGSR1_EDGE_SHARPNESS = "upscaler_sgsr1_edge_sharpness";
    public static final String K_NIS_SHARPNESS = "upscaler_nis_sharpness";
    public static final String K_SGSR2_TEMPORAL_STABILITY =
            "upscaler_sgsr2_temporal_stability";
    public static final String K_ARM_ASR_SHARPENING = "upscaler_arm_asr_sharpening";
    public static final String K_ARM_ASR_SHARPNESS = "upscaler_arm_asr_sharpness";
    public static final String K_GPU_BACKEND = "gpu_backend";
    public static final String K_AUDIO = "audio_enabled";
    public static final String K_OVERLAY = "gamepad_overlay";
    public static final String K_OVERLAY_OPACITY = "overlay_opacity";
    public static final String K_OVERLAY_SHOW_SHARE =
            OverlayLayoutStore.visibleKey(OverlayLayoutStore.SHARE);
    public static final String K_OVERLAY_SHOW_PS =
            OverlayLayoutStore.visibleKey(OverlayLayoutStore.PS);
    public static final String K_OVERLAY_SHOW_TOUCHPAD =
            OverlayLayoutStore.visibleKey(OverlayLayoutStore.TOUCHPAD);
    public static final String K_PERF_HUD = "perf_hud";
    public static final String K_PERF_HUD_UPSCALER_INFO = "perf_hud_upscaler_info";
    public static final String K_TEST_MODE = "test_mode";
    public static final String K_REMOTE_DIAGNOSTICS = RemoteDiagnostics.PREF_ENABLED;
    public static final String K_KEEP_AWAKE = "keep_awake";
    public static final String K_AUTO_LAUNCH_STORE = "auto_launch_store";
    public static final String K_PERSISTENT_JIT_CACHE = "persistent_jit_cache";
    public static final String K_ARM_GPU_FAST_PATH = "managed_arm_gpu_fast_path";
    public static final String K_COARSE_FRAGMENT_2X2 = "managed_coarse_fragment_2x2";
    public static final String K_DISABLE_VK_ROBUSTNESS = "managed_disable_vk_robustness";
    public static final String K_TIERED_JIT = "managed_tiered_jit";
    public static final String K_JIT_TRACE_COMPILATION = "managed_jit_trace_compilation";
    public static final String K_GUEST_CPU_BACKEND = "managed_guest_cpu_backend";
    public static final String K_VULKAN_DRIVER =
            VulkanDriverManager.PREF_DRIVER_SELECTION;
    public static final String K_VULKAN_DRIVER_SELECTION =
            VulkanDriverManager.PREF_DRIVER_SELECTION_ID;
    @Deprecated
    public static final String K_TURNIP_VULKAN_DRIVER =
            VulkanDriverManager.LEGACY_PREF_TURNIP;
    public static final String K_LIMIT_ANISOTROPY_2X = "managed_limit_anisotropy_2x";
    public static final String K_ANISOTROPY_MODE = "managed_anisotropy_mode";
    public static final String K_READBACK_BATCHING = "managed_readback_batching";
    public static final String K_VULKAN_DRIVER_CALLS = "managed_vulkan_driver_calls";
    public static final String K_ASYNC_PIPELINE = "managed_async_pipeline";
    public static final String K_ADAPTIVE_MOBILE_GPU = "managed_adaptive_mobile_gpu";
    public static final String K_AUDIO_SIMD = "managed_audio_simd";
    public static final String K_FAST_GUEST_MEMORY = "managed_fast_guest_memory";
    public static final String K_RELAXED_FP_FUSION = "managed_relaxed_fp_fusion";
    public static final String K_ANDROID_PERFORMANCE_HINT =
            "managed_android_performance_hint";
    public static final String K_OEM_ROOT_PERFORMANCE =
            "managed_oem_root_performance";
    public static final String K_HOST_FLAGM = "managed_host_flagm";
    public static final String K_HOST_SVE2 = "managed_host_sve2";
    public static final String K_HOST_RCPC = "managed_host_rcpc";
    public static final String K_GUEST_SPIN_WAIT_EVENT =
            "managed_guest_spin_wait_event";
    private static final String K_LEGACY_PERSISTENT_JIT_CACHE =
            "jit_persistent_jit_cache";
    public static final String K_DISABLE_DYNAMIC_SHADOWS = "disable_dynamic_shadows";
    public static final String K_DISABLE_SSAO = "disable_ssao";
    public static final String K_DISABLE_MOTION_BLUR = "disable_motion_blur";
    public static final String K_DISABLE_DEPTH_OF_FIELD = "disable_depth_of_field";
    public static final String K_DISABLE_ANTI_ALIASING = "disable_anti_aliasing";
    public static final String K_DISABLE_CHROMATIC_ABERRATION =
            "disable_chromatic_aberration";

    public static final int RES_NATIVE = 0;
    public static final int RES_1080P = 1;
    public static final int RES_720P = 2;
    public static final int RES_1620P = 3;
    public static final int RES_544P = 4;
    public static final int RES_360P = 5;
    public static final int RES_270P = 6;
    public static final int RES_180P = 7;
    public static final int DEFAULT_RES_MODE = RES_720P;
    public static final int DEFAULT_UPSCALED_OUTPUT_RES_MODE = RES_720P;

    @Deprecated
    public static final int DEFAULT_FSR_OUTPUT_RES_MODE = DEFAULT_UPSCALED_OUTPUT_RES_MODE;
    public static final int DEFAULT_DISPLAY_SCALING_MODE = DisplayScaling.DEFAULT;
    public static final int UPSCALER_OFF = 0;
    public static final int UPSCALER_AUTO = 1;
    public static final int UPSCALER_BILINEAR = 2;
    public static final int UPSCALER_SHARP_BILINEAR = 3;
    public static final int UPSCALER_BICUBIC = 4;
    public static final int UPSCALER_FSR1 = 5;
    public static final int UPSCALER_SGSR1 = 6;
    public static final int UPSCALER_NIS = 7;
    public static final int UPSCALER_SGSR2 = 8;
    public static final int UPSCALER_ARM_ASR = 9;
    public static final int DEFAULT_UPSCALER_MODE = UPSCALER_AUTO;
    public static final int ANISOTROPY_GUEST = 0;
    public static final int ANISOTROPY_1X = 1;
    public static final int ANISOTROPY_2X = 2;

    private static final int[] RESOLUTION_MODE_ORDER = {
            RES_NATIVE, RES_1080P, RES_720P, RES_544P,
            RES_360P, RES_270P, RES_180P, RES_1620P
    };
    private static final int[] RESOLUTION_WIDTHS =
            {0, 1920, 1280, 2880, 960, 640, 480, 320};
    private static final int[] RESOLUTION_HEIGHTS =
            {0, 1080, 720, 1620, 544, 360, 270, 180};
    private static final int[] RESOLUTION_ASCENDING_ORDER = {
            RES_180P, RES_270P, RES_360P, RES_544P,
            RES_720P, RES_1080P, RES_1620P, RES_NATIVE
    };

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(AppLocale.wrap(base));
    }

    public static SharedPreferences prefs(Context context) {
        SharedPreferences preferences = ProcessSafePreferences.get(context);
        if (!preferences.contains(K_PERSISTENT_JIT_CACHE)
                && preferences.contains(K_LEGACY_PERSISTENT_JIT_CACHE)) {
            preferences.edit().putBoolean(K_PERSISTENT_JIT_CACHE,
                    preferences.getBoolean(K_LEGACY_PERSISTENT_JIT_CACHE, true)).apply();
        }
        if (!preferences.contains(K_UPSCALED_OUTPUT_RES_MODE)
                && preferences.contains(K_LEGACY_FSR_OUTPUT_RES_MODE)) {
            preferences.edit().putInt(K_UPSCALED_OUTPUT_RES_MODE,
                    preferences.getInt(K_LEGACY_FSR_OUTPUT_RES_MODE,
                            DEFAULT_UPSCALED_OUTPUT_RES_MODE)).commit();
        }
        if (!preferences.contains(K_UPSCALED_OUTPUT_RES_MODE)) {
            int oldMode = sanitizeResolutionMode(
                    preferences.getInt(K_RES_MODE, DEFAULT_RES_MODE));
            int sourceMode = oldMode;
            int outputMode;
            if (preferences.getBoolean(K_FSR_ENABLED, true)) {


                if (oldMode == RES_NATIVE) {
                    sourceMode = RES_1080P;
                    outputMode = RES_NATIVE;
                } else if (oldMode == RES_1620P) {
                    sourceMode = RES_1080P;
                    outputMode = RES_1620P;
                } else if (oldMode == RES_1080P) {
                    sourceMode = RES_720P;
                    outputMode = RES_1080P;
                } else if (oldMode == RES_720P) {
                    sourceMode = RES_544P;
                    outputMode = RES_720P;
                } else {
                    outputMode = defaultUpscaledOutputMode(sourceMode);
                }
            } else {
                outputMode = defaultUpscaledOutputMode(sourceMode);
            }
            preferences.edit()
                    .putInt(K_RES_MODE, sourceMode)
                    .putInt(K_UPSCALED_OUTPUT_RES_MODE, outputMode)
                    .commit();
        }
        if (context instanceof SettingsActivity) {
            SettingsActivity activity = (SettingsActivity) context;
            if (activity.editingPreferences != null) {
                return activity.editingPreferences;
            }
        }
        return preferences;
    }

    public static SharedPreferences prefsForGame(Context context, String titleId) {


        prefs(context);
        SharedPreferences base = ProcessSafePreferences.get(context);
        return GameProfileSettings.forGame(base, titleId);
    }

    private SharedPreferences basePrefs() {
        return ProcessSafePreferences.get(this);
    }

    private SharedPreferences editingPrefs() {
        if (editingPreferences == null) {
            editingPreferences = prefsForGame(this, gameTitleId);
        }
        return editingPreferences;
    }

    private String[] resolutionModeLabels() {
        return new String[]{
                getString(R.string.resolution_native),
                "1080p (1920×1080)", "720p (1280×720)", "544p (960×544)",
                "360p (640×360)", "270p (480×270)", "180p (320×180)",
                getString(R.string.resolution_upscale_150)
        };
    }

    private String[] upscalerModeLabels() {
        return new String[]{
                getString(R.string.upscaler_off),
                getString(R.string.upscaler_auto),
                "Bilinear",
                "Sharp Bilinear",
                "Bicubic (Catmull–Rom)",
                "AMD FSR 1 (EASU + RCAS)",
                "Snapdragon GSR 1",
                "NVIDIA Image Scaling",
                "Snapdragon GSR 2 (Temporal)",
                "Arm ASR (Temporal)"
        };
    }

    static int sanitizeUpscalerMode(int mode) {
        return mode >= UPSCALER_OFF && mode <= UPSCALER_ARM_ASR
                ? mode : DEFAULT_UPSCALER_MODE;
    }

    private String resolutionModeLabel(int mode) {
        switch (sanitizeResolutionMode(mode)) {
            case RES_NATIVE:
                return getString(R.string.resolution_native);
            case RES_1080P:
                return "1080p (1920×1080)";
            case RES_720P:
                return "720p (1280×720)";
            case RES_1620P:
                return getString(R.string.resolution_upscale_150);
            case RES_544P:
                return "544p (960×544)";
            case RES_360P:
                return "360p (640×360)";
            case RES_270P:
                return "270p (480×270)";
            case RES_180P:
                return "180p (320×180)";
            default:
                return getString(R.string.resolution_native);
        }
    }

    private String[] displayScalingModeLabels() {
        return new String[]{
                getString(R.string.scaling_current),
                getString(R.string.scaling_fit),
                getString(R.string.scaling_fill_crop),
                getString(R.string.scaling_stretch),
                getString(R.string.scaling_integer),
                getString(R.string.scaling_native_1_to_1),
                getString(R.string.scaling_aspect_4_3),
                getString(R.string.scaling_aspect_16_9),
                getString(R.string.scaling_aspect_16_10),
                getString(R.string.scaling_aspect_21_9),
                getString(R.string.scaling_aspect_32_9)
        };
    }

    public static int resolutionModeToPosition(int mode) {
        for (int i = 0; i < RESOLUTION_MODE_ORDER.length; ++i) {
            if (RESOLUTION_MODE_ORDER[i] == mode) {
                return i;
            }
        }
        return resolutionModeToPosition(RES_NATIVE);
    }

    public static int resolutionModeAtPosition(int position) {
        return position >= 0 && position < RESOLUTION_MODE_ORDER.length
                ? RESOLUTION_MODE_ORDER[position] : RES_NATIVE;
    }

    public static int resolutionWidth(int mode) {
        return mode >= 0 && mode < RESOLUTION_WIDTHS.length
                ? RESOLUTION_WIDTHS[mode] : RESOLUTION_WIDTHS[RES_NATIVE];
    }

    public static int resolutionHeight(int mode) {
        return mode >= 0 && mode < RESOLUTION_HEIGHTS.length
                ? RESOLUTION_HEIGHTS[mode] : RESOLUTION_HEIGHTS[RES_NATIVE];
    }

    public static int sanitizeResolutionMode(int mode) {
        return resolutionModeAtPosition(resolutionModeToPosition(mode));
    }

    private static long resolutionPixelCount(int mode) {
        mode = sanitizeResolutionMode(mode);
        if (mode == RES_NATIVE) {
            return Long.MAX_VALUE;
        }
        return (long) resolutionWidth(mode) * resolutionHeight(mode);
    }

    public static int[] higherUpscaledOutputModes(int sourceMode) {
        sourceMode = sanitizeResolutionMode(sourceMode);
        if (sourceMode == RES_NATIVE || sourceMode == RES_1620P) {
            return new int[0];
        }
        long sourcePixels = resolutionPixelCount(sourceMode);
        int count = 0;
        for (int mode : RESOLUTION_ASCENDING_ORDER) {
            if (resolutionPixelCount(mode) > sourcePixels) {
                ++count;
            }
        }
        int[] result = new int[count];
        int index = 0;
        for (int mode : RESOLUTION_ASCENDING_ORDER) {
            if (resolutionPixelCount(mode) > sourcePixels) {
                result[index++] = mode;
            }
        }
        return result;
    }

    public static int defaultUpscaledOutputMode(int sourceMode) {
        int[] modes = higherUpscaledOutputModes(sourceMode);
        return modes.length == 0 ? sanitizeResolutionMode(sourceMode) : modes[0];
    }

    public static int sanitizeUpscaledOutputMode(int sourceMode, int requestedMode) {
        for (int mode : higherUpscaledOutputModes(sourceMode)) {
            if (mode == requestedMode) {
                return mode;
            }
        }
        return defaultUpscaledOutputMode(sourceMode);
    }


    @Deprecated
    public static int[] higherFsrOutputModes(int sourceMode) {
        return higherUpscaledOutputModes(sourceMode);
    }

    @Deprecated
    public static int defaultFsrOutputMode(int sourceMode) {
        return defaultUpscaledOutputMode(sourceMode);
    }

    @Deprecated
    public static int sanitizeFsrOutputMode(int sourceMode, int requestedMode) {
        return sanitizeUpscaledOutputMode(sourceMode, requestedMode);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TestModeManager.reconcileBuildScope(this);
        UiChrome.preparePortraitWindow(this);
        gameTitleId = CpuTuningSettings.normalizeTitleId(
                getIntent().getStringExtra(GameProfileSettings.EXTRA_GAME_TITLE_ID));
        gameTitle = getIntent().getStringExtra(GameProfileSettings.EXTRA_GAME_TITLE);
        if (gameTitle == null || gameTitle.trim().isEmpty()) {
            gameTitle = gameTitleId;
        }
        editingPreferences = prefsForGame(this, gameTitleId);
        if (!gameTitleId.isEmpty()) {
            GameProfileSettings.rememberTitle(prefs(this), gameTitleId, gameTitle);
        }
        String screen = getIntent().getStringExtra(EXTRA_SCREEN);
        if (screen == null || screen.isEmpty()) {
            screen = "root";
        }
        setContentView(buildScreen(screen));
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (triggerCaptureSlot >= 0 && event != null
                && event.getAction() == KeyEvent.ACTION_DOWN
                && event.getRepeatCount() == 0) {
            if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
                int cancelledSlot = triggerCaptureSlot;
                triggerCaptureSlot = -1;
                refreshTriggerCaptureButton(cancelledSlot);
                return true;
            }
            int capturedSlot = triggerCaptureSlot;
            HardwareTriggerMapper.capture(editingPrefs(), capturedSlot, event);
            triggerCaptureSlot = -1;
            refreshTriggerCaptureButton(capturedSlot);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private View buildScreen(String screen) {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setClipToPadding(false);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xff111417);
        scroll.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        UiChrome.applyInsets(root, dp(20), dp(14), dp(20), dp(24));

        if ("root".equals(screen)) {
            if (gameTitleId.isEmpty()) {
                buildRoot(root);
            } else {
                addScreenHeader(root, gameTitle);
                buildGameRoot(root);
            }
        } else {
            addScreenHeader(root, titleForScreen(screen));
            switch (screen) {
                case "graphics":
                    buildGraphics(root);
                    break;
                case "gpu":
                    buildGpu(root);
                    break;
                case "patches":
                    buildPatches(root);
                    break;
                case "audio":
                    buildAudio(root);
                    break;
                case "controls":
                    buildControls(root);
                    break;
                case "cache":
                    buildCache(root);
                    break;
                case "managed_optimizations":
                    buildManagedOptimizations(root);
                    break;
                case "cpu_tuning":
                    buildCpuTuning(root);
                    break;
                case "other":
                    buildOther(root);
                    break;
                default:
                    finish();
                    break;
            }
        }
        return scroll;
    }

    private void buildRoot(LinearLayout root) {
        root.addView(title(getString(R.string.settings_title)));
        root.addView(compactSection(getString(R.string.section_general)));

        Spinner language = spinner(AppLocale.NATIVE_LABELS);
        language.setSelection(AppLocale.selectedPosition(this), false);
        language.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (position != AppLocale.selectedPosition(SettingsActivity.this)) {
                    AppLocale.select(SettingsActivity.this, position);
                    recreate();
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        root.addView(settingRow(getString(R.string.language), language),
                matchWrapWithMargins(0, 0, 0, dp(12)));

        root.addView(category(R.string.category_graphics, R.string.category_graphics_summary,
                "graphics"));
        root.addView(category(R.string.category_managed_optimizations,
                R.string.category_managed_optimizations_summary, "managed_optimizations"));
        root.addView(category(R.string.category_cpu_tuning,
                R.string.category_cpu_tuning_summary, "cpu_tuning"));
        root.addView(category(R.string.category_audio, R.string.category_audio_summary, "audio"));
        root.addView(category(R.string.category_controls, R.string.category_controls_summary,
                "controls"));
        root.addView(category(R.string.category_cache, R.string.category_cache_summary, "cache"));
        root.addView(category(R.string.category_other, R.string.category_other_summary, "other"));
    }

    private void buildGameRoot(LinearLayout root) {
        TextView note = hint(getString(R.string.game_profile_scope_note, gameTitle));
        note.setTextColor(0xffb8c7d8);
        note.setPadding(dp(16), dp(14), dp(16), dp(14));
        note.setBackground(cardBackground(0xff17212b, 0xff35516d, 14));
        root.addView(note, matchWrapWithMargins(0, 0, 0, dp(12)));

        root.addView(category(R.string.category_graphics, R.string.category_graphics_summary,
                "graphics"));
        root.addView(category(R.string.category_managed_optimizations,
                R.string.category_managed_optimizations_summary, "managed_optimizations"));
        root.addView(category(R.string.category_cpu_tuning,
                R.string.category_cpu_tuning_summary, "cpu_tuning"));
        root.addView(category(R.string.category_audio, R.string.category_audio_summary, "audio"));
        root.addView(category(R.string.category_controls, R.string.category_controls_summary,
                "controls"));
        root.addView(category(R.string.category_cache, R.string.category_cache_summary, "cache"));
        root.addView(category(R.string.category_other, R.string.category_other_summary, "other"));

        TextView reset = label(getString(R.string.game_profile_reset), 15, 0xffffb4a8);
        reset.setGravity(Gravity.CENTER);
        reset.setMinHeight(dp(56));
        reset.setBackground(cardBackground(0xff2a1d1d, 0xff71433e, 14));
        reset.setClickable(true);
        reset.setFocusable(true);
        reset.setOnClickListener(view -> new AlertDialog.Builder(this)
                .setTitle(R.string.game_profile_reset)
                .setMessage(getString(R.string.game_profile_reset_confirm, gameTitle))
                .setPositiveButton(R.string.game_profile_reset, (dialog, which) -> {
                    GameProfileSettings.clearOverrides(basePrefs(), gameTitleId);
                    CpuTuningSettings.clearGameProfile(basePrefs(), gameTitleId);
                    CpuTuningSettings.setAllGamesException(
                            basePrefs(), gameTitleId, false);
                    editingPreferences = prefsForGame(this, gameTitleId);
                    Toast.makeText(this, R.string.game_profile_reset_done,
                            Toast.LENGTH_LONG).show();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show());
        root.addView(reset, matchWrapWithMargins(0, dp(16), 0, 0));
    }

    private void buildGraphics(LinearLayout root) {
        root.addView(section(getString(R.string.graphics_resolution)));
        SharedPreferences graphicsPreferences = prefs(this);
        int selectedSourceMode = sanitizeResolutionMode(
                graphicsPreferences.getInt(K_RES_MODE, DEFAULT_RES_MODE));
        Spinner resolution = spinner(resolutionModeLabels());
        resolution.setSelection(resolutionModeToPosition(selectedSourceMode), false);
        root.addView(resolution, matchWrapWithMargins(0, dp(6), 0, dp(14)));

        root.addView(section(getString(R.string.graphics_fsr_section)));
        CheckBox fsr = preferenceCheck(
                R.string.graphics_fsr_enabled, K_FSR_ENABLED, true);
        Spinner upscaler = spinner(upscalerModeLabels());
        int selectedUpscaler = sanitizeUpscalerMode(graphicsPreferences.getInt(
                K_UPSCALER_MODE, DEFAULT_UPSCALER_MODE));
        upscaler.setSelection(selectedUpscaler, false);
        upscaler.setContentDescription(getString(R.string.graphics_upscaler_algorithm));
        LinearLayout outputContainer = new LinearLayout(this);
        outputContainer.setOrientation(LinearLayout.VERTICAL);
        outputContainer.setPadding(dp(8), 0, 0, 0);
        LinearLayout parameterContainer = new LinearLayout(this);
        parameterContainer.setOrientation(LinearLayout.VERTICAL);
        parameterContainer.setPadding(dp(8), 0, 0, 0);
        fsr.setOnCheckedChangeListener((button, checked) -> {
            prefs(this).edit().putBoolean(K_FSR_ENABLED, checked).apply();
            upscaler.setEnabled(checked);
            upscaler.setAlpha(checked ? 1.0f : 0.52f);
            int mode = sanitizeUpscalerMode(
                    prefs(this).getInt(K_UPSCALER_MODE, DEFAULT_UPSCALER_MODE));
            int sourceMode = sanitizeResolutionMode(
                    prefs(this).getInt(K_RES_MODE, DEFAULT_RES_MODE));
            renderUpscaledOutputSelector(outputContainer, sourceMode, mode, checked);
            renderUpscalerParameterControls(parameterContainer, mode, checked);
        });
        root.addView(fsr);
        root.addView(section(getString(R.string.graphics_upscaler_algorithm)));
        upscaler.setEnabled(fsr.isChecked());
        upscaler.setAlpha(fsr.isChecked() ? 1.0f : 0.52f);
        upscaler.setOnItemSelectedListener(saveSpinner(upscaler, position -> {
            int mode = sanitizeUpscalerMode(position);
            prefs(this).edit().putInt(K_UPSCALER_MODE, mode).apply();
            int sourceMode = sanitizeResolutionMode(
                    prefs(this).getInt(K_RES_MODE, DEFAULT_RES_MODE));
            renderUpscaledOutputSelector(
                    outputContainer, sourceMode, mode, fsr.isChecked());
            renderUpscalerParameterControls(parameterContainer, mode, fsr.isChecked());
        }));
        root.addView(upscaler, matchWrapWithMargins(0, dp(6), 0, dp(10)));
        root.addView(outputContainer, matchWrap());
        root.addView(parameterContainer, matchWrap());
        renderUpscaledOutputSelector(
                outputContainer, selectedSourceMode, selectedUpscaler, fsr.isChecked());
        renderUpscalerParameterControls(
                parameterContainer, selectedUpscaler, fsr.isChecked());
        resolution.setOnItemSelectedListener(saveSpinner(resolution, position -> {
            int sourceMode = resolutionModeAtPosition(position);
            prefs(this).edit().putInt(K_RES_MODE, sourceMode).apply();
            int mode = sanitizeUpscalerMode(upscaler.getSelectedItemPosition());
            renderUpscaledOutputSelector(
                    outputContainer, sourceMode, mode, fsr.isChecked());
        }));
        TextView fsrHint = hint(getString(R.string.graphics_fsr_hint));
        fsrHint.setPadding(dp(2), 0, dp(2), dp(14));
        root.addView(fsrHint);

        root.addView(section(getString(R.string.graphics_scaling)));
        Spinner scaling = spinner(displayScalingModeLabels());
        scaling.setContentDescription(getString(R.string.graphics_scaling));
        scaling.setSelection(DisplayScaling.positionOf(
                prefs(this).getInt(K_DISPLAY_SCALING_MODE,
                        DEFAULT_DISPLAY_SCALING_MODE)), false);
        scaling.setOnItemSelectedListener(saveSpinner(scaling, position ->
                prefs(this).edit().putInt(K_DISPLAY_SCALING_MODE,
                        DisplayScaling.atPosition(position)).apply()));
        root.addView(scaling, matchWrapWithMargins(0, dp(6), 0, dp(6)));
        TextView scalingHint = hint(getString(R.string.graphics_scaling_hint));
        scalingHint.setPadding(dp(2), 0, dp(2), dp(14));
        root.addView(scalingHint);

        root.addView(category(R.string.graphics_gpu, R.string.graphics_gpu_summary, "gpu"));
        root.addView(category(R.string.graphics_patches, R.string.graphics_patches_summary,
                "patches"));
    }

    private void renderUpscaledOutputSelector(
            LinearLayout container, int sourceMode, int upscalerMode,
            boolean upscalingEnabled) {
        container.removeAllViews();
        int[] outputModes = higherUpscaledOutputModes(sourceMode);
        if (!upscalingEnabled || sanitizeUpscalerMode(upscalerMode) == UPSCALER_OFF
                || outputModes.length == 0) {
            container.setVisibility(View.GONE);
            return;
        }

        container.setVisibility(View.VISIBLE);
        container.addView(section(getString(R.string.graphics_upscaled_output_resolution)));
        String[] labels = new String[outputModes.length];
        for (int index = 0; index < outputModes.length; ++index) {
            labels[index] = resolutionModeLabel(outputModes[index]);
        }
        SharedPreferences preferences = prefs(this);
        int requestedMode = preferences.getInt(
                K_UPSCALED_OUTPUT_RES_MODE, DEFAULT_UPSCALED_OUTPUT_RES_MODE);
        int selectedMode = sanitizeUpscaledOutputMode(sourceMode, requestedMode);
        if (selectedMode != requestedMode) {
            preferences.edit().putInt(K_UPSCALED_OUTPUT_RES_MODE, selectedMode).apply();
        }
        int selectedPosition = 0;
        for (int index = 0; index < outputModes.length; ++index) {
            if (outputModes[index] == selectedMode) {
                selectedPosition = index;
                break;
            }
        }
        Spinner outputResolution = spinner(labels);
        outputResolution.setContentDescription(
                getString(R.string.graphics_upscaled_output_resolution));
        outputResolution.setSelection(selectedPosition, false);
        outputResolution.setOnItemSelectedListener(saveSpinner(outputResolution, position ->
                prefs(this).edit().putInt(
                        K_UPSCALED_OUTPUT_RES_MODE, outputModes[position]).apply()));
        container.addView(outputResolution,
                matchWrapWithMargins(0, dp(6), 0, dp(10)));
    }

    private void renderUpscalerParameterControls(
            LinearLayout container, int mode, boolean upscalingEnabled) {
        container.removeAllViews();
        mode = sanitizeUpscalerMode(mode);
        if (!upscalingEnabled || mode == UPSCALER_OFF) {
            container.setVisibility(View.GONE);
            return;
        }
        container.setVisibility(View.VISIBLE);
        container.addView(section(getString(R.string.graphics_upscaler_parameters)));
        switch (mode) {
            case UPSCALER_SHARP_BILINEAR:
                addPercentSlider(container, R.string.graphics_sharp_bilinear_strength,
                        K_SHARP_BILINEAR_STRENGTH, 100, true);
                break;
            case UPSCALER_BICUBIC:
                Spinner kernel = spinner(new String[]{
                        "Catmull–Rom", "Mitchell–Netravali", "B-spline"
                });
                kernel.setSelection(Math.max(0, Math.min(2,
                        prefs(this).getInt(K_BICUBIC_KERNEL, 0))), false);
                kernel.setOnItemSelectedListener(saveSpinner(kernel, position ->
                        prefs(this).edit().putInt(K_BICUBIC_KERNEL, position).apply()));
                container.addView(settingRow(
                        getString(R.string.graphics_bicubic_kernel), kernel), matchWrap());
                break;
            case UPSCALER_FSR1: {
                boolean enabled = prefs(this).getBoolean(K_RCAS_ENABLED, true);
                CheckBox rcas = check(getString(R.string.graphics_rcas_enabled), enabled);
                rcas.setOnCheckedChangeListener((button, checked) -> {
                    prefs(this).edit().putBoolean(K_RCAS_ENABLED, checked).apply();
                    renderUpscalerParameterControls(container, UPSCALER_FSR1, true);
                });
                container.addView(rcas, cardMargins());
                addPercentSlider(container, R.string.graphics_fsr1_rcas_sharpness,
                        K_FSR1_RCAS_SHARPNESS, 75, enabled);
                break;
            }
            case UPSCALER_SGSR1:
                addPercentSlider(container, R.string.graphics_sgsr1_edge_sharpness,
                        K_SGSR1_EDGE_SHARPNESS, 100, true);
                break;
            case UPSCALER_NIS:
                addPercentSlider(container, R.string.graphics_nis_sharpness,
                        K_NIS_SHARPNESS, 50, true);
                break;
            case UPSCALER_SGSR2:
                addPercentSlider(container, R.string.graphics_sgsr2_temporal_stability,
                        K_SGSR2_TEMPORAL_STABILITY, 70, true);
                break;
            case UPSCALER_ARM_ASR: {
                boolean enabled = prefs(this).getBoolean(K_ARM_ASR_SHARPENING, true);
                CheckBox sharpening = check(
                        getString(R.string.graphics_arm_asr_sharpening), enabled);
                sharpening.setOnCheckedChangeListener((button, checked) -> {
                    prefs(this).edit().putBoolean(K_ARM_ASR_SHARPENING, checked).apply();
                    renderUpscalerParameterControls(container, UPSCALER_ARM_ASR, true);
                });
                container.addView(sharpening, cardMargins());
                addPercentSlider(container, R.string.graphics_arm_asr_sharpness,
                        K_ARM_ASR_SHARPNESS, 25, enabled);
                break;
            }
            case UPSCALER_AUTO:
                container.addView(hint(getString(R.string.graphics_upscaler_auto_parameters)));
                break;
            case UPSCALER_BILINEAR:
            default:
                container.addView(hint(getString(R.string.graphics_upscaler_no_parameters)));
                break;
        }
    }

    private void addPercentSlider(LinearLayout container, int titleResource,
                                  String key, int defaultValue, boolean enabled) {
        int current = Math.max(0, Math.min(100,
                prefs(this).getInt(key, defaultValue)));
        TextView value = label(getString(titleResource) + ": " + current + "%",
                12, enabled ? 0xffaeb8c3 : 0xff68727d);
        SeekBar slider = new SeekBar(this);
        slider.setMax(100);
        slider.setProgress(current);
        slider.setEnabled(enabled);
        slider.setAlpha(enabled ? 1.0f : 0.52f);
        slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                value.setText(getString(titleResource) + ": " + progress + "%");
                if (fromUser) {
                    prefs(SettingsActivity.this).edit().putInt(key, progress).apply();
                }
            }

            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        container.addView(value, matchWrapWithMargins(0, dp(4), 0, 0));
        container.addView(slider, matchWrapWithMargins(0, 0, 0, dp(8)));
    }

    private void buildGpu(LinearLayout root) {
        boolean openGl = GraphicsBackend.OpenGlBackend.ID.equals(
                prefs(this).getString(K_GPU_BACKEND, GraphicsBackend.VulkanBackend.ID));
        if (openGl) {
            prefs(this).edit().putString(
                    K_GPU_BACKEND, GraphicsBackend.VulkanBackend.ID).apply();
        }
        CheckBox vulkan = check(getString(R.string.gpu_vulkan), true);
        CheckBox openGlControl = check(
                getString(R.string.gpu_opengl) + " · "
                        + getString(R.string.gpu_opengl_pending), false);
        openGlControl.setEnabled(false);
        openGlControl.setAlpha(0.52f);

        vulkan.setOnClickListener(view -> {
            vulkan.setChecked(true);
            openGlControl.setChecked(false);
            prefs(this).edit().putString(
                    K_GPU_BACKEND, GraphicsBackend.VulkanBackend.ID).apply();
        });

        root.addView(vulkan, matchWrapWithMargins(0, dp(8), 0, dp(6)));
        root.addView(openGlControl, matchWrapWithMargins(0, 0, 0, dp(12)));
    }

    private void buildPatches(LinearLayout root) {
        List<GamePatchCatalog.Game> games = GamePatchCatalog.load(this);
        if (!gameTitleId.isEmpty()) {
            GamePatchCatalog.Game selected = null;
            for (GamePatchCatalog.Game candidate : games) {
                for (String candidateId : candidate.titleIds) {
                    if (gameTitleId.equalsIgnoreCase(candidateId)) {
                        selected = candidate;
                        break;
                    }
                }
                if (selected != null) {
                    break;
                }
            }
            if (selected == null) {
                root.addView(hint(getString(R.string.patch_no_game_patches, gameTitle)));
            } else {
                LinearLayout patches = new LinearLayout(this);
                patches.setOrientation(LinearLayout.VERTICAL);
                root.addView(patches, matchWrap());
                renderPatches(patches, selected);
            }
            return;
        }
        if (games.isEmpty()) {
            root.addView(hint(getString(R.string.patch_no_games)));
            return;
        }
        root.addView(label(getString(R.string.patch_select_game), 13, 0xffaab4c0));
        String[] names = new String[games.size()];
        for (int i = 0; i < games.size(); ++i) {
            names[i] = games.get(i).name;
        }
        Spinner game = spinner(names);
        root.addView(game, matchWrapWithMargins(0, dp(6), 0, dp(12)));
        LinearLayout patches = new LinearLayout(this);
        patches.setOrientation(LinearLayout.VERTICAL);
        root.addView(patches, matchWrap());
        Runnable render = () -> renderPatches(patches,
                games.get(Math.max(0, game.getSelectedItemPosition())));
        game.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                render.run();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        render.run();
    }

    private void renderPatches(LinearLayout container, GamePatchCatalog.Game game) {
        container.removeAllViews();


        SharedPreferences preferences = basePrefs();
        SharedPreferences.Editor cleanup = null;
        List<GamePatchCatalog.Patch> selectedPatches = new java.util.ArrayList<>();
        for (GamePatchCatalog.Patch patch : game.patches) {
            if (!patch.isChecked(preferences)) {
                continue;
            }
            boolean conflicts = false;
            for (GamePatchCatalog.Patch selected : selectedPatches) {
                if (patchesConflict(patch, selected)) {
                    conflicts = true;
                    break;
                }
            }
            if (!conflicts) {
                selectedPatches.add(patch);
                continue;
            }
            if (cleanup == null) {
                cleanup = preferences.edit();
            }
            cleanup.putBoolean(patch.preferenceKey, patch.inverted);
        }
        if (cleanup != null) {
            cleanup.commit();
            MainActivity.materializeBundledGamePatches(this);
        }

        Map<String, CheckBox> controls = new HashMap<>();
        boolean[] updatingGroup = {false};
        for (GamePatchCatalog.Patch patch : game.patches) {
            String title = patch.titleResource != 0
                    ? getString(patch.titleResource)
                    : (!patch.label.isEmpty() ? patch.label : patch.id);
            CheckBox control = check(title, patch.isChecked(preferences));
            controls.put(patch.preferenceKey, control);
            control.setOnCheckedChangeListener((button, checked) -> {
                if (updatingGroup[0]) {
                    return;
                }
                SharedPreferences.Editor editor = preferences.edit()
                        .putBoolean(patch.preferenceKey,
                                patch.inverted ? !checked : checked);
                if (checked) {
                    for (GamePatchCatalog.Patch peer : game.patches) {
                        if (!peer.preferenceKey.equals(patch.preferenceKey)
                                && patchesConflict(patch, peer)) {
                            editor.putBoolean(peer.preferenceKey, peer.inverted);
                        }
                    }
                }
                if (!editor.commit()) {
                    updatingGroup[0] = true;
                    control.setChecked(patch.isChecked(preferences));
                    updatingGroup[0] = false;
                    return;
                }
                if (checked) {
                    updatingGroup[0] = true;
                    for (GamePatchCatalog.Patch peer : game.patches) {
                        if (!peer.preferenceKey.equals(patch.preferenceKey)
                                && patchesConflict(patch, peer)) {
                            CheckBox peerControl = controls.get(peer.preferenceKey);
                            if (peerControl != null) {
                                peerControl.setChecked(false);
                            }
                        }
                    }
                    updatingGroup[0] = false;
                }
                MainActivity.materializeBundledGamePatches(this);
            });
            container.addView(control, cardMargins());
        }
    }

    private static boolean patchesConflict(GamePatchCatalog.Patch left,
                                           GamePatchCatalog.Patch right) {
        return (!left.group.isEmpty() && left.group.equals(right.group))
                || left.conflictsWith(right);
    }

    private void buildAudio(LinearLayout root) {
        root.addView(preferenceCheck(R.string.audio_enable, K_AUDIO, true));

        TextView moduleStatus = hint(getString(isNgs2ModuleInstalled()
                ? R.string.audio_ngs2_module_ready
                : R.string.audio_ngs2_module_missing));
        moduleStatus.setPadding(dp(2), dp(14), dp(2), dp(8));
        root.addView(moduleStatus);

        TextView importModule = label(
                getString(R.string.audio_import_ngs2_module), 15, 0xffe3e8ef);
        importModule.setGravity(Gravity.CENTER_VERTICAL);
        importModule.setMinHeight(dp(58));
        importModule.setPadding(dp(18), dp(12), dp(16), dp(12));
        importModule.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        importModule.setClickable(true);
        importModule.setFocusable(true);
        importModule.setOnClickListener(view -> pickNgs2Module());
        root.addView(importModule, cardMargins());

        TextView legalHint = hint(getString(R.string.audio_ngs2_module_hint));
        legalHint.setPadding(dp(2), dp(8), dp(2), 0);
        root.addView(legalHint);
    }

    private void pickNgs2Module() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_IMPORT_NGS2_MODULE);
        } catch (ActivityNotFoundException error) {
            Toast.makeText(this, R.string.file_picker_unavailable, Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        if (requestCode == REQUEST_IMPORT_TURNIP_DRIVER) {
            importTurnipDriver(data.getData());
            return;
        }
        if (requestCode != REQUEST_IMPORT_NGS2_MODULE) {
            return;
        }
        Uri source = data.getData();
        Thread importer = new Thread(() -> {
            try {
                importNgs2Module(source);
                runOnUiThread(() -> {
                    Toast.makeText(this, R.string.audio_ngs2_import_success,
                            Toast.LENGTH_LONG).show();
                    recreate();
                });
            } catch (Exception error) {
                runOnUiThread(() -> Toast.makeText(this,
                        getString(R.string.audio_ngs2_import_failed,
                                error.getMessage() == null
                                        ? error.getClass().getSimpleName()
                                        : error.getMessage()),
                        Toast.LENGTH_LONG).show());
            }
        }, "lsx4-ngs2-import");
        importer.setDaemon(true);
        importer.start();
    }

    private boolean isNgs2ModuleInstalled() {
        File module = new File(new File(new File(getFilesDir(), "lsx4-home"),
                "sys_modules"), NGS2_MODULE_NAME);
        if (!module.isFile() || module.length() < 64) {
            return false;
        }
        try {
            return hasSupportedModuleMagic(module);
        } catch (IOException ignored) {
            return false;
        }
    }

    private void importNgs2Module(Uri source) throws IOException {
        String displayName = selectedFileName(source);
        if (displayName == null || !NGS2_MODULE_NAME.equalsIgnoreCase(displayName.trim())) {
            throw new IOException(getString(R.string.audio_ngs2_error_name, NGS2_MODULE_NAME));
        }
        File modules = new File(new File(getFilesDir(), "lsx4-home"), "sys_modules");
        if (!modules.isDirectory() && !modules.mkdirs()) {
            throw new IOException(getString(R.string.audio_ngs2_error_directory));
        }
        File temporary = new File(modules, "." + NGS2_MODULE_NAME + ".importing");
        File destination = new File(modules, NGS2_MODULE_NAME);
        long copied = 0;
        try (InputStream input = getContentResolver().openInputStream(source);
             FileOutputStream output = new FileOutputStream(temporary)) {
            if (input == null) {
                throw new IOException(getString(R.string.audio_ngs2_error_open));
            }
            byte[] buffer = new byte[256 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                if (count == 0) {
                    continue;
                }
                if (copied > MAX_SYSTEM_MODULE_BYTES - count) {
                    throw new IOException(getString(R.string.audio_ngs2_error_size));
                }
                output.write(buffer, 0, count);
                copied += count;
            }
            output.getFD().sync();
        } catch (IOException error) {
            temporary.delete();
            throw error;
        }
        if (copied < 64 || !hasSupportedModuleMagic(temporary)) {
            temporary.delete();
            throw new IOException(getString(R.string.audio_ngs2_error_format));
        }
        try {
            Files.move(temporary.toPath(), destination.toPath(),
                    StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException ignored) {
            Files.move(temporary.toPath(), destination.toPath(),
                    StandardCopyOption.REPLACE_EXISTING);
        }
        TestModeManager.forgetManagedModule(this);
    }

    private String selectedFileName(Uri source) {
        try (Cursor cursor = getContentResolver().query(source,
                new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && !cursor.isNull(0)) {
                return cursor.getString(0);
            }
        } catch (RuntimeException ignored) {
        }
        return source.getLastPathSegment();
    }

    private static boolean hasSupportedModuleMagic(File module) throws IOException {
        byte[] magic = new byte[4];
        try (FileInputStream input = new FileInputStream(module)) {
            if (input.read(magic) != magic.length) {
                return false;
            }
        }
        boolean elf = (magic[0] & 0xff) == 0x7f
                && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
        boolean ps4Self = (magic[0] & 0xff) == 0x4f
                && (magic[1] & 0xff) == 0x15
                && (magic[2] & 0xff) == 0x3d
                && (magic[3] & 0xff) == 0x1d;
        return elf || ps4Self;
    }

    private void buildControls(LinearLayout root) {
        root.addView(preferenceCheck(R.string.controls_enable, K_OVERLAY, true));
        TextView editor = label(getString(R.string.controls_customize_overlay), 15, 0xffeef4fb);
        editor.setGravity(Gravity.CENTER_VERTICAL);
        editor.setMinHeight(dp(58));
        editor.setPadding(dp(18), dp(12), dp(18), dp(12));
        editor.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        editor.setClickable(true);
        editor.setFocusable(true);
        editor.setContentDescription(getString(R.string.controls_customize_overlay_summary));
        editor.setOnClickListener(view -> {
            Intent intent = new Intent(this, OverlayEditorActivity.class);
            if (!gameTitleId.isEmpty()) {
                intent.putExtra(GameProfileSettings.EXTRA_GAME_TITLE_ID, gameTitleId);
                intent.putExtra(GameProfileSettings.EXTRA_GAME_TITLE, gameTitle);
            }
            startActivity(intent);
        });
        root.addView(editor, cardMargins());
        root.addView(hint(getString(R.string.controls_customize_overlay_summary)));

        root.addView(section(getString(R.string.controls_optional_buttons)));
        root.addView(preferenceCheck(R.string.controls_show_share,
                K_OVERLAY_SHOW_SHARE, false));
        root.addView(preferenceCheck(R.string.controls_show_ps,
                K_OVERLAY_SHOW_PS, false));
        root.addView(preferenceCheck(R.string.controls_show_touchpad,
                K_OVERLAY_SHOW_TOUCHPAD, false));

        root.addView(section(getString(R.string.controls_opacity)));
        TextView value = label("", 12, 0xff8d98a5);
        SeekBar opacity = new SeekBar(this);
        opacity.setMax(100);
        opacity.setMin(20);
        opacity.setProgress(prefs(this).getInt(K_OVERLAY_OPACITY, 74));
        value.setText(opacity.getProgress() + "%");
        opacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                value.setText(progress + "%");
                if (fromUser) {
                    prefs(SettingsActivity.this).edit()
                            .putInt(K_OVERLAY_OPACITY, progress).apply();
                }
            }

            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        root.addView(opacity, matchWrap());
        root.addView(value, matchWrap());
        TextView gamepadHint = hint(getString(R.string.controls_gamepad_hint));
        gamepadHint.setPadding(0, dp(12), 0, 0);
        root.addView(gamepadHint);

        root.addView(section(getString(R.string.controls_phone_triggers)));
        root.addView(phoneTriggerRow(HardwareTriggerMapper.SLOT_LEFT,
                R.string.controls_phone_trigger_left), cardMargins());
        root.addView(phoneTriggerRow(HardwareTriggerMapper.SLOT_RIGHT,
                R.string.controls_phone_trigger_right), cardMargins());
        root.addView(hint(getString(R.string.controls_phone_trigger_hint)));
    }

    private View phoneTriggerRow(int slot, int titleResource) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(14), dp(10), dp(10), dp(12));
        card.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        card.addView(label(getString(titleResource), 14, 0xffd7dee7), matchWrap());

        LinearLayout controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.HORIZONTAL);
        controls.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams controlsParams = matchWrapWithMargins(0, dp(6), 0, 0);
        card.addView(controls, controlsParams);

        Button detect = new Button(this);
        triggerCaptureButtons[slot] = detect;
        refreshTriggerCaptureButton(slot);
        detect.setAllCaps(false);
        detect.setOnClickListener(view -> {
            triggerCaptureSlot = slot;
            detect.setText(R.string.controls_trigger_press_now);
        });
        detect.setOnLongClickListener(view -> {
            HardwareTriggerMapper.clear(prefs(this), slot);
            if (triggerCaptureSlot == slot) {
                triggerCaptureSlot = -1;
            }
            refreshTriggerCaptureButton(slot);
            return true;
        });
        controls.addView(detect, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Spinner target = spinner(HardwareTriggerMapper.TARGET_LABELS);
        target.setSelection(HardwareTriggerMapper.targetIndex(prefs(this), slot), false);
        target.setOnItemSelectedListener(saveSpinner(target, position ->
                HardwareTriggerMapper.setTargetIndex(prefs(this), slot, position)));
        LinearLayout.LayoutParams targetParams = new LinearLayout.LayoutParams(
                dp(174), ViewGroup.LayoutParams.WRAP_CONTENT);
        targetParams.leftMargin = dp(8);
        controls.addView(target, targetParams);
        return card;
    }

    private void refreshTriggerCaptureButton(int slot) {
        TextView button = slot >= 0 && slot < triggerCaptureButtons.length
                ? triggerCaptureButtons[slot] : null;
        if (button == null) {
            return;
        }
        SharedPreferences preferences = prefs(this);
        button.setText(HardwareTriggerMapper.isCaptured(preferences, slot)
                ? getString(R.string.controls_trigger_detected,
                        HardwareTriggerMapper.describe(preferences, slot))
                : getString(R.string.controls_trigger_detect));
    }

    private void buildCache(LinearLayout root) {
        SharedPreferences preferences = prefs(this);
        boolean enabled;
        if (gameTitleId.isEmpty()) {
            enabled = GameCacheManager.configuredEnabled(this, preferences);
            try {
                enabled = GameCacheManager.applyStoredSetting(this, preferences);
            } catch (IOException error) {
                Toast.makeText(this, getString(
                        R.string.cache_setting_failed, friendlyMessage(error)),
                        Toast.LENGTH_LONG).show();
            }
        } else {
            enabled = preferences.getBoolean(K_PERSISTENT_JIT_CACHE, true);
        }

        CheckBox cacheEnabled = check(getString(R.string.cache_enable), enabled);
        cacheEnabled.setLayoutParams(cardMargins());
        final boolean[] updating = {false};
        cacheEnabled.setOnCheckedChangeListener((button, checked) -> {
            if (updating[0]) {
                return;
            }
            final boolean previous = !checked;
            try {
                if (gameTitleId.isEmpty()) {
                    GameCacheManager.setUserEnabled(this, checked);
                }
                if (!preferences.edit()
                        .putBoolean(K_PERSISTENT_JIT_CACHE, checked)
                        .commit()) {
                    throw new IOException("SharedPreferences commit failed");
                }
            } catch (IOException error) {
                if (gameTitleId.isEmpty()) {
                    try {
                        GameCacheManager.setUserEnabled(this, previous);
                    } catch (IOException ignored) {
                    }
                }
                updating[0] = true;
                button.setChecked(previous);
                updating[0] = false;
                Toast.makeText(this, getString(
                        R.string.cache_setting_failed,
                        friendlyMessage(error)), Toast.LENGTH_LONG).show();
            }
        });
        root.addView(cacheEnabled);
        root.addView(hint(getString(R.string.cache_hint)));
        root.addView(section(getString(R.string.cache_games)));

        LinearLayout cacheList = new LinearLayout(this);
        cacheList.setOrientation(LinearLayout.VERTICAL);
        root.addView(cacheList, matchWrap());
        refreshGameCacheList(cacheList);
    }

    private void refreshGameCacheList(LinearLayout cacheList) {
        cacheList.removeAllViews();
        TextView scanning = hint(getString(R.string.cache_scanning));
        scanning.setPadding(dp(8), dp(10), dp(8), dp(10));
        cacheList.addView(scanning);

        Thread worker = new Thread(() -> {
            List<GameCacheManager.Entry> entries = null;
            Exception failure = null;
            try {
                entries = GameCacheManager.scan(this);
            } catch (Exception error) {
                failure = error;
            }
            final List<GameCacheManager.Entry> result = entries;
            final Exception error = failure;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) {
                    return;
                }
                cacheList.removeAllViews();
                if (error != null) {
                    TextView failed = hint(getString(
                            R.string.cache_scan_failed,
                            friendlyMessage(error)));
                    failed.setPadding(dp(8), dp(10), dp(8), dp(10));
                    cacheList.addView(failed);
                    return;
                }
                if (result == null || result.isEmpty()) {
                    TextView empty = hint(getString(R.string.cache_empty));
                    empty.setPadding(dp(8), dp(10), dp(8), dp(10));
                    cacheList.addView(empty);
                    return;
                }
                for (GameCacheManager.Entry entry : result) {
                    if (!gameTitleId.isEmpty()
                            && !gameTitleId.equalsIgnoreCase(entry.titleId)) {
                        continue;
                    }
                    cacheList.addView(cacheRow(entry, cacheList),
                            cardMargins());
                }
            });
        }, "lsx4-cache-list-scan");
        worker.setDaemon(true);
        worker.start();
    }

    private View cacheRow(GameCacheManager.Entry entry,
                          LinearLayout cacheList) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(10), dp(10), dp(10));
        row.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        row.setElevation(dp(2));

        ImageView icon = new ImageView(this);
        icon.setScaleType(ImageView.ScaleType.CENTER_CROP);
        Bitmap bitmap = decodeCacheIcon(entry.icon, 144);
        if (bitmap != null) {
            icon.setImageBitmap(bitmap);
        } else {
            icon.setImageResource(R.drawable.lsx4_launcher_icon);
            icon.setPadding(dp(8), dp(8), dp(8), dp(8));
            icon.setBackground(cardBackground(
                    0xff2a313a, 0xff3c4652, 10));
        }
        LinearLayout.LayoutParams iconParams =
                new LinearLayout.LayoutParams(dp(62), dp(62));
        iconParams.setMargins(0, 0, dp(12), 0);
        row.addView(icon, iconParams);

        LinearLayout details = new LinearLayout(this);
        details.setOrientation(LinearLayout.VERTICAL);
        TextView gameTitle = label(entry.title, 15, 0xffeef2f6);
        gameTitle.setTypeface(android.graphics.Typeface.DEFAULT,
                android.graphics.Typeface.BOLD);
        details.addView(gameTitle);
        details.addView(label(entry.titleId, 11, 0xff7f8a97));
        TextView total = label(
                formatCacheBytes(entry.totalBytes()), 13, 0xffd7dee7);
        total.setPadding(0, dp(4), 0, 0);
        details.addView(total);
        details.addView(label(getString(
                R.string.cache_size_breakdown,
                formatCacheBytes(entry.jitBytes),
                formatCacheBytes(entry.gpuBytes)), 10, 0xff7f8a97));
        row.addView(details, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        ImageView delete = new ImageView(this);
        delete.setImageResource(android.R.drawable.ic_menu_delete);
        delete.setColorFilter(0xffff806f);
        delete.setPadding(dp(12), dp(12), dp(12), dp(12));
        delete.setContentDescription(getString(
                R.string.cache_delete_action, entry.title));
        delete.setClickable(true);
        delete.setFocusable(true);
        delete.setBackground(cardBackground(
                0xff232a32, 0xff3d4854, 12));
        delete.setOnClickListener(view ->
                confirmCacheDelete(entry, cacheList, delete));
        LinearLayout.LayoutParams deleteParams =
                new LinearLayout.LayoutParams(dp(48), dp(48));
        deleteParams.setMargins(dp(8), 0, 0, 0);
        row.addView(delete, deleteParams);
        return row;
    }

    private void confirmCacheDelete(GameCacheManager.Entry entry,
                                    LinearLayout cacheList,
                                    ImageView delete) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.cache_delete_title)
                .setMessage(getString(R.string.cache_delete_message,
                        entry.title, formatCacheBytes(entry.totalBytes())))
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.cache_delete_confirm,
                        (dialog, which) -> {
                            if (isGameProcessActive()) {
                                Toast.makeText(this,
                                        R.string.cache_delete_game_running,
                                        Toast.LENGTH_LONG).show();
                                return;
                            }
                            delete.setEnabled(false);
                            Thread worker = new Thread(() -> {
                                Exception failure = null;
                                try {
                                    GameCacheManager.delete(this, entry);
                                } catch (Exception error) {
                                    failure = error;
                                }
                                final Exception error = failure;
                                runOnUiThread(() -> {
                                    if (isFinishing() || isDestroyed()) {
                                        return;
                                    }
                                    if (error != null) {
                                        delete.setEnabled(true);
                                        Toast.makeText(this, getString(
                                                R.string.cache_delete_failed,
                                                friendlyMessage(error)),
                                                Toast.LENGTH_LONG).show();
                                        return;
                                    }
                                    Toast.makeText(this, getString(
                                            R.string.cache_deleted,
                                            entry.title),
                                            Toast.LENGTH_SHORT).show();
                                    refreshGameCacheList(cacheList);
                                });
                            }, "lsx4-cache-delete");
                            worker.setDaemon(true);
                            worker.start();
                        })
                .show();
    }

    private boolean isGameProcessActive() {
        ActivityManager manager =
                (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (manager == null) {
            return false;
        }
        List<ActivityManager.RunningAppProcessInfo> processes =
                manager.getRunningAppProcesses();
        if (processes == null) {
            return false;
        }
        String gameProcess = getPackageName() + ":game";
        for (ActivityManager.RunningAppProcessInfo process : processes) {
            if (gameProcess.equals(process.processName)
                    && process.importance
                    <= ActivityManager.RunningAppProcessInfo
                            .IMPORTANCE_SERVICE) {
                return true;
            }
        }
        return false;
    }

    private static Bitmap decodeCacheIcon(File icon, int requestedPixels) {
        if (icon == null || !icon.isFile()) {
            return null;
        }
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(icon.getAbsolutePath(), bounds);
            int sample = 1;
            while (bounds.outWidth / sample > requestedPixels * 2
                    || bounds.outHeight / sample > requestedPixels * 2) {
                sample *= 2;
            }
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = sample;
            return BitmapFactory.decodeFile(
                    icon.getAbsolutePath(), options);
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static String formatCacheBytes(long bytes) {
        if (bytes >= 1024L * 1024L * 1024L) {
            return String.format(Locale.US, "%.2f GiB",
                    bytes / (1024.0 * 1024.0 * 1024.0));
        }
        if (bytes >= 1024L * 1024L) {
            return String.format(Locale.US, "%.1f MiB",
                    bytes / (1024.0 * 1024.0));
        }
        if (bytes >= 1024L) {
            return String.format(Locale.US, "%.1f KiB",
                    bytes / 1024.0);
        }
        return bytes + " B";
    }

    private static String friendlyMessage(Throwable error) {
        String message = error.getMessage();
        return message == null || message.trim().isEmpty()
                ? error.getClass().getSimpleName() : message;
    }

    private void buildCpuTuning(LinearLayout root) {
        SharedPreferences preferences = basePrefs();
        LinearLayout controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.VERTICAL);
        if (!gameTitleId.isEmpty()) {
            TextView note = hint(getString(R.string.game_profile_scope_note, gameTitle));
            note.setPadding(dp(12), dp(10), dp(12), dp(10));
            root.addView(note, matchWrapWithMargins(0, 0, 0, dp(8)));
            root.addView(controls, matchWrap());
            renderCpuTuningControls(controls, CpuTuningSettings.gameScope(gameTitleId));
            return;
        }

        CheckBox applyAllGames = check(
                getString(R.string.cpu_tuning_apply_all_games),
                CpuTuningSettings.allGamesProfileEnabled(preferences));
        applyAllGames.setLayoutParams(cardMargins());
        root.addView(applyAllGames);
        TextView applyAllGamesHint = hint(
                getString(R.string.cpu_tuning_apply_all_games_hint));
        applyAllGamesHint.setPadding(dp(8), 0, dp(8), dp(6));
        root.addView(applyAllGamesHint);

        TextView exceptions = label("", 15, 0xffdce7f3);
        exceptions.setGravity(Gravity.CENTER_VERTICAL);
        exceptions.setMinHeight(dp(58));
        exceptions.setPadding(dp(18), dp(12), dp(18), dp(12));
        exceptions.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        exceptions.setClickable(true);
        exceptions.setFocusable(true);
        exceptions.setOnClickListener(view -> showCpuAllGamesExceptions(exceptions));
        root.addView(exceptions, cardMargins());
        root.addView(controls, matchWrap());

        updateCpuExceptionControl(exceptions, applyAllGames.isChecked());
        applyAllGames.setOnCheckedChangeListener((button, checked) -> {
            SharedPreferences live = basePrefs();
            if (checked) {
                CpuTuningSettings.activateCurrentProfileForAllGames(live, "");
                renderCpuTuningControls(controls, CpuTuningSettings.ALL_GAMES_PREFIX);
            } else {
                CpuTuningSettings.setAllGamesProfileEnabled(live, false);
                renderCpuTuningControls(controls, CpuTuningSettings.GLOBAL_PREFIX);
            }
            updateCpuExceptionControl(exceptions, checked);
        });
        renderCpuTuningControls(controls,
                applyAllGames.isChecked() ? CpuTuningSettings.ALL_GAMES_PREFIX
                        : CpuTuningSettings.GLOBAL_PREFIX);
    }

    private void updateCpuExceptionControl(TextView control, boolean enabled) {
        java.util.Set<String> selected = CpuTuningSettings.allGamesExceptions(basePrefs());
        List<GameProfileSettings.GameTitle> games =
                GameProfileSettings.installedGames(this, basePrefs());
        List<String> names = new java.util.ArrayList<>();
        for (GameProfileSettings.GameTitle game : games) {
            if (selected.contains(game.titleId)) {
                names.add(game.title);
            }
        }
        String value;
        if (names.isEmpty()) {
            value = getString(R.string.cpu_tuning_exceptions_none);
        } else if (names.size() <= 2) {
            value = android.text.TextUtils.join(", ", names);
        } else {
            value = getString(R.string.cpu_tuning_exceptions_count, names.size());
        }
        control.setText(getString(R.string.cpu_tuning_exceptions, value));
        control.setEnabled(enabled && !games.isEmpty());
        control.setAlpha(control.isEnabled() ? 1f : 0.52f);
        control.setVisibility(enabled ? View.VISIBLE : View.GONE);
    }

    private void showCpuAllGamesExceptions(TextView control) {
        List<GameProfileSettings.GameTitle> games =
                GameProfileSettings.installedGames(this, basePrefs());
        if (games.isEmpty()) {
            return;
        }
        java.util.Set<String> selected = CpuTuningSettings.allGamesExceptions(basePrefs());
        String[] labels = new String[games.size()];
        boolean[] checked = new boolean[games.size()];
        for (int index = 0; index < games.size(); index++) {
            labels[index] = games.get(index).title;
            checked[index] = selected.contains(games.get(index).titleId);
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.cpu_tuning_exceptions_title)
                .setMultiChoiceItems(labels, checked, (dialog, which, isChecked) -> {
                    String titleId = games.get(which).titleId;
                    if (isChecked) {
                        selected.add(titleId);
                    } else {
                        selected.remove(titleId);
                    }
                })
                .setPositiveButton(android.R.string.ok, (dialog, which) -> {
                    CpuTuningSettings.setAllGamesExceptions(basePrefs(), selected);
                    updateCpuExceptionControl(control, true);
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void renderCpuTuningControls(LinearLayout container, String scope) {
        container.removeAllViews();
        SharedPreferences preferences = prefs(this);
        boolean global = CpuTuningSettings.GLOBAL_PREFIX.equals(scope);
        boolean allGames = CpuTuningSettings.ALL_GAMES_PREFIX.equals(scope);
        String titleId = global || allGames ? "" : scope.substring(
                CpuTuningSettings.GAME_PREFIX.length(), scope.length() - 1);
        Map<String, Integer> selectedValues =
                CpuTuningSettings.selectedValues(preferences, titleId);
        Map<String, Integer> effectiveValues =
                CpuTuningSettings.effectiveRuntimeValues(preferences, titleId);
        int currentPreset = selectedValues.get("preset");
        Spinner preset = spinner(new String[]{
                "Compatibility", "Balanced", "Performance", "Extreme", "Custom"
        });
        preset.setSelection(currentPreset, false);
        preset.setOnItemSelectedListener(saveSpinner(preset, position -> {
            if (position != currentPreset) {
                if (position < CpuTuningSettings.PRESET_CUSTOM) {
                    if (!global && !allGames) {
                        enableCpuGameOverrideIfNeeded(titleId);
                    }
                    CpuTuningSettings.applyPreset(
                            prefs(SettingsActivity.this), scope, position);
                } else {
                    if (!global && !allGames) {
                        enableCpuGameOverrideIfNeeded(titleId);
                    }
                    prefs(SettingsActivity.this).edit().putInt(
                            CpuTuningSettings.fieldKey(scope, "preset"),
                            CpuTuningSettings.PRESET_CUSTOM).commit();
                }
                renderCpuTuningControls(container, scope);
            }
        }));
        View presetRow = settingRow("CPU preset", preset);
        presetRow.setLayoutParams(cardMargins());
        container.addView(presetRow);
        TextView restartHint = hint(
                (allGames
                        ? "Editing the shared profile currently used by every game. "
                        : global
                        ? "Controls show global launch selections. Per-game GameDB values can "
                                + "override them before explicit user values. "
                        : "Controls include Global, GameDB and enabled per-game overrides for "
                                + displayGameTitle(titleId) + ". ")
                        + "Every row reports the effective next-launch value after safety and "
                        + "Auto resolution. Changes are saved immediately and require a "
                        + "game-process restart.");
        restartHint.setPadding(dp(8), dp(8), dp(8), dp(4));
        container.addView(restartHint);

        for (int renderedSection = CpuTuningSettings.BASIC;
             renderedSection <= CpuTuningSettings.DANGEROUS; renderedSection++) {
                String sectionTitle = renderedSection == CpuTuningSettings.BASIC
                        ? "Basic"
                        : (renderedSection == CpuTuningSettings.ADVANCED
                                ? "Advanced" : "Dangerous / experimental");
                container.addView(section(sectionTitle));
                if (renderedSection == CpuTuningSettings.DANGEROUS) {
                    TextView warning = hint(
                            "These options can trade correctness for speed. Unsafe memory is "
                                    + "never enabled unless both unsafe selectors are active.");
                    warning.setPadding(dp(8), 0, dp(8), dp(6));
                    container.addView(warning);
                }
            for (CpuTuningSettings.Field field : CpuTuningSettings.FIELDS) {
                if (field.section == renderedSection) {
                    if ("per_game_profile".equals(field.key)) {


                        continue;
                    }
                    if ("jit_disable_mask".equals(field.key)
                            && selectedValues.get("jit_feature_mask") != 2) {
                        continue;
                    }
                    if ("guest_thread_delay_us".equals(field.key)
                            && selectedValues.get("guest_thread_delay_mode") != 1) {
                        continue;
                    }
                    addCpuTuningField(container, preferences, scope, titleId,
                            field, selectedValues.get(field.key),
                            effectiveValues.get(field.key));
                }
            }
        }
    }

    private CpuTuningSettings.Field findCpuTuningField(String key) {
        for (CpuTuningSettings.Field field : CpuTuningSettings.FIELDS) {
            if (key.equals(field.key)) {
                return field;
            }
        }
        throw new IllegalStateException("Missing CPU tuning field " + key);
    }

    private void addCpuTuningField(LinearLayout container, SharedPreferences preferences,
                                   String scope, String titleId,
                                   CpuTuningSettings.Field field, int current,
                                   int effective) {
        if ("jit_disable_mask".equals(field.key)) {
            addCpuJitFeatureMask(container, preferences, scope, titleId,
                    field, current, effective);
            return;
        }
        if (field.type == CpuTuningSettings.TYPE_BOOLEAN) {
            CheckBox control = check(field.title, current != 0);
            control.setOnCheckedChangeListener((button, checked) -> {
                    enableCpuGameOverrideIfNeeded(titleId);
                    CpuTuningSettings.write(prefs(SettingsActivity.this), scope,
                            field, checked ? 1 : 0);
                    container.post(() -> renderCpuTuningControls(container, scope));
            });
            container.addView(control, cardMargins());
            addCpuTuningDescription(container, field, current, effective);
            return;
        }
        if (field.type == CpuTuningSettings.TYPE_CHOICE) {
            Spinner control = spinner(field.choices);
            control.setSelection(current, false);
            control.setOnItemSelectedListener(saveSpinner(control, position -> {
                if (position != current) {
                    enableCpuGameOverrideIfNeeded(titleId);
                    CpuTuningSettings.write(prefs(SettingsActivity.this), scope,
                            field, position);
                    container.post(() -> renderCpuTuningControls(container, scope));
                }
            }));
            View row = settingRow(field.title, control);
            row.setLayoutParams(cardMargins());
            container.addView(row);
            addCpuTuningDescription(container, field, current, effective);
            return;
        }

        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(14), dp(8), dp(10), dp(8));
        card.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        TextView value = label(field.title + ": " + current, 14, 0xffd7dee7);
        SeekBar slider = new SeekBar(this);
        slider.setMax(field.maximum - field.minimum);
        slider.setProgress(current - field.minimum);
        final int[] selectedValue = {current};
        slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int selected = field.minimum + progress;
                selectedValue[0] = selected;
                value.setText(field.title + ": " + selected);
            }

            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {
                if (selectedValue[0] != current) {
                    enableCpuGameOverrideIfNeeded(titleId);
                    CpuTuningSettings.write(prefs(SettingsActivity.this), scope,
                            field, selectedValue[0]);
                    container.post(() -> renderCpuTuningControls(container, scope));
                }
            }
        });
        card.addView(value, matchWrap());
        TextView description = hint(field.description);
        description.setPadding(0, dp(3), 0, dp(3));
        card.addView(description, matchWrap());
        TextView effectiveValue = hint(formatCpuTuningEffectiveValue(
                field, current, effective));
        effectiveValue.setPadding(0, 0, 0, dp(3));
        card.addView(effectiveValue, matchWrap());
        card.addView(slider, matchWrap());
        container.addView(card, cardMargins());
    }

    private void addCpuJitFeatureMask(LinearLayout container,
                                      SharedPreferences preferences, String scope,
                                      String titleId, CpuTuningSettings.Field field,
                                      int current, int effective) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(14), dp(8), dp(10), dp(8));
        card.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        card.addView(label(field.title + ": 0x" + Integer.toHexString(current),
                14, 0xffd7dee7), matchWrap());
        TextView description = hint(field.description);
        description.setPadding(0, dp(3), 0, dp(3));
        card.addView(description, matchWrap());
        for (int bit = 0; bit < CpuTuningSettings.JIT_FEATURE_NAMES.length; bit++) {
            final int featureBit = bit;
            CheckBox disable = check("Disable "
                    + CpuTuningSettings.JIT_FEATURE_NAMES[bit],
                    (current & (1 << bit)) != 0);
            disable.setOnCheckedChangeListener((button, checked) -> {
                int live = CpuTuningSettings.selectedValues(
                        prefs(SettingsActivity.this), titleId).get(field.key);
                int updated = checked
                        ? live | (1 << featureBit)
                        : live & ~(1 << featureBit);
                enableCpuGameOverrideIfNeeded(titleId);
                CpuTuningSettings.write(prefs(SettingsActivity.this), scope,
                        field, updated);
                container.post(() -> renderCpuTuningControls(container, scope));
            });
            card.addView(disable, matchWrap());
        }
        TextView effectiveValue = hint(formatCpuTuningEffectiveValue(
                field, current, effective));
        effectiveValue.setPadding(0, dp(2), 0, 0);
        card.addView(effectiveValue, matchWrap());
        container.addView(card, cardMargins());
    }

    private void addCpuTuningDescription(LinearLayout container,
                                         CpuTuningSettings.Field field,
                                         int current, int effective) {
        TextView description = hint(field.description + "\n"
                + formatCpuTuningEffectiveValue(field, current, effective));
        description.setPadding(dp(12), 0, dp(12), dp(5));
        container.addView(description, matchWrap());
    }

    private void enableCpuGameOverrideIfNeeded(String titleId) {
        if (!titleId.isEmpty()) {
            CpuTuningSettings.setGameProfileEnabled(
                    prefs(SettingsActivity.this), titleId, true);
            if (CpuTuningSettings.allGamesProfileEnabled(basePrefs())) {
                CpuTuningSettings.setAllGamesException(basePrefs(), titleId, true);
            }
        }
    }

    private String displayGameTitle(String titleId) {
        if (titleId.equals(gameTitleId) && !gameTitle.trim().isEmpty()) {
            return gameTitle;
        }
        for (GameProfileSettings.GameTitle game
                : GameProfileSettings.installedGames(this, basePrefs())) {
            if (game.titleId.equals(titleId)) {
                return game.title;
            }
        }
        return getString(R.string.game_profile_unnamed_title);
    }

    private static String formatCpuTuningEffectiveValue(
            CpuTuningSettings.Field field, int selected, int effective) {
        String effectiveText = formatCpuTuningValue(field, effective);
        if (selected == effective) {
            return "Effective next launch: " + effectiveText;
        }
        return "Selected: " + formatCpuTuningValue(field, selected)
                + " · Effective next launch: " + effectiveText;
    }

    private static String formatCpuTuningValue(
            CpuTuningSettings.Field field, int value) {
        if (field.type == CpuTuningSettings.TYPE_BOOLEAN) {
            return value == 0 ? "Off" : "On";
        }
        if (field.type == CpuTuningSettings.TYPE_CHOICE
                && value >= 0 && value < field.choices.length) {
            return field.choices[value];
        }
        if ("jit_disable_mask".equals(field.key)) {
            return "0x" + Integer.toHexString(value);
        }
        return Integer.toString(value);
    }

    private void buildManagedOptimizations(LinearLayout root) {
        SharedPreferences optimizationPreferences = prefs(this);
        boolean driverAxisSupported = VulkanDriverManager.isAdrenoPlatform()
                && VulkanDriverManager.isLoaderBundleAvailable(this);
        vulkanDriverAxisSupported = driverAxisSupported;
        List<VulkanDriverManager.DriverEntry> vulkanDrivers = driverAxisSupported
                ? VulkanDriverManager.listDrivers(this)
                : VulkanDriverManager.listDrivers(this).subList(0, 1);
        String selectedDriver = VulkanDriverManager.readSelectionId(
                this, optimizationPreferences);
        String recoveryNotice = VulkanDriverManager.consumeRecoveryNotice(
                this, optimizationPreferences);
        if (!driverAxisSupported || !recoveryNotice.isEmpty()
                || driverIndex(vulkanDrivers, selectedDriver) < 0) {
            selectedDriver = VulkanDriverManager.SELECTION_SYSTEM;
        }
        final String[] currentDriver = {selectedDriver};
        Spinner vulkanDriverSpinner = spinner(
                vulkanDriverLabels(vulkanDrivers, selectedDriver, recoveryNotice));
        vulkanDriverSpinnerControl = vulkanDriverSpinner;
        vulkanDriverSpinner.setSelection(
                Math.max(0, driverIndex(vulkanDrivers, selectedDriver)), false);
        vulkanDriverSpinner.setEnabled(driverAxisSupported);
        if (driverAxisSupported) {
            vulkanDriverSpinner.setOnItemSelectedListener(saveSpinner(vulkanDriverSpinner, position -> {
                if (isVulkanDriverBusy()) {
                    int currentPosition = driverIndex(vulkanDrivers, currentDriver[0]);
                    if (currentPosition >= 0) {
                        vulkanDriverSpinner.setSelection(currentPosition, false);
                    }
                    return;
                }
                if (position < 0 || position >= vulkanDrivers.size()) {
                    return;
                }
                String requested = vulkanDrivers.get(position).selectionId;
                if (requested.equals(currentDriver[0])) {
                    return;
                }
                selectVulkanDriver(requested, vulkanDriverSpinner);
            }));
        }

        LinearLayout driverControls = new LinearLayout(this);
        driverControls.setOrientation(LinearLayout.HORIZONTAL);
        driverControls.setGravity(Gravity.CENTER_VERTICAL);
        driverControls.addView(vulkanDriverSpinner, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView importTurnip = label("+", 26, 0xff6fb4ff);
        importTurnip.setGravity(Gravity.CENTER);
        importTurnip.setMinWidth(dp(48));
        importTurnip.setMinHeight(dp(48));
        importTurnip.setContentDescription(
                getString(R.string.optimization_vulkan_driver_import));
        importTurnip.setBackground(cardBackground(0xff202832, 0xff3a4652, 12));
        importTurnip.setClickable(true);
        importTurnip.setFocusable(true);
        importTurnip.setEnabled(driverAxisSupported);
        importTurnip.setAlpha(driverAxisSupported ? 1.0f : 0.52f);
        importTurnip.setOnClickListener(view -> pickTurnipDriverArchive());
        vulkanDriverImportControl = importTurnip;
        LinearLayout.LayoutParams importButtonParams = new LinearLayout.LayoutParams(
                dp(48), dp(48));
        importButtonParams.setMargins(dp(6), 0, 0, 0);
        driverControls.addView(importTurnip, importButtonParams);
        updateVulkanDriverControls();

        View vulkanDriverRow = driverSettingRow(
                getString(R.string.optimization_vulkan_driver_axis), driverControls);
        vulkanDriverRow.setLayoutParams(cardMargins());
        root.addView(vulkanDriverRow);
        if (!recoveryNotice.isEmpty()) {
            Toast.makeText(this, getString(
                    R.string.optimization_vulkan_driver_recovery, recoveryNotice),
                    Toast.LENGTH_LONG).show();
        }

        root.addView(preferenceCheck(R.string.optimization_arm_gpu_fast_path,
                K_ARM_GPU_FAST_PATH, true));
        TextView warning = hint(getString(R.string.optimization_arm_gpu_fast_path_hint));
        warning.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(warning);

        root.addView(preferenceCheck(R.string.optimization_coarse_fragment_2x2,
                K_COARSE_FRAGMENT_2X2, false));
        TextView coarseFragmentWarning = hint(
                getString(R.string.optimization_coarse_fragment_2x2_hint));
        coarseFragmentWarning.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(coarseFragmentWarning);

        int anisotropyMode = optimizationPreferences.contains(K_ANISOTROPY_MODE)
                ? optimizationPreferences.getInt(
                        K_ANISOTROPY_MODE, ANISOTROPY_GUEST)
                : (optimizationPreferences.getBoolean(
                        K_LIMIT_ANISOTROPY_2X, false)
                        ? ANISOTROPY_2X : ANISOTROPY_GUEST);
        anisotropyMode = Math.max(
                ANISOTROPY_GUEST,
                Math.min(ANISOTROPY_2X, anisotropyMode));
        Spinner anisotropy = spinner(new String[]{
                getString(R.string.optimization_anisotropy_guest),
                getString(R.string.optimization_anisotropy_1x),
                getString(R.string.optimization_anisotropy_2x)
        });
        anisotropy.setSelection(anisotropyMode);
        anisotropy.setOnItemSelectedListener(saveSpinner(anisotropy, position ->
                prefs(this).edit()
                        .putInt(K_ANISOTROPY_MODE, position)
                        .remove(K_LIMIT_ANISOTROPY_2X)
                        .apply()));
        View anisotropyRow = settingRow(
                getString(R.string.optimization_anisotropy_mode),
                anisotropy);
        anisotropyRow.setLayoutParams(cardMargins());
        root.addView(anisotropyRow);
        TextView anisotropyWarning = hint(
                getString(R.string.optimization_limit_anisotropy_2x_hint));
        anisotropyWarning.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(anisotropyWarning);

        root.addView(preferenceCheck(R.string.optimization_disable_vk_robustness,
                K_DISABLE_VK_ROBUSTNESS, false));
        TextView robustnessWarning = hint(
                getString(R.string.optimization_disable_vk_robustness_hint));
        robustnessWarning.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(robustnessWarning);

        CheckBox tieredJit = preferenceCheck(
                R.string.optimization_tiered_jit, K_TIERED_JIT, false);
        root.addView(tieredJit);
        TextView tieredJitWarning = hint(
                getString(R.string.optimization_tiered_jit_hint));
        tieredJitWarning.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(tieredJitWarning);

        CheckBox traceCompilation = preferenceCheck(
                R.string.optimization_jit_trace_compilation,
                K_JIT_TRACE_COMPILATION, false);
        root.addView(traceCompilation);
        tieredJit.setOnCheckedChangeListener((button, checked) -> {
            prefs(this).edit().putBoolean(K_TIERED_JIT, checked).apply();
            if (!checked && traceCompilation.isChecked()) {
                traceCompilation.setChecked(false);
            }
        });
        traceCompilation.setOnCheckedChangeListener((button, checked) -> {
            prefs(this).edit().putBoolean(
                    K_JIT_TRACE_COMPILATION, checked).apply();
            if (checked && !tieredJit.isChecked()) {
                tieredJit.setChecked(true);
            }
        });
        TextView traceJitWarning = hint(
                getString(R.string.optimization_jit_trace_compilation_hint));
        traceJitWarning.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(traceJitWarning);

        root.addView(preferenceCheck(R.string.optimization_fast_guest_memory,
                K_FAST_GUEST_MEMORY, false));
        TextView fastGuestMemoryHint = hint(
                getString(R.string.optimization_fast_guest_memory_hint));
        fastGuestMemoryHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(fastGuestMemoryHint);

        root.addView(preferenceCheck(R.string.optimization_guest_spin_wait_event,
                K_GUEST_SPIN_WAIT_EVENT, true));
        TextView guestSpinWaitEventHint = hint(
                getString(R.string.optimization_guest_spin_wait_event_hint));
        guestSpinWaitEventHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(guestSpinWaitEventHint);

        root.addView(preferenceCheck(R.string.optimization_relaxed_fp_fusion,
                K_RELAXED_FP_FUSION, false));
        TextView relaxedFpFusionHint = hint(
                getString(R.string.optimization_relaxed_fp_fusion_hint));
        relaxedFpFusionHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(relaxedFpFusionHint);

        root.addView(preferenceCheck(R.string.optimization_android_performance_hint,
                K_ANDROID_PERFORMANCE_HINT, true));
        TextView androidPerformanceHint = hint(
                getString(R.string.optimization_android_performance_hint_hint));
        androidPerformanceHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(androidPerformanceHint);

        root.addView(preferenceCheck(R.string.optimization_oem_root_performance,
                K_OEM_ROOT_PERFORMANCE, false));
        TextView oemRootPerformanceHint = hint(
                getString(R.string.optimization_oem_root_performance_hint));
        oemRootPerformanceHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(oemRootPerformanceHint);

        root.addView(preferenceCheck(R.string.optimization_host_flagm,
                K_HOST_FLAGM, true));
        TextView hostFlagMHint = hint(getString(R.string.optimization_host_flagm_hint));
        hostFlagMHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(hostFlagMHint);

        root.addView(preferenceCheck(R.string.optimization_host_sve2,
                K_HOST_SVE2, true));
        TextView hostSve2Hint = hint(getString(R.string.optimization_host_sve2_hint));
        hostSve2Hint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(hostSve2Hint);

        root.addView(preferenceCheck(R.string.optimization_host_rcpc,
                K_HOST_RCPC, true));
        TextView hostRcpcHint = hint(getString(R.string.optimization_host_rcpc_hint));
        hostRcpcHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(hostRcpcHint);

        root.addView(preferenceCheck(R.string.optimization_readback_batching,
                K_READBACK_BATCHING, true));
        TextView readbackHint = hint(
                getString(R.string.optimization_readback_batching_hint));
        readbackHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(readbackHint);

        root.addView(preferenceCheck(R.string.optimization_vulkan_driver_calls,
                K_VULKAN_DRIVER_CALLS, true));
        TextView driverCallsHint = hint(
                getString(R.string.optimization_vulkan_driver_calls_hint));
        driverCallsHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(driverCallsHint);

        root.addView(preferenceCheck(R.string.optimization_async_pipeline,
                K_ASYNC_PIPELINE, true));
        TextView asyncPipelineHint = hint(
                getString(R.string.optimization_async_pipeline_hint));
        asyncPipelineHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(asyncPipelineHint);

        root.addView(preferenceCheck(R.string.optimization_adaptive_mobile_gpu,
                K_ADAPTIVE_MOBILE_GPU, true));
        TextView mobileGpuHint = hint(
                getString(R.string.optimization_adaptive_mobile_gpu_hint));
        mobileGpuHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(mobileGpuHint);

        root.addView(preferenceCheck(R.string.optimization_audio_simd,
                K_AUDIO_SIMD, true));
        TextView audioSimdHint = hint(
                getString(R.string.optimization_audio_simd_hint));
        audioSimdHint.setPadding(dp(8), dp(10), dp(8), 0);
        root.addView(audioSimdHint);
    }

    private void buildOther(LinearLayout root) {
        root.addView(preferenceCheck(R.string.debug_hud, K_PERF_HUD, false));
        root.addView(preferenceCheck(R.string.debug_hud_upscaler_info,
                K_PERF_HUD_UPSCALER_INFO, false));
        if (!gameTitleId.isEmpty()) {
            return;
        }
        if (RemoteDiagnostics.isConfigured()) {
            CheckBox diagnostics = check(getString(R.string.remote_diagnostics),
                    RemoteDiagnostics.isEnabled(this));
            diagnostics.setLayoutParams(cardMargins());
            diagnostics.setOnCheckedChangeListener((button, checked) ->
                    RemoteDiagnostics.setEnabled(this, checked));
            root.addView(diagnostics);
            root.addView(hint(getString(R.string.remote_diagnostics_hint)));

            Button send = new Button(this);
            send.setText(R.string.remote_diagnostics_send);
            send.setAllCaps(false);
            send.setTextColor(0xffeef2f6);
            send.setBackground(cardBackground(0xff1f4f79, 0xff5aa9e6, 14));
            send.setOnClickListener(view -> {
                send.setEnabled(false);
                send.setText(R.string.remote_diagnostics_sending);
                RemoteDiagnostics.requestImmediateUpload(this, (success, message) ->
                        runOnUiThread(() -> {
                            send.setEnabled(true);
                            send.setText(R.string.remote_diagnostics_send);
                            Toast.makeText(this, success
                                            ? getString(R.string.remote_diagnostics_sent, message)
                                            : getString(R.string.remote_diagnostics_failed, message),
                                    Toast.LENGTH_LONG).show();
                        }));
            });
            root.addView(send, matchWrapWithMargins(0, dp(10), 0, dp(12)));
        }
        if (!TestModeManager.isDebugBuild(this)) {
            return;
        }

        SharedPreferences preferences = prefs(this);
        CheckBox testMode = check(getString(R.string.test_mode),
                preferences.getBoolean(K_TEST_MODE, false));
        testMode.setLayoutParams(cardMargins());
        attachTestModeListener(testMode, preferences);
        root.addView(testMode);

        int hint = TestModeManager.hasBundledModule(this)
                ? R.string.test_mode_hint
                : R.string.test_mode_module_unavailable;
        root.addView(hint(getString(hint)));
    }

    private void attachTestModeListener(CheckBox testMode, SharedPreferences preferences) {
        testMode.setOnCheckedChangeListener((button, checked) -> {
            testMode.setEnabled(false);
            Thread worker = new Thread(() -> {
                if (!checked) {
                    TestModeManager.removeManagedModule(this);
                    preferences.edit().putBoolean(K_TEST_MODE, false).apply();
                    runOnUiThread(() -> {
                        testMode.setEnabled(true);
                        Toast.makeText(this, R.string.test_mode_disabled,
                                Toast.LENGTH_LONG).show();
                    });
                    return;
                }
                try {
                    TestModeManager.Activation result = TestModeManager.activate(this);
                    preferences.edit()
                            .putBoolean(K_TEST_MODE, true)
                            .putBoolean(K_AUDIO, true)
                            .apply();
                    int message = result == TestModeManager.Activation.USER_MODULE_READY
                            ? R.string.test_mode_user_audio_ready
                            : R.string.test_mode_audio_ready;
                    runOnUiThread(() -> {
                        testMode.setEnabled(true);
                        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
                    });
                } catch (Exception error) {
                    preferences.edit().putBoolean(K_TEST_MODE, false).apply();
                    runOnUiThread(() -> {
                        testMode.setOnCheckedChangeListener(null);
                        testMode.setChecked(false);
                        testMode.setEnabled(true);
                        Toast.makeText(this, getString(R.string.test_mode_audio_failed,
                                error.getMessage() == null
                                        ? error.getClass().getSimpleName()
                                        : error.getMessage()), Toast.LENGTH_LONG).show();
                        attachTestModeListener(testMode, preferences);
                    });
                }
            }, "lsx4-test-mode-audio");
            worker.setDaemon(true);
            worker.start();
        });
    }

    private void addScreenHeader(LinearLayout root, String title) {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        TextView back = label("‹", 38, 0xff6fb4ff);
        back.setContentDescription(getString(R.string.back));
        back.setGravity(Gravity.CENTER);
        back.setOnClickListener(view -> finish());
        bar.addView(back, new LinearLayout.LayoutParams(dp(48), dp(48)));
        bar.addView(title(title), new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(bar, matchWrapWithMargins(-dp(8), 0, 0, dp(10)));
    }

    private String titleForScreen(String screen) {
        switch (screen) {
            case "graphics": return getString(R.string.category_graphics);
            case "gpu": return getString(R.string.graphics_gpu);
            case "patches": return getString(R.string.graphics_patches);
            case "audio": return getString(R.string.category_audio);
            case "controls": return getString(R.string.category_controls);
            case "cache": return getString(R.string.category_cache);
            case "managed_optimizations":
                return getString(R.string.category_managed_optimizations);
            case "cpu_tuning": return getString(R.string.category_cpu_tuning);
            case "other": return getString(R.string.category_other);
            default: return getString(R.string.settings_title);
        }
    }

    private View category(int titleResource, int summaryResource, String screen) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(16), dp(14), dp(12), dp(14));
        card.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 16));
        card.setClickable(true);
        card.setFocusable(true);

        LinearLayout text = new LinearLayout(this);
        text.setOrientation(LinearLayout.VERTICAL);
        text.addView(label(getString(titleResource), 16, 0xffeef2f6));
        text.addView(label(getString(summaryResource), 12, 0xff8d98a5));
        card.addView(text, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        card.addView(label("›", 30, 0xff6fb4ff));
        card.setOnClickListener(view -> {
            Intent intent = new Intent(this, SettingsActivity.class);
            intent.putExtra(EXTRA_SCREEN, screen);
            if (!gameTitleId.isEmpty()) {
                intent.putExtra(GameProfileSettings.EXTRA_GAME_TITLE_ID, gameTitleId);
                intent.putExtra(GameProfileSettings.EXTRA_GAME_TITLE, gameTitle);
            }
            startActivity(intent);
        });
        card.setLayoutParams(cardMargins());
        card.setElevation(dp(2));
        return card;
    }

    private CheckBox preferenceCheck(int title, String key, boolean defaultValue) {
        CheckBox control = check(getString(title), prefs(this).getBoolean(key, defaultValue));
        control.setOnCheckedChangeListener((button, checked) ->
                prefs(this).edit().putBoolean(key, checked).apply());
        control.setLayoutParams(cardMargins());
        return control;
    }

    private int driverIndex(List<VulkanDriverManager.DriverEntry> drivers,
                            String selectionId) {
        for (int index = 0; index < drivers.size(); ++index) {
            if (drivers.get(index).selectionId.equals(selectionId)) {
                return index;
            }
        }
        return -1;
    }

    private String[] vulkanDriverLabels(
            List<VulkanDriverManager.DriverEntry> drivers,
            String selected, String recoveryNotice) {
        String[] labels = new String[drivers.size()];
        for (int index = 0; index < drivers.size(); ++index) {
            VulkanDriverManager.DriverEntry entry = drivers.get(index);
            if (entry.isSystem()) {
                labels[index] = getString(R.string.optimization_vulkan_driver_qualcomm);
            } else if (entry.isBundled()) {
                labels[index] = getString(
                        R.string.optimization_vulkan_driver_bundled_option,
                        entry.version);
            } else {
                String sha = entry.driverPackage.sha256.substring(
                        0, Math.min(8, entry.driverPackage.sha256.length()));
                labels[index] = getString(
                        R.string.optimization_vulkan_driver_imported_option,
                        entry.version, sha);
            }
        }
        int selectedIndex = driverIndex(drivers, selected);
        if (selectedIndex < 0) {
            return labels;
        }
        String base = labels[selectedIndex];
        VulkanDriverManager.DriverEntry selectedEntry = drivers.get(selectedIndex);
        if (!recoveryNotice.isEmpty() && selectedEntry.isSystem()) {
            labels[selectedIndex] = getString(
                    R.string.optimization_vulkan_driver_recovered, base);
            return labels;
        }
        String rawStatus = VulkanDriverManager.lastRuntimeStatus(this);
        if (rawStatus.isEmpty()) {
            if (!selectedEntry.isSystem()) {
                labels[selectedIndex] = getString(
                        R.string.optimization_vulkan_driver_restart_pending, base);
            }
            return labels;
        }
        try {
            JSONObject status = new JSONObject(rawStatus);
            String expectedRequest = selectedEntry.isSystem() ? "system" : "turnip";
            String expectedSource = selectedEntry.isSystem() ? "system"
                    : selectedEntry.isBundled() ? "bundled" : "imported";
            String expectedSha = selectedEntry.driverPackage == null ? ""
                    : selectedEntry.driverPackage.sha256;
            boolean requestMatches = status.optBoolean("selectionLocked", false)
                    && expectedRequest.equals(status.optString("requested", ""))
                    && expectedSource.equals(
                    status.optString("requestedPackageSource", ""))
                    && (selectedEntry.isSystem() || expectedSha.equalsIgnoreCase(
                    status.optString("requestedSha256", "")));
            if (!requestMatches) {
                labels[selectedIndex] = getString(
                        R.string.optimization_vulkan_driver_restart_pending, base);
            } else if (selectedEntry.isSystem()
                    && "system".equals(status.optString("effective", ""))) {
                labels[selectedIndex] = getString(
                        R.string.optimization_vulkan_driver_active_simple, base);
            } else if (!selectedEntry.isSystem()
                    && "turnip".equals(status.optString("effective", ""))
                    && expectedSource.equals(status.optString("packageSource", ""))
                    && expectedSha.equalsIgnoreCase(status.optString("sha256", ""))) {
                labels[selectedIndex] = getString(
                        R.string.optimization_vulkan_driver_active_simple, base);
            } else {
                String fallbackReason = status.optString("fallbackReason", "").trim();
                if (fallbackReason.isEmpty() || "none".equals(fallbackReason)) {
                    fallbackReason = "effective-driver-mismatch";
                }
                labels[selectedIndex] = getString(
                        R.string.optimization_vulkan_driver_fallback, base,
                        fallbackReason);
            }
        } catch (Exception ignored) {
            labels[selectedIndex] = getString(
                    R.string.optimization_vulkan_driver_restart_pending, base);
        }
        return labels;
    }

    private void selectVulkanDriver(String selectionId, Spinner spinner) {
        if (turnipSelectionInProgress || turnipImportInProgress) {
            return;
        }
        turnipSelectionInProgress = true;
        updateVulkanDriverControls();
        Context appContext = getApplicationContext();
        SharedPreferences selectionPreferences = editingPrefs();
        Thread selector = new Thread(() -> {
            try {
                VulkanDriverManager.selectDriver(
                        appContext, selectionPreferences, selectionId);
                postIfAlive(() -> {
                    turnipSelectionInProgress = false;
                    updateVulkanDriverControls();
                    Toast.makeText(this,
                            R.string.optimization_vulkan_driver_selected,
                            Toast.LENGTH_LONG).show();
                    recreate();
                });
            } catch (Exception error) {
                postIfAlive(() -> {
                    turnipSelectionInProgress = false;
                    updateVulkanDriverControls();
                    Toast.makeText(this,
                            getString(R.string.optimization_vulkan_driver_selection_failed,
                                    friendlyMessage(error)), Toast.LENGTH_LONG).show();
                    recreate();
                });
            }
        }, "lsx4-turnip-select");
        selector.setDaemon(true);
        selector.start();
    }

    private void pickTurnipDriverArchive() {
        if (turnipImportInProgress || turnipSelectionInProgress) {
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{
                "application/zip", "application/x-zip-compressed",
                "application/octet-stream"});
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(intent, REQUEST_IMPORT_TURNIP_DRIVER);
        } catch (ActivityNotFoundException error) {
            Toast.makeText(this, R.string.file_picker_unavailable, Toast.LENGTH_LONG).show();
        }
    }

    private void importTurnipDriver(Uri source) {
        if (turnipImportInProgress) {
            return;
        }
        turnipImportInProgress = true;
        updateVulkanDriverControls();
        Toast.makeText(this, R.string.optimization_vulkan_driver_importing,
                Toast.LENGTH_SHORT).show();
        String archiveName = selectedFileName(source);
        Context appContext = getApplicationContext();
        Thread importer = new Thread(() -> {
            try {
                VulkanDriverManager.ImportResult result =
                        VulkanDriverManager.importZip(appContext, source, archiveName);
                runOnUiThread(() -> {
                    if (!isSettingsUiAlive()) {
                        VulkanDriverManager.discardInactiveImport(appContext, result);
                        return;
                    }
                    Toast.makeText(this,
                            R.string.optimization_vulkan_driver_probing,
                            Toast.LENGTH_LONG).show();
                    VulkanDriverProbe.start(this, result.driverPackage,
                            probe -> finishTurnipDriverImport(result, probe));
                });
            } catch (Exception error) {
                postIfAlive(() -> {
                    turnipImportInProgress = false;
                    updateVulkanDriverControls();
                    Toast.makeText(this,
                            getString(R.string.optimization_vulkan_driver_import_failed,
                                    friendlyMessage(error)), Toast.LENGTH_LONG).show();
                });
            }
        }, "lsx4-turnip-import");
        importer.setDaemon(true);
        importer.start();
    }

    private void finishTurnipDriverImport(VulkanDriverManager.ImportResult staged,
                                          VulkanDriverProbe.ProbeResult probe) {
        Context appContext = getApplicationContext();
        Thread registrar = new Thread(() -> {
            boolean succeeded = false;
            String detail;
            try {
                if (!probe.passed()) {
                    throw new IOException(probe.reason);
                }
                VulkanDriverManager.DriverPackage registered =
                        VulkanDriverManager.registerImported(
                                appContext, staged.driverPackage, probe.runtimeStatus);
                detail = registered.summary();
                succeeded = true;
            } catch (Exception error) {
                VulkanDriverManager.discardInactiveImport(appContext, staged);
                detail = friendlyMessage(error);
            }
            boolean importSucceeded = succeeded;
            String completionDetail = detail;
            postIfAlive(() -> {
                turnipImportInProgress = false;
                updateVulkanDriverControls();
                int message = importSucceeded
                        ? R.string.optimization_vulkan_driver_import_success
                        : R.string.optimization_vulkan_driver_import_failed;
                Toast.makeText(this, getString(message, completionDetail),
                        Toast.LENGTH_LONG).show();
                recreate();
            });
        }, "lsx4-turnip-register");
        registrar.setDaemon(true);
        registrar.start();
    }

    private boolean isVulkanDriverBusy() {
        return turnipImportInProgress || turnipSelectionInProgress;
    }

    private void updateVulkanDriverControls() {
        boolean enabled = vulkanDriverAxisSupported && !isVulkanDriverBusy();
        if (vulkanDriverSpinnerControl != null) {
            vulkanDriverSpinnerControl.setEnabled(enabled);
            vulkanDriverSpinnerControl.setAlpha(enabled ? 1.0f : 0.52f);
        }
        if (vulkanDriverImportControl != null) {
            vulkanDriverImportControl.setEnabled(enabled);
            vulkanDriverImportControl.setAlpha(enabled ? 1.0f : 0.52f);
        }
    }

    private boolean isSettingsUiAlive() {
        return !isFinishing() && !isDestroyed();
    }

    private void postIfAlive(Runnable action) {
        runOnUiThread(() -> {
            if (isSettingsUiAlive()) {
                action.run();
            }
        });
    }

    private CheckBox check(String text, boolean checked) {
        CheckBox control = new CheckBox(this);
        control.setText(text);
        control.setTextSize(15);
        control.setTextColor(0xffe3e8ef);
        control.setButtonDrawable(new InsetDrawable(
                createCheckBoxButtonDrawable(this), dp(18), 0, dp(4), 0));
        control.setCompoundDrawablePadding(dp(12));
        control.setGravity(Gravity.CENTER_VERTICAL);
        control.setMinHeight(dp(64));
        control.setChecked(checked);
        control.setPadding(dp(12), dp(14), dp(22), dp(14));
        control.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        return control;
    }

    public static Drawable createCheckBoxButtonDrawable(Context context) {
        int size = Math.round(24 * context.getResources().getDisplayMetrics().density);
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_checked},
                new CheckMarkDrawable(size, true));
        states.addState(new int[0], new CheckMarkDrawable(size, false));
        return states;
    }

    private TextView title(String text) {
        return label(text, 24, 0xfff4f7fa);
    }

    private TextView subtitle(String text) {
        TextView view = label(text, 12, 0xff7f8a97);
        view.setPadding(0, dp(2), 0, dp(6));
        return view;
    }

    private TextView section(String text) {
        TextView view = label(text, 13, 0xff6fb4ff);
        view.setPadding(0, dp(20), 0, dp(6));
        return view;
    }

    private TextView compactSection(String text) {
        TextView view = label(text, 13, 0xff6fb4ff);
        view.setPadding(0, dp(8), 0, dp(4));
        return view;
    }

    private View settingRow(String title, View control) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(14), dp(8), dp(10), dp(8));
        row.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        row.addView(label(title, 14, 0xffd7dee7), new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        row.addView(control, new LinearLayout.LayoutParams(
                dp(174), ViewGroup.LayoutParams.WRAP_CONTENT));
        return row;
    }

    private View driverSettingRow(String title, View controls) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(14), dp(10), dp(10), dp(10));
        card.setBackground(cardBackground(0xff1b2026, 0xff2d3640, 14));
        card.addView(label(title, 14, 0xffd7dee7), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams controlParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        controlParams.setMargins(0, dp(4), 0, 0);
        card.addView(controls, controlParams);
        return card;
    }

    private TextView hint(String text) {
        TextView view = label(text, 12, 0xff7f8a97);
        view.setLineSpacing(0, 1.15f);
        return view;
    }

    private TextView label(String text, int sp, int color) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(sp);
        view.setTextColor(color);
        return view;
    }

    private Spinner spinner(String[] values) {
        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(
                this, android.R.layout.simple_spinner_item, values);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        return spinner;
    }

    private AdapterView.OnItemSelectedListener saveSpinner(Spinner spinner, PositionConsumer consumer) {
        SettingSelection selection = new SettingSelection(spinner.getSelectedItemPosition());
        return new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (selection.changed(position)) consumer.accept(position);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        };
    }

    private GradientDrawable cardBackground(int fill, int stroke, int radiusDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fill);
        drawable.setCornerRadius(dp(radiusDp));
        drawable.setStroke(dp(1), stroke);
        return drawable;
    }

    private LinearLayout.LayoutParams cardMargins() {
        return matchWrapWithMargins(0, dp(5), 0, dp(5));
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams matchWrapWithMargins(int left, int top, int right, int bottom) {
        LinearLayout.LayoutParams params = matchWrap();
        params.setMargins(left, top, right, bottom);
        return params;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private interface PositionConsumer {
        void accept(int position);
    }

    private static final class CheckMarkDrawable extends Drawable {
        private final int size;
        private final boolean checked;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        CheckMarkDrawable(int size, boolean checked) {
            this.size = size;
            this.checked = checked;
        }

        @Override
        public void draw(Canvas canvas) {
            Rect bounds = getBounds();
            float inset = Math.max(1f, size * 0.08f);
            float radius = size * 0.24f;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(checked ? 0xff4aa3ff : 0xff151a20);
            canvas.drawRoundRect(bounds.left + inset, bounds.top + inset,
                    bounds.right - inset, bounds.bottom - inset, radius, radius, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Math.max(2f, size * 0.09f));
            paint.setColor(checked ? 0xff91caff : 0xff66717e);
            canvas.drawRoundRect(bounds.left + inset, bounds.top + inset,
                    bounds.right - inset, bounds.bottom - inset, radius, radius, paint);
            if (checked) {
                paint.setStrokeWidth(Math.max(2.5f, size * 0.12f));
                paint.setStrokeCap(Paint.Cap.ROUND);
                paint.setStrokeJoin(Paint.Join.ROUND);
                paint.setColor(0xffffffff);
                float left = bounds.left;
                float top = bounds.top;
                canvas.drawLine(left + size * 0.27f, top + size * 0.52f,
                        left + size * 0.43f, top + size * 0.68f, paint);
                canvas.drawLine(left + size * 0.43f, top + size * 0.68f,
                        left + size * 0.75f, top + size * 0.34f, paint);
            }
        }

        @Override
        public void setAlpha(int alpha) {
            paint.setAlpha(alpha);
        }

        @Override
        public void setColorFilter(ColorFilter colorFilter) {
            paint.setColorFilter(colorFilter);
        }

        @Override
        public int getOpacity() {
            return PixelFormat.TRANSLUCENT;
        }

        @Override
        public int getIntrinsicWidth() {
            return size;
        }

        @Override
        public int getIntrinsicHeight() {
            return size;
        }
    }
}
