package dev.recompbootstrap.smoke;

/**
 * The padding of the two screens drawn with Android's own text views (the graphics self-test of BootstrapActivity and
 * ControllerCheckActivity). With targetSdk 35 Android 15 draws every window edge to edge: the status bar, the
 * navigation bar and a display cutout lie over the window, and a view must keep its text out of them itself
 * (setup-fixes, defect C7-1 of the vm-robust run: the self-test's last lines under the bottom bar, the controller
 * check cut at the largest font). Pure Java: the activities read the insets (WindowInsets.Type.systemBars() and
 * displayCutout()) and set what this returns.
 */
final class WindowFit {
    static final int LEFT = 1, TOP = 2, RIGHT = 4, BOTTOM = 8, ALL = LEFT | TOP | RIGHT | BOTTOM;
    /** Controller Check's text: its size at the system's font size, and the smallest it is made to fit the screen. */
    static final int CHECK_TEXT_SP = 17, CHECK_TEXT_MIN_SP = 6;

    private WindowFit() {}

    /**
     * base: the view's own padding {left, top, right, bottom} in pixels; insets: what the system bars and the cutout
     * cover on each side, in the same order; sides: the sides this view touches (LEFT | TOP ...). Returns the padding
     * to set: base plus the inset on each side the view touches. Negative insets count as none.
     */
    static int[] padded(int[] base, int[] insets, int sides) {
        int[] out = base.clone();
        int[] bit = {LEFT, TOP, RIGHT, BOTTOM};
        for (int i = 0; i < 4; ++i) {
            if ((sides & bit[i]) != 0) out[i] += Math.max(0, insets[i]);
        }
        return out;
    }
}
