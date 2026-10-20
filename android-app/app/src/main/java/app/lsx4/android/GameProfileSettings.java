package app.lsx4.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;








final class GameProfileSettings implements SharedPreferences {
    static final String EXTRA_GAME_TITLE_ID = "settings_game_title_id";
    static final String EXTRA_GAME_TITLE = "settings_game_title";
    static final String PROFILE_PREFIX = "game.profile.";
    private static final String TITLE_PREFIX = "game.library.title.";
    private static final String TITLE_IDS_KEY = "game.library.title_ids";

    static final class GameTitle {
        final String titleId;
        final String title;

        GameTitle(String titleId, String title) {
            this.titleId = titleId;
            this.title = title;
        }
    }

    private final SharedPreferences base;
    private final String titleId;
    private final String prefix;
    private final Map<OnSharedPreferenceChangeListener, OnSharedPreferenceChangeListener>
            listeners = new HashMap<>();

    private GameProfileSettings(SharedPreferences base, String titleId) {
        this.base = base;
        this.titleId = CpuTuningSettings.normalizeTitleId(titleId);
        this.prefix = PROFILE_PREFIX + this.titleId + ".";
    }

    static SharedPreferences forGame(SharedPreferences base, String titleId) {
        String normalized = CpuTuningSettings.normalizeTitleId(titleId);
        return normalized.isEmpty() ? base : new GameProfileSettings(base, normalized);
    }

    static String scopedKey(String titleId, String key) {
        return PROFILE_PREFIX + CpuTuningSettings.normalizeTitleId(titleId) + "." + key;
    }

    static boolean hasOverrides(SharedPreferences preferences, String titleId) {
        String prefix = PROFILE_PREFIX + CpuTuningSettings.normalizeTitleId(titleId) + ".";
        for (String key : preferences.getAll().keySet()) {
            if (key.startsWith(prefix)) {
                return true;
            }
        }
        return false;
    }

    static void clearOverrides(SharedPreferences preferences, String titleId) {
        String prefix = PROFILE_PREFIX + CpuTuningSettings.normalizeTitleId(titleId) + ".";
        SharedPreferences.Editor editor = preferences.edit();
        ProcessSafePreferences.removePrefix(editor, preferences, prefix);
        editor.apply();
    }

    static void rememberTitle(SharedPreferences preferences, String titleId, String title) {
        String normalized = CpuTuningSettings.normalizeTitleId(titleId);
        String cleanTitle = title == null ? "" : title.trim();
        if (!normalized.isEmpty() && !cleanTitle.isEmpty() && !normalized.equals(cleanTitle)) {
            preferences.edit().putString(TITLE_PREFIX + normalized, cleanTitle).apply();
        }
    }

    static void replaceInstalledTitles(SharedPreferences preferences, List<GameTitle> games) {
        Set<String> active = new LinkedHashSet<>();
        SharedPreferences.Editor editor = preferences.edit();
        for (GameTitle game : games) {
            String titleId = CpuTuningSettings.normalizeTitleId(game.titleId);
            String title = game.title == null ? "" : game.title.trim();
            if (titleId.isEmpty()) {
                continue;
            }
            active.add(titleId);
            if (!title.isEmpty() && !title.equalsIgnoreCase(titleId)) {
                editor.putString(TITLE_PREFIX + titleId, title);
            }
        }
        editor.putStringSet(TITLE_IDS_KEY, active).apply();
    }

    static List<GameTitle> installedGames(Context context, SharedPreferences preferences) {
        List<String> titleIds;
        if (preferences.contains(TITLE_IDS_KEY)) {
            Set<String> registered = preferences.getStringSet(
                    TITLE_IDS_KEY, Collections.emptySet());
            titleIds = new ArrayList<>(registered == null
                    ? Collections.emptySet() : registered);
        } else {
            titleIds = CpuTuningSettings.installedTitleIds(context);
        }
        List<GameTitle> games = new ArrayList<>(titleIds.size());
        for (String titleId : titleIds) {
            String title = preferences.getString(TITLE_PREFIX + titleId, "").trim();
            if (title.isEmpty() || title.equalsIgnoreCase(titleId)) {


                title = context.getString(R.string.game_profile_unnamed_title);
            }
            games.add(new GameTitle(titleId, title));
        }
        games.sort((left, right) -> {
            int byTitle = left.title.compareToIgnoreCase(right.title);
            return byTitle != 0 ? byTitle : left.titleId.compareTo(right.titleId);
        });
        return games;
    }

    static boolean isIndividualKey(String key) {
        if (key == null || key.isEmpty()) {
            return false;
        }
        if (key.startsWith(OverlayLayoutStore.PREFIX)
                || key.startsWith(HardwareTriggerMapper.PREFIX)) {
            return true;
        }
        switch (key) {
            case SettingsActivity.K_RES_MODE:
            case SettingsActivity.K_UPSCALED_OUTPUT_RES_MODE:
            case SettingsActivity.K_DISPLAY_SCALING_MODE:
            case SettingsActivity.K_FSR_ENABLED:
            case SettingsActivity.K_UPSCALER_MODE:
            case SettingsActivity.K_RCAS_ENABLED:
            case SettingsActivity.K_SHARP_BILINEAR_STRENGTH:
            case SettingsActivity.K_BICUBIC_KERNEL:
            case SettingsActivity.K_FSR1_RCAS_SHARPNESS:
            case SettingsActivity.K_SGSR1_EDGE_SHARPNESS:
            case SettingsActivity.K_NIS_SHARPNESS:
            case SettingsActivity.K_SGSR2_TEMPORAL_STABILITY:
            case SettingsActivity.K_ARM_ASR_SHARPENING:
            case SettingsActivity.K_ARM_ASR_SHARPNESS:
            case SettingsActivity.K_GPU_BACKEND:
            case SettingsActivity.K_AUDIO:
            case SettingsActivity.K_OVERLAY:
            case SettingsActivity.K_OVERLAY_OPACITY:
            case SettingsActivity.K_PERF_HUD:
            case SettingsActivity.K_PERF_HUD_UPSCALER_INFO:
            case SettingsActivity.K_PERSISTENT_JIT_CACHE:
            case SettingsActivity.K_ARM_GPU_FAST_PATH:
            case SettingsActivity.K_COARSE_FRAGMENT_2X2:
            case SettingsActivity.K_DISABLE_VK_ROBUSTNESS:
            case SettingsActivity.K_TIERED_JIT:
            case SettingsActivity.K_JIT_TRACE_COMPILATION:
            case SettingsActivity.K_GUEST_CPU_BACKEND:
            case SettingsActivity.K_VULKAN_DRIVER:
            case SettingsActivity.K_VULKAN_DRIVER_SELECTION:
            case SettingsActivity.K_LIMIT_ANISOTROPY_2X:
            case SettingsActivity.K_ANISOTROPY_MODE:
            case SettingsActivity.K_READBACK_BATCHING:
            case SettingsActivity.K_VULKAN_DRIVER_CALLS:
            case SettingsActivity.K_ASYNC_PIPELINE:
            case SettingsActivity.K_ADAPTIVE_MOBILE_GPU:
            case SettingsActivity.K_AUDIO_SIMD:
            case SettingsActivity.K_FAST_GUEST_MEMORY:
            case SettingsActivity.K_RELAXED_FP_FUSION:
            case SettingsActivity.K_ANDROID_PERFORMANCE_HINT:
            case SettingsActivity.K_OEM_ROOT_PERFORMANCE:
            case SettingsActivity.K_HOST_FLAGM:
            case SettingsActivity.K_HOST_SVE2:
            case SettingsActivity.K_HOST_RCPC:
            case SettingsActivity.K_GUEST_SPIN_WAIT_EVENT:
                return true;
            default:
                return false;
        }
    }

    private String readKey(String key) {
        String scoped = prefix + key;
        return isIndividualKey(key) && base.contains(scoped) ? scoped : key;
    }

    private String writeKey(String key) {
        return isIndividualKey(key) ? prefix + key : key;
    }

    @Override
    public Map<String, ?> getAll() {
        Map<String, ?> source = base.getAll();
        Map<String, Object> merged = new LinkedHashMap<>();
        for (Map.Entry<String, ?> entry : source.entrySet()) {
            if (!entry.getKey().startsWith(PROFILE_PREFIX)) {
                merged.put(entry.getKey(), entry.getValue());
            }
        }
        for (Map.Entry<String, ?> entry : source.entrySet()) {
            if (entry.getKey().startsWith(prefix)) {
                String logical = entry.getKey().substring(prefix.length());
                if (isIndividualKey(logical)) {
                    merged.put(logical, entry.getValue());
                }
            }
        }
        return Collections.unmodifiableMap(merged);
    }

    @Override public String getString(String key, String defValue) {
        return base.getString(readKey(key), defValue);
    }

    @Override public Set<String> getStringSet(String key, Set<String> defValues) {
        Set<String> value = base.getStringSet(readKey(key), defValues);
        return value == null ? null : Collections.unmodifiableSet(new LinkedHashSet<>(value));
    }

    @Override public int getInt(String key, int defValue) {
        return base.getInt(readKey(key), defValue);
    }

    @Override public long getLong(String key, long defValue) {
        return base.getLong(readKey(key), defValue);
    }

    @Override public float getFloat(String key, float defValue) {
        return base.getFloat(readKey(key), defValue);
    }

    @Override public boolean getBoolean(String key, boolean defValue) {
        return base.getBoolean(readKey(key), defValue);
    }

    @Override public boolean contains(String key) {
        return base.contains(readKey(key));
    }

    @Override public Editor edit() {
        return new ScopedEditor(base.edit());
    }

    @Override
    public synchronized void registerOnSharedPreferenceChangeListener(
            OnSharedPreferenceChangeListener listener) {
        if (listener == null || listeners.containsKey(listener)) {
            return;
        }
        OnSharedPreferenceChangeListener bridge = (preferences, changedKey) -> {
            if (changedKey == null) {
                listener.onSharedPreferenceChanged(this, null);
            } else if (changedKey.startsWith(prefix)) {
                listener.onSharedPreferenceChanged(this,
                        changedKey.substring(prefix.length()));
            } else if (isIndividualKey(changedKey)
                    && !base.contains(prefix + changedKey)) {
                listener.onSharedPreferenceChanged(this, changedKey);
            } else if (!changedKey.startsWith(PROFILE_PREFIX) && !isIndividualKey(changedKey)) {
                listener.onSharedPreferenceChanged(this, changedKey);
            }
        };
        listeners.put(listener, bridge);
        base.registerOnSharedPreferenceChangeListener(bridge);
    }

    @Override
    public synchronized void unregisterOnSharedPreferenceChangeListener(
            OnSharedPreferenceChangeListener listener) {
        OnSharedPreferenceChangeListener bridge = listeners.remove(listener);
        if (bridge != null) {
            base.unregisterOnSharedPreferenceChangeListener(bridge);
        }
    }

    private final class ScopedEditor implements Editor {
        private final Editor editor;

        ScopedEditor(Editor editor) {
            this.editor = editor;
        }

        @Override public Editor putString(String key, String value) {
            editor.putString(writeKey(key), value);
            return this;
        }

        @Override public Editor putStringSet(String key, Set<String> values) {
            editor.putStringSet(writeKey(key), values == null
                    ? null : new LinkedHashSet<>(values));
            return this;
        }

        @Override public Editor putInt(String key, int value) {
            editor.putInt(writeKey(key), value);
            return this;
        }

        @Override public Editor putLong(String key, long value) {
            editor.putLong(writeKey(key), value);
            return this;
        }

        @Override public Editor putFloat(String key, float value) {
            editor.putFloat(writeKey(key), value);
            return this;
        }

        @Override public Editor putBoolean(String key, boolean value) {
            editor.putBoolean(writeKey(key), value);
            return this;
        }

        @Override public Editor remove(String key) {
            editor.remove(writeKey(key));
            return this;
        }

        @Override public Editor clear() {
            ProcessSafePreferences.removePrefix(editor, base, prefix);
            return this;
        }

        @Override public boolean commit() {
            return editor.commit();
        }

        @Override public void apply() {
            editor.apply();
        }
    }
}
