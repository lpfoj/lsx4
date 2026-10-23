package app.lsx4.android;
import android.content.Context;
import android.content.SharedPreferences;
final class SettingsActivity {
    static SharedPreferences preferences;
    static SharedPreferences prefs(Context context) { return preferences; }
}
