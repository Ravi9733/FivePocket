package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.begin;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.caps;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.end;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.TimeZone;

/**
 * Host test of the drawn frame (MenuScenes) and of the pages that are not settings (ShellPages): what a player sees
 * and can tap. The scenes are built from the app's real models and looked at as lists of shapes: which bars are
 * light, which text stands in which colour and where, that nothing leaves its box or the panel and no text is cut,
 * and which touch rectangle a point hits. Run with java -ea.
 */
public final class MenuScenesTest {
    private static final Scene.Measure FIXED = MenuLayoutTest.FIXED;
    private static final TimeZone UTC = TimeZone.getTimeZone("UTC");
    private static final List<String> NOTHING = new ArrayList<String>();
    private static final int PANE = MenuLayout.paneLeft(true), PANE_RIGHT = MenuLayout.RIGHT;
    private static final String REWRITE = "Saving will also rewrite game setting files that are not in the form this screen writes: "
            + "private-trace/diag-v450-fsr1-stops, settings/world-load.txt.";
    private static final String STANDING = SettingsStore.WARNING_STANDING + "private-trace/diag-pace-dpt, private-trace/diag-v450-fsr1-no-rcas.";

    private static Scene.Item text(Scene scene, String text) {
        for (Scene.Item item : scene.items) if (item.type == Scene.TEXT && item.text.equals(text)) return item;
        throw new AssertionError("no text <" + text + "> in the scene");
    }

    private static Scene.Item sized(Scene scene, String text, int size) {
        for (Scene.Item item : scene.items) if (item.type == Scene.TEXT && item.text.equals(text) && item.size == size) return item;
        throw new AssertionError("no text <" + text + "> of size " + size + " in the scene");
    }

    private static boolean hasText(Scene scene, String part) {
        for (Scene.Item item : scene.items) if (item.type == Scene.TEXT && item.text.contains(part)) return true;
        return false;
    }

    private static boolean exact(Scene scene, String text) {
        for (Scene.Item item : scene.items) if (item.type == Scene.TEXT && item.text.equals(text)) return true;
        return false;
    }

    private static List<Scene.Item> rects(Scene scene, int color) {
        List<Scene.Item> found = new ArrayList<Scene.Item>();
        for (Scene.Item item : scene.items) if (item.type == Scene.RECT && item.color == color) found.add(item);
        return found;
    }

    private static List<Scene.Item> shapes(Scene scene, int type) {
        List<Scene.Item> found = new ArrayList<Scene.Item>();
        for (Scene.Item item : scene.items) if (item.type == type) found.add(item);
        return found;
    }

    /** The icon buttons (the X, the check): squares in the Quick Save style, left to right. */
    private static List<Scene.Item> icons(Scene scene) {
        List<Scene.Item> found = new ArrayList<Scene.Item>();
        for (Scene.Item item : scene.items) if (item.type == Scene.ROUND && item.color == MenuLayout.TOUCH_BUTTON_FILL) found.add(item);
        found.sort((a, b) -> Float.compare(a.x, b.x));
        return found;
    }

    /** How many white glyph strokes of this many points stand inside the square. */
    private static int glyphLines(Scene scene, Scene.Item square, int points) {
        int n = 0;
        for (Scene.Item item : scene.items) {
            if (item.type != Scene.LINE || item.color != MenuLayout.TOUCH_BUTTON_GLYPH || item.points.length != 2 * points) continue;
            boolean inside = true;
            for (int i = 0; i < item.points.length; i += 2) {
                inside &= item.points[i] > square.x && item.points[i] < square.x + square.w && item.points[i + 1] > square.y && item.points[i + 1] < square.y + square.h;
            }
            if (inside) n++;
        }
        return n;
    }

    /** The colour of the topmost filled rectangle that covers a text's centre: what the text stands on. */
    private static int under(Scene scene, Scene.Item text) {
        float x = text.x + text.w / 2f, y = text.y + text.h / 2f;
        int color = 0;
        for (Scene.Item item : scene.items) {
            if (item == text) break;
            if (item.type == Scene.RECT && x >= item.x && x < item.x + item.w && y >= item.y && y < item.y + item.h) color = item.color;
        }
        return color;
    }

    /** The texts whose boxes lie inside a rectangle, top to bottom. */
    private static List<Scene.Item> textsIn(Scene scene, Scene.Item box) {
        List<Scene.Item> found = new ArrayList<Scene.Item>();
        for (Scene.Item item : scene.items) {
            if (item.type == Scene.TEXT && item.x >= box.x && item.y >= box.y - 0.5f && item.y + item.h <= box.y + box.h + 0.5f && item.x < box.x + box.w) found.add(item);
        }
        return found;
    }

    private static List<String> words(List<Scene.Item> lines) {
        StringBuilder all = new StringBuilder();
        for (Scene.Item line : lines) all.append(line.text).append(' ');
        return Arrays.asList(all.toString().trim().split("\\s+"));
    }

    /** Every text fits the box it was given, no text is cut, and every shape and touch rectangle lies inside the panel. */
    private static void tidy(Scene scene, String what) {
        for (Scene.Item item : scene.items) {
            if (item.type == Scene.POLY || item.type == Scene.LINE) {
                for (int p = 0; p + 1 < item.points.length; p += 2) {
                    check(item.points[p] >= 0 && item.points[p] <= MenuLayout.W && item.points[p + 1] >= 0 && item.points[p + 1] <= MenuLayout.H, what + ": an arrow head or line inside the panel");
                }
                continue;
            }
            check(item.x >= 0 && item.y >= 0 && item.x + item.w <= MenuLayout.W + 0.5f && item.y + item.h <= MenuLayout.H + 0.5f,
                    what + ": inside the panel: " + (item.text == null ? "shape at " + item.x + "," + item.y : item.text));
            if (item.type == Scene.TEXT) {
                check(FIXED.width(item.text, item.size, item.face) <= item.w + 0.5f, what + ": fits its box: " + item.text);
                // (The models' own "Checking..." and "Saving..." end in an ellipsis without being cut.)
                boolean own = item.text.toLowerCase(java.util.Locale.ROOT).endsWith("ing" + Scene.ELLIPSIS);
                check(own || !item.text.endsWith(Scene.ELLIPSIS), what + ": not cut: " + item.text);
            }
        }
        for (Scene.Hit hit : scene.hits) {
            check(hit.x >= 0 && hit.y >= 0 && hit.x + hit.w <= MenuLayout.W + 0.5f && hit.y + hit.h <= MenuLayout.H + 0.5f, what + ": touch rectangle " + hit.id + " inside the panel");
        }
    }

    private static InstallState install(boolean data, boolean code) throws Exception {
        File files = LauncherTestSupport.newFilesDir();
        if (data) LauncherTestSupport.addRun(files, LauncherTestSupport.ID_A, 1_700_000_000_000L, LauncherTestSupport.addInput(files, "one.bin"));
        if (code) LauncherTestSupport.addCode(files, 4096);
        InstallState state = InstallState.detect(files);
        LauncherTestSupport.discard(files);
        return state;
    }

    private static MenuScenes.Page settingsPage(ShellNav nav, SettingsPages pages, MenuModel model) {
        MenuScenes.Page page = ShellPages.frame(nav, new LauncherMenu(true), "Pad", UTC);
        pages.fill(page, model, nav, 0);
        return page;
    }

    private static ShellNav settingsNav() {
        ShellNav nav = new ShellNav();
        nav.setColumnCount(ShellNav.SETTINGS, SettingsPages.COUNT);
        nav.command(NavInput.PAGE_NEXT, false, false, false);
        return nav;
    }

    private static MenuModel v462() {
        MenuModel model = new MenuModel();
        model.loaded(Settings.defaults(), true, NOTHING, NOTHING, caps(SettingsCoreTestKit.V462_NAMES));
        return model;
    }

    private static boolean has(List<MenuScenes.Prompt> prompts, String glyph, String label) {
        for (MenuScenes.Prompt prompt : prompts) if (prompt.glyph.equals(glyph) && prompt.label.equals(label)) return true;
        return false;
    }

    private static MenuScenes.Prompt last(List<MenuScenes.Prompt> prompts) {
        return prompts.get(prompts.size() - 1);
    }

    public static void main(String[] args) throws Exception {
        begin("MenuScenesTest");
        final InstallState ready = install(true, true), nothingInstalled = install(false, false);

        test("settings: every row of every category, at every value: nothing leaves its box or the panel, nothing is cut", () -> {
            for (String[] build : new String[][] {SettingsCoreTestKit.V669_NAMES, SettingsCoreTestKit.V462_NAMES, SettingsCoreTestKit.ALL_NAMES}) {
                for (List<String> drift : Arrays.asList(NOTHING, Arrays.asList("private-trace/diag-pace-40"))) {
                    MenuModel model = new MenuModel();
                    model.loaded(Settings.defaults(), drift.isEmpty(), NOTHING, drift, caps(build));
                    ShellNav nav = settingsNav();
                    SettingsPages pages = new SettingsPages();
                    for (int category = 0; category < SettingsPages.COUNT; ++category) {
                        tidy(MenuScenes.build(settingsPage(nav, pages, model), FIXED), "category name " + category);
                        nav.command(NavInput.CONFIRM, false, true, false);
                        pages.enter(model, category);
                        int rows = SettingsPages.rows(model, category).length;
                        for (int slot = 0; slot < rows; ++slot) {
                            for (int step = 0; step < 45; ++step) {   // more steps than any row has values
                                tidy(MenuScenes.build(settingsPage(nav, pages, model), FIXED), SettingsPages.TITLES[category] + " row " + slot);
                                if (model.rowKey(model.focused()) == null) break;
                                String before = model.rowValue(model.focused());
                                pages.forward(model, nav, NavInput.RIGHT, false);
                                if (before.equals(model.rowValue(model.focused()))) break;
                            }
                            pages.move(model, 1, false);
                        }
                        nav.command(NavInput.BACK, false, true, false);
                        nav.command(NavInput.DOWN, false, true, false);
                    }
                }
            }
        });

        test("what is selected on the way to the highlight is a light bar: the tab; from the column on its item; in the pane the row", () -> {
            MenuModel model = v462();
            ShellNav nav = settingsNav();
            SettingsPages pages = new SettingsPages();
            nav.command(NavInput.DOWN, false, true, false);

            Scene scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
            same(2, rects(scene, MenuLayout.SELECT).size(), "on a category's name: the tab and the name");
            Scene.Item name = text(scene, "Frame Rate"), tab = text(scene, "SETTINGS");
            same(MenuLayout.SELECT, under(scene, name), "under the category's name");
            same(MenuLayout.SELECT_TEXT, name.color, "whose text is black");
            same(MenuLayout.SELECT, under(scene, tab), "under the tab's name");
            same(MenuLayout.SELECT_TEXT, tab.color, "whose text is black");
            Scene.Item line = rects(scene, MenuLayout.ACCENT).get(0);
            check(line.x == rects(scene, MenuLayout.SELECT).get(0).x && line.y + line.h == MenuLayout.STRIP_TOP, "the coloured line lies directly on the selected tab");
            same(MenuLayout.TEXT, text(scene, "PLAY").color, "another tab: white on the dark bar");
            same(MenuLayout.PANEL, under(scene, text(scene, "PLAY")), "(dark)");
            same(MenuLayout.TEXT, text(scene, "Frame Rate Cap").color, "the rows are shown, none of them light");
            same(MenuLayout.PANEL, under(scene, text(scene, "Display")), "another category stands on a dark bar");

            nav.command(NavInput.CONFIRM, false, true, false);
            pages.enter(model, SettingsPages.FRAME_RATE);
            scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
            same(3, rects(scene, MenuLayout.SELECT).size(), "in the rows: the tab, the category and the row");
            Scene.Item cap = text(scene, "Frame Rate Cap");
            same(MenuLayout.SELECT, under(scene, cap), "under the highlighted row");
            same(MenuLayout.SELECT_TEXT, cap.color, "its label is black");
            same(MenuLayout.SELECT_TEXT, text(scene, "60 fps (default)").color, "its value is black");
            same(MenuLayout.SELECT, under(scene, text(scene, "Frame Rate")), "the category stays light");
            Scene.Item counter = text(scene, "Frame-Rate Counter");
            same(MenuLayout.PANEL, under(scene, counter), "another row stands on the dark block");
            same(MenuLayout.TEXT, counter.color, "in white");
            same(MenuLayout.TEXT_OFF, text(scene, "Frame Generation (experimental)").color, "a row that cannot be used is grey");
            same(MenuLayout.PANEL, under(scene, text(scene, "Frame Generation (experimental)")), "and still dark-backed");

            pages.move(model, 1, false);
            scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
            Scene.Item off = text(scene, "Frame Generation (experimental)");
            same(MenuLayout.SELECT, under(scene, off), "a highlighted row that cannot be used is a light bar too");
            same(MenuLayout.SELECT_TEXT_OFF, off.color, "with grey text");
            same(0, shapes(scene, Scene.POLY).size() - 2, "and no arrows round its value (the two of the tab strip remain)");

            ShellNav strip = settingsNav();
            strip.command(NavInput.UP, false, true, false);
            scene = MenuScenes.build(settingsPage(strip, pages, model), FIXED);
            same(1, rects(scene, MenuLayout.SELECT).size(), "on the tab strip: only the tab");
            same(MenuLayout.PANEL, under(scene, text(scene, "Display")), "no category is light");
        });

        test("a slider is a thin bar at the right end of its row, filled to its value; the arrows stand tight round the highlighted value", () -> {
            MenuModel model = new MenuModel();
            model.loaded(Settings.defaults().with(SettingsSchema.WORLD_TRAFFIC, 80), true, NOTHING, NOTHING, caps(SettingsCoreTestKit.V462_NAMES));
            ShellNav nav = settingsNav();
            nav.command(NavInput.DOWN, false, true, false);
            nav.command(NavInput.DOWN, false, true, false);
            Scene scene = MenuScenes.build(settingsPage(nav, new SettingsPages(), model), FIXED);
            Scene.Item traffic = text(scene, "Traffic");
            int fills = 0;
            for (Scene.Item item : rects(scene, MenuLayout.ACCENT)) {
                if (item.h != MenuLayout.BAR_H) continue;
                boolean trafficRow = item.y > traffic.y && item.y < traffic.y + traffic.h;
                same((float) MenuLayout.barFill(trafficRow ? 0.8f : 1f), item.w, trafficRow ? "Traffic at 80 %" : "a slider at Original is full");
                same((float) (PANE_RIGHT - MenuLayout.VALUE_PAD - MenuLayout.BAR_W), item.x, "the bar ends at the row's right padding");
                same(Scene.CONTENT, item.layer, "it fades in with the row's text");
                fills++;
            }
            same(4, fills, "the four sliders that can be used have an accent bar (Draw Distance is locked: grey)");
            same(5, rects(scene, MenuLayout.TRACK).size() + rects(scene, MenuLayout.TRACK_OFF).size(), "every slider has a track");
            check(!exact(scene, "80 %") && !exact(scene, "Original"), "a slider that is not highlighted says its value with its bar alone, as in the game: no number is written");
            for (Scene.Item item : scene.items) {
                if (item.type == Scene.TEXT && item.y == traffic.y && item.x > PANE) check(item == traffic, "nothing but the label is written on the row: " + item.text);
            }
            same(2, shapes(scene, Scene.POLY).size(), "no arrows while no row is highlighted (the two of the tab strip only)");

            nav.command(NavInput.CONFIRM, false, true, false);
            SettingsPages pages = new SettingsPages();
            pages.enter(model, SettingsPages.WORLD);
            scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
            List<Scene.Item> arrows = shapes(scene, Scene.POLY).subList(2, shapes(scene, Scene.POLY).size());
            same(2, arrows.size(), "two arrows on the highlighted row");
            Scene.Item value = text(scene, "80 %");
            same(MenuLayout.SELECT_TEXT, value.color, "on the highlighted slider the value is written, in black");
            check(!exact(scene, "Original"), "the sliders under it still show their bars alone");
            float leftTip = arrows.get(1).points[0], rightTip = arrows.get(0).points[0];
            same(value.x - MenuLayout.CHEVRON_GAP - MenuLayout.CHEVRON_W, leftTip, "the left arrow stands directly before the value");
            same(value.x + value.w + MenuLayout.CHEVRON_GAP + MenuLayout.CHEVRON_W, rightTip, "the right arrow directly after it");
            same(1, rects(scene, MenuLayout.TRACK_ON_SELECT).size(), "on the light bar the slider's track is dark");
            Scene.Item frame = rects(scene, MenuLayout.TRACK_ON_SELECT).get(0), fill = null;
            for (Scene.Item item : rects(scene, MenuLayout.ACCENT)) if (item.y > frame.y && item.y + item.h < frame.y + frame.h) fill = item;
            same((float) (PANE_RIGHT - MenuLayout.VALUE_PAD), frame.x + frame.w, "the dark track ends at the value's padding like every other bar and value");
            same((float) (PANE_RIGHT - MenuLayout.VALUE_PAD - MenuLayout.BAR_W), frame.x, "and starts where the bars under it start");
            check(fill != null && fill.x > frame.x && fill.x + fill.w < frame.x + frame.w, "the bar lies inside it, with an edge of the track on every side");
            check(rightTip < frame.x, "the value and its arrows stand before the bar");

            // A value the installed build does not support stands in the warning colour on a row that is not highlighted.
            MenuModel old = new MenuModel();
            old.loaded(Settings.defaults().with(SettingsSchema.FRAME_CAP, SettingsSchema.CAP_40), true, NOTHING, NOTHING, caps(SettingsCoreTestKit.V458_NAMES));
            ShellNav onName = settingsNav();
            onName.command(NavInput.DOWN, false, true, false);
            Scene.Item unsupported = text(MenuScenes.build(settingsPage(onName, new SettingsPages(), old), FIXED), "40 fps");
            same(MenuLayout.WARNING, unsupported.color, "40 fps on a build without that cap");
            same((float) (PANE_RIGHT - MenuLayout.VALUE_PAD), unsupported.x + unsupported.w, "a value ends at the row's right padding");
        });

        test("touch: a point on a tab, a column item, a row, the halves of the highlighted value row, a prompt, a strip arrow hits that control", () -> {
            MenuModel model = v462();
            ShellNav nav = settingsNav();
            SettingsPages pages = new SettingsPages();
            nav.command(NavInput.DOWN, false, true, false);
            nav.command(NavInput.CONFIRM, false, true, false);
            pages.enter(model, SettingsPages.FRAME_RATE);
            Scene scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
            for (int tab = 0; tab < ShellNav.TABS; ++tab) {
                Scene.Item name = text(scene, ShellNav.TAB_NAMES[tab]);
                same(MenuLayout.HIT_TAB + tab, scene.hitAt(name.x + name.w / 2, name.y + name.h / 2), "tab " + tab);
            }
            for (int category = 0; category < SettingsPages.COUNT; ++category) {
                Scene.Item name = text(scene, SettingsPages.TITLES[category]);
                same(MenuLayout.HIT_COLUMN + category, scene.hitAt(name.x + 5, name.y + name.h / 2), "category " + category);
                same(MenuLayout.HIT_COLUMN + category, scene.hitAt(name.x + 5, name.y - 1), "the gap above a category belongs to it or its neighbour, never to nothing: " + category);
            }
            float left = PANE + 40, right = PANE_RIGHT - 40;
            Scene.Item cap = text(scene, "Frame Rate Cap"), generation = text(scene, "Frame Generation (experimental)"), counter = text(scene, "Frame-Rate Counter");
            same(MenuLayout.HIT_ARROW_LEFT, scene.hitAt(left, cap.y + cap.h / 2), "the left half of the highlighted choice row steps down");
            same(MenuLayout.HIT_ARROW_RIGHT, scene.hitAt(right, cap.y + cap.h / 2), "its right half steps up");
            same(MenuLayout.HIT_ROW + 1, scene.hitAt(left, generation.y + generation.h / 2), "a row that is not highlighted is one touch area");
            same(MenuLayout.HIT_ROW + 1, scene.hitAt(right, generation.y + generation.h / 2), "also at its value");
            same(MenuLayout.HIT_ROW + 2, scene.hitAt(right, counter.y + counter.h / 2), "the third row");
            same(-1, scene.hitAt(PANE + 50, MenuLayout.slotTop(9)), "the empty pane under the help strip is nothing");
            pages.move(model, 1, false);
            pages.move(model, 1, false);
            scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
            counter = text(scene, "Frame-Rate Counter");
            same(MenuLayout.HIT_ROW + 2, scene.hitAt(left, counter.y + counter.h / 2), "a highlighted switch is one touch area: a tap on it is A");
            same(MenuLayout.HIT_ROW + 2, scene.hitAt(right, counter.y + counter.h / 2), "at both ends");
            for (String[] prompt : new String[][] {{"Select", "" + NavInput.CONFIRM}, {"Back", "" + NavInput.BACK}, {"Restore Original", "" + NavInput.ALT}, {"Save", "" + NavInput.MENU}}) {
                Scene.Item label = sized(scene, prompt[0], MenuLayout.PROMPT_PX);
                same(MenuLayout.HIT_PROMPT + Integer.parseInt(prompt[1]), scene.hitAt(label.x + label.w / 2, label.y + label.h / 2), "the prompt " + prompt[0]);
                same(MenuLayout.HIT_PROMPT + Integer.parseInt(prompt[1]), scene.hitAt(label.x + label.w / 2, MenuLayout.PROMPT_TOP - 10), "its touch area reaches above the strip");
            }
            float stripY = MenuLayout.STRIP_TOP + MenuLayout.STRIP_H / 2f;
            same(MenuLayout.HIT_PROMPT + NavInput.PAGE_PREV, scene.hitAt(MenuLayout.LEFT - 20, stripY), "the arrow before the strip is L1");
            same(MenuLayout.HIT_PROMPT + NavInput.PAGE_NEXT, scene.hitAt(MenuLayout.RIGHT + 20, stripY), "the arrow after the strip is R1");
        });

        test("alert: the whole screen - black, a heading, the question between two thin lines, the answers; nothing else can be tapped", () -> {
            MenuModel model = v462();
            ShellNav nav = settingsNav();
            SettingsPages pages = new SettingsPages();
            nav.command(NavInput.CONFIRM, false, true, false);
            pages.enter(model, SettingsPages.DISPLAY);
            pages.forward(model, nav, NavInput.RIGHT, false);
            model.nav(NavInput.BACK, false);
            Scene scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
            tidy(scene, "the unsaved-changes alert");
            same(Scene.FILL, scene.items.get(0).type, "first the whole window is covered");
            same(MenuLayout.ALERT_BLACK, scene.items.get(0).color, "with black");
            check(!hasText(scene, "SETTINGS") && !hasText(scene, "Display") && !hasText(scene, "Scaler"), "the menu is not drawn under it");
            Scene.Item heading = text(scene, MenuScenes.ALERT_HEADING), question = text(scene, "Save changes?");
            same(Scene.HEAVY, heading.face, "the heading in the heavy weight, as the game's");
            same(MenuLayout.ALERT_YELLOW, heading.color, "in the alert's yellow");
            check(Math.abs(heading.x + heading.w / 2 - MenuLayout.W / 2f) < 1 && Math.abs(question.x + question.w / 2 - MenuLayout.W / 2f) < 1, "heading and question are centred");
            List<Scene.Item> rules = new ArrayList<Scene.Item>();
            for (Scene.Item item : rects(scene, MenuLayout.TEXT)) if (item.h == MenuLayout.ALERT_RULE) rules.add(item);
            same(2, rules.size(), "two thin lines");
            check(heading.y + heading.h <= rules.get(0).y && rules.get(0).y < question.y && question.y + question.h <= rules.get(1).y, "the heading above the first, the question between them");
            String[] answers = {"Save", "Discard", "Keep Editing"};
            for (int b = 0; b < answers.length; ++b) {
                Scene.Item label = sized(scene, answers[b], MenuLayout.ROW_PX);
                check(label.y > rules.get(1).y, answers[b] + " stands under the second line");
                same(MenuLayout.HIT_ALERT_BUTTON + b, scene.hitAt(label.x + label.w / 2, label.y + label.h / 2), "tap on " + answers[b]);
                // Keep Editing is highlighted when the question opens (menu-input-fixes D7).
                same(b == 2 ? MenuLayout.SELECT : MenuLayout.ALERT_CELL, under(scene, label), answers[b] + (b == 2 ? " is the light one" : " is dark"));
                same(b == 2 ? MenuLayout.SELECT_TEXT : MenuLayout.TEXT, label.color, "its text");
            }
            same(-1, scene.hitAt(MenuLayout.LEFT + 50, MenuLayout.STRIP_TOP + 10), "where a tab was: nothing to tap");
            same(-1, scene.hitAt(MenuLayout.LEFT + 50, MenuLayout.slotTop(1) + 10), "where a category was: nothing to tap");
            same(-1, scene.hitAt(PANE + 50, MenuLayout.slotTop(0) + 10), "where a row was: nothing to tap");
            Scene.Item keep = sized(scene, "Keep Editing", MenuLayout.PROMPT_PX);
            same(MenuLayout.HIT_PROMPT + NavInput.BACK, scene.hitAt(keep.x + 2, keep.y + keep.h / 2), "the B prompt names what B does and can be tapped");
            same("alert", scene.contentKey, "an alert is a page of its own for the fade");
        });

        test("alert: a long list shows a window of lines and scrolls three lines at a time, never past its ends", () -> {
            StringBuilder names = new StringBuilder();
            for (int i = 1; i <= 24; ++i) names.append(i == 1 ? "" : "\n").append("file-").append(i);
            MenuScenes.Page page = new MenuScenes.Page();
            page.alert = new ConfirmOverlay("Files", names.toString(), new String[] {"Close"}, 0);
            int lines = MenuScenes.alertDetailLines(names.toString(), FIXED), window = MenuLayout.ALERT_LINES;
            same(24, lines, "one line per file");
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "the list");
            check(exact(scene, "file-1") && exact(scene, "file-" + window) && !exact(scene, "file-" + (window + 1)), "the first window");
            same(2, shapes(scene, Scene.POLY).size(), "an up and a down mark say that there is more");
            page.alert.scroll = ConfirmOverlay.scrolled(0, 1, lines);
            same(3, page.alert.scroll, "DOWN: three lines");
            scene = MenuScenes.build(page, FIXED);
            check(!exact(scene, "file-3") && exact(scene, "file-4") && exact(scene, "file-" + (window + 3)), "the window moved");
            same(0, ConfirmOverlay.scrolled(2, -1, lines), "UP stops at the top");
            same(lines - window, ConfirmOverlay.scrolled(lines - window - 1, 1, lines), "DOWN stops when the last line is shown");
            same(0, ConfirmOverlay.scrolled(0, 1, 3), "a text that fits does not scroll");
            page.alert = new ConfirmOverlay("Put every setting back to the original?", "Then choose Save.", new String[] {"Restore", "Cancel"}, 1);
            scene = MenuScenes.build(page, FIXED);
            tidy(scene, "question and detail");
            check(text(scene, "Put every setting back to the original?").y < text(scene, "Then choose Save.").y, "the detail stands under the question");
            same(MenuLayout.SELECT, under(scene, sized(scene, "Cancel", MenuLayout.ROW_PX)), "the answer the model highlights (Cancel, the second) is the light one");
            same(MenuLayout.ALERT_CELL, under(scene, sized(scene, "Restore", MenuLayout.ROW_PX)), "the other is dark");
            same(0, shapes(scene, Scene.POLY).size(), "no marks when everything is shown");
        });

        test("prompts: one dark strip in the bottom right corner of the screen, label then coloured mark, the last one rightmost", () -> {
            MenuScenes.Page page = new MenuScenes.Page();
            page.prompts.add(new MenuScenes.Prompt("Save", "START", NavInput.MENU));
            page.prompts.add(new MenuScenes.Prompt("Restore Original", "Y", NavInput.ALT));
            page.prompts.add(new MenuScenes.Prompt("Back", "B", NavInput.BACK));
            page.prompts.add(new MenuScenes.Prompt("Select", "A", NavInput.CONFIRM));
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "prompts");
            Scene.Item save = text(scene, "Save"), restore = text(scene, "Restore Original"), back = text(scene, "Back"), select = text(scene, "Select");
            check(save.x < restore.x && restore.x < back.x && back.x < select.x, "left to right in their order");
            List<Scene.Item> discs = new ArrayList<Scene.Item>();
            for (Scene.Item disc : shapes(scene, Scene.DISC)) if (disc.y >= MenuLayout.PROMPT_TOP) discs.add(disc);   // (not the portrait's head)
            same(Arrays.asList(MenuLayout.MARK_Y, MenuLayout.MARK_B, MenuLayout.MARK_A), Arrays.asList(discs.get(0).color, discs.get(1).color, discs.get(2).color), "Y yellow, B red, A green");
            same((float) (MenuLayout.PROMPT_RIGHT - MenuLayout.PROMPT_END), discs.get(2).x + discs.get(2).w, "the last mark ends near the frame's right edge");
            same(select.x + select.w + MenuLayout.PROMPT_GAP, discs.get(2).x, "a mark follows its label");
            int boxes = 0;
            for (Scene.Item round : shapes(scene, Scene.ROUND)) if (round.y >= MenuLayout.PROMPT_TOP) boxes++;   // (not the portrait's shoulders)
            same(1, boxes, "Start is a small rounded box");
            same(Scene.BOLD, text(scene, "A").face, "the letter in the mark is bold");
            Scene.Item strip = null;
            for (Scene.Item item : rects(scene, MenuLayout.PROMPT_STRIP)) if (item.y == MenuLayout.PROMPT_TOP) strip = item;
            check(strip != null, "the strip");
            same((float) MenuLayout.PROMPT_RIGHT, strip.x + strip.w, "ends at the frame's right edge (the console pause menu: inside the screen)");
            same((float) MenuLayout.PROMPT_BOTTOM, strip.y + strip.h, "and a little above the screen's bottom edge");
            same(save.x - MenuLayout.PROMPT_PAD, strip.x, "and starts a padding before the first label");
            page.prompts.clear();
            check(rects(MenuScenes.build(page, FIXED), MenuLayout.PROMPT_STRIP).isEmpty(), "without prompts there is no strip");
        });

        test("prompts on a touch screen without a gamepad: the X and check icons in place of the strip; with a gamepad the strip (owner 2026-10-25)", () -> {
            MenuScenes.Page page = new MenuScenes.Page();
            page.prompts.add(new MenuScenes.Prompt("Save", "START", NavInput.MENU));
            page.prompts.add(new MenuScenes.Prompt("Restore Original", "Y", NavInput.ALT));
            page.prompts.add(new MenuScenes.Prompt("Back", "B", NavInput.BACK));
            page.prompts.add(new MenuScenes.Prompt("Select", "A", NavInput.CONFIRM));
            page.touchIcons = true;
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "icons");
            for (String name : new String[] {"Save", "Restore Original", "Back", "Select", "A", "B", "Y", "START"}) check(!exact(scene, name), "no prompt chip: " + name);
            for (Scene.Item disc : shapes(scene, Scene.DISC)) check(disc.y + disc.h <= MenuLayout.BODY_BOTTOM, "no button mark under the body");
            check(rects(scene, MenuLayout.PROMPT_STRIP).isEmpty(), "no prompt strip");
            List<Scene.Item> icons = icons(scene);
            same(2, icons.size(), "two squares in the Quick Save style (B and A; START and Y have none)");
            Scene.Item x = icons.get(0), ok = icons.get(1);
            same((float) MenuLayout.PROMPT_RIGHT, ok.x + ok.w, "the check ends where the strip would");
            same((float) (MenuLayout.PROMPT_BOTTOM + MenuLayout.ICON_DROP), ok.y + ok.h, "a little above the screen's bottom");
            check(ok.y > MenuLayout.BODY_BOTTOM, "under the body");
            same((float) MenuLayout.TOUCH_BUTTON_GAP, ok.x - (x.x + x.w), "the X left of it, the gap between");
            same(ok.y, x.y, "level");
            same(2, glyphLines(scene, x, 2), "the X: two strokes of two points");
            same(1, glyphLines(scene, ok, 3), "the check: one stroke of three points");
            same(MenuLayout.HIT_PROMPT + NavInput.BACK, scene.hitAt(x.x + x.w / 2, x.y + x.h / 2), "the X is B");
            same(MenuLayout.HIT_PROMPT + NavInput.CONFIRM, scene.hitAt(ok.x + ok.w / 2, ok.y + ok.h / 2), "the check is A");
            // Only one of them offered: it alone, in its own place (the X does not slide into the corner).
            page.prompts.clear();
            page.prompts.add(new MenuScenes.Prompt("Back", "B", NavInput.BACK));
            Scene back = MenuScenes.build(page, FIXED);
            same(1, icons(back).size(), "only B: the X alone");
            same(Arrays.asList(x.x, x.y), Arrays.asList(icons(back).get(0).x, icons(back).get(0).y), "in its place");
            same(-1, back.hitAt(ok.x + ok.w / 2, ok.y + ok.h / 2), "nothing to tap where the check would be");
            page.prompts.clear();
            check(icons(MenuScenes.build(page, FIXED)).isEmpty(), "without prompts there is no icon");
            // A short status stands at the bottom left, its bottom level with the icons'.
            page.prompts.add(new MenuScenes.Prompt("Select", "A", NavInput.CONFIRM));
            page.footerStatus = new MenuScenes.Line(MenuModel.TEXT_SAVED, MenuScenes.TONE_GOOD);
            Scene saved = MenuScenes.build(page, FIXED);
            Scene.Item box = rects(saved, MenuLayout.PROMPT_STRIP).get(0);
            same((float) (MenuLayout.PROMPT_BOTTOM + MenuLayout.ICON_DROP), box.y + box.h, "the status level with the icons");
            check(box.x + box.w <= icons(saved).get(0).x, "and clear of them");
            // The same page with a gamepad: the strip, no icon.
            page.footerStatus = null;
            page.touchIcons = false;
            Scene pad = MenuScenes.build(page, FIXED);
            check(icons(pad).isEmpty() && exact(pad, "Select") && !rects(pad, MenuLayout.PROMPT_STRIP).isEmpty(), "with a gamepad: the strip names A, no icon");
        });

        test("the launcher: no controller - the icons; a controller - the strip (ShellPages.frame from the controller's name)", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            ShellNav nav = new ShellNav();
            for (String name : new String[] {null, " ", "Odin Controller"}) {
                MenuScenes.Page page = ShellPages.frame(nav, menu, name, UTC);
                ShellPages.play(page, nav, menu, name, UTC);
                Scene scene = MenuScenes.build(page, FIXED);
                boolean none = ShellPages.noController(name);
                same(none, page.touchIcons, "icons without a controller only: <" + name + ">");
                same(none, !icons(scene).isEmpty(), "drawn as icons: <" + name + ">");
                same(none, rects(scene, MenuLayout.PROMPT_STRIP).isEmpty(), "and the strip only with a controller: <" + name + ">");
            }
        });

        test("status: never cut - a long reason wraps upward in the bottom left corner, clear of the prompts", () -> {
            String failed = MenuModel.TEXT_SAVE_FAILED + "settings/render-scale.txt could not be written (EACCES: the folder files/settings is not writable by the app), "
                    + "and neither could settings/world-load.txt; nothing the game reads was changed";
            for (String[] status : new String[][] {{MenuModel.TEXT_SAVED, "" + MenuScenes.TONE_GOOD}, {SettingsStore.NOTE_GAME_APPEARED, "" + MenuScenes.TONE_WARNING}, {failed, "" + MenuScenes.TONE_ERROR}}) {
                MenuModel model = v462();
                ShellNav nav = settingsNav();
                SettingsPages pages = new SettingsPages();
                nav.command(NavInput.CONFIRM, false, true, false);
                pages.enter(model, SettingsPages.DISPLAY);
                MenuScenes.Page page = settingsPage(nav, pages, model);   // the prompts of a highlighted row: the most there are
                int tone = Integer.parseInt(status[1]);
                page.footerStatus = new MenuScenes.Line(status[0], tone);
                Scene scene = MenuScenes.build(page, FIXED);
                tidy(scene, "status");
                Scene.Item box = null, prompts = null;
                for (Scene.Item item : rects(scene, MenuLayout.PROMPT_STRIP)) {
                    if (item.y + item.h == MenuLayout.PROMPT_BOTTOM && item.x == MenuLayout.LEFT) box = item;
                    if (item.y == MenuLayout.PROMPT_TOP && item.x > MenuLayout.LEFT) prompts = item;
                }
                check(box != null && prompts != null, "a status block at the frame's bottom left, the prompt strip at its bottom right");
                List<Scene.Item> lines = textsIn(scene, box);
                same(Arrays.asList(status[0].split("\\s+")), words(lines), "every word of the status, in order");
                for (Scene.Item line : lines) same(MenuScenes.color(tone, MenuLayout.TEXT), line.color, "in its tone's colour");
                check(box.x + box.w <= prompts.x, "the block ends before the prompts");
                check(box.y >= 0 && box.h >= MenuLayout.PROMPT_H, "inside the screen");
                if (status[0] == failed) check(lines.size() > 1, "(this reason needs more than one line)");
                else if (status[0] == MenuModel.TEXT_SAVED) same((float) MenuLayout.PROMPT_TOP, box.y, "a short status is one line, level with the prompts");
            }
        });

        test("notices: never cut - every word stands in the info block above the rows, which keep clear of it", () -> {
            Object[][] cases = {
                    {SettingsCoreTestKit.V669_NAMES, false, NOTHING, false},                      // today's phone: "No saved settings yet..."
                    {SettingsCoreTestKit.V462_NAMES, true, Arrays.asList(REWRITE, STANDING), false},
                    {SettingsCoreTestKit.V462_NAMES, true, Arrays.asList(REWRITE, STANDING, STANDING.replace("dpt", "lowlat")), true}};
            for (Object[] c : cases) {
                MenuModel model = new MenuModel();
                model.setGameRunning((Boolean) c[3]);
                @SuppressWarnings("unchecked") List<String> warnings = (List<String>) c[2];
                model.loaded(Settings.defaults(), (Boolean) c[1], warnings, NOTHING, caps((String[]) c[0]));
                String whole = SettingsPages.notice(model).text;
                for (boolean inRows : new boolean[] {false, true}) {
                    ShellNav nav = settingsNav();
                    SettingsPages pages = new SettingsPages();
                    nav.command(NavInput.DOWN, false, true, false);
                    nav.command(NavInput.DOWN, false, true, false);   // World: the category with the most rows
                    if (inRows) {
                        nav.command(NavInput.CONFIRM, false, true, false);
                        pages.enter(model, SettingsPages.WORLD);
                        for (int i = 0; i < 4; ++i) pages.move(model, 1, false);   // its last row
                    }
                    Scene scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
                    tidy(scene, "notices");
                    Scene.Item block = null;
                    for (Scene.Item item : rects(scene, MenuLayout.PANEL)) if (item.x == PANE && item.y == MenuLayout.BODY_TOP) block = item;
                    check(block != null, "the info block stands at the top of the pane");
                    List<Scene.Item> lines = textsIn(scene, block);
                    same(Arrays.asList(whole.split("\\s+")), words(lines), "every word of the notice, in order (" + lines.size() + " lines)");
                    check(block.h > MenuLayout.ROW_H, "the block is taller than a row");
                    Scene.Item first = text(scene, "Traffic"), last = text(scene, "Draw Distance");
                    check(first.y >= block.y + block.h, "the first row stands under the block");
                    check(last.y + last.h <= MenuLayout.BODY_BOTTOM, "the last row is on screen");
                    if (inRows) same(MenuLayout.SELECT, under(scene, last), "and is the highlighted one");
                }
            }
        });

        test("help strip: directly under the rows; grows to four lines; more text than that ends in an ellipsis", () -> {
            MenuScenes.Page page = new MenuScenes.Page();
            page.column = new String[] {"One"};
            for (String label : new String[] {"Row A", "Row B"}) {
                MenuScenes.Row row = new MenuScenes.Row();
                row.label = label;
                page.rows.add(row);
            }
            page.help.add(new MenuScenes.Line("Short.", MenuScenes.TONE_NORMAL));
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "one help line");
            Scene.Item help = text(scene, "Short.");
            same(MenuLayout.HELP_PX, help.size, "small text");
            Scene.Item strip = null;
            for (Scene.Item item : rects(scene, MenuLayout.PANEL)) if (item.x == PANE && item.y == MenuLayout.slotTop(2)) strip = item;
            check(strip != null, "the strip starts in the slot under the last row");
            same((float) MenuLayout.ROW_H, strip.h, "one line: as high as a row");

            StringBuilder longText = new StringBuilder();
            for (int i = 0; i < 120; ++i) longText.append("word").append(i).append(' ');
            page.help.add(new MenuScenes.Line(longText.toString().trim(), MenuScenes.TONE_WARNING));
            scene = MenuScenes.build(page, FIXED);
            strip = null;
            for (Scene.Item item : rects(scene, MenuLayout.PANEL)) if (item.x == PANE && item.y == MenuLayout.slotTop(2)) strip = item;
            same((float) MenuLayout.helpHeight(MenuLayout.HELP_LINES), strip.h, "four lines high, not more");
            List<Scene.Item> lines = textsIn(scene, strip);
            same(MenuLayout.HELP_LINES, lines.size(), "four lines");
            same("Short.", lines.get(0).text, "the first part first");
            same(MenuLayout.WARNING, lines.get(1).color, "the second part in its tone");
            check(lines.get(3).text.endsWith(Scene.ELLIPSIS), "the last line says there is more: " + lines.get(3).text);
            check(FIXED.width(lines.get(3).text, MenuLayout.HELP_PX, Scene.REGULAR) <= lines.get(3).w, "and still fits");
        });

        test("a list longer than the pane shows the rows round the highlighted one and the row with the two marks", () -> {
            MenuScenes.Page page = new MenuScenes.Page();
            for (int i = 0; i < 30; ++i) {
                MenuScenes.Row row = new MenuScenes.Row();
                row.label = "Row " + (char) ('A' + i);
                page.rows.add(row);
            }
            page.rowFocused = 20;
            page.shoulders = false;   // a page without tabs: the only arrow heads are the row's two marks
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "long list");
            int visible = MenuLayout.visibleRows(30, 0, 0);
            check(visible < 30, "(not all rows fit)");
            check(exact(scene, "Row U") && exact(scene, "Row " + (char) ('A' + 20 - visible + 1)) && !exact(scene, "Row " + (char) ('A' + 20 - visible)), "the window ends on the highlighted row");
            Scene.Item focused = text(scene, "Row U");
            same(MenuLayout.HIT_ROW + 20, scene.hitAt(focused.x + 5, focused.y + focused.h / 2), "a tap names the row by its place in the list, not on screen");
            List<Scene.Item> marks = shapes(scene, Scene.POLY);
            same(2, marks.size(), "an up and a down mark");
            float middle = (MenuLayout.LEFT + MenuLayout.RIGHT) / 2f;
            for (Scene.Item mark : marks) {
                same(middle, mark.points[0], "centred in the pane");
                check(mark.points[1] > focused.y + focused.h && mark.points[1] <= MenuLayout.BODY_BOTTOM, "in the slot under the last row, inside the body");
            }
        });

        test("fade: only another tab or category is a new page; the bars never fade, only what is written in the pane", () -> {
            MenuModel model = v462();
            ShellNav nav = settingsNav();
            SettingsPages pages = new SettingsPages();
            String display = MenuScenes.build(settingsPage(nav, pages, model), FIXED).contentKey;
            nav.command(NavInput.DOWN, false, true, false);
            String frameRate = MenuScenes.build(settingsPage(nav, pages, model), FIXED).contentKey;
            check(!display.equals(frameRate), "another category");
            nav.command(NavInput.CONFIRM, false, true, false);
            pages.enter(model, SettingsPages.FRAME_RATE);
            same(frameRate, MenuScenes.build(settingsPage(nav, pages, model), FIXED).contentKey, "going into the rows");
            pages.forward(model, nav, NavInput.RIGHT, false);
            Scene scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
            same(frameRate, scene.contentKey, "a changed value");
            for (Scene.Item bar : rects(scene, MenuLayout.SELECT)) same(Scene.CHROME, bar.layer, "a light bar is drawn at full strength from the first frame");
            for (Scene.Item bar : rects(scene, MenuLayout.PANEL)) same(Scene.CHROME, bar.layer, "and so is every dark block");
            same(Scene.CHROME, text(scene, "Frame Rate").layer, "the column does not fade");
            same(Scene.CONTENT, text(scene, "Frame Rate Cap").layer, "a row's label fades in");
            same(Scene.CONTENT, text(scene, "40 fps").layer, "and its value");
            pages.move(model, 1, false);
            scene = MenuScenes.build(settingsPage(nav, pages, model), FIXED);
            same(frameRate, scene.contentKey, "another row");
            same(3, rects(scene, MenuLayout.SELECT).size(), "(tab, category and the second row are light)");
            for (Scene.Item bar : rects(scene, MenuLayout.SELECT)) same(Scene.CHROME, bar.layer, "the bar of a row further down stands still as well");
            nav.command(NavInput.PAGE_NEXT, false, true, false);
            MenuScenes.Page controller = ShellPages.frame(nav, new LauncherMenu(true), "Pad", UTC);
            ShellPages.controller(controller, nav, new LauncherMenu(true), "Pad");
            check(!frameRate.equals(MenuScenes.build(controller, FIXED).contentKey), "another tab");
        });

        test("legend: a model's footer line becomes prompts - B then A, A the rightmost, labels in title case, directions unnamed", () -> {
            List<MenuScenes.Prompt> prompts = MenuScenes.legendPrompts("A Select   B keep going   Left/Right Choose");
            same(2, prompts.size(), "A and B only");
            same("B", prompts.get(0).glyph, "B first");
            same("Keep Going", prompts.get(0).label, "with its action in title case");
            same(NavInput.BACK, prompts.get(0).command, "tappable as B");
            same("A", prompts.get(1).glyph, "A is the rightmost");
            same(NavInput.CONFIRM, prompts.get(1).command, "tappable as A");
            same(1, MenuScenes.legendPrompts("B Back").size(), "a single part");
            same(0, MenuScenes.legendPrompts(null).size(), "no legend");
            same(0, MenuScenes.legendPrompts("").size(), "an empty legend");
        });

        test("header: the title at the block's left; three small lines in capitals at its right; the game code line without its size", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            ShellNav nav = new ShellNav();
            String longName = "Xbox Wireless Controller (Bluetooth) 045e:0b13 revision 5.17 with a very long name that goes on and on and on and on and on";
            MenuScenes.Page page = ShellPages.frame(nav, menu, longName, UTC);
            ShellPages.play(page, nav, menu, longName, UTC);
            Scene scene = MenuScenes.build(page, FIXED);
            Scene.Item title = text(scene, ShellPages.TITLE);
            same(Scene.TITLE, title.face, "the title in the narrow face");
            same((float) MenuLayout.TITLE_LEFT, title.x, "on the header plate, a little in from the block's left edge (the console pause menu)");
            check(title.y + title.h <= MenuLayout.STRIP_TOP, "above the strip");
            Scene.Item first = text(scene, "PRIVATE BUILD");
            same(Scene.CAPS, first.face, "the small lines in capitals in the narrow face");
            same(Scene.RIGHT, first.align, "right-aligned");
            same((float) MenuLayout.STATUS_RIGHT, first.x + first.w, "ending left of the portrait box at the header's right end");
            Scene.Item name = null;
            for (Scene.Item item : scene.items) if (item.type == Scene.TEXT && item.face == Scene.CAPS && item.text.startsWith("XBOX WIRELESS")) name = item;
            check(name != null && name.text.endsWith(Scene.ELLIPSIS), "a controller name that is too long for the line is cut there");
            check(FIXED.width(name.text, name.size, name.face) <= MenuLayout.STATUS_W, "to the lines' width, so that it cannot run into the title");
            check(name.y + name.h <= MenuLayout.STRIP_TOP - MenuLayout.TAB_LINE, "the last line ends above the selected tab's coloured line");
            same("Game code 2026-10-04 17:26", ShellPages.codeLine("Game code: 338 MB, 2026-10-04 17:26"), "the date of the game code, without its size");
            same("Game code missing", ShellPages.codeLine("Game code: missing"), "or that it is missing");
            same(ShellPages.TEXT_NO_CONTROLLER, ShellPages.frame(nav, menu, " ", UTC).status.get(2).text, "no controller");
        });

        test("Play tab: the item Play, its state above the three facts; ready, not installed, and a build that cannot start the game", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            ShellNav nav = new ShellNav();
            MenuScenes.Page page = ShellPages.frame(nav, menu, "Odin Controller", UTC);
            ShellPages.play(page, nav, menu, "Odin Controller", UTC);
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "Play, ready");
            Scene.Item play = text(scene, "Play");
            same(MenuLayout.SELECT, under(scene, play), "highlighted at the start");
            same(MenuLayout.HIT_COLUMN, scene.hitAt(play.x + 5, play.y + play.h / 2), "and tappable");
            same(MenuLayout.ROW_PX, play.size, "in the size of every row");
            Scene.Item label = text(scene, "Game Data"), value = text(scene, "Found");
            check(label.y == value.y, "a fact's label and value stand on one row");
            same((float) (PANE_RIGHT - MenuLayout.VALUE_PAD), value.x + value.w, "the value ends at the row's right padding");
            same((float) (PANE + MenuLayout.ROW_PAD), label.x, "the label starts at its left padding");
            check(exact(scene, "Game Code") && exact(scene, "Odin Controller"), "the other two facts");
            check(text(scene, "Ready.").y < label.y, "the state stands above the facts");
            check(has(page.prompts, "B", "Back"), "Back");
            same("A", last(page.prompts).glyph, "Select (A) is the rightmost prompt");
            same(-1, page.rowFocused, "the facts cannot be highlighted");
            nav.command(NavInput.UP, false, false, false);
            page = ShellPages.frame(nav, menu, "Odin Controller", UTC);
            ShellPages.play(page, nav, menu, "Odin Controller", UTC);
            same(MenuLayout.PANEL, under(MenuScenes.build(page, FIXED), text(MenuScenes.build(page, FIXED), "Play")), "with the highlight on the tab strip the item is dark");
            nav.command(NavInput.DOWN, false, false, false);

            LauncherMenu fresh = new LauncherMenu(true);
            fresh.setInstall(nothingInstalled);
            page = ShellPages.frame(nav, fresh, null, UTC);
            ShellPages.play(page, nav, fresh, null, UTC);
            scene = MenuScenes.build(page, FIXED);
            tidy(scene, "Play, nothing installed");
            check(exact(scene, "Check Again"), "nothing installed: the item looks at the files again, in title case");
            same(MenuLayout.WARNING, text(scene, fresh.stateLine()).color, "the state warns");
            check(exact(scene, "None"), "no controller");

            LauncherMenu starting = new LauncherMenu(true);
            starting.setInstall(ready);
            starting.tap(LauncherMenu.PRIMARY, false);
            check(starting.preparing(), "(A on Play: the start is being prepared)");
            page = ShellPages.frame(nav, starting, "Odin Controller", UTC);
            ShellPages.play(page, nav, starting, "Odin Controller", UTC);
            check(!has(page.prompts, "A", "Select"), "while a start is being prepared A does nothing, so Select is not offered");
            check(has(page.prompts, "B", "Back"), "Back stays");

            LauncherMenu noHelper = new LauncherMenu(false);
            noHelper.setInstall(ready);
            page = ShellPages.frame(nav, noHelper, null, UTC);
            ShellPages.play(page, nav, noHelper, null, UTC);
            scene = MenuScenes.build(page, FIXED);
            check(!has(page.prompts, "A", "Select"), "a build that cannot start the game offers no Select");
            check(exact(scene, LauncherMenu.NOT_IN_THIS_BUILD), "and says why");
            same(MenuLayout.SELECT_TEXT_OFF, text(scene, "Play").color, "the highlighted item is written in grey");
        });

        test("Play tab: the question about unqualified game code stands whole above the facts, with its own prompts", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            menu.tap(LauncherMenu.PRIMARY, false);
            menu.prepared(LaunchRequest.UNQUALIFIED, false, true);
            ShellNav nav = new ShellNav();
            MenuScenes.Page page = ShellPages.frame(nav, menu, "Pad", UTC);
            ShellPages.play(page, nav, menu, "Pad", UTC);
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "Play, asking");
            Scene.Item block = null;
            for (Scene.Item item : rects(scene, MenuLayout.PANEL)) if (item.x == PANE && item.y == MenuLayout.BODY_TOP) block = item;
            List<String> shown = words(textsIn(scene, block));
            check(shown.containsAll(Arrays.asList(LauncherMenu.UNQUALIFIED_DETAIL.split("\\s+"))), "every word of the question: " + shown);
            check(!hasText(scene, "A: start anyway"), "its short form is not repeated");
            same("Start Anyway", last(page.prompts).label, "A starts it anyway");
            check(has(page.prompts, "B", "Leave It"), "B leaves it");
            same(ShellNav.TO_PLAY_QUESTION, ShellNav.route(nav.tab(), NavInput.BACK, false, false, menu.asking()), "B answers it");
            menu.startFailed("no window");
            page = ShellPages.frame(nav, menu, "Pad", UTC);
            ShellPages.play(page, nav, menu, "Pad", UTC);
            same(MenuScenes.TONE_ERROR, page.info.get(1).tone, "a start failure is shown as an error");
            check(page.info.get(1).text.contains("no window"), "with its reason");
        });

        test("Play tab: the open question is answered with no as soon as the highlight leaves the Play item", () -> {
            for (int leave : new int[] {NavInput.UP, NavInput.PAGE_NEXT}) {
                LauncherMenu menu = new LauncherMenu(true);
                menu.setInstall(ready);
                menu.tap(LauncherMenu.PRIMARY, false);
                menu.prepared(LaunchRequest.UNQUALIFIED, false, true);
                ShellNav nav = new ShellNav();
                int tab = nav.tab();
                nav.command(NavInput.LEFT, false, false, false);   // does nothing on the Play item
                ShellPages.afterMove(tab, nav, menu);
                check(menu.asking(), "a command that leaves the highlight on Play leaves the question open");
                nav.command(leave, false, false, false);
                ShellPages.afterMove(tab, nav, menu);
                check(!menu.asking(), "command " + leave + " takes the highlight away: the question is answered no");
                nav.command(leave == NavInput.UP ? NavInput.DOWN : NavInput.PAGE_PREV, false, false, false);
                MenuScenes.Page page = ShellPages.frame(nav, menu, "Pad", UTC);
                ShellPages.play(page, nav, menu, "Pad", UTC);
                same("Select", last(page.prompts).label, "back on Play, A is an ordinary Select again, not Start Anyway");
            }
            LauncherMenu other = new LauncherMenu(true);
            other.setInstall(ready);
            other.tap(LauncherMenu.PRIMARY, false);
            other.prepared(LaunchRequest.UNQUALIFIED, false, true);
            ShellNav about = new ShellNav();
            about.command(NavInput.PAGE_PREV, false, false, false);
            ShellPages.afterMove(ShellNav.ABOUT, about, other);
            check(other.asking(), "what happens on another tab does not answer it");
        });

        test("Controller tab: the item that opens the check, the controller's name, why it cannot when it cannot", () -> {
            ShellNav nav = new ShellNav();
            nav.command(NavInput.PAGE_NEXT, false, false, false);
            nav.command(NavInput.PAGE_NEXT, false, false, false);
            LauncherMenu menu = new LauncherMenu(true);
            MenuScenes.Page page = ShellPages.frame(nav, menu, "Odin Controller", UTC);
            ShellPages.controller(page, nav, menu, "Odin Controller");
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "Controller");
            same(MenuLayout.SELECT, under(scene, text(scene, ShellPages.LABEL_CONTROLLER_CHECK)), "the item is highlighted");
            same("Select", last(page.prompts).label, "A opens it");
            check(hasText(scene, "Select + Start"), "how to leave the check");
            menu.tap(LauncherMenu.CONTROLLER_CHECK, true);
            page = ShellPages.frame(nav, menu, "Odin Controller", UTC);
            ShellPages.controller(page, nav, menu, "Odin Controller");
            check(exact(MenuScenes.build(page, FIXED), LauncherMenu.CLOSE_GAME_FIRST), "while the game runs it says to close it first");
            LauncherMenu noHelper = new LauncherMenu(false);
            page = ShellPages.frame(nav, noHelper, null, UTC);
            ShellPages.controller(page, nav, noHelper, null);
            check(!has(page.prompts, "A", "Select"), "no Select in a build without the check");
            check(exact(MenuScenes.build(page, FIXED), LauncherMenu.NOT_IN_THIS_BUILD), "and the reason");
        });

        test("About tab: sections at the left, the text at the right; a long text can be entered and scrolled", () -> {
            ShellNav nav = new ShellNav();
            nav.setColumnCount(ShellNav.ABOUT, ShellPages.ABOUT_SECTIONS.length);
            nav.command(NavInput.PAGE_PREV, false, false, false);
            LauncherMenu menu = new LauncherMenu(true);
            List<String> app = MenuScenes.paneText(AboutScreen.sectionText(ShellPages.ABOUT_APP, "App version 0.1.0", "Game code: x", "names"), true, FIXED);
            MenuScenes.Page page = ShellPages.frame(nav, menu, null, UTC);
            ShellPages.about(page, nav, app, 0);
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "About, this app");
            check(hasText(scene, "App version 0.1.0") && hasText(scene, "not for distribution"), "the app section");
            same(MenuLayout.ROW_PX, text(scene, "App version 0.1.0").size, "in the rows' text size");
            check(!ShellPages.aboutScrollable(app.size()), "a short text cannot be entered");
            for (List<String> shortText : Arrays.asList(app, Arrays.asList("One line."))) {
                MenuScenes.Page shortPage = ShellPages.frame(nav, menu, null, UTC);
                ShellPages.about(shortPage, nav, shortText, 0);
                Scene shortScene = MenuScenes.build(shortPage, FIXED);
                Scene.Item block = null;
                for (Scene.Item item : rects(shortScene, MenuLayout.PANEL)) if (item.x == PANE && item.y == MenuLayout.BODY_TOP) block = item;
                check(block != null, "the text's block stands at the top of the pane");
                same(0f, (block.h + MenuLayout.ROW_GAP) % MenuLayout.ROW_PITCH, shortText.size() + " line(s): the block ends on the column's grid, level with a bar's lower edge (height " + block.h + ")");
                List<Scene.Item> lines = textsIn(shortScene, block);
                same(shortText.size(), lines.size(), "every line is inside the block");
                Scene.Item firstLine = lines.get(0), lastLine = lines.get(lines.size() - 1);
                same(firstLine.y - block.y, block.y + block.h - (lastLine.y + lastLine.h), "the lines stand in the middle of the block");
                if (shortText.size() == 1) same((float) MenuLayout.ROW_H, block.h, "one line: the block is as high as the bar beside it");
            }
            check(!has(page.prompts, "A", "Select"), "so A is not offered");
            same(2, shapes(scene, Scene.POLY).size(), "and there is no row with marks (only the tab strip's arrows)");

            nav.command(NavInput.DOWN, false, false, false);
            nav.command(NavInput.DOWN, false, false, false);
            List<String> notices = MenuScenes.paneText(AboutScreen.sectionText(ShellPages.ABOUT_NOTICES, "v", "c", "n"), true, FIXED);
            check(ShellPages.aboutScrollable(notices.size()), "the licence notices are longer than the pane");
            page = ShellPages.frame(nav, menu, null, UTC);
            ShellPages.about(page, nav, notices, 0);
            check(has(page.prompts, "A", "Select"), "A goes into them");
            scene = MenuScenes.build(page, FIXED);
            tidy(scene, "About, notices");
            // (since 2026-11-05 the app's own GPLv3 notice stands before the notices of other software)
            check(hasText(scene, "FivePocket is free software") && !hasText(scene, "Qualcomm Snapdragon"), "the first notice is on top");
            same(4, shapes(scene, Scene.POLY).size(), "the row with the up and down marks says there is more");
            same(2, rects(scene, MenuLayout.SELECT).size(), "that row is dark while the highlight is on the section's name (tab and section are light)");
            same(MenuLayout.HIT_TEXT, scene.hitAt(PANE + 100, MenuLayout.BODY_TOP + 100), "the text is one touch area (dragging scrolls it)");
            same(ShellNav.ENTERED, nav.command(NavInput.CONFIRM, false, true, false), "A goes into the text");
            int scroll = ShellPages.aboutScrolled(0, 3, notices.size());
            same(3, scroll, "DOWN scrolls three lines");
            same(0, ShellPages.aboutScrolled(scroll, -9, notices.size()), "UP stops at the top");
            int end = ShellPages.aboutScrolled(0, 100000, notices.size());
            same(notices.size() - MenuLayout.textLinesVisible(), end, "DOWN stops when the last line is shown");
            page = ShellPages.frame(nav, menu, null, UTC);
            ShellPages.about(page, nav, notices, end);
            scene = MenuScenes.build(page, FIXED);
            tidy(scene, "About, end of the notices");
            check(hasText(scene, "That's all there is to it!"), "the last line of the last notice (the LGPL text) is reachable");
            boolean second = false;
            for (String line : notices) second |= line.contains("SPDX-License-Identifier: BSD-3-Clause");
            check(second, "the second notice still ends with its identifier line, further up");
            same(3, rects(scene, MenuLayout.SELECT).size(), "while reading, the row with the marks is the light one");
            check(has(page.prompts, "B", "Back") && !has(page.prompts, "A", "Select"), "prompts while reading: Back only");

            nav.command(NavInput.BACK, false, true, false);
            nav.command(NavInput.DOWN, false, false, false);
            page = ShellPages.frame(nav, menu, null, UTC);
            ShellPages.about(page, nav, MenuScenes.paneText(AboutScreen.sectionText(ShellPages.ABOUT_SELF_TEST, "v", "c", "n"), true, FIXED), 0);
            check(has(page.prompts, "A", "Select"), "A opens the graphics self-test");
            same(ShellPages.ABOUT_SELF_TEST, page.columnSelected, "the fourth section");
        });

        test("first-run setup: the same frame without tabs, its steps as rows, its question as an alert, work in progress with a bar", () -> {
            File files = LauncherTestSupport.newFilesDir();
            SetupModel model = new SetupModel();
            model.setDisk(SetupModel.inspect(files));
            MenuScenes.Page page = ShellPages.setup(model);
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "setup");
            check(!hasText(scene, "SETTINGS"), "no tabs");
            same(0, shapes(scene, Scene.POLY).size(), "no arrows at the ends of the strip");
            same(-1, scene.hitAt(MenuLayout.W / 2f, MenuLayout.STRIP_TOP + 10), "the heading is not tappable");
            Scene.Item heading = text(scene, "SET UP GAME FILES");
            same(MenuLayout.SELECT, under(scene, heading), "the heading is the one cell of the strip, selected");
            same(SetupModel.STEPS, page.rows.size(), "one row per step");
            for (int step = 0; step < SetupModel.STEPS; ++step) {
                same(Scene.titleCase(model.label(step)), page.rows.get(step).label, "step " + step);
                check(model.stateWord(step).equalsIgnoreCase(page.rows.get(step).value), "the state of step " + step + " is the model's word: " + page.rows.get(step).value);
            }
            same("To Do", page.rows.get(SetupModel.STEP_PLAY).value, "written in title case like every value of the menu (the model's word is 'To do')");
            same(model.focused(), page.rowFocused, "the model's focused step is highlighted");
            same(model.detail(model.focused()), page.help.get(0).text, "its detail is in the help strip");
            Scene.Item first = text(scene, Scene.titleCase(model.label(0)));
            same((float) (MenuLayout.LEFT + MenuLayout.ROW_PAD), first.x, "the rows take the whole block");
            same(MenuLayout.HIT_ROW, scene.hitAt(first.x + 5, first.y + first.h / 2), "a step is tappable");

            int other = model.focused() == 0 ? 2 : 0;
            check(!ShellPages.setupTap(model, other), "a tap on a step that is not highlighted does not act");
            same(other, model.focused(), "it moves the highlight there");
            check(ShellPages.setupTap(model, other), "a tap on the highlighted step is A");

            model.focus(SetupModel.STEP_PLAY);
            model.nav(NavInput.CONFIRM, false);
            page = ShellPages.setup(model);
            check(page.alert != null, "A on the first step puts the question");
            same(model.overlayText(), page.alert.title, "the question is the alert's text");
            same(Arrays.asList(model.overlayButtons()), Arrays.asList(page.alert.buttons), "its answers");
            same(-1, page.rowFocused, "no step is highlighted under it");
            tidy(MenuScenes.build(page, FIXED), "setup question");
            check(has(page.prompts, "B", "Cancel"), "B cancels");
            same("A", last(page.prompts).glyph, "A is the rightmost prompt");
            check(!ShellPages.setupAnswerTap(model, SetupModel.BUTTON_CANCEL), "a tap on an answer that is not highlighted does not choose it");
            same(SetupModel.BUTTON_CANCEL, model.overlayFocus(), "it moves the highlight to it");
            check(model.overlay() != SetupModel.OVERLAY_NONE, "the question is still open");
            check(ShellPages.setupAnswerTap(model, SetupModel.BUTTON_CANCEL), "a tap on the highlighted answer chooses it");
            check(!ShellPages.setupAnswerTap(model, 7), "no such answer");

            model.nav(NavInput.BACK, false);
            model.workStarted(SetupModel.STEP_COPY, 1000);
            model.progress(50, 200, 1, 4, "a/b.bin", 2000);
            page = ShellPages.setup(model);
            same(0.25f, page.infoBar, "a quarter of the bytes are copied");
            check(page.info.get(0).text.startsWith("Copying"), "what is being done stands in the info block: " + page.info.get(0).text);
            scene = MenuScenes.build(page, FIXED);
            Scene.Item fill = null;
            for (Scene.Item item : rects(scene, MenuLayout.ACCENT)) if (item.h == MenuLayout.BAR_H) fill = item;
            check(fill != null, "with a bar");
            same((float) MenuLayout.barFill(0.25f), fill.w, "filled to a quarter");
            check(fill.y < text(scene, Scene.titleCase(model.label(0))).y, "above the steps");
            LauncherTestSupport.discard(files);
        });

        test("controller diagram: every line, ring and label stands inside its block; every label's line ends on the pad; the rows follow under it", () -> {
            MenuScenes.Page page = new MenuScenes.Page();
            page.tabs = new String[] {"Play", "Settings", "Controller", "About"};
            page.tab = 2;
            page.column = new String[] {"Controller Check"};
            page.columnLit = true;
            page.diagram = true;
            MenuScenes.Row fact = new MenuScenes.Row();
            fact.label = "Controller";
            fact.value = "Built-In (Thor)";
            page.rows.add(fact);
            page.help.add(new MenuScenes.Line("A checks every button and stick.", MenuScenes.TONE_NORMAL));
            Scene scene = MenuScenes.build(page, FIXED);
            tidy(scene, "diagram");
            Scene.Item block = null;
            for (Scene.Item item : rects(scene, MenuLayout.PANEL)) if (item.x == PANE && item.y == MenuLayout.BODY_TOP) block = item;
            check(block != null && block.h == MenuLayout.slotsHeight(MenuLayout.DIAGRAM_SLOTS), "a dark block of DIAGRAM_SLOTS slots at the pane's top");
            float padLeft = Float.MAX_VALUE, padRight = -Float.MAX_VALUE;
            for (Scene.Item ring : shapes(scene, Scene.RING)) {
                check(ring.x >= block.x && ring.x + ring.w <= block.x + block.w && ring.y >= block.y && ring.y + ring.h <= block.y + block.h, "a ring inside the block");
                padLeft = Math.min(padLeft, ring.x);
                padRight = Math.max(padRight, ring.x + ring.w);
            }
            List<Scene.Item> lines = shapes(scene, Scene.LINE);
            check(lines.size() >= 18, "the outline and the 18 labels' lines (" + lines.size() + ")");
            for (Scene.Item line : lines) {
                for (int p = 0; p + 1 < line.points.length; p += 2) {
                    check(line.points[p] >= block.x && line.points[p] <= block.x + block.w && line.points[p + 1] >= block.y && line.points[p + 1] <= block.y + block.h, "a line inside the block");
                }
            }
            int labels = 0;
            for (String[] side : MenuScenes.Pad.LABELS) {
                for (String name : side) {
                    Scene.Item label = text(scene, name);
                    check(label.x >= block.x && label.x + label.w <= block.x + block.w, name + ": inside the block across");
                    labels++;
                }
            }
            same(18, labels, "nine controls at each side, as the game's page");
            // the outline drawing: its widest points lie between the two columns of labels
            Scene.Item body = lines.get(0);
            float minX = Float.MAX_VALUE, maxX = -Float.MAX_VALUE;
            for (int p = 0; p < body.points.length; p += 2) { minX = Math.min(minX, body.points[p]); maxX = Math.max(maxX, body.points[p]); }
            check(minX > text(scene, "CHARACTER SWITCH").x + text(scene, "CHARACTER SWITCH").w && maxX < text(scene, "LOOK BEHIND").x, "the pad between its labels");
            Scene.Item row = text(scene, "Controller");
            check(row.y >= block.y + block.h, "the controller's row stands under the diagram");
        });

        end();
    }
}
