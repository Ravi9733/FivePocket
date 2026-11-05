package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;
import static dev.recompbootstrap.smoke.SetupTestKit.sha256;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeMap;

/**
 * Run with java -ea. SetupDiscCopy on the synthetic discs of Disc2DiscsTest (a play disc in the XGD3 layout, an
 * install disc with four content packages). No game data. Each case names the safeguard it holds.
 */
public final class SetupDiscCopyTest {
    static final long GIB = 1L << 30;

    static Disc2Discs.Disc play() {
        return play(Disc2DiscsTest.EXE, Disc2DiscsTest.COMMON);
    }

    static Disc2Discs.Disc play(byte[] exe, byte[] common) {
        return Disc2Discs.open(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, Disc2DiscsTest.playImage(exe, common).data), Disc2DiscsTest.GAME);
    }

    static byte[] installData(byte[][] packages) {
        return Disc2DiscsTest.installImage(packages).data;
    }

    static Disc2Discs.Disc install() {
        return Disc2Discs.open(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, installData(Disc2DiscsTest.packages())), Disc2DiscsTest.GAME);
    }

    static List<Disc2Discs.Pin> playPins() {
        return Disc2DiscsTest.playPins();
    }

    static List<Disc2Discs.Pin> installPins() {
        return Disc2DiscsTest.installPins();
    }

    static List<Disc2Discs.Pin> all() {
        return Disc2DiscsTest.pins();
    }

    /** A progress that records what it was told and can stop the copy once a number of bytes was reported. */
    static final class Watch implements SetupDiscCopy.Progress {
        long stopAt = -1, last = -1;
        /** Stop once this many files were begun and another one starts (-1: never). */
        int stopAfterFiles = -1;
        boolean decreasing;
        final List<String> names = new ArrayList<String>();
        Runnable atFirstReport;

        @Override public void update(long done, long total, int index, int files, String name) {
            if (done < last) decreasing = true;
            last = done;
            if (!name.isEmpty() && (names.isEmpty() || !names.get(names.size() - 1).equals(name))) names.add(name);
            if (atFirstReport != null) {
                Runnable once = atFirstReport;
                atFirstReport = null;
                once.run();
            }
        }

        @Override public boolean cancelled() {
            return (stopAt >= 0 && last >= stopAt) || (stopAfterFiles >= 0 && names.size() > stopAfterFiles);
        }
    }

    static final List<SetupDiscCopy.FileResult> REPORTED = new ArrayList<SetupDiscCopy.FileResult>();
    static final SetupDiscCopy.Report REPORT = new SetupDiscCopy.Report() {
        @Override public void file(SetupDiscCopy.FileResult result) {
            REPORTED.add(result);
        }
    };

    static SetupDiscCopy.Outcome copy(Disc2Discs.Disc disc, List<Disc2Discs.Pin> expected, File root, SetupDiscCopy.Progress progress,
            VerifiedExtractor.Space space) {
        REPORTED.clear();
        return SetupDiscCopy.run(disc, expected, all(), Collections.<String>emptySet(), root, SetupDiscCopy.disk(root), progress, space, REPORT);
    }

    static File input(File root, Disc2Discs.Pin p) {
        return new File(root, "inputs/" + p.sha256 + ".bin");
    }

    static Set<String> names(File dir) {
        Set<String> out = new HashSet<String>();
        String[] list = dir.list();
        if (list != null) out.addAll(Arrays.asList(list));
        return out;
    }

    static Set<String> binNames(List<Disc2Discs.Pin> pins) {
        Set<String> out = new HashSet<String>();
        for (Disc2Discs.Pin p : pins) out.add(p.sha256 + ".bin");
        return out;
    }

    static List<String> verdicts(List<SetupDiscCopy.FileResult> results) {
        List<String> out = new ArrayList<String>();
        for (SetupDiscCopy.FileResult f : results) out.add(f.name + " " + f.verdict + (f.kept ? " kept" : ""));
        return out;
    }

    static File root(String label) throws IOException {
        return new File(SetupTestKit.newFilesDir(label), "private-trace");
    }

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();

        check("play disc: its 4 pinned files become inputs/(sha256).bin, read-only, staging empty, each reported once", () -> {
            File root = root("play");
            SetupDiscCopy.Outcome o = copy(play(), playPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(o.complete && !o.stopped && o.error == null, "complete: " + o.error);
            expectEquals(binNames(playPins()), names(new File(root, "inputs")), "the files made");
            for (Disc2Discs.Pin p : playPins()) {
                expectEquals(p.sha256, sha256(SetupTestKit.read(input(root, p))), "content of " + p.sha256);
                expectEquals("r--------", SetupTestKit.mode(input(root, p)), "mode of " + p.sha256);
            }
            expectEquals(new HashSet<String>(), names(new File(root, SetupDiscCopy.STAGING)), "staging is empty");
            expectEquals(4, o.kept(), "kept");
            expectEquals(o.files.size(), REPORTED.size(), "each file reported");
            expectEquals(4, o.filesToRead, "only candidates of a wanted size are read (nxeart and the update folder are not)");
        });

        check("install disc: its 8 files (four archives, four of 14 bytes) come out of the packages, checked block by block", () -> {
            File root = root("install");
            SetupDiscCopy.Outcome o = copy(install(), installPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(o.complete, "complete: " + o.error);
            expectEquals(binNames(installPins()), names(new File(root, "inputs")), "the files made");
            for (Disc2Discs.Pin p : installPins()) expectEquals(p.sha256, sha256(SetupTestKit.read(input(root, p))), "content");
        });

        check("either order, then a second run: nothing is read again, the files are not touched", () -> {
            File root = root("again");
            expect(copy(install(), installPins(), root, new Watch(), SetupTestKit.PLENTY).complete, "install first");
            expect(copy(play(), playPins(), root, new Watch(), SetupTestKit.PLENTY).complete, "then play");
            TreeMap<String, String> before = SetupTestKit.snapshot(root);
            SetupDiscCopy.Outcome o = copy(play(), playPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(o.complete && o.filesToRead == 0 && o.files.isEmpty(), "nothing to read");
            expectEquals(before, SetupTestKit.snapshot(root), "nothing changed");
            expectEquals(12, names(new File(root, "inputs")).size(), "all 12 entries");
        });

        check("judge: with three of the four 14-byte files in place all four are read; three are ALREADY_INSTALLED and discarded", () -> {
            File root = root("stamps");
            expect(copy(install(), installPins(), root, new Watch(), SetupTestKit.PLENTY).complete, "first");
            List<Disc2Discs.Pin> pins = installPins();
            Files.delete(input(root, pins.get(7)).toPath());   // STAMP3
            Files.delete(input(root, pins.get(6)).toPath());   // PART3, so that the plan has something large to read as well
            SetupDiscCopy.Outcome o = copy(install(), installPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(o.complete, "complete: " + o.error);
            int already = 0, wanted = 0;
            for (SetupDiscCopy.FileResult f : o.files) {
                if (f.verdict == Disc2Discs.Verdict.ALREADY_INSTALLED) {
                    ++already;
                    expect(!f.kept, "an already installed file is not kept again");
                }
                if (f.verdict == Disc2Discs.Verdict.WANTED) ++wanted;
            }
            expectEquals(3, already, "three 14-byte files already in place");
            expectEquals(2, wanted, "the two missing ones");
            expectEquals(new HashSet<String>(), names(new File(root, SetupDiscCopy.STAGING)), "staging is empty");
        });

        check("NOT_PINNED: a file of a pinned size with other content is discarded and the copy reports what the disc did not provide", () -> {
            File root = root("other");
            SetupDiscCopy.Outcome o = copy(play(Disc2DiscsTest.EXE, Disc2DiscsTest.COMMON_OTHER), playPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(!o.complete && o.problem == VerifiedExtractor.Problem.DISC_COPY_BAD, "refused: " + o.problem);
            expectContains(o.error, "did not provide 1 of the 4 files", "sentence");
            boolean seen = false;
            for (SetupDiscCopy.FileResult f : o.files) {
                if (f.name.equals("common.rpf")) {
                    seen = true;
                    expectEquals(Disc2Discs.Verdict.NOT_PINNED, f.verdict, "verdict");
                    expect(!f.kept, "not kept");
                }
            }
            expect(seen, "common.rpf was read and reported");
            expectEquals(3, names(new File(root, "inputs")).size(), "the other three are kept");
            expect(!new File(root, "inputs/" + sha256(Disc2DiscsTest.COMMON_OTHER) + ".bin").exists(), "the unpinned file was not kept");
            expectEquals(1, o.stillMissing.size(), "one entry still missing");
        });

        check("before anything is written: an install disc without its fourth package is refused, nothing is created", () -> {
            File root = root("short");
            byte[][] p = Disc2DiscsTest.packages();
            Disc2Discs.Disc three = Disc2Discs.open(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, installData(new byte[][] {p[0], p[1], p[2]})),
                    Disc2DiscsTest.GAME);
            expect(three.usable(), "recognised by its title alone");
            SetupDiscCopy.Outcome o = copy(three, installPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(o.problem == VerifiedExtractor.Problem.DISC_COPY_BAD, "refused");
            expectContains(o.error, "lacks 2 of the 8 files", "sentence");
            expect(!root.exists(), "nothing was created");
        });

        check("stop: B after the first file keeps it, removes the part file; continuing copies the rest and leaves the first untouched", () -> {
            File root = root("stop");
            Watch w = new Watch();
            w.stopAfterFiles = 1;   // as soon as the second file is begun
            SetupDiscCopy.Outcome o = copy(play(), playPins(), root, w, SetupTestKit.PLENTY);
            expect(o.stopped && !o.complete && o.error == null, "stopped");
            expectEquals(1, names(new File(root, "inputs")).size(), "the first file is kept");
            expectEquals(new HashSet<String>(), names(new File(root, SetupDiscCopy.STAGING)), "no part file left");
            TreeMap<String, String> first = SetupTestKit.snapshot(new File(root, "inputs"));
            SetupDiscCopy.Outcome again = copy(play(), playPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(again.complete, "continued to the end");
            TreeMap<String, String> after = SetupTestKit.snapshot(new File(root, "inputs"));
            for (String name : first.keySet()) expectEquals(first.get(name), after.get(name), "untouched: " + name);
            expectEquals(3, again.files.size(), "only the other three were read");
        });

        check("killed in the middle of a file: the part files it left are counted as free and removed before the space check", () -> {
            File root = root("killed");
            File staging = new File(root, SetupDiscCopy.STAGING);
            SetupTestKit.write(new File(staging, SetupDiscCopy.PART), new byte[50000]);
            SetupTestKit.write(new File(staging, "older.part"), new byte[30000]);
            SetupTestKit.write(new File(staging, "keep-me.txt"), new byte[10]);   // not a part file: never removed
            long need = SetupDiscCopy.bytes(playPins()) + SetupDiscCopy.RESERVE_BYTES;
            // The storage holds exactly the copy plus the part files: only removing them first makes room.
            VerifiedExtractor.Space quota = SetupTestKit.quota(root, need + 10 + 1000);
            expectEquals(need + 10 + 1000 - 80010, quota.usable(root), "before: the part files use the room");
            SetupDiscCopy.Outcome o = copy(play(), playPins(), root, new Watch(), quota);
            expect(o.complete, "complete: " + o.error);
            expectEquals(80000L, o.leftoversRemoved, "bytes removed");
            expectEquals(new HashSet<String>(Arrays.asList("keep-me.txt")), names(staging), "only the part files went");
            expectEquals(80000L, SetupDiscCopy.leftoverBytes(root) + 80000L, "nothing left over now");
            File rootB = root("killed-free");
            SetupTestKit.write(new File(new File(rootB, SetupDiscCopy.STAGING), SetupDiscCopy.PART), new byte[50000]);
            expectEquals(1000L + 50000L, SetupDiscCopy.freeWithLeftovers(rootB, SetupTestKit.spaceOf(1000)), "the page's free figure counts them");
        });

        check("space: too little room is refused before anything is read; the amounts are this disc's missing bytes plus 1 GiB", () -> {
            File root = root("space");
            long need = SetupDiscCopy.bytes(playPins()) + SetupDiscCopy.RESERVE_BYTES;
            SetupDiscCopy.Outcome o = copy(play(), playPins(), root, new Watch(), SetupTestKit.spaceOf(need - 1));
            expect(o.problem == VerifiedExtractor.Problem.NO_SPACE, "no space: " + o.problem);
            expectEquals(need, o.needBytes, "need");
            expectEquals(need - 1, o.freeBytes, "free");
            expect(o.files.isEmpty(), "nothing read");
            expectEquals(new HashSet<String>(), names(new File(root, "inputs")), "nothing made");
            expect(copy(play(), playPins(), root, new Watch(), SetupTestKit.spaceOf(need)).complete, "exactly enough is enough");
        });

        check("space is checked again before every file: a drop after the first file stops before the second is read", () -> {
            File root = root("drop");
            final SetupTestKit.Budget budget = new SetupTestKit.Budget(100 * GIB);
            SetupDiscCopy.Report drop = new SetupDiscCopy.Report() {
                @Override public void file(SetupDiscCopy.FileResult result) {
                    budget.free = 1000;   // another app filled the storage
                }
            };
            SetupDiscCopy.Outcome o = SetupDiscCopy.run(play(), playPins(), all(), Collections.<String>emptySet(), root, SetupDiscCopy.disk(root),
                    new Watch(), budget, drop);
            expect(o.problem == VerifiedExtractor.Problem.NO_SPACE, "stopped for space: " + o.problem);
            expectContains(o.error, "Not enough space left for", "names the next file");
            expectEquals(1, o.files.size(), "the second file was not read");
            expectEquals(1, names(new File(root, "inputs")).size(), "the first is kept");
        });

        check("a failing write is the device's storage, never the disc: STORAGE (or NO_SPACE when the room is short), part removed", () -> {
            File root = root("write");
            File staging = new File(root, SetupDiscCopy.STAGING);
            expect(staging.mkdirs(), "staging");
            expect(staging.setWritable(false, false), "read-only staging");
            try {
                SetupDiscCopy.Outcome o = copy(play(), playPins(), root, new Watch(), SetupTestKit.PLENTY);
                expect(o.problem == VerifiedExtractor.Problem.STORAGE, "storage: " + o.problem + " " + o.error);
                expectContains(o.error, "Could not write the copy of ", "sentence");
                final SetupTestKit.Budget budget = new SetupTestKit.Budget(100 * GIB);
                Watch w = new Watch();
                w.atFirstReport = new Runnable() {
                    @Override public void run() {
                        budget.free = 10;
                    }
                };
                SetupDiscCopy.Outcome full = copy(play(), playPins(), root, w, budget);
                expect(full.problem == VerifiedExtractor.Problem.NO_SPACE, "full: " + full.problem);
                expectContains(full.error, "ran out of space while copying ", "sentence");
            } finally {
                staging.setWritable(true, true);
            }
            expectEquals(new HashSet<String>(), names(staging), "nothing left in staging");
        });

        check("a failing read of the image is SOURCE_UNREADABLE (may work again); the part file is removed", () -> {
            File root = root("read");
            final byte[] data = Disc2DiscsTest.playImage(Disc2DiscsTest.EXE, Disc2DiscsTest.COMMON).data;
            final boolean[] fail = {false};
            DiscSource flaky = new Wrapped(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, data)) {
                @Override public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
                    if (fail[0]) throw new IOException("the storage was removed");
                    super.readFully(position, buffer, offset, length);
                }
            };
            Disc2Discs.Disc disc = Disc2Discs.open(flaky, Disc2DiscsTest.GAME);
            expect(disc.usable(), "recognised");
            fail[0] = true;
            SetupDiscCopy.Outcome o = copy(disc, playPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(o.problem == VerifiedExtractor.Problem.SOURCE_UNREADABLE, "unreadable: " + o.problem);
            expectContains(o.error, "could not be read from your disc image (the storage was removed)", "sentence");
            expectEquals(new HashSet<String>(), names(new File(root, SetupDiscCopy.STAGING)), "no part file");
        });

        check("a block that fails the package's hash, and an image cut off after it was opened, are a bad disc copy", () -> {
            File root = root("damaged");
            byte[] data = installData(Disc2DiscsTest.packages());
            Disc2Discs.Disc ok = Disc2Discs.open(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, data), Disc2DiscsTest.GAME);
            Disc2Discs.Candidate part0 = Disc2DiscsTest.named(ok.candidates, Disc2DiscsTest.PKG + "0/part0.rpf");
            byte[] first = Disc2DiscsTest.content(part0);
            // find the archive's first block in the image and change one byte of it
            int at = indexOf(data, Arrays.copyOf(first, 64));
            expect(at > 0, "found the archive's first block");
            data[at + 10] ^= 1;
            Disc2Discs.Disc bad = Disc2Discs.open(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, data), Disc2DiscsTest.GAME);
            expect(bad.usable(), "the tables are intact");
            SetupDiscCopy.Outcome o = copy(bad, installPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(o.problem == VerifiedExtractor.Problem.DISC_COPY_BAD, "bad copy: " + o.problem + " " + o.error);
            expectContains(o.error, "is damaged in your disc image", "sentence");
            expect(!input(root, installPins().get(0)).exists(), "not kept");
            final byte[] whole = Disc2DiscsTest.playImage(Disc2DiscsTest.EXE, Disc2DiscsTest.COMMON).data;
            final boolean[] cut = {false};
            DiscSource shrinking = new Wrapped(new SetupTestKit.MemoryDisc(Disc2DiscsTest.XGD3, whole)) {
                @Override public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
                    // the file behind the image was cut short after it was opened
                    if (cut[0] && position + length > Disc2DiscsTest.XGD3 + 40 * 2048) throw new java.io.EOFException("read past the end of the file");
                    super.readFully(position, buffer, offset, length);
                }
            };
            Disc2Discs.Disc opened = Disc2Discs.open(shrinking, Disc2DiscsTest.GAME);
            expect(opened.usable(), "recognised while whole");
            cut[0] = true;
            SetupDiscCopy.Outcome c = copy(opened, playPins(), root("cut"), new Watch(), SetupTestKit.PLENTY);
            expect(c.problem == VerifiedExtractor.Problem.DISC_COPY_BAD, "cut: " + c.problem + " " + c.error);
            expectContains(c.error, "is incomplete in your disc image", "sentence");
        });

        check("a final file of the wrong size is left alone until a verified copy replaces it in the rename; a folder there is refused", () -> {
            File root = root("wrong");
            Disc2Discs.Pin exe = playPins().get(0);
            SetupTestKit.write(input(root, exe), new byte[7]);
            Watch w = new Watch();
            w.stopAt = 1;   // stop inside the first file
            copy(play(), playPins(), root, w, SetupTestKit.PLENTY);
            expectEquals(7L, input(root, exe).length(), "untouched while no verified copy exists");
            expect(copy(play(), playPins(), root, new Watch(), SetupTestKit.PLENTY).complete, "complete");
            expectEquals(exe.sha256, sha256(SetupTestKit.read(input(root, exe))), "replaced by the verified copy");
            File rootB = root("folder");
            expect(input(rootB, exe).mkdirs(), "a folder under the name");
            SetupDiscCopy.Outcome o = copy(play(), playPins(), rootB, new Watch(), SetupTestKit.PLENTY);
            expect(o.problem == VerifiedExtractor.Problem.STORAGE, "refused: " + o.problem);
            expectContains(o.error, "a folder stands under the name", "sentence");
            expect(input(rootB, exe).isDirectory(), "the folder is still there");
        });

        check("notTrusted (ShipData.different): a file of the right name and size that is not the pinned file is replaced", () -> {
            File root = root("trust");
            expect(copy(install(), installPins(), root, new Watch(), SetupTestKit.PLENTY).complete, "first");
            Disc2Discs.Pin stamp = installPins().get(1);
            File f = input(root, stamp);
            f.setWritable(true, true);
            SetupTestKit.write(f, new byte[14]);   // right size, wrong content
            SetupDiscCopy.Outcome trusting = copy(install(), installPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(trusting.complete && trusting.files.isEmpty(), "by name and size it is in place");
            REPORTED.clear();
            SetupDiscCopy.Outcome o = SetupDiscCopy.run(install(), installPins(), all(), Collections.singleton(stamp.sha256), root,
                    SetupDiscCopy.disk(root), new Watch(), SetupTestKit.PLENTY, REPORT);
            expect(o.complete, "complete: " + o.error);
            expectEquals(stamp.sha256, sha256(SetupTestKit.read(f)), "replaced");
        });

        check("--hash-only: the same decisions file by file, nothing written, no space needed; the only difference is the store", () -> {
            File root = root("hash");
            File empty = root("hash-disk");
            SetupDiscCopy.Outcome disk = copy(play(Disc2DiscsTest.EXE, Disc2DiscsTest.COMMON_OTHER), playPins(), empty, new Watch(), SetupTestKit.PLENTY);
            List<String> onDisk = verdicts(disk.files);
            REPORTED.clear();
            SetupDiscCopy.Outcome sink = SetupDiscCopy.run(play(Disc2DiscsTest.EXE, Disc2DiscsTest.COMMON_OTHER), playPins(), all(),
                    Collections.<String>emptySet(), root, SetupDiscCopy.hashOnly(new File(root, "inputs")), new Watch(), SetupTestKit.spaceOf(0), REPORT);
            expectEquals(onDisk, verdicts(sink.files), "the same verdicts");
            expectEquals(disk.problem, sink.problem, "the same end");
            expectEquals(disk.error, sink.error, "the same sentence");
            expect(!root.exists(), "nothing was written");
            REPORTED.clear();
            SetupDiscCopy.Outcome whole = SetupDiscCopy.run(play(), playPins(), all(), Collections.<String>emptySet(), root,
                    SetupDiscCopy.hashOnly(new File(root, "inputs")), new Watch(), SetupTestKit.spaceOf(0), REPORT);
            expect(whole.complete && whole.kept() == 4, "complete in the sink with no free space at all");
        });

        check("an image that is not one of the two discs: nothing is read; the page's sentence names no file, the log's keeps the name", () -> {
            File root = root("unusable");
            Disc2Discs.Disc none = Disc2Discs.open(new SetupTestKit.Image(SetupTestKit.Node.dir("", SetupTestKit.Node.file("movie.bin",
                    Disc2DiscsTest.AUDIO))).at(0), Disc2DiscsTest.GAME);
            SetupDiscCopy.Outcome o = copy(none, playPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(o.problem == VerifiedExtractor.Problem.DISC_COPY_BAD && o.files.isEmpty(), "refused before anything is read: " + o.problem);
            expectEquals("This disc image holds no game program, so it is not a disc of Synthetic release.", o.shown, "the page's sentence");
            expectEquals(o.shown + " [default.xex]", o.error, "the log's sentence: the same with the name the page leaves out");
            expect(!new File(root, SetupDiscCopy.STAGING).exists(), "nothing was prepared");
        });

        check("one copy at a time: while another copy holds the lock the copy is BUSY and touches nothing", () -> {
            File root = root("busy");
            expect(VerifiedExtractor.begin(), "take the lock");
            try {
                SetupDiscCopy.Outcome o = copy(play(), playPins(), root, new Watch(), SetupTestKit.PLENTY);
                expect(o.problem == VerifiedExtractor.Problem.BUSY, "busy");
            } finally {
                VerifiedExtractor.end();
            }
            expect(!root.exists(), "nothing created");
            expect(copy(play(), playPins(), root, new Watch(), SetupTestKit.PLENTY).complete, "the lock was given back");
        });

        check("links: a link standing as the part file is never written through; a linked inputs folder is refused", () -> {
            File root = root("links");
            File outside = new File(root.getParentFile(), "outside.bin");
            SetupTestKit.write(outside, new byte[] {1, 2, 3});
            File staging = new File(root, SetupDiscCopy.STAGING);
            expect(staging.mkdirs(), "staging");
            Files.createSymbolicLink(new File(staging, SetupDiscCopy.PART).toPath(), outside.toPath());
            expectEquals(0L, SetupDiscCopy.leftoverBytes(root), "a link is not counted as a left-over");
            expect(copy(play(), playPins(), root, new Watch(), SetupTestKit.PLENTY).complete, "complete");
            expectEquals(3L, outside.length(), "the file the link pointed to is unchanged");
            File rootB = root("linked-inputs");
            File elsewhere = new File(rootB.getParentFile(), "elsewhere");
            expect(elsewhere.mkdirs() && rootB.mkdirs(), "folders");
            Files.createSymbolicLink(new File(rootB, "inputs").toPath(), elsewhere.toPath());
            SetupDiscCopy.Outcome o = copy(play(), playPins(), rootB, new Watch(), SetupTestKit.PLENTY);
            expect(o.problem == VerifiedExtractor.Problem.STORAGE, "refused: " + o.problem + " " + o.error);
            expectEquals(0, names(elsewhere).size(), "nothing written through the link");
        });

        check("a symbolic link under a disc file's name is not the file: the copy reads the disc and puts the verified file in its place, the link's target untouched", () -> {
            // The ship build's Ready rule does not follow such a link; if the copy's own look did, the page would show
            // the disc as not done while the copy found nothing to do.
            File root = root("link-as-file");
            Disc2Discs.Pin exe = playPins().get(0);
            File outside = new File(root.getParentFile(), "outside-" + exe.sha256 + ".bin");
            Disc2Discs.Disc disc = play();
            byte[] right = null;
            for (Disc2Discs.Candidate c : disc.candidates) {
                if (c.bytes != exe.bytes) continue;
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                if (Disc2Discs.sha256(c, bytes, null).equals(exe.sha256)) right = bytes.toByteArray();
            }
            expect(right != null, "(the test found the file's bytes on the disc)");
            SetupTestKit.write(outside, right);                        // a file with exactly the right bytes, outside the app's storage
            expect(new File(root, "inputs").mkdirs(), "inputs");
            Files.createSymbolicLink(input(root, exe).toPath(), outside.toPath());
            expect(!Disc2Discs.installed(new File(root, "inputs"), playPins()).contains(exe.sha256), "the link is not counted as the file in place");
            expectEquals(1, Disc2Discs.missing(new File(root, "inputs"), Collections.singletonList(exe)).size(), "it is missing");
            expect(Disc2Discs.wrongContent(new File(root, "inputs"), Collections.singletonList(exe), null).isEmpty(), "and the slow check does not read through it");
            SetupDiscCopy.Outcome o = copy(disc, playPins(), root, new Watch(), SetupTestKit.PLENTY);
            expect(o.complete, "complete: " + o.error);
            expectEquals(4, o.kept(), "all four files were read and kept, the linked one among them");
            expect(Files.isRegularFile(input(root, exe).toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS), "a real file stands under the name now");
            expectEquals(exe.sha256, sha256(SetupTestKit.read(input(root, exe))), "with the pinned content");
            expect(outside.isFile() && Arrays.equals(right, SetupTestKit.read(outside)), "what the link pointed to is as it was");
        });

        check("progress: bytes never go back, the total is the plan's, the names are the candidates' in order", () -> {
            File root = root("progress");
            Watch w = new Watch();
            SetupDiscCopy.Outcome o = copy(play(), playPins(), root, w, SetupTestKit.PLENTY);
            expect(o.complete, "complete");
            expect(!w.decreasing, "never back");
            expectEquals(o.bytesToRead, w.last, "ends at the total");
            List<String> read = new ArrayList<String>();
            for (SetupDiscCopy.FileResult f : o.files) read.add(f.name);
            expectEquals(read, w.names, "names in order");
        });

        SetupTestKit.finish("SetupDiscCopyTest");
    }

    /** A disc source that passes everything on; tests override one method. */
    static class Wrapped implements DiscSource {
        final DiscSource inner;

        Wrapped(DiscSource inner) {
            this.inner = inner;
        }

        @Override public long size() throws IOException {
            return inner.size();
        }

        @Override public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
            inner.readFully(position, buffer, offset, length);
        }

        @Override public void close() throws IOException {
            inner.close();
        }
    }

    static int indexOf(byte[] data, byte[] part) {
        outer:
        for (int i = 0; i + part.length <= data.length; ++i) {
            for (int j = 0; j < part.length; ++j) {
                if (data[i + j] != part[j]) continue outer;
            }
            return i;
        }
        return -1;
    }
}
