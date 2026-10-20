package app.lsx4.android;

import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.media.projection.MediaProjection;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;


final class CaptureRecorder {
    interface Listener {
        void started(boolean withAudio);
        void finished(File completed, String error);
    }

    private static final int SAMPLE_RATE = 48000;
    private static final int FRAME_BYTES = 4;
    private static final int MAX_PENDING_BYTES = 16 * 1024 * 1024;
    private final CaptureStore store;
    private final MediaProjection projection;
    private final File pending;
    private final int width;
    private final int height;
    private final int density;
    private final boolean requestAudio;
    private final Listener listener;
    private final HandlerThread thread = new HandlerThread("LSX4-capture-encode");
    private Handler worker;
    private MediaCodec videoCodec;
    private MediaCodec audioCodec;
    private AudioRecord audioRecord;
    private MediaMuxer muxer;
    private Surface videoSurface;
    private VirtualDisplay display;
    private int videoTrack = -1;
    private int audioTrack = -1;
    private int audioInput = -1;
    private boolean muxStarted;
    private boolean videoEos;
    private boolean audioEos;
    private boolean audioInputEos;
    private boolean finishing;
    private boolean finished;
    private volatile boolean stopRequested;
    private long drainDeadline;
    private long epochUs;
    private long audioEpochUs;
    private long audioFrames;
    private long lastVideoUs = -1;
    private long lastAudioUs = -1;
    private int videoSamples;
    private int pendingBytes;
    private final List<Sample> waiting = new ArrayList<>();

    CaptureRecorder(CaptureStore store, MediaProjection projection, String title,
            int width, int height, int density, boolean requestAudio, Listener listener)
            throws IOException {
        this.store = store;
        this.projection = projection;
        this.width = width & ~1;
        this.height = height & ~1;
        this.density = density;
        this.requestAudio = requestAudio && Build.VERSION.SDK_INT >= 29;
        this.listener = listener;
        pending = store.createPending(title, "mp4");
    }

    void start() {
        thread.start();
        worker = new Handler(thread.getLooper());
        worker.post(() -> {
            try {
                if (stopRequested) { finish(null); return; }
                prepare();
                listener.started(audioCodec != null);
                pump();
            } catch (Exception | OutOfMemoryError error) {
                finish(error);
            }
        });
    }

    void stop() { stopRequested = true; }

    private void prepare() throws IOException {
        if (width < 2 || height < 2) throw new IOException("Invalid capture dimensions");
        MediaFormat video = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC,
                width, height);
        video.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
        video.setInteger(MediaFormat.KEY_BIT_RATE,
                (int) Math.max(8_000_000L, Math.min(40_000_000L, (long) width * height * 4)));
        video.setInteger(MediaFormat.KEY_FRAME_RATE, 30);
        video.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2);
        videoCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
        videoCodec.configure(video, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        videoSurface = videoCodec.createInputSurface();
        muxer = new MediaMuxer(pending.getPath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
        if (requestAudio) prepareAudio();
        videoCodec.start();
        epochUs = System.nanoTime() / 1000;
        if (audioCodec != null) {
            audioCodec.start();
            audioRecord.startRecording();
            if (audioRecord.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                throw new IOException("Playback audio did not start");
            }
            audioEpochUs = System.nanoTime() / 1000 - epochUs;
        }
        display = projection.createVirtualDisplay("LSX4 gameplay", width, height, density,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, videoSurface, null, worker);
        if (display == null) throw new IOException("Capture display unavailable");
    }

    private void prepareAudio() {
        try {
            AudioPlaybackCaptureConfiguration capture = new AudioPlaybackCaptureConfiguration.Builder(
                    projection).addMatchingUid(Process.myUid())
                    .addMatchingUsage(AudioAttributes.USAGE_GAME)
                    .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                    .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN).build();
            AudioFormat format = new AudioFormat.Builder().setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build();
            int minimum = AudioRecord.getMinBufferSize(SAMPLE_RATE,
                    AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_16BIT);
            if (minimum <= 0) throw new IOException("Unsupported playback audio format");
            audioRecord = new AudioRecord.Builder().setAudioFormat(format)
                    .setBufferSizeInBytes(Math.max(minimum * 2, SAMPLE_RATE * FRAME_BYTES / 5))
                    .setAudioPlaybackCaptureConfig(capture).build();
            if (audioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
                throw new IOException("Playback audio unavailable");
            }
            MediaFormat aac = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC,
                    SAMPLE_RATE, 2);
            aac.setInteger(MediaFormat.KEY_AAC_PROFILE,
                    MediaCodecInfo.CodecProfileLevel.AACObjectLC);
            aac.setInteger(MediaFormat.KEY_BIT_RATE, 192000);
            aac.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384);
            audioCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC);
            audioCodec.configure(aac, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
        } catch (Exception error) {
            Log.w("LSX4Capture", "Playback audio unavailable; recording silent video", error);
            releaseAudio();
        }
    }

    private void pump() {
        if (finished) return;
        try {
            if (stopRequested && !finishing) {
                finishing = true;
                drainDeadline = SystemClock.elapsedRealtime() + 3000;
                if (display != null) { display.release(); display = null; }
                videoCodec.signalEndOfInputStream();
            }
            feedAudio();
            drain(videoCodec, false);
            if (audioCodec != null) drain(audioCodec, true);
            if (finishing && ((videoEos && (audioCodec == null || audioEos))
                    || SystemClock.elapsedRealtime() >= drainDeadline)) {
                finish(null);
            } else {
                worker.postDelayed(this::pump, 8);
            }
        } catch (Exception | OutOfMemoryError error) {
            finish(error);
        }
    }

    private void feedAudio() throws IOException {
        if (audioCodec == null || audioInputEos) return;

        for (int n = 0; n < 8; n++) {
            if (audioInput < 0) audioInput = audioCodec.dequeueInputBuffer(0);
            if (audioInput < 0) return;
            ByteBuffer input = audioCodec.getInputBuffer(audioInput);
            if (input == null) throw new IOException("Audio encoder input unavailable");
            long time = audioEpochUs + audioFrames * 1_000_000L / SAMPLE_RATE;
            if (finishing && SystemClock.elapsedRealtime() >= drainDeadline) {
                endAudioInput(time);
                return;
            }
            input.clear();
            int capacity = Math.min(input.remaining() / FRAME_BYTES * FRAME_BYTES, 8192);
            int bytes = audioRecord.read(input, capacity, AudioRecord.READ_NON_BLOCKING);
            if (bytes <= 0 && finishing) {


                endAudioInput(time);
                return;
            }
            if (bytes == 0) return;
            if (bytes < 0 || bytes % FRAME_BYTES != 0) {
                throw new IOException("Playback audio read failed: " + bytes);
            }
            audioCodec.queueInputBuffer(audioInput, 0, bytes, time, 0);
            audioFrames += bytes / FRAME_BYTES;
            audioInput = -1;
        }
    }

    private void endAudioInput(long time) {
        try { audioRecord.stop(); } catch (IllegalStateException ignored) { }
        audioCodec.queueInputBuffer(audioInput, 0, 0, time, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
        audioInput = -1;
        audioInputEos = true;
    }

    private void drain(MediaCodec codec, boolean audio) throws IOException {
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        for (int count = 0; count < 64; count++) {
            int index = codec.dequeueOutputBuffer(info, 0);
            if (index == MediaCodec.INFO_TRY_AGAIN_LATER) return;
            if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                if (muxStarted || (audio ? audioTrack : videoTrack) >= 0) {
                    throw new IOException("Capture format changed during recording");
                }
                int track = muxer.addTrack(codec.getOutputFormat());
                if (audio) audioTrack = track; else videoTrack = track;
                startMuxerIfReady();
            } else if (index >= 0) {
                try {
                    if ((info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0) info.size = 0;
                    if (info.size > 0) {
                        ByteBuffer data = codec.getOutputBuffer(index);
                        if (data == null) throw new IOException("Encoder output unavailable");
                        data.position(info.offset);
                        data.limit(info.offset + info.size);
                        long pts = Math.max(0, info.presentationTimeUs - (audio ? 0 : epochUs));
                        if (audio) {
                            pts = Math.max(pts, lastAudioUs + 1);
                            lastAudioUs = pts;
                        } else {
                            pts = Math.max(pts, lastVideoUs + 1);
                            lastVideoUs = pts;
                            videoSamples++;
                        }
                        MediaCodec.BufferInfo sampleInfo = new MediaCodec.BufferInfo();
                        sampleInfo.set(0, info.size, pts,
                                info.flags & ~MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                        int track = audio ? audioTrack : videoTrack;
                        if (muxStarted) {
                            muxer.writeSampleData(track, data.slice(), sampleInfo);
                        } else {
                            if (pendingBytes + info.size > MAX_PENDING_BYTES) {
                                throw new IOException("Capture encoder did not publish both formats");
                            }
                            byte[] bytes = new byte[info.size];
                            data.get(bytes);
                            waiting.add(new Sample(track, bytes, sampleInfo));
                            pendingBytes += bytes.length;
                        }
                    }
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                        if (audio) audioEos = true; else videoEos = true;
                    }
                } finally {
                    codec.releaseOutputBuffer(index, false);
                }
            }
        }
    }

    private void startMuxerIfReady() {
        if (muxStarted || videoTrack < 0 || (audioCodec != null && audioTrack < 0)) return;
        muxer.start();
        muxStarted = true;
        for (Sample sample : waiting) {
            muxer.writeSampleData(sample.track, ByteBuffer.wrap(sample.bytes), sample.info);
        }
        waiting.clear();
        pendingBytes = 0;
    }

    private void finish(Throwable failure) {
        if (finished) return;
        finished = true;
        File completed = null;
        try {
            if (display != null) display.release();
        } catch (Exception error) { if (failure == null) failure = error; }
        display = null;
        if (videoCodec != null) {
            try { videoCodec.stop(); } catch (Exception ignored) { }
            try { videoCodec.release(); } catch (Exception ignored) { }
            videoCodec = null;
        }
        releaseAudio();
        if (videoSurface != null) { videoSurface.release(); videoSurface = null; }
        boolean valid = muxStarted && videoSamples > 0;
        if (muxer != null) {
            try {
                if (muxStarted) muxer.stop();
            } catch (Exception error) {
                valid = false;
                if (failure == null) failure = error;
            } finally {
                try { muxer.release(); } catch (Exception ignored) { }
                muxer = null;
            }
        }
        if (valid) {
            try { completed = store.publish(pending); }
            catch (IOException error) { if (failure == null) failure = error; }
        }
        if (completed == null) store.discard(pending);
        waiting.clear();
        if (failure != null) Log.e("LSX4Capture", "Capture ended with an error", failure);
        listener.finished(completed, failure == null ? null : failure.getClass().getSimpleName());
        thread.quitSafely();
    }

    private void releaseAudio() {
        if (audioRecord != null) {
            try { audioRecord.stop(); } catch (Exception ignored) { }
            try { audioRecord.release(); } catch (Exception ignored) { }
            audioRecord = null;
        }
        if (audioCodec != null) {
            try { audioCodec.stop(); } catch (Exception ignored) { }
            try { audioCodec.release(); } catch (Exception ignored) { }
            audioCodec = null;
        }
    }

    private static final class Sample {
        final int track;
        final byte[] bytes;
        final MediaCodec.BufferInfo info;
        Sample(int track, byte[] bytes, MediaCodec.BufferInfo info) {
            this.track = track;
            this.bytes = bytes;
            this.info = info;
        }
    }
}
