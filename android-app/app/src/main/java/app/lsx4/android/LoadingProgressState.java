package app.lsx4.android;


final class LoadingProgressState {
    enum Status { WAITING, ACTIVE, ERROR, HIDDEN }
    private Status status = Status.WAITING;
    private long lastBlocks;

    void reset() {
        status = Status.WAITING;
        lastBlocks = 0;
    }

    Status status() { return status; }

    void sample(long[] counters) {
        if (status == Status.ERROR || status == Status.HIDDEN) return;
        if (counters == null || counters.length == 0) {
            status = Status.WAITING;
            return;
        }
        long blocks = Math.max(0, counters[0]);

        status = blocks > lastBlocks ? Status.ACTIVE : Status.WAITING;
        lastBlocks = blocks;
    }

    void fail() {
        if (status != Status.HIDDEN) status = Status.ERROR;
    }

    void frameReady() {
        if (status != Status.ERROR) status = Status.HIDDEN;
    }


    void event(String line) {
        if (status == Status.HIDDEN || status == Status.ERROR || line == null) return;
        String[] results = { "PS5 launch result: ", "AArch64 JIT launch result: ",
                "Run embedded Box64 mapped entry completed result: ", "Scan game result: " };
        for (String prefix : results) {
            if (line.startsWith(prefix)) {
                try { if (Integer.parseInt(line.substring(prefix.length()).trim()) < 0) fail(); }
                catch (NumberFormatException ignored) {   }
                return;
            }
        }
        if (line.startsWith("AArch64 JIT launch failed: ")
                || line.startsWith("Run embedded Box64 mapped entry failed: ")
                || line.startsWith("Scan game failed: ")
                || line.startsWith("Initialize failed: ")
                || line.startsWith("PS5 runtime status after launch: PS5 Android launch failed:")
                || line.startsWith("PS5 runtime status: PS5 Android launch failed:")) fail();
    }

    void runtimeState(String state) {
        if ("fex_mapped_entry_failed".equals(state)
                || "fex_bootstrap_lifecycle_failed".equals(state)
                || "box64_entry_request_failed".equals(state)) fail();
    }
}
