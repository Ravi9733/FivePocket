package dev.recompbootstrap.smoke;

import java.io.Closeable;
import java.io.IOException;

/** Random-access, read-only bytes of a disc image or of one file of an extracted disc. Pure Java. */
interface DiscSource extends Closeable {
    long size() throws IOException;
    /** Reads exactly length bytes at position or throws java.io.EOFException. */
    void readFully(long position, byte[] buffer, int offset, int length) throws IOException;
}
