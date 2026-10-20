package app.lsx4.android;


final class DeviceGraphicsInfo {
    private DeviceGraphicsInfo() {}

    static String[] collect() {
        try {
            System.loadLibrary("lsx_device_info");
            return query();
        } catch (LinkageError | RuntimeException unavailable) {
            return new String[0];
        }
    }

    private static native String[] query();
}
