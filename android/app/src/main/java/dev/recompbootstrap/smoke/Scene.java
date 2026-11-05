package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;

/**
 * One drawn frame of the menu as a list of plain shapes and texts in design pixels (a 1920 x 1080 panel), plus the
 * rectangles that react to touch. Pure Java. MenuScenes fills it; the app paints it with android.graphics (UiKit's
 * SceneView) and the host preview tool paints the same list with java.awt, so both show the same geometry.
 *
 * A text is placed in a box: vertically centred, horizontally at the box's left edge, centre or right edge. The
 * painter draws exactly the string it is given; fitting and wrapping happen here, with the painter's own font
 * measure, before the text enters the list.
 */
final class Scene {
    /**
     * BACKDROP: what lies behind the menu, over the whole window: the user's own picture when the painter has one
     * (scaled to fill, covered with MenuLayout.BACKDROP_SHADE), else a colour run from color (top) to color2.
     * FILL: one colour over the whole window. ROUND: a filled rectangle with corners of radius stroke. LINE: an open line through
     * points, stroke thick, square ends. RING: the outline of the circle in the box x, y, w, h, stroke thick.
     */
    static final int RECT = 0, DISC = 1, POLY = 2, TEXT = 3, BACKDROP = 4, FILL = 5, ROUND = 6, LINE = 7, RING = 8;
    static final int LEFT = 0, CENTER = 1, RIGHT = 2;
    /** Layers: what stands still, what fades in after another page came up (texts and bars of the pane), the alert. */
    static final int CHROME = 0, CONTENT = 1, ALERT = 2;
    /**
     * Text faces, all of them the bundled Nunito Sans (UiKit): REGULAR, TITLE and CAPS at weight 400 (TITLE and CAPS: the header's
     * title and small capitals, kept apart for their shadow), BOLD at 700 (the letters in the button marks), HEAVY at 900 (the alert's
     * heading word).
     */
    static final int REGULAR = 0, BOLD = 1, TITLE = 2, CAPS = 3, HEAVY = 4;

    /** Width of a text in design pixels at a text size in design pixels, in one of the faces above. */
    interface Measure {
        float width(String text, int sizePx, int face);
    }

    /** The panel this scene was made for (MenuScenes.build: its frame's w x h); the view centres it. */
    int panelW = MenuLayout.W, panelH = MenuLayout.H;

    static final class Item {
        int type, layer, color, color2, size, align, face;
        float x, y, w, h, stroke;
        String text;
        float[] points;
        /** A text with the soft shadow of MenuLayout.SHADOW under it: one that stands on the backdrop itself. */
        boolean shadow;
    }

    static final class Hit {
        final float x, y, w, h;
        final int id;

        Hit(float x, float y, float w, float h, int id) {
            this.x = x;
            this.y = y;
            this.w = w;
            this.h = h;
            this.id = id;
        }
    }

    final List<Item> items = new ArrayList<Item>();
    final List<Hit> hits = new ArrayList<Hit>();
    /** Changes when the page content is another one (tab, category, alert): the painter then fades the content in. */
    String contentKey = "";
    private int layer = CHROME;

    void layer(int layer) {
        this.layer = layer;
    }

    private Item add(int type, float x, float y, float w, float h, int color) {
        Item item = new Item();
        item.type = type;
        item.layer = layer;
        item.x = x;
        item.y = y;
        item.w = w;
        item.h = h;
        item.color = color;
        items.add(item);
        return item;
    }

    void rect(float x, float y, float w, float h, int color) {
        add(RECT, x, y, w, h, color);
    }

    void round(float x, float y, float w, float h, float radius, int color) {
        add(ROUND, x, y, w, h, color).stroke = radius;
    }

    void backdrop(int top, int bottom) {
        add(BACKDROP, 0, 0, MenuLayout.W, MenuLayout.H, top).color2 = bottom;
    }

    void fill(int color) {
        add(FILL, 0, 0, MenuLayout.W, MenuLayout.H, color);
    }

    void disc(float cx, float cy, float radius, int color) {
        add(DISC, cx - radius, cy - radius, 2 * radius, 2 * radius, color);
    }

    /** A filled shape; points are x0, y0, x1, y1, ... */
    void poly(int color, float... points) {
        add(POLY, 0, 0, 0, 0, color).points = points;
    }

    /** An open line through the points (x0, y0, x1, y1, ...), thickness wide. */
    void line(int color, float thickness, float... points) {
        add(LINE, 0, 0, 0, 0, color).points = points;
        items.get(items.size() - 1).stroke = thickness;
    }

    /** The outline of a circle, thickness wide. */
    void ring(float cx, float cy, float radius, float thickness, int color) {
        add(RING, cx - radius, cy - radius, 2 * radius, 2 * radius, color).stroke = thickness;
    }

    void text(String text, float x, float y, float w, float h, int size, int face, int align, int color) {
        text(text, x, y, w, h, size, face, align, color, false);
    }

    /** A text; with shadow, one that stands on the backdrop itself and gets the soft shadow under it. */
    void text(String text, float x, float y, float w, float h, int size, int face, int align, int color, boolean shadow) {
        if (text == null || text.isEmpty()) return;
        Item item = add(TEXT, x, y, w, h, color);
        item.text = text;
        item.size = size;
        item.face = face;
        item.align = align;
        item.shadow = shadow;
    }

    void hit(float x, float y, float w, float h, int id) {
        hits.add(new Hit(x, y, w, h, id));
    }

    /** The id of the touch rectangle at a point, the one added last when several overlap; -1 when there is none. */
    int hitAt(float x, float y) {
        for (int i = hits.size() - 1; i >= 0; --i) {
            Hit hit = hits.get(i);
            if (x >= hit.x && x < hit.x + hit.w && y >= hit.y && y < hit.y + hit.h) return hit.id;
        }
        return -1;
    }

    /**
     * False when next paints exactly what shown paints and reacts to the same touches: a command that changed
     * nothing (a held direction stopped at the end of a list) then costs no frame (vm-pad-hold D4).
     */
    static boolean needsDraw(Scene shown, Scene next) {
        return shown == null || !shown.sameAs(next);
    }

    /** Every item, every touch rectangle and the content key equal, in the same order. */
    boolean sameAs(Scene other) {
        if (other == null || !contentKey.equals(other.contentKey) || items.size() != other.items.size()
                || hits.size() != other.hits.size()) return false;
        for (int i = 0; i < items.size(); ++i) if (!same(items.get(i), other.items.get(i))) return false;
        for (int i = 0; i < hits.size(); ++i) {
            Hit a = hits.get(i), b = other.hits.get(i);
            if (a.id != b.id || Float.compare(a.x, b.x) != 0 || Float.compare(a.y, b.y) != 0
                    || Float.compare(a.w, b.w) != 0 || Float.compare(a.h, b.h) != 0) return false;
        }
        return true;
    }

    private static boolean same(Item a, Item b) {
        return a.type == b.type && a.layer == b.layer && a.color == b.color && a.color2 == b.color2 && a.size == b.size
                && a.align == b.align && a.face == b.face && a.shadow == b.shadow
                && Float.compare(a.x, b.x) == 0 && Float.compare(a.y, b.y) == 0 && Float.compare(a.w, b.w) == 0
                && Float.compare(a.h, b.h) == 0 && Float.compare(a.stroke, b.stroke) == 0
                && Objects.equals(a.text, b.text) && Arrays.equals(a.points, b.points);
    }

    // ------------------------------------------------------------------------------------------ text fitting

    static final String ELLIPSIS = "…";

    /** "Frame-rate counter" becomes "Frame-Rate Counter": every word starts with a capital, also after a hyphen. */
    static String titleCase(String text) {
        StringBuilder out = new StringBuilder(text.length());
        boolean start = true;
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            out.append(start ? Character.toUpperCase(c) : c);
            start = c == ' ' || c == '-';
        }
        return out.toString();
    }

    /** The text itself when it fits into maxWidth, else its longest beginning that fits with an ellipsis at the end. */
    static String fit(String text, int size, int face, float maxWidth, Measure measure) {
        if (text == null) return "";
        if (measure.width(text, size, face) <= maxWidth) return text;
        int end = text.length();
        while (end > 0 && measure.width(text.substring(0, end).trim() + ELLIPSIS, size, face) > maxWidth) end--;
        return text.substring(0, end).trim() + ELLIPSIS;
    }

    /** Lines at least this long count as cut by hand at a fixed width (the licence texts are: their cut lines are 66 to 78 long). */
    static final int REFLOW_FROM = 65;

    /**
     * Joins the lines of paragraphs that were cut by hand at a fixed width, so that wrap() can break them for the
     * pane's own width instead of leaving every second line short. A line is joined to the one before it when that
     * line is REFLOW_FROM characters or longer and does not end a sentence or a heading ('.', ':' or ')'), and the
     * line itself is not empty. No word is added, removed or reordered.
     */
    static String reflow(String text) {
        StringBuilder out = new StringBuilder();
        String previous = "";
        for (String line : text.split("\n", -1)) {
            boolean join = previous.length() >= REFLOW_FROM && !line.trim().isEmpty()
                    && ".:)".indexOf(previous.charAt(previous.length() - 1)) < 0;
            if (out.length() > 0) out.append(join ? " " : "\n");
            out.append(join ? line.trim() : line);
            previous = line;
        }
        return out.toString();
    }

    /**
     * Breaks a text into lines no wider than maxWidth. A line break in the text always starts a new line (an empty
     * line stays an empty line); otherwise lines break at spaces, and a single word wider than a line is cut.
     * Leading spaces of a paragraph are kept (the licence texts indent with them).
     */
    static List<String> wrap(String text, int size, int face, float maxWidth, Measure measure) {
        List<String> lines = new ArrayList<String>();
        if (text == null) return lines;
        for (String paragraph : text.split("\n", -1)) {
            String line = "";
            int at = 0;
            while (at < paragraph.length() && paragraph.charAt(at) == ' ') at++;
            String indent = paragraph.substring(0, at);
            for (String word : paragraph.substring(at).split(" +")) {
                String start = line.isEmpty() ? indent : line + " ";
                if (!line.isEmpty() && measure.width(start + word, size, face) > maxWidth) {
                    lines.add(line);
                    line = "";
                    start = indent;
                }
                // A word that alone is wider than the line is cut into pieces that fit.
                while (measure.width(start + word, size, face) > maxWidth && word.length() > 1) {
                    int cut = word.length() - 1;
                    while (cut > 1 && measure.width(start + word.substring(0, cut), size, face) > maxWidth) cut--;
                    lines.add(start + word.substring(0, cut));
                    word = word.substring(cut);
                    start = indent;
                }
                line = start + word;
            }
            lines.add(line.trim().isEmpty() ? "" : line);
        }
        return lines;
    }
}
