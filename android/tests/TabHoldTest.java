package dev.recompbootstrap.smoke;

/**
 * vm-pad-hold D5: D-pad DOWN held 3.26 s, RB pressed 0.54 s into it (CONTROLLER -> ABOUT): the ABOUT column ran from
 * This App to Captures. A tab change by L1 / R1 now ends the hold (LauncherScreen: ShellNav.endsHold -> host.endHold
 * -> NavInput.endHold); the strip's own walk with a held LEFT / RIGHT goes on as designed.
 */
public final class TabHoldTest {
    private static final String S = "TabHold";

    /** NavInput feeding ShellNav, with LauncherScreen's rule for a tab change. */
    private static final class Shell extends MenuFixKit.Rig {
        final ShellNav nav = new ShellNav();

        Shell() {
            nav.setColumnCount(ShellNav.CONTROLLER, 1);
            nav.setColumnCount(ShellNav.ABOUT, 5);
        }

        @Override void consume(int command, boolean repeat) {
            int answer = nav.command(command, repeat, false, false);
            if (ShellNav.endsHold(answer, command)) input.endHold();
        }
    }

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "the vm-pad-hold case: DOWN held 3.26 s on CONTROLLER, RB at 0.54 s: ABOUT stays on This App", () -> {
            Shell shell = new Shell();
            shell.nav.command(NavInput.PAGE_PREV, false, false, false);   // PLAY -> ABOUT
            shell.nav.command(NavInput.PAGE_PREV, false, false, false);   // ABOUT -> CONTROLLER
            MenuFixKit.eq(ShellNav.CONTROLLER, shell.nav.tab(), "on CONTROLLER");
            shell.input.axes(0f, 1f, 0f, 0f);
            shell.at(540);
            shell.input.key(103, true, 0);   // RB
            shell.input.key(103, false, 0);
            shell.at(3260);
            MenuFixKit.eq(ShellNav.ABOUT, shell.nav.tab(), "RB brought ABOUT");
            MenuFixKit.eq(0, shell.nav.column(ShellNav.ABOUT), "the ABOUT column did not run");
            MenuFixKit.eq("DOWN@0 DOWN+@400 DOWN+@510 PAGE_NEXT@540", shell.take(), "nothing after RB");
            shell.input.axes(0f, 1f, 0f, 0f);
            MenuFixKit.eq("", shell.take(), "the same HAT value reported again: still nothing");
            shell.input.axes(0f, 0f, 0f, 0f);
            shell.input.axes(0f, 1f, 0f, 0f);
            MenuFixKit.eq("DOWN@3260", shell.take(), "let go and pressed again: a step at once");
            MenuFixKit.eq(1, shell.nav.column(ShellNav.ABOUT), "on Game Build");
        });
        MenuFixKit.test(S, "a stick resting at DOWN through LB / RB: its jitter presses nothing on the new tab", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 0f, 0f, 0.9f);
            rig.at(100);
            rig.input.endHold();
            rig.input.axes(0f, 0f, 0.02f, 0.88f);
            rig.input.axes(0f, 0f, -0.01f, 0.91f);
            rig.at(2000);
            MenuFixKit.eq("DOWN@0", rig.take(), "jitter after endHold");
            rig.input.axes(0f, 0f, 0f, 0.2f);
            rig.input.axes(0f, 0f, 0f, 0.9f);
            MenuFixKit.eq("DOWN@2000", rig.take(), "let go and pushed again");
        });
        MenuFixKit.test(S, "after endHold a fresh other direction works at once and repeats", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 1f, 0f, 0f);
            rig.input.endHold();
            rig.input.axes(0f, 1f, 0f, -0.9f);   // the HAT still holds DOWN
            rig.at(400);
            MenuFixKit.eq("DOWN@0 UP@0 UP+@400", rig.take(), "the stick's UP");
            rig.input.axes(0f, 1f, 0f, 0f);
            rig.at(2000);
            MenuFixKit.eq("", rig.take(), "the ended DOWN does not take the repeat back when the stick lets go");
        });
        MenuFixKit.test(S, "control: RIGHT held on the strip still walks the tabs to the end (no hold ended by it)", () -> {
            Shell shell = new Shell();
            shell.nav.command(NavInput.UP, false, false, false);   // PLAY column -> strip
            shell.input.axes(1f, 0f, 0f, 0f);
            shell.at(2532);
            MenuFixKit.eq(ShellNav.ABOUT, shell.nav.tab(), "walked to ABOUT and stopped");
            MenuFixKit.check(!ShellNav.endsHold(ShellNav.TAB, NavInput.RIGHT), "a direction's tab change ends nothing");
            MenuFixKit.check(ShellNav.endsHold(ShellNav.TAB, NavInput.PAGE_NEXT) && ShellNav.endsHold(ShellNav.TAB, NavInput.PAGE_PREV),
                    "L1 / R1 tab changes end the hold");
            MenuFixKit.check(ShellNav.endsHold(ShellNav.TAB, NavInput.NONE), "a tap or a settings answer that changed the tab too");
            MenuFixKit.check(!ShellNav.endsHold(ShellNav.MOVED, NavInput.PAGE_NEXT) && !ShellNav.endsHold(ShellNav.ASK, NavInput.PAGE_NEXT),
                    "no tab change, no end");
        });
        MenuFixKit.test(S, "round 3 minor 6: the stick held LEFT when a question opens; a D-pad LEFT press counts, and its release ends the repeat", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 0f, -1f, 0f);
            rig.at(100);
            rig.input.endHold();                       // the question opened
            rig.at(700);
            rig.input.axes(-1f, 0f, -1f, 0f);          // D-pad LEFT, the stick still held
            rig.at(1150);
            rig.input.axes(0f, 0f, -1f, 0f);           // D-pad let go, the stick still held
            rig.at(3000);
            MenuFixKit.eq("LEFT@0 LEFT@700 LEFT+@1100", rig.take(), "the D-pad press and its own repeat; nothing after its release");
        });
        MenuFixKit.finish(S);
    }
}
