package app.lsx4.android;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.function.Function;


final class GameLibraryView {
    static final int MANUAL = 0;
    static final int PLAYABLE_FIRST = 1;
    static final int UNPLAYABLE_FIRST = 2;

    private GameLibraryView() {}

    static int validMode(int mode) {
        return mode == PLAYABLE_FIRST || mode == UNPLAYABLE_FIRST ? mode : MANUAL;
    }

    static boolean matchesTitle(String title, String query) {
        String needle = query == null ? "" : query.trim().toLowerCase(Locale.ROOT);
        return needle.isEmpty()
                || (title != null && title.toLowerCase(Locale.ROOT).contains(needle));
    }

    static <T> List<T> visible(List<T> manualOrder, int mode, boolean reversed,
                              String query, Function<T, String> title,
                              Function<T, GameCompatibilityStatus> status) {
        List<T> result = new ArrayList<>();
        for (T game : manualOrder) {
            if (matchesTitle(title.apply(game), query)) {
                result.add(game);
            }
        }
        int safeMode = validMode(mode);
        if (safeMode != MANUAL) {

            result.sort((left, right) -> Integer.compare(
                    priority(status.apply(left), safeMode),
                    priority(status.apply(right), safeMode)));
        }
        if (reversed) {
            Collections.reverse(result);
        }
        return result;
    }

    private static int priority(GameCompatibilityStatus status, int mode) {
        if (status == null || status == GameCompatibilityStatus.UNCLASSIFIED) {
            return 2;
        }
        boolean playable = status == GameCompatibilityStatus.SUPPORTED;
        return playable == (mode == PLAYABLE_FIRST) ? 0 : 1;
    }
}
