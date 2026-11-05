package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.List;

/**
 * Host test of PhysicalMotionLog: when the game window writes "PHYSICAL MOTION" to the log. A test tool on the
 * lead's computer reads that line to know that somebody is playing with a stick, the D-pad hat or a trigger alone;
 * a missing line there means a game can be stopped under the player's hands, and a line that should not be there
 * means a game nobody plays is never stopped. Run with java -ea.
 */
public final class PhysicalMotionLogTest {
    private static int passed, failed;

    private interface Case {
        void run() throws Exception;
    }

    private static void check(String name, Case test) {
        try {
            test.run();
            ++passed;
            System.out.println("PASS  " + name);
        } catch (Throwable problem) {
            ++failed;
            System.out.println("FAIL  " + name + "\n      " + problem);
        }
    }

    private static void expect(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    private static final int PAD = 7, OTHER_PAD = 9, INJECTED = -1;

    /** One motion event of the selected controller PAD with these values; the times at which a line was written are kept. */
    private static final class Window {
        final PhysicalMotionLog log = new PhysicalMotionLog();
        final List<Long> lines = new ArrayList<Long>();

        void event(int device, int selected, int lx, int ly, int rx, int ry, int hat, int lt, int rt, long at) {
            if (log.moved(device, selected, lx, ly, rx, ry, hat, lt, rt, at)) lines.add(at);
        }

        void leftStick(int x, int y, long at) {
            event(PAD, PAD, x, y, 0, 0, 0, 0, 0, at);
        }
    }

    public static void main(String[] args) {
        check("rest noise below the rest zone leaves no line: sticks, triggers, and all of them together", () -> {
            Window w = new Window();
            long at = 1000;
            for (int[] v : new int[][] {
                    {0, 0, 0, 0, 0, 0},
                    {7849, 0, 0, 0, 0, 0}, {0, -7849, 0, 0, 0, 0},      // the left stick exactly at the edge of its zone
                    {5550, 5550, 0, 0, 0, 0},                           // on the diagonal: 7849 from the centre, not beyond
                    {0, 0, 8689, 0, 0, 0}, {0, 0, -6144, 6144, 0, 0},   // the right stick, whose zone is wider
                    {0, 0, 0, 0, 30, 0}, {0, 0, 0, 0, 0, 30},           // triggers at their threshold
                    {7000, -3000, 8000, 3000, 30, 30}}) {
                w.event(PAD, PAD, v[0], v[1], v[2], v[3], 0, v[4], v[5], at);
                at += 6000;   // far apart: the rate limit is not what keeps these quiet
            }
            expect(w.lines.isEmpty(), "no line for rest noise: " + w.lines);
        });

        check("each of the six inputs beyond its rest zone leaves a line", () -> {
            int[][] beyond = {
                    {7850, 0, 0, 0, 0, 0, 0}, {0, -7850, 0, 0, 0, 0, 0}, {5551, 5551, 0, 0, 0, 0, 0},   // left stick
                    {0, 0, 8690, 0, 0, 0, 0}, {0, 0, 0, -8690, 0, 0, 0},                               // right stick
                    {0, 0, 0, 0, 1, 0, 0}, {0, 0, 0, 0, 8, 0, 0},                                      // the hat: up, right
                    {0, 0, 0, 0, 0, 31, 0}, {0, 0, 0, 0, 0, 0, 31},                                    // triggers
                    {-32768, -32768, 0, 0, 0, 0, 0}};                                                  // the far corner (no overflow)
            for (int[] v : beyond) {
                Window w = new Window();
                w.event(PAD, PAD, v[0], v[1], v[2], v[3], v[4], v[5], v[6], 500);
                expect(w.lines.size() == 1, "a line for " + java.util.Arrays.toString(v));
            }
            // the left stick's narrower zone is not used for the right stick
            Window right = new Window();
            right.event(PAD, PAD, 0, 0, 8000, 0, 0, 0, 0, 500);
            expect(right.lines.isEmpty(), "8000 on the right stick is inside its zone (8689), though beyond the left stick's (7849)");
        });

        check("a held stick leaves one line every five seconds, not more", () -> {
            Window w = new Window();
            // a stick held for 21 seconds, an event every 8 ms as a controller reports it
            for (long at = 100; at <= 21100; at += 8) w.leftStick(30000, 0, at);
            expect(w.lines.size() == 5, "five lines in 21 seconds: " + w.lines);
            for (int i = 1; i < w.lines.size(); ++i) {
                long gap = w.lines.get(i) - w.lines.get(i - 1);
                expect(gap >= 5000 && gap < 5008, "each at the first event five seconds after the last line: " + gap);
            }
            expect(w.lines.get(0) == 100, "the first line at the first event beyond the zone");
        });

        check("the five seconds count from the last line, also across a rest and a second push", () -> {
            Window w = new Window();
            w.leftStick(20000, 0, 1000);     // line
            w.leftStick(0, 0, 1100);         // released
            w.leftStick(20000, 0, 3000);     // pushed again 2 s after the line: no second line yet
            w.leftStick(20000, 0, 5999);     // 4.999 s after the line: not yet
            w.leftStick(0, 0, 6000);         // at rest when the five seconds are over: rest leaves no line
            w.leftStick(20000, 0, 6500);     // the next push: line
            expect(w.lines.toString().equals("[1000, 6500]"), "lines at " + w.lines);
        });

        check("two controllers: only the one the game listens to counts, and the other one's events change nothing", () -> {
            Window w = new Window();
            w.event(OTHER_PAD, PAD, 30000, 0, 0, 0, 0, 255, 0, 1000);   // a second controller moves: no line
            expect(w.lines.isEmpty(), "the other controller leaves no line");
            w.event(PAD, PAD, 30000, 0, 0, 0, 0, 0, 0, 1200);           // the selected one: line, although the other moved 200 ms before
            expect(w.lines.toString().equals("[1200]"), "the selected controller's first event leaves the line: " + w.lines);
            // both held: still one line every five seconds in all, counted by the selected controller's events
            for (long at = 1300; at <= 11300; at += 10) {
                w.event(OTHER_PAD, PAD, 30000, 0, 0, 0, 0, 0, 0, at);
                w.event(PAD, PAD, 30000, 0, 0, 0, 0, 0, 0, at + 5);
            }
            expect(w.lines.size() == 3, "three lines in ten seconds with two controllers held: " + w.lines);
            // no controller selected at all
            Window none = new Window();
            none.event(PAD, -1, 30000, 0, 0, 0, 0, 0, 0, 1000);
            expect(none.lines.isEmpty(), "no selected controller: no line");
        });

        check("synthetic motion leaves no line: an injected event, also when nothing is selected", () -> {
            Window w = new Window();
            w.event(INJECTED, PAD, 30000, 30000, 30000, 30000, 15, 255, 255, 1000);
            w.event(INJECTED, INJECTED, 30000, 30000, 30000, 30000, 15, 255, 255, 7000);   // "selected" is -1 while no controller is connected
            expect(w.lines.isEmpty(), "no line for injected motion: " + w.lines);
            // and it does not use up the real controller's five seconds
            w.event(PAD, PAD, 30000, 0, 0, 0, 0, 0, 0, 7100);
            expect(w.lines.toString().equals("[7100]"), "the real controller right after it: " + w.lines);
        });

        check("a stick held still (no further event, as Android sends none): the timer's question writes the line every five seconds", () -> {
            // The game window asks stillHeld() at the time nextLineAt() names; this plays that timer for 21 seconds.
            Window w = new Window();
            expect(w.log.nextLineAt() == Long.MIN_VALUE, "nothing to ask for before anything moved");
            w.leftStick(32767, 0, 100);      // pushed to the end stop: one event, then silence
            List<Long> asked = new ArrayList<Long>();
            for (long due; (due = w.log.nextLineAt()) != Long.MIN_VALUE && due <= 21100;) {
                asked.add(due);
                if (w.log.stillHeld(due)) w.lines.add(due);
            }
            expect(w.lines.toString().equals("[100, 5100, 10100, 15100, 20100]"), "lines at " + w.lines);
            expect(asked.toString().equals("[5100, 10100, 15100, 20100]"), "the timer is asked once per line, not more often: " + asked);
            // asked too early (a timer that fires before its time) it writes nothing
            expect(!w.log.stillHeld(20200) && !w.log.stillHeld(25099) && w.log.stillHeld(25100), "not before five seconds have passed");
        });

        check("the timer stops with the stick: back at rest, focus lost or controller gone, nothing more is asked or written", () -> {
            Window w = new Window();
            w.leftStick(32767, 0, 100);
            w.leftStick(1000, 0, 2000);      // back inside the rest zone
            expect(w.log.nextLineAt() == Long.MIN_VALUE && !w.log.stillHeld(9000), "at rest: no timer, no line");
            Window gone = new Window();
            gone.leftStick(32767, 0, 100);
            gone.log.released();             // the window lost focus or the controller went away: the game was given a controller at rest
            expect(gone.log.nextLineAt() == Long.MIN_VALUE && !gone.log.stillHeld(9000), "released: no timer, no line");
            gone.leftStick(32767, 0, 9500);  // pushed again later: a line again (five seconds after the last)
            expect(gone.lines.toString().equals("[100, 9500]"), "lines at " + gone.lines);
            // an event of another controller or an injected one neither starts nor stops the timer
            Window other = new Window();
            other.event(OTHER_PAD, PAD, 32767, 0, 0, 0, 0, 0, 0, 100);
            other.event(INJECTED, PAD, 32767, 0, 0, 0, 0, 0, 0, 200);
            expect(other.log.nextLineAt() == Long.MIN_VALUE, "another controller's push starts no timer");
            other.leftStick(32767, 0, 300);
            other.event(OTHER_PAD, PAD, 0, 0, 0, 0, 0, 0, 0, 400);
            expect(other.log.nextLineAt() == 5300, "another controller at rest does not stop the selected one's timer");
        });

        check("the timer and the events together still leave one line every five seconds", () -> {
            Window w = new Window();
            for (long at = 100; at <= 7000; at += 8) {
                long due = w.log.nextLineAt();
                if (due != Long.MIN_VALUE && due <= at && w.log.stillHeld(due)) w.lines.add(due);   // the timer, when it falls due first
                w.leftStick(30000, 0, at);
            }
            expect(w.lines.toString().equals("[100, 5100]"), "lines at " + w.lines);
        });

        check("the line is the text the lead's tool looks for", () -> {
            // owner_playing.py matches "RecompNativeInput: PHYSICAL " in the log; PrivateGamepadInput logs LINE under that tag.
            expect(PhysicalMotionLog.LINE.equals("PHYSICAL MOTION"), "the line: " + PhysicalMotionLog.LINE);
        });

        System.out.println("PhysicalMotionLogTest: " + passed + " passed, " + failed + " failed");
        System.exit(failed == 0 ? 0 : 1);
    }
}
