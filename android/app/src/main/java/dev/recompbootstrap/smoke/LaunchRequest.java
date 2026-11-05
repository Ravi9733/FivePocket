package dev.recompbootstrap.smoke;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.PosixFilePermission;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Collections;
import java.util.EnumSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Makes the start request the game activity expects (design section 4.C). Pure Java.
 *
 * A measurement script starts the game by creating a directory files/private-trace/run-(id) that holds the game image
 * and a file of start arguments, and then starting the game activity with four text extras. This class does the same
 * on the device, from the launcher: it copies the newest earlier request (the "template") under a new id.
 *
 * What it creates: one directory private-trace/run-(new id) with two files, and its own list
 * settings/launcher-runs.txt. What it deletes: only the file decoded.image.bin inside directories whose id is in that
 * list (copies this class made earlier). It never touches anything else, and nothing under files/saves.
 *
 * The version check: the install kit (or the developer) places the read-only file settings/qualified-code.txt holding
 * the SHA-256 of the game code that is meant to be played. When it exists and the game code file hashes to something
 * else (a build left behind by a measurement run, for example), prepare stops before it creates anything and
 * reports UNQUALIFIED; the launcher then asks, and only a second, explicit Play starts that code. The app never
 * writes that file. Without the file nothing is compared (InstallState.versionOnRecord says which it is).
 *
 * Every reason prepare can give is one of the ERROR_ texts below or starts with one of the _PREFIX texts;
 * LaunchAdvice turns each into the sentence the user reads, with what to do next.
 */
final class LaunchRequest {
    /** The value of the "private_trace" extra the measurement scripts use for a normal game start. */
    static final String MODE = "run-native-display";
    static final String RUNS_LIST = "settings/launcher-runs.txt";
    /** Optional, placed by the developer: the SHA-256 (64 hex digits, first word of the file) of the qualified game code. */
    static final String QUALIFIED_CODE = "settings/qualified-code.txt";
    /** The error of prepare when the game code is not the build named in QUALIFIED_CODE. Nothing was created. */
    static final String UNQUALIFIED = "the game code is not the qualified build";
    /** The other reasons prepare gives. Nothing was created for any of them except, possibly, the IO one. */
    static final String ERROR_CODE_MISSING = "the game code is missing";
    static final String ERROR_NOT_INSTALLED = "the game files are not installed";
    static final String ERROR_SHADERS_MISSING = "the shader file is missing";
    /** Followed by the InstallState.CodeFault's name in lower case, for example "permissions". */
    static final String ERROR_CODE_UNLOADABLE_PREFIX = "the game window would refuse the game code file: ";
    static final String ERROR_CODE_TOO_LARGE = "the game code file is larger than 512 MiB";
    /** Followed by the reason from checkArguments. */
    static final String ERROR_REQUEST_REFUSED_PREFIX = "the game would refuse the start request: ";
    static final String ERROR_SETTINGS_DIRECTORY = "could not create the settings directory";
    /** Followed by the exception's class name and message and a closing bracket. */
    static final String ERROR_IO_PREFIX = "a game file could not be read or written (";
    /** The reasons rewriteArguments gives when an earlier start request cannot be copied under a new id. */
    static final String REWRITE_EMPTY = "the earlier start request is empty",
            REWRITE_BAD_ID = "a run id must be 32 lower-case hex digits",
            REWRITE_SAME_ID = "the new run id equals the earlier one",
            REWRITE_ID_ABSENT = "the earlier start request does not name its own run id",
            REWRITE_ID_LEFT = "the earlier run id is still present after the replacement";
    static final String ARGUMENTS = "arguments.bin";
    static final String IMAGE = "decoded.image.bin";
    static final int MAXIMUM_ARGUMENT_BYTES = 65536;
    static final int MAXIMUM_ARGUMENTS = 1024;
    static final long MAXIMUM_LIBRARY_BYTES = 512L * 1024 * 1024;

    private static final Set<PosixFilePermission> OWNER_DIRECTORY = Collections.unmodifiableSet(EnumSet.of(
            PosixFilePermission.OWNER_READ, PosixFilePermission.OWNER_WRITE, PosixFilePermission.OWNER_EXECUTE));
    private static final Set<PosixFilePermission> OWNER_READ_ONLY =
            Collections.unmodifiableSet(EnumSet.of(PosixFilePermission.OWNER_READ));
    private static final char[] HEX = "0123456789abcdef".toCharArray();

    private LaunchRequest() {}

    /** The result of prepare: either an error, or the three values the game activity needs. */
    static final class Prepared {
        /** null on success; otherwise one line that says what stopped the start. */
        final String error;
        final String requestId, expectedSha256, manifestSha256;
        /** Run ids whose earlier image copy was removed by this call (step 5 of the design). */
        final List<String> imagesRemoved;

        private Prepared(String error, String requestId, String expectedSha256, String manifestSha256,
                List<String> imagesRemoved) {
            this.error = error;
            this.requestId = requestId;
            this.expectedSha256 = expectedSha256;
            this.manifestSha256 = manifestSha256;
            this.imagesRemoved = Collections.unmodifiableList(new ArrayList<String>(imagesRemoved));
        }

        /**
         * The text extras for the game activity as name / value pairs: the same four a measurement script passes
         * with "am start --es". Empty when prepare failed.
         */
        String[][] extras() {
            if (error != null) {
                return new String[0][];
            }
            return new String[][] {
                    {"private_trace", MODE},
                    {"expected_sha256", expectedSha256},
                    {"request_id", requestId},
                    {"manifest_sha256", manifestSha256},
            };
        }
    }

    /** True for 32 lower-case hex digits, the only form of run id the game accepts. */
    static boolean isId(String text) {
        if (text == null || text.length() != 32) {
            return false;
        }
        for (int at = 0; at < 32; ++at) {
            char c = text.charAt(at);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    /** A new random run id: 32 lower-case hex digits. */
    static String newId() {
        byte[] random = new byte[16];
        new SecureRandom().nextBytes(random);
        return hex(random);
    }

    /** SHA-256 of everything the stream still holds, as 64 lower-case hex digits. Does not close the stream. */
    static String sha256Hex(InputStream in) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new IOException("SHA-256 is not available", impossible);
        }
        byte[] buffer = new byte[1 << 16];
        for (int count; (count = in.read(buffer)) != -1; ) {
            digest.update(buffer, 0, count);
        }
        return hex(digest.digest());
    }

    /**
     * Returns the template with every occurrence of oldId replaced by newId. Nothing else changes and the length
     * stays the same. Throws IllegalArgumentException with the reason when that cannot be done.
     */
    static byte[] rewriteArguments(byte[] template, String oldId, String newId) {
        if (template == null || template.length == 0) {
            throw new IllegalArgumentException(REWRITE_EMPTY);
        }
        if (!isId(oldId) || !isId(newId)) {
            throw new IllegalArgumentException(REWRITE_BAD_ID);
        }
        if (oldId.equals(newId)) {
            throw new IllegalArgumentException(REWRITE_SAME_ID);
        }
        byte[] before = oldId.getBytes(StandardCharsets.US_ASCII);
        byte[] after = newId.getBytes(StandardCharsets.US_ASCII);
        byte[] result = template.clone();
        int replaced = 0;
        for (int at = 0; at + before.length <= template.length; ) {
            if (matches(template, at, before)) {
                System.arraycopy(after, 0, result, at, after.length);
                at += before.length;
                ++replaced;
            } else {
                ++at;
            }
        }
        if (replaced == 0) {
            throw new IllegalArgumentException(REWRITE_ID_ABSENT);
        }
        if (indexOf(result, before) >= 0) {
            throw new IllegalArgumentException(REWRITE_ID_LEFT);
        }
        return result;
    }

    /**
     * The arguments (zero-separated) that name a file or directory under filesDirPath/private-trace/inputs/ or
     * filesDirPath/private-trace/modules/, in order of first appearance, each once.
     */
    static List<String> referencedInputs(byte[] arguments, String filesDirPath) {
        String inputs = filesDirPath + "/private-trace/inputs/";
        String modules = filesDirPath + "/private-trace/modules/";
        Set<String> found = new LinkedHashSet<String>();
        int start = 0;
        for (int at = 0; at <= arguments.length; ++at) {
            if (at < arguments.length && arguments[at] != 0) {
                continue;
            }
            String value = new String(arguments, start, at - start, StandardCharsets.ISO_8859_1);
            if (value.startsWith(inputs) || value.startsWith(modules)) {
                found.add(value);
            }
            start = at + 1;
        }
        return Collections.unmodifiableList(new ArrayList<String>(found));
    }

    /**
     * Says whether the game process would accept these start arguments for run id under privateTraceRoot
     * (the absolute path of files/private-trace). Returns null when it would, else the first reason it would not.
     *
     * This repeats the rule the game process applies itself (parseArguments in
     * tools/host_guest_trace/private_trace_arguments.h), so that a start the game would refuse is refused here,
     * with a reason, before anything is created. The game's own check stays the authority.
     */
    static String checkArguments(byte[] arguments, String privateTraceRoot, String id) {
        if (arguments == null || arguments.length == 0) {
            return "the start request is empty";
        }
        if (arguments.length > MAXIMUM_ARGUMENT_BYTES) {
            return "the start request is longer than 65536 bytes";
        }
        if (arguments[arguments.length - 1] != 0) {
            return "the start request does not end with a zero byte";
        }
        if (privateTraceRoot == null || !plainAbsolutePath(privateTraceRoot) || privateTraceRoot.length() > 3000) {
            return "the private-trace directory is not a plain absolute path";
        }
        if (!isId(id)) {
            return "the run id is not 32 lower-case hex digits";
        }
        String run = privateTraceRoot + "/run-" + id + "/";
        String inputs = privateTraceRoot + "/inputs/";
        String modules = privateTraceRoot + "/modules/";
        String first = null, second = null;
        int count = 0, start = 0;
        for (int at = 0; at < arguments.length; ++at) {
            int value = arguments[at] & 255;
            if (value != 0) {
                if (value < 32 || value > 126) {
                    return "an argument holds a byte that is not printable ASCII";
                }
                if (at - start >= 4096) {
                    return "an argument is longer than 4096 bytes";
                }
                continue;
            }
            if (at == start) {
                return "an argument is empty";
            }
            if (count == MAXIMUM_ARGUMENTS) {
                return "there are more than 1024 arguments";
            }
            String argument = new String(arguments, start, at - start, StandardCharsets.US_ASCII);
            if (argument.charAt(0) == '/') {
                if (!plainAbsolutePath(argument)
                        || !(argument.startsWith(run) || argument.startsWith(inputs) || argument.startsWith(modules))) {
                    return "an argument is a path outside this run, the inputs and the modules directories";
                }
            } else if (argument.indexOf('/') >= 0 || argument.indexOf('\\') >= 0) {
                return "an argument is a relative path";
            }
            if (count == 0) {
                first = argument;
            } else if (count == 1) {
                second = argument;
            }
            ++count;
            start = at + 1;
        }
        if (count < 11) {
            return "there are fewer than 11 arguments";
        }
        if (!(run + "host_guest_trace").equals(first)) {
            return "the first argument is not this run's program path";
        }
        if (!(run + IMAGE).equals(second)) {
            return "the second argument is not this run's game image";
        }
        return null;
    }

    /**
     * Makes a new start request from the newest earlier one. Call it off the UI thread: it hashes the game code file.
     * Never throws; a failure is reported in Prepared.error and leaves no new run directory behind.
     */
    static Prepared prepare(File filesDir) {
        return prepare(filesDir, false);
    }

    /**
     * The same; acceptUnqualified true skips the qualified-build check (the user was asked and chose to start the
     * code that is there).
     */
    static Prepared prepare(File filesDir, boolean acceptUnqualified) {
        try {
            return prepareOrThrow(filesDir, acceptUnqualified);
        } catch (IOException | RuntimeException error) {
            String message = error.getMessage();
            return failed(ERROR_IO_PREFIX + error.getClass().getSimpleName()
                    + (message == null ? "" : ": " + message) + ")");
        }
    }

    /**
     * What settings/qualified-code.txt says: null when there is no such file (nothing is compared), the 64 hex
     * digits when its first word is a SHA-256, and "" when it exists but holds anything else (no game code equals
     * that, so the launcher asks). Never throws, never writes.
     */
    static String qualifiedHash(File filesDir) {
        File file = new File(filesDir, QUALIFIED_CODE);
        if (!file.exists() && !Files.isSymbolicLink(file.toPath())) {
            return null;
        }
        try {
            String[] words = new String(readBounded(file, 4096), StandardCharsets.US_ASCII).trim().split("\\s+");
            String first = words[0];
            if (first.length() != 64) {
                return "";
            }
            return isId(first.substring(0, 32)) && isId(first.substring(32)) ? first : "";
        } catch (IOException | RuntimeException unreadable) {
            return "";
        }
    }

    /** True when no qualified hash is on record (null) or the game code's hash equals it. */
    static boolean codeQualified(String qualified, String actual) {
        return qualified == null || qualified.equals(actual);
    }

    private static Prepared failed(String reason) {
        return new Prepared(reason, null, null, null, Collections.<String>emptyList());
    }

    private static Prepared prepareOrThrow(File filesDir, boolean acceptUnqualified) throws IOException {
        InstallState install = InstallState.detect(filesDir);
        switch (install.problem) {
            case NONE:
                break;
            case CODE_MISSING:
                return failed(ERROR_CODE_MISSING);
            case CODE_UNLOADABLE:
                return failed(ERROR_CODE_UNLOADABLE_PREFIX + install.codeFault.name().toLowerCase(Locale.ROOT));
            case SHADERS_MISSING:
                return failed(ERROR_SHADERS_MISSING);
            default:
                return failed(ERROR_NOT_INSTALLED);
        }
        File root = new File(filesDir, InstallState.PRIVATE_TRACE);
        String templateId = install.templateRunId;
        File templateDirectory = new File(root, "run-" + templateId);
        byte[] template = readBounded(new File(templateDirectory, ARGUMENTS), MAXIMUM_ARGUMENT_BYTES);
        String id = unusedId(root);
        byte[] arguments;
        try {
            arguments = rewriteArguments(template, templateId, id);
        } catch (IllegalArgumentException refused) {
            return failed(refused.getMessage());
        }
        String refusal = checkArguments(arguments, root.getAbsolutePath(), id);
        if (refusal != null) {
            return failed(ERROR_REQUEST_REFUSED_PREFIX + refusal);
        }
        File library = new File(root, InstallState.LIVE_LIBRARY);
        if (library.length() > MAXIMUM_LIBRARY_BYTES) {
            return failed(ERROR_CODE_TOO_LARGE);
        }
        String expected;
        try (InputStream in = new FileInputStream(library)) {
            expected = sha256Hex(in);
        }
        if (!acceptUnqualified && !codeQualified(qualifiedHash(filesDir), expected)) {
            return failed(UNQUALIFIED);   // before anything is created
        }
        String manifest = sha256Hex(new ByteArrayInputStream(arguments));

        // The id is written to the launcher's own list BEFORE its directory exists, so that an image copy this
        // launcher made can never be left unlisted (and therefore never cleaned up) if the app stops half way.
        File list = new File(filesDir, RUNS_LIST);
        File settings = list.getParentFile();
        if (!settings.isDirectory() && !settings.mkdir()) {
            return failed(ERROR_SETTINGS_DIRECTORY);
        }
        List<String> listed = readRunList(list);
        List<String> withNew = new ArrayList<String>(listed);
        withNew.add(id);
        writeRunList(list, withNew);

        createRun(new File(root, "run-" + id), new File(templateDirectory, IMAGE), arguments);
        List<String> removed = removeEarlierImages(root, list, withNew, templateId, id);
        return new Prepared(null, id, expected, manifest, removed);
    }

    private static String unusedId(File root) throws IOException {
        for (int attempt = 0; attempt < 4; ++attempt) {
            String id = newId();
            if (!new File(root, "run-" + id).exists() && !new File(root, "used-" + id).exists()) {
                return id;
            }
        }
        throw new IOException("could not find an unused run id");
    }

    /**
     * Creates the run directory (mode 0700) with the game image copy and the arguments (both mode 0400, which is what
     * the game activity requires of the arguments file). If any step fails, removes what this call created and
     * rethrows.
     */
    private static void createRun(File runDirectory, File templateImage, byte[] arguments) throws IOException {
        Path directory = runDirectory.toPath();
        Files.createDirectory(directory);   // throws if it exists: a run id is never used twice
        Path image = directory.resolve(IMAGE);
        Path manifest = directory.resolve(ARGUMENTS);
        try {
            Files.setPosixFilePermissions(directory, OWNER_DIRECTORY);
            copyNew(templateImage.toPath(), image);
            Files.setPosixFilePermissions(image, OWNER_READ_ONLY);
            writeNew(manifest, arguments);
            Files.setPosixFilePermissions(manifest, OWNER_READ_ONLY);
        } catch (IOException | RuntimeException error) {
            deleteQuietly(manifest);
            deleteQuietly(image);
            deleteQuietly(directory);
            throw error;
        }
    }

    private static void copyNew(Path source, Path target) throws IOException {
        try (FileChannel in = FileChannel.open(source, StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS);
                FileChannel out = FileChannel.open(target, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
            long size = in.size();
            long done = 0;
            while (done < size) {
                long moved = in.transferTo(done, size - done, out);
                if (moved <= 0) {
                    break;
                }
                done += moved;
            }
            out.force(true);
            if (done != size || out.size() != size || in.size() != size) {
                throw new IOException("the game image copy is incomplete");
            }
        }
    }

    private static void writeNew(Path target, byte[] content) throws IOException {
        try (FileChannel out = FileChannel.open(target, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE)) {
            ByteBuffer buffer = ByteBuffer.wrap(content);
            while (buffer.hasRemaining()) {
                out.write(buffer);
            }
            out.force(true);
        }
    }

    private static void deleteQuietly(Path path) {
        try {
            Files.deleteIfExists(path);
        } catch (IOException | RuntimeException ignored) {
            // Nothing more can be done here; the caller reports the original failure.
        }
    }

    /** The valid ids in the launcher's list, in order, each once. A missing or unreadable list is empty. */
    static List<String> readRunList(File list) {
        Set<String> ids = new LinkedHashSet<String>();
        try {
            if (!Files.isRegularFile(list.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                return new ArrayList<String>();
            }
            String text = new String(readBounded(list, 1 << 20), StandardCharsets.US_ASCII);
            for (String line : text.split("\n")) {
                String id = line.trim();
                if (isId(id)) {
                    ids.add(id);
                }
            }
        } catch (IOException | RuntimeException unreadable) {
            return new ArrayList<String>();
        }
        return new ArrayList<String>(ids);
    }

    /**
     * The run this launcher made last (the last id of its own list), or null when it has made none. prepare adds
     * the new run at the end of the list before it creates the run, so right after a successful prepare this is
     * the run that is about to be started. Never throws, never writes.
     */
    static String newestLauncherRun(File filesDir) {
        List<String> ids = readRunList(new File(filesDir, RUNS_LIST));
        return ids.isEmpty() ? null : ids.get(ids.size() - 1);
    }

    private static void writeRunList(File list, List<String> ids) throws IOException {
        StringBuilder text = new StringBuilder();
        for (String id : ids) {
            text.append(id).append('\n');
        }
        Path target = list.toPath();
        Path temporary = target.resolveSibling(list.getName() + ".tmp");
        try (FileChannel out = FileChannel.open(temporary, StandardOpenOption.CREATE,
                StandardOpenOption.TRUNCATE_EXISTING, StandardOpenOption.WRITE)) {
            ByteBuffer buffer = ByteBuffer.wrap(text.toString().getBytes(StandardCharsets.US_ASCII));
            while (buffer.hasRemaining()) {
                out.write(buffer);
            }
            out.force(true);
        }
        Files.move(temporary, target, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    /**
     * Step 5 of the design: for every id in the launcher's list other than the template in use and the new run,
     * delete that run's image copy (only that one file) and drop the id from the list. Best effort: a failure keeps
     * the id listed for the next time and never stops the start. Returns the ids whose image was deleted.
     */
    private static List<String> removeEarlierImages(File root, File list, List<String> listed, String templateId,
            String newId) {
        List<String> kept = new ArrayList<String>();
        List<String> removed = new ArrayList<String>();
        for (String id : listed) {
            if (id.equals(newId) || id.equals(templateId)) {
                kept.add(id);
                continue;
            }
            Path directory = new File(root, "run-" + id).toPath();
            Path image = directory.resolve(IMAGE);
            try {
                if (Files.isDirectory(directory, LinkOption.NOFOLLOW_LINKS)
                        && Files.isRegularFile(image, LinkOption.NOFOLLOW_LINKS)) {
                    Files.delete(image);
                    removed.add(id);
                }
            } catch (IOException | RuntimeException failure) {
                kept.add(id);
            }
        }
        if (!kept.equals(listed)) {
            try {
                writeRunList(list, kept);
            } catch (IOException | RuntimeException ignored) {
                // The longer list is still correct; the next start tidies it.
            }
        }
        return removed;
    }

    /** Reads a whole regular file of at most limit bytes; a longer file is an error. */
    static byte[] readBounded(File file, int limit) throws IOException {
        try (FileChannel in = FileChannel.open(file.toPath(), StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS)) {
            long size = in.size();
            if (size > limit) {
                throw new IOException(file.getName() + " is longer than " + limit + " bytes");
            }
            ByteBuffer buffer = ByteBuffer.allocate((int) size);
            while (buffer.hasRemaining() && in.read(buffer) > 0) {
                // keep reading
            }
            if (buffer.hasRemaining()) {
                throw new IOException(file.getName() + " changed while it was read");
            }
            return buffer.array();
        }
    }

    /** The game's rule for a path: absolute, no empty, "." or ".." part, only letters, digits, '_', '-' and '.'. */
    private static boolean plainAbsolutePath(String path) {
        if (path.isEmpty() || path.charAt(0) != '/' || path.charAt(path.length() - 1) == '/') {
            return false;
        }
        int at = 1;
        while (at < path.length()) {
            int end = path.indexOf('/', at);
            String part = end < 0 ? path.substring(at) : path.substring(at, end);
            if (part.isEmpty() || part.equals(".") || part.equals("..")) {
                return false;
            }
            for (int index = 0; index < part.length(); ++index) {
                char c = part.charAt(index);
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '_' || c == '-' || c == '.')) {
                    return false;
                }
            }
            if (end < 0) {
                break;
            }
            at = end + 1;
        }
        return true;
    }

    private static boolean matches(byte[] data, int at, byte[] pattern) {
        for (int index = 0; index < pattern.length; ++index) {
            if (data[at + index] != pattern[index]) {
                return false;
            }
        }
        return true;
    }

    private static int indexOf(byte[] data, byte[] pattern) {
        for (int at = 0; at + pattern.length <= data.length; ++at) {
            if (matches(data, at, pattern)) {
                return at;
            }
        }
        return -1;
    }

    private static String hex(byte[] bytes) {
        char[] text = new char[bytes.length * 2];
        for (int index = 0; index < bytes.length; ++index) {
            text[index * 2] = HEX[(bytes[index] >> 4) & 15];
            text[index * 2 + 1] = HEX[bytes[index] & 15];
        }
        return new String(text);
    }
}
