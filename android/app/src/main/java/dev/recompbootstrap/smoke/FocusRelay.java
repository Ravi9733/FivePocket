package dev.recompbootstrap.smoke;

/**
 * When the app's window gets the input focus back, the visible screen is told (Screen.onWindowBack), except for the
 * window's first focus: the screen was just shown and has looked already. Pure Java; BootstrapActivity calls it from
 * onWindowFocusChanged and the tests call the same method (setup-fixes, defect C3-1 and check finding M2).
 */
final class FocusRelay {
    private boolean focusedBefore;

    /** The window's focus changed. True when the visible screen is to be told that the focus came back. */
    boolean changed(boolean focused) {
        if (!focused) return false;
        boolean tell = focusedBefore;
        focusedBefore = true;
        return tell;
    }
}
