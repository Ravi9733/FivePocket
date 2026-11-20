package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.begin;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.end;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

/**
 * Host test of ShellNav: where the highlight goes for every key on every level of the tabbed frame (tab strip,
 * column, pane), what each tab remembers, where movement wraps and where it stops, what a tap does, and that unsaved
 * settings are asked about before the Settings tab is left. Run with java -ea.
 *
 * The rules are those of local-analysis/agents/ship-week/menu-nav/NAV-CONTRACT.md section 3 (S-1 to S-14); a case
 * that enforces one names it.
 */
public final class ShellNavTest {
    private static final int UP = NavInput.UP, DOWN = NavInput.DOWN, LEFT = NavInput.LEFT, RIGHT = NavInput.RIGHT,
            A = NavInput.CONFIRM, B = NavInput.BACK, L1 = NavInput.PAGE_PREV, R1 = NavInput.PAGE_NEXT,
            START = NavInput.MENU, Y = NavInput.ALT;

    /** A frame as the app builds it: five settings categories, four About sections. */
    private static ShellNav frame() {
        ShellNav nav = new ShellNav();
        nav.setColumnCount(ShellNav.SETTINGS, 5);
        nav.setColumnCount(ShellNav.ABOUT, 4);
        return nav;
    }

    private static int press(ShellNav nav, int command) {
        return nav.command(command, false, true, false);
    }

    private static void at(ShellNav nav, int tab, int level, int column, String what) {
        same(tab, nav.tab(), what + ": tab");
        same(level, nav.level(), what + ": level");
        same(column, nav.column(nav.tab()), what + ": column item");
    }

    public static void main(String[] args) {
        begin("ShellNavTest");

        test("start: the Play tab, the highlight on its action, so that A plays at once", () -> {
            at(frame(), ShellNav.PLAY, ShellNav.COLUMN, 0, "a new frame");
            same(4, ShellNav.TAB_NAMES.length, "one name per tab");
        });

        test("L1 / R1 change the tab from every level, wrap on a fresh press and stop when held", () -> {
            ShellNav nav = frame();
            same(ShellNav.TAB, press(nav, R1), "R1 answers TAB");
            at(nav, ShellNav.SETTINGS, ShellNav.COLUMN, 0, "R1 from Play");
            press(nav, R1);
            press(nav, R1);
            same(ShellNav.ABOUT, nav.tab(), "three R1: About");
            same(ShellNav.NOTHING, nav.command(R1, true, true, false), "held R1 stops at the last tab");
            same(ShellNav.ABOUT, nav.tab(), "still About");
            same(ShellNav.TAB, press(nav, R1), "a fresh R1 wraps");
            same(ShellNav.PLAY, nav.tab(), "to Play");
            same(ShellNav.NOTHING, nav.command(L1, true, true, false), "held L1 stops at the first tab");
            same(ShellNav.TAB, press(nav, L1), "a fresh L1 wraps back");
            same(ShellNav.ABOUT, nav.tab(), "to About");
            // from the pane
            press(nav, L1);
            press(nav, L1);
            press(nav, A);
            at(nav, ShellNav.SETTINGS, ShellNav.PANE, 0, "A enters the rows");
            same(ShellNav.TAB, press(nav, R1), "R1 from inside the rows");
            same(ShellNav.CONTROLLER, nav.tab(), "Controller");
            // from the strip
            press(nav, UP);
            same(ShellNav.STRIP, nav.level(), "UP from the one item: the strip");
            same(ShellNav.TAB, press(nav, L1), "L1 on the strip");
            same(ShellNav.SETTINGS, nav.tab(), "Settings");
        });

        test("strip: LEFT / RIGHT change the tab (wrap fresh, stop held), DOWN and A go into the page, the rest does nothing", () -> {
            ShellNav nav = frame();
            press(nav, UP);
            at(nav, ShellNav.PLAY, ShellNav.STRIP, 0, "UP from the Play action");
            same(ShellNav.TAB, press(nav, RIGHT), "RIGHT");
            at(nav, ShellNav.SETTINGS, ShellNav.STRIP, 0, "the highlight stays on the strip");
            same(ShellNav.TAB, press(nav, LEFT), "LEFT");
            same(ShellNav.NOTHING, nav.command(LEFT, true, true, false), "held LEFT stops at Play");
            same(ShellNav.TAB, press(nav, LEFT), "fresh LEFT wraps");
            same(ShellNav.ABOUT, nav.tab(), "to About");
            same(ShellNav.TAB, press(nav, RIGHT), "fresh RIGHT wraps");
            same(ShellNav.PLAY, nav.tab(), "to Play");
            for (int nothing : new int[] {UP, B, START, Y}) {
                same(ShellNav.NOTHING, press(nav, nothing), "command " + nothing + " on the strip");
                at(nav, ShellNav.PLAY, ShellNav.STRIP, 0, "unchanged");
            }
            same(ShellNav.MOVED, press(nav, DOWN), "DOWN goes into the page");
            same(ShellNav.COLUMN, nav.level(), "on the column");
            press(nav, UP);
            same(ShellNav.MOVED, press(nav, A), "A goes into the page and does not carry out its item");
            same(ShellNav.COLUMN, nav.level(), "on the column");
        });

        test("S-4 column: UP / DOWN move; UP on the first item goes to the strip and DOWN on the last goes round to the first, on a fresh press only", () -> {
            ShellNav nav = frame();
            press(nav, R1);
            for (int i = 1; i <= 4; ++i) {
                same(ShellNav.MOVED, press(nav, DOWN), "DOWN " + i);
                same(i, nav.column(ShellNav.SETTINGS), "category " + i);
            }
            same(ShellNav.NOTHING, nav.command(DOWN, true, true, false), "a held DOWN stops on the last category");
            same(4, nav.column(ShellNav.SETTINGS), "still the last");
            same(ShellNav.MOVED, press(nav, DOWN), "a fresh DOWN on the last category goes round");
            at(nav, ShellNav.SETTINGS, ShellNav.COLUMN, 0, "to the first category, still on the column");
            same(ShellNav.MOVED, nav.command(DOWN, true, true, false), "and a hold goes on from there");
            same(1, nav.column(ShellNav.SETTINGS), "to the second");
            for (int i = 2; i <= 4; ++i) nav.command(DOWN, true, true, false);
            same(4, nav.column(ShellNav.SETTINGS), "a hold that started on the first category ends on the last");
            same(ShellNav.NOTHING, nav.command(DOWN, true, true, false), "and does not go round a second time");
            ShellNav one = frame();
            same(ShellNav.NOTHING, press(one, DOWN), "a column of one item (Play) has nowhere to go round to");
            at(one, ShellNav.PLAY, ShellNav.COLUMN, 0, "the highlight stays on Play");
            for (int i = 3; i >= 0; --i) same(ShellNav.MOVED, nav.command(UP, true, true, false), "held UP moves up");
            same(ShellNav.NOTHING, nav.command(UP, true, true, false), "held UP stops on the first category");
            same(ShellNav.COLUMN, nav.level(), "still the column");
            same(ShellNav.MOVED, press(nav, UP), "a fresh UP goes to the strip");
            same(ShellNav.STRIP, nav.level(), "the strip");
            same(ShellNav.NOTHING, press(frame(), LEFT), "LEFT on a column does nothing");
            same(ShellNav.NOTHING, press(frame(), START), "Start is not the highlight's business");
        });

        test("column: A or RIGHT enters a pane that can be entered; A on an item without one carries it out", () -> {
            ShellNav nav = frame();
            same(ShellNav.ACTIVATE, nav.command(A, false, false, false), "A on Play's action");
            same(ShellNav.NOTHING, nav.command(RIGHT, false, false, false), "RIGHT on Play's action");
            same(ShellNav.COLUMN, nav.level(), "the highlight stays");
            press(nav, R1);
            same(ShellNav.ENTERED, nav.command(RIGHT, false, true, false), "RIGHT enters the rows");
            same(ShellNav.PANE, nav.level(), "in the rows");
            same(ShellNav.PANE_COMMAND, press(nav, LEFT), "in the rows LEFT is the page's (it changes a value)");
            for (int command : new int[] {UP, DOWN, RIGHT, A, START, Y}) {
                same(ShellNav.PANE_COMMAND, press(nav, command), "command " + command + " in the rows is the page's");
            }
            same(ShellNav.MOVED, press(nav, B), "B leaves the rows");
            at(nav, ShellNav.SETTINGS, ShellNav.COLUMN, 0, "back on the category");
            same(ShellNav.MOVED, press(nav, B), "B on the column goes to the strip");
            same(ShellNav.STRIP, nav.level(), "the strip");
        });

        test("a direction still held after it took the highlight into the pane does nothing more until it is let go", () -> {
            ShellNav nav = frame();
            press(nav, R1);
            same(ShellNav.ENTERED, nav.command(RIGHT, false, true, false), "RIGHT on a category's name goes into its rows");
            for (int repeats = 0; repeats < 12; ++repeats) {
                same(ShellNav.NOTHING, nav.command(RIGHT, true, true, false), "held on: repeat " + repeats + " is not passed to the row (it would change its value)");
            }
            same(ShellNav.PANE, nav.level(), "the highlight is in the rows");
            same(ShellNav.PANE_COMMAND, nav.command(RIGHT, false, true, false), "a fresh RIGHT is the row's");
            same(ShellNav.PANE_COMMAND, nav.command(RIGHT, true, true, false), "and held from there it repeats on the row (a slider runs)");
            press(nav, B);
            press(nav, RIGHT);
            same(ShellNav.PANE_COMMAND, nav.command(DOWN, false, true, false), "another key after entering is the row's at once");
            same(ShellNav.PANE_COMMAND, nav.command(DOWN, true, true, false), "also when held");
            same(ShellNav.PANE_COMMAND, nav.command(RIGHT, false, true, false), "and RIGHT after it is no longer the press that entered");
        });

        test("a button pressed while that RIGHT is still held does not end the hold: its repeats still change nothing", () -> {
            // NavInput goes on repeating a held direction while A, B, Start, Y, L1 or R1 are pressed.
            for (int button : new int[] {A, START, Y}) {
                ShellNav nav = frame();
                press(nav, R1);
                same(ShellNav.ENTERED, nav.command(RIGHT, false, true, false), "RIGHT goes into the rows");
                same(ShellNav.PANE_COMMAND, nav.command(button, false, true, false), "button " + button + " during the hold is the row's");
                for (int repeats = 0; repeats < 6; ++repeats) {
                    same(ShellNav.NOTHING, nav.command(RIGHT, true, true, false), "after button " + button + " the held RIGHT is still not passed to the row (repeat " + repeats + ")");
                }
                same(ShellNav.PANE_COMMAND, nav.command(RIGHT, false, true, false), "let go and pressed again, RIGHT is the row's");
            }
            ShellNav nav = frame();
            press(nav, R1);
            nav.command(RIGHT, false, true, false);
            same(ShellNav.MOVED, press(nav, B), "B during the hold goes back to the category's name");
            same(ShellNav.NOTHING, nav.command(RIGHT, true, true, false), "and the held RIGHT does not go into the rows a second time");
            same(ShellNav.COLUMN, nav.level(), "the highlight stays on the name");
            same(ShellNav.ENTERED, press(nav, A), "A then goes into the rows");
            same(ShellNav.NOTHING, nav.command(RIGHT, true, true, false), "where the RIGHT that is held since before still changes nothing");
            same(ShellNav.TAB, press(nav, R1), "R1 during the hold changes the tab");
            same(ShellNav.NOTHING, nav.command(RIGHT, true, true, false), "and the held RIGHT does nothing on the next tab either");
            same(ShellNav.COLUMN, nav.level(), "its highlight stays on its item");
            ShellNav strip = frame();
            press(strip, R1);
            strip.command(RIGHT, false, true, false);
            press(strip, B);
            press(strip, B);
            same(ShellNav.STRIP, strip.level(), "two B during the hold: the tab strip");
            same(ShellNav.NOTHING, strip.command(RIGHT, true, true, false), "where the held RIGHT does not go on to the next tab");
            same(ShellNav.SETTINGS, strip.tab(), "still Settings");
        });

        test("memory: every tab keeps its column item and whether the highlight was in its pane", () -> {
            ShellNav nav = frame();
            press(nav, R1);
            press(nav, DOWN);
            press(nav, DOWN);
            press(nav, A);
            at(nav, ShellNav.SETTINGS, ShellNav.PANE, 2, "in the rows of the third category");
            press(nav, R1);
            press(nav, R1);
            press(nav, DOWN);
            at(nav, ShellNav.ABOUT, ShellNav.COLUMN, 1, "About, second section");
            press(nav, L1);
            at(nav, ShellNav.CONTROLLER, ShellNav.COLUMN, 0, "Controller");
            press(nav, L1);
            at(nav, ShellNav.SETTINGS, ShellNav.PANE, 2, "back in the rows of the third category");
            press(nav, R1);
            press(nav, R1);
            at(nav, ShellNav.ABOUT, ShellNav.COLUMN, 1, "About remembers its section");
        });

        test("a column that got shorter keeps its remembered item inside; place() puts the highlight where the page says", () -> {
            ShellNav nav = frame();
            nav.place(ShellNav.SETTINGS, 4, true);
            nav.setColumnCount(ShellNav.SETTINGS, 3);
            same(2, nav.column(ShellNav.SETTINGS), "moved to the last item");
            nav.place(ShellNav.SETTINGS, 9, false);
            same(2, nav.column(ShellNav.SETTINGS), "place() does not go past the end");
            nav.place(ShellNav.SETTINGS, -1, true);
            same(0, nav.column(ShellNav.SETTINGS), "or before the start");
            press(nav, R1);
            same(ShellNav.PANE, nav.level(), "placed into the pane");
        });

        test("when the page puts the highlight on a row, it leaves the tab strip; a page that only says where its highlight is does not", () -> {
            ShellNav nav = frame();
            press(nav, R1);
            press(nav, UP);
            same(ShellNav.STRIP, nav.level(), "the highlight is on the tab strip of Settings");
            nav.place(ShellNav.SETTINGS, 2, false);
            same(ShellNav.STRIP, nav.level(), "a settings read that ends meanwhile leaves it there");
            nav.intoPane(ShellNav.SETTINGS, 4);
            at(nav, ShellNav.SETTINGS, ShellNav.PANE, 4, "a row was tapped, or Restore ended on Save: the highlight is on that row");
            press(nav, UP);   // the page's own command; the frame is still in the pane
            press(nav, B);
            press(nav, B);
            same(ShellNav.STRIP, nav.level(), "back on the strip");
            press(nav, R1);
            nav.intoPane(ShellNav.SETTINGS, 1);
            same(ShellNav.STRIP, nav.level(), "what the Settings page does while another tab is shown does not move this tab's highlight");
            press(nav, L1);
            same(1, nav.column(ShellNav.SETTINGS), "but Settings remembers its own place");
        });

        test("unsaved settings: changing the tab or B on the categories asks first; Save or Discard then does it", () -> {
            ShellNav nav = frame();
            press(nav, R1);
            press(nav, A);
            same(ShellNav.ASK, nav.command(R1, false, true, true), "R1 with unsaved changes");
            check(nav.asking(), "a question is open");
            at(nav, ShellNav.SETTINGS, ShellNav.PANE, 0, "nothing moved yet");
            same(ShellNav.TAB, nav.leaveGranted(), "granted: the tab changes");
            same(ShellNav.CONTROLLER, nav.tab(), "to the tab that was asked for");
            check(!nav.asking(), "the question is closed");
            same(ShellNav.NOTHING, nav.leaveGranted(), "granting again does nothing");

            nav = frame();
            press(nav, R1);
            same(ShellNav.ASK, nav.command(L1, false, true, true), "L1 with unsaved changes");
            nav.leaveCancelled();
            check(!nav.asking(), "cancelled");
            same(ShellNav.SETTINGS, nav.tab(), "Keep editing: still Settings");
            same(ShellNav.NOTHING, nav.leaveGranted(), "a cancelled question cannot be granted later");
            same(ShellNav.SETTINGS, nav.tab(), "still Settings");

            same(ShellNav.ASK, nav.command(B, false, true, true), "B on the categories with unsaved changes");
            same(ShellNav.COLUMN, nav.level(), "not on the strip yet");
            same(ShellNav.MOVED, nav.leaveGranted(), "granted: the highlight goes to the strip");
            at(nav, ShellNav.SETTINGS, ShellNav.STRIP, 0, "on the strip, same tab");

            same(ShellNav.ASK, nav.command(RIGHT, false, true, true), "RIGHT on the strip with unsaved changes");
            same(ShellNav.SETTINGS, nav.tab(), "not left yet");
            same(ShellNav.ASK, nav.tapTab(ShellNav.PLAY, true), "a tap on another tab asks too");
            same(ShellNav.TAB, nav.leaveGranted(), "granted");
            same(ShellNav.PLAY, nav.tab(), "the tab tapped last");
        });

        test("unsaved settings do not hold back what stays inside the tab or another tab's moves", () -> {
            ShellNav nav = frame();
            press(nav, R1);
            same(ShellNav.MOVED, nav.command(DOWN, false, true, true), "DOWN on the categories");
            same(ShellNav.ENTERED, nav.command(A, false, true, true), "A into the rows");
            same(ShellNav.MOVED, nav.command(B, false, true, true), "B from the rows to the categories");
            same(ShellNav.MOVED, nav.command(UP, false, true, true), "UP to the first category");
            same(ShellNav.MOVED, nav.command(UP, false, true, true), "UP to the strip");
            same(ShellNav.MOVED, nav.command(DOWN, false, true, true), "DOWN back into the page");
            check(!nav.asking(), "no question was put");
            ShellNav other = frame();
            same(ShellNav.TAB, other.command(R1, false, false, true), "leaving Play is never asked about");
            same(ShellNav.MOVED, press(frame(), B), "B on Play goes to the strip without a question");
        });

        test("touch: a tap on a tab selects it and puts the highlight in its page; a tap on a column item selects it", () -> {
            ShellNav nav = frame();
            same(ShellNav.TAB, nav.tapTab(ShellNav.ABOUT, false), "tap About");
            at(nav, ShellNav.ABOUT, ShellNav.COLUMN, 0, "About, on the column");
            same(ShellNav.NOTHING, nav.tapTab(ShellNav.ABOUT, false), "a tap on the current tab changes nothing");
            press(nav, UP);
            same(ShellNav.MOVED, nav.tapTab(ShellNav.ABOUT, false), "from the strip a tap on the current tab goes into the page");
            same(ShellNav.COLUMN, nav.level(), "the column");
            same(ShellNav.NOTHING, nav.tapTab(7, false), "no such tab");
            press(nav, UP);
            same(ShellNav.TAB, nav.tapTab(ShellNav.SETTINGS, false), "from the strip a tap on another tab");
            same(ShellNav.COLUMN, nav.level(), "goes into its page");
            press(nav, A);
            same(ShellNav.MOVED, nav.tapColumn(3, true), "a tap on another category");
            at(nav, ShellNav.SETTINGS, ShellNav.COLUMN, 3, "takes the highlight out of the rows onto it, and does not go in");
            same(ShellNav.NOTHING, nav.tapColumn(5, true), "no such item");
            same(3, nav.column(ShellNav.SETTINGS), "unchanged");
        });

        test("touch: a tap on an action activates it immediately; pane items can be selected before entering", () -> {
            ShellNav nav = frame();
            same(ShellNav.ACTIVATE, nav.tapColumn(0, false), "Play is highlighted at the start: a tap starts it");
            press(nav, UP);
            same(ShellNav.ACTIVATE, nav.tapColumn(0, false), "one tap on Play from the strip activates it immediately");
            same(ShellNav.COLUMN, nav.level(), "the action remains the highlighted item");
            press(nav, R1);
            same(ShellNav.MOVED, nav.tapColumn(2, true), "a category that is not highlighted: the highlight moves");
            same(ShellNav.COLUMN, nav.level(), "and stays on its name");
            same(ShellNav.ENTERED, nav.tapColumn(2, true), "tapped again: into its rows");
            same(ShellNav.PANE, nav.level(), "in the rows");
            same(ShellNav.MOVED, nav.tapColumn(2, true), "from the rows a tap on the category's name goes back to the name");
            same(ShellNav.COLUMN, nav.level(), "on the name");
            same(ShellNav.ACTIVATE, nav.tapColumn(2, false), "a highlighted item without a pane to enter is carried out");
        });

        test("S-9 a pane that cannot be entered any more does not keep the highlight: the next command finds it on the column item", () -> {
            // The settings rows are read again (after Discard), or an About text became short: the page already draws
            // the highlight on the item's name, and the keys must act there.
            ShellNav nav = frame();
            press(nav, R1);
            press(nav, DOWN);
            press(nav, DOWN);
            press(nav, A);
            at(nav, ShellNav.SETTINGS, ShellNav.PANE, 2, "in the rows of the third category");
            same(ShellNav.MOVED, nav.command(DOWN, false, false, false), "the rows are gone: DOWN is not given to them, it moves to the next category");
            at(nav, ShellNav.SETTINGS, ShellNav.COLUMN, 3, "the highlight is on the fourth category's name");
            check(!nav.inPane(ShellNav.SETTINGS), "and the tab no longer says that it is in the rows");

            for (int command : new int[] {LEFT, RIGHT, A, START, Y}) {
                ShellNav gone = frame();
                press(gone, R1);
                press(gone, A);
                int answer = gone.command(command, false, false, false);
                check(answer != ShellNav.PANE_COMMAND, "command " + command + " is not given to rows that do not exist (answer " + answer + ")");
                same(ShellNav.COLUMN, gone.level(), "command " + command + ": the highlight is on the category's name");
            }
            ShellNav back = frame();
            press(back, R1);
            press(back, A);
            same(ShellNav.MOVED, back.command(B, false, false, false), "B goes from the category's name to the strip in one press, not in two");
            same(ShellNav.STRIP, back.level(), "the strip");

            // A tab that is only passed through keeps what it remembers: the rows are usually back when it is shown again.
            ShellNav passing = frame();
            press(passing, R1);
            press(passing, A);
            same(ShellNav.TAB, passing.command(R1, false, false, false), "R1 while the rows are being read");
            same(ShellNav.TAB, passing.command(L1, false, false, false), "and back");
            at(passing, ShellNav.SETTINGS, ShellNav.PANE, 0, "the highlight is in the rows again");
            press(passing, UP);
            same(ShellNav.PANE, passing.level(), "and with the rows there, commands are theirs");
        });

        test("S-13 wentToStrip: true only for the command that took the highlight from an item up to the strip", () -> {
            ShellNav nav = frame();
            check(!nav.wentToStrip(), "a new frame");
            press(nav, UP);
            check(nav.wentToStrip(), "UP from the Play item");
            for (int nothing : new int[] {UP, START, Y, B}) {
                press(nav, nothing);
                check(!nav.wentToStrip(), "command " + nothing + " on the strip does nothing: the highlight has not left anything");
            }
            press(nav, UP);
            press(nav, RIGHT);
            check(!nav.wentToStrip(), "changing the tab on the strip is not going to the strip");
            press(nav, DOWN);
            check(!nav.wentToStrip(), "DOWN into the page");
            press(nav, B);
            check(nav.wentToStrip(), "B from a column item");
            press(nav, A);
            check(!nav.wentToStrip(), "A into the page");
            press(nav, UP);
            check(nav.wentToStrip(), "UP from the first item");
            nav.tapTab(7, false);
            check(!nav.wentToStrip(), "a tap that hits no tab has taken the highlight nowhere");
            press(nav, DOWN);
            press(nav, UP);
            nav.tapColumn(9, true);
            check(!nav.wentToStrip(), "nor has a tap that hits no column item");
            press(nav, DOWN);
            same(ShellNav.ASK, nav.command(B, false, true, true), "B on a category with unsaved changes asks first");
            check(!nav.wentToStrip(), "asked, not gone yet");
            same(ShellNav.MOVED, nav.leaveGranted(), "granted");
            check(nav.wentToStrip(), "now it went");
            same(ShellNav.NOTHING, nav.leaveGranted(), "granting again does nothing");
            check(!nav.wentToStrip(), "and reports nothing");
        });

        test("S-14 no trap: from every place reached by any keys, B leads back to the strip, L1 / R1 change the tab, the column item exists", () -> {
            // 300 walks of 200 random commands and taps (fixed seeds), with the page's two answers (can the pane be
            // entered, are there unsaved changes) changing under the walk as they do in the app.
            int[] counts = {1, 5, 1, 4};
            for (int seed = 0; seed < 300; ++seed) {
                java.util.Random random = new java.util.Random(seed);
                ShellNav nav = frame();
                StringBuilder trail = new StringBuilder();
                for (int step = 0; step < 200; ++step) {
                    boolean enterable = (nav.tab() == ShellNav.SETTINGS || nav.tab() == ShellNav.ABOUT) && random.nextInt(4) != 0;
                    boolean unsaved = nav.tab() == ShellNav.SETTINGS && random.nextInt(5) == 0;
                    int kind = random.nextInt(12);
                    if (kind == 10) {
                        nav.tapTab(random.nextInt(ShellNav.TABS), unsaved);
                        trail.append(" tapTab");
                    } else if (kind == 11) {
                        nav.tapColumn(random.nextInt(5), enterable);
                        trail.append(" tapColumn");
                    } else {
                        int command = 1 + kind;
                        int answer = nav.command(command, command <= RIGHT && random.nextInt(3) == 0, enterable, unsaved);
                        trail.append(' ').append(command);
                        if (answer != ShellNav.TAB && answer != ShellNav.ASK && !enterable) {
                            check(nav.level() != ShellNav.PANE, "walk " + seed + ":" + trail + ": the highlight is in a pane that cannot be entered");
                        }
                    }
                    if (nav.asking()) {
                        if (random.nextBoolean()) nav.leaveGranted(); else nav.leaveCancelled();
                    }
                    int tab = nav.tab();
                    check(nav.column(tab) >= 0 && nav.column(tab) < counts[tab], "walk " + seed + ":" + trail + ": column item " + nav.column(tab) + " of tab " + tab + " does not exist");
                    check(nav.level() != ShellNav.PANE || tab == ShellNav.SETTINGS || tab == ShellNav.ABOUT, "walk " + seed + ":" + trail + ": in the pane of a tab that has none");
                }
                int tab = nav.tab(), level = nav.level();
                same(ShellNav.TAB, nav.command(R1, false, true, false), "walk " + seed + ":" + trail + ": R1 changes the tab");
                same((tab + 1) % ShellNav.TABS, nav.tab(), "walk " + seed + ": to the next one");
                same(ShellNav.TAB, nav.command(L1, false, true, false), "walk " + seed + ":" + trail + ": L1 changes it back");
                same(level, nav.level(), "walk " + seed + ": to where the highlight was");
                for (int presses = level; presses > 0; --presses) {
                    same(ShellNav.MOVED, nav.command(B, false, true, false), "walk " + seed + ":" + trail + ": B goes one level up from level " + presses);
                }
                same(ShellNav.STRIP, nav.level(), "walk " + seed + ":" + trail + ": as many B as levels reach the strip");
                same(ShellNav.NOTHING, nav.command(B, false, true, false), "walk " + seed + ": where B does nothing more (the launcher is never left by B)");
            }
        });

        test("route: who gets a command before the highlight is asked", () -> {
            for (int command = UP; command <= Y; ++command) {
                same(ShellNav.TO_SETTINGS_MODEL, ShellNav.route(ShellNav.SETTINGS, command, true, true, false),
                        "an open settings question takes command " + command + " (L1 / R1 cannot leave under it)");
                same(ShellNav.TO_NAV, ShellNav.route(ShellNav.ABOUT, command, true, true, true), "another tab's command " + command + " is the highlight's");
            }
            same(ShellNav.TO_SETTINGS_MODEL, ShellNav.route(ShellNav.SETTINGS, START, false, true, false), "Start saves from anywhere on the Settings tab");
            same(ShellNav.TO_SETTINGS_MODEL, ShellNav.route(ShellNav.SETTINGS, Y, false, true, false), "Y restores from anywhere on the Settings tab");
            same(ShellNav.TO_NAV, ShellNav.route(ShellNav.SETTINGS, START, false, false, false), "not while the rows are still being read");
            same(ShellNav.TO_NAV, ShellNav.route(ShellNav.SETTINGS, B, false, true, false), "B is the highlight's");
            same(ShellNav.TO_NAV, ShellNav.route(ShellNav.PLAY, START, false, true, false), "Start on Play does not save settings");
            same(ShellNav.TO_PLAY_QUESTION, ShellNav.route(ShellNav.PLAY, B, false, false, true), "B answers Play's open question");
            same(ShellNav.TO_NAV, ShellNav.route(ShellNav.PLAY, A, false, false, true), "A on Play's question is the action's (start anyway)");
            same(ShellNav.TO_NAV, ShellNav.route(ShellNav.PLAY, B, false, false, false), "B without a question moves the highlight");
            same(ShellNav.TO_NAV, ShellNav.route(ShellNav.CONTROLLER, B, false, false, true), "the question is Play's only");
        });

        end();
    }
}
