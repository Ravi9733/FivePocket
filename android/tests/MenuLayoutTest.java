package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.begin;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.end;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.util.Arrays;
import java.util.List;

/**
 * Host test of the menu's layout arithmetic (MenuLayout) and text fitting (Scene): the parts of the frame tile the
 * 1920 x 1080 panel without overlapping or leaving it, blocks grow with what they hold, lists keep the highlighted
 * row on screen, and texts are cut and wrapped to the width they are given. Run with java -ea.
 */
public final class MenuLayoutTest {
    /** Every character is half the text size wide (bold: 0.6, the two narrow faces: 0.35): widths worked out by hand. */
    static final Scene.Measure FIXED = new Scene.Measure() {
        @Override public float width(String text, int sizePx, int face) {
            return text.length() * sizePx * (face == Scene.BOLD ? 0.6f : face == Scene.REGULAR ? 0.5f : 0.35f);
        }
    };

    public static void main(String[] args) {
        begin("MenuLayoutTest");

        test("tabs: any number of tabs fills the block edge to edge with the gap between neighbours", () -> {
            for (int count = 1; count <= 6; ++count) {
                same(MenuLayout.LEFT, MenuLayout.tabLeft(0, count), "the first tab starts the block");
                int widest = 0, narrowest = Integer.MAX_VALUE;
                for (int i = 0; i < count; ++i) {
                    int left = MenuLayout.tabLeft(i, count), width = MenuLayout.tabWidth(i, count);
                    check(width > 0, "tab " + i + " of " + count + " has a width");
                    if (i + 1 < count) same(left + width + MenuLayout.TAB_GAP, MenuLayout.tabLeft(i + 1, count), "gap after tab " + i);
                    else same(MenuLayout.RIGHT, left + width, "the last tab ends the block");
                    widest = Math.max(widest, width);
                    narrowest = Math.min(narrowest, width);
                }
                check(widest - narrowest <= 1, "tabs are equally wide to the pixel");
            }
        });

        test("frame: the block is centred; column, gap and pane fill it; the bands follow each other down the screen", () -> {
            same(MenuLayout.W, MenuLayout.LEFT + MenuLayout.RIGHT, "the block is centred on the screen");
            same(MenuLayout.LEFT + MenuLayout.COLUMN_W + MenuLayout.PANE_GAP, MenuLayout.paneLeft(true), "the pane starts after the column");
            same(MenuLayout.RIGHT, MenuLayout.paneLeft(true) + MenuLayout.paneWidth(true), "and ends the block");
            same(MenuLayout.LEFT, MenuLayout.paneLeft(false), "without a column the pane starts the block");
            same(MenuLayout.RIGHT, MenuLayout.paneLeft(false) + MenuLayout.paneWidth(false), "and takes all of it");
            check(MenuLayout.STATUS_TOP + MenuLayout.STATUS_LINES * MenuLayout.STATUS_LINE_H <= MenuLayout.STRIP_TOP - MenuLayout.TAB_LINE,
                    "the small lines end above the coloured line of the selected tab");
            check(MenuLayout.TITLE_TOP >= 0 && MenuLayout.TITLE_TOP + MenuLayout.TITLE_H <= MenuLayout.STRIP_TOP, "the title stands above the strip");
            check(MenuLayout.STRIP_TOP + MenuLayout.STRIP_H < MenuLayout.BODY_TOP, "the strip ends above the body");
            same(MenuLayout.BODY_BOTTOM, MenuLayout.slotTop(MenuLayout.BODY_SLOTS - 1) + MenuLayout.ROW_H, "the body ends with its last slot");
            check(MenuLayout.BODY_BOTTOM < MenuLayout.PROMPT_TOP, "the body ends above the prompt strip");
            // The console pause menu: the prompt strip stands inside the screen, ending at the frame's right edge (DESIGN.md 3.2).
            same(MenuLayout.PROMPT_BOTTOM, MenuLayout.PROMPT_TOP + MenuLayout.PROMPT_H, "the prompt strip is PROMPT_H high");
            check(MenuLayout.PROMPT_BOTTOM < MenuLayout.H && MenuLayout.PROMPT_BOTTOM >= MenuLayout.H * 0.95f, "and ends a little above the screen's bottom");
            same(MenuLayout.RIGHT, MenuLayout.PROMPT_RIGHT, "at the frame's right edge");
            same(MenuLayout.slotTop(3) - MenuLayout.slotTop(2), MenuLayout.slotsHeight(1) + MenuLayout.ROW_GAP, "a slot is a row and the gap under it");
            same(MenuLayout.slotTop(5) - MenuLayout.ROW_GAP, MenuLayout.slotTop(2) + MenuLayout.slotsHeight(3), "a block of three slots ends where the third row ends");
        });

        test("info block: no block without text; taller than a row from the first line; always high enough for its lines", () -> {
            same(0, MenuLayout.infoSlots(0), "no text, no block");
            same(2, MenuLayout.infoSlots(1), "one line: two slots, so that it does not read as a row");
            same(2, MenuLayout.infoSlots(2), "two lines: two slots");
            for (int lines = 1; lines <= 14; ++lines) {
                int slots = MenuLayout.infoSlots(lines), needed = lines * MenuLayout.INFO_LINE + 2 * MenuLayout.INFO_PAD;
                check(MenuLayout.slotsHeight(slots) >= needed, lines + " lines fit their block");
                if (slots > 2) check(MenuLayout.slotsHeight(slots - 1) < needed, lines + " lines would not fit one slot fewer");
            }
        });

        test("help strip: one line is as high as a row, every further line adds a line", () -> {
            same(0, MenuLayout.helpHeight(0), "no text, no strip");
            same(MenuLayout.ROW_H, MenuLayout.helpHeight(1), "one line");
            same(MenuLayout.HELP_LINE, MenuLayout.helpHeight(4) - MenuLayout.helpHeight(3), "a fourth line");
        });

        test("rows: all of them when they fit; otherwise as many as fit beside the row that says there are more", () -> {
            for (int info : new int[] {0, 2, 5, 9}) {
                for (int help = 0; help <= MenuLayout.HELP_LINES; ++help) {
                    for (int count : new int[] {1, 3, 5, 9, 16, 40}) {
                        int visible = MenuLayout.visibleRows(count, info, help);
                        check(visible >= 1 && visible <= count, "between one row and all of them: " + visible + " of " + count);
                        int slots = info + visible + (visible < count ? 1 : 0);
                        int shown = MenuLayout.helpLinesShown(info, help);
                        if (info <= 5) same(help, shown, "the help strip is whole under a notice of up to five slots");
                        int bottom = MenuLayout.slotTop(slots) - MenuLayout.ROW_GAP + (shown > 0 ? MenuLayout.ROW_GAP + MenuLayout.helpHeight(shown) : 0);
                        check(bottom <= MenuLayout.BODY_BOTTOM, "info " + info + ", help " + help + ", " + count + " rows: everything ends inside the body (" + bottom + ")");
                        if (visible < count) check(bottom + MenuLayout.ROW_PITCH > MenuLayout.BODY_BOTTOM, "one more row would not fit");
                    }
                }
            }
            same(1, MenuLayout.visibleRows(5, MenuLayout.BODY_SLOTS - 2, MenuLayout.HELP_LINES), "under a notice that fills the body one row is still drawn");
            // menu-redesign: rows 52 on a 57 pitch (the game's 37 on 40.5, times 1.4) leave 12 slots: a full help strip and the largest
            // category still fit under a notice of four slots (five or more scroll the rows).
            same(5, MenuLayout.visibleRows(5, 4, MenuLayout.HELP_LINES), "the largest category under a four-slot notice and a full help strip: no scrolling");
        });

        test("window: the list moves only as far as needed to keep the highlighted row on screen", () -> {
            same(0, MenuLayout.windowStart(0, 3, 5, 7), "a list that fits never moves");
            same(0, MenuLayout.windowStart(4, 2, 5, 7), "a list that fits goes back to its start");
            same(0, MenuLayout.windowStart(0, 6, 12, 7), "the last visible row needs no move");
            same(1, MenuLayout.windowStart(0, 7, 12, 7), "one row past the end moves by one");
            same(5, MenuLayout.windowStart(1, 11, 12, 7), "the last row: the window ends at the list's end");
            same(3, MenuLayout.windowStart(5, 3, 12, 7), "a row above the window becomes the first row");
            same(5, MenuLayout.windowStart(5, 8, 12, 7), "a row inside the window moves nothing");
            same(5, MenuLayout.windowStart(9, 11, 12, 7), "a start past the last full window is pulled back");
            same(2, MenuLayout.windowStart(2, -1, 12, 7), "no highlighted row: the window stays");
        });

        test("slider bar: the filled part is the fraction of the bar, never less than nothing or more than all", () -> {
            same(0, MenuLayout.barFill(0f), "0");
            same(MenuLayout.BAR_W, MenuLayout.barFill(1f), "1");
            same(Math.round(MenuLayout.BAR_W * 0.25f), MenuLayout.barFill(0.25f), "a quarter");
            same(Math.round(MenuLayout.BAR_W * 0.8f), MenuLayout.barFill(0.8f), "80 %");
            same(0, MenuLayout.barFill(-0.5f), "below 0");
            same(MenuLayout.BAR_W, MenuLayout.barFill(1.7f), "above 1");
            same(0, MenuLayout.barFill(Float.NaN), "not a number");
        });

        test("scrolled text: the position stays inside the text; the shown lines end above the row that says there is more", () -> {
            same(0, MenuLayout.clampScroll(-3, 40, 17), "not above the top");
            same(23, MenuLayout.clampScroll(99, 40, 17), "not below the last full page");
            same(5, MenuLayout.clampScroll(5, 40, 17), "inside: unchanged");
            same(0, MenuLayout.clampScroll(4, 10, 17), "a text that fits does not scroll");
            int lines = MenuLayout.textLinesVisible();
            int end = MenuLayout.BODY_TOP + 2 * MenuLayout.TEXT_PAD + lines * MenuLayout.TEXT_LINE;
            check(end <= MenuLayout.slotTop(MenuLayout.BODY_SLOTS - 1) - MenuLayout.ROW_GAP, "the lines end above the last slot");
            check(end + MenuLayout.TEXT_LINE > MenuLayout.slotTop(MenuLayout.BODY_SLOTS - 1) - MenuLayout.ROW_GAP, "one more line would not fit");
        });

        test("a text that fits the pane: its block takes whole slots, one line is as high as a row, every line has its room", () -> {
            same(1, MenuLayout.textSlots(1), "one line: one slot, as high as the column's bar beside it");
            same(2, MenuLayout.textSlots(2), "two lines: two slots");
            for (int lines = 1; lines <= MenuLayout.textLinesVisible(); ++lines) {
                int slots = MenuLayout.textSlots(lines);
                check(MenuLayout.slotsHeight(slots) >= lines * MenuLayout.TEXT_LINE, lines + " lines fit their " + slots + " slots");
                check(slots <= MenuLayout.BODY_SLOTS, lines + " lines stay inside the body");
                if (slots > 1) check(MenuLayout.slotsHeight(slots - 1) < lines * MenuLayout.TEXT_LINE + 2 * MenuLayout.INFO_PAD, lines + " lines would be cramped in one slot fewer");
            }
        });

        test("alert: everything it shows lies between the top of the screen and the prompts; its answers are centred", () -> {
            for (int title = 1; title <= 3; ++title) {
                for (int detail : new int[] {0, 1, 3, MenuLayout.ALERT_LINES, 40}) {
                    int top = MenuLayout.alertTop(title, detail), height = MenuLayout.alertHeight(title, detail);
                    check(top >= 0 && top + height <= MenuLayout.PROMPT_TOP, "above the prompts for " + title + " title and " + detail + " detail lines");
                    check(Math.abs((MenuLayout.PROMPT_TOP - height) - 2 * top) <= 1, "centred between the top and the prompts");
                }
            }
            same(MenuLayout.alertHeight(1, MenuLayout.ALERT_LINES), MenuLayout.alertHeight(1, 40), "a long list does not grow the alert past its line limit");
            same(MenuLayout.ALERT_LINE, MenuLayout.alertHeight(1, 3) - MenuLayout.alertHeight(1, 2), "one more detail line, one more line");
            same(MenuLayout.ALERT_LINE, MenuLayout.alertHeight(2, 0) - MenuLayout.alertHeight(1, 0), "one more line of the question, one more line");
            for (int count = 1; count <= 3; ++count) {
                int first = MenuLayout.choiceLeft(0, count), last = MenuLayout.choiceLeft(count - 1, count) + MenuLayout.CHOICE_W;
                same(MenuLayout.W, first + last, count + " answers are centred as a line");
                if (count > 1) same(MenuLayout.CHOICE_W + MenuLayout.TAB_GAP, MenuLayout.choiceLeft(1, count) - first, "answer pitch");
            }
        });

        test("backdrop picture: read at a size that is at least 240 wide and less than 480, whatever the file's size", () -> {
            for (int width : new int[] {240, 479, 480, 1280, 1920, 3840, 8000}) {
                int sample = MenuLayout.backdropSample(width, width * 9 / 16);
                check(Integer.bitCount(sample) == 1, width + ": a power of two (" + sample + ")");
                check(width / sample >= 240 && width / sample < 480, width + " is read " + width / sample + " wide");
            }
            same(8, MenuLayout.backdropSample(1920, 1080), "a 1920 wide screenshot is read at an eighth");
            same(1, MenuLayout.backdropSample(100, 100), "a small picture is read as it is");
        });

        test("backdrop picture: a narrow but very tall picture is reduced further, until it is small in memory", () -> {
            // The second review's files: read by their width alone they took 115 to 1900 MB (Android cannot draw above 100 MB).
            for (int[] size : new int[][] {{479, 20000}, {479, 60000}, {300, 100000}, {950, 120000}, {479, 1000000}, {1, 5000000}, {479, 4000}, {2100, 2}, {300, 4097}}) {
                int sample = MenuLayout.backdropSample(size[0], size[1]);
                // What a decoder holds at most: each side divided by the sample and rounded up, at least one pixel.
                long w = (size[0] + sample - 1) / sample, h = (size[1] + sample - 1) / sample;
                check(Integer.bitCount(sample) == 1, size[0] + " x " + size[1] + ": a power of two (" + sample + ")");
                check(w * h <= MenuLayout.BACKDROP_PIXELS, size[0] + " x " + size[1] + " is held as " + w * h + " pixels (sample " + sample + ")");
                check(w <= MenuLayout.BACKDROP_SIDE && h <= MenuLayout.BACKDROP_SIDE, "and no side is longer than a texture may be: " + w + " x " + h);
                long w2 = (size[0] + sample / 2 - 1) / Math.max(1, sample / 2), h2 = (size[1] + sample / 2 - 1) / Math.max(1, sample / 2);
                check(sample == 1 || size[0] / sample >= 240 || w2 * h2 > MenuLayout.BACKDROP_PIXELS || Math.max(w2, h2) > MenuLayout.BACKDROP_SIDE, "and it is not reduced further than needed");
            }
            check(MenuLayout.BACKDROP_PIXELS * 4L <= 8L << 20, "the limit is a few megabytes at four bytes a pixel");
        });

        test("backdrop picture: the reduced picture is doubled as often as it stays within the memory limit, so that it is drawn smooth", () -> {
            same(2, MenuLayout.backdropDoublings(240, 135), "a screenshot read at 240 x 135 is doubled twice, to 960 x 540");
            same(1, MenuLayout.backdropDoublings(479, 269), "one read at 479 wide once, to 958");
            same(0, MenuLayout.backdropDoublings(700, 1400), "a picture that only just fits is not enlarged");
            same(0, MenuLayout.backdropDoublings(59, 2000), "nor a tall strip whose long side would become too long");
            same(0, MenuLayout.backdropDoublings(0, 100), "a picture without a size: nothing to do, and the count ends");
            for (int[] size : new int[][] {{240, 135}, {479, 269}, {400, 300}, {100, 100}, {1, 1}, {390, 1}, {59, 2000}, {1024, 1024}}) {
                int steps = MenuLayout.backdropDoublings(size[0], size[1]);
                long w = (long) size[0] << steps, h = (long) size[1] << steps;
                check(w * h <= MenuLayout.BACKDROP_PIXELS, size[0] + " x " + size[1] + " doubled " + steps + " times stays within the memory limit (" + w * h + " pixels)");
                check(w <= MenuLayout.BACKDROP_SIDE && h <= MenuLayout.BACKDROP_SIDE, "and no side grows longer than a texture may be: " + w + " x " + h);
                check(w * h * 4 > MenuLayout.BACKDROP_PIXELS || 2 * Math.max(w, h) > MenuLayout.BACKDROP_SIDE, "and one more doubling would not fit");
            }
        });

        test("backdrop picture: the middle part with the window's shape is shown, never a stretched picture", () -> {
            same(Arrays.asList(0, 0, 240, 135), list(MenuLayout.cropToFill(240, 135, 1920, 1080)), "the same shape: all of it");
            same(Arrays.asList(40, 0, 280, 135), list(MenuLayout.cropToFill(320, 135, 1920, 1080)), "a wider picture loses its sides equally");
            same(Arrays.asList(0, 30, 240, 165), list(MenuLayout.cropToFill(240, 195, 1920, 1080)), "a taller picture loses top and bottom equally");
            int[] part = MenuLayout.cropToFill(1000, 1000, 1920, 1200);   // the lower screen's shape, say
            same(1000 * 1200 / 1920, part[3] - part[1], "the part has the window's shape");
            same(1000, part[0] + part[2], "and is centred");
            same(Arrays.asList(0, 0, 240, 135), list(MenuLayout.cropToFill(240, 135, 0, 0)), "a window without a size (a view not laid out yet): all of the picture, no division by nothing");
            same(Arrays.asList(0, 0, 240, 135), list(MenuLayout.cropToFill(240, 135, 1920, 0)), "nor with a height of nothing");
            for (int[] odd : new int[][] {{1, 1, 1920, 1080}, {390, 1, 1920, 1080}, {240, 135, 1920, 1}, {240, 135, 1, 1080}, {59, 7500, 1920, 1080}}) {
                int[] shown = MenuLayout.cropToFill(odd[0], odd[1], odd[2], odd[3]);
                check(shown[2] > shown[0] && shown[3] > shown[1], odd[0] + " x " + odd[1] + " in " + odd[2] + " x " + odd[3] + ": at least one pixel is shown");
                check(shown[0] >= 0 && shown[1] >= 0 && shown[2] <= odd[0] && shown[3] <= odd[1], "and it is a part of the picture");
            }
        });

        test("fade: the content is visible at once and fully opaque after the fade time", () -> {
            same(MenuLayout.FADE_FROM, MenuLayout.fadeAlpha(0), "at the change");
            same(1f, MenuLayout.fadeAlpha(MenuLayout.FADE_MS), "at the end");
            same(1f, MenuLayout.fadeAlpha(10_000), "long after");
            same(MenuLayout.FADE_FROM, MenuLayout.fadeAlpha(-50), "a clock that went back");
            float half = MenuLayout.fadeAlpha(MenuLayout.FADE_MS / 2);
            check(Math.abs(half - (MenuLayout.FADE_FROM + 1f) / 2f) < 0.01f, "half way at half time: " + half);
        });

        test("fit: a text that is too wide is cut and ends in an ellipsis; one that fits is untouched", () -> {
            same("Traffic", Scene.fit("Traffic", 36, Scene.REGULAR, 7 * 18, FIXED), "fits exactly");
            String cut = Scene.fit("Scenario pedestrians", 36, Scene.REGULAR, 10 * 18, FIXED);
            same("Scenario" + Scene.ELLIPSIS, cut, "cut at the width, the space before the ellipsis dropped");
            check(FIXED.width(cut, 36, Scene.REGULAR) <= 10 * 18, "the cut text fits");
            same(Scene.ELLIPSIS, Scene.fit("Traffic", 36, Scene.REGULAR, 5, FIXED), "no room at all: the ellipsis alone");
            same("", Scene.fit(null, 36, Scene.REGULAR, 100, FIXED), "no text");
            same("Traffic", Scene.fit("Traffic", 36, Scene.TITLE, 7 * 13, FIXED), "a narrow face fits where the regular one would not");
        });

        test("wrap: lines break at spaces, keep paragraph breaks and indents, and cut a word wider than a line", () -> {
            List<String> lines = Scene.wrap("one two three four", 20, Scene.REGULAR, 90, FIXED);   // 9 characters per line
            same(Arrays.asList("one two", "three", "four"), lines, "breaks at spaces");
            for (String line : lines) check(FIXED.width(line, 20, Scene.REGULAR) <= 90, "line fits: " + line);
            same(Arrays.asList("a", "", "b"), Scene.wrap("a\n\nb", 20, Scene.REGULAR, 90, FIXED), "an empty line stays");
            same(Arrays.asList("    ab cd", "    ef"), Scene.wrap("    ab cd ef", 20, Scene.REGULAR, 90, FIXED), "the indent is kept on every line of the paragraph");
            same(Arrays.asList("abcdefghi", "jklm"), Scene.wrap("abcdefghijklm", 20, Scene.REGULAR, 90, FIXED), "a word wider than the line is cut");
            same(Arrays.asList("ab", "abcdefghi", "jk lm"), Scene.wrap("ab abcdefghijk lm", 20, Scene.REGULAR, 90, FIXED), "a long word after a short one");
            same(Arrays.asList("x"), Scene.wrap("x", 20, Scene.REGULAR, 90, FIXED), "a short text is one line");
            same(0, Scene.wrap(null, 20, Scene.REGULAR, 90, FIXED).size(), "no text, no line");
        });

        test("reflow: lines cut by hand are joined into paragraphs; headings and sentences keep their line breaks", () -> {
            String wide = "Permission is hereby granted, free of charge, to any person obtaining a copy";   // 76 characters
            same(wide + " of this software", Scene.reflow(wide + "\nof this software"), "a cut line is joined to the next");
            same(wide + ".\nNext", Scene.reflow(wide + ".\nNext"), "a line that ends a sentence is not joined");
            same(wide + ":\nNext", Scene.reflow(wide + ":\nNext"), "a line that ends in a colon is not joined");
            same("Short heading\nNext", Scene.reflow("Short heading\nNext"), "a short line is not joined");
            same(wide + "\n\nNext", Scene.reflow(wide + "\n\nNext"), "an empty line keeps the paragraph apart");
            same(wide + " indented", Scene.reflow(wide + "\n   indented"), "the indent of a joined line goes");
            String notices = AboutScreen.licenceNotices();
            same(Arrays.asList(notices.trim().split("\\s+")), Arrays.asList(Scene.reflow(notices).trim().split("\\s+")), "the licence notices keep every word in order");
            check(Scene.reflow(notices).split("\n").length < notices.split("\n").length, "the licence notices have lines to join");
        });

        test("title case: every word of a label starts with a capital, also after a hyphen; the rest is left alone", () -> {
            same("Frame-Rate Counter", Scene.titleCase("Frame-rate counter"), "after a hyphen too");
            same("FSR Sharpness", Scene.titleCase("FSR sharpness"), "capitals stay capitals");
            same("Save And Restore", Scene.titleCase("save and restore"), "short words too, as the game writes its labels");
            same("", Scene.titleCase(""), "no text");
        });

        test("touch: a point finds the rectangle added last; outside every rectangle there is none", () -> {
            Scene scene = new Scene();
            scene.hit(0, 0, 100, 100, 1);
            scene.hit(50, 50, 100, 100, 2);
            same(1, scene.hitAt(10, 10), "only the first");
            same(2, scene.hitAt(60, 60), "both: the later one is on top");
            same(2, scene.hitAt(149, 149), "the far corner belongs");
            same(-1, scene.hitAt(150, 150), "the edge itself is outside");
            same(-1, scene.hitAt(120, 10), "outside both");
        });

        test("compact frame (the Thor's lower-screen trainer): strip, body and prompts inside the panel, in that order; the touch unit makes rows 4.4 mm", () -> {
            for (int[] size : new int[][] {{969, 844}, {800, 700}, {1240, 1080}}) {
                MenuLayout.Frame f = MenuLayout.Frame.compact(size[0], size[1]);
                String what = size[0] + " x " + size[1];
                check(!f.header, what + ": no header");
                check(f.left > 0 && f.right < f.w && f.left + f.right == f.w, what + ": the block centred with a margin");
                check(f.stripTop - MenuLayout.TAB_LINE >= 0, what + ": the accent line inside the panel");
                check(f.stripTop + MenuLayout.STRIP_H < f.bodyTop, what + ": the body under the strip");
                check(f.bodyBottom < f.promptTop, what + ": the body ends above the prompts");
                // (the buttons stand on the screen's own button row, Quick Save's column: within the margin of the panel's right edge)
                check(f.promptTop + MenuLayout.TOUCH_BUTTON < f.h && f.promptRight <= f.w && f.w - f.promptRight <= 2 * f.left,
                        what + ": the buttons inside the panel, at its right edge");
                check(f.bodySlots >= 6, what + ": room for a useful list (" + f.bodySlots + " slots)");
                same(f.slotTop(0), f.bodyTop, what + ": the slots start the body");
                same(f.right, f.paneLeft(false) + f.paneWidth(false), what + ": without a column the pane takes the block");
            }
            // The Thor lower screen's X and check buttons (owner 2026-10-25): in the bottom right corner, the check at the right, under the body.
            MenuLayout.Frame lower = MenuLayout.Frame.compact(969, 844);
            float[][] buttons = lower.touchButtons();
            check(lower.touchButtons, "the lower screen's frame has the touch buttons");
            same((float) lower.promptRight, buttons[1][2], "the check ends at the block's right edge");
            same((float) MenuLayout.TOUCH_BUTTON_GAP, buttons[1][0] - buttons[0][2], "the X stands left of it, the gap between");
            for (float[] b : buttons) {
                same((float) MenuLayout.TOUCH_BUTTON, b[2] - b[0], "a square: as wide");
                same((float) MenuLayout.TOUCH_BUTTON, b[3] - b[1], "as high");
                check(b[3] <= lower.h && b[1] > lower.bodyBottom, "inside the panel, under the body");
            }
            float u = MenuLayout.touchUnit(419f);
            check(Math.abs(MenuLayout.TOUCH_BUTTON * u - 88f) <= 1.5f && Math.abs(MenuLayout.TOUCH_BUTTON_GAP * u - 16f) <= 1.5f
                    && Math.abs(MenuLayout.TOUCH_BUTTON_RADIUS * u - 10f) <= 1.5f, "on the lower screen: the Quick Save button's 88 px, 16 px apart, radius 10");
            check(!MenuLayout.DEFAULT.touchButtons, "the main screens keep the prompt strip");
            // Without a gamepad on a touch screen (owner 2026-10-25) the main frame's X and check stand under the body, the check
            // ending where the strip ends, a little lower than the strip.
            float[][] icons = MenuLayout.DEFAULT.touchButtons();
            same((float) MenuLayout.PROMPT_RIGHT, icons[1][2], "the check ends at the strip's right edge");
            check(icons[0][1] > MenuLayout.BODY_BOTTOM && icons[1][3] < MenuLayout.H, "the icons under the body, inside the screen");
            // The launcher full screen on a screen taller than 16:9 (main, the owner's "go ahead"): the Fold's 2176 x 1812.
            float fu = Math.min(2176 / 1920f, 1812 / 1080f);
            int tall = MenuLayout.panelHeight(2176, 1812, fu);
            same(1599, tall, "the Fold: a panel 1920 x 1599");
            same(MenuLayout.H, MenuLayout.panelHeight(1920, 1080, 1f), "a 16:9 screen (the Thor): the panel as before");
            same(MenuLayout.H, MenuLayout.panelHeight(2400, 1080, 1f), "a wider phone: as before");
            same(MenuLayout.DEFAULT, MenuLayout.Frame.screen(MenuLayout.H), "1080 high: DEFAULT itself");
            MenuLayout.Frame fold = MenuLayout.Frame.screen(tall);
            same(tall, fold.h, "the frame spans the panel's height");
            same(MenuLayout.W, fold.w, "and its width");
            check(fold.header && fold.stripTop == MenuLayout.STRIP_TOP && fold.bodyTop == MenuLayout.BODY_TOP, "the header, strip and body start as on 16:9");
            same(tall - (MenuLayout.H - MenuLayout.PROMPT_BOTTOM), fold.promptTop + MenuLayout.PROMPT_H, "the strip as far above the screen's bottom as on 16:9");
            float[][] foldIcons = fold.touchButtons();
            check(foldIcons[1][3] < tall && foldIcons[0][1] > fold.bodyBottom && fold.promptTop > fold.bodyBottom, "the icons and the strip under the body, inside");
            check(fold.bodySlots > MenuLayout.BODY_SLOTS, "more body slots in the taller frame (" + fold.bodySlots + ")");
            same(MenuLayout.BODY_SLOTS, (MenuLayout.PROMPT_BOTTOM + MenuLayout.ICON_DROP - MenuLayout.TOUCH_BUTTON - MenuLayout.TOUCH_BUTTON_PAD
                    - MenuLayout.BODY_TOP + MenuLayout.ROW_GAP) / MenuLayout.ROW_PITCH, "(the same rule gives DEFAULT's 12 slots)");
            // The owner ("just move it up bro"): off the lower HUD's black bottom bar, on the screen's own button row - the check on Quick
            // Save's square (right 32, bottom 112 px of the 1240 x 1080 screen), the X on the trainer button's, all over the map.
            MenuLayout.Frame screen = MenuLayout.Frame.compact(Math.round(1240 / u), Math.round(1080 / u), u);
            float[][] on = screen.touchButtons();
            float left = (1240 - screen.w * u) / 2f, top = (1080 - screen.h * u) / 2f;
            float checkRight = left + on[1][2] * u, checkBottom = top + on[1][3] * u, xRight = left + on[0][2] * u;
            check(Math.abs(checkRight - (1240 - 32)) <= 1.5f && Math.abs(checkBottom - (1080 - 112)) <= 1.5f,
                    "the check on Quick Save's square: right edge " + checkRight + ", bottom " + checkBottom);
            check(Math.abs((1240 - 32 - 88 - 16) - xRight) <= 2f, "the X on the trainer button's square, 16 px to the left: right edge " + xRight);
            for (float[] b : on) check(top + b[3] * u <= 1080 - MenuLayout.LOWER_HUD_BAR_1080, "above the HUD's black bottom bar (it starts at y 985)");
            // 419 dots per inch (the Thor's 3.92-inch 1240 x 1080 lower screen): a row pitch of 4.4 mm.
            float unit = MenuLayout.touchUnit(419f), pitchMm = MenuLayout.ROW_PITCH * unit * 25.4f / 419f;
            check(Math.abs(pitchMm - 4.4f) < 0.05f, "a row pitch of 4.4 mm on the lower screen (" + pitchMm + ")");
            same(1f, MenuLayout.touchUnit(0f), "an unknown density: the panel's own size");
            check(MenuLayout.touchUnit(10000f) >= 0.5f && MenuLayout.touchUnit(1f) <= 3f, "a wrong density cannot make it absurd");
        });

        end();
    }

    private static List<Integer> list(int[] values) {
        return Arrays.asList(values[0], values[1], values[2], values[3]);
    }
}
