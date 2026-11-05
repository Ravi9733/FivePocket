package dev.recompbootstrap.smoke;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The one table that turns Settings into the files the game reads, and back. Pure Java.
 * The contract of every file (format, range, when the game reads it, first game build, source line) is the table in
 * local-analysis/agents/app-ui-2/CONTRACTS.md; this class is its implementation.
 *
 * THE FIXED LIST. ownedFiles() names every file the settings code may ever create, replace or remove (plus
 * "&lt;name&gt;.tmp" beside one of them while it is written). Everything else in the app's files/ directory - the game's
 * standing switch files, its data, the run directories and above all files/saves, files/profile and
 * files/saves-sandbox - is never written, renamed or removed by this code. SettingsStore refuses any path that is not
 * on this list. The last eight entries (removeOnlyFiles()) are developer test files of the world sliders: they are
 * only ever removed, never created, and they are not capabilities.
 *
 * WHAT WAS CHECKED AGAINST THE GAME. Every path and every value below was read against the sources of game build v464
 * (local-analysis/attempts/v464/src: scaler_settings.h, render_scale.h, world_load.h, pace_caps.h and the literal paths
 * in generic_renderer.cpp and direct_present.h), and the value files are handed to those headers' own parsers by this
 * lane's connected check (local-analysis/agents/ship-week/menu-settings/contract/run_contract.sh). One path is newer
 * than that build: diag-pace-auto-low was read against the sources of v524 (generic_renderer.cpp, paceWindow).
 *
 * THE RULE OF LEAST CHANGE. plan() names a file only when the installed game build reads it (its name is in the
 * library) and the user's choice needs a certain state of it. A file the build does not read is left exactly as it
 * is; so is a switch file whose question a value file answers (scaler.txt decides: the scaler switch files are not
 * consulted by the game and not touched here). A Save with every row as the screen showed it on opening therefore
 * changes no file the game reads.
 *
 * File kinds: a switch file is a file whose presence is the signal (this code creates it empty); a value file is a
 * short ASCII text with one line feed at the end, written as a temporary file and renamed.
 */
final class RuntimeMapping {
    private RuntimeMapping() { }

    // ---- the fixed list (paths relative to the app's files/ directory)
    static final String SETTINGS_JSON = "settings/settings.json";          // the user's choice
    static final String CAPABILITIES_CACHE = "settings/capabilities.txt";  // cache of the capability scan
    static final String SCALER_TXT = "settings/scaler.txt";                // value: fsr1 | cubic | nearest | gsr1 (v462)
    static final String SHARPNESS_TXT = "settings/fsr-sharpness.txt";      // value: stops 0..4, e.g. 0.5 (v462)
    static final String RENDER_SCALE_TXT = "settings/render-scale.txt";    // value: percent 50..200, e.g. 75 (v463)
    static final String WORLD_LOAD_TXT = "settings/world-load.txt";        // value: lines key=N (v461)
    static final String LIMIT30 = "private-trace/diag-v280-limit30";       // switch: the game's own 30 fps limit
    static final String LIMIT60 = "private-trace/diag-v280-limit60";       // switch: forces 60 over limit30; only ever removed here
    static final String PACE_40 = "private-trace/diag-pace-40";            // switches of the even-pacing caps (v460)
    static final String PACE_30 = "private-trace/diag-pace-30";
    static final String PACE_24 = "private-trace/diag-pace-24";
    static final String PACE_AUTO = "private-trace/diag-pace-auto";
    // Switch beside diag-pace-auto (pace_caps.h: inert without it): the automatic cap goes on below 24 fps to 20 and 15.
    // Written only with frame generation under the automatic cap, in a build that announces it
    // (Capabilities.generationUnderAutomatic); in every other build a file of that name is a developer's and is never touched.
    static final String PACE_AUTO_LOW = "private-trace/diag-pace-auto-low";
    // Switches of frame generation, one per order (frame-gen/DESIGN-stage1.md section 7; both present: between). In no
    // build yet: v464 reads only the timing dry run's diag-fg-pair-* files, which are not these and are never touched.
    static final String FG_BETWEEN = "private-trace/diag-fg-between";
    static final String FG_AHEAD = "private-trace/diag-fg-ahead";
    static final String SCALE_FSR1 = "private-trace/diag-v450-scale-fsr1"; // switch: FSR 1 (builds without scaler.txt)
    static final String SCALE_CUBIC = "private-trace/diag-v409-scale-cubic"; // switch: bicubic (builds without scaler.txt)
    static final String SCALE_LINEAR = "private-trace/diag-v409-scale-linear"; // switch: bilinear; only ever removed here (for Nearest)
    static final String FSR1_STOPS = "private-trace/diag-v450-fsr1-stops"; // value: stops (builds without fsr-sharpness.txt)
    static final String FPS_OVERLAY = "private-trace/diag-fps-overlay";    // switch: frame-rate counter
    static final String INPUT_UNBUFFERED = "private-trace/diag-lt2-unbuffered"; // switch read by the app's game activity

    /**
     * NOT on the fixed list, only looked at: the developer switch that selects Snapdragon GSR 1 (v461). A build that
     * reads scaler.txt consults it only while scaler.txt holds no valid word; plan() then writes the word when this
     * file would give another scaler than the one chosen. It is never created or removed here.
     */
    static final String GSR_SWITCH = "private-trace/diag-v460-scale-gsr1";

    private static final List<String> SETTING_FILES = Collections.unmodifiableList(Arrays.asList(
            SETTINGS_JSON, CAPABILITIES_CACHE, SCALER_TXT, SHARPNESS_TXT, RENDER_SCALE_TXT, WORLD_LOAD_TXT,
            LIMIT30, LIMIT60, PACE_40, PACE_30, PACE_24, PACE_AUTO, PACE_AUTO_LOW, FG_BETWEEN, FG_AHEAD, SCALE_FSR1, SCALE_CUBIC, SCALE_LINEAR,
            FSR1_STOPS, FPS_OVERLAY, INPUT_UNBUFFERED));
    /**
     * Developer test files of the world sliders (world_load.h). A stopped A/B script leaves one behind; the game
     * ignores a file that was there when it started, so removing a leftover changes no picture. Only ever removed.
     */
    private static final List<String> REMOVE_ONLY = Collections.unmodifiableList(Arrays.asList(
            "private-trace/diag-wl-test", "private-trace/diag-wl-test-min", "private-trace/diag-wl-test-vehicles",
            "private-trace/diag-wl-test-peds", "private-trace/diag-wl-test-parked", "private-trace/diag-wl-test-scenario",
            "private-trace/diag-wl-test-lod", "private-trace/diag-wl-lod-unlock"));
    private static final List<String> OWNED = Collections.unmodifiableList(join(SETTING_FILES, REMOVE_ONLY));
    /** SETTING_FILES without the two files only the app reads. */
    private static final List<String> RUNTIME = Collections.unmodifiableList(new ArrayList<String>(SETTING_FILES.subList(2, SETTING_FILES.size())));

    private static final byte[] SWITCH = new byte[0];
    /** The two spellings the game accepts for each slider, in slider order (the second is the one plan writes). */
    private static final String[][] WORLD_SPELLINGS = {{"vehicles", "traffic"}, {"peds", "pedestrians"}, {"parked", "parked_cars"},
            {"scenario", "scenario_pedestrians"}, {"lod", "draw_distance"}};
    private static final String[] WORLD_KEYS = {SettingsSchema.WORLD_TRAFFIC, SettingsSchema.WORLD_PEDESTRIANS,
            SettingsSchema.WORLD_PARKED_CARS, SettingsSchema.WORLD_SCENARIO_PEDESTRIANS, SettingsSchema.WORLD_DRAW_DISTANCE};
    private static final int WORLD_LOD = 4;
    /** What the scaler switch files stand for when they give bilinear: a state no row value expresses. */
    private static final int SCALER_LINEAR = -2;

    private static List<String> join(List<String> first, List<String> second) {
        ArrayList<String> all = new ArrayList<String>(first);
        all.addAll(second);
        return all;
    }

    /** Every path the settings code may create, replace or remove, relative to files/. */
    static List<String> ownedFiles() {
        return OWNED;
    }

    /** The files a game build may read for a setting: the names the capability scan looks for. */
    static List<String> runtimeFiles() {
        return RUNTIME;
    }

    /** The part of ownedFiles() that is only ever removed and never looked for in the game library. */
    static List<String> removeOnlyFiles() {
        return REMOVE_ONLY;
    }

    /** "private-trace/diag-pace-40" -> "diag-pace-40": the name the game library contains when a build reads the file. */
    static String name(String ownedPath) {
        return ownedPath.substring(ownedPath.lastIndexOf('/') + 1);
    }

    /**
     * The state the game's files must have for these settings: path -> null (must be absent), zero bytes (a switch
     * file: it must exist, and like the game this code does not care what it holds), or the exact content of a value
     * file. A path that is missing from the result is left as it is (the rule of least change, see the class text).
     * A setting the installed build cannot honour is planned as its default (Capabilities.effective).
     *
     * files is only read. It is needed in two places where the game asks an older developer file as long as the
     * value file is silent: the scaler switch files under scaler.txt, and diag-v450-fsr1-stops under
     * fsr-sharpness.txt. The value file is planned with the chosen value when those older files would give another
     * one; otherwise it is planned absent, or left alone when it already holds the chosen value. The result is the
     * user's choice in every case.
     */
    static Map<String, byte[]> plan(Settings settings, Capabilities capabilities, FileView files) {
        HashMap<String, byte[]> want = new HashMap<String, byte[]>();

        // Frame rate cap. 60 is what the game does with no cap file. "Off" is never supported and arrives here as 60.
        // A pacing cap file and diag-v280-limit30 never stand together: whenever a cap file is planned, limit30 is
        // planned absent, and whenever limit30 is planned, the four cap files are planned absent (also in a build
        // that does not read them: a later build would). diag-v280-limit60 is touched in one case only: 30 fps made
        // by limit30, which it would undo.
        int cap = capabilities.effective(SettingsSchema.FRAME_CAP, settings.get(SettingsSchema.FRAME_CAP));
        boolean thirtyByLimiter = cap == SettingsSchema.CAP_30 && !capabilities.reads(PACE_30);
        boolean paced = cap == SettingsSchema.CAP_40 || cap == SettingsSchema.CAP_24 || cap == SettingsSchema.CAP_AUTO
                || (cap == SettingsSchema.CAP_30 && !thirtyByLimiter);
        if (thirtyByLimiter) {
            want.put(LIMIT30, SWITCH);
            want.put(LIMIT60, null);
        } else if (paced || (capabilities.reads(LIMIT30) && !on(files, capabilities, LIMIT60))) {
            // (At 60, a limit30 standing beside limit60 has no effect in the game and is left as it is.)
            want.put(LIMIT30, null);
        }
        // The other direction of the same rule: limit30 is created only with every cap file absent, read or not.
        if (capabilities.reads(PACE_40) || thirtyByLimiter) want.put(PACE_40, cap == SettingsSchema.CAP_40 ? SWITCH : null);
        if (capabilities.reads(PACE_30) || thirtyByLimiter) want.put(PACE_30, cap == SettingsSchema.CAP_30 && !thirtyByLimiter ? SWITCH : null);
        if (capabilities.reads(PACE_24) || thirtyByLimiter) want.put(PACE_24, cap == SettingsSchema.CAP_24 ? SWITCH : null);
        if (capabilities.reads(PACE_AUTO) || thirtyByLimiter) want.put(PACE_AUTO, cap == SettingsSchema.CAP_AUTO ? SWITCH : null);

        // Scaler. A build that reads scaler.txt: only the word file, and only when the switch files would say
        // something else. An older build: its switch files. A build that reads neither: nothing.
        int scaler = capabilities.effective(SettingsSchema.SCALER, settings.get(SettingsSchema.SCALER));
        if (capabilities.reads(SCALER_TXT)) {
            if (switchScaler(files, capabilities, true) != scaler) {
                want.put(SCALER_TXT, text(scalerWord(scaler)));
            } else if (!files.exists(SCALER_TXT) || parseScalerWord(content(files, SCALER_TXT)) != scaler) {
                want.put(SCALER_TXT, null); // (a file that already holds the chosen word is left as it is)
            }
        } else {
            if (capabilities.reads(SCALE_FSR1)) want.put(SCALE_FSR1, scaler == SettingsSchema.SCALER_FSR1 ? SWITCH : null);
            if (capabilities.reads(SCALE_CUBIC)) {
                want.put(SCALE_CUBIC, scaler == SettingsSchema.SCALER_FSR1 || scaler == SettingsSchema.SCALER_BICUBIC ? SWITCH : null);
            }
            // Bilinear loses against cubic and FSR 1 in the game, so its switch matters only for Nearest.
            if (capabilities.reads(SCALE_LINEAR) && scaler == SettingsSchema.SCALER_NEAREST) want.put(SCALE_LINEAR, null);
        }

        // FSR sharpness, in tenths of a stop. Same pattern: the value file only when the older file says otherwise.
        int sharpness = capabilities.effective(SettingsSchema.FSR_SHARPNESS, settings.get(SettingsSchema.FSR_SHARPNESS));
        byte[] stops = text(sharpness / 10 + "." + sharpness % 10);
        if (capabilities.reads(SHARPNESS_TXT)) {
            if (olderStops(files, capabilities) != sharpness) {
                want.put(SHARPNESS_TXT, stops);
            } else if (!files.exists(SHARPNESS_TXT) || parseStopsTenths(content(files, SHARPNESS_TXT), true) != sharpness) {
                want.put(SHARPNESS_TXT, null); // (a file that already holds the chosen value is left as it is)
            }
        } else if (capabilities.reads(FSR1_STOPS) && olderStops(files, capabilities) != sharpness) {
            // (A file that already gives the chosen value, in the plain form this code reads, is left as it is.)
            want.put(FSR1_STOPS, sharpness == SettingsSchema.SHARPNESS_DEFAULT ? null : stops);
        }

        // Render resolution, percent per axis. 100 is "no file".
        if (capabilities.reads(RENDER_SCALE_TXT)) {
            int scale = capabilities.effective(SettingsSchema.RENDER_SCALE, settings.get(SettingsSchema.RENDER_SCALE));
            want.put(RENDER_SCALE_TXT, scale == SettingsSchema.RENDER_SCALE_100 ? null
                    : text(SettingsSchema.spec(SettingsSchema.RENDER_SCALE).choiceIds[scale]));
        }

        // Frame generation: one switch file per order, named only when the row is live (the build announces its
        // generator, Capabilities.FRAME_GENERATION_MARKER) - in any other build a file of that name is somebody's test
        // and is left alone. The generator makes its second picture only while the pacer holds 30 fps - and, in a build
        // that announces it, the in-between order also under the automatic cap (Capabilities.generationPaced) -, and a
        // standing switch changes how frames are timed even when it makes none, so in every other case both files are
        // planned absent; the chosen order stays in settings.json and returns with a cap that takes it.
        // (A live row means the build reads diag-pace-30, so 30 fps here is the pacer's cap, never the old limiter's.)
        int generation = capabilities.effective(SettingsSchema.FRAME_GENERATION, settings.get(SettingsSchema.FRAME_GENERATION));
        if (!capabilities.generationPaced(cap, generation)) generation = SettingsSchema.FG_OFF;
        if (capabilities.anyChoice(SettingsSchema.FRAME_GENERATION)) {
            if (capabilities.reads(FG_BETWEEN)) want.put(FG_BETWEEN, generation == SettingsSchema.FG_BETWEEN ? SWITCH : null);
            if (capabilities.reads(FG_AHEAD)) want.put(FG_AHEAD, generation == SettingsSchema.FG_AHEAD ? SWITCH : null);
        }
        // The automatic cap's lower range goes with frame generation under that cap and with nothing else: its file is
        // planned present exactly then and absent in every other case, so that Automatic without frame generation
        // stays the cap that ends at 24 fps. Only a build that announces generation under the automatic cap is asked;
        // any other build that reads the file (v522, v523) has it as a developer's switch, which is left alone.
        if (capabilities.generationUnderAutomatic()) {
            want.put(PACE_AUTO_LOW, cap == SettingsSchema.CAP_AUTO && generation == SettingsSchema.FG_BETWEEN ? SWITCH : null);
        }

        if (capabilities.reads(FPS_OVERLAY)) {
            want.put(FPS_OVERLAY, capabilities.effective(SettingsSchema.FPS_OVERLAY, settings.get(SettingsSchema.FPS_OVERLAY)) == 1 ? SWITCH : null);
        }

        // World sliders. Every slider at the original is "no file": the game then stores nothing at all. The
        // draw-distance line is written only for a build that announces it (it is locked in the runtime otherwise).
        if (capabilities.reads(WORLD_LOAD_TXT)) {
            boolean lod = capabilities.has(Capabilities.WORLD_LOD_MARKER);
            StringBuilder world = new StringBuilder();
            boolean allFull = true;
            for (int i = 0; i < WORLD_KEYS.length; i++) {
                if (i == WORLD_LOD && !lod) continue;
                int percent = capabilities.effective(WORLD_KEYS[i], settings.get(WORLD_KEYS[i]));
                allFull &= percent == 100;
                if (i > 0) world.append('\n');
                world.append(WORLD_SPELLINGS[i][1]).append('=').append(percent);
            }
            want.put(WORLD_LOAD_TXT, allFull ? null : text(world.toString()));
        }

        // The input switch is read by the app's own game activity, whatever the game library is.
        want.put(INPUT_UNBUFFERED, capabilities.effective(SettingsSchema.INPUT_UNBUFFERED, settings.get(SettingsSchema.INPUT_UNBUFFERED)) == 1 ? SWITCH : null);

        for (String leftover : REMOVE_ONLY) want.put(leftover, null);

        LinkedHashMap<String, byte[]> ordered = new LinkedHashMap<String, byte[]>();
        for (String path : OWNED) {
            if (want.containsKey(path)) ordered.put(path, want.remove(path));
        }
        if (!want.isEmpty()) throw new IllegalStateException("the plan names files that are not on the fixed list: " + want.keySet());
        return ordered;
    }

    /**
     * The settings the game's files stand for: the inverse of plan with the game's own precedence. Only reads.
     * A file whose name the installed build does not contain is ignored (the build does not read it), so the result
     * holds supported values only.
     */
    static Settings importFrom(FileView files, Capabilities capabilities) throws IOException {
        Settings settings = Settings.defaults();

        // pace_caps.h: the automatic cap wins over the fixed ones; of several fixed ones the lowest rate wins.
        // Without a cap file the game's own limiter: 60, unless diag-v280-limit30 stands without diag-v280-limit60.
        int cap = on(files, capabilities, PACE_AUTO) ? SettingsSchema.CAP_AUTO
                : on(files, capabilities, PACE_24) ? SettingsSchema.CAP_24
                : on(files, capabilities, PACE_30) ? SettingsSchema.CAP_30
                : on(files, capabilities, PACE_40) ? SettingsSchema.CAP_40
                : on(files, capabilities, LIMIT60) ? SettingsSchema.CAP_60
                : on(files, capabilities, LIMIT30) ? SettingsSchema.CAP_30 : SettingsSchema.CAP_60;
        settings = supported(settings, capabilities, SettingsSchema.FRAME_CAP, cap);

        int scaler = -1;
        if (on(files, capabilities, SCALER_TXT)) scaler = parseScalerWord(content(files, SCALER_TXT));
        boolean gsrOffered = capabilities.valueSupported(SettingsSchema.SCALER, SettingsSchema.SCALER_GSR1);
        if (scaler == SettingsSchema.SCALER_GSR1 && !gsrOffered) {
            scaler = SettingsSchema.SCALER_FSR1; // a build without the pass shows FSR 1 for the word gsr1
        }
        // A standing GSR 1 developer switch in a build where the row cannot offer GSR 1: the row shows what the
        // other switch files say (a Save at the shown value then changes none of them); a warning names the switch.
        if (scaler < 0) scaler = switchScaler(files, capabilities, gsrOffered);
        if (scaler == SCALER_LINEAR) scaler = SettingsSchema.SCALER_NEAREST; // the nearest row value; a Save rewrites
        settings = supported(settings, capabilities, SettingsSchema.SCALER, scaler);

        int sharpness = -1;
        if (on(files, capabilities, SHARPNESS_TXT)) sharpness = parseStopsTenths(content(files, SHARPNESS_TXT), false);
        if (sharpness < 0 && on(files, capabilities, FSR1_STOPS)) sharpness = parseStopsTenths(content(files, FSR1_STOPS), false);
        if (sharpness >= 0) settings = supported(settings, capabilities, SettingsSchema.FSR_SHARPNESS, sharpness);

        if (on(files, capabilities, RENDER_SCALE_TXT)) {
            int index = parseRenderScale(content(files, RENDER_SCALE_TXT));
            if (index >= 0) settings = supported(settings, capabilities, SettingsSchema.RENDER_SCALE, index);
        }

        // Frame generation as the game decides it: the between file wins over the ahead file, and neither makes a
        // picture unless the pacer's 30 fps cap is the cap in force. In a build whose row is not live this stays off.
        if (cap == SettingsSchema.CAP_30 && on(files, capabilities, PACE_30)) {
            int generation = on(files, capabilities, FG_BETWEEN) ? SettingsSchema.FG_BETWEEN
                    : on(files, capabilities, FG_AHEAD) ? SettingsSchema.FG_AHEAD : SettingsSchema.FG_OFF;
            settings = supported(settings, capabilities, SettingsSchema.FRAME_GENERATION, generation);
        } else if (cap == SettingsSchema.CAP_AUTO && capabilities.generationUnderAutomatic() && on(files, capabilities, FG_BETWEEN)) {
            // The one other case, in a build that announces it: the between file under the automatic cap (which is
            // the cap in force only by its own file). The lower-range file is not asked: it widens that cap's range
            // (pace_caps.h) and does not decide the picture; drift names it when it does not stand as plan would have it.
            settings = supported(settings, capabilities, SettingsSchema.FRAME_GENERATION, SettingsSchema.FG_BETWEEN);
        }

        settings = supported(settings, capabilities, SettingsSchema.FPS_OVERLAY, on(files, capabilities, FPS_OVERLAY) ? 1 : 0);

        if (on(files, capabilities, WORLD_LOAD_TXT)) {
            int[] world = parseWorld(content(files, WORLD_LOAD_TXT));
            for (int i = 0; i < WORLD_KEYS.length; i++) {
                settings = supported(settings, capabilities, WORLD_KEYS[i], world[i]);
            }
        }

        // the input switch is read by the app, not by the game library
        settings = supported(settings, capabilities, SettingsSchema.INPUT_UNBUFFERED, files.exists(INPUT_UNBUFFERED) ? 1 : 0);
        return settings;
    }

    /**
     * The files whose present state differs from plan(settings), in the order of the fixed list. Empty right
     * after a save; not empty when something else (a measurement script, a different game build) changed a file.
     */
    static List<String> drift(Settings settings, Capabilities capabilities, FileView files) throws IOException {
        ArrayList<String> differing = new ArrayList<String>();
        for (Map.Entry<String, byte[]> entry : plan(settings, capabilities, files).entrySet()) {
            byte[] want = entry.getValue();
            boolean exists = files.exists(entry.getKey());
            boolean same = want == null ? !exists
                    : exists && (want.length == 0 || Arrays.equals(want, content(files, entry.getKey())));
            if (!same) differing.add(entry.getKey());
        }
        return Collections.unmodifiableList(differing);
    }

    /**
     * The scaler the game uses while scaler.txt holds no valid word, from the switch files the build reads, with the
     * game's precedence (direct_present.h): GSR 1 switch, then FSR 1, then cubic, then linear, else nearest.
     * A build that reads no scaler switch at all: FSR 1, the row's default (nothing can be told from files).
     * withGsrSwitch false leaves the GSR 1 switch out (importFrom, for a build whose row cannot offer GSR 1).
     */
    private static int switchScaler(FileView files, Capabilities capabilities, boolean withGsrSwitch) {
        if (!capabilities.reads(SCALE_FSR1) && !capabilities.reads(SCALE_CUBIC) && !capabilities.reads(GSR_SWITCH)) {
            return SettingsSchema.SCALER_FSR1;
        }
        return withGsrSwitch && on(files, capabilities, GSR_SWITCH) ? SettingsSchema.SCALER_GSR1
                : on(files, capabilities, SCALE_FSR1) ? SettingsSchema.SCALER_FSR1
                : on(files, capabilities, SCALE_CUBIC) ? SettingsSchema.SCALER_BICUBIC
                : on(files, capabilities, SCALE_LINEAR) ? SCALER_LINEAR : SettingsSchema.SCALER_NEAREST;
    }

    /** The word of scaler.txt for a row value. */
    private static String scalerWord(int scaler) {
        return scaler == SettingsSchema.SCALER_BICUBIC ? "cubic" : scaler == SettingsSchema.SCALER_NEAREST ? "nearest"
                : scaler == SettingsSchema.SCALER_GSR1 ? "gsr1" : "fsr1";
    }

    /**
     * The sharpness (tenths of a stop) the game uses while fsr-sharpness.txt is absent: the default without
     * diag-v450-fsr1-stops (or in a build that does not read it); that file's number when it is an exact tenth in
     * the plain form this code can read; -1 when the file is there in any other form (the game reads it with scanf,
     * which accepts more than this code does: then only a value file makes the result certain).
     */
    private static int olderStops(FileView files, Capabilities capabilities) {
        if (!on(files, capabilities, FSR1_STOPS)) return SettingsSchema.SHARPNESS_DEFAULT;
        return parseStopsTenths(content(files, FSR1_STOPS), true);
    }

    // ---- helpers

    private static byte[] text(String line) {
        return (line + "\n").getBytes(StandardCharsets.US_ASCII);
    }

    /** The file's first bytes, or null when it cannot be read (for example because a folder stands under that name). */
    private static byte[] content(FileView files, String path) {
        try {
            return files.read(path);
        } catch (IOException unreadable) {
            return null;
        }
    }

    /** The file exists and the installed build reads it. */
    private static boolean on(FileView files, Capabilities capabilities, String path) {
        return capabilities.reads(path) && files.exists(path);
    }

    /** settings with key = value when the build supports that value; otherwise unchanged. */
    private static Settings supported(Settings settings, Capabilities capabilities, String key, int value) {
        return capabilities.valueSupported(key, value) ? settings.with(key, value) : settings;
    }

    /**
     * The file's text without the blanks, tabs and line ends around it. null when the file is maxBytes long or longer,
     * or holds a byte that is neither printable ASCII nor one of those four blank characters.
     */
    private static String trimmed(byte[] raw, int maxBytes) {
        if (raw == null || raw.length >= maxBytes) return null;
        for (int i = 0; i < raw.length; i++) {
            if ((raw[i] < 0x20 || raw[i] > 0x7E) && !blank(raw[i])) return null;
        }
        return new String(raw, StandardCharsets.US_ASCII).trim();
    }

    private static boolean blank(byte b) {
        return b == ' ' || b == '\t' || b == '\n' || b == '\r';
    }

    /**
     * scaler.txt as lane fsr-sharpness reads it: one of four words, letters in any case, blanks around it ignored,
     * shorter than 64 bytes. Returns the scaler value, or -1 for anything else (the game then uses its switch files).
     */
    private static int parseScalerWord(byte[] raw) {
        String word = trimmed(raw, 64);
        if (word == null) return -1;
        word = lowerAscii(word);
        if (word.equals("fsr1")) return SettingsSchema.SCALER_FSR1;
        if (word.equals("cubic")) return SettingsSchema.SCALER_BICUBIC;
        if (word.equals("nearest")) return SettingsSchema.SCALER_NEAREST;
        if (word.equals("gsr1")) return SettingsSchema.SCALER_GSR1;
        return -1;
    }

    private static String lowerAscii(String text) {
        StringBuilder out = new StringBuilder(text.length());
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            out.append(c >= 'A' && c <= 'Z' ? (char) (c - 'A' + 'a') : c);
        }
        return out.toString();
    }

    /**
     * A sharpness file as scaler_settings.h reads it: digits, optionally '.' and more digits, value 0 to 4, blanks
     * around it ignored, shorter than 64 bytes, at most 15 digits. Returns the value in tenths rounded to the nearest
     * tenth (halves up), or -1. With exactOnly a number that is not a whole count of tenths (0.25) is -1 as well.
     */
    private static int parseStopsTenths(byte[] raw, boolean exactOnly) {
        String number = trimmed(raw, 64);
        if (number == null) return -1;
        long mantissa = 0, scale = 1;
        boolean dot = false;
        int digits = 0;
        for (int i = 0; i < number.length(); i++) {
            char c = number.charAt(i);
            if (c == '.') {
                if (dot) return -1;
                dot = true;
                continue;
            }
            if (c < '0' || c > '9' || ++digits > 15) return -1;
            mantissa = mantissa * 10 + (c - '0');
            if (dot) scale *= 10;
        }
        if (digits == 0 || mantissa > 4 * scale) return -1;
        if (exactOnly && mantissa * 10 % scale != 0) return -1;
        return (int) ((mantissa * 10 + scale / 2) / scale);
    }

    /**
     * render-scale.txt as render_scale.h reads it: one to four digits, optionally followed by '%', blanks around
     * both ignored, at most 63 bytes, 50 to 200. The game rounds the percentage to sixteenths of 1280x720; this
     * returns the choice value that gives the same sixteenths, or -1 when the text is not valid for the game or
     * no offered step gives that size.
     */
    private static int parseRenderScale(byte[] raw) {
        String percent = trimmed(raw, 64);
        if (percent == null) return -1;
        if (percent.endsWith("%")) percent = percent.substring(0, percent.length() - 1).trim();
        if (percent.isEmpty() || percent.length() > 4) return -1;
        for (int i = 0; i < percent.length(); i++) {
            if (percent.charAt(i) < '0' || percent.charAt(i) > '9') return -1;
        }
        int value = Integer.parseInt(percent);
        if (value < 50 || value > 200) return -1;
        String[] ids = SettingsSchema.spec(SettingsSchema.RENDER_SCALE).choiceIds;
        for (int i = 0; i < ids.length; i++) {
            if (sixteenths(Integer.parseInt(ids[i])) == sixteenths(value)) return i;
        }
        return -1;
    }

    /** render_scale.h sixteenthsOf: round to the nearest sixteenth, halves up. */
    private static int sixteenths(int percent) {
        return (percent * 16 + 50) / 100;
    }

    /**
     * world-load.txt as world_load.h reads it: key=N tokens separated by blanks or line ends, N one to three
     * digits, 0 to 100. A slider that is not named, or whose value is not such a number, is at 100; a name given twice:
     * the last one counts; other tokens are ignored; a file longer than 512 bytes counts as absent. A value below the
     * game's floor (25 for the densities, 70 for draw distance) is raised to it, as the game does. Returns the five
     * percentages in slider order, each moved to the slider's nearest step of 5.
     */
    private static int[] parseWorld(byte[] raw) {
        int[] values = {100, 100, 100, 100, 100};
        if (raw == null || raw.length > 512) return values;
        int i = 0;
        while (i < raw.length) {
            while (i < raw.length && blank(raw[i])) i++;
            int start = i;
            while (i < raw.length && !blank(raw[i])) i++;
            if (i == start) break;
            int equals = start;
            while (equals < i && raw[equals] != '=') equals++;
            if (equals == i) continue;
            String name = new String(raw, start, equals - start, StandardCharsets.ISO_8859_1);
            int which = -1;
            for (int slider = 0; slider < WORLD_SPELLINGS.length; slider++) {
                if (name.equals(WORLD_SPELLINGS[slider][0]) || name.equals(WORLD_SPELLINGS[slider][1])) which = slider;
            }
            if (which < 0) continue;
            int digits = i - equals - 1, percent = 0;
            boolean number = digits >= 1 && digits <= 3;
            for (int k = equals + 1; number && k < i; k++) {
                if (raw[k] < '0' || raw[k] > '9') number = false;
                else percent = percent * 10 + (raw[k] - '0');
            }
            int floor = which == WORLD_LOD ? SettingsSchema.WORLD_DRAW_DISTANCE_MIN : SettingsSchema.WORLD_DENSITY_MIN;
            values[which] = number && percent <= 100 ? Math.max(floor, (percent + 2) / 5 * 5) : 100;
        }
        return values;
    }
}
