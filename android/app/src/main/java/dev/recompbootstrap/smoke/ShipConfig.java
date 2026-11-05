package dev.recompbootstrap.smoke;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The record of the game code that travels inside the APK: the asset ship/config.json. Pure Java.
 *
 * tools/ship_stage.py writes it when the APK is built. It names every bundled piece with its size and SHA-256
 * (the game library, the shader pack, the start request), the switch files of the standing configuration, and the
 * game data this app version was made for:
 *
 *   data     every game data file, pinned by path, size and SHA-256 (the "pin"). Always present: a record without
 *            it is refused, because the start request alone does not name everything a start needs (the boot
 *            modules lie in a directory it names, not in its list).
 *   pieces   the pinned files that travel inside the APK (assets "ship/pieces/(path)"): small generated files, for
 *            example the catalogs, the boot modules and the decoded image. ShipFirstRun installs them.
 *   The pinned files that are NOT pieces are the ones that come from the player's own discs: notBundled.
 *
 * ShipFirstRun installs from the record; ShipDataImport checks a data folder against it; ShipData says what is
 * missing. The SHA-256 of the config file itself is the mark of "this app version's game code is installed"
 * (ShipFirstRun.STAMP).
 *
 * parse() checks the shape of every value, so that no later step has to distrust a name or a number: every name is
 * a plain name inside files/private-trace, nothing can point to files/saves or out of the app's storage.
 */
final class ShipConfig {
    static final String SCHEMA = "gta5-ship-config/1";
    /** The asset's name inside the APK's assets folder. */
    static final String ASSET = "ship/config.json";
    static final int MAX_BYTES = 1 << 20;
    /** The loader's own limits (private_trace_jni.cpp, libraryFile and openArguments). */
    static final long MAX_LIBRARY_BYTES = 512L * 1024 * 1024;
    static final long MIN_LIBRARY_BYTES = 64;
    static final long MAX_START_REQUEST_BYTES = 65536;

    /** One bundled file. */
    static final class Piece {
        /** The asset's name inside the APK's assets folder, for example "ship/game-library.bin". */
        final String asset;
        final long bytes;
        final String sha256;

        Piece(String asset, long bytes, String sha256) {
            this.asset = asset;
            this.bytes = bytes;
            this.sha256 = sha256;
        }
    }

    /** One game data file whose identity this app version pins. */
    static final class DataFile {
        /** Relative to the data folder and to the target layout, for example "modules/boot/store/dataflow.spv". */
        final String path;
        final long bytes;
        final String sha256;

        DataFile(String path, long bytes, String sha256) {
            this.path = path;
            this.bytes = bytes;
            this.sha256 = sha256;
        }
    }

    /** One pinned game data file that travels inside the APK. */
    static final class BundledData {
        /** The asset's name inside the APK's assets folder: "ship/pieces/" followed by path. */
        final String asset;
        /** The path of the game data layout, as in DataFile. */
        final String path;
        final long bytes;
        final String sha256;

        BundledData(String asset, String path, long bytes, String sha256) {
            this.asset = asset;
            this.path = path;
            this.bytes = bytes;
            this.sha256 = sha256;
        }
    }

    /** Where bundled game data lies inside the APK's assets folder. */
    static final String PIECES = "ship/pieces/";

    /** A label for texts, for example "v464". */
    final String build;
    /** The absolute path the game library was built for: the app's files directory of the device's main user. */
    final String filesRoot;
    final Piece library, shaderPack, startRequest;
    /** The file name the game library opens its shaders under, inside files/private-trace. */
    final String shaderPackName;
    /** The run id the bundled start request was written for: 32 hex digits. */
    final String runId;
    /** Switch files of the standing configuration: plain names inside files/private-trace. */
    final List<String> switches;
    /** Directories the start request names, relative to files/private-trace; the game fills them itself. */
    final List<String> directories;
    /** Game data files the start request names: "inputs/(sha256).bin". */
    final List<String> requiredInputs;
    /** Every game data file this app version was made for, with size and SHA-256. Never empty. */
    final List<DataFile> pinnedData;
    /** The pinned files that travel inside the APK and are installed by the first start. May be empty. */
    final List<BundledData> bundledData;
    /**
     * The pinned files that are NOT inside the APK, in the record's order: what must come from the player's own
     * game discs. A disc import copies a disc file to inputs/(sha256).bin when its size and SHA-256 equal an entry.
     */
    final List<DataFile> notBundled;
    /** SHA-256 of the config file's bytes. */
    final String sha256;

    private ShipConfig(String build, String filesRoot, Piece library, Piece shaderPack, String shaderPackName,
            Piece startRequest, String runId, List<String> switches, List<String> directories,
            List<String> requiredInputs, List<DataFile> pinnedData, List<BundledData> bundledData, String sha256) {
        this.build = build;
        this.filesRoot = filesRoot;
        this.library = library;
        this.shaderPack = shaderPack;
        this.shaderPackName = shaderPackName;
        this.startRequest = startRequest;
        this.runId = runId;
        this.switches = Collections.unmodifiableList(switches);
        this.directories = Collections.unmodifiableList(directories);
        this.requiredInputs = Collections.unmodifiableList(requiredInputs);
        this.pinnedData = Collections.unmodifiableList(pinnedData);
        this.bundledData = Collections.unmodifiableList(bundledData);
        List<DataFile> fromDiscs = new ArrayList<DataFile>();
        for (DataFile file : pinnedData) {
            if (bundled(file.path) == null) fromDiscs.add(file);
        }
        this.notBundled = Collections.unmodifiableList(fromDiscs);
        this.sha256 = sha256;
    }

    /** The bundled piece for a pinned path, or null when that file is not inside the APK. */
    BundledData bundled(String path) {
        for (BundledData piece : bundledData) {
            if (piece.path.equals(path)) return piece;
        }
        return null;
    }

    /** The bytes of the pinned files that are not inside the APK. */
    long notBundledBytes() {
        long total = 0;
        for (DataFile file : notBundled) total += file.bytes;
        return total;
    }

    /** Size and SHA-256 of a pinned game data file, or null when this app version does not pin that path. */
    DataFile pinned(String path) {
        for (DataFile file : pinnedData) {
            if (file.path.equals(path)) return file;
        }
        return null;
    }

    /** Reads the config. IOException with a plain reason when it is not a config this app version understands. */
    static ShipConfig parse(byte[] json) throws IOException {
        if (json == null || json.length == 0 || json.length > MAX_BYTES) throw bad("it is empty or longer than 1 MiB");
        Object parsed;
        try {
            parsed = JsonLite.parse(new String(json, StandardCharsets.UTF_8));
        } catch (JsonLite.ParseException e) {
            throw bad("it is not JSON");
        }
        if (!(parsed instanceof Map)) throw bad("it is not a JSON object");
        Map<?, ?> map = (Map<?, ?>) parsed;
        if (!SCHEMA.equals(map.get("schema"))) throw bad("its format is not " + SCHEMA);
        String build = text(map.get("build"), "build");
        if (!plainName(build)) throw bad("its build label is not a plain name");
        String filesRoot = text(map.get("filesRoot"), "filesRoot");
        if (!plainAbsolutePath(filesRoot)) throw bad("filesRoot is not a plain absolute path");

        Map<?, ?> libraryMap = object(map.get("library"), "library");
        Piece library = piece(libraryMap, "library");
        if (library.bytes < MIN_LIBRARY_BYTES || library.bytes > MAX_LIBRARY_BYTES) {
            throw bad("the game library's size is outside 64 bytes to 512 MiB");
        }
        Map<?, ?> shaderMap = object(map.get("shaderPack"), "shaderPack");
        Piece shaderPack = piece(shaderMap, "shaderPack");
        String shaderPackName = text(shaderMap.get("name"), "shaderPack.name");
        if (!plainName(shaderPackName)) throw bad("the shader pack's file name is not a plain name");
        // The launcher looks for the shader file under one fixed name (InstallState.SHADER_FILE), and the game
        // library opens that literal name: a pack under any other name would be installed and never found.
        if (!InstallState.SHADER_FILE.equals(shaderPackName)) {
            throw bad("the shader pack's file name is " + shaderPackName + ", but the launcher and the game code look for "
                    + InstallState.SHADER_FILE);
        }
        Map<?, ?> requestMap = object(map.get("startRequest"), "startRequest");
        Piece startRequest = piece(requestMap, "startRequest");
        if (startRequest.bytes < 1 || startRequest.bytes > MAX_START_REQUEST_BYTES) {
            throw bad("the start request's size is outside 1 to 65536 bytes");
        }
        String runId = text(requestMap.get("runId"), "startRequest.runId");
        if (!LaunchRequest.isId(runId)) throw bad("the start request's run id is not 32 hex digits");

        List<String> switches = new ArrayList<String>();
        for (Object o : list(map.get("switches"), "switches")) {
            String name = text(o, "a switch name");
            if (!switchName(name)) throw bad("a switch file name is not allowed: " + name);
            switches.add(name);
        }
        requireDistinct(switches, "switch file");
        List<String> directories = new ArrayList<String>();
        for (Object o : list(map.get("directories"), "directories")) {
            String path = text(o, "a directory");
            if (!dataDirectory(path)) throw bad("a directory is outside inputs/ and modules/: " + path);
            directories.add(path);
        }
        requireDistinct(directories, "directory");
        List<String> requiredInputs = new ArrayList<String>();
        for (Object o : list(map.get("requiredInputs"), "requiredInputs")) {
            String path = text(o, "a required input");
            if (ShipDataManifest.inputHash(path) == null) throw bad("a required input is not inputs/(sha256).bin: " + path);
            requiredInputs.add(path);
        }
        requireDistinct(requiredInputs, "required input");
        List<DataFile> pinned = new ArrayList<DataFile>();
        List<String> pinnedPaths = new ArrayList<String>();
        Object data = map.get("data");
        if (data == null) throw bad("it has no list of pinned game data (\"data\")");
        {
            for (Object o : list(data, "data")) {
                Map<?, ?> entry = object(o, "a data entry");
                String path = text(entry.get("path"), "a data path");
                String problem = ShipDataManifest.pathProblem(path);
                if (problem != null) throw bad("a pinned data file is outside the game data layout: " + path);
                long bytes = number(entry.get("size"), "size of " + path);
                String sha = text(entry.get("sha256"), "sha256 of " + path);
                if (!ShipFiles.isSha256(sha)) throw bad("the SHA-256 of " + path + " is not 64 hex digits");
                String named = ShipDataManifest.inputHash(path);
                if (named != null && !named.equals(sha)) throw bad(path + " is pinned with another SHA-256 than its name");
                pinned.add(new DataFile(path, bytes, sha));
                pinnedPaths.add(path);
            }
            requireDistinct(pinnedPaths, "pinned data file");
        }
        String clash = ShipDataManifest.fileAndFolder(pinnedPaths);
        if (clash != null) throw bad(clash + " is pinned as a file and as a folder");
        // The pin is the whole game data: everything the start request names, and the image beside it.
        for (String input : requiredInputs) {
            if (!pinnedPaths.contains(input)) throw bad("the start request's input " + input + " is not pinned");
        }
        if (!pinnedPaths.contains(ShipDataManifest.IMAGE)) throw bad("the game image is not pinned");
        // The boot modules are named by the start request only as a folder, so the lists above do not ask for them.
        // A record that names that folder and pins no file in it would call an install without boot modules whole.
        // (The cache folders under inputs/ are filled by the game itself and hold no pinned file.)
        String bootFolder = ShipDataManifest.BOOT_MODULES.substring(0, ShipDataManifest.BOOT_MODULES.length() - 1);
        if (directories.contains(bootFolder)) {
            boolean pinnedModule = false;
            for (String path : pinnedPaths) pinnedModule |= path.startsWith(ShipDataManifest.BOOT_MODULES);
            if (!pinnedModule) throw bad("the start request names the folder " + bootFolder + " but no file in it is pinned");
        }

        List<BundledData> bundled = new ArrayList<BundledData>();
        List<String> bundledPaths = new ArrayList<String>();
        Object pieces = map.get("pieces");
        if (pieces != null) {
            for (Object o : list(pieces, "pieces")) {
                Map<?, ?> entry = object(o, "a bundled piece");
                String path = text(entry.get("path"), "a bundled piece's path");
                String asset = text(entry.get("asset"), "the asset name of " + path);
                long bytes = number(entry.get("size"), "size of " + path);
                String sha = text(entry.get("sha256"), "sha256 of " + path);
                DataFile pin = null;
                for (DataFile file : pinned) {
                    if (file.path.equals(path)) pin = file;
                }
                // A piece is never anything but a pinned file: the pin stays the one list of what a start needs.
                if (pin == null) throw bad("the bundled piece " + path + " is not a pinned game data file");
                if (pin.bytes != bytes || !pin.sha256.equals(sha)) {
                    throw bad("the bundled piece " + path + " has another size or SHA-256 than the pin");
                }
                if (!(PIECES + path).equals(asset)) throw bad("the asset name of " + path + " is not " + PIECES + path);
                bundled.add(new BundledData(asset, path, bytes, sha));
                bundledPaths.add(path);
            }
            requireDistinct(bundledPaths, "bundled piece");
        }
        return new ShipConfig(build, filesRoot, library, shaderPack, shaderPackName, startRequest, runId, switches,
                directories, requiredInputs, pinned, bundled, ShipFiles.sha256Hex(json));
    }

    private static Piece piece(Map<?, ?> map, String what) throws IOException {
        String asset = text(map.get("asset"), what + ".asset");
        if (!assetName(asset)) throw bad("the asset name of " + what + " is not ship/(plain name)");
        long bytes = number(map.get("size"), what + ".size");
        String sha = text(map.get("sha256"), what + ".sha256");
        if (!ShipFiles.isSha256(sha)) throw bad("the SHA-256 of " + what + " is not 64 hex digits");
        return new Piece(asset, bytes, sha);
    }

    private static IOException bad(String why) {
        return new IOException("The game code record inside this app cannot be used: " + why + ".");
    }

    private static String text(Object value, String what) throws IOException {
        if (!(value instanceof String)) throw bad("it has no " + what);
        return (String) value;
    }

    private static long number(Object value, String what) throws IOException {
        if (!(value instanceof Long)) throw bad("it has no " + what);
        long number = ((Long) value).longValue();
        if (number < 0) throw bad(what + " is negative");
        return number;
    }

    private static Map<?, ?> object(Object value, String what) throws IOException {
        if (!(value instanceof Map)) throw bad("it has no " + what);
        return (Map<?, ?>) value;
    }

    private static List<?> list(Object value, String what) throws IOException {
        if (!(value instanceof List)) throw bad("it has no list of " + what);
        return (List<?>) value;
    }

    private static void requireDistinct(List<String> names, String what) throws IOException {
        Set<String> seen = new HashSet<String>();
        for (String name : names) {
            if (!seen.add(name)) throw bad("a " + what + " is listed twice: " + name);
        }
    }

    /** One part of a path: starts with a letter or digit, then letters, digits, '.', '_' and '-'; at most 128 long. */
    static boolean plainName(String name) {
        if (name == null || name.isEmpty() || name.length() > 128) return false;
        for (int at = 0; at < name.length(); ++at) {
            char c = name.charAt(at);
            boolean alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (!alnum && (at == 0 || (c != '.' && c != '_' && c != '-'))) return false;
        }
        return true;
    }

    /** A switch file of the game runtime: "diag-" followed by lower-case letters, digits and '-'. */
    static boolean switchName(String name) {
        if (name == null || !name.startsWith("diag-") || name.length() < 6 || name.length() > 80) return false;
        for (int at = 5; at < name.length(); ++at) {
            char c = name.charAt(at);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        }
        return true;
    }

    /** "ship/(plain name)": the only place bundled pieces are read from. */
    static boolean assetName(String asset) {
        return asset != null && asset.startsWith("ship/") && plainName(asset.substring(5));
    }

    /** "inputs/(plain name)" or "modules/(plain name)": the directories a start request may name. */
    static boolean dataDirectory(String path) {
        if (path == null) return false;
        int slash = path.indexOf('/');
        if (slash < 0) return false;
        String top = path.substring(0, slash);
        return (top.equals("inputs") || top.equals("modules")) && plainName(path.substring(slash + 1));
    }

    /** The game's own rule for a path: absolute, no empty, "." or ".." part, only letters, digits, '_', '-' and '.'. */
    static boolean plainAbsolutePath(String path) {
        if (path == null || path.length() < 2 || path.length() > 2000 || path.charAt(0) != '/') return false;
        for (String part : path.substring(1).split("/", -1)) {
            if (part.equals(".") || part.equals("..")) return false;
            if (part.isEmpty()) return false;
            for (int at = 0; at < part.length(); ++at) {
                char c = part.charAt(at);
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                        || c == '_' || c == '-' || c == '.')) {
                    return false;
                }
            }
        }
        return true;
    }
}
