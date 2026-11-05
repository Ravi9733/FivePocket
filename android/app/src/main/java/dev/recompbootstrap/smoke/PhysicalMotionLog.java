package dev.recompbootstrap.smoke;

/**
 * When the game window writes the log line "PHYSICAL MOTION". Plain Java; PrivateGamepadInput asks moved() for every
 * stick, D-pad hat or trigger event it has given to the game and logs LINE under the tag RecompNativeInput when the
 * answer is true.
 *
 * Why the line exists: a button press already leaves a line ("PHYSICAL KEYCODE_... down"), but a player who only
 * moves a stick, the D-pad hat or a trigger left none. A test tool on the lead's computer reads these lines to know
 * that somebody is playing before it may stop a game; without this line stick-only play looked like nobody playing.
 *
 * The rule:
 *   - only the controller the game listens to counts (the selected device). An event of any other device, and an
 *     injected event (Android gives those a device number below 0), never leaves a line;
 *   - something must be beyond its rest zone: a stick further from its centre than the Xbox 360's own rest zones
 *     (7849 for the left stick, 8689 for the right, of 32767), a trigger beyond 30 of 255, or the hat in any
 *     direction. Rest noise below these leaves no line;
 *   - at most one line every INTERVAL_MS, counted from the last line: a stick that is held leaves one line every
 *     five seconds, never more;
 *   - a stick that is held still counts as held. Android sends no event while no value changes (a stick against
 *     its end stop), so events alone would leave one line and then none: the game window also asks stillHeld() at
 *     the time nextLineAt() names, and the line repeats every five seconds until an event brings everything back
 *     into its rest zone, the controller goes away or the window loses focus (released()).
 * Times are milliseconds since the device started (the events' own times, and the same clock for the timer), so
 * the answer does not depend on when an event was handled.
 */
final class PhysicalMotionLog {
    /** The text of the line; with its tag it reads "RecompNativeInput: PHYSICAL MOTION" in the log. */
    static final String LINE = "PHYSICAL MOTION";
    static final long INTERVAL_MS = 5000;
    /** The Xbox 360's own rest zones (the controller the game was written for): stick units of 32767, triggers of 255. */
    static final int LEFT_STICK_REST = 7849, RIGHT_STICK_REST = 8689, TRIGGER_REST = 30;

    private boolean logged;
    private long lastLineMs;
    /** The last event of the controller the game listens to left something beyond its rest zone. */
    private boolean held;

    /** True when the left stick, the right stick, the hat or a trigger is beyond its rest zone. */
    static boolean beyondRest(int leftX, int leftY, int rightX, int rightY, int hat, int leftTrigger, int rightTrigger) {
        return beyond(leftX, leftY, LEFT_STICK_REST) || beyond(rightX, rightY, RIGHT_STICK_REST) || (hat & 15) != 0
                || leftTrigger > TRIGGER_REST || rightTrigger > TRIGGER_REST;
    }

    /** The stick's distance from its centre is greater than the rest zone (a circle, as the Xbox 360 measures it). */
    private static boolean beyond(int x, int y, int rest) {
        return (long) x * x + (long) y * y > (long) rest * rest;
    }

    /**
     * One motion event that reached the game. eventDevice: the device the event came from (below 0 for an injected
     * event); selectedDevice: the controller the game listens to (below 0 when there is none). The values are what
     * the game receives after the event: sticks -32768..32767, hat as the D-pad's four bits, triggers 0..255.
     * Returns true when the line is to be written now.
     */
    boolean moved(int eventDevice, int selectedDevice, int leftX, int leftY, int rightX, int rightY, int hat,
            int leftTrigger, int rightTrigger, long eventTimeMs) {
        if (eventDevice < 0 || eventDevice != selectedDevice) {
            return false;
        }
        held = beyondRest(leftX, leftY, rightX, rightY, hat, leftTrigger, rightTrigger);
        return held && line(eventTimeMs);
    }

    /**
     * The timer's question, asked at the time nextLineAt() named: true when the line is to be written now, because
     * what the last event left beyond its rest zone is still there and five seconds have passed since the last line.
     */
    boolean stillHeld(long nowMs) {
        return held && line(nowMs);
    }

    /** When stillHeld() is to be asked next; Long.MIN_VALUE while everything rests (no timer is needed then). */
    long nextLineAt() {
        return held ? lastLineMs + INTERVAL_MS : Long.MIN_VALUE;
    }

    /** The controller went away or the window lost focus: the game was given a controller at rest. */
    void released() {
        held = false;
    }

    private boolean line(long timeMs) {
        if (logged && timeMs - lastLineMs < INTERVAL_MS) {
            return false;
        }
        logged = true;
        lastLineMs = timeMs;
        return true;
    }
}
