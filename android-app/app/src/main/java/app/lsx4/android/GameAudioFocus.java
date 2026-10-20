package app.lsx4.android;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.os.Handler;
import android.os.Looper;


final class GameAudioFocus {
    interface Listener { void changed(boolean available); }
    private final AudioManager manager;
    private final AudioFocusRequest request;
    private final Listener listener;
    private boolean foreground;
    private boolean held;
    private boolean enabled;

    GameAudioFocus(Context context, Listener listener) {
        this.listener = listener;
        manager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        request = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setWillPauseWhenDucked(true)
                .setOnAudioFocusChangeListener(change -> {
                    if (!foreground || !enabled) return;
                    held = change == AudioManager.AUDIOFOCUS_GAIN;
                    listener.changed(held);
                }, new Handler(Looper.getMainLooper())).build();
    }

    boolean resume(boolean audioEnabled) {
        foreground = true;
        enabled = audioEnabled;
        if (!audioEnabled) {
            if (held) manager.abandonAudioFocusRequest(request);
            held = false;
            listener.changed(true);
            return true;
        }
        if (!held) held = manager.requestAudioFocus(request) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        listener.changed(held);
        return held;
    }

    void pause() {
        foreground = false;
        manager.abandonAudioFocusRequest(request);
        held = false;
    }
}
