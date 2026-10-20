package app.lsx4.android;

import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;







final class ProcessSafePreferences extends PreferenceSnapshot {
    interface Backend {
        Map<String, ?> read();
        boolean commit(Change change);
        void observe(Runnable invalidated);
    }

    static final class Change {
        final Map<String, Object> values = new LinkedHashMap<>();
        final Set<String> clearPrefixes = new LinkedHashSet<>();
        boolean clear;

        Change copy() {
            Change result = new Change();
            result.values.putAll(PreferenceSnapshot.copy(values));
            result.clearPrefixes.addAll(clearPrefixes);
            result.clear = clear;
            return result;
        }
    }

    private static ProcessSafePreferences instance;
    private final Backend backend;
    private final Executor callbacks;
    private final Object refreshLock = new Object();
    private final AtomicLong invalidation = new AtomicLong();
    private final AtomicBoolean refreshQueued = new AtomicBoolean();
    private final Map<OnSharedPreferenceChangeListener, Boolean> listeners = new WeakHashMap<>();
    private volatile Map<String, ?> cached;
    private volatile long loadedInvalidation = -1;
    private volatile long retryReadAfterNanos;



    ProcessSafePreferences(Backend backend, Executor callbacks) {
        super(Collections.emptyMap());
        this.backend = backend;
        this.callbacks = callbacks;
        backend.observe(this::invalidate);
    }

    static synchronized SharedPreferences get(Context context) {
        if (instance == null) {
            Context application = context.getApplicationContext();
            Context storeContext = application == null ? context : application;


            Backend backend = isOwnerProcess(storeContext)
                    ? SettingsProvider.owner(storeContext)
                    : SettingsProvider.remote(storeContext);
            Handler main = new Handler(Looper.getMainLooper());
            instance = new ProcessSafePreferences(backend, task -> {
                if (Looper.myLooper() == main.getLooper()) {
                    task.run();
                } else {
                    main.post(task);
                }
            });
        }
        return instance;
    }


    static void refresh(Context context) {
        ((ProcessSafePreferences) get(context)).refreshSnapshot();
    }

    private static boolean isOwnerProcess(Context context) {
        String name = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                ? Application.getProcessName() : null;
        if (name == null || name.isEmpty()) {
            try (FileInputStream input = new FileInputStream("/proc/self/cmdline")) {
                byte[] buffer = new byte[256];
                int count = input.read(buffer);
                int end = 0;
                while (end < count && buffer[end] != 0) end++;
                name = new String(buffer, 0, end, StandardCharsets.UTF_8);
            } catch (Exception error) {
                throw new IllegalStateException("Cannot identify settings owner process", error);
            }
        }
        if (name == null || name.isEmpty()) {
            throw new IllegalStateException("Empty settings process identity");
        }
        String ownerName = context.getApplicationInfo().processName;
        return (ownerName == null ? context.getPackageName() : ownerName).equals(name);
    }

    private void invalidate() {
        invalidation.incrementAndGet();
        synchronized (listeners) {
            if (listeners.isEmpty() || !refreshQueued.compareAndSet(false, true)) return;
        }
        callbacks.execute(() -> {
            refreshQueued.set(false);


            values();
        });
    }

    @Override protected Map<String, ?> values() {
        Map<String, ?> current = cached;
        if (current != null && (loadedInvalidation == invalidation.get()
                || System.nanoTime() < retryReadAfterNanos)) return current;
        try {
            return refreshSnapshot();
        } catch (RuntimeException unavailable) {
            if (current == null) throw unavailable;
            retryReadAfterNanos = System.nanoTime() + 250_000_000L;
            return current;
        }
    }

    Map<String, ?> refreshSnapshot() {
        Map<String, ?> before;
        Map<String, ?> after;
        synchronized (refreshLock) {
            before = cached;
            long generation;
            do {
                generation = invalidation.get();
                after = PreferenceSnapshot.copy(backend.read());
            } while (generation != invalidation.get());
            cached = after;
            loadedInvalidation = generation;
            retryReadAfterNanos = 0;
        }
        if (before != null) notifyChanges(before, after);
        return after;
    }

    private void notifyChanges(Map<String, ?> before, Map<String, ?> after) {
        Set<String> changed = new LinkedHashSet<>(before.keySet());
        changed.addAll(after.keySet());
        changed.removeIf(key -> before.containsKey(key) == after.containsKey(key)
                && Objects.equals(before.get(key), after.get(key)));
        if (changed.isEmpty()) return;
        List<OnSharedPreferenceChangeListener> targets;
        synchronized (listeners) { targets = new ArrayList<>(listeners.keySet()); }
        if (targets.isEmpty()) return;
        callbacks.execute(() -> {
            for (OnSharedPreferenceChangeListener listener : targets) {
                for (String key : changed) listener.onSharedPreferenceChanged(this, key);
            }
        });
    }

    @Override public void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener) {
        if (listener == null) return;
        values();
        synchronized (listeners) { listeners.put(listener, Boolean.TRUE); }
    }

    @Override public void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener) {
        synchronized (listeners) { listeners.remove(listener); }
    }

    @Override public Editor edit() { return new DeltaEditor(); }

    static void removePrefix(Editor editor, SharedPreferences preferences, String prefix) {
        if (editor instanceof ProcessSafePreferences.DeltaEditor) {
            ((ProcessSafePreferences.DeltaEditor) editor).change.clearPrefixes.add(prefix);
        } else {
            for (String key : preferences.getAll().keySet()) {
                if (key.startsWith(prefix)) editor.remove(key);
            }
        }
    }

    private final class DeltaEditor implements Editor {
        private final Change change = new Change();
        @Override public Editor putString(String key, String value) { change.values.put(key, value); return this; }
        @Override public Editor putStringSet(String key, Set<String> value) {
            change.values.put(key, value == null ? null : new LinkedHashSet<>(value)); return this;
        }
        @Override public Editor putInt(String key, int value) { change.values.put(key, value); return this; }
        @Override public Editor putLong(String key, long value) { change.values.put(key, value); return this; }
        @Override public Editor putFloat(String key, float value) { change.values.put(key, value); return this; }
        @Override public Editor putBoolean(String key, boolean value) { change.values.put(key, value); return this; }
        @Override public Editor remove(String key) { change.values.put(key, null); return this; }
        @Override public Editor clear() { change.clear = true; return this; }
        @Override public boolean commit() {
            Change pending = change.copy();
            change.values.clear();
            change.clearPrefixes.clear();
            change.clear = false;
            boolean success = backend.commit(pending);
            refreshSnapshot();
            return success;
        }
        @Override public void apply() {
            if (!commit()) throw new IllegalStateException("Cannot persist application settings");
        }
    }
}
