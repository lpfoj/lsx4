package app.lsx4.android;

import android.content.SharedPreferences;
import android.view.KeyEvent;


final class HardwareTriggerMapper {
    static final int SLOT_LEFT = 0;
    static final int SLOT_RIGHT = 1;
    static final String PREFIX = "phone_trigger_v1_";

    static final int[] TARGET_MASKS = {
            0x4000, 0x2000, 0x8000, 0x1000,
            0x400, 0x800, 0x100, 0x200,
            0x2, 0x4, 0x8, 0x1,
            0x10000, 0x100000,
            0x10, 0x20, 0x40, 0x80
    };

    static final String[] TARGET_LABELS = {
            "Cross", "Circle", "Square", "Triangle",
            "L1", "R1", "L2", "R2", "L3", "R3", "Options", "Share",
            "PS", "Touch pad", "D-Pad Up", "D-Pad Right", "D-Pad Down", "D-Pad Left"
    };

    private HardwareTriggerMapper() {}

    private static String key(int slot, String suffix) {
        return PREFIX + (slot == SLOT_LEFT ? "left_" : "right_") + suffix;
    }

    static void capture(SharedPreferences preferences, int slot, KeyEvent event) {
        preferences.edit()
                .putInt(key(slot, "key_code"), event.getKeyCode())
                .putInt(key(slot, "scan_code"), event.getScanCode())
                .apply();
    }

    static void clear(SharedPreferences preferences, int slot) {
        preferences.edit()
                .remove(key(slot, "key_code"))
                .remove(key(slot, "scan_code"))
                .apply();
    }

    static boolean isCaptured(SharedPreferences preferences, int slot) {
        return preferences.contains(key(slot, "key_code"));
    }

    static String describe(SharedPreferences preferences, int slot) {
        if (!isCaptured(preferences, slot)) {
            return "Not detected";
        }
        int keyCode = preferences.getInt(key(slot, "key_code"), KeyEvent.KEYCODE_UNKNOWN);
        int scanCode = preferences.getInt(key(slot, "scan_code"), 0);
        return KeyEvent.keyCodeToString(keyCode) + (scanCode == 0 ? "" : " · scan " + scanCode);
    }

    static void setTargetIndex(SharedPreferences preferences, int slot, int index) {
        int safe = Math.max(0, Math.min(TARGET_MASKS.length - 1, index));
        preferences.edit().putInt(key(slot, "target"), safe).apply();
    }

    static int targetIndex(SharedPreferences preferences, int slot) {
        return Math.max(0, Math.min(TARGET_MASKS.length - 1,
                preferences.getInt(key(slot, "target"), slot == SLOT_LEFT ? 6 : 7)));
    }

    static int map(SharedPreferences preferences, KeyEvent event) {
        for (int slot = SLOT_LEFT; slot <= SLOT_RIGHT; ++slot) {
            if (!isCaptured(preferences, slot)) {
                continue;
            }
            int keyCode = preferences.getInt(key(slot, "key_code"), KeyEvent.KEYCODE_UNKNOWN);
            int scanCode = preferences.getInt(key(slot, "scan_code"), 0);
            boolean scanMatches = scanCode == 0 || event.getScanCode() == scanCode;
            if (event.getKeyCode() == keyCode && scanMatches) {
                return TARGET_MASKS[targetIndex(preferences, slot)];
            }
        }
        return 0;
    }
}
