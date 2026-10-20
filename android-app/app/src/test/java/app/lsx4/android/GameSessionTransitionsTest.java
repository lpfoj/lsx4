package app.lsx4.android;

import java.util.ArrayList;
import java.util.List;
import org.junit.Test;
import static org.junit.Assert.*;

public final class GameSessionTransitionsTest {
    static final class Fixture implements GameSessionTransitions.Host<String> {
        final List<String> events = new ArrayList<>();
        final GameSessionTransitions<String> state = new GameSessionTransitions<>(this);
        public void launch(String target, long token) { events.add("launch:" + target + ":" + token); }
        public void stop(long token) { events.add("stop:" + token); }
    }
    @Test public void nextGameWaitsForActualProcessDeath() {
        Fixture f = new Fixture();
        f.state.request("A", true);
        assertEquals(1, f.state.register(1));
        f.state.request("B", true);
        assertEquals(List.of("launch:A:1", "stop:1"), f.events);
        f.state.died(1);
        assertEquals(List.of("launch:A:1", "stop:1", "launch:B:2"), f.events);
    }
    @Test public void exitDoesNotPublishLibraryDuringMp4Finalization() {
        Fixture f = new Fixture();
        f.state.request("A", true); f.state.register(1);
        f.state.request("library", false);
        assertEquals(2, f.events.size());
        assertTrue(f.state.owns(1));
        f.state.died(1);
        assertEquals("launch:library:0", f.events.get(2));
        assertFalse(f.state.owns(1));
    }
    @Test public void rapidLaunchesDuringStartupWaitForRegistrationThenRetirement() {
        Fixture f = new Fixture();
        f.state.request("A", true); f.state.request("B", true); f.state.request("C", true);
        assertEquals(List.of("launch:A:1"), f.events);
        f.state.register(1);
        assertEquals(List.of("launch:A:1", "stop:1"), f.events);
        f.state.died(1);
        assertEquals("launch:C:2", f.events.get(2));
    }
    @Test public void staleDeathCannotClearOrRestartNextSession() {
        Fixture f = new Fixture();
        f.state.request("A", true); f.state.register(1); f.state.request("B", true); f.state.died(1);
        assertEquals(2, f.state.register(2));
        f.state.died(1); f.state.died(0);
        assertTrue(f.state.owns(2));
        assertEquals(3, f.events.size());
        assertEquals(0, f.state.register(1));
        assertEquals(0, f.state.register(2));
    }
    @Test public void processCrashDoesNotAutomaticallyRestartGuest() {
        Fixture f = new Fixture();
        assertEquals(1, f.state.register(0));
        f.state.died(1);
        assertEquals(0, f.state.activeToken());
        assertTrue(f.events.isEmpty());
    }
    @Test public void launcherReturnOnlyResumesAnEstablishedNonRetiringGuest() {
        Fixture f = new Fixture();
        assertFalse(f.state.canResume());
        f.state.request("A", true);
        assertFalse(f.state.canResume());
        f.state.register(1);
        assertTrue(f.state.canResume());

        assertEquals(1, f.state.activeToken());
        assertEquals(List.of("launch:A:1"), f.events);
        f.state.request("library", false);
        assertFalse(f.state.canResume());
        f.state.died(1);
        assertFalse(f.state.canResume());
    }
}
