package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.Node.dir;
import static dev.recompbootstrap.smoke.SetupTestKit.Node.file;
import static dev.recompbootstrap.smoke.SetupTestKit.bytes;
import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.EOFException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** Run with java -ea. Images are built by SetupTestKit.Image; no game data. */
public final class Disc2ImageProbeTest {
    static final byte[] EXE = bytes(5000, 1);

    static SetupTestKit.Image image() {
        return new SetupTestKit.Image(dir("", file("default.xex", EXE), dir("sfx", file("a.rpf", bytes(3000, 2)))));
    }

    /** A source of the given size that holds zero bytes everywhere except the two parts given (it costs no memory). */
    static DiscSource mostlyZero(final long size, final long firstAt, final byte[] first, final long secondAt, final byte[] second) {
        return new DiscSource() {
            @Override
            public long size() {
                return size;
            }

            @Override
            public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
                if (position < 0 || position + length > size) throw new EOFException("read past the end");
                for (int i = 0; i < length; ++i) {
                    long at = position + i;
                    buffer[offset + i] = at >= firstAt && at < firstAt + first.length ? first[(int) (at - firstAt)]
                            : at >= secondAt && at < secondAt + second.length ? second[(int) (at - secondAt)] : 0;
                }
            }

            @Override
            public void close() {
            }
        };
    }

    /** A volume descriptor sector that names a root folder at sector 40 with the given size in bytes. */
    static byte[] descriptor(long rootBytes) {
        byte[] sector = new byte[2048];
        byte[] magic = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(magic, 0, sector, 0, 20);
        System.arraycopy(magic, 0, sector, 2048 - 20, 20);
        SetupTestKit.put32(sector, 20, 40);
        SetupTestKit.put32(sector, 24, rootBytes);
        return sector;
    }

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();
        final SetupTestKit.Image image = image();

        check("probe: the partition offsets are the ones Xdvdfs uses (trimmed 0, XGD2, XGD3, XGD1)", () -> {
            expectEquals("[0, 265879552, 34078720, 405798912]", Arrays.toString(Xdvdfs.PARTITION_OFFSETS), "offsets");
            expectEquals(Disc2ImageProbe.Kind.TRIMMED, Disc2ImageProbe.kindOf(0), "0");
            expectEquals(Disc2ImageProbe.Kind.FULL_XGD2, Disc2ImageProbe.kindOf(0x0FD90000L), "XGD2");
            expectEquals(Disc2ImageProbe.Kind.FULL_XGD3, Disc2ImageProbe.kindOf(0x02080000L), "XGD3");
            expectEquals(Disc2ImageProbe.Kind.FULL_XGD1, Disc2ImageProbe.kindOf(0x18300000L), "XGD1");
        });

        check("probe: a trimmed image and full images of each layout are found and named", () -> {
            Disc2ImageProbe.Kind[] kinds = {Disc2ImageProbe.Kind.TRIMMED, Disc2ImageProbe.Kind.FULL_XGD2, Disc2ImageProbe.Kind.FULL_XGD3,
                    Disc2ImageProbe.Kind.FULL_XGD1};
            for (int i = 0; i < 4; ++i) {
                long offset = Xdvdfs.PARTITION_OFFSETS[i];
                Disc2ImageProbe.Result r = Disc2ImageProbe.probe(image.at(offset));
                expectEquals(kinds[i], r.kind, "kind at " + offset);
                expectEquals(offset, r.partitionOffset, "offset");
                expectContains(r.message, "game partition at byte " + offset, "sentence");
                // and the files are reachable through a window at that offset
                SetupTestKit.MemoryDisc disc = image.at(offset);
                Xdvdfs fs = Xdvdfs.open(new Disc2Window(disc, r.partitionOffset, disc.size() - r.partitionOffset));
                expectEquals(0L, fs.partitionOffset(), "the window starts at the partition");
                expectEquals(5000L, fs.find("default.xex").size, "executable through the window");
            }
        });

        check("probe: a stray volume text at 0 whose root is impossible does not hide the real XGD3 partition", () -> {
            byte[] full = new byte[(int) (0x02080000L + image.data.length)];
            System.arraycopy(image.data, 0, full, 0x02080000, image.data.length);
            byte[] magic = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
            System.arraycopy(magic, 0, full, 32 * 2048, 20);
            System.arraycopy(magic, 0, full, 33 * 2048 - 20, 20);
            SetupTestKit.put32(full, 32 * 2048 + 20, 0x7FFFFFF0L);   // root far beyond the end
            SetupTestKit.put32(full, 32 * 2048 + 24, 2048);
            SetupTestKit.MemoryDisc disc = new SetupTestKit.MemoryDisc(0, full);
            expectEquals(0L, Xdvdfs.open(disc).partitionOffset(), "Xdvdfs alone takes the first text it sees");
            Disc2ImageProbe.Result r = Disc2ImageProbe.probe(disc);
            expectEquals(Disc2ImageProbe.Kind.FULL_XGD3, r.kind, "the probe takes the valid partition");
        });

        check("probe: a stray volume text at 0 whose root folder holds no record does not hide the real XGD3 partition; alone it is damaged", () -> {
            byte[] magic = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
            byte[] full = new byte[(int) (0x02080000L + image.data.length)];
            System.arraycopy(image.data, 0, full, 0x02080000, image.data.length);
            byte[] alone = new byte[100 * 2048];
            for (byte[] target : new byte[][] {full, alone}) {
                System.arraycopy(magic, 0, target, 32 * 2048, 20);
                System.arraycopy(magic, 0, target, 33 * 2048 - 20, 20);
                SetupTestKit.put32(target, 32 * 2048 + 20, 40);   // a root folder inside the file, where only zero bytes are
                SetupTestKit.put32(target, 32 * 2048 + 24, 2048);
            }
            Disc2ImageProbe.Result r = Disc2ImageProbe.probe(new SetupTestKit.MemoryDisc(0, full));
            expectEquals(Disc2ImageProbe.Kind.FULL_XGD3, r.kind, "the probe goes on to the valid partition");
            expectEquals(0x02080000L, r.partitionOffset, "offset");
            Disc2ImageProbe.Result lone = Disc2ImageProbe.probe(new SetupTestKit.MemoryDisc(0, alone));
            expect(!lone.usable(), "a file system without a first record is not usable");
            // setup-fixes: the position is for the log (detail), the player's sentence has none
            expectContains(lone.detail, "the root folder of the disc file system at 0x00000000 is damaged", "log detail");
            expectEquals(Disc2ImageProbe.TEXT_CUT_OFF, lone.message, "sentence");
        });

        check("probe: a volume text whose root folder is larger than any folder list (17 MiB), or empty, is not taken, although that folder"
                + " would lie inside the file and begin with a well-formed record", () -> {
            // one well-formed first record at sector 40: no children, sector 100, 10 bytes, a file, the name a.bin
            byte[] record = new byte[19];
            SetupTestKit.put32(record, 4, 100);
            SetupTestKit.put32(record, 8, 10);
            record[12] = 0x20;
            record[13] = 5;
            System.arraycopy("a.bin".getBytes(StandardCharsets.US_ASCII), 0, record, 14, 5);
            long tooLarge = Xdvdfs.MAX_DIRECTORY_BYTES + (1L << 20);
            for (long rootBytes : new long[] {tooLarge, 0}) {
                Disc2ImageProbe.Result r = Disc2ImageProbe.probe(mostlyZero(1L << 30, 32 * 2048, descriptor(rootBytes), 40 * 2048, record));
                expect(!r.usable(), "a root folder of " + rootBytes + " bytes is not taken (" + r.kind + ": " + r.message + ")");
                expectEquals(Disc2ImageProbe.Kind.CUT_OFF, r.kind, "kind for " + rootBytes);
                expectEquals("Damaged image: the disc file system at 0x00000000 names an impossible root folder.", r.detail, "log detail for " + rootBytes);
                expectEquals(Disc2ImageProbe.TEXT_CUT_OFF, r.message, "sentence for " + rootBytes);
            }
            // the control: the same file with a root folder of one sector is taken, so it is the size that was refused above
            Disc2ImageProbe.Result fine = Disc2ImageProbe.probe(mostlyZero(1L << 30, 32 * 2048, descriptor(2048), 40 * 2048, record));
            expectEquals(Disc2ImageProbe.Kind.TRIMMED, fine.kind, "a root folder of 2048 bytes (" + fine.message + ")");
        });

        check("window: a window hands out its own bytes only: a read that passes its end fails before the source is asked, and a window that"
                + " passes the end of its source cannot be made", () -> {
            byte[] data = bytes(1000, 7);
            SetupTestKit.MemoryDisc source = new SetupTestKit.MemoryDisc(0, data);
            Disc2Window window = new Disc2Window(source, 100, 200);   // bytes 100..299; bytes 300..999 belong to "the next file"
            expectEquals(200L, window.size(), "size");
            byte[] whole = new byte[200];
            window.readFully(0, whole, 0, 200);
            expect(Arrays.equals(Arrays.copyOfRange(data, 100, 300), whole), "the window's bytes, to its last one");
            long[][] beyond = {{151, 50}, {200, 1}, {0, 201}, {-1, 2}};
            for (long[] read : beyond) {
                byte[] buffer = new byte[300];
                Arrays.fill(buffer, (byte) 0x55);
                int readsBefore = source.reads;
                try {
                    window.readFully(read[0], buffer, 0, (int) read[1]);
                    throw new AssertionError("a read of " + read[1] + " bytes at " + read[0] + " of a 200-byte window was served");
                } catch (IOException expected) {
                    expect(read[0] < 0 || expected instanceof EOFException, "a read past the end is an EOFException (" + expected + ")");
                }
                expectEquals(readsBefore, source.reads, "the source was not asked for the read at " + read[0]);
                for (byte b : buffer) expect(b == 0x55, "no byte was handed out by the read at " + read[0]);
            }
            try {
                new Disc2Window(source, 900, 101);
                throw new AssertionError("a window that ends one byte after its source was made");
            } catch (EOFException expected) {
                expectContains(expected.getMessage(), "the image is cut off inside this file", "reason");
            }
            expectEquals(100L, new Disc2Window(source, 900, 100).size(), "a window that ends with its source");
            for (long[] bad : new long[][] {{-1, 10}, {10, -1}}) {
                try {
                    new Disc2Window(source, bad[0], bad[1]);
                    throw new AssertionError("a window at " + bad[0] + " of " + bad[1] + " bytes was made");
                } catch (IOException expected) {
                    expectContains(expected.getMessage(), "negative size or position", "reason");
                }
            }
        });

        check("probe: a cut-off image is named as cut off, with the sizes", () -> {
            // descriptor at sector 32 kept, everything from the root table on cut away
            SetupTestKit.MemoryDisc cut = image.truncated(33 * 2048 + 10);
            Disc2ImageProbe.Result r = Disc2ImageProbe.probe(cut);
            expectEquals(Disc2ImageProbe.Kind.CUT_OFF, r.kind, "kind");
            expect(!r.usable(), "not usable");
            expectContains(r.detail, "the image is cut off", "log detail");
            expectContains(r.detail, "but the file has " + (33 * 2048 + 10), "file size named in the log");
            expectEquals(Disc2ImageProbe.TEXT_CUT_OFF, r.message, "sentence without sizes");
        });

        check("probe: other files get a sentence that says what they are", () -> {
            byte[] pirs = new byte[100 * 2048];
            System.arraycopy("PIRS".getBytes(StandardCharsets.US_ASCII), 0, pirs, 0, 4);
            expectContains(Disc2ImageProbe.probe(new SetupTestKit.MemoryDisc(0, pirs)).message, "content package", "package");
            byte[] xex = new byte[100 * 2048];
            System.arraycopy("XEX2".getBytes(StandardCharsets.US_ASCII), 0, xex, 0, 4);
            expectContains(Disc2ImageProbe.probe(new SetupTestKit.MemoryDisc(0, xex)).message, "a single game program file", "xex");
            byte[] zip = new byte[100 * 2048];
            zip[0] = 'P';
            zip[1] = 'K';
            zip[2] = 3;
            zip[3] = 4;
            expectContains(Disc2ImageProbe.probe(new SetupTestKit.MemoryDisc(0, zip)).message, "ZIP archive", "zip");
            byte[] iso = new byte[100 * 2048];
            System.arraycopy("CD001".getBytes(StandardCharsets.US_ASCII), 0, iso, 0x8001, 5);
            expectContains(Disc2ImageProbe.probe(new SetupTestKit.MemoryDisc(0, iso)).message, "ordinary CD or DVD image", "iso9660");
            expectContains(Disc2ImageProbe.probe(new SetupTestKit.MemoryDisc(0, new byte[1000])).message, "far too small", "tiny");
            expectContains(Disc2ImageProbe.probe(new SetupTestKit.MemoryDisc(0, new byte[1000])).detail, "smaller than any disc image", "tiny, log");
            Disc2ImageProbe.Result zeros = Disc2ImageProbe.probe(new SetupTestKit.MemoryDisc(0, new byte[100 * 2048]));
            expectEquals(Disc2ImageProbe.Kind.NOT_A_DISC_IMAGE, zeros.kind, "zeros");
            expectEquals("This file is not a disc image.", zeros.message, "zeros sentence");
            expectContains(zeros.detail, "no Xbox 360 game partition", "zeros, log");
        });

        check("probe: a read error is reported, not thrown", () -> {
            DiscSource broken = new DiscSource() {
                @Override
                public long size() {
                    return 1L << 30;
                }

                @Override
                public void readFully(long position, byte[] buffer, int offset, int length) throws java.io.IOException {
                    throw new java.io.IOException("I/O error");
                }

                @Override
                public void close() {
                }
            };
            Disc2ImageProbe.Result r = Disc2ImageProbe.probe(broken);
            expectEquals(Disc2ImageProbe.Kind.UNREADABLE, r.kind, "kind");
            expectContains(r.message, "I/O error", "sentence");
        });

        SetupTestKit.finish("Disc2ImageProbeTest");
    }
}
