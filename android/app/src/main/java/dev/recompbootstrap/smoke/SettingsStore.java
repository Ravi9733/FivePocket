package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/**
 * Loads and saves the user's settings and brings the game's input files in line with them (design sections 3.5 and
 * 3.6). Pure Java.
 *
 * What this class guarantees:
 * - load never writes anything.
 * - save writes only when asked, never while the game process runs, and only files on RuntimeMapping.ownedFiles()
 *   (plus "&lt;name&gt;.tmp" beside one while it is written). A file that already has its planned state is not touched,
 *   so its modification time stays. A switch file has its planned state as soon as it exists, whatever it holds.
 * - No path is written through a symbolic link, and no directory is ever removed.
 * - Nothing under files/saves, files/profile or files/saves-sandbox is on the list, and neither is the switch file
 *   diag-save-sandbox, so none of them is ever opened for writing, renamed or removed.
 */
final class SettingsStore {
    interface GameRunning {
        /** True while the game process exists. */
        boolean check();
    }

    static final class Loaded {
        /** The saved settings, or (fromFile false) what the game's files stand for today. */
        final Settings settings;
        /** True when a valid settings.json was read. */
        final boolean fromFile;
        /** One plain sentence per problem. Unmodifiable. */
        final List<String> warnings;
        /** Runtime files that differ from the saved settings' plan. Always empty when fromFile is false. Unmodifiable. */
        final List<String> drift;

        Loaded(Settings settings, boolean fromFile, List<String> warnings, List<String> drift) {
            this.settings = settings;
            this.fromFile = fromFile;
            this.warnings = Collections.unmodifiableList(warnings);
            this.drift = Collections.unmodifiableList(drift);
        }
    }

    static final class SaveResult {
        /** True when settings.json and every runtime file have their planned state. */
        final boolean ok;
        /** True when nothing was written because the game is running. */
        final boolean refusedGameRunning;
        /** Relative paths that were created or replaced / removed by this save, in the order it happened. Unmodifiable. */
        final List<String> written, removed;
        /**
         * One line that says why the save failed (the screen puts "Could not save: " before it). null when ok,
         * with one exception: ok together with NOTE_GAME_APPEARED when the game was started during the save.
         */
        final String error;

        SaveResult(boolean ok, boolean refusedGameRunning, List<String> written, List<String> removed, String error) {
            this.ok = ok;
            this.refusedGameRunning = refusedGameRunning;
            this.written = Collections.unmodifiableList(written);
            this.removed = Collections.unmodifiableList(removed);
            this.error = error;
        }
    }

    static final String ERROR_GAME_RUNNING = "The game is running. Close it, then save.";
    /** SaveResult.error when the game's folder is missing: nothing is written, settings.json neither. */
    static final String ERROR_NO_GAME = "The game is not installed yet, so there is nothing to apply the settings to";
    /** SaveResult.error of a completed save during which the game process appeared. */
    static final String NOTE_GAME_APPEARED = "Saved, but the game was started meanwhile. Restart the game so that it uses all of the new settings.";
    /** Start of the warning that names developer switch files; MenuModel.TEXT_STANDING is the same text. */
    static final String WARNING_STANDING = "A developer switch file overrules settings shown here: ";
    /**
     * Switch files of the game runtime that this screen neither shows nor writes, and that overrule a row. They are
     * only reported, never touched. (The test files of the world sliders are not here any more: the game ignores one
     * that stands when it starts, and a Save removes them - RuntimeMapping.removeOnlyFiles().)
     */
    private static final List<String> PACING_MODIFIERS = Collections.unmodifiableList(Arrays.asList(
            "diag-pace-dpt", "diag-pace-lowlat", "diag-pace-nolend"));
    private static final List<String> PAIR_DRY_RUN = Collections.unmodifiableList(Arrays.asList(
            "diag-fg-pair-ahead", "diag-fg-pair-between"));

    /** Only looked at: the developer switch of the game's render-resolution A/B. */
    private static final String STAGE_A_SWITCH = "diag-rs-stage-a";
    /** The two switch files of frame generation: this screen's own in a build whose row is live, a developer's otherwise. */
    private static final List<String> GENERATION = Collections.unmodifiableList(Arrays.asList(
            RuntimeMapping.FG_BETWEEN, RuntimeMapping.FG_AHEAD));

    /**
     * What a standing developer switch file does to the rows, in a few words; null for any other name, for the
     * GSR 1 switch in a build where the Scaler row accounts for it (a build that reads scaler.txt) or that does not
     * read it at all, for a frame-generation switch in a build that does not read it or whose Frame Generation
     * row is live (the row then shows and writes it), and for the stage A switch in a build without stage B.
     */
    static String standingEffect(String name, Capabilities capabilities) {
        if (name.equals("diag-v450-fsr1-no-rcas")) return "FSR sharpness has no effect";
        if (PACING_MODIFIERS.contains(name)) return "frame pacing is changed";
        if (PAIR_DRY_RUN.contains(name)) return "a frame-pair timing test runs under the 30 fps cap";
        // The developer switch that keeps render-resolution stage A in a stage B build (v464 generic_renderer.cpp
        // rsbLoad): the Render Resolution row's modes, "Native" above all, then do not say what the game shows.
        if (name.equals(STAGE_A_SWITCH)) {
            return capabilities.renderStageB() ? "every render size is shown through a 1280x720 picture, whatever Render Resolution says" : null;
        }
        for (String path : GENERATION) {
            if (!name.equals(RuntimeMapping.name(path))) continue;
            return capabilities.reads(path) && !capabilities.anyChoice(SettingsSchema.FRAME_GENERATION)
                    ? "frame generation is switched on outside this screen" : null;
        }
        if (name.equals(RuntimeMapping.name(RuntimeMapping.GSR_SWITCH))) {
            return capabilities.reads(RuntimeMapping.GSR_SWITCH) && !capabilities.reads(RuntimeMapping.SCALER_TXT)
                    ? "Snapdragon GSR 1 is the scaler, whatever the Scaler row says" : null;
        }
        return null;
    }
    private static final int SETTINGS_JSON_LIMIT = 65536;
    /** One save at a time in this process, whatever number of store objects exist. */
    private static final Object SAVING = new Object();

    private final File filesDir;
    private final GameRunning gameRunning;

    /** filesDir is the app's files directory (Context.getFilesDir()). */
    SettingsStore(File filesDir, GameRunning gameRunning) {
        if (filesDir == null || gameRunning == null) throw new IllegalArgumentException("filesDir and gameRunning are required");
        this.filesDir = filesDir;
        this.gameRunning = gameRunning;
    }

    /** A read-only view of the files directory. */
    FileView view() {
        final File root = filesDir;
        return new FileView() {
            @Override public boolean exists(String relativePath) {
                return new File(root, relativePath).exists();
            }

            @Override public byte[] read(String relativePath) throws IOException {
                return readUpTo(new File(root, relativePath), 4096);
            }
        };
    }

    /**
     * Never writes. With a valid settings.json: the saved settings (a row the file does not name: what the game's
     * files say for it, see SettingsJson) and the runtime files that differ from them.
     * Without one (missing, damaged, or written by a newer app version): what the runtime files stand for today, and
     * a warning when the file exists. The file itself is left as it is. In both cases a warning names developer
     * switch files that overrule a row (see standingEffect).
     */
    Loaded load(Capabilities capabilities) throws IOException {
        FileView view = view();
        ArrayList<String> warnings = new ArrayList<String>();
        String standing = standingSwitches(capabilities);
        File json = new File(filesDir, RuntimeMapping.SETTINGS_JSON);
        if (json.exists()) {
            String text = null;
            try {
                if (json.isFile() && json.length() <= SETTINGS_JSON_LIMIT) {
                    text = new String(readUpTo(json, SETTINGS_JSON_LIMIT), StandardCharsets.UTF_8);
                }
            } catch (IOException unreadable) {
                // handled below as a file that cannot be used
            }
            if (text == null) {
                warnings.add("The saved settings file cannot be read. It is not used; it is replaced when you save.");
            } else {
                SettingsJson.Decoded decoded = SettingsJson.decode(text);
                if (decoded.valid) {
                    if (standing != null) warnings.add(standing);
                    warnings.addAll(decoded.warnings);
                    // A row settings.json does not name was never chosen (it was grey when the file was saved):
                    // it shows what the game's files say, so that a Save leaves those files as they are.
                    Settings saved = decoded.settings;
                    if (!decoded.missing.isEmpty()) {
                        Settings standingState = RuntimeMapping.importFrom(view, capabilities);
                        for (String key : decoded.missing) saved = saved.with(key, standingState.get(key));
                    }
                    return new Loaded(saved, true, warnings, RuntimeMapping.drift(saved, capabilities, view));
                }
                warnings.addAll(decoded.warnings);
            }
        }
        Settings imported = RuntimeMapping.importFrom(view, capabilities);
        List<String> inexact = RuntimeMapping.drift(imported, capabilities, view);
        if (!inexact.isEmpty()) {
            StringBuilder names = new StringBuilder();
            for (String path : inexact) names.append(names.length() == 0 ? "" : ", ").append(RuntimeMapping.name(path));
            // First, so that it is the line the screen shows: a Save from any row would change these files.
            warnings.add(0, "Saving will also rewrite game setting files that are not in the form this screen writes: "
                    + names + ".");
        }
        if (standing != null) warnings.add(inexact.isEmpty() ? 0 : 1, standing);
        return new Loaded(imported, false, warnings, new ArrayList<String>());
    }

    /** The warning that names the developer switch files standing in files/private-trace, or null. Only lists names. */
    private String standingSwitches(Capabilities capabilities) {
        String[] present = new File(filesDir, "private-trace").list();
        if (present == null) return null;
        Arrays.sort(present);
        StringBuilder names = new StringBuilder();
        for (String name : present) {
            String effect = standingEffect(name, capabilities);
            if (effect != null) {
                names.append(names.length() == 0 ? "" : ", ").append(name).append(" (").append(effect).append(')');
            }
        }
        return names.length() == 0 ? null : WARNING_STANDING + names + ".";
    }

    /**
     * Never throws. Refuses while the game runs (nothing is written). Otherwise: creates files/settings if missing,
     * writes settings.json, then brings every runtime file to its planned state in the order of the fixed list.
     * A failure stops the sequence and is reported; settings.json is then already the new choice, and saving again
     * finishes the job (the sequence is idempotent).
     */
    SaveResult save(Settings settings, Capabilities capabilities) {
        synchronized (SAVING) {
            return saveNow(settings, capabilities);
        }
    }

    private SaveResult saveNow(Settings settings, Capabilities capabilities) {
        ArrayList<String> written = new ArrayList<String>(), removed = new ArrayList<String>();
        try {
            if (settings == null || capabilities == null) throw new IllegalArgumentException("settings and capabilities are required");
            if (gameRunning.check()) return new SaveResult(false, true, written, removed, ERROR_GAME_RUNNING);
            // The plan is made from the files as they are now; nothing has been written yet.
            Map<String, byte[]> plan = RuntimeMapping.plan(settings, capabilities, view());
            if (!filesDir.isDirectory()) throw new IOException("The app's files folder does not exist");
            // Asked before settings.json is written: without the game's folder no runtime file could follow it.
            if (!new File(filesDir, "private-trace").isDirectory()) {
                throw new IOException(ERROR_NO_GAME);
            }
            File directory = new File(filesDir, "settings");
            if (!directory.isDirectory() && !directory.mkdir()) throw new IOException("Could not create the settings folder");
            bring(RuntimeMapping.SETTINGS_JSON, SettingsJson.encode(settings, capabilities).getBytes(StandardCharsets.UTF_8), written, removed);
            apply(plan, written, removed);
            // Asked again after the last write: a game started meanwhile may have read old and new files mixed.
            boolean appeared = (!written.isEmpty() || !removed.isEmpty()) && gameRunning.check();
            return new SaveResult(true, false, written, removed, appeared ? NOTE_GAME_APPEARED : null);
        } catch (IOException | RuntimeException failed) {
            String reason = failed.getMessage() == null ? failed.getClass().getSimpleName() : failed.getMessage();
            return new SaveResult(false, false, written, removed, reason);
        }
    }

    /** Brings each file of the plan to its planned state (null: absent; otherwise that content), in the plan's order. */
    void apply(Map<String, byte[]> plan, List<String> written, List<String> removed) throws IOException {
        for (Map.Entry<String, byte[]> entry : plan.entrySet()) {
            bring(entry.getKey(), entry.getValue(), written, removed);
        }
    }

    private void bring(String relativePath, byte[] content, List<String> written, List<String> removed) throws IOException {
        File target = ownedTarget(filesDir, relativePath);
        try {
            if (content == null) {
                if (AtomicFiles.remove(target)) removed.add(relativePath);
            } else if (!holds(target, content)) {
                AtomicFiles.write(target, content);
                written.add(relativePath);
            }
        } catch (IOException failed) {
            throw new IOException(relativePath + " could not be " + (content == null ? "removed" : "written") + " ("
                    + (failed.getMessage() == null ? failed.getClass().getSimpleName() : failed.getMessage()) + ")");
        }
    }

    /**
     * The one gate every write and removal of the settings code passes: the File for a path of the fixed list.
     * IOException when the path is not on RuntimeMapping.ownedFiles(), or when its folder, the file itself or its
     * temporary name is a symbolic link (the path would then leave files/settings or files/private-trace).
     */
    static File ownedTarget(File filesDir, String relativePath) throws IOException {
        if (!RuntimeMapping.ownedFiles().contains(relativePath)) {
            throw new IOException(relativePath + " is not one of the setting files; it is not touched");
        }
        File target = new File(filesDir, relativePath);
        File folder = target.getParentFile();
        File expectedFolder = new File(filesDir.getCanonicalFile(), folder.getName());
        if (!folder.getCanonicalFile().equals(expectedFolder)
                || Files.isSymbolicLink(target.toPath()) || Files.isSymbolicLink(AtomicFiles.tempFor(target).toPath())) {
            throw new IOException(relativePath + " is behind a symbolic link; it is not touched");
        }
        return target;
    }

    /**
     * True when target already has its planned state: a regular file with exactly this content, or, for a switch file
     * (planned with zero bytes), any regular file - the game only asks whether a switch file exists, so one that holds
     * bytes is as good as an empty one and is left alone.
     */
    private static boolean holds(File target, byte[] content) throws IOException {
        if (!Files.isRegularFile(target.toPath(), LinkOption.NOFOLLOW_LINKS)) return false;
        if (content.length == 0) return true;
        return target.length() == content.length && Arrays.equals(readUpTo(target, content.length + 1), content);
    }

    /** The first limit bytes of a file (all of it when it is shorter). Missing file or a directory: FileNotFoundException. */
    private static byte[] readUpTo(File file, int limit) throws IOException {
        if (!file.isFile()) throw new FileNotFoundException(file.getName() + " is not a file");
        FileInputStream in = new FileInputStream(file);
        try {
            byte[] buffer = new byte[limit];
            int size = 0;
            while (size < limit) {
                int read = in.read(buffer, size, limit - size);
                if (read < 0) break;
                size += read;
            }
            return size == limit ? buffer : Arrays.copyOf(buffer, size);
        } finally {
            in.close();
        }
    }
}
