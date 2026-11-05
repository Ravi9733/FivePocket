package dev.recompbootstrap.smoke;

/**
 * menu-input-fixes M3: with D3 every batched HAT sample is read, so a pad that reports its D-pad both as keys (at
 * once) and as the HAT (with the next frame's batch) counted a short tap twice. NavInput pairs a D-pad key and a HAT
 * press of one direction by time (NavInput.PAIR_MS): one physical press gives one command whichever way, or both
 * ways, the pad reports it. Times are the events' own (KeyEvent.getEventTime, MotionEvent.getHistoricalEventTime).
 */
public final class PadKindsTest {
    private static final String S = "PadKinds";
    private static final int KEY_DOWN = 20, KEY_RIGHT = 22;

    /** A joystick event of HAT samples {time, hatX, hatY}; the stick rests. */
    private static NavInput.Motion hat(final float[]... samples) {
        return new NavInput.Motion() {
            @Override public int history() {
                return samples.length - 1;
            }

            @Override public float value(int axis, int position) {
                if (axis == NavInput.AXIS_HAT_X) return samples[position][1];
                if (axis == NavInput.AXIS_HAT_Y) return samples[position][2];
                return 0f;
            }

            @Override public long time(int position) {
                return (long) samples[position][0];
            }
        };
    }

    private static float[] at(long time, float x, float y) {
        return new float[] {time, x, y};
    }

    /** A tap of length ms at t0 on a pad of the given kind; the HAT batch arrives at frame (after the keys). */
    private static String tap(boolean keys, boolean hatAxis, long t0, long ms, long frame) {
        MenuFixKit.Rig rig = new MenuFixKit.Rig();
        rig.at(t0);
        if (keys) rig.input.key(KEY_DOWN, true, 0, t0);
        if (keys && t0 + ms < frame) {
            rig.at(t0 + ms);
            rig.input.key(KEY_DOWN, false, 0, t0 + ms);
        }
        rig.at(frame);
        if (hatAxis) rig.input.motion(ms < frame - t0 ? hat(at(t0, 0, 1), at(t0 + ms, 0, 0)) : hat(at(t0, 0, 1)));
        if (keys && t0 + ms >= frame) {
            rig.at(t0 + ms);
            rig.input.key(KEY_DOWN, false, 0, t0 + ms);
        }
        if (hatAxis && t0 + ms >= frame) {
            rig.at(t0 + ms + 16);
            rig.input.motion(hat(at(t0 + ms, 0, 0)));
        }
        rig.at(t0 + 3000);
        return rig.take();
    }

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "a 10 ms tap with 16 ms frames: one DOWN on a keys-only, a HAT-only and a both-ways pad", () -> {
            MenuFixKit.eq("DOWN@1000", tap(true, false, 1000, 10, 1016), "keys only");
            MenuFixKit.eq("DOWN@1016", tap(false, true, 1000, 10, 1016), "HAT only (read when its batch comes)");
            MenuFixKit.eq("DOWN@1000", tap(true, true, 1000, 10, 1016), "keys and HAT");
        });
        MenuFixKit.test(S, "a 120 ms tap inside one 300 ms frame: one DOWN on each kind", () -> {
            MenuFixKit.eq("DOWN@1000", tap(true, false, 1000, 120, 1300), "keys only");
            MenuFixKit.eq("DOWN@1300", tap(false, true, 1000, 120, 1300), "HAT only");
            MenuFixKit.eq("DOWN@1000", tap(true, true, 1000, 120, 1300), "keys and HAT");
        });
        MenuFixKit.test(S, "a 1000 ms hold with 16 ms frames: one press and the normal repeats on each kind", () -> {
            String keys = tap(true, false, 1000, 1000, 1016);
            MenuFixKit.check(keys.startsWith("DOWN@1000 DOWN+@1400 DOWN+@1510"), "keys only: " + keys);
            MenuFixKit.check(tap(false, true, 1000, 1000, 1016).startsWith("DOWN@1016 DOWN+@1416"), "HAT only");
            MenuFixKit.eq(keys, tap(true, true, 1000, 1000, 1016), "keys and HAT: as keys only");
        });
        MenuFixKit.test(S, "two quick taps of a both-ways pad inside one slow frame: two DOWNs, not four", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.at(1000);
            rig.input.key(KEY_DOWN, true, 0, 1000);
            rig.at(1030);
            rig.input.key(KEY_DOWN, false, 0, 1030);
            rig.at(1100);
            rig.input.key(KEY_DOWN, true, 0, 1100);
            rig.at(1130);
            rig.input.key(KEY_DOWN, false, 0, 1130);
            rig.at(1300);
            rig.input.motion(hat(at(1000, 0, 1), at(1030, 0, 0), at(1100, 0, 1), at(1130, 0, 0)));
            rig.at(3000);
            MenuFixKit.eq("DOWN@1000 DOWN@1100", rig.take(), "one command per physical tap");
        });
        MenuFixKit.test(S, "the HAT's report first, the key after it (unbuffered order): still one DOWN", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.at(1000);
            rig.input.motion(hat(at(1000, 0, 1)));
            rig.at(1010);
            rig.input.motion(hat(at(1010, 0, 0)));
            rig.at(1012);
            rig.input.key(KEY_DOWN, true, 0, 1002);
            rig.input.key(KEY_DOWN, false, 0, 1011);
            rig.at(3000);
            MenuFixKit.eq("DOWN@1000", rig.take(), "one command");
        });
        MenuFixKit.test(S, "control: a key and a HAT press of the same direction far apart are two presses; other directions never pair", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.key(KEY_DOWN, true, 0, 0);
            rig.input.key(KEY_DOWN, false, 0, 10);
            rig.at(500);
            rig.input.motion(hat(at(500, 0, 1), at(510, 0, 0)));
            rig.at(600);
            rig.input.key(KEY_RIGHT, true, 0, 600);
            rig.input.key(KEY_RIGHT, false, 0, 610);
            rig.at(616);
            rig.input.motion(hat(at(600, 0, 1), at(610, 0, 0)));
            rig.at(3000);
            MenuFixKit.eq("DOWN@0 DOWN@500 RIGHT@600 DOWN@616", rig.take(), "four presses");
        });
        MenuFixKit.test(S, "round 3 M1 (the checker's M3w): RIGHT 5-200 and 250-330 ms on keys and HAT, one 300 ms frame: two RIGHTs", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.at(5);
            rig.input.key(KEY_RIGHT, true, 0, 5);
            rig.at(200);
            rig.input.key(KEY_RIGHT, false, 0, 200);
            rig.at(250);
            rig.input.key(KEY_RIGHT, true, 0, 250);
            rig.at(300);
            rig.input.motion(hat(at(5, 1, 0), at(200, 0, 0), at(250, 1, 0)));
            rig.at(330);
            rig.input.key(KEY_RIGHT, false, 0, 330);
            rig.at(600);
            rig.input.motion(hat(at(330, 0, 0)));
            rig.at(3000);
            MenuFixKit.eq("RIGHT@5 RIGHT@250", rig.take(), "one command per press; the late HAT release of press 1 leaves press 2's key alone");
        });
        MenuFixKit.test(S, "round 3 M1: the late HAT release of press 1 does not end press 2's hold: RIGHT 250-1000 ms repeats", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.at(5);
            rig.input.key(KEY_RIGHT, true, 0, 5);
            rig.at(200);
            rig.input.key(KEY_RIGHT, false, 0, 200);
            rig.at(250);
            rig.input.key(KEY_RIGHT, true, 0, 250);
            rig.at(300);
            rig.input.motion(hat(at(5, 1, 0), at(200, 0, 0), at(250, 1, 0)));
            rig.at(760);
            MenuFixKit.eq("RIGHT@5 RIGHT@250 RIGHT+@650 RIGHT+@760", rig.take(), "press 2 repeats from 650 ms");
            rig.at(1000);
            rig.input.key(KEY_RIGHT, false, 0, 1000);
            rig.at(1016);
            rig.input.motion(hat(at(1000, 0, 0)));
            rig.take();
            rig.at(3000);
            MenuFixKit.eq("", rig.take(), "released: nothing more");
        });
        MenuFixKit.test(S, "a report pairs once: a both-ways tap, then a HAT report with no key 30 ms later is a press of its own", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.key(KEY_DOWN, true, 0, 0);
            rig.input.key(KEY_DOWN, false, 0, 10);
            rig.at(100);
            rig.input.motion(hat(at(0, 0, 1), at(10, 0, 0), at(40, 0, 1), at(50, 0, 0)));
            rig.at(3000);
            MenuFixKit.eq("DOWN@0 DOWN@100", rig.take(), "the key's partner is taken: the second HAT press counts");
        });
        MenuFixKit.test(S, "a report of the other source 30 to 200 ms after a release, not paired: it counts (PAIR_MS bounds the pairing)", () -> {
            for (long gap : new long[] {30, 60, 100, 200}) {
                MenuFixKit.Rig rig = new MenuFixKit.Rig();
                rig.input.key(KEY_DOWN, true, 0, 0);
                rig.input.key(KEY_DOWN, false, 0, 80);
                long at = 80 + gap;
                rig.at(at + 16);
                rig.input.motion(hat(at(at, 0, 1), at(at + 10, 0, 0)));
                rig.at(3000);
                MenuFixKit.eq("DOWN@0 DOWN@" + (at + 16), rig.take(), "gap " + gap + " ms");
            }
        });
        MenuFixKit.finish(S);
    }
}
