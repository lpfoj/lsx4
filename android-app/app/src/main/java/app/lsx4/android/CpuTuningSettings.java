package app.lsx4.android;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class CpuTuningSettings {
    static final String GLOBAL_PREFIX = "cpu.global.";
    static final String GAME_PREFIX = "cpu.game.";
    static final String ALL_GAMES_PREFIX = "cpu.all_games.";
    static final String K_EDITOR_SCOPE = "cpu.editor.scope";
    static final String K_GAME_ENABLED = "enabled";
    static final String K_ALL_GAMES_ENABLED = "cpu.all_games.enabled";
    static final String K_ALL_GAMES_EXCEPTIONS = "cpu.all_games.exceptions";
    static final int PRESET_COMPATIBILITY = 0;
    static final int PRESET_BALANCED = 1;
    static final int PRESET_PERFORMANCE = 2;
    static final int PRESET_EXTREME = 3;
    static final int PRESET_CUSTOM = 4;

    static final String[] JIT_FEATURE_NAMES = {
            "LSE atomics", "FlagM", "FlagM2", "LRCPC", "ILRCPC", "LRCPC3",
            "SVE", "SVE2", "AES", "PMULL", "SHA1", "SHA2", "CRC32", "AFP",
            "FCMA", "FRINTTS", "ECV", "RPRES"
    };

    static final int BASIC = 0;
    static final int ADVANCED = 1;
    static final int DANGEROUS = 2;
    static final int TYPE_CHOICE = 0;
    static final int TYPE_BOOLEAN = 1;
    static final int TYPE_NUMBER = 2;

    static final class Field {
        final String key;
        final String title;
        final String description;
        final int section;
        final int type;
        final String[] choices;
        final int minimum;
        final int maximum;

        Field(String key, String title, int section, String[] choices) {
            this.key = key;
            this.title = title;
            this.description = describe(key);
            this.section = section;
            this.type = TYPE_CHOICE;
            this.choices = choices;
            this.minimum = 0;
            this.maximum = choices.length - 1;
        }

        Field(String key, String title, int section) {
            this.key = key;
            this.title = title;
            this.description = describe(key);
            this.section = section;
            this.type = TYPE_BOOLEAN;
            this.choices = null;
            this.minimum = 0;
            this.maximum = 1;
        }

        Field(String key, String title, int section, int minimum, int maximum) {
            this.key = key;
            this.title = title;
            this.description = describe(key);
            this.section = section;
            this.type = TYPE_NUMBER;
            this.choices = null;
            this.minimum = minimum;
            this.maximum = maximum;
        }

        private static String describe(String key) {
            switch (key) {
                case "execution_mode": return "Selects FEX JIT, LSX4 IR JIT, a persistent decoded-block interpreter, or a fresh-decode non-persistent interpreter. Checked/software memory contracts route to IR JIT.";
                case "fast_memory": return "Safe uses LSX4 checked guest-memory accesses; Fast permits direct host mapping when atomics and cache-store accuracy allow it; Unsafe additionally permits guarded bad-access replay only with the unsafe host-mapped manager.";
                case "memory_manager": return "Software and Host mapped safe use the checked LSX4 IR core; Host mapped and Host mapped unsafe can use FEX.";
                case "block_linking": return "Links translated branch targets directly and avoids returning to the dispatcher between hot blocks.";
                case "persistent_cache": return "Persists translated artifacts for LSX4 IR JIT and validated hot-block seeds for FEX. Rebuild deletes only this title/profile cache and resets to On.";
                case "function_replacements": return "Safe enables low-overhead boundaries for proven HLE replacements; Aggressive additionally enables guarded fast libc and clean-buffer paths; Off retains only required system ABI HLE.";
                case "guest_clock_percent": return "Scales guest system counters and RDTSC/RDTSCP without changing the Android hardware clock.";
                case "cycle_skip": return "Advances the guest-visible scheduler clock by a ratio; it does not randomly discard guest instructions.";
                case "idle_loop_skip": return "Parks recognized read-only poll loops with bounded adaptive waits instead of continuously consuming a CPU core.";
                case "wait_loop_detection": return "Tracks the polled guest page and waits on a write event; Safe uses shorter timeouts than Aggressive.";
                case "spin_loop_detection": return "Defines x86 PAUSE handling: no-op, an ARM yield hint, or stronger backoff (scheduler yield in LSX4 and two architectural yields in FEX).";
                case "max_guest_workers": return "Limits concurrently running named guest worker/job threads with a waiting admission gate; guest main, render, audio, and system threads are untouched. 0 means hardware-aware Auto.";
                case "fast_event_timing": return "Accurate keeps normal deadlines, Fast uses the short-timer queue, and Instant expires only sub-threshold service timers immediately.";
                case "jit_block_size": return "Changes actual decode/trace limits. Larger regions reduce dispatch overhead but increase compile cost and invalidation span.";
                case "fp_accuracy": return "Accurate keeps strict LSX4 operations and 80-bit FEX x87; Approximate enables supported estimates and 53-bit x87; Relaxed permits LSX4 fusion and quantizes FEX x87 writes to 24-bit precision.";
                case "atomics": return "Chooses strict, acquire/release-compatible, or fast ordering in both LSX4 and FEX while retaining atomic read-modify-write operations.";
                case "accurate_cache_line_stores": return "Adds strong store barriers and disables relaxed direct-memory sequences for cache-line visibility.";
                case "accurate_dma_timing": return "Flushes deferred GPU transfers at DMA boundaries instead of batching them past the guest-visible command.";
                case "memory_exceptions": return "Delivers exact mapped-memory faults to the guest. If disabled, an unexpected fault is fatal instead of being fabricated or hidden.";
                case "icache_accuracy": return "Audits live instruction bytes for self-modifying code; Off keeps page invalidation but removes sparse byte comparisons.";
                case "ignore_bad_accesses": return "Unsafe only: replays the exact decoded instruction with invalid reads as zero and invalid writes discarded; forces LSX4 IR JIT.";
                case "jit_feature_mask": return "Default uses detected host ISA, Compatibility disables risky vector extensions, and Custom applies the bit mask below.";
                case "jit_disable_mask": return "Custom disable mask for detected ARM64 ISA bits. It can only remove features; it can never advertise unsupported hardware.";
                case "cpu_gpu_dual_thread": return "Dual runs CPU command production and GPU consumption concurrently; Single serializes submission; Auto resolves once from host topology.";
                case "scheduler": return "OS leaves the allowed CPU set intact; Emulator uses role-based affinity; Performance pins heavy roles to the preferred fastest cores; Auto selects role affinity only for a usable heterogeneous topology.";
                case "preferred_heavy_threads": return "Number of highest-scored eligible cores offered to primary guest and compute/JIT roles in Emulator, Performance, and topology-selected Auto scheduling. 0 means hardware-aware Auto.";
                case "subsystem_multithreading": return "Controls optional HLE subsystem workers such as AIO, audio, and AJM; mandatory service threads remain intact.";
                case "separate_io_thread": return "Runs guest asynchronous file I/O on its dedicated worker instead of completing it on the caller.";
                case "separate_audio_hle_thread": return "Runs audio output and AJM codec jobs on dedicated HLE workers instead of inline.";
                case "guest_thread_delay_mode": return "Auto derives a small worker penalty from oversubscription; Custom uses the exact microsecond value below.";
                case "guest_thread_delay_us": return "Custom bounded startup delay applied only to recognized guest worker/job threads.";
                case "sync_on_idle_skip": return "Issues a full memory fence after an idle-loop park before guest execution resumes.";
                case "cpu_gpu_sync": return "Accurate waits after every submit. Adaptive uses a bounded 64/32 high/low watermark to preserve overlap without runaway. Relaxed adds no CPU-side backpressure.";
                case "force_real_clock_sync": return "Forces unscaled monotonic time and disables guest clock scaling and cycle skip.";
                case "hle_mode": return "Prefer HLE resolves implemented host exports first; Auto uses guest exports then HLE; Prefer LLE refuses HLE replacement when that guest module is loaded.";
                case "hle_fast_timers": return "Accurate sleeps to deadline, Fast yield-waits for short sleeps, and Aggressive collapses only bounded short HLE waits to one yield.";
                case "per_game_profile": return "Internal compatibility route. Opening a game's settings and changing a value creates its sparse per-game override automatically.";
                case "auto_game_db": return "Applies conservative title-specific defaults before explicit values from the selected scope; user overrides always win, then safety contracts are enforced.";
                default: return "Applied before guest execution starts.";
            }
        }
    }

    static final List<Field> FIELDS = Collections.unmodifiableList(Arrays.asList(
            new Field("execution_mode", "CPU execution mode", BASIC,
                    new String[]{"JIT (FEX)", "IR JIT (LSX4)",
                            "Cached interpreter", "Interpreter"}),
            new Field("fast_memory", "Fast memory", BASIC,
                    new String[]{"Safe", "Fast", "Unsafe"}),
            new Field("memory_manager", "Memory manager", BASIC,
                    new String[]{"Software", "Host mapped", "Host mapped safe",
                            "Host mapped unsafe"}),
            new Field("block_linking", "JIT block linking", BASIC),
            new Field("persistent_cache", "Persistent translation cache", BASIC,
                    new String[]{"Off", "On", "Rebuild on next launch"}),
            new Field("function_replacements", "Function replacements / HLE", BASIC,
                    new String[]{"Off", "Safe", "Aggressive"}),
            new Field("guest_clock_percent", "Guest CPU clock (%)", BASIC, 25, 400),
            new Field("cycle_skip", "Cycle skip", DANGEROUS,
                    new String[]{"Off", "Mild", "Moderate", "Aggressive"}),
            new Field("idle_loop_skip", "Idle-loop skipping", ADVANCED,
                    new String[]{"Off", "Safe", "Aggressive"}),
            new Field("wait_loop_detection", "Wait-loop detection", ADVANCED,
                    new String[]{"Off", "Safe", "Aggressive"}),
            new Field("spin_loop_detection", "Spin-loop detection", ADVANCED,
                    new String[]{"Off", "Safe", "Aggressive"}),
            new Field("max_guest_workers", "Maximum guest worker threads (0 = Auto)",
                    BASIC, 0, 32),
            new Field("fast_event_timing", "Event timing", ADVANCED,
                    new String[]{"Accurate", "Fast", "Instant where safe"}),
            new Field("jit_block_size", "JIT block size", ADVANCED,
                    new String[]{"Safe", "Mega", "Giga"}),
            new Field("fp_accuracy", "Floating-point accuracy", ADVANCED,
                    new String[]{"Accurate", "Approximate", "Relaxed"}),
            new Field("atomics", "Atomics / reservations", ADVANCED,
                    new String[]{"Strict", "Compatible", "Fast"}),
            new Field("accurate_cache_line_stores", "Accurate cache-line stores", ADVANCED),
            new Field("accurate_dma_timing", "Accurate DMA / transfer timing", ADVANCED),
            new Field("memory_exceptions", "Memory exceptions", ADVANCED),
            new Field("icache_accuracy", "Instruction-cache accuracy", ADVANCED),
            new Field("ignore_bad_accesses", "Ignore invalid memory accesses", DANGEROUS),
            new Field("jit_feature_mask", "JIT host feature mask", ADVANCED,
                    new String[]{"Default", "Compatibility", "Custom disable mask"}),
            new Field("jit_disable_mask", "Custom JIT disable mask", ADVANCED,
                    0, 262143),
            new Field("cpu_gpu_dual_thread", "CPU/GPU threading", BASIC,
                    new String[]{"Single / accurate", "Dual / fast", "Auto"}),
            new Field("scheduler", "Thread scheduler", BASIC,
                    new String[]{"OS", "Emulator", "Performance", "Auto"}),
            new Field("preferred_heavy_threads", "Preferred heavy threads (0 = Auto)",
                    ADVANCED, 0, 8),
            new Field("subsystem_multithreading", "Subsystem multithreading", BASIC,
                    new String[]{"Off", "On", "Auto"}),
            new Field("separate_io_thread", "Separate I/O thread", BASIC,
                    new String[]{"Off", "On", "Auto"}),
            new Field("separate_audio_hle_thread", "Separate audio/HLE thread", BASIC,
                    new String[]{"Off", "On", "Auto"}),
            new Field("guest_thread_delay_mode", "Guest thread delay", DANGEROUS,
                    new String[]{"Auto", "Custom"}),
            new Field("guest_thread_delay_us", "Guest thread start delay (µs)", DANGEROUS,
                    0, 10000),
            new Field("sync_on_idle_skip", "Synchronize after idle skip", ADVANCED),
            new Field("cpu_gpu_sync", "CPU/GPU synchronization", ADVANCED,
                    new String[]{"Accurate", "Adaptive", "Relaxed"}),
            new Field("force_real_clock_sync", "Force real-clock synchronization", ADVANCED),
            new Field("hle_mode", "System module mode", ADVANCED,
                    new String[]{"Auto", "Prefer HLE", "Prefer LLE"}),
            new Field("hle_fast_timers", "HLE timer mode", ADVANCED,
                    new String[]{"Accurate", "Fast", "Aggressive"}),
            new Field("per_game_profile", "Per-game CPU profile", BASIC,
                    new String[]{"Global", "Game profile", "Auto profile"}),
            new Field("auto_game_db", "GameDB policy", BASIC,
                    new String[]{"User overrides only", "Automatic"})
    ));

    private static final Pattern TITLE_ID = Pattern.compile("(?i)(CUSA|PPSA)[0-9]{5}");

    private CpuTuningSettings() {
    }

    static String fieldKey(String scope, String field) {
        return scope + field;
    }

    static String gameScope(String titleId) {
        return GAME_PREFIX + normalizeTitleId(titleId) + ".";
    }

    static String normalizeTitleId(String value) {
        return value == null ? "" : value.trim().toUpperCase(Locale.ROOT);
    }

    static String titleIdFromPath(String path) {
        if (path == null) {
            return "";
        }
        Matcher matcher = TITLE_ID.matcher(path);
        return matcher.find() ? matcher.group().toUpperCase(Locale.ROOT) : "";
    }

    static List<String> installedTitleIds(Context context) {
        List<String> result = new ArrayList<>();
        collectInstalledTitleIds(new File(
                context.getFilesDir(), "lsx4-home/runtime-fs/user/app"), result);
        collectInstalledTitleIds(new File(
                context.getFilesDir(), "lsx4-home/games"), result);
        if (SharedGameStorage.hasAccess()) {
            try {
                collectInstalledTitleIds(SharedGameStorage.gamesDirectory(), result);
            } catch (IOException ignored) {
            }
        }
        result.sort(Comparator.naturalOrder());
        return result;
    }

    private static void collectInstalledTitleIds(File root, List<String> result) {
        File[] directories = root.listFiles(File::isDirectory);
        if (directories != null) {
            for (File directory : directories) {
                String id = normalizeTitleId(directory.getName());
                if (TITLE_ID.matcher(id).matches() && !result.contains(id)) {
                    result.add(id);
                }
            }
        }
    }

    static int preset(SharedPreferences preferences, String scope) {
        return clamp(preferences.getInt(fieldKey(scope, "preset"), PRESET_BALANCED),
                PRESET_COMPATIBILITY, PRESET_CUSTOM);
    }

    static void applyPreset(SharedPreferences preferences, String scope, int requestedPreset) {
        int preset = clamp(requestedPreset, PRESET_COMPATIBILITY, PRESET_EXTREME);
        Map<String, Integer> values = presetValues(preset);
        SharedPreferences.Editor editor = preferences.edit()
                .putInt(fieldKey(scope, "preset"), preset);
        for (Map.Entry<String, Integer> entry : values.entrySet()) {
            editor.putInt(fieldKey(scope, entry.getKey()), entry.getValue());
        }
        editor.commit();
    }

    static int read(SharedPreferences preferences, String scope, Field field) {
        int fallback = presetValues(PRESET_BALANCED).get(field.key);
        return clamp(preferences.getInt(fieldKey(scope, field.key), fallback),
                field.minimum, field.maximum);
    }

    static void write(SharedPreferences preferences, String scope, Field field, int value) {
        preferences.edit()
                .putInt(fieldKey(scope, field.key), clamp(value, field.minimum, field.maximum))
                .putInt(fieldKey(scope, "preset"), PRESET_CUSTOM)
                .commit();
    }

    static boolean gameProfileEnabled(SharedPreferences preferences, String titleId) {
        String normalized = normalizeTitleId(titleId);
        return !normalized.isEmpty() && preferences.getBoolean(
                fieldKey(gameScope(normalized), K_GAME_ENABLED), false);
    }

    static void setGameProfileEnabled(SharedPreferences preferences, String titleId,
                                      boolean enabled) {
        String normalized = normalizeTitleId(titleId);
        if (!normalized.isEmpty()) {
            preferences.edit().putBoolean(
                    fieldKey(gameScope(normalized), K_GAME_ENABLED), enabled).commit();
        }
    }

    static void clearGameProfile(SharedPreferences preferences, String titleId) {
        String scope = gameScope(titleId);
        SharedPreferences.Editor editor = preferences.edit();
        ProcessSafePreferences.removePrefix(editor, preferences, scope);
        editor.commit();
    }

    static boolean allGamesProfileEnabled(SharedPreferences preferences) {
        return preferences.getBoolean(K_ALL_GAMES_ENABLED, false);
    }

    static java.util.Set<String> allGamesExceptions(SharedPreferences preferences) {
        java.util.Set<String> stored = preferences.getStringSet(
                K_ALL_GAMES_EXCEPTIONS, java.util.Collections.emptySet());
        java.util.Set<String> normalized = new java.util.LinkedHashSet<>();
        if (stored != null) {
            for (String titleId : stored) {
                String value = normalizeTitleId(titleId);
                if (TITLE_ID.matcher(value).matches()) {
                    normalized.add(value);
                }
            }
        }
        return normalized;
    }

    static void setAllGamesExceptions(SharedPreferences preferences,
                                      java.util.Set<String> titleIds) {
        java.util.Set<String> normalized = new java.util.LinkedHashSet<>();
        if (titleIds != null) {
            for (String titleId : titleIds) {
                String value = normalizeTitleId(titleId);
                if (TITLE_ID.matcher(value).matches()) {
                    normalized.add(value);
                }
            }
        }
        preferences.edit().putStringSet(K_ALL_GAMES_EXCEPTIONS, normalized).commit();
    }

    static void setAllGamesException(SharedPreferences preferences, String titleId,
                                     boolean excluded) {
        java.util.Set<String> values = allGamesExceptions(preferences);
        String normalized = normalizeTitleId(titleId);
        if (excluded) {
            values.add(normalized);
        } else {
            values.remove(normalized);
        }
        setAllGamesExceptions(preferences, values);
    }

    static boolean allGamesProfileApplies(SharedPreferences preferences, String titleId) {
        String normalized = normalizeTitleId(titleId);
        return allGamesProfileEnabled(preferences)
                && (normalized.isEmpty() || !allGamesExceptions(preferences).contains(normalized));
    }

    static void activateCurrentProfileForAllGames(
            SharedPreferences preferences, String titleId) {




        Map<String, Integer> values = selectedValues(preferences, titleId);
        SharedPreferences.Editor editor = preferences.edit()
                .putBoolean(K_ALL_GAMES_ENABLED, true)
                .putInt(fieldKey(ALL_GAMES_PREFIX, "preset"),
                        values.get("preset"));
        for (Field field : FIELDS) {
            editor.putInt(fieldKey(ALL_GAMES_PREFIX, field.key),
                    values.get(field.key));
        }
        editor.commit();
    }

    static void setAllGamesProfileEnabled(SharedPreferences preferences,
                                          boolean enabled) {
        preferences.edit().putBoolean(K_ALL_GAMES_ENABLED, enabled).commit();
    }

    static File materialize(Context context, String launchPath) throws IOException {
        SharedPreferences preferences = SettingsActivity.prefs(context);
        String titleId = titleIdFromPath(launchPath);
        Map<String, Integer> values = resolvedValues(preferences, titleId);
        File directory = new File(context.getFilesDir(), "lsx4-home/runtime-config");
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("Cannot create " + directory);
        }
        StringBuilder output = new StringBuilder(1024);
        output.append("# LSX4 Android CPU tuning v1\n")
                .append("# title_id=").append(titleId.isEmpty() ? "global" : titleId).append('\n')
                .append("preset=").append(values.get("preset")).append('\n');
        for (Field field : FIELDS) {
            output.append(field.key).append('=').append(values.get(field.key)).append('\n');
        }
        File target = new File(directory, "android-cpu-tuning-v1.ini");
        File temporary = new File(directory, target.getName() + ".tmp");
        try (FileOutputStream stream = new FileOutputStream(temporary, false)) {
            stream.write(output.toString().getBytes(StandardCharsets.UTF_8));
            stream.getFD().sync();
        }
        if (target.exists() && !target.delete()) {
            throw new IOException("Cannot replace " + target);
        }
        if (!temporary.renameTo(target)) {
            throw new IOException("Cannot publish " + target);
        }
        if (values.get("persistent_cache") == 2) {
            String selectedScope = GLOBAL_PREFIX;
            if (allGamesProfileApplies(preferences, titleId)) {
                selectedScope = ALL_GAMES_PREFIX;
            } else if (shouldUseGameScope(preferences, titleId, preferences.getInt(
                    fieldKey(GLOBAL_PREFIX, "per_game_profile"), 2))) {
                selectedScope = gameScope(titleId);
            }
            preferences.edit().putInt(
                    fieldKey(selectedScope, "persistent_cache"), 1).commit();
        }
        return target;
    }

    static Map<String, Integer> resolvedValues(SharedPreferences preferences, String titleId) {
        return resolveValues(preferences, titleId, true);
    }

    static Map<String, Integer> selectedValues(SharedPreferences preferences, String titleId) {
        return resolveValues(preferences, titleId, false);
    }

    private static Map<String, Integer> resolveValues(
            SharedPreferences preferences, String titleId,
            boolean applySafetyContracts) {

        preferences = PreferenceSnapshot.of(preferences);
        int profileMode = clamp(preferences.getInt(
                fieldKey(GLOBAL_PREFIX, "per_game_profile"), 2), 0, 2);
        boolean useAllGamesScope = allGamesProfileApplies(preferences, titleId);
        boolean useGameScope = !useAllGamesScope
                && shouldUseGameScope(preferences, titleId, profileMode);
        String scope = useAllGamesScope ? ALL_GAMES_PREFIX
                : (useGameScope ? gameScope(titleId) : GLOBAL_PREFIX);
        int globalPreset = preset(preferences, GLOBAL_PREFIX);
        int selectedPreset = (useAllGamesScope || useGameScope) && preferences.contains(
                fieldKey(scope, "preset")) ? preset(preferences, scope) : globalPreset;
        Map<String, Integer> values = presetValues(
                globalPreset == PRESET_CUSTOM ? PRESET_BALANCED : globalPreset);


        if ((useAllGamesScope || useGameScope) && selectedPreset != PRESET_CUSTOM
                && preferences.contains(fieldKey(scope, "preset"))) {
            values = presetValues(selectedPreset);
        }
        values.put("preset", selectedPreset);
        String globalAutoGameDbKey = fieldKey(GLOBAL_PREFIX, "auto_game_db");
        int autoGameDb = preferences.contains(globalAutoGameDbKey)
                ? clamp(preferences.getInt(globalAutoGameDbKey,
                        values.get("auto_game_db")), 0, 1)
                : values.get("auto_game_db");
        String scopedAutoGameDbKey = fieldKey(scope, "auto_game_db");
        if ((useAllGamesScope || useGameScope)
                && preferences.contains(scopedAutoGameDbKey)) {
            autoGameDb = clamp(preferences.getInt(scopedAutoGameDbKey, autoGameDb), 0, 1);
        }







        boolean customProfile = selectedPreset == PRESET_CUSTOM;
        if (autoGameDb != 0) {
            if (customProfile) {
                applyGameDb(values, normalizeTitleId(titleId));
            }
        }
        overlayExplicitValues(preferences, GLOBAL_PREFIX, values);
        if (useAllGamesScope || useGameScope) {
            overlayExplicitValues(preferences, scope, values);
        }
        if (autoGameDb != 0) {
            if (!customProfile) {
                applyGameDb(values, normalizeTitleId(titleId));
            }
        }
        values.put("preset", selectedPreset);
        if (applySafetyContracts) {
            enforceContracts(values);
        }
        return values;
    }

    static Map<String, Integer> effectiveRuntimeValues(
            SharedPreferences preferences, String titleId) {
        Map<String, Integer> values = new LinkedHashMap<>(
                resolvedValues(preferences, titleId));
        resolveAutomaticRuntimePolicy(values,
                Math.max(1, Runtime.getRuntime().availableProcessors()));
        return values;
    }

    static void resolveAutomaticRuntimePolicy(Map<String, Integer> values,
                                              int logicalProcessors) {
        int logical = Math.max(1, logicalProcessors);
        if (values.get("max_guest_workers") == 0) {
            values.put("max_guest_workers", clamp(
                    logical > 4 ? logical - 2 : logical, 2, 6));
        }
        if (values.get("preferred_heavy_threads") == 0) {
            values.put("preferred_heavy_threads", clamp(
                    (logical + 1) / 2, 1, 4));
        }
        resolveAutoThreadToggle(values, "cpu_gpu_dual_thread", logical, 6);
        resolveAutoThreadToggle(values, "subsystem_multithreading", logical, 6);
        resolveAutoThreadToggle(values, "separate_io_thread", logical, 6);
        resolveAutoThreadToggle(values, "separate_audio_hle_thread", logical, 8);
        if (values.get("guest_thread_delay_mode") == 0) {
            int oversubscription = Math.max(0,
                    values.get("max_guest_workers")
                            - values.get("preferred_heavy_threads"));
            values.put("guest_thread_delay_us",
                    Math.min(oversubscription * 25, 500));
        }
        enforceContracts(values);
    }

    private static void resolveAutoThreadToggle(Map<String, Integer> values,
                                                String key, int logical,
                                                int threshold) {
        if (values.get(key) == 2) {
            values.put(key, logical >= threshold ? 1 : 0);
        }
    }

    private static void overlayExplicitValues(SharedPreferences preferences, String scope,
                                              Map<String, Integer> values) {
        for (Field field : FIELDS) {
            String key = fieldKey(scope, field.key);
            if (preferences.contains(key)) {
                values.put(field.key, clamp(preferences.getInt(key, values.get(field.key)),
                        field.minimum, field.maximum));
            }
        }
    }

    private static boolean shouldUseGameScope(SharedPreferences preferences,
                                              String titleId, int profileMode) {
        String normalized = normalizeTitleId(titleId);
        if (normalized.isEmpty() || profileMode == 0) {
            return false;
        }
        String enabledKey = fieldKey(gameScope(normalized), K_GAME_ENABLED);
        if (preferences.contains(enabledKey)) {
            return preferences.getBoolean(enabledKey, false);
        }
        if (profileMode != 2) {
            return false;
        }
        String scope = gameScope(normalized);
        for (Field field : FIELDS) {
            if (preferences.contains(fieldKey(scope, field.key))) {
                return true;
            }
        }
        return preferences.contains(fieldKey(scope, "preset"));
    }

    private static void applyGameDb(Map<String, Integer> values, String titleId) {
        final boolean bloodborne = "CUSA03173".equals(titleId);





        if (bloodborne || "CUSA00900".equals(titleId)
                || "CUSA00207".equals(titleId) || "CUSA01363".equals(titleId)) {
            values.put("max_guest_workers", bloodborne ? 6 : 4);





            values.put("scheduler", bloodborne ? 0 : 2);
            values.put("preferred_heavy_threads", 4);
            values.put("fast_event_timing", 1);







            if (bloodborne) {
                values.put("block_linking", 1);
            }



            values.put("jit_block_size", bloodborne ? 0 : 1);




            values.put("persistent_cache", bloodborne ? 0 : 1);





            values.put("icache_accuracy", bloodborne ? 0 : 1);


            values.put("atomics", 1);





            values.put("cpu_gpu_sync", 1);
            values.put("accurate_cache_line_stores", 0);
        }








        if ("CUSA10349".equals(titleId) || "CUSA05667".equals(titleId)
                || "CUSA30277".equals(titleId) || "CUSA10606".equals(titleId)
                || "CUSA11628".equals(titleId) || "CUSA53312".equals(titleId)) {
            values.put("max_guest_workers", 32);
        }







        if ("CUSA30277".equals(titleId)) {
            values.put("jit_block_size", 0);
            values.put("block_linking", 1);
        }




        if ("CUSA53312".equals(titleId)) {
            values.put("jit_block_size", 0);
            values.put("block_linking", 1);
        }




        if ("CUSA11628".equals(titleId)) {
            values.putAll(presetValues(PRESET_BALANCED));
            values.put("jit_block_size", 2);
            values.put("persistent_cache", 0);
            values.put("icache_accuracy", 0);
            values.put("hle_mode", 0);
            values.put("max_guest_workers", 32);
        }





        if ("CUSA18112".equals(titleId)) {
            values.putAll(presetValues(PRESET_BALANCED));
            values.put("jit_block_size", 0);
            values.put("persistent_cache", 0);
            values.put("icache_accuracy", 0);
            values.put("max_guest_workers", 32);
        }




        if ("CUSA35013".equals(titleId)) {
            values.putAll(presetValues(PRESET_COMPATIBILITY));
            values.put("max_guest_workers", 32);
        }




        if ("CUSA00909".equals(titleId)) {
            values.putAll(presetValues(PRESET_COMPATIBILITY));
            values.put("fast_memory", 2);
            values.put("memory_manager", 3);
            values.put("ignore_bad_accesses", 1);
            values.put("max_guest_workers", 32);
        }





        if ("CUSA52139".equals(titleId)) {
            values.putAll(presetValues(PRESET_BALANCED));
            values.put("execution_mode", 1);
        }


        if ("CUSA00486".equals(titleId)) {
            values.putAll(presetValues(PRESET_COMPATIBILITY));
            values.put("fast_memory", 2);
            values.put("memory_manager", 3);
            values.put("ignore_bad_accesses", 1);
        }






        if ("CUSA10606".equals(titleId)) {
            values.putAll(presetValues(PRESET_COMPATIBILITY));
            values.put("max_guest_workers", 32);
        }
    }

    private static void enforceContracts(Map<String, Integer> values) {
        boolean unsafe = values.get("fast_memory") == 2
                && values.get("memory_manager") == 3;
        if (!unsafe) {
            values.put("ignore_bad_accesses", 0);
        } else if (values.get("ignore_bad_accesses") != 0) {


            values.put("execution_mode", 1);
        }
        if (values.get("execution_mode") == 0
                && (values.get("fast_memory") == 0
                || values.get("memory_manager") == 0
                || values.get("memory_manager") == 2)) {
            values.put("execution_mode", 1);
        }
        if (values.get("execution_mode") >= 2) {
            values.put("block_linking", 0);
            values.put("jit_block_size", 0);
        }
        if (values.get("execution_mode") == 3) {
            values.put("persistent_cache", 0);
        }
        if (values.get("jit_feature_mask") != 2) {
            values.put("jit_disable_mask", 0);
        }
        if (values.get("force_real_clock_sync") != 0) {
            values.put("guest_clock_percent", 100);
            values.put("cycle_skip", 0);
        }
    }

    private static Map<String, Integer> presetValues(int preset) {
        Map<String, Integer> values = new LinkedHashMap<>();
        values.put("preset", preset);
        values.put("execution_mode", 0);
        values.put("fast_memory", 1);
        values.put("memory_manager", 1);
        values.put("block_linking", 1);
        values.put("persistent_cache", 1);
        values.put("function_replacements", 1);
        values.put("guest_clock_percent", 100);
        values.put("cycle_skip", 0);
        values.put("idle_loop_skip", 1);
        values.put("wait_loop_detection", 1);
        values.put("spin_loop_detection", 1);
        values.put("max_guest_workers", 0);
        values.put("fast_event_timing", 1);
        values.put("jit_block_size", 1);
        values.put("fp_accuracy", 0);
        values.put("atomics", 1);
        values.put("accurate_cache_line_stores", 0);
        values.put("accurate_dma_timing", 0);
        values.put("memory_exceptions", 1);
        values.put("icache_accuracy", 1);
        values.put("ignore_bad_accesses", 0);
        values.put("jit_feature_mask", 0);
        values.put("jit_disable_mask", 0);
        values.put("cpu_gpu_dual_thread", 2);
        values.put("scheduler", 3);
        values.put("preferred_heavy_threads", 0);
        values.put("subsystem_multithreading", 2);
        values.put("separate_io_thread", 2);
        values.put("separate_audio_hle_thread", 2);
        values.put("guest_thread_delay_mode", 0);
        values.put("guest_thread_delay_us", 0);
        values.put("sync_on_idle_skip", 1);
        values.put("cpu_gpu_sync", 1);
        values.put("force_real_clock_sync", 0);
        values.put("hle_mode", 1);
        values.put("hle_fast_timers", 1);
        values.put("per_game_profile", 2);
        values.put("auto_game_db", 1);
        if (preset == PRESET_COMPATIBILITY) {
            values.put("fast_memory", 0);
            values.put("memory_manager", 0);
            values.put("execution_mode", 1);
            values.put("block_linking", 0);
            values.put("function_replacements", 0);
            values.put("idle_loop_skip", 1);
            values.put("wait_loop_detection", 1);
            values.put("spin_loop_detection", 1);
            values.put("fast_event_timing", 0);
            values.put("jit_block_size", 0);
            values.put("atomics", 0);
            values.put("accurate_cache_line_stores", 1);
            values.put("accurate_dma_timing", 1);
            values.put("cpu_gpu_dual_thread", 0);
            values.put("scheduler", 0);
            values.put("jit_feature_mask", 1);
            values.put("subsystem_multithreading", 0);
            values.put("separate_io_thread", 0);
            values.put("separate_audio_hle_thread", 0);
            values.put("cpu_gpu_sync", 0);
            values.put("force_real_clock_sync", 1);
            values.put("hle_mode", 0);
            values.put("hle_fast_timers", 0);
        } else if (preset == PRESET_PERFORMANCE || preset == PRESET_EXTREME) {
            values.put("fast_memory", preset == PRESET_EXTREME ? 2 : 1);
            values.put("memory_manager", preset == PRESET_EXTREME ? 3 : 1);
            values.put("function_replacements", 2);
            values.put("idle_loop_skip", 2);
            values.put("wait_loop_detection", 2);
            values.put("spin_loop_detection", 2);
            values.put("max_guest_workers", 4);
            values.put("guest_clock_percent", preset == PRESET_EXTREME ? 65 : 85);
            values.put("fast_event_timing", 2);
            values.put("jit_block_size", 2);
            values.put("fp_accuracy", preset == PRESET_EXTREME ? 2 : 1);
            values.put("atomics", 2);
            values.put("memory_exceptions", 0);
            values.put("icache_accuracy", 0);
            values.put("scheduler", 2);
            values.put("preferred_heavy_threads", 4);
            values.put("cpu_gpu_sync", 2);
            values.put("hle_mode", 1);
            values.put("hle_fast_timers", 2);
            if (preset == PRESET_EXTREME) {
                values.put("cycle_skip", 1);
                values.put("sync_on_idle_skip", 0);
            }
        }
        return values;
    }

    private static int clamp(int value, int minimum, int maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }
}
