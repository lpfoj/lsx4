package app.lsx4.android;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;


final class GameLibraryOrder {
    private GameLibraryOrder() {}

    static int compareInsertion(long leftAddedAtMillis, String leftTitleId,
                                long rightAddedAtMillis, String rightTitleId) {
        int timeOrder = Long.compare(leftAddedAtMillis, rightAddedAtMillis);
        if (timeOrder != 0) {
            return timeOrder;
        }
        String safeLeft = leftTitleId == null ? "" : leftTitleId;
        String safeRight = rightTitleId == null ? "" : rightTitleId;
        return safeLeft.compareTo(safeRight);
    }

    static List<String> mergeRemembered(List<String> discovered, List<String> remembered) {
        List<String> merged = new ArrayList<>();
        Set<String> available = new HashSet<>(discovered);
        Set<String> added = new HashSet<>();
        for (String titleId : remembered) {
            if (available.contains(titleId) && added.add(titleId)) {
                merged.add(titleId);
            }
        }
        for (String titleId : discovered) {
            if (added.add(titleId)) {
                merged.add(titleId);
            }
        }
        return merged;
    }

    static List<String> moveBefore(List<String> current, String source, String target) {
        List<String> moved = new ArrayList<>(current);
        if (source == null || target == null || source.equals(target)
                || !moved.remove(source)) {
            return moved;
        }
        int targetIndex = moved.indexOf(target);
        if (targetIndex < 0) {
            moved.add(source);
        } else {
            moved.add(targetIndex, source);
        }
        return moved;
    }
}
