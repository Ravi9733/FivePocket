package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.Arrays;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * setup-fixes, defects C4-2 (major), C4-1 and C4-3 of the vm-robust run. C4-2: an app (an .apk, a ZIP archive) chosen
 * as the disc image was answered "it is a ZIP archive. Unpack it first and choose the image inside" and the unpack
 * advice twice; the row must say what the file is not (a disc image of this game), what is expected (the image file of
 * the row's disc and its usual size), once. C4-1 / C4-3 (defect 6): no hex position and no byte count on the screen;
 * the log's DISC line keeps them. Driven through SetupDiscKit.Device. Run with java -ea.
 */
public final class SetupNotADiscTest {
    static final int PLAY = SetupDiscModel.PLAY, INSTALL = SetupDiscModel.INSTALL;
    static final String PLAY_EXPECTED = "Choose the image file of the play disc (Disc 2), usually an .iso file of about 8.7 GB.";
    static final String INSTALL_EXPECTED = "Choose the image file of the install disc (Disc 1), usually an .iso file of about 8.7 GB.";

    /** A hex value, a byte position or a raw byte count, as a player must not see them. */
    static final Pattern[] TECHNICAL = {Pattern.compile("0x[0-9a-fA-F]+"), Pattern.compile("\\b[0-9A-F]{8}\\b"),
            Pattern.compile("[0-9]{4,} bytes"), Pattern.compile("\\bbyte [0-9]"), Pattern.compile("\\bbytes\\b")};

    static String technical(String text) {
        for (Pattern p : TECHNICAL) {
            Matcher m = p.matcher(text);
            if (m.find()) return m.group();
        }
        return null;
    }

    /** A ZIP archive of these entries, padded with a stored entry to more than 33 sectors (as any real app is). */
    static byte[] zip(String... names) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(out)) {
            z.setLevel(java.util.zip.Deflater.NO_COMPRESSION);   // the archive keeps its size, as an app's large files do
            for (String name : names) {
                z.putNextEntry(new ZipEntry(name));
                z.write(SetupTestKit.bytes(70000, name.length()));
                z.closeEntry();
            }
        }
        return out.toByteArray();
    }

    static String refusedRow(int row, String name, byte[] data, long leadingZeros) throws Exception {
        SetupDiscKit.Device d = new SetupDiscKit.Device("c4-" + name, true).open();
        d.model.focus(row);
        d.nextChoice = d.document(name, new SetupTestKit.MemoryDisc(leadingZeros, data));
        d.press(NavInput.CONFIRM);
        expectEquals("Problem", d.model.stateWord(row), name + " is refused");
        SetupDiscModelTest.agrees(d);
        SetupDiscPageTest.expectFits(d.model, name + " refused");
        String row1 = d.model.detail(row);
        String bad = technical(row1);
        if (bad != null) throw new AssertionError("a hex value or byte count (" + bad + ") on the screen: " + row1);
        return row1;
    }

    static int count(String text, String part) {
        return SetupDiscModelTest.count(text.toLowerCase(java.util.Locale.ROOT), part);
    }

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("C4-2: an app chosen as the play disc's image: not a disc image of this game, it is an app, what is expected; no unpack advice", () -> {
            String row = refusedRow(PLAY, "gta-v-recomp.apk", zip("AndroidManifest.xml", "classes.dex", "lib/arm64-v8a/libgame.so"), 0);
            expectEquals("This file is not a disc image of Synthetic release: it is an Android app. " + PLAY_EXPECTED
                    + " A: choose again.", row, "the whole row");
            expectEquals(0, count(row, "unpack"), "no advice to unpack an app");
            expectEquals(1, count(row, "choose the image file"), "what is expected, said once");
        });

        check("C4-2: the same app on the install disc's row names that disc", () -> {
            String row = refusedRow(INSTALL, "app.apk", zip("META-INF/MANIFEST.MF", "AndroidManifest.xml"), 0);
            expectContains(row, INSTALL_EXPECTED, "the row's own disc");
        });

        check("C4-2: a ZIP archive that is no app: the unpack advice once, then what is expected", () -> {
            String row = refusedRow(PLAY, "disc.zip", zip("Disc2-Play.iso"), 0);
            expectEquals("This file is not a disc image of Synthetic release: it is a ZIP archive. If a disc image is packed inside it, unpack it"
                    + " first. " + PLAY_EXPECTED + " A: choose again.", row, "the whole row");
            expectEquals(1, count(row, "unpack"), "the advice to unpack, once");
        });

        check("C4-1: a file of zeros: no hex positions on the screen; the log's DISC line keeps them", () -> {
            SetupDiscKit.Device d = new SetupDiscKit.Device("c4-zeros", true).open();
            d.nextChoice = d.document("game.iso", new SetupTestKit.MemoryDisc(0, new byte[100 * 2048]));
            d.press(NavInput.CONFIRM);
            String row = d.model.detail(PLAY);
            expectEquals("This file is not a disc image of Synthetic release. " + PLAY_EXPECTED + " A: choose again.", row, "the whole row");
            expectContains(SetupDiscModelTest.lastLine(d.log, "DISC row=play chosen kind=NOT_A_DISC"), "0x0FD90000", "the positions are in the log");
        });

        check("C4-3: the first part of a real image: incomplete, its size in GB, no byte count", () -> {
            byte[] whole = SetupDiscKit.playImage();
            // as the 2 GB file of the vm-robust run: the image's start, cut inside the disc's data
            String row = refusedRow(PLAY, "Disc2-first2GB.iso", Arrays.copyOf(whole, whole.length - 2048), Disc2DiscsTest.XGD3);
            // the synthetic image is 34 MB: "(it is smaller than 0.1 GB)"; 2.1 GB gives "(it is only 2.1 GB)" (SetupFixesUnitTest)
            expectContains(row, "This disc image is incomplete: it ends before the disc's data does (it is smaller than 0.1 GB).", "said plainly");
        });

        check("defect 6: every refusal of the probe and of the disc reader is free of hex values and byte counts", () -> {
            byte[][] p = Disc2DiscsTest.packages();
            byte[] damaged = p[1].clone();
            damaged[(int) Disc2DiscsTest.pkg(1).build().table0(0) + 3] ^= 4;
            Disc2TestKit.Package foreign = Disc2DiscsTest.pkg(3);
            foreign.title = Disc2TestKit.TITLE + 1;
            byte[] tiny = new byte[1000];
            byte[] pirs = new byte[100 * 2048];
            System.arraycopy("PIRS".getBytes("US-ASCII"), 0, pirs, 0, 4);
            byte[][] images = {Disc2DiscsTest.installImage(new byte[][] {p[0], damaged, p[2], p[3]}).data,
                    Disc2DiscsTest.installImage(new byte[][] {p[0], p[1], p[2], foreign.build().data}).data,
                    Disc2DiscsTest.installImage(new byte[][] {p[0], p[1], p[2], Disc2DiscsTest.artPackage()}).data};
            int i = 0;
            for (byte[] image : images) refusedRow(INSTALL, "refused-" + (i++) + ".iso", image, 0);
            refusedRow(PLAY, "tiny.iso", tiny, 0);
            refusedRow(PLAY, "package.iso", pirs, 0);
            byte[] whole = SetupDiscKit.playImage();
            refusedRow(PLAY, "cut-table.iso", Arrays.copyOf(whole, 33 * 2048 + 10), Disc2DiscsTest.XGD3);
        });

        SetupTestKit.finish("SetupNotADiscTest");
    }
}
