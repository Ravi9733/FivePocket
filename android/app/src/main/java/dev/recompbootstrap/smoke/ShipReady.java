package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;

/**
 * Whether a build that carries its game code can start the game right now, and if not, what stands in the way.
 * Pure Java, read-only. One answer for Play (ShipPlay), for the launcher's state line (InstallState.detect with
 * this answer, see CROSS-LANE.md) and for the first screen, so that "Ready" means the same everywhere:
 *
 *   - this app version's game code is in place (ShipFirstRun.installed: the mark, the library, the shader pack,
 *     the bundled start request by its SHA-256, the pin, the record of the qualified code, the bundled pieces);
 *   - the game code file and the shader file have their recorded SHA-256 (ShipFirstRun.changedCode). They are the
 *     two large pieces the app carries; without this the menu read "Ready" over a file of the right size and other
 *     bytes, and only pressing Play found it;
 *   - every pinned game data file is in place with its size, and the bundled pieces and the files up to 64 MiB
 *     also with their SHA-256 (ShipData.look).
 *
 * It reads the record out of the APK each time it is asked, so an app update is seen at once. Call it off the UI
 * thread: it hashes about 490 MB in the real build (the game code, the shader file and 75 MB of small files).
 * Play asks without the second point (ofSizes) because it hashes those two files itself, on the way to the start.
 * The menu's pages ask through remembered(): the second point is then answered from the last whole read of the two
 * files while they are still the same files (ShipVerify, which also writes one log line per read with its time).
 */
final class ShipReady {
    enum Stands {
        /** This APK carries no game code (a developer build): the rule does not apply, the launcher's own does. */
        NOT_BUNDLED,
        /** Nothing: the game can be started. */
        NOTHING,
        /** The app's own file (the APK) cannot be found or read, so nothing can be said: not ready. */
        APP_FILE,
        /** This app version's game code is not in place (first start not finished, or an update is pending). */
        CODE,
        /** A file this app version carries (the game code, the shader file, a bundled piece of game data) is absent or was changed. */
        PIECE,
        /** Game data that comes from the player's own discs is absent or is not the pinned file. */
        DATA
    }

    final Stands stands;
    /** The bundled record; null for NOT_BUNDLED and APP_FILE, and for CODE when the record itself is unusable. */
    final ShipConfig config;
    /**
     * What is in place of the pinned game data; null for NOT_BUNDLED and APP_FILE, and for CODE when the record itself
     * is unusable. (For CODE with a usable record it is there since the second repair: the pages can then count the
     * player's disc files also while one of the app's own pieces is not in place.)
     */
    final ShipData data;
    /** For APP_FILE: why the APK could not be read. Otherwise null. */
    final String detail;
    /** For PIECE: the game code file or the shader file that has other bytes (ShipFirstRun.changedCode); else empty. */
    final java.util.List<String> changedCode;

    private ShipReady(Stands stands, ShipConfig config, ShipData data, String detail) {
        this(stands, config, data, detail, java.util.Collections.<String>emptyList());
    }

    private ShipReady(Stands stands, ShipConfig config, ShipData data, String detail, java.util.List<String> changedCode) {
        this.stands = stands;
        this.config = config;
        this.data = data;
        this.detail = detail;
        this.changedCode = changedCode;
    }

    /** True in a build that carries game code (whatever its state). */
    boolean bundled() {
        return stands != Stands.NOT_BUNDLED;
    }

    /** True when the game can be started: nothing stands in the way. False for a developer build as well. */
    boolean ready() {
        return stands == Stands.NOTHING;
    }

    /**
     * apkPath: the app's own APK file as Android names it (ApplicationInfo.sourceDir). null is not "a developer
     * build" but "unknown": the answer is APP_FILE, which is not ready.
     */
    static ShipReady of(String apkPath, File filesDir) {
        return ofApk(apkPath, filesDir, null);
    }

    /**
     * For the menu's pages (the Play tab, About, the two-disc setup page): the same answer, with the game code file
     * and the shader file read whole only when the last whole read is no longer about the same two files or
     * ShipVerify.again() was called since (see ShipVerify). why: who asks, for the log line of a read.
     */
    static ShipReady remembered(String apkPath, File filesDir, String why) {
        return ofApk(apkPath, filesDir, why);
    }

    /** rememberedWhy null: read the two files now. */
    private static ShipReady ofApk(String apkPath, File filesDir, String rememberedWhy) {
        if (apkPath == null) return new ShipReady(Stands.APP_FILE, null, null, "its place is not known");
        ShipBundle bundle;
        try {
            bundle = PublicGameCode.open(apkPath, filesDir);
        } catch (IOException | RuntimeException unreadable) {
            return new ShipReady(Stands.APP_FILE, null, null, ShipFiles.reason(unreadable));
        }
        try {
            // Public Play checks the retained import as well as the active files. The menu and disc Finish row
            // must use that same producer/consumer gate, otherwise they offer Play for a corrupted import.
            if (bundle instanceof PublicGameCode) {
                ShipConfig contract = ((PublicGameCode) bundle).config;
                String problem = PublicGameCode.installedProblem(InstallGuard.root(filesDir), contract);
                if (problem != null) return new ShipReady(Stands.CODE, contract, ShipData.look(contract, filesDir), problem);
            }
            return rememberedWhy == null ? of(bundle, filesDir) : remembered(bundle, filesDir, rememberedWhy);
        } finally {
            try {
                bundle.close();
            } catch (IOException ignored) {
                // it was only read
            }
        }
    }

    /** bundle null: a build without game code. */
    static ShipReady of(ShipBundle bundle, File filesDir) {
        return of(bundle, filesDir, true, null);
    }

    /** The menu's question with the APK already open (remembered(String, File, String) opens it). bundle null: a build without game code. */
    static ShipReady remembered(ShipBundle bundle, File filesDir, String why) {
        return of(bundle, filesDir, true, why);
    }

    /**
     * For Play alone: the same answer without reading the game code file and the shader file. Play hashes both
     * itself before it starts anything (ShipPlay), so asking here as well would read 412 MB twice at every Play.
     */
    static ShipReady ofSizes(ShipBundle bundle, File filesDir) {
        return of(bundle, filesDir, false, null);
    }

    /** rememberedWhy null: the two files are read now (when hashCode); else the menu's remembered read (ShipVerify). */
    private static ShipReady of(ShipBundle bundle, File filesDir, boolean hashCode, String rememberedWhy) {
        if (bundle == null) return new ShipReady(Stands.NOT_BUNDLED, null, null, null);
        ShipConfig config;
        try {
            config = ShipFirstRun.config(bundle);
        } catch (IOException | RuntimeException unusable) {
            return new ShipReady(Stands.CODE, null, null, null);   // the first screen's run says why
        }
        if (config == null) return new ShipReady(Stands.NOT_BUNDLED, null, null, null);
        ShipData data = ShipData.look(config, filesDir);
        if (!ShipFirstRun.installed(config, filesDir)) return new ShipReady(Stands.CODE, config, data, null);
        if (hashCode) {
            java.util.List<String> changed = rememberedWhy == null ? ShipVerify.read(config, filesDir, ShipVerify.WHY_CHECK)
                    : ShipVerify.remembered(config, filesDir, rememberedWhy);
            if (!changed.isEmpty()) return new ShipReady(Stands.PIECE, config, data, null, changed);
        }
        if (data.complete()) return new ShipReady(Stands.NOTHING, config, data, null);
        return new ShipReady(data.bundledAmongThem > 0 ? Stands.PIECE : Stands.DATA, config, data, null);
    }
}
