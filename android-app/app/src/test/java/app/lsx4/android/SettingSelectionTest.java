package app.lsx4.android;

import static org.junit.Assert.*;
import org.junit.Test;

public final class SettingSelectionTest {
    @Test public void bindingInheritedSelectionDoesNotPersistAnOverride() {
        SettingSelection selection = new SettingSelection(3);
        assertFalse(selection.changed(3));
        assertFalse(selection.changed(3));
        assertFalse(selection.changed(-1));
    }

    @Test public void userCanChangeAndReturnToOriginalChoice() {
        SettingSelection selection = new SettingSelection(3);
        assertTrue(selection.changed(5));
        assertFalse(selection.changed(5));
        assertTrue(selection.changed(3));
    }
}
