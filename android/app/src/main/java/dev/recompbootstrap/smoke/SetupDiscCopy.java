package dev.recompbootstrap.smoke;

import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Copies from one recognised disc image (Disc2Discs.open) the files the game needs into files/private-trace/inputs,
 * each named by its own SHA-256. Pure Java. The two-disc setup page and the command-line proof tool both run run().
 *
 * Order of work, for one disc:
 *   1. one copy at a time in the process (VerifiedExtractor.begin, the lock the developer page's copy uses too);
 *   2. the plan (Disc2Discs.plan) over every pinned disc entry; before anything is written, the disc must hold a file
 *      of the right size for each entry it is to provide (Plan.shortSizes over that disc's entries);
 *   3. what a stopped or killed run left in disc-staging/ is counted and removed, then the free space is checked;
 *   4. file by file: the free space is checked again; the file is written to disc-staging/disc-file.part while it is
 *      hashed, forced to storage and closed; only when its size and SHA-256 equal a pinned entry that is not yet in
 *      place (Plan.judge) is it made read-only and renamed in one atomic step to inputs/(sha256).bin; else it is
 *      removed and reported;
 *   5. afterwards every entry of that disc must be in place (by name and size), else the disc did not provide it.
 * A failure is told apart by kind (VerifiedExtractor.Problem): too little space, a bad disc copy (cut off, a block
 * failing its hash, a file that is not the pinned one), the image unreadable (may work again), the device's storage.
 * A failure has two texts (Outcome): error for the log, which names the disc's file and gives the reason as it
 * came, and shown for the page, which says "File k of n" as the copy line does and carries neither (the lead's
 * decision of 2026-10-06: no internal names on a handed-out build's screens).
 *
 * Where the safeguards of VerifiedExtractor are now, for this copy: the atomic rename and the read-only mode in
 * DiskStore.keep; left-over staging files counted and removed before the space check in DiskStore.prepare and run();
 * the space re-checked before every file in run(); failures told apart in copyOne() (a write failure is a
 * WriteFailed, so it is never mistaken for a read failure of the image); a final file of the wrong size is
 * replaced in that same rename (REPLACE_EXISTING) and untouched until then; a folder under a final name is refused.
 *
 * The store is the one part that differs between writing to the device and the proof tool's --hash-only mode
 * (HashOnlyStore): every decision above is made by the same lines either way.
 */
final class SetupDiscCopy {
    /** The staging folder inside files/private-trace, and the one part file in it. */
    static final String STAGING = "disc-staging", PART = "disc-file.part", INPUTS = "inputs";
    /** Space kept free for the system beyond what the copy still needs. */
    static final long RESERVE_BYTES = 1L << 30;

    interface Progress {
        /** fileIndex: 0-based among the files to read; name: Disc2Discs.Candidate.name. */
        void update(long bytesDone, long bytesTotal, int fileIndex, int files, String name);
        boolean cancelled();
    }

    /** One file of the disc that was read to its end. */
    static final class FileResult {
        final int index, files;
        final String name;
        final long bytes;
        final String sha256;
        final Disc2Discs.Verdict verdict;
        /** True when it was kept as inputs/(sha256).bin (always and only for WANTED). */
        final boolean kept;

        FileResult(int index, int files, String name, long bytes, String sha256, Disc2Discs.Verdict verdict, boolean kept) {
            this.index = index;
            this.files = files;
            this.name = name;
            this.bytes = bytes;
            this.sha256 = sha256;
            this.verdict = verdict;
            this.kept = kept;
        }
    }

    interface Report {
        /** Called once per file read to its end, before the next one is started. */
        void file(FileResult result);
    }

    static final class Outcome {
        /** Every entry this disc is to provide is in place afterwards. */
        final boolean complete;
        /** The player stopped it (Progress.cancelled). */
        final boolean stopped;
        /** NONE unless error is set. */
        final VerifiedExtractor.Problem problem;
        /**
         * null, or one sentence FOR THE LOG (SetupDiscLog.copyEnd): it names the disc's file and gives the reason as
         * the reader or the storage gave it. Not for a screen.
         */
        final String error;
        /**
         * null exactly when error is null, else the sentence FOR THE PAGE: the same failure with "File k of n" (the
         * numbers of the copy line) in place of the file's name, and without the raw reason, which can itself name a
         * file of the disc or of the app's own storage. It never names a file.
         */
        final String shown;
        /** For NO_SPACE: the free bytes needed (reserve included) and found. */
        final long needBytes, freeBytes;
        /** Bytes of left-over part files removed before the copy. */
        final long leftoversRemoved;
        /** Files to read by the plan and the bytes of them. */
        final int filesToRead;
        final long bytesToRead;
        final List<FileResult> files;
        /** The entries of this disc not in place afterwards. */
        final List<Disc2Discs.Pin> stillMissing;

        /** A failure whose one sentence names no file and may be shown as it is (or no failure: error null). */
        Outcome(boolean complete, boolean stopped, VerifiedExtractor.Problem problem, String error, long needBytes, long freeBytes,
                long leftoversRemoved, int filesToRead, long bytesToRead, List<FileResult> files, List<Disc2Discs.Pin> stillMissing) {
            this(complete, stopped, problem, error, error, needBytes, freeBytes, leftoversRemoved, filesToRead, bytesToRead, files, stillMissing);
        }

        Outcome(boolean complete, boolean stopped, VerifiedExtractor.Problem problem, String error, String shown, long needBytes,
                long freeBytes, long leftoversRemoved, int filesToRead, long bytesToRead, List<FileResult> files,
                List<Disc2Discs.Pin> stillMissing) {
            this.complete = complete;
            this.stopped = stopped;
            this.problem = problem;
            this.error = error;
            this.shown = shown;
            this.needBytes = needBytes;
            this.freeBytes = freeBytes;
            this.leftoversRemoved = leftoversRemoved;
            this.filesToRead = filesToRead;
            this.bytesToRead = bytesToRead;
            this.files = Collections.unmodifiableList(new ArrayList<FileResult>(files));
            this.stillMissing = Collections.unmodifiableList(new ArrayList<Disc2Discs.Pin>(stillMissing));
        }

        int kept() {
            int n = 0;
            for (FileResult f : files) {
                if (f.kept) ++n;
            }
            return n;
        }
    }

    /** A write to the part file failed: the device's storage, never the disc image. */
    static final class WriteFailed extends IOException {
        private static final long serialVersionUID = 1L;

        WriteFailed(IOException cause) {
            super(VerifiedExtractor.reason(cause), cause);
        }
    }

    /**
     * Where the bytes go. DiskStore writes them to the device; HashOnlyStore (the proof tool's --hash-only mode)
     * throws them away. Nothing else differs between the two modes.
     */
    interface Store {
        /** False: nothing is written, so no free space is needed (the free-space checks are skipped). */
        boolean writes();
        /** Before the first file: folders in place, left-overs removed. Returns the bytes removed. */
        long prepare() throws IOException;
        /** A new, empty part file to write one disc file to. Its write failures are WriteFailed. */
        OutputStream open() throws IOException;
        /** After the last byte: the part file is on storage and closed. */
        void finish() throws IOException;
        /** The part file becomes inputs/(sha256).bin in one step. */
        void keep(String sha256) throws IOException;
        /** The part file is removed (not wanted, or stopped, or failed). Never throws. */
        void discard();
        /** The SHA-256 values of the entries in place now, by name and size. */
        Set<String> installed(List<Disc2Discs.Pin> pins);
    }

    private SetupDiscCopy() {}

    // ---- the store that writes to the device ---------------------------------------------------------------------

    /** root: files/private-trace. */
    static Store disk(File root) {
        return new DiskStore(root);
    }

    static final class DiskStore implements Store {
        final File root, inputs, staging, part;
        private FileChannel channel;

        DiskStore(File root) {
            this.root = root;
            this.inputs = new File(root, INPUTS);
            this.staging = new File(root, STAGING);
            this.part = new File(staging, PART);
        }

        @Override public boolean writes() {
            return true;
        }

        @Override public long prepare() throws IOException {
            VerifiedExtractor.makeDirectory(root);
            VerifiedExtractor.makeDirectory(inputs);
            VerifiedExtractor.makeDirectory(staging);
            return clearLeftovers(root);
        }

        @Override public OutputStream open() throws IOException {
            try {
                channel = VerifiedExtractor.createNew(part);   // whatever stood there (a killed run's part, a link) is removed first
            } catch (IOException e) {
                throw new WriteFailed(e);   // the device's storage, not the disc image
            }
            final FileChannel out = channel;
            return new OutputStream() {
                @Override public void write(int b) throws IOException {
                    write(new byte[] {(byte) b}, 0, 1);
                }

                @Override public void write(byte[] buffer, int offset, int length) throws IOException {
                    try {
                        ByteBuffer bytes = ByteBuffer.wrap(buffer, offset, length);
                        while (bytes.hasRemaining()) out.write(bytes);
                    } catch (IOException e) {
                        throw new WriteFailed(e);
                    }
                }
            };
        }

        @Override public void finish() throws IOException {
            FileChannel out = channel;
            channel = null;
            try {
                out.force(true);   // the bytes are on storage before the file gets its final name
            } catch (IOException e) {
                throw new WriteFailed(e);
            } finally {
                out.close();
            }
        }

        @Override public void keep(String sha256) throws IOException {
            File target = new File(inputs, sha256 + ".bin");
            if (Files.isDirectory(target.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("a folder stands under the name " + target.getName());
            }
            VerifiedExtractor.ownerReadOnly(part);
            // One step: the verified copy takes the final name. A file of another size that stood there is replaced in
            // that same step and was untouched until now.
            Files.move(part.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            VerifiedExtractor.syncDirectory(inputs);
        }

        @Override public void discard() {
            if (channel != null) {
                try {
                    channel.close();
                } catch (IOException ignored) {
                    // it is removed next
                }
                channel = null;
            }
            try {
                Files.deleteIfExists(part.toPath());
            } catch (IOException ignored) {
                // the next run's prepare removes it
            }
        }

        @Override public Set<String> installed(List<Disc2Discs.Pin> pins) {
            return Disc2Discs.installed(inputs, pins);
        }
    }

    // ---- the store of the proof tool's --hash-only mode -------------------------------------------------------------

    /** inputsAlready: the folder whose files count as in place at the start (read only); kept files are remembered. */
    static Store hashOnly(final File inputsAlready) {
        return new Store() {
            private final Set<String> made = new HashSet<String>();

            @Override public boolean writes() {
                return false;
            }

            @Override public long prepare() {
                return 0;
            }

            @Override public OutputStream open() {
                return new OutputStream() {
                    @Override public void write(int b) {}

                    @Override public void write(byte[] buffer, int offset, int length) {}
                };
            }

            @Override public void finish() {}

            @Override public void keep(String sha256) {
                made.add(sha256);
            }

            @Override public void discard() {}

            @Override public Set<String> installed(List<Disc2Discs.Pin> pins) {
                Set<String> out = Disc2Discs.installed(inputsAlready, pins);
                for (Disc2Discs.Pin p : pins) {
                    if (made.contains(p.sha256)) out.add(p.sha256);
                }
                return out;
            }
        };
    }

    // ---- left-overs --------------------------------------------------------------------------------------------------

    /** Regular files named *.part directly in disc-staging/ (links are not followed and not counted). */
    private static List<File> leftovers(File root) {
        List<File> found = new ArrayList<File>();
        File staging = new File(root, STAGING);
        if (!Files.isDirectory(staging.toPath(), LinkOption.NOFOLLOW_LINKS)) return found;
        File[] children = staging.listFiles();
        if (children == null) return found;
        for (File child : children) {
            if (child.getName().endsWith(".part") && Files.isRegularFile(child.toPath(), LinkOption.NOFOLLOW_LINKS)) found.add(child);
        }
        return found;
    }

    /** The bytes a stopped or killed run left in disc-staging/. Read-only. */
    static long leftoverBytes(File root) {
        long bytes = 0;
        for (File left : leftovers(root)) bytes += left.length();
        return bytes;
    }

    /** The free bytes a copy can count on: what the file system reports plus what the copy itself removes first. */
    static long freeWithLeftovers(File root, VerifiedExtractor.Space space) {
        long free = space.usable(VerifiedExtractor.existingAncestor(root)), left = leftoverBytes(root);
        return free > Long.MAX_VALUE - left ? Long.MAX_VALUE : free + left;
    }

    /**
     * For the app's first screen (ShipFirstRun), which must not be refused for lack of room because of a half file a
     * killed copy left behind: removes what lies in disc-staging/, unless a copy or an import runs right now (the
     * same lock run() takes). root: files/private-trace. Returns the bytes removed; 0 while a copy runs, when there
     * is nothing, or when a file could not be removed. Never throws.
     */
    static long removeLeftOvers(File root) {
        if (!VerifiedExtractor.begin()) return 0;
        try {
            return clearLeftovers(root);
        } catch (IOException | RuntimeException kept) {
            return 0;
        } finally {
            VerifiedExtractor.end();
        }
    }

    /** Removes what a stopped or killed run left in disc-staging/; returns the bytes removed. */
    static long clearLeftovers(File root) throws IOException {
        long removed = 0;
        for (File left : leftovers(root)) {
            long bytes = left.length();
            Files.delete(left.toPath());
            removed += bytes;
        }
        return removed;
    }

    // ---- what a disc is to provide --------------------------------------------------------------------------------

    static long bytes(Collection<Disc2Discs.Pin> pins) {
        long total = 0;
        for (Disc2Discs.Pin p : pins) total += p.bytes;
        return total;
    }

    /** The entries of pins whose SHA-256 is not in installed, each once, in the order given. */
    static List<Disc2Discs.Pin> notIn(List<Disc2Discs.Pin> pins, Set<String> installed) {
        List<Disc2Discs.Pin> out = new ArrayList<Disc2Discs.Pin>();
        Set<String> seen = new HashSet<String>();
        for (Disc2Discs.Pin p : pins) {
            if (seen.add(p.sha256 + "/" + p.bytes) && !installed.contains(p.sha256)) out.add(p);
        }
        return out;
    }

    // ---- the copy ------------------------------------------------------------------------------------------------------

    /**
     * Copies what disc offers. expected: the entries this disc is to provide; allPins: every entry that comes from a
     * disc (a file of disc whose size and SHA-256 equal any of them is kept); notTrusted: SHA-256 values of entries
     * whose file is there by name and size but is known not to be the pinned file (ShipData.different): they count as
     * not in place and are replaced. root: files/private-trace. Never throws.
     */
    static Outcome run(Disc2Discs.Disc disc, List<Disc2Discs.Pin> expected, List<Disc2Discs.Pin> allPins, Collection<String> notTrusted,
            File root, Store store, Progress progress, VerifiedExtractor.Space space, Report report) {
        if (!VerifiedExtractor.begin()) {
            return failed(VerifiedExtractor.Problem.BUSY, VerifiedExtractor.BUSY_MESSAGE, 0, 0, 0, null, new ArrayList<FileResult>(), expected);
        }
        try {
            return runLocked(disc, expected, allPins, notTrusted, root, store, progress, space, report);
        } finally {
            VerifiedExtractor.end();
        }
    }

    private static Outcome runLocked(Disc2Discs.Disc disc, List<Disc2Discs.Pin> expected, List<Disc2Discs.Pin> allPins,
            Collection<String> notTrusted, File root, Store store, Progress progress, VerifiedExtractor.Space space, Report report) {
        List<FileResult> files = new ArrayList<FileResult>();
        if (!disc.usable()) {
            return new Outcome(false, false, VerifiedExtractor.Problem.DISC_COPY_BAD, disc.logged(), disc.message, 0, 0, 0, 0, 0, files,
                    expected);
        }
        Set<String> installed = store.installed(allPins);
        installed.removeAll(notTrusted);
        // Asked before anything is written: does this disc hold a file of the right size for each of its entries?
        Disc2Discs.Plan own = Disc2Discs.plan(disc.candidates, expected, installed);
        if (!own.shortSizes.isEmpty()) {
            return failed(VerifiedExtractor.Problem.DISC_COPY_BAD, "This disc image lacks " + own.shortSizes.size() + " of the "
                    + expected.size() + " files this disc should hold.", 0, 0, 0, null, files, notIn(expected, installed));
        }
        Disc2Discs.Plan plan = Disc2Discs.plan(disc.candidates, allPins, installed);
        int count = plan.read.size();
        long leftovers = 0;
        Set<String> keptNow = new HashSet<String>();
        try {
            if (count > 0) {
                // What a stopped or killed run left is this copy's own and is never read again: it goes before the free
                // space is asked for, or it would be counted against the copy that replaces it.
                leftovers = store.prepare();
                if (store.writes()) {
                    long need = bytes(notIn(expected, installed)) + RESERVE_BYTES;
                    long free = space.usable(VerifiedExtractor.existingAncestor(root));
                    if (need > free) {
                        return noSpace("Not enough space", need, free, leftovers, plan, files, notIn(expected, installed));
                    }
                }
            }
            long done = 0;
            for (int i = 0; i < count; ++i) {
                Disc2Discs.Candidate c = plan.read.get(i);
                if (progress.cancelled()) return stopped(leftovers, plan, files, notIn(expected, installed));
                if (store.writes() && i > 0) {
                    // Other apps use the same storage while this runs: every file starts with the same check.
                    long need = Math.max(bytes(notIn(expected, installed)), c.bytes) + RESERVE_BYTES;
                    long free = space.usable(VerifiedExtractor.existingAncestor(root));
                    if (need > free) {
                        return noSpace("Not enough space left for " + c.name, need, free, leftovers, plan, files, notIn(expected, installed));
                    }
                }
                progress.update(done, plan.bytes, i, count, c.name);
                String sha;
                try {
                    sha = copyOne(c, store, progress, done, plan.bytes, i, count);
                } catch (Disc2Discs.Stopped e) {
                    return stopped(leftovers, plan, files, notIn(expected, installed));
                } catch (WriteFailed e) {
                    long free = space.usable(VerifiedExtractor.existingAncestor(root));
                    long need = Math.max(bytes(notIn(expected, installed)), c.bytes) + RESERVE_BYTES;
                    if (need > free) {
                        return noSpace("This device ran out of space while copying " + c.name, need, free, leftovers, plan, files,
                                notIn(expected, installed));
                    }
                    return failed(VerifiedExtractor.Problem.STORAGE, "Could not write the copy of " + c.name + ": " + e.getMessage() + ".",
                            shown(i, count, SHOWN_NOT_WRITTEN), leftovers, plan, files, notIn(expected, installed));
                } catch (Disc2Stfs.Damaged e) {
                    return failed(VerifiedExtractor.Problem.DISC_COPY_BAD, c.name + " is damaged in your disc image (" + e.getMessage() + ").",
                            shown(i, count, SHOWN_DAMAGED), leftovers, plan, files, notIn(expected, installed));
                } catch (EOFException e) {
                    return failed(VerifiedExtractor.Problem.DISC_COPY_BAD, c.name + " is incomplete in your disc image ("
                            + VerifiedExtractor.reason(e) + ").", shown(i, count, SHOWN_INCOMPLETE), leftovers, plan, files,
                            notIn(expected, installed));
                } catch (IOException e) {
                    return failed(VerifiedExtractor.Problem.SOURCE_UNREADABLE, c.name + " could not be read from your disc image ("
                            + VerifiedExtractor.reason(e) + ").", shown(i, count, SHOWN_NOT_READ), leftovers, plan, files,
                            notIn(expected, installed));
                }
                Disc2Discs.Verdict verdict = plan.judge(c.bytes, sha);
                // The same content offered twice by one disc: the second is not wanted any more.
                if (verdict == Disc2Discs.Verdict.WANTED && installed.contains(sha)) verdict = Disc2Discs.Verdict.ALREADY_INSTALLED;
                boolean kept = false;
                if (verdict == Disc2Discs.Verdict.WANTED) {
                    try {
                        store.keep(sha);
                        kept = true;
                        installed.add(sha);
                        keptNow.add(sha);
                    } catch (IOException | RuntimeException e) {
                        store.discard();
                        return failed(VerifiedExtractor.Problem.STORAGE, "Could not put " + c.name + " in place: " + VerifiedExtractor.reason(e) + ".",
                                shown(i, count, SHOWN_NOT_KEPT), leftovers, plan, files, notIn(expected, installed));
                    }
                } else {
                    store.discard();
                }
                FileResult result = new FileResult(i, count, c.name, c.bytes, sha, verdict, kept);
                files.add(result);
                if (report != null) report.file(result);
                done += c.bytes;
            }
            if (count > 0) progress.update(plan.bytes, plan.bytes, count, count, "");
            // Asked after the copy, of what is really in place.
            Set<String> after = store.installed(allPins);
            for (String distrusted : notTrusted) {
                if (!keptNow.contains(distrusted)) after.remove(distrusted);   // still the file that is not the pinned one
            }
            List<Disc2Discs.Pin> missing = notIn(expected, after);
            if (!missing.isEmpty()) {
                String error = "This disc image did not provide " + missing.size() + " of the " + expected.size() + " files this disc should hold.";
                List<FileResult> damaged = failedCheck(files, missing);
                if (damaged.isEmpty()) return failed(VerifiedExtractor.Problem.DISC_COPY_BAD, error, 0, 0, leftovers, plan, files, missing);
                // A file of a missing entry's size was read to its end and its SHA-256 is not the pinned one: that file is
                // damaged in the image (or the image is not of the supported disc). The page says which file, by its
                // number in the copy line, as for a package block that fails its hash (setup-fixes, C5-1).
                StringBuilder names = new StringBuilder();
                for (FileResult f : damaged) names.append(names.length() == 0 ? "" : ", ").append(f.name);
                String shown = shownDamaged(damaged, count);
                if (damaged.size() < missing.size()) {
                    int other = missing.size() - damaged.size();
                    shown += " It did not provide " + (other == 1 ? "1 other file" : other + " other files") + " this disc should hold.";
                }
                return failed(VerifiedExtractor.Problem.DISC_COPY_BAD, error + " Failed its check: " + names + ".", shown, leftovers, plan,
                        files, missing);
            }
            return new Outcome(true, false, VerifiedExtractor.Problem.NONE, null, 0, 0, leftovers, count, plan.bytes, files, missing);
        } catch (IOException e) {
            store.discard();
            return failed(VerifiedExtractor.Problem.STORAGE, "Could not prepare the game files' folder: " + VerifiedExtractor.reason(e) + ".",
                    SHOWN_NOT_PREPARED, leftovers, plan, files, notIn(expected, installed));
        } catch (RuntimeException e) {
            store.discard();
            return failed(VerifiedExtractor.Problem.OTHER, "Copying stopped unexpectedly: " + VerifiedExtractor.reason(e) + ".",
                    SHOWN_UNEXPECTED, leftovers, plan, files, notIn(expected, installed));
        }
    }

    /** Reads one candidate into the store while hashing it; returns its SHA-256. The part file is removed on any failure. */
    private static String copyOne(Disc2Discs.Candidate c, Store store, final Progress progress, final long doneBefore, final long total,
            final int index, final int count) throws IOException {
        boolean finished = false;
        final String name = c.name;
        try {
            OutputStream out = store.open();
            String sha = Disc2Discs.sha256(c, out, new Disc2Discs.Watch() {
                @Override public boolean more(long bytesDone) {
                    progress.update(doneBefore + bytesDone, total, index, count, name);
                    return !progress.cancelled();
                }
            });
            store.finish();
            finished = true;
            return sha;
        } finally {
            if (!finished) store.discard();
        }
    }

    private static Outcome stopped(long leftovers, Disc2Discs.Plan plan, List<FileResult> files, List<Disc2Discs.Pin> missing) {
        return new Outcome(false, true, VerifiedExtractor.Problem.NONE, null, 0, 0, leftovers, plan.read.size(), plan.bytes, files, missing);
    }

    // ---- what the page says of a failure ---------------------------------------------------------------------------
    // The first five follow "File k of n": the numbers the copy line showed. No name of a file of the disc or of the
    // app's own storage, and no reason as an exception gave it (a package's reason names the file inside it; the
    // storage's reason names the app's own folders). The log's sentence (Outcome.error) keeps both.

    static final String SHOWN_DAMAGED = " is damaged in your disc image.";
    static final String SHOWN_INCOMPLETE = " is incomplete in your disc image.";
    static final String SHOWN_NOT_READ = " could not be read from your disc image.";
    static final String SHOWN_NOT_WRITTEN = " could not be written to this device's storage.";
    static final String SHOWN_NOT_KEPT = " was copied but could not be put in place on this device's storage.";
    static final String SHOWN_NOT_PREPARED = "The folder for the game's files could not be prepared on this device's storage.";
    static final String SHOWN_UNEXPECTED = "Copying stopped unexpectedly.";

    /** After "Files 3 and 10 of 10": more than one file failed its check. */
    static final String SHOWN_DAMAGED_MANY = " are damaged in your disc image.";

    /**
     * The files read to their end whose SHA-256 is no pinned one while an entry of their size is still missing: each
     * missing entry explains at most one of them, in the order they were read.
     */
    static List<FileResult> failedCheck(List<FileResult> files, List<Disc2Discs.Pin> missing) {
        List<Long> sizes = new ArrayList<Long>();
        for (Disc2Discs.Pin p : missing) sizes.add(Long.valueOf(p.bytes));
        List<FileResult> out = new ArrayList<FileResult>();
        for (FileResult f : files) {
            if (f.verdict == Disc2Discs.Verdict.NOT_PINNED && sizes.remove(Long.valueOf(f.bytes))) out.add(f);
        }
        return out;
    }

    /** "File 10 of 10 is damaged in your disc image." or "Files 3 and 10 of 10 are damaged in your disc image." */
    static String shownDamaged(List<FileResult> damaged, int count) {
        if (damaged.size() == 1) return shown(damaged.get(0).index, count, SHOWN_DAMAGED);
        StringBuilder out = new StringBuilder("Files ");
        for (int i = 0; i < damaged.size(); ++i) {
            if (i > 0) out.append(i == damaged.size() - 1 ? " and " : ", ");
            out.append(damaged.get(i).index + 1);
        }
        return out.append(" of ").append(count).append(SHOWN_DAMAGED_MANY).toString();
    }

    /**
     * "File 3 of 8 is damaged in your disc image.": what the page says of a failure about one file. index is 0-based
     * among the files to read, so the sentence counts the file as the copy line did.
     */
    static String shown(int index, int count, String what) {
        return "File " + (index + 1) + " of " + count + what;
    }

    /**
     * what: for the log, and it may name the file the copy stopped at. The page does not show a sentence of this
     * failure: it says how much room is needed and free at the moment it is looked at (SetupDiscModel.failureText).
     */
    private static Outcome noSpace(String what, long need, long free, long leftovers, Disc2Discs.Plan plan, List<FileResult> files,
            List<Disc2Discs.Pin> missing) {
        String amounts = ": needs " + VerifiedExtractor.gigabytes(need, true) + ", free " + VerifiedExtractor.gigabytes(free, false) + ".";
        return new Outcome(false, false, VerifiedExtractor.Problem.NO_SPACE, what + amounts, "Not enough space" + amounts, need, free,
                leftovers, plan.read.size(), plan.bytes, files, missing);
    }

    /** A failure about one file or about the storage: error for the log, shown for the page. */
    private static Outcome failed(VerifiedExtractor.Problem problem, String error, String shown, long leftovers, Disc2Discs.Plan plan,
            List<FileResult> files, List<Disc2Discs.Pin> missing) {
        return new Outcome(false, false, problem, error, shown, 0, 0, leftovers, plan.read.size(), plan.bytes, files, missing);
    }

    private static Outcome failed(VerifiedExtractor.Problem problem, String error, long need, long free, long leftovers, Disc2Discs.Plan plan,
            List<FileResult> files, List<Disc2Discs.Pin> missing) {
        return new Outcome(false, false, problem, error, need, free, leftovers, plan == null ? 0 : plan.read.size(),
                plan == null ? 0 : plan.bytes, files, missing);
    }
}
