package dev.recompbootstrap.smoke;

/**
 * Detects two buttons held together for a given time (design section 4.C). Pure Java.
 *
 * The controller check screen uses it for its exit without touch: Select + Start held for one second.
 * The hold must be unbroken: letting go of either button starts the count again.
 */
final class HoldChord {
    private final int maskA, maskB;
    private final long holdMs;
    private boolean held;      // both masks were down at the last update
    private boolean fired;     // this hold already returned true
    private long heldSinceMs;

    HoldChord(int maskA, int maskB, long holdMs) {
        if (maskA == 0 || maskB == 0 || holdMs < 0) {
            throw new IllegalArgumentException("two button masks and a hold time of zero or more are required");
        }
        this.maskA = maskA;
        this.maskB = maskB;
        this.holdMs = holdMs;
    }

    /**
     * buttons is the current button bit field, nowMs a monotonic clock.
     * Returns true exactly once per hold, at the first update that sees both masks held for holdMs or longer.
     */
    boolean update(int buttons, long nowMs) {
        boolean both = (buttons & maskA) == maskA && (buttons & maskB) == maskB;
        if (!both) {
            held = false;
            fired = false;
            return false;
        }
        if (!held || nowMs < heldSinceMs) {   // a new hold, or the clock went backwards: count from now
            held = true;
            fired = false;
            heldSinceMs = nowMs;
        }
        if (fired || nowMs - heldSinceMs < holdMs) {
            return false;
        }
        fired = true;
        return true;
    }

    /** True while both buttons are held and this hold has not fired yet: the caller should call update again soon. */
    boolean pending() {
        return held && !fired;
    }
}
