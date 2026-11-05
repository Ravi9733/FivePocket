package dev.recompbootstrap.smoke;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.BasicFileAttributes;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Shared by the Ship*Test classes: checks, temporary files directories, tree snapshots, and a made-up "bundle" (what
 * would travel in the APK) and "data folder" (what the player would pick). Nothing here is game code or game data.
 * The record (config.json) and the folder list are written here as text, by hand, so that the tests do not get
 * their expected format from the code they test.
 */
final class ShipTestKit {
    private ShipTestKit() {}

    interface Check {
        void run() throws Exception;
    }

    private static int passed, failed;
    private static final List<File> TEMP = new ArrayList<File>();

    static void check(String name, Check check) {
        try {
            check.run();
            ++passed;
            System.out.println("PASS  " + name);
        } catch (Throwable problem) {
            ++failed;
            System.out.println("FAIL  " + name + "\n      " + problem);
            for (StackTraceElement at : problem.getStackTrace()) {
                if (at.getClassName().endsWith("Test") || at.getClassName().contains("Test$")) {
                    System.out.println("      at " + at);
                    break;
                }
            }
        }
    }

    static void finish(String suite) {
        for (File dir : TEMP) deleteTree(dir);
        System.out.println(suite + ": " + passed + " passed, " + failed + " failed");
        System.exit(failed == 0 ? 0 : 1);
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
        if (text == null || !text.contains(part)) {
            throw new AssertionError(what + ": expected to contain <" + part + "> but was <" + text + ">");
        }
    }

    // ---- bytes and files -----------------------------------------------------------------------------------

    /** length made-up bytes that depend on seed. */
    static byte[] bytes(int length, int seed) {
        byte[] data = new byte[length];
        long state = 0x9E3779B97F4A7C15L * (seed + 1);
        for (int at = 0; at < length; ++at) {
            state ^= state << 13;
            state ^= state >>> 7;
            state ^= state << 17;
            data[at] = (byte) state;
        }
        return data;
    }

    /** Made-up bytes behind the 64-byte header of a 64-bit ARM shared library: what the loader's file rule looks at. */
    static byte[] library(int length, int seed) {
        byte[] data = bytes(length, seed);
        byte[] header = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, (byte) 0xb7, 0, 1, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0};
        System.arraycopy(header, 0, data, 0, header.length);
        return data;
    }

    static String sha256(byte[] data) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            StringBuilder text = new StringBuilder();
            for (byte b : digest) text.append(String.format("%02x", b & 255));
            return text.toString();
        } catch (Exception impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    /** A new, empty "files" directory (the app's Context.getFilesDir()) in its own temporary folder. */
    static File newFilesDir(String label) throws IOException {
        File base = Files.createTempDirectory("ship-" + label + "-").toFile();
        TEMP.add(base);
        File files = new File(base, "files");
        if (!files.mkdir()) throw new IOException("cannot create " + files);
        return files;
    }

    static void write(File file, byte[] data) throws IOException {
        File parent = file.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
        Files.write(file.toPath(), data);
    }

    static byte[] read(File file) throws IOException {
        return Files.readAllBytes(file.toPath());
    }

    static String text(File file) throws IOException {
        return new String(read(file), StandardCharsets.US_ASCII);
    }

    static String mode(File file) throws IOException {
        return PosixFilePermissions.toString(Files.getPosixFilePermissions(file.toPath(), LinkOption.NOFOLLOW_LINKS));
    }

    /**
     * Every file and directory under dir: relative path -> kind, size, SHA-256, mode, modification time and the
     * file system's own identity of the file. Two equal snapshots mean that nothing was created, removed,
     * rewritten or replaced in between (a replaced file has another identity even with the same bytes).
     */
    static TreeMap<String, String> snapshot(final File dir) throws IOException {
        return snapshot(dir, true);
    }

    /** As above; withIdentity false leaves out modification time and identity (to compare two different trees). */
    static TreeMap<String, String> snapshot(final File dir, final boolean withIdentity) throws IOException {
        final TreeMap<String, String> found = new TreeMap<String, String>();
        final Path root = dir.toPath();
        Files.walkFileTree(root, new SimpleFileVisitor<Path>() {
            private void add(Path path, BasicFileAttributes attributes, String kind) throws IOException {
                if (path.equals(root)) return;
                String entry = kind + " " + PosixFilePermissions.toString(Files.getPosixFilePermissions(path, LinkOption.NOFOLLOW_LINKS));
                if (attributes.isRegularFile()) entry += " " + attributes.size() + " " + sha256(Files.readAllBytes(path));
                if (withIdentity && attributes.isRegularFile()) {
                    entry += " " + attributes.lastModifiedTime().toMillis() + " " + attributes.fileKey();
                }
                found.put(root.relativize(path).toString(), entry);
            }

            @Override public FileVisitResult preVisitDirectory(Path path, BasicFileAttributes attributes) throws IOException {
                add(path, attributes, "dir");
                return FileVisitResult.CONTINUE;
            }

            @Override public FileVisitResult visitFile(Path path, BasicFileAttributes attributes) throws IOException {
                add(path, attributes, attributes.isSymbolicLink() ? "link" : "file");
                return FileVisitResult.CONTINUE;
            }
        });
        return found;
    }

    /** The entries of after that are missing from or different in before, and the ones that disappeared ("-path"). */
    static List<String> changes(Map<String, String> before, Map<String, String> after) {
        List<String> changed = new ArrayList<String>();
        for (Map.Entry<String, String> entry : after.entrySet()) {
            if (!entry.getValue().equals(before.get(entry.getKey()))) changed.add(entry.getKey());
        }
        for (String path : before.keySet()) {
            if (!after.containsKey(path)) changed.add("-" + path);
        }
        return changed;
    }

    /** Copies a whole tree with modes and times: a picture of the files directory at one moment. */
    static void copyTree(final File from, final File to) throws IOException {
        final Path source = from.toPath(), target = to.toPath();
        Files.walkFileTree(source, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult preVisitDirectory(Path path, BasicFileAttributes attributes) throws IOException {
                Path made = target.resolve(source.relativize(path));
                if (!Files.isDirectory(made)) Files.createDirectory(made);
                return FileVisitResult.CONTINUE;
            }

            @Override public FileVisitResult visitFile(Path path, BasicFileAttributes attributes) throws IOException {
                Files.copy(path, target.resolve(source.relativize(path)), StandardCopyOption.COPY_ATTRIBUTES, LinkOption.NOFOLLOW_LINKS);
                return FileVisitResult.CONTINUE;
            }

            @Override public FileVisitResult postVisitDirectory(Path path, IOException error) throws IOException {
                Files.setPosixFilePermissions(target.resolve(source.relativize(path)), Files.getPosixFilePermissions(path));
                return FileVisitResult.CONTINUE;
            }
        });
    }

    /** Removes everything inside dir (and dir itself when andItself). */
    static void deleteTree(File dir) {
        try {
            final Path root = dir.toPath();
            if (!Files.exists(root, LinkOption.NOFOLLOW_LINKS)) return;
            Files.walkFileTree(root, new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult preVisitDirectory(Path path, BasicFileAttributes attributes) throws IOException {
                    path.toFile().setWritable(true);
                    path.toFile().setExecutable(true);
                    path.toFile().setReadable(true);
                    return FileVisitResult.CONTINUE;
                }

                @Override public FileVisitResult visitFile(Path path, BasicFileAttributes attributes) throws IOException {
                    Files.delete(path);
                    return FileVisitResult.CONTINUE;
                }

                @Override public FileVisitResult postVisitDirectory(Path path, IOException error) throws IOException {
                    Files.delete(path);
                    return FileVisitResult.CONTINUE;
                }
            });
        } catch (IOException ignored) {
            // a temporary folder that stays behind is harmless
        }
    }

    static void emptyDirectory(File dir) throws IOException {
        File[] children = dir.listFiles();
        if (children == null) throw new IOException("cannot list " + dir);
        for (File child : children) deleteTree(child);
    }

    // ---- free space ----------------------------------------------------------------------------------------

    static final ShipFiles.Space PLENTY = spaceOf(Long.MAX_VALUE / 4);

    static ShipFiles.Space spaceOf(final long bytes) {
        return new ShipFiles.Space() {
            @Override public long usable(File existingDirectory) {
                return bytes;
            }
        };
    }

    // ---- what travels in the APK ---------------------------------------------------------------------------

    /** The APK's assets, in memory. */
    static final class MemoryBundle implements ShipBundle {
        final Map<String, byte[]> assets = new LinkedHashMap<String, byte[]>();
        final List<String> opened = new ArrayList<String>();

        @Override public InputStream open(String asset) throws IOException {
            byte[] data = assets.get(asset);
            if (data == null) throw new FileNotFoundException(asset);
            opened.add(asset);
            return new ByteArrayInputStream(data);
        }

        @Override public void close() {}
    }

    /**
     * A made-up app version: library, shader pack, start request, switch names, and the record that describes them,
     * for one files directory. Change the public fields, then call build() again for another version.
     */
    static final class Version {
        final File files;
        String build = "t1";
        String runId = "0123456789abcdef0123456789abcdef";
        String filesRoot;
        String shaderPackName = "generic-shaders-v41.bin";
        byte[] library = library(300000, 1), pack = bytes(150000, 2);
        /** The game data the start request names: made-up bytes, named by their SHA-256. */
        final List<byte[]> inputs = new ArrayList<byte[]>();
        final List<String> directories = new ArrayList<String>();
        final List<String> switches = new ArrayList<String>();
        /**
         * The text of the record's "data" list (the pin). null: build() pins exactly dataOf(this), which is what
         * tools/ship_stage.py does with the real record. Set it to pin something else; NO_PIN leaves the list out.
         */
        String dataPin;
        static final String NO_PIN = new String("no pin");
        /**
         * Paths of dataOf(this) that travel inside the APK (the record's "pieces" list and the assets
         * ship/pieces/(path)). Empty by default: then every data file comes from the player's folder.
         */
        final List<String> bundledPaths = new ArrayList<String>();
        /** Set by build(). */
        byte[] request, record;
        final MemoryBundle bundle = new MemoryBundle();

        Version(File files) {
            this.files = files;
            this.filesRoot = files.getAbsolutePath();
            inputs.add(bytes(70000, 11));
            inputs.add(bytes(14, 12));
            inputs.add(bytes((1 << 20) + 77, 13));
            directories.add("inputs/native-cache");
            directories.add("modules/boot");
            switches.add("diag-direct-present");
            switches.add("diag-fps-overlay");       // one the settings screen owns (RuntimeMapping.ownedFiles)
            switches.add("diag-v260-write-watch");
        }

        String root() {
            return files.getAbsolutePath() + "/private-trace";
        }

        static String inputPath(byte[] input) {
            return "inputs/" + sha256(input) + ".bin";
        }

        /** The start request of the real one's shape for this files directory: zero-separated arguments. */
        byte[] requestFor(String id) {
            List<String> arguments = new ArrayList<String>();
            String run = root() + "/run-" + id;
            arguments.add(run + "/host_guest_trace");
            arguments.add(run + "/decoded.image.bin");
            for (String number : new String[] {"2181038080", "31162368", "268435456", "67108864", "1879048192", "65536",
                    "1000000000000000000", "64", "95837"}) {
                arguments.add(number);
            }
            for (byte[] input : inputs) {
                arguments.add("--read-only-file");
                arguments.add(root() + "/" + inputPath(input));
            }
            for (String directory : directories) {
                arguments.add("--directory");
                arguments.add(root() + "/" + directory);
            }
            arguments.add("--native-input");
            StringBuilder text = new StringBuilder();
            for (String argument : arguments) text.append(argument).append('\0');
            return text.toString().getBytes(StandardCharsets.US_ASCII);
        }

        Version build() {
            request = requestFor(runId);
            StringBuilder json = new StringBuilder();
            json.append("{\"schema\":\"gta5-ship-config/1\",\"build\":\"").append(build).append("\",\"filesRoot\":\"")
                    .append(filesRoot).append("\",\n");
            json.append("\"library\":{\"asset\":\"ship/game-library.bin\",\"size\":").append(library.length)
                    .append(",\"sha256\":\"").append(sha256(library)).append("\"},\n");
            json.append("\"shaderPack\":{\"asset\":\"ship/shader-pack.bin\",\"name\":\"").append(shaderPackName)
                    .append("\",\"size\":").append(pack.length).append(",\"sha256\":\"").append(sha256(pack)).append("\"},\n");
            json.append("\"startRequest\":{\"asset\":\"ship/start-request.bin\",\"runId\":\"").append(runId)
                    .append("\",\"size\":").append(request.length).append(",\"sha256\":\"").append(sha256(request)).append("\"},\n");
            json.append("\"switches\":[").append(quoted(switches)).append("],\n");
            json.append("\"directories\":[").append(quoted(directories)).append("],\n");
            List<String> required = new ArrayList<String>();
            for (byte[] input : inputs) required.add(inputPath(input));
            json.append("\"requiredInputs\":[").append(quoted(required)).append("]");
            Map<String, byte[]> data = dataOf(this);
            if (dataPin != NO_PIN) json.append(",\n\"data\":[").append(dataPin != null ? dataPin : pinOf(data)).append("]");
            bundle.assets.clear();
            if (!bundledPaths.isEmpty()) {
                StringBuilder pieces = new StringBuilder();
                for (String path : bundledPaths) {
                    byte[] bytes = data.get(path);
                    if (bytes == null) throw new IllegalStateException("not a data file of this version: " + path);
                    if (pieces.length() > 0) pieces.append(',');
                    pieces.append("\n{\"asset\":\"ship/pieces/").append(path).append("\",\"path\":\"").append(path)
                            .append("\",\"size\":").append(bytes.length).append(",\"sha256\":\"").append(sha256(bytes)).append("\"}");
                    bundle.assets.put("ship/pieces/" + path, bytes);
                }
                json.append(",\n\"pieces\":[").append(pieces).append("]");
            }
            json.append("}\n");
            record = json.toString().getBytes(StandardCharsets.US_ASCII);
            bundle.assets.put("ship/config.json", record);
            bundle.assets.put("ship/game-library.bin", library);
            bundle.assets.put("ship/shader-pack.bin", pack);
            bundle.assets.put("ship/start-request.bin", request);
            return this;
        }

        /** The text of a "data" list that pins exactly these files. */
        static String pinOf(Map<String, byte[]> data) {
            StringBuilder text = new StringBuilder();
            for (Map.Entry<String, byte[]> entry : data.entrySet()) {
                if (text.length() > 0) text.append(',');
                text.append("\n{\"path\":\"").append(entry.getKey()).append("\",\"size\":").append(entry.getValue().length)
                        .append(",\"sha256\":\"").append(sha256(entry.getValue())).append("\"}");
            }
            return text.toString();
        }

        private static String quoted(List<String> names) {
            StringBuilder text = new StringBuilder();
            for (String name : names) {
                if (text.length() > 0) text.append(',');
                text.append('"').append(name).append('"');
            }
            return text.toString();
        }

        ShipConfig config() throws IOException {
            return ShipConfig.parse(record);
        }
    }

    // ---- what the player picks -----------------------------------------------------------------------------

    /** The chosen folder, in memory: paths -> bytes, with a record of what was opened. */
    static final class MemoryFolder implements ShipDataImport.Source {
        final Map<String, byte[]> files = new LinkedHashMap<String, byte[]>();
        final List<String> opened = new ArrayList<String>();

        @Override public InputStream open(String relativePath) {
            byte[] data = files.get(relativePath);
            if (data == null) return null;
            opened.add(relativePath);
            return new ByteArrayInputStream(data);
        }

        /** The folder's list, written by hand: one entry per (path, bytes) in order. */
        static String list(Map<String, byte[]> entries) {
            StringBuilder json = new StringBuilder("{\"schema\":\"gta5-game-data/1\",\"files\":[");
            boolean first = true;
            for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
                if (!first) json.append(',');
                first = false;
                json.append("\n{\"path\":\"").append(entry.getKey()).append("\",\"size\":").append(entry.getValue().length)
                        .append(",\"sha256\":\"").append(sha256(entry.getValue())).append("\"}");
            }
            return json.append("]}\n").toString();
        }

        /** Puts the data files and a list that describes exactly them. */
        MemoryFolder with(Map<String, byte[]> entries) {
            files.putAll(entries);
            files.put("game-data-manifest.json", list(entries).getBytes(StandardCharsets.US_ASCII));
            return this;
        }

        /** Replaces the list by the given text (for lists that do not describe the folder). */
        MemoryFolder listed(String json) {
            files.put("game-data-manifest.json", json.getBytes(StandardCharsets.US_ASCII));
            return this;
        }
    }

    /** The three boot modules and the image of dataOf: what the real APK carries as bundled pieces, in small. */
    static final String[] GENERATED = {"modules/boot/r8/vertex/dataflow.spv", "modules/boot/store/dataflow.spv",
            "modules/boot/depth/vertex/guest-code.bin", "decoded.image.bin"};

    /**
     * A disk of a fixed size: what is free is the size minus what lies under the files directory at that moment.
     * With it a part file that a killed copy left behind costs room, as on a real device.
     */
    static ShipFiles.Space diskOf(final File files, final long capacity) {
        return new ShipFiles.Space() {
            @Override public long usable(File existingDirectory) {
                final long[] used = {0};
                try {
                    Files.walkFileTree(files.toPath(), new SimpleFileVisitor<Path>() {
                        @Override public FileVisitResult visitFile(Path path, BasicFileAttributes attributes) {
                            if (attributes.isRegularFile()) used[0] += attributes.size();
                            return FileVisitResult.CONTINUE;
                        }
                    });
                } catch (IOException e) {
                    throw new IllegalStateException(e);
                }
                return Math.max(0, capacity - used[0]);
            }
        };
    }

    /** The bytes of every regular file under dir. */
    static long treeBytes(File dir) {
        return Long.MAX_VALUE / 4 - diskOf(dir, Long.MAX_VALUE / 4).usable(dir);
    }

    /** The game data that belongs to a Version: its inputs, three boot modules and an image. */
    static Map<String, byte[]> dataOf(Version version) {
        Map<String, byte[]> entries = new LinkedHashMap<String, byte[]>();
        for (byte[] input : version.inputs) entries.put(Version.inputPath(input), input);
        entries.put("modules/boot/r8/vertex/dataflow.spv", bytes(34952, 31));
        entries.put("modules/boot/store/dataflow.spv", bytes(8964, 32));
        entries.put("modules/boot/depth/vertex/guest-code.bin", bytes(108, 33));
        entries.put("decoded.image.bin", bytes(500000, 34));
        return entries;
    }

    /** Records what ShipDataImport reports; can ask it to stop once a number of bytes was reported. */
    static final class Recorder implements ShipDataImport.Progress {
        long bytesDone, bytesTotal, stopAtBytes = Long.MAX_VALUE;
        int filesDone, filesTotal;
        boolean decreasing;

        @Override public void update(long done, long total, int files, int filesOf, String current) {
            if (done < bytesDone) decreasing = true;
            bytesDone = done;
            bytesTotal = total;
            filesDone = files;
            filesTotal = filesOf;
        }

        @Override public boolean cancelled() {
            return bytesDone >= stopAtBytes;
        }
    }

    static final ShipFirstRun.Progress QUIET = new ShipFirstRun.Progress() {
        @Override public void at(ShipFirstRun.Step step, long bytesDone, long bytesTotal) {}
    };
}
