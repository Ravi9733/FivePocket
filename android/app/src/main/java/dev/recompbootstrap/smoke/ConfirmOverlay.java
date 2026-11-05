package dev.recompbootstrap.smoke;

/**
 * A question put over a page: an alert that takes the whole screen, with the question, an optional detail text and
 * a line of answers, one of which is highlighted. Pure Java: it only says what the alert shows. Which answer is
 * highlighted and what an answer does is decided by the page's model; MenuScenes draws it and MenuLayout places it.
 */
final class ConfirmOverlay {
    String title, detail;
    /** The word over the question, in place of the frame's "Alert"; null for that default. */
    String heading;
    String[] buttons = new String[0];
    /** Index of the highlighted answer. */
    int focused;
    /** First visible line of a detail text longer than the alert shows at a time. */
    int scroll;

    ConfirmOverlay(String title, String detail, String[] buttons, int focused) {
        this.title = title;
        this.detail = detail;
        this.buttons = buttons;
        this.focused = focused;
    }

    /** A question with its own heading (for example "Copy" or "Stop Copying"). */
    ConfirmOverlay(String heading, String title, String detail, String[] buttons, int focused) {
        this(title, detail, buttons, focused);
        this.heading = heading;
    }

    /**
     * The scroll position after UP (direction -1) or DOWN (+1): three lines at a time, never past the ends.
     * detailLines is the number of lines the detail text takes.
     */
    static int scrolled(int scroll, int direction, int detailLines) {
        int visible = Math.min(detailLines, MenuLayout.ALERT_LINES);
        return MenuLayout.clampScroll(scroll + (direction < 0 ? -3 : 3), detailLines, visible);
    }
}
