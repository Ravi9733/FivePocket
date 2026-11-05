package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.bytes;
import static dev.recompbootstrap.smoke.ShipTestKit.changes;
import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectContains;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;
import static dev.recompbootstrap.smoke.ShipTestKit.library;
import static dev.recompbootstrap.smoke.ShipTestKit.mode;
import static dev.recompbootstrap.smoke.ShipTestKit.read;
import static dev.recompbootstrap.smoke.ShipTestKit.sha256;
import static dev.recompbootstrap.smoke.ShipTestKit.snapshot;
import static dev.recompbootstrap.smoke.ShipTestKit.text;
import static dev.recompbootstrap.smoke.ShipTestKit.write;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.TreeMap;

/**
 * ShipFirstRun against made-up app versions (ShipTestKit.Version) in temporary files directories. Run with java -ea.
 * Expected names, modes and contents are written out here: they are the contract with the game window's loader
 * (private_trace_jni.cpp) and with the launcher (InstallState, LaunchRequest), not values taken from ShipFirstRun.
 */
public final class ShipFirstRunTest {
    private static final String ID = "0123456789abcdef0123456789abcdef";

    private static ShipFirstRun.Result run(ShipTestKit.Version version) {
        return ShipFirstRun.run(version.bundle, version.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
    }

    /** Everything a complete install of version must show, name by name. */
    private static void expectInstalled(ShipTestKit.Version version) throws IOException {
        File trace = new File(version.files, "private-trace");
        expectEquals("rwx------", mode(trace), "mode of private-trace (the loader accepts only 0700)");
        File code = new File(trace, "private-trace-live.so");
        expectEquals(sha256(version.library), sha256(read(code)), "bytes of the game code");
        expectEquals("r--------", mode(code), "mode of the game code (the loader accepts only 0400)");
        expectEquals(Integer.valueOf(1), Files.getAttribute(code.toPath(), "unix:nlink"), "names of the game code file");
        File pack = new File(trace, version.shaderPackName);
        expectEquals(sha256(version.pack), sha256(read(pack)), "bytes of the shader pack");
        File run = new File(trace, "run-" + version.runId);
        expectEquals("rwx------", mode(run), "mode of the run directory");
        File arguments = new File(run, "arguments.bin");
        expect(Arrays.equals(version.request, read(arguments)), "the start request is the bundled one, byte for byte");
        expectEquals("r--------", mode(arguments), "mode of the start request");
        expectEquals(version.runId + "\n", text(new File(version.files, "settings/launch-template.txt")), "the pin of the start request");
        expectEquals(sha256(version.library) + "\n", text(new File(version.files, "settings/qualified-code.txt")),
                "the record Play compares the game code with");
        for (String directory : version.directories) {
            expect(new File(trace, directory).isDirectory(), directory + " exists");
        }
        expectEquals(sha256(version.record) + "\n", text(new File(version.files, "settings/ship-installed.txt")), "the mark");
        for (String name : trace.list()) expect(!name.endsWith(".ship-part"), "no part file is left: " + name);
        expectEquals(ShipFirstRun.State.CURRENT, ShipFirstRun.check(version.bundle, version.files), "check after the run");
    }

    public static void main(String[] args) {
        check("first start on an empty directory: every bundled piece arrives where the loader and Play look for it", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("first")).build();
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "check before");
            final List<String> steps = new ArrayList<String>();
            final long[] codeBytes = new long[2];
            ShipFirstRun.Result result = ShipFirstRun.run(v.bundle, v.files, new ShipFirstRun.Progress() {
                @Override public void at(ShipFirstRun.Step step, long done, long total) {
                    if (steps.isEmpty() || !steps.get(steps.size() - 1).equals(step.name())) steps.add(step.name());
                    if (step == ShipFirstRun.Step.GAME_CODE) {
                        expect(done >= codeBytes[0], "progress of the game code never goes back");
                        codeBytes[0] = done;
                        codeBytes[1] = total;
                    }
                }
            }, ShipTestKit.PLENTY);
            expect(result.ok(), "ok: " + result.error);
            expectInstalled(v);
            for (String name : v.switches) {
                File file = new File(v.files, "private-trace/" + name);
                if (name.equals("diag-fps-overlay")) {
                    // the record lists it, and the record stands; the app does not create it (the counter is off by default)
                    expect(!file.exists(), "the frame-rate counter's switch file is not created by a fresh install");
                    continue;
                }
                expect(file.isFile() && file.length() == 0, "switch file " + name + " exists and is empty");
            }
            expect(v.switches.contains("diag-fps-overlay"), "(the made-up record does list the counter's switch file)");
            expectEquals("[RECORD, SPACE, GAME_CODE, SHADERS, START_REQUEST, GAME_DATA, SWITCHES, FINISH]", steps.toString(), "steps reported");
            expectEquals((long) v.library.length, codeBytes[0], "game code progress ends at its size");
            expectEquals((long) v.library.length, codeBytes[1], "game code total");
            // nothing but the documented set was created
            TreeMap<String, String> tree = snapshot(v.files);
            List<String> unexpected = new ArrayList<String>();
            List<String> allowed = new ArrayList<String>(Arrays.asList("private-trace", "private-trace/private-trace-live.so",
                    "private-trace/generic-shaders-v41.bin", "private-trace/run-" + ID, "private-trace/run-" + ID + "/arguments.bin",
                    "private-trace/inputs", "private-trace/inputs/native-cache", "private-trace/modules", "private-trace/modules/boot",
                    "settings", "settings/launch-template.txt", "settings/qualified-code.txt", "settings/ship-installed.txt"));
            for (String name : v.switches) if (!name.equals("diag-fps-overlay")) allowed.add("private-trace/" + name);
            for (String path : tree.keySet()) if (!allowed.contains(path)) unexpected.add(path);
            expectEquals("[]", unexpected.toString(), "paths created beyond the documented ones");
            expectEquals(allowed.size(), tree.size(), "every documented path exists");
        });

        check("second run: nothing is rewritten, replaced or created, and no bundled piece is copied again", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("second")).build();
            expect(run(v).ok(), "first run");
            TreeMap<String, String> before = snapshot(v.files);
            v.bundle.opened.clear();
            ShipFirstRun.Result again = run(v);
            expect(again.ok(), "second run: " + again.error);
            expectEquals("[]", changes(before, snapshot(v.files)).toString(), "changed by the second run");
            expectEquals("[]", again.changed.toString(), "what the second run says it changed");
            expect(!v.bundle.opened.contains("ship/game-library.bin"), "the game code is not read out of the APK again");
        });

        check("interrupted at any point: the next start finishes the work and ends in the same state", () -> {
            final ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("interrupt"));
            v.library = library((3 << 20) + 500, 5);   // several copy blocks, so that pictures are taken mid-copy
            v.pack = bytes((2 << 20) + 9, 6);
            v.build();
            // A picture of the files directory at every progress report: what a process that died there leaves.
            final File pictures = new File(v.files.getParentFile(), "pictures");
            expect(pictures.mkdir(), "pictures folder");
            final List<String> taken = new ArrayList<String>();
            ShipFirstRun.Result whole = ShipFirstRun.run(v.bundle, v.files, new ShipFirstRun.Progress() {
                @Override public void at(ShipFirstRun.Step step, long done, long total) {
                    String label = taken.size() + "-" + step + "-" + done;
                    try {
                        File picture = new File(pictures, label);
                        expect(picture.mkdir(), "picture " + label);
                        ShipTestKit.copyTree(v.files, picture);
                        taken.add(label);
                    } catch (IOException e) {
                        throw new IllegalStateException(e);
                    }
                }
            }, ShipTestKit.PLENTY);
            expect(whole.ok(), "uninterrupted run: " + whole.error);
            TreeMap<String, String> complete = snapshot(v.files, false);
            boolean partSeen = false, stepsSeen = taken.size() >= 10;
            expect(stepsSeen, "pictures at every step and inside the copies: " + taken.size());
            for (String label : taken) {
                ShipTestKit.emptyDirectory(v.files);
                ShipTestKit.copyTree(new File(pictures, label), v.files);
                TreeMap<String, String> died = snapshot(v.files, false);
                for (String path : died.keySet()) partSeen |= path.endsWith(".ship-part");
                if (!label.contains("-FINISH-")) {
                    expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "after dying at " + label + ", check");
                }
                ShipFirstRun.Result resumed = run(v);
                expect(resumed.ok(), "resumed after " + label + ": " + resumed.error);
                expectEquals(complete.toString(), snapshot(v.files, false).toString(), "state after resuming from " + label);
                expectInstalled(v);
            }
            expect(partSeen, "at least one picture held a half-written part file");
        });

        check("wrong hash: a bundled piece that is not the recorded one is never installed under its name", () -> {
            for (String asset : new String[] {"ship/game-library.bin", "ship/shader-pack.bin", "ship/start-request.bin"}) {
                ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("hash")).build();
                byte[] damaged = v.bundle.assets.get(asset).clone();
                damaged[damaged.length / 2] ^= 1;   // same size, one bit
                v.bundle.assets.put(asset, damaged);
                ShipFirstRun.Result result = run(v);
                expect(!result.ok(), asset + ": the run fails");
                expectContains(result.error, "does not match its recorded SHA-256", asset + ": the sentence");
                expectContains(result.error, "Install the app again", asset + ": what to do");
                String target = asset.contains("library") ? "private-trace/private-trace-live.so"
                        : asset.contains("shader") ? "private-trace/generic-shaders-v41.bin" : "private-trace/run-" + ID + "/arguments.bin";
                expect(!new File(v.files, target).exists(), asset + ": nothing stands under " + target);
                expect(!new File(v.files, "settings/ship-installed.txt").exists(), asset + ": no mark");
                File trace = new File(v.files, "private-trace");
                String[] left = trace.isDirectory() ? trace.list() : new String[0];
                for (String name : left) expect(!name.endsWith(".ship-part"), asset + ": the damaged copy was removed: " + name);
                expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), asset + ": check");
            }
            // a piece that is shorter or longer than recorded is told apart from one with other content
            ShipTestKit.Version shorter = new ShipTestKit.Version(ShipTestKit.newFilesDir("short")).build();
            shorter.bundle.assets.put("ship/game-library.bin", Arrays.copyOf(shorter.library, shorter.library.length - 1));
            expectContains(run(shorter).error, "is shorter than recorded", "a cut-off game code");
            ShipTestKit.Version longer = new ShipTestKit.Version(ShipTestKit.newFilesDir("long")).build();
            longer.bundle.assets.put("ship/shader-pack.bin", Arrays.copyOf(longer.pack, longer.pack.length + 1));
            expectContains(run(longer).error, "is longer than recorded", "a shader pack with a byte too many");
        });

        check("no space: refused with both numbers before anything is written; pieces in place need none", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("space")).build();
            long need = v.library.length + v.pack.length + (64L << 20);
            ShipFirstRun.Result tight = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.spaceOf(need - 1));
            expect(!tight.ok(), "one byte short fails");
            // (the next step follows the first screen since the ship integration: it offers "Try again")
            expectEquals("Not enough free space for the game code: it needs 68 MB and 67 MB is free. Free some space,"
                    + " then try again.", tight.error, "the sentence");
            expectEquals("{}", snapshot(v.files).toString(), "nothing was written");
            ShipFirstRun.Result exact = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.spaceOf(need));
            expect(exact.ok(), "exactly enough succeeds: " + exact.error);
            // an update that changes only the shader pack needs room for the shader pack alone
            v.pack = bytes(200000, 44);
            v.build();
            ShipFirstRun.Result update = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.spaceOf(v.pack.length + (64L << 20)));
            expect(update.ok(), "room for the changed piece is enough: " + update.error);
            ShipFirstRun.Result none = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.spaceOf(0));
            expect(none.ok(), "with everything in place no space is asked for: " + none.error);
        });

        check("an existing phone: saves, profile, game data and the user's own switch states are not touched", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("existing")).build();
            File f = v.files;
            write(new File(f, "saves/c/e0000000/00000001/sgta50015"), bytes(270352, 70));
            write(new File(f, "saves/c/e0000000/00000001/meta.bin"), bytes(336, 71));
            write(new File(f, "profile/identity.txt"), bytes(71, 72));
            write(new File(f, "saves-sandbox/promo/data"), bytes(900, 73));
            write(new File(f, "private-trace/inputs/" + sha256(bytes(5000, 74)) + ".bin"), bytes(5000, 74));
            write(new File(f, "private-trace/diag-census-points"), "640,360;100,100\n".getBytes(StandardCharsets.US_ASCII));
            write(new File(f, "private-trace/diag-direct-present"), "kept".getBytes(StandardCharsets.US_ASCII));
            write(new File(f, "private-trace/off.diag-cexec-bools"), new byte[0]);
            write(new File(f, "private-trace/private-trace-live-qualified-older.so"), library(4000, 75));
            write(new File(f, "private-trace/private-trace-live.so"), library(250000, 76));   // another build's game code
            write(new File(f, "private-trace/generic-shaders-v41.bin"), bytes(120000, 77));    // another pack
            write(new File(f, "settings/settings.json"), "{\"version\":1}\n".getBytes(StandardCharsets.US_ASCII));
            write(new File(f, "settings/launcher-runs.txt"), (ID.replace('0', '9') + "\n").getBytes(StandardCharsets.US_ASCII));
            TreeMap<String, String> before = snapshot(f);
            ShipFirstRun.Result result = run(v);
            expect(result.ok(), "ok: " + result.error);
            expectInstalled(v);
            List<String> changed = changes(before, snapshot(f));
            List<String> expected = new ArrayList<String>(Arrays.asList(
                    "private-trace",                                   // its mode becomes 0700
                    "private-trace/private-trace-live.so", "private-trace/generic-shaders-v41.bin",
                    "private-trace/run-" + ID, "private-trace/run-" + ID + "/arguments.bin",
                    "private-trace/inputs/native-cache", "private-trace/modules", "private-trace/modules/boot",
                    "private-trace/diag-v260-write-watch",
                    "settings/launch-template.txt", "settings/qualified-code.txt", "settings/ship-installed.txt"));
            java.util.Collections.sort(changed);
            java.util.Collections.sort(expected);
            expectEquals(expected.toString(), changed.toString(), "everything the run created or replaced");
            expectEquals("kept", text(new File(f, "private-trace/diag-direct-present")), "a switch file that exists keeps its content");
            expect(!new File(f, "private-trace/diag-fps-overlay").exists(),
                    "a switch the settings screen owns is not created once the user has saved settings");
        });

        check("the frame-rate counter is off unless the player saved it on: not created, removed by an update of a never-saved install, kept once saved", () -> {
            final String counter = "private-trace/diag-fps-overlay", saved = "settings/settings.json";
            // 1. A fresh install: no counter; the Settings tab's own loader, which reads the files, shows it Off.
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("counter")).build();
            expect(run(v).ok(), "first start");
            expect(!new File(v.files, counter).exists(), "a fresh install: no switch file for the counter");
            expect(new File(v.files, "private-trace/diag-direct-present").isFile() && new File(v.files, "private-trace/diag-v260-write-watch").isFile(),
                    "the other switch files of the record are created as before");
            SettingsStore store = new SettingsStore(v.files, () -> false);
            Capabilities caps = SettingsCoreTestKit.v464();
            expectEquals(0, store.load(caps).settings.get(SettingsSchema.FPS_OVERLAY), "Settings, Frame-Rate Counter, as the tab's own loader reads the install: Off");
            // 2. The install an earlier app version made (it created the file), settings never saved; then an update.
            write(new File(v.files, counter), new byte[0]);
            expectEquals(1, store.load(caps).settings.get(SettingsSchema.FPS_OVERLAY), "(with the file the tab reads On: the earlier default)");
            v.build = "t2";
            v.library = library(310000, 82);
            v.build();
            ShipFirstRun.Result update = run(v);
            expect(update.ok(), "the update: " + update.error);
            expect(!new File(v.files, counter).exists(), "an update of an install whose settings were never saved ends with the counter off");
            expect(update.changed.contains("-" + counter), "and the run says that it removed the file: " + update.changed);
            expect(new File(v.files, "private-trace/diag-direct-present").isFile(), "no other switch file is removed");
            // 3. The same without an update (an app update that carries the same record does no full work).
            write(new File(v.files, counter), new byte[0]);
            expect(ShipFirstRun.counterOffUnlessSaved(v.files), "the start's own step removes it");
            expect(!new File(v.files, counter).exists(), "gone");
            expect(!ShipFirstRun.counterOffUnlessSaved(v.files), "nothing to remove the second time");
            // 4. The player turns the counter on in Settings and saves: from then on it is the player's, On ...
            SettingsStore.SaveResult on = store.save(store.load(caps).settings.with(SettingsSchema.FPS_OVERLAY, 1), caps);
            expect(on.ok, "the save");
            expect(new File(v.files, counter).isFile() && new File(v.files, saved).isFile(), "the save wrote the switch file and the settings");
            expect(!ShipFirstRun.counterOffUnlessSaved(v.files), "saved On: the start leaves it");
            v.build = "t3";
            v.library = library(310000, 83);
            v.build();
            expect(run(v).ok(), "another update");
            expect(new File(v.files, counter).isFile(), "saved On stays On through an update");
            // ... and Off: saved Off stays Off (the run does not create it).
            expect(store.save(store.load(caps).settings.with(SettingsSchema.FPS_OVERLAY, 0), caps).ok, "saved Off");
            v.build = "t4";
            v.library = library(310000, 84);
            v.build();
            expect(run(v).ok(), "a third update");
            expect(!new File(v.files, counter).exists(), "saved Off stays Off through an update");
            // 5. Only a regular file under that one name is removed: a folder and a link are left alone.
            ShipTestKit.Version other = new ShipTestKit.Version(ShipTestKit.newFilesDir("counter-other")).build();
            expect(run(other).ok(), "first start");
            File folder = new File(other.files, counter);
            write(new File(folder, "kept.txt"), new byte[] {1});
            expect(!ShipFirstRun.counterOffUnlessSaved(other.files) && new File(folder, "kept.txt").isFile(), "a folder under the name is not touched");
            ShipTestKit.Version linked = new ShipTestKit.Version(ShipTestKit.newFilesDir("counter-link")).build();
            expect(run(linked).ok(), "first start");
            File target = new File(linked.files, "private-trace/diag-direct-present");
            Files.createSymbolicLink(new File(linked.files, counter).toPath(), target.toPath());
            expect(!ShipFirstRun.counterOffUnlessSaved(linked.files), "a link under the name is not removed");
            expect(target.isFile() && Files.isSymbolicLink(new File(linked.files, counter).toPath()), "neither the link nor its target");
        });

        check("an app update: changed pieces are replaced, the mark follows, a new switch appears", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("update")).build();
            expect(run(v).ok(), "version 1");
            v.build = "t2";
            v.library = library(310000, 81);
            v.switches.add("diag-new-in-t2");
            v.build();
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "check sees the update");
            ShipFirstRun.Result result = run(v);
            expect(result.ok(), "version 2: " + result.error);
            expectInstalled(v);
            expect(new File(v.files, "private-trace/diag-new-in-t2").isFile(), "the switch new in this version exists");
            // an update that changes nothing but the list of switch files: only the mark can tell
            v.build = "t3";
            v.switches.add("diag-new-in-t3");
            v.build();
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "check sees an update of the record alone");
            expect(run(v).ok(), "version 3");
            expect(new File(v.files, "private-trace/diag-new-in-t3").isFile(), "the switch new in version 3 exists");
            expectInstalled(v);
        });

        check("game code in place that the loader would refuse is made acceptable without a second copy when possible", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("mode")).build();
            File code = new File(v.files, "private-trace/private-trace-live.so");
            write(code, v.library);   // right bytes, mode rw-r--r--
            v.bundle.opened.clear();
            expect(run(v).ok(), "run over a writable copy");
            expectEquals("r--------", mode(code), "mode after the run");
            expect(!v.bundle.opened.contains("ship/game-library.bin"), "the right bytes were kept, not copied again");
            // a second name for the file (the loader refuses that): replaced by a fresh copy with one name
            ShipFirstRun.forget(v.files);
            Files.createLink(new File(v.files, "private-trace/second-name.so").toPath(), code.toPath());
            expect(run(v).ok(), "run over a file with two names");
            expectEquals(Integer.valueOf(1), Files.getAttribute(code.toPath(), "unix:nlink"), "names of the game code file");
        });

        check("a bundled library that is not a 64-bit ARM library is reported in words, not started", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("elf"));
            v.library = bytes(300000, 91);   // no program header at all
            v.build();
            ShipFirstRun.Result result = run(v);
            expectEquals("The game code inside this app is not usable: it is not a 64-bit program file.", result.error, "the sentence");
            expect(!new File(v.files, "private-trace/private-trace-live.so").exists(), "it did not get the game code's name");
            expect(!new File(v.files, "settings/ship-installed.txt").exists(), "no mark");
            ShipTestKit.Version x86 = new ShipTestKit.Version(ShipTestKit.newFilesDir("elf2"));
            x86.library[18] = 0x3e;   // e_machine x86-64
            x86.build();
            expectContains(run(x86).error, "it is not a 64-bit ARM library", "another processor");
        });

        check("another place than the one the game code was built for: refused, nothing written", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("place"));
            v.filesRoot = "/data/user/0/dev.recompbootstrap.smoke/files";
            v.build();
            ShipFirstRun.Result result = run(v);
            expectContains(result.error, "only works for the device's main user", "the sentence");
            expectContains(result.error, "/data/user/0/dev.recompbootstrap.smoke/files", "names the place it was built for");
            expectEquals("{}", snapshot(v.files).toString(), "nothing was written");
        });

        check("an APK without game code (a developer build): nothing is done", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("dev")).build();
            v.bundle.assets.clear();
            expectEquals(ShipFirstRun.State.NOT_BUNDLED, ShipFirstRun.check(v.bundle, v.files), "check");
            ShipFirstRun.Result result = run(v);
            expect(!result.bundled && result.error == null, "not bundled, no error");
            expectEquals("{}", snapshot(v.files).toString(), "nothing was written");
        });

        check("a record that names anything outside files/private-trace is refused whole", () -> {
            String[][] cases = {
                    {"\"diag-direct-present\"", "\"../saves/sgta50015\"", "a switch file name is not allowed"},
                    {"\"diag-direct-present\"", "\"diag-x/../../saves/a\"", "a switch file name is not allowed"},
                    {"\"diag-direct-present\"", "\"private-trace-live.so\"", "a switch file name is not allowed"},
                    {"\"name\":\"generic-shaders-v41.bin\"", "\"name\":\"../../saves/pack\"", "the shader pack's file name is not a plain name"},
                    // the launcher and the game code look for one fixed name: a pack under another would never be found
                    {"\"name\":\"generic-shaders-v41.bin\"", "\"name\":\"generic-shaders-v42.bin\"",
                            "the launcher and the game code look for generic-shaders-v41.bin"},
                    {"\"name\":\"generic-shaders-v41.bin\"", "\"name\":\"private-trace-live.so\"",
                            "the launcher and the game code look for generic-shaders-v41.bin"},
                    {"\"inputs/native-cache\"", "\"inputs/native-cache\",\"../saves\"", "a directory is outside inputs/ and modules/"},
                    {"\"inputs/native-cache\"", "\"inputs/native-cache\",\"inputs/../../saves\"", "a directory is outside inputs/ and modules/"},
                    {"\"asset\":\"ship/game-library.bin\"", "\"asset\":\"../classes.dex\"", "the asset name of library is not ship/(plain name)"},
                    {"\"runId\":\"" + ID + "\"", "\"runId\":\"../../saves\"", "the start request's run id is not 32 hex digits"},
                    {"gta5-ship-config/1", "gta5-ship-config/2", "its format is not gta5-ship-config/1"},
            };
            for (String[] c : cases) {
                ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("record")).build();
                String record = new String(v.record, StandardCharsets.US_ASCII);
                expect(record.contains(c[0]), "the test's record holds " + c[0]);
                v.bundle.assets.put("ship/config.json", record.replace(c[0], c[1]).getBytes(StandardCharsets.US_ASCII));
                ShipFirstRun.Result result = run(v);
                expect(!result.ok(), c[1] + ": refused");
                expectContains(result.error, c[2], c[1] + ": the reason");
                expectEquals("{}", snapshot(v.files).toString(), c[1] + ": nothing was written");
            }
        });

        check("a start request that does not fit its record or this device is refused before anything is written", () -> {
            // it names an input the record does not list
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("request")).build();
            String record = new String(v.record, StandardCharsets.US_ASCII);
            // in the list of required inputs the path is followed by the next input; in the pin by its size
            String listed = "\"" + ShipTestKit.Version.inputPath(v.inputs.get(1)) + "\",\"inputs/";
            expect(record.contains(listed), "the test's record lists the second input");
            v.bundle.assets.put("ship/config.json", record.replace(listed, "\"inputs/").getBytes(StandardCharsets.US_ASCII));
            ShipFirstRun.Result result = run(v);
            expectContains(result.error, "which its record does not list", "an unlisted input");
            expectEquals("{}", snapshot(v.files).toString(), "nothing was written");
            // it was written for another run id than the record names (the game process would refuse it)
            ShipTestKit.Version other = new ShipTestKit.Version(ShipTestKit.newFilesDir("request2")).build();
            byte[] foreign = other.requestFor("ffffffffffffffffffffffffffffffff");
            String fixed = new String(other.record, StandardCharsets.US_ASCII).replace(sha256(other.request), sha256(foreign));
            other.bundle.assets.put("ship/start-request.bin", foreign);
            other.bundle.assets.put("ship/config.json", fixed.getBytes(StandardCharsets.US_ASCII));
            expectContains(run(other).error, "The start request inside this app does not fit this device", "a request for another run");
            expectEquals("{}", snapshot(other.files).toString(), "nothing was written");
        });

        // ---- repair of 2026-10-05: bundled game data, left-over part files, an update with another run id ----------

        check("bundled game data: the pieces inside the APK arrive in the game's layout, verified, the image beside the start request", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("pieces"));
            v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            v.bundledPaths.add(ShipTestKit.Version.inputPath(v.inputs.get(1)));   // a small catalog, under its SHA-256
            v.build();
            java.util.Map<String, byte[]> data = ShipTestKit.dataOf(v);
            final List<String> dataSteps = new ArrayList<String>();
            ShipFirstRun.Result result = ShipFirstRun.run(v.bundle, v.files, new ShipFirstRun.Progress() {
                @Override public void at(ShipFirstRun.Step step, long done, long total) {
                    if (step == ShipFirstRun.Step.GAME_DATA) dataSteps.add(done + "/" + total);
                }
            }, ShipTestKit.PLENTY);
            expect(result.ok(), "ok: " + result.error);
            expectInstalled(v);
            long bundledBytes = 0;
            for (String path : v.bundledPaths) {
                File file = new File(v.files, "private-trace/" + (path.equals("decoded.image.bin") ? "run-" + ID + "/decoded.image.bin" : path));
                expect(file.isFile(), path + " is in place");
                expectEquals(sha256(data.get(path)), sha256(read(file)), "bytes of " + path);
                expectEquals("r--------", mode(file), "mode of " + path);
                bundledBytes += data.get(path).length;
                expect(result.changed.contains(v.files.toPath().relativize(file.toPath()).toString()), path + " is among what the run says it created");
            }
            expectEquals("0/" + bundledBytes, dataSteps.get(0), "the game data step starts at nothing of the pieces' bytes");
            expectEquals(bundledBytes + "/" + bundledBytes, dataSteps.get(dataSteps.size() - 1), "and ends at all of them");
            // one line per bundled piece for the log: the three fixed pieces first, then the game data, the image last
            expectEquals(3 + v.bundledPaths.size(), result.report.size(), "report lines");
            expectEquals("private-trace/private-trace-live.so bytes=" + v.library.length + " sha256=" + sha256(v.library).substring(0, 12)
                    + " copied", result.report.get(0), "the game code's line");
            expectEquals("private-trace/run-" + ID + "/decoded.image.bin bytes=500000 sha256="
                    + sha256(data.get("decoded.image.bin")).substring(0, 12) + " copied", result.report.get(result.report.size() - 1),
                    "the image's line is the last");
            // the files that do not travel in the APK were not made up
            for (String path : data.keySet()) {
                if (!v.bundledPaths.contains(path)) expect(!new File(v.files, "private-trace/" + path).exists(), path + " is not there");
            }
            // a second run copies none of them again and says so
            ShipFirstRun.forget(v.files);
            TreeMap<String, String> before = snapshot(v.files);
            v.bundle.opened.clear();
            ShipFirstRun.Result again = run(v);
            expect(again.ok(), "second run: " + again.error);
            for (String opened : v.bundle.opened) expect(!opened.startsWith("ship/pieces/"), "a piece in place is not read out of the APK again: " + opened);
            expectEquals("[settings/ship-installed.txt]", changes(before, snapshot(v.files)).toString(), "changed by the second run");
            expect(again.report.get(again.report.size() - 1).endsWith(" kept"), "the report says kept: " + again.report);
        });

        check("bundled game data that is damaged: inside the APK it is refused, on the device it is put back", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("piece-damage"));
            v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            v.build();
            String module = "modules/boot/store/dataflow.spv";
            byte[] right = v.bundle.assets.get("ship/pieces/" + module);
            byte[] wrong = right.clone();
            wrong[100] ^= 1;
            v.bundle.assets.put("ship/pieces/" + module, wrong);
            ShipFirstRun.Result refused = run(v);
            expectEquals("This app's file is damaged: the game data file " + module + " inside it does not match its recorded SHA-256."
                    + " Install the app again.", refused.error, "the sentence for a damaged piece");
            expect(!new File(v.files, "private-trace/" + module).exists(), "the damaged piece has no final name");
            expect(!new File(v.files, "settings/ship-installed.txt").exists(), "no mark");
            v.bundle.assets.remove("ship/pieces/" + module);
            expectContains(run(v).error, "the game data file " + module + " inside it is missing", "a piece the APK does not hold");
            // the right APK over a device that holds other bytes under a piece's name (same size): replaced
            v.bundle.assets.put("ship/pieces/" + module, right);
            write(new File(v.files, "private-trace/" + module), wrong);
            ShipFirstRun.Result repaired = run(v);
            expect(repaired.ok(), "ok: " + repaired.error);
            expectEquals(sha256(right), sha256(read(new File(v.files, "private-trace/" + module))), "the bundled bytes are in place");
            // a piece that disappears afterwards is seen by the cheap check of every app start
            expectEquals(ShipFirstRun.State.CURRENT, ShipFirstRun.check(v.bundle, v.files), "check with everything in place");
            Files.delete(new File(v.files, "private-trace/" + module).toPath());
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "check after a bundled piece was removed");
        });

        check("a first start that was killed inside a copy: its own part file does not make the next start refuse for space", () -> {
            final ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("left-over"));
            v.library = library((3 << 20) + 500, 5);
            v.pack = bytes((2 << 20) + 9, 6);
            v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            v.build();
            long pieces = 0;
            for (String path : v.bundledPaths) pieces += v.bundle.assets.get("ship/pieces/" + path).length;
            // a disk on which the first start just fits: the pieces, the reserve and 4 KiB to spare. Every left-over
            // below is larger than that, so each of them alone makes the start refuse when it is not removed first.
            final long capacity = v.library.length + v.pack.length + pieces + (64L << 20) + (4 << 10);
            // what a process that died inside each copy leaves: a part file beside the target
            write(new File(v.files, "private-trace/private-trace-live.so.ship-part"), bytes(2 << 20, 90));
            write(new File(v.files, "private-trace/generic-shaders-v41.bin.ship-part"), bytes(1 << 20, 91));
            write(new File(v.files, "private-trace/run-" + ID + "/decoded.image.bin.ship-part"), bytes(300000, 92));
            write(new File(v.files, "private-trace/modules/boot/store/dataflow.spv.ship-part"), bytes(200000, 93));
            write(new File(v.files, "settings/ship-installed.txt.ship-part"), bytes(10, 94));
            // files that only look similar are not this class's and stay
            write(new File(v.files, "private-trace/inputs/native-cache/kept.ship-part"), bytes(100, 95));
            write(new File(v.files, "saves/slot-0/save.ship-part"), bytes(100, 96));
            write(new File(v.files, "private-trace/diag-census-points"), bytes(16, 97));
            ShipFirstRun.Result result = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.diskOf(v.files, capacity));
            expect(result.ok(), "the start on the tight disk with left-over part files: " + result.error);
            expectInstalled(v);
            // each place on its own: the same disk, one left-over, larger than what is to spare
            for (String place : new String[] {"private-trace/modules/boot/depth/vertex/guest-code.bin.ship-part",
                    "private-trace/run-" + ID + "/decoded.image.bin.ship-part", "private-trace/inputs/x.ship-part"}) {
                ShipTestKit.Version one = new ShipTestKit.Version(ShipTestKit.newFilesDir("left-over-one"));
                one.library = v.library;
                one.pack = v.pack;
                one.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
                one.build();
                write(new File(one.files, place), bytes(100000, 99));
                ShipFirstRun.Result alone = ShipFirstRun.run(one.bundle, one.files, ShipTestKit.QUIET, ShipTestKit.diskOf(one.files, capacity));
                expect(alone.ok(), "a left-over at " + place + " is removed before the free space is asked for: " + alone.error);
                expect(!new File(one.files, place).exists(), "and is gone: " + place);
            }
            for (String left : new String[] {"private-trace/private-trace-live.so.ship-part", "private-trace/generic-shaders-v41.bin.ship-part",
                    "private-trace/run-" + ID + "/decoded.image.bin.ship-part", "private-trace/modules/boot/store/dataflow.spv.ship-part",
                    "settings/ship-installed.txt.ship-part"}) {
                expect(!new File(v.files, left).exists(), "the left-over part file is gone: " + left);
            }
            expect(new File(v.files, "private-trace/inputs/native-cache/kept.ship-part").isFile(), "a file in the game's own cache folder stays");
            expect(new File(v.files, "saves/slot-0/save.ship-part").isFile(), "a file under saves stays");
            File census = new File(v.files, "private-trace/diag-census-points");
            expect(census.isFile() && sha256(bytes(16, 97)).equals(sha256(read(census))),
                    "a file beside the game code that is no part file stays as it was");
            // the disk model is honest: without room the same start is refused, and the numbers are the disk's
            ShipTestKit.Version tight = new ShipTestKit.Version(ShipTestKit.newFilesDir("left-over-tight")).build();
            long need = tight.library.length + tight.pack.length + (64L << 20);
            write(new File(tight.files, "saves/slot-0/save.bin"), bytes(1000, 98));
            expectContains(ShipFirstRun.run(tight.bundle, tight.files, ShipTestKit.QUIET, ShipTestKit.diskOf(tight.files, need + 999)).error,
                    "Not enough free space for the game code", "a disk that is really too small");
        });

        check("an update whose start request has another run id: the new run is whole before the pin names it", () -> {
            // (a) the image travels in the APK, as in the real build
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("run-id"));
            v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            v.build();
            expect(run(v).ok(), "version 1");
            String oldRun = v.runId, newRun = "fedcba9876543210fedcba9876543210";
            v.build = "t2";
            v.library = library(310000, 81);
            v.runId = newRun;
            v.directories.add("inputs/native-update");
            v.build();
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "check sees the update");
            ShipFirstRun.Result update = run(v);
            expect(update.ok(), "version 2: " + update.error);
            expectInstalled(v);
            File image = new File(v.files, "private-trace/run-" + newRun + "/decoded.image.bin");
            expectEquals(sha256(ShipTestKit.dataOf(v).get("decoded.image.bin")), sha256(read(image)), "the image under the new run");
            expectEquals(newRun + "\n", text(new File(v.files, "settings/launch-template.txt")), "the pin names the new run");
            // What the earlier version put into its run is removed (nobody could reach it), and nothing else.
            expect(!new File(v.files, "private-trace/run-" + oldRun).exists(), "the earlier version's run folder is gone");
            expect(update.changed.contains("-private-trace/run-" + oldRun + "/arguments.bin")
                    && update.changed.contains("-private-trace/run-" + oldRun + "/decoded.image.bin"), "the run says what it removed: " + update.changed);

            // (a2) a file in the earlier run's folder that this class did not put there stays, and so does the folder
            ShipTestKit.Version k = new ShipTestKit.Version(ShipTestKit.newFilesDir("run-id-keep"));
            k.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            k.build();
            expect(run(k).ok(), "version 1");
            write(new File(k.files, "private-trace/run-" + oldRun + "/trace-protocol.txt"), bytes(40, 71));
            write(new File(k.files, "private-trace/run-11111111111111111111111111111111/arguments.bin"), bytes(40, 72));   // a run Play made
            k.build = "t2";
            k.runId = newRun;
            k.build();
            expect(run(k).ok(), "version 2");
            expect(!new File(k.files, "private-trace/run-" + oldRun + "/arguments.bin").exists()
                    && !new File(k.files, "private-trace/run-" + oldRun + "/decoded.image.bin").exists(), "its own two files are removed");
            expect(new File(k.files, "private-trace/run-" + oldRun + "/trace-protocol.txt").isFile(), "another file in that folder stays");
            expect(new File(k.files, "private-trace/run-11111111111111111111111111111111/arguments.bin").isFile(), "another run is not touched");

            // (a4) an install a computer made (no mark of an earlier version of this app): its pinned run is not this
            // class's to remove, whatever its id
            ShipTestKit.Version c = new ShipTestKit.Version(ShipTestKit.newFilesDir("run-id-computer"));
            c.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            c.runId = newRun;
            c.build();
            write(new File(c.files, "private-trace/run-" + oldRun + "/arguments.bin"), c.requestFor(oldRun));
            write(new File(c.files, "private-trace/run-" + oldRun + "/decoded.image.bin"), bytes(500000, 34));
            write(new File(c.files, "settings/launch-template.txt"), (oldRun + "\n").getBytes("US-ASCII"));
            expect(run(c).ok(), "the first start of this app over a computer's install");
            expect(new File(c.files, "private-trace/run-" + oldRun + "/arguments.bin").isFile()
                    && new File(c.files, "private-trace/run-" + oldRun + "/decoded.image.bin").isFile(), "the computer's run is left as it was");

            // (a3) the new run is whole BEFORE the pin names it: an update that fails at its image leaves the pin on the old run
            ShipTestKit.Version f = new ShipTestKit.Version(ShipTestKit.newFilesDir("run-id-fail"));
            f.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            f.build();
            expect(run(f).ok(), "version 1");
            f.build = "t2";
            f.runId = newRun;
            f.build();
            byte[] damaged = f.bundle.assets.get("ship/pieces/decoded.image.bin").clone();
            damaged[1000] ^= 1;
            f.bundle.assets.put("ship/pieces/decoded.image.bin", damaged);   // the APK's image is damaged
            expect(!run(f).ok(), "the update fails at its image");
            expectEquals(oldRun + "\n", text(new File(f.files, "settings/launch-template.txt")), "the pin still names the old run");
            expect(new File(f.files, "private-trace/run-" + oldRun + "/arguments.bin").isFile(), "and the old run is whole");

            // (b) the image is the player's (pinned, not in the APK): it lies beside the OLD start request and is moved
            ShipTestKit.Version w = new ShipTestKit.Version(ShipTestKit.newFilesDir("run-id-adopt")).build();
            expect(run(w).ok(), "version 1");
            java.util.Map<String, byte[]> data = ShipTestKit.dataOf(w);
            File oldImage = new File(w.files, "private-trace/run-" + oldRun + "/decoded.image.bin");
            write(oldImage, data.get("decoded.image.bin"));
            Files.setPosixFilePermissions(oldImage.toPath(), java.nio.file.attribute.PosixFilePermissions.fromString("r--------"));
            Object identity = Files.readAttributes(oldImage.toPath(), java.nio.file.attribute.BasicFileAttributes.class).fileKey();
            // a run that holds a file of the image's size with other bytes is not taken
            write(new File(w.files, "private-trace/run-00000000000000000000000000000001/decoded.image.bin"), bytes(500000, 77));
            w.build = "t2";
            w.runId = newRun;
            w.build();
            ShipFirstRun.Result moved = run(w);
            expect(moved.ok(), "version 2: " + moved.error);
            File newImage = new File(w.files, "private-trace/run-" + newRun + "/decoded.image.bin");
            expectEquals(sha256(data.get("decoded.image.bin")), sha256(read(newImage)), "the pinned image under the new run");
            expectEquals(identity, Files.readAttributes(newImage.toPath(), java.nio.file.attribute.BasicFileAttributes.class).fileKey(),
                    "it is the same file, moved, not a copy");
            expect(!oldImage.exists(), "it no longer lies under the old run");
            expectEquals("r--------", mode(newImage), "mode of the moved image");
            expect(moved.changed.contains("private-trace/run-" + newRun + "/decoded.image.bin"), "the run says it: " + moved.changed);
            expectEquals(sha256(bytes(500000, 77)), sha256(read(new File(w.files,
                    "private-trace/run-00000000000000000000000000000001/decoded.image.bin"))), "the other file stays where it was");
        });

        check("an update whose start request has another run id, killed at any point: the next start ends with the tree of an update that was not interrupted", () -> {
            // The earlier version's run (its start request and its copy of the image) must be gone at the end, wherever
            // the update's first start was killed. The pin is the only record of which run that was: a start killed
            // after the pin was rewritten and before the mark used to leave the two files in the app's storage for good.
            final ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("run-id-killed"));
            v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            v.build();
            expect(run(v).ok(), "version 1");
            final String oldRun = v.runId, newRun = "fedcba9876543210fedcba9876543210";
            final File before = new File(v.files.getParentFile(), "before-update");
            expect(before.mkdir(), "a copy of version 1's storage");
            ShipTestKit.copyTree(v.files, before);
            v.build = "t2";
            v.library = library(310000, 81);
            v.runId = newRun;
            v.build();
            final File pictures = new File(v.files.getParentFile(), "pictures");
            expect(pictures.mkdir(), "pictures folder");
            final List<String> taken = new ArrayList<String>();
            ShipFirstRun.Result whole = ShipFirstRun.run(v.bundle, v.files, new ShipFirstRun.Progress() {
                @Override public void at(ShipFirstRun.Step step, long done, long total) {
                    String label = taken.size() + "-" + step + "-" + done;
                    try {
                        File picture = new File(pictures, label);
                        expect(picture.mkdir(), "picture " + label);
                        ShipTestKit.copyTree(v.files, picture);
                        taken.add(label);
                    } catch (IOException e) {
                        throw new IllegalStateException(e);
                    }
                }
            }, ShipTestKit.PLENTY);
            expect(whole.ok(), "the update, not interrupted: " + whole.error);
            TreeMap<String, String> complete = snapshot(v.files, false);
            expect(!complete.containsKey("private-trace/run-" + oldRun), "(not interrupted, the earlier run's folder is gone)");
            boolean afterPin = false;
            for (String label : taken) {
                ShipTestKit.emptyDirectory(v.files);
                ShipTestKit.copyTree(new File(pictures, label), v.files);
                afterPin |= (newRun + "\n").equals(text(new File(v.files, "settings/launch-template.txt")))
                        && !(sha256(v.record) + "\n").equals(text(new File(v.files, "settings/ship-installed.txt")));
                ShipFirstRun.Result resumed = run(v);
                expect(resumed.ok(), "resumed after " + label + ": " + resumed.error);
                expectEquals(complete.toString(), snapshot(v.files, false).toString(), "the tree after resuming from " + label);
            }
            expect(afterPin, "at least one kill fell between the new pin and the mark (the window in question): " + taken);
        });

        check("a symbolic link where one of the app's folders belongs is not followed: nothing behind it counts, and the first start puts a real folder there", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("folder-link"));
            v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            v.build();
            expect(run(v).ok(), "first start");
            // one file from the player's discs is in place
            ShipConfig config = v.config();
            ShipConfig.DataFile pinned = config.pinned(ShipTestKit.Version.inputPath(v.inputs.get(0)));
            File disc = ShipData.place(config, v.files, pinned.path);
            write(disc, v.inputs.get(0));
            expect(!ShipData.look(config, v.files).absent.contains(pinned), "(it counts as in place)");
            // inputs/ is moved out of the app's storage and a link takes its name: every file is still reachable by its path
            File inputs = new File(v.files, "private-trace/inputs");
            File outside = new File(v.files.getParentFile(), "outside-inputs");
            Files.move(inputs.toPath(), outside.toPath());
            Files.createSymbolicLink(inputs.toPath(), outside.toPath());
            TreeMap<String, String> behind = snapshot(outside);
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "the app's pieces do not count as in place through the link");
            expect(disc.isFile(), "(the disc file can still be read by its path, through the link)");
            expect(ShipData.look(config, v.files).absent.contains(pinned), "the file behind the link counts as absent");
            ShipFirstRun.Result repaired = run(v);
            expect(repaired.ok(), "the next start: " + repaired.error);
            expect(Files.isDirectory(inputs.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS), "a real folder stands under the name");
            expectEquals(behind.toString(), snapshot(outside).toString(), "nothing behind the link was created, changed or removed");
            expectInstalled(v);
            // a link that points nowhere is replaced as well, and the first start does not fail on it
            File cache = new File(v.files, "private-trace/inputs/native-cache");
            Files.delete(cache.toPath());
            Files.createSymbolicLink(cache.toPath(), new File(v.files.getParentFile(), "nowhere").toPath());
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "a link under a folder the start request names: not in place");
            ShipFirstRun.Result again = run(v);
            expect(again.ok(), "the start after that: " + again.error);
            expect(Files.isDirectory(cache.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS), "a real folder stands there");
            // a link that points nowhere under the name of the folder the player's disc files go to: whatever else the
            // record holds there, the app's pieces are not in place, and the next start puts a real, empty folder there
            ShipTestKit.emptyDirectory(inputs);
            Files.delete(inputs.toPath());
            Files.createSymbolicLink(inputs.toPath(), new File(v.files.getParentFile(), "nowhere").toPath());
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(v.bundle, v.files), "a dangling link where the disc files' folder belongs: not in place");
            ShipFirstRun.Result emptied = run(v);
            expect(emptied.ok(), "the start after that: " + emptied.error);
            expect(Files.isDirectory(inputs.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS) && cache.isDirectory(), "real folders stand there");
            // the same where nothing the app carries or names lies below: a version whose boot modules come from the
            // player (not inside the APK), and a link under the name of the folder one of them goes to
            ShipTestKit.Version p = new ShipTestKit.Version(ShipTestKit.newFilesDir("folder-link-player")).build();
            expect(run(p).ok(), "first start of a version that does not carry its modules");
            File store = new File(p.files, "private-trace/modules/boot/store");
            Files.createSymbolicLink(store.toPath(), new File(p.files.getParentFile(), "nowhere").toPath());
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(p.bundle, p.files), "a link where a folder of the player's files belongs: not in place");
            ShipFirstRun.Result made = run(p);
            expect(made.ok(), "the start after that: " + made.error);
            expect(Files.isDirectory(store.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS), "a real folder stands there, ready for the player's file");
            expect(made.changed.contains("private-trace/modules/boot/store/"), "and the run says so: " + made.changed);
            // and when the link stands further up (the folder above the one a file goes to): the folders below are
            // reached through it by their paths, yet none of them is in the app's storage; the link is the one replaced
            File modules = new File(p.files, "private-trace/modules"), away = new File(p.files.getParentFile(), "outside-modules");
            Files.move(modules.toPath(), away.toPath());
            Files.createSymbolicLink(modules.toPath(), away.toPath());
            TreeMap<String, String> awayBefore = snapshot(away);
            expect(store.isDirectory(), "(the folder below can still be reached by its path)");
            expectEquals(ShipFirstRun.State.NEEDED, ShipFirstRun.check(p.bundle, p.files), "a link above a folder of the player's files: not in place");
            ShipFirstRun.Result above = run(p);
            expect(above.ok(), "the start after that: " + above.error);
            expect(Files.isDirectory(modules.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS)
                    && Files.isDirectory(store.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS), "real folders stand there, all the way down");
            expectEquals(awayBefore.toString(), snapshot(away).toString(), "nothing behind the link was created, changed or removed");
            // a file in a folder's place is not removed: the start says so, and the screen names the kind of failure
            Files.delete(cache.toPath());
            write(cache, bytes(5, 5));
            ShipFirstRun.Result blocked = run(v);
            expect(!blocked.ok(), "a file where a folder belongs is an error");
            expectContains(blocked.error, "native-cache exists and is not a folder", "the work's own sentence");
            expectEquals(ShipSetupModel.FAILED_IN_THE_WAY, ShipSetupModel.plainFailure(blocked.error), "the screen's sentence does not blame free storage");
            expect(cache.isFile() && cache.length() == 5, "the file was left alone");
        });

        check("the mark is written only when everything is in place at the end; a record without a whole pin is refused", () -> {
            final ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("mark")).build();
            // something removes a piece between its copy and the end of the run
            ShipFirstRun.Result result = ShipFirstRun.run(v.bundle, v.files, new ShipFirstRun.Progress() {
                @Override public void at(ShipFirstRun.Step step, long done, long total) {
                    if (step == ShipFirstRun.Step.FINISH) new File(v.files, "private-trace/generic-shaders-v41.bin").delete();
                }
            }, ShipTestKit.PLENTY);
            expect(!result.ok(), "the run does not report success");
            expectContains(result.error, "is not in place as this app version expects it", "the sentence");
            expect(!new File(v.files, "settings/ship-installed.txt").exists(), "no mark");
            expect(run(v).ok(), "the next start finishes the work");
            expectInstalled(v);

            // the pin is the whole game data: a record without it, or with a hole in it, is refused whole
            String input = ShipTestKit.Version.inputPath(v.inputs.get(0));
            java.util.Map<String, byte[]> data = ShipTestKit.dataOf(v);
            java.util.Map<String, byte[]> noImage = new java.util.LinkedHashMap<String, byte[]>(data);
            noImage.remove("decoded.image.bin");
            java.util.Map<String, byte[]> noInput = new java.util.LinkedHashMap<String, byte[]>(data);
            noInput.remove(input);
            java.util.Map<String, byte[]> clash = new java.util.LinkedHashMap<String, byte[]>(data);
            clash.put("modules/boot/store", bytes(5, 5));
            java.util.Map<String, byte[]> noModules = new java.util.LinkedHashMap<String, byte[]>(data);
            for (String path : data.keySet()) if (path.startsWith("modules/boot/")) noModules.remove(path);
            String[][] pins = {
                    {ShipTestKit.Version.pinOf(noModules), "the start request names the folder modules/boot but no file in it is pinned"},
                    {ShipTestKit.Version.NO_PIN, "it has no list of pinned game data"},
                    {ShipTestKit.Version.pinOf(noImage), "the game image is not pinned"},
                    {ShipTestKit.Version.pinOf(noInput), "the start request's input " + input + " is not pinned"},
                    {ShipTestKit.Version.pinOf(clash), "modules/boot/store is pinned as a file and as a folder"},
            };
            for (String[] pin : pins) {
                ShipTestKit.Version bad = new ShipTestKit.Version(ShipTestKit.newFilesDir("pin-rule"));
                bad.dataPin = pin[0];
                bad.build();
                ShipFirstRun.Result refused = run(bad);
                expect(!refused.ok(), pin[1] + ": refused");
                expectContains(refused.error, pin[1], "the reason");
                expectEquals("{}", snapshot(bad.files).toString(), pin[1] + ": nothing was written");
            }
            // a bundled piece is always a pinned file, with the pin's size and SHA-256, under the asset name of its path
            ShipTestKit.Version pieces = new ShipTestKit.Version(ShipTestKit.newFilesDir("piece-rule"));
            pieces.bundledPaths.add("modules/boot/store/dataflow.spv");
            pieces.build();
            String record = new String(pieces.record, StandardCharsets.US_ASCII);
            String entry = "{\"asset\":\"ship/pieces/modules/boot/store/dataflow.spv\",\"path\":\"modules/boot/store/dataflow.spv\",\"size\":8964,";
            expect(record.contains(entry), "the test's record holds the piece");
            String[][] cases = {
                    {entry, entry.replace("\"path\":\"modules/boot/store/dataflow.spv\"", "\"path\":\"modules/boot/other/dataflow.spv\""),
                            "the bundled piece modules/boot/other/dataflow.spv is not a pinned game data file"},
                    {entry, entry.replace("\"size\":8964", "\"size\":8965"), "has another size or SHA-256 than the pin"},
                    {entry, entry.replace("\"asset\":\"ship/pieces/modules/", "\"asset\":\"ship/pieces/../modules/"),
                            "the asset name of modules/boot/store/dataflow.spv is not ship/pieces/modules/boot/store/dataflow.spv"},
                    {entry, entry.replace("\"asset\":\"ship/pieces/modules/boot/store/dataflow.spv\"", "\"asset\":\"ship/game-library.bin\""),
                            "the asset name of modules/boot/store/dataflow.spv is not ship/pieces/modules/boot/store/dataflow.spv"},
            };
            for (String[] c : cases) {
                ShipTestKit.Version bad = new ShipTestKit.Version(ShipTestKit.newFilesDir("piece-rule"));
                bad.bundledPaths.add("modules/boot/store/dataflow.spv");
                bad.build();
                bad.bundle.assets.put("ship/config.json", new String(bad.record, StandardCharsets.US_ASCII).replace(c[0], c[1])
                        .getBytes(StandardCharsets.US_ASCII));
                ShipFirstRun.Result refused = run(bad);
                expect(!refused.ok(), c[2] + ": refused");
                expectContains(refused.error, c[2], "the reason");
                expectEquals("{}", snapshot(bad.files).toString(), c[2] + ": nothing was written");
            }
        });

        ShipTestKit.finish("ShipFirstRunTest");
    }
}
