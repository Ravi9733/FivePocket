package dev.recompbootstrap.smoke;

import java.io.IOException;

/**
 * Read-only view of the app's files/ directory, as far as the settings code needs it. Paths are relative to files/
 * and use '/' (for example "private-trace/diag-fps-overlay"). The real one is SettingsStore.view(); tests use one in
 * memory. Pure Java.
 */
interface FileView {
    /** True when something exists under that name (what the game's own access() check sees). */
    boolean exists(String relativePath);

    /** At most 4096 bytes; a longer file gives exactly its first 4096. Missing file: java.io.FileNotFoundException. */
    byte[] read(String relativePath) throws IOException;
}
