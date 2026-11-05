package dev.recompbootstrap.smoke;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.security.MessageDigest;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

/**
 * Shared by the launcher lane's host tests: a tiny runner that prints one line per test, and builders for a
 * temporary copy of the app's files directory (no real game data: every file holds a few made-up bytes).
 */
final class LauncherTestSupport {
    interface Case { void run() throws Exception; }

    private static int passed, failed;
    /** Temporary directories made by newFilesDir and not yet discarded; finish removes what a failed test left. */
    private static final java.util.List<File> temporary = new java.util.ArrayList<File>();

    private LauncherTestSupport() {}

    static void requireAssertions() {
        boolean assertions = false;
        assert assertions = true;
        if (!assertions) {
            throw new IllegalStateException("Run with -ea");
        }
    }

    /** Runs one named test and prints "PASS name" or "FAIL name: reason". */
    static void run(String name, Case test) {
        try {
            test.run();
            ++passed;
            System.out.println("PASS " + name);
        } catch (Throwable problem) {
            ++failed;
            System.out.println("FAIL " + name + ": " + problem);
            StackTraceElement[] trace = problem.getStackTrace();
            for (int index = 0; index < Math.min(4, trace.length); ++index) {
                System.out.println("       at " + trace[index]);
            }
        }
    }

    /** Prints the totals and ends the JVM with a non-zero code when a test failed. */
    static void finish(String suite) {
        for (File left : new java.util.ArrayList<File>(temporary)) {
            discard(left);
        }
        System.out.println(suite + ": " + passed + " passed, " + failed + " failed");
        if (failed != 0) {
            System.exit(1);
        }
    }

    static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    static void equal(Object expected, Object actual, String what) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(what + ": expected <" + expected + "> but was <" + actual + ">");
        }
    }

    // ---- a temporary files directory ----

    static final String ID_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", ID_B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            ID_C = "cccccccccccccccccccccccccccccccc", ID_D = "dddddddddddddddddddddddddddddddd";

    /** A new empty directory standing in for the app's files directory. The path has no links in it. */
    static File newFilesDir() throws IOException {
        File files = new File(Files.createTempDirectory("launcher-test").toFile().getCanonicalFile(), "files");
        check(files.mkdir(), "create " + files);
        temporary.add(files);
        return files;
    }

    static File write(File file, byte[] content) throws IOException {
        Files.createDirectories(file.getParentFile().toPath());
        Files.write(file.toPath(), content);
        return file;
    }

    static File write(File file, String text) throws IOException {
        return write(file, text.getBytes(StandardCharsets.US_ASCII));
    }

    static byte[] bytes(int count, int seed) {
        byte[] content = new byte[count];
        for (int index = 0; index < count; ++index) {
            content[index] = (byte) (index * 31 + seed);
        }
        return content;
    }

    /** Zero-terminated arguments, the form of a run's arguments.bin. */
    static byte[] join(String... arguments) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (String argument : arguments) {
            byte[] text = argument.getBytes(StandardCharsets.ISO_8859_1);
            out.write(text, 0, text.length);
            out.write(0);
        }
        return out.toByteArray();
    }

    /**
     * Start arguments the game accepts for run id under root (the private-trace directory): the program path, the
     * image path, nine plain values, then the extra arguments.
     */
    static String[] validArguments(String root, String id, String... extra) {
        String run = root + "/run-" + id + "/";
        String[] head = {run + "host_guest_trace", run + "decoded.image.bin", "2181038080", "100", "0", "1", "2", "3",
                "4", "5", "6"};
        String[] all = new String[head.length + extra.length];
        System.arraycopy(head, 0, all, 0, head.length);
        System.arraycopy(extra, 0, all, head.length, extra.length);
        return all;
    }

    static File privateTrace(File files) {
        return new File(files, "private-trace");
    }

    /** Creates private-trace/inputs/(name) with a few bytes and returns its absolute path. */
    static String addInput(File files, String name) throws IOException {
        return write(new File(privateTrace(files), "inputs/" + name), bytes(40, name.hashCode())).getAbsolutePath();
    }

    /** Creates private-trace/(relative) as a directory and returns its absolute path. */
    static String addDirectory(File files, String relative) throws IOException {
        File directory = new File(privateTrace(files), relative);
        Files.createDirectories(directory.toPath());
        return directory.getAbsolutePath();
    }

    /**
     * Creates a start request private-trace/run-(id) the way a measurement script leaves it: the directory is mode
     * 0700, arguments.bin is mode 0400 and was last changed at modifiedMs, the image holds made-up bytes.
     * The arguments name the given paths, each after a "--read-only-file" flag.
     */
    static File addRun(File files, String id, long modifiedMs, String... namedPaths) throws IOException {
        String[] extra = new String[namedPaths.length * 2];
        for (int index = 0; index < namedPaths.length; ++index) {
            extra[index * 2] = "--read-only-file";
            extra[index * 2 + 1] = namedPaths[index];
        }
        return addRunWithArguments(files, id, modifiedMs,
                join(validArguments(privateTrace(files).getAbsolutePath(), id, extra)));
    }

    static File addRunWithArguments(File files, String id, long modifiedMs, byte[] arguments) throws IOException {
        File run = new File(privateTrace(files), "run-" + id);
        File argumentsFile = write(new File(run, "arguments.bin"), arguments);
        File image = write(new File(run, "decoded.image.bin"), bytes(5000, id.charAt(0)));
        mode(run, "rwx------");
        mode(argumentsFile, "r--------");
        mode(image, "r--------");
        check(argumentsFile.setLastModified(modifiedMs), "set the time of " + argumentsFile);
        return run;
    }

    /**
     * The game code as the installer leaves it, so that the game window's loader would accept it: the directory
     * private-trace is mode 0700, the file is mode 0400, has one name and begins with the header of a 64-bit ARM
     * shared library (the rest are made-up bytes; a count below 64 gives a file that is too short to be one).
     * The shader file that belongs to the game code is put next to it when it is not there yet (addShaders).
     */
    static File addCode(File files, int count) throws IOException {
        File code = write(new File(privateTrace(files), "private-trace-live.so"), libraryBytes(count));
        mode(code, "r--------");
        mode(privateTrace(files), "rwx------");
        if (!new File(privateTrace(files), InstallState.SHADER_FILE).exists()) {
            addShaders(files);
        }
        return code;
    }

    /** count made-up bytes that begin, when there is room, with the header the game window's loader requires. */
    static byte[] libraryBytes(int count) {
        byte[] content = bytes(count, 7);
        // "\177ELF", 64-bit, little-endian, version 1; type 3 (shared object), machine 0xB7 (64-bit ARM); entry 0
        int[] header = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0xb7, 0, 1, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0};
        for (int at = 0; at < header.length && at < count; ++at) {
            content[at] = (byte) header[at];
        }
        return content;
    }

    /**
     * Creates the shader file next to the game code, with a few made-up bytes. Its name is the launcher's own
     * constant here on purpose: that the name is the one the game code opens is checked in one place, against the
     * real game library (LauncherContractTest).
     */
    static File addShaders(File files) throws IOException {
        return write(new File(privateTrace(files), InstallState.SHADER_FILE), bytes(200, 9));
    }

    /** One state of the installed files: complete, or with one thing wrong. Each is built as real files by tree(). */
    enum Install {
        READY, READY_VERSION_ON_RECORD, READY_PIN_UNUSABLE,
        NOTHING, DISC_COPY_PARTIAL, DISC_COPY_COMPLETE, DATA_MISSING, DATA_ONE_FILE_GONE, DATA_NAMES_NOTHING, CODE_MISSING,
        CODE_NOT_A_PROGRAM, CODE_WRITABLE, CODE_TWO_NAMES, CODE_FOLDER_OPEN, SHADERS_MISSING
    }

    /**
     * A new files directory in that state: two game files, one directory and one start request (ID_A) that names
     * them, the game code and its shader file, with the one thing wrong. The caller discards it.
     */
    static File tree(Install state) throws IOException {
        File files = newFilesDir();
        File root = privateTrace(files);
        if (state == Install.NOTHING) {
            return files;
        }
        if (state == Install.DISC_COPY_PARTIAL || state == Install.DISC_COPY_COMPLETE) {
            write(new File(files, "game-install/install.json"), "{\"version\":1,\"complete\":"
                    + (state == Install.DISC_COPY_COMPLETE) + "}\n");
            if (state == Install.DISC_COPY_COMPLETE) {
                write(new File(files, "game-install/bundle/code.so"), bytes(100, 5));
            }
            return files;
        }
        if (state == Install.DATA_NAMES_NOTHING) {
            addRun(files, ID_A, 1_700_000_000_000L);
        } else if (state != Install.DATA_MISSING) {
            addRun(files, ID_A, 1_700_000_000_000L, addInput(files, "one.bin"), addInput(files, "two.bin"),
                    addDirectory(files, "inputs/native-cache"));
        }
        File code = null;
        if (state != Install.CODE_MISSING) {
            code = state == Install.CODE_NOT_A_PROGRAM
                    ? write(new File(root, "private-trace-live.so"), bytes(4096, 7)) : addCode(files, 4096);
            mode(code, "r--------");
            mode(root, "rwx------");
            check(code.setLastModified(1_791_134_800_000L), "set the code time");   // 2026-10-04 17:26:40 UTC
        }
        if (state != Install.SHADERS_MISSING && !new File(root, InstallState.SHADER_FILE).exists()) {
            addShaders(files);
        }
        switch (state) {
            case DATA_ONE_FILE_GONE:
                check(new File(root, "inputs/two.bin").delete(), "remove a game file");
                break;
            case CODE_WRITABLE:
                mode(code, "rw-------");
                break;
            case CODE_TWO_NAMES:
                Files.createLink(new File(root, "private-trace-live-second-name.so").toPath(), code.toPath());
                break;
            case CODE_FOLDER_OPEN:
                mode(root, "rwxr-xr-x");
                break;
            case SHADERS_MISSING:
                check(new File(root, InstallState.SHADER_FILE).delete(), "remove the shader file");
                break;
            case READY_VERSION_ON_RECORD:
                write(new File(files, "settings/qualified-code.txt"), sha256Quietly(Files.readAllBytes(code.toPath())) + "\n");
                break;
            case READY_PIN_UNUSABLE:
                write(new File(files, "settings/launch-template.txt"), ID_C + "\n");   // names a run that is not there
                break;
            default:
                break;
        }
        return files;
    }

    private static String sha256Quietly(byte[] content) throws IOException {
        try {
            return sha256(content);
        } catch (Exception impossible) {
            throw new IOException(impossible);
        }
    }

    /** What InstallState.detect answers for that state (the tree is built, looked at and discarded). */
    static InstallState detected(Install state) throws IOException {
        File files = tree(state);
        InstallState answer = InstallState.detect(files);
        discard(files);
        return answer;
    }

    /** Files that belong to others and must never change: saves, a standing switch, an unrelated settings file. */
    static void addDecoys(File files) throws IOException {
        write(new File(files, "saves/slot0"), bytes(300, 1));
        write(new File(files, "saves/profile/autosave.bin"), bytes(700, 2));
        write(new File(privateTrace(files), "diag-cexec-bools"), new byte[0]);
        write(new File(privateTrace(files), "diag-oc-queries"), new byte[0]);
        write(new File(files, "settings/settings.json"), "{\"version\":1}\n");
    }

    static void mode(File file, String permissions) throws IOException {
        Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString(permissions));
    }

    static String mode(File file) throws IOException {
        Set<PosixFilePermission> permissions = Files.getPosixFilePermissions(file.toPath(), LinkOption.NOFOLLOW_LINKS);
        return PosixFilePermissions.toString(permissions);
    }

    static String sha256(byte[] content) throws Exception {
        StringBuilder text = new StringBuilder();
        for (byte part : MessageDigest.getInstance("SHA-256").digest(content)) {
            text.append(String.format("%02x", part & 255));
        }
        return text.toString();
    }

    /**
     * Every entry below root: relative path -> kind, permissions and, for files, size, modification time and content
     * hash (for links: the target). With directoryTimes the modification time of each directory is included too, which
     * shows that nothing was created or removed inside it, even for a moment.
     */
    static Map<String, String> snapshot(File root, boolean directoryTimes) throws Exception {
        Map<String, String> entries = new TreeMap<String, String>();
        snapshot(root.toPath(), root.toPath(), directoryTimes, entries);
        return entries;
    }

    private static void snapshot(Path root, Path at, boolean directoryTimes, Map<String, String> entries)
            throws Exception {
        String name = root.equals(at) ? "." : root.relativize(at).toString();
        if (Files.isSymbolicLink(at)) {
            entries.put(name, "link -> " + Files.readSymbolicLink(at));
        } else if (Files.isDirectory(at, LinkOption.NOFOLLOW_LINKS)) {
            entries.put(name, "directory " + mode(at.toFile())
                    + (directoryTimes ? " " + Files.getLastModifiedTime(at).toMillis() : ""));
            File[] children = at.toFile().listFiles();
            if (children != null) {
                for (File child : children) {
                    snapshot(root, child.toPath(), directoryTimes, entries);
                }
            }
        } else {
            String permissions = mode(at.toFile());
            String content = Files.isReadable(at) ? sha256(Files.readAllBytes(at)) : "unreadable";
            entries.put(name, "file " + permissions + " " + Files.size(at) + " "
                    + Files.getLastModifiedTime(at, LinkOption.NOFOLLOW_LINKS).toMillis() + " " + content);
        }
    }

    /** A short description of what differs between two snapshots; empty when they are equal. */
    static String difference(Map<String, String> before, Map<String, String> after) {
        StringBuilder text = new StringBuilder();
        for (Map.Entry<String, String> entry : before.entrySet()) {
            String now = after.get(entry.getKey());
            if (now == null) {
                text.append(" removed:").append(entry.getKey());
            } else if (!now.equals(entry.getValue())) {
                text.append(" changed:").append(entry.getKey());
            }
        }
        for (String name : after.keySet()) {
            if (!before.containsKey(name)) {
                text.append(" added:").append(name);
            }
        }
        return text.toString().trim();
    }

    /** Removes the temporary tree that newFilesDir made (and nothing else: the name is checked). */
    static void discard(File filesDir) {
        File top = filesDir.getParentFile();
        check(top != null && top.getName().startsWith("launcher-test"), "not a test directory: " + filesDir);
        temporary.remove(filesDir);
        deleteTree(top);
    }

    private static void deleteTree(File root) {
        if (!Files.isSymbolicLink(root.toPath()) && root.isDirectory()) {
            root.setReadable(true);
            root.setWritable(true);
            root.setExecutable(true);
            File[] children = root.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteTree(child);
                }
            }
        }
        root.delete();
    }
}
