package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Copies the player's own game data from a folder they chose into the app's private storage, checking every byte.
 * Pure Java; the folder is reached through Source (on the device: the system file picker's folder).
 *
 * The folder is made on a computer from the player's own discs and holds game-data-manifest.json
 * (ShipDataManifest) beside the files it lists. Order of work:
 *   1. read the list; refuse it whole when any entry lies outside the game data layout, or when one name is listed
 *      as a file and as a folder;
 *   2. compare it with the game data this app version pins (ShipConfig.pinnedData): refuse it whole when it names
 *      a file the pin does not, when a pinned file has another size or SHA-256 in the list, or when a pinned file
 *      is neither in the list nor on the device (the pieces the app carries itself are on the device already);
 *   3. remove what an earlier copy that was killed left in the staging folder; see what is already in place; check
 *      the free space for the rest (plus a reserve) before copying anything;
 *   4. file by file, the game image last whatever the list's order: copy to private-trace/ship-import.part/
 *      (sha256).part while hashing, flush, compare size and SHA-256, and only then rename to the final name (0400).
 * A file therefore carries its final name only after it was verified, and that is what makes the copy resumable:
 * a file that is in place with the listed size is skipped and never rewritten, a part file is started again.
 * Small files (up to 64 MiB) that are already in place are hashed again as well, because that costs nothing.
 * The image goes last because it is what makes a start request usable for the launcher: as long as anything else
 * is still to copy, no start request qualifies.
 *
 * Everything is written under files/private-trace: inputs/, modules/boot/ and the image beside the start request.
 * Nothing under files/saves or files/profile is opened; nothing is removed except this class's own part files.
 */
final class ShipDataImport {
    /** Under files/private-trace; holds the files that are being copied. */
    static final String STAGING = "ship-import.part";
    /** Written after a complete import: the list's SHA-256, the number of files and of bytes. */
    static final String RECORD = "settings/ship-data.txt";
    /** Space that must stay free beyond the bytes still to copy. */
    static final long RESERVE_BYTES = 512L << 20;
    /** Files up to this size that are already in place are hashed again instead of trusted by size. */
    static final long REHASH_LIMIT = 64L << 20;
    static final String BUSY = "Another copy is still running; try again in a moment.";

    /** The folder the player chose. */
    interface Source {
        /**
         * The bytes of the file at relativePath ('/' separated), or null when the folder has no such file.
         * The caller closes the stream.
         */
        InputStream open(String relativePath) throws IOException;
    }

    interface Progress {
        void update(long bytesDone, long bytesTotal, int filesDone, int filesTotal, String currentPath);
        boolean cancelled();
    }

    static final class Outcome {
        final boolean complete, cancelled;
        /** null, or one plain sentence. */
        final String error;
        /** Of the list: how many files and bytes are in place now (complete) or were listed (otherwise). */
        final int files;
        final long bytes;
        /** How many files this run copied (the others were in place already). */
        final int copied;

        private Outcome(boolean complete, boolean cancelled, String error, int files, long bytes, int copied) {
            this.complete = complete;
            this.cancelled = cancelled;
            this.error = error;
            this.files = files;
            this.bytes = bytes;
            this.copied = copied;
        }

        static Outcome failed(String error) {
            return new Outcome(false, false, error, 0, 0, 0);
        }
    }

    private static final AtomicBoolean RUNNING = new AtomicBoolean();

    private ShipDataImport() {}

    /**
     * null when every game data file this app version pins is in place (ShipData.look: the size of
     * each, and the SHA-256 of the bundled pieces and of the files up to 64 MiB). Otherwise one sentence. Read-only;
     * call it off the UI thread.
     */
    static String missing(ShipConfig config, File filesDir) {
        return ShipData.look(config, filesDir).sentence();
    }

    /**
     * Removes what a copy that was killed left in the staging folder. Those part files are never read again (a copy
     * always starts a file from nothing); only their room is needed, and nobody else can reach them. Does nothing
     * while a copy runs. Returns the bytes freed.
     */
    static long removeLeftOvers(File filesDir) {
        if (!RUNNING.compareAndSet(false, true)) return 0;
        try {
            return sweep(filesDir);
        } finally {
            RUNNING.set(false);
        }
    }

    private static long sweep(File filesDir) {
        File staging = new File(new File(filesDir, InstallState.PRIVATE_TRACE), STAGING);
        long freed = ShipFiles.removeFiles(staging);
        ShipFiles.deleteQuietly(ShipFiles.partOf(new File(filesDir, RECORD)));
        return freed;
    }

    static Outcome run(ShipConfig config, Source source, File filesDir, Progress progress, ShipFiles.Space space) {
        if (!RUNNING.compareAndSet(false, true)) return Outcome.failed(BUSY);
        try {
            return runLocked(config, source, filesDir, progress, space);
        } finally {
            RUNNING.set(false);
        }
    }

    private static Outcome runLocked(ShipConfig config, Source source, File filesDir, Progress progress,
            ShipFiles.Space space) {
        try {
            if (!config.filesRoot.equals(filesDir.getAbsolutePath())) {
                return Outcome.failed("This app only works for the device's main user; nothing was copied.");
            }
            // 1. the list
            InputStream listStream = source.open(ShipDataManifest.FILE_NAME);
            if (listStream == null) {
                return Outcome.failed("This folder has no " + ShipDataManifest.FILE_NAME + ". Choose the folder that was made"
                        + " on your computer from your own discs. Nothing was copied.");
            }
            byte[] listBytes;
            try {
                listBytes = ShipFiles.readAll(listStream, ShipDataManifest.MAX_BYTES);
            } catch (IOException tooLong) {
                return Outcome.failed("The folder's file list (" + ShipDataManifest.FILE_NAME + ") cannot be used: "
                        + ShipFiles.reason(tooLong) + ". Nothing was copied.");
            }
            ShipDataManifest list;
            try {
                list = ShipDataManifest.parse(listBytes);
            } catch (IOException refused) {
                return Outcome.failed(refused.getMessage());
            }
            File root = new File(filesDir, InstallState.PRIVATE_TRACE);

            // 2. against what this app version needs: the pin is the whole game data, nothing beyond it is installed
            for (ShipDataManifest.Entry entry : list.entries) {
                ShipConfig.DataFile pinned = config.pinned(entry.path);
                if (pinned == null) {
                    return Outcome.failed("This folder's list names " + entry.path + ", which is not a file this app version"
                            + " uses. Nothing was copied.");
                }
                if (pinned.bytes != entry.bytes || !pinned.sha256.equals(entry.sha256)) {
                    return Outcome.failed(entry.path + " in this folder is not the file this app version was made for"
                            + " (its SHA-256 starts " + entry.sha256.substring(0, 12) + ", expected "
                            + pinned.sha256.substring(0, 12) + "). Nothing was copied.");
                }
            }
            List<String> wanted = new ArrayList<String>();
            for (ShipConfig.DataFile pinned : config.pinnedData) wanted.add(pinned.path);
            int absent = 0;
            String firstAbsent = null;
            for (String path : wanted) {
                if (list.entry(path) != null) continue;
                if (ShipFiles.regularFile(new File(root, ShipDataManifest.target(path, config.runId)))) continue;
                if (absent++ == 0) firstAbsent = path;
            }
            if (absent > 0) {
                return Outcome.failed("This folder is not complete: " + absent + (absent == 1 ? " file" : " files")
                        + " the game needs " + (absent == 1 ? "is" : "are") + " not in its list (the first is " + firstAbsent
                        + "). Nothing was copied.");
            }

            // 3. what is in place, and the space for the rest
            sweep(filesDir);   // an earlier copy's part files: before the free space is asked for
            List<ShipDataManifest.Entry> todo = new ArrayList<ShipDataManifest.Entry>();
            ShipDataManifest.Entry imageToCopy = null;
            long done = 0;
            int filesDone = 0;
            for (ShipDataManifest.Entry entry : list.entries) {
                File target = new File(root, ShipDataManifest.target(entry.path, config.runId));
                if (!ShipFiles.present(target)) {
                    if (ShipDataManifest.IMAGE.equals(entry.path)) imageToCopy = entry;
                    else todo.add(entry);
                    continue;
                }
                if (!ShipFiles.regularFile(target) || target.length() != entry.bytes) {
                    return Outcome.failed(entry.path + " is already on this device with another size; it was not changed"
                            + " and nothing was copied.");
                }
                if (entry.bytes <= REHASH_LIMIT && !entry.sha256.equals(ShipFiles.sha256Of(target))) {
                    return Outcome.failed(entry.path + " is already on this device with other content; it was not changed"
                            + " and nothing was copied.");
                }
                done += entry.bytes;
                ++filesDone;
            }
            if (imageToCopy != null) todo.add(imageToCopy);   // last: see the class comment
            long total = list.totalBytes;
            long toCopy = total - done;
            if (!todo.isEmpty()) {
                long need = toCopy + RESERVE_BYTES;
                long free = space.usable(ShipFiles.existingAncestor(root));
                if (need > free) {
                    return Outcome.failed("Not enough free space for the game data: it needs " + ShipFiles.amount(need, true)
                            + " and " + ShipFiles.amount(free, false) + " is free. Nothing was copied.");
                }
            }

            // 4. the copy
            ShipFiles.makeDirectory(root, ShipFiles.OWNER_ONLY, true);
            File staging = new File(root, STAGING);
            if (!todo.isEmpty()) ShipFiles.makeDirectory(staging, ShipFiles.OWNER_ONLY, false);
            int copied = 0;
            for (ShipDataManifest.Entry entry : todo) {
                if (progress.cancelled()) return stopped(list, copied);
                progress.update(done, total, filesDone, list.entries.size(), entry.path);
                File target = new File(root, ShipDataManifest.target(entry.path, config.runId));
                String problem = copyOne(entry, source, staging, target, progress, done, total, filesDone, list.entries.size());
                if (problem == STOPPED) return stopped(list, copied);
                if (problem != null) return new Outcome(false, false, problem, list.entries.size(), total, copied);
                done += entry.bytes;
                ++filesDone;
                ++copied;
            }
            if (ShipFiles.directory(staging)) {
                String[] left = staging.list();
                if (left != null && left.length == 0) staging.delete();   // only when empty; never a file in it
            }
            progress.update(total, total, list.entries.size(), list.entries.size(), "");
            File settings = new File(filesDir, "settings");
            ShipFiles.makeDirectory(settings, ShipFiles.OWNER_ONLY, false);
            ShipFiles.writeSmall(new File(filesDir, RECORD), ("manifest " + list.sha256 + "\nfiles " + list.entries.size()
                    + "\nbytes " + total + "\n").getBytes(StandardCharsets.US_ASCII), ShipFiles.OWNER_WRITE);
            return new Outcome(true, false, null, list.entries.size(), total, copied);
        } catch (IOException e) {
            return Outcome.failed("Could not copy the game data: " + ShipFiles.reason(e) + ".");
        } catch (RuntimeException e) {
            return Outcome.failed("Copying the game data stopped unexpectedly: " + ShipFiles.reason(e) + ".");
        }
    }

    private static final String STOPPED = new String("stopped");   // an identity, never shown

    private static Outcome stopped(ShipDataManifest list, int copied) {
        return new Outcome(false, true, null, list.entries.size(), list.totalBytes, copied);
    }

    /** null when the file is in place under its final name; STOPPED when the user stopped; else the sentence. */
    private static String copyOne(final ShipDataManifest.Entry entry, Source source, File staging, File target,
            final Progress progress, final long doneBefore, final long total, final int filesDone, final int filesTotal)
            throws IOException {
        InputStream in;
        try {
            in = source.open(entry.path);
        } catch (IOException unreadable) {
            return entry.path + " in the folder cannot be read: " + ShipFiles.reason(unreadable) + ".";
        }
        if (in == null) return "The folder's list names " + entry.path + ", but the folder does not hold it.";
        File part = new File(staging, entry.sha256 + ".part");
        ShipFiles.Copy copy;
        try {
            copy = ShipFiles.copyVerified(in, part, entry.bytes, entry.sha256, new ShipFiles.CopyWatch() {
                @Override public boolean copied(long bytes) {
                    progress.update(doneBefore + bytes, total, filesDone, filesTotal, entry.path);
                    return !progress.cancelled();
                }
            });
        } catch (IOException failed) {
            return "Copying " + entry.path + " failed: " + ShipFiles.reason(failed) + ".";
        }
        switch (copy) {
            case VERIFIED:
                break;
            case STOPPED:
                return STOPPED;
            case WRONG_HASH:
                return entry.path + " in the folder is damaged: its SHA-256 does not match the folder's list. It was not installed.";
            case TOO_SHORT:
                return entry.path + " in the folder is shorter than the folder's list says. It was not installed.";
            default:
                return entry.path + " in the folder is longer than the folder's list says. It was not installed.";
        }
        if (ShipFiles.present(target)) {
            ShipFiles.deleteQuietly(part);
            return entry.path + " appeared on this device meanwhile; it was not changed.";
        }
        ShipFiles.makeDirectory(target.getParentFile(), ShipFiles.OWNER_ONLY, false);
        ShipFiles.commit(part, target, ShipFiles.OWNER_READ);
        return null;
    }
}
