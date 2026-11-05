package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.List;

/**
 * The lines the first screen writes to the log (tag RecompShip). Pure Java, so that the exact text is host tested.
 *
 * The shipped build is not debuggable: nobody can list the app's storage from outside. These lines are how an
 * install is checked on a device (VM-TEST.md), and tools/expected_log.py derives the same lines from the staged
 * record, so that a test run can be compared with what must appear. Every line names files of the app's own layout
 * and SHA-256 prefixes only; nothing here is game content.
 */
final class ShipLog {
    static final String TAG = "RecompShip";

    private ShipLog() {}

    private static String identity(ShipConfig config) {
        return "build=" + config.build + " record=" + config.sha256.substring(0, 12) + " code="
                + config.library.sha256.substring(0, 12) + " shaders=" + config.shaderPack.sha256.substring(0, 12);
    }

    /**
     * After ShipFirstRun.run: one "PIECE (place) bytes=(n) sha256=(12 digits) copied|kept" per bundled piece it
     * looked at, then "FIRST_RUN ok|failed changed=(n) pieces=(n) build=... record=... code=... shaders=...
     * [error=...]".
     */
    static List<String> firstRun(ShipFirstRun.Result result) {
        List<String> lines = new ArrayList<String>();
        for (String piece : result.report) lines.add("PIECE " + piece);
        lines.add("FIRST_RUN " + (result.ok() ? "ok" : "failed") + " changed=" + result.changed.size() + " pieces="
                + result.report.size() + (result.config == null ? "" : " " + identity(result.config))
                + (result.error == null ? "" : " error=" + result.error));
        return lines;
    }

    /** On a start that found the bundled game code in place: "CODE in place build=... pieces=(n)". */
    static String codeInPlace(ShipConfig config) {
        return "CODE in place " + identity(config) + " pieces=" + (3 + config.bundledData.size());
    }

    /**
     * What is in place of the pinned game data: "GAME_DATA in place|needed: (sentence) pinned=(n) missing=(n)
     * missing_bytes=(n) bundled=(n) from_discs=(n)", then one "MISSING (path) bytes=(n) sha256=(12 digits)" per
     * pinned file that is absent and one "DIFFERENT ..." per pinned file that is there but is not the pinned one.
     */
    static List<String> gameData(ShipConfig config, ShipData data) {
        List<String> lines = new ArrayList<String>();
        String sentence = data.sentence();
        lines.add("GAME_DATA " + (sentence == null ? "in place" : "needed: " + sentence) + " pinned=" + data.pinned + " missing="
                + data.missing() + " missing_bytes=" + data.missingBytes() + " bundled=" + config.bundledData.size()
                + " from_discs=" + config.notBundled.size());
        for (ShipConfig.DataFile file : data.absent) lines.add(file("MISSING", file));
        for (ShipConfig.DataFile file : data.different) lines.add(file("DIFFERENT", file));
        return lines;
    }

    /**
     * The frame-rate counter's switch file was removed on this start: the install's settings were never saved, and
     * the counter is off by default (ShipFirstRun.counterOffUnlessSaved). Written only when a file was removed.
     */
    static String counterOff() {
        return "COUNTER_OFF removed=" + ShipFirstRun.COUNTER_SWITCH + " why=settings-never-saved";
    }

    /** A killed copy of game data had left half files in the app's storage; they were removed. */
    static String leftOvers(long bytesFreed) {
        return "LEFT_OVERS removed bytes=" + bytesFreed;
    }

    /**
     * A usual start found a piece the app carries absent or with other bytes; the full work follows and puts it
     * back: "PIECE_CHANGED count=(n) first=(path)".
     */
    static String pieceChanged(ShipConfig config, ShipData data) {
        return pieceChanged(config, data, java.util.Collections.<String>emptyList());
    }

    /**
     * The same when the game code file or the shader file is among them (changedCode: ShipFirstRun.changedCode, the
     * places relative to the files directory): they count, and the first of them is the one named.
     */
    static String pieceChanged(ShipConfig config, ShipData data, List<String> changedCode) {
        String first = changedCode.isEmpty() ? "" : changedCode.get(0);
        for (ShipConfig.DataFile file : data.absent) if (first.isEmpty() && config.bundled(file.path) != null) first = file.path;
        for (ShipConfig.DataFile file : data.different) if (first.isEmpty() && config.bundled(file.path) != null) first = file.path;
        return "PIECE_CHANGED count=" + (data.bundledAmongThem + changedCode.size()) + " first=" + first;
    }

    private static String file(String word, ShipConfig.DataFile file) {
        return word + " " + file.path + " bytes=" + file.bytes + " sha256=" + file.sha256.substring(0, 12);
    }

    /** After ShipDataImport.run; missing is ShipDataImport.missing() afterwards. */
    static String dataImport(ShipDataImport.Outcome result, String missing) {
        return "DATA_IMPORT " + (result.complete ? "complete" : result.cancelled ? "stopped" : "failed") + " files=" + result.files
                + " bytes=" + result.bytes + " copied=" + result.copied + (result.error == null ? "" : " error=" + result.error)
                + (missing == null ? "" : " missing=" + missing);
    }
}
