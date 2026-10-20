package app.lsx4.android;








final class RuntimePauseController {
    static final int REASON_ACTIVITY_BACKGROUND = 1 << 0;
    static final int REASON_ACTIVITY_STOPPED = 1 << 1;
    static final int REASON_WINDOW_FOCUS_LOST = 1 << 2;
    static final int REASON_GAME_MENU = 1 << 3;
    static final int REASON_TERMINATING = 1 << 4;
    static final int REASON_SURFACE_UNAVAILABLE = 1 << 5;
    static final int REASON_CAPTURE_GALLERY = 1 << 6;
    static final int REASON_AUDIO_FOCUS_LOST = 1 << 7;

    interface NativePauseBridge {
        int setPaused(boolean paused);
    }

    private final NativePauseBridge nativeBridge;
    private int reasonMask = REASON_ACTIVITY_BACKGROUND | REASON_WINDOW_FOCUS_LOST;
    private boolean runtimeReady;
    private boolean nativePaused;
    private boolean nativeStateKnown;
    private boolean terminating;
    private long pauseTransitions;
    private long resumeTransitions;
    private long rejectedTransitions;
    private int lastNativeResult;

    RuntimePauseController(NativePauseBridge nativeBridge) {
        if (nativeBridge == null) {
            throw new IllegalArgumentException("nativeBridge must not be null");
        }
        this.nativeBridge = nativeBridge;
    }

    synchronized void setRuntimeReady(boolean ready) {
        if (terminating && ready) {
            return;
        }
        if (runtimeReady == ready) {
            if (ready && !nativeStateKnown) applyNativeTransition(reasonMask != 0, true);
            return;
        }
        runtimeReady = ready;
        if (!ready) {
            nativeStateKnown = false;
            return;
        }
        applyNativeTransition(reasonMask != 0, true);
    }

    synchronized void setReason(int reason, boolean active) {
        if (reason == 0) {
            throw new IllegalArgumentException("pause reason must not be zero");
        }
        if (terminating && !active) {
            return;
        }
        final boolean wasPaused = reasonMask != 0;
        final int nextMask = active ? reasonMask | reason : reasonMask & ~reason;
        if (nextMask == reasonMask) {
            if (runtimeReady && !nativeStateKnown) applyNativeTransition(reasonMask != 0, true);
            return;
        }
        reasonMask = nextMask;
        final boolean shouldPause = reasonMask != 0;
        if (runtimeReady && (wasPaused != shouldPause || !nativeStateKnown)) {
            applyNativeTransition(shouldPause, false);
        }
    }

    synchronized void terminate() {
        if (terminating) {
            return;
        }
        setReason(REASON_TERMINATING, true);
        terminating = true;
    }

    synchronized int reasonMask() {
        return reasonMask;
    }

    synchronized boolean shouldBePaused() {
        return reasonMask != 0;
    }

    synchronized String status() {
        return "reasonMask=0x" + Integer.toHexString(reasonMask)
                + " runtimeReady=" + runtimeReady
                + " nativePaused=" + nativePaused
                + " nativeStateKnown=" + nativeStateKnown
                + " terminating=" + terminating
                + " pauseTransitions=" + pauseTransitions
                + " resumeTransitions=" + resumeTransitions
                + " rejectedTransitions=" + rejectedTransitions
                + " lastNativeResult=" + lastNativeResult;
    }

    private void applyNativeTransition(boolean paused, boolean force) {
        if (!force && nativeStateKnown && nativePaused == paused) {
            return;
        }
        lastNativeResult = nativeBridge.setPaused(paused);
        if (lastNativeResult != 0) {
            nativeStateKnown = false;
            rejectedTransitions++;
            return;
        }
        nativePaused = paused;
        nativeStateKnown = true;
        if (paused) {
            pauseTransitions++;
        } else {
            resumeTransitions++;
        }
    }
}
