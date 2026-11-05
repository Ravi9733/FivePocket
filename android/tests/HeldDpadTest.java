package dev.recompbootstrap.smoke;

/**
 * vm-pad-hold D6: D-pad DOWN held 4.8 s, the stick pushed UP from 0.75 s to 2.28 s: after the stick let go the
 * still-held D-pad did nothing. The repeat now goes back to the direction another source still holds, from its first
 * repeat (400 ms later), without a press of its own.
 */
public final class HeldDpadTest {
    private static final String S = "HeldDpad";

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "the vm-pad-hold case: HAT DOWN 0-4800 ms, stick UP 750-2280 ms: DOWN repeats again after the stick", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 1f, 0f, 0f);
            rig.at(750);
            rig.input.axes(0f, 1f, 0f, -0.8f);
            MenuFixKit.eq("DOWN@0 DOWN+@400 DOWN+@510 DOWN+@620 DOWN+@730 UP@750", rig.take(), "before the stick lets go");
            rig.at(2280);
            rig.take();
            rig.input.axes(0f, 1f, 0f, 0f);
            MenuFixKit.eq("", rig.take(), "the stick's release itself sends nothing");
            rig.at(2900);
            MenuFixKit.eq("DOWN+@2680 DOWN+@2790 DOWN+@2900", rig.take(), "the held D-pad repeats again, 400 ms after, then every 110 ms");
            rig.at(4800);
            rig.take();
            rig.input.axes(0f, 0f, 0f, 0f);
            rig.at(7000);
            MenuFixKit.eq("", rig.take(), "the D-pad let go: nothing more");
        });
        MenuFixKit.test(S, "a D-pad key held under the stick also takes the repeat back", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.key(20, true, 0);
            rig.input.axes(0f, 0f, 0.9f, 0f);
            rig.at(100);
            rig.input.axes(0f, 0f, 0f, 0f);
            rig.at(500);
            MenuFixKit.eq("DOWN@0 RIGHT@0 DOWN+@500", rig.take(), "the key's DOWN after the stick's RIGHT");
            rig.input.key(20, false, 0);
            rig.at(2000);
            MenuFixKit.eq("", rig.take(), "key up: nothing more");
        });
        MenuFixKit.test(S, "control: one clean D-pad press is one step, and a lone stick release leaves nothing repeating", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 1f, 0f, 0f);
            rig.at(150);
            rig.input.axes(0f, 0f, 0f, 0f);
            rig.input.axes(0f, 0f, 0f, -0.9f);
            rig.at(300);
            rig.input.axes(0f, 0f, 0f, 0f);
            rig.at(3000);
            MenuFixKit.eq("DOWN@0 UP@150", rig.take(), "one step each");
        });
        MenuFixKit.test(S, "ABOUT column (5 items): the still-held DOWN reaches the last item after the stick's UP", () -> {
            final ShellNav nav = new ShellNav();
            nav.setColumnCount(ShellNav.ABOUT, 5);
            nav.command(NavInput.PAGE_PREV, false, false, false);   // PLAY -> ABOUT (a fresh press wraps)
            MenuFixKit.Rig rig = new MenuFixKit.Rig() {
                @Override void consume(int command, boolean repeat) {
                    nav.command(command, repeat, false, false);
                }
            };
            rig.input.axes(0f, 1f, 0f, 0f);
            rig.at(750);
            rig.input.axes(0f, 1f, 0f, -0.8f);
            rig.at(2280);
            MenuFixKit.eq(0, nav.column(ShellNav.ABOUT), "the stick's UP took the highlight back to the first item");
            rig.input.axes(0f, 1f, 0f, 0f);
            rig.at(4800);
            MenuFixKit.eq(4, nav.column(ShellNav.ABOUT), "the held D-pad took it down to the last item (it stops there)");
        });
        MenuFixKit.test(S, "minor 2: a held LEFT under the stick is not taken back when the stick lets go; a new press works", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(-1f, 0f, 0f, 0f);
            rig.at(100);
            rig.input.axes(-1f, 0f, 0f, 1f);
            rig.at(400);
            rig.input.axes(-1f, 0f, 0f, 0f);
            rig.at(3000);
            MenuFixKit.eq("LEFT@0 DOWN@100", rig.take(), "no LEFT after the stick (it would change a value on the row DOWN moved to)");
            rig.input.axes(0f, 0f, 0f, 0f);
            rig.input.axes(-1f, 0f, 0f, 0f);
            MenuFixKit.eq("LEFT@3000", rig.take(), "LEFT pressed again");
        });
        MenuFixKit.test(S, "minor 2: with a key RIGHT and the HAT DOWN held under the stick, DOWN is taken back, RIGHT is not", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.key(22, true, 0);
            rig.input.axes(0f, 1f, 0f, 0f);
            rig.input.axes(0f, 1f, 0f, -1f);
            rig.at(100);
            rig.input.axes(0f, 1f, 0f, 0f);
            rig.at(610);
            MenuFixKit.eq("RIGHT@0 DOWN@0 UP@0 DOWN+@500 DOWN+@610", rig.take(), "DOWN repeats again, RIGHT never");
        });
        MenuFixKit.finish(S);
    }
}
