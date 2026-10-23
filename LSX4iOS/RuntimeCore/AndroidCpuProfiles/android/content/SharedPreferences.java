package android.content;
import java.util.Map;
import java.util.Set;
/** Host-only preference adapter; no Android device or preferences are modified. */
public interface SharedPreferences {
    int getInt(String key, int fallback);
    boolean getBoolean(String key, boolean fallback);
    Set<String> getStringSet(String key, Set<String> fallback);
    boolean contains(String key);
    Map<String, ?> getAll();
    Editor edit();
    interface Editor {
        Editor putInt(String key, int value);
        Editor putBoolean(String key, boolean value);
        Editor putStringSet(String key, Set<String> value);
        Editor remove(String key);
        boolean commit();
    }
}
