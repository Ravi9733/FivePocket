package dev.recompbootstrap.smoke;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Host test of the capture bounds and files: CaptureStore (one per second, the folder cap, nothing else deleted),
 * CaptureCensus (the renderer's census trigger file) and CaptureExport (the copy to the shared Pictures folder).
 * Needs only a JDK:
 *   javac -d out CaptureStore.java CaptureExport.java CaptureCensus.java android/tests/CaptureStoreTest.java
 *   java -cp out dev.recompbootstrap.smoke.CaptureStoreTest
 * Every file is made in fresh temporary directories, removed at the end.
 */
public final class CaptureStoreTest {
    private interface Case { void run() throws Exception; }
    private static int passed, failed;
    private static final List<File> roots = new ArrayList<>();
    private static final String RUN1 = "run-0123456789abcdef0123456789abcdef", RUN2 = "run-fedcba9876543210fedcba9876543210";

    private static void test(String name, Case body) {
        try { body.run(); ++passed; System.out.println("PASS     " + name); }
        catch (Throwable error) { ++failed; System.out.println("FAIL     " + name + ": " + error); }
    }
    private static void check(boolean condition, String what) { if (!condition) throw new AssertionError(what); }

    private static File newRoot() throws IOException {
        File root = Files.createTempDirectory("capture-store-test").toFile();
        roots.add(root);
        return root;
    }
    private static File write(File dir, String name, int bytes, long modified) throws IOException {
        dir.mkdirs();
        File f = new File(dir, name);
        try (FileOutputStream out = new FileOutputStream(f)) { byte[] b = new byte[bytes]; Arrays.fill(b, (byte) name.length()); out.write(b); }
        check(f.setLastModified(modified), "setLastModified " + f);
        return f;
    }
    private static void delete(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) delete(c);
        f.delete();
    }

    public static void main(String[] args) {
        // ---- one capture per second
        test("Gate: one per second; a refused press does not move the interval", () -> {
            CaptureStore.Gate gate = new CaptureStore.Gate(1000);
            check(gate.allow(5000), "first");
            check(!gate.allow(5999), "999 ms later refused");
            check(gate.allow(6000), "1000 ms after the first allowed (the refused one at 5999 did not restart the interval)");
            check(!gate.allow(6500), "500 ms after");
            check(gate.allow(7000), "again at 7000");
        });
        test("Gate: the very first press at time 0 is allowed", () -> check(new CaptureStore.Gate(1000).allow(0), "time 0"));

        // ---- names and numbers
        test("Run folder: request id, else boot time", () -> {
            check(CaptureStore.runName("0123456789abcdef0123456789abcdef", 5).equals(RUN1), "request id");
            check(CaptureStore.runName("../x", 1234).equals("boot-1234"), "a bad id is not used as a path");
            check(CaptureStore.runName(null, 99).equals("boot-99"), "no id");
        });
        test("Numbers continue after the highest picture or .part in the folder", () -> {
            File run = new File(newRoot(), RUN1);
            check(CaptureStore.nextNumber(run) == 1, "missing folder -> 1");
            write(run, "cap-001.png", 1, 1000); write(run, "cap-007.png.part", 1, 1000); write(run, "notes.txt", 1, 1000);
            check(CaptureStore.nextNumber(run) == 8, "after 7: " + CaptureStore.nextNumber(run));
            check(CaptureStore.pictureName(8).equals("cap-008.png") && CaptureStore.pictureName(1234).equals("cap-1234.png"), "names");
        });

        test("A capture that left no file does not hand its number to the next capture of the same window", () -> {
            File run = new File(newRoot(), RUN1);
            check(CaptureStore.nextNumber(run, 0) == 1, "a window's first capture in a new folder");
            // capture 1 left no file (storage full): the folder is still empty, the window has used the number 1
            check(CaptureStore.nextNumber(run, 1) == 2, "the next is 2, not 1 again: " + CaptureStore.nextNumber(run, 1));
            write(run, "cap-002.png", 1, 1000);
            check(CaptureStore.nextNumber(run, 2) == 3, "after a saved one");
            // a folder that already holds higher numbers (never lower than what is there)
            write(run, "cap-009.png", 1, 1000);
            check(CaptureStore.nextNumber(run, 3) == 10, "above the highest in the folder: " + CaptureStore.nextNumber(run, 3));
        });
        test("Unfinished pictures (.part) of an ended game process are removed; nothing else is", () -> {
            File root = newRoot(), run = new File(root, RUN1), boot = new File(root, "boot-1234");
            File[] gone = {write(run, "cap-003.png.part", 5, 1000), write(boot, "cap-001.png.part", 5, 1000)};
            File[] kept = {write(run, "cap-001.png", 5, 1000), write(run, "notes.txt.part", 5, 1000), write(run, "cap-1.png.part", 5, 1000),
                    write(root, "cap-004.png.part", 5, 1000), write(root, "exported.txt", 5, 1000), write(new File(root, "other"), "cap-005.png.part", 5, 1000),
                    write(new File(run, "deeper"), "cap-006.png.part", 5, 1000)};
            File folder = new File(run, "cap-007.png.part");
            check(folder.mkdir(), "a folder under a part file's name");
            File outside = write(newRoot(), "target.bin", 9, 1000);
            Files.createSymbolicLink(new File(run, "cap-008.png.part").toPath(), outside.toPath());
            check(CaptureStore.removeUnfinished(root) == 2, "two removed");
            for (File f : gone) check(!f.exists(), "removed " + f);
            for (File f : kept) check(f.isFile(), "kept " + f);
            check(folder.isDirectory() && outside.isFile() && outside.length() == 9, "a folder and a link's target are left alone");
            check(CaptureStore.removeUnfinished(root) == 0 && CaptureStore.removeUnfinished(new File(root, "absent")) == 0, "nothing left; a missing folder is fine");
        });

        // ---- the folder cap
        test("Prune keeps the newest N pictures over all runs, deletes the older ones", () -> {
            File root = newRoot();
            for (int i = 1; i <= 5; ++i) write(new File(root, i % 2 == 0 ? RUN2 : RUN1), CaptureStore.pictureName(i), 10, 100_000L * i);
            List<CaptureStore.Picture> gone = CaptureStore.prune(root, 3, Long.MAX_VALUE);
            check(gone.size() == 2, "two deleted: " + gone.size());
            check(gone.get(0).relative().equals(RUN1 + "/cap-001.png") && gone.get(1).relative().equals(RUN2 + "/cap-002.png"), "oldest two");
            check(CaptureStore.list(root).size() == 3, "three kept");
        });
        test("Prune at exactly N deletes nothing", () -> {
            File root = newRoot();
            for (int i = 1; i <= 3; ++i) write(new File(root, RUN1), CaptureStore.pictureName(i), 10, 100_000L * i);
            check(CaptureStore.prune(root, 3, Long.MAX_VALUE).isEmpty(), "nothing deleted");
        });
        test("Prune by bytes: newest first while they fit; the newest is always kept", () -> {
            File root = newRoot();
            write(new File(root, RUN1), "cap-001.png", 40, 100_000);
            write(new File(root, RUN1), "cap-002.png", 10, 200_000);
            write(new File(root, RUN1), "cap-003.png", 50, 300_000);
            List<CaptureStore.Picture> gone = CaptureStore.prune(root, 100, 60);
            check(gone.size() == 1 && gone.get(0).file.getName().equals("cap-001.png"), "001 deleted (50+10+40 > 60): " + gone.size());
            File big = newRoot();
            write(new File(big, RUN1), "cap-001.png", 500, 100_000);
            check(CaptureStore.prune(big, 100, 60).isEmpty(), "one picture larger than the cap is kept");
        });
        test("Prune never keeps an older picture after dropping a newer one", () -> {
            File root = newRoot();
            write(new File(root, RUN1), "cap-001.png", 5, 100_000);   // would fit on its own
            write(new File(root, RUN1), "cap-002.png", 50, 200_000);  // does not fit
            write(new File(root, RUN1), "cap-003.png", 40, 300_000);
            List<CaptureStore.Picture> gone = CaptureStore.prune(root, 100, 60);
            check(gone.size() == 2, "002 and the older 001 dropped: " + gone.size());
        });
        test("Prune deletes nothing but cap-NNN.png in run folders", () -> {
            File root = newRoot();
            File run = new File(root, RUN1);
            List<File> others = new ArrayList<>();
            others.add(write(run, "cap-001.png.part", 10, 1000));
            others.add(write(run, "cap-01.png", 10, 1000));       // two digits: not ours
            others.add(write(run, "cap-002.jpg", 10, 1000));
            others.add(write(run, "notes.txt", 10, 1000));
            others.add(write(root, "exported.txt", 10, 1000));
            others.add(write(root, ".census-owned", 0, 1000));
            others.add(write(new File(root, "other-folder"), "cap-003.png", 10, 1000));
            others.add(write(new File(new File(run, "deeper"), "x"), "cap-004.png", 10, 1000));
            write(run, "cap-005.png", 10, 2000);
            write(run, "cap-006.png", 10, 3000);
            List<CaptureStore.Picture> gone = CaptureStore.prune(root, 1, Long.MAX_VALUE);
            check(gone.size() == 1 && gone.get(0).file.getName().equals("cap-005.png"), "only cap-005 deleted");
            for (File f : others) check(f.exists(), "kept " + f);
            check(run.isDirectory() && new File(run, "cap-006.png").exists(), "folder and newest kept");
        });
        test("List: oldest first, ties by path", () -> {
            File root = newRoot();
            write(new File(root, RUN2), "cap-001.png", 1, 5000);
            write(new File(root, RUN1), "cap-002.png", 1, 5000);
            write(new File(root, RUN1), "cap-001.png", 1, 9000);
            List<CaptureStore.Picture> all = CaptureStore.list(root);
            check(all.get(0).relative().equals(RUN1 + "/cap-002.png") && all.get(1).relative().equals(RUN2 + "/cap-001.png")
                    && all.get(2).relative().equals(RUN1 + "/cap-001.png"), "order");
        });

        // ---- the census trigger
        test("Census: arms by creating the file, removes it at disarm, then cools down", () -> {
            File files = newRoot(); new File(files, "private-trace").mkdirs();
            File trigger = new File(files, CaptureCensus.TRIGGER);
            CaptureCensus c = new CaptureCensus(files, true);
            check(c.arm(1000).equals("armed") && trigger.exists(), "armed, file made");
            check(c.arm(1500).equals("busy"), "busy while held");
            c.disarm();
            check(!trigger.exists() && !new File(files, CaptureCensus.MARKER).exists(), "file and marker removed");
            check(c.arm(1000 + CaptureCensus.COOLDOWN_MS - 1).equals("cooldown"), "cooldown");
            check(c.arm(1000 + CaptureCensus.COOLDOWN_MS).equals("armed"), "after the cooldown");
        });
        test("Census: someone else's trigger file is never created over or removed", () -> {
            File files = newRoot(); new File(files, "private-trace").mkdirs();
            File trigger = write(new File(files, "private-trace"), "diag-frame-census", 3, 1000);
            CaptureCensus c = new CaptureCensus(files, true);
            check(c.arm(1000).equals("present"), "present");
            c.disarm(); c.cleanUpEarlierRun();
            check(trigger.exists() && trigger.length() == 3, "left alone");
        });
        test("Census: off in a build that is not debuggable; no-folder without private-trace", () -> {
            File files = newRoot();
            check(new CaptureCensus(files, false).arm(1000).equals("off"), "off");
            check(new CaptureCensus(files, true).arm(1000).equals("no-folder"), "no-folder");
            check(!new File(files, CaptureCensus.TRIGGER).exists(), "nothing made");
        });
        test("Census: a trigger left by an ended process is removed at the next start", () -> {
            File files = newRoot(); new File(files, "private-trace").mkdirs();
            new CaptureCensus(files, true).arm(1000); // process "ends" without disarm
            check(new File(files, CaptureCensus.TRIGGER).exists(), "still there");
            new CaptureCensus(files, true).cleanUpEarlierRun();
            check(!new File(files, CaptureCensus.TRIGGER).exists() && !new File(files, CaptureCensus.MARKER).exists(), "removed");
        });

        // ---- the copy to the shared Pictures folder
        test("Export: copies every picture once, bytes equal, ledger written", () -> {
            File root = newRoot();
            write(new File(root, RUN1), "cap-001.png", 70000, 1000);
            write(new File(root, RUN1), "cap-002.png", 10, 2000);
            write(new File(root, RUN1), "cap-003.png.part", 10, 3000);
            FakeSink sink = new FakeSink();
            CaptureExport.Result r = CaptureExport.copyAll(root, sink);
            check(r.copied == 2 && r.already == 0 && r.error == null, "copied 2: " + r.sentence());
            check(sink.published.keySet().equals(new java.util.TreeSet<>(Arrays.asList("gta5-" + RUN1 + "-cap-001.png", "gta5-" + RUN1 + "-cap-002.png"))), "names " + sink.published.keySet());
            check(Arrays.equals(sink.published.get("gta5-" + RUN1 + "-cap-001.png"), Files.readAllBytes(new File(root, RUN1 + "/cap-001.png").toPath())), "bytes");
            CaptureExport.Result again = CaptureExport.copyAll(root, sink);
            check(again.copied == 0 && again.already == 2 && sink.opened == 2, "second time: nothing copied");
            long[] s = CaptureExport.summary(root);
            check(s[0] == 2 && s[1] == 70010 && s[2] == 0, "summary " + Arrays.toString(s));
            check(new File(root, RUN1 + "/cap-001.png").exists(), "the private copy stays");
        });
        test("Export: a failed copy is withdrawn, not listed, and stops the copy", () -> {
            File root = newRoot();
            write(new File(root, RUN1), "cap-001.png", 10, 1000);
            write(new File(root, RUN1), "cap-002.png", 10, 2000);
            FakeSink sink = new FakeSink(); sink.failPublish = 1;
            CaptureExport.Result r = CaptureExport.copyAll(root, sink);
            check(r.copied == 0 && r.error != null, "stopped at the first: " + r.sentence());
            check(sink.abandoned == 1 && sink.published.isEmpty() && sink.opened == 1, "withdrawn, the second not tried");
            check(CaptureExport.ledger(root).isEmpty(), "not listed");
            sink.failPublish = 0;
            check(CaptureExport.copyAll(root, sink).copied == 2, "the next request copies both");
        });
        test("Export: a refused destination stops with a sentence", () -> {
            File root = newRoot();
            write(new File(root, RUN1), "cap-001.png", 10, 1000);
            FakeSink sink = new FakeSink(); sink.refuse = true;
            CaptureExport.Result r = CaptureExport.copyAll(root, sink);
            check(r.copied == 0 && r.error != null && r.sentence().contains("refused"), r.sentence());
            check(r.refused, "and the result says that the destination refused (it was not a failure to write)");
            check(CaptureExport.copyAll(newRoot(), sink).sentence().equals("No captures yet."), "empty");
        });

        for (File root : roots) delete(root);
        System.out.println("CaptureStoreTest: " + passed + " passed, " + failed + " failed");
        if (failed > 0) System.exit(1);
    }

    /** A shared folder in memory. */
    private static final class FakeSink implements CaptureExport.Sink {
        final Map<String, byte[]> published = new TreeMap<>();
        int opened, abandoned, failPublish; boolean refuse;
        @Override public CaptureExport.Target open(String name, String mime) {
            if (refuse) return null;
            ++opened;
            check(mime.equals("image/png"), "mime");
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            return new CaptureExport.Target() {
                @Override public OutputStream stream() { return bytes; }
                @Override public void publish() throws IOException {
                    if (failPublish > 0) { --failPublish; throw new IOException("storage full"); }
                    published.put(name, bytes.toByteArray());
                }
                @Override public void abandon() { ++abandoned; }
            };
        }
    }
}
