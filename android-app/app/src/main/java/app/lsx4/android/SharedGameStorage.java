package app.lsx4.android;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;

import java.io.File;
import java.io.IOException;


final class SharedGameStorage {
    private static final String COMMON_DIRECTORY = "Emulation/PS4";

    private SharedGameStorage() {
    }

    static File gamesDirectory() throws IOException {
        File base = new File(Environment.getExternalStorageDirectory(), COMMON_DIRECTORY)
                .getCanonicalFile();
        return new File(base, "games").getCanonicalFile();
    }

    static boolean hasAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R
                || Environment.isExternalStorageManager();
    }

    static void requestAccessIfNeeded(Context context) {
        if (hasAccess()) {
            return;
        }
        Intent intent = new Intent(
                Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + context.getPackageName()));
        context.startActivity(intent);
    }
}
