package app.lsx4.android;

import android.content.SharedPreferences;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;


class PreferenceSnapshot implements SharedPreferences {
    private final Map<String, ?> snapshot;

    PreferenceSnapshot(Map<String, ?> values) {
        snapshot = copy(values);
    }

    static SharedPreferences of(SharedPreferences preferences) {
        return new PreferenceSnapshot(preferences.getAll());
    }

    static Map<String, ?> copy(Map<String, ?> values) {
        Map<String, Object> result = new LinkedHashMap<>();
        for (Map.Entry<String, ?> entry : values.entrySet()) {
            Object value = entry.getValue();
            if (value instanceof Set<?>) {
                value = Collections.unmodifiableSet(new LinkedHashSet<>((Set<?>) value));
            }
            result.put(entry.getKey(), value);
        }
        return Collections.unmodifiableMap(result);
    }

    protected Map<String, ?> values() {
        return snapshot;
    }

    @Override public Map<String, ?> getAll() { return values(); }
    @Override public boolean contains(String key) { return values().containsKey(key); }
    @Override public String getString(String key, String fallback) {
        Object value = values().get(key);
        return value == null ? fallback : (String) value;
    }
    @Override @SuppressWarnings("unchecked")
    public Set<String> getStringSet(String key, Set<String> fallback) {
        Object value = values().get(key);
        Set<String> result = value == null ? fallback : (Set<String>) value;
        return result == null ? null : Collections.unmodifiableSet(new LinkedHashSet<>(result));
    }
    @Override public int getInt(String key, int fallback) {
        Object value = values().get(key);
        return value == null ? fallback : (Integer) value;
    }
    @Override public long getLong(String key, long fallback) {
        Object value = values().get(key);
        return value == null ? fallback : (Long) value;
    }
    @Override public float getFloat(String key, float fallback) {
        Object value = values().get(key);
        return value == null ? fallback : (Float) value;
    }
    @Override public boolean getBoolean(String key, boolean fallback) {
        Object value = values().get(key);
        return value == null ? fallback : (Boolean) value;
    }
    @Override public Editor edit() { throw new UnsupportedOperationException("Read-only snapshot"); }
    @Override public void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener) {}
    @Override public void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener) {}
}
