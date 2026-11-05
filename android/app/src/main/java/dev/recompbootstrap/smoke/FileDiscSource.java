package dev.recompbootstrap.smoke;

import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/** A DiscSource over an ordinary file, opened read-only. Pure Java. */
final class FileDiscSource implements DiscSource {
    private final RandomAccessFile file;

    FileDiscSource(File file) throws IOException {
        this.file = new RandomAccessFile(file, "r");
    }

    @Override
    public long size() throws IOException {
        return file.length();
    }

    @Override
    public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
        if (position < 0 || length < 0 || offset < 0 || offset + length > buffer.length) {
            throw new IOException("read outside the buffer or before the start");
        }
        // The length check comes first so that a position past the end is an EOFException, never a seek error.
        if (position + length > file.length()) throw new EOFException("read past the end of the source");
        file.seek(position);
        file.readFully(buffer, offset, length);
    }

    @Override
    public void close() throws IOException {
        file.close();
    }
}
