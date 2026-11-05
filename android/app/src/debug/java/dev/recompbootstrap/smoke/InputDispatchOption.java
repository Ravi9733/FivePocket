package dev.recompbootstrap.smoke;

import java.io.File;

/**
 * The "low-delay stick input" option of the game activity. Pure Java: nothing from android.* is used here, so the
 * host test (android/tests/InputDispatchOptionTest.java) runs it on a plain JDK.
 *
 * What it controls: Android holds a controller's stick, trigger and HAT (D-pad as an axis) changes - joystick
 * MotionEvents - back until the window's next frame callback and then delivers them in one batch. Button presses
 * (KeyEvents) are never held. With this option the game activity asks its window for unbuffered joystick dispatch
 * (View.requestUnbufferedDispatch(int), Android 11 / API 30 and newer), so each change is delivered when it arrives.
 * The values the game receives are the same; only the moment of delivery changes.
 *
 * How it is switched: the empty file files/private-trace/diag-lt2-unbuffered. The settings screen creates or removes
 * it; the game activity looks for it once when a run starts. File absent = no request, no log line, the behaviour of
 * the app before this option existed. Nothing here polls, writes, or reads a file's content.
 */
final class InputDispatchOption {
    /** Name of the switch file. Its existence is the whole setting; its content is never read. */
    static final String SWITCH_FILE = "diag-lt2-unbuffered";
    /** Directory of the switch file below the app's files directory. */
    static final String SWITCH_DIRECTORY = "private-trace";
    /** First Android version that has View.requestUnbufferedDispatch(int source): Android 11. */
    static final int MIN_SDK = 30;
    /** Logged once, under LOG_TAG, when the request is made. The device test plan looks for exactly this text. */
    static final String LOG_TAG = "RecompNativeInput";
    static final String LOG_LINE = "INPUT_UNBUFFERED on=1";

    /** The launcher's own list of the run ids it made (written by LaunchRequest), below the files directory. */
    static final String LAUNCHER_RUNS = "settings/launcher-runs.txt";
    /** Logged under the game activity's tag when a used launcher request closes the activity. */
    static final String CLOSED_LINE = "CLOSED_USED_LAUNCHER_REQUEST";

    /**
     * True when the game activity must close itself instead of showing "REJECT USED REQUEST": the request id was
     * refused as already used (claimResult 1) AND it is one the launcher made (it is a line of
     * files/settings/launcher-runs.txt). That happens when the user leaves a game started with Play (Home, or the
     * screen going to sleep) and comes back: Android starts the game activity again with the old request. A request
     * made by a measurement script is never on that list, so for script starts this is always false and the refusal
     * screen is shown as before. Reads one small file; never writes; never throws.
     */
    static boolean closesUsedRequest(int claimResult, File filesDirectory, String requestId) {
        if (claimResult != 1 || requestId == null || requestId.length() != 32) return false;
        try {
            File list = new File(filesDirectory, LAUNCHER_RUNS);
            if (!list.isFile() || list.length() > (1 << 20)) return false;
            byte[] bytes = java.nio.file.Files.readAllBytes(list.toPath());
            for (String line : new String(bytes, java.nio.charset.StandardCharsets.US_ASCII).split("\n")) {
                if (line.trim().equals(requestId)) return true;
            }
        } catch (java.io.IOException | RuntimeException unreadable) {
            // not known to be the launcher's: the refusal screen is shown
        }
        return false;
    }

    private final boolean wanted;
    private boolean taken;

    private InputDispatchOption(boolean wanted) {
        this.wanted = wanted;
    }

    /**
     * True when privateTraceDirectory/diag-lt2-unbuffered exists. Like the game runtime's own switch files
     * (access(path, F_OK)), anything of that name counts, whatever it contains. A missing directory or one that may
     * not be searched gives false. Never throws. (The activity always passes a directory; java.io.File would read a
     * null directory as the process's current directory.)
     */
    static boolean requested(File privateTraceDirectory) {
        return new File(privateTraceDirectory, SWITCH_FILE).exists();
    }

    /** The one decision: only for game runs, only when requested, only on API 30 or newer. */
    static boolean shouldRequest(boolean gameRequested, boolean switchPresent, int sdkInt) {
        return gameRequested && switchPresent && sdkInt >= MIN_SDK;
    }

    /**
     * Reads the switch once for a run that was just accepted. filesDirectory is Context.getFilesDir(). The result
     * never looks at the file system again: creating or removing the file later has no effect on a running game.
     */
    static InputDispatchOption forRun(boolean gameRequested, File filesDirectory, int sdkInt) {
        boolean switchPresent = requested(new File(filesDirectory, SWITCH_DIRECTORY));
        return new InputDispatchOption(shouldRequest(gameRequested, switchPresent, sdkInt));
    }

    /**
     * Call on every window focus change. Returns true exactly once: at the first focus gain of a run that wants
     * unbuffered dispatch. The caller then makes the request and writes LOG_LINE. Every other call returns false.
     * (The first focus gain is used because the window's root view is attached to its window by then, so the request
     * reaches the window's input receiver directly.)
     */
    boolean takeRequest(boolean focused) {
        if (!focused || !wanted || taken) return false;
        taken = true;
        return true;
    }
}
