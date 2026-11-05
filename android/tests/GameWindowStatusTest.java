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
import java.util.Map;

/**
 * Host test of GameWindowStatus: what the game window shows for its status words in a start the launcher made, and
 * the refusal it leaves for the launcher. The refusal is followed from the writer through the launcher's own reader
 * (LaunchOutcome.read) to the sentence on the Play page (LaunchAdvice.afterStart): the two ends are tested connected,
 * not each against a file written by the test. Run with java -ea.
 */
public final class GameWindowStatusTest {
    private static final LaunchOutcome.Exit ORDINARY = LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY;
    /** One word of every family PrivateTraceActivity can refuse with after it accepted a request. */
    private static final String[] REFUSALS = {"REJECT_PROCESS", "REJECT_SURFACE", "REJECT_LIBRARY_6", "REJECT_SIZE", "REJECT_HASH",
            "REJECT_MANIFEST_FILE", "REJECT_MANIFEST_SIZE", "REJECT_MANIFEST_HASH", "REJECT_NATIVE_RUN", "REJECT_RUN_SETUP_3",
            "REJECT_EMPTY_PROTOCOL", "REJECT_IO_FileNotFoundException"};
    /** The status words of a start that is under way: nothing is reported for them. */
    private static final String[] NOT_REPORTED = {"CHECKING_PRIVATE_LIBRARY", "WAITING_FOR_NATIVE_SWAP_SURFACE", "RUNNING_GUEST"};

    public static void main(String[] args) {
        LauncherTestSupport.requireAssertions();

        run("GameWindowStatus.nothingIsDrawnOverTheGameWhileItIsCheckedWaitedForOrRunning", () -> {
            for (String word : new String[] {"CHECKING_PRIVATE_LIBRARY", "WAITING_FOR_NATIVE_SWAP_SURFACE", "RUNNING_GUEST"}) {
                equal(null, GameWindowStatus.label(word), "no label for " + word);
            }
        });

        run("GameWindowStatus.aRefusalAndAStopAreSaidInPlainWordsWithTheWayBack", () -> {
            for (String word : REFUSALS) {
                equal("The game did not start.\nPress B or Android's Back to return to the menu.", GameWindowStatus.label(word), word);
            }
            equal("The game has stopped.\nPress B or Android's Back to return to the menu.",
                    GameWindowStatus.label("TRACE_RETURNED_PROTOCOL_SAVED"), "the game code returned");
            // Whatever is shown names the way back and carries none of the game window's developer words.
            for (String word : new String[] {"REJECT_HASH", "REJECT_IO_IOException", "TRACE_RETURNED_PROTOCOL_SAVED"}) {
                String label = GameWindowStatus.label(word);
                check(label.endsWith(GameWindowStatus.TEXT_RETURN), "the way back ends the label of " + word);
                check(!label.contains("REJECT") && !label.contains("Development") && !label.contains("unverified")
                        && !label.contains("protocol"), "plain words only: " + label);
            }
        });

        run("GameWindowStatus.aRefusalOfAnAcceptedStartReachesThePlayPageAsASentence", () -> {
            for (String word : REFUSALS) {
                File files = accepted(ID_A);
                check(GameWindowStatus.report(files, ID_A, word), "the game window reports " + word);
                // The launcher's own reader, then the launcher's own words.
                LaunchOutcome outcome = LaunchOutcome.read(files, ID_A, ORDINARY, -1);
                equal(LaunchOutcome.Kind.REFUSED, outcome.kind, "the launcher finds the refusal " + word);
                equal(word, outcome.word, "with the game window's word");
                String said = LaunchAdvice.afterStart(outcome);
                check(said != null && said.startsWith("The game did not start: ") && said.endsWith(" (" + word + ")"),
                        "the Play page says it, with the word for support: " + said);
                discard(files);
            }
        });

        run("GameWindowStatus.aWordTheReaderWouldNotAcceptIsWrittenSoThatItDoes", () -> {
            File files = accepted(ID_A);
            StringBuilder longName = new StringBuilder("REJECT_IO_");
            while (longName.length() < 90) longName.append("Exception");
            check(GameWindowStatus.report(files, ID_A, longName.toString()), "a word of " + longName.length() + " characters");
            LaunchOutcome cut = LaunchOutcome.read(files, ID_A, ORDINARY, -1);
            equal(LaunchOutcome.Kind.REFUSED, cut.kind, "is still a refusal for the launcher");
            equal(longName.substring(0, 64), cut.word, "cut to the reader's 64 characters");
            check(GameWindowStatus.report(files, ID_A, "REJECT_IO_Outer$Inner Exception"), "a class name with other characters");
            equal("REJECT_IO_Outer_Inner_Exception", LaunchOutcome.read(files, ID_A, ORDINARY, -1).word, "each replaced by _");
            discard(files);
        });

        run("GameWindowStatus.onlyARefusalOfARequestThisWindowAcceptedIsWritten", () -> {
            File files = accepted(ID_A);
            write(new File(privateTrace(files), "run-x/arguments.bin"), new byte[] {1});   // a directory whose name holds no run id
            Map<String, String> before = snapshot(files.getParentFile(), true);
            // A status word that is neither a refusal nor a stop: the game is being checked, waited for or running.
            for (String word : NOT_REPORTED) {
                check(!GameWindowStatus.report(files, ID_A, word), word + " is not reported");
            }
            check(!GameWindowStatus.report(files, ID_A, null), "no word");
            // No accepted launcher request: a script's start, or a refusal before the claim.
            check(!GameWindowStatus.report(files, null, "REJECT_HASH"), "no request id: nothing is written");
            // Something that is not a run id is never turned into a path, also when such a directory exists.
            for (String id : new String[] {"x", "", ID_A.substring(1), ID_A.toUpperCase(), ID_A + "/../run-x"}) {
                check(!GameWindowStatus.report(files, id, "REJECT_HASH"), "id <" + id + ">");
            }
            // A run whose directory is not there (the launcher always makes it): nothing is created for it.
            check(!GameWindowStatus.report(files, ID_B, "REJECT_HASH"), "a run without a directory");
            equal("", difference(before, snapshot(files.getParentFile(), true)), "none of these wrote, created or removed anything");
            equal(LaunchOutcome.Kind.NOTHING_KNOWN, LaunchOutcome.read(files, ID_A, ORDINARY, -1).kind, "and the launcher finds no refusal");
            discard(files);
        });

        run("GameWindowStatus.aGameThatStoppedByItselfReachesThePlayPageAsASentence", () -> {
            File files = accepted(ID_A);
            check(GameWindowStatus.report(files, ID_A, "TRACE_RETURNED_PROTOCOL_SAVED"), "the game window reports that the game code returned");
            check(!new File(privateTrace(files), "run-" + ID_A + "/start-refused.txt").exists(), "it is not written as a refusal");
            LaunchOutcome outcome = LaunchOutcome.read(files, ID_A, ORDINARY, -1);
            equal(LaunchOutcome.Kind.STOPPED, outcome.kind, "the launcher finds the stop");
            equal("The game stopped by itself. Choose Play to start it again.", LaunchAdvice.afterStart(outcome), "the developer build's Play page says it");
            equal("The game stopped by itself. Choose Play to start it again.", ShipAdvice.afterStart(outcome), "and so does the handed-out build's");
            // The window ends its own process when it is left: however Android then says the process ended, it was a stop.
            equal(LaunchOutcome.Kind.STOPPED, LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.CRASH, 5000).kind, "a stop stays a stop");
            // A refusal of the same start is said first: the game never ran.
            check(GameWindowStatus.report(files, ID_A, "REJECT_HASH"), "(a refusal is reported for the same run)");
            equal(LaunchOutcome.Kind.REFUSED, LaunchOutcome.read(files, ID_A, ORDINARY, -1).kind, "the refusal is what the launcher says");
            discard(files);
            // A script's start, or a start that was never accepted: nothing is written for a stop either.
            File script = accepted(ID_A);
            Map<String, String> before = snapshot(script.getParentFile(), true);
            check(!GameWindowStatus.report(script, null, "TRACE_RETURNED_PROTOCOL_SAVED"), "no accepted launcher request: not reported");
            equal("", difference(before, snapshot(script.getParentFile(), true)), "nothing was written");
            discard(script);
        });

        run("GameWindowStatus.theWindowIsPlainInEveryBuildAndOnlyTheDebugBuildsSwitchFileBringsTheLabelBack", () -> {
            File files = newFilesDir();
            // No switch file: plain in both builds (the lead's decision of 2026-10-06: the owner plays on the debug build).
            check(GameWindowStatus.plainWindow(false, files), "a build that is not debuggable: a plain window, whoever made the start");
            check(GameWindowStatus.plainWindow(true, files), "the debug build without its switch file: a plain window too");
            // A file of another name in the same folder is not the switch.
            write(new File(privateTrace(files), "diag-window-banner.off"), new byte[0]);
            check(GameWindowStatus.plainWindow(true, files), "a file of another name is not the switch");
            // The switch file itself, where the game window looks for it: files/private-trace/diag-window-banner.
            write(new File(privateTrace(files), "diag-window-banner"), new byte[0]);
            check(!GameWindowStatus.plainWindow(true, files), "the debug build with the switch file draws its developer label");
            check(GameWindowStatus.plainWindow(false, files), "the build that is handed out stays plain with the same file in place");
            discard(files);
            // The switch lies in files/private-trace, not beside it.
            File other = newFilesDir();
            write(new File(other, "diag-window-banner"), new byte[0]);
            check(GameWindowStatus.plainWindow(true, other), "a file of that name outside files/private-trace is not the switch");
            discard(other);
        });

        LauncherTestSupport.finish("GameWindowStatusTest");
    }

    /** A files directory in which the launcher made the run and the game window claimed it. */
    private static File accepted(String id) throws Exception {
        File files = newFilesDir();
        write(new File(privateTrace(files), "run-" + id + "/arguments.bin"), new byte[] {1});
        write(new File(privateTrace(files), "used-" + id), new byte[0]);
        return files;
    }
}
