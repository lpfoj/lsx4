package app.lsx4.android;

import org.junit.Test;
import static org.junit.Assert.assertEquals;

public final class LoadingProgressStateTest {
    private final LoadingProgressState state = new LoadingProgressState();
    @Test public void unknownAtBootAndWithoutCounters() {
        assertEquals(LoadingProgressState.Status.WAITING, state.status());
        state.sample(null);
        assertEquals(LoadingProgressState.Status.WAITING, state.status());
    }
    @Test public void increasingBlocksAreEvidenceNotElapsedTime() {
        state.sample(new long[]{5});
        assertEquals(LoadingProgressState.Status.ACTIVE, state.status());
        state.sample(new long[]{5});
        assertEquals(LoadingProgressState.Status.WAITING, state.status());
        state.sample(new long[]{6});
        assertEquals(LoadingProgressState.Status.ACTIVE, state.status());
    }
    @Test public void NoTimeoutOrRepeatedPresentsCanInventFailureOrLoading() {
        for (int i=0;i<10000;i++) state.sample(new long[]{0,0,0,0,0,0,i,0,i,0,i});
        assertEquals(LoadingProgressState.Status.WAITING, state.status());
    }
    @Test public void terminalFailureIsStickyDespiteNewBlocksAndLateFrame() {
        state.event("PS5 launch result: -5");
        state.sample(new long[]{900});
        state.frameReady();
        assertEquals(LoadingProgressState.Status.ERROR, state.status());
    }
    @Test public void successAndRecoverableWarningsAreNotFatal() {
        state.event("PS5 launch result: 0");
        state.event("PS5 runtime status: PS5 runtime is not initialized");
        state.event("Audio probe tone failed: unavailable");
        state.event("warning: ErrorDeviceLost example, fallback succeeded");
        state.event("PS5 launch result: unknown");
        state.runtimeState("initialized");
        assertEquals(LoadingProgressState.Status.WAITING, state.status());
    }
    @Test public void ps4AndPs5TerminalEventsAreRecognized() {
        for (String event : new String[]{"PS5 runtime status: PS5 Android launch failed: translation.dispatch_failure",
                "Run embedded Box64 mapped entry completed result: -8",
                "AArch64 JIT launch failed: exception", "Scan game result: -1"}) {
            state.reset(); state.event(event);
            assertEquals(event, LoadingProgressState.Status.ERROR, state.status());
        }
    }
    @Test public void terminalRuntimeStatesAreRecognizedButUnknownIsNotFailure() {
        state.runtimeState("optional_module_missing");
        assertEquals(LoadingProgressState.Status.WAITING, state.status());
        for (String value : new String[]{"fex_mapped_entry_failed", "fex_bootstrap_lifecycle_failed", "box64_entry_request_failed"}) {
            state.reset(); state.runtimeState(value);
            assertEquals(LoadingProgressState.Status.ERROR, state.status());
        }
    }
    @Test public void afterFirstFrameNoStatusWorkChangesTheResult() {
        state.frameReady(); state.sample(new long[]{800}); state.event("PS5 launch result: -5");
        assertEquals(LoadingProgressState.Status.HIDDEN, state.status());
    }
    @Test public void resetAndCounterResetDoNotCarryOldErrorOrProgress() {
        state.fail(); state.reset(); state.sample(new long[]{100}); state.sample(new long[]{1});
        assertEquals(LoadingProgressState.Status.WAITING, state.status());
        state.sample(new long[]{2});
        assertEquals(LoadingProgressState.Status.ACTIVE, state.status());
    }
}
