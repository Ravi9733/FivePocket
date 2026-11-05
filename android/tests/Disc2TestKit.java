package dev.recompbootstrap.smoke;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Synthetic inputs of the mini-disc-on-device tests: PIRS STFS packages (the executable helpers of the set-aside
 * tests are in aside/tests/Disc2XexTestKit.java). No game data, no key. The package builder is a line-for-line port of
 * pycheck/stfs_synth.py build(), whose packages the project's own reader (tools/host_guest_trace/stfs_inventory.py)
 * accepts and extracts byte for byte (pycheck/check_synth.py); it shares no code with Disc2Stfs.
 */
final class Disc2TestKit {
    static final int BLOCK = 4096, END = 0xFFFFFF, HEADER_SIZE = 0x971A;
    static final long BASE = (HEADER_SIZE + BLOCK - 1) & -BLOCK;
    static final long TITLE = 0x5E5E0001L;

    private Disc2TestKit() {}

    static long dataOffset(long block) {
        long physical = block;
        for (long c : new long[] {170, 28900, 4913000}) {
            physical += (block + c) / c;
            if (block < c) break;
        }
        return BASE + physical * BLOCK;
    }

    static long tableOffset(long block, int level) {
        long physical;
        if (level == 0) physical = block < 170 ? 0 : (block / 170) * 171 + block / 28900 + 1 + (block >= 28900 ? 1 : 0);
        else if (level == 1) physical = block < 28900 ? 171 : (block / 28900) * 29071 + 1;
        else physical = 29071;
        return BASE + physical * BLOCK;
    }

    interface RawHook { void edit(String path, byte[] raw); }

    /** A synthetic package and where its parts went. */
    static final class Package {
        byte[] data;
        final Map<String, int[]> layout = new LinkedHashMap<String, int[]>();
        int total, rootLevel;

        // build options (set before build())
        long title = TITLE;
        int contentType = 2, metadataVersion = 2, freeBlocks = 0;
        String display = "Synthetic package";
        RawHook rawHook;
        final Map<Integer, Integer> followingOverride = new HashMap<Integer, Integer>();
        final Map<Integer, Integer> statusOverride = new HashMap<Integer, Integer>();
        private final List<String> paths = new ArrayList<String>();
        private final List<byte[]> contents = new ArrayList<byte[]>();
        private final List<String> extraFolders = new ArrayList<String>();

        Package file(String path, byte[] content) {
            paths.add(path);
            contents.add(content);
            return this;
        }

        Package folder(String path) {
            extraFolders.add(path);
            return this;
        }

        Package build() {
            List<String> folders = new ArrayList<String>();
            List<String> all = new ArrayList<String>(extraFolders);
            all.addAll(paths);
            for (String path : all) {
                String[] parts = path.split("/");
                int upto = extraFolders.contains(path) ? parts.length : parts.length - 1;
                for (int i = 1; i <= upto; ++i) {
                    String folder = join(parts, i);
                    if (!folders.contains(folder)) folders.add(folder);
                }
            }
            List<String> recordPaths = new ArrayList<String>(folders);
            recordPaths.addAll(paths);
            int directoryBlocks = Math.max(1, (recordPaths.size() + 63) / 64);
            int next = directoryBlocks;
            for (int i = 0; i < paths.size(); ++i) {
                int count = (contents.get(i).length + BLOCK - 1) / BLOCK;
                int[] blocks = new int[count];
                for (int k = 0; k < count; ++k) blocks[k] = next + k;
                layout.put(paths.get(i), blocks);
                next += count;
            }
            total = next;
            byte[] directory = new byte[directoryBlocks * BLOCK];
            for (int i = 0; i < recordPaths.size(); ++i) {
                String path = recordPaths.get(i);
                int cut = path.lastIndexOf('/');
                byte[] name = path.substring(cut + 1).getBytes(StandardCharsets.US_ASCII);
                int parent = cut < 0 ? 0xFFFF : recordPaths.indexOf(path.substring(0, cut));
                byte[] raw = new byte[64];
                System.arraycopy(name, 0, raw, 0, name.length);
                int fileIndex = paths.indexOf(path);
                if (fileIndex < 0) {
                    raw[40] = (byte) (name.length | 0x80);
                } else {
                    int[] blocks = layout.get(path);
                    raw[40] = (byte) (name.length | 0x40);
                    le24(raw, 41, blocks.length);
                    le24(raw, 44, blocks.length);
                    le24(raw, 47, blocks.length == 0 ? 0 : blocks[0]);
                    be32(raw, 52, contents.get(fileIndex).length);
                }
                raw[50] = (byte) (parent >>> 8);
                raw[51] = (byte) parent;
                be32(raw, 56, 0x11112222L);
                be32(raw, 60, 0x33334444L);
                if (rawHook != null) rawHook.edit(path, raw);
                System.arraycopy(raw, 0, directory, i * 64, 64);
            }
            byte[][] content = new byte[total][];
            int[] following = new int[total];
            for (int b = 0; b < directoryBlocks; ++b) {
                content[b] = java.util.Arrays.copyOfRange(directory, b * BLOCK, (b + 1) * BLOCK);
                following[b] = b + 1 < directoryBlocks ? b + 1 : END;
            }
            for (int i = 0; i < paths.size(); ++i) {
                int[] blocks = layout.get(paths.get(i));
                byte[] data = contents.get(i);
                for (int k = 0; k < blocks.length; ++k) {
                    content[blocks[k]] = java.util.Arrays.copyOf(java.util.Arrays.copyOfRange(data, k * BLOCK, Math.min(data.length, (k + 1) * BLOCK)), BLOCK);
                    following[blocks[k]] = k + 1 < blocks.length ? blocks[k + 1] : END;
                }
            }
            for (Map.Entry<Integer, Integer> e : followingOverride.entrySet()) following[e.getKey()] = e.getValue();
            rootLevel = total <= 170 ? 0 : total <= 28900 ? 1 : 2;
            long size = Math.max(dataOffset(total - 1), tableOffset(total - 1, 0));
            for (int l = 1; l <= rootLevel; ++l) size = Math.max(size, tableOffset(total - 1, l));
            size += BLOCK;
            data = new byte[(int) size];
            for (int b = 0; b < total; ++b) System.arraycopy(content[b], 0, data, (int) dataOffset(b), BLOCK);
            Map<String, byte[]> tables = new LinkedHashMap<String, byte[]>();
            for (int first = 0; first < total; first += 170) {
                byte[] table = new byte[BLOCK];
                for (int b = first; b < Math.min(first + 170, total); ++b) {
                    int status = statusOverride.containsKey(b) ? statusOverride.get(b) : 2;
                    System.arraycopy(sha1(content[b]), 0, table, (b - first) * 24, 20);
                    be32(table, (b - first) * 24 + 20, ((long) status << 30) | following[b]);
                }
                tables.put("0/" + first, table);
            }
            for (int level = 1; level <= rootLevel; ++level) {
                long span = level == 1 ? 28900 : 4913000, childSpan = level == 1 ? 170 : 28900;
                for (long first = 0; first < total; first += span) {
                    byte[] table = new byte[BLOCK];
                    int slot = 0;
                    for (long child = first; child < Math.min(first + span, total); child += childSpan, ++slot) {
                        System.arraycopy(sha1(tables.get((level - 1) + "/" + child)), 0, table, slot * 24, 20);
                    }
                    tables.put(level + "/" + first, table);
                }
            }
            for (Map.Entry<String, byte[]> e : tables.entrySet()) {
                String[] key = e.getKey().split("/");
                System.arraycopy(e.getValue(), 0, data, (int) tableOffset(Long.parseLong(key[1]), Integer.parseInt(key[0])), BLOCK);
            }
            byte[] rootHash = sha1(tables.get(rootLevel + "/0"));
            data[0] = 'P';
            data[1] = 'I';
            data[2] = 'R';
            data[3] = 'S';
            be32(data, 0x340, HEADER_SIZE);
            be32(data, 0x344, contentType);
            be32(data, 0x348, metadataVersion);
            be32(data, 0x360, title);
            int v = 0x379;
            data[v] = 0x24;
            data[v + 1] = 0;
            data[v + 2] = 1;
            data[v + 3] = (byte) directoryBlocks;
            data[v + 4] = (byte) (directoryBlocks >>> 8);
            le24(data, v + 5, 0);
            System.arraycopy(rootHash, 0, data, v + 8, 20);
            be32(data, v + 28, total);
            be32(data, v + 32, freeBlocks);
            byte[] shown = display.getBytes(StandardCharsets.UTF_16BE);
            System.arraycopy(shown, 0, data, 0x411, Math.min(256, shown.length));
            return this;
        }

        /** Offset (inside data) of the level-0 table that covers block b. */
        long table0(int b) {
            return tableOffset(b, 0);
        }
    }

    /**
     * A package too large to build in memory: one file of fileBlocks blocks ("huge.rpf", contiguous, its last block
     * partly used) and one 14-byte file after it ("huge.timestamp"), as the real install-disc packages are laid out.
     * The data blocks are made on demand from their block number; only the header and the hash tables are kept. With
     * more than 28,900 blocks the hash tree has three levels. Same layout formulas as Package above; that those
     * formulas are the format's is shown by the real disc (logs/05), not by this fixture.
     */
    static final class Sparse implements DiscSource {
        static final int TAIL = 14, UNUSED_IN_LAST_BLOCK = 100;
        final int total, fileBlocks;
        final long fileBytes, size;
        final String fileSha256, tailSha256;
        final Map<Long, byte[]> tables = new HashMap<Long, byte[]>();   // physical position -> table
        /** A data block to hand out with one bit changed (-1: none). */
        int flipBlock = -1;
        private final byte[] head = new byte[(int) BASE];
        private final long[] dataAt;

        Sparse(int fileBlocks) {
            this.fileBlocks = fileBlocks;
            this.total = fileBlocks + 2;
            this.fileBytes = (long) fileBlocks * BLOCK - UNUSED_IN_LAST_BLOCK;
            int rootLevel = total <= 170 ? 0 : total <= 28900 ? 1 : 2;
            dataAt = new long[total];
            long end = 0;
            for (int b = 0; b < total; ++b) {
                dataAt[b] = dataOffset(b);
                end = Math.max(end, dataAt[b]);
            }
            try {
                MessageDigest sha1 = MessageDigest.getInstance("SHA-1"), file = MessageDigest.getInstance("SHA-256");
                byte[] block = new byte[BLOCK];
                byte[] table = null;
                for (int b = 0; b < total; ++b) {
                    if (b % 170 == 0) {
                        table = new byte[BLOCK];
                        tables.put(Long.valueOf(tableOffset(b, 0)), table);
                    }
                    fill(b, block);
                    if (b >= 1 && b <= fileBlocks) file.update(block, 0, b == fileBlocks ? BLOCK - UNUSED_IN_LAST_BLOCK : BLOCK);
                    System.arraycopy(sha1.digest(block), 0, table, b % 170 * 24, 20);
                    int following = b == 0 || b == fileBlocks || b == total - 1 ? END : b + 1;
                    be32(table, b % 170 * 24 + 20, (2L << 30) | following);
                }
                fileSha256 = SetupTestKit.hex(file.digest());
                fill(total - 1, block);
                tailSha256 = SetupTestKit.sha256(java.util.Arrays.copyOf(block, TAIL));
                for (int level = 1; level <= rootLevel; ++level) {
                    long span = level == 1 ? 28900 : 4913000, childSpan = level == 1 ? 170 : 28900;
                    for (long first = 0; first < total; first += span) {
                        byte[] parent = new byte[BLOCK];
                        int slot = 0;
                        for (long child = first; child < Math.min(first + span, total); child += childSpan, ++slot) {
                            System.arraycopy(sha1.digest(tables.get(Long.valueOf(tableOffset(child, level - 1)))), 0, parent, slot * 24, 20);
                        }
                        tables.put(Long.valueOf(tableOffset(first, level)), parent);
                    }
                }
                for (Long at : tables.keySet()) end = Math.max(end, at.longValue());
                size = end + BLOCK;
                head[0] = 'P';
                head[1] = 'I';
                head[2] = 'R';
                head[3] = 'S';
                be32(head, 0x340, HEADER_SIZE);
                be32(head, 0x344, 2);
                be32(head, 0x348, 2);
                be32(head, 0x360, TITLE);
                int v = 0x379;
                head[v] = 0x24;
                head[v + 2] = 1;
                head[v + 3] = 1;   // one directory block, block 0
                System.arraycopy(sha1.digest(tables.get(Long.valueOf(tableOffset(0, rootLevel)))), 0, head, v + 8, 20);
                be32(head, v + 28, total);
            } catch (java.security.NoSuchAlgorithmException e) {
                throw new IllegalStateException(e);
            }
        }

        /** The content of data block b: block 0 is the directory, the others a pattern that depends on b. */
        void fill(int b, byte[] out) {
            if (b == 0) {
                java.util.Arrays.fill(out, (byte) 0);
                record(out, 0, "huge.rpf", fileBlocks, 1, fileBytes);
                record(out, 64, "huge.timestamp", 1, fileBlocks + 1, TAIL);
                return;
            }
            long state = 0x9E3779B97F4A7C15L * (b + 1L);
            for (int i = 0; i < BLOCK; i += 8) {
                state = state * 6364136223846793005L + 1442695040888963407L;
                long word = state ^ (state >>> 29);
                for (int k = 0; k < 8; ++k) out[i + k] = (byte) (word >>> (k * 8));
            }
        }

        private static void record(byte[] directory, int at, String name, int blocks, int start, long bytes) {
            byte[] text = name.getBytes(StandardCharsets.US_ASCII);
            System.arraycopy(text, 0, directory, at, text.length);
            directory[at + 40] = (byte) (text.length | 0x40);
            le24(directory, at + 41, blocks);
            le24(directory, at + 44, blocks);
            le24(directory, at + 47, start);
            directory[at + 50] = (byte) 0xFF;
            directory[at + 51] = (byte) 0xFF;
            be32(directory, at + 52, bytes);
        }

        @Override
        public long size() {
            return size;
        }

        @Override
        public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
            if (position < 0 || position + length > size) throw new java.io.EOFException("read past the end");
            byte[] block = new byte[BLOCK];
            int done = 0;
            while (done < length) {
                long at = position + done;
                int n;
                if (at < BASE) {
                    n = (int) Math.min(length - done, BASE - at);
                    System.arraycopy(head, (int) at, buffer, offset + done, n);
                } else {
                    long start = at - (at - BASE) % BLOCK;
                    int inside = (int) (at - start);
                    n = Math.min(length - done, BLOCK - inside);
                    byte[] table = tables.get(Long.valueOf(start));
                    if (table == null) {
                        int b = java.util.Arrays.binarySearch(dataAt, start);
                        if (b < 0) throw new IOException("a read of a position that holds nothing: " + start);
                        fill(b, block);
                        if (b == flipBlock) block[77] ^= 1;
                        table = block;
                    }
                    System.arraycopy(table, inside, buffer, offset + done, n);
                }
                done += n;
            }
        }

        @Override
        public void close() {
        }
    }

    /**
     * A package far larger than a test could hold or hash: `files` files of blocksEach blocks each (file0.rpf, ...),
     * every data block the same 4096 bytes, so that only the hash tables differ from one another. A level-0 table is
     * made each time it is read; the level-1 and level-2 tables are kept (a few hundred KiB). Same layout formulas
     * as Package above.
     */
    static final class Vast implements DiscSource {
        final int total, level0Tables;
        final long size;
        /** The content of every data block of every file. */
        final byte[] data = new byte[BLOCK];
        private final int blocksEach;
        private final byte[] head = new byte[(int) BASE], directory = new byte[BLOCK];
        private final byte[] directoryHash, dataHash, level2 = new byte[BLOCK];
        private final byte[][] level1;   // one per 28,900 blocks

        Vast(int files, int blocksEach) {
            this.blocksEach = blocksEach;
            total = 1 + files * blocksEach;
            if (total <= 28900 || files > 64) throw new IllegalArgumentException("meant for a three-level package with one directory block");
            for (int i = 0; i < BLOCK; ++i) data[i] = (byte) (i * 31 + 7);
            for (int f = 0; f < files; ++f) Sparse.record(directory, f * 64, "file" + f + ".rpf", blocksEach, 1 + f * blocksEach, (long) blocksEach * BLOCK);
            directoryHash = sha1(directory);
            dataHash = sha1(data);
            level0Tables = (total + 169) / 170;
            level1 = new byte[(total + 28899) / 28900][BLOCK];
            for (int t = 0; t < level0Tables; ++t) System.arraycopy(sha1(table0(t * 170)), 0, level1[t / 170], t % 170 * 24, 20);
            for (int g = 0; g < level1.length; ++g) System.arraycopy(sha1(level1[g]), 0, level2, g * 24, 20);
            size = dataOffset(total - 1) + BLOCK;
            head[0] = 'P';
            head[1] = 'I';
            head[2] = 'R';
            head[3] = 'S';
            be32(head, 0x340, HEADER_SIZE);
            be32(head, 0x344, 2);
            be32(head, 0x348, 2);
            be32(head, 0x360, TITLE);
            int v = 0x379;
            head[v] = 0x24;
            head[v + 2] = 1;
            head[v + 3] = 1;   // one directory block, block 0
            System.arraycopy(sha1(level2), 0, head, v + 8, 20);
            be32(head, v + 28, total);
        }

        /** The level-0 table of the 170 blocks from first on: block 0 is the directory, file f is blocks 1 + f * blocksEach onwards. */
        private byte[] table0(int first) {
            byte[] table = new byte[BLOCK];
            for (int b = first; b < Math.min(first + 170, total); ++b) {
                System.arraycopy(b == 0 ? directoryHash : dataHash, 0, table, (b - first) * 24, 20);
                boolean last = b == 0 || (b - 1) % blocksEach == blocksEach - 1;   // the directory block, or the last block of a file
                be32(table, (b - first) * 24 + 20, (2L << 30) | (last ? END : b + 1));
            }
            return table;
        }

        /** What lies in the package's physical block number p (counted from BASE). */
        private byte[] physical(long p) throws IOException {
            int low = 0, high = total - 1, block = -1;   // the last data block at or before p
            while (low <= high) {
                int middle = (low + high) >>> 1;
                if ((dataOffset(middle) - BASE) / BLOCK <= p) {
                    block = middle;
                    low = middle + 1;
                } else {
                    high = middle - 1;
                }
            }
            if (block >= 0 && (dataOffset(block) - BASE) / BLOCK == p) return block == 0 ? directory : data;
            int next = block + 1;   // p holds a table that stands before this data block
            if (next < total) {
                if ((tableOffset(next, 0) - BASE) / BLOCK == p) return table0(next);
                if ((tableOffset(next, 1) - BASE) / BLOCK == p) return level1[next / 28900];
                if ((tableOffset(next, 2) - BASE) / BLOCK == p) return level2;
            }
            throw new IOException("a read of a position that holds nothing: block " + p);
        }

        @Override
        public long size() {
            return size;
        }

        @Override
        public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
            if (position < 0 || position + length > size) throw new java.io.EOFException("read past the end");
            int done = 0;
            while (done < length) {
                long at = position + done;
                int n;
                if (at < BASE) {
                    n = (int) Math.min(length - done, BASE - at);
                    System.arraycopy(head, (int) at, buffer, offset + done, n);
                } else {
                    int inside = (int) ((at - BASE) % BLOCK);
                    n = Math.min(length - done, BLOCK - inside);
                    System.arraycopy(physical((at - BASE) / BLOCK), inside, buffer, offset + done, n);
                }
                done += n;
            }
        }

        @Override
        public void close() {
        }
    }

    private static String join(String[] parts, int count) {
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < count; ++i) b.append(i == 0 ? "" : "/").append(parts[i]);
        return b.toString();
    }

    static byte[] sha1(byte[] data) {
        try {
            return MessageDigest.getInstance("SHA-1").digest(data);
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    static void be32(byte[] d, int at, long v) {
        d[at] = (byte) (v >>> 24);
        d[at + 1] = (byte) (v >>> 16);
        d[at + 2] = (byte) (v >>> 8);
        d[at + 3] = (byte) v;
    }

    static void le24(byte[] d, int at, int v) {
        d[at] = (byte) v;
        d[at + 1] = (byte) (v >>> 8);
        d[at + 2] = (byte) (v >>> 16);
    }

    /** The package as a DiscSource. */
    static SetupTestKit.MemoryDisc disc(byte[] data) {
        return new SetupTestKit.MemoryDisc(0, data);
    }

    static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[3001];   // odd size: reads straddle blocks and runs
        int n;
        while ((n = in.read(buffer, 0, buffer.length)) >= 0) out.write(buffer, 0, n);
        return out.toByteArray();
    }
}
