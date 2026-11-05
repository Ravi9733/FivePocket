package dev.recompbootstrap.smoke;

/**
 * menu-input-fixes round 3, M2 and minor 4. The stated rule: one stick push gives directions of one axis; a push is
 * over when its own axis is back inside STICK_RELEASE (0.35) and the other axis is inside STICK_PRESS (0.55), or the
 * stick is at rest. A stick that rests 0 to 0.45 off the centre on either axis (either sign) still gives one command
 * for every push on both axes. Out of scope, as today: a stick that rests beyond 0.35 on the axis of a push it just
 * made still holds that push (the release band); that push is made last here.
 */
public final class StickDriftTest {
    private static final String S = "StickDrift";

    /** The pushes for a rest at (rx, ry): the other axis both ways, then this axis away from the drift, then toward it. */
    private static String pushes(float rx, float ry) {
        boolean xDrift = rx != 0f;
        float d = xDrift ? rx : ry;
        float[][] at;
        String[] names;
        if (xDrift) {
            at = new float[][] {{rx, -1f}, {rx, 1f}, {d > 0 ? -1f : 1f, 0f}, {d > 0 ? 1f : -1f, 0f}};
            names = new String[] {"UP", "DOWN", d > 0 ? "LEFT" : "RIGHT", d > 0 ? "RIGHT" : "LEFT"};
        } else {
            at = new float[][] {{-1f, ry}, {1f, ry}, {0f, d > 0 ? -1f : 1f}, {0f, d > 0 ? 1f : -1f}};
            names = new String[] {"LEFT", "RIGHT", d > 0 ? "UP" : "DOWN", d > 0 ? "DOWN" : "UP"};
        }
        final StringBuilder fresh = new StringBuilder();
        NavInputTest.FakeTimer timer = new NavInputTest.FakeTimer();
        NavInput input = new NavInput((command, repeat) -> { if (!repeat) fresh.append(MenuFixKit.NAMES[command]).append(' '); }, timer);
        input.axes(0f, 0f, rx, ry);
        for (float[] push : at) {
            timer.advance(100);
            input.axes(0f, 0f, push[0], push[1]);
            timer.advance(100);
            input.axes(0f, 0f, rx, ry);   // back to the drifted rest
        }
        return fresh.toString().trim() + " | expected " + String.join(" ", names);
    }

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "rests 0 to 0.45 off the centre, on x and on y, both signs: every push on both axes gives its one command", () -> {
            int cases = 0;
            StringBuilder wrong = new StringBuilder();
            for (int step = 0; step <= 9; ++step) {
                float o = step * 0.05f;
                float[][] rests = {{o, 0f}, {-o, 0f}, {0f, o}, {0f, -o}};
                for (float[] rest : rests) {
                    if (o == 0f && rest != rests[0]) continue;
                    String got = rest[0] == 0f && rest[1] == 0f ? pushes(0.0001f, 0f) : pushes(rest[0], rest[1]);
                    String[] parts = got.split(" \\| expected ");
                    cases++;
                    if (!parts[0].equals(parts[1])) wrong.append("\n  rest (").append(rest[0]).append(", ").append(rest[1]).append("): ").append(got);
                }
            }
            MenuFixKit.eq(37, cases, "cases");
            MenuFixKit.check(wrong.length() == 0, "pushes lost or added:" + wrong);
        });
        MenuFixKit.test(S, "the checker's M4a / M4c: rest at x 0.36, UP then RIGHT; rest at y 0.40, RIGHT then DOWN", () -> {
            MenuFixKit.Rig a = new MenuFixKit.Rig();
            a.input.axes(0f, 0f, 0.36f, 0f);
            a.at(100);
            a.input.axes(0f, 0f, 0.36f, -1f);
            a.at(200);
            a.input.axes(0f, 0f, 0.36f, 0f);
            a.at(500);
            a.input.axes(0f, 0f, 1f, 0f);
            MenuFixKit.eq("UP@100 RIGHT@500", a.take(), "x drift 0.36");
            MenuFixKit.Rig c = new MenuFixKit.Rig();
            c.input.axes(0f, 0f, 0f, 0.40f);
            c.at(100);
            c.input.axes(0f, 0f, 1f, 0.40f);
            c.at(200);
            c.input.axes(0f, 0f, 0f, 0.40f);
            c.at(500);
            c.input.axes(0f, 0f, 0f, 1f);
            MenuFixKit.eq("RIGHT@100 DOWN@500", c.take(), "y drift 0.40");
        });
        MenuFixKit.test(S, "minor 4: UP, then on through (0.4, -0.2) to the right: UP then RIGHT", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 0f, 0f, -1f);
            rig.at(16);
            rig.input.axes(0f, 0f, 0.4f, -0.2f);
            rig.at(32);
            rig.input.axes(0f, 0f, 1f, 0f);
            MenuFixKit.eq("UP@0 RIGHT@32", rig.take(), "a second push");
        });
        MenuFixKit.test(S, "control (M4): the sweep (0.3,0.3) (0.6,0.6) (0.9,0.4) (1,0) is still one push: one DOWN", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 0f, 0.3f, 0.3f);
            rig.input.axes(0f, 0f, 0.6f, 0.6f);
            rig.input.axes(0f, 0f, 0.9f, 0.4f);
            rig.input.axes(0f, 0f, 1f, 0f);
            MenuFixKit.eq("DOWN@0", rig.take(), "one command");
        });
        MenuFixKit.finish(S);
    }
}
