package app.lsx4.android;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.List;


public final class GameCaptureService extends Service {
    interface Listener {
        void stateChanged(boolean active, boolean withAudio, long startedAt);
        void completed(File file, boolean failed);
    }

    private static final String CHANNEL = "lsx4_game_capture";
    private static final int NOTIFICATION_ID = 804;
    private static final String ACTION_START = "app.lsx4.android.capture.START";
    private static final String ACTION_STOP = "app.lsx4.android.capture.STOP";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final List<Runnable> idleCallbacks = new ArrayList<>();
    private static GameCaptureService current;
    private static Listener listener;
    private static boolean busy;
    private static boolean stopPending;
    private static boolean recording;
    private static boolean hasAudio;
    private static long startedAt;
    private MediaProjection projection;
    private CaptureRecorder recorder;
    private boolean ending;
    private boolean projectionInputStopped;
    private final MediaProjection.Callback projectionCallback = new MediaProjection.Callback() {
        @Override public void onStop() { requestStop(); }
        @Override public void onCapturedContentVisibilityChanged(boolean visible) {
            if (!visible) requestStop();
        }
    };

    static void setListener(Listener next) {
        listener = next;
        if (next != null) next.stateChanged(recording, hasAudio, startedAt);
    }

    static void clearListener(Listener owner) {
        if (listener == owner) listener = null;
    }

    static boolean isBusy() { return busy; }

    static void start(Context context, Intent consent, int resultCode, int width, int height,
            int density, boolean audio, String title) {
        if (busy) return;
        busy = true;
        stopPending = false;
        Intent intent = new Intent(context, GameCaptureService.class).setAction(ACTION_START)
                .putExtra("consent", consent).putExtra("result", resultCode)
                .putExtra("width", width).putExtra("height", height)
                .putExtra("density", density).putExtra("audio", audio)
                .putExtra("title", title);
        try {
            context.startForegroundService(intent);
        } catch (RuntimeException error) {
            Log.e("LSX4Capture", "Could not start foreground capture", error);
            finishStatic(null, true);
        }
    }

    static void requestStop() {
        if (!busy) return;
        stopPending = true;
        if (current != null) {
            if (current.recorder != null) current.recorder.stop();


            current.stopProjectionInput();
        }
    }

    static void whenIdle(Runnable callback) {
        if (!busy) {
            MAIN.post(callback);
        } else {
            idleCallbacks.add(callback);
            requestStop();
        }
    }

    @Override protected void attachBaseContext(Context base) {
        super.attachBaseContext(AppLocale.wrap(base));
    }

    @Override public void onCreate() {
        super.onCreate();
        current = this;
        NotificationManager manager = getSystemService(NotificationManager.class);
        NotificationChannel channel = new NotificationChannel(CHANNEL,
                getString(R.string.capture_notification_channel), NotificationManager.IMPORTANCE_LOW);
        channel.setSound(null, null);
        channel.enableVibration(false);
        manager.createNotificationChannel(channel);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null || ACTION_STOP.equals(intent.getAction())) {
            requestStop();
            if (!busy) stopSelf();
            return START_NOT_STICKY;
        }
        if (!ACTION_START.equals(intent.getAction()) || recorder != null || ending) {
            return START_NOT_STICKY;
        }
        busy = true;
        try {
            Notification notice = notification(false);
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTIFICATION_ID, notice,
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
            } else {
                startForeground(NOTIFICATION_ID, notice);
            }
            if (stopPending) { end(null, false); return START_NOT_STICKY; }
            Intent consent = Build.VERSION.SDK_INT >= 33
                    ? intent.getParcelableExtra("consent", Intent.class)
                    : intent.getParcelableExtra("consent");
            if (consent == null || intent.getIntExtra("result", 0) != Activity.RESULT_OK) {
                throw new IllegalArgumentException("Missing screen-capture consent");
            }
            MediaProjectionManager manager = getSystemService(MediaProjectionManager.class);
            projection = manager.getMediaProjection(Activity.RESULT_OK, consent);
            if (projection == null) throw new IllegalStateException("Projection unavailable");
            projection.registerCallback(projectionCallback, MAIN);
            recorder = new CaptureRecorder(new CaptureStore(getFilesDir()), projection,
                    intent.getStringExtra("title"), intent.getIntExtra("width", 0),
                    intent.getIntExtra("height", 0), intent.getIntExtra("density", 0),
                    intent.getBooleanExtra("audio", false), new CaptureRecorder.Listener() {
                        @Override public void started(boolean withAudio) {
                            MAIN.post(() -> {
                                if (ending) return;
                                recording = true;
                                hasAudio = withAudio;
                                startedAt = SystemClock.elapsedRealtime();
                                getSystemService(NotificationManager.class).notify(
                                        NOTIFICATION_ID, notification(true));
                                if (listener != null) listener.stateChanged(true, withAudio, startedAt);
                                if (stopPending) recorder.stop();
                            });
                        }
                        @Override public void finished(File completed, String error) {
                            MAIN.post(() -> end(completed, error != null));
                        }
                    });
            recorder.start();
            if (stopPending) recorder.stop();
        } catch (Exception error) {
            Log.e("LSX4Capture", "Capture setup failed", error);
            end(null, true);
        }
        return START_NOT_STICKY;
    }

    private Notification notification(boolean active) {
        PendingIntent stop = PendingIntent.getService(this, 0,
                new Intent(this, GameCaptureService.class).setAction(ACTION_STOP),
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder builder = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.capture_video)
                .setContentTitle(getString(active ? R.string.capture_recording
                        : R.string.capture_recording_starting))
                .setContentText(getString(active && !hasAudio
                        ? R.string.capture_video_only : R.string.capture_notification_detail))
                .setOngoing(true).setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_SERVICE)
                .addAction(new Notification.Action.Builder(
                        android.graphics.drawable.Icon.createWithResource(this, R.drawable.capture_stop),
                        getString(R.string.capture_stop_recording), stop).build());
        if (active) builder.setWhen(System.currentTimeMillis()).setUsesChronometer(true);
        return builder.build();
    }

    private void end(File file, boolean failed) {
        if (ending) return;
        ending = true;
        if (projection != null) {
            try { projection.unregisterCallback(projectionCallback); }
            catch (RuntimeException error) { Log.w("LSX4Capture", "Projection callback cleanup", error); }
            stopProjectionInput();
            projection = null;
        }
        recorder = null;
        stopForeground(STOP_FOREGROUND_REMOVE);
        finishStatic(file, failed);
        stopSelf();
    }

    private void stopProjectionInput() {
        if (projection == null || projectionInputStopped) return;
        projectionInputStopped = true;
        try { projection.stop(); }
        catch (RuntimeException error) { Log.w("LSX4Capture", "Projection input stop failed", error); }
    }

    private static void finishStatic(File file, boolean failed) {
        busy = false;
        stopPending = false;
        recording = false;
        hasAudio = false;
        startedAt = 0;
        if (listener != null) {
            listener.stateChanged(false, false, 0);
            listener.completed(file, failed);
        }
        List<Runnable> callbacks = new ArrayList<>(idleCallbacks);
        idleCallbacks.clear();
        for (Runnable callback : callbacks) MAIN.post(callback);
    }

    @Override public void onDestroy() {
        if (recorder != null) recorder.stop();
        stopProjectionInput();
        if (current == this) current = null;
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) { return null; }
}
