package app.lsx4.android;









final class GameDrawerController {
    enum State {
        CLOSED,
        OPENING,
        OPEN,
        CLOSING
    }

    interface Host {
        void setDrawerPauseReason(boolean active, String source);

        void beginOpen(int generation, String source);

        void beginClose(int generation, String source);
    }

    private final Host host;
    private State state = State.CLOSED;
    private int generation;

    GameDrawerController(Host host) {
        if (host == null) {
            throw new IllegalArgumentException("host must not be null");
        }
        this.host = host;
    }

    void toggle(String source) {
        if (state == State.CLOSED || state == State.CLOSING) {
            requestOpen(source);
        } else {
            requestClose(source);
        }
    }

    void requestOpen(String source) {
        if (state == State.OPEN || state == State.OPENING) {
            return;
        }
        final boolean acquirePause = state == State.CLOSED;
        state = State.OPENING;
        final int lease = ++generation;
        if (acquirePause) {
            host.setDrawerPauseReason(true, source);
        }
        host.beginOpen(lease, source);
    }

    void requestClose(String source) {
        if (state == State.CLOSED || state == State.CLOSING) {
            return;
        }
        state = State.CLOSING;
        host.beginClose(++generation, source);
    }

    boolean completeOpen(int lease) {
        if (lease != generation || state != State.OPENING) {
            return false;
        }
        state = State.OPEN;
        return true;
    }

    boolean completeClose(int lease, String source) {
        if (lease != generation || state != State.CLOSING) {
            return false;
        }
        state = State.CLOSED;
        host.setDrawerPauseReason(false, source);
        return true;
    }

    boolean ownsClosingLease(int lease) {
        return lease == generation && state == State.CLOSING;
    }

    State state() {
        return state;
    }

    int generation() {
        return generation;
    }

    boolean requiresPause() {
        return state != State.CLOSED;
    }
}
