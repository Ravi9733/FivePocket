package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Host test of NavInput (design section 2.2). Run with java -ea. Key codes are the numbers of android.view.KeyEvent;
 * no Android runtime is needed. Prints one line per test and exits non-zero when any test fails.
 */
public final class NavInputTest {
    private static final String[] NAMES = {"NONE", "UP", "DOWN", "LEFT", "RIGHT", "CONFIRM", "BACK", "PAGE_PREV",
            "PAGE_NEXT", "MENU", "ALT"};
    private static final int KEY_UP = 19, KEY_DOWN = 20, KEY_LEFT = 21, KEY_RIGHT = 22, KEY_A = 96;

    /** A clock the test moves by hand. It holds one pending task, as the Timer contract says. */
    static final class FakeTimer implements NavInput.Timer {
        long now;
        private Runnable task;
        private long due;
        /** The task that the latest schedule() or cancel() displaced; a correct NavInput must have made it harmless. */
        Runnable displaced;

        @Override public long now() {
            return now;
        }

        @Override public void schedule(Runnable newTask, long delayMs) {
            if (task != null) displaced = task;
            task = newTask;
            due = now + delayMs;
        }

        @Override public void cancel() {
            if (task != null) displaced = task;
            task = null;
        }

        boolean pending() {
            return task != null;
        }

        /** Moves the clock forward and runs every task that comes due on the way, at its due time. */
        void advance(long ms) {
            long end = now + ms;
            while (task != null && due <= end) {
                now = due;
                Runnable run = task;
                task = null;
                run.run();
            }
            now = end;
        }
    }

    /** Records every command with the clock time: "DOWN@0" for a press, "DOWN+@400" for a repeat. */
    private static final class Rig implements NavInput.Sink {
        final FakeTimer timer = new FakeTimer();
        final NavInput input = new NavInput(this, timer);
        final List<String> events = new ArrayList<>();
        Runnable duringEvent;

        @Override public void onNav(int command, boolean repeat) {
            events.add(NAMES[command] + (repeat ? "+" : "") + "@" + timer.now);
            if (duringEvent != null) duringEvent.run();
        }

        /** The events since the last call, joined by spaces, then forgotten. */
        String take() {
            String joined = String.join(" ", events);
            events.clear();
            return joined;
        }
    }

    private static int failures;

    private static void run(String name, Runnable test) {
        try {
            test.run();
            System.out.println("PASS NavInput: " + name);
        } catch (AssertionError | RuntimeException failure) {
            failures++;
            StackTraceElement at = failure.getStackTrace().length > 1 ? failure.getStackTrace()[1] : null;
            System.out.println("FAIL NavInput: " + name + ": " + failure + (at == null ? "" : " (" + at + ")"));
        }
    }

    private static void eq(Object expected, Object actual, String what) {
        if (!expected.equals(actual)) throw new AssertionError(what + ": expected <" + expected + "> but was <" + actual + ">");
    }

    public static void main(String[] args) {
        boolean assertions = false;
        assert assertions = true;
        if (!assertions) throw new IllegalStateException("Run with -ea");

        run("every key of the table gives its command once", NavInputTest::everyKeyOnce);
        run("Android's own key repeats give nothing", NavInputTest::androidRepeatsIgnored);
        run("an unknown key is not consumed and gives nothing", NavInputTest::unknownKey);
        run("hold-to-repeat: 400 ms, then every 110 ms, every 55 ms after 10 repeats", NavInputTest::repeatTiming);
        run("releasing the key stops the repeat", NavInputTest::releaseStops);
        run("reset() stops the repeat and forgets what was held", NavInputTest::resetStops);
        run("CONFIRM, BACK, L1, R1, Start and Y never repeat", NavInputTest::onlyDirectionsRepeat);
        run("HAT fires beyond 0.5 and releases at the centre", NavInputTest::hatThreshold);
        run("stick presses beyond 0.55 and releases inside 0.35", NavInputTest::stickHysteresis);
        run("stick: only the axis with the larger magnitude counts, one axis per push", NavInputTest::stickDominantAxis);
        run("the same direction from HAT and key is one hold, and the HAT's release ends it", NavInputTest::twoSourcesOneHold);
        run("a key hold survives its own key-up while the HAT still holds the direction", NavInputTest::keyUpWhileHatHolds);
        run("the newest direction takes over the repeat", NavInputTest::newestDirectionRepeats);
        run("a D-pad corner gives one command: keys", NavInputTest::cornerKeys);
        run("a D-pad corner gives one command: HAT", NavInputTest::cornerHat);
        run("a stick held near 45 degrees gives one press, and one push never changes to the other axis", NavInputTest::stickDiagonal);
        run("an axis value that is not a number counts as centred and does not block the other axis", NavInputTest::notANumber);
        run("a displaced timer task that still runs gives nothing", NavInputTest::staleTaskIgnored);
        run("reset() from inside the sink leaves no repeat behind", NavInputTest::resetInsideSink);

        if (failures != 0) {
            System.out.println("NAV_INPUT_FAIL: " + failures + " test(s) failed");
            System.exit(1);
        }
        System.out.println("NAV_INPUT_PASS: key table, Android repeats ignored, repeat timing, release and reset, HAT and "
                + "stick thresholds, dominant axis, one hold for two sources, stale timer tasks");
    }

    private static void cornerKeys() {
        Rig rig = new Rig();
        rig.input.key(KEY_DOWN, true, 0);
        check(rig.input.key(KEY_RIGHT, true, 0), "the second key is still consumed");
        eq("DOWN@0", rig.take(), "DOWN and RIGHT keys down together: only the first is a command");
        rig.timer.advance(400);
        eq("DOWN+@400", rig.take(), "the first direction keeps repeating");
        rig.input.key(KEY_DOWN, false, 0);
        rig.timer.advance(2000);
        eq("", rig.take(), "the ignored key never becomes a command, also not after the first was released");
        rig.input.key(KEY_RIGHT, false, 0);
        rig.input.key(KEY_RIGHT, true, 0);
        eq("RIGHT@2400", rig.take(), "once everything was released, the next press counts again");
    }

    private static void cornerHat() {
        Rig rig = new Rig();
        rig.input.axes(1f, 1f, 0f, 0f);
        eq("DOWN@0", rig.take(), "HAT (1,1) in one event: one command, the vertical one");
        rig.input.axes(1f, 0f, 0f, 0f);
        eq("RIGHT@0", rig.take(), "the thumb rolled on to RIGHT alone: DOWN was let go, RIGHT is a press");
        rig.input.axes(1f, -1f, 0f, 0f);
        eq("", rig.take(), "rocked on to the upper corner while RIGHT is held: nothing");
        rig.timer.advance(400);
        eq("RIGHT+@400", rig.take(), "RIGHT is still the held direction");
        rig.input.axes(0f, 0f, 0f, 0f);
        rig.input.axes(0f, 1f, 0f, 0f);
        rig.input.axes(-1f, 1f, 0f, 0f);
        eq("DOWN@400", rig.take(), "straight DOWN, then the lower-left corner: one DOWN, no LEFT");
        rig.input.axes(0f, 0f, 0f, 0f);
        rig.timer.advance(2000);
        eq("", rig.take(), "after the release");
    }

    private static void stickDiagonal() {
        Rig rig = new Rig();
        for (int event = 0; event < 25; ++event) {
            if (event % 2 == 0) rig.input.axes(0f, 0f, 0.70f, 0.69f);
            else rig.input.axes(0f, 0f, 0.69f, 0.70f);
            rig.timer.advance(8);
        }
        // Near 45 degrees the stick goes vertical, as the D-pad's corner does (menu-input-fixes D2).
        eq("DOWN@0", rig.take(), "25 events wobbling around 45 degrees within 200 ms: one press, the vertical one");
        // One push gives one direction: the other axis does not take over before the stick was back at rest (M4).
        rig.input.axes(0f, 0f, 0.86f, 0.60f);
        eq("", rig.take(), "the other axis larger by 0.26: the held direction stays");
        rig.input.axes(0f, 0f, 1f, 0.30f);
        eq("", rig.take(), "DOWN's own axis inside the release band, x pressing hard: still nothing in this push");
        rig.input.axes(0f, 0f, 0.30f, 0.60f);
        eq("DOWN@200", rig.take(), "the same axis pressing again: DOWN");
        rig.input.axes(0f, 0f, 0.2f, 0.2f);
        rig.input.axes(0f, 0f, 0.9f, 0.1f);
        eq("RIGHT@200", rig.take(), "after rest, a sideways push: RIGHT");
        rig.input.axes(0f, 0f, 0f, 0f);
        rig.timer.advance(2000);
        eq("", rig.take(), "centred");
    }

    private static void notANumber() {
        Rig rig = new Rig();
        rig.input.axes(Float.NaN, Float.NaN, Float.NaN, Float.NaN);
        eq("", rig.take(), "four values that are not numbers");
        rig.input.axes(0f, 0f, 0.9f, Float.NaN);
        eq("RIGHT@0", rig.take(), "stick x 0.9 while y is not a number: RIGHT");
        rig.input.axes(0f, 0f, Float.NaN, 0f);
        rig.timer.advance(2000);
        eq("", rig.take(), "x not a number: RIGHT is let go, nothing repeats");
        rig.input.axes(0f, 0f, Float.NaN, -0.9f);
        eq("UP@2000", rig.take(), "stick y -0.9 while x is not a number: UP");
        rig.input.axes(Float.NaN, 1f, 0f, 0f);
        eq("DOWN@2000", rig.take(), "HAT y 1 while HAT x is not a number: DOWN (the stick, centred, let UP go)");
    }

    private static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    /** The table of design section 2.2, written out from the design, not from the code. */
    private static void everyKeyOnce() {
        int[][] table = {
                {19, NavInput.UP}, {20, NavInput.DOWN}, {21, NavInput.LEFT}, {22, NavInput.RIGHT},
                {96, NavInput.CONFIRM}, {23, NavInput.CONFIRM}, {66, NavInput.CONFIRM},
                {97, NavInput.BACK}, {4, NavInput.BACK}, {111, NavInput.BACK},
                {102, NavInput.PAGE_PREV}, {103, NavInput.PAGE_NEXT}, {108, NavInput.MENU}, {100, NavInput.ALT}};
        for (int[] entry : table) {
            Rig rig = new Rig();
            eq(true, rig.input.key(entry[0], true, 0), "key " + entry[0] + " down is consumed");
            eq(NAMES[entry[1]] + "@0", rig.take(), "key " + entry[0] + " down");
            eq(true, rig.input.key(entry[0], false, 0), "key " + entry[0] + " up is consumed");
            eq("", rig.take(), "key " + entry[0] + " up");
        }
    }

    private static void androidRepeatsIgnored() {
        Rig rig = new Rig();
        rig.input.key(KEY_DOWN, true, 0);
        eq("DOWN@0", rig.take(), "the press");
        eq(true, rig.input.key(KEY_DOWN, true, 1), "a repeated direction key is still consumed");
        eq(true, rig.input.key(KEY_DOWN, true, 2), "a repeated direction key is still consumed");
        eq("", rig.take(), "repeat counts 1 and 2 of a direction key");
        // A button held since before this screen opened arrives only as repeats: it must not act here.
        eq(true, rig.input.key(KEY_A, true, 3), "a repeated A is still consumed");
        eq("", rig.take(), "repeat count 3 of A");
    }

    private static void unknownKey() {
        Rig rig = new Rig();
        for (int code : new int[] {0, 3 /* HOME */, 99 /* BUTTON_X */, 109 /* BUTTON_SELECT */, 62 /* SPACE */}) {
            eq(false, rig.input.key(code, true, 0), "key " + code + " down");
            eq(false, rig.input.key(code, false, 0), "key " + code + " up");
        }
        eq("", rig.take(), "unknown keys");
        eq(false, rig.timer.pending(), "no repeat was started");
    }

    private static void repeatTiming() {
        Rig rig = new Rig();
        rig.input.key(KEY_DOWN, true, 0);
        rig.timer.advance(1600);
        // Press, first repeat after 400 ms, nine more 110 ms apart (the 10th at 1390), then 55 ms apart.
        eq("DOWN@0 DOWN+@400 DOWN+@510 DOWN+@620 DOWN+@730 DOWN+@840 DOWN+@950 DOWN+@1060 DOWN+@1170 DOWN+@1280 "
                + "DOWN+@1390 DOWN+@1445 DOWN+@1500 DOWN+@1555", rig.take(), "a key held for 1.6 s");
    }

    private static void releaseStops() {
        Rig rig = new Rig();
        rig.input.key(KEY_RIGHT, true, 0);
        rig.timer.advance(600);
        eq("RIGHT@0 RIGHT+@400 RIGHT+@510", rig.take(), "held for 600 ms");
        rig.input.key(KEY_RIGHT, false, 0);
        rig.timer.advance(2000);
        eq("", rig.take(), "after the release");
    }

    private static void resetStops() {
        Rig rig = new Rig();
        rig.input.key(KEY_UP, true, 0);
        rig.timer.advance(450);
        eq("UP@0 UP+@400", rig.take(), "held key before reset");
        rig.input.reset();
        rig.timer.advance(2000);
        eq("", rig.take(), "held key after reset");
        eq(true, rig.input.key(KEY_UP, false, 0), "the late key-up is consumed");
        eq("", rig.take(), "the late key-up");

        rig.input.axes(1f, 0f, 0f, 0f);
        rig.timer.advance(450);
        eq("RIGHT@2450 RIGHT+@2850", rig.take(), "held HAT before reset");
        rig.input.reset();
        rig.timer.advance(2000);
        eq("", rig.take(), "held HAT after reset");
        // Nothing is remembered: the HAT is still pressed, so its next event counts as a new press.
        eq(true, rig.input.axes(1f, 0f, 0f, 0f), "the still-pressed HAT reports a change after reset");
        eq("RIGHT@4900", rig.take(), "the still-pressed HAT after reset");
    }

    private static void onlyDirectionsRepeat() {
        int[] codes = {96, 97, 102, 103, 108, 100};
        String[] names = {"CONFIRM", "BACK", "PAGE_PREV", "PAGE_NEXT", "MENU", "ALT"};
        for (int i = 0; i < codes.length; ++i) {
            Rig rig = new Rig();
            rig.input.key(codes[i], true, 0);
            rig.timer.advance(3000);
            eq(names[i] + "@0", rig.take(), names[i] + " held for 3 s");
        }
    }

    private static void hatThreshold() {
        Rig rig = new Rig();
        eq(false, rig.input.axes(0.5f, 0f, 0f, 0f), "HAT x at 0.5 reports no change");
        eq(false, rig.input.axes(-0.5f, -0.5f, 0f, 0f), "HAT at -0.5 reports no change");
        eq("", rig.take(), "HAT at the threshold, not beyond it");
        eq(true, rig.input.axes(0.51f, 0f, 0f, 0f), "HAT x beyond 0.5 reports a change");
        eq("RIGHT@0", rig.take(), "HAT x 0.51");
        eq(false, rig.input.axes(0.51f, 0f, 0f, 0f), "the same HAT value again reports no change");
        eq("", rig.take(), "the same HAT value again");
        eq(true, rig.input.axes(0f, 0f, 0f, 0f), "the release reports a change");
        rig.timer.advance(1000);
        eq("", rig.take(), "after the HAT returned to the centre");
        rig.input.axes(-1f, 0f, 0f, 0f);
        eq("LEFT@1000", rig.take(), "HAT x -1");
        rig.input.axes(0f, -1f, 0f, 0f);
        eq("UP@1000", rig.take(), "HAT y -1 is up");
        rig.input.axes(0f, 1f, 0f, 0f);
        eq("DOWN@1000", rig.take(), "HAT y +1 is down");
    }

    private static void stickHysteresis() {
        Rig rig = new Rig();
        eq(false, rig.input.axes(0f, 0f, 0.55f, 0f), "stick at 0.55 reports no change");
        eq("", rig.take(), "stick at 0.55, not beyond it");
        eq(true, rig.input.axes(0f, 0f, 0.56f, 0f), "stick beyond 0.55 reports a change");
        eq("RIGHT@0", rig.take(), "stick x 0.56");
        // Back to 0.40: between the two thresholds, so still held and still repeating.
        eq(false, rig.input.axes(0f, 0f, 0.40f, 0f), "stick at 0.40 reports no change");
        rig.timer.advance(400);
        eq("RIGHT+@400", rig.take(), "stick at 0.40 is still held");
        eq(true, rig.input.axes(0f, 0f, 0.34f, 0f), "stick inside 0.35 reports the release");
        rig.timer.advance(1000);
        eq("", rig.take(), "stick at 0.34 is released");
        rig.input.axes(0f, 0f, 0.40f, 0f);
        eq("", rig.take(), "0.40 after a release does not press");
        rig.input.axes(0f, 0f, 0f, -0.9f);
        eq("UP@1400", rig.take(), "stick y -0.9 is up");
        rig.input.axes(0f, 0f, 0f, -0.36f);
        rig.input.axes(0f, 0f, 0f, 0.9f);
        eq("DOWN@1400", rig.take(), "stick y +0.9 is down");
        rig.input.axes(0f, 0f, 0f, 0f);   // back at rest: one push gives one axis (menu-input-fixes M4)
        rig.input.axes(0f, 0f, -0.9f, 0f);
        eq("LEFT@1400", rig.take(), "stick x -0.9 is left");
    }

    private static void stickDominantAxis() {
        Rig rig = new Rig();
        rig.input.axes(0f, 0f, 0.9f, 0.6f);
        eq("RIGHT@0", rig.take(), "x 0.9 with y 0.6: only x counts");
        rig.input.axes(0f, 0f, 0.6f, 0.9f);
        eq("", rig.take(), "rolled on to y 0.9 in the same push: nothing more (menu-input-fixes M4)");
        rig.input.axes(0f, 0f, 0f, 0f);
        rig.input.axes(0f, 0f, 0.6f, 0.9f);
        eq("DOWN@0", rig.take(), "x 0.6 with y 0.9: now only y counts");
        rig.timer.advance(400);
        eq("DOWN+@400", rig.take(), "the repeat follows the dominant axis");
        Rig other = new Rig();
        other.input.axes(0f, 0f, 0.7f, -0.8f);
        eq("UP@0", other.take(), "x 0.7 with y -0.8: only y counts");
    }

    /**
     * Android can report one D-pad press as a HAT axis and, for an axis event the activity did not consume, as a
     * made-up D-pad key whose key-up may never come. That must move the menu once and must stop with the HAT.
     */
    private static void twoSourcesOneHold() {
        Rig rig = new Rig();
        rig.input.axes(0f, 1f, 0f, 0f);
        eq(true, rig.input.key(KEY_DOWN, true, 0), "the made-up key is consumed");
        eq("DOWN@0", rig.take(), "HAT down plus D-pad-down key");
        rig.timer.advance(400);
        eq("DOWN+@400", rig.take(), "one repeat stream, not two");
        rig.input.axes(0f, 0f, 0f, 0f);
        rig.timer.advance(2000);
        eq("", rig.take(), "after the HAT's release, without any key-up");
    }

    private static void keyUpWhileHatHolds() {
        Rig rig = new Rig();
        rig.input.key(KEY_LEFT, true, 0);
        rig.input.axes(-1f, 0f, 0f, 0f);
        eq("LEFT@0", rig.take(), "key left, then HAT left");
        rig.input.key(KEY_LEFT, false, 0);
        rig.timer.advance(400);
        eq("LEFT+@400", rig.take(), "the HAT still holds the direction after the key-up");
        rig.input.axes(0f, 0f, 0f, 0f);
        rig.timer.advance(2000);
        eq("", rig.take(), "after both let go");
    }

    private static void newestDirectionRepeats() {
        // Two different sources (a D-pad key and the stick) may hold two directions; the newer one repeats.
        Rig rig = new Rig();
        rig.input.key(KEY_DOWN, true, 0);
        rig.timer.advance(100);
        rig.input.axes(0f, 0f, 1f, 0f);
        eq("DOWN@0 RIGHT@100", rig.take(), "two presses");
        rig.timer.advance(400);
        eq("RIGHT+@500", rig.take(), "400 ms after the second press: only RIGHT repeats");
        rig.input.key(KEY_DOWN, false, 0);
        rig.timer.advance(110);
        eq("RIGHT+@610", rig.take(), "releasing the older source does not stop the newer one");
        rig.input.axes(0f, 0f, 0f, 0f);
        rig.timer.advance(2000);
        eq("", rig.take(), "after releasing the newer key");
    }

    private static void staleTaskIgnored() {
        Rig rig = new Rig();
        rig.input.key(KEY_DOWN, true, 0);
        rig.input.key(KEY_DOWN, false, 0);
        Runnable cancelled = rig.timer.displaced;
        eq(true, cancelled != null, "the release cancelled a scheduled task");
        cancelled.run();
        eq("DOWN@0", rig.take(), "the cancelled task was run anyway");

        rig.input.key(KEY_DOWN, true, 0);
        rig.input.key(KEY_RIGHT, true, 0);
        Runnable replaced = rig.timer.displaced;
        rig.take();
        replaced.run();
        eq("", rig.take(), "the replaced task of the older direction was run anyway");
    }

    private static void resetInsideSink() {
        final Rig rig = new Rig();
        rig.duringEvent = new Runnable() {
            @Override public void run() {
                rig.input.reset();   // what a host does when the command opened another screen
            }
        };
        rig.input.key(KEY_DOWN, true, 0);
        rig.timer.advance(3000);
        eq(Arrays.asList("DOWN@0"), rig.events, "press that made the sink reset");
        eq(false, rig.timer.pending(), "no repeat is left scheduled");
    }
}
