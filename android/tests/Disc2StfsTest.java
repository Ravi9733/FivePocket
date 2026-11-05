package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.bytes;
import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;
import static dev.recompbootstrap.smoke.SetupTestKit.sha256;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Run with java -ea. Packages are built by Disc2TestKit.Package (a port of pycheck/stfs_synth.py); no game data. */
public final class Disc2StfsTest {
    // Values printed by pycheck/pins.py from the Python builder whose packages tools/host_guest_trace/stfs_inventory.py accepts.
    static final String PIN_SMALL = "6cfa35ef65efbade25ba44904eeaec708d5de47299e064181f7084a496c9ad7e";
    static final String PIN_BIG = "7af6a049b1a508cd106396b18633d2315c22622db19f29d989a98948b65e21ee";

    static final byte[] PART0 = bytes(10000, 1), STAMP = bytes(14, 2), INNER = bytes(4096, 3), BIG = bytes(200 * 4096 + 123, 4),
            NOTE = bytes(14, 5);

    static Disc2TestKit.Package small() {
        return new Disc2TestKit.Package().file("part0.rpf", PART0).file("stamp.dat", STAMP).file("sub/inner.bin", INNER)
                .file("empty.bin", new byte[0]);
    }

    static Disc2TestKit.Package big() {
        return new Disc2TestKit.Package().file("big.rpf", BIG).file("note.txt", NOTE);
    }

    static String failure(SetupTestKit.Check action) {
        try {
            action.run();
        } catch (Disc2Stfs.Damaged e) {
            return "Damaged: " + e.getMessage();
        } catch (IOException e) {
            return "IOException: " + e.getMessage();
        } catch (Exception e) {
            throw new AssertionError("unexpected " + e);
        }
        return "no failure";
    }

    static byte[] content(Disc2Stfs p, String path) throws IOException {
        InputStream in = p.openEntry(p.find(path));
        try {
            return Disc2TestKit.readAll(in);
        } finally {
            in.close();
        }
    }

    /** A source that records the largest single read, to show that nothing reads a whole entry at once. */
    static final class Largest implements DiscSource {
        final SetupTestKit.MemoryDisc inner;
        int largest;

        Largest(byte[] data) {
            inner = Disc2TestKit.disc(data);
        }

        @Override
        public long size() {
            return inner.size();
        }

        @Override
        public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
            largest = Math.max(largest, length);
            inner.readFully(position, buffer, offset, length);
        }

        @Override
        public void close() {
        }
    }

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();
        final byte[] smallData = small().build().data;
        final Disc2TestKit.Package bigPackage = big().build();

        check("stfs fixtures: the Java builder writes the same bytes as the Python builder the project reader accepts", () -> {
            expectEquals(PIN_SMALL, sha256(smallData), "small package");
            expectEquals(PIN_BIG, sha256(bigPackage.data), "level-1 package");
            expectEquals(1, bigPackage.rootLevel, "the big package uses a level-1 hash table");
        });

        check("stfs: a level-0 package lists files and folders with sizes, in directory order", () -> {
            Disc2Stfs p = Disc2Stfs.open(Disc2TestKit.disc(smallData), Disc2TestKit.TITLE);
            List<String> listed = new ArrayList<String>();
            for (Disc2Stfs.Entry e : p.entries()) listed.add(e.path + (e.directory ? "/" : "=" + e.size));
            expectEquals(Arrays.asList("sub/", "part0.rpf=10000", "stamp.dat=14", "sub/inner.bin=4096", "empty.bin=0"), listed, "entries");
            expect(p.find("SUB/Inner.BIN") != null, "case-insensitive find");
            expect(p.find("nope") == null, "absent");
        });

        check("stfs: every file of both packages reads back byte for byte (odd read sizes, empty file)", () -> {
            Disc2Stfs p = Disc2Stfs.open(Disc2TestKit.disc(smallData), Disc2TestKit.TITLE);
            expect(Arrays.equals(PART0, content(p, "part0.rpf")), "part0");
            expect(Arrays.equals(STAMP, content(p, "stamp.dat")), "stamp");
            expect(Arrays.equals(INNER, content(p, "sub/inner.bin")), "exactly one block");
            expectEquals(0, content(p, "empty.bin").length, "empty");
            Disc2Stfs q = Disc2Stfs.open(Disc2TestKit.disc(bigPackage.data), Disc2TestKit.TITLE);
            expect(Arrays.equals(BIG, content(q, "big.rpf")), "a file across the second level-0 table (blocks 169 -> 170)");
            expect(Arrays.equals(NOTE, content(q, "note.txt")), "a file after the second table");
        });

        check("stfs: an entry is streamed in runs of at most 256 KiB, never read whole", () -> {
            Largest source = new Largest(bigPackage.data);
            Disc2Stfs q = Disc2Stfs.open(source, Disc2TestKit.TITLE);
            source.largest = 0;
            expect(Arrays.equals(BIG, content(q, "big.rpf")), "content");
            expect(source.largest <= 64 * 4096, "largest read " + source.largest);
            expect(source.largest > 4096, "runs are used (largest read " + source.largest + ")");
        });

        check("stfs: a damaged data block is refused when that entry is read; the other entries still read", () -> {
            byte[] bad = smallData.clone();
            Disc2TestKit.Package layout = small().build();
            bad[(int) Disc2TestKit.dataOffset(layout.layout.get("part0.rpf")[1]) + 5] ^= 1;
            final Disc2Stfs p = Disc2Stfs.open(Disc2TestKit.disc(bad), Disc2TestKit.TITLE);   // opening checks tables only
            expectContains(failure(() -> content(p, "part0.rpf")), "Damaged: damaged package: the data of block", "part0");
            expect(Arrays.equals(STAMP, content(p, "stamp.dat")), "another entry");
        });

        check("stfs: a damaged hash block is refused at open (level 0 table, root hash, level 1 table, second level 0 table)", () -> {
            Disc2TestKit.Package layout = small().build();
            byte[] t0 = smallData.clone();
            t0[(int) layout.table0(0) + 24 * layout.layout.get("part0.rpf")[0] + 3] ^= (byte) 0x80;
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(t0), Disc2TestKit.TITLE)), "Damaged: damaged package: a hash table", "level 0");
            byte[] root = smallData.clone();
            root[0x379 + 8] ^= 1;
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(root), Disc2TestKit.TITLE)), "a hash table does not match", "root");
            for (long offset : new long[] {Disc2TestKit.tableOffset(0, 1), Disc2TestKit.tableOffset(170, 0)}) {
                byte[] bad = bigPackage.data.clone();
                bad[(int) offset + 30] ^= 4;
                expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(bad), Disc2TestKit.TITLE)), "a hash table does not match",
                        "table at " + offset);
            }
        });

        check("stfs: consistent hashes but broken chains are refused (loop, unallocated block, chain too long)", () -> {
            int[] part0 = small().build().layout.get("part0.rpf");
            Disc2TestKit.Package loop = small();
            loop.followingOverride.put(part0[1], part0[0]);
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(loop.build().data), Disc2TestKit.TITLE)), "share a block, or a chain loops", "loop");
            Disc2TestKit.Package free = small();
            free.statusOverride.put(part0[1], 1);
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(free.build().data), Disc2TestKit.TITLE)), "not allocated", "status");
            Disc2TestKit.Package longer = small();
            longer.followingOverride.put(part0[2], part0[2] + 1);
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(longer.build().data), Disc2TestKit.TITLE)), "longer than its record", "long");
        });

        check("stfs: directory records that lie are refused (size vs blocks, missing parent, unsafe name)", () -> {
            Disc2TestKit.Package size = small();
            size.rawHook = (path, raw) -> { if (path.equals("part0.rpf")) Disc2TestKit.be32(raw, 52, 10000 + 4096); };
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(size.build().data), Disc2TestKit.TITLE)), "size and its block count", "size");
            Disc2TestKit.Package orphan = small();
            orphan.rawHook = (path, raw) -> { if (path.equals("sub/inner.bin")) { raw[50] = 0; raw[51] = 40; } };
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(orphan.build().data), Disc2TestKit.TITLE)), "names a parent", "parent");
            Disc2TestKit.Package dots = small();
            dots.rawHook = (path, raw) -> {
                if (path.equals("stamp.dat")) {
                    Arrays.fill(raw, 0, 40, (byte) 0);
                    raw[0] = '.';
                    raw[1] = '.';
                    raw[40] = (byte) ((raw[40] & 0xC0) | 2);
                }
            };
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(dots.build().data), Disc2TestKit.TITLE)), "unsafe name", "..");
        });

        check("stfs: wrong game, wrong container, free blocks and a cut-off file are refused with their own sentence", () -> {
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(smallData), Disc2TestKit.TITLE + 1)), "another game (title 5E5E0001)", "title");
            byte[] live = smallData.clone();
            live[0] = 'L';
            live[1] = 'I';
            live[2] = 'V';
            live[3] = 'E';
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(live), Disc2TestKit.TITLE)), "not a PIRS package", "LIVE");
            Disc2TestKit.Package free = small();
            free.freeBlocks = 1;
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(free.build().data), Disc2TestKit.TITLE)), "block counts do not fit", "free");
            byte[] cut = Arrays.copyOf(smallData, smallData.length - 4096);
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(cut), Disc2TestKit.TITLE)), "block counts do not fit", "cut");
            expectContains(failure(() -> Disc2Stfs.open(Disc2TestKit.disc(new byte[100]), Disc2TestKit.TITLE)), "outside 38,682 bytes", "tiny");
        });

        check("stfs: extractTo writes a new file with the right SHA-256, never overwrites, removes a damaged copy", () -> {
            File dir = SetupTestKit.newFilesDir("stfs");
            Disc2Stfs q = Disc2Stfs.open(Disc2TestKit.disc(bigPackage.data), Disc2TestKit.TITLE);
            File out = new File(dir, "big.bin");
            expectEquals(sha256(BIG), q.extractTo(q.find("big.rpf"), out, null), "returned hash");
            expect(Arrays.equals(BIG, SetupTestKit.read(out)), "file content");
            expectContains(failure(() -> q.extractTo(q.find("note.txt"), out, null)), "IOException", "existing target refused");
            expect(Arrays.equals(BIG, SetupTestKit.read(out)), "existing target unchanged");
            byte[] bad = bigPackage.data.clone();
            bad[(int) Disc2TestKit.dataOffset(bigPackage.layout.get("big.rpf")[180]) + 1] ^= 1;
            final Disc2Stfs d = Disc2Stfs.open(Disc2TestKit.disc(bad), Disc2TestKit.TITLE);
            final File partial = new File(dir, "partial.bin");
            expectContains(failure(() -> d.extractTo(d.find("big.rpf"), partial, null)), "does not match its hash", "damaged");
            expect(!partial.exists(), "the incomplete file is removed");
        });

        SetupTestKit.finish("Disc2StfsTest");
    }
}
