package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectContains;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The words of the build that is handed out. A player of that build has the app and the images of two game discs:
 * no computer, no install kit, no owner's phone. So no screen of it says "private build", "this phone", "Thor", a
 * build number, "kit", "bundle", "switch file", "debug", "development", the name of a file of the app's own
 * storage, or (since the lead's decision of 2026-10-06) the name of a source file, a source folder or an identifier
 * of the code. This test reads what each screen shows in that build, as that build draws it (through ShipWords for the
 * menu's tabs), and looks for those words. The Play tab's sentences are read in ShipLauncherReadyTest and the
 * two-disc page's in SetupDiscModelTest; both use forbiddenIn() of this class or a list of their own.
 * Run with java -ea.
 */
public final class ShipWordsTest {
    /** Whole words and phrases, lower case. */
    private static final String[] WORDS = {"private build", "this phone", "thor", "kit", "bundle", "switch file",
            "private-trace", "debug", "development", "developer", "computer", "usb", "apk"};
    /** A build number (v464), and names of files of the app's own storage. */
    private static final Pattern[] SHAPES = {Pattern.compile("\\bv[0-9]{3}\\b"), Pattern.compile("\\bbuild v[0-9]"),
            Pattern.compile("[a-z0-9_-]+\\.(so|bin|txt|json)\\b"), Pattern.compile("\\bdiag-[a-z0-9-]+"),
            Pattern.compile("\\brun-[0-9a-f]{4,}"),
            // a source file's name (ffx_fsr1.h, direct_present_gsr.frag) and the name of a file of the game's discs (part3.rpf)
            Pattern.compile("[a-z0-9_-]+\\.(h|hpp|c|cc|cpp|frag|vert|comp|glsl|hlsl|spv|java|py|rpf|xex)\\b")};
    /**
     * An identifier of the code, in the text as written: lower-case words joined by "_" (third_party,
     * sgsr1_shader_mobile, _rcas). A status word in capitals (REJECT_NATIVE_RUN), which stands in brackets at the end
     * of a sentence for support, is not one.
     */
    private static final Pattern IDENTIFIER = Pattern.compile("(?<![A-Za-z0-9])[a-z0-9]*_[a-z0-9_]*[a-z0-9]");

    /** What a sentence that sends the player to install again must also say (see forbiddenIn). */
    static final String INSTALL_AGAIN_UNSAFE = "install again, without: over this one, without removing it first";

    /**
     * The first forbidden word or shape in a text, or null. A sentence that tells the player to install the app again
     * without saying "over this one, without removing it first" is refused as well: a player who removes the app
     * first loses the copied game files and the saves, whatever the sentence promises about them.
     */
    static String forbiddenIn(String text) {
        if (text == null) return null;
        String lower = text.toLowerCase(Locale.ROOT);
        if ((lower.contains("install the app again") || lower.contains("install it again"))
                && !(lower.contains("over this one") && lower.contains("without removing it first"))) {
            return INSTALL_AGAIN_UNSAFE;
        }
        for (String word : WORDS) {
            Matcher m = Pattern.compile("(?<![a-z0-9])" + Pattern.quote(word) + "(?![a-z0-9])").matcher(lower);
            if (m.find()) return word;
        }
        for (Pattern shape : SHAPES) {
            Matcher m = shape.matcher(lower);
            if (m.find()) return m.group();
        }
        Matcher identifier = IDENTIFIER.matcher(text);
        if (identifier.find()) return identifier.group();
        return null;
    }

    /** Everything a page shows, as text. */
    static List<String> textsOf(MenuScenes.Page page) {
        List<String> out = new ArrayList<String>();
        out.add(page.title);
        for (MenuScenes.Line line : page.status) out.add(line.text);
        out.addAll(Arrays.asList(page.tabs));
        if (page.column != null) out.addAll(Arrays.asList(page.column));
        for (MenuScenes.Line line : page.info) out.add(line.text);
        for (MenuScenes.Row row : page.rows) {
            out.add(row.label);
            out.add(row.value);
        }
        for (MenuScenes.Line line : page.help) out.add(line.text);
        if (page.text != null) out.addAll(page.text);
        for (MenuScenes.Prompt prompt : page.prompts) out.add(prompt.label);
        if (page.footerStatus != null) out.add(page.footerStatus.text);
        if (page.alert != null) {
            out.add(page.alert.title);
            out.add(page.alert.detail);
            out.addAll(Arrays.asList(page.alert.buttons));
        }
        return out;
    }

    /**
     * A text of the About notices without the licence texts of other people. Those are kept word for word
     * (ThirdPartyNoticesTest) and are not this app's sentences; the LGPL speaks of "the user's computer system".
     * Taken out as written and as the About tab joins their lines.
     */
    static String ownWords(String text) {
        for (String licence : new String[] {MoreNotices.XENIA, MoreNotices.REXGLUE, MoreNotices.XENONRECOMP, MoreNotices.SIMDE, MoreNotices.LGPL21,
                FontNotice.OFL}) {
            text = text.replace(licence, "").replace(Scene.reflow(licence), "");
        }
        return text;
    }

    private static int read(String where, List<String> texts) {
        int count = 0;
        for (String text : texts) {
            if (text == null || text.isEmpty()) continue;
            ++count;
            String found = forbiddenIn(text);
            expect(found == null, where + ": <" + found + "> in: " + text);
        }
        return count;
    }

    private static String build0(InstallState install) {
        return ShipAdvice.aboutBuild(install, null);
    }

    private static final int LEFT = NavInput.LEFT, RIGHT = NavInput.RIGHT, B = NavInput.BACK, Y = NavInput.ALT;

    /** Every page of the Settings tab a player can bring up with the standing game code: each row, each of its values. */
    private static List<MenuScenes.Page> settingsPages() {
        List<MenuScenes.Page> pages = new ArrayList<MenuScenes.Page>();
        Capabilities v464 = SettingsCoreTestKit.caps(SettingsCoreTestKit.V464_NAMES);
        for (int category = 0; category < SettingsPages.COUNT; ++category) {
            SettingsPagesTest.Tab tab = new SettingsPagesTest.Tab(v464, Settings.defaults(), true, new ArrayList<String>());
            for (int presses = 0; presses < category; ++presses) tab.press(NavInput.DOWN);
            pages.add(tab.page());   // the category's own description, highlight on its name
            int rows = SettingsPages.rows(tab.model, category).length;
            for (int slot = 0; slot < rows; ++slot) {
                SettingsPagesTest.Tab row = new SettingsPagesTest.Tab(v464, Settings.defaults(), true, new ArrayList<String>());
                row.into(category, slot);
                pages.add(row.page());
                for (int step = 0; step < 12; ++step) {   // every value to the right, then every value to the left
                    row.press(RIGHT);
                    pages.add(row.page());
                }
                for (int step = 0; step < 24; ++step) {
                    row.press(LEFT);
                    pages.add(row.page());
                }
                row.press(Y);        // "Put every setting back to the original?"
                pages.add(row.page());
                row.press(B);
                row.press(B);        // leaving with a changed value: "Save changes?"
                row.press(B);
                pages.add(row.page());
            }
        }
        return pages;
    }

    public static void main(String[] args) {
        check("the word list finds what it is for, and leaves ordinary words alone", () -> {
            String[][] cases = {
                    {"Measured on an AYN Thor: holds indoors.", "thor"}, {"The Thor's second screen", "thor"},
                    {"run the install kit again", "kit"}, {"Private build - not for distribution.", "private build"},
                    {"needs game build v463 or newer", "v463"}, {"the file private-trace-live.so", "private-trace"}, {"the shader file generic-shaders-v41.bin", "generic-shaders-v41.bin"},
                    {"A developer switch file overrules", "switch file"}, {"diag-pace-30 is present", "diag-pace-30"},
                    {"Connect this device to the computer", "computer"}, {"saved in settings.json", "settings.json"},
                    {"THE AUTHORS OR COPYRIGHT HOLDERS", null}, {"a thorough check", null}, {"the kitchen", null},
                    {"Version 0.1.0 (version code 1)", null}, {"16.4 GB", null},
                    {"FSR 1", null}, {"Draw Distance", null},
                    // source file names, source folders and identifiers (they stood in Licence Notices), and a disc file's own name
                    {"FSR 1: ffx_a.h, ffx_fsr1.h", "ffx_a.h"}, {"direct_present_fsr_easu.frag / _rcas.frag compiled into the library", "direct_present_fsr_easu.frag"},
                    {"vendored unmodified in third_party/fsr1", "third_party"}, {"Qualcomm's sgsr1_shader_mobile shaders", "sgsr1_shader_mobile"},
                    {"file 3 of 10, 545408A700000003/part3.rpf.", "part3.rpf"},
                    {"The game did not start: the game stopped while it was starting. (REJECT_NATIVE_RUN)", null},
                    {"(REJECT_IO_FileNotFoundException)", null}, {"AMD FidelityFX Super Resolution 1 (FSR 1)", null},
                    {"Source: https://github.com/SnapdragonStudios/snapdragon-gsr", null}, {"SPDX-License-Identifier: BSD-3-Clause", null},
                    // the four sentences as they stood: each promises that the saves are kept and does not say how
                    {"If it happens again, install the app again; your game data and your saves are kept.", INSTALL_AGAIN_UNSAFE},
                    {"This app cannot read its own file. Install the app again; your game data and your saves are kept.", INSTALL_AGAIN_UNSAFE},
                    {"Try again; if it happens again, install the app again.", INSTALL_AGAIN_UNSAFE},
                    {"If it happens again, install the app again over this one.", INSTALL_AGAIN_UNSAFE},
                    {"Install the app again over this one, without removing it first: your game data and your saves are then kept.", null}};
            for (String[] c : cases) expectEquals(c[1], forbiddenIn(c[0]), "in <" + c[0] + ">");
        });

        check("every sentence that sends the player to install again says how: over this one, without removing it first", () -> {
            List<String> sentences = new ArrayList<String>();
            // a start that was not made, for every reason Play's preparation gives
            for (String reason : new String[] {null, ShipFirstRun.NOT_INSTALLED, ShipFirstRun.CODE_CHANGED, ShipFirstRun.PIECE_CHANGED, ShipPlay.OTHER_REQUEST,
                    ShipPlay.NO_APP_FILE, ShipPlay.APP_FILE_UNREADABLE_PREFIX + " (ZipException)", LaunchRequest.ERROR_CODE_MISSING,
                    LaunchRequest.ERROR_SETTINGS_DIRECTORY, LaunchRequest.ERROR_IO_PREFIX + "IOException)", LaunchAdvice.cannotOpen("game window", "SecurityException")}) {
                sentences.add(ShipAdvice.startFailure(reason));
            }
            // a start that went wrong, as the menu says it afterwards
            sentences.addAll(Arrays.asList(ShipAdvice.OUTCOME_NOT_ACCEPTED, ShipAdvice.OUTCOME_REFUSED_FILES, ShipAdvice.OUTCOME_REFUSED_START));
            sentences.add(ShipAdvice.controllerCheckFailed(LaunchAdvice.cannotOpen("controller check", "ActivityNotFoundException")));
            sentences.add(ShipAdvice.HELP_APP_FILE);
            // the two-disc page's Finish row, and the first screen's failures
            for (ShipReady.Stands stands : ShipReady.Stands.values()) sentences.add(SetupDiscModel.codeProblem(stands));
            sentences.addAll(Arrays.asList(ShipSetupModel.FAILED_APP_FILE, ShipSetupModel.FAILED_WRITING, ShipSetupModel.FAILED_IN_THE_WAY,
                    ShipSetupModel.plainFailure("This app's own file cannot be read: ZipException. Install the app again."),
                    ShipSetupModel.plainFailure("This app's file is damaged: the game code inside it is missing. Install the app again.")));
            // what the developer build's sentences become in this build
            sentences.addAll(Arrays.asList(ShipWords.plain(LaunchAdvice.KIT_APP), ShipWords.plain("If it happens again, c" + LaunchAdvice.KIT_AGAIN.substring(1))));
            sentences.addAll(Arrays.asList(ShipWords.plain(LaunchAdvice.KIT_RUN), ShipWords.plain(LaunchAdvice.FAIL_CODE_MISSING), ShipAdvice.FINISH_FROM_ANY_TAB));
            // No sentence of this build sends the player to close the app: the menu cannot be closed with the controller,
            // and coming back to it from the home screen does not run the first screen's check.
            for (String sentence : sentences) {
                expect(sentence == null || !sentence.toLowerCase(Locale.ROOT).contains("close the app"), "it does not say to close the app: " + sentence);
            }
            int naming = 0;
            for (String sentence : sentences) {
                if (sentence == null) continue;
                expectEquals(null, forbiddenIn(sentence), "in <" + sentence + ">");
                if (sentence.toLowerCase(Locale.ROOT).contains("install the app again")) {
                    ++naming;
                    expect(sentence.contains("your game data and your saves are then kept"), "it says what is kept that way: " + sentence);
                }
            }
            expect(naming >= 8, "the sentences that name a reinstall were among them: " + naming);
            // the one sentence where only removing the app helps says what that costs
            expect(ShipSetupModel.FAILED_IN_THE_WAY.contains("removing it also removes your copied game data and your saves"), ShipSetupModel.FAILED_IN_THE_WAY);
        });

        check("Settings: every row, value, note, reason and question reads without those words, and no row is lost by the rewording", () -> {
            int texts = 0, pages = 0, named = 0;
            for (MenuScenes.Page page : settingsPages()) {
                int rows = page.rows.size();
                boolean before = false;
                for (String text : textsOf(page)) before |= text != null && text.toLowerCase(Locale.ROOT).contains("thor");
                if (before) ++named;
                ShipWords.page(page);
                expectEquals(rows, page.rows.size(), "the rows stay");
                texts += read("Settings", textsOf(page));
                ++pages;
            }
            expect(pages > 100 && texts > 1000, "the pages were really built: " + pages + " pages, " + texts + " texts");
            // (control) the developer build's pages do name the phone the figures were measured on: the rewording is what this test reads
            expect(named > 5, "pages that name the Thor before the rewording: " + named);
        });

        check("a measured note keeps what it says and only loses the phone's name", () -> {
            expectEquals("Measured on one test device: holds indoors.", ShipWords.plain(SettingsSchema.hint(SettingsSchema.FRAME_CAP, SettingsSchema.CAP_40)),
                    "the 40 fps note (its second sentence is about the Thor's second screen and is left out)");
            expectEquals("Measured on one test device: holds while driving (84 % of frames evenly paced).",
                    ShipWords.plain(SettingsSchema.hint(SettingsSchema.FRAME_CAP, SettingsSchema.CAP_30)), "the 30 fps note");
            expectEquals("Not available in this version.", ShipWords.plain("Not available: " + Capabilities.REASON_NEEDS_463), "a reason that named a build number, as one sentence (setup-fixes)");
            // a save that failed: the developer build names the file and the folder
            String failed = MenuModel.TEXT_SAVE_FAILED + "private-trace/diag-pace-30 could not be written (/data/user/0/dev.recompbootstrap.smoke/files/"
                    + "private-trace/diag-pace-30: Permission denied)";
            expectEquals("Could not save: a settings file could not be written. Check that this device has free storage, then save again.",
                    ShipWords.plain(failed), "a failed save");
            expectEquals("Could not save: The game is running. Close it first.", ShipWords.plain("Could not save: The game is running. Close it first."),
                    "a failure that names no file is said as it is");
            String untouched = "Shows the frame rate in the top right corner while the game runs.";
            expect(ShipWords.plain(untouched) == untouched, "a sentence without such words is not touched");
            expectEquals(null, ShipWords.plain(null), "nothing stays nothing");
        });

        check("About: This App, Game Build, Licence Notices, Graphics Self-Test and Captures", () -> {
            File files = ShipTestKit.newFilesDir("words-about");
            String version = "App version 0.1.0-ship3 (version code 1)";
            String names = AboutScreen.namesBlock(SettingsCoreTestKit.caps(SettingsCoreTestKit.V464_NAMES), null);
            List<String> texts = new ArrayList<String>();
            InstallState nothing = InstallState.detect(files);
            for (ShipReady ship : new ShipReady[] {ShipReady.of((String) null, files), null}) {
                String build = ShipAdvice.aboutBuild(nothing, ship);
                for (int section = 0; section < ShellPages.ABOUT_SECTIONS.length; ++section) {
                    for (String captures : new String[] {CaptureAbout.text(null, false, null), CaptureAbout.text(new long[] {12, 34000000, 3}, false, null),
                            CaptureAbout.text(new long[] {12, 34000000, 3}, true, null),
                            CaptureAbout.text(new long[] {12, 34000000, 0}, false, new CaptureExport.Result(3, 9, 12, null)),
                            CaptureAbout.text(new long[] {2, 5000000, 1}, false, new CaptureExport.Result(1, 0, 2, "run-0123456789abcdef0123456789abcdef/cap-002.png: ENOSPC")),
                            CaptureAbout.text(new long[] {2, 5000000, 2}, false, new CaptureExport.Result(0, 0, 2, "needs Android 10 or later"))}) {
                        texts.add(ownWords(ShipWords.plain(AboutScreen.sectionText(section, version, build, names, true, captures))));
                    }
                }
            }
            texts.addAll(Arrays.asList(ShellPages.ABOUT_SECTIONS));
            texts.add(ShellPages.PROMPT_COPY);
            expect(read("About", texts) > 40, "the sections were built");
            // This App in the handed-out build: the version and what the app is, as that build's own sentence (not a
            // developer sentence with its words taken out)
            expectEquals(version + "\nThis app runs your own copy of the game from the images of your two game discs.",
                    AboutScreen.sectionText(ShellPages.ABOUT_APP, version, build0(nothing), names, true, ""), "This App");
            // (control) the developer build's About does say what kind of build it is
            expectEquals("private build", forbiddenIn(AboutScreen.sectionText(ShellPages.ABOUT_APP, version, "", "")), "the developer build's This App");
            // Game Build lists what a player understands: the game's own code, the files from the discs, the shader file
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("words-build"));
            v.bundledPaths.addAll(Arrays.asList(ShipTestKit.GENERATED));
            v.build();
            expect(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).ok(), "first start");
            ShipReady ship = ShipReady.of(v.bundle, v.files);
            expectEquals("Game code: installed.\nGame data: 0 of 3 files from your two game discs.\nShader file: present.",
                    ShipAdvice.aboutBuild(InstallState.detect(v.files, ship), ship), "Game Build after a first start");
            expectEquals("Game code: not set up yet.\nGame data: not checked yet.\nShader file: missing.", ShipAdvice.aboutBuild(nothing, null), "before it");
        });

        check("Licence Notices in the handed-out build: both product names, every licence text in full, no source file, folder or identifier", () -> {
            File root = new File(System.getProperty("repo.root", "."));
            String shown = AboutScreen.licenceNoticesPlain();
            // what the About tab draws for the section in that build is this text (wrapped), not the developer build's
            expectEquals(Scene.reflow(shown), AboutScreen.sectionText(ShellPages.ABOUT_NOTICES, "v", "", "", true, ""), "the section in the handed-out build");
            expectEquals(Scene.reflow(AboutScreen.licenceNotices()), AboutScreen.sectionText(ShellPages.ABOUT_NOTICES, "v", "", ""), "the developer build keeps its text");
            for (String name : new String[] {"AMD FidelityFX Super Resolution 1", "Snapdragon Game Super Resolution"}) {
                expectContains(shown, name, "the product name");
            }
            // Every licence text in full, word for word, as the tracked licence files hold them.
            String amd = new String(Files.readAllBytes(new File(root, "third_party/fsr1/license.txt").toPath()), StandardCharsets.UTF_8);
            String both = new String(Files.readAllBytes(new File(root, "third_party/amd-fsr1/LICENSE").toPath()), StandardCharsets.UTF_8);
            String qualcomm = new String(Files.readAllBytes(new File(root, "third_party/snapdragon-gsr/LICENSE").toPath()), StandardCharsets.UTF_8);
            expect(amd.contains("Copyright (c) 2021 Advanced Micro Devices, Inc.") && amd.contains("THE SOFTWARE IS PROVIDED \"AS IS\""), "(AMD's licence file was read)");
            expectContains(shown, amd.trim(), "AMD's licence text, whole");
            String second = both.substring(both.indexOf("MIT LICENSE"));
            expect(second.contains("Copyright (c) 2014 Michal Drobot") && second.length() > 900, "(the second notice of AMD's files was read)");
            expectContains(shown, second.trim(), "the second notice AMD's files carry, whole");
            expectContains(shown, qualcomm.trim(), "Qualcomm's licence text, whole");
            expectContains(shown, "Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.", "the copyright line of the parts that are used");
            // None of what the developer build's headings name. The rule is for this app's own sentences: the licence
            // texts of MoreNotices are other people's words, kept word for word (ThirdPartyNoticesTest), and the LGPL
            // speaks of "the user's computer system". They are taken out before the rule reads the rest.
            for (String text : new String[] {MoreNotices.XENIA, MoreNotices.REXGLUE, MoreNotices.XENONRECOMP, MoreNotices.SIMDE, MoreNotices.LGPL21,
                FontNotice.OFL}) {
                expect(shown.contains(text), "(a further licence text is shown)");
            }
            String own = ownWords(shown);
            expect(shown.length() - own.length() > 30000, "(the licence texts were taken out: more than 30,000 characters)");
            expectEquals(null, forbiddenIn(own), "a forbidden word or shape in the app's own sentences");
            // the About tab joins the lines of the texts; taken out of that form too, the rest passes the same rule
            String drawn = AboutScreen.sectionText(ShellPages.ABOUT_NOTICES, "v", "", "", true, "");
            expect(drawn.length() - ownWords(drawn).length() > 30000, "(the joined licence texts were taken out as well)");
            expectEquals(null, forbiddenIn(ownWords(drawn)), "a forbidden word or shape in what the About tab draws besides the licence texts");
            expect(forbiddenIn(shown) != null, "(control) with the licence texts left in, the rule finds a word of theirs");
            for (String name : new String[] {"Xenia", "ReXGlue", "FFmpeg", "XenonRecomp", "SIMDe", "Android NDK runtime libraries"}) {
                expectContains(own, name, "a further product name, in the app's own sentences");
            }
            // (control) a source file's name in one of the app's own sentences would be found.
            expect(forbiddenIn(own.replace("its graphics helpers", "xenos_texture_2d.cpp")) != null, "(control) a source file named in an own sentence is refused");
            for (String internal : new String[] {"ffx_", ".frag", "third_party", "FsrEasuCon", "FsrRcasCon", "EASU", "RCAS", "sgsr1", "direct_present",
                    "vendored", "presenter", "compiled into the library", "LICENSE)"}) {
                expect(!shown.contains(internal), "the text names no <" + internal + ">");
            }
            // (control) the developer build's text does name them: the rule finds what it is for.
            expect(forbiddenIn(AboutScreen.licenceNotices()) != null, "the developer build's notices are refused by the same rule");
        });

        check("the graphics test screen of the handed-out build shows the lines about the picture and none of the developer's", () -> {
            // The native test's report, as android/app/src/main/cpp/smoke_runtime.cpp writes it.
            String report = "Native library loaded\nABI: arm64-v8a\nLifecycle: resumed\nRenderer: presenting\nSurface: 1920x1080\n"
                    + "Vulkan: 1.3, frames presented\nGuest address space: reserved (4 GiB virtual)\nGame executable: not loaded; no guest execution";
            expectEquals("Renderer: presenting\nSurface: 1920x1080\nVulkan: 1.3, frames presented", ShipWords.selfTest(report), "the three lines about the picture");
            String shown = ShipWords.selfTest(report);
            for (String developer : new String[] {"Native library", "ABI", "Lifecycle", "Guest", "Game executable", "no guest execution"}) {
                expect(!shown.contains(developer), "no <" + developer + "> on the screen");
            }
            expectEquals(null, forbiddenIn(shown), "and no forbidden word");
            expectEquals(ShipWords.SELF_TEST_FAILED, ShipWords.selfTest("Native runtime initialization failed"), "a test that could not start is said plainly");
            expectEquals(ShipWords.SELF_TEST_FAILED, ShipWords.selfTest(null), "also without any report");
            expectEquals(null, forbiddenIn(ShipWords.SELF_TEST_FAILED), "in plain words");
            // The report's own lines are the ones the native code writes: a renamed line would silently vanish from the screen.
            String native0 = new String(Files.readAllBytes(new File(System.getProperty("repo.root", "."), "android/app/src/main/cpp/smoke_runtime.cpp").toPath()),
                    StandardCharsets.UTF_8);
            for (String kept : new String[] {"\"Renderer: \"", "\"Surface: \"", "\"Vulkan: \""}) {
                expect(native0.contains(kept), "the native report writes a line that begins " + kept);
            }
        });

        check("the Controller tab, the first screen, the game window and the controller check", () -> {
            List<String> texts = new ArrayList<String>();
            for (String controller : new String[] {null, "Xbox Wireless Controller"}) {
                LauncherMenu menu = new LauncherMenu(true, true);
                menu.setInstall(InstallState.detect(ShipTestKit.newFilesDir("words-controller")), null);
                menu.controllerCheckFailed(LaunchAdvice.cannotOpen("controller check", "ActivityNotFoundException"));
                ShellNav nav = new ShellNav();
                nav.command(NavInput.PAGE_NEXT, false, false, false);
                nav.command(NavInput.PAGE_NEXT, false, false, false);
                MenuScenes.Page page = ShellPages.frame(nav, menu, controller, TimeZone.getTimeZone("UTC"));
                ShellPages.controller(page, nav, menu, controller);
                ShipWords.page(page);
                texts.addAll(textsOf(page));
            }
            ShipSetupModel first = new ShipSetupModel();
            texts.add(first.heading());
            texts.add(first.text());
            first.prepareStarted();
            for (ShipFirstRun.Step step : ShipFirstRun.Step.values()) {
                first.prepareProgress(step, 120000000L, 339203240L);
                texts.add(first.text());
            }
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("words-first")).build();
            first.prepareFailed(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.spaceOf(1000)).error);   // no room
            texts.addAll(Arrays.asList(first.heading(), first.text(), first.primary()));
            v.bundle.assets.put("ship/shader-pack.bin", ShipTestKit.bytes(v.pack.length, 99));                            // a damaged app file
            first.prepareFailed(ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).error);
            texts.add(first.text());
            first.prepareFailed(ShipStart.GAME_RUNNING);
            texts.add(first.text());
            for (String word : new String[] {"CHECKING_PRIVATE_LIBRARY", "RUNNING_GUEST", "REJECT_HASH", "TRACE_RETURNED_PROTOCOL_SAVED"}) {
                texts.add(GameWindowStatus.label(word));
            }
            texts.add(ControllerCheckText.unreadable());
            texts.add(ControllerCheckText.screen("AYN Thor / Odin -> Xbox 360, player 1\nOdin Controller (id 5)\nRight stick: AXIS_Z / AXIS_RZ",
                    0x1000, 0, 255, 0, 0, 0, 0, 0x1000, false, true, 3));
            texts.add(ControllerCheckText.screen(null, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0));
            // a file in the place of one of the app's folders, and every line the game window's capture corner can show
            texts.add(ShipSetupModel.plainFailure("Could not put the game code in place: inputs" + ShipFiles.NOT_A_FOLDER + "."));
            texts.addAll(Arrays.asList(CaptureBadge.TOO_SOON, CaptureBadge.BUSY, CaptureBadge.NO_PICTURE_YET, CaptureBadge.NO_MEMORY,
                    CaptureBadge.NO_STORAGE, CaptureBadge.FAILED, CaptureBadge.saved(12), CaptureBadge.notSaved(12), CaptureBadge.noPicture(12),
                    CaptureBadge.removedOldest(3), CaptureBadge.refused(true), CaptureBadge.refused(false)));
            texts.add(CaptureAbout.STOPPED_REFUSED);
            texts.add(CaptureAbout.STOPPED_WRITING);
            expect(read("other screens", texts) > 20, "the screens were built");
        });

        check("the menu draws every tab through the rewording in the build that is handed out, and only there", () -> {
            // -Dapp.main names the folder of the app's main sources (the test runner sets it); else it is found under -Drepo.root.
            String main = System.getProperty("app.main");
            if (main == null && System.getProperty("repo.root") != null) {
                main = System.getProperty("repo.root") + "/android/app/src/main/java/dev/recompbootstrap/smoke";
            }
            expect(main != null, "-Dapp.main or -Drepo.root says where the app's sources are");
            String screen = new String(Files.readAllBytes(new File(main, "LauncherScreen.java").toPath()), StandardCharsets.UTF_8).replaceAll("\\s+", "");
            int reworded = screen.indexOf("if(menu.selfInstalling())ShipWords.page(page);");
            // (the draw call takes the view's frame as a third argument since the menu redesign: either form is the draw)
            int drawn = screen.indexOf("view.show(MenuScenes.build(page,view");
            expect(drawn > 0 && reworded > 0 && reworded < drawn,
                    "LauncherScreen.render passes the page through ShipWords.page before it is drawn, when the build carries its game code");
            expect(screen.indexOf("ShipWords.page(page)") == screen.lastIndexOf("ShipWords.page(page)"), "in one place, under that condition");
        });

        ShipTestKit.finish("ShipWordsTest");
    }
}
