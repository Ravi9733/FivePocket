package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * What the app's first screen does when the app is opened, in the order it does it. Pure Java: ShipStartActivity
 * calls run() off the UI thread, draws what Watch is told, writes Outcome.log to the log and opens the menu.
 *
 *   - A build without game code inside (a developer build): nothing, the menu opens at once.
 *   - While the game is running: nothing is touched. When the app's pieces are in place the menu opens; when they
 *     are not, the screen says to close the game first.
 *   - The usual start: the mark of this app version is there and every piece has its place and size. Then the
 *     pinned game data is looked at once (this hashes the small pieces the app carries), and the game code file
 *     and the shader file are hashed. When one of the app's own pieces is found changed, it is not left for the
 *     player to discover: the mark is removed and the full work runs, which puts the right piece back.
 *   - A first start, an update, an interrupted run, or a start the menu asked for ("Finish Setting Up", force):
 *     ShipFirstRun.run checks every piece by its SHA-256 and copies what is missing or changed.
 * Nothing here asks the player anything. The game data from the player's discs is the menu's business.
 *
 * The log lines (tag RecompShip; the build is not debuggable, so they are how an install is checked from outside):
 * a first start writes ShipLog.firstRun's lines, a later start "CODE in place ..."; then "COUNTER_OFF removed=..."
 * when the frame-rate counter's switch file was removed (settings never saved: the counter is off by default); then
 * "LEFT_OVERS removed bytes=(n)" when a killed copy had left half files; then ShipLog.gameData's lines.
 */
final class ShipStart {
    static final String GAME_RUNNING = "The game is running. Close the game, then try again: the app still has to finish"
            + " setting itself up.";

    /** Told while run() works; called on the worker thread. */
    interface Watch extends ShipFirstRun.Progress {
        /** The full work starts now (it copies or hashes several hundred megabytes): show progress. */
        void working();
    }

    static final class Outcome {
        /** False when the APK carries no game code; nothing was done then. */
        final boolean bundled;
        /** null when the app's pieces are in place; otherwise one or two plain sentences. */
        final String error;
        /** The bundled record; null when there is none or it could not be read. */
        final ShipConfig config;
        /** What is in place of the pinned game data after the work; null when it was not looked at. */
        final ShipData data;
        /** The lines for the log, in order. */
        final List<String> log;

        private Outcome(boolean bundled, String error, ShipConfig config, ShipData data, List<String> log) {
            this.bundled = bundled;
            this.error = error;
            this.config = config;
            this.data = data;
            this.log = Collections.unmodifiableList(log);
        }

        /** True when the menu can follow: a developer build, or the app's own pieces are in place. */
        boolean ok() {
            return error == null;
        }
    }

    private ShipStart() {}

    /**
     * force: the menu's "Finish Setting Up" opened the screen: every piece is checked by its SHA-256 again, whatever
     * the mark says. gameRunning: the game's process exists right now. Never throws.
     */
    static Outcome run(ShipBundle bundle, File filesDir, boolean force, boolean gameRunning, Watch watch, ShipFiles.Space space) {
        List<String> log = new ArrayList<String>();
        ShipConfig config = null;
        try {
            ShipFirstRun.State state = ShipFirstRun.check(bundle, filesDir);
            if (state == ShipFirstRun.State.NOT_BUNDLED) {
                return new Outcome(false, null, null, null, log);
            }
            if (gameRunning) {
                // The game holds the game code and reads the game data: nothing is checked, copied or removed now.
                if (state == ShipFirstRun.State.CURRENT && !force) {
                    return new Outcome(true, null, ShipFirstRun.config(bundle), null, log);
                }
                return new Outcome(true, GAME_RUNNING, null, null, log);
            }
            if (state == ShipFirstRun.State.CURRENT && !force) {
                config = ShipFirstRun.config(bundle);
                ShipData data = ShipData.look(config, filesDir);
                // The whole read of the game code file and the shader file: one VERIFY_CODE line with its time, and the
                // menu's first look answers from it instead of reading the 412 MB a second time.
                List<String> changedCode = ShipVerify.read(config, filesDir, ShipVerify.WHY_START);
                if (data.bundledAmongThem == 0 && changedCode.isEmpty()) {
                    log.add(ShipLog.codeInPlace(config));
                    // An app update that carries the same record does no full work: the counter's default is made to hold here.
                    if (ShipFirstRun.counterOffUnlessSaved(filesDir)) log.add(ShipLog.counterOff());
                    long freed = ShipFirstRun.removeCopyLeftOvers(filesDir);
                    if (freed > 0) log.add(ShipLog.leftOvers(freed));
                    log.addAll(ShipLog.gameData(config, data));
                    return new Outcome(true, null, config, data, log);
                }
                // A piece the app carries is absent or has other bytes: the full work below puts it back.
                log.add(ShipLog.pieceChanged(config, data, changedCode));
                force = true;
            }
            if (force) {
                ShipFirstRun.forget(filesDir);
            }
            watch.working();
            ShipFirstRun.Result result = ShipFirstRun.run(bundle, filesDir, watch, space);
            log.addAll(ShipLog.firstRun(result));
            if (!result.ok()) {
                return new Outcome(true, result.error == null ? "Setting up stopped without saying why." : result.error,
                        result.config, null, log);
            }
            config = result.config;
            if (result.changed.contains("-" + ShipFirstRun.COUNTER_SWITCH)) log.add(ShipLog.counterOff());
            if (result.leftOversFreed > 0) log.add(ShipLog.leftOvers(result.leftOversFreed));
            ShipData data = ShipData.look(config, filesDir);
            log.addAll(ShipLog.gameData(config, data));
            return new Outcome(true, null, config, data, log);
        } catch (IOException | RuntimeException e) {
            return new Outcome(true, "This app's own file cannot be read: " + ShipFiles.reason(e) + ". Install the app again.",
                    config, null, log);
        }
    }
}
