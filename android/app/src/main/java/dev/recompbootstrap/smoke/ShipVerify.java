package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.TimeUnit;

/**
 * The whole read of the game code file and the shader file that stands behind "Ready" in a build that carries its
 * game code: both files are read from the first byte to the last (412 MB in the real build) and their SHA-256 is
 * compared with the record's (ShipFirstRun.changedCode). Pure Java. This class makes that cost visible and keeps it
 * from being paid more often than it must be; it removes no check.
 *
 * Visible: every such read writes one line to the log, tag RecompShip (the sink is set by ShipApplication):
 *
 *   VERIFY_CODE ms=(milliseconds) bytes=(bytes of the two files) result=same|changed why=(who asked) n=(count)
 *
 * n counts the reads since the app's process started. why: "start" (the app's first screen, ShipStart), "menu" (the
 * Play tab's look), "about" (the About tab), "setup-page" (the two-disc page), "check" (any other caller).
 * ShipPlay writes a line of its own for the read Play makes on the way to a start (VERIFY_PLAY).
 *
 * Not more often than it must be: the menu's three pages ask through remembered(). It answers from the last read
 * while that read is still about the same two files: the same record, and each file with the same size, the same
 * time of last change and the same identity on storage (a file that was replaced is another file, also under the
 * same name). again() makes the next question read anew; LauncherScreen calls it when the player comes back from
 * the game, from the two-disc setup page or from the first screen ("Finish Setting Up"). So the two files are read
 *   - once per start of the app (by the first screen on a usual start; by the menu's first look after a first
 *     start or an update, whose own copy checks each piece as it is written),
 *   - once per return from the game, from the two-disc setup page or from the first screen,
 * and not when a tab is changed, About is opened, the setup page looks at the files, the app comes back from Home
 * or from the controller check.
 *
 * What a remembered answer cannot see: a change of a file's bytes that leaves its size, its time of last change and
 * its identity as they were. Nothing in the app writes these two files that way (ShipFirstRun replaces them by a
 * new file), and a player cannot reach them in a build that is not debuggable. Play does not ask here at all: it
 * hashes both files itself before every start, so changed game code is never started whatever is remembered.
 */
final class ShipVerify {
    /** Where the lines go. Android's log in the app (ShipApplication); a list in a host test. */
    interface Sink {
        void line(String line);
    }

    static final String WHY_START = "start", WHY_MENU = "menu", WHY_ABOUT = "about", WHY_SETUP_PAGE = "setup-page",
            WHY_CHECK = "check";

    private static final Object LOCK = new Object();
    private static volatile Sink sink;
    /** What the remembered answer is about (the record and the two files as they were); null: nothing is remembered. */
    private static String rememberedFor;
    private static List<String> remembered;
    private static int reads;

    private ShipVerify() {}

    /** Where the VERIFY_ lines go from now on; null: nowhere. */
    static void setSink(Sink to) {
        sink = to;
    }

    /** Writes one line to the sink. Never throws: a log that fails must not fail the check. */
    static void line(String text) {
        Sink to = sink;
        if (to == null) return;
        try {
            to.line(text);
        } catch (RuntimeException ignored) {
            // the line is lost; the check's answer stands
        }
    }

    /**
     * Reads both files whole now, writes one VERIFY_CODE line and remembers the answer for remembered(). Returns
     * ShipFirstRun.changedCode's answer: the names of the files whose bytes are not the recorded ones; empty when
     * both are. One read at a time: a second caller waits for the first.
     */
    static List<String> read(ShipConfig config, File filesDir, String why) {
        synchronized (LOCK) {
            String before = identity(config, filesDir);
            long started = System.nanoTime();
            List<String> changed = Collections.unmodifiableList(ShipFirstRun.changedCode(config, filesDir));
            long ms = TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started);
            String after = identity(config, filesDir);
            // Remembered only when the two files were the same files before and after the read.
            boolean steady = before.equals(after);
            rememberedFor = steady ? after : null;
            remembered = steady ? changed : null;
            ++reads;
            line("VERIFY_CODE ms=" + ms + " bytes=" + bytes(config, filesDir) + " result=" + (changed.isEmpty() ? "same" : "changed")
                    + " why=" + why + " n=" + reads);
            return changed;
        }
    }

    /**
     * For the menu's pages: the answer of the last read while it is still about the same record and the same two
     * files (see the class comment), without reading and without a log line; otherwise read().
     */
    static List<String> remembered(ShipConfig config, File filesDir, String why) {
        synchronized (LOCK) {
            if (remembered != null && identity(config, filesDir).equals(rememberedFor)) return remembered;
            return read(config, filesDir, why);
        }
    }

    /** The next remembered() reads anew: the player was somewhere that may have changed the files. */
    static void again() {
        synchronized (LOCK) {
            rememberedFor = null;
            remembered = null;
        }
    }

    private static File[] files(ShipConfig config, File filesDir) {
        File root = new File(filesDir, InstallState.PRIVATE_TRACE);
        return new File[] {new File(root, InstallState.LIVE_LIBRARY), new File(root, config.shaderPackName)};
    }

    /** The record and, for each of the two files, its place, size, time of last change and identity on storage. */
    private static String identity(ShipConfig config, File filesDir) {
        StringBuilder text = new StringBuilder(config.sha256);
        for (File file : files(config, filesDir)) {
            text.append('|').append(file.getAbsolutePath()).append(':');
            try {
                BasicFileAttributes found = Files.readAttributes(file.toPath(), BasicFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
                text.append(found.isRegularFile() ? "file" : "other").append(',').append(found.size()).append(',')
                        .append(found.lastModifiedTime().to(TimeUnit.NANOSECONDS)).append(',').append(found.fileKey());
            } catch (IOException | RuntimeException absent) {
                text.append("absent");
            }
        }
        return text.toString();
    }

    private static long bytes(ShipConfig config, File filesDir) {
        long total = 0;
        for (File file : files(config, filesDir)) {
            if (ShipFiles.regularFile(file)) total += file.length();
        }
        return total;
    }
}
