package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.Node.dir;
import static dev.recompbootstrap.smoke.SetupTestKit.Node.file;
import static dev.recompbootstrap.smoke.SetupTestKit.bytes;
import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectContains;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;
import static dev.recompbootstrap.smoke.SetupTestKit.input;
import static dev.recompbootstrap.smoke.SetupTestKit.item;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.TreeMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Run with java -ea. Synthetic files only; every run works in its own temporary files/ directory. */
public final class VerifiedExtractorTest {
    // sizes chosen around the 1 MiB copy buffer: smaller, exactly one, several and a part
    private static final byte[] EXECUTABLE = bytes(70000, 21), SMALL = bytes(1028, 22), ONE = bytes(1 << 20, 23),
            BIG = bytes((3 << 20) + 12345, 24), EMPTY = new byte[0];
    private static final SetupCatalog.Item I_EXE = item("default.xex", EXECUTABLE), I_SMALL = item("common/data/small.meta", SMALL),
            I_ONE = item("one.rpf", ONE), I_BIG = item("sfx/big.rpf", BIG), I_EMPTY = item("empty.bin", EMPTY);
    private static final List<SetupCatalog.Item> ITEMS = Arrays.asList(I_EXE, I_SMALL, I_ONE, I_BIG, I_EMPTY);
    private static final SetupCatalog.Spec SPEC = SetupTestKit.spec(EXECUTABLE, ITEMS, true);

    private static SetupTestKit.MemoryFolder folder() {
        return new SetupTestKit.MemoryFolder().put("default.xex", EXECUTABLE).put("common/data/small.meta", SMALL)
                .put("one.rpf", ONE).put("sfx/big.rpf", BIG).put("empty.bin", EMPTY);
    }

    private static SetupTestKit.Image image() {
        return new SetupTestKit.Image(dir("", file("default.xex", EXECUTABLE), file("one.rpf", ONE), file("empty.bin", EMPTY),
                dir("common", dir("data", file("small.meta", SMALL))), dir("sfx", file("big.rpf", BIG))));
    }

    private static File root(File files) {
        return new File(files, "game-install");
    }

    /** The bytes of the catalog's file number i. */
    private static byte[] content(int i) {
        return new byte[][] {EXECUTABLE, SMALL, ONE, BIG, EMPTY}[i];
    }

    private static void expectAllInPlace(File root) throws IOException {
        for (SetupCatalog.Item it : ITEMS) {
            File f = input(root, it);
            expect(f.isFile(), it.path + " is in place");
            expectEquals(it.sha256, SetupTestKit.sha256(SetupTestKit.read(f)), "bytes of " + it.path);
            expectEquals("r--------", SetupTestKit.mode(f), "mode of " + it.path);
        }
        String[] parts = new File(root, "staging").list();
        expectEquals(0, parts == null ? -1 : parts.length, "nothing left in staging");
    }

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();

        check("extract: from a disc image, every file arrives byte for byte under its hash name", () -> {
            File files = SetupTestKit.newFilesDir("extract-image");
            SetupTestKit.Recorder progress = new SetupTestKit.Recorder();
            SetupTestKit.MemoryDisc disc = image().at(0x02080000L);
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.imageReader(disc), root(files), progress,
                    SPEC, SetupTestKit.PLENTY);
            expect(o.complete && !o.cancelled && o.error == null, "complete: " + o.error);
            expectAllInPlace(root(files));
            long total = EXECUTABLE.length + SMALL.length + ONE.length + BIG.length;
            expectEquals(total, progress.bytesDone, "progress ends at the total");
            expectEquals(total, progress.bytesTotal, "total");
            expectEquals(5, progress.filesDone, "files done");
            expect(!progress.decreasing, "progress never goes back");
            expectEquals("{\"version\":1,\"executableSha256\":\"" + SPEC.executableSha256
                    + "\",\"discFilesComplete\":true,\"bundleComplete\":false,\"complete\":false}\n",
                    new String(SetupTestKit.read(new File(root(files), "install.json")), "UTF-8"), "install.json");
        });

        check("extract: from an extracted folder, with files found without regard to case", () -> {
            File files = SetupTestKit.newFilesDir("extract-folder");
            SetupTestKit.MemoryFolder folder = new SetupTestKit.MemoryFolder().put("DEFAULT.XEX", EXECUTABLE)
                    .put("Common/Data/small.meta", SMALL).put("one.rpf", ONE).put("sfx/big.rpf", BIG).put("empty.bin", EMPTY);
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(o.complete, "complete: " + o.error);
            expectAllInPlace(root(files));
            for (SetupTestKit.MemoryDisc source : folder.sources) expect(source.closed, "every opened file is closed");
        });

        check("extract: a file with the wrong content is rejected, named, and never gets its final name", () -> {
            File files = SetupTestKit.newFilesDir("extract-wrong-hash");
            byte[] changed = BIG.clone();
            changed[changed.length - 1] ^= 1;                                   // same size, one bit differs
            SetupTestKit.MemoryFolder folder = folder().put("sfx/big.rpf", changed);
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(!o.complete && !o.cancelled, "not complete");
            expectContains(o.error, "sfx/big.rpf in your disc copy is not the file of the supported disc", "the error names the file");
            expectContains(o.error, "check code " + SetupTestKit.sha256(changed).substring(0, 12) + ")", "and the first 12 digits of the hash that was found");
            expectEquals(VerifiedExtractor.Problem.DISC_COPY_BAD, o.problem, "the kind: a new disc copy is needed");
            expect(!input(root(files), I_BIG).exists(), "no final file");
            expectEquals(0, new File(root(files), "staging").list().length, "no partial file is kept");
            expect(input(root(files), I_ONE).isFile(), "files verified before it stay");
            expect(!new File(root(files), "install.json").exists(), "no record of an install");
        });

        check("extract: a source that ends early (cut image, short file) is an incomplete disc copy, with where it ends", () -> {
            File files = SetupTestKit.newFilesDir("extract-truncated");
            SetupTestKit.Image image = image();
            int record = image.records.get("sfx/big.rpf");
            int sector = (image.data[record + 4] & 0xFF) | (image.data[record + 5] & 0xFF) << 8;
            SetupTestKit.MemoryDisc cut = image.truncated(sector * 2048 + (2 << 20));    // 2 MiB of the 3 MiB file are there
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.imageReader(cut), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(!o.complete, "not complete");
            expectContains(o.error, "sfx/big.rpf is incomplete in your disc copy: the disc image ends inside it", "cut image");
            expectEquals(VerifiedExtractor.Problem.DISC_COPY_BAD, o.problem, "cut image: a new disc copy is needed, trying again does not help");
            expect(!input(root(files), I_BIG).exists(), "no final file");
            expectEquals(0, new File(root(files), "staging").list().length, "no partial file is kept");

            // an image whose table of the sfx folder is wrong: the disc check reads the top folder only, so the copy meets it
            File files3 = SetupTestKit.newFilesDir("extract-damaged-table");
            byte[] damaged = image.data.clone();
            damaged[record + 13] = 0;                                           // the folder's one entry has a name of length 0
            VerifiedExtractor.Outcome o3 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.imageReader(new SetupTestKit.MemoryDisc(0, damaged)),
                    root(files3), new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectEquals("sfx/big.rpf cannot be found in your disc copy: the image is damaged (bad name in a folder table).", o3.error, "damaged folder table");
            expectEquals(VerifiedExtractor.Problem.DISC_COPY_BAD, o3.problem, "damaged folder table: a new disc copy is needed");
            expect(input(root(files3), I_ONE).isFile(), "the files before it are kept");

            File files2 = SetupTestKit.newFilesDir("extract-short");
            SetupTestKit.MemoryFolder folder = folder().put("one.rpf", Arrays.copyOf(ONE, ONE.length - 1));
            VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files2),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectContains(o2.error, "one.rpf is incomplete in your disc copy: it ends after " + (ONE.length - 1) + " of " + ONE.length + " bytes",
                    "short file");
            expectEquals(VerifiedExtractor.Problem.DISC_COPY_BAD, o2.problem, "short file: the same kind");
            expect(!input(root(files2), I_ONE).exists(), "no final file");
            expectEquals(0, new File(root(files2), "staging").list().length, "no partial file is kept");
        });

        check("extract: a file that is longer than the catalog says, or missing, is a bad disc copy", () -> {
            File files = SetupTestKit.newFilesDir("extract-long");
            byte[] longer = Arrays.copyOf(SMALL, SMALL.length + 1);
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS,
                    VerifiedExtractor.folderReader(folder().put("common/data/small.meta", longer)), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectContains(o.error, "common/data/small.meta in your disc copy is not the file of the supported disc (it is longer than "
                    + SMALL.length + " bytes)", "longer");
            expectEquals(VerifiedExtractor.Problem.DISC_COPY_BAD, o.problem, "longer: the kind");
            expect(!input(root(files), I_SMALL).exists(), "no final file");
            SetupTestKit.MemoryFolder without = folder();
            without.files.remove("sfx/big.rpf");
            VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(without), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectEquals("sfx/big.rpf is missing from the disc copy you chose.", o2.error, "missing");
            expectEquals(VerifiedExtractor.Problem.DISC_COPY_BAD, o2.problem, "missing: the kind");
        });

        check("extract: a read that fails (storage removed) is told apart from a bad disc copy, and leaves no partial file", () -> {
            File files = SetupTestKit.newFilesDir("extract-read-fails");
            final VerifiedExtractor.ItemReader good = VerifiedExtractor.folderReader(folder());
            VerifiedExtractor.ItemReader failing = new VerifiedExtractor.ItemReader() {
                @Override
                public InputStream open(final SetupCatalog.Item it) throws IOException {
                    final InputStream in = good.open(it);
                    if (!it.path.equals("sfx/big.rpf")) return in;
                    return new InputStream() {   // one block, then the storage is gone
                        private int blocks;
                        @Override public int read() throws IOException { throw new IOException("card removed"); }
                        @Override public int read(byte[] b, int off, int len) throws IOException {
                            if (blocks++ > 0) throw new IOException("card removed");
                            return in.read(b, off, len);
                        }
                    };
                }
            };
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, failing, root(files), new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectEquals("sfx/big.rpf could not be read from your disc copy (card removed).", o.error, "the sentence");
            expectEquals(VerifiedExtractor.Problem.SOURCE_UNREADABLE, o.problem, "the kind: it may work when tried again");
            expect(!input(root(files), I_BIG).exists() && input(root(files), I_ONE).isFile(), "the file has no final name; the ones before it stay");
            expectEquals(0, new File(root(files), "staging").list().length, "no partial file is kept");
            VerifiedExtractor.ItemReader absent = new VerifiedExtractor.ItemReader() {
                @Override public InputStream open(SetupCatalog.Item it) throws IOException { throw new IOException("permission gone"); }
            };
            VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(ITEMS, absent, root(files), new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectEquals("sfx/big.rpf could not be read from your disc copy (permission gone).", o2.error, "a file that cannot be opened");
            expectEquals(VerifiedExtractor.Problem.SOURCE_UNREADABLE, o2.problem, "the same kind");
        });

        check("extract: resume after an interruption gives identical bytes, skips finished files, restarts the partial one", () -> {
            File files = SetupTestKit.newFilesDir("extract-resume");
            // run 1: stopped in the middle of the big file
            SetupTestKit.Recorder first = new SetupTestKit.Recorder();
            first.cancelAtBytes = EXECUTABLE.length + SMALL.length + ONE.length + (1 << 20);
            VerifiedExtractor.Outcome o1 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files), first,
                    SPEC, SetupTestKit.PLENTY);
            expect(o1.cancelled && !o1.complete && o1.error == null, "run 1 was stopped");
            expect(input(root(files), I_ONE).isFile(), "a finished file stays");
            expect(!input(root(files), I_BIG).exists(), "the interrupted file has no final name");
            // a process that was killed leaves its partial file behind; here: longer than the real file and wrong
            File part = new File(root(files), "staging/" + I_BIG.sha256 + ".part");
            SetupTestKit.write(part, bytes(BIG.length + 5000, 99));
            Object finishedKey = Files.readAttributes(input(root(files), I_ONE).toPath(), java.nio.file.attribute.BasicFileAttributes.class).fileKey();
            long finishedTime = input(root(files), I_ONE).lastModified();
            // run 2
            SetupTestKit.MemoryFolder folder = folder();
            SetupTestKit.Recorder second = new SetupTestKit.Recorder();
            VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files), second,
                    SPEC, SetupTestKit.PLENTY);
            expect(o2.complete, "run 2 completes: " + o2.error);
            expectAllInPlace(root(files));
            expectEquals(Arrays.asList("sfx/big.rpf", "empty.bin"), folder.opened, "only the unfinished files are read again");
            expectEquals(finishedKey, Files.readAttributes(input(root(files), I_ONE).toPath(),
                    java.nio.file.attribute.BasicFileAttributes.class).fileKey(), "the finished file is the same file");
            expectEquals(finishedTime, input(root(files), I_ONE).lastModified(), "and was not written again");
            expect(!second.decreasing && second.bytesDone == second.bytesTotal, "progress of run 2 ends at the total");
        });

        check("extract: nothing is written when an install already exists (developer data, developer code, complete record)", () -> {
            for (int variant = 0; variant < 3; ++variant) {
                File files = SetupTestKit.newFilesDir("extract-existing-" + variant);
                if (variant == 0) SetupTestKit.write(new File(files, "private-trace/inputs/abc.bin"), bytes(50, 31));
                if (variant == 1) SetupTestKit.write(new File(files, "private-trace/private-trace-live.so"), bytes(80, 32));
                if (variant == 2) {
                    SetupTestKit.write(new File(files, "game-install/install.json"),
                            new InstallGuard.Record(SPEC.executableSha256, true, true).encode());
                    SetupTestKit.write(new File(files, "game-install/inputs/" + I_ONE.sha256 + ".bin"), bytes(10, 33));
                }
                SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 34));
                TreeMap<String, String> before = SetupTestKit.snapshot(files);
                SetupTestKit.MemoryFolder folder = folder();
                VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files),
                        new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
                expect(!o.complete && !o.cancelled, "refused (variant " + variant + ")");
                expectContains(o.error, "An install already exists and was not changed", "reason (variant " + variant + ")");
                expectEquals(VerifiedExtractor.Problem.EXISTING_INSTALL, o.problem, "kind (variant " + variant + ")");
                expectEquals(before, SetupTestKit.snapshot(files), "files/ is untouched (variant " + variant + ")");
                expect(folder.opened.isEmpty(), "the disc is not even read (variant " + variant + ")");
            }
        });

        check("extract: a file in place with the catalog's size is kept as it is and not read again", () -> {
            File files = SetupTestKit.newFilesDir("extract-keep");
            byte[] foreign = bytes(SMALL.length, 41);                           // the catalog's size, other content
            SetupTestKit.write(input(root(files), I_SMALL), foreign);
            SetupTestKit.MemoryFolder folder = folder();
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(o.complete, "complete: " + o.error);
            expect(Arrays.equals(foreign, SetupTestKit.read(input(root(files), I_SMALL))), "the existing file kept its bytes");
            expect(!folder.opened.contains("common/data/small.meta"), "and its source was not read");
        });

        check("extract: a file in place that changed size since it was copied is replaced, and only by a verified copy", () -> {
            // (a) the disc copy is good: the damaged file gives way to the verified one, in the rename that names it
            File files = SetupTestKit.newFilesDir("extract-resized");
            expect(VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files), new SetupTestKit.Recorder(),
                    SPEC, SetupTestKit.PLENTY).complete, "first run");
            File one = input(root(files), I_ONE);
            expect(one.setWritable(true, true), "make it writable for the test");
            SetupTestKit.write(one, Arrays.copyOf(ONE, ONE.length - 4096));     // cut short behind the app's back
            SetupTestKit.MemoryFolder folder = folder();
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(o.complete, "the second run completes: " + o.error);
            expectEquals(Arrays.asList("one.rpf"), folder.opened, "only the damaged file is read again");
            expectAllInPlace(root(files));

            // (b) the disc copy is bad at that very file: the file in place is exactly as it was
            for (int variant = 0; variant < 2; ++variant) {
                File files2 = SetupTestKit.newFilesDir("extract-resized-bad-" + variant);
                byte[] damaged = bytes(BIG.length + 77, 42);
                SetupTestKit.write(input(root(files2), I_BIG), damaged);
                TreeMap<String, String> before = SetupTestKit.snapshot(new File(root(files2), "inputs"));
                byte[] changed = BIG.clone();
                changed[100] ^= 1;
                SetupTestKit.Recorder progress = new SetupTestKit.Recorder();
                if (variant == 1) progress.cancelAtBytes = 1;                   // stopped after the first block of the new copy
                VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(Arrays.asList(I_BIG),
                        VerifiedExtractor.folderReader(variant == 0 ? folder().put("sfx/big.rpf", changed) : folder()), root(files2), progress,
                        SPEC, SetupTestKit.PLENTY);
                expect(!o2.complete && (variant == 0 ? o2.problem == VerifiedExtractor.Problem.DISC_COPY_BAD : o2.cancelled),
                        "not replaced (variant " + variant + "): " + o2.error);
                expectEquals(before, SetupTestKit.snapshot(new File(root(files2), "inputs")), "the file in place is untouched (variant " + variant + ")");
                expectEquals(0, new File(root(files2), "staging").list().length, "no partial file is kept (variant " + variant + ")");
            }

            // (c) a folder under the final name is not a file to replace: said before the disc is read, nothing changed
            File files3 = SetupTestKit.newFilesDir("extract-folder-in-place");
            expect(new File(root(files3), "inputs/" + I_ONE.sha256 + ".bin").mkdirs(), "mkdir");
            SetupTestKit.MemoryFolder folder3 = folder();
            VerifiedExtractor.Outcome o3 = VerifiedExtractor.extract(Arrays.asList(I_ONE), VerifiedExtractor.folderReader(folder3), root(files3),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectEquals("Could not put one.rpf in place: a folder stands under its name.", o3.error, "the reason");
            expectEquals(VerifiedExtractor.Problem.STORAGE, o3.problem, "the kind");
            expect(folder3.opened.isEmpty(), "the disc was not read for it");
        });

        check("extract: too little free space stops before anything is written; one byte more is enough", () -> {
            long need = EXECUTABLE.length + SMALL.length + ONE.length + BIG.length + VerifiedExtractor.RESERVE_BYTES;
            File files = SetupTestKit.newFilesDir("extract-space");
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 51));
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            SetupTestKit.MemoryFolder folder = folder();
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.spaceOf(need - 1));
            expect(!o.complete, "refused");
            expectContains(o.error, "Not enough space: needs 0.6 GB, free 0.5 GB", "the message gives both amounts");
            expect(o.problem == VerifiedExtractor.Problem.NO_SPACE && o.needBytes == need && o.freeBytes == need - 1,
                    "and the outcome carries them exactly: " + o.needBytes + ", " + o.freeBytes);
            expectEquals(before, SetupTestKit.snapshot(files), "not even a directory was created");
            expect(folder.opened.isEmpty(), "the disc was not read");
            VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.spaceOf(need));
            expect(o2.complete, "exactly enough: " + o2.error);
        });

        check("extract: the space check counts only what is still missing", () -> {
            File files = SetupTestKit.newFilesDir("extract-space-resume");
            VerifiedExtractor.Outcome all = VerifiedExtractor.extract(Arrays.asList(I_EXE, I_SMALL, I_ONE, I_EMPTY),
                    VerifiedExtractor.folderReader(folder()), root(files), new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(all.complete, "first part: " + all.error);
            VerifiedExtractor.Outcome rest = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.spaceOf(BIG.length + VerifiedExtractor.RESERVE_BYTES));
            expect(rest.complete, "space for the big file alone is enough: " + rest.error);
        });

        check("extract: stopping keeps finished files, removes the partial one, and reports 'stopped', not an error", () -> {
            File files = SetupTestKit.newFilesDir("extract-cancel");
            SetupTestKit.Recorder progress = new SetupTestKit.Recorder();
            progress.cancelAtBytes = 0;                                         // stop before the first file
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files), progress,
                    SPEC, SetupTestKit.PLENTY);
            expect(o.cancelled && !o.complete && o.error == null, "stopped");
            expectEquals(0, new File(root(files), "inputs").list().length, "no file yet");
            SetupTestKit.Recorder mid = new SetupTestKit.Recorder();
            mid.cancelAtBytes = EXECUTABLE.length + SMALL.length + ONE.length + 1;   // after the first block of sfx/big.rpf
            VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files), mid,
                    SPEC, SetupTestKit.PLENTY);
            expect(o2.cancelled, "stopped again");
            expectEquals(3, new File(root(files), "inputs").list().length, "the three finished files");
            expect(!input(root(files), I_BIG).exists(), "the file that was being copied has no final name");
            expectEquals(0, new File(root(files), "staging").list().length, "no partial file");
            expect(!new File(root(files), "install.json").exists(), "no record yet");
        });

        check("extract: the same file listed twice is copied once", () -> {
            File files = SetupTestKit.newFilesDir("extract-twice");
            SetupCatalog.Item twin = item("copy/of/one.rpf", ONE);
            SetupTestKit.MemoryFolder folder = folder().put("copy/of/one.rpf", ONE);
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(Arrays.asList(I_ONE, twin), VerifiedExtractor.folderReader(folder),
                    root(files), new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(o.complete, "complete: " + o.error);
            expectEquals(Arrays.asList("one.rpf"), folder.opened, "read once");
            expectEquals(1, new File(root(files), "inputs").list().length, "one file");
        });

        check("extract: a second run while one is working is refused and writes nothing", () -> {
            final File files = SetupTestKit.newFilesDir("extract-busy");
            final File files2 = SetupTestKit.newFilesDir("extract-busy-2");
            final CountDownLatch inside = new CountDownLatch(1), release = new CountDownLatch(1);
            final VerifiedExtractor.Outcome[] firstOutcome = new VerifiedExtractor.Outcome[1];
            Thread worker = new Thread(new Runnable() {
                @Override
                public void run() {
                    firstOutcome[0] = VerifiedExtractor.extract(ITEMS, new VerifiedExtractor.ItemReader() {
                        @Override
                        public InputStream open(SetupCatalog.Item it) throws IOException {
                            inside.countDown();
                            try {
                                release.await(20, TimeUnit.SECONDS);
                            } catch (InterruptedException e) {
                                throw new IOException("interrupted");
                            }
                            return VerifiedExtractor.folderReader(folder()).open(it);
                        }
                    }, root(files), new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
                }
            });
            worker.start();
            expect(inside.await(20, TimeUnit.SECONDS), "the first run is working");
            SetupTestKit.MemoryFolder folder = folder();
            VerifiedExtractor.Outcome second = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files2),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            release.countDown();
            worker.join(20000);
            expectEquals(VerifiedExtractor.BUSY_MESSAGE, second.error, "the second run is refused");
            expectEquals(VerifiedExtractor.Problem.BUSY, second.problem, "with its kind");
            expect(folder.opened.isEmpty() && !root(files2).exists(), "and did nothing");
            expect(firstOutcome[0] != null && firstOutcome[0].complete, "the first run finished normally");
            VerifiedExtractor.Outcome third = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files2),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expect(third.complete, "afterwards a run is accepted again: " + third.error);
        });

        check("extract: a link standing where a partial file goes is removed or refused, never written through", () -> {
            // (a) a link to a file that does not exist: writing through it would create that file among the saves
            File files = SetupTestKit.newFilesDir("extract-link-new");
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 111));
            expect(new File(root(files), "staging").mkdirs(), "mkdir staging");
            Files.createSymbolicLink(new File(root(files), "staging/" + I_SMALL.sha256 + ".part").toPath(),
                    new File(files, "saves/made-through-a-link").toPath());
            TreeMap<String, String> saves = SetupTestKit.snapshot(new File(files, "saves"));
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectEquals(saves, SetupTestKit.snapshot(new File(files, "saves")), "nothing appeared among the saves");
            expect(o.complete, "the link was replaced by a real partial file and the copy went through: " + o.error);
            expectAllInPlace(root(files));
            // (b) a link to an existing save: writing through it would overwrite the save
            File files2 = SetupTestKit.newFilesDir("extract-link-save");
            SetupTestKit.write(new File(files2, "saves/slot0"), bytes(64, 112));
            expect(new File(root(files2), "staging").mkdirs(), "mkdir staging");
            Files.createSymbolicLink(new File(root(files2), "staging/" + I_ONE.sha256 + ".part").toPath(),
                    new File(files2, "saves/slot0").toPath());
            TreeMap<String, String> saves2 = SetupTestKit.snapshot(new File(files2, "saves"));
            VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files2),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectEquals(saves2, SetupTestKit.snapshot(new File(files2, "saves")), "the save is untouched");
            expect(!o2.complete, "and the copy stops");
            expectContains(o2.error, "leaves the setup directory", "with the reason");
        });

        check("extract: stopped at each file boundary in turn, the next run copies exactly the rest and rewrites nothing", () -> {
            long[] boundary = new long[ITEMS.size()];                           // bytes done when file k is about to start
            for (int k = 1; k < ITEMS.size(); ++k) boundary[k] = boundary[k - 1] + ITEMS.get(k - 1).bytes;
            for (int k = 0; k < ITEMS.size(); ++k) {
                File files = SetupTestKit.newFilesDir("extract-boundary-" + k);
                SetupTestKit.Recorder first = new SetupTestKit.Recorder();
                first.cancelAtBytes = boundary[k];
                SetupTestKit.MemoryFolder folder1 = folder();
                VerifiedExtractor.Outcome o1 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder1), root(files), first,
                        SPEC, SetupTestKit.PLENTY);
                expect(o1.cancelled && !o1.complete && o1.error == null, "boundary " + k + ": run 1 was stopped");
                List<String> paths = new ArrayList<String>();
                for (SetupCatalog.Item it : ITEMS) paths.add(it.path);
                expectEquals(paths.subList(0, k), folder1.opened, "boundary " + k + ": run 1 read the files before it and no other");
                TreeMap<String, String> finished = SetupTestKit.snapshot(new File(root(files), "inputs"));
                expectEquals(k, finished.size(), "boundary " + k + ": that many files have their final name");
                String[] parts = new File(root(files), "staging").list();
                expectEquals(0, parts == null ? 0 : parts.length, "boundary " + k + ": no partial file");
                expect(!new File(root(files), "install.json").exists(), "boundary " + k + ": no record of an unfinished copy");

                SetupTestKit.MemoryFolder folder2 = folder();
                SetupTestKit.Recorder second = new SetupTestKit.Recorder();
                VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder2), root(files), second,
                        SPEC, SetupTestKit.PLENTY);
                expect(o2.complete, "boundary " + k + ": run 2 completes: " + o2.error);
                expectEquals(paths.subList(k, paths.size()), folder2.opened, "boundary " + k + ": run 2 read the rest and no other");
                expectAllInPlace(root(files));
                TreeMap<String, String> after = SetupTestKit.snapshot(new File(root(files), "inputs"));
                for (String name : finished.keySet()) {
                    expectEquals(finished.get(name), after.get(name), "boundary " + k + ": a file finished by run 1 is the same file, not rewritten");
                }
                expect(!second.decreasing && second.bytesDone == second.bytesTotal, "boundary " + k + ": progress of run 2 ends at the total");
            }
        });

        check("extract: a run killed after a file was checked but before it got its final name copies that file again", () -> {
            for (int k = 0; k < ITEMS.size(); ++k) {
                File files = SetupTestKit.newFilesDir("extract-killed-" + k);
                for (int i = 0; i < k; ++i) {                                   // the files the killed run had finished
                    SetupTestKit.write(input(root(files), ITEMS.get(i)), content(i));
                    VerifiedExtractor.ownerReadOnly(input(root(files), ITEMS.get(i)));
                }
                // what the killed run left: the whole, correct, read-only partial file of file k
                File part = new File(root(files), "staging/" + ITEMS.get(k).sha256 + ".part");
                SetupTestKit.write(part, content(k));
                VerifiedExtractor.ownerReadOnly(part);
                SetupTestKit.MemoryFolder folder = folder();
                VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files),
                        new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
                expect(o.complete, "file " + k + ": completes: " + o.error);
                expectEquals(ITEMS.get(k).path, folder.opened.get(0), "file " + k + ": it is read from the disc again (a partial file is never trusted)");
                expectEquals(ITEMS.size() - k, folder.opened.size(), "file " + k + ": with the files after it");
                expectAllInPlace(root(files));
            }
        });

        check("extract: a run killed after the last file but before the record leaves nothing to copy; the next run reads nothing and writes the record", () -> {
            File files = SetupTestKit.newFilesDir("extract-killed-before-record");
            for (int i = 0; i < ITEMS.size(); ++i) {
                SetupTestKit.write(input(root(files), ITEMS.get(i)), content(i));
                VerifiedExtractor.ownerReadOnly(input(root(files), ITEMS.get(i)));
            }
            TreeMap<String, String> inputs = SetupTestKit.snapshot(new File(root(files), "inputs"));
            SetupTestKit.MemoryFolder folder = folder();
            SetupTestKit.Recorder progress = new SetupTestKit.Recorder();
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files), progress, SPEC,
                    SetupTestKit.spaceOf(0));                                   // a full storage does not matter: nothing is to copy
            expect(o.complete, "complete: " + o.error);
            expect(folder.opened.isEmpty(), "the disc is not read");
            expectEquals(inputs, SetupTestKit.snapshot(new File(root(files), "inputs")), "the files are not touched");
            InstallGuard.Record record = InstallGuard.readRecord(root(files));
            expect(record != null && record.discFilesComplete && !record.bundleComplete, "the record now says that the disc files are complete");
            expect(progress.bytesDone == progress.bytesTotal && progress.filesDone == ITEMS.size(), "progress reports everything done");
        });

        check("extract: a file of the disc copy that changed size between two runs is refused; a file copied before stays", () -> {
            File files = SetupTestKit.newFilesDir("extract-source-resized");
            SetupTestKit.Recorder first = new SetupTestKit.Recorder();
            first.cancelAtBytes = EXECUTABLE.length + SMALL.length + ONE.length;      // stopped before sfx/big.rpf
            expect(VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files), first, SPEC,
                    SetupTestKit.PLENTY).cancelled, "run 1 was stopped");
            TreeMap<String, String> finished = SetupTestKit.snapshot(new File(root(files), "inputs"));
            // run 2: on the disc copy, the file still to copy lost its last byte and a file already copied grew
            SetupTestKit.MemoryFolder shrunk = folder().put("sfx/big.rpf", Arrays.copyOf(BIG, BIG.length - 1))
                    .put("one.rpf", Arrays.copyOf(ONE, ONE.length + 9));
            VerifiedExtractor.Outcome o2 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(shrunk), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectEquals("sfx/big.rpf is incomplete in your disc copy: it ends after " + (BIG.length - 1) + " of " + BIG.length + " bytes.",
                    o2.error, "the shrunken file is named with both sizes");
            expectEquals(VerifiedExtractor.Problem.DISC_COPY_BAD, o2.problem, "the kind");
            expectEquals(Arrays.asList("sfx/big.rpf"), shrunk.opened, "the file copied before is not read again");
            expectEquals(finished, SetupTestKit.snapshot(new File(root(files), "inputs")), "and stays as it was; the shrunken one has no final name");
            // run 3: grown instead
            SetupTestKit.MemoryFolder grown = folder().put("sfx/big.rpf", Arrays.copyOf(BIG, BIG.length + 1));
            VerifiedExtractor.Outcome o3 = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(grown), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.PLENTY);
            expectContains(o3.error, "sfx/big.rpf in your disc copy is not the file of the supported disc (it is longer than " + BIG.length, "grown");
            expectEquals(finished, SetupTestKit.snapshot(new File(root(files), "inputs")), "still nothing new in place");
            // run 4: the right file again
            expect(VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files), new SetupTestKit.Recorder(), SPEC,
                    SetupTestKit.PLENTY).complete, "with the right file the copy completes");
            expectAllInPlace(root(files));
        });

        check("extract: free space that drops while copying stops the copy at the next file, before that file is read", () -> {
            final long reserve = VerifiedExtractor.RESERVE_BYTES;
            final long all = EXECUTABLE.length + SMALL.length + ONE.length + BIG.length;
            for (int k = 1; k < 4; ++k) {                                       // before file k: small.meta, one.rpf, sfx/big.rpf
                File files = SetupTestKit.newFilesDir("extract-space-drops-" + k);
                long left = all;
                for (int i = 0; i < k; ++i) left -= ITEMS.get(i).bytes;
                final long need = left + reserve;
                final SetupTestKit.Budget space = new SetupTestKit.Budget(all + reserve);   // exactly enough at the start
                SetupTestKit.Recorder progress = new SetupTestKit.Recorder();
                progress.atPath = ITEMS.get(k).path;
                progress.atPathDo = () -> space.free = need - 1;                // another app took the room meanwhile
                SetupTestKit.MemoryFolder folder = folder();
                VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder), root(files), progress, SPEC, space);
                expect(!o.complete && !o.cancelled, "file " + k + ": stopped");
                expectEquals(VerifiedExtractor.Problem.NO_SPACE, o.problem, "file " + k + ": the kind");
                expectContains(o.error, "Not enough space left for " + ITEMS.get(k).path + ": needs ", "file " + k + ": the file it stopped before");
                expect(o.needBytes == need && o.freeBytes == need - 1, "file " + k + ": what is still to copy plus the reserve, and what is free: "
                        + o.needBytes + ", " + o.freeBytes);
                expectEquals(k, folder.opened.size(), "file " + k + ": that file was not read");
                expectEquals(k, new File(root(files), "inputs").list().length, "file " + k + ": the files before it are kept");
                expectEquals(0, new File(root(files), "staging").list().length, "file " + k + ": no partial file");
                space.free = need;                                              // room was made: one byte more is enough
                VerifiedExtractor.Outcome again = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files),
                        new SetupTestKit.Recorder(), SPEC, space);
                expect(again.complete, "file " + k + ": with room again the copy completes: " + again.error);
                expectAllInPlace(root(files));
            }
        });

        check("extract: a write that fails is reported as a space problem only when the device is short of space", () -> {
            // The host cannot fill a disk. The write failure is provoked by taking the write permission from staging/
            // just before the partial file of sfx/big.rpf is created; what the storage then reports decides the kind.
            for (int variant = 0; variant < 2; ++variant) {
                final File files = SetupTestKit.newFilesDir("extract-write-fails-" + variant);
                final long reserve = VerifiedExtractor.RESERVE_BYTES, all = EXECUTABLE.length + SMALL.length + ONE.length + BIG.length;
                final SetupTestKit.Budget space = new SetupTestKit.Budget(all + reserve);
                final boolean full = variant == 0;
                final VerifiedExtractor.ItemReader good = VerifiedExtractor.folderReader(folder());
                final File staging = new File(root(files), "staging");
                VerifiedExtractor.ItemReader reader = new VerifiedExtractor.ItemReader() {
                    @Override
                    public InputStream open(SetupCatalog.Item it) throws IOException {
                        if (it.path.equals("sfx/big.rpf")) {
                            expect(staging.setWritable(false, false), "take the write permission");
                            if (full) space.free = 1000;                        // and the storage is as good as full
                        }
                        return good.open(it);
                    }
                };
                VerifiedExtractor.Outcome o;
                try {
                    o = VerifiedExtractor.extract(ITEMS, reader, root(files), new SetupTestKit.Recorder(), SPEC, space);
                } finally {
                    staging.setWritable(true, true);
                }
                expect(!o.complete && !o.cancelled, "variant " + variant + ": failed");
                if (full) {
                    expectEquals(VerifiedExtractor.Problem.NO_SPACE, o.problem, "short of space: the kind");
                    expectContains(o.error, "This device ran out of space while copying sfx/big.rpf: needs ", "short of space: the sentence");
                    expect(o.needBytes == BIG.length + reserve && o.freeBytes == 1000, "short of space: the file's bytes plus the reserve, and what is free");
                } else {
                    expectEquals(VerifiedExtractor.Problem.STORAGE, o.problem, "room enough: the kind");
                    expectContains(o.error, "Could not write the copy of sfx/big.rpf: ", "room enough: the sentence");
                }
                expect(input(root(files), I_ONE).isFile() && !input(root(files), I_BIG).exists(), "variant " + variant + ": files before it are kept");
            }
        });

        check("extract: what an interrupted run left in staging counts as free space and is removed before the copy", () -> {
            File files = SetupTestKit.newFilesDir("extract-leftovers");
            long need = EXECUTABLE.length + SMALL.length + ONE.length + BIG.length + VerifiedExtractor.RESERVE_BYTES;
            File staging = new File(root(files), "staging");
            SetupTestKit.write(new File(staging, I_BIG.sha256 + ".part"), bytes(2 << 20, 121));          // a killed copy
            SetupTestKit.write(new File(staging, "0123.part"), bytes(5000, 122));                          // of a file no catalog lists any more
            SetupTestKit.write(new File(staging, "bundle.part/lib/x.so"), bytes(7000, 123));               // a killed import
            SetupTestKit.write(new File(staging, "notes.txt"), bytes(10, 124));                            // not a leftover of a run
            SetupTestKit.write(new File(files, "saves/slot0"), bytes(64, 125));
            Files.createSymbolicLink(new File(staging, "bundle.part/to-the-saves").toPath(), new File(files, "saves").toPath());
            TreeMap<String, String> saves = SetupTestKit.snapshot(new File(files, "saves"));
            long leftovers = (2 << 20) + 5000 + 7000;
            expectEquals(leftovers, VerifiedExtractor.leftoverBytes(root(files)), "the leftovers' bytes (the link and the other file do not count)");
            expectEquals(0L, VerifiedExtractor.leftoverBytes(new File(files, "nowhere/game-install")), "none where nothing is");
            // A storage of fixed size, in which the setup's own files take up room (so removing the leftovers gives it back).
            long used = SetupTestKit.regularBytes(root(files)), free = need - leftovers;      // free: what the storage reports now
            // one byte short even with the leftovers: refused, and they are still there (nothing is changed before a refusal)
            TreeMap<String, String> before = SetupTestKit.snapshot(files);
            VerifiedExtractor.Outcome refused = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.quota(root(files), used + free - 1));
            expect(refused.problem == VerifiedExtractor.Problem.NO_SPACE && refused.freeBytes == need - 1, "one byte short with them: refused: " + refused.error);
            expectEquals(before, SetupTestKit.snapshot(files), "nothing was changed");
            // exactly enough with them (not without): the copy runs to the end, and they are gone
            VerifiedExtractor.Outcome o = VerifiedExtractor.extract(ITEMS, VerifiedExtractor.folderReader(folder()), root(files),
                    new SetupTestKit.Recorder(), SPEC, SetupTestKit.quota(root(files), used + free));
            expect(o.complete, "enough with them: " + o.error);
            expectEquals(Arrays.asList("notes.txt"), Arrays.asList(staging.list()), "the leftovers are removed; what is not one stays");
            expectEquals(saves, SetupTestKit.snapshot(new File(files, "saves")), "the link inside a leftover was removed, not followed");
            for (SetupCatalog.Item it : ITEMS) expect(input(root(files), it).isFile(), it.path + " is in place");
        });

        check("extract: amounts of space are shown in a way that never promises too much", () -> {
            expectEquals("8.7 GB", VerifiedExtractor.gigabytes(8683375620L, true), "a need is rounded up");
            expectEquals("8.6 GB", VerifiedExtractor.gigabytes(8683375620L, false), "free space is rounded down");
            expectEquals("1.0 GB", VerifiedExtractor.gigabytes(1000000000L, true), "exact values stay");
            expectEquals("0.0 GB", VerifiedExtractor.gigabytes(99999999L, false), "less than a tenth free");
        });

        SetupTestKit.finish("VerifiedExtractorTest");
    }
}
