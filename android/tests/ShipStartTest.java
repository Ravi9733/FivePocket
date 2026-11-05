package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.bytes;
import static dev.recompbootstrap.smoke.ShipTestKit.changes;
import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectContains;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;
import static dev.recompbootstrap.smoke.ShipTestKit.library;
import static dev.recompbootstrap.smoke.ShipTestKit.read;
import static dev.recompbootstrap.smoke.ShipTestKit.sha256;
import static dev.recompbootstrap.smoke.ShipTestKit.snapshot;
import static dev.recompbootstrap.smoke.ShipTestKit.write;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * What the app does when it is opened (ShipStart: the first screen's work), on made-up app versions in temporary
 * files directories: a fresh install, a later start, a carried piece that was changed, the menu's "Finish Setting
 * Up", a killed game data copy's half file before an update, a running game, a developer build. Run with java -ea.
 */
public final class ShipStartTest {
    /** Counts what the screen is told. */
    private static final class Seen implements ShipStart.Watch {
        int working;
        final List<ShipFirstRun.Step> steps = new ArrayList<ShipFirstRun.Step>();

        @Override public void working() {
            ++working;
        }

        @Override public void at(ShipFirstRun.Step step, long bytesDone, long bytesTotal) {
            if (steps.isEmpty() || steps.get(steps.size() - 1) != step) steps.add(step);
        }
    }

    private static ShipTestKit.Version version(String label) throws Exception {
        ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir(label));
        v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
        return v.build();
    }

    private static ShipStart.Outcome open(ShipTestKit.Version v, boolean fromMenu, Seen seen) {
        return ShipStart.run(v.bundle, v.files, fromMenu, false, seen, ShipTestKit.PLENTY);
    }

    private static int count(List<String> log, String start) {
        int n = 0;
        for (String line : log) if (line.startsWith(start)) ++n;
        return n;
    }

    /** Replaces a file's bytes in place (the installed files are read-only for the app). */
    private static void overwrite(File file, byte[] data) throws Exception {
        Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("rw-------"));
        Files.write(file.toPath(), data);
        Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("r--------"));
    }

    public static void main(String[] args) {
        check("a fresh install: the app's pieces are put in place without a question, and missing disc files do not hold it up", () -> {
            ShipTestKit.Version v = version("start-fresh");
            Seen seen = new Seen();
            ShipStart.Outcome first = open(v, false, seen);
            expect(first.ok() && first.bundled, "the menu can follow: " + first.error);
            expectEquals(1, seen.working, "the screen is told once that the work has started");
            expect(seen.steps.contains(ShipFirstRun.Step.GAME_CODE) && seen.steps.contains(ShipFirstRun.Step.GAME_DATA), "progress is reported");
            expectEquals(ShipReady.Stands.DATA, ShipReady.of(v.bundle, v.files).stands, "afterwards only the player's own files are missing");
            expectEquals(3, first.data.discFilesMissing, "which the outcome counts: the three files that are not in the APK");
            // the log is how an install is checked from outside: one line per piece, the result, what is missing
            expectEquals(3 + v.bundledPaths.size(), count(first.log, "PIECE "), "PIECE lines: library, shader pack, start request, bundled pieces");
            expectEquals(1, count(first.log, "FIRST_RUN ok "), "one FIRST_RUN ok line");
            expectEquals(1, count(first.log, "GAME_DATA needed: "), "one GAME_DATA needed line");
            expectEquals(3, count(first.log, "MISSING inputs/"), "one MISSING line per file that comes from the discs");
            expect(first.log.indexOf(first.log.stream().filter(l -> l.startsWith("FIRST_RUN")).findFirst().get())
                    < first.log.indexOf(first.log.stream().filter(l -> l.startsWith("GAME_DATA")).findFirst().get()), "in that order");
        });

        check("a later start copies nothing and says what is in place", () -> {
            ShipTestKit.Version v = version("start-later");
            expect(open(v, false, new Seen()).ok(), "first start");
            Map<String, String> before = snapshot(v.files);
            Seen seen = new Seen();
            ShipStart.Outcome later = open(v, false, seen);
            expect(later.ok(), "ok: " + later.error);
            expectEquals(0, seen.working, "the screen is not told of any work");
            expectEquals(new ArrayList<String>(), changes(before, snapshot(v.files)), "nothing was created, removed or rewritten");
            expect(later.log.get(0).startsWith("CODE in place "), "the first line: " + later.log.get(0));
            expectEquals(0, count(later.log, "PIECE "), "no PIECE line");
            expectEquals(1, count(later.log, "GAME_DATA needed: "), "the game data line again");
        });

        check("an app update with the same record: the usual start switches the counter off in an install whose settings were never saved, once, and says so", () -> {
            final String counter = "private-trace/diag-fps-overlay";
            ShipTestKit.Version v = version("start-counter");
            expect(open(v, false, new Seen()).ok(), "first start");
            expect(!new File(v.files, counter).exists(), "a fresh install has no switch file for the counter");
            // The install as an earlier app version left it: the counter's switch file is there, settings never saved.
            // The new app version carries the same record, so its start is a usual start (nothing is copied).
            write(new File(v.files, counter), new byte[0]);
            Seen seen = new Seen();
            ShipStart.Outcome usual = open(v, false, seen);
            expect(usual.ok() && seen.working == 0, "a usual start, no full work: " + usual.error);
            expect(!new File(v.files, counter).exists(), "the counter's switch file is gone: the counter is off");
            expectEquals("COUNTER_OFF removed=private-trace/diag-fps-overlay why=settings-never-saved", usual.log.get(1), "the log says so, after the CODE line: " + usual.log);
            expect(usual.log.get(0).startsWith("CODE in place "), "the first line: " + usual.log.get(0));
            ShipStart.Outcome next = open(v, false, new Seen());
            expectEquals(0, count(next.log, "COUNTER_OFF"), "the start after that has nothing to remove and says nothing");
            // While the game runs nothing is touched (the game read the switch when it started).
            write(new File(v.files, counter), new byte[0]);
            ShipStart.Outcome running = ShipStart.run(v.bundle, v.files, false, true, new Seen(), ShipTestKit.PLENTY);
            expect(running.ok() && new File(v.files, counter).isFile(), "the game is running: the file stays");
            // Saved settings: the file is the player's and stays, and no line is written.
            write(new File(v.files, "settings/settings.json"), "{\"version\":1}\n".getBytes(java.nio.charset.StandardCharsets.US_ASCII));
            ShipStart.Outcome saved = open(v, false, new Seen());
            expect(saved.ok() && new File(v.files, counter).isFile(), "settings were saved: the counter's file is kept");
            expectEquals(0, count(saved.log, "COUNTER_OFF"), "and nothing is said");
            // The full work (Finish Setting Up) on a never-saved install says it as well.
            ShipTestKit.Version w = version("start-counter-forced");
            expect(open(w, false, new Seen()).ok(), "first start");
            write(new File(w.files, counter), new byte[0]);
            ShipStart.Outcome forced = open(w, true, new Seen());
            expect(forced.ok() && !new File(w.files, counter).exists(), "the full work removes it too: " + forced.error);
            expectEquals(1, count(forced.log, "COUNTER_OFF removed="), "and the log says so: " + forced.log);
            expect(forced.log.indexOf("COUNTER_OFF removed=private-trace/diag-fps-overlay why=settings-never-saved")
                    > forced.log.indexOf(forced.log.stream().filter(l -> l.startsWith("FIRST_RUN")).findFirst().get()), "after the FIRST_RUN line");
        });

        check("a piece the app carries that has other bytes of the same size is put back by the next start of the app", () -> {
            ShipTestKit.Version v = version("start-changed-piece");
            expect(open(v, false, new Seen()).ok(), "first start");
            String module = "modules/boot/store/dataflow.spv";
            File file = new File(v.files, "private-trace/" + module);
            byte[] right = read(file);
            overwrite(file, bytes(right.length, 777));
            expectEquals(ShipFirstRun.State.CURRENT, ShipFirstRun.check(v.bundle, v.files), "(the cheap check compares sizes: it cannot see this)");
            expectEquals(ShipReady.Stands.PIECE, ShipReady.of(v.bundle, v.files).stands, "the menu's rule sees it: not ready");
            expectEquals(3, ShipReady.of(v.bundle, v.files).data.discFilesMissing, "(it is not counted among the files from the discs: those are 3)");
            Seen seen = new Seen();
            ShipStart.Outcome repaired = open(v, false, seen);
            expect(repaired.ok(), "ok: " + repaired.error);
            expectEquals(sha256(right), sha256(read(file)), "the carried bytes are back");
            expectEquals(1, seen.working, "the full work ran");
            expectEquals(1, count(repaired.log, "PIECE_CHANGED count=1 first=" + module), "the log says which piece was found changed");
            expectEquals(ShipReady.Stands.DATA, ShipReady.of(v.bundle, v.files).stands, "and only the player's files are missing again");
        });

        check("the game code file or the shader file with other bytes of the same size: the menu's rule says not ready, and the next start puts it back", () -> {
            for (String name : new String[] {"private-trace-live.so", "generic-shaders-v41.bin"}) {
                ShipTestKit.Version v = version("start-changed-code");
                expect(open(v, false, new Seen()).ok(), "first start");
                File file = new File(v.files, "private-trace/" + name);
                byte[] right = read(file);
                byte[] other = right.clone();
                other[other.length / 2] ^= 1;                           // one bit in the middle: the same size, a library's header still
                overwrite(file, other);
                expectEquals(ShipFirstRun.State.CURRENT, ShipFirstRun.check(v.bundle, v.files), name + ": (the cheap check compares sizes: it cannot see this)");
                ShipReady ready = ShipReady.of(v.bundle, v.files);
                expectEquals(ShipReady.Stands.PIECE, ready.stands, name + ": the menu's rule reads the file: not ready");
                expectEquals("[private-trace/" + name + "]", ready.changedCode.toString(), name + ": and names it");
                Seen seen = new Seen();
                ShipStart.Outcome repaired = open(v, false, seen);
                expect(repaired.ok(), name + ": ok: " + repaired.error);
                expectEquals(sha256(right), sha256(read(file)), name + ": the carried bytes are back");
                expectEquals(1, seen.working, name + ": the full work ran");
                expectEquals(1, count(repaired.log, "PIECE_CHANGED count=1 first=private-trace/" + name), name + ": the log says which file was found changed: " + repaired.log);
                expectEquals(ShipReady.Stands.DATA, ShipReady.of(v.bundle, v.files).stands, name + ": and only the player's files are missing again");
                // an unchanged install is not touched by the reading: a later start copies nothing
                Seen quiet = new Seen();
                ShipStart.Outcome later = open(v, false, quiet);
                expect(later.ok() && quiet.working == 0 && count(later.log, "PIECE") == 0, name + ": the start after that copies nothing: " + later.log);
            }
        });

        check("Finish Setting Up (opened from the menu) checks every piece by its content again, whatever was marked", () -> {
            ShipTestKit.Version v = version("start-force");
            expect(open(v, false, new Seen()).ok(), "first start");
            // Something a usual start does not look for: a folder the game's start names is gone (the mark is there and
            // every piece has its place, its size and its content).
            File cache = new File(v.files, "private-trace/inputs/native-cache");
            Files.delete(cache.toPath());
            Seen usualSeen = new Seen();
            ShipStart.Outcome usual = open(v, false, usualSeen);
            expect(usual.ok() && usualSeen.working == 0, "a usual start does not do the full work");
            expect(!cache.exists(), "(a usual start does not put the folder back)");
            Seen seen = new Seen();
            ShipStart.Outcome forced = open(v, true, seen);
            expect(forced.ok(), "ok: " + forced.error);
            expect(cache.isDirectory(), "the folder is back");
            expectEquals(1, seen.working, "the full work ran");
        });

        check("the half file of a killed game data copy does not make an update refuse for lack of room, and is removed", () -> {
            ShipTestKit.Version v = version("start-left-over");
            expect(open(v, false, new Seen()).ok(), "version 1");
            // what a copy of a disc file leaves when the app is killed inside it: 2 MiB of a half-written file
            File half = new File(v.files, "private-trace/" + SetupDiscCopy.STAGING + "/" + SetupDiscCopy.PART);
            write(half, bytes(2 << 20, 55));
            // the update: other game code. The disk holds it with 64 KiB to spare - but only without the half file.
            v.build = "t2";
            v.library = library(v.library.length + 4096, 56);
            v.build();
            long capacity = ShipTestKit.treeBytes(v.files) - (2 << 20) + v.library.length + ShipFirstRun.RESERVE_BYTES + (64 << 10);
            ShipStart.Outcome update = ShipStart.run(v.bundle, v.files, false, false, new Seen(), ShipTestKit.diskOf(v.files, capacity));
            expect(update.ok(), "the update is put in place: " + update.error);
            expect(!half.exists(), "the half file is gone");
            expectEquals(1, count(update.log, "LEFT_OVERS removed bytes=" + (2 << 20)), "and the log says how much was freed");
            expectEquals(sha256(v.library), sha256(read(new File(v.files, "private-trace/private-trace-live.so"))), "version 2's game code");
        });

        check("while a copy of game data runs, its half file is left alone", () -> {
            ShipTestKit.Version v = version("start-copy-running");
            expect(open(v, false, new Seen()).ok(), "first start");
            File half = new File(v.files, "private-trace/" + SetupDiscCopy.STAGING + "/" + SetupDiscCopy.PART);
            write(half, bytes(5000, 57));
            expect(VerifiedExtractor.begin(), "(a copy holds the lock)");
            try {
                expectEquals(0L, ShipFirstRun.removeCopyLeftOvers(v.files), "nothing is removed under a running copy");
                expect(half.isFile(), "the half file is still there");
            } finally {
                VerifiedExtractor.end();
            }
            expectEquals(5000L, ShipFirstRun.removeCopyLeftOvers(v.files), "once the copy has ended it is removed");
        });

        check("while the game runs nothing is touched: in place, the menu follows; not in place, the screen says to close the game", () -> {
            ShipTestKit.Version v = version("start-game-runs");
            Map<String, String> empty = snapshot(v.files);
            ShipStart.Outcome notYet = ShipStart.run(v.bundle, v.files, false, true, new Seen(), ShipTestKit.PLENTY);
            expectEquals(ShipStart.GAME_RUNNING, notYet.error, "a first start under a running game");
            expectEquals(new ArrayList<String>(), changes(empty, snapshot(v.files)), "nothing was written");
            expect(open(v, false, new Seen()).ok(), "first start");
            Map<String, String> before = snapshot(v.files);
            ShipStart.Outcome running = ShipStart.run(v.bundle, v.files, false, true, new Seen(), ShipTestKit.PLENTY);
            expect(running.ok() && running.data == null, "pieces in place: the menu follows, and nothing was read");
            expectEquals(ShipStart.GAME_RUNNING, ShipStart.run(v.bundle, v.files, true, true, new Seen(), ShipTestKit.PLENTY).error,
                    "Finish Setting Up under a running game waits for the game to be closed");
            expectEquals(new ArrayList<String>(), changes(before, snapshot(v.files)), "nothing was written in either case");
        });

        check("a build without game code inside (a developer build): nothing is done and the menu follows", () -> {
            File files = ShipTestKit.newFilesDir("start-developer");
            write(new File(files, "private-trace/private-trace-live.so"), library(5000, 3));
            Map<String, String> before = snapshot(files);
            Seen seen = new Seen();
            ShipStart.Outcome outcome = ShipStart.run(new ShipTestKit.MemoryBundle(), files, false, false, seen, ShipTestKit.PLENTY);
            expect(outcome.ok() && !outcome.bundled && outcome.log.isEmpty() && seen.working == 0, "nothing to do, nothing logged");
            expectEquals(new ArrayList<String>(), changes(before, snapshot(files)), "nothing was touched");
        });

        check("a failure is the first run's own sentence, and the menu does not follow", () -> {
            ShipTestKit.Version v = version("start-fail");
            ShipStart.Outcome failed = ShipStart.run(v.bundle, v.files, false, false, new Seen(), ShipTestKit.spaceOf(1000));
            expect(!failed.ok(), "not ok");
            expectContains(failed.error, "Not enough free space", "the sentence");
            expectEquals(1, count(failed.log, "FIRST_RUN failed "), "the log has the failure");
        });

        ShipTestKit.finish("ShipStartTest");
    }
}
