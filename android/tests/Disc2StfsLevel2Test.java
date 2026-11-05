package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;

/**
 * Run with java -ea. Disc2Stfs on a package whose hash tree has three levels (more than 28,900 data blocks), which is
 * the form of all four packages on the real install disc. The package is Disc2TestKit.Sparse: 29,302 blocks whose
 * data is made on demand, so the test needs a few megabytes. The file in it crosses block 28,900, where the second
 * level-1 table and the level-2 table sit. No game data.
 */
public final class Disc2StfsLevel2Test {
    static final int FILE_BLOCKS = 29300;

    static String sha256Of(InputStream in) throws Exception {
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        byte[] buffer = new byte[70001];   // odd size: reads straddle runs and tables
        int n;
        while ((n = in.read(buffer, 0, buffer.length)) >= 0) sha.update(buffer, 0, n);
        in.close();
        return SetupTestKit.hex(sha.digest());
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

    /** The bytes the program holds on to: the heap in use after the collector ran (the lowest of three readings). */
    static long keptBytes() {
        Runtime runtime = Runtime.getRuntime();
        long least = Long.MAX_VALUE;
        for (int i = 0; i < 3; ++i) {
            System.gc();
            least = Math.min(least, runtime.totalMemory() - runtime.freeMemory());
        }
        return least;
    }

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();
        final Disc2TestKit.Sparse sparse = new Disc2TestKit.Sparse(FILE_BLOCKS);

        check("stfs level 2: a package of 29,302 blocks opens and both files read back with the right SHA-256", () -> {
            Disc2Stfs p = Disc2Stfs.open(sparse, Disc2TestKit.TITLE);
            expectEquals(FILE_BLOCKS + 2, p.dataBlocks(), "data blocks");
            Disc2Stfs.Entry huge = p.find("huge.rpf");
            expectEquals(sparse.fileBytes, huge.size, "size");
            expectEquals(sparse.fileSha256, sha256Of(p.openEntry(huge)), "a file across block 28,900");
            expectEquals(sparse.tailSha256, sha256Of(p.openEntry(p.find("huge.timestamp"))), "the file in the last block");
        });

        check("stfs level 2: a changed data block beyond block 28,900 is refused when it is read", () -> {
            Disc2TestKit.Sparse bad = new Disc2TestKit.Sparse(FILE_BLOCKS);
            final Disc2Stfs p = Disc2Stfs.open(bad, Disc2TestKit.TITLE);
            bad.flipBlock = 29100;
            expectContains(failure(() -> sha256Of(p.openEntry(p.find("huge.rpf")))), "Damaged: damaged package: the data of block 29100 of huge.rpf",
                    "the block is named");
        });

        check("stfs level 2: a changed level-2 table, second level-1 table, or level-0 table beyond block 28,900 is refused at open", () -> {
            long[] positions = {Disc2TestKit.tableOffset(0, 2), Disc2TestKit.tableOffset(28900, 1), Disc2TestKit.tableOffset(29240, 0)};
            String[] levels = {"level 2", "level 1", "level 0"};
            for (int i = 0; i < positions.length; ++i) {
                Disc2TestKit.Sparse bad = new Disc2TestKit.Sparse(FILE_BLOCKS);
                byte[] table = bad.tables.get(Long.valueOf(positions[i]));
                expect(table != null, "the fixture has a table at " + positions[i]);
                table[5] ^= 2;
                expectContains(failure(() -> Disc2Stfs.open(bad, Disc2TestKit.TITLE)), "a hash table does not match its hash (" + levels[i],
                        "table at " + positions[i]);
            }
        });

        check("stfs level 2: a package of 1,800,001 blocks is opened with less than 24 MiB kept, although its 10,589 level-0 hash tables"
                + " alone are 41 MiB: the tables are not all held; a file behind dropped tables still reads", () -> {
            Disc2TestKit.Vast vast = new Disc2TestKit.Vast(3, 600000);
            expectEquals(10589, vast.level0Tables, "level-0 tables of the fixture");
            long before = keptBytes();
            Disc2Stfs p = Disc2Stfs.open(vast, Disc2TestKit.TITLE);   // walks every file's chain, so it reads every level-0 table
            long kept = keptBytes() - before;
            expectEquals(1800001, p.dataBlocks(), "data blocks");
            expect(kept < 24L << 20, "bytes kept by the open package: " + kept + " (the block lists are 7.2 MB; all tables would add 43.4 MB)");
            InputStream in = p.openEntry(p.find("file2.rpf"));
            byte[] got = new byte[300 * 4096];
            int done = 0, n;
            while (done < got.length && (n = in.read(got, done, got.length - done)) >= 0) done += n;
            in.close();
            expectEquals(got.length, done, "bytes read from the last file");
            for (int i = 0; i < got.length; ++i) expect(got[i] == vast.data[i % 4096], "byte " + i + " of the last file");
        });

        SetupTestKit.finish("Disc2StfsLevel2Test");
    }
}
