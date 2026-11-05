package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.bytes;
import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;
import static dev.recompbootstrap.smoke.SetupTestKit.item;

import java.io.File;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Run with java -ea. The setup screen's decisions, driven by the same commands the controller sends.
 * The texts are checked by what they must tell a person (the fact, the next step, the way that works instead), as
 * written out here, not by comparing them with the model's own constants.
 */
public final class SetupModelTest {
    private static final long GB = 1000000000L;
    private static final VerifiedExtractor.Problem[] COPY_PROBLEMS = {VerifiedExtractor.Problem.DISC_COPY_BAD,
            VerifiedExtractor.Problem.SOURCE_UNREADABLE, VerifiedExtractor.Problem.STORAGE, VerifiedExtractor.Problem.BUSY,
            VerifiedExtractor.Problem.OTHER, VerifiedExtractor.Problem.EXISTING_INSTALL};

    private static InstallGuard.Existing existing(boolean data, boolean code, boolean unreadable, boolean complete) {
        return new InstallGuard.Existing(data, code, unreadable, complete);
    }

    private static SetupModel.Disk disk(int present, int total, boolean bundle) {
        return new SetupModel.Disk(null, present, total, (long) (total - present) * GB, 20 * GB, bundle, false);
    }

    private static SetupModel fresh() {
        SetupModel m = new SetupModel();
        m.setDisk(disk(0, 10, false));
        return m;
    }

    private static DiscIdentifier.Result supported() {
        return new DiscIdentifier.Result(DiscIdentifier.Kind.SUPPORTED_PLAY_DISC, "ab", "Recognised: Test release, the play disc.", null);
    }

    private static void press(SetupModel m, int... commands) {
        for (int c : commands) m.nav(c, false);
    }

    /** Chooses an image on a model whose focus is on step 1 and reports this identification result. */
    private static void choose(SetupModel m, DiscIdentifier.Result result) {
        m.focus(SetupModel.STEP_PLAY);
        press(m, NavInput.CONFIRM, NavInput.CONFIRM);
        expectEquals(SetupModel.ACT_PICK_PLAY_IMAGE, m.takeAction(), "the image chooser is asked for");
        m.identifyStarted();
        m.playDiscIdentified(result);
    }

    /** A model whose play disc was chosen and recognised; the focus is on "Copy game files". */
    private static SetupModel identified() {
        SetupModel m = fresh();
        choose(m, supported());
        return m;
    }

    /** A model whose copy just ended with this outcome, with present of 10 files in place. */
    private static SetupModel copyEnded(VerifiedExtractor.Outcome outcome, int present) {
        SetupModel m = identified();
        press(m, NavInput.CONFIRM);
        expectEquals(SetupModel.ACT_START_COPY, m.takeAction(), "the copy starts");
        m.workStarted(SetupModel.STEP_COPY, 0);
        m.setDisk(disk(present, 10, false));
        m.workFinished(outcome);
        return m;
    }

    /** A model with the play disc's files in place whose import just ended with this outcome. */
    private static SetupModel importEnded(VerifiedExtractor.Outcome outcome, boolean bundleNow) {
        SetupModel m = new SetupModel();
        m.setDisk(disk(10, 10, false));
        press(m, NavInput.CONFIRM);
        expectEquals(SetupModel.ACT_PICK_BUNDLE, m.takeAction(), "the bundle chooser is asked for");
        m.workStarted(SetupModel.STEP_IMPORT, 0);
        m.setDisk(disk(10, 10, bundleNow));
        m.workFinished(outcome);
        return m;
    }

    /** A face a tenth wider than the layout tests' fixed one: a text that fits here fits with room to spare. */
    private static final Scene.Measure WIDE = new Scene.Measure() {
        @Override
        public float width(String text, int sizePx, int face) {
            return text.length() * sizePx * 1.1f * (face == Scene.BOLD ? 0.6f : face == Scene.REGULAR ? 0.5f : 0.35f);
        }
    };

    private static int fitted;

    /** Every text of the page as the app lays it out, with each row highlighted in turn: none is cut and none leaves its box. */
    private static void expectFits(SetupModel m, String what) {
        ++fitted;
        int rows = m.overlay() == SetupModel.OVERLAY_NONE ? SetupModel.STEPS : 1;
        int kept = m.focused();
        for (int step = 0; step < rows; ++step) {
            if (rows > 1) m.focus(step);
            Scene scene = MenuScenes.build(ShellPages.setup(m), WIDE);
            for (Scene.Item it : scene.items) {
                if (it.type != Scene.TEXT) continue;
                String where = what + ", row " + step + ": <" + it.text + ">";
                expect(WIDE.width(it.text, it.size, it.face) <= it.w + 0.5f, "wider than its box: " + where);
                boolean own = it.text.endsWith("ing" + Scene.ELLIPSIS);          // "Copying...", "Stopping...": the model's own
                expect(own || !it.text.endsWith(Scene.ELLIPSIS), "cut off: " + where);
                expect(it.y >= 0 && it.y + it.h <= MenuLayout.H + 0.5f, "outside the screen: " + where);
            }
        }
        if (rows > 1) m.focus(kept);
    }

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();

        check("model: five steps; UP and DOWN move by one and wrap on a fresh press only", () -> {
            SetupModel m = fresh();
            expectEquals(SetupModel.STEP_PLAY, m.focused(), "start");
            int[] order = {1, 2, 3, 4, 0};
            for (int expected : order) {
                expect(m.nav(NavInput.DOWN, false), "handled");
                expectEquals(expected, m.focused(), "down");
            }
            m.nav(NavInput.UP, false);
            expectEquals(SetupModel.STEP_DONE, m.focused(), "up from the first wraps to the last");
            m.nav(NavInput.DOWN, true);
            expectEquals(SetupModel.STEP_DONE, m.focused(), "a held DOWN stops at the last");
            for (int i = 0; i < 9; ++i) m.nav(NavInput.UP, true);
            expectEquals(SetupModel.STEP_PLAY, m.focused(), "a held UP stops at the first");
            expect(m.label(0).contains("play disc") && m.label(1).contains("install disc") && m.label(2).contains("Copy")
                    && m.label(3).contains("Import") && m.label(4).contains("Finish"), "the five labels");
        });

        check("model: before the device was looked at, nothing can be started", () -> {
            SetupModel m = new SetupModel();
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.OVERLAY_NONE, m.overlay(), "no question");
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "no action");
            expect(!m.actionable(SetupModel.STEP_PLAY), "not actionable");
            expectContains(m.detail(SetupModel.STEP_PLAY), "Looking at what is already on this device", "says why");
            expectEquals("B Back", m.footer(), "only B is offered");
            expectFits(m, "not looked yet");
        });

        check("model: an install the page did not make closes it: nothing can be chosen, and it says why and what can be done", () -> {
            // what is on the device -> the state word of every step, and what the text must say
            Map<InstallGuard.Existing, String[]> cases = new LinkedHashMap<InstallGuard.Existing, String[]>();
            cases.put(existing(true, true, false, false), new String[] {"Not available", "The game on this device was installed from a computer.",
                    "This page never changes such an install", "go back (B) and choose Play", "If Play is not offered, run the install kit again"});
            cases.put(existing(true, false, false, false), new String[] {"Not available", "but the game code is not there",
                    "This page never changes such an install", "Run the install kit again to complete the install."});
            cases.put(existing(false, true, false, false), new String[] {"Not available", "but the game's data files are not there",
                    "This page never changes such an install", "Run the install kit again to complete the install."});
            cases.put(existing(false, false, true, false), new String[] {"Not available", "record of an earlier setup cannot be read",
                    "this page changes nothing", "Install the game with the install kit"});
            cases.put(existing(false, false, false, true), new String[] {"Done", "A complete install made by this page is on this device",
                    "B: back to the main menu, then choose Play."});
            for (Map.Entry<InstallGuard.Existing, String[]> c : cases.entrySet()) {
                String[] want = c.getValue();
                SetupModel m = new SetupModel();
                m.setDisk(new SetupModel.Disk(c.getKey(), 4, 10, 6 * GB, 20 * GB, false, false));
                expect(m.readOnly(), "read-only: " + want[1]);
                for (int step = 0; step < SetupModel.STEPS; ++step) {
                    expectEquals(want[0], m.stateWord(step), "state of step " + step + " (" + want[1] + ")");
                    for (int i = 1; i < want.length; ++i) expectContains(m.detail(step), want[i], "text of step " + step);
                    expect(!m.actionable(step) && !m.detailIsProblem(step), "not actionable, not drawn as a failure");
                    m.focus(step);
                    press(m, NavInput.CONFIRM);
                    expectEquals(SetupModel.OVERLAY_NONE, m.overlay(), "no question at step " + step);
                    expectEquals(SetupModel.ACT_NONE, m.takeAction(), "no action at step " + step);
                    expectEquals("B Back", m.footer(), "A is not offered at step " + step);
                }
                expect(!m.nav(NavInput.BACK, false), "B is left to the host: leave");
                expectFits(m, "closed: " + want[1]);
            }
            // The install appears while the page is open (the install kit ran meanwhile): the copy that was ready to
            // start, and the import that was ready to start, are closed too.
            SetupModel ready = identified();
            ready.setDisk(new SetupModel.Disk(existing(true, true, false, false), 0, 10, 10 * GB, 20 * GB, false, false));
            expectEquals(SetupModel.STEP_COPY, ready.focused(), "the highlight is still on the copy");
            press(ready, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, ready.takeAction(), "A no longer starts the copy");
            expectContains(ready.detail(SetupModel.STEP_COPY), "was installed from a computer", "and the step says why");
            SetupModel copied = new SetupModel();
            copied.setDisk(disk(10, 10, false));
            copied.setDisk(new SetupModel.Disk(existing(true, false, false, false), 10, 10, 0, 20 * GB, false, false));
            expectEquals(SetupModel.STEP_IMPORT, copied.focused(), "the highlight is still on the import");
            press(copied, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, copied.takeAction(), "A no longer asks for a bundle");
        });

        check("model: step 1 says what the page can and cannot do, then asks where the disc copy is; image, folder and cancel by controller and by touch", () -> {
            SetupModel m = fresh();
            String text = m.detail(SetupModel.STEP_PLAY);
            expectContains(text, "comes on two discs", "what the game is on");
            expectContains(text, "copies the play disc (Disc 2) from an image file or a folder on this device", "what this page does");
            expectContains(text, "cannot read the install disc (Disc 1) yet, so it cannot finish an install by itself", "what it cannot do, before any time is spent");
            expectContains(text, "to play now, use the install kit", "the way that works");
            expectContains(text, "A: choose your play disc.", "the next step");
            expectEquals("A Select   B Back", m.footer(), "prompts on a step that can be chosen");
            expectFits(fresh(), "fresh");
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.OVERLAY_SOURCE, m.overlay(), "the question is open");
            expectEquals(Arrays.asList("Image file", "Folder", "Cancel"), Arrays.asList(m.overlayButtons()), "buttons");
            expectEquals(SetupModel.BUTTON_IMAGE, m.overlayFocus(), "focus starts on the image file");
            expectContains(m.overlayText(), "one .iso file of the whole disc", "what an image file is");
            expectContains(m.overlayText(), "a folder that holds the disc's files (default.xex and the .rpf files)", "what a folder is");
            expectContains(m.overlayText(), "if the controller does not move in it, use the touch screen", "the chooser is the system's: what to do if the controller fails there");
            expectEquals("A Select   B Cancel   Left/Right Choose", m.footer(), "prompts of the question");
            expectFits(m, "source question");
            press(m, NavInput.LEFT);
            expectEquals(SetupModel.BUTTON_IMAGE, m.overlayFocus(), "LEFT stops at the first button");
            press(m, NavInput.RIGHT, NavInput.RIGHT, NavInput.RIGHT);
            expectEquals(SetupModel.BUTTON_CANCEL, m.overlayFocus(), "RIGHT stops at the last button");
            press(m, NavInput.DOWN);
            expectEquals(SetupModel.STEP_PLAY, m.focused(), "the list does not move under an open question");
            press(m, NavInput.LEFT, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_PICK_PLAY_FOLDER, m.takeAction(), "Folder");
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "an action is handed out once");
            expectEquals(SetupModel.OVERLAY_NONE, m.overlay(), "the question closed");
            press(m, NavInput.CONFIRM, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_PICK_PLAY_IMAGE, m.takeAction(), "Image file");
            press(m, NavInput.CONFIRM, NavInput.RIGHT, NavInput.RIGHT, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "Cancel does nothing");
            press(m, NavInput.CONFIRM);
            expect(m.nav(NavInput.BACK, false), "B inside the question is handled");
            expectEquals(SetupModel.OVERLAY_NONE, m.overlay(), "and closes it");
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "without an action");
            press(m, NavInput.CONFIRM);
            m.overlayTap(SetupModel.BUTTON_FOLDER);
            expectEquals(SetupModel.ACT_PICK_PLAY_FOLDER, m.takeAction(), "a tap on Folder");
            m.overlayTap(SetupModel.BUTTON_IMAGE);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "a tap with no question open does nothing");
        });

        check("model: a file chooser that returns nothing (closed, or none on the device) is said, with what can be done", () -> {
            SetupModel m = fresh();
            press(m, NavInput.CONFIRM, NavInput.CONFIRM);
            m.takeAction();
            m.nothingChosen(SetupModel.STEP_PLAY);
            String text = m.detail(SetupModel.STEP_PLAY);
            expectContains(text, "Nothing was chosen.", "the fact");
            expectContains(text, "A: open the file chooser again.", "the next step");
            expectContains(text, "If no file chooser opened, this device has none", "why A may seem to do nothing");
            expectContains(text, "the install kit is the way to install", "the way that works then");
            expectEquals("To do", m.stateWord(SetupModel.STEP_PLAY), "still to do, not a failure");
            expect(!m.detail(SetupModel.STEP_IMPORT).contains("Nothing was chosen"), "it is said at the step it belongs to only");
            expectFits(m, "nothing chosen");
            m = fresh();
            m.nothingChosen(SetupModel.STEP_PLAY);
            m.identifyStarted();
            expect(!m.detail(SetupModel.STEP_PLAY).contains("Nothing was chosen"), "gone once something was chosen");
            m.playDiscIdentified(new DiscIdentifier.Result(DiscIdentifier.Kind.NOT_A_GAME_DISC, null, "FOUND.", "DO THIS."));
            m.nothingChosen(SetupModel.STEP_PLAY);
            expectEquals("FOUND. DO THIS. A: choose again.", m.detail(SetupModel.STEP_PLAY), "after a result, a closed chooser leaves that result standing");
            // the same at the import step
            SetupModel i = new SetupModel();
            i.setDisk(disk(10, 10, false));
            i.nothingChosen(SetupModel.STEP_IMPORT);
            expectContains(i.detail(SetupModel.STEP_IMPORT), "Nothing was chosen. A: open the file chooser again.", "at the import step");
            i.workStarted(SetupModel.STEP_IMPORT, 0);
            i.workFinished(VerifiedExtractor.Outcome.stopped());
            expect(!i.detail(SetupModel.STEP_IMPORT).contains("Nothing was chosen"), "gone once a file was chosen");
        });

        check("model: the install disc cannot be chosen in this version; it says what is needed and that the install kit does it", () -> {
            SetupModel m = fresh();
            m.focus(SetupModel.STEP_INSTALL);
            expectEquals("Not available", m.stateWord(SetupModel.STEP_INSTALL), "state word");
            String text = m.detail(SetupModel.STEP_INSTALL);
            expect(text.startsWith("Not available in this version: this app cannot read the install disc (Disc 1) yet."), "the reason comes first: " + text);
            expectContains(text, "The game needs that disc's files as well as the play disc's.", "what is needed");
            expectContains(text, "The install kit installs the whole game, both discs, from a computer with this device connected by USB.", "who does it, and how");
            expect(!m.actionable(SetupModel.STEP_INSTALL) && !m.detailIsProblem(SetupModel.STEP_INSTALL), "shown as unavailable, not as a failure");
            expectEquals("B Back", m.footer(), "A is not offered on it");
            press(m, NavInput.CONFIRM);
            expect(m.overlay() == SetupModel.OVERLAY_NONE && m.takeAction() == SetupModel.ACT_NONE, "CONFIRM does nothing");
        });

        check("model: copying starts only after the play disc was recognised; importing only after the copy", () -> {
            SetupModel m = fresh();
            m.focus(SetupModel.STEP_COPY);
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "no disc yet");
            expectEquals("Choose your play disc first (step 1). It needs 10.6 GB of free space; this device has 20.0 GB free.",
                    m.detail(SetupModel.STEP_COPY), "says what is missing, and already what the copy will need");
            expectEquals("B Back", m.footer(), "A is not offered on a step that cannot start");
            m.focus(SetupModel.STEP_IMPORT);
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "no copy yet");
            expectEquals("Copy the game files first (step 3).", m.detail(SetupModel.STEP_IMPORT), "says what is missing");
            expectEquals("Finish the steps above first.", m.detail(SetupModel.STEP_DONE), "the last step too");

            m = identified();
            expectEquals("Done", m.stateWord(SetupModel.STEP_PLAY), "step 1 is done");
            expectEquals("Recognised: Test release, the play disc. Next: step 3 copies its files. A: choose a different disc copy.",
                    m.detail(SetupModel.STEP_PLAY), "the result and the next step");
            expectEquals(SetupModel.STEP_COPY, m.focused(), "the focus moved on to the copy");
            String text = m.detail(SetupModel.STEP_COPY);
            expectContains(text, "copies the play disc's files to this device and checks each one against the supported disc", "what it does");
            expectContains(text, "It needs 10.6 GB of free space; this device has 20.0 GB free.", "required and free space are shown first");
            expectContains(text, "B stops it at any time: finished files are kept. A: start.", "that it can be stopped, and how it starts");
            expectFits(identified(), "disc recognised");
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_START_COPY, m.takeAction(), "now the copy starts");
            // While the copy runs, step 1 names no button: A does nothing there (the row is grey, the prompt is Stop only).
            m.workStarted(SetupModel.STEP_COPY, 1_000);
            m.focus(SetupModel.STEP_PLAY);
            expectEquals("Recognised: Test release, the play disc. Step 3 is copying its files.", m.detail(SetupModel.STEP_PLAY),
                    "step 1 while the copy runs");
            expect(!m.actionable(SetupModel.STEP_PLAY), "(A does nothing on step 1 meanwhile)");
            press(m, NavInput.CONFIRM);
            expect(m.overlay() == SetupModel.OVERLAY_NONE && m.takeAction() == SetupModel.ACT_NONE, "(and A asks and starts nothing)");
            m.focus(SetupModel.STEP_IMPORT);
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "still no import before the copy is done");
        });

        check("model: a disc that is not recognised is a problem, shown with what was found and what to do, and can be chosen again", () -> {
            SetupModel m = fresh();
            press(m, NavInput.CONFIRM, NavInput.CONFIRM);
            m.takeAction();
            m.identifyStarted();
            expectEquals("Working", m.stateWord(SetupModel.STEP_PLAY), "while reading");
            expectContains(m.detail(SetupModel.STEP_PLAY), "Reading your disc copy and checking that it is the supported game", "what is going on");
            expectEquals("B Back", m.footer(), "only B, and it leaves");
            expect(!m.nav(NavInput.BACK, false), "B leaves while the disc is read: nothing is written, and a source that never answers must not hold the player");
            expectEquals(SetupModel.OVERLAY_NONE, m.overlay(), "and asks nothing");
            press(m, NavInput.CONFIRM);
            expect(m.overlay() == SetupModel.OVERLAY_NONE && m.takeAction() == SetupModel.ACT_NONE, "A starts nothing while the disc is read");
            expectFits(m, "reading the disc");
            m = fresh();
            choose(m, new DiscIdentifier.Result(DiscIdentifier.Kind.UNSUPPORTED_VERSION, "cd",
                    "This is not the play disc this version supports (its game program has check code 0123456789ab).", "Choose Disc 2 instead."));
            expectEquals("Problem", m.stateWord(SetupModel.STEP_PLAY), "state");
            expect(m.detailIsProblem(SetupModel.STEP_PLAY), "drawn as a problem");
            expectEquals("This is not the play disc this version supports (its game program has check code 0123456789ab). Choose Disc 2 instead."
                    + " A: choose again.", m.detail(SetupModel.STEP_PLAY), "what was found, what to do, and the button");
            expectEquals(SetupModel.STEP_PLAY, m.focused(), "the focus stays");
            m.focus(SetupModel.STEP_COPY);
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "no copy from an unrecognised disc");
            m.focus(SetupModel.STEP_PLAY);
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.OVERLAY_SOURCE, m.overlay(), "the disc can be chosen again");
        });

        check("model: while copying, B asks before stopping; Stop stops, Continue and B go on; a stopped copy says what is kept", () -> {
            SetupModel m = identified();
            press(m, NavInput.CONFIRM);
            m.takeAction();
            m.workStarted(SetupModel.STEP_COPY, 1000);
            expect(m.busy(), "busy");
            expectEquals("Working", m.stateWord(SetupModel.STEP_COPY), "state");
            expectEquals("B Stop", m.footer(), "B is named for what it does now; A does nothing and is not offered");
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "CONFIRM starts nothing while working");
            expect(m.nav(NavInput.BACK, false), "B is handled, the screen is not left");
            expectEquals(SetupModel.OVERLAY_STOP, m.overlay(), "the question");
            expectEquals(Arrays.asList("Stop", "Continue"), Arrays.asList(m.overlayButtons()), "buttons");
            expectEquals(SetupModel.BUTTON_STOP, m.overlayFocus(), "focus on Stop");
            expectEquals("Stop copying? The files that are finished are kept. The file that is being copied now starts again next time.",
                    m.overlayText(), "the question says exactly what a stop costs");
            expectFits(m, "stop copying?");
            expect(m.nav(NavInput.BACK, false), "B inside the question");
            expectEquals(SetupModel.OVERLAY_NONE, m.overlay(), "means Continue");
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "nothing stops");
            press(m, NavInput.BACK, NavInput.RIGHT, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "Continue: nothing stops");
            expect(m.busy(), "still working");
            press(m, NavInput.BACK, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_STOP_WORK, m.takeAction(), "Stop");
            expectContains(m.detail(SetupModel.STEP_COPY), "Stopping", "shown while it winds down");
            expectEquals("B Back", m.footer(), "while it winds down B leaves");
            expect(!m.nav(NavInput.BACK, false), "B leaves while the work winds down: a copy that never answers must not hold the player");
            expectEquals(SetupModel.OVERLAY_NONE, m.overlay(), "without asking again");
            m.setDisk(disk(3, 10, false));
            m.workFinished(VerifiedExtractor.Outcome.stopped());
            expect(!m.busy(), "no longer busy");
            expectEquals("To do", m.stateWord(SetupModel.STEP_COPY), "back to To do");
            expectEquals("Stopped. 3 of 10 files are already copied and checked. The file that was being copied starts again."
                    + " It needs 7.6 GB of free space; this device has 20.0 GB free. A: continue.", m.detail(SetupModel.STEP_COPY),
                    "what is kept, what is not, what the rest needs, and the button");
            expect(!m.nav(NavInput.BACK, false), "now B leaves");
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_START_COPY, m.takeAction(), "A continues");
            m.workStarted(SetupModel.STEP_COPY, 0);
            m.workFinished(VerifiedExtractor.Outcome.done());                       // (the device still says 3 of 10)
            expect(!m.detail(SetupModel.STEP_COPY).contains("Stopped"), "'Stopped' is said for the last run only");
        });

        check("model: a copy that failed says what happened and the next step for that kind of failure", () -> {
            Map<VerifiedExtractor.Problem, String[]> next = new LinkedHashMap<VerifiedExtractor.Problem, String[]>();
            next.put(VerifiedExtractor.Problem.DISC_COPY_BAD, new String[] {"Your disc copy is damaged or is not the supported disc.",
                    "Make a new copy of the play disc and choose it in step 1."});
            next.put(VerifiedExtractor.Problem.SOURCE_UNREADABLE, new String[] {"Check that the storage holding your disc copy is still connected, then press A to continue.",
                    "If it stops at the same file again, choose the disc copy again in step 1."});
            next.put(VerifiedExtractor.Problem.STORAGE, new String[] {"Press A to try again."});
            next.put(VerifiedExtractor.Problem.BUSY, new String[] {"Wait a moment, then press A again."});
            next.put(VerifiedExtractor.Problem.OTHER, new String[] {"Press A to try again."});
            next.put(VerifiedExtractor.Problem.EXISTING_INSTALL, new String[] {"This page does not replace what is in place.", "Install the game with the install kit."});
            expectEquals(COPY_PROBLEMS.length, next.size(), "every kind a copy can end with is in the table");
            for (VerifiedExtractor.Problem kind : COPY_PROBLEMS) {
                SetupModel m = copyEnded(VerifiedExtractor.Outcome.failed(kind, "WHAT HAPPENED (sfx/x.rpf)."), 4);
                expectEquals("Problem", m.stateWord(SetupModel.STEP_COPY), kind + ": state");
                expect(m.detailIsProblem(SetupModel.STEP_COPY), kind + ": drawn as a problem");
                String text = m.detail(SetupModel.STEP_COPY);
                expect(text.startsWith("WHAT HAPPENED (sfx/x.rpf). "), kind + ": the work's own sentence, which names the file, comes first: " + text);
                for (String part : next.get(kind)) expectContains(text, part, kind + ": the next step");
                boolean keeps = kind != VerifiedExtractor.Problem.BUSY && kind != VerifiedExtractor.Problem.EXISTING_INSTALL;
                expectEquals(keeps, text.contains("The 4 files already copied are kept."), kind + ": whether it says that finished files are kept");
                for (Map.Entry<VerifiedExtractor.Problem, String[]> other : next.entrySet()) {
                    if (other.getValue()[0].equals(next.get(kind)[0])) continue;
                    expect(!text.contains(other.getValue()[0]), kind + ": not the next step of " + other.getKey());
                }
                press(m, NavInput.CONFIRM);
                expectEquals(SetupModel.ACT_START_COPY, m.takeAction(), kind + ": A tries again");
                expectFits(m, "copy failed: " + kind);
            }
            SetupModel none = copyEnded(VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.DISC_COPY_BAD, "WHAT HAPPENED."), 0);
            expect(!none.detail(SetupModel.STEP_COPY).contains("already copied are kept"), "with no file copied nothing is said to be kept");
            expectEquals(SetupModel.OVERLAY_NONE, none.overlay(), "no question is open");
            // an open "Stop copying?" closes when the work ends by itself
            SetupModel asked = identified();
            press(asked, NavInput.CONFIRM);
            asked.takeAction();
            asked.workStarted(SetupModel.STEP_COPY, 0);
            asked.nav(NavInput.BACK, false);
            asked.workFinished(VerifiedExtractor.Outcome.failed("WHAT HAPPENED."));
            expectEquals(SetupModel.OVERLAY_NONE, asked.overlay(), "an open question closes when the work ends");
        });

        check("model: a finished copy moves on to the import and says the disc copy is not needed again", () -> {
            SetupModel m = copyEnded(VerifiedExtractor.Outcome.done(), 10);
            expectEquals("Done", m.stateWord(SetupModel.STEP_COPY), "done");
            expectEquals("All 10 files of the play disc are on this device; each one matched the supported disc when it was copied.", m.detail(SetupModel.STEP_COPY),
                    "what is true now (the bytes were checked on their way in; the copy is not read back)");
            expectEquals(SetupModel.STEP_IMPORT, m.focused(), "the focus moved on to the import");
            expectEquals("All 10 files of the play disc are on this device. The image file or folder you chose is not needed by this page again.",
                    m.detail(SetupModel.STEP_PLAY), "step 1 says the disc copy is not needed");
            expect(!m.actionable(SetupModel.STEP_PLAY), "and cannot be chosen again");
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_PICK_BUNDLE, m.takeAction(), "the bundle chooser is asked for");
            m.focus(SetupModel.STEP_COPY);
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "a finished copy is not started again");
        });

        check("model: 'Done' is said only for what is really in place", () -> {
            // the work says done, the device says one file is missing
            SetupModel m = copyEnded(VerifiedExtractor.Outcome.done(), 9);
            expectEquals("To do", m.stateWord(SetupModel.STEP_COPY), "not Done");
            expectContains(m.detail(SetupModel.STEP_COPY), "9 of 10 files are already copied and checked.", "it says how far it is");
            SetupModel i = importEnded(VerifiedExtractor.Outcome.done(), false);
            expectEquals("To do", i.stateWord(SetupModel.STEP_IMPORT), "an import whose result is not on the device is not Done");
        });

        check("model: too little free space is said before the disc is looked for, and at the copy with how much to free", () -> {
            // 8 GB to copy and 8 GB free: the reserve of 512 MiB is missing
            SetupModel m = new SetupModel();
            m.setDisk(new SetupModel.Disk(null, 0, 10, 8 * GB, 8 * GB, false, false));
            expectContains(m.detail(SetupModel.STEP_PLAY), "This device has too little free space for the copy: 8.6 GB are needed and 8.0 GB are free (see step 3).",
                    "step 1, before the player goes looking for the disc");
            expectEquals("Choose your play disc first (step 1). Not enough free space on this device: 8.6 GB are needed and 8.0 GB are free."
                    + " Free at least 0.6 GB (remove apps or files you no longer need), before you start the copy.", m.detail(SetupModel.STEP_COPY), "step 3 before the disc is chosen");
            expectEquals("To do", m.stateWord(SetupModel.STEP_COPY), "not yet a problem of this step: the disc comes first");
            expectFits(m, "fresh, too little space");
            choose(m, supported());
            expectEquals("Problem", m.stateWord(SetupModel.STEP_COPY), "with the disc chosen, the space is what stands in the way");
            expect(m.detailIsProblem(SetupModel.STEP_COPY), "drawn as a problem");
            expectEquals("Not enough free space on this device: 8.6 GB are needed and 8.0 GB are free. Free at least 0.6 GB (remove apps or"
                    + " files you no longer need), then press A to try again.", m.detail(SetupModel.STEP_COPY), "the need rounded up, the free amount down, and how much to free");
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_START_COPY, m.takeAction(), "A tries: the copy looks at the space itself, the page's figure may be old");
            // enough room exactly: no warning anywhere
            SetupModel ok = new SetupModel();
            ok.setDisk(new SetupModel.Disk(null, 0, 10, 8 * GB, 8 * GB + VerifiedExtractor.RESERVE_BYTES, false, false));
            expect(!ok.detail(SetupModel.STEP_PLAY).contains("too little free space"), "exactly enough: no warning at step 1");
            choose(ok, supported());
            expectEquals("To do", ok.stateWord(SetupModel.STEP_COPY), "exactly enough: the copy can start");
        });

        check("model: a copy that ran out of space says so with the amounts of this moment, and when there is room again", () -> {
            SetupModel m = identified();
            press(m, NavInput.CONFIRM);
            m.takeAction();
            m.workStarted(SetupModel.STEP_COPY, 0);
            m.setDisk(new SetupModel.Disk(null, 4, 10, 6 * GB, 2 * GB, false, false));
            m.workFinished(VerifiedExtractor.Outcome.noSpace("This device ran out of space while copying sfx/x.rpf", 3 * GB, 1));
            expectEquals("Problem", m.stateWord(SetupModel.STEP_COPY), "state");
            expectEquals("The copy stopped. Not enough free space on this device: 6.6 GB are needed and 2.0 GB are free. Free at least 4.6 GB"
                    + " (remove apps or files you no longer need), then press A to continue. The 4 files already copied are kept.",
                    m.detail(SetupModel.STEP_COPY), "what the whole rest needs now, not the figures of the failed file");
            expectFits(m, "copy out of space");
            m.setDisk(new SetupModel.Disk(null, 4, 10, 6 * GB, 9 * GB, false, false));       // the player made room and came back
            expectEquals("The copy stopped for lack of free space. There is enough now. It needs 6.6 GB of free space; this device has 9.0 GB free."
                    + " A: continue. The 4 files already copied are kept.", m.detail(SetupModel.STEP_COPY), "with room again it says so");
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_START_COPY, m.takeAction(), "and A continues");
        });

        check("model: opened again after an interruption, the page says what is kept and that the disc is chosen again", () -> {
            SetupModel m = new SetupModel();
            m.setDisk(disk(3, 10, false));
            expectEquals(SetupModel.STEP_PLAY, m.focused(), "it opens at step 1: the disc copy has to be chosen again");
            expectEquals("3 of 10 files of the play disc are already on this device and checked. To copy the rest, choose the same disc copy again:"
                    + " its image file or its folder. A: choose.", m.detail(SetupModel.STEP_PLAY), "step 1");
            expectEquals("3 of 10 files are already copied and checked. To copy the rest, choose your play disc again (step 1)."
                    + " It needs 7.6 GB of free space; this device has 20.0 GB free.", m.detail(SetupModel.STEP_COPY), "step 3");
            expect(!m.actionable(SetupModel.STEP_COPY), "the copy cannot continue before the disc is chosen");
            expectFits(m, "opened again, 3 of 10");
            choose(m, supported());
            expectEquals("3 of 10 files are already copied and checked. It needs 7.6 GB of free space; this device has 20.0 GB free. A: continue.",
                    m.detail(SetupModel.STEP_COPY), "with the disc chosen: continue");
            // one file is "is", not "are", in each of the three sentences that count files
            SetupModel one = new SetupModel();
            one.setDisk(disk(1, 10, false));
            expectContains(one.detail(SetupModel.STEP_PLAY), "1 of 10 files of the play disc is already on this device and checked.", "one file, step 1");
            expectContains(one.detail(SetupModel.STEP_COPY), "1 of 10 files is already copied and checked.", "one file, step 3");
            expectContains(copyEnded(VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.DISC_COPY_BAD, "WHAT HAPPENED."), 1).detail(SetupModel.STEP_COPY),
                    "The 1 file already copied is kept.", "one file, after a failure");
        });

        check("model: import: what it needs, the stop question, each kind of failure with its next step, success", () -> {
            SetupModel m = new SetupModel();
            m.setDisk(disk(10, 10, false));
            expectEquals(SetupModel.STEP_IMPORT, m.focused(), "with the files copied the screen opens at the import step");
            String text = m.detail(SetupModel.STEP_IMPORT);
            expectContains(text, "has to be rebuilt for this device", "why game code is needed at all");
            expectContains(text, "done on a computer, from your own disc", "where it comes from");
            expectContains(text, "writes one file, the code bundle (its name normally ends in .bundle)", "what to look for, as the build tool names it");
            expectContains(text, "Copy that file to this device, then press A and choose it.", "the next step");
            expectEquals("To do", m.stateWord(SetupModel.STEP_DONE), "the last step is not done yet");
            expectFits(m, "import to do");
            m.focus(SetupModel.STEP_DONE);
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_NONE, m.takeAction(), "nothing to leave with yet");
            m.focus(SetupModel.STEP_IMPORT);
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_PICK_BUNDLE, m.takeAction(), "choose the bundle");
            m.workStarted(SetupModel.STEP_IMPORT, 0);
            press(m, NavInput.BACK);
            expectEquals("Stop the import? Nothing of it is kept: it starts again from the beginning next time.", m.overlayText(), "the question fits the import");
            expectFits(m, "stop the import?");
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_STOP_WORK, m.takeAction(), "Stop");
            m.workFinished(VerifiedExtractor.Outcome.stopped());
            expectEquals("Stopped: nothing was imported. A: choose the bundle file again.", m.detail(SetupModel.STEP_IMPORT), "after a stop");

            Map<VerifiedExtractor.Problem, String> next = new LinkedHashMap<VerifiedExtractor.Problem, String>();
            next.put(VerifiedExtractor.Problem.NOT_A_BUNDLE, "Choose the code bundle file that the build tool on your computer wrote. A: choose again.");
            next.put(VerifiedExtractor.Problem.BUNDLE_DAMAGED, "The file is incomplete or was changed after it was built. Copy it to this device again, then press A and choose it.");
            next.put(VerifiedExtractor.Problem.BUNDLE_OTHER, "Build the bundle again with the build tool that belongs to this app version, from the play disc you copied in step 3.");
            next.put(VerifiedExtractor.Problem.SOURCE_UNREADABLE, "Check that the storage holding the file is still connected, then press A and choose it again.");
            next.put(VerifiedExtractor.Problem.EXISTING_INSTALL, "This page does not replace what is in place. Install the game with the install kit.");
            next.put(VerifiedExtractor.Problem.STORAGE, "Press A to try again.");
            for (Map.Entry<VerifiedExtractor.Problem, String> e : next.entrySet()) {
                SetupModel f = importEnded(VerifiedExtractor.Outcome.failed(e.getKey(), "WHAT HAPPENED."), false);
                expectEquals("Problem", f.stateWord(SetupModel.STEP_IMPORT), e.getKey() + ": state");
                expectEquals("WHAT HAPPENED. " + e.getValue(), f.detail(SetupModel.STEP_IMPORT), e.getKey() + ": what happened, then the next step");
                press(f, NavInput.CONFIRM);
                expectEquals(SetupModel.ACT_PICK_BUNDLE, f.takeAction(), e.getKey() + ": another file can be chosen");
                expectFits(f, "import failed: " + e.getKey());
            }
            SetupModel space = importEnded(VerifiedExtractor.Outcome.noSpace("Not enough space", 900000001L, 399999999L), false);
            expectEquals("Not enough free space on this device: 1.0 GB are needed and 0.3 GB are free. Free at least 0.6 GB (remove apps or"
                    + " files you no longer need), then press A and choose the bundle file again.", space.detail(SetupModel.STEP_IMPORT),
                    "too little space for the bundle: the import's own amounts (the page cannot know a bundle's size)");

            SetupModel ok = importEnded(VerifiedExtractor.Outcome.done(), true);
            expectEquals("Done", ok.stateWord(SetupModel.STEP_IMPORT), "imported");
            expectEquals("The game code is on this device and checked.", ok.detail(SetupModel.STEP_IMPORT), "what is true now");
            expectEquals(SetupModel.STEP_DONE, ok.focused(), "the focus moved on to the last step");
        });

        check("model: the last step never says 'Done' for an install that cannot start; it says why and names the install kit", () -> {
            SetupModel m = importEnded(VerifiedExtractor.Outcome.done(), true);
            expectEquals("Not available", m.stateWord(SetupModel.STEP_DONE), "this version cannot finish: not 'Done'");
            String text = m.detail(SetupModel.STEP_DONE);
            expectContains(text, "The play disc's files and the game code are on this device.", "what is in place");
            expectContains(text, "That is all this page can install in this version", "the limit");
            expectContains(text, "without the install disc's files the game does not start from this install", "the consequence");
            expectContains(text, "To play now, use the install kit.", "the way that works");
            expectContains(text, "A or B: back to the main menu.", "the buttons");
            expect(m.actionable(SetupModel.STEP_DONE), "the row can be chosen");
            press(m, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_LEAVE, m.takeAction(), "A on the last step leaves");
            expectFits(m, "all this page can do");

            SetupModel reopened = new SetupModel();
            reopened.setDisk(disk(10, 10, true));
            expectEquals(SetupModel.STEP_DONE, reopened.focused(), "a page with everything in place opens at the last step");

            // a catalog that covers every disc (a later version): now it is done, and Play is the next step
            SetupModel whole = new SetupModel();
            whole.setDisk(new SetupModel.Disk(null, 10, 10, 0, 20 * GB, true, true));
            expectEquals("Done", whole.stateWord(SetupModel.STEP_DONE), "with every disc covered the last step is Done");
            expectEquals("Setup is complete. A or B: back to the main menu, then choose Play.", whole.detail(SetupModel.STEP_DONE), "and says what comes next");
            press(whole, NavInput.CONFIRM);
            expectEquals(SetupModel.ACT_LEAVE, whole.takeAction(), "A leaves");
        });

        check("model: progress text, bar and time left", () -> {
            SetupModel m = identified();
            expectEquals(-1f, m.progressFraction(), "no bar while nothing runs");
            press(m, NavInput.CONFIRM);
            m.takeAction();
            m.workStarted(SetupModel.STEP_COPY, 10000);
            expectEquals("Copying\u2026", m.detail(SetupModel.STEP_COPY), "before the first report");
            m.progress(2 * GB, 10 * GB, 2, 10, "sfx/audio_misc.rpf", 10000);      // 2 GB were already in place
            m.progress(4 * GB, 10 * GB, 2, 10, "sfx/audio_misc.rpf", 30000);      // 2 GB in 20 s, 6 GB left -> 60 s
            expectEquals("Copying sfx/audio_misc.rpf: 4.0 GB of 10.0 GB, file 3 of 10, about 1 min left.", m.detail(SetupModel.STEP_COPY),
                    "text");
            expectEquals(0.4f, m.progressFraction(), "bar");
            expectFits(m, "copying");
            SetupModel i = new SetupModel();
            i.setDisk(disk(10, 10, false));
            i.workStarted(SetupModel.STEP_IMPORT, 0);
            i.progress(100000000L, 400000000L, 1, 4, "lib/arm64-v8a/libgame.so", 500);
            expectEquals("Importing lib/arm64-v8a/libgame.so: 100 MB of 400 MB, file 2 of 4, estimating the time left.", i.detail(SetupModel.STEP_IMPORT),
                    "the import says what it is doing in its own word");
            expectEquals(60L, SetupModel.secondsLeft(2 * GB, 6 * GB, 20000), "seconds left from this run's speed");
            expectEquals(-1L, SetupModel.secondsLeft(2 * GB, 6 * GB, 1999), "too early to say (time)");
            expectEquals(-1L, SetupModel.secondsLeft((16L << 20) - 1, 6 * GB, 20000), "too early to say (bytes)");
            expectEquals(0L, SetupModel.secondsLeft(0, 0, 0), "nothing left");
            expectEquals("estimating the time left", SetupModel.formatTimeLeft(-1), "unknown");
            expectEquals("less than a minute left", SetupModel.formatTimeLeft(59), "under a minute");
            expectEquals("about 1 min left", SetupModel.formatTimeLeft(60), "a minute");
            expectEquals("about 3 min left", SetupModel.formatTimeLeft(121), "rounded up");
            expectEquals("8.1 GB", SetupModel.formatBytes(8146504708L), "gigabytes");
            expectEquals("26 MB", SetupModel.formatBytes(26628096L), "megabytes");
            expectEquals("1 KB", SetupModel.formatBytes(1028L), "kilobytes");
        });

        check("model: looking at the device reads only; it reports files in place, the bundle, what closes the setup, and room that a run frees itself", () -> {
            byte[] exe = bytes(5000, 101), other = bytes(700, 102);
            List<SetupCatalog.Item> items = Arrays.asList(item("default.xex", exe), item("common.rpf", other));
            SetupCatalog.Spec spec = SetupTestKit.spec(exe, items, true);
            File files = SetupTestKit.newFilesDir("model-inspect");
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 103));
            SetupModel.Disk empty = SetupModel.inspect(files, spec, SetupTestKit.spaceOf(123));
            expect(!empty.existing.any() && empty.filesPresent == 0 && empty.filesTotal == 2 && !empty.bundlePresent, "empty");
            expectEquals(5700L, empty.bytesMissing, "bytes missing");
            expectEquals(123L, empty.freeBytes, "free space");
            SetupTestKit.write(SetupTestKit.input(InstallGuard.root(files), items.get(0)), exe);
            SetupTestKit.write(new File(files, "game-install/bundle/manifest.json"), bytes(10, 104));
            SetupTestKit.write(new File(files, "game-install/staging/" + items.get(1).sha256 + ".part"), bytes(300, 106));   // a killed copy left this
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            SetupModel.Disk partly = SetupModel.inspect(files, spec, SetupTestKit.spaceOf(123));
            expect(partly.filesPresent == 1 && partly.bytesMissing == 700 && partly.bundlePresent && partly.coversAllDiscs, "partly");
            expectEquals(423L, partly.freeBytes, "the partial file a killed copy left counts as free: the next copy removes it first");
            SetupTestKit.write(new File(files, "private-trace/private-trace-live.so"), bytes(10, 105));
            before = SetupTestKit.snapshot(files);
            InstallGuard.Existing found = SetupModel.inspect(files, spec, SetupTestKit.spaceOf(123)).existing;
            expect(found.computerCode && !found.computerData, "game code put there from a computer closes the setup");
            expectEquals(before, SetupTestKit.snapshot(files), "nothing was created or changed");
        });

        check("layout: every text of the states above was laid out as the app does, in a face a tenth wider, and fits; so do the longest results", () -> {
            // the longest real results of the disc check and of a copy, with the app's own catalog label
            DiscIdentifier.Result notAnImage = DiscIdentifier.identifyImage(new SetupTestKit.MemoryDisc(0, bytes(200000, 14)));
            DiscIdentifier.Result otherDisc = DiscIdentifier.identifyFolder(new SetupTestKit.MemoryFolder().put("default.xex", bytes(100, 15)));
            DiscIdentifier.Result noProgram = DiscIdentifier.identifyFolder(new SetupTestKit.MemoryFolder());
            DiscIdentifier.Result recognised = new DiscIdentifier.Result(DiscIdentifier.Kind.SUPPORTED_PLAY_DISC, "ab",
                    "Recognised: " + SetupCatalog.LABEL + ", the play disc.", null);
            expect(notAnImage.kind == DiscIdentifier.Kind.NOT_A_GAME_DISC && otherDisc.kind == DiscIdentifier.Kind.UNSUPPORTED_VERSION
                    && noProgram.kind == DiscIdentifier.Kind.NOT_A_GAME_DISC, "the three results are the ones meant");
            for (DiscIdentifier.Result r : new DiscIdentifier.Result[] {notAnImage, otherDisc, noProgram, recognised}) {
                SetupModel m = new SetupModel();
                m.setDisk(new SetupModel.Disk(null, 0, 10, 8146504708L, 3 * GB, false, false));    // with too little space as well
                choose(m, r);
                expectFits(m, "disc check: " + r.kind);
            }
            String longest = "common/data/installpartitions.meta in your disc copy is not the file of the supported disc (it is longer than 1028 bytes).";
            expectFits(copyEnded(VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.DISC_COPY_BAD, longest), 9), "the longest copy failure");
            expectFits(copyEnded(VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.SOURCE_UNREADABLE,
                    "common/data/installpartitions.meta could not be read from your disc copy (read failed: EIO (Input/output error))."), 9),
                    "the longest read failure");
            expect(fitted >= 30, "the states of the cases above were laid out too: " + fitted);
            // the check can fail: a text a few sentences longer than any of the page's is cut
            SetupModel tooLong = copyEnded(VerifiedExtractor.Outcome.failed(VerifiedExtractor.Problem.DISC_COPY_BAD, longest + " " + longest + " " + longest), 9);
            boolean cut = false;
            try {
                expectFits(tooLong, "control");
            } catch (AssertionError expected) {
                cut = expected.getMessage().startsWith("cut off");
            }
            expect(cut, "control: a text three sentences longer is reported as cut off");
        });

        SetupTestKit.finish("SetupModelTest");
    }
}
