package app.lsx4.android;

import org.junit.Test;
import static org.junit.Assert.*;

public final class GamePlatformMetadataTest {
    @Test public void ps4JoinSessionJsonWithoutTitleIdIsNotPs5() {
        assertFalse(GamePlatformMetadata.isPs5TitleId(null));
        assertFalse(GamePlatformMetadata.isPs5TitleId(""));
        assertFalse(GamePlatformMetadata.isPs5TitleId("CUSA26423"));
    }
    @Test public void actualPs5GoldenTitlesKeepTheirRoute() {
        assertTrue(GamePlatformMetadata.isPs5TitleId("PPSA02929"));
        assertTrue(GamePlatformMetadata.isPs5TitleId("PPSA03061"));
    }
    @Test public void malformedIdsAreNotPlatformProof() {
        for (String id : new String[]{"PPSA", "PPSA1234", "PPSA123456", "PPSA12X45", "CUSA02929", "PPSA１２３４５"})
            assertFalse(id, GamePlatformMetadata.isPs5TitleId(id));
    }
}
