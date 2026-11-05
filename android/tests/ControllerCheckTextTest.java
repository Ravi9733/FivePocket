package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.equal;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;

/**
 * Host test of ControllerCheckText: the words of the controller check. It reads the screen as a player does: what is
 * pressed, what is still to press, how to leave. Run with java -ea.
 */
public final class ControllerCheckTextTest {
    private static final int UP = 1, DOWN = 2, LEFT = 4, RIGHT = 8, START = 16, BACK = 32, L3 = 64, R3 = 128, LB = 256, RB = 512,
            GUIDE = 1024, A = 4096, B = 8192, X = 16384, Y = 32768;
    private static final String THOR = "AYN Thor / Odin -> Xbox 360, player 1\nOdin Controller (id 5)\nRight stick: AXIS_Z / AXIS_RZ; LT/RT: AXIS_BRAKE / AXIS_GAS";

    private static String line(String screen, String start) {
        for (String line : screen.split("\n")) {
            if (line.startsWith(start)) return line;
        }
        throw new AssertionError("no line starts with <" + start + "> in:\n" + screen);
    }

    private static int count(String text, String part) {
        int count = 0;
        for (int at = text.indexOf(part); at >= 0; at = text.indexOf(part, at + 1)) ++count;
        return count;
    }

    public static void main(String[] args) {
        LauncherTestSupport.requireAssertions();

        run("ControllerCheckText.theScreenNamesWhatIsPressedNowByTheGamesButtonNames", () -> {
            String idle = ControllerCheckText.screen(THOR, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0);
            equal("Pressed now: nothing", line(idle, "Pressed now"), "nothing held");
            String held = ControllerCheckText.screen(THOR, A | B | UP | LB, 17, 255, -32768, 12, 3, 32767, 0, false, false, 0);
            equal("Pressed now: Up LB A B", line(held, "Pressed now"), "four buttons, in the screen's fixed order");
            equal("Left trigger: 17     Right trigger: 255", line(held, "Left trigger"), "the two trigger values");
            equal("Left stick: -32768, 12     Right stick: 3, 32767", line(held, "Left stick"), "the two sticks");
            // Each of the fifteen buttons has its own name.
            int[] bits = {UP, DOWN, LEFT, RIGHT, START, BACK, L3, R3, LB, RB, GUIDE, A, B, X, Y};
            String[] names = {"Up", "Down", "Left", "Right", "Start", "Back", "L3", "R3", "LB", "RB", "Guide", "A", "B", "X", "Y"};
            for (int i = 0; i < bits.length; ++i) {
                equal("Pressed now: " + names[i], line(ControllerCheckText.screen(THOR, bits[i], 0, 0, 0, 0, 0, 0, 0, false, false, 0), "Pressed now"), "bit " + bits[i]);
            }
        });

        run("ControllerCheckText.theScreenSaysWhatHasBeenSeenAndWhatIsStillToPress", () -> {
            String fresh = ControllerCheckText.screen(THOR, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0);
            equal("Buttons seen so far: nothing yet", line(fresh, "Buttons seen"), "nothing seen");
            equal("Still to press: Up Down Left Right Start Back L3 R3 LB RB A B X Y", line(fresh, "Still to press"), "every button but Guide is asked for");
            equal("Triggers seen: left no, right no     Stick directions seen: 0 of 8", line(fresh, "Triggers seen"), "no trigger, no stick");
            int allButX = UP | DOWN | LEFT | RIGHT | START | BACK | L3 | R3 | LB | RB | A | B | Y;
            String nearly = ControllerCheckText.screen(THOR, 0, 0, 0, 0, 0, 0, 0, allButX, true, false, 0x0f);
            equal("Still to press: X", line(nearly, "Still to press"), "one button left");
            equal("Triggers seen: left yes, right no     Stick directions seen: 4 of 8", line(nearly, "Triggers seen"), "one trigger, one stick all the way round");
            // Complete without Guide, which the Thor's pad does not have; a bit above the eight directions is not a direction.
            String done = ControllerCheckText.screen(THOR, 0, 0, 0, 0, 0, 0, 0, allButX | X, true, true, 0x1ff);
            equal("Still to press: nothing", line(done, "Still to press"), "all fourteen seen");
            equal("Triggers seen: left yes, right yes     Stick directions seen: 8 of 8", line(done, "Triggers seen"), "everything seen");
        });

        run("ControllerCheckText.oneWayOutAndNoDeveloperWording", () -> {
            String[] screens = {ControllerCheckText.screen(THOR, A, 1, 2, 3, 4, 5, 6, A, true, false, 3), ControllerCheckText.unreadable()};
            for (String screen : screens) {
                equal(1, count(screen, "To leave, hold Select + Start for one second."), "the way out is said once: " + screen);
                check(!screen.contains("Android Back") && !screen.contains("Back exits"), "and not in the developer page's words: " + screen);
                for (String developer : new String[] {"SNAPSHOT", "packet", "Synthetic", "PHYSICAL", "THOR ->", "sign-in", "Rumble", "AXIS_", "(id "}) {
                    check(!screen.contains(developer), "no developer wording <" + developer + ">: " + screen);
                }
            }
            check(ControllerCheckText.unreadable().contains("cannot be read"), "the unreadable screen says what is wrong");
        });

        run("ControllerCheckText.withoutAControllerThePageSaysSoAndNamesAWayOutThatNeedsNoController", () -> {
            for (String none : new String[] {null, "No controller connected"}) {
                String screen = ControllerCheckText.screen(none, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0);
                check(screen.contains("No controller is connected."), "it says that none is connected: " + screen);
                equal(0, count(screen, "hold Select + Start"), "it does not name the way out that needs two of the controller's buttons: " + screen);
                equal(1, count(screen, "To leave now, use this device's Back button or Back gesture."), "it names Android's own way out, once: " + screen);
                check(!screen.contains("Pressed now") && !screen.contains("Still to press"), "and shows no rows of a controller that is not there: " + screen);
            }
            // with a controller the held pair stays the one way out (B is one of the buttons under test)
            String with = ControllerCheckText.screen(THOR, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0);
            check(with.contains("To leave, hold Select + Start for one second.") && !with.contains("Back gesture"), "with a controller: " + with);
        });

        run("ControllerCheckText.whenTheControllerCannotBeReadThePageAlsoNamesAWayOutThatNeedsNoController", () -> {
            // That page is shown whatever is connected, also with no controller: the held pair alone would leave no way out then.
            String screen = ControllerCheckText.unreadable();
            check(screen.contains("hold Select + Start"), "the held pair is named: " + screen);
            equal(1, count(screen, "use this device's Back button or Back gesture"), "and Android's own way out, once: " + screen);
        });

        run("ControllerCheckText.theControllerIsNamedByItsOwnName", () -> {
            equal("Controller: Odin Controller", line(ControllerCheckText.screen(THOR, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0), "Controller:"), "the Thor's pad");
            equal("Xbox Wireless Controller", ControllerCheckText.controllerName("Android gamepad -> Xbox 360, player 1\nXbox Wireless Controller (id 12)\nRight stick: AXIS_Z / AXIS_RZ"), "another pad");
            equal("Pad (2)", ControllerCheckText.controllerName("Android gamepad -> Xbox 360, player 1\nPad (2) (id 7)\nx"), "only the device number is cut off");
            for (String none : new String[] {null, "", "No controller connected", "No controller"}) {
                equal(null, ControllerCheckText.controllerName(none), "no controller: <" + none + ">");
                equal("Controller: none found", line(ControllerCheckText.screen(none, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0), "Controller:"), "and the screen says so");
            }
        });

        LauncherTestSupport.finish("ControllerCheckTextTest");
    }
}
