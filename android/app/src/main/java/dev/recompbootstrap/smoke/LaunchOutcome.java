package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;

/**
 * What became of a start the launcher made, found by looking at what the game window left behind. Pure Java;
 * read() never writes and never throws. The words for each answer are LaunchAdvice.afterStart's.
 *
 * The game window runs in its own process and shows a refusal only in its own window (a status word such as
 * "REJECT LIBRARY 6"), then ends its process when it is left. The launcher is told nothing, so on its return it
 * looks for three things, in this order:
 *
 *   1. the marker private-trace/used-(id). The game window creates it before it loads anything, as the proof that
 *      it accepted the request (private_trace_jni.cpp, claimRequest). No marker: the start was never accepted.
 *   2. the file private-trace/run-(id)/start-refused.txt: the status word with which the game window refused a
 *      start it had accepted. GameWindowStatus.report writes it (LaunchOutcomeTest runs writer and reader
 *      together); the game window must hand its status words to that method (LauncherContractTest says whether
 *      the PrivateTraceActivity of this tree does; one that does not leaves step 2 with nothing to find).
 *   2b. the file private-trace/run-(id)/game-stopped.txt, which the same writer leaves when the game code returned
 *      by itself (the game stopped; the player did not leave it).
 *   3. how Android says the game's process ended (crash, low memory, not responding), which the screen asks
 *      Android for and passes in as exit.
 */
final class LaunchOutcome {
    enum Kind { NOTHING_KNOWN, NOT_ACCEPTED, REFUSED, STOPPED, CRASHED, LOW_MEMORY, NOT_RESPONDING }

    /** How the game's process ended, as far as the launcher needs to tell apart. */
    enum Exit { UNKNOWN_OR_ORDINARY, CRASH, LOW_MEMORY, NOT_RESPONDING }

    static final String CLAIM_PREFIX = "used-";
    static final String REFUSAL_FILE = "start-refused.txt";
    /** Written by GameWindowStatus.report when the game code returned by itself: the game stopped, nobody left it. */
    static final String STOPPED_FILE = "game-stopped.txt";
    /** The game window claims a request while it is created; a return sooner than this is not judged yet. */
    static final long SETTLE_MS = 3000;
    /** The longest status word read from the refusal file; GameWindowStatus.report writes no longer one. */
    static final int MAXIMUM_WORD = 64;

    final Kind kind;
    /** For REFUSED: the game window's status word, for example "REJECT_NATIVE_RUN". Otherwise "". */
    final String word;
    /** How long the game ran before its process ended, in milliseconds; -1 when that is not known. */
    final long ranMs;

    private LaunchOutcome(Kind kind, String word, long ranMs) {
        this.kind = kind;
        this.word = word;
        this.ranMs = ranMs;
    }

    /**
     * runId: the run that was started (LaunchRequest.newestLauncherRun right after the prepare).
     * exit and ranMs: what Android reports about the game process's end since that start; UNKNOWN_OR_ORDINARY and
     * -1 when it reports nothing (or the Android version cannot be asked).
     */
    static LaunchOutcome read(File filesDir, String runId, Exit exit, long ranMs) {
        if (!LaunchRequest.isId(runId)) {
            return new LaunchOutcome(Kind.NOTHING_KNOWN, "", -1);
        }
        File root = new File(filesDir, InstallState.PRIVATE_TRACE);
        if (!exists(new File(root, CLAIM_PREFIX + runId))) {
            return new LaunchOutcome(Kind.NOT_ACCEPTED, "", -1);
        }
        String word = refusalWord(new File(new File(root, "run-" + runId), REFUSAL_FILE));
        if (word != null) {
            return new LaunchOutcome(Kind.REFUSED, word, -1);
        }
        // The game code returned by itself and the window said so: that is what happened, whatever ended the process
        // afterwards (the window ends its own process when it is left).
        if (regularFile(new File(new File(root, "run-" + runId), STOPPED_FILE))) {
            return new LaunchOutcome(Kind.STOPPED, "", -1);
        }
        switch (exit) {
            case CRASH:
                return new LaunchOutcome(Kind.CRASHED, "", ranMs);
            case LOW_MEMORY:
                return new LaunchOutcome(Kind.LOW_MEMORY, "", ranMs);
            case NOT_RESPONDING:
                return new LaunchOutcome(Kind.NOT_RESPONDING, "", ranMs);
            default:
                return new LaunchOutcome(Kind.NOTHING_KNOWN, "", -1);
        }
    }

    /**
     * The values of android.app.ApplicationExitInfo.getReason() that say the game did not end by being left
     * (LauncherContractTest compares them with Android's own constants). Every other reason is ordinary: the game
     * window ends its own process whenever it is left.
     */
    static final int ANDROID_REASON_LOW_MEMORY = 3, ANDROID_REASON_CRASH = 4, ANDROID_REASON_CRASH_NATIVE = 5,
            ANDROID_REASON_ANR = 6;

    static Exit exitFor(int androidReason) {
        switch (androidReason) {
            case ANDROID_REASON_CRASH:
            case ANDROID_REASON_CRASH_NATIVE:
                return Exit.CRASH;
            case ANDROID_REASON_LOW_MEMORY:
                return Exit.LOW_MEMORY;
            case ANDROID_REASON_ANR:
                return Exit.NOT_RESPONDING;
            default:
                return Exit.UNKNOWN_OR_ORDINARY;
        }
    }

    /**
     * True when a return to the launcher this long after the start can be judged: the game window has had the time
     * to claim the request. (A return sooner than that is another event, such as the screen being turned.)
     */
    static boolean settled(long sinceStartMs) {
        return sinceStartMs >= SETTLE_MS;
    }

    /**
     * The one start whose outcome has not been looked at yet. The screen tells it of every start it makes and asks
     * it on every return whether a look is due; it answers each start once, and not before the start has settled.
     */
    static final class Watch {
        private String run;
        private long startedUptimeMs, startedWallMs;

        /** A start was made for runId (null when the run is not known: nothing is watched then). */
        void started(String runId, long uptimeMs, long wallMs) {
            run = runId;
            startedUptimeMs = uptimeMs;
            startedWallMs = wallMs;
        }

        /**
         * The run to look at now, or null: when no start waits, or when this return came too soon after the start
         * (the look is then left for the next return). A run is handed out once.
         */
        String due(long uptimeMs) {
            if (run == null || !settled(uptimeMs - startedUptimeMs)) {
                return null;
            }
            String answer = run;
            run = null;
            return answer;
        }

        /**
         * How long from now until the waiting start can be judged, in milliseconds: 0 when due() would hand it out
         * now, -1 when no start waits. A return that came too soon (the player left a refusal at once) is looked
         * at again after this long, if the launcher is then still in front.
         */
        long settlesIn(long uptimeMs) {
            if (run == null) {
                return -1;
            }
            return Math.max(0, SETTLE_MS - (uptimeMs - startedUptimeMs));
        }

        /** The wall-clock time of the last start told to started(), for comparing with Android's record of the process's end. */
        long startedWallMs() {
            return startedWallMs;
        }
    }

    /** The first line of the refusal file when it is one status word that begins with "REJECT_"; else null. */
    private static String refusalWord(File file) {
        try {
            if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                return null;
            }
            String text = new String(LaunchRequest.readBounded(file, 256), StandardCharsets.US_ASCII);
            int end = text.indexOf('\n');
            String word = (end < 0 ? text : text.substring(0, end)).trim();
            if (!word.startsWith("REJECT_") || word.length() > MAXIMUM_WORD) {
                return null;
            }
            for (int at = 0; at < word.length(); ++at) {
                char c = word.charAt(at);
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                    return null;
                }
            }
            return word;
        } catch (IOException | RuntimeException unreadable) {
            return null;
        }
    }

    private static boolean regularFile(File file) {
        try {
            return Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS);
        } catch (RuntimeException invalid) {
            return false;
        }
    }

    private static boolean exists(File file) {
        try {
            return Files.exists(file.toPath(), LinkOption.NOFOLLOW_LINKS);
        } catch (RuntimeException invalid) {
            return false;
        }
    }
}
