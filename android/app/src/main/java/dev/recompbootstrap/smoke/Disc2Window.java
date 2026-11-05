package dev.recompbootstrap.smoke;

import java.io.EOFException;
import java.io.IOException;

/**
 * A DiscSource that is a window [offset, offset + length) of another one: a file inside a disc image (for example an
 * install-disc package) read without copying it. Read-only. Closing the window does not close the source it looks
 * into; whoever opened that source closes it. Pure Java.
 */
final class Disc2Window implements DiscSource {
    private final DiscSource source;
    private final long offset, length;

    Disc2Window(DiscSource source, long offset, long length) throws IOException {
        if (offset < 0 || length < 0 || offset + length < offset) throw new IOException("a window with a negative size or position");
        if (offset + length > source.size()) throw new EOFException("the image is cut off inside this file");
        this.source = source;
        this.offset = offset;
        this.length = length;
    }

    @Override
    public long size() {
        return length;
    }

    @Override
    public void readFully(long position, byte[] buffer, int at, int count) throws IOException {
        if (position < 0 || count < 0 || at < 0 || at + count > buffer.length) {
            throw new IOException("read outside the buffer or before the start");
        }
        if (position + count > length) throw new EOFException("read past the end of the file");
        source.readFully(offset + position, buffer, at, count);
    }

    @Override
    public void close() {
        // the window owns nothing
    }
}
