package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectContains;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;
import static dev.recompbootstrap.smoke.ShipTestKit.library;
import static dev.recompbootstrap.smoke.ShipTestKit.mode;
import static dev.recompbootstrap.smoke.ShipTestKit.read;
import static dev.recompbootstrap.smoke.ShipTestKit.sha256;
import static dev.recompbootstrap.smoke.ShipTestKit.snapshot;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Arrays;
import java.util.Map;
import java.util.TreeMap;

/**
 * The connection the ship build exists for: Play (the launcher's own LaunchRequest, unchanged) starts the game from
 * what ShipFirstRun and ShipDataImport installed. Run with java -ea. Everything up to the four values handed to the
 * game window is exercised here; the window itself and its loader run only on a device.
 */
public final class ShipPlayTest {
    private static ShipTestKit.Version freshInstall(String label) throws IOException {
        ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir(label)).build();
        ShipFirstRun.Result first = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
        expect(first.ok(), "first start: " + first.error);
        ShipDataImport.Outcome data = ShipDataImport.run(v.config(), new ShipTestKit.MemoryFolder().with(ShipTestKit.dataOf(v)), v.files,
                new ShipTestKit.Recorder(), ShipTestKit.PLENTY);
        expect(data.complete, "data import: " + data.error);
        return v;
    }

    /** As freshInstall, with the boot modules and the image inside the APK, as the real build carries them. */
    private static ShipTestKit.Version freshInstallWithPieces(String label) throws IOException {
        ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir(label));
        v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
        v.build();
        ShipFirstRun.Result first = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
        expect(first.ok(), "first start: " + first.error);
        ShipDataImport.Outcome data = ShipDataImport.run(v.config(), new ShipTestKit.MemoryFolder().with(ShipTestKit.dataOf(v)), v.files,
                new ShipTestKit.Recorder(), ShipTestKit.PLENTY);
        expect(data.complete, "data import: " + data.error);
        return v;
    }

    private static void overwrite(File file, byte[] bytes) throws IOException {
        Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("rw-------"));
        Files.write(file.toPath(), bytes);
        Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("r--------"));
    }

    private static int runDirectories(File files) {
        int count = 0;
        for (String name : new File(files, "private-trace").list()) if (name.startsWith("run-")) ++count;
        return count;
    }

    public static void main(String[] args) {
        check("fresh install, first start, data import: Play prepares a start of exactly the installed game code", () -> {
            ShipTestKit.Version v = freshInstall("play");
            ShipPlay.Result play = ShipPlay.prepare(v.bundle, v.files, false);
            expectEquals(null, play.error, "Play's error");
            LaunchRequest.Prepared request = play.request;
            expect(request.requestId.matches("[0-9a-f]{32}") && !request.requestId.equals(v.runId), "a new run id: " + request.requestId);
            expectEquals(sha256(v.library), request.expectedSha256, "the SHA-256 the game window will require of the game code");
            File run = new File(v.files, "private-trace/run-" + request.requestId);
            byte[] arguments = read(new File(run, "arguments.bin"));
            expectEquals(sha256(arguments), request.manifestSha256, "the SHA-256 the game window will require of the start arguments");
            expect(Arrays.equals(v.requestFor(request.requestId), arguments), "the arguments are the bundled ones, for the new run");
            expectEquals("r--------", mode(new File(run, "arguments.bin")), "mode of the new start request (the loader's rule)");
            expectEquals("rwx------", mode(run), "mode of the new run directory (the loader's rule)");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            expectEquals(sha256(data.get("decoded.image.bin")), sha256(read(new File(run, "decoded.image.bin"))),
                    "the new run's game image is the imported one");
            // what the game process itself checks of every argument: each path it names exists
            for (String named : LaunchRequest.referencedInputs(arguments, v.files.getAbsolutePath())) {
                expect(new File(named).exists(), "the start request names something that is there: " + named);
            }
        });

        check("before the first start has finished its work Play refuses and creates nothing", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("early")).build();
            TreeMap<String, String> before = snapshot(v.files);
            ShipPlay.Result play = ShipPlay.prepare(v.bundle, v.files, true);
            expectEquals("this app version's game code is not in place yet; close the app and open it again", play.error, "the reason");
            expect(play.request == null, "no request");
            expectEquals(before.toString(), snapshot(v.files).toString(), "files");
            // after an update whose game code is not yet in place: the same refusal, although an older install is complete
            ShipTestKit.Version installed = freshInstall("pending");
            installed.library = library(310000, 61);
            installed.build();
            expectEquals("this app version's game code is not in place yet; close the app and open it again",
                    ShipPlay.prepare(installed.bundle, installed.files, false).error, "after an update, before its first start");
        });

        check("game code that is not the bundled one is never started, not even with 'start anyway'; the next start repairs it", () -> {
            ShipTestKit.Version v = freshInstall("changed");
            File code = new File(v.files, "private-trace/private-trace-live.so");
            byte[] other = v.library.clone();
            other[other.length - 1] ^= 1;   // same size, same mode: only its SHA-256 tells
            Files.setPosixFilePermissions(code.toPath(), PosixFilePermissions.fromString("rw-------"));
            Files.write(code.toPath(), other);
            Files.setPosixFilePermissions(code.toPath(), PosixFilePermissions.fromString("r--------"));
            String[] runsBefore = new File(v.files, "private-trace").list();
            for (boolean accept : new boolean[] {true, false}) {   // "start anyway" first: it must not start it either
                ShipPlay.Result play = ShipPlay.prepare(v.bundle, v.files, accept);
                expect(play.request == null && play.error != null, "refused (start anyway " + accept + ")");
                expect(!LaunchRequest.UNQUALIFIED.equals(play.error), "no offer to start it anyway: " + play.error);
            }
            expectEquals(runsBefore.length, new File(v.files, "private-trace").list().length, "no run directory was created");
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "the next start has work to do");
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "the next start");
            expectEquals(sha256(v.library), sha256(read(code)), "the bundled game code is back");
            expectEquals(null, ShipPlay.prepare(v.bundle, v.files, false).error, "Play works again");
        });

        check("a game code file the game window would refuse although its bytes are right: Play refuses, and the menu's own item repairs it", () -> {
            ShipTestKit.Version v = freshInstall("unusable");
            File code = new File(v.files, "private-trace/private-trace-live.so");
            // A second name for the file: the loader refuses that. Sizes, modes and bytes are all as recorded, so the
            // cheap look and a usual start of the app find nothing ("close the app and open it again" repaired nothing).
            Files.createLink(new File(v.files, "private-trace/second-name.so").toPath(), code.toPath());
            expectEquals(ShipFirstRun.State.CURRENT, ShipFirstRun.check(v.bundle, v.files), "(the cheap check finds everything in place)");
            ShipPlay.Result play = ShipPlay.prepare(v.bundle, v.files, false);
            expect(play.request == null && play.error != null && play.error.startsWith(LaunchRequest.ERROR_CODE_UNLOADABLE_PREFIX), "Play refuses: " + play.error);
            expect(ShipPlay.fileNotUsable(play.error), "the reason is one about a file of the app");
            // The mark is gone, so the menu's next look offers the item that checks every piece by its content.
            expectEquals(ShipReady.Stands.CODE, ShipReady.of(v.bundle, v.files).stands, "the menu's rule now says: the app's own pieces are not in place");
            String sentence = ShipAdvice.startFailure(play.error);
            expectContains(sentence, "Choose Finish Setting Up on this page", "the sentence names the item the controller can choose");
            expect(!sentence.toLowerCase(java.util.Locale.ROOT).contains("close the app"), "and no longer says to close the app: " + sentence);
            ShipFirstRun.Result repaired = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
            expect(repaired.ok(), "Finish Setting Up: " + repaired.error);
            expectEquals(Integer.valueOf(1), Files.getAttribute(code.toPath(), "unix:nlink"), "the game code has one name again");
            expectEquals(null, ShipPlay.prepare(v.bundle, v.files, false).error, "Play works again");
            // The two reasons that are about the device at this moment, and Android's own refusal, leave the mark alone.
            expect(!ShipPlay.fileNotUsable(LaunchRequest.ERROR_SETTINGS_DIRECTORY) && !ShipPlay.fileNotUsable(LaunchRequest.ERROR_IO_PREFIX + "IOException)")
                    && !ShipPlay.fileNotUsable(LaunchAdvice.cannotOpen("game window", "SecurityException")) && !ShipPlay.fileNotUsable(null),
                    "a failed read or write is not taken as a file of the app that cannot be used");
            expect(ShipPlay.fileNotUsable(LaunchRequest.ERROR_CODE_MISSING) && ShipPlay.fileNotUsable(LaunchRequest.ERROR_SHADERS_MISSING), "a missing file is");
        });

        check("a developer build (no game code inside the APK) starts as before, 'start anyway' included", () -> {
            ShipTestKit.Version v = freshInstall("developer");
            expectEquals(null, ShipPlay.prepare((ShipBundle) null, v.files, false).error, "the qualified code starts");
            File code = new File(v.files, "private-trace/private-trace-live.so");
            Files.setPosixFilePermissions(code.toPath(), PosixFilePermissions.fromString("rw-------"));
            Files.write(code.toPath(), library(280000, 62));   // a measurement build left in place
            Files.setPosixFilePermissions(code.toPath(), PosixFilePermissions.fromString("r--------"));
            expectEquals(LaunchRequest.UNQUALIFIED, ShipPlay.prepare((ShipBundle) null, v.files, false).error, "the launcher asks first");
            ShipPlay.Result anyway = ShipPlay.prepare((ShipBundle) null, v.files, true);
            expectEquals(null, anyway.error, "and starts it after the answer");
            expectEquals(sha256(library(280000, 62)), anyway.request.expectedSha256, "with the hash of the code that is there");
            expect(new File(v.files, "settings/ship-installed.txt").exists(), "the mark is left alone");
        });

        // ---- repair of 2026-10-05 ------------------------------------------------------------------------------

        check("pinned game data that is missing or is not the pinned file: Play refuses and creates nothing, although every file the start request names is there", () -> {
            // a boot module is gone: the start request names only the directory modules/boot, which is still there
            ShipTestKit.Version v = freshInstall("data-gone");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            File module = new File(v.files, "private-trace/modules/boot/store/dataflow.spv");
            Files.delete(module.toPath());
            expect(InstallState.detect(v.files).ready(), "the launcher's own look at the start request still says ready");
            int runs = runDirectories(v.files);
            ShipPlay.Result play = ShipPlay.prepare(v.bundle, v.files, false);
            expectEquals("your game data is not complete: 1 of 7 game data files are missing", play.error, "the reason");
            expect(play.request == null, "no request");
            expectEquals(runs, runDirectories(v.files), "no run directory was created");
            expectEquals(null, ShipPlay.prepare(v.bundle, v.files, true).request, "not with 'start anyway' either");
            // put back with the pinned size but other bytes: a small file is compared by its SHA-256
            byte[] other = data.get("modules/boot/store/dataflow.spv").clone();
            other[7] ^= 1;
            ShipTestKit.write(module, other);
            expectEquals("your game data is not complete: 1 of 7 game data files are missing or are not the files this app version was"
                    + " made for", ShipPlay.prepare(v.bundle, v.files, false).error, "other content under a pinned name");
            ShipTestKit.write(module, data.get("modules/boot/store/dataflow.spv"));
            expectEquals(null, ShipPlay.prepare(v.bundle, v.files, false).error, "with the pinned file back, Play prepares a start");
            // an input with another size (a copy that was cut off under its final name by something else)
            File input = new File(v.files, "private-trace/" + ShipTestKit.Version.inputPath(v.inputs.get(2)));
            overwrite(input, Arrays.copyOf(v.inputs.get(2), v.inputs.get(2).length - 1));
            expect(InstallState.detect(v.files).ready(), "the launcher's own look does not see a size");
            expectEquals("your game data is not complete: 1 of 7 game data files are missing or are not the files this app version was"
                    + " made for", ShipPlay.prepare(v.bundle, v.files, false).error, "another size under a pinned name");
        });

        check("an update whose start request has another run id: Play starts the new game code with the new start request", () -> {
            for (boolean imageInApk : new boolean[] {true, false}) {
                ShipTestKit.Version v = imageInApk ? freshInstallWithPieces("update-run-a") : freshInstall("update-run-b");
                ShipPlay.Result played = ShipPlay.prepare(v.bundle, v.files, false);   // version 1 was played once
                expectEquals(null, played.error, "version 1 plays (image in the APK: " + imageInApk + ")");
                v.build = "t2";
                v.library = library(310000, 81);
                v.runId = "fedcba9876543210fedcba9876543210";
                v.directories.add("inputs/native-update");
                v.build();
                expectEquals("this app version's game code is not in place yet; close the app and open it again",
                        ShipPlay.prepare(v.bundle, v.files, false).error, "before the update's first start Play refuses");
                ShipFirstRun.Result update = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
                expect(update.ok(), "the update's first start: " + update.error);
                expectEquals(null, ShipDataImport.missing(v.config(), v.files), "the game data is still complete: no folder is asked for again");
                InstallState install = InstallState.detect(v.files);
                expectEquals(v.runId, install.templateRunId, "the run the launcher copies start requests from");
                expect(!install.pinIgnored, "the pin is used");
                ShipPlay.Result play = ShipPlay.prepare(v.bundle, v.files, false);
                expectEquals(null, play.error, "Play after the update");
                expectEquals(sha256(v.library), play.request.expectedSha256, "version 2's game code");
                byte[] arguments = read(new File(v.files, "private-trace/run-" + play.request.requestId + "/arguments.bin"));
                expect(Arrays.equals(v.requestFor(play.request.requestId), arguments), "the arguments are version 2's bundled start request");
                expect(new String(arguments, java.nio.charset.StandardCharsets.ISO_8859_1).contains("inputs/native-update"),
                        "they name the directory that is new in version 2");
            }
        });

        check("the shader pack and the bundled game data are compared with their SHA-256 at every Play; the next start puts them back", () -> {
            ShipTestKit.Version v = freshInstallWithPieces("pack");
            File pack = new File(v.files, "private-trace/generic-shaders-v41.bin");
            byte[] other = v.pack.clone();
            other[other.length / 2] ^= 1;   // same size, other content
            overwrite(pack, other);
            expectEquals(ShipFirstRun.State.CURRENT, ShipFirstRun.check(v.bundle, v.files), "the cheap check of an app start sees sizes only");
            int runs = runDirectories(v.files);
            ShipPlay.Result play = ShipPlay.prepare(v.bundle, v.files, true);
            expectEquals("a file this app version carries is missing or was changed on this device; close the app and open it again"
                    + " to put it back", play.error, "the reason for a changed shader pack");
            expectEquals(runs, runDirectories(v.files), "no run directory was created");
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "the next start has work to do");
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "the next start");
            expectEquals(sha256(v.pack), sha256(read(pack)), "the bundled shader pack is back");
            expectEquals(null, ShipPlay.prepare(v.bundle, v.files, false).error, "Play works again");
            // a bundled boot module with other bytes of the same size
            File module = new File(v.files, "private-trace/modules/boot/r8/vertex/dataflow.spv");
            byte[] changed = read(module);
            changed[0] ^= 1;
            overwrite(module, changed);
            expect(InstallState.detect(v.files).ready(), "the launcher's own look still says ready");
            expectEquals("a file this app version carries is missing or was changed on this device; close the app and open it again"
                    + " to put it back", ShipPlay.prepare(v.bundle, v.files, false).error, "the reason for a changed bundled piece");
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "after that refusal the next start has work to do");
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "the next start");
            expectEquals(sha256(ShipTestKit.dataOf(v).get("modules/boot/r8/vertex/dataflow.spv")), sha256(read(module)), "the bundled module is back");
            expectEquals(null, ShipPlay.prepare(v.bundle, v.files, false).error, "Play works again");
        });

        check("the start request beside the pin is compared by content: one of the same size with other bytes is not this version's", () -> {
            ShipTestKit.Version v = freshInstall("request-bytes");
            File arguments = new File(v.files, "private-trace/run-" + v.runId + "/arguments.bin");
            byte[] other = v.request.clone();
            int digit = new String(other, java.nio.charset.StandardCharsets.ISO_8859_1).indexOf("2181038080");
            other[digit] = '3';   // another number in an argument: the size and the shape stay, the launcher would copy it
            overwrite(arguments, other);
            expect(InstallState.detect(v.files).ready(), "the launcher's own look says ready");
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "the check of every app start");
            int runs = runDirectories(v.files);
            expectEquals("this app version's game code is not in place yet; close the app and open it again",
                    ShipPlay.prepare(v.bundle, v.files, false).error, "Play's reason");
            expectEquals(runs, runDirectories(v.files), "no run directory was created");
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "the next start");
            expect(Arrays.equals(v.request, read(arguments)), "the bundled start request is back");
            expectEquals(null, ShipPlay.prepare(v.bundle, v.files, false).error, "Play works again");
        });

        check("the app's own file is not known: Play refuses, it does not treat the build as a developer build", () -> {
            ShipTestKit.Version v = freshInstall("no-apk");
            File code = new File(v.files, "private-trace/private-trace-live.so");
            overwrite(code, library(280000, 62));   // game code that is not the bundled one
            int runs = runDirectories(v.files);
            for (boolean accept : new boolean[] {true, false}) {
                ShipPlay.Result play = ShipPlay.prepare((String) null, v.files, accept);
                expectEquals("this app cannot find its own file; install the app again", play.error, "the reason (start anyway " + accept + ")");
                expect(play.request == null, "no request");
            }
            expectEquals(runs, runDirectories(v.files), "no run directory was created");
            expectContains(ShipPlay.prepare(new File(v.files, "no-such.apk").getPath(), v.files, true).error,
                    "this app's own file cannot be read", "a path that cannot be opened");
        });

        check("the launcher would copy another run's start request than the bundled one: Play refuses", () -> {
            ShipTestKit.Version v = freshInstall("other-run");
            expectEquals(null, ShipPlay.prepare(v.bundle, v.files, false).error, "a first Play (it leaves a launcher-made run behind)");
            // the pinned run's directory is replaced by a link to it: the files are still reached through the link,
            // but the launcher's detection does not accept a linked run and falls back to the newest other run
            File trace = new File(v.files, "private-trace");
            File run = new File(trace, "run-" + v.runId), moved = new File(trace, "moved-away");
            Files.move(run.toPath(), moved.toPath());
            Files.createSymbolicLink(run.toPath(), moved.toPath());
            InstallState install = InstallState.detect(v.files);
            expect(install.ready() && install.pinIgnored, "the launcher's own look says ready with the pin ignored");
            int runs = runDirectories(v.files);
            ShipPlay.Result play = ShipPlay.prepare(v.bundle, v.files, false);
            // The ship build's own rule does not follow a link under the run's name either (ShipFiles.regularFileIn), so
            // the refusal comes one step before the question which run the launcher would copy: the app's pieces are
            // not in place. Either way nothing is started from the other run.
            expectEquals(ShipFirstRun.NOT_INSTALLED, play.error, "the reason");
            expect(play.request == null, "no start was prepared");
            expectEquals(runs, runDirectories(v.files), "no run directory was created");
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "the next start has work to do");
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "and does it");
            expect(Files.isDirectory(run.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS), "a real folder holds the start request again");
            expectEquals(null, ShipPlay.prepare(v.bundle, v.files, false).error, "and Play prepares a start");
        });

        ShipTestKit.finish("ShipPlayTest");
    }
}
