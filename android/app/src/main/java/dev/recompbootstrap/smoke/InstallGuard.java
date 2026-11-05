package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/**
 * The safety rules of the first-run setup in one place. Pure Java.
 *
 * 1. Every path the setup writes to is built by under() / resolve(). The result always lies inside
 *    files/game-install; a path that leaves it (also through a symbolic link) or that has an element named
 *    "saves" or "private-trace" is refused with an IOException.
 * 2. existing() says whether an install is already there and of which kind. When one is, extraction and import
 *    refuse to start and the screen is read-only; existingInstall() is the same answer as one sentence.
 * 3. install.json is the record of what is in place. It is derived from the files (refreshRecord), never typed in.
 */
final class InstallGuard {
    static final String ROOT_NAME = "game-install";
    static final String RECORD_NAME = "install.json";
    private static final String[] FORBIDDEN = {"saves", "private-trace"};
    private static final int MAX_RECORD_BYTES = 4096;

    private InstallGuard() {}

    /** files/game-install. */
    static File root(File filesDir) {
        return new File(filesDir, ROOT_NAME);
    }

    /** A file under files/game-install; relative is '/' separated. Throws when the path is not allowed. */
    static File resolve(File filesDir, String relative) throws IOException {
        return under(root(filesDir), relative);
    }

    /** null when root may be used as the setup's directory; else the reason. Creates nothing. */
    static String rootProblem(File root) {
        if (root == null) return "No setup directory was given.";
        if (!ROOT_NAME.equals(root.getName())) return "The setup directory must be named " + ROOT_NAME + ".";
        File canonical;
        try {
            canonical = root.getCanonicalFile();
        } catch (IOException e) {
            return "The setup directory cannot be resolved.";
        }
        if (!ROOT_NAME.equals(canonical.getName())) return "The setup directory is a link to another place.";
        for (File at = canonical.getParentFile(); at != null; at = at.getParentFile()) {
            if (forbidden(at.getName())) return "The setup directory lies inside " + at.getName() + ".";
        }
        if (root.exists() && !root.isDirectory()) return "The setup directory is not a directory.";
        return null;
    }

    /** A file under root (which must pass rootProblem); relative is '/' separated. Creates nothing. */
    static File under(File root, String relative) throws IOException {
        String problem = rootProblem(root);
        if (problem != null) throw new IOException(problem);
        if (relative == null || relative.isEmpty() || relative.indexOf('\\') >= 0 || relative.indexOf('\0') >= 0) {
            throw new IOException("path not allowed for setup files");
        }
        for (String part : relative.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals("..") || part.length() > 255 || forbidden(part)) {
                throw new IOException("path not allowed for setup files: " + relative);
            }
        }
        File file = new File(root, relative);
        String base = root.getCanonicalPath() + File.separator;
        String canonical = file.getCanonicalPath();
        if (!canonical.startsWith(base)) throw new IOException("path leaves the setup directory: " + relative);
        for (String part : canonical.substring(base.length()).replace(File.separatorChar, '/').split("/")) {
            if (forbidden(part)) throw new IOException("path not allowed for setup files: " + relative);
        }
        return file;
    }

    private static boolean forbidden(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        for (String f : FORBIDDEN) {
            if (f.equals(lower)) return true;
        }
        return false;
    }

    /**
     * What is already installed, as far as it closes the setup. Gathered by existing(), which only reads.
     * An install exists when the layout that only a computer can fill (over USB) holds game data
     * (files/private-trace/inputs exists and is not empty) or game code (files/private-trace/private-trace-live.so
     * exists), or when files/game-install/install.json says "complete": true. An install.json that cannot be
     * understood also counts: nothing is overwritten on a guess.
     */
    static final class Existing {
        /** Game data, game code: put on the device from a computer. */
        final boolean computerData, computerCode;
        /** The setup's own record: cannot be understood, or says that the install is complete. */
        final boolean recordUnreadable, recordComplete;

        Existing(boolean computerData, boolean computerCode, boolean recordUnreadable, boolean recordComplete) {
            this.computerData = computerData;
            this.computerCode = computerCode;
            this.recordUnreadable = recordUnreadable;
            this.recordComplete = recordComplete;
        }

        /** True when the setup must not change anything. */
        boolean any() {
            return computerData || computerCode || recordUnreadable || recordComplete;
        }

        /** One sentence that names the first thing found (with its place, for a bug report), or null when not any(). */
        String reason() {
            if (computerData) return "Game data is already installed (files/private-trace/inputs).";
            if (computerCode) return "Game code is already installed (files/private-trace/private-trace-live.so).";
            if (recordUnreadable) return "The install record files/game-install/install.json cannot be read.";
            if (recordComplete) return "A complete install is recorded in files/game-install/install.json.";
            return null;
        }
    }

    /** Read-only. Never throws. */
    static Existing existing(File filesDir) {
        File trace = new File(filesDir, "private-trace");
        File inputs = new File(trace, "inputs");
        boolean data = false;
        if (inputs.exists()) {
            String[] names = inputs.list();
            data = names == null || names.length > 0;
        }
        boolean code = new File(trace, "private-trace-live.so").exists();
        boolean unreadable = false, complete = false;
        if (new File(root(filesDir), RECORD_NAME).exists()) {
            Record r = readRecord(root(filesDir));
            unreadable = r == null;
            complete = r != null && r.complete;
        }
        return new Existing(data, code, unreadable, complete);
    }

    /** A reason when an install already exists (see Existing), else null. Read-only. */
    static String existingInstall(File filesDir) {
        return existing(filesDir).reason();
    }

    /** The content of install.json. */
    static final class Record {
        final String executableSha256;
        final boolean discFilesComplete, bundleComplete, complete;

        Record(String executableSha256, boolean discFilesComplete, boolean bundleComplete) {
            this.executableSha256 = executableSha256;
            this.discFilesComplete = discFilesComplete;
            this.bundleComplete = bundleComplete;
            this.complete = discFilesComplete && bundleComplete;
        }

        /** The exact file content: one JSON object, keys in this order, a line feed at the end. */
        byte[] encode() {
            Map<String, Object> map = new LinkedHashMap<String, Object>();
            map.put("version", Long.valueOf(1));
            map.put("executableSha256", executableSha256);
            map.put("discFilesComplete", Boolean.valueOf(discFilesComplete));
            map.put("bundleComplete", Boolean.valueOf(bundleComplete));
            map.put("complete", Boolean.valueOf(complete));
            return (JsonLite.write(map) + "\n").getBytes(StandardCharsets.UTF_8);
        }
    }

    /** null when install.json is absent, unreadable or not a valid record. Never throws. */
    static Record readRecord(File root) {
        try {
            byte[] bytes = readSmall(new File(root, RECORD_NAME), MAX_RECORD_BYTES);
            if (bytes == null) return null;
            Object parsed = JsonLite.parse(new String(bytes, StandardCharsets.UTF_8));
            if (!(parsed instanceof Map)) return null;
            Map<?, ?> map = (Map<?, ?>) parsed;
            Object version = map.get("version"), sha = map.get("executableSha256");
            Object disc = map.get("discFilesComplete"), bundle = map.get("bundleComplete"), complete = map.get("complete");
            if (!(version instanceof Number) || ((Number) version).longValue() != 1) return null;
            if (!(sha instanceof String) || !SetupCatalog.isSha256((String) sha)) return null;
            if (!(disc instanceof Boolean) || !(bundle instanceof Boolean) || !(complete instanceof Boolean)) return null;
            Record record = new Record((String) sha, ((Boolean) disc).booleanValue(), ((Boolean) bundle).booleanValue());
            if (record.complete != ((Boolean) complete).booleanValue()) return null;   // contradicts itself
            return record;
        } catch (IOException e) {
            return null;
        } catch (JsonLite.ParseException e) {
            return null;
        } catch (RuntimeException e) {
            return null;
        }
    }

    /**
     * Writes install.json from what is in place and returns it:
     *  discFilesComplete = the catalog covers every disc the game needs and every catalog file is in inputs/ with
     *                      its catalog size (files get there only after their SHA-256 was checked);
     *  bundleComplete    = bundle/manifest.json exists (the bundle directory appears only after every component
     *                      was checked);
     *  complete          = both.
     * The file is written atomically, and only when its content would change.
     */
    static Record refreshRecord(File root, SetupCatalog.Spec spec) throws IOException {
        boolean disc = spec.coversAllDiscs;
        for (SetupCatalog.Item item : spec.items) {
            if (!VerifiedExtractor.present(root, item)) disc = false;
        }
        boolean bundle = new File(under(root, CodeBundleImporter.BUNDLE), CodeBundle.MANIFEST_NAME).isFile();
        Record record = new Record(spec.executableSha256, disc, bundle);
        File target = under(root, RECORD_NAME);
        byte[] wanted = record.encode();
        if (!Arrays.equals(wanted, readSmall(target, MAX_RECORD_BYTES))) AtomicFiles.write(target, wanted);
        return record;
    }

    /** The whole file, or null when it is absent, not a regular file or longer than max. */
    static byte[] readSmall(File file, int max) throws IOException {
        if (!file.isFile() || file.length() > max) return null;
        byte[] data = new byte[(int) file.length()];
        FileInputStream in = new FileInputStream(file);
        try {
            int done = 0;
            while (done < data.length) {
                int n = in.read(data, done, data.length - done);
                if (n < 0) return null;
                done += n;
            }
        } finally {
            in.close();
        }
        return data;
    }
}
