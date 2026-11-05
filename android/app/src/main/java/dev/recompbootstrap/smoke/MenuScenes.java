package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.MenuLayout.*;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Draws one menu frame into a Scene from a plain description of what the frame shows (Page). Pure Java: the app and
 * the host tests both call build().
 *
 * The frame (local-analysis/menu-redesign/DESIGN.md) is the game's pause menu: one block over the backdrop; the header
 * plate with the title at its left and three small lines in capitals and a portrait box at its right; the tab strip;
 * under it a column of items at the left and the pane at the right - an info block, the controller diagram, the rows,
 * the row that says "there are more", the help strip - or a text. The button prompts stand in one strip at the
 * frame's bottom right, a status line at its bottom left. What is selected on the way to the highlight is a light bar
 * with black text: the tab, the column item, the row. An alert takes the whole screen. A compact frame
 * (MenuLayout.Frame.compact: the Thor's lower screen) has no header.
 */
final class MenuScenes {
    private MenuScenes() { }

    static final int TONE_NORMAL = 0, TONE_WARNING = 1, TONE_ERROR = 2, TONE_GOOD = 3;
    /** Marks at the left edge of a column item: none, something in it was changed, something in it needs attention. */
    static final int MARK_NONE = 0, MARK_CHANGED = 1, MARK_WARNING = 2;
    /** The alert's heading as the game writes it: one lower-case word. */
    static final String ALERT_HEADING = "alert";
    /** The heading of the controller diagram (DESIGN.md 5.7). */
    static final String DIAGRAM_HEADING = "ON FOOT";
    private static final int LEFTWARD = POINT_LEFT, RIGHTWARD = POINT_RIGHT, UPWARD = POINT_UP, DOWNWARD = POINT_DOWN;

    static final class Line {
        final String text;
        final int tone;

        Line(String text, int tone) {
            this.text = text;
            this.tone = tone;
        }
    }

    static final class Row {
        String label, value;
        /** The value can be stepped: on the highlighted row it stands between a left and a right arrow. */
        boolean arrows, canLeft, canRight;
        /** Touch: the left and right half of the highlighted row step the value; without, a tap on it is A. */
        boolean halves;
        boolean enabled = true, marked;
        /** The setting applies the next time the game starts: a small "RESTART" before the value (the in-game menu). */
        boolean restart;
        /** Slider position 0..1; negative for a row without a bar. */
        float fraction = -1f;
        int valueTone;
    }

    static final class Prompt {
        final String label, glyph;
        final int command;

        Prompt(String label, String glyph, int command) {
            this.label = label;
            this.glyph = glyph;
            this.command = command;
        }
    }

    static final class Page {
        String title = "";
        /** The header plate with the title, the small lines and the portrait box; false in a compact frame. */
        boolean header = true;
        /** The portrait box at the header's right end (our own head-and-shoulders mark). */
        boolean portrait = true;
        /** The accent: the selected tab's line, the sliders, the progress bar (MenuLayout.ACCENT_*). */
        int accent = ACCENT;
        /** The controller diagram at the top of the pane (the Controller tab), DIAGRAM_SLOTS slots high. */
        boolean diagram;
        /** The backdrop picture under the frame; false: the screen under it only dimmed (the Thor's lower screen over its HUD). */
        boolean backdrop = true;
        /**
         * The prompts drawn as the X and check icons instead of the strip: a touch screen without a gamepad (owner 2026-10-25: "only
         * on touch screen devices, and only when a gamepad isn't connected"). With a gamepad the strip names the buttons.
         */
        boolean touchIcons;
        /** The small lines at the top right, drawn in capitals; at most MenuLayout.STATUS_LINES. */
        final List<Line> status = new ArrayList<Line>();
        String[] tabs = new String[0];
        int tab;
        /** False for a page outside the tabs (first-run setup): its one heading fills the strip, no arrows at its ends. */
        boolean shoulders = true;

        /** null: no column, the pane takes the whole block. */
        String[] column;
        boolean[] columnEnabled;
        int[] columnMarks;
        int columnSelected;
        /** The selected column item is a light bar: the highlight is on it or further in, on a row of its pane. */
        boolean columnLit;

        /** Notices at the top of the pane, each wrapped to as many lines as it needs. */
        final List<Line> info = new ArrayList<Line>();
        /** 0..1 draws a thin bar at the right of the info block (work in progress); negative draws none. */
        float infoBar = -1f;
        final List<Row> rows = new ArrayList<Row>();
        int rowFocused = -1, rowFirst;
        /** The help strip under the rows: what the highlighted row is and why it cannot be used. */
        final List<Line> help = new ArrayList<Line>();

        /** Not null: the pane shows these lines (already wrapped) from line textScroll on, instead of rows. */
        List<String> text;
        int textScroll;
        boolean textFocused;

        final List<Prompt> prompts = new ArrayList<Prompt>();
        Line footerStatus;
        ConfirmOverlay alert;
    }

    static int color(int tone, int normal) {
        switch (tone) {
            case TONE_WARNING: return WARNING;
            case TONE_ERROR: return ERROR;
            case TONE_GOOD: return ACCENT_FRANKLIN;
            default: return normal;
        }
    }

    /** Wraps a text for the pane's text view. */
    static List<String> paneText(String text, boolean column, Scene.Measure measure) {
        return Scene.wrap(text, ROW_PX, Scene.REGULAR, paneWidth(column) - 2 * ROW_PAD, measure);
    }

    static Scene build(Page page, Scene.Measure measure) {
        return build(page, measure, DEFAULT);
    }

    static Scene build(Page page, Scene.Measure measure, Frame f) {
        Scene scene = new Scene();
        scene.panelW = f.w;
        scene.panelH = f.h;
        if (page.alert != null) {
            scene.layer(Scene.ALERT);
            alert(scene, page.alert, measure);
            prompts(scene, page, measure, f);
            scene.contentKey = "alert";
            return scene;
        }
        if (page.backdrop) scene.backdrop(BACKDROP_TOP, BACKDROP_BOTTOM);
        else scene.fill(DIM_OVER_HUD);
        if (page.header && f.header) header(scene, page, measure);
        strip(scene, page, measure, f);
        boolean column = page.column != null;
        if (column) column(scene, page, measure, f);
        if (page.text != null) text(scene, page, column, f);
        else pane(scene, page, column, measure, f);
        scene.layer(Scene.CHROME);
        float promptsLeft = prompts(scene, page, measure, f);
        status(scene, page, promptsLeft, measure, f);
        scene.contentKey = page.tab + "/" + page.columnSelected + "/" + (page.text != null);
        return scene;
    }

    // ------------------------------------------------------------------------------------------ header, strip

    /**
     * The console pause menu's header: a dark plate over the block's width; the title centred on it at the left; at the right the
     * small lines in capitals (the last one lowest, whatever their number) and the portrait box with our own head-and-shoulders mark.
     * The title and the small lines carry the soft shadow.
     */
    private static void header(Scene scene, Page page, Scene.Measure measure) {
        scene.rect(LEFT, PLATE_TOP, BLOCK_W, PLATE_H, PLATE);
        int statusRight = page.portrait ? STATUS_RIGHT : RIGHT - ROW_PAD - 8;
        float titleWidth = statusRight - STATUS_W - TITLE_LEFT;
        scene.text(Scene.fit(page.title, TITLE_PX, Scene.TITLE, titleWidth, measure), TITLE_LEFT, TITLE_TOP, titleWidth, TITLE_H,
                TITLE_PX, Scene.TITLE, Scene.LEFT, TEXT, true);
        int count = Math.min(page.status.size(), STATUS_LINES);
        float top = STATUS_TOP + (STATUS_LINES - count) * STATUS_PITCH;
        for (int i = 0; i < count; ++i, top += STATUS_PITCH) {
            Line line = page.status.get(i);
            scene.text(Scene.fit(line.text.toUpperCase(Locale.ROOT), CAPS_PX, Scene.CAPS, STATUS_W, measure), statusRight - STATUS_W,
                    top, STATUS_W, STATUS_LINE_H, CAPS_PX, Scene.CAPS, Scene.RIGHT, color(line.tone, TEXT), true);
        }
        if (page.portrait) portrait(scene, PORTRAIT_LEFT, PORTRAIT_TOP, PORTRAIT_W, PORTRAIT_H);
    }

    /** The portrait box: black, with a head (a disc) and shoulders (a rounded body cut by the box's lower edge), white. */
    static void portrait(Scene scene, float x, float y, float w, float h) {
        scene.rect(x, y, w, h, 0xD9000000);
        scene.disc(x + w / 2f, y + w * 0.36f, w * 0.19f, TEXT);
        float bodyTop = y + w * 0.60f;
        scene.round(x + w * 0.17f, bodyTop, w * 0.66f, y + h - bodyTop, w * 0.25f, TEXT);
        scene.rect(x + w * 0.17f, y + h - w * 0.12f, w * 0.66f, w * 0.12f, TEXT);
    }

    private static void strip(Scene scene, Page page, Scene.Measure measure, Frame f) {
        int count = page.tabs.length, stripTop = f.stripTop;
        int touchTop = stripTop - TAB_LINE - ROW_PAD, touchHeight = f.bodyTop - ROW_PAD - touchTop;
        for (int i = 0; i < count; ++i) {
            int left = f.tabLeft(i, count), width = f.tabWidth(i, count);
            boolean current = i == page.tab;
            scene.rect(left, stripTop, width, STRIP_H, current ? SELECT : PANEL);
            if (current) scene.rect(left, stripTop - TAB_LINE, width, TAB_LINE, page.accent);
            String name = page.tabs[i].toUpperCase(Locale.ROOT);
            scene.text(Scene.fit(name, TAB_PX, Scene.REGULAR, width - 2 * ROW_PAD, measure), left, stripTop, width,
                    STRIP_H, TAB_PX, Scene.REGULAR, Scene.CENTER, current ? SELECT_TEXT : TEXT);
            if (page.shoulders || count > 1) scene.hit(left, touchTop, width, touchHeight, HIT_TAB + i);
        }
        if (!page.shoulders || f.left < ARROW_OUT + ARROW_W) return;
        // The arrows outside the strip: white where there is a tab that way, faint at the ends (the console pause menu).
        int arrowTop = stripTop + (STRIP_H - ARROW_H) / 2 - ARROW_RISE;
        chevron(scene, f.left - ARROW_OUT - ARROW_W, arrowTop, ARROW_W, ARROW_H, ARROW_T, LEFTWARD, page.tab > 0 ? TEXT : ARROW_DIM);
        chevron(scene, f.right + ARROW_OUT, arrowTop, ARROW_W, ARROW_H, ARROW_T, RIGHTWARD, page.tab < count - 1 ? TEXT : ARROW_DIM);
        scene.hit(f.left - ARROW_TOUCH, touchTop, ARROW_TOUCH, touchHeight, HIT_PROMPT + NavInput.PAGE_PREV);
        scene.hit(f.right, touchTop, ARROW_TOUCH, touchHeight, HIT_PROMPT + NavInput.PAGE_NEXT);
    }

    /** An arrow head inside a box: a bent line of thickness t with square-cut ends, as the game's (MenuLayout.chevron). */
    private static void chevron(Scene scene, float x, float y, float w, float h, float t, int direction, int color) {
        scene.poly(color, MenuLayout.chevron(x, y, w, h, t, direction));
    }

    // ------------------------------------------------------------------------------------------ column

    private static void column(Scene scene, Page page, Scene.Measure measure, Frame f) {
        int textWidth = COLUMN_W - 2 * ROW_PAD, LEFT = f.left;
        for (int i = 0; i < page.column.length && i < f.bodySlots; ++i) {
            int top = f.slotTop(i);
            boolean lit = i == page.columnSelected && page.columnLit;
            boolean enabled = page.columnEnabled == null || page.columnEnabled[i];
            scene.rect(LEFT, top, COLUMN_W, ROW_H, lit ? SELECT : PANEL);
            int mark = page.columnMarks == null ? MARK_NONE : page.columnMarks[i];
            if (mark != MARK_NONE) scene.rect(LEFT, top, MARK_W, ROW_H, mark == MARK_WARNING ? WARNING : lit ? MARK_ON_SELECT : page.accent);
            scene.text(Scene.fit(page.column[i], ROW_PX, Scene.REGULAR, textWidth, measure), LEFT + ROW_PAD, top, textWidth,
                    ROW_H, ROW_PX, Scene.REGULAR, Scene.LEFT,
                    lit ? (enabled ? SELECT_TEXT : SELECT_TEXT_OFF) : enabled ? TEXT : TEXT_OFF);
            scene.hit(LEFT, top - ROW_GAP / 2f, COLUMN_W, ROW_PITCH, HIT_COLUMN + i);
        }
    }

    // ------------------------------------------------------------------------------------------ pane: info, rows, help

    /** A text wrapped into lines, each with the colour of the part it came from. */
    private static void wrapInto(List<Line> out, List<Line> parts, int size, float width, Scene.Measure measure) {
        for (Line part : parts) {
            for (String line : Scene.wrap(part.text, size, Scene.REGULAR, width, measure)) out.add(new Line(line, part.tone));
        }
    }

    /**
     * The first row the pane draws: the page's rowFirst moved only as far as needed to show the focused row, in the room the info
     * block, the diagram and the help strip leave (what pane() draws; the in-game menu keeps it between frames).
     */
    static int firstRow(Page page, Scene.Measure measure, Frame f) {
        boolean column = page.column != null;
        int width = f.paneWidth(column), textWidth = width - 2 * ROW_PAD;
        List<Line> info = new ArrayList<Line>();
        wrapInto(info, page.info, ROW_PX, page.infoBar >= 0f ? textWidth - BAR_W - BAR_GAP : textWidth, measure);
        int slot = infoSlots(info.size()) + (page.diagram ? DIAGRAM_SLOTS : 0);
        List<Line> help = new ArrayList<Line>();
        wrapInto(help, page.help, HELP_PX, textWidth, measure);
        int count = page.rows.size(), visible = f.visibleRows(count, slot, Math.min(help.size(), HELP_LINES));
        return windowStart(page.rowFirst, page.rowFocused, count, visible);
    }

    private static void pane(Scene scene, Page page, boolean column, Scene.Measure measure, Frame f) {
        int left = f.paneLeft(column), width = f.paneWidth(column), textWidth = width - 2 * ROW_PAD, BODY_TOP = f.bodyTop;
        boolean bar = page.infoBar >= 0f;

        // The info block: every line of every notice, never cut.
        List<Line> info = new ArrayList<Line>();
        wrapInto(info, page.info, ROW_PX, bar ? textWidth - BAR_W - BAR_GAP : textWidth, measure);
        int slot = infoSlots(info.size());
        if (slot > 0) {
            int height = slotsHeight(slot);
            float top = BODY_TOP + (height - info.size() * INFO_LINE) / 2f;
            scene.layer(Scene.CHROME);
            scene.rect(left, BODY_TOP, width, height, PANEL);
            scene.layer(Scene.CONTENT);
            for (int i = 0; i < info.size(); ++i) {
                scene.text(info.get(i).text, left + ROW_PAD, top + i * INFO_LINE, textWidth, INFO_LINE, ROW_PX, Scene.REGULAR,
                        Scene.LEFT, color(info.get(i).tone, TEXT));
            }
            if (bar) {
                int barLeft = left + width - VALUE_PAD - BAR_W, barTop = barTop(BODY_TOP, height);
                scene.rect(barLeft, barTop, BAR_W, BAR_H, track(page.accent));
                scene.rect(barLeft, barTop, barFill(page.infoBar), BAR_H, page.accent);
            }
        }
        if (page.diagram) {
            scene.layer(Scene.CHROME);
            diagram(scene, left, f.slotTop(slot), width, measure);
            slot += DIAGRAM_SLOTS;
        }

        List<Line> help = new ArrayList<Line>();
        wrapInto(help, page.help, HELP_PX, textWidth, measure);
        int helpLines = f.helpLinesShown(slot, Math.min(help.size(), HELP_LINES));

        // The rows: one dark block; the highlighted row is a light bar on the grid.
        int count = page.rows.size();
        if (count > 0) {
            int visible = f.visibleRows(count, slot, helpLines);
            int first = windowStart(page.rowFirst, page.rowFocused, count, visible), last = Math.min(count, first + visible);
            scene.layer(Scene.CHROME);
            scene.rect(left, f.slotTop(slot), width, slotsHeight(last - first + (visible < count ? 1 : 0)), PANEL);
            for (int index = first; index < last; ++index) {
                row(scene, page.rows.get(index), index, f.slotTop(slot + index - first), left, width, index == page.rowFocused,
                        page.accent, measure);
            }
            slot += last - first;
            if (visible < count) {
                scrollRow(scene, left, f.slotTop(slot), width, false, first > 0, last < count);
                slot++;
            }
        }

        if (helpLines > 0) {
            int top = f.slotTop(slot);
            scene.layer(Scene.CHROME);
            scene.rect(left, top, width, helpHeight(helpLines), PANEL);
            scene.layer(Scene.CONTENT);
            for (int i = 0; i < helpLines; ++i) {
                String line = help.get(i).text;
                // More text than the strip holds: the last line says so. (No row's own help is that long; a test checks it.)
                if (i == helpLines - 1 && help.size() > helpLines) {
                    line = Scene.fit(line + " " + Scene.ELLIPSIS + Scene.ELLIPSIS, HELP_PX, Scene.REGULAR, textWidth, measure);
                }
                scene.text(line, left + ROW_PAD, top + (ROW_H - HELP_LINE) / 2f + i * HELP_LINE, textWidth, HELP_LINE, HELP_PX,
                        Scene.REGULAR, Scene.LEFT, color(help.get(i).tone, TEXT));
            }
        }
    }

    private static void row(Scene scene, Row row, int index, int top, int left, int width, boolean focused, int accent,
            Scene.Measure measure) {
        int ink = focused ? (row.enabled ? SELECT_TEXT : SELECT_TEXT_OFF) : row.enabled ? TEXT : TEXT_OFF;
        // The bars stand still; only what is written on them fades in after another page came up.
        scene.layer(Scene.CHROME);
        if (focused) scene.rect(left, top, width, ROW_H, SELECT);
        if (row.marked) scene.rect(left, top, MARK_W, ROW_H, focused ? MARK_ON_SELECT : accent);
        scene.hit(left, top - ROW_GAP / 2f, width, ROW_PITCH, HIT_ROW + index);
        scene.layer(Scene.CONTENT);
        float labelLeft = left + ROW_PAD, right = left + width - VALUE_PAD, labelRight = right;
        boolean slider = row.fraction >= 0f;
        if (row.value != null && slider) {
            int barLeft = left + width - VALUE_PAD - BAR_W, barTop = barTop(top, ROW_H);
            if (focused) {
                // On the light bar the track is dark, taller than the bar and the bar ends a little inside it, so that
                // a full bar still has an edge. It takes the bar's own place: it ends at the value's padding.
                scene.rect(barLeft, barTop - BAR_FRAME, BAR_W, BAR_H + 2 * BAR_FRAME, TRACK_ON_SELECT);
                scene.rect(barLeft + BAR_FRAME, barTop, barFill(row.fraction, BAR_W - 2 * BAR_FRAME), BAR_H, row.enabled ? accent : TEXT_OFF);
            } else {
                scene.rect(barLeft, barTop, BAR_W, BAR_H, row.enabled ? track(accent) : TRACK_OFF);
                scene.rect(barLeft, barTop, barFill(row.fraction), BAR_H, row.enabled ? accent : TEXT_OFF);
            }
            right = barLeft - BAR_GAP;
            labelRight = right - 2 * ROW_PAD;
        }
        // A slider says its value with its bar alone, as in the game. Its number is written before the bar only on
        // the highlighted row, where it is being set.
        if (row.value != null && (!slider || focused)) {
            boolean arrows = focused && row.arrows && row.enabled;
            int chevronTop = top + (ROW_H - CHEVRON_H) / 2;   // a whole pixel: the heads' tips are sharp
            if (arrows) {
                chevron(scene, right - CHEVRON_W, chevronTop, CHEVRON_W, CHEVRON_H, CHEVRON_T, RIGHTWARD, row.canRight ? SELECT_TEXT : 0x33000000);
                right -= CHEVRON_W + CHEVRON_GAP;
            }
            // The label keeps what it needs (at most 45 % of the row); the value takes the rest.
            float labelWidth = Math.min(measure.width(row.label, ROW_PX, Scene.REGULAR), width * 0.45f);
            float room = right - (labelLeft + labelWidth + 2 * ROW_PAD) - (arrows ? CHEVRON_W + CHEVRON_GAP : 0);
            String value = Scene.fit(row.value, ROW_PX, Scene.REGULAR, room, measure);
            float valueWidth = measure.width(value, ROW_PX, Scene.REGULAR);
            scene.text(value, right - valueWidth, top, valueWidth, ROW_H, ROW_PX, Scene.REGULAR, Scene.RIGHT,
                    focused ? ink : color(row.valueTone, ink));
            labelRight = right - valueWidth - 2 * ROW_PAD;
            if (arrows) {
                chevron(scene, right - valueWidth - CHEVRON_GAP - CHEVRON_W, chevronTop, CHEVRON_W, CHEVRON_H, CHEVRON_T, LEFTWARD,
                        row.canLeft ? SELECT_TEXT : 0x33000000);
                labelRight -= CHEVRON_W + CHEVRON_GAP;
            }
        }
        if (row.restart && row.value != null) {
            // A setting that applies at the next start: "RESTART" small and dim before the value.
            float w = measure.width(RESTART, RESTART_PX, Scene.REGULAR);
            float x = labelRight + ROW_PAD - w;
            if (x > labelLeft + 4 * ROW_PAD) {
                scene.text(RESTART, x, top, w, ROW_H, RESTART_PX, Scene.REGULAR, Scene.LEFT, focused ? SELECT_TEXT_OFF : TEXT_DIM);
                labelRight = x - 2 * ROW_PAD;
            }
        }
        scene.text(Scene.fit(row.label, ROW_PX, Scene.REGULAR, labelRight - labelLeft, measure), labelLeft, top,
                labelRight - labelLeft, ROW_H, ROW_PX, Scene.REGULAR, Scene.LEFT, ink);
        if (focused && row.halves) {
            scene.hit(left, top - ROW_GAP / 2f, width / 2f, ROW_PITCH, HIT_ARROW_LEFT + index);
            scene.hit(left + width / 2f, top - ROW_GAP / 2f, width / 2f, ROW_PITCH, HIT_ARROW_RIGHT + index);
        }
    }

    /** The row that says "there is more above or below": an up and a down mark, centred. Light when the highlight is on it. */
    private static void scrollRow(Scene scene, int left, int top, int width, boolean lit, boolean up, boolean down) {
        scene.layer(Scene.CHROME);
        scene.rect(left, top, width, ROW_H, lit ? SELECT : PANEL);
        int ink = lit ? SELECT_TEXT : TEXT, faint = lit ? 0x33000000 : 0x40F0F0F0;
        int x = left + (width - SCROLL_W) / 2, y = top + (ROW_H - 2 * SCROLL_H - SCROLL_GAP) / 2;
        chevron(scene, x, y, SCROLL_W, SCROLL_H, SCROLL_T, UPWARD, up ? ink : faint);
        chevron(scene, x, y + SCROLL_H + SCROLL_GAP, SCROLL_W, SCROLL_H, SCROLL_T, DOWNWARD, down ? ink : faint);
    }

    /** The mark before the value of a setting that applies at the next start. */
    static final String RESTART = "RESTART";

    // ------------------------------------------------------------------------------------------ controller diagram

    /**
     * The controller diagram (DESIGN.md 5.7): a dark block DIAGRAM_SLOTS slots high, "ON FOOT" at its top, our own outline drawing of
     * a 360-style pad in its middle and the nine on-foot controls at each side, each joined to its button by a right-angled line.
     * Every number is in game pixels (g) from the pad's centre, times K.
     */
    private static void diagram(Scene scene, int left, int top, int width, Scene.Measure measure) {
        scene.rect(left, top, width, slotsHeight(DIAGRAM_SLOTS), PANEL);
        scene.text(DIAGRAM_HEADING, left, top + 8, width, ROW_H - 16, ROW_PX, Scene.REGULAR, Scene.CENTER, TEXT);
        float cx = left + width / 2f, cy = top + 165 * K, t = 2.4f * K;
        for (float[] path : Pad.OUTLINE) scene.line(TEXT, t, place(path, cx, cy));
        for (float[] ring : Pad.RINGS) scene.ring(cx + ring[0] * K, cy + ring[1] * K, ring[2] * K, t, TEXT);
        scene.line(TEXT, 3 * K, place(new float[] {-8, -30, 8, -14}, cx, cy));
        scene.line(TEXT, 3 * K, place(new float[] {8, -30, -8, -14}, cx, cy));
        String[] faces = {"Y", "X", "B", "A"};
        for (int i = 0; i < 4; ++i) mark(scene, faces[i], cx + Pad.FACES[2 * i] * K, cy + Pad.FACES[2 * i + 1] * K, 19 * K);
        int lead = 0x9FF0F0F0;
        for (int side = 0; side < 2; ++side) {
            float sign = side == 0 ? -1 : 1;
            for (int i = 0; i < Pad.LABELS[side].length; ++i) {
                float y = top + (52 + 30 * i) * K, labelX = cx + sign * 185 * K;
                float w = measure.width(Pad.LABELS[side][i], DIAGRAM_PX, Scene.REGULAR);
                scene.text(Pad.LABELS[side][i], side == 0 ? labelX - w : labelX, y - 15, w, 30, DIAGRAM_PX, Scene.REGULAR,
                        side == 0 ? Scene.RIGHT : Scene.LEFT, TEXT);
                float tx = cx + Pad.TARGETS[side][2 * i] * K, ty = cy + Pad.TARGETS[side][2 * i + 1] * K, sx = labelX - sign * 7 * K;
                if (Math.abs(ty - y) < 3 * K) scene.line(lead, 1.4f * K, sx, y, tx, y);
                else scene.line(lead, 1.4f * K, sx, y, tx, y, tx, ty);
            }
        }
    }

    private static float[] place(float[] g, float cx, float cy) {
        float[] out = new float[g.length];
        for (int i = 0; i < g.length; i += 2) {
            out[i] = cx + g[i] * K;
            out[i + 1] = cy + g[i + 1] * K;
        }
        return out;
    }

    /** The pad's outline drawing in g from its centre: the body (flattened curves), bumpers and triggers, sticks, D-pad, guide. */
    static final class Pad {
        private Pad() { }

        static final float[][] OUTLINE;
        /** Circles: x, y, radius. */
        static final float[][] RINGS = {{-92, -12, 22}, {-92, -12, 13}, {48, 30, 22}, {48, 30, 13}, {0, -22, 15}, {-30, -22, 6}, {30, -22, 6}};
        /** The face buttons' centres: Y, X, B, A. */
        static final float[] FACES = {92, -36, 72, -16, 112, -16, 92, 4};
        static final String[][] LABELS = {
                {"TARGET LOCK ON", "WEAPON WHEEL", "SWITCH CAMERA", "MOVE", "STEALTH MODE", "USE PHONE", "PHONE LEFT", "CHARACTER SWITCH", "PHONE RIGHT"},
                {"FIRE WEAPON", "TAKE COVER", "PAUSE MENU", "ENTER VEHICLE", "RELOAD", "SPRINT", "JUMP", "LOOK BEHIND", "LOOK"}};
        /** Where each label's line ends: the part it names. */
        static final float[][] TARGETS = {
                {-97, -98, -88, -67, -30, -28, -114, -12, -92, -12, -46, 10, -66, 30, -46, 50, -26, 30},
                {97, -98, 88, -67, 30, -28, 92, -45, 121, -16, 92, 13, 63, -16, 48, 30, 70, 30}};

        static {
            List<float[]> paths = new ArrayList<float[]>();
            // The body: one closed curve of cubic segments (start, control 1, control 2, end), flattened to lines.
            float[][] body = {
                    {-60, -62, -85, -66, -108, -62, -118, -50}, {-118, -50, -138, -30, -152, 10, -150, 40},
                    {-150, 40, -150, 75, -138, 95, -118, 92}, {-118, 92, -95, 88, -80, 58, -62, 52},
                    {-62, 52, -30, 45, 30, 45, 62, 52}, {62, 52, 80, 58, 95, 88, 118, 92},
                    {118, 92, 138, 95, 150, 75, 150, 40}, {150, 40, 152, 10, 138, -30, 118, -50},
                    {118, -50, 108, -62, 85, -66, 60, -62}, {60, -62, 30, -55, -30, -55, -60, -62}};
            List<Float> outline = new ArrayList<Float>();
            for (float[] c : body) {
                for (int step = outline.isEmpty() ? 0 : 1; step <= 12; ++step) {
                    float u = step / 12f, v = 1 - u;
                    outline.add(v * v * v * c[0] + 3 * v * v * u * c[2] + 3 * v * u * u * c[4] + u * u * u * c[6]);
                    outline.add(v * v * v * c[1] + 3 * v * v * u * c[3] + 3 * v * u * u * c[5] + u * u * u * c[7]);
                }
            }
            float[] o = new float[outline.size()];
            for (int i = 0; i < o.length; ++i) o[i] = outline.get(i);
            paths.add(o);
            for (int side = -1; side <= 1; side += 2) {
                float x0 = side < 0 ? -112 : 82;
                paths.add(new float[] {x0 + 7, -98, x0 + 23, -98, x0 + 30, -91, x0 + 30, -75, x0 + 23, -68, x0 + 7, -68, x0, -75, x0, -91, x0 + 7, -98});
                paths.add(new float[] {side * 64, -70, side * 112, -64});
            }
            float a = 7, b = 20, dx = -46, dy = 30;
            paths.add(new float[] {dx - a, dy - b, dx + a, dy - b, dx + a, dy - a, dx + b, dy - a, dx + b, dy + a, dx + a, dy + a,
                    dx + a, dy + b, dx - a, dy + b, dx - a, dy + a, dx - b, dy + a, dx - b, dy - a, dx - a, dy - a, dx - a, dy - b});
            OUTLINE = paths.toArray(new float[0][]);
        }
    }

    // ------------------------------------------------------------------------------------------ text

    private static void text(Scene scene, Page page, boolean column, Frame f) {
        int left = f.paneLeft(column), width = f.paneWidth(column), total = page.text.size(), visible = f.textLinesVisible();
        int BODY_TOP = f.bodyTop, BODY_SLOTS = f.bodySlots, BODY_BOTTOM = f.bodyBottom;
        boolean more = total > visible;
        int shown = more ? visible : total, first = more ? clampScroll(page.textScroll, total, visible) : 0;
        // A text that fits takes whole slots and stands in their middle (one line reads like a row beside the
        // column's bars); a longer one fills the body down to the row with the marks and starts at its top.
        int height = more ? slotsHeight(BODY_SLOTS - 1) : slotsHeight(textSlots(shown));
        float top = more ? BODY_TOP + TEXT_PAD : BODY_TOP + (height - shown * TEXT_LINE) / 2f;
        scene.rect(left, BODY_TOP, width, height, PANEL);
        scene.layer(Scene.CONTENT);
        for (int i = 0; i < shown; ++i) {
            scene.text(page.text.get(first + i), left + ROW_PAD, top + i * TEXT_LINE, width - 2 * ROW_PAD,
                    TEXT_LINE, ROW_PX, Scene.REGULAR, Scene.LEFT, TEXT);
        }
        if (more) scrollRow(scene, left, f.slotTop(BODY_SLOTS - 1), width, page.textFocused, first > 0, first + visible < total);
        scene.hit(left, BODY_TOP, width, BODY_BOTTOM - BODY_TOP, HIT_TEXT);
    }

    // ------------------------------------------------------------------------------------------ prompts, status

    /** Width of a prompt's button mark: a disc for one letter, a small rounded box for a name. */
    static float markWidth(String glyph) {
        return glyph.length() == 1 ? MARK_D : START_W;
    }

    /** A button mark centred at (cx, cy), d across: our own disc in the button's colour with its letter (A, B, X, Y). */
    static void mark(Scene scene, String glyph, float cx, float cy, float d) {
        boolean a = glyph.equals("A"), b = glyph.equals("B"), x = glyph.equals("X");
        scene.disc(cx, cy, d / 2f, a ? MARK_A : b ? MARK_B : x ? MARK_X : MARK_Y);
        int size = Math.round(d * MARK_PX / MARK_D);
        scene.text(glyph, cx - d / 2f, cy - d / 2f, d, d, size, Scene.BOLD, Scene.CENTER, a ? MARK_A_INK : b ? MARK_B_INK : x ? MARK_X_INK : MARK_Y_INK);
    }

    /**
     * The prompts: one dark strip that ends in the bottom right corner of the screen; the last of the list is the
     * rightmost. Each is its label, then its button mark. Returns the strip's left edge (the screen's right edge
     * when there is no prompt).
     */
    /**
     * The two touch buttons in place of the prompt strip (MenuLayout.TOUCH_BUTTON): an X (B: back, then close) and a check (A: the
     * highlighted row) at the right, each a square in the Quick Save button's style with our own white glyph. The Thor's lower screen
     * (a frame of touch buttons) always has both; a touch screen without a gamepad (Page.touchIcons) has the X where the page's
     * prompts offer B and the check where they offer A, each in its own place (prompts of START and Y get none: their rows do the
     * same by touch). Their touch rectangles reach TOUCH_BUTTON_PAD beyond the squares. Returns the leftmost drawn square's left edge.
     */
    private static float touchButtons(Scene scene, Page page, Frame f) {
        float[][] boxes = f.touchButtons();
        int[] commands = {NavInput.BACK, NavInput.CONFIRM};
        float leftmost = f.promptRight;
        for (int i = 0; i < 2; ++i) {
            if (!f.touchButtons && !offers(page, commands[i])) continue;
            float[] b = boxes[i];
            leftmost = Math.min(leftmost, b[0]);
            float side = b[2] - b[0], t = side * TOUCH_GLYPH_STROKE, x = b[0], y = b[1];
            scene.round(x, y, side, side, TOUCH_BUTTON_RADIUS, TOUCH_BUTTON_FILL);
            if (i == 0) {
                float g = side * 0.29f;
                scene.line(TOUCH_BUTTON_GLYPH, t, x + g, y + g, x + side - g, y + side - g);
                scene.line(TOUCH_BUTTON_GLYPH, t, x + side - g, y + g, x + g, y + side - g);
            } else {
                scene.line(TOUCH_BUTTON_GLYPH, t, x + side * 0.22f, y + side * 0.53f, x + side * 0.42f, y + side * 0.72f,
                        x + side * 0.79f, y + side * 0.30f);
            }
            float p = TOUCH_BUTTON_PAD;
            scene.hit(b[0] - p, b[1] - p, side + 2 * p, side + 2 * p, HIT_PROMPT + commands[i]);
        }
        return leftmost;
    }

    /** True when one of the page's prompts is this command (NavInput.BACK or CONFIRM). */
    private static boolean offers(Page page, int command) {
        for (Prompt prompt : page.prompts) if (prompt.command == command) return true;
        return false;
    }

    private static float prompts(Scene scene, Page page, Scene.Measure measure, Frame f) {
        if (f.touchButtons || page.touchIcons) return touchButtons(scene, page, f);
        int count = page.prompts.size(), PROMPT_TOP = f.promptTop, W = f.promptRight;
        if (count == 0) return W;
        float total = PROMPT_PAD + PROMPT_END + (count - 1) * PROMPT_SPACE;
        for (Prompt prompt : page.prompts) total += measure.width(prompt.label, PROMPT_PX, Scene.REGULAR) + PROMPT_GAP + markWidth(prompt.glyph);
        float left = W - total, x = left + PROMPT_PAD, cy = PROMPT_TOP + PROMPT_H / 2f;
        scene.rect(left, PROMPT_TOP, total, PROMPT_H, PROMPT_STRIP);
        for (Prompt prompt : page.prompts) {
            float label = measure.width(prompt.label, PROMPT_PX, Scene.REGULAR), mark = markWidth(prompt.glyph);
            float markLeft = x + label + PROMPT_GAP;
            scene.text(prompt.label, x, PROMPT_TOP, label, PROMPT_H, PROMPT_PX, Scene.REGULAR, Scene.LEFT, TEXT);
            if (prompt.glyph.length() == 1) {
                mark(scene, prompt.glyph, markLeft + MARK_D / 2f, cy, MARK_D);
            } else {
                scene.round(markLeft, cy - START_H / 2f, mark, START_H, START_H / 2f, MARK_START);
                scene.text(prompt.glyph, markLeft, PROMPT_TOP, mark, PROMPT_H, START_PX, Scene.BOLD, Scene.CENTER, MARK_START_INK);
            }
            // Every prompt is a button of its own on the touch screen; its rectangle reaches a little above the strip.
            float hitLeft = Math.max(left, x - PROMPT_SPACE / 2f), hitRight = Math.min(W, markLeft + mark + PROMPT_SPACE / 2f);
            scene.hit(hitLeft, PROMPT_TOP - PROMPT_TOUCH_UP, hitRight - hitLeft, PROMPT_H + PROMPT_TOUCH_UP, HIT_PROMPT + prompt.command);
            x = markLeft + mark + PROMPT_SPACE;
        }
        return left;
    }

    /**
     * The status ("Saving...", "Saved.", why a save failed): a dark strip in the bottom left corner of the screen,
     * wrapped to as many lines as it needs in the room the prompts leave. It is never cut.
     */
    private static void status(Scene scene, Page page, float promptsLeft, Scene.Measure measure, Frame f) {
        if (page.footerStatus == null) return;
        float room = promptsLeft - PROMPT_SPACE - 2 * PROMPT_PAD - f.left, widest = 0;
        List<String> lines = Scene.wrap(page.footerStatus.text, PROMPT_PX, Scene.REGULAR, room, measure);
        for (String line : lines) widest = Math.max(widest, measure.width(line, PROMPT_PX, Scene.REGULAR));
        // (its bottom level with the strip's; with the icons of a touch screen without a gamepad, with theirs)
        float bottom = page.touchIcons && !f.touchButtons ? f.iconTop + TOUCH_BUTTON : f.promptTop + PROMPT_H;
        float height = PROMPT_H + (lines.size() - 1) * HELP_LINE, top = bottom - height;
        scene.rect(f.left, top, widest + 2 * PROMPT_PAD, height, PROMPT_STRIP);
        for (int i = 0; i < lines.size(); ++i) {
            scene.text(lines.get(i), f.left + PROMPT_PAD, top + (PROMPT_H - HELP_LINE) / 2f + i * HELP_LINE, room, HELP_LINE, PROMPT_PX,
                    Scene.REGULAR, Scene.LEFT, color(page.footerStatus.tone, TEXT));
        }
    }

    /**
     * Button prompts from a legend line such as "A Select   B Back   Left/Right Choose" (the first-run setup's
     * model writes its footer that way): parts are separated by two or more spaces, the first word of a part is the
     * button, the rest the action. A and B become prompts, A the rightmost; directions are not named.
     */
    static List<Prompt> legendPrompts(String legend) {
        List<Prompt> prompts = new ArrayList<Prompt>();
        if (legend == null) return prompts;
        for (String part : legend.trim().split(" {2,}")) {
            int space = part.indexOf(' ');
            if (space <= 0) continue;
            String button = part.substring(0, space), action = Scene.titleCase(part.substring(space + 1).trim());
            if (button.equals("A")) prompts.add(new Prompt(action, "A", NavInput.CONFIRM));
            else if (button.equals("B")) prompts.add(0, new Prompt(action, "B", NavInput.BACK));
        }
        return prompts;
    }

    // ------------------------------------------------------------------------------------------ alert

    private static List<String> alertWrap(String text, Scene.Measure measure) {
        return text == null ? new ArrayList<String>() : Scene.wrap(text, ALERT_PX, Scene.REGULAR, ALERT_W - 4 * SCROLL_W, measure);
    }

    /** How many lines an alert's detail text takes, for scrolling a long list. */
    static int alertDetailLines(String detail, Scene.Measure measure) {
        return alertWrap(detail, measure).size();
    }

    /**
     * A question over the whole screen, as the game's alert: black, the heading in our heaviest weight in yellow, the question (and
     * its detail, of which a long list shows a window) in white between two short thin rules, the answers as a line of cells of which
     * one is highlighted. The menu is not drawn under it, so nothing else can be tapped.
     */
    private static void alert(Scene scene, ConfirmOverlay alert, Scene.Measure measure) {
        scene.fill(ALERT_BLACK);
        List<String> title = alertWrap(alert.title, measure), detail = alertWrap(alert.detail, measure);
        int shown = Math.min(detail.size(), ALERT_LINES), first = clampScroll(alert.scroll, detail.size(), shown);
        int ruleLeft = (W - ALERT_W) / 2, shortRule = (W - ALERT_RULE_W) / 2, y = alertTop(title.size(), detail.size());
        scene.text(alert.heading != null && !alert.heading.isEmpty() ? alert.heading : ALERT_HEADING, 0, y, W, ALERT_TITLE_H,
                ALERT_TITLE_PX, Scene.HEAVY, Scene.CENTER, ALERT_YELLOW);
        y += ALERT_TITLE_H + ALERT_SPACE;
        scene.rect(shortRule, y, ALERT_RULE_W, ALERT_RULE, TEXT);
        y += ALERT_RULE + ALERT_SPACE;
        for (String line : title) {
            scene.text(line, ruleLeft, y, ALERT_W, ALERT_LINE, ALERT_PX, Scene.REGULAR, Scene.CENTER, TEXT);
            y += ALERT_LINE;
        }
        if (detail.size() > shown) {
            int x = ruleLeft + ALERT_W - SCROLL_W, markTop = y + (shown * ALERT_LINE - 2 * SCROLL_H - SCROLL_GAP) / 2;
            chevron(scene, x, markTop, SCROLL_W, SCROLL_H, SCROLL_T, UPWARD, first > 0 ? TEXT : 0x40F0F0F0);
            chevron(scene, x, markTop + SCROLL_H + SCROLL_GAP, SCROLL_W, SCROLL_H, SCROLL_T, DOWNWARD, first + shown < detail.size() ? TEXT : 0x40F0F0F0);
        }
        for (int i = 0; i < shown; ++i) {
            scene.text(detail.get(first + i), ruleLeft, y, ALERT_W, ALERT_LINE, ALERT_PX, Scene.REGULAR, Scene.CENTER, TEXT);
            y += ALERT_LINE;
        }
        y += ALERT_SPACE;
        scene.rect(shortRule, y, ALERT_RULE_W, ALERT_RULE, TEXT);
        y += ALERT_RULE + 2 * ALERT_SPACE;
        for (int i = 0; i < alert.buttons.length; ++i) {
            int x = choiceLeft(i, alert.buttons.length);
            boolean lit = i == alert.focused;
            scene.rect(x, y, CHOICE_W, CHOICE_H, lit ? SELECT : ALERT_CELL);
            if (lit) scene.rect(x, y - TAB_LINE, CHOICE_W, TAB_LINE, ACCENT_FRANKLIN);
            scene.text(Scene.fit(Scene.titleCase(alert.buttons[i]), ROW_PX, Scene.REGULAR, CHOICE_W - 2 * ROW_PAD, measure), x, y,
                    CHOICE_W, CHOICE_H, ROW_PX, Scene.REGULAR, Scene.CENTER, lit ? SELECT_TEXT : TEXT);
            scene.hit(x, y - CHOICE_TOUCH, CHOICE_W, CHOICE_H + 2 * CHOICE_TOUCH, HIT_ALERT_BUTTON + i);
        }
    }

    /**
     * The in-game menu's restart notice (DESIGN.md 5.8, the public build's drawing of the owner's alert, layout A): black, "alert"
     * in yellow, the message between two 150 x 3 rules at the measured places, "Continue" with A at the bottom right.
     */
    static Scene restartAlert(String message, Scene.Measure measure) {
        return restartAlert(message, measure, false);
    }

    /** The restart notice; touchIcons: its Continue as the check icon (a touch screen without a gamepad). */
    static Scene restartAlert(String message, Scene.Measure measure, boolean touchIcons) {
        Scene scene = new Scene();
        scene.layer(Scene.ALERT);
        scene.fill(ALERT_BLACK);
        scene.text(ALERT_HEADING, 0, RESTART_ALERT_TITLE_TOP, W, ALERT_TITLE_H, ALERT_TITLE_PX, Scene.HEAVY, Scene.CENTER, ALERT_YELLOW);
        int rule = (W - ALERT_RULE_W) / 2;
        scene.rect(rule, RESTART_ALERT_RULE1, ALERT_RULE_W, ALERT_RULE, TEXT);
        scene.text(Scene.fit(message, ALERT_PX, Scene.REGULAR, ALERT_W, measure), 0, RESTART_ALERT_TEXT_TOP, W, ALERT_LINE, ALERT_PX,
                Scene.REGULAR, Scene.CENTER, TEXT);
        scene.rect(rule, RESTART_ALERT_RULE2, ALERT_RULE_W, ALERT_RULE, TEXT);
        Page page = new Page();
        page.touchIcons = touchIcons;
        page.prompts.add(new Prompt("Continue", "A", NavInput.CONFIRM));
        prompts(scene, page, measure, new Frame(W, H, LEFT, BLOCK_W, false, STRIP_TOP, BODY_TOP, BODY_SLOTS, W - 98, H - 55));
        scene.hit(0, 0, W, H, HIT_ALERT_BUTTON);
        scene.contentKey = "restart-alert";
        return scene;
    }
}
