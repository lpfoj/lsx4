package app.lsx4.android;

import android.content.Context;

public final class GuestCpuBackend {
    public static final int CURRENT = 0;
    public static final int FEX_EXPERIMENTAL = 1;

    private GuestCpuBackend() {
    }

    public static int sanitize(int storedId) {
        return storedId == FEX_EXPERIMENTAL ? FEX_EXPERIMENTAL : CURRENT;
    }

    public static int fromPosition(int position) {
        return position == 1 ? FEX_EXPERIMENTAL : CURRENT;
    }

    public static int toPosition(int backendId) {
        return sanitize(backendId) == FEX_EXPERIMENTAL ? 1 : 0;
    }

    public static String label(Context context, int backendId) {
        return context.getString(sanitize(backendId) == FEX_EXPERIMENTAL
                ? R.string.guest_cpu_backend_fex_experimental
                : R.string.guest_cpu_backend_current);
    }

    public static String[] labels(Context context) {
        return new String[]{
                context.getString(R.string.guest_cpu_backend_current),
                context.getString(R.string.guest_cpu_backend_fex_experimental)
        };
    }
}
