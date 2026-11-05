package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.TimeZone;

/**
 * Host test of MenuModel, the logic of the settings screen (design section 2.5). Run with java -ea. It drives the
 * model the way the screen does (NavInput commands, taps, the save round trip) and looks only at what the screen
 * would draw or do. Lane A's classes are used through their documented surface only (SettingsSchema, SettingSpec,
 * Settings, Capabilities.of). Prints one line per test and exits non-zero when any test fails.
 */
public final class MenuModelTest {
    private static final int UP = NavInput.UP, DOWN = NavInput.DOWN, LEFT = NavInput.LEFT, RIGHT = NavInput.RIGHT,
            CONFIRM = NavInput.CONFIRM, BACK = NavInput.BACK, MENU = NavInput.MENU, ALT = NavInput.ALT;
    private static final String CAP = SettingsSchema.FRAME_CAP, SCALER = SettingsSchema.SCALER,
            SHARPNESS = SettingsSchema.FSR_SHARPNESS, RENDER = SettingsSchema.RENDER_SCALE,
            FRAME_GEN = SettingsSchema.FRAME_GENERATION, OVERLAY = SettingsSchema.FPS_OVERLAY,
            TRAFFIC = SettingsSchema.WORLD_TRAFFIC, INPUT = SettingsSchema.INPUT_UNBUFFERED;
    private static final List<String> NOTHING = new ArrayList<>();

    /** The seven input-file names build v458 contains (design section 3.3). */
    private static final String[] V458_NAMES = {"diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60",
            "diag-v409-scale-cubic", "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1"};
    /** What a build with every lane of section 3.3 would add. */
    private static final String[] LATER_NAMES = {"diag-pace-40", "diag-pace-30", "diag-pace-24", "diag-pace-auto",
            "world-load.txt", "render-scale.txt", "scaler.txt", "fsr-sharpness.txt", "recomp-cap:gsr1", "recomp-cap:wl-lod",
            "diag-v460-scale-gsr1"};

    private static int failures;

    private static void run(String name, Runnable test) {
        try {
            test.run();
            System.out.println("PASS MenuModel: " + name);
        } catch (AssertionError | RuntimeException failure) {
            failures++;
            StackTraceElement at = failure.getStackTrace().length > 1 ? failure.getStackTrace()[1] : null;
            System.out.println("FAIL MenuModel: " + name + ": " + failure + (at == null ? "" : " (" + at + ")"));
        }
    }

    private static void eq(Object expected, Object actual, String what) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(what + ": expected <" + expected + "> but was <" + actual + ">");
        }
    }

    // ---- fixtures -------------------------------------------------------------------------------------------------

    /** Build v458 as installed today: frame cap 60 / 30, scaler FSR 1 / bicubic / nearest, sharpness, the counter. */
    private static Capabilities v458() {
        return Capabilities.of(new HashSet<>(Arrays.asList(V458_NAMES)), true, true);
    }

    /** A build in which every value except "frame cap off" and frame generation is supported: every other row is enabled. */
    private static Capabilities everything() {
        Set<String> names = new HashSet<>(Arrays.asList(V458_NAMES));
        names.addAll(Arrays.asList(LATER_NAMES));
        return Capabilities.of(names, true, true);
    }

    /** A model as the screen has it after loading a valid settings.json whose files match (no banner). */
    private static MenuModel open(Capabilities capabilities, Settings saved) {
        MenuModel model = new MenuModel();
        model.loaded(saved, true, NOTHING, NOTHING, capabilities);
        return model;
    }

    private static SettingSpec spec(String key) {
        return SettingsSchema.spec(key);
    }

    private static int row(MenuModel model, String key) {
        return rowLabelled(model, spec(key).label);
    }

    private static int rowLabelled(MenuModel model, String label) {
        for (int i = 0; i < model.rowCount(); ++i) if (label.equals(model.rowLabel(i))) return i;
        throw new AssertionError("no row labelled " + label);
    }

    /** Moves the focus to a row with DOWN presses, the way a player would. */
    private static void goTo(MenuModel model, int index) {
        for (int presses = 0; presses < model.rowCount() && model.focused() != index; ++presses) model.nav(DOWN, false);
        eq(index, model.focused(), "focus reached row " + index);
    }

    /** The row of a setting must show the text of that value. */
    private static void shows(MenuModel model, String key, int value, String what) {
        eq(spec(key).format(value), model.rowValue(row(model, key)), what);
    }

    /** Presses Start, takes the save the model asks for and returns the settings it wants written. */
    private static Settings startSave(MenuModel model) {
        model.nav(MENU, false);
        eq(MenuModel.ACTION_SAVE, model.pendingAction(), "Start asks for a save");
        return model.beginSave();
    }

    public static void main(String[] args) {
        boolean assertions = false;
        assert assertions = true;
        if (!assertions) throw new IllegalStateException("Run with -ea");

        run("focus visits every row in order and a fresh press wraps", MenuModelTest::focusOrderAndWrap);
        run("a held direction stops at the end instead of wrapping", MenuModelTest::heldDirectionStops);
        run("a disabled row takes the focus, so its reason can be scrolled to with a gamepad, and is inert", MenuModelTest::disabledRowsFocusableAndInert);
        run("frame generation: disabled with 'Not in this version' on every build, also one that has the dry-run switches", MenuModelTest::frameGenerationNotBuilt);
        run("the line under a row says what the chosen value means (measured caps, supersampling, softer and cooler)", MenuModelTest::valueHints);
        run("world sliders: Original at 100, never below the game's floor of 25, draw distance locked", MenuModelTest::worldRows);
        run("a disabled row shows its value and the one-line reason", MenuModelTest::disabledRowShowsReason);
        run("group titles stand above the first row of each group", MenuModelTest::groupTitles);
        run("the footer legend follows the state", MenuModelTest::legend);
        run("LEFT / RIGHT skip unsupported values and stop at the ends", MenuModelTest::leftRightSkipUnsupported);
        run("CONFIRM on a choice goes to the next supported value and wraps", MenuModelTest::confirmCyclesChoice);
        run("slider: one step per command, stops at both limits, CONFIRM does nothing", MenuModelTest::sliderSteps);
        run("toggle: LEFT is Off, RIGHT is On, CONFIRM flips", MenuModelTest::toggle);
        run("a disabled row ignores every change, also by touch", MenuModelTest::disabledRowIgnoresChanges);
        run("FSR sharpness is disabled unless the scaler in use is FSR 1", MenuModelTest::sharpnessNeedsFsr1);
        run("dirty follows the difference to the saved copy; nothing asks for a save by itself", MenuModelTest::dirtyFlag);
        run("BACK with no change leaves", MenuModelTest::backWhenCleanLeaves);
        run("BACK with changes asks; Save writes, then leaves", MenuModelTest::backWhenDirtySave);
        run("BACK with changes asks; Discard leaves without writing", MenuModelTest::backWhenDirtyDiscard);
        run("BACK with changes asks; Keep editing and BACK close the question", MenuModelTest::backWhenDirtyKeepEditing);
        run("a save from the question that fails keeps the screen open", MenuModelTest::failedSaveFromQuestionStays);
        run("Restore original: asks with the focus on Cancel, sets defaults, writes nothing", MenuModelTest::restoreQuestion);
        run("Start and the Save row save the edited copy; the screen stays", MenuModelTest::menuAndSaveRowSave);
        run("commands are ignored while a save is being written", MenuModelTest::ignoredWhileSaving);
        run("the save round trip cannot be entered out of order", MenuModelTest::saveProtocol);
        run("a save refused because the game runs keeps the changes and says why", MenuModelTest::refusedSaveKeepsDirty);
        run("banner when the files differ: Show lists them, Apply again writes the saved copy", MenuModelTest::banner);
        run("a saved value the build no longer supports is shown as a warning and kept", MenuModelTest::unsupportedSavedValue);
        run("the second line names the values that are not available", MenuModelTest::unavailableValuesNamed);
        run("while loading and after a failed load only BACK does something", MenuModelTest::loadingAndFailed);
        run("L1 / R1 jump to the first row of the previous / next group", MenuModelTest::pageJumps);
        run("touch: taps focus, step, flip and answer questions", MenuModelTest::taps);
        run("holding a direction on the controller sweeps a slider and runs the focus to the end", MenuModelTest::holdToRepeat);
        run("the line above the rows: problems in the file, or nothing saved yet", MenuModelTest::noticeLine);
        run("a row holding an unsupported value can be reached and set back alone", MenuModelTest::unsupportedValueReachable);
        run("the line above the rows says so when the game is running", MenuModelTest::gameRunningNotice);
        run("a developer-switch warning survives a save; a note that the game appeared replaces \"Saved.\"", MenuModelTest::standingAndAppeared);
        run("scrolling keeps the focused row and its neighbours visible", MenuModelTest::scrollOffset);
        run("the header's game-code line", MenuModelTest::gameCodeLine);

        if (failures != 0) {
            System.out.println("MENU_MODEL_FAIL: " + failures + " test(s) failed");
            System.exit(1);
        }
        System.out.println("MENU_MODEL_PASS: focus order, wrap, held direction, disabled rows, value stepping, questions, "
                + "save round trip, banner, touch, hold-to-repeat, scroll position");
    }

    // ---- focus ----------------------------------------------------------------------------------------------------

    private static void focusOrderAndWrap() {
        MenuModel model = open(everything(), Settings.defaults());
        List<String> expected = new ArrayList<>();
        for (SettingSpec spec : SettingsSchema.all()) expected.add(spec.label);
        expected.add("Restore original");
        expected.add("Save");
        List<String> visited = new ArrayList<>();
        visited.add(model.rowLabel(model.focused()));
        for (int i = 1; i < expected.size(); ++i) {
            model.nav(DOWN, false);
            visited.add(model.rowLabel(model.focused()));
        }
        eq(expected, visited, "rows visited by DOWN from the start");
        model.nav(DOWN, false);
        eq(expected.get(0), model.rowLabel(model.focused()), "DOWN on the last row");
        model.nav(UP, false);
        eq("Save", model.rowLabel(model.focused()), "UP on the first row");
    }

    private static void heldDirectionStops() {
        MenuModel model = open(everything(), Settings.defaults());
        for (int i = 0; i < 40; ++i) model.nav(DOWN, true);
        eq("Save", model.rowLabel(model.focused()), "after many DOWN repeats");
        for (int i = 0; i < 40; ++i) model.nav(UP, true);
        eq(spec(CAP).label, model.rowLabel(model.focused()), "after many UP repeats");
    }

    private static void disabledRowsFocusableAndInert() {
        MenuModel model = open(v458(), Settings.defaults());
        // v458 has no input for render resolution, frame generation and the five world sliders: those rows are
        // disabled. DOWN still stops on each of them, in order, so the list scrolls there and the reason can be read.
        List<String> expected = new ArrayList<>();
        for (SettingSpec spec : SettingsSchema.all()) expected.add(spec.label);
        expected.addAll(Arrays.asList("Restore original", "Save", spec(CAP).label));
        List<String> visited = new ArrayList<>();
        visited.add(model.rowLabel(model.focused()));
        for (int i = 1; i < expected.size(); ++i) {
            model.nav(DOWN, false);
            visited.add(model.rowLabel(model.focused()));
        }
        eq(expected, visited, "rows visited by DOWN with build v458");
        for (String key : new String[] {RENDER, FRAME_GEN, TRAFFIC}) {
            eq(false, model.rowEnabled(row(model, key)), key + " enabled with build v458");
        }
        // On a disabled row every command that would change a value does nothing.
        Settings before = startSaveCopy(model);
        for (String key : new String[] {RENDER, FRAME_GEN, TRAFFIC}) {
            goTo(model, row(model, key));
            eq(row(model, key), model.focused(), "the focus stands on the disabled row " + key);
            for (int command : new int[] {LEFT, RIGHT, CONFIRM, LEFT, RIGHT}) model.nav(command, false);
            for (int command : new int[] {LEFT, RIGHT}) model.nav(command, true);
            shows(model, key, spec(key).defaultValue, key + " after LEFT, RIGHT and A on the disabled row");
            eq(true, model.rowSecondLine(model.focused()) != null, "the focused disabled row shows its reason");
            eq(true, model.legend().startsWith("Not available"), "the footer says the row is not available: " + model.legend());
            eq(true, model.legend().contains("Y Restore original") && model.legend().contains("Start Save") && model.legend().contains("B Back"),
                    "the footer of a grey row names every key that still works there: " + model.legend());
        }
        eq(false, model.dirty(), "nothing changed");
        eq(before, startSaveCopy(model), "the settings a save would write are unchanged");
        // L1 / R1 land on the first row of a group even when it is disabled.
        goTo(model, row(model, CAP));
        model.nav(NavInput.PAGE_NEXT, false);
        eq(spec(TRAFFIC).label, model.rowLabel(model.focused()), "R1 from Picture lands on the first World row, disabled or not");

        // A build whose only extra is the frame-rate counter: the first row (frame cap) is disabled and still where the focus starts.
        MenuModel sparse = open(Capabilities.of(new HashSet<>(Arrays.asList("diag-fps-overlay")), true, false),
                Settings.defaults());
        eq(spec(CAP).label, sparse.rowLabel(sparse.focused()), "where the focus starts when the first row is disabled");
        // The phone's standing library reads no setting file: every game row is disabled, every row is reachable.
        MenuModel old = open(Capabilities.of(new HashSet<String>(), true, true), Settings.defaults());
        int reachable = 0;
        for (int i = 0; i < old.rowCount(); ++i) {
            eq(i, old.focused(), "DOWN reaches row " + i + " with the old library");
            if (old.rowKind(i) != MenuModel.KIND_ACTION && !spec(INPUT).label.equals(old.rowLabel(i))) {
                eq(false, old.rowEnabled(i), old.rowLabel(i) + " enabled with a library that reads no setting file");
                eq(true, old.rowSecondLine(i) != null, old.rowLabel(i) + " has a reason");
            }
            old.nav(DOWN, false);
            reachable++;
        }
        eq(14, reachable, "twelve setting rows and the two action rows");
    }

    /** The settings the model would hand to a save, without disturbing the model under test (a fresh press of Start on a copy is not possible, so read the rows). */
    private static Settings startSaveCopy(MenuModel model) {
        Settings out = Settings.defaults();
        for (SettingSpec spec : SettingsSchema.all()) {
            String shown = model.rowValue(row(model, spec.key));
            for (int value = spec.min; value <= spec.max; value += spec.step) {
                if (spec.format(value).equals(shown)) out = out.with(spec.key, value);
            }
        }
        return out;
    }

    private static void frameGenerationNotBuilt() {
        Set<String> withDryRun = new HashSet<>(Arrays.asList(V458_NAMES));
        withDryRun.addAll(Arrays.asList(LATER_NAMES));
        withDryRun.addAll(Arrays.asList("diag-fg-pair-ahead", "diag-fg-pair-between", "diag-fg-hold", "diag-fg-nocap"));
        for (Capabilities build : new Capabilities[] {v458(), everything(), Capabilities.of(withDryRun, true, true),
                Capabilities.of(new HashSet<String>(), true, true)}) {
            MenuModel model = open(build, Settings.defaults());
            int generation = row(model, FRAME_GEN);
            eq(false, model.rowEnabled(generation), "frame generation enabled");
            eq("Not in this version", model.rowSecondLine(generation), "the reason under frame generation");
            eq(MenuModel.TONE_DISABLED, model.rowSecondLineTone(generation), "tone of the reason");
            eq("Off", model.rowValue(generation), "the value shown");
            goTo(model, generation);
            model.nav(RIGHT, false);
            model.nav(CONFIRM, false);
            model.tapArrow(generation, 1);
            eq("Off", model.rowValue(generation), "the value after RIGHT, A and a tap");
            eq(false, model.dirty(), "nothing to save");
        }
    }

    private static void valueHints() {
        MenuModel model = open(everything(), Settings.defaults());
        int cap = row(model, CAP), render = row(model, RENDER);
        goTo(model, cap);
        model.nav(RIGHT, false);
        eq("40 fps", model.rowValue(cap), "one RIGHT from 60");
        eq("Measured on an AYN Thor: holds indoors. The Thor's second-screen dashboard makes some frames uneven.", model.rowSecondLine(cap), "40 fps");
        model.nav(RIGHT, false);
        eq("Measured on an AYN Thor: holds while driving (84 % of frames evenly paced).", model.rowSecondLine(cap), "30 fps");
        model.nav(RIGHT, false);
        model.nav(RIGHT, false);
        eq("Automatic", model.rowValue(cap), "the last value");
        eq("Picks the cap itself. Measured on an AYN Thor: 40 indoors, 24 while driving.", model.rowSecondLine(cap), "automatic");
        goTo(model, render);
        for (int index = 4; index <= 6; ++index) {
            model.nav(RIGHT, false);
            eq(true, model.rowValue(render).contains("supersampling"), "above 100 % the value is labelled supersampling: " + model.rowValue(render));
            eq(SettingsSchema.HINT_SUPERSAMPLING, model.rowSecondLine(render), "above 100 %: the line under the row");
        }
        eq("Supersampling through a 1280x720 picture: smoother edges, more heat. Not native 1080p.", model.rowSecondLine(render), "150 % is not called native 1080p");
        eq(true, model.rowValue(render).startsWith("150 % (1920x1080"), "the last step");
        for (int i = 0; i < 4; ++i) model.nav(LEFT, false);
        eq(true, model.rowValue(render).startsWith("88 %"), "below 100 %");
        eq(SettingsSchema.HINT_SOFTER, model.rowSecondLine(render), "below 100 %: softer and cooler");
        eq("Softer and cooler: fewer pixels are drawn.", SettingsSchema.HINT_SOFTER, "the text");
        // Every second line is one line on the screen: about 100 characters fit at the hint size.
        for (Capabilities build : new Capabilities[] {v458(), everything(), Capabilities.of(new HashSet<String>(), true, false)}) {
            for (SettingSpec spec : SettingsSchema.all()) {
                for (int value = spec.min; value <= spec.max; value += spec.step) {
                    String hint = SettingsSchema.hint(spec.key, value), reason = build.reason(spec.key, value);
                    eq(true, hint == null || hint.length() <= 100, "the hint fits one line: " + hint);
                    eq(true, reason == null || reason.length() <= 100, "the reason fits one line: " + reason);
                }
            }
        }
        MenuModel far = open(everything(), Settings.defaults().with(SettingsSchema.WORLD_DRAW_DISTANCE, 80).with(TRAFFIC, 50));
        eq("Also shortens how far away traffic and people are simulated.", far.rowSecondLine(row(far, SettingsSchema.WORLD_DRAW_DISTANCE)), "draw distance below the original");
        eq("Moving cars the game adds around you.", far.rowSecondLine(row(far, TRAFFIC)), "traffic below the original");
        eq(null, far.rowSecondLine(row(far, SettingsSchema.WORLD_PEDESTRIANS)), "a slider at the original has no line");
    }

    private static void worldRows() {
        MenuModel model = open(everything(), Settings.defaults());
        for (String key : new String[] {TRAFFIC, SettingsSchema.WORLD_PEDESTRIANS, SettingsSchema.WORLD_PARKED_CARS,
                SettingsSchema.WORLD_SCENARIO_PEDESTRIANS}) {
            int index = row(model, key);
            eq("Original", model.rowValue(index), key + " at the default");
            goTo(model, index);
            for (int i = 0; i < 40; ++i) model.nav(LEFT, false);
            eq("25 %", model.rowValue(index), key + " cannot go below the game's floor");
            eq(false, model.rowCanStep(index, -1), key + ": no further step down");
        }
        int distance = row(model, SettingsSchema.WORLD_DRAW_DISTANCE);
        goTo(model, distance);
        for (int i = 0; i < 40; ++i) model.nav(LEFT, false);
        eq("70 %", model.rowValue(distance), "draw distance cannot go below 70 in a build that has it");
        // v461: the world file is read, draw distance is locked inside the game
        MenuModel v461 = open(Capabilities.of(new HashSet<>(Arrays.asList(SettingsCoreTestKit.V461_NAMES)), true, true), Settings.defaults());
        eq(true, v461.rowEnabled(row(v461, TRAFFIC)), "v461: traffic enabled");
        int locked = row(v461, SettingsSchema.WORLD_DRAW_DISTANCE);
        eq(false, v461.rowEnabled(locked), "v461: draw distance enabled");
        eq("Not in this version", v461.rowSecondLine(locked), "v461: the reason under draw distance");
        goTo(v461, locked);
        v461.nav(LEFT, false);
        eq("Original", v461.rowValue(locked), "v461: LEFT on the locked row");
    }

    private static void disabledRowShowsReason() {
        Capabilities capabilities = v458();
        MenuModel model = open(capabilities, Settings.defaults());
        int traffic = row(model, TRAFFIC);
        eq(spec(TRAFFIC).format(100), model.rowValue(traffic), "a disabled row still shows its value");
        eq(capabilities.reason(TRAFFIC, 50), model.rowSecondLine(traffic), "reason under the traffic row");
        eq(MenuModel.TONE_DISABLED, model.rowSecondLineTone(traffic), "tone of that reason");
        eq(MenuModel.TONE_DISABLED, model.rowValueTone(traffic), "tone of the disabled row's value");
        eq(capabilities.reason(FRAME_GEN, 1), model.rowSecondLine(row(model, FRAME_GEN)), "reason under frame generation");
        eq(true, model.rowSecondLine(traffic) != null && !model.rowSecondLine(traffic).isEmpty(), "the reason is a text");
        eq(MenuModel.TONE_NORMAL, model.rowValueTone(row(model, OVERLAY)), "tone of an enabled row's value");
        // a world that is not the original is visible at a glance (lane world-load): the value in the warning colour
        MenuModel world = open(everything(), Settings.defaults().with(TRAFFIC, 95));
        eq(MenuModel.TONE_WARNING, world.rowValueTone(row(world, TRAFFIC)), "tone of a world value below Original");
        eq(MenuModel.TONE_NORMAL, world.rowValueTone(row(world, SettingsSchema.WORLD_PEDESTRIANS)), "tone of a world value at Original");
        eq(MenuModel.TONE_NORMAL, open(everything(), Settings.defaults().with(SHARPNESS, 10)).rowValueTone(row(world, SHARPNESS)), "a picture slider off its default keeps the normal tone");

        // A build with only the frame-rate counter: the frame cap row is disabled. Its values have three different
        // reasons (off: one, 30 fps: one, 40 / 24 / automatic: the same one); the row shows the one most values share.
        Capabilities sparse = Capabilities.of(new HashSet<>(Arrays.asList("diag-fps-overlay")), true, false);
        MenuModel other = open(sparse, Settings.defaults());
        eq(false, other.rowEnabled(row(other, CAP)), "frame cap enabled in the sparse build");
        eq(sparse.reason(CAP, 2), other.rowSecondLine(row(other, CAP)), "reason under the disabled frame cap row");
    }

    private static void groupTitles() {
        MenuModel model = open(everything(), Settings.defaults());
        eq("Picture", model.rowGroupTitle(row(model, CAP)), "title above the first row");
        eq(null, model.rowGroupTitle(row(model, SCALER)), "title above a row inside a group");
        eq("World\nOriginal = the game as shipped. Lower values are your choice: the game then creates fewer cars and people.",
                model.rowGroupTitle(row(model, TRAFFIC)), "title above the first world row, with the lane's line under it");
        eq("Controller", model.rowGroupTitle(row(model, INPUT)), "title above the controller row");
        eq(null, model.rowGroupTitle(rowLabelled(model, "Restore original")), "title above Restore original");
        eq(null, model.rowGroupTitle(rowLabelled(model, "Save")), "title above Save");
        MenuModel drifted = new MenuModel();
        drifted.loaded(Settings.defaults(), true, NOTHING, Arrays.asList("settings/scaler.txt"), everything());
        eq(null, drifted.rowGroupTitle(0), "title above the banner");
        eq("Picture", drifted.rowGroupTitle(1), "title above the first setting under a banner");
    }

    private static void legend() {
        // What matters is that the legend says what B, Y and Start do in the state the player is in.
        eq("B Back", new MenuModel().legend(), "legend while loading");
        MenuModel model = open(v458(), Settings.defaults());
        for (String part : new String[] {"B Back", "Y Restore original", "Start Save"}) {
            eq(true, model.legend().contains(part), "legend on the list names \"" + part + "\": " + model.legend());
        }
        model.nav(ALT, false);
        eq(true, model.legend().contains("B Cancel"), "legend in the restore question: " + model.legend());
        eq(false, model.legend().contains("Start Save"), "Start is not offered inside a question: " + model.legend());
        model.nav(BACK, false);
        goTo(model, row(model, OVERLAY));
        model.nav(RIGHT, false);
        model.nav(BACK, false);
        eq(true, model.legend().contains("B Keep editing"), "legend in the save question: " + model.legend());
    }

    private static void pageJumps() {
        MenuModel model = open(everything(), Settings.defaults());
        String[] forward = {spec(TRAFFIC).label, spec(INPUT).label, "Restore original", "Restore original"};
        for (String label : forward) {
            model.nav(NavInput.PAGE_NEXT, false);
            eq(label, model.rowLabel(model.focused()), "after R1");
        }
        String[] backward = {spec(INPUT).label, spec(TRAFFIC).label, spec(CAP).label, spec(CAP).label};
        for (String label : backward) {
            model.nav(NavInput.PAGE_PREV, false);
            eq(label, model.rowLabel(model.focused()), "after L1");
        }
        goTo(model, row(model, RENDER));
        model.nav(NavInput.PAGE_PREV, false);
        eq(spec(CAP).label, model.rowLabel(model.focused()), "L1 inside the first group goes to its first row");

        MenuModel old = open(v458(), Settings.defaults());
        old.nav(NavInput.PAGE_NEXT, false);
        eq(spec(TRAFFIC).label, old.rowLabel(old.focused()), "R1 lands on a group whose rows are all disabled, so that it can be read");
    }

    // ---- values ---------------------------------------------------------------------------------------------------

    private static void leftRightSkipUnsupported() {
        // v458 supports 60 fps (the default, index 1) and 30 fps (index 3); off, 40, 24 and automatic are not in it.
        MenuModel model = open(v458(), Settings.defaults());
        int cap = row(model, CAP);
        eq(cap, model.focused(), "the frame cap row has the focus");
        eq(false, model.rowCanStep(cap, -1), "left arrow active at 60 fps");
        eq(true, model.rowCanStep(cap, 1), "right arrow active at 60 fps");
        model.nav(RIGHT, false);
        shows(model, CAP, 3, "RIGHT from 60 fps skips 40 fps");
        eq(true, model.rowCanStep(cap, -1), "left arrow active at 30 fps");
        eq(false, model.rowCanStep(cap, 1), "right arrow active at 30 fps");
        model.nav(RIGHT, false);
        shows(model, CAP, 3, "RIGHT at the last supported value");
        model.nav(LEFT, false);
        shows(model, CAP, 1, "LEFT from 30 fps skips 40 fps");
        model.nav(LEFT, false);
        shows(model, CAP, 1, "LEFT at the first supported value");
    }

    private static void confirmCyclesChoice() {
        // v458 scaler: FSR 1 (0), bicubic (2) and nearest (3); GSR 1 (1) is not in it.
        MenuModel model = open(v458(), Settings.defaults());
        goTo(model, row(model, SCALER));
        model.nav(CONFIRM, false);
        shows(model, SCALER, 2, "A on FSR 1 skips GSR 1");
        model.nav(CONFIRM, false);
        shows(model, SCALER, 3, "A on bicubic");
        model.nav(CONFIRM, false);
        shows(model, SCALER, 0, "A on the last value wraps to the first");
    }

    private static void sliderSteps() {
        MenuModel model = open(everything(), Settings.defaults());
        int traffic = row(model, TRAFFIC);
        goTo(model, traffic);
        eq(1f, model.rowFraction(traffic), "bar at 100 %");
        eq(-1f, model.rowFraction(row(model, CAP)), "a choice row has no bar");
        model.nav(RIGHT, false);
        shows(model, TRAFFIC, 100, "RIGHT at the upper limit");
        model.nav(LEFT, false);
        shows(model, TRAFFIC, 95, "one LEFT");
        model.nav(LEFT, true);
        shows(model, TRAFFIC, 90, "a repeated LEFT is one more step");
        model.nav(CONFIRM, false);
        shows(model, TRAFFIC, 90, "A on a slider");
        for (int i = 0; i < 30; ++i) model.nav(LEFT, i % 2 == 0);
        shows(model, TRAFFIC, 25, "LEFT far past the lower limit (the game's floor)");
        eq(0.25f, model.rowFraction(traffic), "the bar at the floor shows a quarter, not an empty bar (25 % is not none)");
        eq(false, model.rowCanStep(traffic, -1), "left arrow active at the lower limit");

        // FSR sharpness counts tenths: 0 .. 40 in steps of 1, default 2.
        int sharpness = row(model, SHARPNESS);
        model.tapRow(sharpness);
        model.nav(LEFT, false);
        shows(model, SHARPNESS, 1, "sharpness one LEFT from 0.2");
        for (int i = 0; i < 5; ++i) model.nav(LEFT, false);
        shows(model, SHARPNESS, 0, "sharpness at its lower limit");
        for (int i = 0; i < 60; ++i) model.nav(RIGHT, true);
        shows(model, SHARPNESS, 40, "sharpness at its upper limit");
    }

    private static void toggle() {
        MenuModel model = open(v458(), Settings.defaults());
        goTo(model, row(model, OVERLAY));
        model.nav(LEFT, false);
        shows(model, OVERLAY, 0, "LEFT when Off");
        model.nav(RIGHT, false);
        shows(model, OVERLAY, 1, "RIGHT");
        model.nav(RIGHT, false);
        shows(model, OVERLAY, 1, "RIGHT when On");
        model.nav(LEFT, false);
        shows(model, OVERLAY, 0, "LEFT");
        model.nav(CONFIRM, false);
        shows(model, OVERLAY, 1, "A when Off");
        model.nav(CONFIRM, false);
        shows(model, OVERLAY, 0, "A when On");
    }

    private static void disabledRowIgnoresChanges() {
        // Low-delay input is not available here (third argument false), so that toggle is disabled as well.
        MenuModel model = open(Capabilities.of(new HashSet<>(Arrays.asList(V458_NAMES)), true, false), Settings.defaults());
        int start = model.focused();
        model.tapArrow(row(model, TRAFFIC), -1);
        shows(model, TRAFFIC, 100, "left arrow tapped on the disabled traffic slider");
        model.tapArrow(row(model, RENDER), 1);
        shows(model, RENDER, spec(RENDER).defaultValue, "right arrow tapped on disabled render resolution");
        model.tapRow(row(model, INPUT));
        shows(model, INPUT, 0, "tap on the disabled low-delay toggle");
        eq(row(model, INPUT), model.focused(), "a tap puts the highlight on a disabled row (its reason is then in view)");
        eq(true, start != model.focused(), "test setup: the tapped row is not where the focus started");
        eq(false, model.dirty(), "nothing changed");
        eq(false, model.rowCanStep(row(model, TRAFFIC), -1), "arrow of a disabled row drawn active");
    }

    private static void sharpnessNeedsFsr1() {
        MenuModel model = open(v458(), Settings.defaults());
        int scaler = row(model, SCALER), sharpness = row(model, SHARPNESS);
        eq(true, model.rowEnabled(sharpness), "sharpness enabled with FSR 1");
        goTo(model, scaler);
        model.nav(CONFIRM, false);   // bicubic
        eq(false, model.rowEnabled(sharpness), "sharpness enabled with bicubic");
        eq(MenuModel.TEXT_ONLY_FSR1, model.rowSecondLine(sharpness), "the reason under sharpness");
        eq(MenuModel.TONE_DISABLED, model.rowSecondLineTone(sharpness), "tone of that reason");
        model.tapArrow(sharpness, 1);
        shows(model, SHARPNESS, 2, "arrow tapped on sharpness while the scaler is bicubic");
        eq(false, model.rowCanStep(sharpness, 1), "sharpness arrow drawn active while the scaler is bicubic");
        goTo(model, scaler);
        model.nav(DOWN, false);
        eq(spec(SHARPNESS).label, model.rowLabel(model.focused()), "DOWN from the scaler stops on the disabled sharpness row");
        model.nav(RIGHT, false);
        model.nav(CONFIRM, false);
        shows(model, SHARPNESS, 2, "RIGHT and A on the focused sharpness row while the scaler is bicubic");
        goTo(model, scaler);
        model.nav(CONFIRM, false);   // nearest
        model.nav(CONFIRM, false);   // FSR 1 again
        eq(true, model.rowEnabled(sharpness), "sharpness enabled after returning to FSR 1");

        // GSR 1 was saved but v458 does not have it: the game uses FSR 1, so sharpness applies.
        MenuModel rolledBack = open(v458(), Settings.defaults().with(SCALER, 1));
        eq(true, rolledBack.rowEnabled(row(rolledBack, SHARPNESS)), "sharpness enabled when the scaler in use is FSR 1");
    }

    private static void unsupportedSavedValue() {
        // 40 fps (index 2) was saved, then build v458 came back, which cannot do it.
        MenuModel model = open(v458(), Settings.defaults().with(CAP, 2));
        int cap = row(model, CAP);
        shows(model, CAP, 2, "the saved value is still shown");
        eq(MenuModel.TONE_WARNING, model.rowValueTone(cap), "tone of the unsupported value");
        eq(spec(CAP).format(2) + " - not in this game build; the game uses " + spec(CAP).format(1),
                model.rowSecondLine(cap), "second line of the row");
        // that line is one line on the screen for every value of every row (review: 113 characters for 150 %)
        for (SettingSpec each : SettingsSchema.all()) {
            for (int value = each.min; value <= each.max; value += each.step) {
                if (value == each.defaultValue) continue;
                MenuModel old = open(Capabilities.of(new HashSet<String>(), true, false), Settings.defaults().with(each.key, value));
                String line = old.rowSecondLine(row(old, each.key));
                eq(true, line.contains(" - not in this game build; the game uses ") && line.length() <= 100, "the line fits (" + line.length() + "): " + line);
            }
        }
        eq(MenuModel.TONE_WARNING, model.rowSecondLineTone(cap), "tone of that line");
        eq(false, model.dirty(), "showing it is not a change");
        eq(Settings.defaults().with(CAP, 2), startSave(model), "a save without edits keeps the unsupported choice");

        MenuModel right = open(v458(), Settings.defaults().with(CAP, 2));
        right.nav(RIGHT, false);
        shows(right, CAP, 3, "RIGHT from the unsupported 40 fps");
        MenuModel left = open(v458(), Settings.defaults().with(CAP, 2));
        left.nav(LEFT, false);
        shows(left, CAP, 1, "LEFT from the unsupported 40 fps");
    }

    private static void unavailableValuesNamed() {
        Capabilities capabilities = v458();
        MenuModel model = open(capabilities, Settings.defaults());
        SettingSpec cap = spec(CAP);
        // "Off" and "40 / 24 / automatic" have different reasons, so the plain sentence introduces the list.
        eq(MenuModel.TEXT_NOT_IN_BUILD + ": " + cap.format(0) + ", " + cap.format(2) + ", " + cap.format(4) + ", "
                + cap.format(5), model.rowSecondLine(row(model, CAP)), "second line of the frame cap row");
        eq(MenuModel.TONE_NORMAL, model.rowSecondLineTone(row(model, CAP)), "tone of that line");
        // Only GSR 1 is missing from the scaler row: its own reason introduces it.
        eq(capabilities.reason(SCALER, 1) + ": " + spec(SCALER).format(1), model.rowSecondLine(row(model, SCALER)),
                "second line of the scaler row");
        eq(null, model.rowSecondLine(row(model, OVERLAY)), "a row with every value available and no change");
    }

    // ---- dirty, questions, saving -----------------------------------------------------------------------------------

    private static void dirtyFlag() {
        MenuModel model = open(v458(), Settings.defaults());
        int counter = row(model, OVERLAY);
        eq(false, model.dirty(), "dirty right after loading");
        goTo(model, counter);
        model.nav(RIGHT, false);
        eq(true, model.dirty(), "dirty after a change");
        eq(true, model.rowChanged(counter), "the changed row is marked");
        eq(false, model.rowChanged(row(model, CAP)), "an untouched row is marked");
        eq(MenuModel.TEXT_NEXT_START, model.rowSecondLine(counter), "hint under the changed row");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "an edit alone asks for nothing");
        model.nav(LEFT, false);
        eq(false, model.dirty(), "dirty after changing the value back");
        eq(false, model.rowChanged(counter), "the row is marked after changing it back");
        eq(null, model.rowSecondLine(counter), "hint after changing it back");
    }

    private static void backWhenCleanLeaves() {
        MenuModel model = open(v458(), Settings.defaults());
        model.nav(DOWN, false);
        eq(false, model.nav(BACK, false), "BACK is handled (false means: pop this screen)");
        eq(MenuModel.ACTION_LEAVE, model.pendingAction(), "pending action");
        eq(MenuModel.OVERLAY_NONE, model.overlay(), "no question is shown");
        // Until the host has removed the screen: BACK keeps saying "pop me", everything else is swallowed.
        int focus = model.focused();
        eq(true, model.nav(DOWN, false), "DOWN after leaving is swallowed");
        eq(focus, model.focused(), "focus after leaving");
        eq(false, model.nav(BACK, false), "a second BACK still asks to be popped");
    }

    /** A model with one unsaved change (frame-rate counter on) and the save question open. */
    private static MenuModel askedToSave() {
        MenuModel model = open(v458(), Settings.defaults());
        goTo(model, row(model, OVERLAY));
        model.nav(RIGHT, false);
        eq(true, model.nav(BACK, false), "BACK with changes is handled by the screen");
        eq(MenuModel.OVERLAY_SAVE_QUESTION, model.overlay(), "the save question is open");
        eq("Save changes?", model.overlayTitle(), "the question");
        eq(null, model.overlayDetail(), "the save question has no second line");
        // Keep editing has the focus first: A at once writes and loses nothing (menu-input-fixes D7).
        eq("Keep editing", model.overlayButtonLabel(model.overlayFocused()), "button with the focus");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "nothing happens before an answer");
        model.nav(LEFT, false);
        model.nav(LEFT, false);
        eq("Save", model.overlayButtonLabel(model.overlayFocused()), "two steps left: Save");
        return model;
    }

    private static void backWhenDirtySave() {
        MenuModel model = askedToSave();
        model.nav(CONFIRM, false);
        eq(MenuModel.ACTION_SAVE, model.pendingAction(), "Save answers with a save");
        eq(Settings.defaults().with(OVERLAY, 1), model.beginSave(), "what is written");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "nothing more is pending while writing");
        model.saveFinished(true, false, null);
        eq(MenuModel.ACTION_LEAVE, model.pendingAction(), "after the write the screen leaves");
    }

    private static void backWhenDirtyDiscard() {
        MenuModel model = askedToSave();
        model.nav(RIGHT, false);
        eq("Discard", model.overlayButtonLabel(model.overlayFocused()), "button right of Save");
        model.nav(CONFIRM, false);
        eq(MenuModel.ACTION_LEAVE, model.pendingAction(), "Discard leaves; no save was asked for");
    }

    private static void backWhenDirtyKeepEditing() {
        MenuModel model = askedToSave();
        model.nav(MENU, false);
        model.nav(ALT, false);
        eq(MenuModel.OVERLAY_SAVE_QUESTION, model.overlay(), "Start and Y do nothing inside a question");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "Start inside a question asks for nothing");
        model.nav(LEFT, false);
        eq("Save", model.overlayButtonLabel(model.overlayFocused()), "LEFT on the first button stays");
        for (int i = 0; i < 5; ++i) model.nav(RIGHT, false);
        eq("Keep editing", model.overlayButtonLabel(model.overlayFocused()), "RIGHT stops on the last button");
        model.nav(CONFIRM, false);
        eq(MenuModel.OVERLAY_NONE, model.overlay(), "Keep editing closes the question");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "Keep editing asks for nothing");
        eq(true, model.dirty(), "the change is still there");

        MenuModel other = askedToSave();
        eq(true, other.nav(BACK, false), "BACK inside the question is handled");
        eq(MenuModel.OVERLAY_NONE, other.overlay(), "BACK closes the question");
        eq(MenuModel.ACTION_NONE, other.pendingAction(), "BACK inside the question does not leave");
        eq(true, other.dirty(), "the change is still there");
    }

    private static void failedSaveFromQuestionStays() {
        MenuModel model = askedToSave();
        model.nav(CONFIRM, false);
        model.beginSave();
        model.saveFinished(false, true, "The game is running. Close it, then save.");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "a refused save does not leave");
        eq(true, model.dirty(), "the change is kept");
        // A later, ordinary save must not leave either: the question's "then leave" ended with the failure.
        startSave(model);
        model.saveFinished(true, false, null);
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "an ordinary save after the failed one stays on the screen");

        MenuModel broken = askedToSave();
        broken.nav(CONFIRM, false);
        broken.beginSave();
        broken.saveFinished(false, false, "disk full");
        eq(MenuModel.ACTION_NONE, broken.pendingAction(), "a save that failed with an error does not leave");
        eq(MenuModel.TEXT_SAVE_FAILED + "disk full", broken.status(), "the error is shown");
        eq(MenuModel.TONE_ERROR, broken.statusTone(), "tone of the error");
        eq(true, broken.dirty(), "the change is kept");
        startSave(broken);
        broken.saveFinished(false, false, null);
        eq(MenuModel.TEXT_SAVE_FAILED + "unknown error", broken.status(), "a failure without a text");
    }

    private static void restoreQuestion() {
        Settings saved = Settings.defaults().with(OVERLAY, 1).with(CAP, 3).with(SHARPNESS, 9);
        MenuModel model = open(v458(), saved);
        model.nav(ALT, false);
        eq(MenuModel.OVERLAY_RESTORE, model.overlay(), "Y opens the restore question");
        eq("Put every setting back to the original?", model.overlayTitle(), "the question");
        eq("Then choose Save.", model.overlayDetail(), "the line under the question");
        eq("Cancel", model.overlayButtonLabel(model.overlayFocused()), "button with the focus");
        model.nav(RIGHT, false);
        eq("Cancel", model.overlayButtonLabel(model.overlayFocused()), "RIGHT on the last button stays there");
        model.nav(CONFIRM, false);
        eq(MenuModel.OVERLAY_NONE, model.overlay(), "Cancel closes the question");
        shows(model, CAP, 3, "Cancel changes nothing");
        eq(false, model.dirty(), "Cancel changes nothing");

        model.nav(ALT, false);
        model.nav(BACK, false);
        eq(MenuModel.OVERLAY_NONE, model.overlay(), "BACK closes the question");
        shows(model, OVERLAY, 1, "BACK changes nothing");

        goTo(model, rowLabelled(model, "Restore original"));
        model.nav(CONFIRM, false);
        eq(MenuModel.OVERLAY_RESTORE, model.overlay(), "the Restore row opens the question");
        eq("Cancel", model.overlayButtonLabel(model.overlayFocused()), "button with the focus");
        model.nav(LEFT, false);
        eq("Restore", model.overlayButtonLabel(model.overlayFocused()), "button left of Cancel");
        model.nav(CONFIRM, false);
        eq(MenuModel.OVERLAY_NONE, model.overlay(), "Restore closes the question");
        for (SettingSpec spec : SettingsSchema.all()) shows(model, spec.key, spec.defaultValue, spec.key + " after Restore");
        eq(true, model.dirty(), "the defaults differ from what was saved");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "Restore itself writes nothing");
        eq("Save", model.rowLabel(model.focused()), "the focus is on Save afterwards");
        model.nav(CONFIRM, false);
        eq(MenuModel.ACTION_SAVE, model.pendingAction(), "A on Save");
        eq(Settings.defaults(), model.beginSave(), "what is written after Restore");

        MenuModel alreadyOriginal = open(v458(), Settings.defaults());
        alreadyOriginal.nav(ALT, false);
        alreadyOriginal.nav(LEFT, false);
        alreadyOriginal.nav(CONFIRM, false);
        eq(false, alreadyOriginal.dirty(), "Restore when everything already is the original");
    }

    private static void menuAndSaveRowSave() {
        MenuModel model = open(v458(), Settings.defaults());
        model.nav(RIGHT, false);   // frame cap 30
        Settings written = startSave(model);
        eq(Settings.defaults().with(CAP, 3), written, "what Start writes");
        eq(MenuModel.TEXT_SAVING, model.status(), "status while writing");
        model.saveFinished(true, false, null);
        eq(false, model.dirty(), "dirty after a successful save");
        eq(false, model.rowChanged(row(model, CAP)), "the row is marked after the save");
        eq(MenuModel.TEXT_SAVED, model.status(), "status after the save");
        eq(MenuModel.TONE_GOOD, model.statusTone(), "tone of that status");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "the screen stays open");
        shows(model, CAP, 3, "the saved value is still shown");
        model.clearSavedStatus();
        eq(null, model.status(), "status after two seconds");

        goTo(model, rowLabelled(model, "Save"));
        model.nav(CONFIRM, false);
        eq(MenuModel.ACTION_SAVE, model.pendingAction(), "A on the Save row");
        eq(Settings.defaults().with(CAP, 3), model.beginSave(), "what the Save row writes");
    }

    private static void ignoredWhileSaving() {
        MenuModel model = open(v458(), Settings.defaults());
        model.nav(RIGHT, false);
        startSave(model);
        int focus = model.focused();
        model.nav(DOWN, false);
        model.nav(LEFT, false);
        model.nav(ALT, false);
        model.tapRow(row(model, OVERLAY));
        eq(true, model.nav(BACK, false), "BACK while writing is swallowed");
        eq(focus, model.focused(), "focus while writing");
        shows(model, CAP, 3, "value while writing");
        shows(model, OVERLAY, 0, "a tapped toggle while writing");
        eq(MenuModel.OVERLAY_NONE, model.overlay(), "no question opens while writing");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "nothing new is pending while writing");
        model.saveFinished(true, false, null);
        model.nav(DOWN, false);
        eq(true, model.focused() != focus, "commands work again after the write");
    }

    private static void saveProtocol() {
        MenuModel model = open(v458(), Settings.defaults());
        boolean refused = false;
        try {
            model.beginSave();
        } catch (IllegalStateException expected) {
            refused = true;
        }
        eq(true, refused, "beginSave without a pending save is refused");
        model.saveFinished(true, false, null);
        eq(null, model.status(), "a result without a save in progress shows nothing");
        eq(false, model.dirty(), "a result without a save in progress changes nothing");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "a result without a save in progress asks for nothing");
    }

    private static void refusedSaveKeepsDirty() {
        MenuModel model = open(v458(), Settings.defaults());
        model.nav(RIGHT, false);
        startSave(model);
        model.saveFinished(false, true, "The game is running. Close it, then save.");
        eq(true, model.dirty(), "dirty after a refused save");
        eq(true, model.rowChanged(row(model, CAP)), "the row is still marked");
        eq(MenuModel.TEXT_GAME_RUNNING, model.status(), "the line that says why");
        eq(MenuModel.TONE_WARNING, model.statusTone(), "tone of that line");
        model.clearSavedStatus();
        eq(MenuModel.TEXT_GAME_RUNNING, model.status(), "the two-second timer only removes \"Saved.\"");
        model.nav(DOWN, false);
        eq(null, model.status(), "the next command removes the line");
    }

    private static void banner() {
        List<String> drift = Arrays.asList("private-trace/diag-fps-overlay", "settings/scaler.txt");
        Settings saved = Settings.defaults().with(CAP, 3);
        MenuModel model = new MenuModel();
        model.loaded(saved, true, NOTHING, drift, v458());
        eq(MenuModel.KIND_BANNER, model.rowKind(0), "kind of the first row");
        eq(true, model.rowLabel(0).endsWith("(2 files)."), "the banner counts the files: " + model.rowLabel(0));
        eq(spec(CAP).label, model.rowLabel(model.focused()), "the focus starts on the first setting, not on the banner");

        goTo(model, row(model, OVERLAY));
        model.nav(RIGHT, false);
        eq(true, model.dirty(), "an unsaved edit exists");

        goTo(model, row(model, CAP));
        model.nav(UP, false);
        eq(0, model.focused(), "UP from the first setting reaches the banner");
        eq("Show", model.bannerButtonLabel(model.bannerFocusedButton()), "banner button with the focus");
        model.nav(CONFIRM, false);
        eq(MenuModel.OVERLAY_DRIFT_LIST, model.overlay(), "Show opens the list");
        eq("Setting files that differ from your saved settings", model.overlayTitle(), "title of the list");
        eq("Close", model.overlayButtonLabel(model.overlayFocused()), "the list's one button");
        eq(true, model.legend().contains("B Close"), "legend while the list is open: " + model.legend());
        eq("private-trace/diag-fps-overlay\nsettings/scaler.txt", model.overlayDetail(), "the list");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "Show writes nothing");
        model.nav(CONFIRM, false);
        eq(MenuModel.OVERLAY_NONE, model.overlay(), "Close");

        model.nav(LEFT, false);
        eq("Apply again", model.bannerButtonLabel(model.bannerFocusedButton()), "banner button left of Show");
        model.nav(CONFIRM, false);
        eq(MenuModel.ACTION_SAVE, model.pendingAction(), "Apply again asks for a save");
        eq(saved, model.beginSave(), "Apply again writes the saved settings, not the unsaved edit");
        model.saveFinished(true, false, null);
        eq(MenuModel.KIND_CHOICE, model.rowKind(0), "the banner is gone after the write");
        eq(spec(CAP).label, model.rowLabel(model.focused()), "the focus moved from the banner to the first setting");
        eq(true, model.dirty(), "the unsaved edit survived");
        shows(model, OVERLAY, 1, "the unsaved edit survived");
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "the screen stays open");

        // The banner goes after any successful save; the focus stays on the row it was on.
        MenuModel other = new MenuModel();
        other.loaded(saved, true, NOTHING, drift, v458());
        goTo(other, row(other, OVERLAY));
        startSave(other);
        other.saveFinished(true, false, null);
        eq(MenuModel.KIND_CHOICE, other.rowKind(0), "the banner is gone after a save from the list");
        eq(spec(OVERLAY).label, other.rowLabel(other.focused()), "the focus stayed on its row when the banner went");

        // Focus on the banner, first setting row disabled: the focus goes to the first row all the same.
        MenuModel sparse = new MenuModel();
        sparse.loaded(Settings.defaults(), true, NOTHING, drift,
                Capabilities.of(new HashSet<>(Arrays.asList("diag-fps-overlay")), true, false));
        sparse.tapBannerButton(MenuModel.BANNER_APPLY);
        sparse.beginSave();
        sparse.saveFinished(true, false, null);
        eq(spec(CAP).label, sparse.rowLabel(sparse.focused()), "focus after the banner went, first row disabled");

        MenuModel one = new MenuModel();
        one.loaded(saved, true, NOTHING, Arrays.asList("settings/scaler.txt"), v458());
        eq(true, one.rowLabel(0).endsWith("(1 file)."), "one file is counted in the singular: " + one.rowLabel(0));
        eq(MenuModel.KIND_CHOICE, open(v458(), saved).rowKind(0), "no banner when no file differs");
    }

    private static void loadingAndFailed() {
        MenuModel model = new MenuModel();
        eq(MenuModel.TEXT_LOADING, model.placeholder(), "text while loading");
        eq(0, model.rowCount(), "rows while loading");
        for (int command : new int[] {UP, DOWN, LEFT, RIGHT, CONFIRM, MENU, ALT, NavInput.PAGE_NEXT}) {
            eq(true, model.nav(command, false), "command " + command + " while loading is swallowed");
        }
        model.tapRow(0);
        model.tapOverlayButton(0);
        model.tapBannerButton(MenuModel.BANNER_APPLY);
        eq(MenuModel.ACTION_NONE, model.pendingAction(), "nothing is pending while loading");
        eq(false, model.dirty(), "dirty while loading");
        eq(false, model.nav(BACK, false), "BACK while loading leaves");
        eq(MenuModel.ACTION_LEAVE, model.pendingAction(), "pending after BACK");

        MenuModel failed = new MenuModel();
        failed.loadFailed("IOException: unreadable");
        eq(true, failed.placeholder().contains("IOException: unreadable"), "the failure is shown: " + failed.placeholder());
        eq(true, failed.nav(MENU, false), "Start after a failed load is swallowed");
        eq(MenuModel.ACTION_NONE, failed.pendingAction(), "nothing can be saved after a failed load");
        eq(false, failed.nav(BACK, false), "BACK after a failed load leaves");

        MenuModel ready = open(v458(), Settings.defaults());
        eq(null, ready.placeholder(), "no placeholder once the rows exist");
        eq(false, ready.nav(NavInput.NONE, false), "NONE is not a command");
        eq(false, ready.nav(99, false), "99 is not a command");
        eq(MenuModel.ACTION_NONE, ready.pendingAction(), "a number that is no command does nothing");
    }

    // ---- touch, controller hold, texts, geometry --------------------------------------------------------------------

    private static void taps() {
        MenuModel model = open(everything(), Settings.defaults());
        int traffic = row(model, TRAFFIC), counter = row(model, OVERLAY), scaler = row(model, SCALER);
        model.tapRow(traffic);
        eq(traffic, model.focused(), "a tapped row gets the focus");
        shows(model, TRAFFIC, 100, "a tap on a slider row changes nothing");
        model.tapRow(scaler);
        shows(model, SCALER, 0, "a tap on a choice row changes nothing");
        model.tapArrow(traffic, -1);
        shows(model, TRAFFIC, 95, "tap on the left arrow");
        model.tapArrow(traffic, 1);
        shows(model, TRAFFIC, 100, "tap on the right arrow");
        eq(traffic, model.focused(), "a tapped arrow focuses its row");
        model.tapRow(counter);
        shows(model, OVERLAY, 1, "a tap flips a toggle");
        eq(counter, model.focused(), "the tapped toggle has the focus");
        model.tapRow(-1);
        model.tapRow(model.rowCount());
        eq(counter, model.focused(), "taps outside the rows");

        model.nav(BACK, false);   // dirty: the save question opens
        model.tapRow(traffic);
        model.tapArrow(traffic, -1);
        eq(counter, model.focused(), "rows under a question cannot be tapped");
        shows(model, TRAFFIC, 100, "rows under a question cannot be changed");
        model.tapOverlayButton(7);
        eq(MenuModel.OVERLAY_SAVE_QUESTION, model.overlay(), "a tap on a button that does not exist");
        model.tapOverlayButton(1);
        eq(MenuModel.ACTION_LEAVE, model.pendingAction(), "tap on Discard");
        model.tapRow(traffic);
        eq(counter, model.focused(), "a tap after the screen decided to leave");

        MenuModel saving = open(everything(), Settings.defaults());
        saving.tapRow(rowLabelled(saving, "Save"));
        eq(MenuModel.ACTION_SAVE, saving.pendingAction(), "tap on the Save row");

        // Without a banner its buttons do not exist: a tap meant for them must not reach the row that is first now.
        saving.beginSave();
        saving.saveFinished(true, false, null);
        saving.tapBannerButton(MenuModel.BANNER_APPLY);
        eq(MenuModel.ACTION_NONE, saving.pendingAction(), "banner tap without a banner");
        eq(false, saving.dirty(), "banner tap without a banner changes nothing");

        MenuModel drifted = new MenuModel();
        drifted.loaded(Settings.defaults(), true, NOTHING, Arrays.asList("settings/scaler.txt"), everything());
        drifted.tapBannerButton(5);
        eq(MenuModel.OVERLAY_NONE, drifted.overlay(), "tap on a banner button that does not exist");
        eq(MenuModel.ACTION_NONE, drifted.pendingAction(), "tap on a banner button that does not exist");
        drifted.tapBannerButton(MenuModel.BANNER_SHOW);
        eq(MenuModel.OVERLAY_DRIFT_LIST, drifted.overlay(), "tap on the banner's Show");
        drifted.tapOverlayButton(0);
        drifted.tapBannerButton(MenuModel.BANNER_APPLY);
        eq(MenuModel.ACTION_SAVE, drifted.pendingAction(), "tap on the banner's Apply again");
    }

    /** The connection the player feels: key held -> NavInput repeats -> MenuModel steps. */
    private static void holdToRepeat() {
        final MenuModel model = open(everything(), Settings.defaults());
        NavInputTest.FakeTimer timer = new NavInputTest.FakeTimer();
        NavInput input = new NavInput(new NavInput.Sink() {
            @Override public void onNav(int command, boolean repeat) {
                model.nav(command, repeat);
            }
        }, timer);
        goTo(model, row(model, TRAFFIC));
        input.key(21, true, 0);    // D-pad left held
        timer.advance(1500);       // press + repeats at 400, 510 .. 1390 (ten) + 1445, 1500 = 13 steps of 5 %
        input.key(21, false, 0);
        timer.advance(1000);
        shows(model, TRAFFIC, 35, "traffic after holding LEFT for 1.5 s");
        input.key(21, true, 0);
        timer.advance(5000);
        input.key(21, false, 0);
        shows(model, TRAFFIC, 25, "traffic after holding LEFT for another 5 s");

        input.key(20, true, 0);    // D-pad down held
        timer.advance(5000);
        input.key(20, false, 0);
        eq("Save", model.rowLabel(model.focused()), "focus after holding DOWN for 5 s: the last row, not wrapped");
        input.key(20, true, 0);    // a fresh press
        input.key(20, false, 0);
        eq(spec(CAP).label, model.rowLabel(model.focused()), "a fresh DOWN on the last row wraps");
    }

    private static void noticeLine() {
        MenuModel imported = new MenuModel();
        imported.loaded(Settings.defaults(), false, NOTHING, NOTHING, v458());
        eq(MenuModel.TEXT_NO_FILE, imported.notice(), "notice when no settings file exists");
        eq(MenuModel.TONE_NORMAL, imported.noticeTone(), "tone of that notice");
        startSave(imported);
        imported.saveFinished(true, false, null);
        eq(null, imported.notice(), "notice after the first save");

        MenuModel damaged = new MenuModel();
        damaged.loaded(Settings.defaults(), true, Arrays.asList("frameCap was not a known value.", "x", "y"), NOTHING, v458());
        eq("frameCap was not a known value.\nx\ny", damaged.notice(), "notice for three problems: one per line");
        MenuModel four = new MenuModel();
        four.loaded(Settings.defaults(), true, Arrays.asList("a", "b", "c", "d"), NOTHING, v458());
        eq("a\nb\nc (and 1 more)", four.notice(), "notice for four problems: three lines and a count");
        eq(MenuModel.TONE_WARNING, damaged.noticeTone(), "tone for problems");
        startSave(damaged);
        damaged.saveFinished(true, false, null);
        eq(null, damaged.notice(), "the problems are gone once the file was written again");

        eq(null, open(v458(), Settings.defaults()).notice(), "no notice for a clean file");
        eq(null, new MenuModel().notice(), "no notice while loading");
    }

    private static void unsupportedValueReachable() {
        // Traffic 50 % was saved with a build that has the world sliders; v458 has none, so the row is otherwise disabled.
        MenuModel model = open(v458(), Settings.defaults().with(TRAFFIC, 50));
        int traffic = row(model, TRAFFIC);
        eq(true, model.rowEnabled(traffic), "the row with the unsupported value can take the focus");
        eq(false, model.rowEnabled(row(model, SettingsSchema.WORLD_PEDESTRIANS)), "its neighbour at the default is still disabled");
        model.tapRow(traffic);
        eq(traffic, model.focused(), "a tap focuses it");
        model.nav(RIGHT, false);
        shows(model, TRAFFIC, 100, "RIGHT goes to the nearest supported value: the default");
        eq(true, model.dirty(), "that is a change to save");
        eq(false, model.rowEnabled(traffic), "at the default the row is disabled again");
        model.nav(LEFT, false);
        shows(model, TRAFFIC, 100, "and ignores further changes");
        MenuModel walked = open(v458(), Settings.defaults().with(TRAFFIC, 50));
        boolean reached = false;
        for (int press = 0; press < walked.rowCount(); ++press) {
            walked.nav(DOWN, false);
            reached |= walked.focused() == row(walked, TRAFFIC);
        }
        eq(true, reached, "DOWN reaches the row");
    }

    private static void gameRunningNotice() {
        MenuModel model = new MenuModel();
        model.setGameRunning(true);
        model.loaded(Settings.defaults(), true, NOTHING, NOTHING, v458());
        eq(MenuModel.TEXT_GAME_RUNNING_NOTICE, model.notice(), "notice at opening while the game runs");
        eq(MenuModel.TONE_WARNING, model.noticeTone(), "tone of that notice");
        MenuModel withWarning = new MenuModel();
        withWarning.setGameRunning(true);
        withWarning.loaded(Settings.defaults(), true, Arrays.asList("w"), NOTHING, v458());
        eq(MenuModel.TEXT_GAME_RUNNING_NOTICE + "\nw", withWarning.notice(), "the game-running line comes before the warnings");
        MenuModel quiet = new MenuModel();
        quiet.loaded(Settings.defaults(), true, NOTHING, NOTHING, v458());
        eq(null, quiet.notice(), "no notice when the game does not run");
        // A refused save sets the line, a completed save removes it.
        quiet.nav(RIGHT, false);
        startSave(quiet);
        quiet.saveFinished(false, true, MenuModel.TEXT_GAME_RUNNING);
        eq(MenuModel.TEXT_GAME_RUNNING_NOTICE, quiet.notice(), "notice after a refused save");
        startSave(quiet);
        quiet.saveFinished(true, false, null);
        eq(null, quiet.notice(), "notice after a completed save");
    }

    private static void standingAndAppeared() {
        String standing = MenuModel.TEXT_STANDING + "diag-v450-fsr1-no-rcas (FSR sharpness has no effect).";
        MenuModel model = new MenuModel();
        model.loaded(Settings.defaults(), false, Arrays.asList("Saving will also rewrite x.", standing), NOTHING, v458());
        eq("Saving will also rewrite x.\n" + standing, model.notice(), "both lines before the save");
        startSave(model);
        model.saveFinished(true, false, null);
        eq(standing, model.notice(), "after the save only the developer-switch line is left");
        eq(MenuModel.TEXT_SAVED, model.status(), "status of a plain save");

        MenuModel appeared = askedToSave();
        appeared.nav(CONFIRM, false);   // Save in the question: save, then leave
        appeared.beginSave();
        appeared.saveFinished(true, false, "Saved, but the game was started meanwhile.");
        eq("Saved, but the game was started meanwhile.", appeared.status(), "the note takes the place of \"Saved.\"");
        eq(MenuModel.TONE_WARNING, appeared.statusTone(), "tone of the note");
        eq(false, appeared.dirty(), "the settings were saved");
        eq(MenuModel.ACTION_NONE, appeared.pendingAction(), "the screen stays open so that the note can be read");
        eq(MenuModel.TEXT_GAME_RUNNING_NOTICE, appeared.notice(), "and the notice says the game is running");
    }

    private static void scrollOffset() {
        // Arguments: current scroll, viewport height, content height, focused row top/bottom, span top/bottom.
        eq(100, MenuModel.scrollOffset(100, 800, 2000, 300, 404, 196, 508), "span already visible: do not move");
        eq(208, MenuModel.scrollOffset(0, 800, 2000, 800, 904, 696, 1008), "span below the window: just far enough down");
        eq(196, MenuModel.scrollOffset(500, 800, 2000, 300, 404, 196, 508), "span above the window: just far enough up");
        eq(0, MenuModel.scrollOffset(300, 800, 2000, 40, 144, 0, 248), "first row: back to the very top");
        eq(1200, MenuModel.scrollOffset(0, 800, 2000, 1896, 2000, 1792, 2000), "last row: the very end");
        eq(154, MenuModel.scrollOffset(0, 250, 2000, 300, 404, 196, 508), "span taller than the window: the focused row alone");
        eq(300, MenuModel.scrollOffset(600, 250, 2000, 300, 404, 196, 508), "the same, scrolling up");
        eq(0, MenuModel.scrollOffset(50, 800, 600, 300, 404, 196, 508), "content shorter than the window");
    }

    private static void gameCodeLine() {
        long modified = 1791127560000L;   // 2026-10-04 15:26:00 UTC
        eq("Game code: 338 MB, 2026-10-04 17:26",
                MenuModel.gameCodeLine(338310896L, modified, TimeZone.getTimeZone("GMT+02:00")), "the v458 library, UTC+2");
        eq("Game code: 338 MB, 2026-10-04 15:26",
                MenuModel.gameCodeLine(338310896L, modified, TimeZone.getTimeZone("UTC")), "the same in UTC");
        eq("Game code: 2 MB, 2026-01-01 00:00",
                MenuModel.gameCodeLine(1500000L, 1767225600000L, TimeZone.getTimeZone("UTC")), "1.5 MB rounds up; January is month 01");
        eq("Game code not installed", MenuModel.gameCodeLine(0L, 0L, TimeZone.getTimeZone("UTC")), "no library");
    }
}
