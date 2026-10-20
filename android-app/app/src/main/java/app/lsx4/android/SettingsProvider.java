package app.lsx4.android;

import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.ContentObserver;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.Process;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArrayList;


public final class SettingsProvider extends ContentProvider {
    private static final String READ = "read";
    private static final String COMMIT = "commit";
    private static final String VALUES = "values";
    private static final String REMOVED = "removed";
    private static final String PREFIXES = "prefixes";
    private static final String CLEAR = "clear";
    private static final String SUCCESS = "success";
    private static OwnerBackend owner;

    private static Uri uri(Context context) {
        return Uri.parse("content://" + context.getPackageName() + ".settings");
    }

    static synchronized ProcessSafePreferences.Backend owner(Context context) {
        if (owner == null) {
            owner = new OwnerBackend(context.getSharedPreferences("lsx4_settings", Context.MODE_PRIVATE));
        }
        return owner;
    }

    @Override public boolean onCreate() {
        Context context = getContext();
        if (context == null) return false;
        owner(context).observe(() -> context.getContentResolver().notifyChange(uri(context), null));
        return true;
    }

    @Override public Bundle call(String method, String arg, Bundle extras) {
        if (Binder.getCallingUid() != Process.myUid()) throw new SecurityException("Private settings");
        ProcessSafePreferences.Backend store = owner(getContext());
        Bundle result = new Bundle();
        if (READ.equals(method)) {
            result.putBundle(VALUES, pack(store.read()));
        } else if (COMMIT.equals(method) && extras != null) {
            ProcessSafePreferences.Change change = new ProcessSafePreferences.Change();
            change.clear = extras.getBoolean(CLEAR, false);
            change.values.putAll(unpack(extras.getBundle(VALUES)));
            List<String> removed = extras.getStringArrayList(REMOVED);
            if (removed != null) for (String key : removed) change.values.put(key, null);
            List<String> prefixes = extras.getStringArrayList(PREFIXES);
            if (prefixes != null) change.clearPrefixes.addAll(prefixes);
            result.putBoolean(SUCCESS, store.commit(change));
        } else {
            throw new IllegalArgumentException("Unknown settings operation");
        }
        return result;
    }

    static ProcessSafePreferences.Backend remote(Context context) {
        return new ProcessSafePreferences.Backend() {
            private final ContentResolver resolver = context.getContentResolver();
            private final Uri address = uri(context);
            private ContentObserver observer;
            private Bundle callOwner(String method, Bundle extras) {
                for (int attempt = 0; ; attempt++) {
                    try {
                        Bundle response = resolver.call(address, method, null, extras);
                        if (response == null) throw new IllegalStateException("Settings owner unavailable");
                        return response;
                    } catch (RuntimeException unavailable) {


                        if (!READ.equals(method) || attempt != 0) throw unavailable;
                    }
                }
            }
            @Override public Map<String, ?> read() {
                Bundle values = callOwner(READ, null).getBundle(VALUES);
                if (values == null) throw new IllegalStateException("Missing settings snapshot");
                return unpack(values);
            }
            @Override public boolean commit(ProcessSafePreferences.Change change) {
                Bundle data = new Bundle();
                data.putBoolean(CLEAR, change.clear);
                data.putBundle(VALUES, pack(change.values));
                ArrayList<String> removed = new ArrayList<>();
                for (Map.Entry<String, Object> entry : change.values.entrySet()) {
                    if (entry.getValue() == null) removed.add(entry.getKey());
                }
                data.putStringArrayList(REMOVED, removed);
                data.putStringArrayList(PREFIXES, new ArrayList<>(change.clearPrefixes));
                return callOwner(COMMIT, data).getBoolean(SUCCESS, false);
            }
            @Override public void observe(Runnable invalidated) {


                observer = new ContentObserver(null) {
                    @Override public void onChange(boolean selfChange) { invalidated.run(); }
                };
                resolver.registerContentObserver(address, true, observer);
            }
        };
    }

    private static final class OwnerBackend implements ProcessSafePreferences.Backend {
        private final SharedPreferences preferences;
        private final List<Runnable> observers = new CopyOnWriteArrayList<>();
        OwnerBackend(SharedPreferences preferences) { this.preferences = preferences; }
        @Override public synchronized Map<String, ?> read() { return PreferenceSnapshot.copy(preferences.getAll()); }
        @Override public boolean commit(ProcessSafePreferences.Change change) {
            boolean success;
            synchronized (this) {
                SharedPreferences.Editor editor = preferences.edit();
                if (change.clear) editor.clear();
                if (!change.clearPrefixes.isEmpty()) {
                    for (String key : preferences.getAll().keySet()) {
                        for (String prefix : change.clearPrefixes) {
                            if (key.startsWith(prefix)) { editor.remove(key); break; }
                        }
                    }
                }
                for (Map.Entry<String, Object> entry : change.values.entrySet()) {
                    put(editor, entry.getKey(), entry.getValue());
                }
                success = editor.commit();
            }
            for (Runnable observer : observers) observer.run();
            return success;
        }
        @Override public void observe(Runnable invalidated) { observers.add(invalidated); }
    }

    @SuppressWarnings("unchecked")
    private static void put(SharedPreferences.Editor editor, String key, Object value) {
        if (value == null) editor.remove(key);
        else if (value instanceof String) editor.putString(key, (String) value);
        else if (value instanceof Boolean) editor.putBoolean(key, (Boolean) value);
        else if (value instanceof Integer) editor.putInt(key, (Integer) value);
        else if (value instanceof Long) editor.putLong(key, (Long) value);
        else if (value instanceof Float) editor.putFloat(key, (Float) value);
        else if (value instanceof Set<?>) editor.putStringSet(key, (Set<String>) value);
        else throw new IllegalArgumentException("Unsupported preference value for " + key);
    }

    @SuppressWarnings("unchecked")
    private static Bundle pack(Map<String, ?> values) {
        Bundle result = new Bundle();
        for (Map.Entry<String, ?> entry : values.entrySet()) {
            String key = entry.getKey();
            Object value = entry.getValue();
            if (value == null) continue;
            if (value instanceof String) result.putString(key, (String) value);
            else if (value instanceof Boolean) result.putBoolean(key, (Boolean) value);
            else if (value instanceof Integer) result.putInt(key, (Integer) value);
            else if (value instanceof Long) result.putLong(key, (Long) value);
            else if (value instanceof Float) result.putFloat(key, (Float) value);
            else if (value instanceof Set<?>) result.putStringArrayList(key, new ArrayList<>((Set<String>) value));
            else throw new IllegalArgumentException("Unsupported preference value for " + key);
        }
        return result;
    }

    private static Map<String, Object> unpack(Bundle values) {
        Map<String, Object> result = new LinkedHashMap<>();
        if (values == null) return result;
        for (String key : values.keySet()) {
            Object value = values.get(key);
            if (value instanceof ArrayList<?>) value = new LinkedHashSet<>((ArrayList<?>) value);
            if (value instanceof String || value instanceof Boolean || value instanceof Integer
                    || value instanceof Long || value instanceof Float || value instanceof Set<?>) {
                result.put(key, value);
            } else {
                throw new IllegalArgumentException("Unsupported preference value for " + key);
            }
        }
        return result;
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) { throw new UnsupportedOperationException(); }
    @Override public String getType(Uri uri) { return "vnd.android.cursor.item/vnd.lsx4.settings"; }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException(); }
}
