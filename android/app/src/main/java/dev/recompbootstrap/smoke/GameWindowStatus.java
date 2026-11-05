package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/**
 * What the game window shows and leaves behind. Pure Java: the game window (PrivateTraceActivity, debug source
 * set) hands every status word it has to label() and report(), asks plainWindow() once, and decides nothing of this
 * itself, so the rules run in a host test.
 *
 * The game window knows its state as a status word: "CHECKING_PRIVATE_LIBRARY", "WAITING_FOR_NATIVE_SWAP_SURFACE",
 * "RUNNING_GUEST", "TRACE_RETURNED_PROTOCOL_SAVED" (the game code returned: the game has stopped), or a refusal, which
 * begins with "REJECT_".
 *
 * Which window draws what (the lead's decision of 2026-10-06, which replaces the product's rule D for the debug build):
 *   - The game window is plain in every build, whoever made the start (Play, or the test harness's private replay
 *     start). While the game is being checked, waited for or running, nothing is drawn over the picture: no
 *     developer label and no grey "CHECKING PRIVATE LIBRARY" page. A refusal or a stop is said in two plain lines,
 *     with the way back to the menu. The owner plays on the debug build, and the harness's screenshots must show
 *     the game's picture alone.
 *   - The developer label stays available in the debug build behind a switch file, files/private-trace/
 *     diag-window-banner, looked for once when the window starts. The build that is handed out (not debuggable)
 *     never looks for it and never draws the label.
 *   - In both, a refusal or a stop of a start the launcher made and the window had accepted is written to the run's
 *     directory, where the launcher looks for it on its return (LaunchOutcome.read) and says it in a sentence.
 */
final class GameWindowStatus {
    private GameWindowStatus() {}

    static final String REFUSAL_PREFIX = "REJECT_";
    static final String WORD_STOPPED = "TRACE_RETURNED_PROTOCOL_SAVED";
    static final String TEXT_DID_NOT_START = "The game did not start.";
    static final String TEXT_STOPPED = "The game has stopped.";
    static final String TEXT_RETURN = "Press B or Android's Back to return to the menu.";

    /** The switch file (in files/private-trace) that brings the developer label back in the debug build. */
    static final String BANNER_SWITCH = "diag-window-banner";

    /**
     * True when the game window draws no developer label at all and shows label() instead: always in a build that
     * is not debuggable, and in the debug build unless the switch file files/private-trace/diag-window-banner
     * exists. The game window asks once, when it starts, before anything is shown; this is the one place that
     * decides it. A build that is not debuggable does not look at the file at all.
     */
    static boolean plainWindow(boolean debuggable, File filesDir) {
        if (!debuggable) {
            return true;
        }
        try {
            return !new File(new File(filesDir, InstallState.PRIVATE_TRACE), BANNER_SWITCH).exists();
        } catch (RuntimeException unreadable) {
            return true;   // a switch that cannot be looked for is a switch that is off
        }
    }

    /** True for a status word that says the start was refused. */
    static boolean refusal(String word) {
        return word != null && word.startsWith(REFUSAL_PREFIX);
    }

    /**
     * The text the game window shows for a status word in a start the launcher made, or null: nothing is shown and
     * the picture is the game's alone. A text is shown only when the game is not (or no longer) running, and it
     * always ends with the way back.
     */
    static String label(String word) {
        if (refusal(word)) {
            return TEXT_DID_NOT_START + "\n" + TEXT_RETURN;
        }
        if (WORD_STOPPED.equals(word)) {
            return TEXT_STOPPED + "\n" + TEXT_RETURN;
        }
        return null;
    }

    /**
     * Leaves a refusal or a stop where the launcher reads it: files/private-trace/run-(id)/start-refused.txt for a
     * refusal, run-(id)/game-stopped.txt when the game code returned by itself; one line, the status word.
     * acceptedRequestId is the request this window accepted (its claim succeeded) when the launcher made it; null
     * otherwise, and then nothing is written: a script's run directory stays as it is, and a start that was never
     * accepted has no refusal to report (the launcher sees that from the missing claim marker). Any other word
     * (checking, waiting, running) writes nothing. The word is cut to what the reader accepts, and every character
     * the reader would not accept becomes "_". Best effort: returns false, and never throws, when it wrote nothing.
     */
    static boolean report(File filesDir, String acceptedRequestId, String word) {
        boolean stopped = WORD_STOPPED.equals(word);
        if ((!refusal(word) && !stopped) || !LaunchRequest.isId(acceptedRequestId)) {
            return false;
        }
        StringBuilder line = new StringBuilder();
        for (int at = 0; at < word.length() && at < LaunchOutcome.MAXIMUM_WORD; ++at) {
            char c = word.charAt(at);
            boolean kept = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            line.append(kept ? c : '_');
        }
        File file = new File(new File(new File(filesDir, InstallState.PRIVATE_TRACE), "run-" + acceptedRequestId),
                stopped ? LaunchOutcome.STOPPED_FILE : LaunchOutcome.REFUSAL_FILE);
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(line.append('\n').toString().getBytes(StandardCharsets.US_ASCII));
            return true;
        } catch (IOException | RuntimeException failed) {
            return false;
        }
    }
}
