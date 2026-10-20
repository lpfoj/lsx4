package app.lsx4.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.SoundPool;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;






final class MenuSoundEngine {
    static final String KEY_EFFECTS = "home_menu_sound_effects_v1";
    static final String KEY_MUSIC = "home_menu_music_v1";

    private static final String TAG = "LSX4.MenuSound";
    private static final int RATE = 44100;
    private static final float TICK_VOLUME = 0.3f;
    private static final float CUE_VOLUME = 0.45f;
    private static final float MUSIC_VOLUME = 0.5f;
    private static final long MIN_TICK_INTERVAL_MS = 34L;

    private final Context context;
    private final SharedPreferences preferences;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Object lock = new Object();
    private SoundPool pool;
    private int tickId;
    private int confirmId;
    private int denyId;
    private int loadedMask;
    private MediaPlayer ambient;
    private boolean ambientWanted;
    private boolean ambientPlaying;
    private float ambientGain;
    private long lastTickAt;
    private volatile boolean released;

    MenuSoundEngine(Context context, SharedPreferences preferences) {
        this.context = context.getApplicationContext();
        this.preferences = preferences;
        Thread worker = new Thread(this::prepare, "lsx4-menu-sound");
        worker.setDaemon(true);
        worker.start();
    }

    boolean effectsEnabled() {
        return preferences.getBoolean(KEY_EFFECTS, true);
    }

    boolean musicEnabled() {
        return preferences.getBoolean(KEY_MUSIC, true);
    }

    void setEffectsEnabled(boolean enabled) {
        preferences.edit().putBoolean(KEY_EFFECTS, enabled).apply();
    }

    void setMusicEnabled(boolean enabled) {
        preferences.edit().putBoolean(KEY_MUSIC, enabled).apply();
        setAmbientActive(ambientWanted);
    }

    void tick() {
        long now = SystemClock.uptimeMillis();
        if (now - lastTickAt < MIN_TICK_INTERVAL_MS) {
            return;
        }
        lastTickAt = now;
        play(tickId, 1, TICK_VOLUME);
    }


    void softTick() {
        play(tickId, 1, TICK_VOLUME * 0.66f);
    }

    void confirm() {
        play(confirmId, 2, CUE_VOLUME);
    }

    void deny() {
        play(denyId, 4, CUE_VOLUME);
    }


    void setAmbientActive(boolean active) {
        synchronized (lock) {
            ambientWanted = active;
            if (released || ambient == null) {
                return;
            }
            boolean play = active && musicEnabled();
            if (play == ambientPlaying) {
                return;
            }
            ambientPlaying = play;
            try {
                if (play) {
                    ambientGain = 0f;
                    ambient.setVolume(0f, 0f);
                    ambient.start();
                    main.removeCallbacks(fadeIn);
                    main.post(fadeIn);
                } else {
                    main.removeCallbacks(fadeIn);
                    ambient.pause();
                }
            } catch (IllegalStateException error) {
                Log.w(TAG, "Ambient state change failed", error);
            }
        }
    }

    void release() {
        synchronized (lock) {
            released = true;
            main.removeCallbacks(fadeIn);
            if (ambient != null) {
                try {
                    ambient.stop();
                } catch (IllegalStateException ignored) {
                }
                ambient.release();
                ambient = null;
            }
            if (pool != null) {
                pool.release();
                pool = null;
            }
        }
    }

    private final Runnable fadeIn = new Runnable() {
        @Override
        public void run() {
            synchronized (lock) {
                if (released || ambient == null || !ambientPlaying) {
                    return;
                }
                ambientGain = Math.min(1f, ambientGain + 0.05f);
                float volume = MUSIC_VOLUME * ambientGain;
                ambient.setVolume(volume, volume);
                if (ambientGain < 1f) {
                    main.postDelayed(this, 60L);
                }
            }
        }
    };

    private void play(int soundId, int bit, float volume) {
        if (!effectsEnabled()) {
            return;
        }
        synchronized (lock) {
            if (released || pool == null || (loadedMask & bit) == 0) {
                return;
            }
            pool.play(soundId, volume, volume, 1, 0, 1f);
        }
    }

    private void prepare() {
        try {
            File directory = new File(context.getCacheDir(), "menu-sounds");
            if (!directory.isDirectory() && !directory.mkdirs()) {
                throw new IOException("cannot create " + directory);
            }
            File confirm = writeWav(new File(directory, "confirm-v1.wav"), synthConfirm());
            File deny = writeWav(new File(directory, "deny-v1.wav"), synthDeny());
            SoundPool soundPool = new SoundPool.Builder()
                    .setMaxStreams(4)
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build())
                    .build();
            synchronized (lock) {
                if (released) {
                    soundPool.release();
                    return;
                }
                pool = soundPool;
                pool.setOnLoadCompleteListener((loadedPool, sampleId, status) -> {
                    if (status != 0) {
                        return;
                    }
                    synchronized (lock) {
                        if (sampleId == tickId) loadedMask |= 1;
                        if (sampleId == confirmId) loadedMask |= 2;
                        if (sampleId == denyId) loadedMask |= 4;
                    }
                });
                tickId = pool.load(context, R.raw.menu_select, 1);
                confirmId = pool.load(confirm.getAbsolutePath(), 1);
                denyId = pool.load(deny.getAbsolutePath(), 1);
            }
        } catch (Throwable error) {
            Log.w(TAG, "Menu effects unavailable", error);
        }
        try {
            MediaPlayer player = MediaPlayer.create(context, R.raw.menu_ambient);
            if (player == null) {
                return;
            }
            player.setLooping(true);
            synchronized (lock) {
                if (released) {
                    player.release();
                    return;
                }
                ambient = player;
            }
            main.post(() -> setAmbientActive(ambientWanted));
        } catch (Throwable error) {
            Log.w(TAG, "Menu music unavailable", error);
        }
    }


    private static float[] synthConfirm() {
        int frames = (int) (RATE * 0.42f);
        float[] out = new float[frames];
        double[] notes = {1318.51, 1975.53};
        double[] starts = {0.0, 0.065};
        for (int n = 0; n < notes.length; n++) {
            int start = (int) (starts[n] * RATE);
            for (int i = start; i < frames; i++) {
                double t = (double) (i - start) / RATE;
                double attack = 1.0 - Math.exp(-t / 0.002);
                double envelope = attack * Math.exp(-t / 0.13);
                double angle = 2 * Math.PI * notes[n] * t;
                out[i] += (float) (0.32 * envelope * (Math.sin(angle) + 0.28 * Math.sin(2 * angle)
                        + 0.08 * Math.sin(3 * angle)));
            }
        }
        return out;
    }


    private static float[] synthDeny() {
        int frames = (int) (RATE * 0.16f);
        float[] out = new float[frames];
        for (int i = 0; i < frames; i++) {
            double t = (double) i / RATE;
            double attack = 1.0 - Math.exp(-t / 0.002);
            double envelope = attack * Math.exp(-t / 0.045);
            double angle = 2 * Math.PI * (330.0 - 60.0 * t / 0.16) * t;
            out[i] = (float) (0.45 * envelope * (Math.sin(angle) + 0.35 * Math.sin(2 * angle)));
        }
        return out;
    }

    private static File writeWav(File target, float[] samples) throws IOException {
        int dataBytes = samples.length * 2;
        if (target.isFile() && target.length() == 44L + dataBytes) {
            return target;
        }
        ByteBuffer buffer = ByteBuffer.allocate(44 + dataBytes).order(ByteOrder.LITTLE_ENDIAN);
        buffer.put(new byte[]{'R', 'I', 'F', 'F'}).putInt(36 + dataBytes)
                .put(new byte[]{'W', 'A', 'V', 'E', 'f', 'm', 't', ' '})
                .putInt(16).putShort((short) 1).putShort((short) 1)
                .putInt(RATE).putInt(RATE * 2).putShort((short) 2).putShort((short) 16)
                .put(new byte[]{'d', 'a', 't', 'a'}).putInt(dataBytes);
        for (float sample : samples) {
            float clamped = Math.max(-1f, Math.min(1f, sample));
            buffer.putShort((short) Math.round(clamped * Short.MAX_VALUE));
        }
        File temporary = new File(target.getPath() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temporary)) {
            output.write(buffer.array());
        }
        if (!temporary.renameTo(target)) {
            throw new IOException("cannot publish " + target);
        }
        return target;
    }
}
