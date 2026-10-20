package app.lsx4.android;

import static org.junit.Assert.*;

import android.content.SharedPreferences;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import org.junit.Test;

public final class ProcessSafePreferencesTest {

    private static final class Store {
        Map<String, Object> durable = new LinkedHashMap<>();
        final List<Client> clients = new ArrayList<>();
        boolean fail;
        boolean failReads;

        Client client() {
            Client result = new Client();
            clients.add(result);
            return result;
        }

        final class Client implements ProcessSafePreferences.Backend {
            Runnable observer;
            boolean notifications = true;
            int reads;
            ProcessSafePreferences.Change lastWrite;
            @Override public Map<String, ?> read() {
                reads++;
                if (failReads) throw new IllegalStateException("Owner restarting");
                return new LinkedHashMap<>(durable);
            }
            @Override public boolean commit(ProcessSafePreferences.Change change) {
                lastWrite = change;
                if (fail) return false;
                Map<String, Object> next = change.clear
                        ? new LinkedHashMap<>() : new LinkedHashMap<>(durable);
                for (String prefix : change.clearPrefixes) {
                    next.keySet().removeIf(key -> key.startsWith(prefix));
                }
                for (Map.Entry<String, Object> entry : change.values.entrySet()) {
                    if (entry.getValue() == null) next.remove(entry.getKey());
                    else next.put(entry.getKey(), entry.getValue());
                }
                durable = next;
                for (Client client : clients) {
                    if (client.notifications && client.observer != null) client.observer.run();
                }
                return true;
            }
            @Override public void observe(Runnable invalidated) { observer = invalidated; }
        }
    }

    private static ProcessSafePreferences preferences(Store.Client client) {
        return new ProcessSafePreferences(client, Runnable::run);
    }

    @Test public void staleGameWriteCannotEraseNewGlobalOrOtherGameChanges() {
        Store store = new Store();
        Store.Client gameTransport = store.client();
        gameTransport.notifications = false;
        ProcessSafePreferences game = preferences(gameTransport);
        ProcessSafePreferences launcher = preferences(store.client());
        game.getAll();
        launcher.edit().putInt("cpu.global.guest_clock_percent", 85)
                .putBoolean("game.profile.CUSA00002.audio", false).commit();
        game.edit().putInt("game.profile.CUSA00001.overlay_opacity", 62).apply();
        assertEquals(Collections.singleton("game.profile.CUSA00001.overlay_opacity"),
                gameTransport.lastWrite.values.keySet());
        ProcessSafePreferences coldRestart = preferences(store.client());
        assertEquals(85, coldRestart.getInt("cpu.global.guest_clock_percent", 0));
        assertFalse(coldRestart.getBoolean("game.profile.CUSA00002.audio", true));
        assertEquals(62, coldRestart.getInt("game.profile.CUSA00001.overlay_opacity", 0));
    }

    @Test public void inheritedGameReadsRemainSparseAndTrackGlobalUpdates() {
        Store store = new Store();
        ProcessSafePreferences base = preferences(store.client());
        ProcessSafePreferences launcher = preferences(store.client());
        SharedPreferences game = GameProfileSettings.forGame(base, "CUSA00001");
        launcher.edit().putInt(SettingsActivity.K_RES_MODE, SettingsActivity.RES_720P).commit();
        assertEquals(SettingsActivity.RES_720P, game.getInt(SettingsActivity.K_RES_MODE, 0));
        assertFalse(GameProfileSettings.hasOverrides(base, "CUSA00001"));
        launcher.edit().putInt(SettingsActivity.K_RES_MODE, SettingsActivity.RES_1080P).commit();
        assertEquals(SettingsActivity.RES_1080P, game.getInt(SettingsActivity.K_RES_MODE, 0));
        game.edit().putInt(SettingsActivity.K_RES_MODE, SettingsActivity.RES_544P).commit();
        launcher.edit().putInt(SettingsActivity.K_RES_MODE, SettingsActivity.RES_360P).commit();
        assertEquals(SettingsActivity.RES_544P, game.getInt(SettingsActivity.K_RES_MODE, 0));
        assertEquals(SettingsActivity.RES_360P, base.getInt(SettingsActivity.K_RES_MODE, 0));
        game.edit().remove(SettingsActivity.K_RES_MODE).commit();
        assertEquals(SettingsActivity.RES_360P, game.getInt(SettingsActivity.K_RES_MODE, 0));
    }

    @Test public void delayedScopedClearRemovesNewestOwnKeysButPreservesOtherProfiles() {
        Store store = new Store();
        Store.Client staleTransport = store.client();
        staleTransport.notifications = false;
        ProcessSafePreferences stale = preferences(staleTransport);
        ProcessSafePreferences other = preferences(store.client());
        SharedPreferences game = GameProfileSettings.forGame(stale, "CUSA00001");
        game.getAll();
        SharedPreferences.Editor reset = game.edit().clear();
        other.edit().putBoolean(GameProfileSettings.scopedKey("CUSA00001", SettingsActivity.K_AUDIO), false)
                .putBoolean(GameProfileSettings.scopedKey("CUSA00002", SettingsActivity.K_AUDIO), false)
                .putBoolean(SettingsActivity.K_AUDIO, true).commit();
        reset.commit();
        assertFalse(GameProfileSettings.hasOverrides(other, "CUSA00001"));
        assertTrue(GameProfileSettings.hasOverrides(other, "CUSA00002"));
        assertTrue(other.getBoolean(SettingsActivity.K_AUDIO, false));
    }

    @Test public void clearThenPutUsesAtomicEditorSemantics() {
        Store store = new Store();
        ProcessSafePreferences preferences = preferences(store.client());
        preferences.edit().putInt("old", 1).commit();
        preferences.edit().putInt("kept", 2).clear().putInt("new", 3).commit();
        assertFalse(preferences.contains("old"));
        assertEquals(2, preferences.getInt("kept", 0));
        assertEquals(3, preferences.getInt("new", 0));
    }

    @Test public void notificationInvalidatesOnlyOnceAndSteadyReadsStayLocal() {
        Store store = new Store();
        Store.Client transport = store.client();
        ProcessSafePreferences game = preferences(transport);
        ProcessSafePreferences launcher = preferences(store.client());
        List<String> changed = new ArrayList<>();
        SharedPreferences.OnSharedPreferenceChangeListener listener = (prefs, key) -> changed.add(key);
        game.registerOnSharedPreferenceChangeListener(listener);
        launcher.edit().putInt("value", 3).commit();
        assertEquals(Collections.singletonList("value"), changed);
        int reads = transport.reads;
        for (int frame = 0; frame < 1000; frame++) assertEquals(3, game.getInt("value", 0));
        assertEquals(reads, transport.reads);
    }

    @Test public void explicitRefreshRecoversMissedObserverBeforeLaunch() {
        Store store = new Store();
        Store.Client transport = store.client();
        transport.notifications = false;
        ProcessSafePreferences game = preferences(transport);
        ProcessSafePreferences launcher = preferences(store.client());
        game.getAll();
        launcher.edit().putInt("value", 9).commit();
        assertEquals(0, game.getInt("value", 0));
        game.refreshSnapshot();
        assertEquals(9, game.getInt("value", 0));
    }

    @Test public void failedInvalidationReadKeepsLastSnapshotButExplicitRefreshReportsFailure() {
        Store store = new Store();
        Store.Client transport = store.client();
        ProcessSafePreferences game = preferences(transport);
        game.edit().putInt("value", 4).commit();
        SharedPreferences.OnSharedPreferenceChangeListener listener = (prefs, key) -> {};
        game.registerOnSharedPreferenceChangeListener(listener);
        store.failReads = true;
        transport.observer.run();
        int reads = transport.reads;
        for (int i = 0; i < 5; i++) assertEquals(4, game.getInt("value", 0));
        assertEquals(reads, transport.reads);
        try { game.refreshSnapshot(); fail("Launch must not silently use a stale revision"); }
        catch (IllegalStateException expected) {}
        store.failReads = false;
        store.durable.put("value", 8);
        game.refreshSnapshot();
        assertEquals(8, game.getInt("value", 0));
    }

    @Test public void stringSetsCannotMutateStoredOrInheritedValues() {
        Store store = new Store();
        ProcessSafePreferences base = preferences(store.client());
        Set<String> input = new LinkedHashSet<>(Arrays.asList("a", "b"));
        SharedPreferences.Editor editor = base.edit().putStringSet("set", input);
        input.add("c");
        editor.commit();
        assertEquals(new LinkedHashSet<>(Arrays.asList("a", "b")), base.getStringSet("set", null));
        try { base.getStringSet("set", null).add("changed"); fail("Mutable preference set"); }
        catch (UnsupportedOperationException expected) {}
    }

    @Test public void failedDurableWriteIsNotReportedAsSaved() {
        Store store = new Store();
        ProcessSafePreferences base = preferences(store.client());
        store.fail = true;
        assertFalse(base.edit().putInt("value", 1).commit());
        try { base.edit().putInt("value", 2).apply(); fail("Failure was silently ignored"); }
        catch (IllegalStateException expected) {}
        assertFalse(base.contains("value"));
    }

    @Test public void scopedListenerIgnoresOtherGameAndReportsLogicalKey() {
        Store store = new Store();
        ProcessSafePreferences base = preferences(store.client());
        SharedPreferences game = GameProfileSettings.forGame(base, "CUSA00001");
        List<String> changed = new ArrayList<>();
        SharedPreferences.OnSharedPreferenceChangeListener listener = (prefs, key) -> changed.add(key);
        game.registerOnSharedPreferenceChangeListener(listener);
        base.edit().putBoolean(GameProfileSettings.scopedKey("CUSA00002", SettingsActivity.K_AUDIO), false).commit();
        assertTrue(changed.isEmpty());
        game.edit().putBoolean(SettingsActivity.K_AUDIO, false).commit();
        assertEquals(Collections.singletonList(SettingsActivity.K_AUDIO), changed);
    }

    @Test public void cpuResolutionUsesOneSnapshotAndDoesNotChangeProfilePriority() {
        Store store = new Store();
        ProcessSafePreferences base = preferences(store.client());
        base.edit().putInt("cpu.global.preset", CpuTuningSettings.PRESET_CUSTOM)
                .putInt("cpu.global.guest_clock_percent", 85)
                .putInt("cpu.game.CUSA00001.preset", CpuTuningSettings.PRESET_CUSTOM)
                .putInt("cpu.game.CUSA00001.guest_clock_percent", 60)
                .putBoolean("cpu.game.CUSA00001.enabled", true).commit();
        SharedPreferences singleSnapshot = new PreferenceSnapshot(base.getAll()) {
            @Override public int getInt(String key, int fallback) { throw new AssertionError("Unfrozen read"); }
            @Override public boolean contains(String key) { throw new AssertionError("Unfrozen read"); }
        };
        assertEquals(Integer.valueOf(60), CpuTuningSettings.resolvedValues(singleSnapshot, "CUSA00001").get("guest_clock_percent"));
        assertEquals(Integer.valueOf(85), CpuTuningSettings.resolvedValues(base, "CUSA00002").get("guest_clock_percent"));
        CpuTuningSettings.clearGameProfile(base, "CUSA00001");
        assertEquals(Integer.valueOf(85), CpuTuningSettings.resolvedValues(base, "CUSA00001").get("guest_clock_percent"));
    }

    @Test public void namedSharedPresetRetainsBloodborneContractButCustomKeepsExplicitChoice() {
        Store store = new Store();
        ProcessSafePreferences base = preferences(store.client());
        base.edit().putBoolean(CpuTuningSettings.K_ALL_GAMES_ENABLED, true)
                .putInt("cpu.all_games.preset", CpuTuningSettings.PRESET_BALANCED)
                .putInt("cpu.all_games.block_linking", 0).commit();
        assertEquals(Integer.valueOf(1), CpuTuningSettings.resolvedValues(base, "CUSA03173").get("block_linking"));
        base.edit().putInt("cpu.all_games.preset", CpuTuningSettings.PRESET_CUSTOM).commit();
        assertEquals(Integer.valueOf(0), CpuTuningSettings.resolvedValues(base, "CUSA03173").get("block_linking"));
    }
}
