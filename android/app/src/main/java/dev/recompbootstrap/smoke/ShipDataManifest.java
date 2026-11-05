package dev.recompbootstrap.smoke;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The list of files in the folder the player prepares on a computer from their own discs: the file
 * game-data-manifest.json in that folder. Pure Java.
 *
 *   {"schema": "gta5-game-data/1",
 *    "files": [{"path": "inputs/(sha256).bin", "size": 26628096, "sha256": "(64 hex digits)"}, ...]}
 *
 * A path is where the file lies in the folder AND where it belongs in the app, relative to files/private-trace.
 * Exactly three shapes exist (the layout the game library reads, local-analysis/agents/ship-kit/
 * STANDING-CONFIGURATION.md):
 *   inputs/(sha256).bin                 a game file under its own SHA-256 (the name must equal the content's hash);
 *   modules/boot/(one to six names)     a boot shader module, for example modules/boot/r8/vertex/dataflow.spv;
 *   decoded.image.bin                   the decoded game image; it is stored beside the start request.
 * Anything else is refused before a single byte is copied: a list cannot make the app write outside that layout,
 * and never under files/saves.
 */
final class ShipDataManifest {
    static final String FILE_NAME = "game-data-manifest.json";
    static final String SCHEMA = "gta5-game-data/1";
    static final String IMAGE = "decoded.image.bin";
    static final String INPUTS = "inputs/", BOOT_MODULES = "modules/boot/";
    static final int MAX_BYTES = 1 << 20, MAX_FILES = 4096, MAX_MODULE_DEPTH = 6;
    /** A bound on one file, far above any game file (the largest is 2.8 GB): it only keeps sums from overflowing. */
    static final long MAX_FILE_BYTES = 1L << 36;

    static final class Entry {
        final String path;
        final long bytes;
        final String sha256;

        Entry(String path, long bytes, String sha256) {
            this.path = path;
            this.bytes = bytes;
            this.sha256 = sha256;
        }
    }

    /** In the list's order. Unmodifiable. */
    final List<Entry> entries;
    /** SHA-256 of the list file's bytes. */
    final String sha256;
    final long totalBytes;
    private final Map<String, Entry> byPath = new HashMap<String, Entry>();

    private ShipDataManifest(List<Entry> entries, String sha256) {
        this.entries = Collections.unmodifiableList(entries);
        this.sha256 = sha256;
        long total = 0;
        for (Entry entry : entries) {
            total += entry.bytes;
            byPath.put(entry.path, entry);
        }
        this.totalBytes = total;
    }

    /** null when the list has no such path. */
    Entry entry(String path) {
        return byPath.get(path);
    }

    /** The 64 hex digits of "inputs/(64 lower-case hex digits).bin", or null for any other path. */
    static String inputHash(String path) {
        if (path == null || path.length() != INPUTS.length() + 64 + 4 || !path.startsWith(INPUTS) || !path.endsWith(".bin")) {
            return null;
        }
        String hash = path.substring(INPUTS.length(), INPUTS.length() + 64);
        return ShipFiles.isSha256(hash) ? hash : null;
    }

    /** null when path has one of the three shapes of the game data layout; else why not. */
    static String pathProblem(String path) {
        if (path == null || path.isEmpty()) return "an entry has no path";
        if (IMAGE.equals(path) || inputHash(path) != null) return null;
        if (path.startsWith(BOOT_MODULES)) {
            String[] parts = path.substring(BOOT_MODULES.length()).split("/", -1);
            if (parts.length > MAX_MODULE_DEPTH) return "it lies too deep under " + BOOT_MODULES;
            for (String part : parts) {
                if (!moduleName(part)) return "it has a name that is not allowed under " + BOOT_MODULES;
            }
            return null;
        }
        return "it is not inputs/(sha256).bin, " + BOOT_MODULES + "(name) or " + IMAGE;
    }

    /** One name under modules/boot: lower-case letter or digit first, then lower-case letters, digits, '.', '_', '-'. */
    private static boolean moduleName(String name) {
        if (name.isEmpty() || name.length() > 64) return false;
        for (int at = 0; at < name.length(); ++at) {
            char c = name.charAt(at);
            boolean alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (!alnum && (at == 0 || (c != '.' && c != '_' && c != '-'))) return false;
        }
        return true;
    }

    /**
     * Where an entry belongs, relative to files/private-trace. The image goes beside the start request of the run
     * the app copies start requests from; everything else keeps its path.
     */
    static String target(String path, String templateRunId) {
        return IMAGE.equals(path) ? "run-" + templateRunId + "/" + IMAGE : path;
    }

    /**
     * The first path that another path of the list lies under ("modules/boot/a" beside "modules/boot/a/b"), or null.
     * One name cannot be a file and a folder: such a list is refused whole, before anything is created.
     */
    static String fileAndFolder(Collection<String> paths) {
        Set<String> all = new HashSet<String>(paths);
        for (String path : paths) {
            for (int slash = path.indexOf('/'); slash >= 0; slash = path.indexOf('/', slash + 1)) {
                String above = path.substring(0, slash);
                if (all.contains(above)) return above;
            }
        }
        return null;
    }

    /** Reads the list. IOException with one plain sentence when it cannot be used; nothing was copied then. */
    static ShipDataManifest parse(byte[] json) throws IOException {
        if (json == null || json.length == 0) throw unusable("it is empty");
        if (json.length > MAX_BYTES) throw unusable("it is longer than 1 MiB");
        Object parsed;
        try {
            parsed = JsonLite.parse(new String(json, StandardCharsets.UTF_8));
        } catch (JsonLite.ParseException e) {
            throw unusable("it is not JSON");
        }
        if (!(parsed instanceof Map)) throw unusable("it is not a JSON object");
        Map<?, ?> map = (Map<?, ?>) parsed;
        Object schema = map.get("schema");
        if (!SCHEMA.equals(schema)) {
            throw unusable("its format is " + (schema instanceof String ? schema : "not named") + ", this app reads " + SCHEMA);
        }
        Object files = map.get("files");
        if (!(files instanceof List) || ((List<?>) files).isEmpty()) throw unusable("it lists no files");
        if (((List<?>) files).size() > MAX_FILES) throw unusable("it lists more than " + MAX_FILES + " files");
        List<Entry> entries = new ArrayList<Entry>();
        Map<String, Boolean> seen = new HashMap<String, Boolean>();
        for (Object o : (List<?>) files) {
            if (!(o instanceof Map)) throw unusable("an entry is not a JSON object");
            Map<?, ?> entry = (Map<?, ?>) o;
            Object path = entry.get("path"), size = entry.get("size"), sha = entry.get("sha256");
            if (!(path instanceof String)) throw unusable("an entry has no path");
            String name = (String) path;
            String problem = pathProblem(name);
            if (problem != null) {
                throw new IOException("The folder's file list names a file outside the game data layout (" + shown(name)
                        + ": " + problem + "). Nothing was copied.");
            }
            if (seen.put(name, Boolean.TRUE) != null) throw unusable(name + " is listed twice");
            if (!(size instanceof Long) || ((Long) size).longValue() < 0 || ((Long) size).longValue() > MAX_FILE_BYTES) {
                throw unusable(name + " has no usable size");
            }
            if (!(sha instanceof String) || !ShipFiles.isSha256((String) sha)) throw unusable(name + " has no SHA-256 of 64 hex digits");
            String named = inputHash(name);
            if (named != null && !named.equals(sha)) {
                throw unusable(name + " is listed with another SHA-256 than its name");
            }
            entries.add(new Entry(name, ((Long) size).longValue(), (String) sha));
        }
        String clash = fileAndFolder(seen.keySet());
        if (clash != null) throw unusable(clash + " is listed as a file and as a folder");
        return new ShipDataManifest(entries, ShipFiles.sha256Hex(json));
    }

    private static IOException unusable(String why) {
        return new IOException("The folder's file list (" + FILE_NAME + ") cannot be used: " + why + ". Nothing was copied.");
    }

    /** A path for a message: cut when long, and only printable ASCII. */
    private static String shown(String path) {
        StringBuilder text = new StringBuilder();
        for (int at = 0; at < path.length() && at < 80; ++at) {
            char c = path.charAt(at);
            text.append(c >= 32 && c <= 126 ? c : '?');
        }
        return path.length() > 80 ? text + "..." : text.toString();
    }
}
