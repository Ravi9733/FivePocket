package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
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
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

/**
 * Shared helpers of the settings-core host tests (lane A of the app front end): a named-case runner, temporary
 * directories, directory snapshots, an in-memory FileView, the two reference capability sets and a directory that
 * mimics the phone's files/ folder. Not a test itself.
 */
final class SettingsCoreTestKit {
    private SettingsCoreTestKit() { }

    interface Case { void run() throws Exception; }

    private static String suite = "?";
    private static int passed, failed, skipped;

    /** Starts a suite. Like the other Java tests here, it refuses to run without -ea. */
    static void begin(String name) {
        boolean assertions = false;
        assert assertions = true;
        if (!assertions) throw new IllegalStateException("Run with -ea");
        suite = name;
        passed = failed = skipped = 0;
    }

    /** Runs one named case and prints PASS or FAIL with the reason. */
    static void test(String name, Case body) {
        try {
            body.run();
            passed++;
            System.out.println("PASS  " + suite + ": " + name);
        } catch (Throwable problem) {
            failed++;
            String where = "";
            for (StackTraceElement frame : problem.getStackTrace()) {
                if (frame.getClassName().contains(suite)) { where = " [" + frame.getFileName() + ":" + frame.getLineNumber() + "]"; break; }
            }
            System.out.println("FAIL  " + suite + ": " + name + where + "\n      " + problem);
        }
    }

    /** Records a case that could not run here, with the reason. A skip is printed, never counted as a pass. */
    static void skip(String name, String why) {
        skipped++;
        System.out.println("SKIP  " + suite + ": " + name + " (" + why + ")");
    }

    /** Prints the totals and ends the process with exit code 1 when a case failed. */
    static void end() {
        System.out.println("      " + suite + ": " + passed + " passed, " + failed + " failed, " + skipped + " skipped");
        if (failed > 0) System.exit(1);
    }

    // ---- checks

    static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    static void same(Object expected, Object actual, String what) {
        boolean equal = expected == null ? actual == null : expected.equals(actual);
        if (!equal) throw new AssertionError(what + ": expected <" + expected + "> but was <" + actual + ">");
    }

    static void sameBytes(byte[] expected, byte[] actual, String what) {
        if (!Arrays.equals(expected, actual)) {
            throw new AssertionError(what + ": expected <" + show(expected) + "> but was <" + show(actual) + ">");
        }
    }

    static String show(byte[] bytes) {
        if (bytes == null) return "absent";
        return "\"" + new String(bytes, StandardCharsets.ISO_8859_1).replace("\n", "\\n") + "\"";
    }

    /** The body must throw an exception of that type (or a subtype); it is returned. */
    static <T extends Throwable> T throwsA(Class<T> type, String what, Case body) {
        try {
            body.run();
        } catch (Throwable thrown) {
            if (type.isInstance(thrown)) return type.cast(thrown);
            throw new AssertionError(what + ": expected " + type.getSimpleName() + " but got " + thrown);
        }
        throw new AssertionError(what + ": expected " + type.getSimpleName() + " but nothing was thrown");
    }

    static byte[] ascii(String text) {
        return text.getBytes(StandardCharsets.US_ASCII);
    }

    // ---- capability sets

    // The probe names found in the real libraries on this machine. CapabilityScanReportTest scans the libraries
    // themselves and fails when one of these lists is not what its library gives.
    /** The library the phone ran until 2026-10-05 (qualified v669, sha256 78a1c820...): it reads none of the setting files. */
    static final String[] V669_NAMES = {};
    /** The seven input-file names build v458 contains. */
    static final String[] V458_NAMES = {"diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60", "diag-v409-scale-cubic",
            "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1"};
    /** Build v461: v458 plus the four pacing caps, the world sliders and the GSR 1 developer switch. */
    static final String[] V461_NAMES = {"diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60", "diag-v409-scale-cubic",
            "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1", "diag-pace-40", "diag-pace-30", "diag-pace-24",
            "diag-pace-auto", "world-load.txt", "diag-v460-scale-gsr1"};
    /** Build v462: v461 plus the two value files and the GSR 1 marker. */
    static final String[] V462_NAMES = {"diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60", "diag-v409-scale-cubic",
            "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1", "diag-pace-40", "diag-pace-30", "diag-pace-24",
            "diag-pace-auto", "world-load.txt", "diag-v460-scale-gsr1", "scaler.txt", "fsr-sharpness.txt", "recomp-cap:gsr1"};
    /** Build v463: v462 plus render resolution (stage A: every render size is brought to 1280x720 before the scaler). */
    static final String[] V463_NAMES = {"diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60", "diag-v409-scale-cubic",
            "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1", "diag-pace-40", "diag-pace-30", "diag-pace-24",
            "diag-pace-auto", "world-load.txt", "diag-v460-scale-gsr1", "scaler.txt", "fsr-sharpness.txt", "recomp-cap:gsr1",
            "render-scale.txt"};
    /** Build v464, the phone's standing library since 2026-10-05: v463 plus the marker of render-resolution stage B. */
    static final String[] V464_NAMES = {"diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60", "diag-v409-scale-cubic",
            "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1", "diag-pace-40", "diag-pace-30", "diag-pace-24",
            "diag-pace-auto", "world-load.txt", "diag-v460-scale-gsr1", "scaler.txt", "fsr-sharpness.txt", "recomp-cap:gsr1",
            "render-scale.txt", "recomp-cap:render-stage-b"};
    /** A future build with everything this app can set: v462 plus render resolution and unlocked draw distance. */
    static final String[] ALL_NAMES = {"diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60", "diag-v409-scale-cubic",
            "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1", "diag-pace-40", "diag-pace-30", "diag-pace-24",
            "diag-pace-auto", "world-load.txt", "diag-v460-scale-gsr1", "scaler.txt", "fsr-sharpness.txt", "recomp-cap:gsr1",
            "render-scale.txt", "recomp-cap:wl-lod"};
    /** The setting-related files standing on the phone on 2026-10-05 (agents/app-ui/apk/device-test/after-play/files.txt). */
    static final String[] PHONE_STANDING = {"private-trace/diag-fps-overlay", "private-trace/diag-v280-limit60",
            "private-trace/diag-v409-scale-cubic", "private-trace/diag-v450-scale-fsr1"};

    static Set<String> names(String... names) {
        return new TreeSet<String>(Arrays.asList(names));
    }

    /** A game library is present and contains exactly these names; the input option is available. */
    static Capabilities caps(String... names) {
        return Capabilities.of(names(names), true, true);
    }

    static Capabilities v458() { return caps(V458_NAMES); }

    static Capabilities v669() { return caps(V669_NAMES); }

    static Capabilities v461() { return caps(V461_NAMES); }

    static Capabilities v462() { return caps(V462_NAMES); }

    static Capabilities v463() { return caps(V463_NAMES); }

    static Capabilities v464() { return caps(V464_NAMES); }

    static Capabilities allLanes() { return caps(ALL_NAMES); }

    /** base plus more names. */
    static String[] plus(String[] base, String... more) {
        ArrayList<String> all = new ArrayList<String>(Arrays.asList(base));
        all.addAll(Arrays.asList(more));
        return all.toArray(new String[0]);
    }

    /** base without some names. */
    static String[] minus(String[] base, String... less) {
        ArrayList<String> all = new ArrayList<String>(Arrays.asList(base));
        all.removeAll(Arrays.asList(less));
        return all.toArray(new String[0]);
    }

    // ---- an in-memory files/ directory

    static final class MemoryFiles implements FileView {
        final TreeMap<String, byte[]> files = new TreeMap<String, byte[]>();

        MemoryFiles put(String path, String content) {
            files.put(path, ascii(content));
            return this;
        }

        /** Empty files under these paths. */
        MemoryFiles touch(String... paths) {
            for (String path : paths) files.put(path, new byte[0]);
            return this;
        }

        /** What a save does: null removes, anything else replaces. */
        MemoryFiles apply(Map<String, byte[]> plan) {
            for (Map.Entry<String, byte[]> entry : plan.entrySet()) {
                if (entry.getValue() == null) files.remove(entry.getKey());
                else files.put(entry.getKey(), entry.getValue());
            }
            return this;
        }

        @Override public boolean exists(String relativePath) {
            return files.containsKey(relativePath);
        }

        @Override public byte[] read(String relativePath) throws IOException {
            byte[] content = files.get(relativePath);
            if (content == null) throw new FileNotFoundException(relativePath);
            return content.length > 4096 ? Arrays.copyOf(content, 4096) : content;
        }
    }

    // ---- real directories

    private static final List<File> TEMPORARY = new ArrayList<File>();

    /** A new empty directory under java.io.tmpdir that is removed when the JVM ends. */
    static File tempDir() throws IOException {
        final File directory = Files.createTempDirectory("settings-core-test-").toFile().getCanonicalFile();
        synchronized (TEMPORARY) {
            if (TEMPORARY.isEmpty()) {
                Runtime.getRuntime().addShutdownHook(new Thread() {
                    @Override public void run() {
                        synchronized (TEMPORARY) {
                            for (File made : TEMPORARY) deleteTree(made);
                        }
                    }
                });
            }
            TEMPORARY.add(directory);
        }
        return directory;
    }

    /** Removes a directory this kit created, without following symbolic links. */
    private static void deleteTree(File root) {
        try {
            Files.walkFileTree(root.toPath(), new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) throws IOException {
                    Files.delete(file);
                    return FileVisitResult.CONTINUE;
                }

                @Override public FileVisitResult postVisitDirectory(Path directory, IOException problem) throws IOException {
                    Files.delete(directory);
                    return FileVisitResult.CONTINUE;
                }
            });
        } catch (IOException ignored) {
            // a leftover temporary directory is harmless
        }
    }

    /** Writes a file (plain, not atomic), creating its folders. */
    static void put(File root, String path, byte[] content) throws IOException {
        File file = new File(root, path);
        Files.createDirectories(file.getParentFile().toPath());
        Files.write(file.toPath(), content);
    }

    static void put(File root, String path, String content) throws IOException {
        put(root, path, content.getBytes(StandardCharsets.UTF_8));
    }

    /** The file's bytes, or null when nothing is there. */
    static byte[] bytes(File root, String path) throws IOException {
        File file = new File(root, path);
        return file.isFile() ? Files.readAllBytes(file.toPath()) : null;
    }

    /** A fixed moment in the past (2026-10-01 12:00:00 UTC), so that any rewrite shows as a newer time. */
    static final long OLD_TIME = 1790856000000L;

    /** Gives every file under root the modification time OLD_TIME. */
    static void age(File root) throws IOException {
        Files.walkFileTree(root.toPath(), new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) throws IOException {
                if (!attributes.isSymbolicLink() && !file.toFile().setLastModified(OLD_TIME)) throw new IOException("cannot set the time of " + file);
                return FileVisitResult.CONTINUE;
            }
        });
    }

    /**
     * Everything under root: path -> "dir", "link -> target" or "file &lt;size&gt; &lt;sha-256&gt; &lt;modification time&gt;".
     * Two snapshots are equal exactly when no file was added, removed, changed or rewritten in between.
     */
    static TreeMap<String, String> snapshot(final File root) throws IOException {
        final TreeMap<String, String> entries = new TreeMap<String, String>();
        final Path base = root.toPath();
        Files.walkFileTree(base, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult preVisitDirectory(Path directory, BasicFileAttributes attributes) {
                if (!directory.equals(base)) entries.put(base.relativize(directory).toString(), "dir");
                return FileVisitResult.CONTINUE;
            }

            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) throws IOException {
                String path = base.relativize(file).toString();
                if (Files.isSymbolicLink(file)) {
                    entries.put(path, "link -> " + Files.readSymbolicLink(file));
                } else {
                    entries.put(path, "file " + attributes.size() + " " + sha256(Files.readAllBytes(file)) + " "
                            + Files.getLastModifiedTime(file, LinkOption.NOFOLLOW_LINKS).toMillis());
                }
                return FileVisitResult.CONTINUE;
            }
        });
        return entries;
    }

    /** One line per difference between two snapshots: "added p", "removed p", "changed p". Empty when they are equal. */
    static List<String> differences(Map<String, String> before, Map<String, String> after) {
        ArrayList<String> lines = new ArrayList<String>();
        TreeSet<String> paths = new TreeSet<String>(before.keySet());
        paths.addAll(after.keySet());
        for (String path : paths) {
            String was = before.get(path), is = after.get(path);
            if (was == null) lines.add("added " + path);
            else if (is == null) lines.add("removed " + path);
            else if (!was.equals(is)) lines.add("changed " + path);
        }
        return lines;
    }

    static String sha256(byte[] content) {
        try {
            StringBuilder hex = new StringBuilder();
            for (byte b : MessageDigest.getInstance("SHA-256").digest(content)) hex.append(String.format("%02x", b & 255));
            return hex.toString();
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    // ---- a directory that mimics the phone

    /** Standing switch files of the phone that are NOT setting files: the settings code must never touch them. */
    static final String[] STANDING_FOREIGN = {"diag-cexec-bools", "diag-oc-queries", "diag-direct-present", "diag-v245-swap",
            "diag-v42x-scale-packed", "diag-v450-fsr1-no-rcas", "diag-jc-loop", "diag-df-float", "diag-pw-fix",
            "diag-v366-render-trims2", "diag-v362-render-trims", "diag-ms-prime-noyield", "diag-se-big", "diag-calls-gpr",
            "diag-v407-swap-delay", "diag-v312-release",
            // names that only start like a setting file
            "diag-fps-overlay-old", "diag-pace-400", "diag-v280-limit60.bak", "diag-v450-scale-fsr1x"};
    /** Standing switch files of the phone that ARE on the fixed list (the default scaler and the 60 fps measurement switch). */
    static final String[] STANDING_OWNED = {"diag-v450-scale-fsr1", "diag-v409-scale-cubic", "diag-v280-limit60"};

    /**
     * Builds, inside root, a files/ directory like the phone's before this app update: the game library (a small stand-in
     * that contains the given names as the real one does), the standing switch files, game data, a run directory and
     * saves. No settings folder. Every file gets the modification time OLD_TIME. Returns the files directory.
     */
    static File phoneFiles(File root, String[] libraryNames, String... extraPrivateTraceFiles) throws IOException {
        File files = new File(root, "files");
        for (String name : STANDING_FOREIGN) put(files, "private-trace/" + name, new byte[0]);
        for (String name : STANDING_OWNED) put(files, "private-trace/" + name, new byte[0]);
        for (String name : extraPrivateTraceFiles) put(files, "private-trace/" + name, new byte[0]);
        put(files, "private-trace/" + "private-trace-live.so", library(libraryNames));
        put(files, "private-trace/diag-v266-profile.txt", "not empty: a diagnostic value file that is not a setting\n");
        put(files, "private-trace/inputs/5f2a.bin", "game data stand-in");
        put(files, "private-trace/modules/boot.bin", "boot module stand-in");
        put(files, "private-trace/run-0123456789abcdef0123456789abcdef/arguments.bin", "arguments stand-in");
        put(files, "private-trace/run-0123456789abcdef0123456789abcdef/decoded.image.bin", "image stand-in");
        put(files, "saves/slot0", "the user's save stand-in");
        put(files, "saves/profile/autosave.bin", "another save stand-in");
        age(files);
        return files;
    }

    /** A stand-in for a game library: filler bytes with each name stored the way a compiler stores a string. */
    static byte[] library(String... names) {
        StringBuilder text = new StringBuilder("\u007fELF filler ");
        for (String name : names) text.append("/data/user/0/dev.recompbootstrap.smoke/files/private-trace/").append(name).append('\0').append("xx");
        text.append("more filler");
        return text.toString().getBytes(StandardCharsets.ISO_8859_1);
    }
}
