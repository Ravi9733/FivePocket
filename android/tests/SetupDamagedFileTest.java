package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * setup-fixes, defect C5-1 of the vm-robust run: a play disc image of full length with one changed byte in its last
 * file was reported "This disc image did not provide 1 of the 10 files", which reads as a missing file. The row must
 * say "File k of n is damaged in your disc image" (k, n: the copy line's numbers) and that the other files were kept;
 * which files are accepted does not change (the damaged file is not kept, the good image then copies only it).
 * Driven through SetupDiscKit.Device. Run with java -ea.
 */
public final class SetupDamagedFileTest {
    static final int PLAY = SetupDiscModel.PLAY;

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("C5-1: one changed byte in a play disc file: the row names the file by its number, says it is damaged and that the others"
                + " are kept; the log keeps the name; the good image then copies only that file", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("c5-damaged", true).open();
            byte[] data = SetupDiscKit.playImage();
            data[SetupDiscCopyTest.indexOf(data, java.util.Arrays.copyOf(Disc2DiscsTest.AUDIO, 64)) + 100] ^= 0x55;
            SetupDiscModelTest.copyToEnd(d, PLAY, d.document("Damaged.iso", data));
            expectEquals(VerifiedExtractor.Problem.DISC_COPY_BAD, d.lastCopy.problem, "a bad copy");
            expectEquals(3, d.lastCopy.kept(), "the three good files were kept");
            // the file's number as the copy line and the FILE log line count it
            String file = null;
            for (String line : d.log) {
                if (line.startsWith("FILE ") && line.contains(" NOT_PINNED ")) file = line;
            }
            expect(file != null, "the FILE line of the damaged file: " + d.log);
            Matcher k = Pattern.compile("^FILE ([0-9]+)/([0-9]+) ").matcher(file);
            expect(k.find(), "numbers in " + file);
            String row = SetupDiscModelTest.failedRow(d, PLAY);
            expectEquals("File " + k.group(1) + " of " + k.group(2) + " is damaged in your disc image (or this image is not a copy of the supported disc):"
                    + " it did not pass its check and was not kept. The 3 files already copied are kept. " + Disc2Discs.NEXT_NEW_COPY + " Then press A and choose it; only the missing files are"
                    + " copied.", row, "the whole row");
            expect(!row.contains("did not provide"), "not called a missing file");
            SetupDiscPageTest.expectFits(d.model, "the damaged-file row");
            String failed = SetupDiscModelTest.lastLine(d.log, "COPY failed disc=play ");
            expectContains(failed, "This disc image did not provide 1 of the 4 files this disc should hold. Failed its check: ", "the log keeps its sentence");
            expectContains(failed, "audio", "and names the file");
            // the good image: only the missing file is read
            SetupDiscModelTest.copyToEnd(d, PLAY, d.document("Disc2-Play.iso", SetupDiscKit.playImage()));
            expect(d.lastCopy.complete, "complete");
            expectEquals(1, d.lastCopy.kept(), "only the damaged file was copied again");
            expectEquals("Done", d.model.stateWord(PLAY), "done");
        });

        SetupTestKit.finish("SetupDamagedFileTest");
    }
}
