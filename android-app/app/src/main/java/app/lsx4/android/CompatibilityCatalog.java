package app.lsx4.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.HashSet;
import java.util.Set;


final class CompatibilityCatalog {
    static final class Sample {
        final String video;
        final int second, fps;
        Sample(JSONObject json) throws Exception {
            video = json.getString("video"); second = json.getInt("second"); fps = json.getInt("fps");
            int duration = video.equals("CQpTIQlKarM") ? 4382 : video.equals("4UmC0u8DJmY") ? 4541 : -1;
            if (second < 0 || second >= duration || fps <= 0 || fps > 240)
                throw new IllegalArgumentException("Invalid video FPS sample");
        }
    }
    static final class Performance {
        final List<Sample> samples;
        final boolean smallIndie, aaa;
        Performance(JSONArray json, boolean smallIndie, boolean aaa) throws Exception {
            List<Sample> parsed = new ArrayList<>();
            if (json != null) for (int i = 0; i < json.length(); i++) parsed.add(new Sample(json.getJSONObject(i)));
            samples = Collections.unmodifiableList(parsed); this.smallIndie = smallIndie; this.aaa = aaa;
        }
        String range(int delta) {
            if (samples.isEmpty()) return "";
            int low = Integer.MAX_VALUE, high = 0;
            for (Sample sample : samples) { low = Math.min(low, sample.fps); high = Math.max(high, sample.fps); }
            low = Math.max(0, low + delta); high = Math.max(0, high + delta);
            return low == high ? Integer.toString(low) : low + "–" + high;
        }
    }
    static final class Entry {
        final String title, image, platform, notes;
        final Performance performance;
        Entry(JSONObject json, JSONObject samples, Set<String> indie, Set<String> aaa) throws Exception {
            title = json.getString("title"); image = json.getString("image");
            platform = json.getString("platform"); notes = json.optString("notes");
            if (!image.matches("assets/screenshots/[a-z0-9-]+\\.(png|jpg)"))
                throw new IllegalArgumentException("Invalid screenshot path");
            String file = image.substring(image.lastIndexOf('/') + 1);
            performance = new Performance(samples.optJSONArray(file), indie.contains(file), aaa.contains(file));
        }
    }
    final String revision, captured;
    final List<Entry> entries;
    CompatibilityCatalog(Context context) throws Exception {
        JSONObject snapshot;
        try (InputStream input = context.getResources().openRawResource(R.raw.compatibility_catalog)) {
            snapshot = new JSONObject(new String(readBounded(input, 256 * 1024), StandardCharsets.UTF_8));
        }
        revision = snapshot.getString("revision"); captured = snapshot.getString("captured");
        if (!revision.matches("[a-f0-9]{40}")) throw new IllegalArgumentException("Invalid revision");
        JSONArray games = snapshot.getJSONArray("games");
        JSONObject performance;
        try (InputStream input = context.getResources().openRawResource(R.raw.compatibility_performance)) {
            performance = new JSONObject(new String(readBounded(input, 256 * 1024), StandardCharsets.UTF_8));
        }
        JSONObject estimates = performance.getJSONObject("gen3_user_estimate");
        Set<String> indie = stringSet(estimates.getJSONArray("small_indie"));
        Set<String> aaa = stringSet(estimates.getJSONArray("aaa"));
        List<Entry> parsed = new ArrayList<>();
        for (int i = 0; i < games.length(); i++) parsed.add(new Entry(games.getJSONObject(i),
                performance.getJSONObject("samples"), indie, aaa));
        entries = Collections.unmodifiableList(parsed);
    }
    private static Set<String> stringSet(JSONArray array) throws Exception {
        Set<String> values = new HashSet<>();
        for (int i = 0; i < array.length(); i++) values.add(array.getString(i));
        return values;
    }
    File imageFile(Context context, Entry entry) {
        return new File(new File(context.getCacheDir(), "compatibility-menu/" + revision),
                entry.image.substring(entry.image.lastIndexOf('/') + 1));
    }

    Bitmap screenshot(Context context, Entry entry) throws Exception {
        File destination = imageFile(context, entry);
        if (!destination.isFile()) {
            if (!destination.getParentFile().isDirectory() && !destination.getParentFile().mkdirs())
                throw new IllegalStateException("Screenshot cache unavailable");
            HttpURLConnection connection = (HttpURLConnection) new URL(
                    "https://raw.githubusercontent.com/llayerlinux/lsx4/" + revision + "/" + entry.image)
                    .openConnection();
            connection.setConnectTimeout(6000); connection.setReadTimeout(8000);
            connection.setInstanceFollowRedirects(false);
            File temporary = File.createTempFile("screenshot-", ".part", destination.getParentFile());
            try {
                if (connection.getResponseCode() != 200) throw new IllegalStateException("Screenshot unavailable");
                byte[] bytes;
                try (InputStream input = connection.getInputStream()) { bytes = readBounded(input, 8 * 1024 * 1024); }
                BitmapFactory.Options bounds = new BitmapFactory.Options(); bounds.inJustDecodeBounds = true;
                BitmapFactory.decodeByteArray(bytes, 0, bytes.length, bounds);
                if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || bounds.outWidth > 16384 || bounds.outHeight > 16384)
                    throw new IllegalArgumentException("Invalid screenshot dimensions");
                try (FileOutputStream output = new FileOutputStream(temporary)) { output.write(bytes); }
                if (!temporary.renameTo(destination)) throw new IllegalStateException("Screenshot cache write failed");
            } finally { connection.disconnect(); temporary.delete(); }
        }
        BitmapFactory.Options bounds = new BitmapFactory.Options(); bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(destination.getPath(), bounds);
        BitmapFactory.Options options = new BitmapFactory.Options(); options.inSampleSize = 1;
        while (bounds.outWidth / options.inSampleSize > 1280 || bounds.outHeight / options.inSampleSize > 1280)
            options.inSampleSize *= 2;
        Bitmap bitmap = BitmapFactory.decodeFile(destination.getPath(), options);
        if (bitmap == null) throw new IllegalArgumentException("Screenshot decode failed");
        return bitmap;
    }
    private static byte[] readBounded(InputStream input, int limit) throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192]; int count;
        while ((count = input.read(buffer)) != -1) {
            if (output.size() + count > limit) throw new IllegalArgumentException("Response too large");
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }
}
