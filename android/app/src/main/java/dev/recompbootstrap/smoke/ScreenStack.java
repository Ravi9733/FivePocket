package dev.recompbootstrap.smoke;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;

/**
 * The rules of the screen host that need no Android (design section 2.3). Pure Java; BootstrapActivity applies them.
 *
 * 1. Which face the launcher activity shows for an intent: the old graphics self-test or the new launcher.
 * 2. The scale that turns design pixels (a 1920 x 1080 panel) into real pixels.
 * 3. The stack of screens: the bottom one (the launcher) is never removed; exactly one screen is shown at a time.
 *
 * S is the screen type. The host is told what to do through Listener, in this order:
 *   push:  hidden(old top), attach(new), shown(new)
 *   pop:   hidden(top), detach(top), shown(new top)
 * A push or pop asked for while one of these is still running (a screen calling the host from inside its own
 * onShown, for example) is carried out afterwards, so no screen ever sees a half-changed stack.
 */
final class ScreenStack<S> {
    /**
     * The game runs in the process named (package name) + this suffix; the debug manifest declares it for the game
     * activity and the measurement scripts look for it with "pidof". ScreenHost.gameRunning() looks for the same name.
     */
    static final String GAME_PROCESS_SUFFIX = ":private_trace";

    /** Read access to the extras of the intent that started the activity. */
    interface Extras {
        /** The text extra of that name, or null. */
        String text(String name);
        /** The true/false extra of that name; false when absent. */
        boolean flag(String name);
    }

    interface Listener<S> {
        /** Create the screen's view and add it to the window. Called once per screen, before its first shown. */
        void attach(S screen);
        /** Remove the screen's view from the window. Called once per screen, after its last hidden. */
        void detach(S screen);
        void shown(S screen);
        void hidden(S screen);
    }

    /**
     * True when the activity must run its old self-test code instead of the launcher: the text extra
     * "rgba_diagnostic" is present (three developer scripts start the activity with it), or the true/false extra
     * "smoke" is true (the About screen's "Graphics self-test"). A start with neither shows the launcher.
     */
    static boolean smokeRequested(Extras extras) {
        return extras.text("rgba_diagnostic") != null || extras.flag("smoke");
    }

    /**
     * True when a start of the launcher must show the first-run setup page on top of it: only in a debuggable build
     * and only for the text extra "open" with the value "setup" ("am start -n .../.BootstrapActivity --es open
     * setup"). It exists for device checks of that page: nothing in the app links to the page in this version, and
     * no screen of the app sends this extra. B on the page returns to the launcher.
     */
    static boolean setupRequested(Extras extras, boolean debuggable) {
        return debuggable && "setup".equals(extras.text("open"));
    }

    /** The key of the activity's saved state that says which screen waits for another app's answer, and its one value. */
    static final String AWAITING_KEY = "awaiting_result", AWAITING_SETUP = "setup";

    /**
     * What the activity saves when Android may destroy it while another app (the system's file chooser) answers for a
     * screen: AWAITING_SETUP when a screen waits for an answer, is still on the stack and is the setup page; else
     * null, and nothing is saved. Only the setup page starts anything for an answer.
     */
    static String awaiting(boolean waitingScreenOnStack, boolean itIsTheSetupPage) {
        return waitingScreenOnStack && itIsTheSetupPage ? AWAITING_SETUP : null;
    }

    /**
     * True when an activity that Android made anew must show the setup page again on top of the menu and hand it the
     * answer that is on its way: its destroyed instance had saved AWAITING_SETUP. Without this the new activity
     * showed the menu alone and the chosen file was dropped without a word.
     */
    static boolean restoresSetupPage(String saved) {
        return AWAITING_SETUP.equals(saved);
    }

    /**
     * The scale for a window of these two side lengths in pixels (either order). 1 for 1920 x 1080. The design
     * defines it by the short side (height / 1080); the long side also limits it so that a narrower window, such as
     * the device's lower screen, still shows the full width.
     */
    static float unit(int sideA, int sideB) {
        int longSide = Math.max(sideA, sideB), shortSide = Math.min(sideA, sideB);
        if (shortSide <= 0) {
            return 1f;
        }
        return Math.min(shortSide / 1080f, longSide / 1920f);
    }

    /** android.view.KeyEvent.FLAG_FALLBACK: the key was made up by Android because the app did not use another one. */
    static final int KEY_FLAG_FALLBACK = 0x400;

    /**
     * True for a key the launcher swallows without doing anything. Two kinds: a key Android made up as a
     * replacement (a stick click that nobody used comes back as DPAD_CENTER, which the menus would take for A), and
     * a gamepad button the menus have no use for (stick clicks, X, Select, L2, R2 and the numbered buttons; key codes
     * 96..110 and 188..203 except A 96, B 97, Y 100, L1 102, R1 103, Start 108), so that Android makes up nothing.
     */
    static boolean ignoresKey(int keyCode, int flags) {
        if ((flags & KEY_FLAG_FALLBACK) != 0) {
            return true;
        }
        boolean gamepadButton = (keyCode >= 96 && keyCode <= 110) || (keyCode >= 188 && keyCode <= 203);
        return gamepadButton && keyCode != 96 && keyCode != 97 && keyCode != 100 && keyCode != 102
                && keyCode != 103 && keyCode != 108;
    }

    /**
     * Whether the game process exists. processNames: the names of this app's running processes as Android lists
     * them, or null when Android gave no list. No list counts as "running": the settings screen must not write the
     * game's files on a guess.
     */
    static boolean gameProcessListed(List<String> processNames, String packageName) {
        return processNames == null || processNames.contains(packageName + GAME_PROCESS_SUFFIX);
    }

    private final Listener<S> listener;
    private final List<S> screens = new ArrayList<S>();
    private final ArrayDeque<Runnable> waiting = new ArrayDeque<Runnable>();
    private boolean changing;

    ScreenStack(Listener<S> listener) {
        this.listener = listener;
    }

    int size() {
        return screens.size();
    }

    /** The visible screen, or null when the stack is empty. */
    S top() {
        return screens.isEmpty() ? null : screens.get(screens.size() - 1);
    }

    boolean contains(S screen) {
        return screens.contains(screen);
    }

    /** Shows screen on top of the current one. */
    void push(final S screen) {
        if (screen == null) {
            throw new IllegalArgumentException("a screen is required");
        }
        run(new Runnable() {
            @Override public void run() {
                S below = top();
                if (below != null) {
                    listener.hidden(below);
                }
                screens.add(screen);
                listener.attach(screen);
                listener.shown(screen);
            }
        });
    }

    /** Returns to the screen below. Does nothing when only the bottom screen is left. */
    void pop() {
        run(new Runnable() {
            @Override public void run() {
                if (screens.size() <= 1) {
                    return;
                }
                S leaving = screens.remove(screens.size() - 1);
                listener.hidden(leaving);
                listener.detach(leaving);
                listener.shown(top());
            }
        });
    }

    /**
     * A BACK command was offered to asked and it answered "not handled, pop me". Pops only if asked is still the
     * top screen: a screen that already left by itself while handling BACK must not take the one below with it.
     */
    void backNotHandledBy(S asked) {
        if (asked != null && asked == top()) {
            pop();
        }
    }

    /** The activity is going away: hides the top screen and detaches all, top first. The stack is empty after. */
    void clear() {
        run(new Runnable() {
            @Override public void run() {
                if (!screens.isEmpty()) {
                    listener.hidden(top());
                }
                while (!screens.isEmpty()) {
                    listener.detach(screens.remove(screens.size() - 1));
                }
            }
        });
    }

    private void run(Runnable change) {
        waiting.add(change);
        if (changing) {
            return;   // carried out by the loop below, after the change that is running now
        }
        changing = true;
        try {
            for (Runnable next; (next = waiting.poll()) != null; ) {
                next.run();
            }
        } finally {
            changing = false;
            waiting.clear();
        }
    }
}
