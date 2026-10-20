package app.lsx4.android;

import static org.junit.Assert.assertEquals;

import java.util.Arrays;
import java.util.List;

import org.junit.Test;

public final class GameLibraryOrderTest {
    @Test
    public void rememberedMovesStayAuthoritativeAndNewGamesAppend() {
        List<String> result = GameLibraryOrder.mergeRemembered(
                Arrays.asList("CUSA00003", "CUSA00001", "CUSA00004"),
                Arrays.asList("CUSA00001", "CUSA00002", "CUSA00003"));
        assertEquals(Arrays.asList("CUSA00001", "CUSA00003", "CUSA00004"), result);
    }

    @Test
    public void movePlacesSelectedGameBeforeDestinationWithoutSwapping() {
        List<String> result = GameLibraryOrder.moveBefore(
                Arrays.asList("CUSA00001", "CUSA00002", "CUSA00003", "CUSA00004"),
                "CUSA00004", "CUSA00002");
        assertEquals(Arrays.asList(
                "CUSA00001", "CUSA00004", "CUSA00002", "CUSA00003"), result);
    }

    @Test
    public void statusIsAbsentFromOrderingInputs() {
        List<String> result = GameLibraryOrder.mergeRemembered(
                Arrays.asList("CUSA00002", "CUSA00001"),
                Arrays.asList("CUSA00002", "CUSA00001"));
        assertEquals(Arrays.asList("CUSA00002", "CUSA00001"), result);
    }

    @Test
    public void firstPersistentOrderIsSeededByInstallationTime() {
        assertEquals(-1, Integer.signum(GameLibraryOrder.compareInsertion(
                100L, "CUSA00009", 200L, "CUSA00001")));
        assertEquals(1, Integer.signum(GameLibraryOrder.compareInsertion(
                200L, "CUSA00001", 100L, "CUSA00009")));
        assertEquals(-1, Integer.signum(GameLibraryOrder.compareInsertion(
                100L, "CUSA00001", 100L, "CUSA00002")));
    }
}
