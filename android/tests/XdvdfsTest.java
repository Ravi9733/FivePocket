package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.Node.dir;
import static dev.recompbootstrap.smoke.SetupTestKit.Node.file;
import static dev.recompbootstrap.smoke.SetupTestKit.Node.link;
import static dev.recompbootstrap.smoke.SetupTestKit.bytes;
import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Run with java -ea. The disc image is built by the test itself (SetupTestKit.Image); no game data. */
public final class XdvdfsTest {
    private static final byte[] EXECUTABLE = bytes(5000, 1);      // 2 sectors and a part: crosses sector boundaries
    private static final byte[] ARCHIVE = bytes(100, 2);
    private static final byte[] META = bytes(1028, 3);
    private static final byte[] LEAF = bytes(2048, 4);            // exactly one sector
    private static final byte[] AUDIO_A = bytes(4097, 5), AUDIO_B = bytes(7, 6), AUDIO_C = bytes(3000, 7);

    private static SetupTestKit.Node tree(SetupTestKit.Node... extraInData) {
        List<SetupTestKit.Node> data = new ArrayList<SetupTestKit.Node>();
        data.add(file("installpartitions.meta", META));
        data.add(dir("deep", file("leaf.bin", LEAF)));
        data.addAll(Arrays.asList(extraInData));
        return dir("",
                file("default.xex", EXECUTABLE),
                file("Common.RPF", ARCHIVE),
                file("empty.bin", new byte[0]),
                dir("common", dir("data", data.toArray(new SetupTestKit.Node[0]))),
                dir("sfx", file("audio_a.rpf", AUDIO_A), file("audio_b.rpf", AUDIO_B), file("audio_c.rpf", AUDIO_C)),
                dir("hollow"));
    }

    private static byte[] content(DiscSource source, Xdvdfs.Entry entry) throws IOException {
        InputStream in = VerifiedExtractor.rangeStream(source, entry.offset, entry.size, false);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[777];   // an odd size, so reads straddle sector boundaries
        int n;
        while ((n = in.read(buffer, 0, buffer.length)) >= 0) out.write(buffer, 0, n);
        return out.toByteArray();
    }

    private static String failure(SetupTestKit.Check action) {
        try {
            action.run();
        } catch (IOException e) {
            return e.getClass().getSimpleName() + ": " + e.getMessage();
        } catch (Exception e) {
            throw new AssertionError("unexpected " + e);
        }
        return "no failure";
    }

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();
        final SetupTestKit.Image image = new SetupTestKit.Image(tree());

        check("xdvdfs: the volume descriptor is found at each of the four partition offsets", () -> {
            long[] offsets = {0L, 0x0FD90000L, 0x02080000L, 0x18300000L};
            expectEquals(Arrays.toString(offsets), Arrays.toString(Xdvdfs.PARTITION_OFFSETS), "the supported offsets");
            for (long offset : offsets) {
                SetupTestKit.MemoryDisc disc = image.at(offset);
                Xdvdfs fs = Xdvdfs.open(disc);
                expect(fs != null, "found at " + offset);
                expectEquals(offset, fs.partitionOffset(), "partition offset");
                Xdvdfs.Entry exe = fs.find("default.xex");
                expect(exe != null && !exe.directory, "default.xex at " + offset);
                // the test's own image builder knows where it put the data: record -> first sector
                int record = image.records.get("default.xex");
                long sector = (image.data[record + 4] & 0xFFL) | (image.data[record + 5] & 0xFFL) << 8;
                expectEquals(offset + sector * 2048, exe.offset, "data position");
                expect(Arrays.equals(EXECUTABLE, content(disc, exe)), "bytes of default.xex at " + offset);
            }
        });

        check("xdvdfs: a file without the descriptor (or with half of it, or at an unknown offset) is not a disc", () -> {
            expect(Xdvdfs.open(new SetupTestKit.MemoryDisc(0, new byte[40 * 2048])) == null, "zeros");
            expect(Xdvdfs.open(new SetupTestKit.MemoryDisc(0, new byte[100])) == null, "too short");
            byte[] half = image.data.clone();
            Arrays.fill(half, 33 * 2048 - 20, 33 * 2048, (byte) 0);     // the text at the end of sector 32 is gone
            expect(Xdvdfs.open(new SetupTestKit.MemoryDisc(0, half)) == null, "only the first text");
            expect(Xdvdfs.open(image.at(0x1000)) == null, "unknown partition offset");
        });

        check("xdvdfs: three folder levels, names matched without regard to case, path reported as stored", () -> {
            SetupTestKit.MemoryDisc disc = image.at(0);
            Xdvdfs.Entry leaf = Xdvdfs.open(disc).find("COMMON/Data/DEEP/Leaf.BIN");
            expect(leaf != null, "found");
            expectEquals("common/data/deep/leaf.bin", leaf.path, "stored path");
            expectEquals(2048L, leaf.size, "size");
            expect(Arrays.equals(LEAF, content(disc, leaf)), "bytes");
            expect(Arrays.equals(META, content(disc, Xdvdfs.open(disc).find("common/data/installpartitions.meta"))), "meta bytes");
            expect(Arrays.equals(ARCHIVE, content(disc, Xdvdfs.open(disc).find("common.rpf"))), "a file beside a folder of a similar name");
        });

        check("xdvdfs: files that cross sector boundaries come out byte for byte", () -> {
            SetupTestKit.MemoryDisc disc = image.at(0x02080000L);
            Xdvdfs fs = Xdvdfs.open(disc);
            expect(Arrays.equals(AUDIO_A, content(disc, fs.find("sfx/audio_a.rpf"))), "4097 bytes");
            expect(Arrays.equals(AUDIO_C, content(disc, fs.find("sfx/audio_c.rpf"))), "3000 bytes");
            expectEquals(0L, fs.find("empty.bin").size, "empty file");
            expectEquals(0, content(disc, fs.find("empty.bin")).length, "empty file content");
        });

        check("xdvdfs: absent names, a file used as a folder, and folders", () -> {
            Xdvdfs fs = Xdvdfs.open(image.at(0));
            expect(fs.find("nope.bin") == null, "absent in the root");
            expect(fs.find("common/data/nope") == null, "absent in a folder");
            expect(fs.find("default.xex/inside") == null, "a file is not a folder");
            expect(fs.find("hollow/anything") == null, "nothing in an empty folder");
            expect(fs.find("common/data").directory, "a folder is reported as one");
            expect(fs.find("hollow").directory, "an empty folder too");
        });

        check("xdvdfs: list() gives every file once, depth first, names in alphabetical order", () -> {
            List<String> paths = new ArrayList<String>();
            for (Xdvdfs.Entry e : Xdvdfs.open(image.at(0)).list()) {
                expect(!e.directory, "files only");
                paths.add(e.path);
            }
            expectEquals(Arrays.asList("common/data/deep/leaf.bin", "common/data/installpartitions.meta", "Common.RPF",
                    "default.xex", "empty.bin", "sfx/audio_a.rpf", "sfx/audio_b.rpf", "sfx/audio_c.rpf"), paths, "listing");
        });

        check("xdvdfs: a folder that contains itself is refused, by list() and by a long find()", () -> {
            final SetupTestKit.Image looping = new SetupTestKit.Image(tree(link("again", "common")));
            // the damage is real: one step through the loop arrives at the same folder again
            expect(Xdvdfs.open(looping.at(0)).find("common/data/again/data/again/data/deep/leaf.bin") != null, "the loop exists");
            expectContains(failure(() -> Xdvdfs.open(looping.at(0)).list()), "Damaged: damaged disc image: a folder contains itself", "list()");
            StringBuilder deep = new StringBuilder("common");
            for (int i = 0; i < 40; ++i) deep.append("/data/again");
            expectContains(failure(() -> Xdvdfs.open(looping.at(0)).find(deep.toString())), "nested too deeply", "find()");
        });

        check("xdvdfs: a folder table whose entries loop is refused", () -> {
            byte[] data = image.data.clone();
            int record = image.records.get("sfx/audio_a.rpf");                  // not the first record of its table
            int table = (int) Xdvdfs.open(image.at(0)).find("sfx").offset;
            expect(record > table, "a record with a non-zero position");
            SetupTestKit.put16(data, record, (record - table) / 4);             // its left child is itself
            expectContains(failure(() -> Xdvdfs.open(new SetupTestKit.MemoryDisc(0, data)).list()), "table loops", "list()");
        });

        check("xdvdfs: damaged tables are refused with the reason", () -> {
            int record = image.records.get("sfx/audio_b.rpf");
            byte[] child = image.data.clone();
            SetupTestKit.put16(child, record + 2, 0x7000);                      // right child far outside the table
            expectContains(failure(() -> Xdvdfs.open(new SetupTestKit.MemoryDisc(0, child)).find("sfx/zzz")), "Damaged: damaged disc image: a folder entry lies outside its table", "child");
            byte[] name = image.data.clone();
            name[record + 13] = 0;                                              // a name of length 0
            expectContains(failure(() -> Xdvdfs.open(new SetupTestKit.MemoryDisc(0, name)).list()), "bad name", "name");
            byte[] huge = image.data.clone();
            SetupTestKit.put32(huge, 32 * 2048 + 24, 17L << 20);                // root table "17 MiB"
            expectContains(failure(() -> Xdvdfs.open(new SetupTestKit.MemoryDisc(0, huge)).list()), "larger than 16 MiB", "size");
            int sfxTable = (int) Xdvdfs.open(image.at(0)).find("sfx").offset;
            String cut = failure(() -> Xdvdfs.open(image.truncated(sfxTable + 100)).find("sfx/audio_a.rpf"));
            expectContains(cut, "EOFException", "an image cut inside a folder table");
            expect(!cut.contains("Damaged"), "a cut image is not reported as wrong tables: the two get different advice");
        });

        check("xdvdfs: a range that passes the end of the image fails when read, not silently", () -> {
            Xdvdfs.Entry exe = Xdvdfs.open(image.at(0)).find("default.xex");
            SetupTestKit.MemoryDisc cut = image.truncated((int) exe.offset + 3000);     // 3000 of 5000 bytes left
            Xdvdfs.Entry again = Xdvdfs.open(cut).find("default.xex");
            expectEquals(5000L, again.size, "the table still says 5000");
            expectContains(failure(() -> content(cut, again)), "EOFException", "reading it");
        });

        check("file disc source: reads a real file, refuses reads past its end", () -> {
            File files = SetupTestKit.newFilesDir("xdvdfs");
            File iso = new File(files, "disc.iso");
            SetupTestKit.write(iso, image.data);
            FileDiscSource source = new FileDiscSource(iso);
            try {
                expectEquals((long) image.data.length, source.size(), "size");
                Xdvdfs fs = Xdvdfs.open(source);
                expect(Arrays.equals(AUDIO_A, content(source, fs.find("sfx/audio_a.rpf"))), "bytes through a file");
                byte[] two = new byte[2];
                boolean eof = false;
                try {
                    source.readFully(image.data.length - 1, two, 0, 2);
                } catch (EOFException e) {
                    eof = true;
                }
                expect(eof, "EOFException one byte past the end");
                source.readFully(image.data.length - 2, two, 0, 2);
                expectEquals(image.data[image.data.length - 1], two[1], "the last byte");
            } finally {
                source.close();
            }
        });

        // For run_tests.sh: the synthetic image and the hashes of three of its files, so that the repository's own
        // host reader (class XisoDisc in tools/own_disc/disc_builder.py) can be asked to read the same image.
        String out = System.getProperty("setup.kitimage.out");
        if (out != null) {
            try {
                SetupTestKit.write(new File(out), image.data);
                String expected = "default.xex " + SetupTestKit.sha256(EXECUTABLE) + "\ncommon/data/deep/leaf.bin "
                        + SetupTestKit.sha256(LEAF) + "\nSFX/AUDIO_A.RPF " + SetupTestKit.sha256(AUDIO_A) + "\n";
                SetupTestKit.write(new File(out + ".expected"), expected.getBytes(java.nio.charset.StandardCharsets.US_ASCII));
                SetupTestKit.note("this test's synthetic disc image was written for the repository's host reader");
            } catch (IOException e) {
                throw new IllegalStateException(e);
            }
        }
        SetupTestKit.finish("XdvdfsTest");
    }
}
