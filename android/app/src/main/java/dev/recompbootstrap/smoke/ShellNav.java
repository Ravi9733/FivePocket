package dev.recompbootstrap.smoke;

/**
 * Where the highlight is in the tabbed frame and how the controller and touch move it
 * (local-analysis/agents/app-ui-4/DESIGN.md section 4). Pure Java; LauncherScreen carries out what it returns.
 *
 * Three levels: the tab strip, the column of the current tab's page, and the pane at the right of the column (the
 * rows of a settings category, the text of an About section). Each tab remembers its column item and whether the
 * highlight was in its pane, so coming back to a tab lands where it was left.
 *
 * L1 / R1 change the tab from every level; LEFT / RIGHT do so on the strip. A fresh press wraps round from the last
 * tab to the first and back; a held direction stops at the end. In the column UP on the first item goes to the strip
 * and DOWN on the last item goes round to the first, both on a fresh press only: a held direction stops at either end.
 * Leaving the Settings tab, or pressing B on its column, while changes are unsaved is not done at once: the answer is
 * ASK, the caller puts the question, and leaveGranted() or leaveCancelled() finishes it.
 *
 * The highlight is never left in a pane that cannot be entered (rows that are being read again, a text that became
 * short): the next command inside the tab finds it on the column item, where the page already draws it.
 *
 * A direction that is still held after it took the highlight into the pane does nothing more until it is let go:
 * RIGHT held on a category's name would otherwise go on to change the value of the row it landed on. Another
 * button pressed during that hold (A, B, Start, Y, L1, R1) does not end it; only a fresh direction does.
 *
 * Touch: a tap on a column item that is not highlighted only moves the highlight there; a tap on the highlighted
 * one is A.
 */
final class ShellNav {
    static final int PLAY = 0, SETTINGS = 1, CONTROLLER = 2, ABOUT = 3, TABS = 4;
    static final String[] TAB_NAMES = {"PLAY", "SETTINGS", "CONTROLLER", "ABOUT"};

    static final int STRIP = 0, COLUMN = 1, PANE = 2;

    /** Answers of command(): what the caller must do. */
    static final int NOTHING = 0,   // consumed, nothing changed
            MOVED = 1,              // the highlight moved inside the tab: draw again
            TAB = 2,                // another tab is current now
            ENTERED = 3,            // the highlight went from the column into the pane
            ACTIVATE = 4,           // A on a column item that has no pane to enter: carry out the item
            PANE_COMMAND = 5,       // the highlight is in the pane: the page handles this command
            ASK = 6;                // unsaved changes: put the question, then leaveGranted() or leaveCancelled()

    /** Who gets a command: ShellNav itself, the settings model, or the Play question (B answers it with "no"). */
    static final int TO_NAV = 0, TO_SETTINGS_MODEL = 1, TO_PLAY_QUESTION = 2;

    /**
     * Decides who gets a command before the highlight is asked. On the Settings tab the model gets everything while
     * one of its questions is open or a save runs (settingsCapturing), and Start and Y whenever its rows exist
     * (settingsReady). On the Play tab B answers an open "start anyway?" question (playAsking) instead of moving.
     */
    static int route(int tab, int command, boolean settingsCapturing, boolean settingsReady, boolean playAsking) {
        if (tab == SETTINGS) {
            if (settingsCapturing) return TO_SETTINGS_MODEL;
            if (settingsReady && (command == NavInput.MENU || command == NavInput.ALT)) return TO_SETTINGS_MODEL;
        }
        if (tab == PLAY && playAsking && command == NavInput.BACK) return TO_PLAY_QUESTION;
        return TO_NAV;
    }

    /**
     * True when the answer to a command means that a direction still held must stop: another tab came up by L1 / R1
     * (or by A on the settings question that a tab change put). A direction walking along the strip changes the tab
     * too, and goes on doing so while it is held: that is the strip's own walk, not ended here (vm-pad-hold D5).
     */
    static boolean endsHold(int answer, int command) {
        return answer == TAB && !(command >= NavInput.UP && command <= NavInput.RIGHT);
    }

    private int tab = PLAY;
    private boolean onStrip;
    private final int[] column = new int[TABS];
    private final int[] columnCount = {1, 1, 1, 1};
    private final boolean[] inPane = new boolean[TABS];
    private int pendingTab = -1;
    private boolean pendingStrip;
    /** The direction whose press took the highlight into the pane, while it may still be held; else NavInput.NONE. */
    private int enteredBy = NavInput.NONE;
    /** The last command, tap or granted leave took the highlight from a column item or its pane up to the tab strip. */
    private boolean wentToStrip;

    int tab() {
        return tab;
    }

    int level() {
        return onStrip ? STRIP : inPane[tab] ? PANE : COLUMN;
    }

    int column(int ofTab) {
        return column[ofTab];
    }

    /** True when a tab's highlight is, or was when the tab was left, in its pane. */
    boolean inPane(int ofTab) {
        return inPane[ofTab];
    }

    /**
     * True when the last command(), tap or leaveGranted() took the highlight from a column item (or its pane) up to
     * the tab strip. False while it only stays on the strip: a button that does nothing there has not left anything.
     */
    boolean wentToStrip() {
        return wentToStrip;
    }

    /** How many items the column of a tab has now; a remembered item past the end moves to the last one. */
    void setColumnCount(int ofTab, int count) {
        columnCount[ofTab] = Math.max(1, count);
        column[ofTab] = Math.min(column[ofTab], columnCount[ofTab] - 1);
    }

    /** The page itself says where a tab's highlight is inside the page (back on the category's name, for example). */
    void place(int ofTab, int columnItem, boolean pane) {
        column[ofTab] = Math.max(0, Math.min(columnItem, columnCount[ofTab] - 1));
        inPane[ofTab] = pane;
    }

    /**
     * The page put the highlight on a row of a column item's pane (a tapped row, a question that ends on the Save
     * row). On the tab that is shown it is then no longer on the tab strip either.
     */
    void intoPane(int ofTab, int columnItem) {
        place(ofTab, columnItem, true);
        if (ofTab == tab) onStrip = false;
    }

    /**
     * A NavInput command. paneEnterable: the current column item has a pane the highlight can go into.
     * unsaved: the settings page holds unsaved changes.
     */
    int command(int command, boolean repeat, boolean paneEnterable, boolean unsaved) {
        boolean stripBefore = onStrip;
        int answer = handle(command, repeat, paneEnterable, unsaved);
        wentToStrip = onStrip && !stripBefore;
        return answer;
    }

    private int handle(int command, boolean repeat, boolean paneEnterable, boolean unsaved) {
        // The direction that took the highlight into the pane, still held: it does nothing on any level.
        boolean stillHeld = repeat && command == enteredBy;
        // NavInput repeats one direction at a time and a fresh direction takes the repeat over; a button pressed
        // meanwhile does not stop it. So only a fresh direction says that the hold which entered the pane is over.
        if (!repeat && command >= NavInput.UP && command <= NavInput.RIGHT) enteredBy = NavInput.NONE;
        if (command == NavInput.PAGE_PREV) return changeTab(wrapped(tab - 1, repeat), unsaved);
        if (command == NavInput.PAGE_NEXT) return changeTab(wrapped(tab + 1, repeat), unsaved);
        if (onStrip) {
            if (stillHeld) return NOTHING;
            if (command == NavInput.LEFT) return changeTab(wrapped(tab - 1, repeat), unsaved);
            if (command == NavInput.RIGHT) return changeTab(wrapped(tab + 1, repeat), unsaved);
            if (command == NavInput.DOWN || command == NavInput.CONFIRM) {
                onStrip = false;
                if (!paneEnterable) inPane[tab] = false;   // into the page: onto the item, when its pane is gone
                return MOVED;
            }
            return NOTHING;
        }
        // The pane the highlight was in cannot be entered any more: the command finds the highlight on the column item.
        // (Only for a command that stays inside the tab: a tab that is passed through with L1 / R1, or along the
        // strip, keeps what it remembers - its rows are usually back when it is shown again.)
        if (!paneEnterable) inPane[tab] = false;
        if (stillHeld) return NOTHING;
        if (inPane[tab]) {
            if (command != NavInput.BACK) return PANE_COMMAND;
            inPane[tab] = false;
            return MOVED;
        }
        switch (command) {
            case NavInput.UP:
                if (column[tab] > 0) {
                    column[tab]--;
                    return MOVED;
                }
                if (repeat) return NOTHING;
                onStrip = true;
                return MOVED;
            case NavInput.DOWN:
                if (column[tab] < columnCount[tab] - 1) {
                    column[tab]++;
                    return MOVED;
                }
                // On the last item a fresh press goes round to the first, as in a category's rows; a held DOWN stops.
                if (repeat || columnCount[tab] < 2) return NOTHING;
                column[tab] = 0;
                return MOVED;
            case NavInput.CONFIRM:
            case NavInput.RIGHT:
                if (paneEnterable) {
                    inPane[tab] = true;
                    // (A never repeats; and A must not make a RIGHT forgotten that is still held from before.)
                    if (command == NavInput.RIGHT) enteredBy = command;
                    return ENTERED;
                }
                return command == NavInput.CONFIRM ? ACTIVATE : NOTHING;
            case NavInput.BACK:
                if (tab == SETTINGS && unsaved) {
                    pendingTab = -1;
                    pendingStrip = true;
                    return ASK;
                }
                onStrip = true;
                return MOVED;
            default:
                return NOTHING;
        }
    }

    /** Touch: a tap on a tab. */
    int tapTab(int target, boolean unsaved) {
        wentToStrip = false;   // a tap on a tab never puts the highlight on the strip
        if (target < 0 || target >= TABS) return NOTHING;
        if (target == tab) {
            if (!onStrip) return NOTHING;
            onStrip = false;
            return MOVED;
        }
        int answer = changeTab(target, unsaved);
        if (answer == TAB) onStrip = false;
        return answer;
    }

    /**
     * Touch: a tap on a column item of the current tab. On the highlighted item it is A (ENTERED or ACTIVATE);
     * an explicit tap on an action activates it; a pane item is first selected, then entered.
     */
    int tapColumn(int item, boolean paneEnterable) {
        wentToStrip = false;   // nor does a tap on a column item
        if (item < 0 || item >= columnCount[tab]) return NOTHING;
        // A direct tap is an explicit activation. Do not require the player to tap the same
        // Play / Controller / About action twice just to move the highlight first.
        if (!paneEnterable) {
            onStrip = false;
            inPane[tab] = false;
            column[tab] = item;
            return ACTIVATE;
        }
        if (level() == COLUMN && column[tab] == item) {
            inPane[tab] = true;
            return ENTERED;
        }
        onStrip = false;
        inPane[tab] = false;
        column[tab] = item;
        return MOVED;
    }

    /** The question was answered with Save (and the save worked) or Discard: do what was asked for. Returns the answer of the move. */
    int leaveGranted() {
        int target = pendingTab;
        boolean strip = pendingStrip;
        leaveCancelled();
        wentToStrip = false;
        if (target >= 0) {
            tab = target;
            return TAB;
        }
        if (strip) {
            wentToStrip = true;   // the question was put by B on a category's name
            onStrip = true;
            return MOVED;
        }
        return NOTHING;
    }

    /** The question was closed without an answer, or the save failed: stay. */
    void leaveCancelled() {
        pendingTab = -1;
        pendingStrip = false;
    }

    /** True between an ASK and its leaveGranted() or leaveCancelled(). */
    boolean asking() {
        return pendingTab >= 0 || pendingStrip;
    }

    /** The tab index one step further; -1 when a held direction has reached the end. */
    private static int wrapped(int target, boolean repeat) {
        if (target >= 0 && target < TABS) return target;
        return repeat ? -1 : (target + TABS) % TABS;
    }

    private int changeTab(int target, boolean unsaved) {
        if (target < 0 || target == tab) return NOTHING;
        if (tab == SETTINGS && unsaved) {
            pendingTab = target;
            pendingStrip = false;
            return ASK;
        }
        tab = target;
        return TAB;
    }
}
