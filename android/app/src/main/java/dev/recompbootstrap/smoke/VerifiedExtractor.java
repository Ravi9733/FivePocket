package dev.recompbootstrap.smoke;

import java.io.EOFException;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Copies the catalog's files from the player's disc into files/game-install/inputs, checking every byte. Pure Java.
 *
 * Order of work: refuse when an install exists; check the free space; then file by file: check the free space
 * again, copy to staging/<sha256>.part while hashing, flush to storage, compare size and SHA-256, and only then
 * rename to inputs/<sha256>.bin. A file therefore carries its final name only after it was verified.
 * Resume: a final file with the catalog's size is skipped (and never rewritten); a .part file is started again.
 * What an interrupted run left in staging/ is removed before the first file is copied, and its bytes count as
 * free space in the check before that (they are freed by this very run).
 * A final file whose size is not the catalog's cannot be a verified copy (they are renamed into place whole): it is
 * left alone until a new copy of that file was verified, and is replaced by it in the same rename.
 */
final class VerifiedExtractor {
    static final String STAGING = "staging", INPUTS = "inputs";
    /** Space that must stay free beyond the bytes still to copy. */
    static final long RESERVE_BYTES = 512L << 20;
    private static final int BUFFER_BYTES = 1 << 20;

    interface Progress {
        void update(long bytesDone, long bytesTotal, int filesDone, int filesTotal, String currentPath);
        boolean cancelled();
    }

    /**
     * Why a copy or an import did not finish, as far as the next step for the player depends on it. The sentence
     * that says what happened is Outcome.error; SetupModel adds what to do.
     */
    enum Problem {
        NONE,
        /** Too little free space on this device (Outcome.needBytes and freeBytes say how much). */
        NO_SPACE,
        /** A file of the chosen disc copy is missing, too short, too long or has other contents: a new copy is needed. */
        DISC_COPY_BAD,
        /** Reading the chosen disc copy or bundle file failed (storage removed, permission gone): it may work when tried again. */
        SOURCE_UNREADABLE,
        /** The chosen file is not a code bundle at all. */
        NOT_A_BUNDLE,
        /** The bundle file is incomplete or was changed after it was built: copy it to the device again. */
        BUNDLE_DAMAGED,
        /** The bundle is for another app version or was built from another game. */
        BUNDLE_OTHER,
        /** Writing to this device failed for another reason than space. */
        STORAGE,
        /** An install already exists; nothing was changed. */
        EXISTING_INSTALL,
        /** Another copy or import still holds the lock. */
        BUSY,
        OTHER
    }

    static final class Outcome {
        final boolean complete, cancelled;
        /** null, or one plain sentence that names the file. */
        final String error;
        /** NONE unless error is set. */
        final Problem problem;
        /** For NO_SPACE: the free bytes the work needs (its reserve included) and the free bytes found. Else 0. */
        final long needBytes, freeBytes;

        private Outcome(boolean complete, boolean cancelled, String error, Problem problem, long needBytes, long freeBytes) {
            this.complete = complete;
            this.cancelled = cancelled;
            this.error = error;
            this.problem = problem;
            this.needBytes = needBytes;
            this.freeBytes = freeBytes;
        }

        static Outcome done() { return new Outcome(true, false, null, Problem.NONE, 0, 0); }
        static Outcome stopped() { return new Outcome(false, true, null, Problem.NONE, 0, 0); }
        static Outcome failed(String error) { return failed(Problem.OTHER, error); }
        static Outcome failed(Problem problem, String error) { return new Outcome(false, false, error, problem, 0, 0); }

        /** what: the beginning of the sentence, for example "Not enough space". The two amounts are added to it. */
        static Outcome noSpace(String what, long needBytes, long freeBytes) {
            return new Outcome(false, false, what + ": needs " + gigabytes(needBytes, true) + ", free " + gigabytes(freeBytes, false) + ".",
                    Problem.NO_SPACE, needBytes, freeBytes);
        }
    }

    interface ItemReader {
        /** The item's bytes from the player's disc. FileNotFoundException when the disc does not have it. */
        InputStream open(SetupCatalog.Item item) throws IOException;
    }

    /** Usable bytes of the file system that holds an existing directory. */
    interface Space {
        long usable(File existingDirectory);
    }

    static final Space DISK = new Space() {
        @Override
        public long usable(File existingDirectory) {
            return existingDirectory.getUsableSpace();
        }
    };

    /** One copy or import at a time in this process: two runs would fight over the same staging files. */
    private static final AtomicBoolean RUNNING = new AtomicBoolean();
    static final String BUSY_MESSAGE = "The last copy or import has not stopped yet.";
    static final String EXISTING_MESSAGE = "An install already exists and was not changed. ";

    static boolean begin() {
        return RUNNING.compareAndSet(false, true);
    }

    static void end() {
        RUNNING.set(false);
    }

    private VerifiedExtractor() {}

    static Outcome extract(List<SetupCatalog.Item> items, ItemReader reader, File root, Progress progress) {
        return extract(items, reader, root, progress, SetupCatalog.spec(), DISK);
    }

    /** As above with the catalog and the free-space source given (tests use synthetic ones). */
    static Outcome extract(List<SetupCatalog.Item> items, ItemReader reader, File root, Progress progress,
            SetupCatalog.Spec spec, Space space) {
        if (!begin()) return Outcome.failed(Problem.BUSY, BUSY_MESSAGE);
        try {
            return extractLocked(items, reader, root, progress, spec, space);
        } finally {
            end();
        }
    }

    private static Outcome extractLocked(List<SetupCatalog.Item> items, ItemReader reader, File root, Progress progress,
            SetupCatalog.Spec spec, Space space) {
        try {
            String problem = InstallGuard.rootProblem(root);
            if (problem != null) return Outcome.failed(problem);
            String existing = InstallGuard.existingInstall(root.getAbsoluteFile().getParentFile());
            if (existing != null) return Outcome.failed(Problem.EXISTING_INSTALL, EXISTING_MESSAGE + existing);

            long total = 0, done = 0;
            int filesDone = 0;
            List<SetupCatalog.Item> todo = new ArrayList<SetupCatalog.Item>();
            for (SetupCatalog.Item item : items) {
                total += item.bytes;
                target(root, item);   // refuses a name that leads out of the setup directory before anything is done
                if (present(root, item)) {
                    done += item.bytes;
                    ++filesDone;
                } else {
                    todo.add(item);   // absent, or there with another size: see the class comment
                }
            }
            if (!todo.isEmpty()) {
                // What an interrupted run left in staging is removed below, so its bytes are free for this run.
                long free = freeWithLeftovers(root, space);
                long need = total - done + RESERVE_BYTES;
                if (need > free) return Outcome.noSpace("Not enough space", need, free);
                makeDirectory(InstallGuard.under(root, STAGING));
                makeDirectory(InstallGuard.under(root, INPUTS));
                clearLeftovers(root);
            }
            byte[] buffer = new byte[BUFFER_BYTES];
            boolean first = true;
            for (SetupCatalog.Item item : todo) {
                if (progress.cancelled()) return Outcome.stopped();
                progress.update(done, total, filesDone, items.size(), item.path);
                if (!present(root, item)) {   // the same file can be listed twice
                    if (!first) {
                        // Other apps use the same storage while this runs: every file starts with the same check.
                        long free = space.usable(existingAncestor(root));
                        long need = total - done + RESERVE_BYTES;
                        if (need > free) return Outcome.noSpace("Not enough space left for " + item.path, need, free);
                    }
                    Outcome failure = copyOne(item, reader, root, buffer, progress, done, total, filesDone, items.size(), space);
                    if (failure != null) return failure;
                }
                first = false;
                done += item.bytes;
                ++filesDone;
            }
            progress.update(total, total, items.size(), items.size(), "");
            if (root.isDirectory()) InstallGuard.refreshRecord(root, spec);
            return Outcome.done();
        } catch (IOException e) {
            return Outcome.failed(Problem.STORAGE, "Could not write the game files: " + reason(e) + ".");
        } catch (RuntimeException e) {
            return Outcome.failed("Copying stopped unexpectedly: " + reason(e) + ".");
        }
    }

    /** null when the item is now in place under its final name; else the outcome to return. */
    private static Outcome copyOne(SetupCatalog.Item item, ItemReader reader, File root, byte[] buffer, Progress progress,
            long doneBefore, long total, int filesDone, int filesTotal, Space space) throws IOException {
        File part = InstallGuard.under(root, STAGING + "/" + item.sha256 + ".part");
        File target = target(root, item);
        if (Files.isDirectory(target.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            return Outcome.failed(Problem.STORAGE, "Could not put " + item.path + " in place: a folder stands under its name.");
        }
        InputStream in;
        try {
            in = reader.open(item);
        } catch (FileNotFoundException e) {
            return Outcome.failed(Problem.DISC_COPY_BAD, item.path + " is missing from the disc copy you chose.");
        } catch (Xdvdfs.Damaged e) {   // a folder table of the image is wrong: reading it again gives the same
            return Outcome.failed(Problem.DISC_COPY_BAD, item.path + " cannot be found in your disc copy: the image is damaged (" + e.what() + ").");
        } catch (IOException e) {
            return Outcome.failed(Problem.SOURCE_UNREADABLE, item.path + " could not be read from your disc copy (" + reason(e) + ").");
        }
        MessageDigest sha = sha256();
        long copied = 0;
        String readFailure = null, cutOff = null;
        boolean cancelled = false, tooLong = false;
        try {
            FileChannel out = createNew(part);   // a .part file is always started again
            try {
                while (copied < item.bytes) {
                    if (progress.cancelled()) {
                        cancelled = true;
                        break;
                    }
                    int n;
                    try {
                        n = in.read(buffer, 0, (int) Math.min(buffer.length, item.bytes - copied));
                    } catch (EOFException e) {
                        cutOff = "the disc image ends inside it";   // rangeStream: the file's range passes the end of its source
                        break;
                    } catch (IOException e) {
                        readFailure = reason(e);
                        break;
                    }
                    if (n < 0) {
                        cutOff = "it ends after " + copied + " of " + item.bytes + " bytes";
                        break;
                    }
                    writeAll(out, buffer, n);
                    sha.update(buffer, 0, n);
                    copied += n;
                    progress.update(doneBefore + copied, total, filesDone, filesTotal, item.path);
                }
                if (!cancelled && readFailure == null && cutOff == null) {
                    try {
                        tooLong = in.read() >= 0;
                    } catch (IOException e) {
                        readFailure = reason(e);
                    }
                    out.force(true);
                }
            } finally {
                out.close();
            }
        } catch (IOException e) {
            // read failures are kept in readFailure, so this is the copy itself (storage full, staging not writable)
            part.delete();
            // Asked after the partial file is gone: with too little room left for this file, space is the reason to give.
            long free = space.usable(existingAncestor(root));
            long need = total - doneBefore + RESERVE_BYTES;
            if (need > free) return Outcome.noSpace("This device ran out of space while copying " + item.path, need, free);
            return Outcome.failed(Problem.STORAGE, "Could not write the copy of " + item.path + ": " + reason(e) + ".");
        } finally {
            try {
                in.close();
            } catch (IOException ignored) {
                // the source is read-only; nothing is lost
            }
        }
        if (cancelled) {
            part.delete();
            return Outcome.stopped();
        }
        if (readFailure != null) {
            part.delete();
            return Outcome.failed(Problem.SOURCE_UNREADABLE, item.path + " could not be read from your disc copy (" + readFailure + ").");
        }
        if (cutOff != null) {
            part.delete();
            return Outcome.failed(Problem.DISC_COPY_BAD, item.path + " is incomplete in your disc copy: " + cutOff + ".");
        }
        String found = hex(sha.digest());
        if (tooLong || !found.equals(item.sha256)) {
            part.delete();
            return Outcome.failed(Problem.DISC_COPY_BAD, item.path + " in your disc copy is not the file of the supported disc ("
                    + (tooLong ? "it is longer than " + item.bytes + " bytes" : "check code " + found.substring(0, 12)) + ").");
        }
        ownerReadOnly(part);
        // One step: the verified copy takes the final name. A file of another size that stood there (see the class
        // comment) is replaced in that same step and was untouched until now.
        Files.move(part.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        syncDirectory(target.getParentFile());
        return null;
    }

    /**
     * The bytes of what interrupted runs left in staging/: files named *.part and the tree staging/bundle.part.
     * Links are neither followed nor counted. Read-only.
     */
    static long leftoverBytes(File root) {
        final long[] bytes = {0};
        try {
            for (File left : leftovers(root)) {
                Files.walkFileTree(left.toPath(), new SimpleFileVisitor<Path>() {
                    @Override
                    public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) {
                        if (attributes.isRegularFile()) bytes[0] += attributes.size();
                        return FileVisitResult.CONTINUE;
                    }
                });
            }
        } catch (IOException e) {
            return 0;   // what cannot be looked at is not promised as free
        } catch (RuntimeException e) {
            return 0;
        }
        return bytes[0];
    }

    /** The free bytes a run can count on: what the file system reports plus what the run itself removes first. */
    static long freeWithLeftovers(File root, Space space) {
        long free = space.usable(existingAncestor(root)), left = leftoverBytes(root);
        return free > Long.MAX_VALUE - left ? Long.MAX_VALUE : free + left;
    }

    /**
     * Removes what interrupted runs left in staging/ (see leftoverBytes). Only one run works at a time, so nothing
     * in staging is in use when a run starts. A link standing there is not touched here: the path check refuses it
     * or the file creation replaces it.
     */
    static void clearLeftovers(File root) throws IOException {
        for (File left : leftovers(root)) {
            Files.walkFileTree(left.toPath(), new SimpleFileVisitor<Path>() {
                @Override
                public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) throws IOException {
                    Files.delete(file);   // a link met inside a tree is removed as a link, never followed
                    return FileVisitResult.CONTINUE;
                }

                @Override
                public FileVisitResult postVisitDirectory(Path directory, IOException error) throws IOException {
                    if (error != null) throw error;
                    Files.delete(directory);
                    return FileVisitResult.CONTINUE;
                }
            });
        }
    }

    /** staging/*.part that are real files or real directories. Empty when staging is absent or is itself a link. */
    private static List<File> leftovers(File root) {
        List<File> found = new ArrayList<File>();
        File staging = new File(root, STAGING);
        if (!Files.isDirectory(staging.toPath(), LinkOption.NOFOLLOW_LINKS)) return found;
        File[] children = staging.listFiles();
        if (children == null) return found;
        for (File child : children) {
            if (!child.getName().endsWith(".part") || Files.isSymbolicLink(child.toPath())) continue;
            found.add(child);
        }
        return found;
    }

    /** True when the item's verified copy is in place: a regular file inputs/<sha256>.bin of the catalog's size. */
    static boolean present(File root, SetupCatalog.Item item) {
        File target = new File(new File(root, INPUTS), item.sha256 + ".bin");
        return target.isFile() && target.length() == item.bytes;
    }

    private static File target(File root, SetupCatalog.Item item) throws IOException {
        return InstallGuard.under(root, INPUTS + "/" + item.sha256 + ".bin");
    }

    /** Reads the items out of a disc image through its file system. IOException when the image has none. */
    static ItemReader imageReader(final DiscSource image) throws IOException {
        final Xdvdfs fs = Xdvdfs.open(image);
        if (fs == null) throw new IOException("no Xbox 360 disc file system in this file");
        return new ItemReader() {
            @Override
            public InputStream open(SetupCatalog.Item item) throws IOException {
                Xdvdfs.Entry entry = fs.find(item.path);
                if (entry == null || entry.directory) throw new FileNotFoundException(item.path);
                return rangeStream(image, entry.offset, entry.size, false);
            }
        };
    }

    /** Reads the items out of an extracted disc folder. */
    static ItemReader folderReader(final DiscIdentifier.FolderSource folder) {
        return new ItemReader() {
            @Override
            public InputStream open(SetupCatalog.Item item) throws IOException {
                DiscSource file = folder.open(item.path);
                if (file == null) throw new FileNotFoundException(item.path);
                return rangeStream(file, 0, file.size(), true);
            }
        };
    }

    /** length bytes of source from offset on. A range that passes the end of the source fails with EOFException when read. */
    static InputStream rangeStream(final DiscSource source, final long offset, final long length, final boolean closeSource) {
        return new InputStream() {
            private long at = offset;
            private final long end = offset + length;

            @Override
            public int read() throws IOException {
                byte[] one = new byte[1];
                return read(one, 0, 1) < 0 ? -1 : one[0] & 0xFF;
            }

            @Override
            public int read(byte[] buffer, int off, int len) throws IOException {
                if (len == 0) return 0;
                if (at >= end) return -1;
                int n = (int) Math.min(len, end - at);
                if (at + n > source.size()) throw new EOFException("the source is cut off inside this file");
                source.readFully(at, buffer, off, n);
                at += n;
                return n;
            }

            @Override
            public void close() throws IOException {
                if (closeSource) source.close();
            }
        };
    }

    static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 is missing", e);
        }
    }

    static String hex(byte[] bytes) {
        char[] digits = "0123456789abcdef".toCharArray();
        char[] out = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; ++i) {
            out[i * 2] = digits[(bytes[i] >> 4) & 15];
            out[i * 2 + 1] = digits[bytes[i] & 15];
        }
        return new String(out);
    }

    /** Decimal gigabytes with one decimal; a need is rounded up and a free amount down, so the text never promises too much. */
    static String gigabytes(long bytes, boolean roundUp) {
        long tenths = roundUp ? (bytes + 99999999L) / 100000000L : bytes / 100000000L;
        return String.format(Locale.ROOT, "%d.%d GB", tenths / 10, tenths % 10);
    }

    /** The nearest directory at or above file that exists (free space can only be asked of an existing one). */
    static File existingAncestor(File file) {
        File at = file.getAbsoluteFile();
        while (at != null && !at.isDirectory()) at = at.getParentFile();
        return at == null ? file.getAbsoluteFile() : at;
    }

    /**
     * A new, empty file to write to. Whatever stands under that name - a left-over file or a symbolic link - is
     * removed first and the file is created new, so that nothing is ever written through a link to another place.
     */
    static FileChannel createNew(File file) throws IOException {
        Files.deleteIfExists(file.toPath());
        return FileChannel.open(file.toPath(), StandardOpenOption.WRITE, StandardOpenOption.CREATE_NEW);
    }

    static void writeAll(FileChannel out, byte[] buffer, int length) throws IOException {
        ByteBuffer bytes = ByteBuffer.wrap(buffer, 0, length);
        while (bytes.hasRemaining()) out.write(bytes);
    }

    /**
     * Makes sure directory exists as a real directory, creating it and missing parents one by one. A link or a file
     * standing under one of the names is an error; it is never followed and never replaced.
     */
    static void makeDirectory(File directory) throws IOException {
        if (Files.isDirectory(directory.toPath(), LinkOption.NOFOLLOW_LINKS)) return;
        File parent = directory.getAbsoluteFile().getParentFile();
        if (parent != null && !Files.isDirectory(parent.toPath(), LinkOption.NOFOLLOW_LINKS)) makeDirectory(parent);
        try {
            Files.createDirectory(directory.toPath());
        } catch (FileAlreadyExistsException e) {
            throw new IOException("cannot create " + directory.getName() + ": something else is in its place");
        }
    }

    /** Mode 0400: readable by the app only, not writable. */
    static void ownerReadOnly(File file) {
        file.setExecutable(false, false);
        file.setWritable(false, false);
        file.setReadable(false, false);
        file.setReadable(true, true);
    }

    static void syncDirectory(File directory) {
        try {
            FileChannel channel = FileChannel.open(directory.toPath(), StandardOpenOption.READ);
            try {
                channel.force(true);
            } finally {
                channel.close();
            }
        } catch (IOException ignored) {
            // some platforms cannot flush a directory; the rename itself already happened
        }
    }

    static String reason(Exception e) {
        String message = e.getMessage();
        return message == null || message.isEmpty() ? e.getClass().getSimpleName() : message;
    }
}
