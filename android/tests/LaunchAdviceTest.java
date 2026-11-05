package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_A;
import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_B;
import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.detected;
import static dev.recompbootstrap.smoke.LauncherTestSupport.discard;
import static dev.recompbootstrap.smoke.LauncherTestSupport.equal;
import static dev.recompbootstrap.smoke.LauncherTestSupport.join;
import static dev.recompbootstrap.smoke.LauncherTestSupport.privateTrace;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;
import static dev.recompbootstrap.smoke.LauncherTestSupport.tree;
import static dev.recompbootstrap.smoke.LauncherTestSupport.validArguments;
import static dev.recompbootstrap.smoke.LauncherTestSupport.write;

import dev.recompbootstrap.smoke.LauncherTestSupport.Install;
import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;

/**
 * Host test of LaunchAdvice: what the launcher says in every state of the installed files, for every reason a start
 * can fail, and after a start that is known to have failed. Run with java -ea.
 *
 * Every state is a real tree of files looked at by InstallState.detect, and every failure reason is the one
 * LaunchRequest.prepare (or rewriteArguments, checkArguments) really gives for such a tree: the tests follow a state
 * from the files to the sentence. Three properties are asked of every sentence a player can see:
 *   - it uses no word that only a developer of this project knows;
 *   - where the app cannot repair the state itself it says what the install kit does and what the player does next;
 *   - it fits the Play page as the app's own page builder lays it out (nothing cut, the three facts still shown).
 */
public final class LaunchAdviceTest {
    /** Words of the project's inner workings. None may stand in a sentence for the player (the bracket of support detail apart). */
    private static final String[] DEVELOPER_WORDS = {"start request", "template", "qualified", "measurement", "private-trace",
            "arguments", "run id", "launcher", "bundle", "sha-256", "sha256", "hash", "developer", "build", "debug", "library",
            "manifest", "reject", ".so", ".bin", ".txt", "run-"};

    /** A measure with every character half its size wide (as the other menu tests use), and one a tenth wider. */
    private static Scene.Measure measure(final float perCharacter) {
        return new Scene.Measure() {
            @Override public float width(String text, int sizePx, int face) {
                return text.length() * sizePx * (face == Scene.BOLD ? perCharacter + 0.1f : face == Scene.REGULAR ? perCharacter : 0.35f);
            }
        };
    }

    public static void main(String[] args) throws Exception {
        LauncherTestSupport.requireAssertions();
        final Map<Install, InstallState> states = new LinkedHashMap<Install, InstallState>();
        for (Install install : Install.values()) {
            states.put(install, detected(install));
        }

        run("LaunchAdvice.eachStateOfTheFilesHasItsOwnLineAndTheReadyOnesSayReady", () -> {
            Map<Install, InstallState.Problem> expected = new LinkedHashMap<Install, InstallState.Problem>();
            expected.put(Install.READY, InstallState.Problem.NONE);
            expected.put(Install.READY_VERSION_ON_RECORD, InstallState.Problem.NONE);
            expected.put(Install.READY_PIN_UNUSABLE, InstallState.Problem.NONE);
            expected.put(Install.NOTHING, InstallState.Problem.NOTHING_INSTALLED);
            expected.put(Install.DISC_COPY_PARTIAL, InstallState.Problem.DISC_COPY_ONLY);
            expected.put(Install.DISC_COPY_COMPLETE, InstallState.Problem.DISC_COPY_ONLY);
            expected.put(Install.DATA_MISSING, InstallState.Problem.DATA_MISSING);
            expected.put(Install.DATA_ONE_FILE_GONE, InstallState.Problem.DATA_INCOMPLETE);
            expected.put(Install.DATA_NAMES_NOTHING, InstallState.Problem.DATA_INCOMPLETE);
            expected.put(Install.CODE_MISSING, InstallState.Problem.CODE_MISSING);
            expected.put(Install.CODE_NOT_A_PROGRAM, InstallState.Problem.CODE_UNLOADABLE);
            expected.put(Install.CODE_WRITABLE, InstallState.Problem.CODE_UNLOADABLE);
            expected.put(Install.CODE_TWO_NAMES, InstallState.Problem.CODE_UNLOADABLE);
            expected.put(Install.CODE_FOLDER_OPEN, InstallState.Problem.CODE_UNLOADABLE);
            expected.put(Install.SHADERS_MISSING, InstallState.Problem.SHADERS_MISSING);
            equal(Install.values().length, expected.size(), "every fixture state has an expectation");
            Map<String, InstallState.Problem> lines = new LinkedHashMap<String, InstallState.Problem>();
            for (Install install : Install.values()) {
                InstallState state = states.get(install);
                equal(expected.get(install), state.problem, "what stands in the way, " + install);
                String line = LaunchAdvice.stateLine(state);
                equal(state.ready(), line.equals("Ready."), "only a ready state says Ready, " + install + ": " + line);
                // Two different problems never share a line (the player could not tell them apart).
                InstallState.Problem other = lines.put(line, state.problem);
                check(other == null || other == state.problem, "one line for two problems: " + line);
            }
            equal("1 of 3 game data files is missing.", LaunchAdvice.stateLine(states.get(Install.DATA_ONE_FILE_GONE)),
                    "the line counts what is missing, one file in the singular");
            // Two of the three files gone: the same install with a second input removed.
            File twoGone = tree(Install.DATA_ONE_FILE_GONE);
            check(new File(privateTrace(twoGone), "inputs/one.bin").delete(), "(the second game file removed)");
            equal("2 of 3 game data files are missing.", LaunchAdvice.stateLine(InstallState.detect(twoGone)), "two files in the plural");
            discard(twoGone);
            check(!LaunchAdvice.stateLine(states.get(Install.DATA_NAMES_NOTHING)).contains(" of "),
                    "start settings that name nothing are not counted as 0 of 0");
            equal(LaunchAdvice.STATE_CHECKING, LaunchAdvice.stateLine(null), "before the first look");
        });

        run("LaunchAdvice.everyStateThatIsNotReadySaysWhatTheKitDoesAndWhatThePlayerDoesNext", () -> {
            for (Install install : Install.values()) {
                InstallState state = states.get(install);
                for (boolean gameWindow : new boolean[] {true, false}) {
                    String help = LaunchAdvice.help(state, gameWindow);
                    if (state.ready()) {
                        check(!help.contains(LaunchAdvice.THEN_CHECK), "a ready state does not ask to check again, " + install);
                        continue;
                    }
                    check(help.contains("install kit"), install + ": names the install kit: " + help);
                    check(help.contains(LaunchAdvice.KIT_RUN) || help.contains(LaunchAdvice.KIT_AGAIN),
                            install + ": says how the kit is used: " + help);
                    check(help.endsWith(LaunchAdvice.THEN_CHECK), install + ": ends with what the player does then: " + help);
                    equal(help, LaunchAdvice.help(state, !gameWindow), install + ": the same whether or not this copy can open the game window");
                }
            }
            // What the kit copies is said for the piece that is missing, not for another one.
            check(LaunchAdvice.help(states.get(Install.CODE_MISSING), true).contains("The game code is the game's program"), "game code");
            check(LaunchAdvice.help(states.get(Install.DATA_MISSING), true).contains("The game data is the files from the game discs"), "game data");
            check(LaunchAdvice.help(states.get(Install.DATA_ONE_FILE_GONE), true).contains("The game data is the files from the game discs"), "game data, one file gone");
            check(LaunchAdvice.help(states.get(Install.SHADERS_MISSING), true).contains("loads its shaders from this file"), "shader file");
            check(LaunchAdvice.help(states.get(Install.NOTHING), true).contains("the game data, the game code and the shader file"), "everything");
            check(LaunchAdvice.help(states.get(Install.DISC_COPY_COMPLETE), true).contains("The disc files stay where they are."), "disc copies are kept");
            equal(LaunchAdvice.help(states.get(Install.DISC_COPY_PARTIAL), true), LaunchAdvice.help(states.get(Install.DISC_COPY_COMPLETE), true),
                    "a partial and a complete disc copy get the same advice: neither can be started");
            // Each fault of the game code file has its own sentence.
            List<String> faults = new ArrayList<String>();
            for (Install install : new Install[] {Install.CODE_NOT_A_PROGRAM, Install.CODE_WRITABLE, Install.CODE_TWO_NAMES, Install.CODE_FOLDER_OPEN}) {
                String help = LaunchAdvice.help(states.get(install), true);
                String fault = LaunchAdvice.faultSentence(states.get(install).codeFault);
                check(!fault.isEmpty() && help.startsWith(fault), install + ": begins with its fault: " + help);
                check(!faults.contains(fault), install + ": its own sentence");
                faults.add(fault);
            }
            check(LaunchAdvice.faultSentence(InstallState.CodeFault.TOO_LARGE).contains("512 MiB"), "the size limit is said");
            equal("", LaunchAdvice.faultSentence(InstallState.CodeFault.NONE), "no fault, no sentence");
            equal("", LaunchAdvice.help(null, true), "nothing before the first look");
        });

        run("LaunchAdvice.aReadyStateSaysHowTheGameIsLeftAndAtMostOneNote", () -> {
            String plain = LaunchAdvice.help(states.get(Install.READY_VERSION_ON_RECORD), true);
            equal(LaunchAdvice.HELP_READY, plain, "version on record, pin usable: no note");
            check(plain.contains("closes it at once") && plain.contains("progress since the last save is lost"), "leaving the game ends it");
            equal(LaunchAdvice.HELP_READY + " " + LaunchAdvice.HELP_NO_RECORD, LaunchAdvice.help(states.get(Install.READY), true),
                    "no version on record: Play does not compare, and says so");
            // READY_PIN_UNUSABLE has no version on record either: the unusable start settings are the note that is shown.
            check(!states.get(Install.READY_PIN_UNUSABLE).versionOnRecord && states.get(Install.READY_PIN_UNUSABLE).pinIgnored, "fixture");
            equal(LaunchAdvice.HELP_READY + " " + LaunchAdvice.HELP_PIN_IGNORED, LaunchAdvice.help(states.get(Install.READY_PIN_UNUSABLE), true),
                    "start settings that could not be used");
            equal(LaunchAdvice.HELP_NO_GAME_WINDOW, LaunchAdvice.help(states.get(Install.READY), false),
                    "a copy of the app without the game window says why Play is not offered");
        });

        run("LaunchAdvice.everyReasonPrepareGivesForATreeHasItsOwnSentenceWithTheNextStep", () -> {
            Map<Install, String> expected = new LinkedHashMap<Install, String>();
            expected.put(Install.NOTHING, LaunchAdvice.FAIL_DATA);
            expected.put(Install.DISC_COPY_COMPLETE, LaunchAdvice.FAIL_DATA);
            expected.put(Install.DATA_MISSING, LaunchAdvice.FAIL_DATA);
            expected.put(Install.DATA_ONE_FILE_GONE, LaunchAdvice.FAIL_DATA);
            expected.put(Install.DATA_NAMES_NOTHING, LaunchAdvice.FAIL_DATA);
            expected.put(Install.CODE_MISSING, LaunchAdvice.FAIL_CODE_MISSING);
            expected.put(Install.SHADERS_MISSING, LaunchAdvice.FAIL_SHADERS);
            for (Map.Entry<Install, String> entry : expected.entrySet()) {
                equal(entry.getValue(), LaunchAdvice.startFailure(preparedError(entry.getKey())), "sentence for " + entry.getKey());
            }
            for (Install install : new Install[] {Install.CODE_NOT_A_PROGRAM, Install.CODE_WRITABLE, Install.CODE_TWO_NAMES, Install.CODE_FOLDER_OPEN}) {
                String text = LaunchAdvice.startFailure(preparedError(install));
                String fault = LaunchAdvice.faultSentence(states.get(install).codeFault);
                equal(LaunchAdvice.FAIL_CODE_UNUSABLE + " " + fault + " " + LaunchAdvice.HELP_CODE_REPLACED, text, "sentence for " + install);
            }
            equal(LaunchAdvice.FAIL_CODE_UNUSABLE + " " + LaunchAdvice.faultSentence(InstallState.CodeFault.TOO_LARGE) + " "
                    + LaunchAdvice.HELP_CODE_REPLACED, LaunchAdvice.startFailure(LaunchRequest.ERROR_CODE_TOO_LARGE), "a game code file over the size limit");
            check(LaunchAdvice.startFailure(LaunchRequest.ERROR_CODE_UNLOADABLE_PREFIX + "something new").startsWith(
                    LaunchAdvice.FAIL_CODE_UNUSABLE + " " + LaunchAdvice.HELP_CODE_REPLACED), "a fault this version has no sentence for still gets the advice");

            // A file that cannot be copied: the image of the start request is unreadable.
            File files = tree(Install.READY);
            LauncherTestSupport.mode(new File(privateTrace(files), "run-" + ID_A + "/decoded.image.bin"), "---------");
            String reason = LaunchRequest.prepare(files).error;
            discard(files);
            String copy = LaunchAdvice.startFailure(reason);
            check(copy.startsWith(LaunchAdvice.FAIL_FILE + " ("), "the sentence for a file that could not be copied: " + copy);
            equal(LaunchAdvice.FAIL_FILE + " (AccessDeniedException: decoded.image.bin)", copy, "the error's class and the file's own name, in one bracket at the end");
            equal("FileSystemException: a.bin: No space left on device", LaunchAdvice.withoutDirectories("FileSystemException: /data/user/0/x/files/a.bin: No space left on device"),
                    "what follows the path is kept");
            equal("IOException: the game image copy is incomplete", LaunchAdvice.withoutDirectories("IOException: the game image copy is incomplete"), "a text without a path is kept whole");
            equal("NoSuchFileException: ", LaunchAdvice.withoutDirectories("NoSuchFileException: /"), "a path that is only a slash");
            equal(LaunchAdvice.FAIL_SETTINGS_FOLDER, LaunchAdvice.startFailure(LaunchRequest.ERROR_SETTINGS_DIRECTORY), "no settings folder");

            // A reason nobody has written a sentence for is shown as it is, with a next step, never dropped.
            String unknown = LaunchAdvice.startFailure("the moon is in the way");
            check(unknown.contains("the moon is in the way") && unknown.contains("Choose Play again."), "an unknown reason: " + unknown);
            check(LaunchAdvice.startFailure(null).contains("Choose Play again.") && LaunchAdvice.startFailure(" ").contains("no reason was given"), "no reason at all");
        });

        run("LaunchAdvice.startSettingsThatCannotBeCopiedOrThatTheGameWouldRefuseGetOneSentence", () -> {
            // Each refusal of rewriteArguments, provoked for real.
            byte[] template = join("/r/run-" + ID_A + "/host_guest_trace", "value");
            String old = "01010101010101010101010101010101", fresh = "20101010101010101010101010101010";
            Object[][] rewrites = {{template, "cccccccccccccccccccccccccccccccc", ID_B}, {template, ID_A, ID_B.substring(1)}, {template, ID_A, ID_A},
                    {new byte[0], ID_A, ID_B}, {(old + "1").getBytes("US-ASCII"), old, fresh}};
            List<String> seen = new ArrayList<String>();
            for (Object[] rewrite : rewrites) {
                String reason = null;
                try {
                    LaunchRequest.rewriteArguments((byte[]) rewrite[0], (String) rewrite[1], (String) rewrite[2]);
                } catch (IllegalArgumentException refused) {
                    reason = refused.getMessage();
                }
                check(reason != null && !seen.contains(reason), "a refusal of its own: " + reason);
                seen.add(reason);
                equal(LaunchAdvice.FAIL_START_SETTINGS, LaunchAdvice.startFailure(reason), "sentence for <" + reason + ">");
            }
            equal(5, seen.size(), "the five refusals of rewriteArguments");
            // A request the game's own rule would refuse, with the reason checkArguments really gives.
            String root = "/data/user/0/dev.recompbootstrap.smoke/files/private-trace";
            String refusal = LaunchRequest.checkArguments(join(validArguments(root, ID_A)), root, ID_B);
            check(refusal != null, "the rule refuses a request under another run's id");
            equal(LaunchAdvice.FAIL_START_SETTINGS, LaunchAdvice.startFailure(LaunchRequest.ERROR_REQUEST_REFUSED_PREFIX + refusal), "a refused request");
            check(LaunchAdvice.FAIL_START_SETTINGS.contains("The install kit writes new ones."), "the kit writes the start settings");
        });

        run("LaunchAdvice.afterAStartOnlyWhatIsKnownToHaveGoneWrongIsSaid", () -> {
            File files = LauncherTestSupport.newFilesDir();
            LaunchOutcome notAccepted = LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1);
            write(new File(privateTrace(files), "used-" + ID_A), new byte[0]);
            equal(null, LaunchAdvice.afterStart(LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1)),
                    "the player left the game: nothing");
            equal(LaunchAdvice.OUTCOME_NOT_ACCEPTED, LaunchAdvice.afterStart(notAccepted), "not accepted");
            equal("The game closed because of an error in the game code (a crash) after 42 seconds. Choose Play to start it again.",
                    LaunchAdvice.afterStart(LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.CRASH, 42_999)), "a crash");
            equal("Android closed the game to free memory after 12 minutes. Close other apps, then choose Play again.",
                    LaunchAdvice.afterStart(LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.LOW_MEMORY, 12 * 60_000 + 10_000)), "low memory");
            equal("Android closed the game because it had stopped responding. Choose Play to start it again.",
                    LaunchAdvice.afterStart(LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.NOT_RESPONDING, -1)), "not responding, time unknown");
            equal(" after 1 second", LaunchAdvice.after(1_000), "one second");
            equal(" after 89 seconds", LaunchAdvice.after(89_999), "seconds up to a minute and a half");
            equal(" after 2 minutes", LaunchAdvice.after(90_000), "minutes from there, rounded to the nearest");
            equal(" after 0 seconds", LaunchAdvice.after(0), "at once");

            // The game window's refusal words: each family has its sentence, and the word is kept in a bracket.
            Map<String, String> words = new LinkedHashMap<String, String>();
            words.put("REJECT_LIBRARY_6", LaunchAdvice.OUTCOME_CODE_REFUSED);
            words.put("REJECT_SIZE", LaunchAdvice.OUTCOME_CODE_REFUSED);
            words.put("REJECT_HASH", LaunchAdvice.OUTCOME_CODE_REFUSED);
            words.put("REJECT_MANIFEST_FILE", LaunchAdvice.OUTCOME_SETTINGS_REFUSED);
            words.put("REJECT_MANIFEST_HASH", LaunchAdvice.OUTCOME_SETTINGS_REFUSED);
            words.put("REJECT_NATIVE_RUN", LaunchAdvice.OUTCOME_CODE_STOPPED);
            words.put("REJECT_RUN_SETUP_3", LaunchAdvice.OUTCOME_CODE_STOPPED);
            words.put("REJECT_EMPTY_PROTOCOL", LaunchAdvice.OUTCOME_CODE_STOPPED);
            words.put("REJECT_PROCESS", LaunchAdvice.OUTCOME_WINDOW);
            words.put("REJECT_SURFACE", LaunchAdvice.OUTCOME_WINDOW);
            words.put("REJECT_IO_IOException", LaunchAdvice.OUTCOME_WINDOW);
            for (Map.Entry<String, String> word : words.entrySet()) {
                write(new File(privateTrace(files), "run-" + ID_A + "/" + LaunchOutcome.REFUSAL_FILE), word.getKey() + "\n");
                equal(word.getValue() + " (" + word.getKey() + ")",
                        LaunchAdvice.afterStart(LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1)), word.getKey());
            }
            discard(files);
        });

        run("LaunchAdvice.noSentenceUsesWordsOnlyADeveloperKnowsAndEachSaysWhatToDoNext", () -> {
            for (Map.Entry<String, String> text : everyText(states).entrySet()) {
                String shown = text.getValue();
                // What stands in the bracket at the very end is detail for support; the sentence before it must stand alone.
                String sentence = shown.endsWith(")") && shown.lastIndexOf(" (") > 0 ? shown.substring(0, shown.lastIndexOf(" (")) : shown;
                String lower = sentence.toLowerCase(Locale.ROOT);
                for (String word : DEVELOPER_WORDS) {
                    check(!lower.contains(word), text.getKey() + ": the word <" + word + "> in: " + sentence);
                }
                check(!sentence.contains("  ") && !sentence.contains(" ."), text.getKey() + ": spacing: " + sentence);
                for (String vague : new String[] {" may ", " might ", " could be "}) {
                    check(!lower.contains(vague), text.getKey() + ": a guess (<" + vague.trim() + ">) in: " + sentence);
                }
            }
            // Every text about something that went wrong ends in something the player can do.
            for (Map.Entry<String, String> text : everyText(states).entrySet()) {
                if (!text.getKey().startsWith("failure") && !text.getKey().startsWith("after") && !text.getKey().startsWith("question")) continue;
                String shown = text.getValue();
                String lower = shown.toLowerCase(Locale.ROOT);
                check(lower.contains("choose play") || lower.contains("install kit") || lower.contains("press a on play again"),
                        text.getKey() + ": no next step in: " + shown);
            }
        });

        run("LaunchAdvice.everyTextFitsThePlayPageAsTheAppLaysItOut", () -> {
            TimeZone utc = TimeZone.getTimeZone("UTC");
            int pages = 0;
            for (float perCharacter : new float[] {0.5f, 0.55f}) {
                Scene.Measure measure = measure(perCharacter);
                float textWidth = MenuLayout.paneWidth(true) - 2 * MenuLayout.ROW_PAD;
                for (Install install : Install.values()) {
                    for (boolean gameWindow : new boolean[] {true, false}) {
                        InstallState state = states.get(install);
                        LauncherMenu menu = new LauncherMenu(gameWindow);
                        menu.setInstall(state);
                        String what = install + (gameWindow ? "" : ", no game window") + ", " + perCharacter;
                        equal(1, Scene.wrap(menu.stateLine(), MenuLayout.ROW_PX, Scene.REGULAR, textWidth, measure).size(), what + ": the state line is one line");
                        int helpLines = Scene.wrap(menu.detail(), MenuLayout.HELP_PX, Scene.REGULAR, textWidth, measure).size();
                        check(helpLines <= MenuLayout.HELP_LINES, what + ": the advice takes " + helpLines + " lines, the strip holds " + MenuLayout.HELP_LINES);
                        laidOut(menu, measure, utc, what);
                        ++pages;
                        if (!state.ready()) {
                            menu.tap(LauncherMenu.PRIMARY, false);   // "Check again", then the same state: the note under the state line
                            menu.setInstall(state);
                            equal(LaunchAdvice.CHECKED_NO_CHANGE, menu.secondLine(LauncherMenu.PRIMARY), what + ": (the note is up)");
                            laidOut(menu, measure, utc, what + ", checked again");
                            ++pages;
                        }
                    }
                }
                // Everything that can stand above the facts of a ready page: each failure, each outcome, the question.
                // (READY_PIN_UNUSABLE has the longest advice of a ready state under the facts.)
                for (Map.Entry<String, String> reason : failureReasons(states).entrySet()) {
                    LauncherMenu menu = new LauncherMenu(true);
                    menu.setInstall(states.get(Install.READY_PIN_UNUSABLE));
                    menu.startFailed(reason.getValue());
                    equal(LaunchAdvice.startFailure(reason.getValue()), menu.failure(), "(the failure is up)");
                    laidOut(menu, measure, utc, reason.getKey() + ", " + perCharacter);
                    ++pages;
                }
                for (Map.Entry<String, LaunchOutcome> outcome : outcomes().entrySet()) {
                    LauncherMenu menu = new LauncherMenu(true);
                    menu.setInstall(states.get(Install.READY_PIN_UNUSABLE));
                    menu.startOutcome(outcome.getValue());
                    equal(LaunchAdvice.afterStart(outcome.getValue()), menu.failure(), "(the outcome is up)");
                    laidOut(menu, measure, utc, outcome.getKey() + ", " + perCharacter);
                    ++pages;
                }
                LauncherMenu asking = new LauncherMenu(true);
                asking.setInstall(states.get(Install.READY_PIN_UNUSABLE));
                asking.tap(LauncherMenu.PRIMARY, false);
                asking.prepared(LaunchRequest.UNQUALIFIED, false, true);
                check(asking.asking(), "(the question is up)");
                laidOut(asking, measure, utc, "the question, " + perCharacter);
                ++pages;
            }
            System.out.println("     " + pages + " Play pages laid out with the app's page builder at two text widths");
        });

        run("LaunchAdvice.aboutSaysWhatIsInstalledInFullSentences", () -> {
            String ready = LaunchAdvice.aboutInstall(states.get(Install.READY_VERSION_ON_RECORD));
            equal("Game data: 3 files and folders found.\nShader file: found.\n" + LaunchAdvice.ABOUT_VERSION_COMPARED, ready, "a complete install with a version on record");
            check(LaunchAdvice.aboutInstall(states.get(Install.READY)).endsWith(LaunchAdvice.ABOUT_VERSION_NOT_COMPARED), "no version on record");
            check(LaunchAdvice.aboutInstall(states.get(Install.DATA_ONE_FILE_GONE)).startsWith("Game data: 1 of 3 files missing.\n"), "one file gone");
            // An install whose start settings name a single file: a count of one is said in the singular.
            File single = LauncherTestSupport.newFilesDir();
            LauncherTestSupport.addRun(single, ID_A, 1_700_000_000_000L, LauncherTestSupport.addInput(single, "one.bin"));
            check(LaunchAdvice.aboutInstall(InstallState.detect(single)).startsWith("Game data: 1 file or folder found.\n"),
                    "one file: " + LaunchAdvice.aboutInstall(InstallState.detect(single)));
            discard(single);
            check(LaunchAdvice.aboutInstall(states.get(Install.NOTHING)).startsWith("Game data: not installed.\nShader file: missing.\n"), "nothing");
            check(LaunchAdvice.aboutInstall(states.get(Install.SHADERS_MISSING)).contains("\nShader file: missing.\n"), "no shader file");
            check(LaunchAdvice.aboutInstall(states.get(Install.CODE_WRITABLE)).contains("\nGame code file: "
                    + LaunchAdvice.faultSentence(InstallState.CodeFault.PERMISSIONS) + "\n"), "a game code file the game window would refuse");
            check(!ready.contains("Game code file:"), "no such line for a file that loads");
            String block = AboutScreen.installBlock(states.get(Install.READY_VERSION_ON_RECORD), TimeZone.getTimeZone("UTC"));
            equal("Game code: 0 MB, 2026-10-04 17:26\n" + ready, block, "the About section: the game code line, then the facts");
            check(AboutScreen.installBlock(states.get(Install.CODE_MISSING), TimeZone.getTimeZone("UTC")).startsWith("Game code: not installed\n"), "no game code");
            // What a player can use: the settings this game version lacks, by the names of their rows on the Settings tab.
            equal(LaunchAdvice.ABOUT_NAMES + "\n    Frame Generation (experimental)\n    Draw Distance", AboutScreen.namesBlock(SettingsCoreTestKit.v464(), null),
                    "game build v464: the two rows that are not available, each on its line");
            equal(LaunchAdvice.ABOUT_NAMES_NONE, AboutScreen.namesBlock(Capabilities.of(new java.util.TreeSet<String>(), true, false), null), "game code that has none of the settings");
            // The input option is the app's: it is never listed as the game's, whether this app and Android have it or not.
            java.util.Set<String> v464 = SettingsCoreTestKit.names(SettingsCoreTestKit.V464_NAMES);
            equal(AboutScreen.namesBlock(Capabilities.of(v464, true, true), null), AboutScreen.namesBlock(Capabilities.of(v464, true, false), null),
                    "the same list with and without the input option");
            // A build that has them all (the generator announced with its files, draw distance unlocked) says so in one sentence.
            java.util.Set<String> all = SettingsCoreTestKit.names(SettingsCoreTestKit.plus(SettingsCoreTestKit.ALL_NAMES,
                    Capabilities.FRAME_GENERATION_MARKER, "diag-fg-between", "diag-fg-ahead"));
            equal(LaunchAdvice.ABOUT_NAMES_ALL, AboutScreen.namesBlock(Capabilities.of(all, true, false), null), "a build with every setting");
            for (String said : new String[] {AboutScreen.namesBlock(SettingsCoreTestKit.v464(), null), LaunchAdvice.ABOUT_NAMES_NONE, LaunchAdvice.ABOUT_NAMES_ALL}) {
                check(!said.contains("diag-") && !said.contains(".txt") && !said.contains("recomp-cap"), "no file name of the game's internals: " + said);
            }
            equal("The game code file could not be read (IOException).", AboutScreen.namesBlock(null, "IOException"), "a file that cannot be read");
            String section = AboutScreen.sectionText(ShellPages.ABOUT_BUILD, "v", block, "");
            equal(block, section, "without a list the section is the facts alone, with no empty lines after them");
        });

        LauncherTestSupport.finish("LaunchAdviceTest");
    }

    /** The reason LaunchRequest.prepare really gives for a tree in that state. */
    private static String preparedError(Install install) throws Exception {
        File files = tree(install);
        String error = LaunchRequest.prepare(files).error;
        discard(files);
        check(error != null, "prepare refuses a tree in the state " + install);
        return error;
    }

    /** Every reason a start can be refused with, as LaunchRequest.prepare or the screen gives it, under a name. */
    private static Map<String, String> failureReasons(Map<Install, InstallState> states) throws Exception {
        Map<String, String> reasons = new LinkedHashMap<String, String>();
        for (Install install : Install.values()) {
            if (!states.get(install).ready()) {
                reasons.put("failure for a tree, " + install, preparedError(install));
            }
        }
        reasons.put("failure, too large", LaunchRequest.ERROR_CODE_TOO_LARGE);
        reasons.put("failure, start settings", LaunchRequest.REWRITE_ID_ABSENT);
        reasons.put("failure, settings folder", LaunchRequest.ERROR_SETTINGS_DIRECTORY);
        reasons.put("failure, file", LaunchRequest.ERROR_IO_PREFIX + "NoSuchFileException: /data/user/0/dev.recompbootstrap.smoke/files/private-trace/run-"
                + ID_A + "/decoded.image.bin)");
        reasons.put("failure, Android", LaunchAdvice.cannotOpen("game window", "ActivityNotFoundException"));
        return reasons;
    }

    /** Every outcome of a start that has a sentence, read from real files, under a name. */
    private static Map<String, LaunchOutcome> outcomes() throws Exception {
        Map<String, LaunchOutcome> outcomes = new LinkedHashMap<String, LaunchOutcome>();
        File files = LauncherTestSupport.newFilesDir();
        outcomes.put("after a start, not accepted", LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1));
        write(new File(privateTrace(files), "used-" + ID_A), new byte[0]);
        outcomes.put("after a start, crash", LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.CRASH, 3_600_000));
        outcomes.put("after a start, low memory", LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.LOW_MEMORY, 61_000));
        outcomes.put("after a start, not responding", LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.NOT_RESPONDING, 5_000));
        for (String word : new String[] {"REJECT_LIBRARY_6", "REJECT_MANIFEST_HASH", "REJECT_RUN_SETUP_3", "REJECT_IO_IOException"}) {
            write(new File(privateTrace(files), "run-" + ID_A + "/" + LaunchOutcome.REFUSAL_FILE), word + "\n");
            outcomes.put("after a start, " + word, LaunchOutcome.read(files, ID_A, LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1));
        }
        discard(files);
        return outcomes;
    }

    /** Every sentence a player can be shown, under a name that says where it comes from. */
    private static Map<String, String> everyText(Map<Install, InstallState> states) throws Exception {
        Map<String, String> texts = new LinkedHashMap<String, String>();
        for (Install install : Install.values()) {
            texts.put("state line, " + install, LaunchAdvice.stateLine(states.get(install)));
            texts.put("advice, " + install, LaunchAdvice.help(states.get(install), true));
            texts.put("advice without the game window, " + install, LaunchAdvice.help(states.get(install), false));
            texts.put("about, " + install, LaunchAdvice.aboutInstall(states.get(install)));
        }
        for (InstallState.CodeFault fault : InstallState.CodeFault.values()) {
            texts.put("fault, " + fault, LaunchAdvice.faultSentence(fault));
        }
        for (Map.Entry<String, String> reason : failureReasons(states).entrySet()) {
            texts.put(reason.getKey(), LaunchAdvice.startFailure(reason.getValue()));
        }
        for (Map.Entry<String, LaunchOutcome> outcome : outcomes().entrySet()) {
            texts.put(outcome.getKey(), LaunchAdvice.afterStart(outcome.getValue()));
        }
        texts.put("failure, controller check", LaunchAdvice.controllerCheckFailed(LaunchAdvice.cannotOpen("controller check", "SecurityException")));
        texts.put("question", LaunchAdvice.OTHER_VERSION_DETAIL);
        texts.put("short form of the question", LaunchAdvice.OTHER_VERSION);
        for (String fixed : new String[] {LaunchAdvice.CHECK_AGAIN, LaunchAdvice.CHECKING_AGAIN, LaunchAdvice.CHECKED_NO_CHANGE, LaunchAdvice.CHECKING_CODE,
                LaunchAdvice.ALREADY_RUNNING, LaunchAdvice.NOT_IN_FRONT, LaunchAdvice.NOT_IN_FRONT_DETAIL, LaunchAdvice.NO_GAME_WINDOW, LaunchAdvice.CLOSE_GAME_FIRST,
                LaunchAdvice.ABOUT_CHECKING, LaunchAdvice.ABOUT_NAMES, LaunchAdvice.ABOUT_NAMES_NONE, LaunchAdvice.ABOUT_NAMES_ALL}) {
            texts.put("fixed text <" + fixed + ">", fixed);
        }
        return texts;
    }

    /**
     * Lays the Play page out with the app's own page builder and checks what a cut or crowded text would break:
     * nothing ends in an ellipsis that is not its own, every text fits its box, the three facts are all on the page
     * and nothing of the pane stands below the body's end.
     */
    private static void laidOut(LauncherMenu menu, Scene.Measure measure, TimeZone zone, String what) {
        ShellNav nav = new ShellNav();
        MenuScenes.Page page = ShellPages.frame(nav, menu, "Odin Controller", zone);
        ShellPages.play(page, nav, menu, "Odin Controller", zone);
        Scene scene = MenuScenes.build(page, measure);
        int facts = 0;
        for (Scene.Item item : scene.items) {
            if (item.type != Scene.TEXT) continue;
            boolean own = item.text.toLowerCase(Locale.ROOT).endsWith("ing" + Scene.ELLIPSIS);
            check(own || !item.text.endsWith(Scene.ELLIPSIS), what + ": cut: " + item.text);
            check(measure.width(item.text, item.size, item.face) <= item.w + 0.5f, what + ": wider than its box: " + item.text);
            if (item.x >= MenuLayout.paneLeft(true) && item.y >= MenuLayout.BODY_TOP && item.y < MenuLayout.PROMPT_TOP) {
                check(item.y + item.h <= MenuLayout.BODY_BOTTOM + 0.5f, what + ": below the body's end: " + item.text);
            }
            if (item.text.equals("Game Data") || item.text.equals("Game Code") || item.text.equals("Controller")) ++facts;
        }
        equal(3, facts, what + ": the three facts are on the page");
    }
}
