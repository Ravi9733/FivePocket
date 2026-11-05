package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_A;
import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_B;
import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.difference;
import static dev.recompbootstrap.smoke.LauncherTestSupport.discard;
import static dev.recompbootstrap.smoke.LauncherTestSupport.equal;
import static dev.recompbootstrap.smoke.LauncherTestSupport.newFilesDir;
import static dev.recompbootstrap.smoke.LauncherTestSupport.privateTrace;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;
import static dev.recompbootstrap.smoke.LauncherTestSupport.snapshot;
import static dev.recompbootstrap.smoke.LauncherTestSupport.write;

import java.io.File;
import java.nio.file.Files;
import java.util.Map;

/**
 * Host test of LaunchOutcome: what the launcher concludes, on its return, from what the game window left behind
 * for the run it started. Run with java -ea. Every read goes through readUnchanged, which proves that looking
 * created, changed and removed nothing.
 */
public final class LaunchOutcomeTest {
    private static final LaunchOutcome.Exit ORDINARY = LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY;

    public static void main(String[] args) {
        LauncherTestSupport.requireAssertions();

        run("LaunchOutcome.aStartTheGameWindowNeverAcceptedIsNotAccepted", () -> {
            File files = newFilesDir();
            write(new File(privateTrace(files), "run-" + ID_A + "/arguments.bin"), new byte[] {1});
            // The game window creates used-(id) before it loads anything. Without it the start was never accepted.
            equal(LaunchOutcome.Kind.NOT_ACCEPTED, readUnchanged(files, ID_A, ORDINARY, -1).kind, "no marker");
            // Another run's marker is not this run's.
            write(new File(privateTrace(files), "used-" + ID_B), new byte[0]);
            equal(LaunchOutcome.Kind.NOT_ACCEPTED, readUnchanged(files, ID_A, ORDINARY, -1).kind, "another run's marker");
            // Not accepted is said even when Android also reports a crash: the game was never loaded.
            equal(LaunchOutcome.Kind.NOT_ACCEPTED, readUnchanged(files, ID_A, LaunchOutcome.Exit.CRASH, 500).kind, "no marker and a crash");
            write(new File(privateTrace(files), "used-" + ID_A), new byte[0]);
            LaunchOutcome accepted = readUnchanged(files, ID_A, ORDINARY, -1);
            equal(LaunchOutcome.Kind.NOTHING_KNOWN, accepted.kind, "with the marker nothing is known to be wrong");
            equal("", accepted.word, "no word");
            // The game window refuses a request whose marker name is taken by anything at all, also by a link.
            check(new File(privateTrace(files), "used-" + ID_A).delete(), "remove the marker");
            Files.createSymbolicLink(new File(privateTrace(files), "used-" + ID_A).toPath(), new File(files, "nowhere").toPath());
            equal(LaunchOutcome.Kind.NOTHING_KNOWN, readUnchanged(files, ID_A, ORDINARY, -1).kind, "a link with the marker's name");
            discard(files);
        });

        run("LaunchOutcome.somethingThatIsNotARunIdIsNeverLookedUp", () -> {
            File files = newFilesDir();
            write(new File(files, "used-x"), new byte[0]);
            for (String id : new String[] {null, "", "x", "../../used-x", ID_A.substring(1), ID_A.toUpperCase(), ID_A + "/.."}) {
                equal(LaunchOutcome.Kind.NOTHING_KNOWN, readUnchanged(files, id, LaunchOutcome.Exit.CRASH, 1).kind, "id <" + id + ">");
            }
            discard(files);
        });

        run("LaunchOutcome.howTheProcessEndedIsReportedForAnAcceptedStart", () -> {
            File files = newFilesDir();
            write(new File(privateTrace(files), "used-" + ID_A), new byte[0]);
            LaunchOutcome crash = readUnchanged(files, ID_A, LaunchOutcome.Exit.CRASH, 754_000);
            equal(LaunchOutcome.Kind.CRASHED, crash.kind, "crash");
            equal(754_000L, crash.ranMs, "how long the game ran");
            equal(LaunchOutcome.Kind.LOW_MEMORY, readUnchanged(files, ID_A, LaunchOutcome.Exit.LOW_MEMORY, 10).kind, "low memory");
            equal(LaunchOutcome.Kind.NOT_RESPONDING, readUnchanged(files, ID_A, LaunchOutcome.Exit.NOT_RESPONDING, 10).kind, "not responding");
            LaunchOutcome left = readUnchanged(files, ID_A, ORDINARY, 99);
            equal(LaunchOutcome.Kind.NOTHING_KNOWN, left.kind, "the game was left");
            equal(-1L, left.ranMs, "no time is claimed for an ordinary end");
            discard(files);
        });

        run("LaunchOutcome.androidsExitReasonsAreTakenAsCrashLowMemoryNotRespondingOrOrdinary", () -> {
            // The numbers are android.app.ApplicationExitInfo's (LauncherContractTest compares them with android.jar).
            equal(LaunchOutcome.Exit.CRASH, LaunchOutcome.exitFor(4), "REASON_CRASH");
            equal(LaunchOutcome.Exit.CRASH, LaunchOutcome.exitFor(5), "REASON_CRASH_NATIVE");
            equal(LaunchOutcome.Exit.LOW_MEMORY, LaunchOutcome.exitFor(3), "REASON_LOW_MEMORY");
            equal(LaunchOutcome.Exit.NOT_RESPONDING, LaunchOutcome.exitFor(6), "REASON_ANR");
            // 2 is REASON_SIGNALED: how the game window ends its own process every time it is left.
            for (int ordinary : new int[] {0, 1, 2, 7, 8, 9, 10, 11, 12, 13, 14, -1, 99}) {
                equal(LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, LaunchOutcome.exitFor(ordinary), "reason " + ordinary);
            }
        });

        run("LaunchOutcome.theGameWindowsRefusalWordIsReadFromTheRunsDirectory", () -> {
            File files = newFilesDir();
            write(new File(privateTrace(files), "used-" + ID_A), new byte[0]);
            File refusal = new File(privateTrace(files), "run-" + ID_A + "/" + LaunchOutcome.REFUSAL_FILE);
            for (String word : new String[] {"REJECT_NATIVE_RUN", "REJECT_LIBRARY_6", "REJECT_RUN_SETUP_3", "REJECT_IO_IOException"}) {
                write(refusal, word + "\n");
                LaunchOutcome refused = readUnchanged(files, ID_A, LaunchOutcome.Exit.CRASH, 5);
                equal(LaunchOutcome.Kind.REFUSED, refused.kind, "refused with " + word + " (said before a crash)");
                equal(word, refused.word, "the word");
            }
            write(refusal, "REJECT_HASH\nsecond line\n");
            equal("REJECT_HASH", readUnchanged(files, ID_A, ORDINARY, -1).word, "only the first line");
            write(refusal, "  REJECT_SURFACE  ");
            equal("REJECT_SURFACE", readUnchanged(files, ID_A, ORDINARY, -1).word, "no line end, spaces round it");
            // Anything else in that file is not a refusal word and is not shown to anyone.
            StringBuilder longWord = new StringBuilder("REJECT_");
            while (longWord.length() <= 64) longWord.append('X');
            for (String other : new String[] {"", "\n", "RUNNING_GUEST\n", "reject_hash\n", "REJECT HASH\n", "REJECT_<b>\n",
                    "REJECT_é\n", longWord + "\n"}) {
                write(refusal, other.getBytes("ISO-8859-1"));
                equal(LaunchOutcome.Kind.NOTHING_KNOWN, readUnchanged(files, ID_A, ORDINARY, -1).kind, "content <" + other.trim() + ">");
            }
            write(refusal, longWord.substring(0, 64) + "\n");
            equal(LaunchOutcome.Kind.REFUSED, readUnchanged(files, ID_A, ORDINARY, -1).kind, "a word of 64 characters");
            // Not a regular file: a directory, and a link to a file that holds a word.
            check(refusal.delete() && refusal.mkdir(), "a directory with that name");
            equal(LaunchOutcome.Kind.NOTHING_KNOWN, readUnchanged(files, ID_A, ORDINARY, -1).kind, "a directory");
            check(refusal.delete(), "remove it");
            Files.createSymbolicLink(refusal.toPath(), write(new File(files, "elsewhere.txt"), "REJECT_HASH\n").toPath());
            equal(LaunchOutcome.Kind.NOTHING_KNOWN, readUnchanged(files, ID_A, ORDINARY, -1).kind, "a link");
            discard(files);
        });

        run("LaunchOutcome.aReturnIsJudgedOnlyAfterTheGameWindowHadTimeToAcceptTheRequest", () -> {
            check(!LaunchOutcome.settled(0) && !LaunchOutcome.settled(2999), "sooner than three seconds: not judged");
            check(LaunchOutcome.settled(3000) && LaunchOutcome.settled(3_600_000), "from three seconds on: judged");
            check(!LaunchOutcome.settled(-5), "a clock that went backwards: not judged");
        });

        run("LaunchOutcome.eachStartIsLookedAtOnceAndNotBeforeItHasSettled", () -> {
            LaunchOutcome.Watch watch = new LaunchOutcome.Watch();
            equal(null, watch.due(1_000_000), "no start was made: nothing is due");
            watch.started(ID_A, 50_000, 1_700_000_000_000L);
            equal(1_700_000_000_000L, watch.startedWallMs(), "the start's wall-clock time");
            equal(null, watch.due(50_000 + 2_999), "a return sooner than three seconds after the start is not judged");
            equal(ID_A, watch.due(50_000 + 3_000), "the look is left for the next return, which is late enough");
            equal(null, watch.due(50_000 + 9_000), "a start is looked at once");
            // A second start replaces the first, also one that was never looked at.
            watch.started(ID_A, 60_000, 1L);
            watch.started(ID_B, 70_000, 2L);
            equal(ID_B, watch.due(80_000), "the newest start is the one looked at");
            equal(2L, watch.startedWallMs(), "with its own time");
            equal(null, watch.due(90_000), "and nothing is left of the earlier one");
            // A start whose run is not known is not watched.
            watch.started(null, 100_000, 3L);
            equal(null, watch.due(200_000), "no run, no look");
        });

        run("LaunchOutcome.aStartLeftTooSoonSaysWhenItCanBeJudged", () -> {
            // A refusal the player leaves at once brings the launcher back before the start has settled. The screen
            // then asks how long to wait before it looks again.
            LaunchOutcome.Watch watch = new LaunchOutcome.Watch();
            equal(-1L, watch.settlesIn(1_000_000), "no start was made: nothing to wait for");
            watch.started(ID_A, 50_000, 1L);
            equal(3_000L, watch.settlesIn(50_000), "at the start: the whole settling time");
            equal(1L, watch.settlesIn(50_000 + 2_999), "one millisecond short");
            equal(null, watch.due(50_000 + 2_999), "(and the start is not handed out yet)");
            equal(0L, watch.settlesIn(50_000 + 3_000), "settled: no wait");
            equal(0L, watch.settlesIn(50_000 + 60_000), "long settled: no wait, never a negative one");
            equal(ID_A, watch.due(50_000 + 60_000), "(the start is handed out)");
            equal(-1L, watch.settlesIn(50_000 + 60_001), "a start that was looked at is not waited for");
            watch.started(null, 70_000, 2L);
            equal(-1L, watch.settlesIn(70_000), "a start whose run is not known is not waited for");
        });

        LauncherTestSupport.finish("LaunchOutcomeTest");
    }

    /** read, with the proof that it created, changed and removed nothing. */
    private static LaunchOutcome readUnchanged(File files, String id, LaunchOutcome.Exit exit, long ranMs) throws Exception {
        Map<String, String> before = snapshot(files.getParentFile(), true);
        LaunchOutcome outcome = LaunchOutcome.read(files, id, exit, ranMs);
        equal("", difference(before, snapshot(files.getParentFile(), true)), "read changed the directory tree");
        check(outcome != null && outcome.kind != null && outcome.word != null, "an answer is always given");
        return outcome;
    }
}
