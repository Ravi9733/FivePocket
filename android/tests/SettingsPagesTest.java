package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.begin;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.caps;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.end;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Host test of SettingsPages: the settings rows regrouped into categories, driven the way the app drives them
 * (ShellNav for the highlight, MenuModel for values and questions). It looks at what the frame would draw and where
 * the highlight is, never at how the classes keep it. Run with java -ea.
 */
public final class SettingsPagesTest {
    private static final List<String> NOTHING = new ArrayList<String>();
    private static final int UP = NavInput.UP, DOWN = NavInput.DOWN, LEFT = NavInput.LEFT, RIGHT = NavInput.RIGHT,
            A = NavInput.CONFIRM, B = NavInput.BACK, R1 = NavInput.PAGE_NEXT, START = NavInput.MENU, Y = NavInput.ALT;

    /** One settings tab: the frame's highlight, the model and the pages, as SettingsScreen holds them. (Also used by RenderStageBTest.) */
    static final class Tab {
        final ShellNav nav = new ShellNav();
        final SettingsPages pages = new SettingsPages();
        final MenuModel model = new MenuModel();

        Tab(Capabilities capabilities, Settings saved, boolean fromFile, List<String> drift) {
            nav.setColumnCount(ShellNav.SETTINGS, SettingsPages.COUNT);
            nav.command(R1, false, false, false);
            model.loaded(saved, fromFile, NOTHING, drift, capabilities);
        }

        /** A command as LauncherScreen and SettingsScreen pass it on. */
        int press(int command) {
            if (ShellNav.route(nav.tab(), command, model.overlay() != MenuModel.OVERLAY_NONE, true, false) == ShellNav.TO_SETTINGS_MODEL) {
                pages.forward(model, nav, command, false);
                return ShellNav.NOTHING;
            }
            boolean enterable = SettingsPages.rows(model, nav.column(ShellNav.SETTINGS)).length > 0;
            int answer = nav.command(command, false, enterable, model.dirty());
            if (answer == ShellNav.ENTERED) pages.enter(model, nav.column(ShellNav.SETTINGS));
            else if (answer == ShellNav.PANE_COMMAND && (command == UP || command == DOWN)) pages.move(model, command == UP ? -1 : 1, false);
            else if (answer == ShellNav.PANE_COMMAND) pages.forward(model, nav, command, false);
            return answer;
        }

        /** A key held down: the fresh press, then this many repeats, as NavInput sends them. */
        void hold(int command, int repeats) {
            press(command);
            for (int i = 0; i < repeats; ++i) {
                boolean enterable = SettingsPages.rows(model, nav.column(ShellNav.SETTINGS)).length > 0;
                int answer = nav.command(command, true, enterable, model.dirty());
                if (answer == ShellNav.PANE_COMMAND && (command == UP || command == DOWN)) pages.move(model, command == UP ? -1 : 1, true);
                else if (answer == ShellNav.PANE_COMMAND) pages.forward(model, nav, command, true);
            }
        }

        /** Goes to a row with B, UP / DOWN and A presses, the way a player would; fails when that does not get there. */
        void into(int category, int slot) {
            for (int presses = 0; presses < 3 && nav.level() != ShellNav.COLUMN; ++presses) press(B);
            for (int presses = 0; presses < SettingsPages.COUNT && nav.column(ShellNav.SETTINGS) > category; ++presses) press(UP);
            for (int presses = 0; presses < SettingsPages.COUNT && nav.column(ShellNav.SETTINGS) < category; ++presses) press(DOWN);
            press(A);
            for (int presses = 0; presses < 8 && SettingsPages.slotOfFocus(model) != slot; ++presses) press(DOWN);
            same(ShellNav.PANE, nav.level(), "the highlight is in the rows");
            same(category, nav.column(ShellNav.SETTINGS), "of the category asked for");
            same(slot, SettingsPages.slotOfFocus(model), "on the row asked for");
        }

        MenuScenes.Page page() {
            MenuScenes.Page page = new MenuScenes.Page();
            pages.fill(page, model, nav, 0);
            return page;
        }

        String focusedLabel() {
            return model.rowLabel(model.focused());
        }
    }

    private static Tab v462() {
        return new Tab(caps(SettingsCoreTestKit.V462_NAMES), Settings.defaults(), true, NOTHING);
    }

    private static List<String> labels(MenuScenes.Page page) {
        List<String> labels = new ArrayList<String>();
        for (MenuScenes.Row row : page.rows) labels.add(row.label);
        return labels;
    }

    private static String text(List<MenuScenes.Line> lines) {
        StringBuilder all = new StringBuilder();
        for (MenuScenes.Line line : lines) all.append(line.text).append('\n');
        return all.toString();
    }

    private static boolean has(List<MenuScenes.Prompt> prompts, String glyph, String label) {
        for (MenuScenes.Prompt prompt : prompts) if (prompt.glyph.equals(glyph) && prompt.label.equals(label)) return true;
        return false;
    }

    public static void main(String[] args) {
        begin("SettingsPagesTest");

        test("grouping: every row of the model is in exactly one category, for a list with and without the files-differ row", () -> {
            for (List<String> drift : Arrays.asList(NOTHING, Arrays.asList("private-trace/diag-pace-40"))) {
                Tab tab = new Tab(caps(SettingsCoreTestKit.ALL_NAMES), Settings.defaults(), true, drift);
                int[] seen = new int[tab.model.rowCount()];
                for (int category = 0; category < SettingsPages.COUNT; ++category) {
                    int[] rows = SettingsPages.rows(tab.model, category);
                    check(rows.length > 0, SettingsPages.TITLES[category] + " has rows");
                    check(rows.length == MenuLayout.visibleRows(rows.length, MenuLayout.infoSlots(6), MenuLayout.HELP_LINES), SettingsPages.TITLES[category] + " fits the pane without scrolling, under a six-line notice and over a full help strip");
                    for (int row : rows) seen[row]++;
                }
                for (int row = 0; row < seen.length; ++row) same(1, seen[row], "row " + tab.model.rowLabel(row) + " is shown once");
            }
            same(SettingsPages.COUNT, SettingsPages.TITLES.length, "one title per category");
        });

        test("categories: the rows a player finds under each name, top to bottom", () -> {
            Tab tab = v462();
            same(Arrays.asList("Scaler", "FSR Sharpness", "Render Resolution"), labels(tab.page()), "Display, labels in title case");
            tab.press(DOWN);
            same(Arrays.asList("Frame Rate Cap", "Frame Generation (experimental)", "Frame-Rate Counter"), labels(tab.page()), "Frame rate");
            tab.press(DOWN);
            same(Arrays.asList("Traffic", "Pedestrians", "Parked Cars", "Scenario Pedestrians", "Draw Distance"), labels(tab.page()), "World");
            tab.press(DOWN);
            same(Arrays.asList("Low-Delay Stick Input"), labels(tab.page()), "Controls");
            tab.press(DOWN);
            same(Arrays.asList("Restore Original", "Save"), labels(tab.page()), "Save and restore");
            same(-1, tab.page().rowFocused, "on a category's name no row is highlighted");
            check(tab.page().columnLit, "the category's name is the light bar");
            same(SettingsPages.SAVE, tab.page().columnSelected, "the fifth category");
            same(Arrays.asList("Display", "Frame Rate", "World", "Controls", "Save And Restore"), Arrays.asList(tab.page().column), "the categories, in title case");
        });

        test("rows: UP / DOWN stay inside the category, wrap on a fresh press, stop when held", () -> {
            Tab tab = v462();
            tab.press(DOWN);
            tab.press(DOWN);
            same(ShellNav.ENTERED, tab.press(A), "A enters World");
            same("Traffic", tab.focusedLabel(), "on its first row");
            same(0, tab.page().rowFocused, "which is highlighted");
            check(tab.page().columnLit, "the category's name stays light: it is on the way to the highlight");
            for (String next : new String[] {"Pedestrians", "Parked cars", "Scenario pedestrians", "Draw distance"}) {
                tab.press(DOWN);
                same(next, tab.focusedLabel(), "DOWN");
            }
            check(!tab.pages.move(tab.model, 1, true), "held DOWN stops on the last row");
            same("Draw distance", tab.focusedLabel(), "still the last row");
            tab.press(DOWN);
            same("Traffic", tab.focusedLabel(), "a fresh DOWN wraps to the first row of World, not into Controls");
            check(!tab.pages.move(tab.model, -1, true), "held UP stops on the first row");
            tab.press(UP);
            same("Draw distance", tab.focusedLabel(), "a fresh UP wraps to the last row of World");
            same(SettingsPages.WORLD, tab.page().columnSelected, "still World");
            Tab one = v462();
            one.into(SettingsPages.CONTROLS, 0);
            check(!one.pages.move(one.model, 1, false), "a category of one row has nowhere to go");
            same("Low-delay stick input", one.focusedLabel(), "unchanged");
        });

        test("memory: each category brings the highlight back to the row it was on", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.WORLD, 3);
            tab.into(SettingsPages.DISPLAY, 1);
            tab.press(B);
            tab.press(DOWN);
            tab.press(DOWN);
            tab.press(A);
            same("Scenario pedestrians", tab.focusedLabel(), "World: the fourth row again");
            tab.press(B);
            tab.press(UP);
            tab.press(UP);
            tab.press(RIGHT);
            same("FSR sharpness", tab.focusedLabel(), "Display: the second row again (RIGHT enters like A)");
        });

        test("values: LEFT / RIGHT and A on a row change what the row shows, through the model", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.FRAME_RATE, 0);
            tab.press(RIGHT);
            same("40 fps", tab.page().rows.get(0).value, "RIGHT on the frame cap");
            check(tab.page().rows.get(0).marked, "the changed row carries the dot");
            same(MenuScenes.MARK_CHANGED, tab.page().columnMarks[SettingsPages.FRAME_RATE], "and so does its category");
            same(MenuScenes.MARK_NONE, tab.page().columnMarks[SettingsPages.WORLD], "another category does not");
            check(tab.model.dirty(), "the settings count as changed");
            tab.into(SettingsPages.FRAME_RATE, 2);
            tab.press(A);
            same("On", tab.page().rows.get(2).value, "A turns the counter on");
            tab.into(SettingsPages.WORLD, 0);
            tab.press(LEFT);
            MenuScenes.Row traffic = tab.page().rows.get(0);
            same("95 %", traffic.value, "LEFT on Traffic");
            check(Math.abs(traffic.fraction - 0.95f) < 0.001f, "its bar is at 95 %");
            check(traffic.arrows && traffic.halves && traffic.canLeft && traffic.canRight, "it can go both ways, by key and by a tap on either half");
            check(!tab.page().rows.get(1).marked, "a row that was not changed carries no mark");

            // A saved value the installed game build does not support (40 fps on v458): its row says so in the warning colour.
            Tab old = new Tab(caps(SettingsCoreTestKit.V458_NAMES), Settings.defaults().with(SettingsSchema.FRAME_CAP, SettingsSchema.CAP_40), true, NOTHING);
            old.press(DOWN);
            MenuScenes.Row cap = old.page().rows.get(0);
            same("40 fps", cap.value, "the saved value is shown");
            same(MenuScenes.TONE_WARNING, cap.valueTone, "in the warning colour");
            same(MenuScenes.TONE_NORMAL, old.page().rows.get(2).valueTone, "a value the build supports is plain");
        });

        test("a disabled row can be highlighted, shows its reason in the help strip and changes nothing", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.FRAME_RATE, 1);
            MenuScenes.Page page = tab.page();
            same("Frame Generation (experimental)", page.rows.get(1).label, "the row");
            same(1, page.rowFocused, "is highlighted");
            check(!page.rows.get(1).enabled, "and drawn disabled");
            check(text(page.help).contains(SettingsPages.notAvailable(tab.model.rowSecondLine(tab.model.focused()))), "the reason, in the help strip: " + text(page.help));
            check(text(page.help).startsWith(SettingsPages.explain(SettingsSchema.FRAME_GENERATION)), "after what the row is");
            String before = page.rows.get(1).value;
            for (int command : new int[] {LEFT, RIGHT, A}) tab.press(command);
            same(before, tab.page().rows.get(1).value, "LEFT, RIGHT and A change nothing");
            check(!tab.model.dirty(), "nothing counts as changed");
            check(!has(page.prompts, "A", "Select"), "no Select prompt on it");
            check(has(page.prompts, "Y", "Restore Original") && has(page.prompts, "START", "Save") && has(page.prompts, "B", "Back"), "Back, Restore Original and Save stay");
        });

        test("today's phone build: every game row is disabled with a reason, the stick option can be changed", () -> {
            Tab tab = new Tab(caps(SettingsCoreTestKit.V669_NAMES), Settings.defaults(), false, NOTHING);
            for (int category = SettingsPages.DISPLAY; category <= SettingsPages.WORLD; ++category) {
                int[] rows = SettingsPages.rows(tab.model, category);
                for (int slot = 0; slot < rows.length; ++slot) {
                    tab.into(category, slot);
                    MenuScenes.Page page = tab.page();
                    check(!page.rows.get(slot).enabled, page.rows.get(slot).label + " is disabled");
                    check(text(page.help).contains("Not available"), page.rows.get(slot).label + " says why");
                }
            }
            tab.into(SettingsPages.CONTROLS, 0);
            check(tab.page().rows.get(0).enabled, "Low-delay stick input is enabled");
            check(has(tab.page().prompts, "A", "Select"), "and can be selected");
            same(MenuModel.TEXT_NO_FILE, tab.page().info.get(0).text, "the info block above the rows says, whole, that nothing is saved yet");
            same(MenuScenes.TONE_NORMAL, tab.page().info.get(0).tone, "in plain white: it is no warning");
        });

        test("help strip: what the row is, a measured note, a category's blurb; never the same sentence twice", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.FRAME_RATE, 0);
            tab.press(RIGHT);
            check(text(tab.page().help).contains(SettingsSchema.hint(SettingsSchema.FRAME_CAP, SettingsSchema.CAP_40)), "the measured note for 40 fps");
            check(text(tab.page().help).startsWith(SettingsPages.explain(SettingsSchema.FRAME_CAP)), "after what the row is");
            tab.press(B);
            tab.press(DOWN);
            same(MenuModel.TEXT_WORLD_NOTE + "\n", text(tab.page().help), "World's own line, on its name");
            for (int slot = 0; slot < 5; ++slot) {
                tab.into(SettingsPages.WORLD, slot);
                for (int step = 0; step < 2; ++step) {   // at Original, then one step below it
                    List<String> sentences = new ArrayList<String>();
                    for (MenuScenes.Line line : tab.page().help) sentences.addAll(Arrays.asList(line.text.split("(?<=\\.) ")));
                    check(!sentences.isEmpty(), tab.focusedLabel() + " is explained");
                    for (int a = 0; a < sentences.size(); ++a) {
                        for (int b = a + 1; b < sentences.size(); ++b) {
                            check(!sentences.get(a).equals(sentences.get(b)), tab.focusedLabel() + ": no sentence twice: " + sentences);
                            check(!(sentences.get(a).contains("moving cars") && sentences.get(b).contains("Moving cars")), tab.focusedLabel() + ": not the same thing in other words: " + sentences);
                        }
                    }
                    tab.press(LEFT);
                }
            }
            for (String key : new String[] {SettingsSchema.FRAME_CAP, SettingsSchema.SCALER, SettingsSchema.FSR_SHARPNESS, SettingsSchema.RENDER_SCALE,
                    SettingsSchema.FRAME_GENERATION, SettingsSchema.FPS_OVERLAY, SettingsSchema.WORLD_TRAFFIC, SettingsSchema.WORLD_PEDESTRIANS,
                    SettingsSchema.WORLD_PARKED_CARS, SettingsSchema.WORLD_SCENARIO_PEDESTRIANS, SettingsSchema.WORLD_DRAW_DISTANCE, SettingsSchema.INPUT_UNBUFFERED}) {
                check(SettingsPages.explain(key).length() > SettingsSchema.spec(key).label.length() + 5, key + " has a sentence of its own");
            }
        });

        test("notices: the whole text stands in the info block, on a category's name and on a row alike", () -> {
            List<String> warnings = Arrays.asList("Saving will also rewrite game setting files that are not in the form this screen writes: private-trace/diag-wl-test1.",
                    SettingsStore.WARNING_STANDING + "private-trace/diag-pace-dpt.");
            Tab tab = new Tab(caps(SettingsCoreTestKit.V462_NAMES), Settings.defaults(), true, NOTHING);
            tab.model.setGameRunning(true);
            tab.model.loaded(Settings.defaults(), true, warnings, NOTHING, caps(SettingsCoreTestKit.V462_NAMES));
            String whole = MenuModel.TEXT_GAME_RUNNING_NOTICE + "\n" + warnings.get(0) + "\n" + warnings.get(1);
            same(whole, tab.page().info.get(0).text, "on a category's name: the game-running line and both warnings");
            same(MenuScenes.TONE_WARNING, tab.page().info.get(0).tone, "in the warning colour");
            tab.into(SettingsPages.WORLD, 2);
            same(whole, tab.page().info.get(0).text, "on a row: the same, nothing left out");
            same(1, tab.page().info.size(), "once");
            check(!text(tab.page().help).contains("developer switch"), "the help strip is the row's own; the notice is not repeated in it");
            same(0, v462().page().info.size(), "nothing to say: no info block");
        });

        test("Y from a category's name: the question, and after Restore the highlight is on Save", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.WORLD, 0);
            tab.press(LEFT);
            tab.press(B);
            tab.press(Y);
            MenuScenes.Page page = tab.page();
            check(page.alert != null, "the question is drawn");
            same(Arrays.asList("Restore", "Cancel"), Arrays.asList(page.alert.buttons), "its buttons");
            same(1, page.alert.focused, "Cancel is highlighted first");
            check(has(page.prompts, "B", "Cancel") && has(page.prompts, "A", "Select"), "the prompts are the question's");
            same(2, page.prompts.size(), "and only the question's (no Save, no Restore Original under it)");
            tab.press(LEFT);
            tab.press(A);
            page = tab.page();
            check(page.alert == null, "the question is gone");
            same(ShellNav.PANE, tab.nav.level(), "the highlight is in the rows");
            same(SettingsPages.SAVE, page.columnSelected, "of Save and restore");
            same("Save", page.rows.get(page.rowFocused).label, "on Save");
            check(!tab.model.dirty(), "the edit is undone (the saved settings were the original)");
        });

        test("B with unsaved changes: the question; Keep editing stays, Discard and Save leave", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.FRAME_RATE, 0);
            tab.press(RIGHT);
            same(ShellNav.MOVED, tab.press(B), "B from the rows goes to the category's name without a question");
            same(ShellNav.ASK, tab.press(B), "B on the name asks");
            tab.model.nav(B, false);
            MenuScenes.Page page = tab.page();
            same("Save changes?", page.alert.title, "the question");
            same(Arrays.asList("Save", "Discard", "Keep editing"), Arrays.asList(page.alert.buttons), "its buttons");
            same(2, page.alert.focused, "Keep editing is highlighted first (menu-input-fixes D7)");
            tab.press(RIGHT);
            tab.press(RIGHT);
            same(2, tab.page().alert.focused, "RIGHT twice: still Keep editing");
            int under = tab.model.focused();
            check(!tab.pages.move(tab.model, 1, false) && !tab.pages.enter(tab.model, SettingsPages.WORLD), "under the question the highlight cannot be moved");
            same(under, tab.model.focused(), "it is where it was");
            tab.press(A);
            check(tab.page().alert == null && tab.model.pendingAction() == MenuModel.ACTION_NONE, "Keep editing closes the question and nothing else");
            check(tab.model.dirty(), "the edit stands");
            same("40 fps", tab.page().rows.get(0).value, "40 fps still shown");
            check(tab.page().rows.get(0).marked, "with its mark");

            tab.model.nav(B, false);
            tab.press(LEFT);
            tab.press(A);
            same(MenuModel.ACTION_LEAVE, tab.model.pendingAction(), "Discard leaves");

            Tab saving = v462();
            saving.into(SettingsPages.FRAME_RATE, 0);
            saving.press(RIGHT);
            saving.press(B);
            saving.press(B);
            saving.model.nav(B, false);
            saving.press(LEFT);
            saving.press(LEFT);
            saving.press(A);
            same(MenuModel.ACTION_SAVE, saving.model.pendingAction(), "Save asks for a save");
            same(SettingsSchema.CAP_40, saving.model.beginSave().get(SettingsSchema.FRAME_CAP), "of the edited settings");
            saving.model.saveFinished(true, false, null);
            same(MenuModel.ACTION_LEAVE, saving.model.pendingAction(), "and leaves when it worked");
        });

        test("Start saves from the rows; the status line says so and the highlight stays on its row", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.DISPLAY, 0);
            tab.press(RIGHT);
            tab.press(START);
            same(MenuModel.ACTION_SAVE, tab.model.pendingAction(), "Start asks for a save");
            boolean onDrift = SettingsPages.onDriftRow(tab.model);
            tab.model.beginSave();
            same(MenuModel.TEXT_SAVING, tab.page().footerStatus.text, "while it runs");
            tab.model.saveFinished(true, false, null);
            tab.pages.afterSave(tab.model, tab.nav, onDrift);
            MenuScenes.Page page = tab.page();
            same(MenuModel.TEXT_SAVED, page.footerStatus.text, "afterwards");
            same(MenuScenes.TONE_GOOD, page.footerStatus.tone, "in the good colour");
            same("Scaler", page.rows.get(page.rowFocused).label, "the highlight is where it was");
            check(!page.rows.get(0).marked, "the mark is gone");
        });

        test("a save that failed, was refused or met the game: the status says so whole, in the error or warning colour, and the edit stands", () -> {
            String reason = "settings/scaler.txt could not be written (EACCES: the folder files/settings is not writable by the app)";
            Object[][] cases = {
                    {false, false, reason, MenuModel.TEXT_SAVE_FAILED + reason, MenuScenes.TONE_ERROR, true},
                    {false, true, null, MenuModel.TEXT_GAME_RUNNING, MenuScenes.TONE_WARNING, true},
                    {true, false, SettingsStore.NOTE_GAME_APPEARED, SettingsStore.NOTE_GAME_APPEARED, MenuScenes.TONE_WARNING, false}};
            for (Object[] c : cases) {
                Tab tab = v462();
                tab.into(SettingsPages.DISPLAY, 0);
                tab.press(RIGHT);
                tab.press(START);
                tab.model.beginSave();
                tab.model.saveFinished((Boolean) c[0], (Boolean) c[1], (String) c[2]);
                MenuScenes.Page page = tab.page();
                check(page.footerStatus != null, "a status is shown for: " + c[3]);
                same(c[3], page.footerStatus.text, "the whole text");
                same(c[4], page.footerStatus.tone, "its colour");
                same(c[5], page.rows.get(0).marked, (Boolean) c[5] ? "nothing was written: the edit still stands as unsaved" : "it was written: the row is no longer marked");
                check(page.alert == null && page.rowFocused == 0, "no question is open and the highlight is on its row");
            }
        });

        test("Frame Generation in a build that announces it: off by default, unavailable with its reason unless the frame cap is 30 fps", () -> {
            Tab tab = new Tab(SetTestKit.framegen(), Settings.defaults(), true, NOTHING);
            tab.into(SettingsPages.FRAME_RATE, 1);
            MenuScenes.Page page = tab.page();
            MenuScenes.Row row = page.rows.get(1);
            same("Frame Generation (experimental)", row.label, "the row");
            same("Off", row.value, "an opt-in: Off at the original settings");
            check(!row.enabled, "at 60 fps it is drawn as unavailable");
            check(!row.canLeft && !row.canRight, "with no arrow that would do anything");
            same(SettingsPages.explain(SettingsSchema.FRAME_GENERATION) + "\nNot available: Needs Frame Rate Cap at 30 fps\n", text(page.help),
                    "the help strip: what the row is, then the one-line reason");
            check(!has(page.prompts, "A", "Select"), "no Select prompt on it");
            for (int command : new int[] {LEFT, RIGHT, A}) tab.press(command);
            check(tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ARROW_RIGHT, 1) && tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 1), "taps on it are the row's");
            same("Off", tab.page().rows.get(1).value, "LEFT, RIGHT, A and taps change nothing");
            check(!tab.model.dirty(), "nothing counts as changed");

            // With the 30 fps cap the row is live: both orders can be chosen, each with its own description.
            tab.into(SettingsPages.FRAME_RATE, 0);
            tab.press(RIGHT);
            tab.press(RIGHT);
            same("30 fps (original console cap)", tab.page().rows.get(0).value, "(the cap is 30 fps now)");
            MenuScenes.Row live = tab.page().rows.get(1);
            check(live.enabled && live.canRight && !live.canLeft, "the row is live: RIGHT leads on from Off");
            tab.into(SettingsPages.FRAME_RATE, 1);
            check(has(tab.page().prompts, "A", "Select"), "Select is offered");
            check(!text(tab.page().help).contains("Not available"), "nothing is called unavailable: " + text(tab.page().help));
            tab.press(RIGHT);
            same("In-Between Frames", tab.page().rows.get(1).value, "RIGHT: the first order");
            same(SettingsPages.explain(SettingsSchema.FRAME_GENERATION) + "\nExperimental; may switch itself off. Made pictures go between drawn frames, two refreshes late.\n",
                    text(tab.page().help), "its description");
            tab.press(RIGHT);
            same("Predicted Frames", tab.page().rows.get(1).value, "RIGHT again: the second order");
            check(text(tab.page().help).contains("A predicted picture follows each frame;"), "its description: " + text(tab.page().help));
            tab.press(LEFT);

            // Any other cap takes it away again. The row keeps the chosen order, which returns with 30 fps.
            tab.into(SettingsPages.FRAME_RATE, 0);
            // From 30 fps: LEFT to 40 fps, RIGHT RIGHT over 30 to 24 fps, RIGHT to Automatic.
            int[][] steps = {{LEFT}, {RIGHT, RIGHT}, {RIGHT}};
            String[] caps = {"40 fps", "24 fps", "Automatic"};
            for (int step = 0; step < steps.length; ++step) {
                for (int command : steps[step]) tab.press(command);
                same(caps[step], tab.page().rows.get(0).value, "(the cap)");
                MenuScenes.Row grey = tab.page().rows.get(1);
                check(!grey.enabled && !grey.canLeft && !grey.canRight, "unavailable at " + caps[step]);
                same("In-Between Frames", grey.value, "the chosen order is kept at " + caps[step]);
            }
            tab.into(SettingsPages.FRAME_RATE, 1);
            check(text(tab.page().help).contains("Not available: Needs Frame Rate Cap at 30 fps"), "under Automatic the reason stands again");
            tab.press(RIGHT);
            same("In-Between Frames", tab.page().rows.get(1).value, "and RIGHT does nothing");
            tab.into(SettingsPages.FRAME_RATE, 0);
            tab.press(LEFT);
            tab.press(LEFT);
            same("30 fps (original console cap)", tab.page().rows.get(0).value, "(30 fps again)");
            check(tab.page().rows.get(1).enabled, "the row is live again");
            same("In-Between Frames", tab.page().rows.get(1).value, "with the order that was chosen");
        });

        test("Frame Generation where the cap is not what stands in the way: the build's own reason, and a saved order the build lacks can be put back", () -> {
            // The phone's v464: the dry run's names, no generator. Also at 30 fps the reason is the build's.
            Tab none = new Tab(SetTestKit.v464WithDryRun(), Settings.defaults().with(SettingsSchema.FRAME_CAP, SettingsSchema.CAP_30), true, NOTHING);
            none.into(SettingsPages.FRAME_RATE, 1);
            check(!none.page().rows.get(1).enabled, "v464: the row is unavailable");
            same(SettingsPages.explain(SettingsSchema.FRAME_GENERATION) + "\n" + SettingsPages.TEXT_NOT_IN_VERSION + "\n", text(none.page().help), "v464: the reason is the build's, as one sentence (setup-fixes)");
            // An order saved under another build, opened under v464 at 60 fps: the row says so in the warning colour and
            // LEFT puts it back to Off. (The cap's reason would be untrue here, and would lock the row.)
            Tab stale = new Tab(SetTestKit.v464WithDryRun(), Settings.defaults().with(SettingsSchema.FRAME_GENERATION, SettingsSchema.FG_AHEAD), true, NOTHING);
            stale.into(SettingsPages.FRAME_RATE, 1);
            MenuScenes.Row row = stale.page().rows.get(1);
            same("Predicted Frames", row.value, "the saved order is shown");
            same(MenuScenes.TONE_WARNING, row.valueTone, "in the warning colour");
            check(row.enabled, "and the row can be used");
            check(!text(stale.page().help).contains("Needs Frame Rate Cap"), "the help does not blame the cap: " + text(stale.page().help));
            stale.press(LEFT);
            same("Off", stale.page().rows.get(1).value, "LEFT puts it back to Off");
        });

        test("Frame-Rate Counter: the row says where the counter is, and under every frame cap but 60 fps that it may be missing", () -> {
            // Game build v464, as shipped: the counter was seen at the top right with the 60 fps cap and was missing with 30 fps.
            String what = SettingsPages.explain(SettingsSchema.FPS_OVERLAY);
            check(what.contains("top right corner"), "the row says where the counter is drawn: " + what);
            Tab tab = new Tab(SettingsCoreTestKit.v464(), Settings.defaults(), true, NOTHING);
            tab.into(SettingsPages.FRAME_RATE, 2);
            same("Frame-Rate Counter", tab.page().rows.get(2).label, "(the counter's row)");
            same(what + "\n", text(tab.page().help), "60 fps: what the row is and nothing else");
            // The cap as edited, not as saved: RIGHT on the cap row goes 40, 30, 24, Automatic.
            String[] caps = {"40 fps", "30 fps (original console cap)", "24 fps", "Automatic"};
            for (String cap : caps) {
                tab.into(SettingsPages.FRAME_RATE, 0);
                tab.press(RIGHT);
                same(cap, tab.page().rows.get(0).value, "(the cap)");
                check(!text(tab.page().help).contains(SettingsPages.TEXT_COUNTER_UNDER_CAP), cap + ": the cap's own row does not carry the counter's note");
                tab.into(SettingsPages.FRAME_RATE, 1);
                check(!text(tab.page().help).contains(SettingsPages.TEXT_COUNTER_UNDER_CAP), cap + ": nor does Frame Generation's row");
                tab.into(SettingsPages.FRAME_RATE, 2);
                same(what + "\n" + SettingsPages.TEXT_COUNTER_UNDER_CAP + "\n", text(tab.page().help), cap + ": the note stands under what the row is, counter off");
            }
            // Switching the counter on under a cap: the note stays, and the model's own line follows it.
            tab.press(A);
            same("On", tab.page().rows.get(2).value, "(the counter is switched on)");
            same(what + "\n" + SettingsPages.TEXT_COUNTER_UNDER_CAP + "\n" + MenuModel.TEXT_NEXT_START + "\n", text(tab.page().help), "counter on under Automatic: the note, then when the change applies");
            // Back at 60 fps the note is gone.
            tab.into(SettingsPages.FRAME_RATE, 0);
            for (int presses = 0; presses < 4; ++presses) tab.press(LEFT);
            same("60 fps (default)", tab.page().rows.get(0).value, "(60 fps again)");
            tab.into(SettingsPages.FRAME_RATE, 2);
            same(what + "\n" + MenuModel.TEXT_NEXT_START + "\n", text(tab.page().help), "60 fps again: no note");
            // A build that has the caps but no counter: the row is unavailable and gives the build's reason, not the note.
            Tab without = new Tab(caps("diag-pace-30", "diag-pace-40", "diag-pace-24", "diag-pace-auto"),
                    Settings.defaults().with(SettingsSchema.FRAME_CAP, SettingsSchema.CAP_30), true, NOTHING);
            without.into(SettingsPages.FRAME_RATE, 2);
            check(!without.page().rows.get(2).enabled, "(no counter in that build: the row is unavailable)");
            same(what + "\nNot available: " + Capabilities.REASON_NOT_IN_BUILD + "\n", text(without.page().help), "an unavailable counter row gives its reason only");
        });

        test("Frame Rate Cap: the help does not explain the absence of Off, a value the row never offers; a missing cap is still named", () -> {
            String what = SettingsPages.explain(SettingsSchema.FRAME_CAP);
            // v464 honours every cap the row offers. 60 fps and 24 fps have no measured note of their own.
            for (int cap : new int[] {SettingsSchema.CAP_60, SettingsSchema.CAP_24}) {
                Tab tab = new Tab(SettingsCoreTestKit.v464(), Settings.defaults().with(SettingsSchema.FRAME_CAP, cap), true, NOTHING);
                tab.into(SettingsPages.FRAME_RATE, 0);
                same(what + "\n", text(tab.page().help), "v464, cap value " + cap + ": what the row is and nothing else");
                tab.press(LEFT);
                tab.press(LEFT);
                tab.press(LEFT);
                tab.press(LEFT);
                same("60 fps (default)", tab.page().rows.get(0).value, "(LEFT stops at 60 fps: Off is not a step of the row)");
            }
            // A build without the 40 fps cap: that is a value a player may look for, so the model's list stands.
            Tab older = new Tab(caps("diag-pace-30", "diag-pace-24", "diag-pace-auto"), Settings.defaults(), true, NOTHING);
            older.into(SettingsPages.FRAME_RATE, 0);
            same(what + "\n" + Capabilities.REASON_NOT_IN_BUILD + ": Off, 40 fps\n", text(older.page().help), "a build without 40 fps names what is missing");
        });

        test("descriptions: every row has one sentence that fits one line of the help strip; no state of the Frame Rate page overfills the strip", () -> {
            float width = MenuLayout.paneWidth(true) - 2 * MenuLayout.ROW_PAD;
            List<String> descriptions = new ArrayList<String>();
            for (SettingSpec spec : SettingsSchema.all()) descriptions.add(SettingsPages.explain(spec.key));
            descriptions.addAll(Arrays.asList(SettingsPages.TEXT_SAVE_HELP, SettingsPages.TEXT_RESTORE_HELP, SettingsPages.TEXT_DRIFT_HELP));
            for (String description : descriptions) {
                same(1, Scene.wrap(description, MenuLayout.HELP_PX, Scene.REGULAR, width, MenuLayoutTest.FIXED).size(), "lines of: " + description);
                check(description.endsWith(".") && Character.isUpperCase(description.charAt(0)), "a sentence: " + description);
            }
            // Frame Rate in the announced build: every cap with every order, the highlight on each of the three rows.
            for (int cap = SettingsSchema.CAP_60; cap <= SettingsSchema.CAP_AUTO; ++cap) {
                for (int generation = SettingsSchema.FG_OFF; generation <= SettingsSchema.FG_AHEAD; ++generation) {
                    Settings saved = Settings.defaults().with(SettingsSchema.FRAME_CAP, cap).with(SettingsSchema.FRAME_GENERATION, generation);
                    for (int slot = 0; slot < 3; ++slot) {
                        Tab tab = new Tab(SetTestKit.framegen(), saved, true, NOTHING);
                        tab.into(SettingsPages.FRAME_RATE, slot);
                        int lines = 0;
                        for (MenuScenes.Line line : tab.page().help) lines += Scene.wrap(line.text, MenuLayout.HELP_PX, Scene.REGULAR, width, MenuLayoutTest.FIXED).size();
                        check(lines >= 1 && lines <= MenuLayout.HELP_LINES, "cap " + cap + ", generation " + generation + ", row " + slot + ": " + lines + " help lines: " + text(tab.page().help));
                    }
                }
            }
        });

        test("a failure while reading the files: the tab says what happened and what to do before it names the exception", () -> {
            String said = SettingsPages.failure(new java.io.FileNotFoundException("private-trace-live.so (Permission denied)"));
            same("something went wrong with the files. Open this tab again to try once more. (FileNotFoundException: private-trace-live.so (Permission denied))", said, "with a cause");
            same("something went wrong with the files. Open this tab again to try once more. (IllegalStateException)", SettingsPages.failure(new IllegalStateException()), "without a message");
            // As the tab shows it: MenuModel puts its own words in front and the line stands in place of the rows.
            Tab tab = v462();
            MenuModel failed = new MenuModel();
            failed.loadFailed(said);
            MenuScenes.Page page = new MenuScenes.Page();
            tab.pages.fill(page, failed, tab.nav, 0);
            same("Could not read the settings: " + said, page.info.get(0).text, "the line in place of the rows");
            same(MenuScenes.TONE_ERROR, page.info.get(0).tone, "in the error colour");
            same(0, page.rows.size(), "no rows");
        });

        test("the help strip of the two action rows: Restore Original says it writes nothing, Save says what it writes", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.SAVE, 0);
            same("Restore Original", tab.page().rows.get(tab.page().rowFocused).label, "(on Restore Original)");
            same(SettingsPages.TEXT_RESTORE_HELP + "\n", text(tab.page().help), "its help");
            tab.into(SettingsPages.SAVE, 1);
            same("Save", tab.page().rows.get(tab.page().rowFocused).label, "(on Save)");
            same(SettingsPages.TEXT_SAVE_HELP + "\n", text(tab.page().help), "its help");
            check(SettingsPages.TEXT_RESTORE_HELP.contains("Nothing is written") && SettingsPages.TEXT_SAVE_HELP.startsWith("Writes"), "(the two texts say what their names promise)");
        });

        test("the list of differing files: the frame's scroll position reaches the alert", () -> {
            List<String> files = new ArrayList<String>();
            for (int i = 0; i < 14; ++i) files.add("private-trace/diag-test-" + i);
            Tab tab = new Tab(caps(SettingsCoreTestKit.V462_NAMES), Settings.defaults(), true, files);
            tab.into(SettingsPages.SAVE, 0);
            tab.press(A);   // Show
            MenuScenes.Page page = new MenuScenes.Page();
            tab.pages.fill(page, tab.model, tab.nav, 3);
            check(page.alert != null && page.alert.detail.contains("diag-test-13"), "the list is open and holds every file");
            same(3, page.alert.scroll, "it is drawn from the line the frame scrolled to");
            page = new MenuScenes.Page();
            tab.pages.fill(page, tab.model, tab.nav, 0);
            same(0, page.alert.scroll, "and from its first line before any scrolling");
        });

        test("files differ: a warning mark and line, the row in Save and restore, and Apply again ends on the category's name", () -> {
            Tab tab = new Tab(caps(SettingsCoreTestKit.V462_NAMES), Settings.defaults(), true, Arrays.asList("private-trace/diag-pace-40", "settings/world-load.txt"));
            MenuScenes.Page page = tab.page();
            same(SettingsPages.DISPLAY, page.columnSelected, "the tab opens on its first category as always");
            same(MenuScenes.MARK_WARNING, page.columnMarks[SettingsPages.SAVE], "Save and restore carries the warning mark");
            check(page.info.get(0).text.contains("(2 files)") && page.info.get(0).text.endsWith(" See Save And Restore."), "the info block above the rows: " + page.info.get(0).text);
            same(MenuScenes.TONE_WARNING, page.info.get(0).tone, "in the warning colour");
            tab.into(SettingsPages.SAVE, 0);
            page = tab.page();
            same(Arrays.asList(SettingsPages.LABEL_DRIFT, "Restore Original", "Save"), labels(page), "the rows");
            same("Show", page.rows.get(0).value, "Show is chosen first: it writes nothing");
            check(page.rows.get(0).canLeft && !page.rows.get(0).canRight, "Apply again is to its left");
            check(page.rows.get(0).halves, "a tap on its halves chooses between the two, and never acts");
            check(text(page.help).contains(SettingsPages.TEXT_DRIFT_HELP), "what the two choices do");
            tab.press(A);
            page = tab.page();
            check(page.alert != null, "A on Show lists the files");
            check(page.alert.detail.contains("diag-pace-40") && page.alert.detail.contains("world-load.txt"), "both of them");
            check(has(page.prompts, "B", "Close"), "B closes the list");
            tab.press(B);
            tab.press(LEFT);
            same("Apply Again", tab.page().rows.get(0).value, "LEFT chooses Apply Again");
            tab.press(A);
            same(MenuModel.ACTION_SAVE, tab.model.pendingAction(), "A on it asks for a save");
            boolean onDrift = SettingsPages.onDriftRow(tab.model);
            check(onDrift, "started from the files-differ row");
            same(Settings.defaults(), tab.model.beginSave(), "of the saved settings");
            tab.model.saveFinished(true, false, null);
            tab.pages.afterSave(tab.model, tab.nav, onDrift);
            page = tab.page();
            same(Arrays.asList("Restore Original", "Save"), labels(page), "the row is gone");
            same(SettingsPages.SAVE, page.columnSelected, "still Save and restore");
            check(tab.nav.level() == ShellNav.COLUMN && page.columnLit && page.rowFocused == -1, "the highlight is on the category's name");
            same(MenuScenes.MARK_NONE, page.columnMarks[SettingsPages.SAVE], "the warning mark is gone");
            check(page.info.isEmpty(), "and so is the info block above the rows");
        });

        test("a save started elsewhere while the files differ keeps the highlight where it is", () -> {
            Tab tab = new Tab(caps(SettingsCoreTestKit.V462_NAMES), Settings.defaults(), true, Arrays.asList("private-trace/diag-pace-40"));
            tab.into(SettingsPages.SAVE, 1);
            same("Restore original", tab.focusedLabel(), "on Restore original, under the files-differ row");
            tab.press(START);
            boolean onDrift = SettingsPages.onDriftRow(tab.model);
            tab.model.beginSave();
            tab.model.saveFinished(true, false, null);
            tab.pages.afterSave(tab.model, tab.nav, onDrift);
            MenuScenes.Page page = tab.page();
            same("Restore Original", page.rows.get(page.rowFocused).label, "still on Restore original, now the first row");
            same(0, page.rowFocused, "the first row");
            tab.press(B);
            tab.press(A);
            same("Restore original", tab.focusedLabel(), "and the category remembers that place");

            // From a category's name, after the highlight had been on the files-differ row: the name stays highlighted.
            Tab named = new Tab(caps(SettingsCoreTestKit.V462_NAMES), Settings.defaults(), true, Arrays.asList("private-trace/diag-pace-40"));
            named.into(SettingsPages.SAVE, 0);
            named.press(B);
            named.press(UP);
            named.press(START);
            onDrift = SettingsPages.onDriftRow(named.model);
            named.model.beginSave();
            named.model.saveFinished(true, false, null);
            named.pages.afterSave(named.model, named.nav, onDrift);
            page = named.page();
            check(named.nav.level() == ShellNav.COLUMN && page.rowFocused == -1, "Start on a category's name leaves the highlight on the name");
            same(SettingsPages.CONTROLS, page.columnSelected, "the same category");
        });

        test("touch: a tap on a row that is not highlighted only moves the highlight there; a tap on the highlighted row acts", () -> {
            Tab tab = v462();
            tab.press(DOWN);   // Frame rate, the highlight on the category's name
            check(tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 2), "tap the frame-rate counter (a switch)");
            same(ShellNav.PANE, tab.nav.level(), "the highlight goes into the rows");
            same(2, tab.page().rowFocused, "onto the tapped row");
            same("Off", tab.page().rows.get(2).value, "which is not switched by that first tap");
            check(!tab.model.dirty(), "nothing changed");
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 2);
            same("On", tab.page().rows.get(2).value, "a tap on the highlighted switch switches it");

            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ARROW_RIGHT, 0);
            same(0, tab.page().rowFocused, "a tap on another row moves the highlight to it");
            same("60 fps (default)", tab.page().rows.get(0).value, "and does not step its value, wherever on the row it lands");
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ARROW_RIGHT, 0);
            same("40 fps", tab.page().rows.get(0).value, "a tap on the right half of the highlighted row steps up");
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ARROW_LEFT, 0);
            same("60 fps (default)", tab.page().rows.get(0).value, "on its left half, down");
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 0);
            same("60 fps (default)", tab.page().rows.get(0).value, "a choice has no place that is A");
            check(!tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 3), "no fourth row in this category");

            tab.press(B);
            tab.press(DOWN);
            tab.press(DOWN);
            tab.press(DOWN);
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 1);
            same(MenuModel.ACTION_NONE, tab.model.pendingAction(), "a first tap on Save saves nothing");
            same("Save", tab.focusedLabel(), "it puts the highlight on Save");
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 0);
            same(MenuModel.OVERLAY_NONE, tab.model.overlay(), "a tap that lands on Restore Original instead only moves the highlight");
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 1);
            same(MenuModel.ACTION_NONE, tab.model.pendingAction(), "back on Save: still nothing written");
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 1);
            same(MenuModel.ACTION_SAVE, tab.model.pendingAction(), "the tap on the highlighted Save asks for a save");
        });

        test("touch: a tap on a row while the highlight is on the tab strip puts the highlight on that row", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.FRAME_RATE, 2);   // the counter (a switch) was the highlighted row ...
            tab.press(B);
            tab.press(B);                            // ... and now the highlight is two levels up
            same(ShellNav.STRIP, tab.nav.level(), "the highlight is on the tab strip");
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 2);
            same(ShellNav.PANE, tab.nav.level(), "after the tap it is in the rows");
            same(2, tab.page().rowFocused, "on the tapped row");
            same("Off", tab.page().rows.get(2).value, "which is unchanged: it was not the highlighted row when it was tapped");
            tab.pages.tap(tab.model, tab.nav, MenuLayout.HIT_ROW, 2);
            same("On", tab.page().rows.get(2).value, "tapped again, it switches");
        });

        test("touch: a tap on an answer of a question that is not highlighted only highlights it; on the highlighted one it chooses", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.WORLD, 0);
            tab.press(LEFT);
            tab.press(Y);
            same(1, tab.page().alert.focused, "Restore original? - Cancel is highlighted");
            tab.pages.tapAnswer(tab.model, tab.nav, 0);
            check(tab.page().alert != null, "a tap on Restore does not restore yet");
            same(0, tab.page().alert.focused, "it highlights Restore");
            check(tab.model.dirty(), "the change stands");
            tab.pages.tapAnswer(tab.model, tab.nav, 0);
            check(tab.page().alert == null && !tab.model.dirty(), "the second tap restores");
            same("Save", tab.focusedLabel(), "and the highlight is on Save, as after A");
            same(SettingsPages.SAVE, tab.page().columnSelected, "in Save And Restore");

            tab.into(SettingsPages.WORLD, 0);
            tab.press(LEFT);
            tab.press(B);
            tab.press(B);
            tab.model.nav(B, false);
            same(2, tab.page().alert.focused, "Save changes? - Keep Editing is highlighted (menu-input-fixes D7)");
            tab.pages.tapAnswer(tab.model, tab.nav, 0);
            same(0, tab.page().alert.focused, "a tap on Save, two cells away, highlights it");
            same(MenuModel.ACTION_NONE, tab.model.pendingAction(), "nothing is saved or discarded by it");
            tab.pages.tapAnswer(tab.model, tab.nav, 5);
            same(0, tab.page().alert.focused, "no such answer: nothing happens");
            tab.pages.tapAnswer(tab.model, tab.nav, 2);
            same(2, tab.page().alert.focused, "a tap on Keep Editing highlights it");
            same(MenuModel.ACTION_NONE, tab.model.pendingAction(), "nothing is saved or discarded by that either");
            tab.pages.tapAnswer(tab.model, tab.nav, 2);
            check(tab.page().alert == null && tab.model.dirty(), "tapped again: the question closes, the change stands");
        });

        test("a held RIGHT that takes the highlight from a category's name into its rows changes no value", () -> {
            Tab tab = v462();
            tab.press(DOWN);   // Frame rate
            tab.hold(RIGHT, 8);
            same(ShellNav.PANE, tab.nav.level(), "the highlight is in the rows");
            same("60 fps (default)", tab.page().rows.get(0).value, "the frame cap is what it was");
            check(!tab.model.dirty(), "nothing counts as changed");
            tab.hold(RIGHT, 2);
            same("24 fps", tab.page().rows.get(0).value, "pressed again and held, RIGHT steps the value and repeats: three steps for the press and two repeats");
        });

        test("Restore original ends on Save from wherever the highlight was: a category's name, the tab strip", () -> {
            Tab named = v462();
            named.into(SettingsPages.SAVE, 1);   // the model's own focus is the Save row already
            named.press(B);
            named.press(UP);
            named.press(UP);
            same(SettingsPages.WORLD, named.nav.column(ShellNav.SETTINGS), "the highlight is on the name World");
            named.press(Y);
            named.press(LEFT);
            named.press(A);
            same(ShellNav.PANE, named.nav.level(), "after Restore the highlight is in the rows");
            same(SettingsPages.SAVE, named.page().columnSelected, "of Save And Restore");
            same("Save", named.page().rows.get(named.page().rowFocused).label, "on Save");

            Tab strip = v462();
            strip.press(B);
            same(ShellNav.STRIP, strip.nav.level(), "the highlight is on the tab strip");
            strip.press(Y);
            strip.press(LEFT);
            strip.press(A);
            same(ShellNav.PANE, strip.nav.level(), "after Restore it is in the rows");
            same("Save", strip.page().rows.get(strip.page().rowFocused).label, "on Save");

            Tab cancelled = v462();
            cancelled.press(DOWN);
            cancelled.press(Y);
            cancelled.press(A);   // Cancel is the highlighted answer
            same(ShellNav.COLUMN, cancelled.nav.level(), "Cancel leaves the highlight where it was");
            same(SettingsPages.FRAME_RATE, cancelled.page().columnSelected, "on the same name");
        });

        test("a freshly read model: the highlight returns to its row, also when the read ends while another tab is shown", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.WORLD, 3);
            MenuModel fresh = new MenuModel();
            fresh.loaded(Settings.defaults(), true, NOTHING, NOTHING, caps(SettingsCoreTestKit.V462_NAMES));
            tab.pages.reloaded(fresh, tab.nav);
            same("Scenario pedestrians", fresh.rowLabel(fresh.focused()), "the new model's focus is on the row the highlight was on");
            MenuScenes.Page page = new MenuScenes.Page();
            tab.pages.fill(page, fresh, tab.nav, 0);
            same("Scenario Pedestrians", page.rows.get(page.rowFocused).label, "which is the highlighted one");

            tab.nav.command(R1, false, false, false);
            MenuModel later = new MenuModel();
            later.loaded(Settings.defaults(), true, NOTHING, NOTHING, caps(SettingsCoreTestKit.V462_NAMES));
            tab.pages.reloaded(later, tab.nav);
            tab.nav.command(NavInput.PAGE_PREV, false, false, false);
            same(ShellNav.PANE, tab.nav.level(), "back on the Settings tab the highlight is in the rows");
            same("Scenario pedestrians", later.rowLabel(later.focused()), "and LEFT / RIGHT would change the row that is shown highlighted");

            MenuModel failed = new MenuModel();
            failed.loadFailed("boom");
            tab.pages.reloaded(failed, tab.nav);
            same(ShellNav.COLUMN, tab.nav.level(), "without rows the highlight is on the category's name");
            same(SettingsPages.WORLD, tab.nav.column(ShellNav.SETTINGS), "of the same category");

            Tab named = v462();
            named.press(DOWN);
            MenuModel again = new MenuModel();
            again.loaded(Settings.defaults(), true, NOTHING, NOTHING, caps(SettingsCoreTestKit.V462_NAMES));
            int before = again.focused();
            named.pages.reloaded(again, named.nav);
            same(ShellNav.COLUMN, named.nav.level(), "a highlight on a category's name stays there");
            same(before, again.focused(), "and the model is left alone");
        });

        test("loading and a failed read: the categories are shown, no rows, the message in the info block; A is not offered", () -> {
            Tab tab = v462();
            MenuModel loading = new MenuModel();
            MenuScenes.Page page = new MenuScenes.Page();
            tab.pages.fill(page, loading, tab.nav, 0);
            same(Arrays.asList(SettingsPages.TITLES), Arrays.asList(page.column), "the categories");
            same(0, page.rows.size(), "no rows");
            same(MenuModel.TEXT_LOADING, page.info.get(0).text, "the loading line");
            check(!has(page.prompts, "START", "Save"), "nothing to save yet");
            check(!has(page.prompts, "A", "Select"), "A would do nothing on a category without rows, so it is not offered");
            check(has(page.prompts, "B", "Back"), "B still goes back");
            check(!tab.pages.enter(loading, 0), "the highlight cannot go into rows that do not exist");
            loading.loadFailed("boom");
            page = new MenuScenes.Page();
            tab.pages.fill(page, loading, tab.nav, 0);
            same(MenuScenes.TONE_ERROR, page.info.get(0).tone, "a failed read is shown as an error");
            check(page.info.get(0).text.contains("boom"), "with its reason");
        });

        test("while the files are read again the highlight is never nowhere: the category's name is light", () -> {
            Tab tab = v462();
            tab.into(SettingsPages.WORLD, 1);
            MenuScenes.Page page = new MenuScenes.Page();
            tab.pages.fill(page, new MenuModel(), tab.nav, 0);   // the fresh model has no rows yet; the frame still says "in the rows"
            same(ShellNav.PANE, tab.nav.level(), "(the frame's highlight is in the rows)");
            same(-1, page.rowFocused, "no row can be light");
            check(page.columnLit, "so the category's name is");
            same(SettingsPages.WORLD, page.columnSelected, "World");
            tab.press(B);
            tab.press(B);
            page = new MenuScenes.Page();
            tab.pages.fill(page, new MenuModel(), tab.nav, 0);
            check(!page.columnLit, "on the tab strip no category is light");
        });

        test("prompts: what the bottom right corner offers on the strip, on a category and on each kind of row; A is the rightmost", () -> {
            Tab tab = v462();
            List<MenuScenes.Prompt> onName = tab.page().prompts;
            check(has(onName, "A", "Select") && has(onName, "B", "Back") && has(onName, "Y", "Restore Original") && has(onName, "START", "Save"), "on a category's name");
            same("A", onName.get(onName.size() - 1).glyph, "Select (A) is the rightmost");
            same("B", onName.get(onName.size() - 2).glyph, "Back (B) before it");
            tab.into(SettingsPages.DISPLAY, 0);
            check(has(tab.page().prompts, "A", "Select"), "a choice: Select (A goes to its next value)");
            tab.into(SettingsPages.WORLD, 0);
            check(!has(tab.page().prompts, "A", "Select"), "a slider: A does nothing, so no Select");
            tab.into(SettingsPages.SAVE, 0);
            check(has(tab.page().prompts, "A", "Select"), "an action row: Select");
            for (MenuScenes.Prompt prompt : tab.page().prompts) {
                if (prompt.glyph.equals("A")) same(A, prompt.command, "tapping the A prompt is A");
                if (prompt.glyph.equals("B")) same(B, prompt.command, "tapping the B prompt is B");
                if (prompt.glyph.equals("Y")) same(Y, prompt.command, "tapping the Y prompt is Y");
                if (prompt.glyph.equals("START")) same(START, prompt.command, "tapping the Start prompt is Start");
            }
            tab.press(B);
            tab.press(B);
            same(ShellNav.STRIP, tab.nav.level(), "on the strip");
            check(has(tab.page().prompts, "A", "Select") && !has(tab.page().prompts, "B", "Back"), "the strip: Select, no Back");
        });

        end();
    }
}
