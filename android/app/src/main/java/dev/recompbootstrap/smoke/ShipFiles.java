package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Collections;
import java.util.EnumSet;
import java.util.Locale;
import java.util.Set;

/**
 * File work shared by ShipFirstRun and ShipDataImport. Pure Java.
 *
 * The one rule both follow: a file gets its final name only after every byte of it was written, flushed and
 * compared with the size and SHA-256 on record. Until then it is a ".ship-part" file, which the next run starts
 * again. Nothing here follows a symbolic link for writing, and nothing here removes a directory that holds files.
 */
final class ShipFiles {
    static final int BUFFER_BYTES = 1 << 20;
    /** Mode 0400: what the game window's loader requires of the game code and of a start request. */
    static final Set<PosixFilePermission> OWNER_READ = Collections.unmodifiableSet(EnumSet.of(PosixFilePermission.OWNER_READ));
    /** Mode 0700: what the loader requires of files/private-trace and of a run directory. */
    static final Set<PosixFilePermission> OWNER_ONLY = Collections.unmodifiableSet(EnumSet.of(
            PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE, PosixFilePermission.OWNER_EXECUTE));
    /** Mode 0600: a file the app may write again (a record, a switch file). */
    static final Set<PosixFilePermission> OWNER_WRITE = Collections.unmodifiableSet(EnumSet.of(
            PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE));
    private static final char[] HEX = "0123456789abcdef".toCharArray();

    private ShipFiles() {}

    /** Usable bytes of the file system that holds an existing directory. */
    interface Space {
        long usable(File existingDirectory);
    }

    static final Space DISK = new Space() {
        @Override public long usable(File existingDirectory) {
            return existingDirectory.getUsableSpace();
        }
    };

    /** Told about every block that was copied; may stop the copy. */
    interface CopyWatch {
        /** bytes: of this file, so far. Return false to stop the copy (the part file is then removed). */
        boolean copied(long bytes);
    }

    static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException("SHA-256 is missing", impossible);
        }
    }

    static String hex(byte[] bytes) {
        char[] text = new char[bytes.length * 2];
        for (int index = 0; index < bytes.length; ++index) {
            text[index * 2] = HEX[(bytes[index] >> 4) & 15];
            text[index * 2 + 1] = HEX[bytes[index] & 15];
        }
        return new String(text);
    }

    static String sha256Hex(byte[] bytes) {
        return hex(sha256().digest(bytes));
    }

    /** True for exactly 64 lower-case hex digits. */
    static boolean isSha256(String text) {
        if (text == null || text.length() != 64) return false;
        for (int at = 0; at < 64; ++at) {
            char c = text.charAt(at);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        }
        return true;
    }

    /** SHA-256 of a regular file's bytes. A link is not followed. */
    static String sha256Of(File file) throws IOException {
        MessageDigest sha = sha256();
        try (FileChannel in = FileChannel.open(file.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            ByteBuffer buffer = ByteBuffer.allocate(BUFFER_BYTES);
            while (in.read(buffer) >= 0) {
                buffer.flip();
                sha.update(buffer);
                buffer.clear();
            }
        }
        return hex(sha.digest());
    }

    static boolean regularFile(File file) {
        try {
            return Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS);
        } catch (RuntimeException invalid) {
            return false;
        }
    }

    static boolean directory(File file) {
        try {
            return Files.isDirectory(file.toPath(), LinkOption.NOFOLLOW_LINKS);
        } catch (RuntimeException invalid) {
            return false;
        }
    }

    /**
     * True when file is a regular file and every folder between base and it is a real folder: no name on the way
     * from base down to the file is a symbolic link. base itself is taken as it is (Android reaches the app's files
     * directory through a link of its own). The rule for "this piece is in place": a file that is only reached
     * through a link somebody put under a folder's name is not in the app's storage.
     */
    static boolean regularFileIn(File base, File file) {
        if (!regularFile(file)) return false;
        File stop = base.getAbsoluteFile();
        for (File at = file.getAbsoluteFile().getParentFile(); at != null && !at.equals(stop); at = at.getParentFile()) {
            if (!directory(at)) return false;
        }
        return true;
    }

    /** True when anything stands under that name, also a link that points nowhere. */
    static boolean present(File file) {
        try {
            return Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS);
        } catch (RuntimeException invalid) {
            return false;
        }
    }

    /** "r--------" and the like; "" when the file system has no such permissions or the file is missing. */
    static String mode(File file) {
        try {
            return PosixFilePermissions.toString(Files.getPosixFilePermissions(file.toPath(), LinkOption.NOFOLLOW_LINKS));
        } catch (IOException | RuntimeException unknown) {
            return "";
        }
    }

    static void setMode(File file, Set<PosixFilePermission> mode) throws IOException {
        Files.setPosixFilePermissions(file.toPath(), mode);
    }

    /** How many names the file has, or -1 when this platform cannot tell. The loader refuses game code with more than one. */
    static int linkCount(File file) {
        try {
            Object count = Files.getAttribute(file.toPath(), "unix:nlink", LinkOption.NOFOLLOW_LINKS);
            return count instanceof Number ? ((Number) count).intValue() : -1;
        } catch (IOException | RuntimeException unknown) {
            return -1;
        }
    }

    /**
     * True when directory is a real folder and every folder between base and it is one too (no symbolic link on the
     * way from base down, base itself taken as it is): regularFileIn's rule for a folder.
     */
    static boolean directoryIn(File base, File directory) {
        File stop = base.getAbsoluteFile();
        for (File at = directory.getAbsoluteFile(); at != null && !at.equals(stop); at = at.getParentFile()) {
            if (!directory(at)) return false;
        }
        return true;
    }

    /** How makeDirectory's error ends when a file stands where a folder belongs (ShipSetupModel has a sentence for it). */
    static final String NOT_A_FOLDER = " exists and is not a folder";

    /**
     * Makes sure directory exists as a real directory with the given mode, creating missing parents (with the same
     * mode) one by one. A symbolic link under one of the names is never followed: the link itself is removed (what
     * it points to is not touched) and a real, empty folder takes its place. A file under one of the names is an
     * error and is left alone. An existing directory keeps its mode unless enforceMode is true.
     */
    static void makeDirectory(File directory, Set<PosixFilePermission> mode, boolean enforceMode) throws IOException {
        Path path = directory.toPath();
        if (Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) {
            if (enforceMode && !mode.equals(Files.getPosixFilePermissions(path, LinkOption.NOFOLLOW_LINKS))) {
                Files.setPosixFilePermissions(path, mode);
            }
            return;
        }
        if (Files.isSymbolicLink(path)) {
            Files.delete(path);   // the link, not what it points to
        } else if (Files.exists(path, LinkOption.NOFOLLOW_LINKS)) {
            throw new IOException(directory.getName() + NOT_A_FOLDER);
        }
        File parent = directory.getAbsoluteFile().getParentFile();
        if (parent != null && !Files.isDirectory(parent.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            makeDirectory(parent, mode, false);
        }
        try {
            Files.createDirectory(path);
        } catch (FileAlreadyExistsException raced) {
            throw new IOException(directory.getName() + NOT_A_FOLDER);
        }
        Files.setPosixFilePermissions(path, mode);
    }

    /**
     * makeDirectory for a folder of the app's own storage: every folder from base down to directory is made sure of
     * in turn, the outermost first, so that a symbolic link under the name of one of the folders on the way is
     * replaced as well (makeDirectory alone looks at the last name only and would reach it through the link). base
     * itself is taken as it is. Only directory gets the enforced mode.
     */
    static void makeDirectoryIn(File base, File directory, Set<PosixFilePermission> mode, boolean enforceMode) throws IOException {
        java.util.List<File> chain = new java.util.ArrayList<File>();
        File stop = base.getAbsoluteFile();
        for (File at = directory.getAbsoluteFile(); at != null && !at.equals(stop); at = at.getParentFile()) chain.add(0, at);
        for (int i = 0; i < chain.size(); ++i) makeDirectory(chain.get(i), mode, enforceMode && i == chain.size() - 1);
    }

    /** The whole file; null when it is absent or not a regular file. IOException when it is longer than max. */
    static byte[] readSmall(File file, int max) throws IOException {
        if (!regularFile(file)) return null;
        try (FileChannel in = FileChannel.open(file.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            long size = in.size();
            if (size > max) throw new IOException(file.getName() + " is longer than " + max + " bytes");
            ByteBuffer buffer = ByteBuffer.allocate((int) size);
            while (buffer.hasRemaining() && in.read(buffer) > 0) {
                // keep reading
            }
            if (buffer.hasRemaining()) throw new IOException(file.getName() + " changed while it was read");
            return buffer.array();
        }
    }

    /** Everything the stream holds, at most max bytes; IOException when there is more. Closes the stream. */
    static byte[] readAll(InputStream in, int max) throws IOException {
        try {
            byte[] data = new byte[Math.min(max, 1 << 16)];
            int length = 0;
            while (true) {
                if (length == data.length) {
                    if (data.length >= max) {
                        if (in.read() >= 0) throw new IOException("it is longer than " + max + " bytes");
                        break;
                    }
                    data = java.util.Arrays.copyOf(data, Math.min(max, data.length * 2));
                }
                int n = in.read(data, length, data.length - length);
                if (n < 0) break;
                length += n;
            }
            return java.util.Arrays.copyOf(data, length);
        } finally {
            in.close();
        }
    }

    /** The name a file carries while it is written. */
    static File partOf(File target) {
        return new File(target.getParentFile(), target.getName() + PART_SUFFIX);
    }

    /** The ending of the name a file carries while it is written. Nothing but this class's callers makes such names. */
    static final String PART_SUFFIX = ".ship-part";

    /**
     * Removes the left-over part files (names ending in ".ship-part") directly inside directory and, when depth is
     * more than 0, inside the directories below it to that depth. A part file is what a run that was killed inside
     * a copy leaves behind; it is never read again, only its room is needed. Links are not followed and nothing
     * else is removed. Returns the bytes that were freed. A directory that is absent or unreadable frees nothing.
     */
    static long removeParts(File directory, int depth) {
        File[] children;
        try {
            children = directory(directory) ? directory.listFiles() : null;
        } catch (RuntimeException unreadable) {
            children = null;
        }
        if (children == null) return 0;
        long freed = 0;
        for (File child : children) {
            if (regularFile(child)) {
                if (child.getName().endsWith(PART_SUFFIX)) freed += remove(child);
            } else if (depth > 0 && directory(child)) {
                freed += removeParts(child, depth - 1);
            }
        }
        return freed;
    }

    /** Removes every regular file directly inside directory (a staging folder of one class). Returns the bytes freed. */
    static long removeFiles(File directory) {
        File[] children;
        try {
            children = directory(directory) ? directory.listFiles() : null;
        } catch (RuntimeException unreadable) {
            children = null;
        }
        if (children == null) return 0;
        long freed = 0;
        for (File child : children) {
            if (regularFile(child)) freed += remove(child);
        }
        return freed;
    }

    private static long remove(File file) {
        long bytes = file.length();
        try {
            return Files.deleteIfExists(file.toPath()) ? bytes : 0;
        } catch (IOException | RuntimeException kept) {
            return 0;   // it stays and is counted as used space, which is the truth
        }
    }

    /**
     * A small file, complete or not there at all: written beside the target, flushed, given its mode, then renamed
     * over the target in one step. The target's folder must exist.
     */
    static void writeSmall(File target, byte[] content, Set<PosixFilePermission> mode) throws IOException {
        File part = partOf(target);
        try (FileChannel out = createNew(part)) {
            ByteBuffer buffer = ByteBuffer.wrap(content);
            while (buffer.hasRemaining()) out.write(buffer);
            out.force(true);
        }
        commit(part, target, mode);
    }

    /**
     * A new, empty file to write to. Whatever stands under that name (a left-over part file, a link) is removed
     * first and the file is created new, so that nothing is ever written through a link to another place.
     */
    static FileChannel createNew(File file) throws IOException {
        Files.deleteIfExists(file.toPath());
        return FileChannel.open(file.toPath(), StandardOpenOption.WRITE, StandardOpenOption.CREATE_NEW);
    }

    /** What copyVerified found. */
    enum Copy { VERIFIED, STOPPED, TOO_SHORT, TOO_LONG, WRONG_HASH }

    /**
     * Copies the stream into part (started again from nothing), flushes it to storage and compares it with the size
     * and SHA-256 on record. Anything but VERIFIED removes the part file. Closes the stream. An IOException is a
     * failure of reading or writing; the part file is removed then, too.
     */
    static Copy copyVerified(InputStream in, File part, long bytes, String sha256, CopyWatch watch) throws IOException {
        Copy result = null;
        try {
            MessageDigest sha = sha256();
            byte[] buffer = new byte[BUFFER_BYTES];
            long copied = 0;
            try (FileChannel out = createNew(part)) {
                while (copied < bytes) {
                    int n = in.read(buffer, 0, (int) Math.min(buffer.length, bytes - copied));
                    if (n < 0) break;
                    ByteBuffer block = ByteBuffer.wrap(buffer, 0, n);
                    while (block.hasRemaining()) out.write(block);
                    sha.update(buffer, 0, n);
                    copied += n;
                    if (watch != null && !watch.copied(copied)) {
                        result = Copy.STOPPED;
                        break;
                    }
                }
                if (result == null) {
                    if (copied < bytes) result = Copy.TOO_SHORT;
                    else if (in.read() >= 0) result = Copy.TOO_LONG;
                    else {
                        out.force(true);
                        result = hex(sha.digest()).equals(sha256) ? Copy.VERIFIED : Copy.WRONG_HASH;
                    }
                }
            }
            return result;
        } finally {
            if (result != Copy.VERIFIED) deleteQuietly(part);
            try {
                in.close();
            } catch (IOException ignored) {
                // the source is only read; nothing is lost
            }
        }
    }

    /** Gives a verified part file its mode and its final name in one step; replaces what carried that name. */
    static void commit(File part, File target, Set<PosixFilePermission> mode) throws IOException {
        try {
            setMode(part, mode);
            Files.move(part.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException | RuntimeException failed) {
            deleteQuietly(part);
            throw failed;
        }
        syncDirectory(target.getParentFile());
    }

    static void deleteQuietly(File file) {
        try {
            Files.deleteIfExists(file.toPath());
        } catch (IOException | RuntimeException ignored) {
            // the next run removes it before it writes
        }
    }

    static void syncDirectory(File directory) {
        if (directory == null) return;
        try (FileChannel channel = FileChannel.open(directory.toPath(), StandardOpenOption.READ)) {
            channel.force(true);
        } catch (IOException | RuntimeException ignored) {
            // some platforms cannot flush a directory; the rename itself already happened
        }
    }

    /** The nearest directory at or above file that exists (free space can only be asked of an existing one). */
    static File existingAncestor(File file) {
        File at = file.getAbsoluteFile();
        while (at != null && !at.isDirectory()) at = at.getParentFile();
        return at == null ? file.getAbsoluteFile() : at;
    }

    /** Decimal gigabytes with one decimal; a need is rounded up and a free amount down, so the text never promises too much. */
    static String gigabytes(long bytes, boolean roundUp) {
        long tenths = roundUp ? (bytes + 99999999L) / 100000000L : bytes / 100000000L;
        return String.format(Locale.ROOT, "%d.%d GB", tenths / 10, tenths % 10);
    }

    /**
     * An amount of storage for a sentence: "16.9 GB" from one gigabyte on, "480 MB" below. A need is rounded up and
     * a free amount down, so the text never promises too much.
     */
    static String amount(long bytes, boolean roundUp) {
        if (bytes >= 1000000000L) return gigabytes(bytes, roundUp);
        return (roundUp ? (bytes + 999999L) / 1000000L : bytes / 1000000L) + " MB";
    }

    /** "339 MB", "5 KB": decimal units as storage is sold, rounded down. */
    static String megabytes(long bytes) {
        if (bytes >= 1000000L) return bytes / 1000000L + " MB";
        return bytes / 1000L + " KB";
    }

    static String reason(Exception e) {
        String message = e.getMessage();
        return message == null || message.isEmpty() ? e.getClass().getSimpleName() : message;
    }
}
