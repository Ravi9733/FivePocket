package dev.recompbootstrap.smoke;

import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.BitSet;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * Read-only reader for the PIRS STFS packages on the install disc. Pure Java.
 *
 * A port of the project's own host reader, tools/host_guest_trace/stfs_inventory.py (class Package): the same header
 * fields, the same hash-table positions, the same directory and chain checks, the same refusals. What differs:
 *  - the package is any DiscSource (a file, or a window of a disc image: Disc2Window), never loaded whole;
 *  - hash tables are kept in a small cache that is emptied when full (a re-read table is checked again up to the
 *    root hash), instead of a 32 MiB cap that refuses;
 *  - an entry is read as a stream (openEntry) that checks the SHA-1 of every block before handing out its bytes, in
 *    runs of up to 64 blocks per read; extractTo writes it to a new file with a running SHA-256.
 * As in the Python tool: the hash tree and the directory and chain structure are checked; Microsoft signatures and
 * licences are not authenticated; no decryption; CON, LIVE, SVOD and packages with two hash tables are refused.
 */
final class Disc2Stfs {
    static final int BLOCK = 4096;
    static final int END = 0xFFFFFF;
    static final long MAX_PACKAGE = 8L << 30;
    static final int MAX_ENTRIES = 4096;
    static final int HEADER_BYTES = 0x971A;
    private static final int MAX_BLOCKS = 170 * 170 * 170;
    private static final int TABLE_CACHE = 256;          // tables kept (1 MiB)
    private static final int RUN_BLOCKS = 64;            // blocks per read while streaming (256 KiB)

    /** The package's hash tree or structure contradicts itself: damaged, or not what it claims to be. */
    static final class Damaged extends IOException {
        private static final long serialVersionUID = 1L;

        Damaged(String message) {
            super(message);
        }
    }

    /**
     * The file is intact as far as it was looked at, but it is not a package this reader takes (another container,
     * type, version, title or volume format). Reading it again gives the same result. A plain IOException from
     * open(), by contrast, comes from the source: the read itself failed.
     */
    static final class Refused extends IOException {
        private static final long serialVersionUID = 1L;

        Refused(String message) {
            super(message);
        }
    }

    static final class Entry {
        final int index;
        /** Path inside the package, parts joined by '/', as stored. */
        final String path;
        final long size;
        final int[] blocks;
        final boolean directory;
        final long created, modified;

        Entry(int index, String path, long size, int[] blocks, boolean directory, long created, long modified) {
            this.index = index;
            this.path = path;
            this.size = size;
            this.blocks = blocks;
            this.directory = directory;
            this.created = created;
            this.modified = modified;
        }
    }

    interface Cancel {
        boolean cancelled();
    }

    private final DiscSource source;
    private final long size, base, titleId;
    private final int total, directoryCount, firstDirectory, rootLevel;
    private final byte[] rootHash, displayBytes;
    private final BitSet claimed = new BitSet();
    private final Map<Long, byte[]> tables = new HashMap<Long, byte[]>();
    private final MessageDigest sha1 = digest("SHA-1");
    private final List<Entry> entries;

    private Disc2Stfs(DiscSource source, long size, long base, long titleId, int total, int directoryCount, int firstDirectory,
            byte[] rootHash, byte[] displayBytes) throws IOException {
        this.source = source;
        this.size = size;
        this.base = base;
        this.titleId = titleId;
        this.total = total;
        this.directoryCount = directoryCount;
        this.firstDirectory = firstDirectory;
        this.rootLevel = total <= 170 ? 0 : total <= 28900 ? 1 : 2;
        this.rootHash = rootHash;
        this.displayBytes = displayBytes;
        this.entries = Collections.unmodifiableList(directory());
        checkUnchanged();
    }

    /**
     * Opens a package and checks its header, its hash tree as far as the directory and every file's chain reach,
     * and the directory. The file data itself is checked block by block when it is read. Never writes.
     */
    static Disc2Stfs open(DiscSource source, long titleId) throws IOException {
        long size = source.size();
        if (size < HEADER_BYTES || size > MAX_PACKAGE) {
            throw new Refused("not a package this app reads: " + size + " bytes is outside 38,682 bytes to 8 GiB");
        }
        byte[] h = new byte[HEADER_BYTES];
        source.readFully(0, h, 0, HEADER_BYTES);
        if (h[0] != 'P' || h[1] != 'I' || h[2] != 'R' || h[3] != 'S') throw new Refused("not a PIRS package");
        long version = be32(h, 0x348);
        if (be32(h, 0x344) != 2 || (version != 1 && version != 2)) {
            throw new Refused("a package type or version this app does not read");
        }
        long title = be32(h, 0x360);
        if (title != titleId) {
            throw new Refused("a package of another game (title " + String.format(Locale.ROOT, "%08X", title) + ")");
        }
        long headerSize = be32(h, 0x340);
        if (headerSize < HEADER_BYTES || headerSize > Math.min(size, 1L << 20)) throw new Damaged("damaged package: its header size is impossible");
        long base = (headerSize + BLOCK - 1) & -BLOCK;
        int v = 0x379;
        if ((h[v] & 0xFF) != 0x24 || h[v + 1] != 0 || h[v + 2] != 1 || !zero(h, 0x39D, 0x3AD)) {
            throw new Refused("only single-file read-only packages of format version zero are read");
        }
        long total = be32(h, v + 28), free = be32(h, v + 32);
        int directoryCount = (h[v + 3] & 0xFF) | (h[v + 4] & 0xFF) << 8;
        int firstDirectory = le24(h, v + 5);
        if (total < 1 || total > Math.min(MAX_BLOCKS, size / BLOCK) || free != 0
                || directoryCount < 1 || directoryCount > MAX_ENTRIES / 64
                || dataOffset(total - 1, base) + BLOCK > size) {
            throw new Damaged("damaged package: its block counts do not fit the file (it may be cut off)");
        }
        byte[] rootHash = Arrays.copyOfRange(h, v + 8, v + 28);
        String display;
        try {
            display = StandardCharsets.UTF_16BE.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(h, 0x411, 256)).toString();
        } catch (CharacterCodingException e) {
            throw new Damaged("damaged package: its display name is not valid text");
        }
        int nul = display.indexOf('\0');
        if (nul >= 0) display = display.substring(0, nul);
        byte[] displayBytes = Arrays.copyOf(display.getBytes(StandardCharsets.UTF_16BE), 256);
        return new Disc2Stfs(source, size, base, titleId, (int) total, directoryCount, firstDirectory, rootHash, displayBytes);
    }

    List<Entry> entries() {
        return entries;
    }

    /** The entry with this path (case-insensitive, '/' separated), or null. */
    Entry find(String path) {
        for (Entry e : entries) {
            if (e.path.equalsIgnoreCase(path)) return e;
        }
        return null;
    }

    long titleId() {
        return titleId;
    }

    int dataBlocks() {
        return total;
    }

    /** The 256-byte UTF-16BE display name, cut at its first NUL and padded with NULs (as stfs_inventory.py keeps it). */
    byte[] displayBytes() {
        return displayBytes.clone();
    }

    /**
     * This package's 308-byte record of the installed-content catalog, byte for byte as stfs_inventory.py writes it:
     * source 1 and type 2 (big-endian), the display name, then the package's file name padded to 44 bytes.
     */
    byte[] contentRecord(String packageName) throws IOException {
        if (!validPackageName(packageName)) throw new IOException("a package name must be 1 to 40 letters and digits");
        ByteBuffer out = ByteBuffer.allocate(308);
        out.putInt(1).putInt(2).put(displayBytes).put(Arrays.copyOf(packageName.getBytes(StandardCharsets.US_ASCII), 44));
        return out.array();
    }

    static boolean validPackageName(String name) {
        if (name == null || name.isEmpty() || name.length() > 40) return false;
        for (int i = 0; i < name.length(); ++i) {
            char c = name.charAt(i);
            if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9')) return false;
        }
        return true;
    }

    // ---- layout (identical to stfs_inventory.py data_offset / table_offset) -------------------------------------

    static long dataOffset(long block, long base) {
        long physical = block;
        long[] coverage = {170L, 28900L, 4913000L};
        for (long c : coverage) {
            physical += (block + c) / c;
            if (block < c) break;
        }
        return base + physical * BLOCK;
    }

    static long tableOffset(long block, int level, long base) {
        long physical;
        if (level == 0) {
            physical = block < 170 ? 0 : (block / 170) * 171 + block / 28900 + 1 + (block >= 28900 ? 1 : 0);
        } else if (level == 1) {
            physical = block < 28900 ? 171 : (block / 28900) * 29071 + 1;
        } else if (level == 2) {
            physical = 29071;
        } else {
            throw new IllegalArgumentException("unsupported hash level");
        }
        return base + physical * BLOCK;
    }

    // ---- hash tree ----------------------------------------------------------------------------------------------

    private byte[] read(long offset, int length) throws IOException {
        if (offset < 0 || length < 0 || offset + length > size) throw new Damaged("damaged package: a read leaves the package");
        byte[] data = new byte[length];
        source.readFully(offset, data, 0, length);
        return data;
    }

    private byte[] table(int block, int level) throws IOException {
        long offset = tableOffset(block, level, base);
        Long key = Long.valueOf(offset);
        byte[] cached = tables.get(key);
        if (cached != null) return cached;
        byte[] expected;
        if (level == rootLevel) {
            expected = rootHash;
        } else {
            byte[] parent = table(block, level + 1);
            int slot = (int) ((block / (level == 0 ? 170L : 28900L)) % 170);
            expected = Arrays.copyOfRange(parent, slot * 24, slot * 24 + 20);
        }
        byte[] data = read(offset, BLOCK);
        if (!Arrays.equals(sha1(data, 0, BLOCK), expected)) {
            throw new Damaged("damaged package: a hash table does not match its hash (level " + level + ", block " + block + ")");
        }
        if (tables.size() >= TABLE_CACHE) tables.clear();
        tables.put(key, data);
        return data;
    }

    /** The block's next block; its expected SHA-1 is copied into hash (20 bytes). */
    private int record(int block, byte[] hash) throws IOException {
        if (block < 0 || block >= total) throw new Damaged("damaged package: a block number lies outside the package");
        byte[] table = table(block, 0);
        int at = block % 170 * 24;
        long info = be32(table, at + 20);
        if (info >>> 30 != 2) throw new Damaged("damaged package: a file uses a block that is not allocated");
        System.arraycopy(table, at, hash, 0, 20);
        return (int) (info & END);
    }

    private int[] chain(int start, int count) throws IOException {
        if (count < 0 || count > total) throw new Damaged("damaged package: a chain is longer than the package");
        int[] out = new int[count];
        byte[] hash = new byte[20];
        int current = start;
        for (int i = 0; i < count; ++i) {
            int following = record(current, hash);
            if (claimed.get(current)) throw new Damaged("damaged package: two files share a block, or a chain loops");
            claimed.set(current);
            out[i] = current;
            current = following;
        }
        if (count > 0 && current != END) throw new Damaged("damaged package: a chain is longer than its record says");
        return out;
    }

    private byte[] block(int number) throws IOException {
        byte[] expected = new byte[20];
        record(number, expected);
        byte[] data = read(dataOffset(number, base), BLOCK);
        if (!Arrays.equals(sha1(data, 0, BLOCK), expected)) {
            throw new Damaged("damaged package: the data of block " + number + " does not match its hash");
        }
        return data;
    }

    private List<Entry> directory() throws IOException {
        int[] chain = chain(firstDirectory, directoryCount);
        List<Entry> out = new ArrayList<Entry>();
        Map<Integer, Entry> byIndex = new HashMap<Integer, Entry>();
        Set<String> seen = new HashSet<String>();
        for (int b = 0; b < chain.length; ++b) {
            byte[] data = block(chain[b]);
            for (int slot = 0; slot < 64; ++slot) {
                int index = b * 64 + slot, r = slot * 64;
                if (zero(data, r, r + 64)) continue;
                int length = data[r + 40] & 63;
                boolean isDirectory = (data[r + 40] & 0x80) != 0;
                if (length < 1 || length > 40 || !zero(data, r + length, r + 40)) throw new Damaged("damaged package: a name in its directory is malformed");
                String name = component(data, r, length);
                int parent = (data[r + 50] & 0xFF) << 8 | (data[r + 51] & 0xFF);
                if (parent != 0xFFFF) {
                    Entry p = byIndex.get(Integer.valueOf(parent));
                    if (p == null || !p.directory) throw new Damaged("damaged package: a directory record names a parent that is not a folder before it");
                    name = p.path + "/" + name;
                }
                String lower = name.toLowerCase(Locale.ROOT);
                if (name.length() > 1024 || count(name, '/') > 32 || !seen.add(lower)) {
                    throw new Damaged("damaged package: a path is repeated or nested too deeply");
                }
                int valid = le24(data, r + 41), allocated = le24(data, r + 44), start = le24(data, r + 47);
                long fileSize = be32(data, r + 52);
                if (allocated != valid || allocated != (fileSize + BLOCK - 1) / BLOCK || (isDirectory && (allocated != 0 || fileSize != 0))) {
                    throw new Damaged("damaged package: a file's size and its block count disagree");
                }
                int[] blocks = chain(start, allocated);
                if ((data[r + 40] & 0x40) != 0) {
                    for (int i = 0; i < blocks.length; ++i) {
                        if (blocks[i] != start + i) throw new Damaged("damaged package: a file marked contiguous is not");
                    }
                }
                Entry entry = new Entry(index, name, fileSize, blocks, isDirectory, be32(data, r + 56), be32(data, r + 60));
                byIndex.put(Integer.valueOf(index), entry);
                out.add(entry);
            }
        }
        return out;
    }

    private static String component(byte[] data, int at, int length) throws Damaged {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < length; ++i) {
            int c = data[at + i] & 0xFF;
            if (c < 32 || c >= 128 || "/\\:*?\"<>|".indexOf(c) >= 0) throw new Damaged("damaged package: unsafe name in its directory");
            b.append((char) c);
        }
        String name = b.toString();
        if (name.equals(".") || name.equals("..") || name.endsWith(".") || name.endsWith(" ")) {
            throw new Damaged("damaged package: unsafe name in its directory");
        }
        return name;
    }

    private void checkUnchanged() throws IOException {
        if (source.size() != size) throw new IOException("the package changed while it was read");
    }

    // ---- reading entries ---------------------------------------------------------------------------------------

    /**
     * The entry's bytes. Every block's SHA-1 is checked before any of its bytes are handed out; a mismatch is a
     * Damaged exception from read(). Memory: one run buffer (256 KiB) and the table cache, whatever the entry's size.
     */
    InputStream openEntry(final Entry entry) throws IOException {
        if (entry.directory || !entries.contains(entry)) throw new IOException("only a file of this package can be read");
        return new InputStream() {
            private final byte[] run = new byte[RUN_BLOCKS * BLOCK];
            private final byte[] expected = new byte[20];
            private int next;                 // index into entry.blocks of the next block to load
            private long left = entry.size;   // bytes not yet handed out
            private int at, end;              // unread part of run

            @Override
            public int read() throws IOException {
                byte[] one = new byte[1];
                return read(one, 0, 1) < 0 ? -1 : one[0] & 0xFF;
            }

            @Override
            public int read(byte[] buffer, int offset, int length) throws IOException {
                if (length == 0) return 0;
                if (at == end && !fill()) return -1;
                int n = Math.min(length, end - at);
                System.arraycopy(run, at, buffer, offset, n);
                at += n;
                return n;
            }

            private boolean fill() throws IOException {
                if (left == 0) return false;
                int first = entry.blocks[next];
                long physical = dataOffset(first, base);
                int count = 1;
                while (next + count < entry.blocks.length && count < RUN_BLOCKS
                        && dataOffset(entry.blocks[next + count], base) == physical + (long) count * BLOCK) {
                    ++count;
                }
                if (physical + (long) count * BLOCK > size) throw new Damaged("damaged package: a read leaves the package");
                source.readFully(physical, run, 0, count * BLOCK);
                for (int i = 0; i < count; ++i) {
                    record(entry.blocks[next + i], expected);
                    if (!Arrays.equals(sha1(run, i * BLOCK, BLOCK), expected)) {
                        throw new Damaged("damaged package: the data of block " + entry.blocks[next + i] + " of " + entry.path
                                + " does not match its hash");
                    }
                }
                next += count;
                at = 0;
                end = (int) Math.min(left, (long) count * BLOCK);
                left -= end;
                if (left == 0) checkUnchanged();
                return true;
            }
        };
    }

    /**
     * Writes the entry to target, which must not exist yet (it is never overwritten), with a running SHA-256, and
     * flushes it to storage. Returns the SHA-256 (64 hex digits). On any failure the incomplete target is removed.
     */
    String extractTo(Entry entry, File target, Cancel cancel) throws IOException {
        InputStream in = openEntry(entry);
        MessageDigest sha = digest("SHA-256");
        boolean done = false;
        FileChannel out = FileChannel.open(target.toPath(), StandardOpenOption.WRITE, StandardOpenOption.CREATE_NEW);
        try {
            byte[] buffer = new byte[1 << 20];
            long copied = 0;
            int n;
            while ((n = in.read(buffer, 0, buffer.length)) >= 0) {
                if (cancel != null && cancel.cancelled()) throw new IOException("stopped");
                ByteBuffer bytes = ByteBuffer.wrap(buffer, 0, n);
                while (bytes.hasRemaining()) out.write(bytes);
                sha.update(buffer, 0, n);
                copied += n;
            }
            if (copied != entry.size) throw new EOFException("the entry ended after " + copied + " of " + entry.size + " bytes");
            out.force(true);
            done = true;
        } finally {
            out.close();
            in.close();
            if (!done) target.delete();
        }
        return VerifiedExtractor.hex(sha.digest());
    }

    // ---- small helpers -----------------------------------------------------------------------------------------

    private byte[] sha1(byte[] data, int offset, int length) {
        sha1.reset();
        sha1.update(data, offset, length);
        return sha1.digest();
    }

    static MessageDigest digest(String name) {
        try {
            return MessageDigest.getInstance(name);
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(name + " is missing", e);
        }
    }

    private static boolean zero(byte[] data, int from, int to) {
        for (int i = from; i < to; ++i) {
            if (data[i] != 0) return false;
        }
        return true;
    }

    private static int count(String text, char c) {
        int n = 0;
        for (int i = 0; i < text.length(); ++i) {
            if (text.charAt(i) == c) ++n;
        }
        return n;
    }

    static long be32(byte[] d, int at) {
        return (d[at] & 0xFFL) << 24 | (d[at + 1] & 0xFFL) << 16 | (d[at + 2] & 0xFFL) << 8 | (d[at + 3] & 0xFFL);
    }

    static int le24(byte[] d, int at) {
        return (d[at] & 0xFF) | (d[at + 1] & 0xFF) << 8 | (d[at + 2] & 0xFF) << 16;
    }
}
