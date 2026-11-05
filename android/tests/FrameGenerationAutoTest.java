package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.begin;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.bytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.caps;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.end;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.minus;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.phoneFiles;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.plus;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.sameBytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.snapshot;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.tempDir;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Host test of frame generation under the automatic frame cap: a game build that announces it (the marker
 * recomp-cap:framegen-auto1 together with the name of the file diag-pace-auto-low) against the builds that do not.
 * It looks at the files a Save plans (RuntimeMapping) and writes (SettingsStore on a real directory, after the app's
 * own scan of a stand-in library), at what standing files read back as, and at what the Frame Generation row shows
 * (the settings tab driven the way the app drives it: SettingsPagesTest.Tab). The marker, the paths and the sentences
 * are written out here, independently of the constants in the code under test. Run with java -ea.
 */
public final class FrameGenerationAutoTest {
    private static final List<String> NOTHING = new ArrayList<String>();
    private static final int LEFT = NavInput.LEFT, RIGHT = NavInput.RIGHT, START = NavInput.MENU;
    private static final String CAP = "frameCap", GENERATION = "frameGeneration";
    /** Values of frameCap (Off, 60, 40, 30, 24, Automatic) and of frameGeneration (Off, In-Between Frames, Predicted Frames). */
    private static final int CAP_60 = 1, CAP_40 = 2, CAP_30 = 3, CAP_24 = 4, CAP_AUTO = 5, OFF = 0, BETWEEN_ORDER = 1, AHEAD_ORDER = 2;
    /** The rows of the Frame Rate category, top to bottom. */
    private static final int ROW_CAP = 0, ROW_GENERATION = 1, ROW_COUNTER = 2;

    private static final String MARKER = "recomp-cap:framegen-auto1", LOW_NAME = "diag-pace-auto-low";
    private static final String JSON = "settings/settings.json", LIMIT30 = "private-trace/diag-v280-limit30",
            PACE40 = "private-trace/diag-pace-40", PACE30 = "private-trace/diag-pace-30", PACE24 = "private-trace/diag-pace-24",
            PACEAUTO = "private-trace/diag-pace-auto", LOW = "private-trace/diag-pace-auto-low",
            BETWEEN = "private-trace/diag-fg-between", AHEAD = "private-trace/diag-fg-ahead";

    /**
     * The build the marker is written for: the phone's v464 names, the generator's own marker with its in-between
     * switch, the new marker and the name of the lower-range file. The predicted order is in no build yet (the game's
     * fg_gen.h keeps the name of its switch out of the library), so that name is not here.
     */
    private static final String[] ANNOUNCED = plus(minus(SetTestKit.FRAMEGEN_NAMES, "diag-fg-ahead"), MARKER, LOW_NAME);
    /** A later build that also has the predicted order: the rule for that order under the automatic cap shows only there. */
    private static final String[] BOTH_ORDERS = plus(SetTestKit.FRAMEGEN_NAMES, MARKER, LOW_NAME);
    /** The builds before it (v522, v523): the generator, and the lower-range file as a developer's switch. No new marker. */
    private static final String[] BEFORE = plus(minus(SetTestKit.FRAMEGEN_NAMES, "diag-fg-ahead"), LOW_NAME);
    /** The new marker without the file's name: a build that announces what it cannot be told to do. */
    private static final String[] MARKER_ONLY = plus(SetTestKit.FRAMEGEN_NAMES, MARKER);

    private static final String WHAT_30 = "Adds a generated picture to every frame the game draws under the 30 fps cap.";
    private static final String WHAT_30_OR_AUTO = "Adds a generated picture to every frame the game draws under the 30 fps or Automatic cap.";
    private static final String NEEDS_30 = "Not available: Needs Frame Rate Cap at 30 fps";
    private static final String NEEDS_30_OR_AUTO = "Not available: Needs Frame Rate Cap at 30 fps or Automatic";
    private static final String PREDICTED_NEEDS_30 = "Predicted Frames needs Frame Rate Cap at 30 fps";
    private static final String HINT_BETWEEN = "Experimental; may switch itself off. Made pictures go between drawn frames, two refreshes late.";
    private static final String HINT_AHEAD = "Experimental; may switch itself off. A predicted picture follows each frame; one wrong on a turn.";

    private static final SettingsStore.GameRunning NOT_RUNNING = new SettingsStore.GameRunning() {
        @Override public boolean check() { return false; }
    };

    private static Settings chosen(int cap, int generation) {
        return Settings.defaults().with(CAP, cap).with(GENERATION, generation);
    }

    private static SettingsCoreTestKit.MemoryFiles files() { return new SettingsCoreTestKit.MemoryFiles(); }

    /** The phone's setting-related files of 2026-10-05. */
    private static SettingsCoreTestKit.MemoryFiles phone() { return files().touch(SettingsCoreTestKit.PHONE_STANDING); }

    /** Starting states of the game's files: nothing, the phone, and leftovers of a developer's runs under each cap. */
    private static SettingsCoreTestKit.MemoryFiles[] states() {
        return new SettingsCoreTestKit.MemoryFiles[] {files(), phone(), phone().touch(PACEAUTO, BETWEEN, LOW), phone().touch(PACE30, AHEAD, LOW),
                files().touch(PACE40, BETWEEN, LIMIT30), phone().touch(PACEAUTO, AHEAD, BETWEEN)};
    }

    private static SettingsCoreTestKit.MemoryFiles copy(SettingsCoreTestKit.MemoryFiles files) {
        SettingsCoreTestKit.MemoryFiles out = files();
        out.files.putAll(files.files);
        return out;
    }

    /** Every file of a state with what it holds. */
    private static TreeMap<String, String> state(SettingsCoreTestKit.MemoryFiles files) {
        TreeMap<String, String> out = new TreeMap<String, String>();
        for (Map.Entry<String, byte[]> entry : files.files.entrySet()) out.put(entry.getKey(), SettingsCoreTestKit.show(entry.getValue()));
        return out;
    }

    private static List<String> list(String... items) { return Arrays.asList(items); }

    /**
     * The plan's entries about the frame cap and frame generation, as text: "path" for a switch file that must
     * exist, "-path" for a file that must be absent. A file the plan does not name is not in the list.
     */
    private static List<String> planned(Settings settings, Capabilities capabilities, FileView files) {
        List<String> out = new ArrayList<String>();
        for (Map.Entry<String, byte[]> entry : RuntimeMapping.plan(settings, capabilities, files).entrySet()) {
            String path = entry.getKey();
            if (!path.contains("diag-pace") && !path.contains("diag-fg") && !path.contains("limit")) continue;
            out.add(entry.getValue() == null ? "-" + path : path);
        }
        return out;
    }

    /** A whole plan as text, entry for entry, each file with what it must hold. */
    private static List<String> whole(Settings settings, Capabilities capabilities, FileView files) {
        List<String> out = new ArrayList<String>();
        for (Map.Entry<String, byte[]> entry : RuntimeMapping.plan(settings, capabilities, files).entrySet()) {
            out.add(entry.getKey() + " " + SettingsCoreTestKit.show(entry.getValue()));
        }
        return out;
    }

    private static Settings imported(Capabilities capabilities, SettingsCoreTestKit.MemoryFiles files) throws IOException {
        return RuntimeMapping.importFrom(files, capabilities);
    }

    /**
     * The order whose picture the game makes for a cap and a chosen order, in a build that announces generation under
     * the automatic cap: either order under 30 fps, the in-between order under the automatic cap, nothing elsewhere.
     * predicted: whether the build has the predicted order at all.
     */
    private static int made(int cap, int generation, boolean predicted) {
        if (generation == AHEAD_ORDER && !predicted) return OFF;
        if (cap == CAP_30) return generation;
        return cap == CAP_AUTO && generation == BETWEEN_ORDER ? BETWEEN_ORDER : OFF;
    }

    /** The settings tab of a build, opened on saved settings, with the highlight on the Frame Generation row. */
    private static SettingsPagesTest.Tab generationRow(String[] build, Settings saved) {
        SettingsPagesTest.Tab tab = new SettingsPagesTest.Tab(caps(build), saved, true, NOTHING);
        tab.into(SettingsPages.FRAME_RATE, ROW_GENERATION);
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

    private static SettingsStore store(File files) {
        return new SettingsStore(files, NOT_RUNNING);
    }

    private static SettingsStore.SaveResult saved(File files, Settings settings, Capabilities capabilities) {
        SettingsStore.SaveResult result = store(files).save(settings, capabilities);
        check(result.ok && result.error == null, "save failed: " + result.error);
        return result;
    }

    public static void main(String[] args) {
        begin("FrameGenerationAutoTest");

        test("plan: under the automatic cap In-Between Frames gets its switch and the lower-range file; every other cap and order plans that file absent", () -> {
            // A build with both orders, so that the table's last column says something. The three files the table is
            // about stand when the plan is made: "absent" is then a planned removal, not a file nobody named.
            SettingsCoreTestKit.MemoryFiles standing = phone().touch(BETWEEN, AHEAD, LOW);
            String[] paths = {BETWEEN, AHEAD, LOW};
            // Per frame cap, for the orders Off / In-Between Frames / Predicted Frames: the between switch, the ahead
            // switch and the lower-range file, + for present and - for absent.
            String[][] expected = {
                    {"---", "---", "---"},    // Off: no build has it, it is planned as 60 fps
                    {"---", "---", "---"},    // 60 fps
                    {"---", "---", "---"},    // 40 fps
                    {"---", "+--", "-+-"},    // 30 fps: either order as before, and never the lower-range file
                    {"---", "---", "---"},    // 24 fps
                    {"---", "+-+", "---"}};   // Automatic: In-Between Frames with the lower-range file; Predicted Frames as before
            for (int cap = 0; cap < expected.length; ++cap) {
                for (int generation = OFF; generation <= AHEAD_ORDER; ++generation) {
                    List<String> got = planned(chosen(cap, generation), caps(BOTH_ORDERS), standing);
                    for (int file = 0; file < paths.length; ++file) {
                        String want = (expected[cap][generation].charAt(file) == '+' ? "" : "-") + paths[file];
                        check(got.contains(want), "cap " + cap + ", frame generation " + generation + ": expected " + want + " in " + got);
                    }
                }
            }
            // The build the marker is written for, from the phone's files: the three files of the case, and the automatic
            // cap's own file with them. (Its library does not hold the predicted order's switch name: that file is not named.)
            Map<String, byte[]> plan = RuntimeMapping.plan(chosen(CAP_AUTO, BETWEEN_ORDER), caps(ANNOUNCED), phone());
            for (String path : list(PACEAUTO, LOW, BETWEEN)) sameBytes(new byte[0], plan.get(path), path + ": a switch file, created empty");
            check(!plan.containsKey(AHEAD), "the predicted order's switch is not in that build and not in the plan");
        });

        test("builds without the marker are planned and read as before, also one that reads the lower-range file; the marker without that file's name opens nothing", () -> {
            // BEFORE reads diag-pace-auto-low as a developer's switch; MARKER_ONLY announces what it cannot be told to do.
            // Each is set against the same build without that one name - a build the tests of before this marker
            // cover - for every cap and order and every starting state: the same plan, entry for entry, and the same
            // settings read back. The one name must make no difference.
            String[][][] pairs = {{BEFORE, minus(BEFORE, LOW_NAME)}, {MARKER_ONLY, minus(MARKER_ONLY, MARKER)}};
            for (String[][] pair : pairs) {
                Capabilities build = caps(pair[0]), without = caps(pair[1]);
                for (SettingsCoreTestKit.MemoryFiles state : states()) {
                    String where = "files " + state.files.keySet() + ", build " + build.names();
                    same(imported(without, state), imported(build, state), "read back from " + where);
                    for (int cap = 0; cap <= CAP_AUTO; ++cap) {
                        for (int generation = OFF; generation <= AHEAD_ORDER; ++generation) {
                            same(whole(chosen(cap, generation), without, state), whole(chosen(cap, generation), build, state),
                                    "the plan for cap " + cap + ", frame generation " + generation + ", " + where);
                        }
                    }
                }
            }
            // And said outright for the case the marker is about, with the developer's files standing.
            for (String[] names : new String[][] {BEFORE, MARKER_ONLY}) {
                List<String> got = planned(chosen(CAP_AUTO, BETWEEN_ORDER), caps(names), phone().touch(BETWEEN, LOW));
                check(got.contains("-" + BETWEEN), "no marker or no file name: the between switch is planned absent under the automatic cap: " + got);
                check(!got.contains(LOW) && !got.contains("-" + LOW), "and the lower-range file is not named (a developer's, or not read at all): " + got);
                same(OFF, imported(caps(names), files().touch(PACEAUTO, BETWEEN, LOW)).get(GENERATION), "standing files read back as Off, build " + Arrays.asList(names));
            }
        });

        test("import: under the automatic cap the between file reads back as In-Between Frames in the announced build; the ahead file and every other cap as before", () -> {
            Capabilities announced = caps(ANNOUNCED), both = caps(BOTH_ORDERS);
            Settings read = imported(announced, files().touch(PACEAUTO, BETWEEN, LOW));
            same(CAP_AUTO, read.get(CAP), "the three files: the automatic cap");
            same(BETWEEN_ORDER, read.get(GENERATION), "the three files: In-Between Frames");
            same(BETWEEN_ORDER, imported(announced, files().touch(PACEAUTO, BETWEEN)).get(GENERATION), "without the lower-range file, which widens the cap's range and decides no picture: In-Between Frames");
            same(BETWEEN_ORDER, imported(both, files().touch(PACEAUTO, AHEAD, BETWEEN)).get(GENERATION), "both switches: between, as the game decides");
            same(OFF, imported(both, files().touch(PACEAUTO, AHEAD, LOW)).get(GENERATION), "the ahead file makes no picture under the automatic cap: Off");
            same(OFF, imported(announced, files().touch(PACEAUTO, LOW)).get(GENERATION), "no switch: Off, whatever the lower-range file says");
            same(OFF, imported(announced, files().touch(PACE40, BETWEEN, LOW)).get(GENERATION), "under 40 fps: Off, as before");
            same(OFF, imported(announced, files().touch(PACE24, BETWEEN, LOW)).get(GENERATION), "under 24 fps: Off, as before");
            same(OFF, imported(announced, files().touch(BETWEEN, LOW)).get(GENERATION), "under 60 fps: Off, as before");
            same(BETWEEN_ORDER, imported(announced, files().touch(PACE30, BETWEEN)).get(GENERATION), "under 30 fps: In-Between Frames, as before");
            same(AHEAD_ORDER, imported(both, files().touch(PACE30, AHEAD)).get(GENERATION), "under 30 fps: Predicted Frames, as before");

            // Files in a form this screen does not write: a Save would change them, and drift names exactly those.
            SettingsCoreTestKit.MemoryFiles exact = phone().touch(PACEAUTO, LOW, BETWEEN);
            same(list(), RuntimeMapping.drift(imported(announced, exact), announced, exact), "the three files: a Save changes nothing");
            SettingsCoreTestKit.MemoryFiles upper = phone().touch(PACEAUTO, BETWEEN);
            same(list(LOW), RuntimeMapping.drift(imported(announced, upper), announced, upper), "generation under the automatic cap without its lower range: a Save adds the file");
            SettingsCoreTestKit.MemoryFiles bare = phone().touch(PACEAUTO, LOW);
            same(list(LOW), RuntimeMapping.drift(imported(announced, bare), announced, bare), "the lower range without generation: a Save removes the file");
            SettingsCoreTestKit.MemoryFiles thirty = phone().touch(PACE30, BETWEEN, LOW);
            same(list(LOW), RuntimeMapping.drift(imported(announced, thirty), announced, thirty), "the lower-range file under 30 fps: a Save removes it");
        });

        test("round trip in the announced builds: after a Save nothing differs, a second Save changes nothing, and the files read back as what the game will make", () -> {
            for (String[] names : new String[][] {ANNOUNCED, BOTH_ORDERS}) {
                Capabilities build = caps(names);
                boolean predicted = Arrays.asList(names).contains("diag-fg-ahead");
                for (SettingsCoreTestKit.MemoryFiles start : states()) {
                    for (int cap = 0; cap <= CAP_AUTO; ++cap) {
                        for (int generation = OFF; generation <= AHEAD_ORDER; ++generation) {
                            Settings settings = chosen(cap, generation);
                            SettingsCoreTestKit.MemoryFiles disk = copy(start);
                            disk.apply(RuntimeMapping.plan(settings, build, disk));
                            String what = "cap " + cap + ", frame generation " + generation + ", from " + start.files.keySet() + (predicted ? ", both orders" : "");
                            same(list(), RuntimeMapping.drift(settings, build, disk), "drift right after applying: " + what);
                            same(state(disk), state(copy(disk).apply(RuntimeMapping.plan(settings, build, disk))), "a second save changes nothing: " + what);
                            Settings back = imported(build, disk);
                            same(cap == 0 ? CAP_60 : cap, back.get(CAP), "the cap read back: " + what);
                            same(made(cap, generation, predicted), back.get(GENERATION), "frame generation read back: " + what);
                            // No switch stands that makes no picture (a standing one changes how frames are timed all the same).
                            same(made(cap, generation, predicted) == BETWEEN_ORDER, disk.exists(BETWEEN), "the between switch: " + what);
                            if (predicted) same(made(cap, generation, predicted) == AHEAD_ORDER, disk.exists(AHEAD), "the ahead switch: " + what);
                            // The lower-range file belongs to In-Between Frames under the automatic cap and stands nowhere else.
                            same(cap == CAP_AUTO && generation == BETWEEN_ORDER, disk.exists(LOW), "the lower-range file: " + what);
                            check(!disk.exists(LOW) || (disk.exists(PACEAUTO) && disk.exists(BETWEEN)), "it never stands without the automatic cap's file and the between switch: " + what);
                        }
                    }
                }
            }
        });

        test("a real directory: the app's own scan finds the marker and the file's name, a Save writes the three files and takes two away again; a developer's file under the build before is left as it is", () -> {
            // The library is a stand-in that holds the names of the announced build; the scan is the app's own.
            File files = phoneFiles(tempDir(), ANNOUNCED);
            Capabilities capabilities = CapabilityScanner.load(files, true);
            Settings shown = store(files).load(capabilities).settings;
            Settings between = shown.with(CAP, CAP_AUTO).with(GENERATION, BETWEEN_ORDER);
            SettingsStore.SaveResult on = saved(files, between, capabilities);
            same(list(JSON, PACEAUTO, LOW, BETWEEN), on.written, "Automatic with In-Between Frames: written, in the order of the fixed list");
            same(list(), on.removed, "nothing removed");
            sameBytes(new byte[0], bytes(files, LOW), "the lower-range file is a switch file: empty");
            SettingsStore.Loaded loaded = store(files).load(capabilities);
            same(between, loaded.settings, "read back");
            same(list(), loaded.drift, "nothing differs after the save");
            SettingsStore.SaveResult off = saved(files, between.with(GENERATION, OFF), capabilities);
            same(list(LOW, BETWEEN), off.removed, "frame generation off: the lower-range file goes with the switch");
            check(new File(files, PACEAUTO).isFile(), "and the automatic cap's file stays");

            // v523 as it is: it reads the lower-range file, a developer left one, and nothing announces generation under
            // the automatic cap. The same choice writes the cap's file only, and the developer's file keeps its bytes and time.
            File before = phoneFiles(tempDir(), BEFORE, LOW_NAME);
            Capabilities older = CapabilityScanner.load(before, true);
            String developers = snapshot(before).get(LOW);
            check(developers != null, "test setup: the developer's file stands");
            Settings choice = store(before).load(older).settings.with(CAP, CAP_AUTO).with(GENERATION, BETWEEN_ORDER);
            same(list(JSON, PACEAUTO), saved(before, choice, older).written, "the build before: the cap's file only");
            same(developers, snapshot(before).get(LOW), "the developer's file was not touched");
            check(!new File(before, BETWEEN).exists(), "and no between switch was written");
            same(list(), store(before).load(older).drift, "nothing differs: the file is not this screen's there");
        });

        test("Frame Generation in the announced build: usable under 30 fps and under Automatic, elsewhere unavailable with a reason that names both caps; what the row lets through is what a Save writes", () -> {
            SettingsPagesTest.Tab tab = generationRow(ANNOUNCED, Settings.defaults());
            MenuScenes.Row grey = row(tab, ROW_GENERATION);
            check(!grey.enabled && !grey.canLeft && !grey.canRight, "at 60 fps the row is drawn as unavailable");
            same(list(WHAT_30_OR_AUTO, NEEDS_30_OR_AUTO), help(tab), "the help strip at 60 fps: what the row is, then the reason");
            tab.into(SettingsPages.FRAME_RATE, ROW_COUNTER);
            same("Shows the frame rate in the top right corner while the game runs.", help(tab).get(0), "(the sentence with both caps is Frame Generation's: the counter's row keeps its own)");
            // The cap as edited: RIGHT on its row goes 40, 30, 24, Automatic.
            tab.into(SettingsPages.FRAME_RATE, ROW_CAP);
            for (int presses = 0; presses < 4; ++presses) tab.press(RIGHT);
            same("Automatic", row(tab, ROW_CAP).value, "(the cap is Automatic now)");
            MenuScenes.Row live = row(tab, ROW_GENERATION);
            same("Off", live.value, "(frame generation is still Off)");
            check(live.enabled && live.canRight, "under Automatic the row is usable from Off: RIGHT leads on");
            tab.into(SettingsPages.FRAME_RATE, ROW_GENERATION);
            check(!help(tab).toString().contains("Not available"), "nothing is called unavailable: " + help(tab));
            tab.press(RIGHT);
            same("In-Between Frames", row(tab, ROW_GENERATION).value, "RIGHT: In-Between Frames");
            same(list(WHAT_30_OR_AUTO, HINT_BETWEEN), help(tab), "the help strip: what the row is and what the order is, no reason and no note");
            tab.press(START);
            same(MenuModel.ACTION_SAVE, tab.model.pendingAction(), "Start asks for a save");
            List<String> written = planned(tab.model.beginSave(), caps(ANNOUNCED), phone());
            check(written.containsAll(list(PACEAUTO, LOW, BETWEEN)), "the settings the screen hands over plan the three files: " + written);

            // The chosen order under each cap, opened as saved: it is kept everywhere and can be used under two caps.
            String[] label = {null, "60 fps", "40 fps", "30 fps", "24 fps", "Automatic"};
            for (int cap = CAP_60; cap <= CAP_AUTO; ++cap) {
                SettingsPagesTest.Tab at = generationRow(ANNOUNCED, chosen(cap, BETWEEN_ORDER));
                MenuScenes.Row row = row(at, ROW_GENERATION);
                same("In-Between Frames", row.value, "the chosen order is kept at " + label[cap]);
                boolean usable = cap == CAP_30 || cap == CAP_AUTO;
                same(usable, row.enabled, "usable at " + label[cap]);
                same(usable ? list(WHAT_30_OR_AUTO, HINT_BETWEEN) : list(WHAT_30_OR_AUTO, NEEDS_30_OR_AUTO), help(at), "the help strip at " + label[cap]);
                at.press(LEFT);
                same(usable ? "Off" : "In-Between Frames", row(at, ROW_GENERATION).value, "LEFT at " + label[cap]);
            }
        });

        test("Predicted Frames under Automatic, in a build with both orders: the row says that this order needs 30 fps, stays usable, and its switch is not written", () -> {
            SettingsPagesTest.Tab tab = generationRow(BOTH_ORDERS, chosen(CAP_AUTO, AHEAD_ORDER));
            MenuScenes.Row row = row(tab, ROW_GENERATION);
            same("Predicted Frames", row.value, "the saved order is shown");
            check(row.enabled && row.canLeft, "the row can be used: LEFT leads back");
            same(list(WHAT_30_OR_AUTO, PREDICTED_NEEDS_30, HINT_AHEAD), help(tab), "the help strip: what the row is, the note, what the order is");
            for (int other : new int[] {ROW_CAP, ROW_COUNTER}) {
                tab.into(SettingsPages.FRAME_RATE, other);
                check(!help(tab).contains(PREDICTED_NEEDS_30), "the note is Frame Generation's own: row " + other + " does not carry it: " + help(tab));
            }
            tab.into(SettingsPages.FRAME_RATE, ROW_GENERATION);
            List<String> plan = planned(chosen(CAP_AUTO, AHEAD_ORDER), caps(BOTH_ORDERS), phone().touch(AHEAD, LOW));
            check(plan.containsAll(list(PACEAUTO, "-" + LOW, "-" + BETWEEN, "-" + AHEAD)), "(what the note is about: a Save writes the cap's file and no switch: " + plan + ")");
            tab.press(LEFT);
            same("In-Between Frames", row(tab, ROW_GENERATION).value, "LEFT: In-Between Frames");
            same(list(WHAT_30_OR_AUTO, HINT_BETWEEN), help(tab), "and the note is gone");
            tab.press(LEFT);
            same("Off", row(tab, ROW_GENERATION).value, "LEFT again: Off");
            check(!help(tab).contains(PREDICTED_NEEDS_30), "no note at Off: " + help(tab));
            // Under the 30 fps cap the predicted order is as usable as before, and nothing is added to its row.
            same(list(WHAT_30_OR_AUTO, HINT_AHEAD), help(generationRow(BOTH_ORDERS, chosen(CAP_30, AHEAD_ORDER))), "Predicted Frames at 30 fps");
            // A saved Predicted Frames in the build that lacks that order: the build's own line stands, in the warning
            // colour, and the note about the cap - untrue there - does not.
            SettingsPagesTest.Tab stale = generationRow(ANNOUNCED, chosen(CAP_AUTO, AHEAD_ORDER));
            same(MenuScenes.TONE_WARNING, row(stale, ROW_GENERATION).valueTone, "an order the build lacks: the warning colour");
            same(list(WHAT_30_OR_AUTO, "Predicted Frames - not in this game build; the game uses Off"), help(stale), "and the build's own line, without the note");
            stale.press(LEFT);
            same("In-Between Frames", row(stale, ROW_GENERATION).value, "LEFT puts it back to an order the build has");
        });

        test("builds without the marker keep the row of before under Automatic: the 30 fps reason and the old sentence, also for a build that reads the lower-range file", () -> {
            for (String[] names : new String[][] {BEFORE, MARKER_ONLY}) {
                String build = "build " + Arrays.asList(names);
                SettingsPagesTest.Tab tab = generationRow(names, chosen(CAP_AUTO, BETWEEN_ORDER));
                MenuScenes.Row row = row(tab, ROW_GENERATION);
                check(!row.enabled && !row.canLeft && !row.canRight, "unavailable under Automatic, " + build);
                same("In-Between Frames", row.value, "the chosen order is kept, " + build);
                same(list(WHAT_30, NEEDS_30), help(tab), "the help strip under Automatic, " + build);
                same(list(WHAT_30, NEEDS_30), help(generationRow(names, chosen(CAP_24, BETWEEN_ORDER))), "and under 24 fps, " + build);
            }
        });

        test("no state of the Frame Rate page overfills the help strip in the announced builds, and none of its texts names a file or a build", () -> {
            float width = MenuLayout.paneWidth(true) - 2 * MenuLayout.ROW_PAD;
            for (String[] names : new String[][] {ANNOUNCED, BOTH_ORDERS}) {
                for (int cap = CAP_60; cap <= CAP_AUTO; ++cap) {
                    for (int generation = OFF; generation <= AHEAD_ORDER; ++generation) {
                        for (int slot = 0; slot < 3; ++slot) {
                            SettingsPagesTest.Tab tab = new SettingsPagesTest.Tab(caps(names), chosen(cap, generation), true, NOTHING);
                            tab.into(SettingsPages.FRAME_RATE, slot);
                            String where = "cap " + cap + ", frame generation " + generation + ", row " + slot;
                            int lines = 0;
                            for (String line : help(tab)) lines += Scene.wrap(line, MenuLayout.HELP_PX, Scene.REGULAR, width, MenuLayoutTest.FIXED).size();
                            check(lines >= 1 && lines <= MenuLayout.HELP_LINES, where + ": " + lines + " help lines: " + help(tab));
                            if (slot == ROW_GENERATION) {
                                same(1, Scene.wrap(help(tab).get(0), MenuLayout.HELP_PX, Scene.REGULAR, width, MenuLayoutTest.FIXED).size(), where + ": what the row is fits one line");
                            }
                            // As the build that is handed out draws it (ShipWords), read with that build's word list.
                            MenuScenes.Page page = tab.page();
                            ShipWords.page(page);
                            for (String text : ShipWordsTest.textsOf(page)) same(null, ShipWordsTest.forbiddenIn(text), where + ", in: " + text);
                        }
                    }
                }
            }
        });

        end();
    }
}
