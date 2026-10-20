package app.lsx4.android;

import android.annotation.TargetApi;
import android.content.Context;
import android.os.Build;

final class AndroidApiCompat {
    private AndroidApiCompat() {
    }

    static Object createPerformanceHintSession(
            Context context, int[] threadIds, long targetDurationNanos) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return null;
        }
        return PerformanceHintApi31.create(context, threadIds, targetDurationNanos);
    }

    static void closePerformanceHintSession(Object session) {
        if (session == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return;
        }
        PerformanceHintApi31.close(session);
    }

    static void reportActualWorkDuration(Object session, long durationNanos) {
        if (session == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return;
        }
        PerformanceHintApi31.reportActualWorkDuration(session, durationNanos);
    }

    static void setPerformanceHintThreads(Object session, int[] threadIds) {
        if (session == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            return;
        }
        PerformanceHintApi34.setThreads(session, threadIds);
    }

    static void reportGameState(Context context, boolean loading, boolean gameplay) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return;
        }
        GameStateApi33.report(context, loading, gameplay);
    }

    @TargetApi(Build.VERSION_CODES.S)
    private static final class PerformanceHintApi31 {
        private PerformanceHintApi31() {
        }

        static Object create(Context context, int[] threadIds, long targetDurationNanos) {
            android.os.PerformanceHintManager manager =
                    context.getSystemService(android.os.PerformanceHintManager.class);
            return manager == null
                    ? null
                    : manager.createHintSession(threadIds, targetDurationNanos);
        }

        static void close(Object session) {
            ((android.os.PerformanceHintManager.Session) session).close();
        }

        static void reportActualWorkDuration(Object session, long durationNanos) {
            ((android.os.PerformanceHintManager.Session) session)
                    .reportActualWorkDuration(durationNanos);
        }
    }

    @TargetApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
    private static final class PerformanceHintApi34 {
        private PerformanceHintApi34() {
        }

        static void setThreads(Object session, int[] threadIds) {
            ((android.os.PerformanceHintManager.Session) session).setThreads(threadIds);
        }
    }

    @TargetApi(Build.VERSION_CODES.TIRAMISU)
    private static final class GameStateApi33 {
        private GameStateApi33() {
        }

        static void report(Context context, boolean loading, boolean gameplay) {
            android.app.GameManager manager =
                    context.getSystemService(android.app.GameManager.class);
            if (manager == null) {
                return;
            }
            int mode = gameplay
                    ? android.app.GameState.MODE_GAMEPLAY_UNINTERRUPTIBLE
                    : android.app.GameState.MODE_NONE;
            manager.setGameState(new android.app.GameState(loading, mode));
        }
    }
}
