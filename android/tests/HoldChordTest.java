package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;

/**
 * Host test of HoldChord, the "hold Select + Start for one second" exit of the controller check screen.
 * Run with java -ea. Xbox button bits as that screen feeds them: Back (Select) = 32, Start = 16, A = 4096.
 */
public final class HoldChordTest {
    private static final int SELECT = 32, START = 16, A = 4096;

    public static void main(String[] args) {
        LauncherTestSupport.requireAssertions();

        run("HoldChord.firesOnceWhenBothAreHeldForTheHoldTime", () -> {
            HoldChord chord = new HoldChord(SELECT, START, 1000);
            check(!chord.update(SELECT | START, 5000), "not at the moment both go down");
            check(chord.pending(), "pending while held");
            check(!chord.update(SELECT | START, 5999), "not one millisecond early");
            check(chord.update(SELECT | START, 6000), "fires at exactly the hold time");
            check(!chord.pending(), "no longer pending after it fired");
            check(!chord.update(SELECT | START, 6001) && !chord.update(SELECT | START, 9000), "fires once per hold");
        });

        run("HoldChord.oneButtonAloneNeverFires", () -> {
            HoldChord chord = new HoldChord(SELECT, START, 1000);
            check(!chord.update(SELECT, 0) && !chord.update(SELECT, 5000), "Select alone");
            check(!chord.pending(), "not pending with one button");
            check(!chord.update(START, 5001) && !chord.update(START, 20000), "Start alone");
            check(!chord.update(A | SELECT, 30000) && !chord.update(A | SELECT, 40000), "Select with another button");
            check(!chord.update(0, 50000), "nothing held");
        });

        run("HoldChord.lettingGoStartsTheCountAgain", () -> {
            HoldChord chord = new HoldChord(SELECT, START, 1000);
            check(!chord.update(SELECT | START, 0), "down");
            check(!chord.update(SELECT, 600), "Start let go after 600 ms");
            check(!chord.pending(), "not pending after letting go");
            check(!chord.update(SELECT | START, 700), "down again");
            check(!chord.update(SELECT | START, 1300), "1300 ms after the first press, but only 600 ms of this hold");
            check(!chord.update(SELECT | START, 1699), "999 ms of this hold");
            check(chord.update(SELECT | START, 1700), "1000 ms of this hold");
        });

        run("HoldChord.firesAgainOnANewHold", () -> {
            HoldChord chord = new HoldChord(SELECT, START, 1000);
            chord.update(SELECT | START, 0);
            check(chord.update(SELECT | START, 1000), "first hold");
            check(!chord.update(0, 1100), "released");
            check(!chord.update(SELECT | START, 1200), "second hold begins");
            check(chord.update(SELECT | START, 2200), "second hold fires");
        });

        run("HoldChord.otherButtonsDoNotMatter", () -> {
            HoldChord chord = new HoldChord(SELECT, START, 1000);
            check(!chord.update(SELECT | START | A, 0), "down with A");
            check(!chord.update(SELECT | START, 400), "A released: the two are still held");
            check(chord.update(SELECT | START | A, 1000), "fires with A down again");
        });

        run("HoldChord.aClockThatJumpsBackStartsTheCountAgain", () -> {
            HoldChord chord = new HoldChord(SELECT, START, 1000);
            chord.update(SELECT | START, 10_000);
            check(!chord.update(SELECT | START, 2_000), "the clock went back");
            check(!chord.update(SELECT | START, 2_999), "999 ms after the jump");
            check(chord.update(SELECT | START, 3_000), "1000 ms after the jump");
        });

        run("HoldChord.refusesMeaninglessSettings", () -> {
            for (int[] bad : new int[][] {{0, START, 1000}, {SELECT, 0, 1000}, {SELECT, START, -1}}) {
                try {
                    new HoldChord(bad[0], bad[1], bad[2]);
                } catch (IllegalArgumentException expected) {
                    continue;
                }
                throw new AssertionError("accepted masks " + bad[0] + ", " + bad[1] + " and hold time " + bad[2]);
            }
        });

        LauncherTestSupport.finish("HoldChordTest");
    }
}
