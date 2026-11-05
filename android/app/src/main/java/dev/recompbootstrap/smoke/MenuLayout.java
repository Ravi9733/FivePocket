package dev.recompbootstrap.smoke;

/**
 * The numbers of the menu's look (local-analysis/menu-redesign/DESIGN.md, 2026-10-25): colours, text sizes and the geometry of
 * every part on a 1920 x 1080 panel, with the arithmetic that places tabs, rows, the slider bar, the help strip and the alert. Pure
 * Java; MenuScenes draws with these numbers on the phone and the host tests read them.
 *
 * The geometry is the game's own menu measured in game pixels (g: pixels of its 1920 x 1080 menu; the owner's PC references and the
 * Thor's own capture of the console pause menu) times K = 1.4 panel pixels per g: rows 37 g high on a 40.5 g pitch become 52 on 57,
 * the 1302 g block becomes 1824 (95 % of the panel), so that a row is 3.9 mm on the Thor's main screen and 4.4 mm on the Fold. The
 * header is the console pause menu's: a dark plate with the title in the regular weight, three small capitals lines and a portrait
 * box at its right. The prompts stand in one strip at the frame's bottom right, inside the screen. One face for every text: Nunito
 * Sans (OFL) at weight 400, bundled in assets/fonts (UiKit); its capital height is 0.705 of the size.
 *
 * Frame: the same parts on a panel of another size (the Thor's lower-screen trainer: no header, the strip at the top).
 * LookMetricsTest holds the references' numbers.
 */
final class MenuLayout {
    private MenuLayout() { }

    static final int W = 1920, H = 1080;
    /** Panel pixels per game pixel (DESIGN.md 3). */
    static final float K = 1.4f;
    /** The one centred block: the column at its left, the pane at its right (game: 1302, 432, 3). */
    static final int BLOCK_W = 1824, LEFT = (W - BLOCK_W) / 2, RIGHT = LEFT + BLOCK_W, COLUMN_W = 605, PANE_GAP = 4;

    // ---- colours (ARGB) ----
    /** First start (no saved frame yet): a dark colour run. Over the saved, blurred game frame: black at 58 % (42 % of its light). */
    static final int BACKDROP_TOP = 0xFF2B2F2C, BACKDROP_BOTTOM = 0xFF141614, BACKDROP_SHADE = 0x94000000;
    /** Every bar and block: black at 73 %. */
    static final int PANEL = 0xBA000000;
    /** The plate behind the title and the small lines (the console pause menu): black at 22 %. The prompt strip: black at 60 %. */
    static final int PLATE = 0x38000000, PROMPT_STRIP = 0x99000000;
    static final int SELECT = 0xFFEFF1EE, SELECT_TEXT = 0xFF000000, SELECT_TEXT_OFF = 0xFF7D7F7C;
    static final int TEXT = 0xFFF0F0F0, TEXT_OFF = 0xFF858785, TEXT_DIM = 0xFFB4B6B3, ARROW_DIM = 0x2BF0F0F0;
    /**
     * The accent (the line on the selected tab, the sliders, progress): the character's colour in the game. Franklin's is measured
     * on the PC reference, Michael's on the Thor's capture of the pause menu, Trevor's is not measured yet. ACCENT = Franklin's, the
     * one used while the app does not know the character.
     */
    static final int ACCENT = 0xFFACECA9, ACCENT_FRANKLIN = ACCENT, ACCENT_MICHAEL = 0xFF81B7D2, ACCENT_TREVOR = 0xFFF0A75E;
    static final int TRACK = 0x4DACECA9, TRACK_ON_SELECT = 0xE6000000, TRACK_OFF = 0x4D858785;
    /** The "changed" strip on a light bar, where the accent itself would not show. */
    static final int MARK_ON_SELECT = 0xFF4E9E4B;
    static final int WARNING = 0xFFF0C850, ERROR = 0xFFFF7A70, ALERT_YELLOW = 0xFFF0C84B;
    static final int MARK_A = 0xFF5FB400, MARK_A_INK = 0xFFD8F5B2, MARK_B = 0xFFAC0400, MARK_B_INK = 0xFFF9C6C2,
            MARK_X = 0xFF0866C9, MARK_X_INK = 0xFFC6DDF7, MARK_Y = 0xFFD1A600, MARK_Y_INK = 0xFFFAF0C0,
            MARK_START = 0xFFEFF1EE, MARK_START_INK = 0xFF101010;
    static final int ALERT_BLACK = 0xFF000000, ALERT_CELL = 0xFF242624;
    /** Over the Thor's lower-screen HUD (its trainer): the HUD stays faintly visible under the frame. */
    static final int DIM_OVER_HUD = 0xB3000000;

    /** The slider's track: the accent at 30 %. */
    static int track(int accent) {
        return 0x4D000000 | (accent & 0xFFFFFF);
    }

    // ---- text sizes in panel pixels (Nunito Sans: capital height = 0.705 of the size) ----
    /** Capital height of the face as a part of its size. */
    static final float CAP = 0.705f;
    static final int TITLE_PX = 50, CAPS_PX = 24, TAB_PX = 28, ROW_PX = 28, HELP_PX = 26, PROMPT_PX = 26, MARK_PX = 21,
            START_PX = 13, ALERT_TITLE_PX = 100, ALERT_PX = 31, RESTART_PX = 20, DIAGRAM_PX = 24;

    /**
     * The soft shadow under the texts on the header plate (the title, the small capitals): black at 85 %, moved right and down by
     * 0.08 of the capital height and blurred by 0.18 of it (DESIGN.md 5.1).
     */
    static final int SHADOW = 0xD9000000;

    static float shadowOffset(int sizePx) {
        return sizePx * CAP * 0.08f;
    }

    static float shadowBlur(int sizePx) {
        return sizePx * CAP * 0.18f;
    }

    // ---- header (the console pause menu): plate y 84..218, title centred on it, three lines and the portrait box at its right ----
    static final int PLATE_TOP = 84, PLATE_H = 134, TITLE_LEFT = LEFT + 22, TITLE_TOP = PLATE_TOP, TITLE_H = PLATE_H;
    static final int STATUS_TOP = 106, STATUS_LINE_H = 29, STATUS_LINES = 3, STATUS_W = 640, STATUS_RIGHT = RIGHT - 112;
    /** The small lines' pitch: the game's 21 g. */
    static final float STATUS_PITCH = 21 * K;
    static final int PORTRAIT_W = 77, PORTRAIT_H = 76, PORTRAIT_TOP = 113, PORTRAIT_LEFT = RIGHT - 18 - PORTRAIT_W;
    // ---- tab strip ----
    static final int STRIP_TOP = 220, STRIP_H = 54, TAB_GAP = 4, TAB_LINE = 10;
    /** The small arrows outside the two ends of the strip; they stand ARROW_RISE above the middle of the cells, as the game's. */
    static final int ARROW_OUT = 17, ARROW_W = 18, ARROW_H = 27, ARROW_RISE = 1, ARROW_TOUCH = 48;
    /**
     * Thickness of the line an arrow head is drawn with: the strip's arrows, the two round a value, the "more rows" marks (the game's
     * 4.07, 3.05 and 3.37 g, LookMetricsTest).
     */
    static final float ARROW_T = 5.7f, CHEVRON_T = 4.27f, SCROLL_T = 4.72f;
    /** Where an arrow head points. */
    static final int POINT_LEFT = 0, POINT_RIGHT = 1, POINT_UP = 2, POINT_DOWN = 3;

    // ---- body: rows on one grid of slots for the column and the pane ----
    static final int BODY_TOP = 302, ROW_H = 52, ROW_GAP = 5, ROW_PITCH = ROW_H + ROW_GAP, BODY_SLOTS = 12,
            BODY_BOTTOM = BODY_TOP + BODY_SLOTS * ROW_PITCH - ROW_GAP;
    static final int ROW_PAD = 14, VALUE_PAD = 17, MARK_W = 6, BAR_W = 262, BAR_H = 11, BAR_GAP = 22, BAR_FRAME = 3;
    /** The two small arrows round the value of the highlighted row. */
    static final int CHEVRON_W = 12, CHEVRON_H = 19, CHEVRON_GAP = 10;
    /** The up and down marks of the row that says "there are more rows": two arrow heads, one close above the other. */
    static final int SCROLL_W = 24, SCROLL_H = 16, SCROLL_GAP = 4;
    static final int INFO_LINE = 34, INFO_PAD = 5, HELP_LINE = 34, HELP_LINES = 4, TEXT_LINE = 42, TEXT_PAD = 14;
    /** The controller diagram at the top of the Controller tab's pane takes this many slots. */
    static final int DIAGRAM_SLOTS = 8;

    // ---- button prompts: one strip ending at the frame's right edge, 3.5 % above the screen's bottom (DESIGN.md 3.2) ----
    static final int PROMPT_H = 52, PROMPT_BOTTOM = 1042, PROMPT_TOP = PROMPT_BOTTOM - PROMPT_H, PROMPT_RIGHT = RIGHT,
            PROMPT_PAD = 18, PROMPT_END = 18, PROMPT_GAP = 18, PROMPT_SPACE = 39, MARK_D = 29, START_W = 55, START_H = 24,
            PROMPT_TOUCH_UP = 21;
    /**
     * A touch screen without a gamepad (owner 2026-10-25: "only on touch screen devices, and only when a gamepad isn't connected")
     * shows the lower screen's X and check buttons (TOUCH_BUTTON) in place of the strip, the check at the strip's right edge; their
     * bottom ICON_DROP below the strip's (24 px above a 1080 screen's bottom), so that they stay under the body (BODY_BOTTOM 981).
     */
    static final int ICON_DROP = 14;

    // ---- the Thor lower screen's two touch buttons in place of the prompt strip (owner 2026-10-25: "remove the a and b button, and just
    //      make an x and check in the same style as the other save icon"): the Quick Save button's style - a square, black at 78 %,
    //      round corners, a white glyph - at its size: 88 x 88 device px with 16 between them and a radius of 10, which is 69, 12 and 8
    //      panel px at the lower screen's touch unit (1.28: MenuLayout.touchUnit at its 419 dots per inch).
    static final int TOUCH_BUTTON = 69, TOUCH_BUTTON_GAP = 12, TOUCH_BUTTON_RADIUS = 8, TOUCH_BUTTON_PAD = 6;
    /**
     * Where they stand (owner 2026-10-25: "just move it up bro" - off the lower HUD's black bar, which fills its bottom 95 of 1080 px):
     * the check exactly on the Quick Save button's square, the X on the trainer button's, 16 px to its left - the lower screen's own
     * button row, right 32 and bottom 112 device px at 1080 (WheelGesture.saveRect, quick_save.h), over the map.
     */
    static final float QUICK_SAVE_RIGHT_1080 = 32f, QUICK_SAVE_BOTTOM_1080 = 112f, LOWER_HUD_BAR_1080 = 95f;
    static final int TOUCH_BUTTON_FILL = 0xC7000000, TOUCH_BUTTON_GLYPH = 0xFFFFFFFF;
    /** The glyphs' stroke as a part of the button's side (the floppy's 72 px glyph in an 88 px button is drawn with ~13 px strokes). */
    static final float TOUCH_GLYPH_STROKE = 0.15f;

    // ---- alert: the whole screen (the owner's alert: heading ink y 468..544, rules 150 x 3, the message between them) ----
    static final int ALERT_W = 1100, ALERT_RULE = 3, ALERT_RULE_W = 150, ALERT_TITLE_H = 110, ALERT_LINE = 44, ALERT_LINES = 10,
            ALERT_SPACE = 26, CHOICE_W = 300, CHOICE_H = 52, CHOICE_TOUCH = 20;

    /** The pane's texts fade in over this time after a tab or category change. Input is never held back by it. */
    static final int FADE_MS = 120;
    static final float FADE_FROM = 0.35f;

    // ---- touch rectangles: base + index (or + the NavInput command for a prompt) ----
    static final int HIT_TAB = 100, HIT_COLUMN = 200, HIT_ROW = 300, HIT_ARROW_LEFT = 400, HIT_ARROW_RIGHT = 500,
            HIT_PROMPT = 600, HIT_ALERT_BUTTON = 700, HIT_TEXT = 800;

    // ------------------------------------------------------------------------------------------ frames

    /**
     * Where the parts stand on a panel: the launcher's and the main screens' (DEFAULT, 1920 x 1080 with the header) or a smaller one
     * without the header (compact: the Thor's lower-screen trainer, the strip at the top). Row heights, text sizes and paddings are the
     * same in every frame; only the outline changes.
     */
    static final class Frame {
        final int w, h, left, right, blockW, stripTop, bodyTop, bodySlots, bodyBottom, promptRight, promptTop;
        /** Top of the X and check buttons: promptTop in a frame of touch buttons, else ICON_DROP under the strip's bottom. */
        final int iconTop;
        final boolean header;
        /** The bottom right holds the two touch buttons (TOUCH_BUTTON high) instead of the prompt strip: the Thor's lower screen. */
        final boolean touchButtons;

        Frame(int w, int h, int left, int blockW, boolean header, int stripTop, int bodyTop, int bodySlots, int promptRight,
                int promptBottom) {
            this(w, h, left, blockW, header, stripTop, bodyTop, bodySlots, promptRight, promptBottom, false);
        }

        Frame(int w, int h, int left, int blockW, boolean header, int stripTop, int bodyTop, int bodySlots, int promptRight,
                int promptBottom, boolean touchButtons) {
            this.w = w;
            this.h = h;
            this.left = left;
            this.blockW = blockW;
            this.right = left + blockW;
            this.header = header;
            this.stripTop = stripTop;
            this.bodyTop = bodyTop;
            this.bodySlots = bodySlots;
            this.bodyBottom = bodyTop + bodySlots * ROW_PITCH - ROW_GAP;
            this.promptRight = promptRight;
            this.touchButtons = touchButtons;
            this.promptTop = promptBottom - (touchButtons ? TOUCH_BUTTON : PROMPT_H);
            this.iconTop = touchButtons ? promptTop : promptBottom + ICON_DROP - TOUCH_BUTTON;
        }

        /**
         * The launcher's frame on a screen taller than 16:9 (owner 2026-10-25, via main: the Fold's launcher full screen - on its
         * 2176 x 1812 inner screen a panel 1920 x 1599): DEFAULT's width, header, strip and body top; the prompts (or the icons) at the
         * screen's bottom, as far above it as on a 1080 screen; as many body slots as fit between. h <= H: DEFAULT itself.
         */
        static Frame screen(int h) {
            if (h <= H) return DEFAULT;
            int promptBottom = h - (H - PROMPT_BOTTOM);
            int slots = (promptBottom + ICON_DROP - TOUCH_BUTTON - TOUCH_BUTTON_PAD - BODY_TOP + ROW_GAP) / ROW_PITCH;
            return new Frame(W, h, LEFT, BLOCK_W, true, STRIP_TOP, BODY_TOP, slots, PROMPT_RIGHT, promptBottom);
        }

        /** compact(w, h, unit) for a panel that spans a 1080 px tall screen (the Thor's lower screen). */
        static Frame compact(int w, int h) {
            return compact(w, h, 1080f / h);
        }

        /**
         * A panel of w x h (shown at unit device px per panel px) without the header: margin all round, the strip's accent line at
         * the top margin, the body under the strip at the default gap, the two touch buttons (X, check) on the screen's own button
         * row at the bottom right (QUICK_SAVE_*: off the HUD's bottom bar), as many body slots as fit above them.
         */
        static Frame compact(int w, int h, float unit) {
            int margin = 22, stripTop = margin + TAB_LINE, bodyTop = stripTop + STRIP_H + (BODY_TOP - STRIP_TOP - STRIP_H);
            float s = Math.min(1f, h * unit / 1080f);
            int promptBottom = h - Math.round(QUICK_SAVE_BOTTOM_1080 * s / unit), promptRight = w - Math.round(QUICK_SAVE_RIGHT_1080 * s / unit);
            int slots = Math.max(1, (promptBottom - TOUCH_BUTTON - 8 - bodyTop + ROW_GAP) / ROW_PITCH);
            return new Frame(w, h, margin, w - 2 * margin, false, stripTop, bodyTop, slots, promptRight, promptBottom, true);
        }

        /** The touch buttons' boxes {x0, y0, x1, y1}: [0] the X (back, close), [1] the check (select) at the right end. */
        float[][] touchButtons() {
            float y0 = iconTop, y1 = iconTop + TOUCH_BUTTON, checkX1 = promptRight, checkX0 = checkX1 - TOUCH_BUTTON;
            float xX1 = checkX0 - TOUCH_BUTTON_GAP, xX0 = xX1 - TOUCH_BUTTON;
            return new float[][] {{xX0, y0, xX1, y1}, {checkX0, y0, checkX1, y1}};
        }

        /** Left edge of tab index of count equal tabs with TAB_GAP between them; the last one ends at the block's end. */
        int tabLeft(int index, int count) {
            return left + index * (blockW + TAB_GAP) / count;
        }

        int tabWidth(int index, int count) {
            return tabLeft(index + 1, count) - TAB_GAP - tabLeft(index, count);
        }

        int paneLeft(boolean column) {
            return column ? left + COLUMN_W + PANE_GAP : left;
        }

        int paneWidth(boolean column) {
            return right - paneLeft(column);
        }

        /** Top of a slot of the body's grid; the column's items and the pane's rows both stand on it. */
        int slotTop(int slot) {
            return bodyTop + slot * ROW_PITCH;
        }

        /**
         * How many of helpLines help lines are shown under an info block of infoSlots slots: all of them while one row and the "more"
         * row still fit above them; else as many as do (a long notice fills the body; the last shown line then ends in an ellipsis).
         */
        int helpLinesShown(int infoSlots, int helpLines) {
            int lines = helpLines;
            while (lines > 0 && slotTop(infoSlots + 2) + helpHeight(lines) > bodyBottom) lines--;
            return lines;
        }

        /** As MenuLayout.visibleRows, in this frame's body. */
        int visibleRows(int count, int infoSlots, int helpLines) {
            helpLines = helpLinesShown(infoSlots, helpLines);
            int room = bodyBottom - slotTop(infoSlots) - (helpLines > 0 ? helpHeight(helpLines) + ROW_GAP : 0);
            int fit = (room + ROW_GAP) / ROW_PITCH;
            if (count <= fit) return count;
            return Math.max(1, fit - 1);
        }

        /** Lines of a text longer than the pane that are shown at a time (the slot under them says "there is more"). */
        int textLinesVisible() {
            return (slotsHeight(bodySlots - 1) - 2 * TEXT_PAD) / TEXT_LINE;
        }
    }

    /**
     * The panel's height in a view of viewW x viewH shown at u device px per panel px: H, or more in a landscape view taller than
     * 16:9 (its frame: Frame.screen).
     */
    static int panelHeight(int viewW, int viewH, float u) {
        if (u <= 0f || viewW < viewH) return H;
        return Math.max(H, Math.round(viewH / u));
    }

    /** The launcher's and the main screens' frame. */
    static final Frame DEFAULT = new Frame(W, H, LEFT, BLOCK_W, true, STRIP_TOP, BODY_TOP, BODY_SLOTS, PROMPT_RIGHT, PROMPT_BOTTOM);

    /**
     * Device pixels per panel pixel that make a row pitch 4.4 mm on a screen of xdpi pixels per inch (DESIGN.md 3.1): the touch size.
     * The Thor's lower screen (about 419 per inch): 1.28. Not above 3 (a screen that reports a wrong density).
     */
    static float touchUnit(float xdpi) {
        if (!(xdpi > 0f)) return 1f;
        float mmPerPx = 25.4f / xdpi, k = 4.4f / (mmPerPx * 40.5f);
        return Math.min(3f, Math.max(0.5f, k / K));
    }

    // ------------------------------------------------------------------------------------------ tab strip

    /** Left edge of tab index of count equal tabs with TAB_GAP between them; the last one ends at the block's end. */
    static int tabLeft(int index, int count) {
        return DEFAULT.tabLeft(index, count);
    }

    static int tabWidth(int index, int count) {
        return DEFAULT.tabWidth(index, count);
    }

    // ------------------------------------------------------------------------------------------ arrow heads

    /**
     * An arrow head as the game draws it: one line of thickness t, bent once at the tip, its two ends cut square
     * to the line. It fills the box x, y, w, h exactly: the tip touches the side it points to, the ends the three
     * other sides. Returns its six corners (x0, y0, x1, y1, ...), the tip first, then round the shape.
     */
    static float[] chevron(float x, float y, float w, float h, float t, int direction) {
        boolean sideways = direction == POINT_LEFT || direction == POINT_RIGHT;
        // Worked out for a head whose tip lies `along` ahead of its two ends, which lie `across` apart; then turned.
        float along = sideways ? w : h, across = sideways ? h : w, half = across / 2f;
        double angle = armAngle(along, across, t), sin = Math.sin(angle), cos = Math.cos(angle);
        float cut = (float) (t * sin), drop = (float) (t * cos), notch = (float) (t / sin);
        // (how far ahead of the ends, how far across): tip, one end's outer and inner corner, the notch behind the
        // tip, the other end's inner and outer corner.
        float[] shape = {along, half, cut, 0, 0, drop, along - notch, half, 0, across - drop, cut, across};
        float[] corners = new float[shape.length];
        for (int i = 0; i < shape.length; i += 2) {
            float ahead = shape[i], side = shape[i + 1];
            switch (direction) {
                case POINT_RIGHT: corners[i] = x + ahead; corners[i + 1] = y + side; break;
                case POINT_LEFT: corners[i] = x + w - ahead; corners[i + 1] = y + side; break;
                case POINT_DOWN: corners[i] = x + side; corners[i + 1] = y + ahead; break;
                default: corners[i] = x + side; corners[i + 1] = y + h - ahead; break;   // up
            }
        }
        return corners;
    }

    /**
     * The angle between an arm of an arrow head and the direction the head points in, for which a head of
     * thickness t with square-cut ends fills a box `along` by `across`: the root of
     * along * sin - t * sin^2 - across / 2 * cos, found by halving (it is 45 degrees for the game's own heads).
     */
    static double armAngle(float along, float across, float t) {
        double low = 0, high = Math.PI / 2;
        for (int i = 0; i < 40; ++i) {
            double middle = (low + high) / 2, sin = Math.sin(middle);
            if (along * sin - t * sin * sin - across / 2 * Math.cos(middle) < 0) low = middle;
            else high = middle;
        }
        return (low + high) / 2;
    }

    // ------------------------------------------------------------------------------------------ body

    static int paneLeft(boolean column) {
        return DEFAULT.paneLeft(column);
    }

    static int paneWidth(boolean column) {
        return DEFAULT.paneWidth(column);
    }

    /** Top of a slot of the body's grid; the column's items and the pane's rows both stand on it. */
    static int slotTop(int slot) {
        return DEFAULT.slotTop(slot);
    }

    /** Height of a block that covers this many slots (the gaps between them included, the one after them not). */
    static int slotsHeight(int slots) {
        return slots * ROW_PITCH - ROW_GAP;
    }

    /**
     * How many slots the info block at the top of the pane takes for this many text lines: two for one or two lines
     * (the block is taller than a row, so that it does not read as one), more as the text needs. 0 without text.
     */
    static int infoSlots(int lines) {
        if (lines <= 0) return 0;
        return Math.max(2, (lines * INFO_LINE + 2 * INFO_PAD + ROW_GAP + ROW_PITCH - 1) / ROW_PITCH);
    }

    /** Height of the help strip under the rows for this many lines (one line: as high as a row); 0 without text. */
    static int helpHeight(int lines) {
        return lines <= 0 ? 0 : ROW_H + (lines - 1) * HELP_LINE;
    }

    /**
     * How many rows are drawn when count rows follow an info block of infoSlots slots and a help strip of helpLines
     * lines stands under them. All of them when they fit above the body's end; otherwise as many as fit beside the
     * row that says "there are more" (at least one).
     */
    static int visibleRows(int count, int infoSlots, int helpLines) {
        return DEFAULT.visibleRows(count, infoSlots, helpLines);
    }

    /** Frame.helpLinesShown in the default frame. */
    static int helpLinesShown(int infoSlots, int helpLines) {
        return DEFAULT.helpLinesShown(infoSlots, helpLines);
    }

    /**
     * The first row to draw so that the focused row is on screen: the list moves only as far as needed, and never
     * past its ends. first is the row drawn first until now.
     */
    static int windowStart(int first, int focused, int count, int visible) {
        if (focused >= 0 && focused < first) first = focused;
        if (focused >= first + visible) first = focused - visible + 1;
        return Math.max(0, Math.min(first, count - visible));
    }

    /** Width of the filled part of a slider bar for a fraction 0..1 (values outside are taken as the nearest end). */
    static int barFill(float fraction) {
        return barFill(fraction, BAR_W);
    }

    /** The same for a bar of another width (the bar inside its dark frame on the highlighted row). */
    static int barFill(float fraction, int width) {
        if (!(fraction > 0f)) return 0;
        return Math.round(width * Math.min(1f, fraction));
    }

    /** Top of a slider bar in a row or block that starts at top and is height high: on a whole pixel, so that its edges are sharp. */
    static int barTop(int top, int height) {
        return top + (height - BAR_H) / 2;
    }

    // ------------------------------------------------------------------------------------------ scrolled text

    /** Lines of a text longer than the pane that are shown at a time (the slot under them says "there is more"). */
    static int textLinesVisible() {
        return DEFAULT.textLinesVisible();
    }

    /**
     * How many slots the block of a text that fits the pane takes: whole slots, so that its lower edge lies on the
     * column's grid (one line is as high as a row, two lines as two rows with their gap).
     */
    static int textSlots(int lines) {
        return Math.max(1, (lines * TEXT_LINE + 2 * INFO_PAD + ROW_GAP + ROW_PITCH - 1) / ROW_PITCH);
    }

    /** A scroll position (first visible line) kept inside 0 .. total - visible. */
    static int clampScroll(int scroll, int total, int visible) {
        return Math.max(0, Math.min(scroll, total - visible));
    }

    // ------------------------------------------------------------------------------------------ alert

    /** Left edge of choice index of count choices, the whole line centred on the screen. */
    static int choiceLeft(int index, int count) {
        int line = count * CHOICE_W + (count - 1) * TAB_GAP;
        return (W - line) / 2 + index * (CHOICE_W + TAB_GAP);
    }

    /**
     * Height of everything an alert shows: the heading, the two thin lines with the question's lines and the shown
     * lines of its detail between them (a detail longer than ALERT_LINES shows a window), and the answers.
     */
    static int alertHeight(int titleLines, int detailLines) {
        int lines = titleLines + Math.min(detailLines, ALERT_LINES);
        return ALERT_TITLE_H + ALERT_SPACE + ALERT_RULE + ALERT_SPACE + lines * ALERT_LINE + ALERT_SPACE + ALERT_RULE
                + 2 * ALERT_SPACE + CHOICE_H;
    }

    /** Top of the alert's heading: the whole group is centred between the top of the screen and the prompts. */
    static int alertTop(int titleLines, int detailLines) {
        return (PROMPT_TOP - alertHeight(titleLines, detailLines)) / 2;
    }

    /** The owner's alert (layout A), measured: the heading's ink from y 468, the rules at y 583 and 659, the message at 633. */
    static final int RESTART_ALERT_TITLE_TOP = 456, RESTART_ALERT_RULE1 = 583, RESTART_ALERT_RULE2 = 659, RESTART_ALERT_TEXT_TOP = 599;

    // ------------------------------------------------------------------------------------------ backdrop picture

    /**
     * The most pixels of the user's backdrop picture that are ever held in memory (4 MB at four bytes a pixel), and
     * its longest side (no larger than a graphics chip's smallest texture limit).
     */
    static final int BACKDROP_PIXELS = 1 << 20, BACKDROP_SIDE = 2048;

    /** True when a picture of this size may be held and drawn. */
    static boolean backdropFits(long width, long height) {
        return width <= BACKDROP_SIDE && height <= BACKDROP_SIDE && width * height <= BACKDROP_PIXELS;
    }

    /** A side of a picture after it was read reduced by sample: divided and rounded up, the larger of what decoders give. */
    static long reduced(int side, int sample) {
        return ((long) side + sample - 1) / sample;
    }

    /**
     * By how much the user's backdrop picture is reduced when it is read (a power of two): the largest reduction that
     * leaves it at least 240 wide, and more than that for a picture that is narrow but very tall, until it fits
     * (backdropFits). Reading it small is what blurs it, and a file of any size costs little memory.
     */
    static int backdropSample(int width, int height) {
        int sample = 1;
        while (width / (sample * 2) >= 240) sample *= 2;
        while (!backdropFits(reduced(width, sample), reduced(height, sample))) sample *= 2;
        return sample;
    }

    /**
     * How many times the reduced picture is then doubled, each time with filtering, before it is kept: as often as
     * it still fits (a screenshot read at 240 x 135 becomes 960 x 540). Enlarged in doublings the picture comes out
     * smoothly blurred; enlarged to the screen in one step it shows the squares of its pixels.
     */
    static int backdropDoublings(int width, int height) {
        if (width <= 0 || height <= 0) return 0;
        int steps = 0;
        while (backdropFits((long) width << (steps + 1), (long) height << (steps + 1))) steps++;
        return steps;
    }

    /**
     * The part of a picture (left, top, right, bottom) that fills a window of another shape without being
     * stretched: the middle of the picture, cut at the sides or at top and bottom, and never less than one pixel.
     * All of the picture for a window without a size (a view that was not laid out yet).
     */
    static int[] cropToFill(int pictureW, int pictureH, int windowW, int windowH) {
        if (windowW <= 0 || windowH <= 0) return new int[] {0, 0, pictureW, pictureH};
        if ((long) pictureW * windowH > (long) pictureH * windowW) {
            int w = Math.max(1, (int) ((long) pictureH * windowW / windowH)), left = (pictureW - w) / 2;
            return new int[] {left, 0, left + w, pictureH};
        }
        int h = Math.max(1, (int) ((long) pictureW * windowH / windowW)), top = (pictureH - h) / 2;
        return new int[] {0, top, pictureW, top + h};
    }

    // ------------------------------------------------------------------------------------------ motion

    /** Opacity of the pane's texts elapsedMs after another page came up: FADE_FROM at once, 1 from FADE_MS on. */
    static float fadeAlpha(long elapsedMs) {
        if (elapsedMs >= FADE_MS) return 1f;
        if (elapsedMs <= 0) return FADE_FROM;
        return FADE_FROM + (1f - FADE_FROM) * elapsedMs / FADE_MS;
    }
}
