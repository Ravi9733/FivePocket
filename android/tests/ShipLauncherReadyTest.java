package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectContains;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;
import static dev.recompbootstrap.smoke.ShipTestKit.library;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.TimeZone;

/**
 * What the menu's Play tab says and does in the build that is handed out (it carries its game code), on made-up app
 * versions in temporary files directories. The answer is read as the screen gets it: LauncherScreen.detect hands the
 * menu InstallState.detect(filesDir, ship) together with ship = ShipReady.of(apkPath, filesDir), and draws
 * ShellPages.play. Run with java -ea.
 *
 * (Lane apk-standalone wrote the first version of this test for its launcher patches; the ship integration of
 * 2026-10-05 replaced those patches' texts by the product's: "Set Up Game Files" opens the two-disc setup page,
 * "Finish Setting Up" the app's first screen.)
 */
public final class ShipLauncherReadyTest {
    /** The split of the real build in small: boot modules and image inside the APK, the inputs from the player's discs. */
    private static ShipTestKit.Version firstStart(String label) throws IOException {
        ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir(label));
        v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
        v.build();
        ShipFirstRun.Result first = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY);
        expect(first.ok(), "first start: " + first.error);
        return v;
    }

    /** Puts the disc files in place as a finished copy leaves them (the copy itself is SetupDiscCopyTest's subject). */
    private static void placeDiscFiles(ShipTestKit.Version v, int howMany) throws IOException {
        for (int i = 0; i < howMany; ++i) {
            File file = ShipData.place(v.config(), v.files, ShipTestKit.Version.inputPath(v.inputs.get(i)));
            ShipTestKit.write(file, v.inputs.get(i));
            Files.setPosixFilePermissions(file.toPath(), PosixFilePermissions.fromString("r--------"));
        }
    }

    /** What the launcher's screen gets after a look at the files. */
    private static InstallState look(ShipTestKit.Version v) {
        return InstallState.detect(v.files, ShipReady.of(v.bundle, v.files));
    }

    /** The menu of the handed-out build after a look at the files, as LauncherScreen.detect fills it. */
    private static LauncherMenu menuOf(ShipTestKit.Version v) {
        LauncherMenu menu = new LauncherMenu(true, true);
        ShipReady ship = ShipReady.of(v.bundle, v.files);
        menu.setInstall(InstallState.detect(v.files, ship), ship);
        return menu;
    }

    /** The Play tab as it is drawn, highlight on the item. */
    private static MenuScenes.Page playPage(LauncherMenu menu, String controller) {
        ShellNav nav = new ShellNav();   // a fresh frame: the highlight is on the Play tab's item
        MenuScenes.Page page = ShellPages.frame(nav, menu, controller, TimeZone.getTimeZone("UTC"));
        ShellPages.play(page, nav, menu, controller, TimeZone.getTimeZone("UTC"));
        return page;
    }

    private static List<String> texts(List<MenuScenes.Line> lines) {
        List<String> out = new ArrayList<String>();
        for (MenuScenes.Line line : lines) out.add(line.text);
        return out;
    }

    private static String row(MenuScenes.Page page, String label) {
        for (MenuScenes.Row row : page.rows) if (label.equals(row.label)) return row.value;
        throw new AssertionError("no row " + label);
    }

    private static List<String> prompts(MenuScenes.Page page) {
        List<String> out = new ArrayList<String>();
        for (MenuScenes.Prompt prompt : page.prompts) out.add(prompt.glyph + " " + prompt.label);
        return out;
    }

    public static void main(String[] args) {
        check("data missing: the first item is Set Up Game Files, it opens the two-disc setup, and the page says what is missing", () -> {
            ShipTestKit.Version v = firstStart("discs-missing");
            LauncherMenu menu = menuOf(v);
            MenuScenes.Page page = playPage(menu, "Test Pad");
            expectEquals("[Set Up Game Files]", Arrays.toString(page.column), "the first item");
            expectEquals("Your game data is not on this device yet.", page.info.get(0).text, "the state line");
            expectEquals(MenuScenes.TONE_WARNING, page.info.get(0).tone, "drawn as a warning");
            long missing = 0;
            for (byte[] input : v.inputs) missing += input.length;
            expectEquals("[The game's own code is installed. Your game data is not on this device yet: 3 files from your two game"
                    + " discs, " + ShipFiles.amount(missing, true) + ". Choose Set Up Game Files to copy them from the images of your two discs.]",
                    texts(page.help).toString(), "under the facts: what is missing, in the player's words, and what to do");
            expectEquals("0 of 3 files", row(page, "Game Data"), "the Game Data row");
            expectEquals("Installed", row(page, "Game Code"), "the Game Code row");
            expect(prompts(page).contains("A Select"), "A is offered on the item: " + prompts(page));
            expectEquals(LauncherMenu.Action.OPEN_GAME_FILES_SETUP, menu.confirm(false), "A opens the two-disc setup page");
            expect(!menu.preparing(), "and starts no game");
            expectEquals(null, menu.secondLine(LauncherMenu.PRIMARY), "no 'Checking' line stays on the page while the setup page is open");
        });

        check("some disc files in place: the page counts what is still missing, and the item stays Set Up Game Files", () -> {
            ShipTestKit.Version v = firstStart("discs-some");
            placeDiscFiles(v, 2);
            LauncherMenu menu = menuOf(v);
            MenuScenes.Page page = playPage(menu, "Test Pad");
            expectEquals("1 of the 3 game data files is still missing.", page.info.get(0).text, "the state line");
            expectEquals("2 of 3 files", row(page, "Game Data"), "the Game Data row");
            expectContains(page.help.get(0).text, "1 of the 3 files from your two game discs is still missing (", "under the facts");
            expectContains(page.help.get(0).text, "Choose Set Up Game Files to copy the rest.", "and what to do");
            expectEquals("Set up game files", menu.label(LauncherMenu.PRIMARY), "the first item");
        });

        check("everything in place: the item is Play, the page says Ready, and A prepares a start", () -> {
            ShipTestKit.Version v = firstStart("ready");
            placeDiscFiles(v, v.inputs.size());
            LauncherMenu menu = menuOf(v);
            MenuScenes.Page page = playPage(menu, "Test Pad");
            expectEquals("[Play]", Arrays.toString(page.column), "the first item");
            expectEquals("Ready.", page.info.get(0).text, "the state line");
            expectEquals(MenuScenes.TONE_NORMAL, page.info.get(0).tone, "not a warning");
            expectEquals("3 of 3 files", row(page, "Game Data"), "the Game Data row");
            // a start that was refused reaches this page as a sentence of this build (no install kit in it)
            File refusedRun = new File(v.files, "private-trace/run-fedcba9876543210fedcba9876543210");
            ShipTestKit.write(new File(refusedRun, "arguments.bin"), new byte[] {1});
            ShipTestKit.write(new File(v.files, "private-trace/used-fedcba9876543210fedcba9876543210"), new byte[0]);
            expect(GameWindowStatus.report(v.files, "fedcba9876543210fedcba9876543210", "REJECT_NATIVE_RUN"), "(the game window refuses a start)");
            menu.startOutcome(LaunchOutcome.read(v.files, "fedcba9876543210fedcba9876543210", LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1));
            expectEquals("The game did not start: the game stopped while it was starting. Choose Play again. If it happens again, install the app"
                    + " again over this one, without removing it first: your game data and your saves are then kept. (REJECT_NATIVE_RUN)",
                    menu.failure(), "the sentence on the Play page");
            expect(look(v).ready(), "(the refused run changes nothing about Ready)");
            expectEquals(LauncherMenu.Action.PLAY, menu.confirm(false), "A on Play");
            // the header of the handed-out build says nothing about what kind of build it is
            expect(!texts(page.status).contains("Private build"), "the header: " + texts(page.status));
            expectEquals("FivePocket", page.title, "the menu's own header (the project's name, owner 2026-10-25)");
        });

        check("no controller: the page says so in one sentence under the facts, the row reads None, and Play is still offered", () -> {
            ShipTestKit.Version v = firstStart("no-controller");
            placeDiscFiles(v, v.inputs.size());
            for (boolean ship : new boolean[] {true, false}) {
                LauncherMenu menu = new LauncherMenu(true, ship);
                ShipReady rule = ShipReady.of(v.bundle, v.files);
                menu.setInstall(InstallState.detect(v.files, rule), ship ? rule : null);
                for (String none : new String[] {null, "", "  "}) {
                    MenuScenes.Page page = playPage(menu, none);
                    String last = page.help.get(page.help.size() - 1).text;
                    expectEquals("Connect a controller to play: the game needs one.", last, (ship ? "handed-out" : "developer") + " build, controller <" + none + ">");
                    expectEquals("None", row(page, "Controller"), "the Controller row");
                    expectEquals("[Play]", Arrays.toString(page.column), "Play is still the item");
                    expect(page.columnEnabled[0] && prompts(page).contains("A Select"), "and A is offered on it");
                }
                MenuScenes.Page with = playPage(menu, "Xbox Wireless Controller");
                expect(!texts(with.help).contains("Connect a controller to play: the game needs one."), "with a controller the sentence is gone");
                expectEquals("Xbox Wireless Controller", row(with, "Controller"), "the Controller row names it");
                expectEquals(LauncherMenu.Action.PLAY, menu.confirm(false), "A on Play prepares a start, whatever the controller row says");
            }
        });

        check("a file that is not the pinned one takes Ready away although the start request is whole; a changed piece of the app's own asks for Finish Setting Up", () -> {
            ShipTestKit.Version v = firstStart("module-gone");
            placeDiscFiles(v, v.inputs.size());
            expect(look(v).ready(), "with everything in place");
            Map<String, byte[]> data = ShipTestKit.dataOf(v);
            File image = ShipData.place(v.config(), v.files, "decoded.image.bin");
            File input = ShipData.place(v.config(), v.files, ShipTestKit.Version.inputPath(v.inputs.get(0)));
            // (1) a disc file with the pinned size and other content: the launcher's own look cannot see it
            byte[] other = v.inputs.get(0).clone();
            other[5] ^= 1;
            Files.setPosixFilePermissions(input.toPath(), PosixFilePermissions.fromString("rw-------"));
            Files.write(input.toPath(), other);
            expect(InstallState.detect(v.files).ready(), "(the launcher's own look still says ready)");
            LauncherMenu changed = menuOf(v);
            expectEquals("Set up game files", changed.label(LauncherMenu.PRIMARY), "Play is not offered; the two-disc setup replaces the file");
            expectEquals("1 of the 3 game data files is still missing.", changed.stateLine(), "the state line counts it as missing");
            expectEquals(LauncherMenu.Action.OPEN_GAME_FILES_SETUP, changed.confirm(false), "A does not start the game");
            Files.write(input.toPath(), v.inputs.get(0));
            expect(look(v).ready(), "with the pinned file back");
            // (2) a piece the app carries with other content: the app's own file is not in place
            byte[] imageBytes = data.get("decoded.image.bin").clone();
            imageBytes[9] ^= 1;
            Files.setPosixFilePermissions(image.toPath(), PosixFilePermissions.fromString("rw-------"));
            Files.write(image.toPath(), imageBytes);
            expect(InstallState.detect(v.files).ready(), "(the launcher's own look still says ready)");
            LauncherMenu piece = menuOf(v);
            MenuScenes.Page page = playPage(piece, "Test Pad");
            expectEquals("[Finish Setting Up]", Arrays.toString(page.column), "the first item");
            expectEquals("The app has not finished setting itself up.", page.info.get(0).text, "the state line");
            expectContains(page.help.get(0).text, "Choose Finish Setting Up: the app checks its files and puts back what is missing.", "what to do");
            expectEquals(LauncherMenu.Action.OPEN_FIRST_SCREEN, piece.confirm(false), "A opens the app's first screen, which repairs it");
        });

        check("an update that is pending: not Ready before its first start has run, whatever the old install looks like", () -> {
            ShipTestKit.Version v = firstStart("pending");
            placeDiscFiles(v, v.inputs.size());
            expect(look(v).ready(), "version 1");
            v.build = "t2";
            v.library = library(310000, 81);
            v.build();
            expect(InstallState.detect(v.files).ready(), "(the launcher's own look says ready: version 1's files are whole)");
            LauncherMenu pending = menuOf(v);
            expect(!look(v).ready(), "not ready");
            expectEquals("Finish setting up", pending.label(LauncherMenu.PRIMARY), "the first item");
            expectEquals("Not set up yet", row(playPage(pending, "Test Pad"), "Game Code"), "the Game Code row");
            // The disc files are all there, and the page still counts them while the app's own pieces wait for their update.
            expectEquals(v.inputs.size() + " of " + v.inputs.size() + " files", row(playPage(pending, "Test Pad"), "Game Data"), "the Game Data row");
            expectEquals(LauncherMenu.Action.OPEN_FIRST_SCREEN, pending.confirm(false), "A opens the first screen");
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "the update's first start");
            expect(look(v).ready(), "after it: ready, with the disc files kept");
            expectEquals("Play", menuOf(v).label(LauncherMenu.PRIMARY), "and the item is Play again");
        });

        check("a build without game code inside: the launcher's answer is its own, and the rule never gives Ready", () -> {
            ShipTestKit.Version v = firstStart("developer");
            placeDiscFiles(v, v.inputs.size());
            InstallState own = InstallState.detect(v.files);
            expectEquals(own.problem, InstallState.detect(v.files, ShipReady.of((ShipBundle) null, v.files)).problem, "no game code inside");
            expectEquals(own.problem, InstallState.detect(v.files, null).problem, "no answer at all");
            // not ready by the launcher's own look (the shader file is gone): the ship rule cannot make it ready
            File pack = new File(v.files, "private-trace/generic-shaders-v41.bin");
            byte[] bytes = Files.readAllBytes(pack.toPath());
            Files.delete(pack.toPath());
            expectEquals(InstallState.Problem.SHADERS_MISSING, InstallState.detect(v.files).problem, "the launcher's own look");
            expect(!InstallState.detect(v.files, ShipReady.of(v.bundle, v.files)).ready(), "never ready through the ship rule");
            Files.write(pack.toPath(), bytes);
            // an APK that cannot be found is not "ready"
            ShipReady unknown = ShipReady.of((String) null, v.files);
            expect(!InstallState.detect(v.files, unknown).ready(), "the app's own file unknown");
            LauncherMenu menu = new LauncherMenu(true, true);
            menu.setInstall(InstallState.detect(v.files, unknown), unknown);
            expectEquals("The app cannot read its own file.", menu.stateLine(), "and the page says so");
            // the developer build's page keeps its own first item, sentences and header
            LauncherMenu developer = new LauncherMenu(true, false);
            Files.delete(pack.toPath());
            developer.setInstall(InstallState.detect(v.files));
            MenuScenes.Page page = playPage(developer, "Test Pad");
            expectEquals("[Check Again]", Arrays.toString(page.column), "the developer build's first item");
            expectEquals(LauncherMenu.Action.RECHECK, developer.confirm(false), "which looks at the files again");
            expect(texts(page.status).contains("Private build"), "the developer build's header: " + texts(page.status));
        });

        check("the sentences for Play's reasons and for a start that went wrong name no install kit, no computer and no file of the app", () -> {
            String[][] cases = {
                    // After these the Play tab's first item reads Finish Setting Up: the sentence names that item, not a second way.
                    {ShipFirstRun.NOT_INSTALLED, "one of the app's own files is missing or was changed on this device. Choose Finish Setting Up on this page"},
                    {ShipFirstRun.CODE_CHANGED, "one of the app's own files is missing or was changed on this device. Choose Finish Setting Up on this page"},
                    {ShipFirstRun.PIECE_CHANGED, "one of the app's own files is missing or was changed on this device. Choose Finish Setting Up on this page"},
                    {ShipPlay.OTHER_REQUEST, "one of the app's own files is missing or was changed on this device. Choose Finish Setting Up on this page"},
                    {ShipPlay.DATA_MISSING_PREFIX + "18 of 39 game data files are missing", "your game data is not complete. Choose Set Up Game Files"},
                    {ShipPlay.NO_APP_FILE, "this app cannot read its own file. Install the app again over this one, without removing it first"},
                    {ShipPlay.APP_FILE_UNREADABLE_PREFIX + " (ZipException)", "this app cannot read its own file. Install the app again over this one, without removing it first"},
                    {LaunchRequest.ERROR_CODE_MISSING, "one of the app's own files is missing or was changed on this device. Choose Finish Setting Up on this page"},
                    {LaunchRequest.ERROR_SHADERS_MISSING, "one of the app's own files is missing or was changed on this device. Choose Finish Setting Up on this page"},
                    {LaunchRequest.ERROR_CODE_UNLOADABLE_PREFIX + "its file has more than one name", "Choose Finish Setting Up on this page"},
                    {LaunchRequest.ERROR_IO_PREFIX + "NoSuchFileException: /data/user/0/x/files/private-trace/run-1/arguments.bin)", "a file could not be read or written"},
                    {LaunchAdvice.cannotOpen("game window", "ActivityNotFoundException"), "Android would not open the game window (ActivityNotFoundException)"},
                    {null, "no reason was given"},
            };
            LauncherMenu menu = new LauncherMenu(true, true);
            for (String[] c : cases) {
                menu.startFailed(c[0]);
                String sentence = menu.failure();
                expectContains(sentence, c[1], "the sentence for <" + c[0] + ">");
                expect(sentence.startsWith("Not started: "), "it says the game was not started: " + sentence);
                // The menu cannot be closed with the controller, and coming back to it does not run the check.
                expect(!sentence.toLowerCase(java.util.Locale.ROOT).contains("close the app"), "no sentence sends the player to close the app: " + sentence);
                plain(sentence);
            }
            // what the game window left behind, as the launcher reads it on its return
            File files = ShipTestKit.newFilesDir("outcomes");
            String run = "0123456789abcdef0123456789abcdef";
            ShipTestKit.write(new File(files, "private-trace/run-" + run + "/arguments.bin"), new byte[] {1});
            plain(ShipAdvice.afterStart(LaunchOutcome.read(files, run, LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1)));   // never accepted
            ShipTestKit.write(new File(files, "private-trace/used-" + run), new byte[0]);
            for (String word : new String[] {"REJECT_LIBRARY_6", "REJECT_HASH", "REJECT_MANIFEST_FILE", "REJECT_NATIVE_RUN", "REJECT_SURFACE"}) {
                expect(GameWindowStatus.report(files, run, word), "(the game window reports " + word + ")");
                String said = ShipAdvice.afterStart(LaunchOutcome.read(files, run, LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY, -1));
                expect(said.startsWith("The game did not start: ") && said.endsWith(" (" + word + ")"), "a refusal is said, with the word for support: " + said);
                // The next step is one the controller can take: Play again (which checks the files before every start).
                expect(said.contains("Choose Play again") && !said.toLowerCase(java.util.Locale.ROOT).contains("close the app"),
                        "the next step can be taken with the controller: " + said);
                plain(said.substring(0, said.lastIndexOf(" (")));
            }
            for (LaunchOutcome.Exit exit : new LaunchOutcome.Exit[] {LaunchOutcome.Exit.CRASH, LaunchOutcome.Exit.LOW_MEMORY, LaunchOutcome.Exit.NOT_RESPONDING}) {
                File other = ShipTestKit.newFilesDir("outcome-exit");
                ShipTestKit.write(new File(other, "private-trace/used-" + run), new byte[0]);
                plain(ShipAdvice.afterStart(LaunchOutcome.read(other, run, exit, 40000)));
            }
            plain(ShipAdvice.controllerCheckFailed(LaunchAdvice.cannotOpen("controller check", "ActivityNotFoundException")));
        });

        check("the menu asks for a new whole read of the game code once after the game, the setup page and the first screen, and never otherwise", () -> {
            // Ready: Play is prepared and the game is started; the look after the return reads the files whole.
            ShipTestKit.Version v = firstStart("whole-look");
            placeDiscFiles(v, v.inputs.size());
            LauncherMenu menu = menuOf(v);
            expect(!menu.takeWholeLook(), "the first look of the menu after a start of the app: the first screen has just read the files");
            menu.shown();
            menu.move(1, false);
            menu.move(-1, false);
            expect(!menu.takeWholeLook(), "the screen shown again, the highlight moved: no new read");
            expectEquals(LauncherMenu.Action.OPEN_CONTROLLER_CHECK, menu.tap(LauncherMenu.CONTROLLER_CHECK, false), "(the controller check is opened)");
            expect(!menu.takeWholeLook(), "back from the controller check: no new read");
            expectEquals(LauncherMenu.Action.PLAY, menu.tap(LauncherMenu.PRIMARY, false), "(Play)");
            expect(!menu.takeWholeLook(), "while Play is being prepared nothing was started yet");
            expect(!menu.prepared("a game file could not be read or written (IOException)", false, true), "(the preparation fails)");
            expect(!menu.takeWholeLook(), "a start that was not made: no new read");
            expectEquals(LauncherMenu.Action.PLAY, menu.tap(LauncherMenu.PRIMARY, false), "(Play again)");
            expect(!menu.prepared(null, false, false), "(prepared, but the menu is no longer in front: the game is not started)");
            expect(!menu.takeWholeLook(), "no start: no new read");
            expectEquals(LauncherMenu.Action.PLAY, menu.tap(LauncherMenu.PRIMARY, false), "(Play a third time)");
            expect(menu.prepared(null, false, true), "(the game is started)");
            expect(menu.takeWholeLook(), "back from the game: this look reads the files whole");
            expect(!menu.takeWholeLook(), "once: the look after that does not");
            // Disc files missing: Set Up Game Files; the look after the return from that page reads once.
            ShipTestKit.Version discs = firstStart("whole-look-discs");
            LauncherMenu missing = menuOf(discs);
            expectEquals(LauncherMenu.Action.OPEN_GAME_FILES_SETUP, missing.confirm(false), "(the two-disc setup page is opened)");
            expect(missing.takeWholeLook() && !missing.takeWholeLook(), "back from the setup page: one whole read");
            // A piece of the app missing: Finish Setting Up; the look after the return from the first screen reads once.
            Files.delete(new File(discs.files, "private-trace/generic-shaders-v41.bin").toPath());
            LauncherMenu piece = menuOf(discs);
            expectEquals(LauncherMenu.Action.OPEN_FIRST_SCREEN, piece.confirm(false), "(the first screen is opened)");
            expect(piece.takeWholeLook() && !piece.takeWholeLook(), "back from the first screen: one whole read");
            // A developer build's "Check again" is a look like any other.
            LauncherMenu developer = new LauncherMenu(true, false);
            developer.setInstall(InstallState.detect(discs.files));
            expectEquals(LauncherMenu.Action.RECHECK, developer.confirm(false), "(Check again)");
            expect(!developer.takeWholeLook(), "Check again: no new read is asked for");
        });

        check("one of the app's own pieces missing: the page says Not set up yet, and still counts the disc files that are there", () -> {
            ShipTestKit.Version v = firstStart("piece-facts");
            placeDiscFiles(v, v.inputs.size());
            Files.delete(new File(v.files, "private-trace/generic-shaders-v41.bin").toPath());   // only the shader file is removed
            LauncherMenu menu = menuOf(v);
            MenuScenes.Page page = playPage(menu, "Test Pad");
            expectEquals("[Finish Setting Up]", Arrays.toString(page.column), "the first item");
            expectEquals("Not set up yet", row(page, "Game Code"), "the Game Code row (it read Not installed yet while the game code file was in place)");
            expectEquals(v.inputs.size() + " of " + v.inputs.size() + " files", row(page, "Game Data"),
                    "the Game Data row counts the player's files (it read Not checked yet)");
            expect(texts(page.status).contains("Game code not set up yet"), "the header's line: " + texts(page.status));
            ShipReady ship = ShipReady.of(v.bundle, v.files);
            expectEquals("Game code: not set up yet.\nGame data: " + v.inputs.size() + " of " + v.inputs.size() + " files from your two game discs.\nShader file: missing.",
                    ShipAdvice.aboutBuild(InstallState.detect(v.files, ship), ship), "About, Game Build");
            // The two-disc page counts alike in that state, and its Finish row sends the player to the menu's item.
            SetupDiscModel.Disk disk = SetupDiscModel.inspect(v.files, ship, new SetupDiscStore(v.files), SetupTestKit.PLENTY);
            expect(!disk.ready && disk.codeProblem != null && disk.codeProblem.contains("choose Finish Setting Up"), "the setup page's Finish row: " + disk.codeProblem);
        });

        ShipTestKit.finish("ShipLauncherReadyTest");
    }

    /** A sentence of the handed-out build: none of the words a player of that build has no use for. */
    private static void plain(String sentence) {
        expect(sentence != null && !sentence.isEmpty(), "a sentence is said");
        String found = ShipWordsTest.forbiddenIn(sentence);
        expect(found == null, "<" + found + "> in a sentence of the handed-out build: " + sentence);
    }
}
