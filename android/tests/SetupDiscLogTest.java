package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.util.ArrayList;
import java.util.List;

/**
 * Run with java -ea. SetupDiscLog: the exact lines DEVICE-STEPS.md reads from the device's log (tag RecompSetup),
 * written along a whole two-disc setup by SetupDiscKit.Device. No document address and no file content in any line.
 */
public final class SetupDiscLogTest {
    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("the tag and the line of each event, along a whole setup of both discs", () -> {
            expectEquals("RecompSetup", SetupDiscLog.TAG, "tag");
            SetupDiscKit.Device d = new SetupDiscKit.Device("log", true).open();
            d.nextChoice = d.document("Disc2-Play.iso", SetupDiscKit.playImage());
            d.press(NavInput.CONFIRM);
            d.press(NavInput.CONFIRM);
            d.nextChoice = d.document("disc1.iso", SetupDiscKit.installImage());
            d.model.focus(SetupDiscModel.INSTALL);
            d.press(NavInput.CONFIRM);
            d.press(NavInput.CONFIRM);
            List<String> log = d.log;
            expect(log.get(0).startsWith("PAGE shown play=0/4 install=0/8 other_missing=0 free="), "page: " + log.get(0));
            expect(log.get(0).endsWith(" ready=no code=ok remembered_play=no remembered_install=no"), "page end: " + log.get(0));
            expectEquals("CHOOSER row=play opened", log.get(1), "chooser");
            expectEquals("CHOICE disc=play remembered", log.get(2), "remembered");
            expect(log.get(3).startsWith("DISC row=play chosen kind=PLAY serves=play lacking=0 candidates="), "disc: " + log.get(3));
            expect(log.get(3).contains(" partition=" + Disc2DiscsTest.XGD3 + " bytes="), "partition and size: " + log.get(3));
            // A refused image's line ends with the name the page leaves out (SetupDiscModelTest). A recognised disc's
            // line is as it was: it ends with the image's size, and nothing follows.
            expect(log.get(3).matches(".* bytes=[0-9]+"), "a recognised disc's line ends with the image's size: " + log.get(3));
            expect(log.get(4).startsWith("COPY start disc=play in_place=0/4 need=" + (SetupDiscCopy.bytes(Disc2DiscsTest.playPins())
                    + SetupDiscCopy.RESERVE_BYTES) + " free="), "copy start: " + log.get(4));
            List<String> files = new ArrayList<String>();
            for (String line : log) if (line.startsWith("FILE ")) files.add(line);
            expectEquals(12, files.size(), "one FILE line per file read");
            String exe = " WANTED kept bytes=" + Disc2DiscsTest.EXE.length + " sha256=" + SetupTestKit.sha256(Disc2DiscsTest.EXE) + " name=default.xex";
            int exeLines = 0;
            for (String f : files) if (f.matches("FILE [1-4]/4 .*") && f.endsWith(exe)) ++exeLines;
            expectEquals(1, exeLines, "the executable's line, with its full SHA-256: " + files);
            expect(log.contains("COPY done disc=play read=4/4 kept=4 left_overs=0 missing=0"), "copy done: " + log);
            expect(log.contains("CHOICE disc=play forgotten why=done"), "forgotten when done");
            expect(log.contains("FINISH not-ready play=4/4 install=0/8 missing_bytes=" + SetupDiscCopy.bytes(Disc2DiscsTest.installPins())),
                    "finish after one disc: " + log);
            expectEquals("FINISH ready play=4/4 install=8/8 missing_bytes=0", log.get(log.size() - 1), "finish at the end");
            for (String line : log) expect(!line.contains("content://") && !line.contains("\n"), "no address, one line: " + line);
        });

        check("stopped and failed copies, a remembered image that is gone, nothing chosen", () -> {
            SetupDiscCopy.Outcome stopped = new SetupDiscCopy.Outcome(false, true, VerifiedExtractor.Problem.NONE, null, 0, 0, 5, 10, 0,
                    new ArrayList<SetupDiscCopy.FileResult>(), new ArrayList<Disc2Discs.Pin>());
            expectEquals("COPY stopped disc=install read=0/10 kept=0 left_overs=5 missing=0", SetupDiscLog.copyEnd(1, stopped), "stopped");
            SetupDiscCopy.Outcome failed = new SetupDiscCopy.Outcome(false, false, VerifiedExtractor.Problem.NO_SPACE, "Not enough space: needs\n2.0 GB.",
                    0, 0, 0, 4, 0, new ArrayList<SetupDiscCopy.FileResult>(), new ArrayList<Disc2Discs.Pin>());
            expectEquals("COPY failed disc=play read=0/4 kept=0 left_overs=0 missing=0 problem=NO_SPACE error=Not enough space: needs 2.0 GB.",
                    SetupDiscLog.copyEnd(0, failed), "failed, on one line");
            expectEquals("DISC row=install remembered unreadable reason=gone", SetupDiscLog.rememberedGone(1, "gone"), "gone");
            expectEquals("CHOSEN row=play nothing", SetupDiscLog.nothingChosen(0), "nothing");
            expectEquals("CHOSEN row=install bytes=7 permission=not-kept", SetupDiscLog.chosen(1, 7, false), "chosen");
            expectEquals("CHOOSER row=play none", SetupDiscLog.chooser(0, false), "no chooser");
        });

        SetupTestKit.finish("SetupDiscLogTest");
    }
}
