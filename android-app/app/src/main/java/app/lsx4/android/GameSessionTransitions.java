package app.lsx4.android;


final class GameSessionTransitions<T> {
    interface Host<T> {
        void launch(T target, long token);
        void stop(long token);
    }
    private final Host<T> host;
    private long generation;
    private long active;
    private boolean registered;
    private boolean stopping;
    private T pending;
    private boolean pendingGame;

    GameSessionTransitions(Host<T> host) { this.host = host; }

    synchronized void request(T target, boolean game) {
        if (target == null) throw new IllegalArgumentException("target");
        if (active == 0) {
            dispatch(target, game);
            return;
        }
        pending = target;
        pendingGame = game;
        stopIfReady();
    }

    synchronized long register(long token) {

        if (token == 0 && active == 0) active = ++generation;
        else if (token == 0 || token != active || registered) return 0;
        registered = true;
        stopIfReady();
        return active;
    }

    synchronized void died(long token) {
        if (token == 0 || token != active) return;
        active = 0;
        registered = false;
        stopping = false;
        T next = pending;
        boolean game = pendingGame;
        pending = null;
        if (next != null) dispatch(next, game);
    }

    synchronized boolean owns(long token) { return token != 0 && token == active; }
    synchronized long activeToken() { return active; }
    synchronized boolean canResume() {
        return active != 0 && registered && !stopping && pending == null;
    }
    synchronized boolean waitingForRegistration(long token) {
        return owns(token) && !registered;
    }

    private void stopIfReady() {
        if (pending != null && registered && !stopping) {
            stopping = true;
            host.stop(active);
        }
    }

    private void dispatch(T target, boolean game) {
        active = game ? ++generation : 0;
        host.launch(target, active);
    }
}
