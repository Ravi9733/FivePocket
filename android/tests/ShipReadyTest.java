package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.bytes;
import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;
import static dev.recompbootstrap.smoke.ShipTestKit.sha256;
import static dev.recompbootstrap.smoke.ShipTestKit.snapshot;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * The one rule "this build can start the game" (ShipReady, ShipData) and the list of pinned files that do not
 * travel inside the APK (ShipConfig.notBundled), against made-up app versions. Run with java -ea.
 *
 * The made-up version here has the real build's split in small: the boot modules, the image and one small catalog
 * are inside the APK; the other inputs come from the player's folder.
 */
public final class ShipReadyTest {
    private static ShipTestKit.Version split(String label) throws IOException {
        ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir(label));
        v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
        v.bundledPaths.add(ShipTestKit.Version.inputPath(v.inputs.get(1)));   // the 14-byte input: a "catalog"
        return v.build();
    }

    private static List<String> paths(List<ShipConfig.DataFile> files) {
        List<String> paths = new ArrayList<String>();
        for (ShipConfig.DataFile file : files) paths.add(file.path);
        return paths;
    }

    /** The made-up version as an APK file: a ZIP with the same assets under "assets/". */
    private static File apkOf(ShipTestKit.Version v) throws IOException {
        File apk = new File(v.files.getParentFile(), "base.apk");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(apk))) {
            for (Map.Entry<String, byte[]> asset : v.bundle.assets.entrySet()) {
                zip.putNextEntry(new ZipEntry("assets/" + asset.getKey()));
                zip.write(asset.getValue());
                zip.closeEntry();
            }
        }
        return apk;
    }

    public static void main(String[] args) {
        check("the record says which pinned files are not inside the APK: exactly those, in the record's order, with their bytes", () -> {
            ShipTestKit.Version v = split("not-bundled");
            ShipConfig config = v.config();
            String first = ShipTestKit.Version.inputPath(v.inputs.get(0)), third = ShipTestKit.Version.inputPath(v.inputs.get(2));
            expectEquals(Arrays.asList(first, third).toString(), paths(config.notBundled).toString(), "the files that must come from the discs");
            expectEquals(70000L + (1 << 20) + 77, config.notBundledBytes(), "their bytes");
            expectEquals(sha256(v.inputs.get(0)), config.notBundled.get(0).sha256, "each with the SHA-256 a disc file is compared with");
            expectEquals(70000L, config.notBundled.get(0).bytes, "and its size");
            expect(config.bundled("decoded.image.bin") != null && config.bundled(first) == null, "bundled() tells them apart");
            // without pieces every pinned file must come from outside
            ShipTestKit.Version plain = new ShipTestKit.Version(ShipTestKit.newFilesDir("not-bundled-plain")).build();
            expectEquals(7, plain.config().notBundled.size(), "a record without pieces: every pinned file");
        });

        check("the states of a fresh install: nothing bundled, code needed, data from the discs needed, ready", () -> {
            ShipTestKit.Version v = split("states");
            expectEquals(ShipReady.Stands.NOT_BUNDLED, ShipReady.of((ShipBundle) null, v.files).stands, "no bundle");
            ShipTestKit.MemoryBundle developer = new ShipTestKit.MemoryBundle();
            ShipReady none = ShipReady.of(developer, v.files);
            expect(none.stands == ShipReady.Stands.NOT_BUNDLED && !none.bundled() && !none.ready(), "an APK without the record");
            TreeMap<String, String> empty = snapshot(v.files);
            ShipReady before = ShipReady.of(v.bundle, v.files);
            expect(before.stands == ShipReady.Stands.CODE && before.bundled() && !before.ready(), "before the first start: " + before.stands);
            expectEquals(empty.toString(), snapshot(v.files).toString(), "asking writes nothing");

            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "first start");
            ShipReady after = ShipReady.of(v.bundle, v.files);
            expectEquals(ShipReady.Stands.DATA, after.stands, "after the first start");
            expectEquals("Your game data is not on this device yet.", after.data.sentence(), "the sentence");
            expectEquals(paths(v.config().notBundled).toString(), paths(after.data.absent).toString(), "absent: exactly the files not in the APK");
            expectEquals(2, after.data.missing(), "missing");
            expectEquals(7, after.data.pinned, "pinned");
            expectEquals(v.config().notBundledBytes(), after.data.missingBytes(), "missing bytes");

            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            expect(ShipDataImport.run(v.config(), new ShipTestKit.MemoryFolder().with(data), v.files, new ShipTestKit.Recorder(),
                    ShipTestKit.PLENTY).complete, "import");
            ShipReady ready = ShipReady.of(v.bundle, v.files);
            expect(ready.stands == ShipReady.Stands.NOTHING && ready.ready(), "with everything in place: " + ready.stands);
            expectEquals(null, ready.data.sentence(), "no sentence");

            // an update that is pending: not ready until its first start has run
            v.build = "t2";
            v.build();
            expectEquals(ShipReady.Stands.CODE, ShipReady.of(v.bundle, v.files).stands, "an update before its first start");
        });

        check("the log lines of a first start and of a later start name every piece, what is missing and what is different", () -> {
            ShipTestKit.Version v = split("log");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            String run = "private-trace/run-" + v.runId, record = sha256(v.record).substring(0, 12);
            String identity = "build=t1 record=" + record + " code=" + sha256(v.library).substring(0, 12) + " shaders="
                    + sha256(v.pack).substring(0, 12);
            ShipFirstRun.Result first = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
            List<String> lines = ShipLog.firstRun(first);
            String catalog = ShipTestKit.Version.inputPath(v.inputs.get(1));
            List<String> expected = new ArrayList<String>(Arrays.asList(
                    "PIECE private-trace/private-trace-live.so bytes=300000 sha256=" + sha256(v.library).substring(0, 12) + " copied",
                    "PIECE private-trace/generic-shaders-v41.bin bytes=150000 sha256=" + sha256(v.pack).substring(0, 12) + " copied",
                    "PIECE " + run + "/arguments.bin bytes=" + v.request.length + " sha256=" + sha256(v.request).substring(0, 12) + " copied",
                    "PIECE private-trace/modules/boot/r8/vertex/dataflow.spv bytes=34952 sha256="
                            + sha256(data.get("modules/boot/r8/vertex/dataflow.spv")).substring(0, 12) + " copied",
                    "PIECE private-trace/modules/boot/store/dataflow.spv bytes=8964 sha256="
                            + sha256(data.get("modules/boot/store/dataflow.spv")).substring(0, 12) + " copied",
                    "PIECE private-trace/modules/boot/depth/vertex/guest-code.bin bytes=108 sha256="
                            + sha256(data.get("modules/boot/depth/vertex/guest-code.bin")).substring(0, 12) + " copied",
                    "PIECE private-trace/" + catalog + " bytes=14 sha256=" + sha256(v.inputs.get(1)).substring(0, 12) + " copied",
                    "PIECE " + run + "/decoded.image.bin bytes=500000 sha256=" + sha256(data.get("decoded.image.bin")).substring(0, 12) + " copied",
                    // 3 fixed pieces, 5 bundled, the pin and the record of the code, inputs/native-cache, 2 of the record's 3
                    // switches (the frame-rate counter's is not created: off by default), the mark
                    "FIRST_RUN ok changed=14 pieces=8 " + identity));
            expectEquals(expected.toString(), lines.toString(), "the lines of the first start");
            String first0 = ShipTestKit.Version.inputPath(v.inputs.get(0)), third = ShipTestKit.Version.inputPath(v.inputs.get(2));
            expectEquals(Arrays.asList(
                    "GAME_DATA needed: Your game data is not on this device yet. pinned=7 missing=2 missing_bytes=" + (70000 + (1 << 20) + 77)
                            + " bundled=5 from_discs=2",
                    "MISSING " + first0 + " bytes=70000 sha256=" + sha256(v.inputs.get(0)).substring(0, 12),
                    "MISSING " + third + " bytes=" + ((1 << 20) + 77) + " sha256=" + sha256(v.inputs.get(2)).substring(0, 12)).toString(),
                    ShipLog.gameData(v.config(), ShipData.look(v.config(), v.files)).toString(), "the lines about the game data");
            expectEquals("CODE in place " + identity + " pieces=8", ShipLog.codeInPlace(v.config()), "the line of a later start");
            // a pinned file that is there but is not the pinned one is named as such
            ShipTestKit.write(ShipData.place(v.config(), v.files, first0), bytes(70000, 123));
            expectEquals(Arrays.asList(
                    "GAME_DATA needed: 2 of 7 game data files are missing or are not the files this app version was made for. pinned=7"
                            + " missing=2 missing_bytes=" + (70000 + (1 << 20) + 77) + " bundled=5 from_discs=2",
                    "MISSING " + third + " bytes=" + ((1 << 20) + 77) + " sha256=" + sha256(v.inputs.get(2)).substring(0, 12),
                    "DIFFERENT " + first0 + " bytes=70000 sha256=" + sha256(v.inputs.get(0)).substring(0, 12)).toString(),
                    ShipLog.gameData(v.config(), ShipData.look(v.config(), v.files)).toString(), "with a file that is not the pinned one");
            // a failed first start says so, with the reason
            ShipTestKit.Version tight = split("log-fail");
            List<String> failed = ShipLog.firstRun(ShipFirstRun.run(tight.bundle, tight.files, ShipTestKit.QUIET, ShipTestKit.spaceOf(1000)));
            expect(failed.size() == 1 && failed.get(0).startsWith("FIRST_RUN failed changed=0 pieces=0 build=t1 ")
                    && failed.get(0).contains(" error=Not enough free space for the game code"), "the line of a failed first start: " + failed);
        });

        check("ready needs every pinned file with its size, and the bundled pieces and the small files with their SHA-256", () -> {
            ShipTestKit.Version v = split("rule");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "first start");
            expect(ShipDataImport.run(v.config(), new ShipTestKit.MemoryFolder().with(data), v.files, new ShipTestKit.Recorder(),
                    ShipTestKit.PLENTY).complete, "import");
            String disc = ShipTestKit.Version.inputPath(v.inputs.get(0));
            // a file between 1 MiB and 64 MiB, as three of the real disc files are (the game's executable among them)
            String medium = ShipTestKit.Version.inputPath(v.inputs.get(2));
            String[][] cases = {   // path, what is done to it, the answer
                    {medium, "changed", "DATA", "1 of 7 game data files are missing or are not the files this app version was made for."},
                    {disc, "link", "DATA", "1 of 7 game data files are missing."},      // a link to a file with the pinned bytes is no file
                    {disc, "folder", "DATA", "1 of 7 game data files are missing."},
                    {disc, "removed", "DATA", "1 of 7 game data files are missing."},
                    {disc, "cut", "DATA", "1 of 7 game data files are missing or are not the files this app version was made for."},
                    {disc, "changed", "DATA", "1 of 7 game data files are missing or are not the files this app version was made for."},
                    {"modules/boot/store/dataflow.spv", "removed", "CODE", null},      // a bundled piece: the cheap check sees it
                    {"modules/boot/store/dataflow.spv", "changed", "PIECE", "1 of 7 game data files are missing or are not the files"
                            + " this app version was made for."},
                    {"decoded.image.bin", "changed", "PIECE", "1 of 7 game data files are missing or are not the files this app version"
                            + " was made for."},
            };
            for (String[] c : cases) {
                File file = ShipData.place(v.config(), v.files, c[0]);
                byte[] right = data.get(c[0]);
                expect(ShipReady.of(v.bundle, v.files).ready(), c[0] + " " + c[1] + ": ready before");
                Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("rw-------"));
                if (c[1].equals("removed")) {
                    Files.delete(file.toPath());
                } else if (c[1].equals("link")) {
                    File elsewhere = new File(v.files.getParentFile(), "elsewhere.bin");
                    Files.write(elsewhere.toPath(), right);
                    Files.delete(file.toPath());
                    Files.createSymbolicLink(file.toPath(), elsewhere.toPath());
                } else if (c[1].equals("folder")) {
                    Files.delete(file.toPath());
                    Files.createDirectory(file.toPath());
                } else if (c[1].equals("cut")) {
                    Files.write(file.toPath(), Arrays.copyOf(right, right.length - 1));
                } else {
                    byte[] other = right.clone();
                    other[other.length - 1] ^= 1;   // the pinned size, other content
                    Files.write(file.toPath(), other);
                }
                ShipReady answer = ShipReady.of(v.bundle, v.files);
                expectEquals(c[2], answer.stands.name(), c[0] + " " + c[1]);
                expect(!answer.ready(), c[0] + " " + c[1] + ": not ready");
                if (c[3] != null) expectEquals(c[3], answer.data.sentence(), c[0] + " " + c[1] + ": the sentence");
                if (c[1].equals("link") || c[1].equals("folder")) Files.delete(file.toPath());
                Files.write(file.toPath(), right);
                Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("r--------"));
            }
        });

        check("a pinned file above 64 MiB is compared by its size and is not read; one up to 64 MiB is read", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("large"));
            byte[] large = new byte[(64 << 20) + 1];   // one byte above the limit; zeros
            v.inputs.add(large);
            v.build();
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "first start");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            String largePath = ShipTestKit.Version.inputPath(large), smallPath = ShipTestKit.Version.inputPath(v.inputs.get(0));
            File largeFile = ShipData.place(v.config(), v.files, largePath), smallFile = ShipData.place(v.config(), v.files, smallPath);
            for (Map.Entry<String, byte[]> entry : data.entrySet()) {
                File file = ShipData.place(v.config(), v.files, entry.getKey());
                if (entry.getKey().equals(largePath)) {
                    file.getParentFile().mkdirs();
                    try (java.io.RandomAccessFile zeros = new java.io.RandomAccessFile(file, "rw")) {
                        zeros.setLength(large.length);   // the pinned content, without writing 64 MiB
                    }
                } else {
                    ShipTestKit.write(file, entry.getValue());
                }
            }
            expect(ShipData.look(v.config(), v.files).complete(), "with every file in place");
            // a file that cannot be read: above the limit it is not read at all, up to the limit it is
            Files.setPosixFilePermissions(largeFile.toPath(), PosixFilePermissions.fromString("---------"));
            expect(ShipData.look(v.config(), v.files).complete(), "a file above 64 MiB is not read: its name and size are enough");
            Files.setPosixFilePermissions(smallFile.toPath(), PosixFilePermissions.fromString("---------"));
            ShipData unreadable = ShipData.look(v.config(), v.files);
            expectEquals("[" + smallPath + "]", paths(unreadable.different).toString(), "a file up to 64 MiB that cannot be read is not the pinned file");
            Files.setPosixFilePermissions(smallFile.toPath(), PosixFilePermissions.fromString("r--------"));
            // another size is seen without reading
            Files.setPosixFilePermissions(largeFile.toPath(), PosixFilePermissions.fromString("rw-------"));
            try (java.io.RandomAccessFile shorter = new java.io.RandomAccessFile(largeFile, "rw")) {
                shorter.setLength(large.length - 1);
            }
            ShipData cut = ShipData.look(v.config(), v.files);
            expectEquals("[" + largePath + "]", paths(cut.different).toString(), "a file above the limit of another size is named as different");
            expectEquals("[]", paths(cut.absent).toString(), "nothing is absent");
            expectEquals(ShipReady.Stands.DATA, ShipReady.of(v.bundle, v.files).stands, "and it stands in the way of a start");
        });

        check("asked through the APK file: the same answers, and an APK that cannot be found is never 'ready' or 'a developer build'", () -> {
            ShipTestKit.Version v = split("apk");
            File apk = apkOf(v);
            expectEquals(ShipReady.Stands.CODE, ShipReady.of(apk.getPath(), v.files).stands, "before the first start");
            ShipBundle.Apk bundle = new ShipBundle.Apk(apk.getPath());
            ShipFirstRun.Result first = ShipFirstRun.run(bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
            bundle.close();
            expect(first.ok(), "the first start out of the APK file: " + first.error);
            for (String path : v.bundledPaths) {
                expect(ShipData.place(v.config(), v.files, path).isFile(), path + " came out of assets/ship/pieces/");
            }
            expectEquals(ShipReady.Stands.DATA, ShipReady.of(apk.getPath(), v.files).stands, "after the first start");
            ShipReady unknown = ShipReady.of((String) null, v.files);
            expect(unknown.stands == ShipReady.Stands.APP_FILE && unknown.bundled() && !unknown.ready(), "no path: " + unknown.stands);
            ShipReady unreadable = ShipReady.of(new File(v.files, "no-such.apk").getPath(), v.files);
            expect(unreadable.stands == ShipReady.Stands.APP_FILE && !unreadable.ready(), "a path that cannot be opened: " + unreadable.stands);
            // an APK without game code is a developer build
            File plain = new File(v.files.getParentFile(), "plain.apk");
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(plain))) {
                zip.putNextEntry(new ZipEntry("classes.dex"));
                zip.write(bytes(10, 1));
                zip.closeEntry();
            }
            expectEquals(ShipReady.Stands.NOT_BUNDLED, ShipReady.of(plain.getPath(), v.files).stands, "an APK without the record");
        });

        ShipTestKit.finish("ShipReadyTest");
    }
}
