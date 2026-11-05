package dev.recompbootstrap.smoke;

import java.io.File;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Every state of the two-disc setup page, reached the way the player reaches it (SetupDiscKit.Device over the
 * synthetic discs). Used by SetupDiscPageTest (layout of each state) and by the lane's preview renderer.
 * The synthetic discs are small, so the amounts in these states are kilobytes, not gigabytes.
 */
final class SetupDiscPreviewStates {
    private SetupDiscPreviewStates() {}

    static List<SetupDiscModel> models() throws Exception {
        return new ArrayList<SetupDiscModel>(named().values());
    }

    /** name -> the page's model in that state, in the order the player meets them. */
    static Map<String, SetupDiscModel> named() throws Exception {
        Map<String, SetupDiscModel> out = new LinkedHashMap<String, SetupDiscModel>();
        out.put("00-checking", new SetupDiscModel());

        SetupDiscKit.Device d = device("pv-fresh", true).open();
        out.put("01-fresh", d.model);

        d = device("pv-ask", true).open();
        d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
        d.press(NavInput.CONFIRM);
        out.put("02-play-recognised-question", d.model);

        d = device("pv-copying", true).open();
        d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
        d.press(NavInput.CONFIRM);
        d.press(NavInput.BACK);   // not now: the row shows Ready
        out.put("03-play-ready", d.model);
        SetupDiscModel copying = device("pv-copying-2", true).open().model;
        copying.copyStarted(SetupDiscModel.PLAY, 0);
        copying.progress(0, 8146504708L, 0, 10, "default.xex", 0);
        copying.progress(2852056068L, 8146504708L, 4, 10, "installpartitions.meta", 7300);
        out.put("04-play-copying", copying);
        SetupDiscModel stopQ = device("pv-stopq", true).open().model;
        stopQ.copyStarted(SetupDiscModel.PLAY, 0);
        stopQ.progress(1431193600L, 8146504708L, 3, 10, "xbox360b.rpf", 3000);
        stopQ.nav(NavInput.BACK, false);
        out.put("05-stop-question", stopQ);

        final SetupDiscKit.Device s = device("pv-stopped", true).open();
        String uri = s.document("Disc2-Play.iso", SetupDiscKit.playImage());
        s.copyProgress = new SetupDiscCopy.Progress() {
            @Override public void update(long done, long total, int index, int count, String name) {}

            @Override public boolean cancelled() {
                return new File(s.trace(), "inputs").list() != null && countBins(new File(s.trace(), "inputs")) >= 1;
            }
        };
        s.nextChoice = uri;
        s.press(NavInput.CONFIRM);
        s.press(NavInput.CONFIRM);
        out.put("06-play-stopped-continue", s.model);
        s.copyProgress = null;
        s.restart();
        out.put("07-after-restart-continue", s.model);

        d = device("pv-playdone", true).open();
        d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
        d.press(NavInput.CONFIRM);
        d.press(NavInput.CONFIRM);
        out.put("08-play-done-install-next", d.model);
        SetupDiscModel playDoneFocus = d.model;
        d = device("pv-playdone-2", true).open();
        d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
        d.press(NavInput.CONFIRM);
        d.press(NavInput.CONFIRM);
        d.model.focus(SetupDiscModel.PLAY);
        out.put("09-play-done-may-delete", d.model);
        playDoneFocus.focus(SetupDiscModel.ROW_FINISH);

        d = device("pv-swap", true).open();
        d.nextChoice = d.document("disc1.iso", SetupDiscKit.installImage());
        d.press(NavInput.CONFIRM);
        out.put("10-install-chosen-on-play-row", d.model);

        d = device("pv-other", true).open();
        d.nextChoice = d.document("other.iso", Disc2DiscsTest.playImage(Disc2DiscsTest.OTHER_EXE, Disc2DiscsTest.COMMON).data);
        d.press(NavInput.CONFIRM);
        out.put("11-other-game", d.model);

        d = device("pv-notdisc", true).open();
        d.nextChoice = d.document("photo.jpg", new SetupTestKit.MemoryDisc(0, new byte[5000]));
        d.press(NavInput.CONFIRM);
        out.put("12-not-a-disc", d.model);

        d = device("pv-cut", true).open();
        byte[] whole = SetupDiscKit.installImage();
        d.nextChoice = d.document("disc1.iso", java.util.Arrays.copyOf(whole, whole.length / 2));
        d.model.focus(SetupDiscModel.INSTALL);
        d.press(NavInput.CONFIRM);
        out.put("13-cut-off-image", d.model);

        d = device("pv-lacking", true).open();
        byte[][] p = Disc2DiscsTest.packages();
        d.nextChoice = d.document("disc1.iso", Disc2DiscsTest.installImage(new byte[][] {p[0], p[1], p[2]}).data);
        d.model.focus(SetupDiscModel.INSTALL);
        d.press(NavInput.CONFIRM);
        out.put("14-install-lacks-a-package", d.model);

        d = device("pv-space", true);
        d.space = SetupTestKit.spaceOf(600000000L);
        d.open();
        d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
        d.press(NavInput.CONFIRM);
        out.put("15-low-space", d.model);

        d = device("pv-nothing", true).open();
        d.nextChoice = null;
        d.press(NavInput.CONFIRM);
        out.put("16-nothing-chosen", d.model);

        d = device("pv-gone", true).open();
        String gone = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
        d.nextChoice = gone;
        d.press(NavInput.CONFIRM);
        d.press(NavInput.BACK);
        d.restart();
        d.documents.remove(gone);
        d.press(NavInput.CONFIRM);
        out.put("17-document-gone", d.model);

        SetupDiscModel failed = device("pv-failed", true).open().model;
        failed.copyStarted(SetupDiscModel.INSTALL, 0);
        // The two texts of a failed copy as SetupDiscCopy.run gives them: the first for the log, the second for the page.
        failed.copyFinished(new SetupDiscCopy.Outcome(false, false, VerifiedExtractor.Problem.SOURCE_UNREADABLE,
                "545408A700000002/part2.rpf could not be read from your disc image (I/O error).",
                SetupDiscCopy.shown(4, 8, SetupDiscCopy.SHOWN_NOT_READ), 0, 0, 0, 8, 0,
                new ArrayList<SetupDiscCopy.FileResult>(), new ArrayList<Disc2Discs.Pin>()));
        failed.focus(SetupDiscModel.INSTALL);
        out.put("18-copy-failed-unreadable", failed);

        d = device("pv-ready", true).open();
        d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
        d.press(NavInput.CONFIRM);
        d.press(NavInput.CONFIRM);
        d.nextChoice = d.document("disc1.iso", SetupDiscKit.installImage());
        d.model.focus(SetupDiscModel.INSTALL);
        d.press(NavInput.CONFIRM);
        d.press(NavInput.CONFIRM);
        out.put("19-finish-ready", d.model);
        out.put("20-finish-missing", playDoneFocus);

        d = device("pv-code", false).open();
        d.model.focus(SetupDiscModel.ROW_FINISH);
        out.put("21-code-not-in-place", d.model);
        return out;
    }

    /** A device with 41.3 GB free, so that the texts show realistic free space (the disc amounts stay the synthetic ones). */
    static SetupDiscKit.Device device(String label, boolean firstRunDone) throws Exception {
        SetupDiscKit.Device d = new SetupDiscKit.Device(label, firstRunDone);
        d.space = SetupTestKit.spaceOf(41300000000L);
        return d;
    }

    static int countBins(File dir) {
        int n = 0;
        String[] names = dir.list();
        if (names == null) return 0;
        for (String name : names) {
            if (name.endsWith(".bin")) ++n;
        }
        return n;
    }
}
