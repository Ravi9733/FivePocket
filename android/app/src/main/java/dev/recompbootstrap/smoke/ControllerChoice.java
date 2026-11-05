package dev.recompbootstrap.smoke;

/**
 * Which connected controller plays (alpha 1.0.1, 2026-10-07). Plain numbers in and out, so the rule runs in a host test.
 *
 * Before this the first controller Android listed was taken, and a handheld's built-in pad always won: a DualShock or an
 * Xbox pad connected to such a handheld was never heard, and neither was a pad whose first listed part is not its
 * buttons (a DualShock is listed as two or three devices: the pad, its touchpad, on some systems its motion sensors).
 *
 * The rule now: the controller that was last given a button press plays. With nothing chosen yet, the best listed
 * device is taken: a built-in pad, else a device with gamepad buttons, else any joystick.
 */
final class ControllerChoice {
    static final int NONE = -1;

    private ControllerChoice() {}

    /** A listed device's rank; 0 is not a controller. */
    static int rank(boolean supported, boolean builtIn, boolean gamepadButtons) {
        if (!supported) return 0;
        return builtIn ? 3 : gamepadButtons ? 2 : 1;
    }

    /**
     * The device to play with: {@code current} for as long as it is still listed as a controller, else the listed
     * device of the highest rank (the first of equals, in Android's order), else NONE.
     */
    static int pick(int current, int[] ids, int[] ranks) {
        int best = NONE, bestRank = 0;
        for (int i = 0; i < ids.length; ++i) {
            if (ranks[i] <= 0 || ids[i] < 0) continue;
            if (ids[i] == current) return current;
            if (ranks[i] > bestRank) {
                best = ids[i];
                bestRank = ranks[i];
            }
        }
        return best;
    }

    /**
     * True when a key event makes its device the one that plays: a real (not injected) press of a gamepad button on a
     * controller other than the current one. Releases, sticks and test events never change the choice.
     */
    static boolean adopts(int current, int eventDevice, boolean eventDeviceIsController, boolean gamepadButton,
            boolean down, boolean injected) {
        return !injected && down && gamepadButton && eventDeviceIsController && eventDevice >= 0
                && eventDevice != current;
    }
}
