package app.lsx4.android;
import java.io.File;
/** The export receives a catalogue, so no Android storage enumeration is used. */
final class SharedGameStorage {
    static boolean hasAccess() { return false; }
    static File gamesDirectory() throws java.io.IOException { throw new UnsupportedOperationException("Host catalogue supplied explicitly"); }
}
