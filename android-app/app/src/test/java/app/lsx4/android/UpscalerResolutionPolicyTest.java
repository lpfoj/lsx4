package app.lsx4.android;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;

import org.junit.Test;

public final class UpscalerResolutionPolicyTest {
    @Test
    public void lowResolutionSourcesExposeEveryHigherResolution() {
        assertArrayEquals(new int[]{
                        SettingsActivity.RES_270P,
                        SettingsActivity.RES_360P,
                        SettingsActivity.RES_544P,
                        SettingsActivity.RES_720P,
                        SettingsActivity.RES_1080P,
                        SettingsActivity.RES_1620P,
                        SettingsActivity.RES_NATIVE
                },
                SettingsActivity.higherUpscaledOutputModes(SettingsActivity.RES_180P));
        assertArrayEquals(new int[]{
                        SettingsActivity.RES_360P,
                        SettingsActivity.RES_544P,
                        SettingsActivity.RES_720P,
                        SettingsActivity.RES_1080P,
                        SettingsActivity.RES_1620P,
                        SettingsActivity.RES_NATIVE
                },
                SettingsActivity.higherUpscaledOutputModes(SettingsActivity.RES_270P));
        assertArrayEquals(new int[]{
                        SettingsActivity.RES_544P,
                        SettingsActivity.RES_720P,
                        SettingsActivity.RES_1080P,
                        SettingsActivity.RES_1620P,
                        SettingsActivity.RES_NATIVE
                },
                SettingsActivity.higherUpscaledOutputModes(SettingsActivity.RES_360P));
    }

    @Test
    public void invalidOutputFallsBackToNearestHigherResolution() {
        assertEquals(SettingsActivity.RES_270P,
                SettingsActivity.sanitizeUpscaledOutputMode(
                        SettingsActivity.RES_180P, SettingsActivity.RES_180P));
        assertEquals(SettingsActivity.RES_720P,
                SettingsActivity.sanitizeUpscaledOutputMode(
                        SettingsActivity.RES_544P, SettingsActivity.RES_360P));
        assertEquals(SettingsActivity.RES_1620P,
                SettingsActivity.sanitizeUpscaledOutputMode(
                        SettingsActivity.RES_1620P, SettingsActivity.RES_1080P));
    }
}
