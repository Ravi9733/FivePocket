package dev.recompbootstrap.smoke;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.CRC32;

/**
 * Shared helpers of the first-run-setup tests (lane E). Everything here is synthetic: small made-up byte patterns,
 * a disc image the tests build themselves, a code bundle the tests build themselves. No game data.
 */
final class SetupTestKit {
    interface Check { void run() throws Exception; }

    private static int passed, failed;
    private static final List<File> TEMP = new ArrayList<File>();

    private SetupTestKit() {}

    static void requireAssertions() {
        boolean on = false;
        assert on = true;
        if (!on) throw new IllegalStateException("Run with -ea");
    }

    /** Runs one named test and prints its result. A failure does not stop the others. */
    static void check(String name, Check check) {
        try {
            check.run();
            ++passed;
            System.out.println("PASS  " + name);
        } catch (Throwable t) {
            ++failed;
            System.out.println("FAIL  " + name + ": " + t);
        }
    }

    static void note(String text) {
        System.out.println("NOTE  " + text);
    }

    static void finish(String suite) {
        for (File dir : TEMP) deleteQuietly(dir);
        System.out.println(suite + ": " + passed + " passed, " + failed + " failed");
        if (failed > 0) System.exit(1);
    }

    static void expect(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    static void expectEquals(Object expected, Object actual, String what) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(what + ": expected <" + expected + "> but was <" + actual + ">");
        }
    }

    static void expectContains(String text, String part, String what) {
        if (text == null || !text.contains(part)) throw new AssertionError(what + ": <" + text + "> lacks <" + part + ">");
    }

    // ---- bytes and files ---------------------------------------------------------------------------------------

    /** length bytes of a fixed pseudo-random pattern that depends on seed. */
    static byte[] bytes(int length, int seed) {
        byte[] out = new byte[length];
        long state = 0x9E3779B97F4A7C15L * (seed + 1);
        for (int i = 0; i < length; ++i) {
            state = state * 6364136223846793005L + 1442695040888963407L;
            out[i] = (byte) (state >>> 56);
        }
        return out;
    }

    static String sha256(byte[] data) {
        try {
            return hex(MessageDigest.getInstance("SHA-256").digest(data));
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder();
        for (byte b : bytes) out.append(String.format(Locale.ROOT, "%02x", b & 0xFF));
        return out.toString();
    }

    /** A fresh, empty directory named "files" inside a new temporary directory (removed by finish()). */
    static File newFilesDir(String label) throws IOException {
        File base = Files.createTempDirectory("setup-" + label + "-").toFile();
        TEMP.add(base);
        File files = new File(base, "files");
        expect(files.mkdir(), "mkdir files");
        return files;
    }

    static void write(File file, byte[] data) throws IOException {
        File parent = file.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdirs " + parent);
        FileOutputStream out = new FileOutputStream(file);
        try {
            out.write(data);
        } finally {
            out.close();
        }
    }

    static byte[] read(File file) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        FileInputStream in = new FileInputStream(file);
        try {
            byte[] buffer = new byte[65536];
            int n;
            while ((n = in.read(buffer)) >= 0) out.write(buffer, 0, n);
        } finally {
            in.close();
        }
        return out.toByteArray();
    }

    /**
     * Every path under dir (relative, sorted) with what it is: "dir", "link -> target" or "file <size> <sha256>
     * <modified ms> <file identity>". Two equal snapshots mean: nothing was created, removed, renamed, rewritten,
     * replaced by an equal copy, or touched.
     */
    static TreeMap<String, String> snapshot(final File dir) throws IOException {
        final TreeMap<String, String> out = new TreeMap<String, String>();
        if (!dir.exists()) return out;
        final Path base = dir.toPath();
        Files.walkFileTree(base, new SimpleFileVisitor<Path>() {
            @Override
            public FileVisitResult preVisitDirectory(Path d, BasicFileAttributes a) {
                if (!d.equals(base)) out.put(base.relativize(d).toString(), "dir");
                return FileVisitResult.CONTINUE;
            }

            @Override
            public FileVisitResult visitFile(Path f, BasicFileAttributes a) throws IOException {
                String key = base.relativize(f).toString();
                if (Files.isSymbolicLink(f)) out.put(key, "link -> " + Files.readSymbolicLink(f));
                else out.put(key, "file " + a.size() + " " + sha256(read(f.toFile())) + " "
                        + Files.getLastModifiedTime(f, LinkOption.NOFOLLOW_LINKS).toMillis() + " " + a.fileKey());
                return FileVisitResult.CONTINUE;
            }
        });
        return out;
    }

    private static void deleteQuietly(File dir) {
        try {
            Files.walkFileTree(dir.toPath(), new SimpleFileVisitor<Path>() {
                @Override
                public FileVisitResult visitFile(Path f, BasicFileAttributes a) throws IOException {
                    Files.delete(f);
                    return FileVisitResult.CONTINUE;
                }

                @Override
                public FileVisitResult postVisitDirectory(Path d, IOException e) throws IOException {
                    Files.delete(d);
                    return FileVisitResult.CONTINUE;
                }
            });
        } catch (IOException ignored) {
            // a temporary directory that stays behind does no harm
        }
    }

    static SetupCatalog.Item item(String path, byte[] content) {
        return new SetupCatalog.Item(SetupCatalog.PLAY, path, content.length, sha256(content));
    }

    static SetupCatalog.Spec spec(byte[] executable, List<SetupCatalog.Item> items, boolean coversAllDiscs) {
        return new SetupCatalog.Spec("Synthetic test release", "default.xex", sha256(executable), items, coversAllDiscs);
    }

    /** The final file of an item under a game-install directory. */
    static File input(File root, SetupCatalog.Item item) {
        return new File(root, "inputs/" + item.sha256 + ".bin");
    }

    static String mode(File file) throws IOException {
        return java.nio.file.attribute.PosixFilePermissions.toString(Files.getPosixFilePermissions(file.toPath()));
    }

    // ---- progress recorder -------------------------------------------------------------------------------------

    static final class Recorder implements VerifiedExtractor.Progress {
        long bytesDone, bytesTotal;
        int filesDone, filesTotal, updates;
        String current = "";
        boolean decreasing;
        /** Cancel once this many bytes were reported done (-1: never). */
        long cancelAtBytes = -1;
        /** Runs once, at the first report that names atPath: the work is about to start on that file and has not touched it. */
        String atPath;
        Check atPathDo;

        @Override
        public void update(long done, long total, int files, int filesOf, String currentPath) {
            if (done < bytesDone) decreasing = true;
            bytesDone = done;
            bytesTotal = total;
            filesDone = files;
            filesTotal = filesOf;
            current = currentPath;
            ++updates;
            if (atPathDo != null && currentPath.equals(atPath)) {
                Check once = atPathDo;
                atPathDo = null;
                try {
                    once.run();
                } catch (Exception e) {
                    throw new IllegalStateException(e);
                }
            }
        }

        @Override
        public boolean cancelled() {
            return cancelAtBytes >= 0 && bytesDone >= cancelAtBytes;
        }
    }

    static final VerifiedExtractor.Space PLENTY = new VerifiedExtractor.Space() {
        @Override
        public long usable(File existingDirectory) {
            return Long.MAX_VALUE / 4;
        }
    };

    /** Free space that a test changes while the work runs (another app filling the storage). */
    static final class Budget implements VerifiedExtractor.Space {
        long free;

        Budget(long free) {
            this.free = free;
        }

        @Override
        public long usable(File existingDirectory) {
            return free;
        }
    }

    /** The bytes of the regular files under dir, links neither followed nor counted (the test's own count). */
    static long regularBytes(File dir) throws IOException {
        final long[] bytes = {0};
        if (!dir.exists()) return 0;
        Files.walkFileTree(dir.toPath(), new SimpleFileVisitor<Path>() {
            @Override
            public FileVisitResult visitFile(Path f, BasicFileAttributes a) {
                if (a.isRegularFile()) bytes[0] += a.size();
                return FileVisitResult.CONTINUE;
            }
        });
        return bytes[0];
    }

    /** A small storage: what lies under dir uses it up, so removing a file there gives its bytes back. */
    static VerifiedExtractor.Space quota(final File dir, final long capacity) {
        return new VerifiedExtractor.Space() {
            @Override
            public long usable(File existingDirectory) {
                try {
                    return capacity - regularBytes(dir);
                } catch (IOException e) {
                    throw new IllegalStateException(e);
                }
            }
        };
    }

    static VerifiedExtractor.Space spaceOf(final long bytes) {
        return new VerifiedExtractor.Space() {
            @Override
            public long usable(File existingDirectory) {
                return bytes;
            }
        };
    }

    // ---- disc sources ------------------------------------------------------------------------------------------

    /** A DiscSource in memory: leadingZeros zero bytes followed by data (so a far partition offset costs nothing). */
    static final class MemoryDisc implements DiscSource {
        final long leadingZeros;
        final byte[] data;
        int reads;
        boolean closed;

        MemoryDisc(long leadingZeros, byte[] data) {
            this.leadingZeros = leadingZeros;
            this.data = data;
        }

        @Override
        public long size() {
            return leadingZeros + data.length;
        }

        @Override
        public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
            ++reads;
            if (position < 0 || position + length > size()) throw new EOFException("read past the end");
            for (int i = 0; i < length; ++i) {
                long at = position + i - leadingZeros;
                buffer[offset + i] = at < 0 ? 0 : data[(int) at];
            }
        }

        @Override
        public void close() {
            closed = true;
        }
    }

    /** An extracted disc folder in memory: path -> bytes. Names are matched without regard to case. */
    static final class MemoryFolder implements DiscIdentifier.FolderSource {
        final Map<String, byte[]> files = new LinkedHashMap<String, byte[]>();
        final List<String> opened = new ArrayList<String>();
        final List<MemoryDisc> sources = new ArrayList<MemoryDisc>();

        MemoryFolder put(String path, byte[] data) {
            files.put(path, data);
            return this;
        }

        @Override
        public DiscSource open(String relativePath) {
            for (Map.Entry<String, byte[]> e : files.entrySet()) {
                if (e.getKey().equalsIgnoreCase(relativePath)) {
                    opened.add(e.getKey());
                    MemoryDisc source = new MemoryDisc(0, e.getValue());
                    sources.add(source);
                    return source;
                }
            }
            return null;
        }
    }

    // ---- a synthetic XDVDFS image ------------------------------------------------------------------------------

    static final class Node {
        final String name;
        final byte[] data;            // file content, or null
        final List<Node> children;    // directory content, or null
        final String linkTo;          // a directory entry that points at the table of this directory path ("" = root), or null
        long sector, size;

        private Node(String name, byte[] data, List<Node> children, String linkTo) {
            this.name = name;
            this.data = data;
            this.children = children;
            this.linkTo = linkTo;
        }

        static Node file(String name, byte[] data) {
            return new Node(name, data, null, null);
        }

        static Node dir(String name, Node... children) {
            return new Node(name, null, new ArrayList<Node>(Arrays.asList(children)), null);
        }

        /** A directory entry whose table is another directory's table: how a damaged disc makes a folder contain itself. */
        static Node link(String name, String targetDirectoryPath) {
            return new Node(name, null, null, targetDirectoryPath);
        }

        boolean directory() {
            return children != null || linkTo != null;
        }
    }

    /**
     * Builds the bytes of a game partition: the volume descriptor at sector 32, then directory tables and file data
     * from sector 33 on. A directory table is written as a balanced binary tree over the names in alphabetical order
     * (so left and right children both occur); an empty directory is one sector of 0xFF bytes, as on real discs.
     */
    static final class Image {
        static final int SECTOR = 2048;
        final byte[] data;
        /** Path -> position (inside data) of that entry's 14-byte record. */
        final Map<String, Integer> records = new LinkedHashMap<String, Integer>();
        private final Map<String, Node> directories = new LinkedHashMap<String, Node>();
        private long next = 33;

        Image(Node root) {
            assign(root, "");
            for (Map.Entry<String, Node> e : new ArrayList<Map.Entry<String, Node>>(directories.entrySet())) resolveLinks(e.getValue());
            data = new byte[(int) (next * SECTOR)];
            byte[] magic = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
            System.arraycopy(magic, 0, data, 32 * SECTOR, 20);
            put32(data, 32 * SECTOR + 20, root.sector);
            put32(data, 32 * SECTOR + 24, root.size);
            System.arraycopy(magic, 0, data, 33 * SECTOR - 20, 20);
            for (Map.Entry<String, Node> e : directories.entrySet()) writeTable(e.getValue(), e.getKey());
        }

        private static int recordBytes(Node n) {
            return (14 + n.name.length() + 3) & ~3;
        }

        private void assign(Node dir, String path) {
            directories.put(path, dir);
            long table = 0;
            for (Node c : dir.children) table += recordBytes(c);
            dir.size = table == 0 ? SECTOR : (table + SECTOR - 1) / SECTOR * SECTOR;
            dir.sector = next;
            next += dir.size / SECTOR;
            for (Node c : dir.children) {
                String childPath = path.isEmpty() ? c.name : path + "/" + c.name;
                if (c.children != null) {
                    assign(c, childPath);
                } else if (c.data != null) {
                    c.size = c.data.length;
                    c.sector = next;
                    next += (c.data.length + SECTOR - 1) / SECTOR;
                }
            }
        }

        private void resolveLinks(Node dir) {
            for (Node c : dir.children) {
                if (c.linkTo != null) {
                    Node target = directories.get(c.linkTo);
                    c.sector = target.sector;
                    c.size = target.size;
                }
            }
        }

        private void writeTable(Node dir, String path) {
            int base = (int) (dir.sector * SECTOR);
            Arrays.fill(data, base, base + (int) dir.size, (byte) 0xFF);
            List<Node> sorted = new ArrayList<Node>(dir.children);
            Collections.sort(sorted, new Comparator<Node>() {
                @Override
                public int compare(Node a, Node b) {
                    return a.name.toUpperCase(Locale.ROOT).compareTo(b.name.toUpperCase(Locale.ROOT));
                }
            });
            int[] cursor = {0};
            writeSubtree(sorted, 0, sorted.size(), base, cursor, path);
            for (Node c : dir.children) {
                if (c.data != null) System.arraycopy(c.data, 0, data, (int) (c.sector * SECTOR), c.data.length);
            }
        }

        /** Writes the middle name as the node, then its left and right halves; returns the node's offset or -1. */
        private int writeSubtree(List<Node> sorted, int from, int to, int base, int[] cursor, String path) {
            if (from >= to) return -1;
            int middle = (from + to) / 2;
            Node n = sorted.get(middle);
            int at = cursor[0];
            cursor[0] += recordBytes(n);
            int left = writeSubtree(sorted, from, middle, base, cursor, path);
            int right = writeSubtree(sorted, middle + 1, to, base, cursor, path);
            int p = base + at;
            put16(data, p, left < 0 ? 0 : left / 4);
            put16(data, p + 2, right < 0 ? 0 : right / 4);
            put32(data, p + 4, n.sector);
            put32(data, p + 8, n.size);
            data[p + 12] = (byte) (n.directory() ? 0x10 : 0x20);
            data[p + 13] = (byte) n.name.length();
            byte[] name = n.name.getBytes(StandardCharsets.ISO_8859_1);
            System.arraycopy(name, 0, data, p + 14, name.length);
            for (int i = 14 + name.length; i < recordBytes(n); ++i) data[p + i] = (byte) 0xFF;
            records.put(path.isEmpty() ? n.name : path + "/" + n.name, Integer.valueOf(p));
            return at;
        }

        /** The image as a disc source with the partition at the given offset. */
        MemoryDisc at(long partitionOffset) {
            return new MemoryDisc(partitionOffset, data);
        }

        /** The same, cut off after length bytes of the partition. */
        MemoryDisc truncated(int length) {
            return new MemoryDisc(0, Arrays.copyOf(data, length));
        }
    }

    static void put16(byte[] data, int at, int value) {
        data[at] = (byte) value;
        data[at + 1] = (byte) (value >>> 8);
    }

    static void put32(byte[] data, int at, long value) {
        data[at] = (byte) value;
        data[at + 1] = (byte) (value >>> 8);
        data[at + 2] = (byte) (value >>> 16);
        data[at + 3] = (byte) (value >>> 24);
    }

    // ---- a synthetic code bundle -------------------------------------------------------------------------------

    /**
     * Writes a code bundle (format "gta5-code-bundle/1", tools/own_disc/BUNDLE-FORMAT.md) byte by byte, with switches
     * for every way a test needs it to be wrong. Independent of the importer: it shares no code with it.
     */
    static final class Bundle {
        final Map<String, byte[]> components = new LinkedHashMap<String, byte[]>();
        final Map<String, String> roles = new LinkedHashMap<String, String>();
        String schema = "gta5-code-bundle/1";
        String runtimeAbi = "private-trace-fd-dlopen/1";
        String executableSha256;
        /** When set, written instead of the generated manifest. */
        String manifestOverride;
        /** Entry name -> bytes written into the ZIP instead of the component's (the manifest keeps the real hash). */
        final Map<String, byte[]> storedInstead = new LinkedHashMap<String, byte[]>();
        /** Entry name -> name written into the ZIP instead. */
        final Map<String, String> renamedInZip = new LinkedHashMap<String, String>();
        /** Entries written a second time at the end. */
        final List<String> repeated = new ArrayList<String>();
        /** Extra ZIP entries that the manifest does not list. */
        final Map<String, byte[]> unlisted = new LinkedHashMap<String, byte[]>();
        /** Component names left out of the ZIP. */
        final List<String> leftOut = new ArrayList<String>();
        int flags, method;
        boolean manifestFirst = true;
        /** Position of the first byte of the central directory in the last build() result. */
        int centralDirectoryAt;
        /** Entry name -> position of its first data byte in the last build() result. */
        final Map<String, Integer> dataAt = new LinkedHashMap<String, Integer>();

        Bundle(String executableSha256) {
            this.executableSha256 = executableSha256;
        }

        Bundle add(String name, String role, byte[] data) {
            components.put(name, data);
            roles.put(name, role);
            return this;
        }

        String manifest() {
            if (manifestOverride != null) return manifestOverride;
            StringBuilder m = new StringBuilder();
            m.append("{\n \"components\": [");
            boolean first = true;
            for (Map.Entry<String, byte[]> e : new TreeMap<String, byte[]>(components).entrySet()) {
                m.append(first ? "\n" : ",\n");
                first = false;
                m.append("  {\"name\": \"").append(e.getKey()).append("\", \"provenance\": \"built\", \"role\": \"")
                        .append(roles.get(e.getKey())).append("\", \"sha256\": \"").append(sha256(e.getValue()))
                        .append("\", \"size\": ").append(e.getValue().length).append(", \"stage\": \"test\"}");
            }
            m.append("\n ],\n \"flavour\": \"test\",\n \"game\": {\"executable_sha256\": \"").append(executableSha256)
                    .append("\", \"executable_size\": 1, \"id\": \"synthetic\"},\n \"profile\": \"test\",\n \"runtime_abi\": \"")
                    .append(runtimeAbi).append("\",\n \"schema\": \"").append(schema)
                    .append("\",\n \"stages\": [],\n \"versions\": {}\n}\n");
            return m.toString();
        }

        byte[] build() {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            ByteArrayOutputStream central = new ByteArrayOutputStream();
            int count = 0;
            byte[] manifest = manifest().getBytes(StandardCharsets.UTF_8);
            if (manifestFirst) {
                entry(out, central, "manifest.json", manifest, 0, 0);
                ++count;
            }
            for (Map.Entry<String, byte[]> e : new TreeMap<String, byte[]>(components).entrySet()) {
                if (leftOut.contains(e.getKey())) continue;
                byte[] stored = storedInstead.containsKey(e.getKey()) ? storedInstead.get(e.getKey()) : e.getValue();
                String name = renamedInZip.containsKey(e.getKey()) ? renamedInZip.get(e.getKey()) : e.getKey();
                entry(out, central, name, stored, flags, method);
                ++count;
            }
            for (Map.Entry<String, byte[]> e : unlisted.entrySet()) {
                entry(out, central, e.getKey(), e.getValue(), 0, 0);
                ++count;
            }
            for (String name : repeated) {
                entry(out, central, name, components.get(name), 0, 0);
                ++count;
            }
            if (!manifestFirst) {
                entry(out, central, "manifest.json", manifest, 0, 0);
                ++count;
            }
            centralDirectoryAt = out.size();
            byte[] directory = central.toByteArray();
            out.write(directory, 0, directory.length);
            byte[] end = new byte[22];
            put32(end, 0, 0x06054b50L);
            put16(end, 8, count);
            put16(end, 10, count);
            put32(end, 12, directory.length);
            put32(end, 16, centralDirectoryAt);
            put16(end, 20, 85);
            out.write(end, 0, end.length);
            return seal(out.toByteArray());
        }

        private void entry(ByteArrayOutputStream out, ByteArrayOutputStream central, String name, byte[] data, int flag, int how) {
            CRC32 crc = new CRC32();
            crc.update(data, 0, data.length);
            byte[] nameBytes = name.getBytes(StandardCharsets.UTF_8);
            int offset = out.size();
            byte[] local = new byte[30];
            put32(local, 0, 0x04034b50L);
            put16(local, 4, 20);
            put16(local, 6, flag);
            put16(local, 8, how);
            put16(local, 12, 0x21);
            put32(local, 14, crc.getValue());
            put32(local, 18, data.length);
            put32(local, 22, data.length);
            put16(local, 26, nameBytes.length);
            out.write(local, 0, local.length);
            out.write(nameBytes, 0, nameBytes.length);
            dataAt.put(name, Integer.valueOf(out.size()));
            out.write(data, 0, data.length);
            byte[] dir = new byte[46];
            put32(dir, 0, 0x02014b50L);
            put16(dir, 4, 0x0314);
            put16(dir, 6, 20);
            put16(dir, 8, flag);
            put16(dir, 10, how);
            put16(dir, 14, 0x21);
            put32(dir, 16, crc.getValue());
            put32(dir, 20, data.length);
            put32(dir, 24, data.length);
            put16(dir, 28, nameBytes.length);
            put32(dir, 38, 0100644L << 16);
            put32(dir, 42, offset);
            central.write(dir, 0, dir.length);
            central.write(nameBytes, 0, nameBytes.length);
        }

        /** Appends the 85-byte seal comment: "own-disc-seal sha256=" + the SHA-256 of everything before it. */
        static byte[] seal(byte[] withoutComment) {
            byte[] comment = ("own-disc-seal sha256=" + sha256(withoutComment)).getBytes(StandardCharsets.US_ASCII);
            byte[] out = Arrays.copyOf(withoutComment, withoutComment.length + comment.length);
            System.arraycopy(comment, 0, out, withoutComment.length, comment.length);
            return out;
        }

        /** Recomputes the seal of a bundle whose bytes were changed after build(). */
        static byte[] reseal(byte[] bundle) {
            return seal(Arrays.copyOf(bundle, bundle.length - 85));
        }
    }

    /** A stream that hands out its bytes in chunks of the given sizes, over and over (to stress buffer edges). */
    static InputStream chunked(final byte[] data, final int... sizes) {
        return new InputStream() {
            private int at, turn;

            @Override
            public int read() {
                return at >= data.length ? -1 : data[at++] & 0xFF;
            }

            @Override
            public int read(byte[] buffer, int offset, int length) {
                if (at >= data.length) return -1;
                int n = Math.min(Math.min(length, sizes[turn++ % sizes.length]), data.length - at);
                System.arraycopy(data, at, buffer, offset, n);
                at += n;
                return n;
            }
        };
    }

    /** A stream that fails the test when anything reads from it. */
    static InputStream untouchable() {
        return new InputStream() {
            @Override
            public int read() throws IOException {
                throw new IOException("the stream must not be read");
            }
        };
    }
}
