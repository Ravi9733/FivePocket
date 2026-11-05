package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;

/**
 * Which disc image the player chose for each disc, kept in files/settings so that a copy can be continued after the
 * app was stopped without choosing again. Pure Java. One small file per disc:
 *   files/settings/setup-disc-play.txt, files/settings/setup-disc-install.txt
 * each three lines: the document's address (a content:// Uri as text), its name for the player, its size in bytes.
 * Written atomically (AtomicFiles). A file that cannot be read or is not in this form counts as "nothing remembered".
 * The read permission itself is the system's (ContentResolver.takePersistableUriPermission, by SetupDiscScreen);
 * this class only remembers the address.
 *
 * Beside them, one marker per disc about its last copy (setup-disc-copying-play.txt, setup-disc-copying-install.txt),
 * two lines: its state and the token of the process that wrote it (setup-fixes, defects C1-1 and M1 of the check):
 *   running  written before the copy writes anything (Marker.begin);
 *   stopped  the copy was stopped with B (Marker.end);
 *   left     the copy was stopped because the app left the front: Home, another app, the screen switched off;
 *   (none)   the copy ended (done or failed): the marker is removed.
 * lastCopy() reads it: a "running" marker of ANOTHER process means the copy was cut off (the app was killed or the
 * device restarted); one of this process means a copy is still running here. The marker is only for the row's words:
 * nothing is decided by it.
 */
final class SetupDiscStore {
    static final String FOLDER = "settings";
    static final String[] NAMES = {"setup-disc-play.txt", "setup-disc-install.txt"};
    static final String[] COPYING = {"setup-disc-copying-play.txt", "setup-disc-copying-install.txt"};
    /** What lastCopy() says of a disc's last copy. */
    static final int LAST_NONE = 0, LAST_INTERRUPTED = 1, LAST_STOPPED = 2, LAST_LEFT = 3;
    static final String RUNNING = "running", STOPPED = "stopped", LEFT = "left";
    /** This process's token: a new one in every process (the app killed or the device restarted gives a new one). */
    static final String PROCESS = java.util.UUID.randomUUID().toString();
    /** Longer than any address a document provider gives; a longer file is not one of ours. */
    static final int MAX_BYTES = 8192;

    /** One remembered choice. */
    static final class Choice {
        /** The document's address as the system gave it. */
        final String uri;
        /** Its name for the player, for example "Disc2-Play.iso". */
        final String name;
        /** Its size in bytes when it was chosen (to say how much deleting it frees); -1 when not known. */
        final long bytes;

        Choice(String uri, String name, long bytes) {
            this.uri = uri;
            this.name = name;
            this.bytes = bytes;
        }
    }

    private final File folder;
    private final String process;

    SetupDiscStore(File filesDir) {
        this(filesDir, PROCESS);
    }

    /** process: the token written into a marker and compared with (tests give one per simulated process). */
    SetupDiscStore(File filesDir, String process) {
        this.folder = new File(filesDir, FOLDER);
        this.process = process;
    }

    File file(int disc) {
        return new File(folder, NAMES[disc]);
    }

    /** The remembered choice of disc (SetupDiscModel.PLAY or INSTALL), or null. Never throws. */
    Choice load(int disc) {
        File file = file(disc);
        try {
            if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS) || file.length() > MAX_BYTES) return null;
            String[] lines = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).split("\n", -1);
            if (lines.length < 3 || lines[0].isEmpty() || !lines[0].startsWith("content://")) return null;
            long bytes;
            try {
                bytes = Long.parseLong(lines[2].trim());
            } catch (NumberFormatException e) {
                return null;
            }
            return new Choice(lines[0], lines[1].isEmpty() ? null : lines[1], bytes < 0 ? -1 : bytes);
        } catch (IOException | RuntimeException e) {
            return null;
        }
    }

    /** Remembers choice for disc, replacing what was there. The address must be a content:// Uri without a line break. */
    void save(int disc, Choice choice) throws IOException {
        if (choice.uri == null || !choice.uri.startsWith("content://") || choice.uri.indexOf('\n') >= 0) {
            throw new IOException("not a document address");
        }
        String name = choice.name == null ? "" : choice.name.replace('\n', ' ');
        VerifiedExtractor.makeDirectory(folder);
        byte[] text = (choice.uri + "\n" + name + "\n" + choice.bytes + "\n").getBytes(StandardCharsets.UTF_8);
        if (text.length > MAX_BYTES) throw new IOException("the document address is too long");
        AtomicFiles.write(file(disc), text);
    }

    /** Forgets the choice of disc. True when there was one. */
    boolean forget(int disc) {
        try {
            return AtomicFiles.remove(file(disc));
        } catch (IOException | RuntimeException e) {
            return false;
        }
    }

    // ---- the marker of a running copy -------------------------------------------------------------------------------

    File copyingFile(int disc) {
        return new File(folder, COPYING[disc]);
    }

    /** One copy's marker. The screen and the tests' twin of it make one per copy and call the same three methods. */
    Marker marker(int disc) {
        return new Marker(this, disc);
    }

    static final class Marker {
        private final SetupDiscStore store;
        private final int disc;
        /** Set from the UI thread when the app leaves the front during this copy; read by the copy's thread at its end. */
        private volatile boolean left;

        Marker(SetupDiscStore store, int disc) {
            this.store = store;
            this.disc = disc;
        }

        /** Before the copy writes anything. False when the marker could not be written (the copy still runs). */
        boolean begin() {
            return store.write(disc, RUNNING);
        }

        /** The copy was told to stop because the app left the front (SetupDiscModel.appLeft returned true). */
        void appLeft() {
            left = true;
        }

        /** The copy's end as the page sees it: stopped (by B or by leaving) or not. Never throws. */
        void end(boolean stopped) {
            String state = after(stopped, left);
            // If the few bytes of "stopped"/"left" cannot be written (a full storage), the marker goes, so that a stop is
            // never told as an interruption later; the row then says only what is in place (check-2).
            if (state == null || !store.write(disc, state)) store.remove(disc);
        }

        /** What stays after a copy: null (removed) when it ended by itself, else who stopped it. */
        static String after(boolean stopped, boolean left) {
            return !stopped ? null : left ? LEFT : STOPPED;
        }
    }

    private boolean write(int disc, String state) {
        try {
            VerifiedExtractor.makeDirectory(folder);
            AtomicFiles.write(copyingFile(disc), (state + "\n" + process + "\n").getBytes(StandardCharsets.UTF_8));
            return true;
        } catch (IOException | RuntimeException e) {
            return false;
        }
    }

    private boolean remove(int disc) {
        try {
            return AtomicFiles.remove(copyingFile(disc));
        } catch (IOException | RuntimeException e) {
            return false;
        }
    }

    /** LAST_NONE, LAST_INTERRUPTED, LAST_STOPPED or LAST_LEFT. A file that is not a marker counts as none. Never throws. */
    int lastCopy(int disc) {
        File file = copyingFile(disc);
        try {
            if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS) || file.length() > MAX_BYTES) return LAST_NONE;
            String[] lines = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).split("\n", -1);
            if (lines.length < 2) return LAST_NONE;
            if (lines[0].equals(RUNNING)) return lines[1].equals(process) ? LAST_NONE : LAST_INTERRUPTED;
            if (lines[0].equals(STOPPED)) return LAST_STOPPED;
            if (lines[0].equals(LEFT)) return LAST_LEFT;
            return LAST_NONE;
        } catch (IOException | RuntimeException e) {
            return LAST_NONE;
        }
    }
}
