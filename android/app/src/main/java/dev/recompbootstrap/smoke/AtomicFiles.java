package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;

/**
 * Whole-file writes that are either complete or not there at all (design section 3.5). Pure Java, no Android.
 *
 * A write goes to "&lt;name&gt;.tmp" beside the target, is flushed to the device, and is then renamed over the target in one
 * step. A reader (the game, or this app after a crash) therefore sees the old file or the new file, never a part of one.
 * Nothing here creates a directory, follows a symbolic link for writing, or removes a directory.
 */
final class AtomicFiles {
    private AtomicFiles() { }

    /** The temporary name used while target is written. */
    static File tempFor(File target) {
        return new File(target.getParentFile(), target.getName() + ".tmp");
    }

    /** Temporary file + fsync + atomic rename + directory fsync. Creates no directories. */
    static void write(File target, byte[] content) throws IOException {
        File temp = writeTemp(target, content);
        commit(temp, target);
    }

    /**
     * First half of write: the complete content, flushed to the device, under the temporary name. The target is not
     * touched. A crash after this step leaves the old target and a stray .tmp, which the next write of the same target
     * replaces. Returns the temporary file.
     */
    static File writeTemp(File target, byte[] content) throws IOException {
        if (content == null) throw new IOException("No content for " + target.getName());
        File directory = target.getParentFile();
        if (directory == null || !directory.isDirectory()) throw new IOException("The folder of " + target.getName() + " does not exist");
        if (Files.isDirectory(target.toPath(), LinkOption.NOFOLLOW_LINKS)) throw new IOException(target.getName() + " is a folder, not a file");
        File temp = tempFor(target);
        Path tempPath = temp.toPath();
        if (Files.isDirectory(tempPath, LinkOption.NOFOLLOW_LINKS)) throw new IOException(temp.getName() + " is a folder; it is not replaced");
        // A leftover from an interrupted write is replaced. It is unlinked first and the new file is created with
        // CREATE_NEW, so that a symbolic link standing under the temporary name is never written through.
        Files.deleteIfExists(tempPath);
        FileChannel channel = FileChannel.open(tempPath, StandardOpenOption.WRITE, StandardOpenOption.CREATE_NEW);
        boolean complete = false;
        try {
            try {
                ByteBuffer buffer = ByteBuffer.wrap(content);
                while (buffer.hasRemaining()) channel.write(buffer);
                channel.force(true);
            } finally {
                channel.close();
            }
            complete = true;
        } finally {
            if (!complete) Files.deleteIfExists(tempPath); // the incomplete file this call created
        }
        return temp;
    }

    /** Second half of write: the rename over the target, then a flush of the directory entry. */
    static void commit(File temp, File target) throws IOException {
        try {
            Files.move(temp.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException | RuntimeException failed) {
            try {
                Files.deleteIfExists(temp.toPath());
            } catch (IOException ignored) {
                // the first failure is the one to report
            }
            throw failed;
        }
        syncDirectory(target.getParentFile());
    }

    /** Returns true if a file was removed, false if there was none. Refuses directories. */
    static boolean remove(File target) throws IOException {
        Path path = target.toPath();
        if (!Files.exists(path, LinkOption.NOFOLLOW_LINKS)) return false;
        if (Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) throw new IOException(target.getName() + " is a folder; it is not removed");
        Files.delete(path);
        syncDirectory(target.getParentFile());
        return true;
    }

    /** Flushes a directory entry. Some platforms cannot open a directory for this; that one failure is ignored. */
    private static void syncDirectory(File directory) {
        if (directory == null) return;
        try {
            FileChannel channel = FileChannel.open(directory.toPath(), StandardOpenOption.READ);
            try {
                channel.force(true);
            } finally {
                channel.close();
            }
        } catch (IOException | RuntimeException ignored) {
            // the rename itself already happened; durability of the directory entry is then up to the file system
        }
    }
}
