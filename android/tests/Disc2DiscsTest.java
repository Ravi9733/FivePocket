package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.Node.dir;
import static dev.recompbootstrap.smoke.SetupTestKit.Node.file;
import static dev.recompbootstrap.smoke.SetupTestKit.bytes;
import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;
import static dev.recompbootstrap.smoke.SetupTestKit.sha256;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.channels.Channels;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.TreeSet;

/**
 * Run with java -ea. Disc2Discs on synthetic images only: a play disc in the full XGD3 layout and a trimmed install
 * disc holding four PIRS packages, laid out like the real discs (a package of another content type named nxeart on
 * both, a system-update folder, an install disc whose own default.xex is not the play disc's). No game data.
 */
public final class Disc2DiscsTest {
    static final byte[] EXE = bytes(70000, 31), OTHER_EXE = bytes(70000, 32);
    static final byte[] COMMON = bytes(3000, 33), COMMON_OTHER = bytes(3000, 34), AUDIO = bytes(9000, 35), META = bytes(1028, 36),
            UPDATE = bytes(50000, 37);
    static final byte[] PART0 = bytes(200 * 4096 + 123, 38), PART1 = bytes(20000, 39), PART2 = bytes(12000, 40), PART3 = bytes(5000, 41);
    static final byte[] STAMP0 = bytes(14, 42), STAMP1 = bytes(14, 43), STAMP2 = bytes(14, 44), STAMP3 = bytes(14, 45);
    static final long XGD3 = 0x02080000L;
    static final String PKG = "5E5E00010000000";   // + 0..3: the package file names, as on the real disc (title + number)
    static final String FOLDER = "content/0000000000000000/5E5E0001/00000002/";   // the game's install-content folder
    static final Disc2Discs.Game GAME = new Disc2Discs.Game("Synthetic release", "default.xex", sha256(EXE), Disc2TestKit.TITLE);
    /** What the player is told to do with an image that is not one of the two supported discs as far as it shows. */
    static final String ADVICE = "This version works with the two discs of Synthetic release only: choose the image of its play disc (Disc 2)"
            + " or of its install disc (Disc 1). If this image was made from one of them, make a new copy of that disc: this copy is damaged.";

    /** A package of this game's title that is not install content, as "nxeart" on both real discs. */
    static byte[] artPackage() {
        Disc2TestKit.Package art = new Disc2TestKit.Package().file("art.bin", bytes(5000, 46));
        art.contentType = 0x30000;
        return art.build().data;
    }

    /** The play disc; sfx/audio_a.rpf is the last file in the image. */
    static SetupTestKit.Image playImage(byte[] executable, byte[] common) {
        return new SetupTestKit.Image(dir("", file("default.xex", executable), file("common.rpf", common),
                dir("common", dir("data", file("installpartitions.meta", META))), dir("$SystemUpdate", file("su.bin", UPDATE)),
                file("nxeart", artPackage()), dir("sfx", file("audio_a.rpf", AUDIO))));
    }

    static Disc2TestKit.Package pkg(int number) {
        byte[][] parts = {PART0, PART1, PART2, PART3}, stamps = {STAMP0, STAMP1, STAMP2, STAMP3};
        Disc2TestKit.Package p = new Disc2TestKit.Package().file("part" + number + ".rpf", parts[number])
                .file("part" + number + ".timestamp", stamps[number]);
        if (number == 3) p.folder("extras");   // a folder inside a package is not a file to offer
        return p;
    }

    /** The install disc with the given four packages; the last package is the last file in the image. */
    static SetupTestKit.Image installImage(byte[][] packages) {
        return installImage("5E5E0001", packages);
    }

    /** The same with the content folder of another title: packages[i] becomes content/0000000000000000/TITLE/00000002/PKG+i. */
    static SetupTestKit.Image installImage(String titleFolder, byte[][] packages) {
        Disc2TestKit.Package other = new Disc2TestKit.Package().file("x.bin", bytes(10, 47));
        other.title = Disc2TestKit.TITLE + 1;
        SetupTestKit.Node[] files = new SetupTestKit.Node[packages.length];
        for (int i = 0; i < packages.length; ++i) files[i] = file(PKG + i, packages[i]);
        return new SetupTestKit.Image(dir("", file("default.xex", OTHER_EXE), file("common.rpf", COMMON), file("nxeart", artPackage()),
                file("OTHER", other.build().data), dir("$SystemUpdate", file("su.bin", UPDATE)),
                dir("content", dir("0000000000000000", dir(titleFolder, dir("00000002", files))))));
    }

    static byte[][] packages() {
        return new byte[][] {pkg(0).build().data, pkg(1).build().data, pkg(2).build().data, pkg(3).build().data};
    }

    static List<String> listed(List<Disc2Discs.Candidate> candidates) {
        List<String> out = new ArrayList<String>();
        for (Disc2Discs.Candidate c : candidates) out.add(c.name + "=" + c.bytes + (c.blockChecked ? " checked" : ""));
        return out;
    }

    static Disc2Discs.Candidate named(List<Disc2Discs.Candidate> candidates, String name) {
        for (Disc2Discs.Candidate c : candidates) {
            if (c.name.equals(name)) return c;
        }
        throw new AssertionError("no candidate " + name);
    }

    static byte[] content(Disc2Discs.Candidate c) throws IOException {
        InputStream in = c.open();
        try {
            return Disc2TestKit.readAll(in);
        } finally {
            in.close();
        }
    }

    static Disc2Discs.Pin pin(byte[] content) {
        return new Disc2Discs.Pin(sha256(content), content.length);
    }

    /** The pinned record of the synthetic release: the play disc's four files, then the install disc's eight. */
    static List<Disc2Discs.Pin> pins() {
        return Arrays.asList(pin(EXE), pin(COMMON), pin(META), pin(AUDIO), pin(PART0), pin(STAMP0), pin(PART1), pin(STAMP1), pin(PART2),
                pin(STAMP2), pin(PART3), pin(STAMP3));
    }

    /** The entries the play disc is to provide, and those the install disc is to provide. */
    static List<Disc2Discs.Pin> playPins() {
        return pins().subList(0, 4);
    }

    static List<Disc2Discs.Pin> installPins() {
        return pins().subList(4, 12);
    }

    // ---- BEGIN the copy loop of INTEGRATION.md section 3 ----
    /**
     * Copies from one disc image what the pinned record expects of that disc. root is the folder that holds inputs/
     * and staging/ (files/private-trace on the device); expected are the pinned entries this disc is to provide.
     * Returns null when all of them are in place afterwards, else the sentence for the player.
     */
    static String copyDisc(DiscSource image, Disc2Discs.Game game, List<Disc2Discs.Pin> expected, File root, Disc2Discs.Watch watch)
            throws IOException {
        File inputs = new File(root, "inputs"), staging = new File(root, "staging");
        VerifiedExtractor.makeDirectory(inputs);
        VerifiedExtractor.makeDirectory(staging);
        VerifiedExtractor.clearLeftovers(root);   // staging/*.part of a run that was stopped or killed
        Disc2Discs.Disc disc = Disc2Discs.open(image, game);
        if (!disc.usable()) return disc.message + " " + disc.next;
        Disc2Discs.Plan plan = Disc2Discs.plan(disc.candidates, expected, Disc2Discs.installed(inputs, expected));
        if (!plan.shortSizes.isEmpty()) {   // asked before anything is copied
            return "This disc image lacks " + plan.shortSizes.size() + " of the files this disc should hold. " + Disc2Discs.NEXT_NEW_COPY;
        }
        for (Disc2Discs.Candidate c : plan.read) {   // plan.bytes is the progress total and the space still needed
            File part = new File(staging, "disc-file.part");
            try {
                String sha;
                FileChannel out = VerifiedExtractor.createNew(part);
                try {
                    sha = Disc2Discs.sha256(c, Channels.newOutputStream(out), watch);
                    out.force(true);   // the bytes are on storage before the file gets its final name
                } finally {
                    out.close();
                }
                if (plan.judge(c.bytes, sha) == Disc2Discs.Verdict.WANTED) {
                    Files.move(part.toPath(), new File(inputs, sha + ".bin").toPath(), StandardCopyOption.ATOMIC_MOVE);
                    VerifiedExtractor.syncDirectory(inputs);
                }
            } finally {
                part.delete();   // nothing to remove after the rename; after a stop, a failure or an unwanted file it empties staging
            }
        }
        List<Disc2Discs.Pin> missing = Disc2Discs.missing(inputs, expected);   // asked after the copy
        if (missing.isEmpty()) return null;
        return "This disc image did not provide " + missing.size() + " of the files this disc should hold. " + Disc2Discs.NEXT_NEW_COPY;
    }
    // ---- END the copy loop of INTEGRATION.md section 3 ----

    /** The names in a folder, sorted. */
    static List<String> filesIn(File directory) {
        String[] names = directory.list();
        return new ArrayList<String>(new TreeSet<String>(Arrays.asList(names == null ? new String[0] : names)));
    }

    static List<String> finalNames(List<Disc2Discs.Pin> pins) {
        TreeSet<String> out = new TreeSet<String>();
        for (Disc2Discs.Pin p : pins) out.add(p.sha256 + ".bin");
        return new ArrayList<String>(out);
    }

    static List<String> names(List<Disc2Discs.Candidate> candidates) {
        List<String> out = new ArrayList<String>();
        for (Disc2Discs.Candidate c : candidates) out.add(c.name);
        return out;
    }

    /** A source that fails every read that touches [from, to). */
    static DiscSource failingBetween(final DiscSource inner, final long from, final long to) {
        return new DiscSource() {
            @Override
            public long size() throws IOException {
                return inner.size();
            }

            @Override
            public void readFully(long position, byte[] buffer, int offset, int length) throws IOException {
                if (position < to && position + length > from) throw new IOException("I/O error");
                inner.readFully(position, buffer, offset, length);
            }

            @Override
            public void close() {
            }
        };
    }

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();
        final SetupTestKit.Image play = playImage(EXE, COMMON);
        final byte[][] packages = packages();
        final SetupTestKit.Image install = installImage(packages);

        check("discs: the play disc is recognised; it offers its files outside the system-update folder and each reads back byte for byte", () -> {
            Disc2Discs.Disc disc = Disc2Discs.open(play.at(XGD3), GAME);
            expectEquals(Disc2Discs.Kind.PLAY, disc.kind, "kind (" + disc.message + ")");
            expectEquals("Recognised: Synthetic release, the play disc (5 files).", disc.message, "sentence");
            expect(disc.next == null && disc.usable(), "usable, nothing to do");
            expectEquals(XGD3, disc.partitionOffset, "partition");
            expectEquals(Arrays.asList("common/data/installpartitions.meta=1028", "common.rpf=3000", "default.xex=70000",
                    "nxeart=" + artPackage().length, "sfx/audio_a.rpf=9000"), listed(disc.candidates), "candidates");
            expect(Arrays.equals(META, content(named(disc.candidates, "common/data/installpartitions.meta"))), "meta");
            expect(Arrays.equals(AUDIO, content(named(disc.candidates, "sfx/audio_a.rpf"))), "the last file of the image");
            expect(Arrays.equals(EXE, content(named(disc.candidates, "default.xex"))), "executable");
            expectContains(disc.ignored.toString(), "nxeart (a package of content type 196608, not install content)", "the art package is named");
        });

        check("discs: the install disc is recognised by its content packages and offers the files inside them, block-checked;"
                + " other packages and the disc's loose files are not offered", () -> {
            Disc2Discs.Disc disc = Disc2Discs.open(install.at(0), GAME);
            expectEquals(Disc2Discs.Kind.INSTALL, disc.kind, "kind (" + disc.message + ")");
            expectEquals("Recognised: Synthetic release, the install disc (4 content packages, 8 files).", disc.message, "sentence");
            expect(disc.next == null && disc.usable(), "usable, nothing to do");
            expectEquals(Arrays.asList(PKG + "0/part0.rpf=" + PART0.length + " checked", PKG + "0/part0.timestamp=14 checked",
                    PKG + "1/part1.rpf=20000 checked", PKG + "1/part1.timestamp=14 checked", PKG + "2/part2.rpf=12000 checked",
                    PKG + "2/part2.timestamp=14 checked", PKG + "3/part3.rpf=5000 checked", PKG + "3/part3.timestamp=14 checked"),
                    listed(disc.candidates), "candidates");
            expect(Arrays.equals(PART0, content(named(disc.candidates, PKG + "0/part0.rpf"))), "a file under a level-1 hash table");
            expect(Arrays.equals(STAMP3, content(named(disc.candidates, PKG + "3/part3.timestamp"))), "the last file of the last package");
            expectEquals("content/0000000000000000/5E5E0001/00000002/" + PKG + "0", disc.packages.get(0), "package path");
            expectContains(disc.ignored.toString(), "OTHER (install content of another title, 5E5E0002)", "another title's package");
            expectContains(disc.ignored.toString(), "nxeart (a package of content type 196608, not install content)", "the art package");
        });

        check("discs: a package block that does not match its hash stops the read of that file as Disc2Stfs.Damaged; other files still read", () -> {
            byte[] bad = packages[0].clone();
            bad[(int) Disc2TestKit.dataOffset(pkg(0).build().layout.get("part0.rpf")[150]) + 9] ^= 1;
            final Disc2Discs.Disc disc = Disc2Discs.open(installImage(new byte[][] {bad, packages[1], packages[2], packages[3]}).at(0), GAME);
            expectEquals(Disc2Discs.Kind.INSTALL, disc.kind, "the tables are intact, so the disc opens");
            try {
                Disc2Discs.sha256(named(disc.candidates, PKG + "0/part0.rpf"), null, null);
                throw new AssertionError("the damaged file read to its end");
            } catch (Disc2Stfs.Damaged e) {
                expectContains(e.getMessage(), "of part0.rpf does not match its hash", "reason");
            }
            expectEquals(sha256(STAMP0), Disc2Discs.sha256(named(disc.candidates, PKG + "0/part0.timestamp"), null, null), "the next file");
        });

        check("discs: a damaged hash table, and a package in a form this app does not read, refuse the whole disc with a sentence that does"
                + " not name the package", () -> {
            byte[] bad = packages[1].clone();
            bad[(int) pkg(1).build().table0(0) + 3] ^= 4;
            Disc2Discs.Disc disc = Disc2Discs.open(installImage(new byte[][] {packages[0], bad, packages[2], packages[3]}).at(0), GAME);
            expectEquals(Disc2Discs.Kind.DAMAGED, disc.kind, "kind (" + disc.message + ")");
            // setup-fixes: the package's own reason (level, block) goes to the log's DISC line, not to the screen
            expectEquals("This disc image is damaged: one of its content packages does not pass its own checks.", disc.message, "sentence");
            expectContains(disc.named, "damaged package: a hash table does not match its hash (level 0, block 0)", "the reason, for the log");
            expectEquals(Disc2Discs.NEXT_NEW_COPY, disc.next, "advice");
            expect(disc.candidates.isEmpty(), "nothing is offered from a damaged disc");

            Disc2TestKit.Package newer = pkg(2);
            newer.metadataVersion = 3;
            Disc2Discs.Disc other = Disc2Discs.open(installImage(new byte[][] {packages[0], packages[1], newer.build().data, packages[3]}).at(0), GAME);
            expectEquals(Disc2Discs.Kind.OTHER_GAME, other.kind, "a form that is not read is not called damaged (" + other.message + ")");
            expectEquals("This disc image cannot be used: one of its content packages is in a form this version does not read.", other.message,
                    "sentence");
            expectContains(other.named, "a package type or version this app does not read", "the reason, for the log");
            expectEquals(ADVICE, other.next, "the advice names both ways out: choose the right image, or make a new copy");
            expect(other.candidates.isEmpty() && !other.usable(), "nothing is offered");
        });

        check("discs: a truncated image is named as incomplete with both sizes, without a path (play disc, install disc, folder table)", () -> {
            int cutPlay = play.data.length - 2048;   // inside sfx/audio_a.rpf, the last file; the executable is intact
            Disc2Discs.Disc a = Disc2Discs.open(new SetupTestKit.MemoryDisc(XGD3, Arrays.copyOf(play.data, cutPlay)), GAME);
            expectEquals(Disc2Discs.Kind.DAMAGED, a.kind, "play kind (" + a.message + ")");
            // setup-fixes (C4-3): the size in GB, never a byte count or position; both go to the log's DISC line
            expectEquals("This disc image is incomplete: it ends before the disc's data does (it is smaller than 0.1 GB).", a.message,
                    "said plainly, no path, no byte count");
            expect(!a.message.contains("sfx/"), "no path on the player's screen");
            expectContains(a.named, "the image has " + (XGD3 + cutPlay) + " bytes", "the size is in the log");
            expectEquals(Disc2Discs.NEXT_NEW_COPY, a.next, "advice");
            expect(a.candidates.isEmpty(), "nothing is offered");

            Disc2Discs.Disc b = Disc2Discs.open(install.truncated(install.data.length - 4096), GAME);   // inside the last package
            expectEquals(Disc2Discs.Kind.DAMAGED, b.kind, "install kind (" + b.message + ")");
            expectContains(b.message, "This disc image is incomplete: it ends before the disc's data does", "said plainly");
            expect(!b.message.contains("content/"), "no path on the player's screen");

            Disc2Discs.Disc c = Disc2Discs.open(play.truncated(33 * 2048 + 10), GAME);   // inside the root folder table
            expectEquals(Disc2Discs.Kind.DAMAGED, c.kind, "table kind (" + c.message + ")");
            expectEquals(Disc2ImageProbe.TEXT_CUT_OFF, c.message, "sentence");
            expectContains(c.named, "the image is cut off", "the log's detail");
        });

        check("discs: an image of another game is named as such (another program, another game's install disc, no program)", () -> {
            Disc2Discs.Disc a = Disc2Discs.open(playImage(OTHER_EXE, COMMON).at(XGD3), GAME);
            expectEquals(Disc2Discs.Kind.OTHER_GAME, a.kind, "kind (" + a.message + ")");
            expectEquals("This is not the play disc of Synthetic release, or its copy is damaged: its game program is not the supported one.",
                    a.message, "the sentence says both, the image cannot tell which");
            expectEquals(ADVICE, a.next, "the advice names both ways out: choose the right image, or make a new copy");
            expect(a.candidates.isEmpty() && !a.usable(), "nothing is offered");

            Disc2TestKit.Package foreign = pkg(0);
            foreign.title = Disc2TestKit.TITLE + 1;
            Disc2Discs.Disc b = Disc2Discs.open(installImage("5E5E0002", new byte[][] {foreign.build().data}).at(0), GAME);
            expectEquals(Disc2Discs.Kind.OTHER_GAME, b.kind, "another game's install disc (" + b.message + ")");
            expectContains(b.ignored.toString(), "content/0000000000000000/5E5E0002/00000002/" + PKG + "0 (install content of another title, 5E5E0002)",
                    "its package is named");
            expectEquals(ADVICE, b.next, "advice");
            expect(b.candidates.isEmpty(), "nothing is offered");

            Disc2Discs.Disc c = Disc2Discs.open(new SetupTestKit.Image(dir("", file("movie.bin", AUDIO))).at(0), GAME);
            expectEquals(Disc2Discs.Kind.OTHER_GAME, c.kind, "no program (" + c.message + ")");
            expectEquals("This disc image holds no game program, so it is not a disc of Synthetic release.", c.message, "sentence");
            expectEquals(ADVICE, c.next, "advice");
        });

        check("discs: a file in the game's install-content folder that is not an install package of the game (first bytes, content type,"
                + " title, too small) refuses the disc with a sentence that says why, instead of being left out without a word", () -> {
            byte[] firstBytes = packages[3].clone();
            firstBytes[1] ^= 4;   // "PIRS" no more
            Disc2TestKit.Package foreign = pkg(3);
            foreign.title = Disc2TestKit.TITLE + 1;
            byte[][] odd = {firstBytes, artPackage(), foreign.build().data, bytes(100, 50)};
            // setup-fixes (C4-1): no number or hex value on the screen; the log's DISC line keeps them
            String[] why = {"it does not begin as a content package does", "it is a package of another kind, not install content",
                    "it belongs to another game", "it is too small to be a content package"};
            String[] logged = {null, "its header names content type 196608, not install content", "its header names another title, 5E5E0002", null};
            for (int i = 0; i < odd.length; ++i) {
                Disc2Discs.Disc disc = Disc2Discs.open(installImage(new byte[][] {packages[0], packages[1], packages[2], odd[i]}).at(0), GAME);
                expectEquals(Disc2Discs.Kind.OTHER_GAME, disc.kind, "kind, case " + i + " (" + disc.message + ")");
                expectEquals("This disc image cannot be used: a file in the game's install-content folder is not an install"
                        + " package of the game (" + why[i] + ").", disc.message, "sentence, case " + i);
                if (logged[i] != null) expectContains(disc.named, logged[i], "log, case " + i);
                expectEquals(ADVICE, disc.next, "advice, case " + i);
                expect(disc.candidates.isEmpty() && !disc.usable(), "nothing is offered, case " + i);
            }
        });

        check("discs: an image cut off inside a sub-folder's list is named as incomplete with its size; a folder list that contradicts itself"
                + " is named as damaged; both get the advice to copy again", () -> {
            // the list of folder sfx lies behind the other files' data, as a folder list lies near the end of both real discs
            int record = play.records.get("sfx").intValue();
            long list = (play.data[record + 4] & 0xFFL | (play.data[record + 5] & 0xFFL) << 8 | (play.data[record + 6] & 0xFFL) << 16
                    | (play.data[record + 7] & 0xFFL) << 24) * 2048;   // the first sector of that list, from the folder's record
            int cut = (int) list + 10;
            Disc2Discs.Disc a = Disc2Discs.open(new SetupTestKit.MemoryDisc(XGD3, Arrays.copyOf(play.data, cut)), GAME);
            expectEquals(Disc2Discs.Kind.DAMAGED, a.kind, "cut-off kind (" + a.message + ")");
            expectEquals("This disc image is incomplete: it ends before the disc's data does (it is smaller than 0.1 GB).", a.message, "sentence");
            expectEquals("image bytes=" + (XGD3 + cut), a.named, "the byte count, for the log");
            expectEquals(Disc2Discs.NEXT_NEW_COPY, a.next, "advice");
            expect(a.candidates.isEmpty() && !a.usable(), "nothing is offered");

            SetupTestKit.Image loop = new SetupTestKit.Image(dir("", file("default.xex", EXE), dir("sub", SetupTestKit.Node.link("again", ""))));
            Disc2Discs.Disc b = Disc2Discs.open(loop.at(XGD3), GAME);
            expectEquals(Disc2Discs.Kind.DAMAGED, b.kind, "contradiction kind (" + b.message + ")");
            expectEquals("This disc image is damaged (a folder contains itself).", b.message, "sentence");
            expectEquals(Disc2Discs.NEXT_NEW_COPY, b.next, "advice");
            expect(b.candidates.isEmpty() && !b.usable(), "nothing is offered");
        });

        check("plan: a pinned entry or a game whose SHA-256 is not 64 lower-case hex digits, or a negative size, is refused when it is made", () -> {
            String good = sha256(EXE);
            String[] bad = {good.toUpperCase(java.util.Locale.ROOT), good.substring(1), good + "0", good.substring(0, 63) + "g", "", null};
            for (String text : bad) {
                try {
                    new Disc2Discs.Pin(text, 14);
                    throw new AssertionError("a pinned entry with the SHA-256 <" + text + "> was accepted: it could never match a file");
                } catch (IllegalArgumentException expected) {
                    expectContains(expected.getMessage(), "a pinned entry needs a SHA-256 value and a size", "reason");
                }
                try {
                    new Disc2Discs.Game("Synthetic release", "default.xex", text, Disc2TestKit.TITLE);
                    throw new AssertionError("a game with the SHA-256 <" + text + "> was accepted: no play disc could ever match it");
                } catch (IllegalArgumentException expected) {
                    expectContains(expected.getMessage(), "a game needs a label, an executable name and its SHA-256", "reason");
                }
            }
            try {
                new Disc2Discs.Pin(good, -1);
                throw new AssertionError("a negative size was accepted");
            } catch (IllegalArgumentException expected) {
                expectContains(expected.getMessage(), "a pinned entry needs", "reason");
            }
            expectEquals(good, new Disc2Discs.Pin(good, 0).sha256, "an empty file can be pinned");
        });

        check("discs: a file that is not a disc image and a source that fails to read get their own kind and advice", () -> {
            Disc2Discs.Disc zeros = Disc2Discs.open(new SetupTestKit.MemoryDisc(0, new byte[100 * 2048]), GAME);
            expectEquals(Disc2Discs.Kind.NOT_A_DISC, zeros.kind, "zeros");
            expectEquals(Disc2Discs.NEXT_CHOOSE_IMAGE, zeros.next, "advice");
            Disc2Discs.Disc lone = Disc2Discs.open(new SetupTestKit.MemoryDisc(0, Arrays.copyOf(packages[0], 100 * 2048)), GAME);
            expectEquals(Disc2Discs.Kind.NOT_A_DISC, lone.kind, "a package picked instead of an image");
            expectEquals("This file is not a disc image of Synthetic release: it is an Xbox 360 content package (a download, an add-on or a save).",
                    lone.message, "sentence");

            // the read fails inside the first package's hash tables: the image is not damaged, the storage is unreadable
            int record = install.records.get("content/0000000000000000/5E5E0001/00000002/" + PKG + "0").intValue();
            long start = (install.data[record + 4] & 0xFFL | (install.data[record + 5] & 0xFFL) << 8 | (install.data[record + 6] & 0xFFL) << 16
                    | (install.data[record + 7] & 0xFFL) << 24) * 2048;   // the package's first sector, from its folder record
            Disc2Discs.Disc failing = Disc2Discs.open(failingBetween(install.at(0), start + Disc2TestKit.BASE, start + Disc2TestKit.BASE + 4096), GAME);
            expectEquals(Disc2Discs.Kind.UNREADABLE, failing.kind, "kind (" + failing.message + ")");
            expectContains(failing.message, "could not be read (I/O error)", "sentence");
            expectEquals(Disc2Discs.NEXT_RECONNECT, failing.next, "advice");
        });

        check("sha256: reads exactly the candidate's bytes, writes them when asked, stops when told, refuses a shorter or longer stream", () -> {
            final byte[] data = bytes(3 * (1 << 20) + 17, 48);   // several buffers and a rest
            Disc2Discs.Candidate exact = new Disc2Discs.Candidate("exact", data.length, false, () -> new ByteArrayInputStream(data));
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            final List<Long> seen = new ArrayList<Long>();
            expectEquals(sha256(data), Disc2Discs.sha256(exact, out, done -> seen.add(Long.valueOf(done))), "hash");
            expect(Arrays.equals(data, out.toByteArray()), "written bytes");
            expectEquals(Long.valueOf(data.length), seen.get(seen.size() - 1), "the watch saw the end");
            ByteArrayOutputStream partial = new ByteArrayOutputStream();
            try {
                Disc2Discs.sha256(exact, partial, done -> false);
                throw new AssertionError("not stopped");
            } catch (Disc2Discs.Stopped e) {
                expect(partial.size() > 0 && partial.size() < data.length, "stopped after the first part (" + partial.size() + " bytes)");
            }
            Disc2Discs.Candidate shorter = new Disc2Discs.Candidate("short", data.length + 1, false, () -> new ByteArrayInputStream(data));
            try {
                Disc2Discs.sha256(shorter, null, null);
                throw new AssertionError("a short stream was accepted");
            } catch (EOFException e) {
                expectContains(e.getMessage(), "ends after " + data.length + " of " + (data.length + 1) + " bytes", "short");
            }
            Disc2Discs.Candidate longer = new Disc2Discs.Candidate("long", data.length - 1, false, () -> new ByteArrayInputStream(data));
            try {
                Disc2Discs.sha256(longer, null, null);
                throw new AssertionError("a long stream was accepted");
            } catch (IOException e) {
                expectContains(e.getMessage(), "longer than " + (data.length - 1) + " bytes", "long");
            }
        });

        check("plan: with nothing installed every candidate of a pinned size is read and no other; the bytes add up", () -> {
            Disc2Discs.Disc p = Disc2Discs.open(play.at(XGD3), GAME);
            Disc2Discs.Plan plan = Disc2Discs.plan(p.candidates, pins(), new HashSet<String>());
            expectEquals(Arrays.asList("common/data/installpartitions.meta", "common.rpf", "default.xex", "sfx/audio_a.rpf"), names(plan.read),
                    "the art package has no pinned size and is not read");
            expectEquals((long) (META.length + COMMON.length + EXE.length + AUDIO.length), plan.bytes, "bytes");
            expectEquals(12, plan.wanted.size(), "every pinned entry is wanted");
            expectEquals(8, plan.noCandidate.size(), "the install disc's entries cannot come from the play disc");
            for (Disc2Discs.Candidate c : plan.read) {
                expectEquals(Disc2Discs.Verdict.WANTED, plan.judge(c.bytes, Disc2Discs.sha256(c, null, null)), c.name);
            }
            Disc2Discs.Disc i = Disc2Discs.open(install.at(0), GAME);
            Disc2Discs.Plan second = Disc2Discs.plan(i.candidates, pins(), new HashSet<String>());
            expectEquals(names(i.candidates), names(second.read), "all eight package files");
            expectEquals((long) (PART0.length + PART1.length + PART2.length + PART3.length + 4 * 14), second.bytes, "bytes");
            expectEquals(4, second.noCandidate.size(), "the play disc's entries cannot come from the install disc's packages");
        });

        check("plan: a file of a pinned size whose content is not pinned is read and then judged NOT_PINNED", () -> {
            Disc2Discs.Disc p = Disc2Discs.open(playImage(EXE, COMMON_OTHER).at(XGD3), GAME);
            Disc2Discs.Plan plan = Disc2Discs.plan(p.candidates, pins(), new HashSet<String>());
            Disc2Discs.Candidate common = named(plan.read, "common.rpf");
            String sha = Disc2Discs.sha256(common, null, null);
            expectEquals(sha256(COMMON_OTHER), sha, "what was read");
            expectEquals(Disc2Discs.Verdict.NOT_PINNED, plan.judge(common.bytes, sha), "verdict");
            expect(plan.pin(common.bytes, sha) == null, "no pinned entry");
            expectEquals(Disc2Discs.Verdict.NOT_PINNED, plan.judge(common.bytes + 1, sha256(COMMON)), "a pinned hash under another size is not that entry");
        });

        check("plan: four files of one size, three installed -> all four are read (56 bytes), one is WANTED; installed big files are not read", () -> {
            Disc2Discs.Disc i = Disc2Discs.open(install.at(0), GAME);
            HashSet<String> installed = new HashSet<String>(Arrays.asList(sha256(STAMP0), sha256(STAMP1), sha256(STAMP3), sha256(PART0),
                    sha256(PART1), sha256(PART2), sha256(EXE), sha256(COMMON), sha256(META), sha256(AUDIO)));
            Disc2Discs.Plan plan = Disc2Discs.plan(i.candidates, pins(), installed);
            expectEquals(Arrays.asList(PKG + "0/part0.timestamp", PKG + "1/part1.timestamp", PKG + "2/part2.timestamp", PKG + "3/part3.rpf",
                    PKG + "3/part3.timestamp"), names(plan.read), "files to read");
            expectEquals((long) (4 * 14 + PART3.length), plan.bytes, "bytes");
            List<String> verdicts = new ArrayList<String>();
            for (Disc2Discs.Candidate c : plan.read) verdicts.add(plan.judge(c.bytes, Disc2Discs.sha256(c, null, null)).name());
            expectEquals(Arrays.asList("ALREADY_INSTALLED", "ALREADY_INSTALLED", "WANTED", "WANTED", "ALREADY_INSTALLED"), verdicts, "verdicts");
            expect(plan.noCandidate.isEmpty(), "both missing entries have a candidate of their size");
        });

        check("plan: installed() takes only a regular file of the pinned size under its final name; with all in place nothing is read", () -> {
            File inputs = new File(SetupTestKit.newFilesDir("discs-installed"), "inputs");
            List<Disc2Discs.Pin> pins = new ArrayList<Disc2Discs.Pin>(pins());
            pins.add(pin(PART0));   // the same entry listed twice counts once
            SetupTestKit.write(new File(inputs, sha256(PART1) + ".bin"), PART1);
            SetupTestKit.write(new File(inputs, sha256(STAMP1) + ".bin"), STAMP1);
            SetupTestKit.write(new File(inputs, sha256(PART2) + ".bin"), Arrays.copyOf(PART2, PART2.length - 1));   // cut short
            expect(new File(inputs, sha256(STAMP2) + ".bin").mkdir(), "a folder under a final name");
            SetupTestKit.write(new File(inputs, sha256(PART3) + ".part"), PART3);   // not a final name
            expectEquals(new HashSet<String>(Arrays.asList(sha256(PART1), sha256(STAMP1))), Disc2Discs.installed(inputs, pins), "installed");
            Disc2Discs.Disc i = Disc2Discs.open(install.at(0), GAME);
            Disc2Discs.Plan some = Disc2Discs.plan(i.candidates, pins, Disc2Discs.installed(inputs, pins));
            expectEquals(10, some.wanted.size(), "12 distinct entries, two installed");
            expect(!names(some.read).contains(PKG + "1/part1.rpf"), "the installed archive is not read again");
            HashSet<String> all = new HashSet<String>();
            for (Disc2Discs.Pin p : pins) all.add(p.sha256);
            Disc2Discs.Plan none = Disc2Discs.plan(i.candidates, pins, all);
            expectEquals(new ArrayList<String>(), names(none.read), "nothing to read");
            expectEquals(0L, none.bytes, "no bytes");
        });

        check("plan: an install disc that lacks one of its four packages is short by that package's two files before anything is read;"
                + " a complete disc is short by nothing; noCandidate, which goes by size alone, does not show the 14-byte file", () -> {
            Disc2Discs.Disc three = Disc2Discs.open(installImage(new byte[][] {packages[0], packages[1], packages[2]}).at(0), GAME);
            expectEquals(Disc2Discs.Kind.INSTALL, three.kind, "a disc without its fourth package is still recognised (" + three.message + ")");
            Disc2Discs.Plan plan = Disc2Discs.plan(three.candidates, installPins(), new HashSet<String>());
            expectEquals(Arrays.asList(Long.valueOf(PART3.length), Long.valueOf(14)), plan.shortSizes, "short by the archive and one 14-byte file");
            expectEquals(1, plan.noCandidate.size(), "noCandidate names the archive only: three other files have 14 bytes");
            expectEquals(sha256(PART3), plan.noCandidate.get(0).sha256, "the archive");
            Disc2Discs.Disc whole = Disc2Discs.open(install.at(0), GAME);
            expectEquals(new ArrayList<Long>(), Disc2Discs.plan(whole.candidates, installPins(), new HashSet<String>()).shortSizes, "the whole disc");
            // one 14-byte entry wanted and three candidates of 14 bytes: by sizes the disc may hold it, so it is not short
            HashSet<String> allButStamp3 = new HashSet<String>();
            for (Disc2Discs.Pin p : installPins()) allButStamp3.add(p.sha256);
            allButStamp3.remove(sha256(STAMP3));
            expectEquals(new ArrayList<Long>(), Disc2Discs.plan(three.candidates, installPins(), allButStamp3).shortSizes, "one wanted, three candidates");
            // given the other disc's entries as well, those count as short: the caller passes the entries of the disc in hand
            expectEquals(4, Disc2Discs.plan(whole.candidates, pins(), new HashSet<String>()).shortSizes.size(), "the play disc's four entries");
        });

        check("plan: missing() names the pinned entries without a file in place; wrongContent() reads the files that are in place and finds"
                + " one whose content is not what its name says, which installed() takes on trust", () -> {
            File inputs = new File(SetupTestKit.newFilesDir("discs-missing"), "inputs");
            List<Disc2Discs.Pin> pins = new ArrayList<Disc2Discs.Pin>(installPins());
            pins.add(pin(PART0));   // the same entry listed twice counts once
            SetupTestKit.write(new File(inputs, sha256(PART0) + ".bin"), PART0);
            SetupTestKit.write(new File(inputs, sha256(STAMP0) + ".bin"), STAMP0);
            byte[] changed = PART1.clone();
            changed[PART1.length - 1] ^= 1;   // the pinned size, another content
            SetupTestKit.write(new File(inputs, sha256(PART1) + ".bin"), changed);
            SetupTestKit.write(new File(inputs, sha256(PART2) + ".bin"), Arrays.copyOf(PART2, PART2.length - 1));   // cut short: not in place
            List<String> missing = new ArrayList<String>();
            for (Disc2Discs.Pin p : Disc2Discs.missing(inputs, pins)) missing.add(p.sha256);
            expectEquals(Arrays.asList(sha256(STAMP1), sha256(PART2), sha256(STAMP2), sha256(PART3), sha256(STAMP3)), missing, "missing, in the order given");
            expect(Disc2Discs.installed(inputs, pins).contains(sha256(PART1)), "installed() goes by name and size");
            List<Disc2Discs.Pin> wrong = Disc2Discs.wrongContent(inputs, pins, null);
            expectEquals(1, wrong.size(), "one file has another content than its name says");
            expectEquals(sha256(PART1), wrong.get(0).sha256, "the changed file");
            try {
                Disc2Discs.wrongContent(inputs, pins, done -> false);
                throw new AssertionError("the content check went on after it was told to stop");
            } catch (Disc2Discs.Stopped expected) {
                expect(true, "stopped");
            }
            expectEquals(new ArrayList<Disc2Discs.Pin>(), Disc2Discs.missing(inputs, new ArrayList<Disc2Discs.Pin>(Arrays.asList(pin(PART0), pin(STAMP0)))),
                    "nothing is missing of the two that are in place");
        });

        check("loop: the copy loop printed in INTEGRATION.md installs both discs, removes what a killed run left in staging, leaves no part"
                + " file after a stop, and never gives a final name to a file that is not pinned", () -> {
            File root = new File(SetupTestKit.newFilesDir("discs-loop"), "private-trace");
            File inputs = new File(root, "inputs"), staging = new File(root, "staging");
            SetupTestKit.write(new File(staging, "disc-file.part"), bytes(5000, 60));   // a killed run's part file
            SetupTestKit.write(new File(staging, "older.part"), bytes(10, 61));
            try {
                copyDisc(install.at(0), GAME, installPins(), root, done -> done != 14);   // stop when the second file, of 14 bytes, was read
                throw new AssertionError("the copy was not stopped");
            } catch (Disc2Discs.Stopped expected) {
                expectEquals(new ArrayList<String>(), filesIn(staging), "staging after a stop (the killed run's files are gone as well)");
            }
            expectEquals(Arrays.asList(sha256(PART0) + ".bin"), filesIn(inputs), "the file finished before the stop is in place, no other");
            expectEquals(null, copyDisc(install.at(0), GAME, installPins(), root, null), "the second run of the install disc");
            // a play disc whose common.rpf has the pinned size and another content: three files are installed, that one is not
            String said = copyDisc(playImage(EXE, COMMON_OTHER).at(XGD3), GAME, playPins(), root, null);
            expectEquals("This disc image did not provide 1 of the files this disc should hold. " + Disc2Discs.NEXT_NEW_COPY, said, "what the player is told");
            expect(!filesIn(inputs).contains(sha256(COMMON_OTHER) + ".bin"), "the file that is not pinned got no final name");
            expectEquals(new ArrayList<String>(), filesIn(staging), "staging after a file that was not wanted");
            expectEquals(null, copyDisc(play.at(XGD3), GAME, playPins(), root, null), "the right play disc");
            expectEquals(finalNames(pins()), filesIn(inputs), "exactly the twelve pinned files");
            for (Disc2Discs.Pin p : pins()) expectEquals(p.sha256, sha256(SetupTestKit.read(new File(inputs, p.sha256 + ".bin"))), "content of " + p.sha256);
            expectEquals(new ArrayList<Disc2Discs.Pin>(), Disc2Discs.wrongContent(inputs, pins(), null), "the slow check finds nothing wrong");

            // an install disc without its fourth package is turned down before a byte is copied
            File fresh = new File(SetupTestKit.newFilesDir("discs-loop-short"), "private-trace");
            SetupTestKit.MemoryDisc three = installImage(new byte[][] {packages[0], packages[1], packages[2]}).at(0);
            String lacks = copyDisc(three, GAME, installPins(), fresh, null);
            expectEquals("This disc image lacks 2 of the files this disc should hold. " + Disc2Discs.NEXT_NEW_COPY, lacks, "what the player is told");
            expectEquals(new ArrayList<String>(), filesIn(new File(fresh, "inputs")), "nothing was copied from the short disc");
        });

        SetupTestKit.finish("Disc2DiscsTest");
    }
}
