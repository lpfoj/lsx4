package app.lsx4.android;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

import org.junit.Test;

public final class GameLibraryViewTest {
    private static final List<String> MANUAL = Collections.unmodifiableList(
            Arrays.asList("Unknown", "Playable B", "Unplayable B", "Playable A", "Unplayable A"));

    private static GameCompatibilityStatus status(String title) {
        if (title.startsWith("Playable")) return GameCompatibilityStatus.SUPPORTED;
        if (title.startsWith("Unplayable")) return GameCompatibilityStatus.UNSUPPORTED;
        return GameCompatibilityStatus.UNCLASSIFIED;
    }

    private static List<String> view(int mode, boolean reverse, String query) {
        return GameLibraryView.visible(MANUAL, mode, reverse, query,
                title -> title, GameLibraryViewTest::status);
    }

    @Test public void defaultPreservesManualOrderAndSourceIsNeverMutated() {
        assertEquals(MANUAL, view(GameLibraryView.MANUAL, false, ""));
        view(GameLibraryView.PLAYABLE_FIRST, true, "a");
        assertEquals("Unknown", MANUAL.get(0));
        assertEquals(MANUAL, view(GameLibraryView.MANUAL, false, ""));
    }

    @Test public void reverseReversesManualOrderExactly() {
        List<String> expected = new ArrayList<>(MANUAL);
        Collections.reverse(expected);
        assertEquals(expected, view(GameLibraryView.MANUAL, true, ""));
    }

    @Test public void playableFirstIsStableAndUnknownIsLast() {
        assertEquals(Arrays.asList("Playable B", "Playable A", "Unplayable B", "Unplayable A", "Unknown"),
                view(GameLibraryView.PLAYABLE_FIRST, false, ""));
    }

    @Test public void unplayableFirstIsStableAndUnknownIsLast() {
        assertEquals(Arrays.asList("Unplayable B", "Unplayable A", "Playable B", "Playable A", "Unknown"),
                view(GameLibraryView.UNPLAYABLE_FIRST, false, ""));
    }

    @Test public void reverseAppliesToCurrentStatusSortedListNotJustManualOrder() {
        for (int mode : new int[] {GameLibraryView.PLAYABLE_FIRST, GameLibraryView.UNPLAYABLE_FIRST}) {
            List<String> expected = view(mode, false, "");
            Collections.reverse(expected);
            assertEquals(expected, view(mode, true, ""));
            Collections.reverse(expected);
            assertEquals(expected, view(mode, false, ""));
        }
    }

    @Test public void queryIsTrimmedCaseInsensitiveAndComposesWithSortAndReverse() {
        assertEquals(Arrays.asList("Playable B", "Unplayable B"),
                view(GameLibraryView.MANUAL, false, "  ABLE B  "));
        assertEquals(Arrays.asList("Unplayable B", "Playable B"),
                view(GameLibraryView.PLAYABLE_FIRST, true, "able b"));
        assertEquals(Collections.emptyList(), view(GameLibraryView.MANUAL, false, "missing"));
    }

    @Test public void clearingSearchRestoresAllGamesAndTheirOrder() {
        view(GameLibraryView.UNPLAYABLE_FIRST, false, "missing");
        assertEquals(5, view(GameLibraryView.UNPLAYABLE_FIRST, false, " ").size());
        assertEquals(MANUAL, view(GameLibraryView.MANUAL, false, null));
    }

    @Test public void searchIsIndependentOfDeviceLocaleAndSupportsNonLatinTitles() {
        Locale previous = Locale.getDefault();
        try {
            Locale.setDefault(new Locale("tr", "TR"));
            assertTrue(GameLibraryView.matchesTitle("INSIDE", "inside"));
            assertTrue(GameLibraryView.matchesTitle("Игра", "ИГР"));
            assertFalse(GameLibraryView.matchesTitle(null, "inside"));
        } finally {
            Locale.setDefault(previous);
        }
    }

    @Test public void invalidSavedModeFallsBackToManual() {
        assertEquals(MANUAL, view(777, false, ""));
    }

    @Test public void newlyClassifiedGameMovesGroupsWithoutChangingTheBaseOrder() {
        List<String> before = view(GameLibraryView.PLAYABLE_FIRST, false, "");
        List<String> after = GameLibraryView.visible(MANUAL, GameLibraryView.PLAYABLE_FIRST,
                false, "", title -> title, title -> title.equals("Unknown")
                        ? GameCompatibilityStatus.SUPPORTED : status(title));
        assertEquals("Unknown", before.get(4));
        assertEquals("Unknown", after.get(0));
        assertEquals("Unknown", MANUAL.get(0));
    }
}
