package app.lsx4.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.text.format.Formatter;
import android.util.LruCache;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.MediaController;
import android.widget.TextView;
import android.widget.VideoView;

import androidx.core.content.FileProvider;

import java.io.File;
import java.text.DateFormat;
import java.util.Date;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;


final class CaptureGallery implements AutoCloseable {
    private final Activity activity;
    private final CaptureStore store;
    private final Runnable onDismiss;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ThreadPoolExecutor worker = new ThreadPoolExecutor(1, 1, 0L,
            TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(16),
            runnable -> new Thread(runnable, "LSX4-capture-preview"),
            new ThreadPoolExecutor.DiscardPolicy());
    private final LruCache<String, Bitmap> cache = new LruCache<String, Bitmap>(8 * 1024 * 1024) {
        @Override protected int sizeOf(String key, Bitmap value) {
            return value.getAllocationByteCount();
        }
    };
    private AlertDialog dialog;
    private AlertDialog preview;
    private VideoView video;
    private boolean closed;

    CaptureGallery(Activity activity, CaptureStore store, Runnable onDismiss) {
        this.activity = activity;
        this.store = store;
        this.onDismiss = onDismiss;
    }

    void show() {
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        TextView loading = new TextView(activity);
        loading.setText(R.string.capture_gallery_loading);
        loading.setPadding(dp(20), dp(20), dp(20), dp(20));
        content.addView(loading);
        dialog = new AlertDialog.Builder(activity)
                .setTitle(R.string.capture_gallery)
                .setView(content)
                .setNegativeButton(R.string.capture_close, null)
                .create();
        dialog.setOnDismissListener(ignored -> close());
        dialog.show();
        worker.execute(() -> {
            List<File> files = store.list();
            main.post(() -> {
                if (closed) return;
                content.removeAllViews();
                if (files.isEmpty()) {
                    loading.setText(R.string.capture_gallery_empty);
                    content.addView(loading);
                } else {
                    ListView list = new ListView(activity);
                    list.setAdapter(new Items(files));
                    content.addView(list, new LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT, dp(280)));
                }
            });
        });
    }

    private final class Items extends BaseAdapter {
        private final List<File> files;
        Items(List<File> files) { this.files = files; }
        @Override public int getCount() { return files.size(); }
        @Override public File getItem(int position) { return files.get(position); }
        @Override public long getItemId(int position) { return position; }
        @Override public View getView(int position, View recycled, ViewGroup parent) {
            File file = getItem(position);
            LinearLayout row = new LinearLayout(activity);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(12), dp(6), dp(4), dp(6));
            ImageView thumbnail = new ImageView(activity);
            thumbnail.setScaleType(ImageView.ScaleType.CENTER_CROP);
            thumbnail.setImageResource(CaptureStore.isVideo(file)
                    ? R.drawable.capture_video : R.drawable.capture_screenshot);
            row.addView(thumbnail, new LinearLayout.LayoutParams(dp(88), dp(58)));
            LinearLayout labels = new LinearLayout(activity);
            labels.setOrientation(LinearLayout.VERTICAL);
            labels.setPadding(dp(12), 0, dp(4), 0);
            TextView name = new TextView(activity);
            name.setText(file.getName().replaceFirst("_\\d{4}-\\d{2}-\\d{2}_.*$", ""));
            name.setMaxLines(2);
            name.setTextSize(15);
            labels.addView(name);
            TextView detail = new TextView(activity);
            detail.setText((CaptureStore.isVideo(file)
                    ? activity.getString(R.string.capture_video_label)
                    : activity.getString(R.string.capture_image_label)) + " · "
                    + DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
                            .format(new Date(file.lastModified()))
                    + " · " + Formatter.formatShortFileSize(activity, file.length()));
            detail.setTextSize(11);
            detail.setTextColor(0xffaaaaaa);
            labels.addView(detail);
            row.addView(labels, new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            ImageButton share = new ImageButton(activity);
            share.setImageResource(R.drawable.capture_share);
            share.setBackgroundColor(0x00000000);
            share.setContentDescription(activity.getString(R.string.capture_share));
            share.setOnClickListener(view -> share(file));
            row.addView(share, new LinearLayout.LayoutParams(dp(48), dp(48)));
            row.setOnClickListener(view -> preview(file));
            Bitmap cached = cache.get(file.getPath());
            if (cached != null) {
                thumbnail.setImageBitmap(cached);
            } else {
                worker.execute(() -> {
                    Bitmap image = loadPreview(file, 256);
                    if (image == null) return;
                    main.post(() -> {
                        if (closed) { image.recycle(); return; }
                        cache.put(file.getPath(), image);
                        thumbnail.setImageBitmap(image);
                    });
                });
            }
            return row;
        }
    }

    private void preview(File file) {
        if (!store.isCompleted(file) || closed) return;
        if (preview != null) preview.dismiss();
        LinearLayout content = new LinearLayout(activity);
        content.setGravity(Gravity.CENTER);
        content.setBackgroundColor(0xff000000);
        if (CaptureStore.isVideo(file)) {
            video = new VideoView(activity);
            content.addView(video, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(260)));
            MediaController controls = new MediaController(activity);
            controls.setAnchorView(video);
            video.setMediaController(controls);
            video.setVideoURI(uri(file));
            video.setOnPreparedListener(player -> {
                if (video != null) video.start();
            });
        } else {
            ImageView image = new ImageView(activity);
            image.setScaleType(ImageView.ScaleType.FIT_CENTER);
            content.addView(image, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(260)));
            worker.execute(() -> {
                Bitmap decoded = loadPreview(file, 1600);
                main.post(() -> {
                    if (closed || !image.isAttachedToWindow()) {
                        if (decoded != null) decoded.recycle();
                    } else image.setImageBitmap(decoded);
                });
            });
        }
        preview = new AlertDialog.Builder(activity)
                .setView(content)
                .setPositiveButton(R.string.capture_share, (ignored, which) -> share(file))
                .setNegativeButton(R.string.capture_close, null).create();
        preview.setOnDismissListener(ignored -> stopPreview());
        preview.show();
    }

    private void share(File file) {
        if (!store.isCompleted(file)) return;
        stopPreview();
        Uri uri = uri(file);
        Intent intent = new Intent(Intent.ACTION_SEND);
        intent.setType(CaptureStore.isVideo(file) ? "video/mp4" : "image/png");
        intent.putExtra(Intent.EXTRA_STREAM, uri);
        intent.setClipData(ClipData.newRawUri(file.getName(), uri));
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            activity.startActivity(Intent.createChooser(intent,
                    activity.getString(R.string.capture_share)));
        } catch (android.content.ActivityNotFoundException error) {
            android.widget.Toast.makeText(activity, R.string.capture_share_unavailable,
                    android.widget.Toast.LENGTH_LONG).show();
        }
    }

    private Uri uri(File file) {
        return FileProvider.getUriForFile(activity,
                activity.getPackageName() + ".captures", file);
    }

    private static Bitmap loadPreview(File file, int size) {
        try {
            if (CaptureStore.isVideo(file)) {
                MediaMetadataRetriever reader = new MediaMetadataRetriever();
                try {
                    reader.setDataSource(file.getPath());
                    if (android.os.Build.VERSION.SDK_INT >= 27) {
                        return reader.getScaledFrameAtTime(0,
                                MediaMetadataRetriever.OPTION_CLOSEST_SYNC, size, size);
                    }
                    Bitmap frame = reader.getFrameAtTime(0,
                            MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
                    if (frame == null || Math.max(frame.getWidth(), frame.getHeight()) <= size) {
                        return frame;
                    }
                    float ratio = (float) size / Math.max(frame.getWidth(), frame.getHeight());
                    Bitmap small = Bitmap.createScaledBitmap(frame,
                            Math.max(1, Math.round(frame.getWidth() * ratio)),
                            Math.max(1, Math.round(frame.getHeight() * ratio)), true);
                    if (small != frame) frame.recycle();
                    return small;
                } finally {
                    reader.release();
                }
            }
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(file.getPath(), options);
            options.inSampleSize = 1;
            while (Math.max(options.outWidth, options.outHeight) / options.inSampleSize > size) {
                options.inSampleSize *= 2;
            }
            options.inJustDecodeBounds = false;
            return BitmapFactory.decodeFile(file.getPath(), options);
        } catch (Exception | OutOfMemoryError error) {
            return null;
        }
    }

    private void stopPreview() {
        if (video != null) {
            video.stopPlayback();
            video = null;
        }
    }

    @Override public void close() {
        if (closed) return;
        closed = true;
        stopPreview();
        if (preview != null) preview.dismiss();
        if (dialog != null && dialog.isShowing()) dialog.dismiss();
        worker.shutdownNow();
        cache.evictAll();
        onDismiss.run();
    }

    private int dp(int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
