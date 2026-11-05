package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.File;

/**
 * setup-fixes, defect C3-1 of the vm-robust run (major): after a copy ran out of space the row kept the free-space
 * figures of the moment of the failure although the player had freed space. The figures must be read again whenever
 * the page is shown, its window regains the focus, or A is pressed, and the row must be built from them. Driven through
 * SetupDiscKit.Device (windowBack() is what SetupDiscScreen.onWindowBack / onAppBack do; open() is onShown).
 * Which Android calls lead there is read from the sources in ScreenFitContractTest. Run with java -ea.
 */
public final class SetupSpaceFiguresTest {
    static final int PLAY = SetupDiscModel.PLAY;
    static final long GB = 1000000000L;

    /** A play copy that runs out of space in its first file; the free figure then is free[0]. */
    static SetupDiscKit.Device ranOut(String label, final long[] free) throws Exception {
        final SetupDiscKit.Device d = new SetupDiscKit.Device(label, true);
        d.space = new VerifiedExtractor.Space() {
            @Override public long usable(File existingDirectory) {
                return free[0];
            }
        };
        d.open();
        String uri = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
        d.copyProgress = new SetupDiscCopy.Progress() {
            @Override public void update(long done, long total, int index, int count, String name) {
                d.model.progress(done, total, index, count, name, 0);
                if (index == 0) free[0] = 2 * GB / 10;   // another app fills the storage: 0.2 GB free
            }

            @Override public boolean cancelled() {
                return false;
            }
        };
        SetupDiscModelTest.copyToEnd(d, PLAY, uri);
        d.copyProgress = null;
        expectEquals(VerifiedExtractor.Problem.NO_SPACE, d.lastCopy.problem, "the copy stopped for lack of space");
        expectContains(d.model.detail(PLAY), "and 0.2 GB are free", "the figure of the moment of the failure");
        return d;
    }

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("C3-1: the player frees space and the window gets the focus back: the row shows the figures of now and says A continues", () -> {
            long[] free = {100 * GB};
            SetupDiscKit.Device d = ranOut("c3-focus", free);
            free[0] = 50 * GB;   // e.g. files deleted from the notification shade's download list
            d.windowBack();
            String row = d.model.detail(PLAY);
            expect(!row.contains("0.2 GB are free"), "the old figure is gone: " + row);
            expectContains(row, "There is enough now.", "says the space is there now");
            expectContains(row, "this device has 50.0 GB free.", "with the figure of now");
            expect(row.contains("A: continue."), "and that A continues: " + row);
            SetupDiscModelTest.agrees(d);
            SetupDiscPageTest.expectFits(d.model, "the no-space row with enough now");
            d.press(NavInput.CONFIRM);
            expectEquals("Done", d.model.stateWord(PLAY), "A continued to the end");
        });

        check("C3-1: still too little after freeing some: the figures are of now, with what is still to free", () -> {
            long[] free = {100 * GB};
            SetupDiscKit.Device d = ranOut("c3-some", free);
            free[0] = 5 * GB / 10;
            d.open();   // the page shown again
            String row = d.model.detail(PLAY);
            expectEquals("Problem", d.model.stateWord(PLAY), "still a problem");
            expectContains(row, "and 0.5 GB are free.", "the figure of now");
            expect(!row.contains("0.2 GB"), "not the old one: " + row);
            expectContains(row, "then press A to continue", "what A does");
        });

        check("C3-1: A on the row reads the figures again before anything else", () -> {
            long[] free = {100 * GB};
            SetupDiscKit.Device d = ranOut("c3-a", free);
            free[0] = 4 * GB / 10;   // more room, still not enough; no focus change: only A
            d.press(NavInput.CONFIRM);
            expectContains(d.model.detail(PLAY), "and 0.4 GB are free.", "the figures after A are of now: " + d.model.detail(PLAY));
        });

        SetupTestKit.finish("SetupSpaceFiguresTest");
    }
}
