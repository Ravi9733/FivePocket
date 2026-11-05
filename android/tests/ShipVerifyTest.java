package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;
import static dev.recompbootstrap.smoke.ShipTestKit.library;
import static dev.recompbootstrap.smoke.ShipTestKit.read;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.attribute.FileTime;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.regex.Pattern;

/**
 * The whole read of the game code file and the shader file behind "Ready" (ShipVerify): that every read writes one
 * log line with its time, that the menu's pages do not repeat the read, and that a remembered answer is never given
 * for files that are no longer the same files. Run with java -ea.
 *
 * The two ends are tested connected: the first screen's start (ShipStart.run) makes the read, and the menu's own
 * question (ShipReady.remembered, what LauncherScreen, AboutScreen and SetupDiscScreen call) is answered from it.
 * The lines are counted as the sink receives them, which is what the app writes to the log.
 *
 * What this does NOT show: how many milliseconds the read takes on a device (the lead measures that with these
 * lines), and that the three screens call remembered() at the right moments on a device: their calls are held as
 * text by ShipGlueTest, and LauncherMenu's rule for "read again" by ShipLauncherReadyTest.
 */
public final class ShipVerifyTest {
    private static final Pattern LINE = Pattern.compile("VERIFY_CODE ms=[0-9]+ bytes=[0-9]+ result=(same|changed) why=[a-z-]+ n=[0-9]+");

    /** Collects what the app would write to the log. */
    private static final class Lines implements ShipVerify.Sink {
        final List<String> lines = new ArrayList<String>();

        @Override public void line(String line) {
            lines.add(line);
        }

        int reads() {
            int n = 0;
            for (String line : lines) if (line.startsWith("VERIFY_CODE ")) ++n;
            return n;
        }

        String last() {
            return lines.get(lines.size() - 1);
        }
    }

    private static Lines listen() {
        Lines lines = new Lines();
        ShipVerify.setSink(lines);
        ShipVerify.again();   // nothing remembered from the case before
        return lines;
    }

    private static ShipTestKit.Version installed(String label) throws Exception {
        ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir(label));
        v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
        v.build();
        expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "first start");
        return v;
    }

    private static ShipStart.Outcome start(ShipTestKit.Version v) {
        return ShipStart.run(v.bundle, v.files, false, false, new ShipStart.Watch() {
            @Override public void working() {
            }

            @Override public void at(ShipFirstRun.Step step, long bytesDone, long bytesTotal) {
            }
        }, ShipTestKit.PLENTY);
    }

    /** The three pages of the menu ask, as LauncherScreen, AboutScreen and SetupDiscScreen do. */
    private static ShipReady.Stands[] menuAsks(ShipTestKit.Version v) {
        return new ShipReady.Stands[] {ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands,
                ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_ABOUT).stands,
                ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_SETUP_PAGE).stands};
    }

    /** Another file with other bytes of the same size takes the name (what a replaced file is on storage). */
    private static void replace(File file, byte[] bytes) throws Exception {
        File other = new File(file.getParentFile(), file.getName() + ".other");
        Files.write(other.toPath(), bytes);
        Files.setPosixFilePermissions(other.toPath(), PosixFilePermissions.fromString("r--------"));
        Files.move(other.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    }

    /** The same file is written over with other bytes of the same size; its time of last change is set to time. */
    private static void rewrite(File file, byte[] bytes, FileTime time) throws Exception {
        Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("rw-------"));
        Files.write(file.toPath(), bytes);
        Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("r--------"));
        Files.setLastModifiedTime(file.toPath(), time);
    }

    public static void main(String[] args) {
        check("a start of the app reads the two files once and says so in one line with its time; the menu's pages then read nothing", () -> {
            ShipTestKit.Version v = installed("verify-start");
            Lines log = listen();
            ShipStart.Outcome usual = start(v);
            expect(usual.ok(), "a usual start: " + usual.error);
            expectEquals(1, log.reads(), "one whole read by the first screen: " + log.lines);
            expect(LINE.matcher(log.last()).matches(), "the line's form: " + log.last());
            expect(log.last().contains(" bytes=" + (v.library.length + v.pack.length) + " result=same why=start "),
                    "it names the bytes of the two files, the answer and who asked: " + log.last());
            // The menu opens: the Play tab looks, About is opened, the two-disc page looks (several times each).
            for (int visit = 0; visit < 3; ++visit) {
                for (ShipReady.Stands stands : menuAsks(v)) expectEquals(ShipReady.Stands.DATA, stands, "what the pages are told (the disc files are missing)");
            }
            expectEquals(1, log.reads(), "nine questions of the menu's pages: no further read, no further line: " + log.lines);
            // (control) a caller that does not ask through the memory reads every time.
            ShipReady.of(v.bundle, v.files);
            expectEquals(2, log.reads(), "ShipReady.of reads");
            expect(log.last().contains(" why=check "), log.last());
        });

        check("back from the game, the setup page or the first screen: the next look reads once, and only once", () -> {
            ShipTestKit.Version v = installed("verify-again");
            Lines log = listen();
            menuAsks(v);
            expectEquals(1, log.reads(), "nothing was remembered: the first question of the menu reads");
            expect(log.last().contains(" why=menu "), "and the line names the page that asked: " + log.last());
            ShipVerify.again();   // what LauncherScreen does when LauncherMenu.takeWholeLook answers true
            menuAsks(v);
            menuAsks(v);
            expectEquals(2, log.reads(), "one more read after the return, however often the pages then ask: " + log.lines);
            int n = Integer.parseInt(log.last().substring(log.last().indexOf(" n=") + 3));
            menuAsks(v);
            ShipVerify.again();
            menuAsks(v);
            expect(log.last().endsWith(" n=" + (n + 1)), "n counts the reads of this process: " + log.last());
        });

        check("a file that was replaced, or written again, is read again and the change is found; another app version is never answered from memory", () -> {
            for (String name : new String[] {"private-trace-live.so", "generic-shaders-v41.bin"}) {
                ShipTestKit.Version v = installed("verify-changed");
                Lines log = listen();
                expectEquals(ShipReady.Stands.DATA, menuAsks(v)[0], name + ": in place");
                File file = new File(v.files, "private-trace/" + name);
                byte[] right = read(file), other = right.clone();
                other[other.length / 2] ^= 1;
                FileTime time = Files.getLastModifiedTime(file.toPath());
                // 1. Another file under the same name: same size, and even the same time of last change.
                replace(file, other);
                Files.setLastModifiedTime(file.toPath(), time);
                int before = log.reads();
                expectEquals(ShipReady.Stands.PIECE, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands,
                        name + ": a replaced file is read again and found changed");
                expectEquals(before + 1, log.reads(), name + ": by one read");
                expect(log.last().contains(" result=changed "), name + ": " + log.last());
                expectEquals(ShipReady.Stands.PIECE, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_ABOUT).stands, name + ": the answer \"changed\" is remembered too");
                expectEquals(before + 1, log.reads(), name + ": without a second read");
                // 2. The right bytes are put back as a new file (what the first screen's repair does): read again, in place.
                replace(file, right);
                expectEquals(ShipReady.Stands.DATA, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands, name + ": repaired");
                expectEquals(before + 2, log.reads(), name + ": by one more read");
                // 3. The same file written over in place at another time.
                rewrite(file, other, FileTime.fromMillis(Files.getLastModifiedTime(file.toPath()).toMillis() + 5000));
                expectEquals(ShipReady.Stands.PIECE, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands,
                        name + ": a file written over is read again and found changed");
                expectEquals(before + 3, log.reads(), name + ": by one read");
            }
            // An update: the record is another one. The old version's answer must not be given for it.
            ShipTestKit.Version v = installed("verify-update");
            Lines log = listen();
            menuAsks(v);
            v.build = "t2";
            v.library = library(v.library.length, 91);   // the same size as before
            v.build();
            expectEquals(ShipReady.Stands.CODE, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands, "an update is pending: not ready");
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "the update's first start");
            int before = log.reads();
            expectEquals(ShipReady.Stands.DATA, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands, "after it");
            expectEquals(before + 1, log.reads(), "the new version's files were read, not remembered from the old one");
            // An update that changes the record and leaves both files as they are (a new switch file, say): the answer
            // that was given for the earlier record is not given for this one.
            v.build = "t3";
            v.switches.add("diag-new-in-t3");
            v.build();
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "that update's first start");
            before = log.reads();
            expectEquals(ShipReady.Stands.DATA, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands, "after it");
            expectEquals(before + 1, log.reads(), "another record: read, not remembered");
        });

        check("whatever is remembered, Play reads both files itself: changed game code is never started", () -> {
            ShipTestKit.Version v = installed("verify-play");
            ShipDataImport.Outcome data = ShipDataImport.run(v.config(), new ShipTestKit.MemoryFolder().with(ShipTestKit.dataOf(v)), v.files,
                    new ShipTestKit.Recorder(), ShipTestKit.PLENTY);
            expect(data.complete, "the disc files: " + data.error);
            Lines log = listen();
            expectEquals(ShipReady.Stands.NOTHING, menuAsks(v)[0], "ready");
            // The one change a remembered answer cannot see: the same file, written over, with its old time put back.
            File code = new File(v.files, "private-trace/private-trace-live.so");
            byte[] other = read(code);
            other[other.length / 2] ^= 1;
            rewrite(code, other, Files.getLastModifiedTime(code.toPath()));
            expectEquals(ShipReady.Stands.NOTHING, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands,
                    "(the limit, stated in ShipVerify: this change is not seen until the next whole read)");
            ShipPlay.Result play = ShipPlay.prepare(v.bundle, v.files, false);
            expect(play.request == null && ShipFirstRun.CODE_CHANGED.equals(play.error), "Play refuses: " + play.error);
            expect(log.last().startsWith("VERIFY_PLAY ms=") && log.last().endsWith(" result=refused"), "Play's own read has its line: " + log.last());
            expectEquals(ShipReady.Stands.CODE, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands,
                    "and the menu's look after that refusal offers Finish Setting Up (Play removed the mark)");
            // A developer build (no game code inside) writes no VERIFY line at all.
            int lines = log.lines.size();
            ShipPlay.prepare((ShipBundle) null, v.files, false);
            expectEquals(lines, log.lines.size(), "a build without game code writes no VERIFY line");
        });

        check("a log that fails does not fail the check", () -> {
            ShipTestKit.Version v = installed("verify-sink");
            listen();
            ShipVerify.setSink(new ShipVerify.Sink() {
                @Override public void line(String line) {
                    throw new IllegalStateException("the log is gone");
                }
            });
            expectEquals(ShipReady.Stands.DATA, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands, "the answer stands");
            ShipVerify.setSink(null);
            ShipVerify.again();
            expectEquals(ShipReady.Stands.DATA, ShipReady.remembered(v.bundle, v.files, ShipVerify.WHY_MENU).stands, "and without any log");
        });

        ShipTestKit.finish("ShipVerifyTest");
    }
}
