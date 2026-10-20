package app.lsx4.android;

import java.io.File;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.UUID;


final class CaptureStore {
    private final File directory;

    CaptureStore(File filesDirectory) throws IOException {
        File privateRoot = filesDirectory.getCanonicalFile();
        directory = new File(privateRoot, "captures").getCanonicalFile();
        if (!privateRoot.equals(directory.getParentFile())) {
            throw new IOException("Capture directory escapes private storage");
        }
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("Cannot create capture directory");
        }
    }

    File createPending(String title, String extension) throws IOException {
        if (!"png".equals(extension) && !"mp4".equals(extension)) {
            throw new IllegalArgumentException("Unsupported capture type");
        }
        String stamp = new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.ROOT).format(new Date());
        String name = safeTitle(title) + "_" + stamp + "_" + UUID.randomUUID().toString()
                .substring(0, 8) + "." + extension + ".pending";
        File file = new File(directory, name);
        if (!file.createNewFile()) {
            throw new IOException("Capture already exists");
        }
        return file;
    }

    File publish(File pending) throws IOException {
        requireChild(pending);
        String name = pending.getName();
        if (!name.endsWith(".pending") || pending.length() == 0) {
            throw new IOException("Capture is incomplete");
        }
        File result = new File(directory, name.substring(0, name.length() - 8));
        if (!isSupported(result) || result.exists() || !pending.renameTo(result)) {
            throw new IOException("Cannot publish capture");
        }
        return result;
    }

    List<File> list() {
        File[] candidates = directory.listFiles();
        List<File> results = new ArrayList<>();
        if (candidates == null) return results;
        Arrays.sort(candidates, (a, b) -> {
            int byTime = Long.compare(b.lastModified(), a.lastModified());
            return byTime != 0 ? byTime : b.getName().compareTo(a.getName());
        });
        for (File file : candidates) {
            if (isCompleted(file)) results.add(file);
        }
        return results;
    }

    boolean isCompleted(File file) {
        try {
            requireChild(file);
            return file.isFile() && file.length() > 0 && isSupported(file);
        } catch (IOException error) {
            return false;
        }
    }

    void discard(File pending) {
        if (pending == null) return;
        try {
            requireChild(pending);
            if (pending.getName().endsWith(".pending")) pending.delete();
        } catch (IOException ignored) {

        }
    }

    static boolean isVideo(File file) {
        return file.getName().endsWith(".mp4");
    }

    static String safeTitle(String title) {
        StringBuilder out = new StringBuilder();
        if (title != null) {
            title.codePoints().limit(64).forEach(c -> {
                if (Character.isLetterOrDigit(c) || c == ' ' || c == '-' || c == '_') {
                    out.appendCodePoint(c);
                } else {
                    out.append('_');
                }
            });
        }
        String result = out.toString().trim();
        return result.isEmpty() ? "LSX4" : result;
    }

    private static boolean isSupported(File file) {
        return file.getName().endsWith(".png") || isVideo(file);
    }

    private void requireChild(File file) throws IOException {
        if (file == null || !directory.equals(file.getCanonicalFile().getParentFile())) {
            throw new IOException("Capture is outside the gallery");
        }
    }
}
