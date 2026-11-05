package dev.recompbootstrap.smoke;

import java.io.EOFException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Read-only reader for the file system of an Xbox 360 game disc image (XDVDFS), full or trimmed. Pure Java.
 *
 * On-disc layout as read here (the same fields and offsets as the repository's own host reader, class XisoDisc in
 * tools/own_disc/disc_builder.py; no code from another project):
 *  - Sectors are 2048 bytes. The game partition starts at one of PARTITION_OFFSETS inside the image.
 *  - Sector 32 of the partition is the volume descriptor: bytes 0..19 are the ASCII text MICROSOFT*XBOX*MEDIA,
 *    bytes 20..23 the root directory's first sector, bytes 24..27 the root directory's size in bytes (both
 *    little-endian), and bytes 2028..2047 repeat the text.
 *  - A directory is a table of entries that form a binary tree. An entry is: left child (2 bytes), right child
 *    (2 bytes), first sector (4), size in bytes (4), attributes (1; bit 0x10 = directory), name length (1), name.
 *    Child values count 4-byte units from the start of the table; 0 and 0xFFFF mean "no child". An entry whose two
 *    children are 0xFFFF and whose sector is 0xFFFFFFFF is padding.
 *  - A file's data is contiguous: it starts at partition + first sector * 2048.
 */
final class Xdvdfs {
    static final int SECTOR = 2048;
    /** Start of the game partition in an image: 0 for a trimmed image, then the full-disc layouts (XGD2, XGD3, XGD1). */
    static final long[] PARTITION_OFFSETS = {0L, 0x0FD90000L, 0x02080000L, 0x18300000L};
    static final int MAX_DIRECTORY_BYTES = 16 << 20;
    static final int MAX_ENTRIES = 100000;
    static final int MAX_DEPTH = 64;
    private static final int VOLUME_SECTOR = 32;
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);

    /** The image's own tables are wrong: reading it again gives the same result, a new image is needed. */
    static final class Damaged extends IOException {
        private static final String PREFIX = "damaged disc image: ";

        Damaged(String message) {
            super(message);
        }

        /** What is wrong, without the words "damaged disc image" (for a sentence that already says so). */
        String what() {
            String message = getMessage();
            return message.startsWith(PREFIX) ? message.substring(PREFIX.length()) : message;
        }
    }

    static final class Entry {
        /** Path inside the disc as stored, parts joined by '/'. */
        final String path;
        /** Byte position of the data (or, for a directory, of its table) inside the image. */
        final long offset;
        final long size;
        final boolean directory;

        Entry(String path, long offset, long size, boolean directory) {
            this.path = path;
            this.offset = offset;
            this.size = size;
            this.directory = directory;
        }
    }

    private final DiscSource source;
    private final long partition;
    private final long imageSize;
    private final long rootSector;
    private final long rootSize;

    private Xdvdfs(DiscSource source, long partition, long imageSize, long rootSector, long rootSize) {
        this.source = source;
        this.partition = partition;
        this.imageSize = imageSize;
        this.rootSector = rootSector;
        this.rootSize = rootSize;
    }

    /** Looks for the volume descriptor at sector 32 of each known game-partition offset. null if none. */
    static Xdvdfs open(DiscSource source) throws IOException {
        long size = source.size();
        byte[] head = new byte[SECTOR];
        for (long offset : PARTITION_OFFSETS) {
            if (offset + (VOLUME_SECTOR + 1L) * SECTOR > size) continue;
            source.readFully(offset + (long) VOLUME_SECTOR * SECTOR, head, 0, SECTOR);
            if (magicAt(head, 0) && magicAt(head, SECTOR - MAGIC.length)) {
                return new Xdvdfs(source, offset, size, u32(head, 20), u32(head, 24));
            }
        }
        return null;
    }

    /** The partition offset that was recognised. */
    long partitionOffset() {
        return partition;
    }

    /** Case-insensitive, '/' separated. null if absent. A directory is returned with directory = true. */
    Entry find(String path) throws IOException {
        long sector = rootSector, size = rootSize;
        String[] parts = path.replace('\\', '/').split("/");
        Entry hit = null;
        int depth = 0;
        for (String part : parts) {
            if (part.isEmpty()) continue;
            if (hit != null && !hit.directory) return null;
            if (++depth > MAX_DEPTH) throw new Damaged("damaged disc image: folders nested too deeply");
            Entry next = null;
            String prefix = hit == null ? "" : hit.path + "/";
            for (Raw raw : readDirectory(sector, size)) {
                if (raw.name.equalsIgnoreCase(part)) {
                    next = raw.entry(prefix, partition);
                    sector = raw.sector;
                    size = raw.size;
                    break;
                }
            }
            if (next == null) return null;
            hit = next;
        }
        return hit;
    }

    /** All files, depth first, each directory's names in alphabetical order. Refuses cycles and more than 100000 entries. */
    List<Entry> list() throws IOException {
        List<Entry> out = new ArrayList<Entry>();
        int[] seen = {0};
        walk(rootSector, rootSize, "", new HashSet<Long>(), out, seen, 1);
        return out;
    }

    private void walk(long sector, long size, String prefix, Set<Long> ancestors, List<Entry> out, int[] seen, int depth)
            throws IOException {
        if (depth > MAX_DEPTH) throw new Damaged("damaged disc image: folders nested too deeply");
        Long key = Long.valueOf(sector);
        if (!ancestors.add(key)) throw new Damaged("damaged disc image: a folder contains itself");
        List<Raw> raws = readDirectory(sector, size);
        Collections.sort(raws, new Comparator<Raw>() {
            @Override
            public int compare(Raw a, Raw b) {
                return a.name.toUpperCase(Locale.ROOT).compareTo(b.name.toUpperCase(Locale.ROOT));
            }
        });
        for (Raw raw : raws) {
            if (++seen[0] > MAX_ENTRIES) throw new Damaged("damaged disc image: more than " + MAX_ENTRIES + " entries");
            if (raw.directory) {
                walk(raw.sector, raw.size, prefix + raw.name + "/", ancestors, out, seen, depth + 1);
            } else {
                out.add(raw.entry(prefix, partition));
            }
        }
        ancestors.remove(key);
    }

    private static final class Raw {
        final String name;
        final long sector, size;
        final boolean directory;

        Raw(String name, long sector, long size, boolean directory) {
            this.name = name;
            this.sector = sector;
            this.size = size;
            this.directory = directory;
        }

        Entry entry(String prefix, long partition) {
            return new Entry(prefix + name, partition + sector * SECTOR, size, directory);
        }
    }

    /** The entries of one directory table, in the order the tree is visited. */
    private List<Raw> readDirectory(long sector, long size) throws IOException {
        List<Raw> out = new ArrayList<Raw>();
        if (size == 0) return out;
        if (size > MAX_DIRECTORY_BYTES) throw new Damaged("damaged disc image: a folder table is larger than 16 MiB");
        long start = partition + sector * SECTOR;
        if (start + size > imageSize) throw new EOFException("the image is cut off inside a folder table");
        byte[] data = new byte[(int) size];
        source.readFully(start, data, 0, data.length);
        Set<Integer> visited = new HashSet<Integer>();
        List<Integer> todo = new ArrayList<Integer>();
        todo.add(Integer.valueOf(0));
        while (!todo.isEmpty()) {
            int at = todo.remove(todo.size() - 1).intValue();
            if (!visited.add(Integer.valueOf(at))) throw new Damaged("damaged disc image: a folder table loops");
            if (at + 14 > data.length) throw new Damaged("damaged disc image: a folder entry lies outside its table");
            int left = u16(data, at), right = u16(data, at + 2);
            long first = u32(data, at + 4), bytes = u32(data, at + 8);
            if (left == 0xFFFF && right == 0xFFFF && first == 0xFFFFFFFFL) continue;   // padding
            int attributes = data[at + 12] & 0xFF, nameLength = data[at + 13] & 0xFF;
            if (nameLength == 0 || at + 14 + nameLength > data.length) throw new Damaged("damaged disc image: bad name in a folder table");
            String name = new String(data, at + 14, nameLength, StandardCharsets.ISO_8859_1);
            if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0 || name.equals(".") || name.equals("..")) {
                throw new Damaged("damaged disc image: unusable name in a folder table");
            }
            out.add(new Raw(name, first, bytes, (attributes & 0x10) != 0));
            if (out.size() > MAX_ENTRIES) throw new Damaged("damaged disc image: more than " + MAX_ENTRIES + " entries");
            if (left != 0 && left != 0xFFFF) todo.add(Integer.valueOf(left * 4));
            if (right != 0 && right != 0xFFFF) todo.add(Integer.valueOf(right * 4));
        }
        return out;
    }

    private static boolean magicAt(byte[] data, int at) {
        for (int i = 0; i < MAGIC.length; ++i) {
            if (data[at + i] != MAGIC[i]) return false;
        }
        return true;
    }

    private static int u16(byte[] data, int at) {
        return (data[at] & 0xFF) | (data[at + 1] & 0xFF) << 8;
    }

    private static long u32(byte[] data, int at) {
        return ((data[at] & 0xFFL) | (data[at + 1] & 0xFFL) << 8 | (data[at + 2] & 0xFFL) << 16 | (data[at + 3] & 0xFFL) << 24);
    }
}
