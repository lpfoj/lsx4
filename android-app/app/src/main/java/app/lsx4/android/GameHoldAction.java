package app.lsx4.android;

enum GameHoldAction {
    TAP,
    SELECT,
    CLEAR_SELECTION,
    MARK_UNSUPPORTED,
    MARK_SUPPORTED;

    static final long SELECT_MS = 2_000L;
    static final long CLEAR_SELECTION_MS = 3_000L;
    static final long MARK_UNSUPPORTED_MS = 4_000L;
    static final long MARK_SUPPORTED_MS = 5_000L;

    static GameHoldAction fromDuration(long durationMs) {
        if (durationMs >= MARK_SUPPORTED_MS) {
            return MARK_SUPPORTED;
        }
        if (durationMs >= MARK_UNSUPPORTED_MS) {
            return MARK_UNSUPPORTED;
        }
        if (durationMs >= CLEAR_SELECTION_MS) {
            return CLEAR_SELECTION;
        }
        if (durationMs >= SELECT_MS) {
            return SELECT;
        }
        return TAP;
    }
}
