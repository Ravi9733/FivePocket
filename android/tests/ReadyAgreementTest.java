package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.bytes;
import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * "Ready" is asked in three places of the build that is handed out, and a player must never see them disagree: the
 * menu's Play tab (the first item is "Play" and the line reads "Ready."), the two-disc setup page's Finish row
 * ("Done"), and Play itself (it prepares a start instead of refusing). This test puts the same files directory
 * before all three, as each of them reads it on the device, for 0, 10, 17 and 18 installed disc files of 18, and for
 * a file with a wrong size or wrong content. Run with java -ea.
 *
 * The made-up app version has the real build's split: 18 files come from two discs (10 from the play disc, 8 from
 * the install disc), the boot modules and the image travel inside the APK.
 */
public final class ReadyAgreementTest {
    private static final int PLAY_FILES = 10, INSTALL_FILES = 8, DISC_FILES = PLAY_FILES + INSTALL_FILES;

    /** One device after its first start: the app version, and the two discs' lists as the setup page uses them. */
    private static final class Device {
        final ShipTestKit.Version version;
        final List<SetupCatalog.Item> play = new ArrayList<SetupCatalog.Item>(), install = new ArrayList<SetupCatalog.Item>();

        Device(String label) throws Exception {
            this(label, -1);
        }

        /** large: the index of one disc file that is above 64 MiB (such a file is compared by its size, not read); -1 for none. */
        Device(String label, int large) throws Exception {
            version = new ShipTestKit.Version(ShipTestKit.newFilesDir(label));
            version.inputs.clear();
            for (int i = 0; i < DISC_FILES; ++i) {
                // sizes like the real ones in small: most a few KiB, four of 14 bytes, one above 1 MiB
                byte[] file = i == large ? new byte[(64 << 20) + 1]
                        : bytes(i >= PLAY_FILES && i % 2 == 1 ? 14 : i == 3 ? (1 << 20) + 300 : 4000 + 700 * i, 500 + i);
                version.inputs.add(file);
                (i < PLAY_FILES ? play : install).add(new SetupCatalog.Item(i < PLAY_FILES ? SetupCatalog.PLAY : SetupCatalog.INSTALL,
                        "file-" + i, file.length, ShipTestKit.sha256(file)));
            }
            version.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            version.build();
            ShipFirstRun.Result first = ShipFirstRun.run(version.bundle, version.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
            expect(first.ok(), "first start: " + first.error);
        }

        File place(int index) throws Exception {
            return ShipData.place(version.config(), version.files, ShipTestKit.Version.inputPath(version.inputs.get(index)));
        }

        /** Puts a disc file in place as a finished copy leaves it: its name, the given bytes, read-only. */
        void put(int index, byte[] content) throws Exception {
            File file = place(index);
            if (file.exists()) Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("rw-------"));
            ShipTestKit.write(file, content);
            Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("r--------"));
        }

        void putFirst(int count) throws Exception {
            for (int i = 0; i < count; ++i) put(i, version.inputs.get(i));
        }

        /** Sets the length of a disc file that is in place (for the file above 64 MiB: no need to write it again). */
        void setLength(int index, long length) throws Exception {
            File file = place(index);
            Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("rw-------"));
            try (java.io.RandomAccessFile access = new java.io.RandomAccessFile(file, "rw")) {
                access.setLength(length);
            }
            Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("r--------"));
        }

        /** The three answers, each asked the way the device asks it; fails when they differ. Returns the common answer. */
        boolean ready(String situation) throws Exception {
            ShipReady ship = ShipReady.of(version.bundle, version.files);
            // 1. the menu's Play tab (LauncherScreen.detect, LauncherMenu, ShellPages.play)
            LauncherMenu menu = new LauncherMenu(true, true);
            menu.setInstall(InstallState.detect(version.files, ship), ship);
            boolean playItem = "Play".equals(menu.label(LauncherMenu.PRIMARY));
            expectEquals(playItem, "Ready.".equals(menu.stateLine()), situation + ": the Play tab's line reads Ready exactly when its item is Play");
            // 2. the two-disc setup page's Finish row (SetupDiscScreen.look, SetupDiscModel)
            SetupDiscModel page = new SetupDiscModel();
            SetupDiscModel.Disk disk = SetupDiscModel.inspect(version.files, ShipReady.of(version.bundle, version.files),
                    new SetupDiscStore(version.files), SetupTestKit.PLENTY, play, install);
            page.setDisk(disk);
            boolean finish = page.finishReady();
            expectEquals(finish, "Done".equals(page.stateWord(SetupDiscModel.ROW_FINISH)), situation + ": the Finish row reads Done exactly when it is ready");
            // 3. Play itself (DebugGameLauncher.prepare -> ShipPlay.prepare)
            boolean play = ShipPlay.prepare(version.bundle, version.files, false).error == null;
            expectEquals(playItem, finish, situation + ": the Play tab and the Finish row");
            expectEquals(playItem, play, situation + ": the Play tab and Play itself");
            // and they count alike: what the Play tab says is missing is what the page's two disc rows lack
            int lacking = (disk.total(SetupDiscModel.PLAY) - disk.have(SetupDiscModel.PLAY))
                    + (disk.total(SetupDiscModel.INSTALL) - disk.have(SetupDiscModel.INSTALL));
            expectEquals(ship.data.discFilesMissing, lacking, situation + ": the files the Play tab counts as missing and the page's rows");
            // and when one disc is done and the other is not, the two pages give the same amount for what is left
            for (int done = 0; done < SetupDiscModel.DISCS; ++done) {
                int other = 1 - done;
                if (disk.done(done) && disk.have(other) == 0 && disk.total(other) > 0) {
                    String amount = SetupModel.formatBytes(disk.missingBytes(other));
                    expect(menu.detail().contains("still missing (" + amount + ")"), situation + ": the Play tab's amount: " + menu.detail());
                    expect(page.detail(SetupDiscModel.ROW_FINISH).contains(", " + amount + "."), situation + ": the Finish row's amount: "
                            + page.detail(SetupDiscModel.ROW_FINISH));
                }
            }
            return playItem;
        }
    }

    public static void main(String[] args) {
        for (final int installed : new int[] {0, 10, 17, 18}) {
            check(installed + " of 18 disc files in place: the Play tab, the Finish row and Play agree, and say "
                    + (installed == DISC_FILES ? "ready" : "not ready"), () -> {
                Device d = new Device("agree-" + installed);
                d.putFirst(installed);
                expectEquals(installed == DISC_FILES, d.ready(installed + " installed"), "the common answer");
            });
        }

        check("the 17 that are in place need not be the first 17: any one file missing takes Ready away everywhere", () -> {
            for (int missing : new int[] {0, 3, 9, 10, 17}) {   // a play-disc file, the large one, the last of each disc
                Device d = new Device("agree-one-missing");
                d.putFirst(DISC_FILES);
                Files.delete(d.place(missing).toPath());
                expect(!d.ready("file " + missing + " missing"), "not ready without file " + missing);
            }
        });

        check("one file with a wrong size: not ready everywhere, although its name is there", () -> {
            for (int index : new int[] {1, 3, 11}) {   // an ordinary file, the one above 1 MiB, a 14-byte file of the install disc
                byte[] right = new Device("agree-size-right").version.inputs.get(index);
                Device shorter = new Device("agree-size-short");
                shorter.putFirst(DISC_FILES);
                shorter.put(index, Arrays.copyOf(right, right.length - 1));
                expect(!shorter.ready("file " + index + " one byte short"), "one byte short: not ready");
                Device longer = new Device("agree-size-long");
                longer.putFirst(DISC_FILES);
                longer.put(index, Arrays.copyOf(right, right.length + 1));
                expect(!longer.ready("file " + index + " one byte long"), "one byte long: not ready");
                // (control) with the right bytes back all three say ready again
                longer.put(index, right);
                expect(longer.ready("file " + index + " put right"), "put right: ready");
            }
        });

        check("a file above 64 MiB, which is compared by its size alone: one byte short or long is not ready everywhere", () -> {
            Device d = new Device("agree-large", 6);
            d.putFirst(DISC_FILES);
            long right = d.version.inputs.get(6).length;
            expect(d.ready("the large file in place"), "ready with the large file in place");
            d.setLength(6, right - 1);
            expect(!d.ready("the large file one byte short"), "one byte short: not ready");
            d.setLength(6, right + 1);
            expect(!d.ready("the large file one byte long"), "one byte long: not ready");
            d.setLength(6, right);
            expect(d.ready("the large file at its size again"), "at its size again: ready");
        });

        check("one file of the right size with other content: not ready everywhere", () -> {
            Device d = new Device("agree-content");
            d.putFirst(DISC_FILES);
            byte[] other = d.version.inputs.get(3).clone();
            other[other.length / 2] ^= 1;
            d.put(3, other);
            expect(!d.ready("file 3 with other content"), "not ready");
        });

        check("the app's own pieces not in place (an update is pending): not ready everywhere, with all 18 disc files there", () -> {
            Device d = new Device("agree-update");
            d.putFirst(DISC_FILES);
            expect(d.ready("all in place"), "ready before the update");
            d.version.build = "t2";
            d.version.library = ShipTestKit.library(310000, 81);
            d.version.build();
            ShipReady ship = ShipReady.of(d.version.bundle, d.version.files);
            LauncherMenu menu = new LauncherMenu(true, true);
            menu.setInstall(InstallState.detect(d.version.files, ship), ship);
            SetupDiscModel page = new SetupDiscModel();
            page.setDisk(SetupDiscModel.inspect(d.version.files, ship, new SetupDiscStore(d.version.files), SetupTestKit.PLENTY, d.play, d.install));
            expect(!"Play".equals(menu.label(LauncherMenu.PRIMARY)) && !page.finishReady()
                    && ShipPlay.prepare(d.version.bundle, d.version.files, false).error != null, "none of the three says ready");
        });

        check("the game code file or the shader file with the right size and other bytes: not ready everywhere before Play is ever pressed", () -> {
            for (String name : new String[] {"private-trace-live.so", "generic-shaders-v41.bin"}) {
                Device d = new Device("agree-changed-code");
                d.putFirst(DISC_FILES);
                expect(d.ready(name + ": all in place"), "ready before");
                File file = new File(d.version.files, "private-trace/" + name);
                byte[] other = Files.readAllBytes(file.toPath());
                other[other.length / 2] ^= 1;
                Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("rw-------"));
                Files.write(file.toPath(), other);
                Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("r--------"));
                // What the player sees before pressing anything: the page names the next step, and the disc files still count.
                ShipReady ship = ShipReady.of(d.version.bundle, d.version.files);
                LauncherMenu menu = new LauncherMenu(true, true);
                menu.setInstall(InstallState.detect(d.version.files, ship), ship);
                expectEquals(LauncherMenu.Action.OPEN_FIRST_SCREEN, menu.confirm(false), name + ": the Play tab's item is Finish Setting Up");
                expectEquals("The app has not finished setting itself up.", menu.stateLine(), name + ": the line on the page");
                expectEquals(DISC_FILES + " of " + DISC_FILES, (ship.data.discFiles - ship.data.discFilesMissing) + " of " + ship.data.discFiles,
                        name + ": the disc files are still counted as in place");
                // The Play tab and the Finish row are asked before Play, in that order (Device.ready).
                expect(!d.ready(name + " changed"), "the Play tab, the Finish row and Play all say not ready");
                ShipStart.Outcome repaired = ShipStart.run(d.version.bundle, d.version.files, true, false, new ShipStart.Watch() {
                    @Override public void working() {}

                    @Override public void at(ShipFirstRun.Step step, long done, long total) {}
                }, ShipTestKit.PLENTY);
                expect(repaired.ok(), name + ": Finish Setting Up: " + repaired.error);
                expect(d.ready(name + ": after Finish Setting Up"), "and all three say ready again");
            }
        });

        check("a symbolic link under a disc file's name, pointing to a file with the right bytes: not ready everywhere, and the page's disc row is not done", () -> {
            Device d = new Device("agree-link");
            d.putFirst(DISC_FILES);
            int index = 4;                                               // a play-disc file
            File file = d.place(index);
            File outside = new File(d.version.files.getParentFile(), "outside-disc-file.bin");
            Files.move(file.toPath(), outside.toPath());
            Files.createSymbolicLink(file.toPath(), outside.toPath());
            expect(!d.ready("a link under a disc file's name"), "the Play tab, the Finish row and Play all say not ready");
            // the copy engine's own look agrees: the file is to be copied (it does not count the link as the file)
            List<Disc2Discs.Pin> pins = new ArrayList<Disc2Discs.Pin>();
            for (SetupCatalog.Item item : d.play) pins.add(new Disc2Discs.Pin(item.sha256, item.bytes));
            List<Disc2Discs.Pin> missing = Disc2Discs.missing(file.getParentFile(), pins);
            expectEquals(1, missing.size(), "the copy engine finds one play-disc file to copy");
            expectEquals(ShipTestKit.sha256(d.version.inputs.get(index)), missing.get(0).sha256, "that one");
            SetupDiscModel page = new SetupDiscModel();
            page.setDisk(SetupDiscModel.inspect(d.version.files, ShipReady.of(d.version.bundle, d.version.files), new SetupDiscStore(d.version.files),
                    SetupTestKit.PLENTY, d.play, d.install));
            expect(!"Done".equals(page.stateWord(SetupDiscModel.ROW_PLAY)), "the play disc's row is not Done: " + page.stateWord(SetupDiscModel.ROW_PLAY));
        });

        check("the folder of the disc files replaced by a link to a folder that holds them all: not ready everywhere; Finish Setting Up puts a real folder there", () -> {
            Device d = new Device("agree-folder-link");
            d.putFirst(DISC_FILES);
            expect(d.ready("all in place"), "ready before");
            File inputs = new File(d.version.files, "private-trace/inputs");
            File outside = new File(d.version.files.getParentFile(), "outside-inputs");
            Files.move(inputs.toPath(), outside.toPath());
            Files.createSymbolicLink(inputs.toPath(), outside.toPath());
            java.util.TreeMap<String, String> behind = ShipTestKit.snapshot(outside);
            ShipReady ship = ShipReady.of(d.version.bundle, d.version.files);
            LauncherMenu menu = new LauncherMenu(true, true);
            menu.setInstall(InstallState.detect(d.version.files, ship), ship);
            SetupDiscModel page = new SetupDiscModel();
            page.setDisk(SetupDiscModel.inspect(d.version.files, ship, new SetupDiscStore(d.version.files), SetupTestKit.PLENTY, d.play, d.install));
            expect(!"Play".equals(menu.label(LauncherMenu.PRIMARY)), "the Play tab does not offer Play: " + menu.label(LauncherMenu.PRIMARY));
            expect(!"Ready.".equals(menu.stateLine()), "nor read Ready: " + menu.stateLine());
            expect(!page.finishReady(), "the Finish row is not Done");
            expect(ShipPlay.prepare(d.version.bundle, d.version.files, false).error != null, "and Play prepares no start");
            expectEquals(LauncherMenu.Action.OPEN_FIRST_SCREEN, menu.confirm(false), "the Play tab's item is Finish Setting Up");
            ShipStart.Outcome repaired = ShipStart.run(d.version.bundle, d.version.files, true, false, new ShipStart.Watch() {
                @Override public void working() {}

                @Override public void at(ShipFirstRun.Step step, long done, long total) {}
            }, ShipTestKit.PLENTY);
            expect(repaired.ok(), "Finish Setting Up: " + repaired.error);
            expect(Files.isDirectory(inputs.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS), "a real folder stands there");
            expectEquals(behind.toString(), ShipTestKit.snapshot(outside).toString(), "nothing behind the link was created, changed or removed");
            // the disc files were behind the link: they are missing now, and all three places say so alike
            expect(!d.ready("after the repair, the disc files to copy again"), "not ready: the disc files are to be copied again");
            ShipReady after = ShipReady.of(d.version.bundle, d.version.files);
            expectEquals(ShipReady.Stands.DATA, after.stands, "only game data from the discs is missing");
            expectEquals(DISC_FILES, after.data.discFilesMissing, "all 18");
        });

        check("a folder the game's start names is gone, with all 18 disc files there: not ready everywhere, and the first screen puts it back", () -> {
            Device d = new Device("agree-folder");
            d.putFirst(DISC_FILES);
            expect(d.ready("all in place"), "ready before");
            File cache = new File(d.version.files, "private-trace/inputs/native-cache");
            Files.delete(cache.toPath());
            expect(ShipReady.of(d.version.bundle, d.version.files).ready(), "(the ship build's own rule does not look at that folder)");
            expect(!d.ready("the cache folder is gone"), "the Play tab, the Finish row and Play all say not ready");
            // what the player is told to do next works: the menu's item is Finish Setting Up, and that screen's work repairs it
            ShipReady ship = ShipReady.of(d.version.bundle, d.version.files);
            LauncherMenu menu = new LauncherMenu(true, true);
            menu.setInstall(InstallState.detect(d.version.files, ship), ship);
            expectEquals(LauncherMenu.Action.OPEN_FIRST_SCREEN, menu.confirm(false), "the Play tab's item opens the first screen");
            SetupDiscModel page = new SetupDiscModel();
            page.setDisk(SetupDiscModel.inspect(d.version.files, ship, new SetupDiscStore(d.version.files), SetupTestKit.PLENTY, d.play, d.install));
            expect(page.detail(SetupDiscModel.ROW_FINISH).contains("choose Finish Setting Up"), "the Finish row says the same: " + page.detail(SetupDiscModel.ROW_FINISH));
            ShipStart.Outcome repaired = ShipStart.run(d.version.bundle, d.version.files, true, false, new ShipStart.Watch() {
                @Override public void working() {}

                @Override public void at(ShipFirstRun.Step step, long done, long total) {}
            }, ShipTestKit.PLENTY);
            expect(repaired.ok() && cache.isDirectory(), "Finish Setting Up put the folder back: " + repaired.error);
            expect(d.ready("after Finish Setting Up"), "and all three say ready again");
        });

        ShipTestKit.finish("ReadyAgreementTest");
    }
}
