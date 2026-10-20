package app.lsx4.android;

import android.app.Application;
import android.os.Build;

import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;

public final class Lsx4Application extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        if (isDriverProbeProcess()) {
            return;
        }
        RemoteDiagnostics.initialize(this);
    }

    private boolean isDriverProbeProcess() {
        String processName = "";
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            processName = Application.getProcessName();
        } else {
            try (FileInputStream input = new FileInputStream("/proc/self/cmdline")) {
                byte[] buffer = new byte[256];
                int count = input.read(buffer);
                if (count > 0) {
                    int end = 0;
                    while (end < count && buffer[end] != 0) {
                        end++;
                    }
                    processName = new String(buffer, 0, end, StandardCharsets.UTF_8);
                }
            } catch (Exception ignored) {
            }
        }
        return (getPackageName() + ":driverprobe").equals(processName);
    }
}
