package app.lsx4.android;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.media.projection.MediaProjectionConfig;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.PixelCopy;
import android.view.SurfaceView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.Supplier;


final class GameCaptureController implements AutoCloseable {
    static final int REQUEST_SCREEN_CAPTURE = 7301;
    static final int REQUEST_GAME_AUDIO = 7302;

    interface Listener {
        void onRecordingStateChanged(boolean recording);
        void onGalleryVisibilityChanged(boolean visible);
        default void onRecordingStatusChanged(String status) { }
    }

    private final Activity activity;
    private final SurfaceView surface;
    private final Supplier<String> gameName;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService files = Executors.newSingleThreadExecutor(
            runnable -> new Thread(runnable, "LSX4-capture-save"));
    private CaptureStore store;
    private CaptureGallery gallery;
    private AlertDialog prompt;
    private boolean screenshotBusy;
    private boolean consentPending;
    private boolean requestAudio;
    private Intent approvedConsent;
    private boolean foreground;
    private boolean runtimePaused;
    private boolean closed;
    private boolean recording;
    private boolean audio;
    private long recordingAt;
    private boolean recorderClosed;
    private Runnable finishClose;
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (!recording || closed) return;
            long seconds = Math.max(0, (SystemClock.elapsedRealtime() - recordingAt) / 1000);
            String time = String.format(Locale.ROOT, "%02d:%02d", seconds / 60, seconds % 60);
            listener.onRecordingStatusChanged(audio ? "● " + time
                    : activity.getString(R.string.capture_silent_status, time));
            main.postDelayed(this, 1000);
        }
    };
    private final GameCaptureService.Listener serviceListener = new GameCaptureService.Listener() {
        @Override public void stateChanged(boolean active, boolean withAudio, long startedAt) {
            recording = active;
            audio = withAudio;
            recordingAt = startedAt;
            main.removeCallbacks(tick);
            if (closed) return;
            listener.onRecordingStateChanged(active);
            if (active) {
                if (!withAudio) toast(R.string.capture_video_only);
                tick.run();
            } else listener.onRecordingStatusChanged("");
        }
        @Override public void completed(File file, boolean failed) {
            if (closed) return;
            if (file != null) toast(R.string.capture_video_saved);
            else if (failed) toast(R.string.capture_recording_failed);
        }
    };

    GameCaptureController(Activity activity, SurfaceView surface, Supplier<String> gameName,
            Listener listener) {
        this.activity = activity;
        this.surface = surface;
        this.gameName = gameName;
        this.listener = listener;
        GameCaptureService.setListener(serviceListener);
    }

    void takeScreenshot() {
        if (closed || screenshotBusy) return;
        if (!ensureStore()) return;
        screenshotBusy = true;
        copyScreenshot(0);
    }

    private void copyScreenshot(int attempt) {
        if (closed || !surface.getHolder().getSurface().isValid()) {
            screenshotFinished(false);
            return;
        }
        Rect source = surface.getHolder().getSurfaceFrame();
        int width = source.width();
        int height = source.height();
        if (width <= 0 || height <= 0) { screenshotFinished(false); return; }
        Bitmap allocated = null;
        try {
            allocated = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            final Bitmap bitmap = allocated;
            PixelCopy.request(surface, bitmap, result -> {
                if (result != PixelCopy.SUCCESS || closed) {
                    bitmap.recycle();
                    if (!closed && result == PixelCopy.ERROR_SOURCE_NO_DATA && attempt < 2) {
                        main.postDelayed(() -> copyScreenshot(attempt + 1), 150);
                    } else screenshotFinished(false);
                    return;
                }
                String title = gameName.get();
                files.execute(() -> {
                    File pending = null;
                    boolean saved = false;
                    try {
                        pending = store.createPending(title, "png");
                        try (FileOutputStream output = new FileOutputStream(pending)) {
                            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, output)) {
                                throw new IOException("PNG encoding failed");
                            }
                            output.getFD().sync();
                        }
                        store.publish(pending);
                        saved = true;
                    } catch (Exception error) {
                        Log.w("LSX4Capture", "Screenshot save failed", error);
                        store.discard(pending);
                    } finally {
                        bitmap.recycle();
                    }
                    boolean success = saved;
                    main.post(() -> screenshotFinished(success));
                });
            }, main);
        } catch (Exception | OutOfMemoryError error) {
            if (allocated != null) allocated.recycle();
            Log.w("LSX4Capture", "Surface capture failed", error);
            screenshotFinished(false);
        }
    }

    private void screenshotFinished(boolean success) {
        screenshotBusy = false;
        if (!closed) toast(success ? R.string.capture_screenshot_saved : R.string.capture_screenshot_failed);
        maybeFinishClose();
    }

    void toggleRecording() {
        if (closed) return;
        if (GameCaptureService.isBusy()) {
            GameCaptureService.requestStop();
            listener.onRecordingStatusChanged(activity.getString(R.string.capture_finishing));
            return;
        }
        if (consentPending || !ensureStore()) return;
        consentPending = true;
        if (Build.VERSION.SDK_INT < 29) {
            offerSilent(R.string.capture_audio_requires_android10);
        } else if (activity.checkSelfPermission(Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED) {
            screenConsent(true);
        } else {
            prompt = new AlertDialog.Builder(activity)
                    .setTitle(R.string.capture_record)
                    .setMessage(R.string.capture_audio_explanation)
                    .setPositiveButton(R.string.capture_allow_game_audio, (dialog, which) -> {
                        activity.requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO},
                                REQUEST_GAME_AUDIO);
                    })
                    .setNegativeButton(R.string.capture_video_only, (dialog, which) -> screenConsent(false))
                    .setOnCancelListener(dialog -> consentPending = false).show();
        }
    }

    boolean onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        if (requestCode != REQUEST_GAME_AUDIO) return false;
        if (closed || !consentPending) return true;
        if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) {
            screenConsent(true);
        } else offerSilent(R.string.capture_audio_denied);
        return true;
    }

    private void offerSilent(int explanation) {
        if (closed) return;
        prompt = new AlertDialog.Builder(activity)
                .setTitle(R.string.capture_video_only).setMessage(explanation)
                .setPositiveButton(R.string.capture_continue, (dialog, which) -> screenConsent(false))
                .setNegativeButton(R.string.capture_cancel, (dialog, which) -> consentPending = false)
                .setOnCancelListener(dialog -> consentPending = false).show();
    }

    private void screenConsent(boolean withAudio) {
        if (closed) return;
        requestAudio = withAudio;
        try {
            MediaProjectionManager manager = activity.getSystemService(MediaProjectionManager.class);

            Intent request = Build.VERSION.SDK_INT >= 34
                    ? manager.createScreenCaptureIntent(
                            MediaProjectionConfig.createConfigForDefaultDisplay())
                    : manager.createScreenCaptureIntent();
            activity.startActivityForResult(request, REQUEST_SCREEN_CAPTURE);
        } catch (RuntimeException error) {
            consentPending = false;
            toast(R.string.capture_recording_failed);
        }
    }

    boolean onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_SCREEN_CAPTURE) return false;
        if (closed) return true;
        if (!consentPending || resultCode != Activity.RESULT_OK || data == null) {
            consentPending = false;
            return true;
        }
        approvedConsent = data;
        startApprovedRecording();
        return true;
    }

    private void startApprovedRecording() {
        if (!foreground || runtimePaused || closed || approvedConsent == null) return;
        Intent consent = approvedConsent;
        approvedConsent = null;
        consentPending = false;
        Rect size;
        if (Build.VERSION.SDK_INT >= 30) {
            size = activity.getWindowManager().getMaximumWindowMetrics().getBounds();
        } else {
            android.util.DisplayMetrics metrics = new android.util.DisplayMetrics();
            activity.getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
            size = new Rect(0, 0, metrics.widthPixels, metrics.heightPixels);
        }
        GameCaptureService.start(activity, consent, Activity.RESULT_OK, size.width(), size.height(),
                activity.getResources().getConfiguration().densityDpi, requestAudio, gameName.get());
    }

    void onGameForegrounded() {
        foreground = true;
        startApprovedRecording();
    }

    void onGameBackgrounded() {
        foreground = false;
        GameCaptureService.requestStop();
    }


    void onRuntimePaused(boolean paused) {
        runtimePaused = paused;
        if (paused && !consentPending) GameCaptureService.requestStop();
        if (!paused) startApprovedRecording();
    }

    boolean isRecording() { return recording; }

    void showGallery() {
        if (closed || gallery != null || !ensureStore()) return;
        GameCaptureService.whenIdle(() -> {
            if (closed || gallery != null) return;
            listener.onGalleryVisibilityChanged(true);
            gallery = new CaptureGallery(activity, store, () -> {
                gallery = null;
                listener.onGalleryVisibilityChanged(false);
            });
            gallery.show();
        });
    }


    void closeWhenFinished(Runnable completion) {
        if (finishClose != null) return;
        finishClose = completion;
        close();
        GameCaptureService.whenIdle(() -> {
            recorderClosed = true;
            maybeFinishClose();
        });
        main.postDelayed(() -> {
            if (finishClose == null) return;

            Log.w("LSX4Capture", "Capture shutdown deadline reached; incomplete media stays private");
            finishNow();
        }, 10000);
    }

    @Override public void close() {
        if (closed) return;
        closed = true;
        approvedConsent = null;
        consentPending = false;
        main.removeCallbacks(tick);
        if (prompt != null) prompt.dismiss();
        if (gallery != null) gallery.close();
        GameCaptureService.requestStop();
        GameCaptureService.clearListener(serviceListener);
        if (!screenshotBusy) files.shutdown();
    }

    private void maybeFinishClose() {
        if (closed && !screenshotBusy) files.shutdown();
        if (finishClose != null && recorderClosed && !screenshotBusy) finishNow();
    }

    private void finishNow() {
        Runnable completion = finishClose;
        finishClose = null;
        if (completion != null) completion.run();
    }

    private boolean ensureStore() {
        if (store != null) return true;
        try {
            store = new CaptureStore(activity.getFilesDir());
            return true;
        } catch (IOException error) {
            toast(R.string.capture_storage_failed);
            return false;
        }
    }

    private void toast(int message) {
        Toast.makeText(activity, message, Toast.LENGTH_LONG).show();
    }
}
