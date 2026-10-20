package app.lsx4.android;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class GameHoldActionTest {
    @Test
    public void durationBoundariesMatchLibraryGestureContract() {
        assertEquals(GameHoldAction.TAP, GameHoldAction.fromDuration(1_999));
        assertEquals(GameHoldAction.SELECT, GameHoldAction.fromDuration(2_000));
        assertEquals(GameHoldAction.SELECT, GameHoldAction.fromDuration(2_999));
        assertEquals(GameHoldAction.CLEAR_SELECTION,
                GameHoldAction.fromDuration(3_000));
        assertEquals(GameHoldAction.CLEAR_SELECTION,
                GameHoldAction.fromDuration(3_999));
        assertEquals(GameHoldAction.MARK_UNSUPPORTED,
                GameHoldAction.fromDuration(4_000));
        assertEquals(GameHoldAction.MARK_UNSUPPORTED,
                GameHoldAction.fromDuration(4_999));
        assertEquals(GameHoldAction.MARK_SUPPORTED,
                GameHoldAction.fromDuration(5_000));
    }
}
