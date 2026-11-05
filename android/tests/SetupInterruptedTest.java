package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/**
 * setup-fixes, defect C1-1 of the vm-robust run (major) and finding M1 of the check. After the app was killed or the
 * device restarted in the middle of a copy the row must say that the last copy was interrupted, that the finished files
 * are kept and were checked, what became of the unfinished file, and that A continues. A copy the player stopped (B, or
 * leaving the app: Home, the screen off) must never be called interrupted, in this process (B to the menu and the page
 * again) or in a later one. Driven through SetupDiscKit.Device: the screen's glue in pure Java, calling the same
 * SetupDiscStore.Marker and SetupDiscModel methods SetupDiscScreen calls; restart() is a new process (new token),
 * reopen() a new page in the same process. Run with java -ea.
 */
public final class SetupInterruptedTest {
    static final int PLAY = SetupDiscModel.PLAY;
    static final String INTERRUPTED = "The last copy was interrupted (the app was closed or the device restarted).";
    static final String LONG_NAME = "Grand Theft Auto V (Disc 2 - Play, full image).iso";

    /**
     * Starts a copy of the play disc with an image of this name and kills the process inside file killAt + 1: what the
     * storage holds at that moment (the half-written part file) is put back, as a kill leaves it. Then the app starts
     * again: its first screen (ShipFirstRun, removes the half file) when firstScreen, and the page in a new process.
     */
    static SetupDiscKit.Device killed(String label, boolean persistable, final int killAt, String name, boolean firstScreen) throws IOException {
        final SetupDiscKit.Device d = new SetupDiscKit.Device(label, true).open();
        d.persistable = persistable;
        String uri = d.document(name, SetupDiscKit.playImage());
        final File part = new File(new File(d.trace(), SetupDiscCopy.STAGING), SetupDiscCopy.PART);
        final File saved = new File(d.files.getParentFile(), label + "-half.part");
        d.copyProgress = new SetupDiscCopy.Progress() {
            @Override public void update(long done, long total, int index, int count, String name) {
                d.model.progress(done, total, index, count, name, 0);
                if (index == killAt && part.isFile() && part.length() > 0 && !saved.exists()) {
                    try {
                        Files.copy(part.toPath(), saved.toPath());
                    } catch (IOException e) {
                        throw new IllegalStateException(e);
                    }
                    d.killNow = true;   // the process dies; the page never sees the copy end
                }
            }

            @Override public boolean cancelled() {
                return d.killNow;
            }
        };
        SetupDiscModelTest.copyToEnd(d, PLAY, uri);
        expect(saved.exists(), "the kill came inside file " + (killAt + 1));
        Files.move(saved.toPath(), part.toPath(), StandardCopyOption.REPLACE_EXISTING);
        d.copyProgress = null;
        expect(SetupDiscCopy.leftoverBytes(d.trace()) > 0, "the half-written file lies in the staging folder after the kill");
        if (firstScreen) d.firstRun();   // the app's first screen at the next start (LEFT_OVERS removed)
        d.restart();                     // a new process, the page opened again
        return d;
    }

    /** A copy of the play disc stopped inside its second file: by B (left false) or by the app leaving the front. */
    static SetupDiscKit.Device stopped(String label, final boolean byLeaving) throws IOException {
        return stopped(label, byLeaving, false);
    }

    /** As above; homeAfterB: after B and Stop, Home comes before the copy has wound down. */
    static SetupDiscKit.Device stopped(String label, final boolean byLeaving, final boolean homeAfterB) throws IOException {
        final SetupDiscKit.Device d = new SetupDiscKit.Device(label, true).open();
        String uri = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
        final boolean[] stop = {false};
        d.copyProgress = new SetupDiscCopy.Progress() {
            @Override public void update(long done, long total, int index, int count, String name) {
                d.model.progress(done, total, index, count, name, 0);
                if (index == 1 && !stop[0]) {
                    stop[0] = true;
                    if (byLeaving) {
                        d.appLeft();   // Home or the power key: SetupDiscScreen.onAppLeft
                    } else {
                        expect(d.model.nav(NavInput.BACK, false), "B");
                        d.model.nav(NavInput.CONFIRM, false);   // "Stop"
                        expectEquals(SetupDiscModel.ACT_STOP, d.model.takeAction(), "stop asked");
                        if (homeAfterB) d.appLeft();   // Home before the copy thread saw the stop
                    }
                }
            }

            @Override public boolean cancelled() {
                return stop[0];
            }
        };
        SetupDiscModelTest.copyToEnd(d, PLAY, uri);
        d.copyProgress = null;
        expect(d.lastCopy.stopped, "the copy stopped at the next block");
        return d;
    }

    static void notInterrupted(SetupDiscKit.Device d, String what) {
        String row = d.model.detail(PLAY);
        expect(!row.contains("interrupted") && !row.contains("closed or the device restarted"), what + ": not called interrupted: " + row);
    }

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("C1-1: killed in the middle of a copy: after the restart the Continue row says the copy was interrupted, the finished file"
                + " is kept and was checked, any unfinished file was removed, and A continues", () -> {
            SetupDiscKit.Device d = killed("c1-kill", true, 1, LONG_NAME, true);
            expectEquals("Continue", d.model.stateWord(PLAY), "the row offers Continue");
            String row = d.model.detail(PLAY);
            expect(row.startsWith(INTERRUPTED), "the row begins by saying the copy was interrupted: " + row);
            expectContains(row, "1 of the 4 files is finished and kept; it was checked when it was copied.", "what is kept, and that it was checked");
            expectContains(row, "Any file that was not finished was removed. The copy goes on from " + LONG_NAME + ".", "what became of the half file");
            expect(row.endsWith("A: continue."), "and what A does: " + row);
            expectEquals(0L, SetupDiscCopy.leftoverBytes(d.trace()), "the sentence is true: the half file is gone after the start");
            SetupDiscModelTest.agrees(d);   // no developer word, no forbidden shape, no name of a disc file
            SetupDiscPageTest.expectFits(d.model, "the interrupted Continue row");   // drawn whole, nothing cut off
            expectContains(SetupDiscModelTest.lastLine(d.log, "PAGE shown "), " last_copy_play=interrupted", "the PAGE log line says it too");
            int choosers = d.choosersOpened;
            d.press(NavInput.CONFIRM);
            expectEquals(choosers, d.choosersOpened, "A continues without the chooser");
            expectEquals("Done", d.model.stateWord(PLAY), "and the copy ends");
            d.restart();
            expect(!d.model.detail(PLAY).contains("interrupted"), "after a copy that ended the sentence is gone: " + d.model.detail(PLAY));
            expect(!SetupDiscModelTest.lastLine(d.log, "PAGE shown ").contains("last_copy"), "and from the log line");
        });

        check("C1-1: killed in the third file: the count of finished files is right (plural)", () -> {
            SetupDiscKit.Device d = killed("c1-third", true, 2, "Disc2-Play.iso", true);
            expectContains(d.model.detail(PLAY), "2 of the 4 files are finished and kept; each was checked when it was copied.", "two files");
        });

        check("C1-1: the half file still on the device (the first screen did not run): the row says so instead of \"removed\"", () -> {
            SetupDiscKit.Device d = killed("c1-half", true, 1, "Disc2-Play.iso", false);
            String row = d.model.detail(PLAY);
            expectContains(row, SetupDiscModel.TEXT_HALF_LEFT, "still there, removed when the copy goes on");
            expect(!row.contains("was removed"), "not called removed: " + row);
            d.press(NavInput.CONFIRM);
            expect(d.lastCopy.leftoversRemoved > 0, "and the copy that goes on removes it first");
        });

        check("C1-1: without a lasting permission the To Do row says the same and asks for the image again", () -> {
            SetupDiscKit.Device d = killed("c1-todo", false, 1, "Disc2-Play.iso", true);
            expectEquals("To do", d.model.stateWord(PLAY), "nothing remembered: To Do");
            String row = d.model.detail(PLAY);
            expect(row.startsWith(INTERRUPTED), "interrupted: " + row);
            expectContains(row, "1 of the 4 files is finished and kept; it was checked when it was copied.", "kept");
            expectContains(row, "Any file that was not finished was removed. To go on, choose the play disc's disc image again.", "what to do");
            SetupDiscModelTest.agrees(d);
            SetupDiscPageTest.expectFits(d.model, "the interrupted To Do row");
        });

        check("M1: Home stops the copy, the player comes back, B to the menu and Set Up again (same process): the row says the copy"
                + " stopped because the app was left, never interrupted; so after the process ended and a new start", () -> {
            SetupDiscKit.Device d = stopped("m1-home", true);
            expect(d.model.detail(PLAY).startsWith(SetupDiscModel.TEXT_STOPPED_LEFT), "the page that saw it: " + d.model.detail(PLAY));
            d.reopen();   // B to the menu, Set Up Game Files again: a new page in the same process
            String row = d.model.detail(PLAY);
            expect(row.startsWith(SetupDiscModel.TEXT_STOPPED_LEFT), "the new page in the same process: " + row);
            notInterrupted(d, "same process");
            expectContains(SetupDiscModelTest.lastLine(d.log, "PAGE shown "), " last_copy_play=left", "the log line");
            d.restart();   // Android ended the process while the app was not in front; the player starts it again
            expect(d.model.detail(PLAY).startsWith(SetupDiscModel.TEXT_STOPPED_LEFT), "a later start: " + d.model.detail(PLAY));
            notInterrupted(d, "later start");
            d.press(NavInput.CONFIRM);
            expectEquals("Done", d.model.stateWord(PLAY), "A goes on to the end");
            d.restart();
            expect(!d.model.detail(PLAY).contains("stopped"), "after the copy ended nothing is said: " + d.model.detail(PLAY));
        });

        check("M1 control: a copy stopped with B is \"Stopped.\" on a new page and after a restart (force-stop), never interrupted", () -> {
            SetupDiscKit.Device d = stopped("m1-b", false);
            d.reopen();
            expect(d.model.detail(PLAY).startsWith(SetupDiscModel.TEXT_STOPPED), "same process: " + d.model.detail(PLAY));
            notInterrupted(d, "same process");
            d.restart();
            expectEquals("Continue", d.model.stateWord(PLAY), "Continue");
            expect(d.model.detail(PLAY).startsWith(SetupDiscModel.TEXT_STOPPED), "after a restart: " + d.model.detail(PLAY));
            notInterrupted(d, "after a restart");
        });

        check("check-2 minor 1: B, Stop, then Home before the copy ends: \"Stopped.\" on a new page and after a restart", () -> {
            SetupDiscKit.Device d = stopped("b-then-home", false, true);
            expect(d.model.detail(PLAY).startsWith(SetupDiscModel.TEXT_STOPPED), "the page that saw it: " + d.model.detail(PLAY));
            d.reopen();
            expect(d.model.detail(PLAY).startsWith(SetupDiscModel.TEXT_STOPPED), "a new page: " + d.model.detail(PLAY));
            expectContains(SetupDiscModelTest.lastLine(d.log, "PAGE shown "), " last_copy_play=stopped", "the log line");
            d.restart();
            expect(d.model.detail(PLAY).startsWith(SetupDiscModel.TEXT_STOPPED), "after a restart: " + d.model.detail(PLAY));
        });

        check("minor: an image name of 255 characters is shortened in the middle; the rows are drawn whole", () -> {
            StringBuilder name = new StringBuilder("GTA-V-Disc-2-Play-");
            while (name.length() < 251) name.append('x');
            name.append(".iso");
            SetupDiscKit.Device d = killed("long-name", true, 1, name.toString(), true);
            String row = d.model.detail(PLAY);
            expectContains(row, "GTA-V-Disc-2-Play-", "the name's start");
            expectContains(row, "xx.iso.", "and its end");
            expectContains(row, "…", "shortened in the middle");
            expect(!row.contains(name.toString()), "not the whole name");
            SetupDiscPageTest.expectFits(d.model, "a 255-character name, interrupted");
            d.press(NavInput.CONFIRM);
            expectEquals("Done", d.model.stateWord(PLAY), "done");
            SetupDiscPageTest.expectFits(d.model, "a 255-character name, Done");
            expect(d.model.detail(PLAY).contains("\u2026") && !d.model.detail(PLAY).contains(name.toString()),
                    "the Done row names the image shortened too: " + d.model.detail(PLAY));
        });

        SetupTestKit.finish("SetupInterruptedTest");
    }
}
