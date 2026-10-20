package app.lsx4.android;

import org.junit.Test;
import org.junit.Rule;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.nio.file.Files;

import static org.junit.Assert.*;

public final class CaptureStoreTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test public void partialAndEmptyFilesNeverReachGallery() throws Exception {
        CaptureStore store = new CaptureStore(temporary.getRoot());
        File partial = store.createPending("../Game/演示", "mp4");
        assertTrue(store.list().isEmpty());
        try {
            store.publish(partial);
            fail("Empty video published");
        } catch (java.io.IOException expected) { }
        Files.write(partial.toPath(), new byte[]{1, 2, 3});
        File video = store.publish(partial);
        assertTrue(CaptureStore.isVideo(video));
        assertEquals(1, store.list().size());
        assertEquals(video, store.list().get(0));
        assertFalse(partial.exists());
    }

    @Test public void shareScopeRejectsTraversalAndUnsupportedTypes() throws Exception {
        CaptureStore store = new CaptureStore(temporary.getRoot());
        File outside = temporary.newFile("private.mp4");
        Files.write(outside.toPath(), new byte[]{1});
        assertFalse(store.isCompleted(outside));
        File traversal = new File(temporary.getRoot(), "captures/../private.mp4");
        assertFalse(store.isCompleted(traversal));
        store.discard(outside);
        assertTrue(outside.exists());
        try {
            store.createPending("Game", "xml");
            fail("Unsupported file type accepted");
        } catch (IllegalArgumentException expected) { }
    }

    @Test public void namesAreUnicodeSafeUniqueAndDirectChildren() throws Exception {
        CaptureStore store = new CaptureStore(temporary.getRoot());
        File first = store.createPending("../演示\\Game\n:*?", "png");
        File second = store.createPending("../演示\\Game\n:*?", "png");
        assertNotEquals(first, second);
        assertEquals(new File(temporary.getRoot(), "captures").getCanonicalFile(),
                first.getCanonicalFile().getParentFile());
        assertTrue(first.getName().contains("演示"));
        assertEquals("LSX4", CaptureStore.safeTitle(" "));
        store.discard(first);
        assertFalse(first.exists());
    }

    @Test public void failedPublishDoesNotOverwriteExistingCapture() throws Exception {
        CaptureStore store = new CaptureStore(temporary.getRoot());
        File pending = store.createPending("Game", "png");
        Files.write(pending.toPath(), new byte[]{1});
        File destination = new File(pending.getParentFile(),
                pending.getName().substring(0, pending.getName().length() - 8));
        Files.write(destination.toPath(), new byte[]{9});
        try {
            store.publish(pending);
            fail("Existing image was overwritten");
        } catch (java.io.IOException expected) { }
        assertArrayEquals(new byte[]{9}, Files.readAllBytes(destination.toPath()));
    }
}
