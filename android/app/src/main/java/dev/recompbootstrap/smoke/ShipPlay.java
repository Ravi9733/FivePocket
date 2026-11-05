package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;

/**
 * What Play does before the game window opens, in a build that carries its game code. Pure Java.
 *
 * A developer build (no game code inside the APK) prepares the start exactly as before. A build with game code
 * inside adds rules, all about what is started, never about convenience:
 *   - while the bundled game code is not in place (the first screen did not finish, or an update is pending) Play
 *     refuses with a reason and creates nothing;
 *   - while a pinned game data file is absent or is not the pinned file (ShipReady: sizes of all of them, SHA-256 of
 *     the bundled pieces and of the files up to 64 MiB) Play refuses with a reason and creates nothing. The start
 *     request alone does not show this: the boot modules lie in a directory it names, not in its list;
 *   - the start request Play copies must be this app version's: the launcher's own look (InstallState) must have
 *     taken the bundled run, not fallen back to another one;
 *   - the shader pack is compared with its recorded SHA-256 at every Play, as the game code is;
 *   - "start anyway" does not exist: game code that is not the bundled one is never started. When a piece the app
 *     carries is found changed, the mark of the install is removed, so that the next start of the app checks every
 *     piece and puts the right one back.
 * When the app's own file cannot be found the answer is a refusal, never "this must be a developer build".
 */
final class ShipPlay {
    /** Play's reason when the app does not know where its own APK file is (ShipApplication did not run). */
    static final String NO_APP_FILE = "this app cannot find its own file; install the app again";
    /** Play's reason when the app's own APK file is known but cannot be opened; the cause follows in brackets. */
    static final String APP_FILE_UNREADABLE_PREFIX = "this app's own file cannot be read";
    /** Followed by ShipData.counted() without its full stop, for example "18 of 39 game data files are missing". */
    static final String DATA_MISSING_PREFIX = "your game data is not complete: ";
    /** Play's reason when the launcher would copy another start request than the one this app version carries. */
    static final String OTHER_REQUEST = "the start settings on this device are not the ones this app version carries; close"
            + " the app and open it again to put them back";

    static final class Result {
        /** null on success; otherwise one line for the launcher. */
        final String error;
        /** The prepared start; null when error is not null. */
        final LaunchRequest.Prepared request;

        private Result(String error, LaunchRequest.Prepared request) {
            this.error = error;
            this.request = request;
        }
    }

    private ShipPlay() {}

    /**
     * True for a reason of LaunchRequest.prepare that says one of the app's own files is missing or cannot be used.
     * False for the two that are about the device at this moment (a folder that could not be made, a read or write
     * that failed): for those the files may be fine, and Play again is the next step.
     */
    static boolean fileNotUsable(String reason) {
        return reason != null && !reason.equals(LaunchRequest.ERROR_SETTINGS_DIRECTORY) && !reason.startsWith(LaunchRequest.ERROR_IO_PREFIX)
                && !reason.startsWith("Android would not open");
    }

    /**
     * apkPath: the app's own APK file. null means "not known", and then Play refuses: the rules below cannot be
     * asked, and starting without them would be starting unchecked code in a build that may carry its own.
     */
    static Result prepare(String apkPath, File filesDir, boolean acceptUnqualified) {
        if (apkPath == null) return new Result(NO_APP_FILE, null);
        ShipBundle bundle;
        try {
            bundle = PublicGameCode.open(apkPath, filesDir);
        } catch (IOException | RuntimeException unreadable) {
            return new Result(APP_FILE_UNREADABLE_PREFIX + " (" + ShipFiles.reason(unreadable) + ")", null);
        }
        try {
            if (bundle instanceof PublicGameCode) {
                String problem = PublicGameCode.installedProblem(InstallGuard.root(filesDir), ((PublicGameCode) bundle).config);
                if (problem != null) return new Result(problem, null);
            }
            return prepare(bundle, filesDir, acceptUnqualified);
        } finally {
            try {
                bundle.close();
            } catch (IOException ignored) {
                // it was only read
            }
        }
    }

    /** bundle null: a build that is known to carry no game code (a developer build). */
    static Result prepare(ShipBundle bundle, File filesDir, boolean acceptUnqualified) {
        long started = System.nanoTime();
        boolean[] carried = {false};
        Result result = prepareUntimed(bundle, filesDir, acceptUnqualified, carried);
        // Play's own whole read of the two files, in a build that carries them: its time, for the log (tag RecompShip).
        if (carried[0]) {
            ShipVerify.line("VERIFY_PLAY ms=" + java.util.concurrent.TimeUnit.NANOSECONDS.toMillis(System.nanoTime() - started)
                    + " result=" + (result.error == null ? "prepared" : "refused"));
        }
        return result;
    }

    /** carried[0] is set when the build carries its game code (a developer build writes no VERIFY_ line). */
    private static Result prepareUntimed(ShipBundle bundle, File filesDir, boolean acceptUnqualified, boolean[] carried) {
        // Without the two large files' content: both are hashed below, on the way to the start (ShipReady.ofSizes).
        ShipReady ready = ShipReady.ofSizes(bundle, filesDir);
        carried[0] = ready.bundled();
        switch (ready.stands) {
            case NOT_BUNDLED: {
                LaunchRequest.Prepared prepared = LaunchRequest.prepare(filesDir, acceptUnqualified);
                return prepared.error != null ? new Result(prepared.error, null) : new Result(null, prepared);
            }
            case NOTHING:
                break;
            case PIECE:
                ShipFirstRun.forget(filesDir);   // the next start of the app puts the bundled piece back
                return new Result(ShipFirstRun.PIECE_CHANGED, null);
            case DATA: {
                String sentence = ready.data.counted();
                return new Result(DATA_MISSING_PREFIX + sentence.substring(0, sentence.length() - 1), null);
            }
            case APP_FILE:
                return new Result(NO_APP_FILE, null);
            default:
                return new Result(ShipFirstRun.NOT_INSTALLED, null);
        }
        ShipConfig config = ready.config;
        // The shader pack: the game code is hashed below by LaunchRequest, the pack here (73 MB, well under a second).
        File pack = new File(new File(filesDir, InstallState.PRIVATE_TRACE), config.shaderPackName);
        boolean packIsBundled;
        try {
            packIsBundled = config.shaderPack.sha256.equals(ShipFiles.sha256Of(pack));
        } catch (IOException | RuntimeException unreadable) {
            packIsBundled = false;
        }
        if (!packIsBundled) {
            ShipFirstRun.forget(filesDir);
            return new Result(ShipFirstRun.PIECE_CHANGED, null);
        }
        // The start request the launcher will copy must be the bundled one. ShipReady has compared its bytes; this
        // asks the launcher's own detection which run it takes, because that is the code that does the copying.
        InstallState install = InstallState.detect(filesDir);
        if (install.ready() && (!config.runId.equals(install.templateRunId) || install.pinIgnored)) {
            ShipFirstRun.forget(filesDir);
            return new Result(OTHER_REQUEST, null);
        }
        LaunchRequest.Prepared prepared = LaunchRequest.prepare(filesDir, false);
        if (LaunchRequest.UNQUALIFIED.equals(prepared.error)) {
            ShipFirstRun.forget(filesDir);
            return new Result(ShipFirstRun.CODE_CHANGED, null);
        }
        if (prepared.error != null && fileNotUsable(prepared.error)) {
            // One of the app's own files is missing or cannot be used although the cheap look found everything in
            // place. The mark goes, so that the menu's next look offers Finish Setting Up, which checks every piece
            // by its content: the way a player can take with the controller (closing the app cannot be done with it).
            ShipFirstRun.forget(filesDir);
        }
        return prepared.error != null ? new Result(prepared.error, null) : new Result(null, prepared);
    }
}
