package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.List;

/**
 * Shared pieces of the menu-input-fixes tests (vm-pad-hold D1-D7). Uses only what today's sources have, so that a
 * test class that needs a new method fails alone (compile) and the others run to their asserts. Run with java -ea.
 */
final class MenuFixKit {
    private MenuFixKit() { }

    private static int failures;

    static void requireAssertions() {
        boolean assertions = false;
        assert assertions = true;
        if (!assertions) throw new IllegalStateException("Run with -ea");
    }

    static void test(String suite, String name, Runnable body) {
        try {
            body.run();
            System.out.println("PASS  " + suite + ": " + name);
        } catch (AssertionError | RuntimeException | LinkageError failure) {   // LinkageError: a load failure that got through
            failures++;
            StackTraceElement at = null;
            for (StackTraceElement element : failure.getStackTrace()) {
                if (element.getClassName().startsWith("dev.recompbootstrap.smoke.") && !element.getClassName().endsWith("MenuFixKit")) {
                    at = element;
                    break;
                }
            }
            System.out.println("FAIL  " + suite + ": " + name + ": " + failure + (at == null ? "" : " (" + at + ")"));
        }
    }

    /** Prints the closing line and exits non-zero when a test failed. */
    static void finish(String suite) {
        if (failures != 0) {
            System.out.println(suite + "_FAIL: " + failures + " test(s) failed");
            System.exit(1);
        }
        System.out.println(suite + "_PASS");
    }

    static void eq(Object expected, Object actual, String what) {
        if (expected == null ? actual != null : !expected.equals(actual)) {
            throw new AssertionError(what + ": expected <" + expected + "> but was <" + actual + ">");
        }
    }

    static void check(boolean condition, String what) {
        if (!condition) throw new AssertionError(what);
    }

    static final String[] NAMES = {"NONE", "UP", "DOWN", "LEFT", "RIGHT", "CONFIRM", "BACK", "PAGE_PREV", "PAGE_NEXT",
            "MENU", "ALT"};

    /** NavInput with a hand-moved clock; records "DOWN@0" for a press, "DOWN+@400" for a repeat. */
    static class Rig implements NavInput.Sink {
        final NavInputTest.FakeTimer timer = new NavInputTest.FakeTimer();
        final NavInput input = new NavInput(this, timer);
        final List<String> events = new ArrayList<String>();

        @Override public void onNav(int command, boolean repeat) {
            events.add(NAMES[command] + (repeat ? "+" : "") + "@" + timer.now);
            consume(command, repeat);
        }

        /** What the screen does with the command (a test's consumer); nothing by default. */
        void consume(int command, boolean repeat) { }

        String take() {
            String joined = String.join(" ", events);
            events.clear();
            return joined;
        }

        /** The clock moves to an absolute time, running the repeats that come due on the way. */
        void at(long time) {
            timer.advance(time - timer.now);
        }
    }
}
