package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Run with java -ea. The two-disc setup page's model, driven through SetupDiscKit.Device (the screen's glue in pure
 * Java) over a temporary files directory, a made-up ship build (ShipTestKit.Version) and the synthetic discs. After
 * every event the Finish row is compared with the ship build's own rule, ShipReady.ready(), and every text of the page
 * is read for a developer's word, for a word or shape the handed-out build's screens must not show
 * (ShipWordsTest.forbiddenIn) and for a name of a file of the discs.
 */
public final class SetupDiscModelTest {
    static final int PLAY = SetupDiscModel.PLAY, INSTALL = SetupDiscModel.INSTALL, FINISH = SetupDiscModel.ROW_FINISH;

    /** Words that must not reach a player's screen. */
    static final String[] DEVELOPER_WORDS = {"bundle", "kit", "switch file", "private-trace", "check code", "debug", "v464", "content://",
            "sha", "uri", "staging", "pinned", "candidate", "stfs", "xdvdfs", "null"};

    static void agrees(SetupDiscKit.Device d) {
        expectEquals(d.ship().ready(), d.model.finishReady(), "the Finish row and ShipReady.ready() agree");
        expectEquals(d.ship().ready() ? SetupDiscModel.DONE : SetupDiscModel.TODO, d.model.state(FINISH) == SetupDiscModel.PROBLEM
                ? SetupDiscModel.TODO : d.model.state(FINISH), "Finish is Done exactly when ready");
        for (String text : d.texts()) {
            String lower = text.toLowerCase(Locale.ROOT);
            for (String word : DEVELOPER_WORDS) {
                if (word.equals("uri") ? lower.matches(".*\\buri\\b.*") : word.equals("sha") ? lower.matches(".*\\bsha.*") : lower.contains(word)) {
                    throw new AssertionError("a developer word (" + word + ") on the page: " + text);
                }
            }
            // The page is a screen of the build that is handed out: the same word rule as its other screens, and no
            // name of a file, folder or package of the discs (the lead's decision of 2026-10-06). The image's own
            // name (Disc2-Play.iso, the player's file) is not one of them.
            String forbidden = ShipWordsTest.forbiddenIn(text);
            if (forbidden != null) throw new AssertionError("a word or shape the handed-out build must not show (" + forbidden + ") on the page: " + text);
            for (String name : DISC_NAMES) {
                if (text.contains(name)) throw new AssertionError("a name of the disc (" + name + ") on the page: " + text);
            }
        }
    }

    /** Names of files, folders and packages of the synthetic discs (Disc2DiscsTest), and the game program's file name. */
    static final String[] DISC_NAMES = {Disc2DiscsTest.PKG, "content/", "default.xex", ".rpf", ".timestamp", "nxeart", "installpartitions", "sfx/",
            "part0", "part1", "part2", "part3"};
    static final String PKG0_PART0 = Disc2DiscsTest.PKG + "0/part0.rpf";

    /** The one row text the page shows for disc d after a failure, and the page as a whole passes agrees(). */
    static String failedRow(SetupDiscKit.Device d, int disc) {
        expectEquals("Problem", d.model.stateWord(disc), "the row is a problem");
        agrees(d);
        return d.model.detail(disc);
    }

    static String lastLine(List<String> log, String prefix) {
        String found = null;
        for (String line : log) {
            if (line.startsWith(prefix)) found = line;
        }
        if (found == null) throw new AssertionError("no log line begins <" + prefix + ">: " + log);
        return found;
    }

    static void copyToEnd(SetupDiscKit.Device d, int row, String uri) {
        d.nextChoice = uri;
        d.model.focus(row);
        d.press(NavInput.CONFIRM);   // opens the chooser; the image is recognised; the question asks to start
        expectEquals(SetupDiscModel.OVERLAY_START, d.model.overlay(), "the page asks to start");
        d.press(NavInput.CONFIRM);   // Start
    }

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("fresh: three rows, To Do; each disc row says what it copies, the space, and once that the chooser needs touch", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("fresh", true).open();
            SetupDiscModel m = d.model;
            expectEquals("1  Play Disc (Disc 2)", m.label(0), "row 1");
            expectEquals("2  Install Disc (Disc 1)", m.label(1), "row 2");
            expectEquals("3  Finish", m.label(2), "row 3");
            for (int r = 0; r < 3; ++r) expectEquals("To do", m.stateWord(r), "state " + r);
            expectEquals(PLAY, m.focused(), "focus on the play disc");
            String play = m.detail(PLAY);
            expectContains(play, "The play disc (Disc 2): choose its disc image (an .iso file) on this device. The game needs 4 files from it", "intro");
            expectContains(play, "On most devices the file chooser works with the touch screen only.", "touch, said plainly");
            expectEquals(1, count(play, "touch screen"), "said once");
            expectContains(m.detail(INSTALL), "The install disc (Disc 1): choose its disc image", "install intro");
            String finish = m.detail(FINISH);
            expectContains(finish, "The play disc's files are still missing: 4 files, ", "play missing");
            expectContains(finish, "The install disc's files are still missing: 8 files, ", "install missing");
            expectContains(finish, "plus room for one disc image at a time", "space for the whole install");
            expectEquals("A Select   B Back", m.footer(), "prompts");
            expect(!m.actionable(FINISH), "Finish does nothing yet");
            agrees(d);
        });

        check("play disc on its row: recognised, asked with space needed and free, copied, Done; the image may be deleted", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("play", true).open();
            String uri = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            d.nextChoice = uri;
            d.press(NavInput.CONFIRM);
            expectEquals(SetupDiscModel.OVERLAY_START, d.model.overlay(), "asked");
            String q = d.model.overlayText();
            expectContains(q, "Copy the play disc's files to this device? 4 files, ", "what");
            expectContains(q, "of free space (1 GB of it stays free for the system); this device has ", "need and free");
            expectEquals("[Start, Not now]", java.util.Arrays.toString(d.model.overlayButtons()), "answers");
            agrees(d);
            d.press(NavInput.CONFIRM);
            expect(d.lastCopy != null && d.lastCopy.complete, "copied");
            expectEquals("Done", d.model.stateWord(PLAY), "row 1 done");
            expectEquals(INSTALL, d.model.focused(), "focus moves to the next disc");
            String done = d.model.detail(PLAY);
            expectContains(done, "All 4 files the game needs from the play disc are on this device", "done text");
            expectContains(done, "You may now delete the image file Disc2-Play.iso to free ", "may be deleted");
            expectContains(done, "never deletes your files", "the app never deletes");
            expect(new SetupDiscStore(d.files).load(PLAY) == null, "the choice is forgotten once done");
            expectContains(d.model.detail(FINISH), "The install disc's files are still missing: 8 files", "finish names the rest");
            agrees(d);
        });

        check("the install disc chosen on the play row is accepted and serves row 2, with a sentence saying so", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("swap", true).open();
            String uri = d.document("disc1.iso", SetupDiscKit.installImage());
            d.nextChoice = uri;
            d.press(NavInput.CONFIRM);   // on row 1
            expectEquals(INSTALL, d.model.focused(), "focus on row 2");
            expectEquals(SetupDiscModel.OVERLAY_START, d.model.overlay(), "asked to copy it");
            expectContains(d.model.overlayText(), "You chose the install disc (Disc 1) on the row of the play disc (Disc 2). That is fine: it is used"
                    + " for row 2.", "the sentence");
            expectEquals("To do", d.model.stateWord(PLAY), "row 1 is untouched");
            d.press(NavInput.CONFIRM);
            expectEquals("Done", d.model.stateWord(INSTALL), "row 2 done");
            expectEquals(PLAY, d.model.focused(), "then back to the disc still needed");
            // and the reverse
            String play = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            d.nextChoice = play;
            d.model.focus(INSTALL);
            d.press(NavInput.CONFIRM);   // row 2 is done: A does nothing there
            expectEquals(0, countChoosers(d, 0), "no chooser on a done row");
            agrees(d);
        });

        check("the wrong image on a row: another game, not a disc, cut off, unreadable: each its sentence and a next step", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("wrong", true).open();
            String other = d.document("other.iso", Disc2DiscsTest.playImage(Disc2DiscsTest.OTHER_EXE, Disc2DiscsTest.COMMON).data);
            d.nextChoice = other;
            d.press(NavInput.CONFIRM);
            expectEquals("Problem", d.model.stateWord(PLAY), "problem");
            String t = d.model.detail(PLAY);
            expectContains(t, "This is not the play disc of Synthetic release, or its copy is damaged", "other game");
            expectContains(t, "choose the image of its play disc (Disc 2) or of its install disc (Disc 1)", "next step");
            expect(t.endsWith("A: choose again."), "A");
            expect(d.model.detailIsProblem(PLAY), "drawn as a problem");
            agrees(d);   // no developer word in the other-game sentence
            String text = d.document("notes.txt", new SetupTestKit.MemoryDisc(0, "just a text file, not a disc image".getBytes("UTF-8")));
            d.nextChoice = text;
            d.press(NavInput.CONFIRM);
            // setup-fixes (C4-2): what is expected is this row's disc image, said once
            expectContains(d.model.detail(PLAY), SetupDiscModel.expectedImage(PLAY), "not a disc: choose the play disc's image");
            byte[] whole = SetupDiscKit.installImage();
            String cut = d.document("cut.iso", java.util.Arrays.copyOf(whole, whole.length / 2));
            d.nextChoice = cut;
            d.press(NavInput.CONFIRM);
            expectContains(d.model.detail(PLAY), "This disc image is incomplete", "cut off");
            expectContains(d.model.detail(PLAY), Disc2Discs.NEXT_NEW_COPY, "make a new copy");
            d.nextChoice = "content://test.documents/document/vanished";
            d.press(NavInput.CONFIRM);
            expectContains(d.model.detail(PLAY), "The file you chose could not be opened", "unreadable");
            expectContains(d.model.detail(PLAY), Disc2Discs.NEXT_RECONNECT, "reconnect");
            d.nextChoice = null;
            d.press(NavInput.CONFIRM);
            expectEquals("Problem", d.model.stateWord(PLAY), "closing the chooser changes nothing");
            expectContains(d.model.detail(PLAY), "The file you chose could not be opened", "the last answer stays");
            agrees(d);
        });

        check("nothing chosen: the row says so and what that means; A opens the chooser again", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("nothing", true).open();
            d.nextChoice = null;
            d.press(NavInput.CONFIRM);
            expectContains(d.model.detail(PLAY), "Nothing was chosen. If no file chooser opened, this device has none", "said");
            expectEquals("To do", d.model.stateWord(PLAY), "still to do");
            int before = d.choosersOpened;
            d.press(NavInput.CONFIRM);
            expectEquals(before + 1, d.choosersOpened, "A opens it again");
        });

        check("an install disc without its fourth package: refused before copying, with how many files it lacks", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("lacking", true).open();
            byte[][] p = Disc2DiscsTest.packages();
            d.nextChoice = d.document("disc1.iso", Disc2DiscsTest.installImage(new byte[][] {p[0], p[1], p[2]}).data);
            d.model.focus(INSTALL);
            d.press(NavInput.CONFIRM);
            expectEquals("Problem", d.model.stateWord(INSTALL), "problem");
            expectContains(d.model.detail(INSTALL), "But this image lacks 2 of the 8 files the game needs from the install disc", "how many");
            for (byte[] f : SetupDiscKit.INSTALL_FILES) {
                expect(!new File(d.trace(), "inputs/" + SetupTestKit.sha256(f) + ".bin").exists(), "nothing copied");
            }
            expect(!new File(d.trace(), SetupDiscCopy.STAGING).exists(), "not even the staging folder was made");
        });

        check("too little space: the row says what is needed and free and how much to free; A looks again", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("space", true);
            d.space = SetupTestKit.spaceOf(1000);
            d.open();
            expectContains(d.model.detail(PLAY), "Not enough free space on this device:", "said before choosing");
            d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            d.press(NavInput.CONFIRM);
            expectEquals(SetupDiscModel.OVERLAY_NONE, d.model.overlay(), "no question without room");
            expectEquals("Problem", d.model.stateWord(PLAY), "problem");
            expectContains(d.model.detail(PLAY), "Recognised: the play disc (Disc 2). Not enough free space on this device:", "space");
            expectContains(d.model.detail(PLAY), "then press A to look again", "next");
            d.space = SetupTestKit.PLENTY;
            d.press(NavInput.CONFIRM);   // looks again
            expectEquals("Ready", d.model.stateWord(PLAY), "ready now");
            d.press(NavInput.CONFIRM);
            expectEquals(SetupDiscModel.OVERLAY_START, d.model.overlay(), "asked");
        });

        check("stop and continue: B asks, Stop keeps finished files; the row offers Continue; A goes on without choosing again", () -> {
            final SetupDiscKit.Device d = new SetupDiscKit.Device("stop", true).open();
            String uri = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            final boolean[] stopped = {false};
            d.copyProgress = new SetupDiscCopy.Progress() {
                @Override public void update(long done, long total, int index, int count, String name) {
                    d.model.progress(done, total, index, count, name, 0);
                    if (index == 1 && !stopped[0]) {
                        // the player presses B during the second file, then A on "Stop"
                        expect(d.model.nav(NavInput.BACK, false), "B is taken");
                        expectEquals(SetupDiscModel.OVERLAY_STOP, d.model.overlay(), "the stop question");
                        expectEquals(SetupDiscModel.TEXT_STOP, d.model.overlayText(), "its text");
                        d.model.nav(NavInput.CONFIRM, false);
                        expectEquals(SetupDiscModel.ACT_STOP, d.model.takeAction(), "stop");
                        expectEquals("B Back", d.model.footer(), "winding down");
                        stopped[0] = true;
                    }
                }

                @Override public boolean cancelled() {
                    return stopped[0];
                }
            };
            copyToEnd(d, PLAY, uri);
            expect(d.lastCopy.stopped, "stopped");
            expectEquals("Continue", d.model.stateWord(PLAY), "Continue");
            expectContains(d.model.detail(PLAY), "Stopped. 1 of 4 files of the play disc is on this device. The copy can go on from Disc2-Play.iso", "text");
            d.copyProgress = null;
            int choosers = d.choosersOpened;
            d.press(NavInput.CONFIRM);
            expectEquals(choosers, d.choosersOpened, "no chooser");
            expectEquals("Done", d.model.stateWord(PLAY), "done");
            agrees(d);
        });

        check("killed in the middle of a file: after the restart the row offers Continue, the half file is removed, the copy ends", () -> {
            final SetupDiscKit.Device d = new SetupDiscKit.Device("killed", true).open();
            String uri = d.document("disc1.iso", SetupDiscKit.installImage());
            final File part = new File(new File(d.trace(), SetupDiscCopy.STAGING), SetupDiscCopy.PART);
            final File saved = new File(d.files.getParentFile(), "half.part");
            d.copyProgress = new SetupDiscCopy.Progress() {
                @Override public void update(long done, long total, int index, int count, String name) {
                    if (index == 1 && part.isFile() && part.length() > 0 && !saved.exists()) {
                        try {
                            Files.copy(part.toPath(), saved.toPath());   // what the storage holds at the moment of the kill
                        } catch (IOException e) {
                            throw new IllegalStateException(e);
                        }
                    }
                }

                @Override public boolean cancelled() {
                    return saved.exists();
                }
            };
            copyToEnd(d, INSTALL, uri);
            expect(saved.exists(), "the copy was inside its second file");
            Files.move(saved.toPath(), part.toPath(), StandardCopyOption.REPLACE_EXISTING);   // the kill skipped the clean-up
            d.copyProgress = null;
            d.restart();
            expectEquals(INSTALL, d.model.focused(), "focus on the unfinished disc");
            expectEquals("Continue", d.model.stateWord(INSTALL), "Continue after the restart");
            expectContains(d.model.detail(INSTALL), "1 of 8 files of the install disc is on this device. The copy can go on from disc1.iso", "text");
            expect(SetupDiscCopy.leftoverBytes(d.trace()) > 0, "the half file is there");
            d.press(NavInput.CONFIRM);
            expect(d.lastCopy.complete, "complete");
            expect(d.lastCopy.leftoversRemoved > 0, "the half file was removed first");
            expectEquals("Done", d.model.stateWord(INSTALL), "done");
            expect(contains(d.log, "COPY done disc=install") && contains(d.log, "left_overs=" + d.lastCopy.leftoversRemoved), "logged");
            expect(contains(d.log, "COPY start disc=install in_place=1/8 "), "the continue is visible in the log");
            agrees(d);
        });

        check("document gone: Continue finds the remembered image unreadable, says so, forgets it, and asks again", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("gone", true).open();
            String uri = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            d.nextChoice = uri;
            d.press(NavInput.CONFIRM);
            d.press(NavInput.BACK);   // "Not now" by B: the question closes
            expectEquals(SetupDiscModel.OVERLAY_NONE, d.model.overlay(), "closed");
            d.restart();
            expectEquals("Continue", d.model.stateWord(PLAY), "remembered");
            d.documents.remove(uri);   // the player deleted or moved the image
            d.press(NavInput.CONFIRM);
            expectEquals("Problem", d.model.stateWord(PLAY), "problem");
            expectContains(d.model.detail(PLAY), "The disc image you chose before can no longer be read", "said");
            expectContains(d.model.detail(PLAY), "A: choose it again.", "next");
            expect(new SetupDiscStore(d.files).load(PLAY) == null, "forgotten");
            int choosers = d.choosersOpened;
            d.press(NavInput.CONFIRM);
            expectEquals(choosers + 1, d.choosersOpened, "A asks again");
        });

        check("no lasting permission offered: the copy works now; after a restart the row asks again", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("transient", true).open();
            d.persistable = false;
            String uri = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            d.nextChoice = uri;
            d.press(NavInput.CONFIRM);
            expect(new SetupDiscStore(d.files).load(PLAY) == null, "nothing remembered");
            d.press(NavInput.BACK);
            d.restart();
            expectEquals("To do", d.model.stateWord(PLAY), "asks again");
        });

        check("both discs: Finish is Done exactly when ShipReady says ready; A leaves; before the first start it says so", () -> {
            SetupDiscKit.Device early = new SetupDiscKit.Device("early", false).open();
            expectEquals("Problem", early.model.stateWord(FINISH), "code not in place");
            expectContains(early.model.detail(FINISH), "The app has not finished setting itself up. Go back to the main menu (B) and choose Finish Setting Up.", "sentence");
            agrees(early);
            SetupDiscKit.Device d = new SetupDiscKit.Device("both", true).open();
            copyToEnd(d, PLAY, d.document("Disc2-Play.iso", SetupDiscKit.playImage()));
            agrees(d);
            expect(!d.model.finishReady(), "one disc is not enough");
            copyToEnd(d, INSTALL, d.document("disc1.iso", SetupDiscKit.installImage()));
            expect(d.ship().ready(), "the ship rule: ready");
            agrees(d);
            expectEquals("Done", d.model.stateWord(FINISH), "Finish done");
            expectEquals(SetupDiscModel.TEXT_READY, d.model.detail(FINISH), "ready text");
            InstallState launcher = InstallState.detect(d.files);
            expect(launcher.ready(), "the launcher's own detection (InstallState.detect) is ready too: " + launcher.problem);
            expectEquals(FINISH, d.model.focused(), "focus on Finish");
            d.model.nav(NavInput.CONFIRM, false);
            expectEquals(SetupDiscModel.ACT_LEAVE, d.model.takeAction(), "A leaves");
            expect(!d.model.nav(NavInput.BACK, false), "B leaves");
            expect(contains(d.log, "FINISH ready play=4/4 install=8/8 missing_bytes=0"), "logged");
        });

        check("an entry on neither disc's list: both discs Done, Finish not ready (as ShipReady), and it says what is missing", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("other-entry", false);
            d.version.inputs.add(SetupTestKit.bytes(500, 77));   // a pinned file no disc holds
            d.version.build();
            d.firstRun();
            d.open();
            copyToEnd(d, PLAY, d.document("Disc2-Play.iso", SetupDiscKit.playImage()));
            copyToEnd(d, INSTALL, d.document("disc1.iso", SetupDiscKit.installImage()));
            expect(d.model.state(PLAY) == SetupDiscModel.DONE && d.model.state(INSTALL) == SetupDiscModel.DONE, "both discs done");
            expect(!d.ship().ready(), "the ship rule: not ready");
            agrees(d);
            expectContains(d.model.detail(FINISH), "1 other game file is still missing (0 KB); they are on neither disc's list.", "said");
        });

        check("a file in place that is not the pinned one (ShipData.different) is not counted, and the copy replaces it", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("different", true).open();
            copyToEnd(d, INSTALL, d.document("disc1.iso", SetupDiscKit.installImage()));
            File stamp = new File(d.trace(), "inputs/" + SetupTestKit.sha256(Disc2DiscsTest.STAMP0) + ".bin");
            stamp.setWritable(true, true);
            SetupTestKit.write(stamp, new byte[14]);
            d.restart();
            expectEquals("To do", d.model.stateWord(INSTALL), "not done");
            expectContains(d.model.detail(INSTALL), "7 of 8 files of the install disc are already on this device", "counted by the ship rule");
            copyToEnd(d, INSTALL, d.document("disc1.iso", SetupDiscKit.installImage()));
            expectEquals("Done", d.model.stateWord(INSTALL), "replaced and done");
            agrees(d);
        });

        check("B: leaves while idle or reading, asks while copying, closes a question; a held B (repeat) does nothing", () -> {
            SetupDiscModel m = new SetupDiscKit.Device("back", true).open().model;
            expect(!m.nav(NavInput.BACK, false), "idle: leave");
            m.readingStarted(PLAY, false);
            expect(!m.nav(NavInput.BACK, false), "reading: leave");
            expectEquals("B Back", m.footer(), "prompt while reading");
            m.copyStarted(PLAY, 0);
            expect(m.nav(NavInput.BACK, true), "repeat taken");
            expectEquals(SetupDiscModel.OVERLAY_NONE, m.overlay(), "repeat does not ask");
            expect(m.nav(NavInput.BACK, false), "copying: asks");
            expectEquals(SetupDiscModel.OVERLAY_STOP, m.overlay(), "the question");
            expectEquals("A Select   B Continue   Left/Right Choose", m.footer(), "its prompts");
            m.nav(NavInput.BACK, false);
            expectEquals(SetupDiscModel.OVERLAY_NONE, m.overlay(), "closed");
            expectEquals("B Stop", m.footer(), "copying prompt");
        });

        check("progress: file k of n with the amounts and the time left, without the file's own name; and the bar", () -> {
            SetupDiscModel m = new SetupDiscKit.Device("progress", true).open().model;
            m.copyStarted(INSTALL, 0);
            m.progress(0, 4000000000L, 0, 8, "545408A700000000/part0.rpf", 0);
            m.progress(1000000000L, 4000000000L, 2, 8, "545408A700000001/part1.rpf", 4000);
            String t = m.detail(INSTALL);
            expectEquals("Copying the install disc's files: file 3 of 8. 1.0 GB of 4.0 GB, less than a minute left. B: stop.", t, "the whole line");
            // The game's own name of the file is an internal name: no part of it is on the page (the log's FILE line keeps it).
            for (String text : new String[] {t, m.detail(PLAY), m.detail(FINISH), m.footer()}) {
                expect(!text.contains("545408A7") && !text.contains("part1") && !text.contains(".rpf"), "no file name of the disc in: " + text);
            }
            expect(Math.abs(m.progressFraction() - 0.25f) < 1e-6, "bar");
            expectEquals("Copying", m.stateWord(INSTALL), "word");
        });

        check("leaving the app during a copy stops it as B does: finished files stay, no half file, the row says why and A goes on", () -> {
            final SetupDiscKit.Device d = new SetupDiscKit.Device("left", true).open();
            String uri = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            expect(!d.model.appLeft(), "no copy runs (the file chooser is open, say): leaving the app stops nothing");
            final boolean[] stop = {false};
            final int[] asked = {0};
            d.copyProgress = new SetupDiscCopy.Progress() {
                @Override public void update(long done, long total, int index, int count, String name) {
                    d.model.progress(done, total, index, count, name, 0);
                    if (index == 1 && asked[0] == 0) {
                        // Home is pressed during the second file: the screen is told (onAppLeft) and asks the model.
                        ++asked[0];
                        expect(d.model.nav(NavInput.BACK, false), "(B had been pressed first: the stop question is open)");
                        expectEquals(SetupDiscModel.OVERLAY_STOP, d.model.overlay(), "the stop question");
                        stop[0] = d.model.appLeft();
                        expect(stop[0], "the model says: stop the copy");
                        expectEquals(SetupDiscModel.OVERLAY_NONE, d.model.overlay(), "the open question is closed: it was answered");
                        expect(!d.model.appLeft(), "told twice: the copy is already winding down");
                        expectEquals("Stopping\u2026", d.model.detail(PLAY), "the row while it winds down");
                        expectEquals("B Back", d.model.footer(), "B then leaves the page");
                    }
                }

                @Override public boolean cancelled() {
                    return stop[0];   // what SetupDiscScreen.onAppLeft sets when appLeft() answers true
                }
            };
            d.nextChoice = uri;
            d.press(NavInput.CONFIRM);
            expectContains(d.model.overlayText(), "B stops it at any time, and so does leaving the app; the files that are finished are kept.",
                    "the question says before the copy what stops it");
            d.press(NavInput.CONFIRM);   // Start
            expect(d.lastCopy.stopped && d.lastCopy.error == null, "the copy ended as a stopped copy, not as a failure");
            expectEquals(1, d.lastCopy.kept(), "the file that was finished is kept");
            expect(!new File(d.trace(), SetupDiscCopy.STAGING + "/" + SetupDiscCopy.PART).exists(), "no half file is left in the app's storage");
            expectEquals("Continue", d.model.stateWord(PLAY), "the row offers Continue");
            expectContains(d.model.detail(PLAY), "The copy stopped because the app was left. 1 of 4 files of the play disc is on this device."
                    + " The copy can go on from Disc2-Play.iso where it stopped", "the row says why it stopped");
            expect(contains(d.log, "COPY stopped disc=play read=1/"), "the log: " + d.log);
            agrees(d);
            d.copyProgress = null;
            int choosers = d.choosersOpened;
            d.press(NavInput.CONFIRM);
            expectEquals(choosers, d.choosersOpened, "A goes on without the chooser");
            expectEquals("Done", d.model.stateWord(PLAY), "and the copy ends");
            expect(!d.model.detail(PLAY).contains("because the app was left"), "the reason is gone with the copy that follows");
            agrees(d);
        });

        check("no lasting permission and no room left during the copy: the row says press A to continue, and A does that without the chooser", () -> {
            // Twice the same device up to the failure: once the player makes room before A, once not.
            for (boolean roomMade : new boolean[] {true, false}) {
                final SetupDiscKit.Device d = new SetupDiscKit.Device("held-" + roomMade, true);
                final long[] free = {Long.MAX_VALUE / 4};
                d.space = new VerifiedExtractor.Space() {
                    @Override public long usable(File existingDirectory) {
                        return free[0];
                    }
                };
                d.open();
                d.persistable = false;   // the image's provider gives no lasting permission: nothing is remembered on storage
                String uri = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
                d.copyProgress = new SetupDiscCopy.Progress() {
                    @Override public void update(long done, long total, int index, int count, String name) {
                        d.model.progress(done, total, index, count, name, 0);
                        if (index == 0) free[0] = 1000;   // another app fills the storage while the first file is copied
                    }

                    @Override public boolean cancelled() {
                        return false;
                    }
                };
                copyToEnd(d, PLAY, uri);
                expectEquals(VerifiedExtractor.Problem.NO_SPACE, d.lastCopy.problem, "the copy stopped for lack of space");
                expect(new SetupDiscStore(d.files).load(PLAY) == null, "nothing is remembered on storage");
                expectEquals("Problem", d.model.stateWord(PLAY), "the row");
                expectContains(d.model.detail(PLAY), "then press A to continue", "what the row says A does");
                d.copyProgress = null;
                int choosers = d.choosersOpened;
                if (roomMade) {
                    free[0] = Long.MAX_VALUE / 4;   // the player made room, as the row asked
                    d.press(NavInput.CONFIRM);
                    expectEquals(choosers, d.choosersOpened, "A did what the row said: no file chooser was opened");
                    expectEquals("Done", d.model.stateWord(PLAY), "the copy went on with the image the page holds, to its end");
                    agrees(d);
                } else {
                    d.press(NavInput.CONFIRM);
                    expectEquals(choosers, d.choosersOpened, "still no room: A looks again, and no file chooser is opened");
                    expectContains(d.model.detail(PLAY), "Not enough free space on this device:", "the row has this moment's figures");
                    expectContains(d.model.detail(PLAY), "then press A to look again", "and says what A does now");
                }
            }
            // (the other way round) an image that could not be opened any more is not held: A then opens the chooser.
            SetupDiscKit.Device gone = new SetupDiscKit.Device("held-gone", true).open();
            gone.persistable = false;
            String other = gone.document("Disc2-Play.iso", SetupDiscKit.playImage());
            final boolean[] stopNow = {false};
            gone.copyProgress = new SetupDiscCopy.Progress() {
                @Override public void update(long done, long total, int index, int count, String name) {
                    if (index == 1) stopNow[0] = true;
                }

                @Override public boolean cancelled() {
                    return stopNow[0];
                }
            };
            copyToEnd(gone, PLAY, other);
            gone.documents.remove(other);   // the image is deleted while the row offers Continue
            gone.copyProgress = null;
            gone.press(NavInput.CONFIRM);
            expectContains(gone.model.detail(PLAY), "can no longer be read", "the row says the image is gone");
            int before = gone.choosersOpened;
            gone.press(NavInput.CONFIRM);
            expectEquals(before + 1, gone.choosersOpened, "A now opens the chooser, as that row says (choose it again)");
        });

        check("while one disc is read or copied no other row's text offers A: A does nothing there until that work has finished", () -> {
            final SetupDiscKit.Device d = new SetupDiscKit.Device("busy-rows", true).open();
            String uri = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            expectContains(d.model.detail(INSTALL), SetupDiscModel.TEXT_CHOOSER, "(idle: the install row offers the chooser)");
            final List<String> seen = new ArrayList<String>();
            d.copyProgress = new SetupDiscCopy.Progress() {
                @Override public void update(long done, long total, int index, int count, String name) {
                    d.model.progress(done, total, index, count, name, 0);
                    if (!seen.isEmpty()) return;
                    // The play disc is being copied; the highlight goes to the install disc's row.
                    seen.add(d.model.detail(INSTALL));
                    expect(!d.model.actionable(INSTALL), "A does nothing on the install row now");
                    d.model.focus(INSTALL);
                    d.model.nav(NavInput.CONFIRM, false);
                    expectEquals(SetupDiscModel.ACT_NONE, d.model.takeAction(), "A on it: nothing");
                    d.model.focus(PLAY);
                }

                @Override public boolean cancelled() {
                    return false;
                }
            };
            copyToEnd(d, PLAY, uri);
            expectEquals(1, seen.size(), "the install row was read while the play disc was being copied");
            String busy = seen.get(0);
            expect(!busy.contains("A:") && !busy.contains("press A") && !busy.contains("file chooser"), "it offers nothing A cannot do: " + busy);
            expectContains(busy, SetupDiscModel.TEXT_WAIT, "it says when the row can be used");
            expectContains(busy, "The install disc (Disc 1): choose its disc image", "and still what the row is for");
            // When the copy has finished the row offers A again.
            expectContains(d.model.detail(INSTALL), SetupDiscModel.TEXT_CHOOSER, "after the copy the row offers the chooser again");
            expect(!d.model.detail(INSTALL).contains(SetupDiscModel.TEXT_WAIT), "and the wait sentence is gone");
            // While an image is read (a choice for the install row), the same holds for a row that would offer Continue or Start.
            SetupDiscModel m = new SetupDiscKit.Device("busy-reading", true).open().model;
            m.readingStarted(INSTALL, false);
            expectContains(m.detail(PLAY), SetupDiscModel.TEXT_WAIT, "the play row while the install disc's image is read");
            expect(!m.detail(PLAY).contains(SetupDiscModel.TEXT_CHOOSER), "no chooser is offered there meanwhile");
            // A row whose own text ends with "A: choose again." (a refused image) says as well that it waits.
            final SetupDiscKit.Device p = new SetupDiscKit.Device("busy-problem", true).open();
            p.model.focus(INSTALL);
            p.nextChoice = p.document("notes.txt", new SetupTestKit.MemoryDisc(0, "just a text file, not a disc image".getBytes("UTF-8")));
            p.press(NavInput.CONFIRM);
            expectEquals("Problem", p.model.stateWord(INSTALL), "(the install row has a refused image)");
            expect(p.model.detail(INSTALL).endsWith("A: choose again.") && !p.model.detail(INSTALL).contains(SetupDiscModel.TEXT_WAIT), "idle: it offers A, and no wait");
            final List<String> problem = new ArrayList<String>();
            p.copyProgress = new SetupDiscCopy.Progress() {
                @Override public void update(long done, long total, int index, int count, String name) {
                    if (problem.isEmpty()) problem.add(p.model.detail(INSTALL));
                }

                @Override public boolean cancelled() {
                    return false;
                }
            };
            copyToEnd(p, PLAY, p.document("Disc2-Play.iso", SetupDiscKit.playImage()));
            expect(problem.size() == 1 && problem.get(0).endsWith(SetupDiscModel.TEXT_WAIT), "while the play disc is copied the row says that it waits: " + problem);
        });

        check("a copy that fails at one file: the row says file k of n as the copy line did, and names no file of the disc; the log keeps the name"
                + " and the reason", () -> {
            final String badCopy = " This disc image is damaged or is not a copy of the supported disc. " + Disc2Discs.NEXT_NEW_COPY
                    + " Then press A and choose it.";
            // (1) one block of the first file of the install disc is damaged in the image
            SetupDiscKit.Device d = new SetupDiscKit.Device("fail-damaged", true).open();
            byte[] data = SetupDiscKit.installImage();
            Disc2Discs.Disc whole = Disc2Discs.open(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, data), Disc2DiscsTest.GAME);
            byte[] first = Disc2DiscsTest.content(Disc2DiscsTest.named(whole.candidates, PKG0_PART0));
            data[SetupDiscCopyTest.indexOf(data, java.util.Arrays.copyOf(first, 64)) + 10] ^= 1;
            copyToEnd(d, INSTALL, d.document("disc1.iso", data));
            // setup-fixes (C5-1): a file that fails its check is named by its number, and the files before it stay
            expectEquals("File 1 of 8 is damaged in your disc image (or this image is not a copy of the supported disc): it did not pass its check and"
                    + " was not kept. " + Disc2Discs.NEXT_NEW_COPY
                    + " Then press A and choose it; only the missing files are copied.", failedRow(d, INSTALL), "a damaged file: the whole row");
            String line = lastLine(d.log, "COPY failed disc=install ");
            expectContains(line, " problem=DISC_COPY_BAD error=" + PKG0_PART0 + " is damaged in your disc image (damaged package:"
                    + " the data of block ", "the log names the file");
            expectContains(line, " of part0.rpf does not match its hash).", "and gives the reason as the reader gave it");

            // (2) the storage the image is on fails during the second file of the play disc
            final SetupDiscKit.Device r = new SetupDiscKit.Device("fail-read", true).open();
            final boolean[] fail = {false};
            String flaky = r.document("Disc2-Play.iso", new SetupDiscCopyTest.Wrapped(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, SetupDiscKit.playImage())) {
                @Override public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
                    if (fail[0]) throw new IOException("read failed: EIO (I/O error) at /storage/1234-5678/Disc2-Play.iso");
                    super.readFully(position, buffer, offset, length);
                }
            });
            r.copyProgress = new SetupDiscCopy.Progress() {
                @Override public void update(long done, long total, int index, int count, String name) {
                    r.model.progress(done, total, index, count, name, 0);
                    if (index == 1) fail[0] = true;
                }

                @Override public boolean cancelled() {
                    return false;
                }
            };
            copyToEnd(r, PLAY, flaky);
            expectEquals(VerifiedExtractor.Problem.SOURCE_UNREADABLE, r.lastCopy.problem, "(the image could not be read)");
            expectEquals("File 2 of 4 could not be read from your disc image. Check that the storage the disc image is on is still connected, then"
                    + " press A to continue. The 1 file already copied is kept.", failedRow(r, PLAY), "an unreadable file: the whole row");
            expectContains(lastLine(r.log, "COPY failed disc=play "), " error=common.rpf could not be read from your disc image (read failed: EIO"
                    + " (I/O error) at /storage/1234-5678/Disc2-Play.iso).", "the log names the file and gives the storage's reason");

            // (3) the image file was cut short after it was recognised
            final SetupDiscKit.Device c = new SetupDiscKit.Device("fail-cut", true).open();
            final boolean[] cut = {false};
            String shrinking = c.document("Disc2-Play.iso", new SetupDiscCopyTest.Wrapped(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3,
                    SetupDiscKit.playImage())) {
                @Override public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
                    if (cut[0] && position + length > Disc2DiscsTest.XGD3 + 40 * 2048) throw new java.io.EOFException("read past the end of the file");
                    super.readFully(position, buffer, offset, length);
                }
            });
            c.copyProgress = new SetupDiscCopy.Progress() {
                @Override public void update(long done, long total, int index, int count, String name) {
                    c.model.progress(done, total, index, count, name, 0);
                    cut[0] = true;
                }

                @Override public boolean cancelled() {
                    return false;
                }
            };
            copyToEnd(c, PLAY, shrinking);
            expectEquals(VerifiedExtractor.Problem.DISC_COPY_BAD, c.lastCopy.problem, "(the image is cut off)");
            expectEquals("File 1 of 4 is incomplete in your disc image." + badCopy, failedRow(c, PLAY), "a cut-off file: the whole row");
            expectContains(lastLine(c.log, "COPY failed disc=play "), " is incomplete in your disc image (read past the end of the file).",
                    "the log gives the reason");

            // (4) the app's own storage refuses the write: its reason names the app's folder and its part file
            SetupDiscKit.Device w = new SetupDiscKit.Device("fail-write", true).open();
            File staging = new File(w.trace(), SetupDiscCopy.STAGING);
            expect(staging.mkdirs() && staging.setWritable(false, false), "(a staging folder that cannot be written)");
            try {
                copyToEnd(w, PLAY, w.document("Disc2-Play.iso", SetupDiscKit.playImage()));
            } finally {
                staging.setWritable(true, true);
            }
            expectEquals(VerifiedExtractor.Problem.STORAGE, w.lastCopy.problem, "(the write failed)");
            expectEquals("File 1 of 4 could not be written to this device's storage. Press A to try again.", failedRow(w, PLAY),
                    "a write that failed: the whole row");
            expectContains(lastLine(w.log, "COPY failed disc=play "), " problem=STORAGE error=Could not write the copy of"
                    + " common/data/installpartitions.meta: ", "the log names the file, and the storage's reason follows");

            // (5) a folder stands where the copy of the third file belongs
            SetupDiscKit.Device f = new SetupDiscKit.Device("fail-keep", true).open();
            String exe = SetupTestKit.sha256(Disc2DiscsTest.EXE);
            expect(new File(f.trace(), "inputs/" + exe + ".bin").mkdirs(), "(a folder under the final name)");
            copyToEnd(f, PLAY, f.document("Disc2-Play.iso", SetupDiscKit.playImage()));
            expectEquals(VerifiedExtractor.Problem.STORAGE, f.lastCopy.problem, "(the copy could not be put in place)");
            expectEquals("File 3 of 4 was copied but could not be put in place on this device's storage. Press A to try again."
                    + " The 2 files already copied are kept.", failedRow(f, PLAY), "a copy that could not be put in place: the whole row");
            expectContains(lastLine(f.log, "COPY failed disc=play "), " error=Could not put default.xex in place: a folder stands under the name " + exe
                    + ".bin.", "the log names the disc's file and the file of the app's storage");
        });

        check("an image that is refused: the row says why without the name of a package, a file or the game program; the log's DISC line has it", () -> {
            final String advice = " " + Disc2DiscsTest.ADVICE + " A: choose again.";
            // no game program in the image
            SetupDiscKit.Device d = new SetupDiscKit.Device("refuse-none", true).open();
            d.nextChoice = d.document("notes.iso", new SetupTestKit.Image(SetupTestKit.Node.dir("", SetupTestKit.Node.file("readme.rpf", new byte[4096]))).data);
            d.press(NavInput.CONFIRM);
            expectEquals("This disc image holds no game program, so it is not a disc of Synthetic release." + advice, failedRow(d, PLAY), "no program");
            expect(lastLine(d.log, "DISC row=play chosen kind=OTHER_GAME ").endsWith(" named=default.xex"), "the log names the program's file: " + d.log);
            // a package of the install disc that fails its own checks; one in a form that is not read; a file among them that is no package
            byte[][] p = Disc2DiscsTest.packages();
            byte[] damaged = p[1].clone();
            damaged[(int) Disc2DiscsTest.pkg(1).build().table0(0) + 3] ^= 4;
            Disc2TestKit.Package newer = Disc2DiscsTest.pkg(2);
            newer.metadataVersion = 3;
            byte[] notPackage = p[3].clone();
            notPackage[1] ^= 4;
            byte[][][] images = {{p[0], damaged, p[2], p[3]}, {p[0], p[1], newer.build().data, p[3]}, {p[0], p[1], p[2], notPackage}};
            String[] sentence = {
                    // setup-fixes (C4-1): the package's own reason is in the log's DISC line (named=path: reason), not on the screen
                    "This disc image is damaged: one of its content packages does not pass its own checks. " + Disc2Discs.NEXT_NEW_COPY + " A: choose again.",
                    "This disc image cannot be used: one of its content packages is in a form this version does not read." + advice,
                    "This disc image cannot be used: a file in the game's install-content folder is not an install package of the game (it does not"
                            + " begin as a content package does)." + advice};
            for (int i = 0; i < images.length; ++i) {
                SetupDiscKit.Device e = new SetupDiscKit.Device("refuse-" + i, true).open();
                e.model.focus(INSTALL);
                e.nextChoice = e.document("disc1.iso", Disc2DiscsTest.installImage(images[i]).data);
                e.press(NavInput.CONFIRM);
                expectEquals(sentence[i], failedRow(e, INSTALL), "refused image " + i);
                String named = " named=" + Disc2DiscsTest.FOLDER + Disc2DiscsTest.PKG + (i + 1);
                String disc = lastLine(e.log, "DISC row=install chosen ");
                expect(disc.endsWith(named) || disc.contains(named + ": "),
                        "the log names the package's path, image " + i + ": " + e.log);
            }
        });

        SetupTestKit.finish("SetupDiscModelTest");
    }

    static int count(String text, String part) {
        int n = 0;
        for (int at = text.indexOf(part); at >= 0; at = text.indexOf(part, at + 1)) ++n;
        return n;
    }

    static boolean contains(List<String> lines, String part) {
        for (String line : lines) {
            if (line.contains(part)) return true;
        }
        return false;
    }

    static int countChoosers(SetupDiscKit.Device d, int unused) {
        int n = 0;
        List<String> after = new ArrayList<String>(d.log);
        for (String line : after) {
            if (line.startsWith("CHOOSER row=install")) ++n;
        }
        return n;
    }
}
