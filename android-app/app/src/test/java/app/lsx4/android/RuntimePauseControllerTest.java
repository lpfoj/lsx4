package app.lsx4.android;

import java.util.ArrayList;
import java.util.List;
import org.junit.Test;
import static org.junit.Assert.*;

public final class RuntimePauseControllerTest {
    private RuntimePauseController running(List<Boolean> calls) {
        RuntimePauseController c = new RuntimePauseController(paused -> { calls.add(paused); return 0; });
        c.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, false);
        c.setReason(RuntimePauseController.REASON_WINDOW_FOCUS_LOST, false);
        c.setRuntimeReady(true);
        assertEquals(List.of(false), calls);
        return c;
    }
    @Test public void foregroundAndSurfaceRestorationDoNotBypassContinue() {
        List<Boolean> calls = new ArrayList<>();
        RuntimePauseController c = running(calls);
        c.setReason(RuntimePauseController.REASON_GAME_MENU, true);
        c.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, true);
        c.setReason(RuntimePauseController.REASON_SURFACE_UNAVAILABLE, true);
        c.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, false);
        c.setReason(RuntimePauseController.REASON_SURFACE_UNAVAILABLE, false);
        assertEquals(List.of(false, true), calls);
        c.setReason(RuntimePauseController.REASON_GAME_MENU, false);
        assertEquals(List.of(false, true, false), calls);
    }
    @Test public void continueWaitsForValidSurfaceAndAudioFocus() {
        List<Boolean> calls = new ArrayList<>();
        RuntimePauseController c = running(calls);
        c.setReason(RuntimePauseController.REASON_GAME_MENU, true);
        c.setReason(RuntimePauseController.REASON_SURFACE_UNAVAILABLE, true);
        c.setReason(RuntimePauseController.REASON_AUDIO_FOCUS_LOST, true);
        c.setReason(RuntimePauseController.REASON_GAME_MENU, false);
        c.setReason(RuntimePauseController.REASON_SURFACE_UNAVAILABLE, false);
        assertEquals(List.of(false, true), calls);
        c.setReason(RuntimePauseController.REASON_AUDIO_FOCUS_LOST, false);
        assertEquals(List.of(false, true, false), calls);
    }
    @Test public void staleClosingAnimationCannotResumeReopenedMenu() {
        List<Boolean> calls = new ArrayList<>();
        RuntimePauseController c = running(calls);
        GameDrawerController drawer = new GameDrawerController(new GameDrawerController.Host() {
            public void setDrawerPauseReason(boolean paused, String source) {
                c.setReason(RuntimePauseController.REASON_GAME_MENU, paused);
            }
            public void beginOpen(int g, String source) { }
            public void beginClose(int g, String source) { }
        });
        drawer.requestOpen("tap"); drawer.completeOpen(drawer.generation());
        drawer.requestClose("continue"); int old = drawer.generation();
        drawer.requestOpen("background");
        assertFalse(drawer.completeClose(old, "stale-animation"));
        assertTrue(c.shouldBePaused());
        drawer.completeOpen(drawer.generation()); drawer.requestClose("continue");
        assertTrue(drawer.completeClose(drawer.generation(), "animation"));
        assertEquals(List.of(false, true, false), calls);
    }
    @Test public void failedTransitionIsRetriedAtNextLifecycleSignal() {
        List<Boolean> calls = new ArrayList<>();
        RuntimePauseController c = new RuntimePauseController(paused -> {
            calls.add(paused); return calls.size() == 1 ? -1 : 0;
        });
        c.setRuntimeReady(true);
        c.setReason(RuntimePauseController.REASON_ACTIVITY_BACKGROUND, true);
        assertEquals(List.of(true, true), calls);
    }
    @Test public void terminationCannotBeUndoneByFocusOrAnimationCallbacks() {
        List<Boolean> calls = new ArrayList<>();
        RuntimePauseController c = running(calls);
        c.terminate();
        c.setReason(RuntimePauseController.REASON_TERMINATING, false);
        c.setReason(RuntimePauseController.REASON_GAME_MENU, false);
        c.setRuntimeReady(false); c.setRuntimeReady(true);
        assertTrue(c.shouldBePaused());
        assertEquals(List.of(false, true), calls);
    }
}
