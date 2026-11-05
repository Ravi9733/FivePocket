package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.begin;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.caps;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.end;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

/**
 * Host test of the menu's look against the game's own menus (menu-redesign, local-analysis/menu-redesign/DESIGN.md section 2): the
 * owner's two PC references (rows, tabs, arrow heads) and the Thor's capture of the console pause menu (the header: plate, title,
 * small lines). What is compared is what a player sees in a drawn frame: the scenes are built from the app's real models and read as
 * lists of shapes and texts, and their sizes and places are held against figures measured in the pictures (REF_*, in the pictures'
 * own pixels). The menu is drawn SCALE (MenuLayout.K) times as large as the game's menu and its tab strip starts at the reference
 * strip's place in the frame (y 181 of the game, MenuLayout.STRIP_TOP here).
 *
 * Only figures that do not need a font file are here: sizes, places and outlines. Run with java -ea.
 */
public final class LookMetricsTest {
    static final float SCALE = MenuLayout.K;
    /** Top of the tab strip in both references and in the Thor's capture of the pause menu. */
    static final float REF_STRIP_TOP = 181f;
    /** Nunito Sans, the bundled face: the height of a capital, the ascent and the descent as parts of the text size. */
    static final float CAP = 0.705f, ASCENT = 1.011f, DESCENT = 0.353f;

    // Both PC references: capitals of the tab names ("MAP", "SETTINGS") and of the row labels ("Texture Quality") are equally high.
    static final float REF_ROW_CAP = 14f;
    // The Thor's capture of the pause menu: the title "Grand Theft Auto V" in the regular weight - its capitals, the row they stand on.
    static final float REF_TITLE_CAP = 25f, REF_TITLE_BASELINE = 144f;
    // The same capture: the three small lines at the right ("MICHAEL", the day and time, the money) - capitals and rows.
    static final float REF_CAPS_CAP = 12f;
    static final float[] REF_CAPS_BASELINES = {116f, 137f, 158f};
    // The arrow heads (width, height, thickness of their line): outside the strip (reference 2), round the
    // highlighted value (reference 1, "Very High"), the two "more rows" marks (reference 1), and the gap between those two.
    static final float[] REF_STRIP_ARROW = {12.6f, 19.6f, 4.07f}, REF_VALUE_ARROW = {8.8f, 13.5f, 3.05f}, REF_MORE_MARK = {17.3f, 11.2f, 3.37f};
    static final float REF_MORE_GAP = 2.9f, REF_STRIP_ARROW_MIDDLE = 199.03f;
    /** Every arrow head of the game has its two arms at this angle to the direction it points in. */
    static final double REF_ARM_DEGREES = 45;

    private static final Scene.Measure FIXED = MenuLayoutTest.FIXED;
    private static final TimeZone UTC = TimeZone.getTimeZone("UTC");
    private static final List<String> NOTHING = new ArrayList<String>();

    /** Where a row of the reference picture lies in the menu. */
    private static float placed(float referenceY) {
        return MenuLayout.STRIP_TOP + (referenceY - REF_STRIP_TOP) * SCALE;
    }

    /** The row a text's letters stand on: every painter centres the face's ascent and descent in the text's box. */
    private static float baseline(Scene.Item text) {
        return text.y + text.h / 2f + (ASCENT - DESCENT) / 2f * text.size;
    }

    private static void near(float expected, float actual, float tolerance, String what) {
        check(Math.abs(actual - expected) <= tolerance, String.format(Locale.ROOT, "%s: reference x %.1f = %.2f, drawn %.2f (tolerance %.2f)", what, SCALE, expected, actual, tolerance));
    }

    private static Scene.Item text(Scene scene, String text) {
        for (Scene.Item item : scene.items) if (item.type == Scene.TEXT && item.text.equals(text)) return item;
        throw new AssertionError("no text <" + text + "> in the scene");
    }

    private static List<Scene.Item> arrows(Scene scene) {
        List<Scene.Item> found = new ArrayList<Scene.Item>();
        for (Scene.Item item : scene.items) if (item.type == Scene.POLY) found.add(item);
        return found;
    }

    /** The settings tab under the phone's standing game build, the highlight on a category's name. */
    private static MenuScenes.Page settings(ShellNav nav, SettingsPages pages, MenuModel model) {
        MenuScenes.Page page = ShellPages.frame(nav, new LauncherMenu(true), "Odin Controller", UTC);
        pages.fill(page, model, nav, 0);
        return page;
    }

    private static ShellNav onSettings() {
        ShellNav nav = new ShellNav();
        nav.setColumnCount(ShellNav.SETTINGS, SettingsPages.COUNT);
        nav.command(NavInput.PAGE_NEXT, false, false, false);
        return nav;
    }

    private static MenuModel standingBuild() {
        MenuModel model = new MenuModel();
        model.loaded(Settings.defaults(), true, NOTHING, NOTHING, caps(SettingsCoreTestKit.V464_NAMES));
        return model;
    }

    /**
     * An arrow head as drawn (six corners, the tip first) held against the game's: its size and the thickness of
     * its line are the reference's, its arms stand at the reference's angle, its two ends are cut square to the
     * arms, and it is the same above and below the line it points along.
     */
    private static void arrowHead(Scene.Item head, float[] reference, boolean sideways, String what) {
        float[] p = head.points;
        same(12, p.length, what + ": a bent line has six corners");
        float minX = p[0], maxX = p[0], minY = p[1], maxY = p[1];
        for (int i = 2; i < p.length; i += 2) {
            minX = Math.min(minX, p[i]);
            maxX = Math.max(maxX, p[i]);
            minY = Math.min(minY, p[i + 1]);
            maxY = Math.max(maxY, p[i + 1]);
        }
        near(reference[0] * SCALE, maxX - minX, 1f, what + ": width");
        near(reference[1] * SCALE, maxY - minY, 1f, what + ": height");
        // Corners: 0 the tip, 1 and 2 one end (outer, inner), 3 the notch behind the tip, 4 and 5 the other end (inner, outer).
        double armX = p[2] - p[0], armY = p[3] - p[1], arm = Math.hypot(armX, armY);
        double cutX = p[4] - p[2], cutY = p[5] - p[3], cut = Math.hypot(cutX, cutY);
        double thickness = Math.abs(armX * (p[7] - p[1]) - armY * (p[6] - p[0])) / arm;   // the notch's distance from the outer edge
        near(reference[2] * SCALE, (float) thickness, 0.3f, what + ": thickness of its line");
        near(reference[2] * SCALE, (float) cut, 0.3f, what + ": its end is as wide as its line is thick");
        check(Math.abs(armX * cutX + armY * cutY) / (arm * cut) < 0.02, what + ": its end is cut square to its arm (the cosine of the corner is "
                + (armX * cutX + armY * cutY) / (arm * cut) + ")");
        double degrees = Math.toDegrees(Math.atan2(sideways ? Math.abs(armY) : Math.abs(armX), sideways ? Math.abs(armX) : Math.abs(armY)));
        check(Math.abs(degrees - REF_ARM_DEGREES) <= 4, what + ": its arms stand at " + degrees + " degrees to the way it points (the game's: 45)");
        double innerX = p[4] - p[6], innerY = p[5] - p[7];
        check(Math.abs(armX * innerY - armY * innerX) / (arm * Math.hypot(innerX, innerY)) < 0.02, what + ": the inner edge of an arm runs along its outer edge");
        for (int a = 2, b = 10; a < b; a += 2, b -= 2) {   // corner 1 mirrors 5, corner 2 mirrors 4
            if (sideways) check(Math.abs(p[a] - p[b]) < 0.01f && Math.abs((p[a + 1] - p[1]) + (p[b + 1] - p[1])) < 0.01f, what + ": the lower arm mirrors the upper one");
            else check(Math.abs(p[a + 1] - p[b + 1]) < 0.01f && Math.abs((p[a] - p[0]) + (p[b] - p[0])) < 0.01f, what + ": the right arm mirrors the left one");
        }
        check(sideways ? Math.abs(p[7] - p[1]) < 0.01f : Math.abs(p[6] - p[0]) < 0.01f, what + ": the notch lies straight behind the tip");
    }

    public static void main(String[] args) {
        begin("LookMetricsTest");

        test("arrow heads: bent lines with square-cut ends in the game's sizes and line thickness - beside the strip, round a value, the more-rows marks", () -> {
            MenuModel model = standingBuild();
            ShellNav nav = onSettings();
            SettingsPages pages = new SettingsPages();
            nav.command(NavInput.DOWN, false, true, false);
            nav.command(NavInput.CONFIRM, false, true, false);
            pages.enter(model, SettingsPages.FRAME_RATE);
            pages.forward(model, nav, NavInput.RIGHT, false);   // Frame Rate Cap one step on: the value can go both ways
            List<Scene.Item> heads = arrows(MenuScenes.build(settings(nav, pages, model), FIXED));
            same(4, heads.size(), "the strip's two arrows and the two round the highlighted value");
            arrowHead(heads.get(0), REF_STRIP_ARROW, true, "the arrow before the strip");
            arrowHead(heads.get(1), REF_STRIP_ARROW, true, "the arrow after the strip");
            arrowHead(heads.get(2), REF_VALUE_ARROW, true, "the arrow after the value");
            arrowHead(heads.get(3), REF_VALUE_ARROW, true, "the arrow before the value");
            check(heads.get(0).points[0] < MenuLayout.LEFT && heads.get(0).points[2] > heads.get(0).points[0], "the arrow before the strip points left, away from it");
            check(heads.get(1).points[0] > MenuLayout.RIGHT && heads.get(1).points[2] < heads.get(1).points[0], "the arrow after the strip points right");
            for (Scene.Item head : heads.subList(0, 2)) {
                near(placed(REF_STRIP_ARROW_MIDDLE), head.points[1], 1f, "a strip arrow's tip stands a little above the middle of the tab cells, as the game's");
            }

            MenuScenes.Page list = new MenuScenes.Page();
            for (int i = 0; i < 30; ++i) {
                MenuScenes.Row row = new MenuScenes.Row();
                row.label = "Row " + i;
                list.rows.add(row);
            }
            list.rowFocused = 10;
            list.shoulders = false;
            List<Scene.Item> marks = arrows(MenuScenes.build(list, FIXED));
            same(2, marks.size(), "a list longer than the pane: the up and the down mark");
            arrowHead(marks.get(0), REF_MORE_MARK, false, "the up mark");
            arrowHead(marks.get(1), REF_MORE_MARK, false, "the down mark");
            check(marks.get(0).points[1] < marks.get(0).points[3] && marks.get(1).points[1] > marks.get(1).points[3], "the upper mark points up, the lower one down");
            float upperEnd = marks.get(0).points[5], lowerStart = marks.get(1).points[5];   // the ends' inner corners: where the two marks face each other
            near(REF_MORE_GAP * SCALE, lowerStart - upperEnd, 1f, "the gap between the two marks");

            MenuScenes.Page alert = new MenuScenes.Page();
            StringBuilder names = new StringBuilder();
            for (int i = 1; i <= 24; ++i) names.append(i == 1 ? "" : "\n").append("file-").append(i);
            alert.alert = new ConfirmOverlay("Files", names.toString(), new String[] {"Close"}, 0);
            List<Scene.Item> alertMarks = arrows(MenuScenes.build(alert, FIXED));
            same(2, alertMarks.size(), "an alert with a long list has the same two marks");
            arrowHead(alertMarks.get(0), REF_MORE_MARK, false, "the alert's up mark");
            arrowHead(alertMarks.get(1), REF_MORE_MARK, false, "the alert's down mark");
        });

        test("text sizes: the capitals of tabs, rows, title and small lines are as high as the game's", () -> {
            MenuModel model = standingBuild();
            ShellNav nav = onSettings();
            Scene scene = MenuScenes.build(settings(nav, new SettingsPages(), model), FIXED);
            Scene.Item tab = text(scene, "SETTINGS"), other = text(scene, "PLAY"), category = text(scene, "Display"), row = text(scene, "Scaler");
            near(REF_ROW_CAP * SCALE, tab.size * CAP, 0.6f, "capitals of the selected tab's name");
            near(REF_ROW_CAP * SCALE, other.size * CAP, 0.6f, "capitals of another tab's name");
            near(REF_ROW_CAP * SCALE, category.size * CAP, 0.6f, "capitals of a category's name");
            near(REF_ROW_CAP * SCALE, row.size * CAP, 0.6f, "capitals of a row's label");
            Scene.Item title = text(scene, ShellPages.TITLE);
            near(REF_TITLE_CAP * SCALE, title.size * CAP, 0.6f, "capitals of the title");
            near(placed(REF_TITLE_BASELINE), baseline(title), 1f, "the row the title stands on");
            Scene.Item heading = null;
            for (Scene.Item item : MenuScenes.build(alertPage(), FIXED).items) if (item.type == Scene.TEXT && item.text.equals(MenuScenes.ALERT_HEADING)) heading = item;
            check(heading != null && heading.size > title.size && heading.face == Scene.HEAVY, "(the alert's heading is in the heavy weight, larger)");
        });

        test("small lines: three lines of capitals as high as the game's, standing on the game's rows above the strip", () -> {
            MenuModel model = standingBuild();
            MenuScenes.Page page = settings(onSettings(), new SettingsPages(), model);
            same(REF_CAPS_BASELINES.length, page.status.size(), "the frame has the game's three lines");
            Scene scene = MenuScenes.build(page, FIXED);
            for (int i = 0; i < REF_CAPS_BASELINES.length; ++i) {
                Scene.Item line = text(scene, page.status.get(i).text.toUpperCase(Locale.ROOT));
                near(REF_CAPS_CAP * SCALE, line.size * CAP, 0.6f, "capitals of small line " + (i + 1));
                near(placed(REF_CAPS_BASELINES[i]), baseline(line), 1f, "the row small line " + (i + 1) + " stands on");
            }
            // With fewer lines the last one keeps its place just above the strip.
            page.status.remove(0);
            Scene two = MenuScenes.build(page, FIXED);
            near(placed(REF_CAPS_BASELINES[2]), baseline(text(two, page.status.get(1).text.toUpperCase(Locale.ROOT))), 1f, "with two lines the second stands on the third's row");
        });

        test("shadow: the texts that stand on the backdrop itself (title, small lines) carry it; nothing on a bar or block does", () -> {
            MenuModel model = standingBuild();
            ShellNav nav = onSettings();
            SettingsPages pages = new SettingsPages();
            nav.command(NavInput.CONFIRM, false, true, false);
            pages.enter(model, SettingsPages.DISPLAY);
            MenuScenes.Page page = settings(nav, pages, model);
            page.footerStatus = new MenuScenes.Line(MenuModel.TEXT_SAVED, MenuScenes.TONE_GOOD);
            Scene scene = MenuScenes.build(page, FIXED);
            int shadowed = 0, plain = 0;
            for (Scene.Item item : scene.items) {
                if (item.type != Scene.TEXT) {
                    check(!item.shadow, "only texts have a shadow");
                    continue;
                }
                boolean onBackdrop = item.face == Scene.TITLE || item.face == Scene.CAPS;
                same(onBackdrop, item.shadow, (onBackdrop ? "on the backdrop, with the shadow: " : "on a bar or block, without: ") + item.text);
                if (item.shadow) shadowed++;
                else plain++;
            }
            same(1 + page.status.size(), shadowed, "the title and every small line");
            check(plain > 10, "(tabs, categories, rows, values, help, status and prompts were looked at: " + plain + " texts)");
            check(!text(scene, "SETTINGS").shadow && !text(scene, "Display").shadow && !text(scene, "Scaler").shadow && !text(scene, MenuModel.TEXT_SAVED).shadow
                    && !text(scene, "Select").shadow, "tab, category, row, status and prompt by name");
        });

        test("shadow: its offset and blur grow with the text, as the game's", () -> {
            float title = MenuLayout.shadowOffset(MenuLayout.TITLE_PX), small = MenuLayout.shadowOffset(MenuLayout.CAPS_PX);
            check(title > 0 && small > 0, "right and down of the letters");
            float share = (float) MenuLayout.CAPS_PX / MenuLayout.TITLE_PX;
            check(Math.abs(small - title * share) < 0.05f, "the small lines' shadow lies as much nearer as their text is smaller: " + small + " px against the title's " + title);
            check(Math.abs(MenuLayout.shadowBlur(MenuLayout.CAPS_PX) - MenuLayout.shadowBlur(MenuLayout.TITLE_PX) * share) < 0.05f, "and is as much sharper");
        });

        test("header and prompts: the plate, the portrait box and the prompt strip where and as large as the console pause menu's", () -> {
            MenuModel model = standingBuild();
            Scene scene = MenuScenes.build(settings(onSettings(), new SettingsPages(), model), FIXED);
            Scene.Item plate = null, box = null;
            for (Scene.Item item : scene.items) {
                if (item.type == Scene.RECT && item.color == MenuLayout.PLATE) plate = item;
                if (item.type == Scene.RECT && item.color == 0xD9000000 && item.y < MenuLayout.STRIP_TOP) box = item;
            }
            // The Thor's capture: the plate y 84..180 across the block; the portrait box 55 x 54 at y 105, 13 to 14 before the block's end.
            check(plate != null && plate.x == MenuLayout.LEFT && plate.w == MenuLayout.BLOCK_W, "the plate spans the block");
            near(placed(84f), plate.y, 1f, "the plate's top");
            near(placed(180f), plate.y + plate.h, 1f, "the plate's bottom");
            check(box != null, "the portrait box");
            near(55f * SCALE, box.w, 1.5f, "the portrait box's width");
            near(54f * SCALE, box.h, 1.5f, "the portrait box's height");
            near(placed(105f), box.y, 1.5f, "the portrait box's top");
            near(MenuLayout.RIGHT - 13.5f * SCALE, box.x + box.w, 1.5f, "the portrait box's right end");
            // The frame's proportions (both PC references, the console capture): block 1302, column 432, rows 37 on 40.5, strip 38.5.
            near(1302f * SCALE, MenuLayout.BLOCK_W, 2f, "the block's width");
            near(432f * SCALE, MenuLayout.COLUMN_W, 1.5f, "the column's width");
            near(37f * SCALE, MenuLayout.ROW_H, 1f, "a row's height");
            near(40.5f * SCALE, MenuLayout.ROW_PITCH, 1f, "the rows' pitch");
            near(38.5f * SCALE, MenuLayout.STRIP_H, 1f, "the tab strip's height");
            // The capture's prompt strip is as high as a row (37), its marks 21 across; label text capitals 13.
            near(37f * SCALE, MenuLayout.PROMPT_H, 1f, "the prompt strip's height");
            near(21f * SCALE, MenuLayout.MARK_D, 1f, "a button mark's size");
            near(13f * SCALE, MenuLayout.PROMPT_PX * CAP, 0.6f, "capitals of a prompt's label");
        });

        test("restart alert (public builds): the owner's alert screen's measured places - heading, two 150 x 3 rules, the message between them", () -> {
            Scene scene = MenuScenes.restartAlert("This setting change requires a restart.", FIXED);
            same(MenuLayout.ALERT_BLACK, scene.items.get(0).color, "black");
            List<Scene.Item> rules = new ArrayList<Scene.Item>();
            Scene.Item heading = null, message = null;
            for (Scene.Item item : scene.items) {
                if (item.type == Scene.RECT && item.color == MenuLayout.TEXT) rules.add(item);
                if (item.type == Scene.TEXT && item.text.equals(MenuScenes.ALERT_HEADING)) heading = item;
                if (item.type == Scene.TEXT && item.text.startsWith("This setting")) message = item;
            }
            same(2, rules.size(), "two rules");
            same(583f, rules.get(0).y, "the first rule where the owner's is (y 583 of 1080)");
            same(659f, rules.get(1).y, "the second (y 659)");
            for (Scene.Item rule : rules) {
                same(150f, rule.w, "150 wide");
                same(3f, rule.h, "3 high");
                same((float) MenuLayout.W, 2 * rule.x + rule.w, "centred");
            }
            check(heading != null && heading.face == Scene.HEAVY && heading.color == MenuLayout.ALERT_YELLOW, "the heading: heavy, yellow");
            float headingInk = baseline(heading);
            near(544f, headingInk, 4f, "the heading's letters stand on the owner's row (ink to y 544)");
            check(message != null && baseline(message) > rules.get(0).y && baseline(message) < rules.get(1).y, "the message between the rules");
            near(633f, baseline(message), 2f, "on the owner's row (y 633)");
            near(22f, message.size * CAP, 0.8f, "capitals 22 high");
            same(MenuLayout.HIT_ALERT_BUTTON, scene.hitAt(10, 10), "a tap anywhere continues");
        });

        end();
    }

    private static MenuScenes.Page alertPage() {
        MenuScenes.Page page = new MenuScenes.Page();
        page.alert = new ConfirmOverlay("Save changes?", null, new String[] {"Save", "Discard"}, 0);
        return page;
    }
}
