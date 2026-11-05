package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.bytes;
import static dev.recompbootstrap.smoke.ShipTestKit.changes;
import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectContains;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;
import static dev.recompbootstrap.smoke.ShipTestKit.mode;
import static dev.recompbootstrap.smoke.ShipTestKit.read;
import static dev.recompbootstrap.smoke.ShipTestKit.sha256;
import static dev.recompbootstrap.smoke.ShipTestKit.snapshot;
import static dev.recompbootstrap.smoke.ShipTestKit.text;
import static dev.recompbootstrap.smoke.ShipTestKit.write;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * ShipDataImport against made-up data folders (ShipTestKit.MemoryFolder) in temporary files directories. Run with
 * java -ea. The target layout the checks spell out is the one the game library reads: files/private-trace/inputs/
 * (sha256).bin, files/private-trace/modules/boot/..., and the image beside the start request.
 */
public final class ShipDataImportTest {
    private static final String ID = "0123456789abcdef0123456789abcdef";

    /** A files directory on which the first start has done its work. */
    private static ShipTestKit.Version installed(String label) throws IOException {
        ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir(label)).build();
        ShipFirstRun.Result result = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
        expect(result.ok(), "first start: " + result.error);
        return v;
    }

    private static ShipDataImport.Outcome run(ShipTestKit.Version v, ShipTestKit.MemoryFolder folder, ShipDataImport.Progress progress)
            throws IOException {
        return ShipDataImport.run(v.config(), folder, v.files, progress, ShipTestKit.PLENTY);
    }

    private static File target(ShipTestKit.Version v, String path) {
        return new File(v.files, "private-trace/" + (path.equals("decoded.image.bin") ? "run-" + ID + "/decoded.image.bin" : path));
    }

    private static void expectAllInPlace(ShipTestKit.Version v, Map<String, byte[]> data) throws IOException {
        for (Map.Entry<String, byte[]> entry : data.entrySet()) {
            File file = target(v, entry.getKey());
            expect(file.isFile(), entry.getKey() + " is in place");
            expectEquals(sha256(entry.getValue()), sha256(read(file)), "bytes of " + entry.getKey());
            expectEquals("r--------", mode(file), "mode of " + entry.getKey());
        }
        expect(!new File(v.files, "private-trace/ship-import.part").exists(), "the staging folder is gone");
    }

    public static void main(String[] args) {
        check("import: every listed file arrives byte for byte in the game's layout, and the result says how much", () -> {
            ShipTestKit.Version v = installed("import");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            expectEquals("Your game data is not on this device yet.", ShipDataImport.missing(v.config(), v.files), "before");
            TreeMap<String, String> before = snapshot(v.files);
            ShipTestKit.Recorder progress = new ShipTestKit.Recorder();
            ShipDataImport.Outcome outcome = run(v, new ShipTestKit.MemoryFolder().with(data), progress);
            expect(outcome.complete && !outcome.cancelled && outcome.error == null, "complete: " + outcome.error);
            expectAllInPlace(v, data);
            long total = 0;
            for (byte[] bytes : data.values()) total += bytes.length;
            expectEquals(7, outcome.files, "files in the result");
            expectEquals(total, outcome.bytes, "bytes in the result");
            expectEquals(7, outcome.copied, "files copied");
            expectEquals(total, progress.bytesDone, "progress ends at the total");
            expectEquals(total, progress.bytesTotal, "progress total");
            expect(!progress.decreasing, "progress never goes back");
            expectEquals(null, ShipDataImport.missing(v.config(), v.files), "after: nothing is missing");
            // only the data paths and the result record appeared
            List<String> changed = changes(before, snapshot(v.files));
            List<String> expected = new ArrayList<String>(Arrays.asList("private-trace/modules/boot/r8", "private-trace/modules/boot/r8/vertex",
                    "private-trace/modules/boot/store", "private-trace/modules/boot/depth", "private-trace/modules/boot/depth/vertex",
                    "settings/ship-data.txt"));
            for (String path : data.keySet()) expected.add(v.files.toPath().relativize(target(v, path).toPath()).toString());
            java.util.Collections.sort(changed);
            java.util.Collections.sort(expected);
            expectEquals(expected.toString(), changed.toString(), "everything the import created");
            expectContains(text(new File(v.files, "settings/ship-data.txt")), "files 7\nbytes " + total + "\n", "the result record");
        });

        check("resume: a stopped copy keeps what was finished, and the next run copies only the rest", () -> {
            ShipTestKit.Version v = installed("resume");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(data);
            ShipTestKit.Recorder stopping = new ShipTestKit.Recorder();
            stopping.stopAtBytes = 70000 + 14 + 1000;   // inside the third file (1 MiB + 77 bytes)
            ShipDataImport.Outcome stopped = run(v, folder, stopping);
            expect(stopped.cancelled && !stopped.complete && stopped.error == null, "stopped, not failed: " + stopped.error);
            expectEquals(2, stopped.copied, "files finished before the stop");
            List<String> names = new ArrayList<String>(data.keySet());
            expect(target(v, names.get(0)).isFile() && target(v, names.get(1)).isFile(), "the two finished files stay");
            expect(!target(v, names.get(2)).exists(), "the file that was being copied has no final name");
            String[] parts = new File(v.files, "private-trace/ship-import.part").list();
            expectEquals(0, parts == null ? -1 : parts.length, "its part file was removed");
            expect(ShipDataImport.missing(v.config(), v.files) != null, "the data is still reported as missing");
            expect(!new File(v.files, "settings/ship-data.txt").exists(), "no result record after a stop");

            // a process that died mid-file leaves a part file behind: it is started again, not trusted
            write(new File(v.files, "private-trace/ship-import.part/" + sha256(data.get(names.get(2))) + ".part"), bytes(4000, 99));
            TreeMap<String, String> first = snapshot(v.files);
            folder.opened.clear();
            ShipTestKit.Recorder progress = new ShipTestKit.Recorder();
            ShipDataImport.Outcome resumed = run(v, folder, progress);
            expect(resumed.complete, "the second run completes: " + resumed.error);
            expectEquals(5, resumed.copied, "files the second run copied");
            expect(!folder.opened.contains(names.get(0)) && !folder.opened.contains(names.get(1)),
                    "finished files are not read from the folder again: " + folder.opened);
            TreeMap<String, String> second = snapshot(v.files);
            for (int at = 0; at < 2; ++at) {
                String path = v.files.toPath().relativize(target(v, names.get(at)).toPath()).toString();
                expectEquals(first.get(path), second.get(path), "the finished file " + at + " was not rewritten");
            }
            expectAllInPlace(v, data);
            long total = 0;
            for (byte[] bytes : data.values()) total += bytes.length;
            expectEquals(total, progress.bytesDone, "progress of the second run ends at the whole total");
            // a third run finds everything in place and reads no data file
            folder.opened.clear();
            ShipDataImport.Outcome third = run(v, folder, new ShipTestKit.Recorder());
            expect(third.complete, "a third run completes: " + third.error);
            expectEquals(0, third.copied, "and copies nothing");
            expectEquals("[game-data-manifest.json]", folder.opened.toString(), "what the third run read from the folder");
        });

        check("a list that names a file outside the game data layout is refused whole: nothing is read, nothing written", () -> {
            String[] outside = {"saves/c/e0000000/sgta50015", "../saves/sgta50015", "inputs/../../saves/sgta50015",
                    "/data/user/0/dev.recompbootstrap.smoke/files/saves/x", "private-trace-live.so", "generic-shaders-v41.bin",
                    "settings/qualified-code.txt", "run-" + ID + "/arguments.bin", "diag-fps-overlay", "inputs/readme.txt",
                    "inputs/" + sha256(bytes(9, 1)).toUpperCase() + ".bin", "inputs/" + sha256(bytes(9, 1)) + ".bin/x",
                    "modules/boot/../../../saves/x", "modules/other/dataflow.spv", "modules/boot/.hidden", "modules/boot//x",
                    "modules/boot/a/b/c/d/e/f/g", "decoded.image.bin/../x", "profile/identity.txt", ""};
            for (String path : outside) {
                ShipTestKit.Version v = installed("outside");
                write(new File(v.files, "saves/c/e0000000/sgta50015"), bytes(1000, 5));
                Map<String, byte[]> data = ShipTestKit.dataOf(v);
                Map<String, byte[]> listed = new LinkedHashMap<String, byte[]>(data);
                listed.put(path, bytes(64, 3));
                ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(data);
                folder.files.put(path, bytes(64, 3));
                folder.listed(ShipTestKit.MemoryFolder.list(listed));
                TreeMap<String, String> before = snapshot(v.files);
                ShipDataImport.Outcome outcome = run(v, folder, new ShipTestKit.Recorder());
                expect(!outcome.complete && outcome.error != null, "<" + path + ">: refused");
                expectContains(outcome.error, "outside the game data layout", "<" + path + ">: the sentence");
                expectContains(outcome.error, "Nothing was copied", "<" + path + ">: says nothing was copied");
                expectEquals("[]", changes(before, snapshot(v.files)).toString(), "<" + path + ">: files changed");
                expectEquals("[game-data-manifest.json]", folder.opened.toString(), "<" + path + ">: what was read from the folder");
            }
        });

        check("a file whose bytes are not the listed ones is not installed; files before it stay", () -> {
            ShipTestKit.Version v = installed("damaged");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            List<String> names = new ArrayList<String>(data.keySet());
            ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(data);
            byte[] damaged = data.get(names.get(2)).clone();
            damaged[damaged.length - 1] ^= 1;
            folder.files.put(names.get(2), damaged);   // the list still describes the undamaged file
            ShipDataImport.Outcome outcome = run(v, folder, new ShipTestKit.Recorder());
            expect(!outcome.complete && !outcome.cancelled, "failed");
            expectEquals(names.get(2) + " in the folder is damaged: its SHA-256 does not match the folder's list. It was not installed.",
                    outcome.error, "the sentence");
            expect(!target(v, names.get(2)).exists(), "the damaged file has no final name");
            expect(target(v, names.get(0)).isFile() && target(v, names.get(1)).isFile(), "the files before it stay");
            String[] parts = new File(v.files, "private-trace/ship-import.part").list();
            expectEquals(0, parts == null ? -1 : parts.length, "its part file was removed");
            // shorter and longer than listed are told apart
            folder.files.put(names.get(2), Arrays.copyOf(data.get(names.get(2)), 1000));
            expectContains(run(v, folder, new ShipTestKit.Recorder()).error, "is shorter than the folder's list says", "cut off");
            folder.files.put(names.get(2), Arrays.copyOf(data.get(names.get(2)), data.get(names.get(2)).length + 1));
            expectContains(run(v, folder, new ShipTestKit.Recorder()).error, "is longer than the folder's list says", "too long");
            // listed but not in the folder
            folder.files.remove(names.get(2));
            expectContains(run(v, folder, new ShipTestKit.Recorder()).error, "but the folder does not hold it", "absent");
        });

        check("a hash-named input must be listed under the SHA-256 that is its name", () -> {
            ShipTestKit.Version v = installed("name");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            String first = data.keySet().iterator().next();
            String list = ShipTestKit.MemoryFolder.list(data).replace("\"sha256\":\"" + sha256(data.get(first)) + "\"",
                    "\"sha256\":\"" + sha256(bytes(5, 5)) + "\"");
            ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(data).listed(list);
            // the folder holds, under that name, exactly the bytes the list's (other) hash describes
            folder.files.put(first, bytes(5, 5));
            ShipDataImport.Outcome outcome = run(v, folder, new ShipTestKit.Recorder());
            expectContains(outcome.error, "is listed with another SHA-256 than its name", "the sentence");
            expect(!target(v, first).exists(), "nothing was installed under the hash name");
        });

        check("no space: refused with both numbers before anything is copied; files in place are not counted", () -> {
            ShipTestKit.Version v = installed("space");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            long total = 0;
            for (byte[] bytes : data.values()) total += bytes.length;
            long need = total + (512L << 20);
            ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(data);
            TreeMap<String, String> before = snapshot(v.files);
            ShipDataImport.Outcome tight = ShipDataImport.run(v.config(), folder, v.files, new ShipTestKit.Recorder(), ShipTestKit.spaceOf(need - 1));
            expectEquals("Not enough free space for the game data: it needs 539 MB and 538 MB is free. Nothing was copied.",
                    tight.error, "the sentence");
            expectEquals("[]", changes(before, snapshot(v.files)).toString(), "files changed");
            expectEquals("[game-data-manifest.json]", folder.opened.toString(), "what was read from the folder");
            // the largest file is in place already: the same free space is now enough
            String big = new ArrayList<String>(data.keySet()).get(2);
            write(target(v, big), data.get(big));
            ShipDataImport.Outcome enough = ShipDataImport.run(v.config(), folder, v.files, new ShipTestKit.Recorder(),
                    ShipTestKit.spaceOf(need - data.get(big).length));
            expect(enough.complete, "enough for the rest: " + enough.error);
            expectEquals(6, enough.copied, "files copied");
        });

        check("a folder that lacks a file the start request needs is refused before copying, unless that file is in place", () -> {
            ShipTestKit.Version v = installed("incomplete");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            String needed = new ArrayList<String>(data.keySet()).get(1);
            Map<String, byte[]> fewer = new LinkedHashMap<String, byte[]>(data);
            fewer.remove(needed);
            ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(fewer);
            TreeMap<String, String> before = snapshot(v.files);
            ShipDataImport.Outcome outcome = run(v, folder, new ShipTestKit.Recorder());
            expectEquals("This folder is not complete: 1 file the game needs is not in its list (the first is " + needed
                    + "). Nothing was copied.", outcome.error, "the sentence");
            expectEquals("[]", changes(before, snapshot(v.files)).toString(), "files changed");
            // the image is needed as well
            Map<String, byte[]> noImage = new LinkedHashMap<String, byte[]>(data);
            noImage.remove("decoded.image.bin");
            expectContains(run(v, new ShipTestKit.MemoryFolder().with(noImage), new ShipTestKit.Recorder()).error,
                    "(the first is decoded.image.bin)", "a folder without the image");
            // with the missing input already on the device the same folder is enough
            write(target(v, needed), data.get(needed));
            ShipDataImport.Outcome enough = run(v, folder, new ShipTestKit.Recorder());
            expect(enough.complete, "complete: " + enough.error);
            expectEquals(null, ShipDataImport.missing(v.config(), v.files), "nothing is missing");
        });

        check("pinned game data: a folder with another file under a pinned name is refused before copying", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("pin"));
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            StringBuilder pin = new StringBuilder();
            for (Map.Entry<String, byte[]> entry : data.entrySet()) {
                if (pin.length() > 0) pin.append(',');
                pin.append("{\"path\":\"").append(entry.getKey()).append("\",\"size\":").append(entry.getValue().length)
                        .append(",\"sha256\":\"").append(sha256(entry.getValue())).append("\"}");
            }
            v.dataPin = pin.toString();
            v.build();
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "first start");
            Map<String, byte[]> other = new LinkedHashMap<String, byte[]>(data);
            other.put("modules/boot/store/dataflow.spv", bytes(8964, 55));   // same name and size, other bytes
            TreeMap<String, String> before = snapshot(v.files);
            ShipDataImport.Outcome outcome = run(v, new ShipTestKit.MemoryFolder().with(other), new ShipTestKit.Recorder());
            expectContains(outcome.error, "modules/boot/store/dataflow.spv in this folder is not the file this app version was made for",
                    "the sentence");
            expectEquals("[]", changes(before, snapshot(v.files)).toString(), "files changed");
            // a folder without a pinned module is incomplete
            Map<String, byte[]> fewer = new LinkedHashMap<String, byte[]>(data);
            fewer.remove("modules/boot/depth/vertex/guest-code.bin");
            expectContains(run(v, new ShipTestKit.MemoryFolder().with(fewer), new ShipTestKit.Recorder()).error,
                    "(the first is modules/boot/depth/vertex/guest-code.bin)", "a folder without a pinned module");
            // the right folder is accepted, and "missing" then counts the pinned files too
            expectContains(ShipDataImport.missing(v.config(), v.files), "not on this device yet", "before");
            expect(run(v, new ShipTestKit.MemoryFolder().with(data), new ShipTestKit.Recorder()).complete, "the pinned data imports");
            expectEquals(null, ShipDataImport.missing(v.config(), v.files), "after");
            expect(target(v, "modules/boot/store/dataflow.spv").delete(), "remove one pinned module");
            expectEquals("1 of 7 game data files are missing.", ShipDataImport.missing(v.config(), v.files), "one pinned file gone");
        });

        check("files already on the device are never rewritten: another size or other content stops the import", () -> {
            ShipTestKit.Version v = installed("foreign");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            List<String> names = new ArrayList<String>(data.keySet());
            write(new File(v.files, "saves/c/e0000000/sgta50015"), bytes(270352, 70));
            write(target(v, names.get(0)), bytes(123, 1));                                   // another size
            TreeMap<String, String> before = snapshot(v.files);
            ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(data);
            ShipDataImport.Outcome size = run(v, folder, new ShipTestKit.Recorder());
            expectEquals(names.get(0) + " is already on this device with another size; it was not changed and nothing was copied.",
                    size.error, "the sentence for another size");
            expectEquals("[]", changes(before, snapshot(v.files)).toString(), "files changed");
            expect(target(v, names.get(0)).delete(), "remove it");
            write(target(v, "decoded.image.bin"), bytes(500000, 35));                        // the listed size, other bytes
            before = snapshot(v.files);
            ShipDataImport.Outcome content = run(v, folder, new ShipTestKit.Recorder());
            expectEquals("decoded.image.bin is already on this device with other content; it was not changed and nothing was copied.",
                    content.error, "the sentence for other content");
            expectEquals("[]", changes(before, snapshot(v.files)).toString(), "files changed");
        });

        check("a folder that is not a data folder says so", () -> {
            ShipTestKit.Version v = installed("nolist");
            ShipTestKit.MemoryFolder empty = new ShipTestKit.MemoryFolder();
            expectContains(run(v, empty, new ShipTestKit.Recorder()).error, "This folder has no game-data-manifest.json", "no list");
            expectContains(run(v, new ShipTestKit.MemoryFolder().listed("{\"schema\":\"gta5-code-bundle/1\",\"files\":[]}"),
                    new ShipTestKit.Recorder()).error, "its format is gta5-code-bundle/1, this app reads gta5-game-data/1", "another format");
            expectContains(run(v, new ShipTestKit.MemoryFolder().listed("not json"), new ShipTestKit.Recorder()).error,
                    "cannot be used: it is not JSON", "not JSON");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            String twice = ShipTestKit.MemoryFolder.list(data).replace("]}", ",{\"path\":\"decoded.image.bin\",\"size\":1,\"sha256\":\""
                    + sha256(bytes(1, 1)) + "\"}]}");
            expectContains(run(v, new ShipTestKit.MemoryFolder().with(data).listed(twice), new ShipTestKit.Recorder()).error,
                    "decoded.image.bin is listed twice", "a path listed twice");
        });

        // ---- repair of 2026-10-05: image last, nothing beyond the pin, no name twice in two roles, left-overs --------

        check("a list in another order: the image is copied last, so a copy stopped before the boot modules leaves no start request", () -> {
            ShipTestKit.Version v = installed("image-last");
            // the order of a list sorted by path: decoded.image.bin, inputs/..., modules/...
            final Map<String, byte[]> data = new LinkedHashMap<String, byte[]>(new TreeMap<String, byte[]>(ShipTestKit.dataOf(v)));
            expectEquals("decoded.image.bin", data.keySet().iterator().next(), "the test's list starts with the image");
            final boolean[] atModules = {false};
            ShipDataImport.Progress stopAtModules = new ShipDataImport.Progress() {
                @Override public void update(long done, long total, int files, int filesOf, String current) {
                    if (current.startsWith("modules/")) atModules[0] = true;
                }

                @Override public boolean cancelled() {
                    return atModules[0];
                }
            };
            ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(data);
            ShipDataImport.Outcome stopped = run(v, folder, stopAtModules);
            expect(stopped.cancelled, "stopped at the first boot module: " + stopped.error);
            expect(!target(v, "decoded.image.bin").exists(), "the image was not copied before the boot modules");
            expect(!folder.opened.contains("decoded.image.bin"), "the image was not even read yet: " + folder.opened);
            expect(!InstallState.detect(v.files).ready(), "the launcher's own look does not say ready");
            expectEquals("4 of 7 game data files are missing.", ShipDataImport.missing(v.config(), v.files), "what is missing");
            // the whole copy: the image is the last file read from the folder
            ShipTestKit.MemoryFolder again = new ShipTestKit.MemoryFolder().with(data);
            ShipDataImport.Outcome complete = run(v, again, new ShipTestKit.Recorder());
            expect(complete.complete, "the second run completes: " + complete.error);
            expectEquals("decoded.image.bin", again.opened.get(again.opened.size() - 1), "the last file read from the folder");
            expectAllInPlace(v, data);
            expect(InstallState.detect(v.files).ready(), "now the launcher's own look says ready");
        });

        check("a list that names a file beyond the pinned ones is refused whole: nothing is read, nothing written", () -> {
            ShipTestKit.Version v = installed("beyond");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            byte[] foreign = bytes(5000, 77);
            String[] extras = {"modules/boot/extra/dataflow.spv", "inputs/" + sha256(foreign) + ".bin"};
            for (String extra : extras) {
                Map<String, byte[]> more = new LinkedHashMap<String, byte[]>();
                more.put(extra, foreign);
                more.putAll(data);
                ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(more);
                TreeMap<String, String> before = snapshot(v.files);
                ShipDataImport.Outcome outcome = run(v, folder, new ShipTestKit.Recorder());
                expectEquals("This folder's list names " + extra + ", which is not a file this app version uses. Nothing was copied.",
                        outcome.error, "the sentence");
                expect(!outcome.complete, "not complete");
                expectEquals("[]", changes(before, snapshot(v.files)).toString(), "files changed");
                expectEquals("[game-data-manifest.json]", folder.opened.toString(), "what was read from the folder");
            }
        });

        check("a list in which one name is a file and a folder is refused whole: nothing is read, nothing written", () -> {
            ShipTestKit.Version v = installed("clash");
            Map<String, byte[]> data = new LinkedHashMap<String, byte[]>();
            for (Map.Entry<String, byte[]> entry : ShipTestKit.dataOf(v).entrySet()) {
                if (entry.getKey().equals("decoded.image.bin")) {
                    data.put("modules/boot/a", bytes(10, 71));
                    data.put("modules/boot/a/b", bytes(11, 72));
                }
                data.put(entry.getKey(), entry.getValue());
            }
            ShipTestKit.MemoryFolder folder = new ShipTestKit.MemoryFolder().with(data);
            TreeMap<String, String> before = snapshot(v.files);
            ShipDataImport.Outcome outcome = run(v, folder, new ShipTestKit.Recorder());
            expectEquals("The folder's file list (game-data-manifest.json) cannot be used: modules/boot/a is listed as a file and as a"
                    + " folder. Nothing was copied.", outcome.error, "the sentence");
            expectEquals("[]", changes(before, snapshot(v.files)).toString(), "files changed");
            expectEquals("[game-data-manifest.json]", folder.opened.toString(), "what was read from the folder");
        });

        check("a copy that was killed inside a file: its part file does not make the next copy refuse for space, and a start of the app removes it", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("killed"));
            v.inputs.add(bytes((6 << 20) + 3, 14));   // one large input
            v.build();
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "first start");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            long total = 0;
            for (byte[] file : data.values()) total += file.length;
            // a disk on which the whole import fits, with 1 MiB to spare
            long capacity = ShipTestKit.treeBytes(v.files) + total + (512L << 20) + (1 << 20);
            // what a process that died 4 MiB into the large input leaves in the staging folder
            File staging = new File(v.files, "private-trace/ship-import.part");
            File part = new File(staging, sha256(v.inputs.get(3)) + ".part");
            write(part, bytes(4 << 20, 15));
            write(new File(staging, "kept-folder/inside.part"), bytes(10, 16));   // not a file directly in the staging folder
            ShipDataImport.Outcome resumed = ShipDataImport.run(v.config(), new ShipTestKit.MemoryFolder().with(data), v.files,
                    new ShipTestKit.Recorder(), ShipTestKit.diskOf(v.files, capacity));
            expect(resumed.complete, "choosing the folder again on the same disk: " + resumed.error);
            expect(!part.exists(), "the left-over part file is gone");
            // the disk model is honest: 5 MiB less room and the same import is refused with the disk's numbers
            ShipTestKit.Version tight = installed("killed-tight");
            Map<String, byte[]> small = ShipTestKit.dataOf(tight);
            long smallTotal = 0;
            for (byte[] file : small.values()) smallTotal += file.length;
            expectContains(ShipDataImport.run(tight.config(), new ShipTestKit.MemoryFolder().with(small), tight.files, new ShipTestKit.Recorder(),
                    ShipTestKit.diskOf(tight.files, ShipTestKit.treeBytes(tight.files) + smallTotal + (512L << 20) - 1)).error,
                    "Not enough free space for the game data", "a disk that is really too small");
            // a player who never imports again: a start of the app removes the left-over, and says how much
            File later = new File(new File(tight.files, "private-trace/ship-import.part"), sha256(bytes(1, 1)) + ".part");
            write(later, bytes(123456, 17));
            write(new File(tight.files, "private-trace/inputs/native-cache/kept.part"), bytes(100, 18));
            expectEquals(123456L, ShipDataImport.removeLeftOvers(tight.files), "bytes freed");
            expect(!later.exists(), "the part file is gone");
            expect(new File(tight.files, "private-trace/inputs/native-cache/kept.part").isFile(), "a file elsewhere stays");
            expectEquals(0L, ShipDataImport.removeLeftOvers(tight.files), "nothing left to free");
        });

        ShipTestKit.finish("ShipDataImportTest");
    }
}
