package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherMenu.ABOUT;
import static dev.recompbootstrap.smoke.LauncherMenu.CONTROLLER_CHECK;
import static dev.recompbootstrap.smoke.LauncherMenu.PRIMARY;
import static dev.recompbootstrap.smoke.LauncherMenu.SETTINGS;
import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_A;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addCode;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addInput;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addRun;
import static dev.recompbootstrap.smoke.LauncherTestSupport.bytes;
import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.discard;
import static dev.recompbootstrap.smoke.LauncherTestSupport.equal;
import static dev.recompbootstrap.smoke.LauncherTestSupport.newFilesDir;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;
import static dev.recompbootstrap.smoke.LauncherTestSupport.write;

import java.io.File;
import java.util.TimeZone;

/**
 * Host test of LauncherMenu: what the launcher shows in each install state, how the focus moves, and what a confirm
 * or a tap does. Run with java -ea. The install states are real results of InstallState.detect on small temporary
 * directory trees.
 */
public final class LauncherMenuTest {
    private static final boolean GAME_RUNNING = true, GAME_NOT_RUNNING = false;

    public static void main(String[] args) throws Exception {
        LauncherTestSupport.requireAssertions();
        final InstallState ready = detected(true, true, false);
        final InstallState codeNeeded = detected(true, false, false);
        final InstallState setupNeeded = detected(false, false, false);
        final InstallState fromDiscs = detected(false, false, true);
        equal(InstallState.Problem.NONE, ready.problem, "fixture: ready");
        equal(InstallState.Problem.CODE_MISSING, codeNeeded.problem, "fixture: code missing");
        equal(InstallState.Problem.NOTHING_INSTALLED, setupNeeded.problem, "fixture: nothing installed");
        equal(InstallState.Problem.DISC_COPY_ONLY, fromDiscs.problem, "fixture: copied from discs");

        run("LauncherMenu.stateLineAndFirstItemFollowTheInstallState", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            equal(LaunchAdvice.STATE_CHECKING, menu.stateLine(), "before the first detection");
            check(!menu.enabled(PRIMARY), "the first item cannot be used before the first detection");
            equal(LauncherMenu.Action.NONE, menu.confirm(GAME_NOT_RUNNING), "confirm before the first detection");
            equal("", menu.detail(), "nothing is explained before the first detection");

            // Whatever is missing, the first item looks again; the page says what is missing and what to do.
            for (InstallState notReady : new InstallState[] {setupNeeded, codeNeeded, fromDiscs}) {
                menu.setInstall(notReady);
                equal(LaunchAdvice.stateLine(notReady), menu.stateLine(), "state line, " + notReady.problem);
                check(menu.stateLineIsWarning(), notReady.problem + " is shown as a warning");
                equal("Check again", menu.label(PRIMARY), "first item, " + notReady.problem);
                check(menu.enabled(PRIMARY), "it can be used, " + notReady.problem);
                equal(LaunchAdvice.help(notReady, true), menu.detail(), "what to do, " + notReady.problem);
            }
            check(!LaunchAdvice.stateLine(setupNeeded).equals(LaunchAdvice.stateLine(codeNeeded))
                    && !LaunchAdvice.stateLine(codeNeeded).equals(LaunchAdvice.stateLine(fromDiscs)), "each state has its own line");

            menu.setInstall(ready);
            equal("Ready.", menu.stateLine(), "ready");
            check(!menu.stateLineIsWarning(), "ready is not a warning");
            equal("Play", menu.label(PRIMARY), "first item, ready");
            equal(LaunchAdvice.help(ready, true), menu.detail(), "what Play does");

            equal("Settings", menu.label(SETTINGS), "second item");
            equal("Controller check", menu.label(CONTROLLER_CHECK), "third item");
            equal("About", menu.label(ABOUT), "fourth item");
        });

        run("LauncherMenu.checkAgainLooksAtTheFilesAndSaysWhenNothingChanged", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(codeNeeded);
            equal(null, menu.secondLine(PRIMARY), "no message after an ordinary look at the files");
            equal(LauncherMenu.Action.RECHECK, menu.confirm(GAME_NOT_RUNNING), "A on the first item");
            equal(LauncherMenu.CHECKING_AGAIN, menu.secondLine(PRIMARY), "while the look runs");
            check(!menu.preparing(), "no Play is prepared");
            // The same state comes back: say so, or the press seems to have done nothing.
            menu.setInstall(detected(true, false, false));
            equal(LaunchAdvice.CHECKED_NO_CHANGE, menu.secondLine(PRIMARY), "the same state again");
            equal(LauncherMenu.Tone.HINT, menu.secondLineTone(PRIMARY), "it is a note, not an error");
            equal(null, menu.failure(), "and not a failure");
            // A later look that was not asked for (the screen was shown again) takes the note away.
            menu.setInstall(detected(true, false, false));
            equal(null, menu.secondLine(PRIMARY), "no note after an ordinary look");
            // Another state comes back: its own line says it, no note.
            menu.confirm(GAME_NOT_RUNNING);
            menu.setInstall(setupNeeded);
            equal(null, menu.secondLine(PRIMARY), "a different state needs no note");
            equal(LaunchAdvice.stateLine(setupNeeded), menu.stateLine(), "the new state");
            // The note is given only when the page reads as before. Another line with the same advice is a change ...
            InstallState noData = LauncherTestSupport.detected(LauncherTestSupport.Install.DATA_MISSING);
            InstallState oneGone = LauncherTestSupport.detected(LauncherTestSupport.Install.DATA_ONE_FILE_GONE);
            equal(LaunchAdvice.help(noData, true), LaunchAdvice.help(oneGone, true), "fixture: the same advice");
            menu.setInstall(noData);
            menu.confirm(GAME_NOT_RUNNING);
            menu.setInstall(oneGone);
            equal(null, menu.secondLine(PRIMARY), "another state line is a change");
            // ... and so is the same line with another advice (another fault of the game code file).
            InstallState writable = LauncherTestSupport.detected(LauncherTestSupport.Install.CODE_WRITABLE);
            InstallState twoNames = LauncherTestSupport.detected(LauncherTestSupport.Install.CODE_TWO_NAMES);
            equal(LaunchAdvice.stateLine(writable), LaunchAdvice.stateLine(twoNames), "fixture: the same state line");
            menu.setInstall(writable);
            menu.confirm(GAME_NOT_RUNNING);
            menu.setInstall(twoNames);
            equal(null, menu.secondLine(PRIMARY), "another advice is a change");
            menu.confirm(GAME_NOT_RUNNING);
            menu.setInstall(LauncherTestSupport.detected(LauncherTestSupport.Install.CODE_TWO_NAMES));
            equal(LaunchAdvice.CHECKED_NO_CHANGE, menu.secondLine(PRIMARY), "the same fault again is no change");
            // The pieces arrived: Play, without a left-over message.
            equal(LauncherMenu.Action.RECHECK, menu.tap(PRIMARY, GAME_NOT_RUNNING), "a tap on it is the same");
            menu.setInstall(ready);
            equal("Play", menu.label(PRIMARY), "ready after the look");
            equal(null, menu.secondLine(PRIMARY), "no message under Play");
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "and A now starts the game");
            // The look does not ask whether the game runs: it starts nothing.
            LauncherMenu other = new LauncherMenu(true);
            other.setInstall(codeNeeded);
            equal(LauncherMenu.Action.RECHECK, other.confirm(GAME_RUNNING), "check again while the game runs");
        });

        run("LauncherMenu.focusStartsOnTheFirstItemAndWrapsOnlyOnAFreshPress", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            equal(PRIMARY, menu.focused(), "initial focus");
            check(menu.move(1, false) && menu.focused() == SETTINGS, "down");
            check(menu.move(1, false) && menu.focused() == CONTROLLER_CHECK, "down");
            check(menu.move(1, false) && menu.focused() == ABOUT, "down");
            check(!menu.move(1, true) && menu.focused() == ABOUT, "a held down stops at the last item");
            check(menu.move(1, false) && menu.focused() == PRIMARY, "a fresh down wraps to the first item");
            check(!menu.move(-1, true) && menu.focused() == PRIMARY, "a held up stops at the first item");
            check(menu.move(-1, false) && menu.focused() == ABOUT, "a fresh up wraps to the last item");
            check(menu.move(-1, true) && menu.focused() == CONTROLLER_CHECK, "a held up moves inside the list");
        });

        run("LauncherMenu.confirmGivesEachItemsAction", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(setupNeeded);
            equal(LauncherMenu.Action.RECHECK, menu.confirm(GAME_NOT_RUNNING), "nothing installed: check again");
            menu.setInstall(codeNeeded);
            equal(LauncherMenu.Action.RECHECK, menu.confirm(GAME_NOT_RUNNING), "game code missing: check again");
            menu.setInstall(fromDiscs);
            equal(LauncherMenu.Action.RECHECK, menu.confirm(GAME_NOT_RUNNING), "disc copies only: check again");
            menu.move(1, false);
            equal(LauncherMenu.Action.OPEN_SETTINGS, menu.confirm(GAME_NOT_RUNNING), "settings");
            menu.move(1, false);
            equal(LauncherMenu.Action.OPEN_CONTROLLER_CHECK, menu.confirm(GAME_NOT_RUNNING), "controller check");
            menu.move(1, false);
            equal(LauncherMenu.Action.OPEN_ABOUT, menu.confirm(GAME_NOT_RUNNING), "about");
        });

        run("LauncherMenu.playIsPreparedOnceAndThenStarted", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "first confirm");
            equal(LaunchAdvice.CHECKING_CODE, menu.secondLine(PRIMARY), "message while preparing");
            equal(LauncherMenu.Action.NONE, menu.confirm(GAME_NOT_RUNNING), "a second confirm while preparing");
            menu.move(1, false);
            equal(LauncherMenu.Action.NONE, menu.confirm(GAME_NOT_RUNNING), "no other screen opens while preparing");
            check(menu.prepared(null, GAME_NOT_RUNNING, true), "prepared without error: start the game");
            equal(null, menu.secondLine(PRIMARY), "no message after a start");
            check(!menu.prepared(null, GAME_NOT_RUNNING, true), "a second 'prepared' without a Play starts nothing");
            equal(LauncherMenu.Action.OPEN_SETTINGS, menu.confirm(GAME_NOT_RUNNING), "other items work again");
        });

        run("LauncherMenu.playIsRefusedWhileTheGameRuns", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            equal(LauncherMenu.Action.NONE, menu.confirm(GAME_RUNNING), "confirm while the game runs");
            equal(LaunchAdvice.ALREADY_RUNNING, menu.secondLine(PRIMARY), "message");
            check(!menu.preparing(), "nothing is prepared");
            menu.shown();
            equal(null, menu.secondLine(PRIMARY), "the message goes when the screen is shown again");

            // The game appeared while the request was prepared: do not start a second one.
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "confirm");
            check(!menu.prepared(null, GAME_RUNNING, true), "not started");
            equal(LaunchAdvice.ALREADY_RUNNING, menu.secondLine(PRIMARY), "message after preparing");

            // The user left the launcher while the request was prepared: do not pop a game up in front of them.
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "confirm");
            check(!menu.prepared(null, GAME_NOT_RUNNING, false), "not started when the launcher is not in front");
            menu.shown();
            equal(LauncherMenu.NOT_IN_FRONT, menu.secondLine(PRIMARY), "that message is still there on return");
            equal(LauncherMenu.NOT_IN_FRONT_DETAIL, menu.failure(), "and the right panel has the whole sentence");
            check(!menu.failureIsError(), "it is a note, not a failure");

            // "Already running" also goes when the highlight moves: it may no longer be true.
            equal(LauncherMenu.Action.NONE, menu.confirm(GAME_RUNNING), "confirm while the game runs");
            menu.move(1, false);
            menu.move(-1, false);
            equal(null, menu.secondLine(PRIMARY), "the message goes when the highlight leaves Play");
        });

        run("LauncherMenu.codeThatIsNotTheQualifiedBuildIsStartedOnlyAfterASecondExplicitConfirm", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "first confirm");
            check(!menu.acceptsUnqualified(), "the first Play does not accept unqualified code");
            check(!menu.prepared(LaunchRequest.UNQUALIFIED, GAME_NOT_RUNNING, true), "not started");
            check(menu.asking(), "the question stands");
            equal(LauncherMenu.UNQUALIFIED, menu.secondLine(PRIMARY), "short line under Play");
            equal(LauncherMenu.UNQUALIFIED_DETAIL, menu.failure(), "the whole question in the right panel");
            check(!menu.failureIsError(), "a question, not a failure");

            // B, moving away, opening another item, and the screen being shown again all answer "no".
            menu.leavePlayItem();
            check(!menu.asking() && menu.secondLine(PRIMARY) == null && menu.failure() == null, "B answers no and clears the text");
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "Play after a no is a first confirm again");
            check(!menu.acceptsUnqualified(), "and does not accept unqualified code");
            menu.prepared(LaunchRequest.UNQUALIFIED, GAME_NOT_RUNNING, true);
            menu.move(1, false);
            check(!menu.asking(), "moving the highlight answers no");
            menu.move(-1, false);
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "Play after moving away and back");
            check(!menu.acceptsUnqualified(), "is a first confirm again");
            menu.prepared(LaunchRequest.UNQUALIFIED, GAME_NOT_RUNNING, true);
            menu.shown();
            check(!menu.asking(), "the screen being shown again answers no");
            menu.confirm(GAME_NOT_RUNNING);
            menu.prepared(LaunchRequest.UNQUALIFIED, GAME_NOT_RUNNING, true);
            equal(LauncherMenu.Action.OPEN_ABOUT, menu.tap(ABOUT, GAME_NOT_RUNNING), "a tap on another item opens it");
            check(!menu.asking(), "and answers no");
            menu.tap(PRIMARY, GAME_NOT_RUNNING);
            check(!menu.acceptsUnqualified(), "Play after that is a first confirm again");

            // The second confirm, with the question standing, is the yes.
            menu.prepared(LaunchRequest.UNQUALIFIED, GAME_NOT_RUNNING, true);
            check(menu.asking(), "asked again");
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "second confirm");
            check(menu.acceptsUnqualified(), "this Play accepts unqualified code");
            check(menu.prepared(null, GAME_NOT_RUNNING, true), "and is started");
            check(!menu.acceptsUnqualified() && !menu.asking(), "the yes is used up");
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "a later Play");
            check(!menu.acceptsUnqualified(), "is checked again");

            // A user who left the launcher while the code was checked is not asked behind their back.
            check(!menu.prepared(LaunchRequest.UNQUALIFIED, GAME_NOT_RUNNING, false), "not started");
            check(!menu.asking(), "no question when the launcher is not in front");
            equal(LauncherMenu.NOT_IN_FRONT, menu.secondLine(PRIMARY), "the 'not started' note instead");
            // The game running at the second confirm: no start, and the yes is gone.
            menu.confirm(GAME_NOT_RUNNING);
            menu.prepared(LaunchRequest.UNQUALIFIED, GAME_NOT_RUNNING, true);
            equal(LauncherMenu.Action.NONE, menu.confirm(GAME_RUNNING), "the game runs at the second confirm");
            check(!menu.asking(), "the question is closed");
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "the next Play");
            check(!menu.acceptsUnqualified(), "does not carry the old yes");
        });

        run("LauncherMenu.aFailedStartIsShownUntilTheNextAttempt", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            menu.confirm(GAME_NOT_RUNNING);
            String reason = LaunchRequest.ERROR_IO_PREFIX + "IOException: No space left on device)";
            String shown = LaunchAdvice.startFailure(reason);
            check(!menu.prepared(reason, GAME_NOT_RUNNING, true), "not started");
            equal(shown, menu.secondLine(PRIMARY), "the sentence for that reason, with the next step");
            equal(LauncherMenu.Tone.ERROR, menu.secondLineTone(PRIMARY), "drawn as an error");
            equal(shown, menu.failure(), "the same text for the right panel");
            check(menu.failureIsError(), "it is a failure");
            menu.shown();
            equal(shown, menu.secondLine(PRIMARY), "kept when shown again");
            menu.setInstall(ready);
            equal(shown, menu.secondLine(PRIMARY), "kept when the files are looked at again and nothing is missing");
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "the next attempt");
            equal(null, menu.failure(), "the failure is gone during the next attempt");
            equal(LauncherMenu.Tone.HINT, menu.secondLineTone(PRIMARY), "the progress line is not an error");
            // The start failed because a piece went missing: after the next look the state line says which.
            menu.prepared(LaunchRequest.ERROR_CODE_MISSING, GAME_NOT_RUNNING, true);
            equal(LaunchAdvice.FAIL_CODE_MISSING, menu.failure(), "the failure names the missing piece");
            menu.setInstall(codeNeeded);
            equal(null, menu.secondLine(PRIMARY), "no Play message under 'Check again'");
            equal(null, menu.failure(), "and none in the right panel");
            // What the screen reports itself (Android would not open the game window) is shown the same way.
            menu.setInstall(ready);
            menu.confirm(GAME_NOT_RUNNING);
            check(menu.prepared(null, GAME_NOT_RUNNING, true), "prepared");
            menu.startFailed(LaunchAdvice.cannotOpen("game window", "ActivityNotFoundException"));
            check(menu.failure().contains("Android would not open the game window (ActivityNotFoundException)"),
                    "what happened: " + menu.failure());
            check(menu.failure().contains("Choose Play again."), "and the next step: " + menu.failure());
        });

        run("LauncherMenu.whatBecameOfAStartIsSaidWhenItIsKnownToHaveFailed", () -> {
            File files = newFilesDir();
            LaunchOutcome notAccepted = LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1);
            write(new File(files, "private-trace/used-" + ID_A), new byte[0]);
            LaunchOutcome left = LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1);
            LaunchOutcome crashed = LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.CRASH, 42_000);
            discard(files);
            equal(LaunchOutcome.Kind.NOT_ACCEPTED, notAccepted.kind, "fixture: not accepted");
            equal(LaunchOutcome.Kind.NOTHING_KNOWN, left.kind, "fixture: the player left the game");
            equal(LaunchOutcome.Kind.CRASHED, crashed.kind, "fixture: crashed");

            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            menu.confirm(GAME_NOT_RUNNING);
            check(menu.prepared(null, GAME_NOT_RUNNING, true), "started");
            menu.shown();
            menu.startOutcome(left);
            equal(null, menu.secondLine(PRIMARY), "the player left the game: nothing is said");
            menu.startOutcome(null);
            equal(null, menu.secondLine(PRIMARY), "no look was made: nothing is said");
            menu.startOutcome(crashed);
            equal(LaunchAdvice.afterStart(crashed), menu.failure(), "a crash is said, with how long the game ran");
            check(menu.failureIsError() && menu.secondLineTone(PRIMARY) == LauncherMenu.Tone.ERROR, "as an error");
            menu.shown();
            equal(LaunchAdvice.afterStart(crashed), menu.failure(), "it stays when the screen is shown again");
            equal(LauncherMenu.Action.PLAY, menu.confirm(GAME_NOT_RUNNING), "the next Play");
            equal(null, menu.failure(), "takes it away");
            // An answer that arrives while another Play is under way, or while a question stands, is not shown.
            menu.startOutcome(notAccepted);
            equal(LaunchAdvice.CHECKING_CODE, menu.secondLine(PRIMARY), "the progress line stays during a preparation");
            menu.prepared(LaunchRequest.UNQUALIFIED, GAME_NOT_RUNNING, true);
            menu.startOutcome(notAccepted);
            check(menu.asking() && LauncherMenu.UNQUALIFIED_DETAIL.equals(menu.failure()), "the question stays");
            menu.leavePlayItem();
            menu.startOutcome(notAccepted);
            equal(LaunchAdvice.OUTCOME_NOT_ACCEPTED, menu.failure(), "a refused start is said");
            // With a piece missing the state line speaks; an old start is not mentioned under "Check again".
            menu.setInstall(codeNeeded);
            menu.startOutcome(crashed);
            equal(null, menu.secondLine(PRIMARY), "nothing about a start while the game code is missing");
            LauncherMenu early = new LauncherMenu(true);
            early.startOutcome(crashed);
            equal(null, early.secondLine(PRIMARY), "nothing before the first look at the files");
        });

        run("LauncherMenu.withoutTheLaunchHelperPlayAndControllerCheckAreDisabledWithAReason", () -> {
            LauncherMenu menu = new LauncherMenu(false);
            menu.setInstall(ready);
            check(!menu.enabled(PRIMARY) && !menu.enabled(CONTROLLER_CHECK), "both disabled");
            check(menu.enabled(SETTINGS) && menu.enabled(ABOUT), "the others are not");
            equal(LaunchAdvice.NO_GAME_WINDOW, menu.secondLine(PRIMARY), "reason under Play");
            equal(LaunchAdvice.NO_GAME_WINDOW, menu.secondLine(CONTROLLER_CHECK), "reason under Controller check");
            equal(LaunchAdvice.HELP_NO_GAME_WINDOW, menu.detail(), "the page says why");
            check(LaunchAdvice.NO_GAME_WINDOW.endsWith(LaunchAdvice.KIT_APP), "and the reason under the item ends with what to do about it");
            equal(LauncherMenu.Action.NONE, menu.confirm(GAME_NOT_RUNNING), "confirm on disabled Play");
            check(!menu.preparing(), "nothing is prepared");
            check(menu.move(1, false) && menu.move(1, false) && menu.focused() == CONTROLLER_CHECK,
                    "a disabled item keeps its place and takes the focus");
            equal(LauncherMenu.Action.NONE, menu.confirm(GAME_NOT_RUNNING), "confirm on disabled Controller check");
            equal(LauncherMenu.Action.NONE, menu.tap(PRIMARY, GAME_NOT_RUNNING), "tap on disabled Play");
            // Looking at the files again does not need the helper.
            menu.setInstall(setupNeeded);
            check(menu.enabled(PRIMARY), "'Check again' is enabled");
            equal(null, menu.secondLine(PRIMARY), "no reason under it");
            equal(LauncherMenu.Action.RECHECK, menu.confirm(GAME_NOT_RUNNING), "it looks again");
            equal(LaunchAdvice.help(setupNeeded, false), menu.detail(), "and the page says what the install kit does");
        });

        run("LauncherMenu.controllerCheckIsRefusedWhileTheGameRuns", () -> {
            // The controller check shares the game's process; opening it would disturb a running game.
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            equal(LauncherMenu.Action.NONE, menu.tap(CONTROLLER_CHECK, GAME_RUNNING), "tap while the game runs");
            equal(LaunchAdvice.CLOSE_GAME_FIRST, menu.secondLine(CONTROLLER_CHECK), "message");
            menu.shown();
            equal(null, menu.secondLine(CONTROLLER_CHECK), "the message goes when the screen is shown again");
            equal(LauncherMenu.Action.OPEN_CONTROLLER_CHECK, menu.tap(CONTROLLER_CHECK, GAME_NOT_RUNNING), "tap");
            menu.controllerCheckFailed(LaunchAdvice.cannotOpen("controller check", "SecurityException"));
            String failure = menu.secondLine(CONTROLLER_CHECK);
            check(failure.startsWith("Android would not open the controller check (SecurityException).")
                    && failure.endsWith(LaunchAdvice.KIT_APP), "a failure to open is shown with the next step: " + failure);
        });

        run("LauncherMenu.aTapFocusesTheItemAndActivatesIt", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            equal(LauncherMenu.Action.OPEN_ABOUT, menu.tap(ABOUT, GAME_NOT_RUNNING), "tap About");
            equal(ABOUT, menu.focused(), "focus follows the tap");
            equal(LauncherMenu.Action.PLAY, menu.tap(PRIMARY, GAME_NOT_RUNNING), "tap Play");
            equal(PRIMARY, menu.focused(), "focus follows the tap");
        });

        run("LauncherMenu.rightPanelFacts", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            TimeZone utc = TimeZone.getTimeZone("UTC");
            equal("Game data: checking…", menu.dataFact(), "data before detection");
            menu.setInstall(setupNeeded);
            equal("Game data: missing", menu.dataFact(), "data missing");
            equal("Game code: missing", menu.codeFact(utc), "code missing");
            menu.setInstall(detectedWith(Fault.A_GAME_FILE_GONE));
            equal("Game data: incomplete", menu.dataFact(), "one of the named files is gone");
            menu.setInstall(detectedWith(Fault.CODE_WRITABLE));
            equal("Game code: cannot be used", menu.codeFact(utc), "a game code file the game window would refuse");
            equal("Game data: found", menu.dataFact(), "its data is found");
            menu.setInstall(ready);
            equal("Game data: found", menu.dataFact(), "data found");
            // The fixture's code file: 4096 bytes, last changed 2026-10-04 17:26:40 UTC.
            equal("Game code: 0 MB, 2026-10-04 17:26", menu.codeFact(utc), "code line in UTC");
            equal("Game code: 0 MB, 2026-10-04 19:26", menu.codeFact(TimeZone.getTimeZone("GMT+02:00")), "local time");
            // 338,310,896 bytes is the size of the current game code build: shown as 338 MB.
            equal("338 MB, 1970-01-01 00:00", LauncherMenu.codeSizeAndDate(338_310_896L, 0, utc), "megabytes");
            equal("1 MB, 1970-01-01 00:00", LauncherMenu.codeSizeAndDate(500_000L, 0, utc), "rounds to the nearest");
            equal("Controller: none", LauncherMenu.controllerFact(null), "no controller");
            equal("Controller: none", LauncherMenu.controllerFact("  "), "blank name");
            equal("Controller: AYN Odin Controller", LauncherMenu.controllerFact(" AYN Odin Controller "), "a name");
        });

        LauncherTestSupport.finish("LauncherMenuTest");
    }

    private enum Fault { A_GAME_FILE_GONE, CODE_WRITABLE }

    /** A real detection result for a complete tree with one thing wrong. */
    private static InstallState detectedWith(Fault fault) throws Exception {
        File files = newFilesDir();
        addRun(files, ID_A, 1_700_000_000_000L, addInput(files, "one.bin"), addInput(files, "two.bin"));
        File code = addCode(files, 4096);
        if (fault == Fault.A_GAME_FILE_GONE) {
            check(new File(files, "private-trace/inputs/two.bin").delete(), "remove a game file");
        } else {
            LauncherTestSupport.mode(code, "rw-------");
        }
        InstallState state = InstallState.detect(files);
        discard(files);
        return state;
    }

    /** A real detection result for a small tree with or without game data, game code and a finished disc install. */
    private static InstallState detected(boolean data, boolean code, boolean discInstall) throws Exception {
        File files = newFilesDir();
        if (data) {
            addRun(files, ID_A, 1_700_000_000_000L, addInput(files, "one.bin"));
        }
        if (code) {
            File file = addCode(files, 4096);
            check(file.setLastModified(1_791_134_800_000L), "set the code time");   // 2026-10-04 17:26:40 UTC
        }
        if (discInstall) {
            write(new File(files, "game-install/install.json"), "{\"version\":1,\"complete\":true}\n");
            write(new File(files, "game-install/bundle/code.so"), bytes(100, 5));
        }
        InstallState state = InstallState.detect(files);
        discard(files);
        return state;
    }
}
