package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.V463_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.V464_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.age;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.ascii;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.begin;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.bytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.caps;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.differences;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.end;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.minus;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.phoneFiles;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.put;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.sameBytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.snapshot;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.tempDir;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.v463;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.v464;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/**
 * Host test of the Render Resolution row under a game build with render-resolution stage B (the marker
 * recomp-cap:render-stage-b, in the library from v464), against a build without it (v463), and of the Frame Rate Cap
 * notes, which in such a build follow the render size chosen with the cap. It drives the settings tab the way the app
 * does (SettingsPagesTest.Tab: ShellNav, MenuModel, SettingsPages) and looks at what the frame would draw - the row's
 * value, its colour, whether a row is grey, the help strip - and, for the files, at real directories through
 * SettingsStore. Run with java -ea.
 */
public final class RenderStageBTest {
    private static final List<String> NOTHING = new ArrayList<String>();
    private static final int LEFT = NavInput.LEFT, RIGHT = NavInput.RIGHT, A = NavInput.CONFIRM, START = NavInput.MENU;
    private static final String RENDER = SettingsSchema.RENDER_SCALE, SCALER = SettingsSchema.SCALER, SHARPNESS = SettingsSchema.FSR_SHARPNESS,
            CAP = SettingsSchema.FRAME_CAP;
    /** The rows of the Display category, top to bottom. */
    private static final int ROW_SCALER = 0, ROW_SHARPNESS = 1, ROW_RENDER = 2;
    /** The Frame Rate Cap row of the Frame Rate category. */
    private static final int ROW_CAP = 0;
    /** Values of renderScalePercent: the places of 50, 75, 88, 100, 115, 125 and 150 among its ids. */
    private static final int P50 = 0, P75 = 1, P88 = 2, P100 = 3, P115 = 4, P125 = 5, P150 = 6;

    private static final String RESTART = "A change needs a restart of the game.";
    private static final String NATIVE_HELP = "The game draws every pixel of this screen. Sharpest picture. Measured on an AYN Thor: about 3 ms more per"
            + " frame (49 to 43 fps indoors), slightly more power. Scaler and sharpness do nothing here.";
    private static final String PERFORMANCE_HELP = "Fewer pixels, enlarged by the scaler. Measured on an AYN Thor: about half a millisecond faster,"
            + " little or no power saved. Softer than Quality.";
    private static final String JSON = "settings/settings.json", RENDER_TXT = "settings/render-scale.txt", SCALER_TXT = "settings/scaler.txt",
            SHARPNESS_TXT = "settings/fsr-sharpness.txt";

    private static final SettingsStore.GameRunning NOT_RUNNING = new SettingsStore.GameRunning() {
        @Override public boolean check() { return false; }
    };

    /** The settings tab of a build, opened on saved settings, with the highlight on a row of Display. */
    private static SettingsPagesTest.Tab display(Capabilities capabilities, Settings saved, int row) {
        SettingsPagesTest.Tab tab = new SettingsPagesTest.Tab(capabilities, saved, true, NOTHING);
        tab.into(SettingsPages.DISPLAY, row);
        return tab;
    }

    private static Settings at(int render) {
        return Settings.defaults().with(RENDER, render);
    }

    /** The settings tab of a build, opened on saved settings, with the highlight on the Frame Rate Cap row. */
    private static SettingsPagesTest.Tab frameRate(Capabilities capabilities, Settings saved) {
        SettingsPagesTest.Tab tab = new SettingsPagesTest.Tab(capabilities, saved, true, NOTHING);
        tab.into(SettingsPages.FRAME_RATE, ROW_CAP);
        return tab;
    }

    private static MenuScenes.Row row(SettingsPagesTest.Tab tab, int row) {
        return tab.page().rows.get(row);
    }

    /** The help strip as the frame gets it: one line of text per part. */
    private static List<String> help(SettingsPagesTest.Tab tab) {
        List<String> lines = new ArrayList<String>();
        for (MenuScenes.Line line : tab.page().help) lines.add(line.text);
        return lines;
    }

    /** Every value the highlighted row can be stepped to: LEFT to the end, then RIGHT to the other end. */
    private static List<String> steps(SettingsPagesTest.Tab tab, int row) {
        for (int presses = 0; presses < 10; ++presses) tab.press(LEFT);
        List<String> seen = new ArrayList<String>();
        seen.add(row(tab, row).value);
        for (int presses = 0; presses < 10 && row(tab, row).canRight; ++presses) {
            tab.press(RIGHT);
            seen.add(row(tab, row).value);
        }
        return seen;
    }

    /** Start, as the settings screen handles it: the settings the model hands to the store. */
    private static Settings startSave(SettingsPagesTest.Tab tab) {
        tab.press(START);
        same(MenuModel.ACTION_SAVE, tab.model.pendingAction(), "Start asks for a save");
        return tab.model.beginSave();
    }

    private static SettingsStore store(File files) {
        return new SettingsStore(files, NOT_RUNNING);
    }

    private static void save(File files, Settings settings, Capabilities capabilities) {
        SettingsStore.SaveResult result = store(files).save(settings, capabilities);
        check(result.ok && result.error == null, "save failed: " + result.error);
    }

    public static void main(String[] args) {
        begin("RenderStageBTest");

        test("which builds get the five modes: the marker together with the file's name; a v463 build keeps its seven steps and texts", () -> {
            SettingsPagesTest.Tab old = display(v463(), Settings.defaults(), ROW_RENDER);
            same(Arrays.asList("50 % (640x360)", "75 % (960x540, Performance)", "88 % (1120x630, Balanced)", "100 % (1280x720, original)",
                    "115 % (1440x810, supersampling)", "125 % (1600x900, supersampling)", "150 % (1920x1080, supersampling)"), steps(old, ROW_RENDER),
                    "v463 (the file's name, no marker): the steps");
            same(Arrays.asList("The size the game draws at before the picture is enlarged.",
                    "Supersampling through a 1280x720 picture: smoother edges, more heat. Not native 1080p."), help(old), "v463 at 150 %: the help strip");
            check(row(old, ROW_SCALER).enabled && row(old, ROW_SHARPNESS).enabled, "v463 at 150 %: Scaler and FSR Sharpness stay in use (the scaler works there)");

            SettingsPagesTest.Tab held = display(caps(minus(V464_NAMES, "render-scale.txt")), Settings.defaults(), ROW_RENDER);
            same("100 % (1280x720, original)", row(held, ROW_RENDER).value, "the marker without the file's name: the old text");
            check(!row(held, ROW_RENDER).enabled, "and the row cannot be used");
            same(Arrays.asList("The size the game draws at before the picture is enlarged.",
                    "Not available: Not in the installed game build (needs game build v463 or newer)"), help(held), "with the reason, which names the first build that has it");

            same(Arrays.asList("Performance: 960x540, upscaled", "Balanced: 1120x630, upscaled", "Quality: 1280x720, original", "Ultra: 1440x810, upscaled",
                    "Native: 1920x1080, no upscaling"), steps(display(v464(), Settings.defaults(), ROW_RENDER), ROW_RENDER), "v464 (file and marker): the steps");
        });

        test("the five modes: the default is Quality, LEFT stops at Performance, RIGHT at Native, A goes round the five", () -> {
            SettingsPagesTest.Tab tab = display(v464(), Settings.defaults(), ROW_RENDER);
            same("Quality: 1280x720, original", row(tab, ROW_RENDER).value, "nothing saved: the default");
            same(MenuScenes.TONE_NORMAL, row(tab, ROW_RENDER).valueTone, "in the plain colour");
            List<String> round = new ArrayList<String>();
            for (int presses = 0; presses < 5; ++presses) {
                tab.press(A);
                round.add(row(tab, ROW_RENDER).value);
            }
            same(Arrays.asList("Ultra: 1440x810, upscaled", "Native: 1920x1080, no upscaling", "Performance: 960x540, upscaled",
                    "Balanced: 1120x630, upscaled", "Quality: 1280x720, original"), round, "A five times");
            tab.press(LEFT);
            tab.press(LEFT);
            check(!row(tab, ROW_RENDER).canLeft && row(tab, ROW_RENDER).canRight, "at Performance the left arrow is faint");
            tab.press(LEFT);
            same("Performance: 960x540, upscaled", row(tab, ROW_RENDER).value, "LEFT at Performance changes nothing (50 % is not offered)");
            for (int presses = 0; presses < 4; ++presses) tab.press(RIGHT);
            check(row(tab, ROW_RENDER).canLeft && !row(tab, ROW_RENDER).canRight, "at Native the right arrow is faint");
            same(P150, startSave(tab).get(RENDER), "what a Save at Native hands to the store: 150");
        });

        test("help strip with FSR 1: what each mode is with what was measured for it (nothing at Quality, the reference), then the restart sentence once", () -> {
            String[][] expected = {
                    {"Performance: 960x540, upscaled", PERFORMANCE_HELP},
                    {"Balanced: 1120x630, upscaled", "Fewer pixels than the original, then enlarged by the scaler. Measured on an AYN Thor: no faster than Quality indoors."},
                    {"Quality: 1280x720, original", "The original game's size, enlarged by the scaler."},
                    {"Ultra: 1440x810, upscaled", "More pixels than the original, then enlarged by the scaler. Measured on an AYN Thor: about 1 to 1.5 ms more per frame indoors."},
                    {"Native: 1920x1080, no upscaling", NATIVE_HELP}};
            SettingsPagesTest.Tab tab = display(v464(), at(P75), ROW_RENDER);
            for (String[] mode : expected) {
                same(mode[0], row(tab, ROW_RENDER).value, "the mode");
                same(Arrays.asList(mode[1], RESTART), help(tab), "the help strip at " + mode[0]);
                for (MenuScenes.Line line : tab.page().help) same(MenuScenes.TONE_NORMAL, line.tone, "in the plain colour at " + mode[0]);
                tab.press(RIGHT);
            }
        });

        test("scaler dependence: with Bicubic or Nearest, Performance, Balanced and Ultra say that the picture goes through 1280x720 first", () -> {
            for (int scaler = SettingsSchema.SCALER_FSR1; scaler <= SettingsSchema.SCALER_NEAREST; ++scaler) {
                String name = scaler == SettingsSchema.SCALER_BICUBIC ? "Bicubic" : scaler == SettingsSchema.SCALER_NEAREST ? "Nearest" : null;
                for (int mode : new int[] {P75, P88, P100, P115, P150}) {
                    SettingsPagesTest.Tab tab = display(v464(), at(mode).with(SCALER, scaler), ROW_RENDER);
                    List<String> help = help(tab);
                    String where = row(tab, ROW_RENDER).value + " with " + row(tab, ROW_SCALER).value;
                    boolean says = name != null && mode != P100 && mode != P150;
                    same(says ? 3 : 2, help.size(), "lines at " + where + ": " + help);
                    if (says) same("With " + name + " as the scaler the picture goes through 1280x720 first: softer.", help.get(1), "the second line at " + where);
                    same(RESTART, help.get(help.size() - 1), "the last line at " + where);
                    int lines = 0;
                    for (String part : help) lines += Scene.wrap(part, MenuLayout.HELP_PX, Scene.REGULAR, MenuLayout.paneWidth(true) - 2 * MenuLayout.ROW_PAD, MenuLayoutTest.FIXED).size();
                    check(lines <= MenuLayout.HELP_LINES, "the strip holds the whole text at the test's fixed character width, at " + where + ": " + lines + " lines");
                }
            }
            // The sentence follows the scaler chosen in this visit, saved or not: the game will run what a Save writes.
            SettingsPagesTest.Tab chosen = display(v464(), at(P75), ROW_SCALER);
            chosen.press(RIGHT);
            chosen.press(RIGHT);
            same("Bicubic", row(chosen, ROW_SCALER).value, "FSR 1 saved, two steps right on Scaler");
            check(row(chosen, ROW_SCALER).marked, "not saved yet");
            chosen.into(SettingsPages.DISPLAY, ROW_RENDER);
            same(Arrays.asList(PERFORMANCE_HELP, "With Bicubic as the scaler the picture goes through 1280x720 first: softer.", RESTART), help(chosen),
                    "Performance with Bicubic chosen but not saved");
            SettingsPagesTest.Tab back = display(v464(), at(P75).with(SCALER, SettingsSchema.SCALER_BICUBIC), ROW_SCALER);
            back.press(LEFT);
            back.press(LEFT);
            same("FSR 1 (default)", row(back, ROW_SCALER).value, "Bicubic saved, two steps left on Scaler");
            back.into(SettingsPages.DISPLAY, ROW_RENDER);
            same(Arrays.asList(PERFORMANCE_HELP, RESTART), help(back), "Performance with Bicubic saved but FSR 1 chosen");

            // A saved Bicubic that the build cannot use (the game then runs FSR 1): the sentence would be false.
            SettingsPagesTest.Tab none = display(caps("render-scale.txt", "recomp-cap:render-stage-b"), at(P75).with(SCALER, SettingsSchema.SCALER_BICUBIC), ROW_RENDER);
            same(Arrays.asList(PERFORMANCE_HELP, RESTART), help(none), "a build that reads no scaler input, Bicubic saved under another build");
        });

        test("Native: Scaler and FSR Sharpness are grey with their reason, take no change and keep their saved values; off Native they are back", () -> {
            Settings saved = Settings.defaults().with(SHARPNESS, 7);
            SettingsPagesTest.Tab tab = display(v464(), saved, ROW_RENDER);
            check(row(tab, ROW_SCALER).enabled && row(tab, ROW_SHARPNESS).enabled, "at Quality both rows are in use");
            tab.press(RIGHT);
            check(row(tab, ROW_SCALER).enabled && row(tab, ROW_SHARPNESS).enabled, "at Ultra too");
            tab.press(RIGHT);
            same("Native: 1920x1080, no upscaling", row(tab, ROW_RENDER).value, "two steps right");
            String[] what = {"How the game picture is enlarged to fill this screen.", "How strongly FSR 1 sharpens the enlarged picture."};
            for (int grey : new int[] {ROW_SCALER, ROW_SHARPNESS}) {
                tab.into(SettingsPages.DISPLAY, grey);
                MenuScenes.Page page = tab.page();
                check(!page.rows.get(grey).enabled, page.rows.get(grey).label + " is grey at Native");
                same(Arrays.asList(what[grey], "Not available: Not used at Native"), help(tab), page.rows.get(grey).label + ": what the row is, then the reason");
                for (MenuScenes.Prompt prompt : page.prompts) check(!prompt.label.equals("Select"), "no Select prompt on a grey row");
                String before = page.rows.get(grey).value;
                for (int command : new int[] {LEFT, RIGHT, A}) tab.press(command);
                tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ARROW_RIGHT, grey);
                tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, grey);
                same(before, row(tab, grey).value, page.rows.get(grey).label + " after LEFT, RIGHT, A and two taps");
                check(!row(tab, grey).marked, "it does not count as changed");
            }
            same("FSR 1 (default)", row(tab, ROW_SCALER).value, "the Scaler row still shows the saved scaler");
            Settings written = startSave(tab);
            same(saved.with(RENDER, P150), written, "a Save at Native hands over the saved scaler and sharpness, and 150");
            tab.model.saveFinished(true, false, null);
            tab.into(SettingsPages.DISPLAY, ROW_RENDER);
            tab.press(LEFT);
            check(row(tab, ROW_SCALER).enabled && row(tab, ROW_SHARPNESS).enabled, "one step left (Ultra): both rows are in use again");

            // With another scaler than FSR 1 the sharpness row is grey anyway; at Native the reason is Native's.
            SettingsPagesTest.Tab bicubic = display(v464(), at(P150).with(SCALER, SettingsSchema.SCALER_BICUBIC), ROW_SHARPNESS);
            same(Arrays.asList("How strongly FSR 1 sharpens the enlarged picture.", "Not available: Not used at Native"), help(bicubic), "Bicubic at Native: the sharpness row's reason");
            same("Bicubic", row(bicubic, ROW_SCALER).value, "and the Scaler row shows Bicubic, grey");
            SettingsPagesTest.Tab ultra = display(v464(), at(P115).with(SCALER, SettingsSchema.SCALER_BICUBIC), ROW_SHARPNESS);
            same(Arrays.asList("How strongly FSR 1 sharpens the enlarged picture.", "Not available: Only used by FSR 1"), help(ultra), "Bicubic at Ultra: the older reason");
            check(row(ultra, ROW_SCALER).enabled, "and the Scaler row can be changed");
        });

        test("a saved 125 % or 50 %: shown with its size in the warning colour, said not to be a mode, left by LEFT / RIGHT and not offered again", () -> {
            SettingsPagesTest.Tab tab = display(v464(), at(P125).with(SCALER, SettingsSchema.SCALER_BICUBIC), ROW_RENDER);
            MenuScenes.Page page = tab.page();
            same("125 %: 1600x900, upscaled", page.rows.get(ROW_RENDER).value, "a saved 125 %");
            same(MenuScenes.TONE_WARNING, page.rows.get(ROW_RENDER).valueTone, "in the warning colour");
            check(page.rows.get(ROW_RENDER).enabled && page.rows.get(ROW_RENDER).canLeft && page.rows.get(ROW_RENDER).canRight, "the row can be used, both ways");
            same(Arrays.asList("Not one of the five modes. More pixels than the original, then enlarged by the scaler.",
                    "With Bicubic as the scaler the picture goes through 1280x720 first: softer.", RESTART), help(tab), "125 % with Bicubic: the help strip");
            same(MenuScenes.TONE_WARNING, page.help.get(0).tone, "its first line in the warning colour");
            same(MenuScenes.TONE_NORMAL, page.help.get(1).tone, "the rest plain");
            check(!tab.model.dirty(), "showing it changes nothing");
            check(page.rows.get(ROW_SCALER).enabled, "at 125 % the scaler works, so the Scaler row can be used");
            check(row(display(v464(), at(P125), ROW_RENDER), ROW_SHARPNESS).enabled, "and with FSR 1 the FSR Sharpness row too");
            tab.press(RIGHT);
            same("Native: 1920x1080, no upscaling", row(tab, ROW_RENDER).value, "RIGHT from 125 %");
            tab.press(LEFT);
            same("Ultra: 1440x810, upscaled", row(tab, ROW_RENDER).value, "LEFT from Native: Ultra, 125 % is not offered again");
            same(MenuScenes.TONE_NORMAL, tab.page().help.get(0).tone, "a mode's help is plain");

            SettingsPagesTest.Tab low = display(v464(), at(P50).with(SCALER, SettingsSchema.SCALER_BICUBIC), ROW_RENDER);
            same("50 %: 640x360, upscaled", row(low, ROW_RENDER).value, "a saved 50 %");
            same(MenuScenes.TONE_WARNING, row(low, ROW_RENDER).valueTone, "in the warning colour");
            same(Arrays.asList("Not one of the five modes. The picture goes through 1280x720 first: softer.", RESTART), help(low),
                    "50 %: it goes through 1280x720 with every scaler, said once");
            check(!row(low, ROW_RENDER).canLeft && row(low, ROW_RENDER).canRight, "50 % is the lowest value: only RIGHT");
            low.press(RIGHT);
            low.press(LEFT);
            same("Performance: 960x540, upscaled", row(low, ROW_RENDER).value, "RIGHT then LEFT: Performance, 50 % is not offered again");
        });

        test("offered and supported: they differ only for 50 % and 125 % of a stage B build; those two stay supported", () -> {
            Capabilities[] builds = {SettingsCoreTestKit.v458(), v463(), v464(), caps(SettingsCoreTestKit.plus(SettingsCoreTestKit.ALL_NAMES, "recomp-cap:render-stage-b"))};
            boolean[] stageB = {false, false, true, true};
            for (int b = 0; b < builds.length; ++b) {
                same(stageB[b], builds[b].renderStageB(), "stage B, build " + b);
                for (SettingSpec spec : SettingsSchema.all()) {
                    for (int value = spec.min; value <= spec.max; value += spec.step) {
                        boolean notAMode = stageB[b] && spec.key.equals(RENDER) && (value == P50 || value == P125);
                        if (notAMode) check(builds[b].valueSupported(spec.key, value), "build " + b + ": " + spec.format(value) + " is still supported");
                        same(builds[b].valueSupported(spec.key, value) && !notAMode, builds[b].offered(spec.key, value), "build " + b + ", " + spec.label + " " + spec.format(value) + ": offered");
                    }
                }
            }
        });

        test("the marker changes nothing a Save writes: the same files and the same settings.json as under v463, for every render size", () -> {
            Settings[] choices = {Settings.defaults(), at(P50), at(P75).with(SCALER, SettingsSchema.SCALER_BICUBIC), at(P88), at(P115).with(SHARPNESS, 7),
                    at(P125).with(SCALER, SettingsSchema.SCALER_NEAREST), at(P150).with(SCALER, SettingsSchema.SCALER_GSR1).with(SHARPNESS, 12)};
            String[] renderFile = {null, "50\n", "75\n", "88\n", "115\n", "125\n", "150\n"};
            SettingsCoreTestKit.MemoryFiles[] states = {new SettingsCoreTestKit.MemoryFiles(),
                    new SettingsCoreTestKit.MemoryFiles().put(RENDER_TXT, "125\n").put(SCALER_TXT, "cubic\n").put(SHARPNESS_TXT, "0.7\n")};
            for (int c = 0; c < choices.length; ++c) {
                for (SettingsCoreTestKit.MemoryFiles files : states) {
                    Map<String, byte[]> withMarker = RuntimeMapping.plan(choices[c], v464(), files), without = RuntimeMapping.plan(choices[c], v463(), files);
                    same(new ArrayList<String>(without.keySet()), new ArrayList<String>(withMarker.keySet()), "choice " + c + ": the planned files");
                    for (String path : without.keySet()) sameBytes(without.get(path), withMarker.get(path), "choice " + c + ": " + path);
                    sameBytes(renderFile[c] == null ? null : ascii(renderFile[c]), withMarker.get(RENDER_TXT), "choice " + c + ": render-scale.txt holds the bare percentage (absent for 100)");
                }
                same(SettingsJson.encode(choices[c], v463()), SettingsJson.encode(choices[c], v464()), "choice " + c + ": settings.json");
            }
        });

        test("first visit under v464 (scan, load, Save at the defaults): the scan finds the marker, two files appear, none changes; a second Save writes nothing", () -> {
            File files = phoneFiles(tempDir(), V464_NAMES);
            Map<String, String> before = snapshot(files);
            Capabilities capabilities = CapabilityScanner.load(files, true);
            check(capabilities.renderStageB(), "the app's own scan of the library finds the marker");
            check(!CapabilityScanner.load(phoneFiles(tempDir(), V463_NAMES), true).renderStageB(), "and does not in a library without it");
            SettingsStore.Loaded loaded = store(files).load(capabilities);
            same(Settings.defaults(), loaded.settings, "the phone's standing files are the defaults");
            save(files, loaded.settings, capabilities);
            same(Arrays.asList("added settings", "added settings/capabilities.txt", "added " + JSON), differences(before, snapshot(files)), "what the visit changed");
            same("{\"version\":1,\"frameCap\":\"60\",\"scaler\":\"fsr1\",\"fsrSharpnessTenths\":2,\"renderScalePercent\":100,\"fpsOverlay\":false,"
                    + "\"worldTraffic\":100,\"worldPedestrians\":100,\"worldParkedCars\":100,\"worldScenarioPedestrians\":100,\"inputUnbuffered\":false}\n",
                    new String(bytes(files, JSON), StandardCharsets.UTF_8), "settings.json");
            age(files);
            Map<String, String> saved = snapshot(files);
            save(files, store(files).load(capabilities).settings, capabilities);
            same(new ArrayList<String>(), differences(saved, snapshot(files)), "what a second Save at the defaults changed");
        });

        test("a render-scale.txt that holds 125 or 50: shown as it is and left as it is by a Save; rewritten only when the row is changed", () -> {
            // 125 saved under v463 by the previous app version, then v464 became the installed build.
            File files = phoneFiles(tempDir(), V464_NAMES);
            save(files, at(P125), v463());
            sameBytes(ascii("125\n"), bytes(files, RENDER_TXT), "the fixture: 125 saved under v463");
            age(files);
            Map<String, String> before = snapshot(files);
            SettingsStore.Loaded loaded = store(files).load(v464());
            check(loaded.fromFile, "the saved settings are read");
            same(P125, loaded.settings.get(RENDER), "125 is shown");
            same(new ArrayList<String>(), loaded.drift, "and the files do not differ from it");
            SettingsPagesTest.Tab tab = new SettingsPagesTest.Tab(v464(), loaded.settings, true, loaded.drift);
            tab.into(SettingsPages.DISPLAY, ROW_RENDER);
            same("125 %: 1600x900, upscaled", row(tab, ROW_RENDER).value, "on the row");
            save(files, startSave(tab), v464());
            same(new ArrayList<String>(), differences(before, snapshot(files)), "a Save without a change rewrites nothing (bytes and times)");
            tab.model.saveFinished(true, false, null);
            tab.press(LEFT);
            save(files, startSave(tab), v464());
            sameBytes(ascii("115\n"), bytes(files, RENDER_TXT), "after LEFT (Ultra) and Save");

            // 50 written by a test script, nothing saved by the app yet.
            File bare = phoneFiles(tempDir(), V464_NAMES);
            put(bare, RENDER_TXT, "50\n");
            age(bare);
            Map<String, String> bareBefore = snapshot(bare);
            SettingsStore.Loaded imported = store(bare).load(v464());
            check(!imported.fromFile, "no settings.json");
            same(P50, imported.settings.get(RENDER), "the file's 50 is shown");
            save(bare, imported.settings, v464());
            same(Arrays.asList("added " + JSON), differences(bareBefore, snapshot(bare)), "a Save of what is shown adds settings.json and leaves render-scale.txt alone");
            check(new String(bytes(bare, JSON), StandardCharsets.UTF_8).contains("\"renderScalePercent\":50,"), "which records 50");
        });

        test("Native with Bicubic and sharpness 0.7 saved: the two greyed rows' files stay as they are; only render-scale.txt is written", () -> {
            File files = phoneFiles(tempDir(), V464_NAMES);
            Settings saved = Settings.defaults().with(SCALER, SettingsSchema.SCALER_BICUBIC).with(SHARPNESS, 7);
            save(files, saved, v464());
            sameBytes(ascii("cubic\n"), bytes(files, SCALER_TXT), "the fixture: Bicubic saved");
            sameBytes(ascii("0.7\n"), bytes(files, SHARPNESS_TXT), "the fixture: sharpness 0.7 saved");
            age(files);
            Map<String, String> before = snapshot(files);
            SettingsPagesTest.Tab tab = new SettingsPagesTest.Tab(v464(), store(files).load(v464()).settings, true, NOTHING);
            tab.into(SettingsPages.DISPLAY, ROW_RENDER);
            for (int presses = 0; presses < 2; ++presses) tab.press(RIGHT);
            save(files, startSave(tab), v464());
            same(Arrays.asList("added " + RENDER_TXT, "changed " + JSON), differences(before, snapshot(files)), "what the Save at Native changed");
            sameBytes(ascii("150\n"), bytes(files, RENDER_TXT), "render-scale.txt");
        });

        test("Frame Rate Cap notes in a stage B build: 40 fps and 30 fps at Native say what was measured at Native; at every size but Quality the"
                + " others say where they were measured; a v463 build keeps its notes", () -> {
            String what = "The most frames the game shows each second. A lower cap runs cooler and steadier.";
            String native40 = "Measured on an AYN Thor at Native: uneven indoors (two frames in three evenly paced). Does not hold outdoors.";
            String native30 = "Measured on an AYN Thor at Native: holds indoors and outdoors (70 to 82 % of frames evenly paced).";
            int[] noted = {SettingsSchema.CAP_40, SettingsSchema.CAP_30, SettingsSchema.CAP_AUTO};
            String[] atQuality = {"Measured on an AYN Thor: holds indoors. The Thor's second-screen dashboard makes some frames uneven.",
                    "Measured on an AYN Thor: holds while driving (84 % of frames evenly paced).", "Picks the cap itself. Measured on an AYN Thor: 40 indoors, 24 while driving."};
            String[] elsewhere = {"Measured on an AYN Thor at Quality: holds indoors. The Thor's second-screen dashboard makes some frames uneven.",
                    "Measured on an AYN Thor at Quality: holds while driving (84 % of frames evenly paced).",
                    "Picks the cap itself. Measured on an AYN Thor at Quality: 40 indoors, 24 while driving."};
            String[] sizes = {"50 %", "Performance", "Balanced", "Quality", "Ultra", "125 %", "Native"};
            for (int render = P50; render <= P150; ++render) {
                String size = sizes[render];
                for (int c = 0; c < noted.length; ++c) {
                    String cap = SettingsSchema.spec(CAP).format(noted[c]);
                    String expected = render == P100 ? atQuality[c] : render == P150 && noted[c] == SettingsSchema.CAP_40 ? native40
                            : render == P150 && noted[c] == SettingsSchema.CAP_30 ? native30 : elsewhere[c];
                    same(Arrays.asList(what, expected), help(frameRate(v464(), at(render).with(CAP, noted[c]))), "v464, " + cap + " at " + size);
                    same(Arrays.asList(what, atQuality[c]), help(frameRate(v463(), at(render).with(CAP, noted[c]))), "v463 (no marker), " + cap + " at " + size);
                }
                for (int quiet : new int[] {SettingsSchema.CAP_60, SettingsSchema.CAP_24}) {   // caps without a measured note: the size adds none
                    same(Arrays.asList(what), help(frameRate(v464(), at(render).with(CAP, quiet))),
                            "v464, " + SettingsSchema.spec(CAP).format(quiet) + " at " + size);
                }
            }
            // Only the Frame Rate Cap row has these notes. Bicubic and sharpness 0.2 are the value 2 of their settings, as 40 fps is of its own.
            SettingsPagesTest.Tab other = display(v464(), at(P115).with(SCALER, SettingsSchema.SCALER_BICUBIC), ROW_SCALER);
            same(Arrays.asList("How the game picture is enlarged to fill this screen."), help(other), "the Scaler row at Bicubic");
            same(Arrays.asList("How strongly FSR 1 sharpens the enlarged picture."), help(display(v464(), at(P115), ROW_SHARPNESS)), "the FSR Sharpness row at 0.2");

            // The note follows the render size chosen in this visit, saved or not: a Save writes both.
            SettingsPagesTest.Tab tab = display(v464(), Settings.defaults().with(CAP, SettingsSchema.CAP_40), ROW_RENDER);
            tab.press(RIGHT);
            tab.press(RIGHT);
            same("Native: 1920x1080, no upscaling", row(tab, ROW_RENDER).value, "Quality saved, two steps right");
            tab.into(SettingsPages.FRAME_RATE, ROW_CAP);
            same(Arrays.asList(what, native40), help(tab), "40 fps with Native chosen but not saved");
            for (MenuScenes.Line line : tab.page().help) same(MenuScenes.TONE_NORMAL, line.tone, "in the plain colour");
            tab.into(SettingsPages.DISPLAY, ROW_RENDER);
            tab.press(LEFT);
            tab.press(LEFT);
            tab.into(SettingsPages.FRAME_RATE, ROW_CAP);
            same(Arrays.asList(what, atQuality[0]), help(tab), "40 fps with Quality chosen again");
        });

        end();
    }
}
