package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.List;

/**
 * Turns key and axis states into menu commands with hold-to-repeat (design section 2.2). Pure Java.
 *
 * <p>One direction can be held by three sources at once: a D-pad key, the HAT axes and the left stick. The stick and
 * the D-pad holding one direction together are one hold, not a second press.
 *
 * <p>The D-pad's two reports. A pad (or Android, making up a key for an axis event nobody consumed) may report one
 * physical D-pad press both as a key and as the HAT. The key arrives at once, the HAT with the next frame's batch, so
 * the two reports of a press, and the reports of several presses, can arrive in any order and in one batch. The
 * invariant (PairingInvariantTest, 20,000 generated sequences): every physical press yields exactly one command,
 * whether it was reported once or twice and in whatever order and batch; two physical presses yield two. The rule:
 * every D-pad report is recorded with its own time (the event's, not the arrival's); a report pairs with the OLDEST
 * not yet paired report of the same direction from the other D-pad source whose press time is within PAIR_MS of its
 * own (a report pairs once). A paired report is that press's second report and sends nothing. A HAT release ends
 * only the key hold it is paired with (a made-up key may never be released), never a newer key press: a release that
 * arrives late in a batch belongs to its own press (menu-input-fixes round 3, M1). A pad that reports only keys, or
 * only the HAT, never pairs.
 *
 * <p>One source holds one direction at a time. A D-pad pressed on a corner goes vertical. The stick goes sideways
 * only when its horizontal axis clearly leads (STICK_SIDEWAYS), vertically when its vertical axis presses and it does
 * not; between the two (a firm push 35.5 to 45 degrees from the horizontal whose vertical part is not beyond
 * STICK_PRESS) it gives nothing until it moves into one of them. One stick push gives directions of one axis only. A
 * push is over when its own axis is back inside STICK_RELEASE and the other axis is not pressed (inside STICK_PRESS):
 * a stick that drifts up to 0.55 off the centre on either axis still ends its pushes (round 3, M2). A push that sweeps
 * on past a diagonal (its own axis falls back while the other is pressed) keeps the command it gave first (M4).
 *
 * <p>When the direction that repeats is let go while another source still holds UP or DOWN (the D-pad held under a
 * stick that took the repeat over), that direction repeats again, from its first repeat (FIRST_REPEAT_MS), without a
 * press of its own. A held LEFT or RIGHT is not taken back (it would change a value on another row): it does
 * nothing until it is pressed again. endHold() ends every hold for the rest of its press (a tab change by L1 / R1, a question that
 * opens): what is still held then does nothing until it is let go and pressed again.
 */
final class NavInput {
    static final int NONE = 0, UP = 1, DOWN = 2, LEFT = 3, RIGHT = 4, CONFIRM = 5, BACK = 6,
            PAGE_PREV = 7, PAGE_NEXT = 8, MENU = 9, ALT = 10;

    interface Sink { void onNav(int command, boolean repeat); }

    interface Timer {
        /** Milliseconds from a monotonic clock. */
        long now();
        /** Run task once after delayMs on the same thread; a later call replaces an earlier pending one. */
        void schedule(Runnable task, long delayMs);
        void cancel();
    }

    /** First repeat after the press, then the slow interval, then the fast interval once FAST_AFTER repeats were sent. */
    static final long FIRST_REPEAT_MS = 400, REPEAT_MS = 110, FAST_REPEAT_MS = 55;
    static final int FAST_AFTER = 10;
    static final float HAT_PRESS = 0.5f, STICK_PRESS = 0.55f, STICK_RELEASE = 0.35f;
    /**
     * A stick press goes sideways only when |x| > STICK_SIDEWAYS * |y| (within about 35.5 degrees of the
     * horizontal); otherwise the vertical axis counts. An exact diagonal and anything within about 9.5 degrees of it
     * moves the highlight instead of changing the value of the row it is on (vm-pad-hold D2).
     */
    static final float STICK_SIDEWAYS = 1.4f;
    /**
     * A D-pad key and a HAT press of one direction whose times are this close may be one press (see the class
     * comment): the two reports of one press come at most 40 ms apart (the invariant's bound), plus 10 ms.
     */
    static final long PAIR_MS = 50;

    /** Joystick axis numbers of android.view.MotionEvent. */
    static final int AXIS_X = 0, AXIS_Y = 1, AXIS_HAT_X = 15, AXIS_HAT_Y = 16;

    /**
     * One joystick MOVE event as Android hands it over: the samples it batched since the last frame (oldest first,
     * positions 0 .. history() - 1) and its current one (position history()), each with its time (uptime ms).
     */
    interface Motion {
        int history();
        float value(int axis, int position);
        long time(int position);
    }

    private static final int KEYS = 0, HAT = 1, STICK = 2;
    private static final int VERTICAL = 1 << UP | 1 << DOWN, HORIZONTAL = 1 << LEFT | 1 << RIGHT;   // direction bits
    /** Remembered D-pad presses: enough for a burst of taps inside one slow frame. */
    private static final int REMEMBERED = 16;

    private final Sink sink;
    private final Timer timer;
    /** Per source, one bit (1 << direction) for every direction it holds. */
    private final int[] held = new int[3];
    private int repeating = NONE;
    /**
     * Per source, the held directions whose hold endHold() ended: they do nothing until that source lets them go. A
     * fresh press of the same direction from another control counts (round 3, minor 6).
     */
    private final int[] ended = new int[3];
    private int repeats;
    /** Identifies the one timer task that may still fire; every schedule and every stop makes older tasks stale. */
    private int ticket;
    /** The axis (VERTICAL or HORIZONTAL) the stick's current push gave a direction on; 0 at rest. */
    private int stickAxis;

    /** One D-pad report of one source, from its time to its release (Long.MAX_VALUE while held), and its pair. */
    private static final class Press {
        final int source, direction;
        final long from;
        long to = Long.MAX_VALUE;
        Press partner;

        Press(int source, int direction, long from) {
            this.source = source;
            this.direction = direction;
            this.from = from;
        }
    }

    /** The key Press that owns the current key hold, even when an older HAT batch is still arriving. */
    private Press keyHold;

    private final List<Press> presses = new ArrayList<Press>();

    NavInput(Sink sink, Timer timer) {
        this.sink = sink;
        this.timer = timer;
    }

    /** keyCode is an android.view.KeyEvent code. Returns true when the key belongs to the menu (consume it). */
    boolean key(int keyCode, boolean down, int repeatCount) {
        return key(keyCode, down, repeatCount, timer.now());
    }

    /** As above; time is the key event's own time (KeyEvent.getEventTime, uptime ms). */
    boolean key(int keyCode, boolean down, int repeatCount, long time) {
        int command = commandForKey(keyCode);
        if (command == NONE) return false;
        if (!isDirection(command)) {
            if (down && repeatCount == 0) sink.onNav(command, false);
            return true;
        }
        if (!down) release(KEYS, command, time);
        else if (repeatCount == 0 && (held[KEYS] & ~(1 << command)) == 0) press(KEYS, command, time);
        return true;
    }

    /**
     * Every sample of a joystick event in order, the batched older ones first: a press and its release that arrive
     * in one event (a short tap during a slow frame) still count once (vm-pad-hold D3). Returns true when any sample
     * fired a command or changed a held direction.
     */
    boolean motion(Motion event) {
        boolean changed = false;
        int history = Math.max(0, event.history());
        for (int position = 0; position <= history; ++position) {
            changed |= axes(event.value(AXIS_HAT_X, position), event.value(AXIS_HAT_Y, position),
                    event.value(AXIS_X, position), event.value(AXIS_Y, position), event.time(position));
        }
        return changed;
    }

    /** Current HAT and left-stick values in -1..1. Returns true when a command fired or a held direction changed. */
    boolean axes(float hatX, float hatY, float stickX, float stickY) {
        return axes(hatX, hatY, stickX, stickY, timer.now());
    }

    /** As above, for a sample taken at time (uptime ms). */
    boolean axes(float hatX, float hatY, float stickX, float stickY, long time) {
        int hat = 0;
        if (hatX > HAT_PRESS) hat |= 1 << RIGHT;
        else if (hatX < -HAT_PRESS) hat |= 1 << LEFT;
        if (hatY > HAT_PRESS) hat |= 1 << DOWN;
        else if (hatY < -HAT_PRESS) hat |= 1 << UP;
        boolean changed = hold(HAT, oneDirection(hat, held[HAT]), time);
        int stick = stickDirection(stickX, stickY, directionOf(held[STICK]));
        changed |= hold(STICK, stick == NONE ? 0 : 1 << stick, time);
        if (stick != NONE) stickAxis = (1 << stick & VERTICAL) != 0 ? VERTICAL : HORIZONTAL;
        return changed;
    }

    /**
     * Forget every held key and axis and cancel the repeat (focus loss, pause, screen change, a pad taken away).
     * Nothing is remembered: an axis that is still deflected counts as a new press at its next event.
     */
    void reset() {
        held[KEYS] = held[HAT] = held[STICK] = 0;
        ended[KEYS] = ended[HAT] = ended[STICK] = 0;
        stickAxis = 0;
        presses.clear();
        keyHold = null;
        stopRepeat();
    }

    /**
     * Ends the repeat and every hold without forgetting what is held (a tab change by L1 / R1, a question that opens,
     * vm-pad-hold D5, menu-input-fixes M2): a direction that is still held does nothing more, also not when a stick
     * that rests there jitters, until it is let go and pressed again. A direction pressed afresh afterwards works at
     * once.
     */
    void endHold() {
        for (int source = 0; source < held.length; ++source) ended[source] = held[source];
        stopRepeat();
    }

    /** The directions some source holds and endHold() has not ended. */
    private int live() {
        return held[KEYS] & ~ended[KEYS] | held[HAT] & ~ended[HAT] | held[STICK] & ~ended[STICK];
    }

    private static int commandForKey(int keyCode) {
        switch (keyCode) {
            case 19: return UP;          // KEYCODE_DPAD_UP
            case 20: return DOWN;        // KEYCODE_DPAD_DOWN
            case 21: return LEFT;        // KEYCODE_DPAD_LEFT
            case 22: return RIGHT;       // KEYCODE_DPAD_RIGHT
            case 96:                     // KEYCODE_BUTTON_A
            case 23:                     // KEYCODE_DPAD_CENTER
            case 66: return CONFIRM;     // KEYCODE_ENTER
            case 97:                     // KEYCODE_BUTTON_B
            case 4:                      // KEYCODE_BACK
            case 111: return BACK;       // KEYCODE_ESCAPE
            case 102: return PAGE_PREV;  // KEYCODE_BUTTON_L1
            case 103: return PAGE_NEXT;  // KEYCODE_BUTTON_R1
            case 108: return MENU;       // KEYCODE_BUTTON_START
            case 100: return ALT;        // KEYCODE_BUTTON_Y
            default: return NONE;
        }
    }

    /**
     * At most one of the directions in now: the one already held (was) while it is still pressed; otherwise, for a
     * corner pressed in one go, the vertical one (moving the highlight changes nothing, changing a value would).
     */
    private static int oneDirection(int now, int was) {
        if ((now & was) != 0) return now & was;
        int vertical = now & VERTICAL;
        return vertical != 0 ? vertical : now;
    }

    private static boolean isDirection(int command) {
        return command >= UP && command <= RIGHT;
    }

    private static int directionOf(int bits) {
        for (int direction = UP; direction <= RIGHT; ++direction) if (bits == 1 << direction) return direction;
        return NONE;
    }

    /**
     * The one direction the left stick asks for. A held direction stays while its own axis is outside the release
     * band. Otherwise: sideways when x clearly leads, vertical when y presses; and once this push gave a direction,
     * none of the other axis until the stick has been back at rest.
     */
    private int stickDirection(float x, float y, int current) {
        if (x != x) x = 0;   // NaN counts as centred
        if (y != y) y = 0;
        float ax = Math.abs(x), ay = Math.abs(y);
        if (ax < STICK_RELEASE && ay < STICK_RELEASE) {
            stickAxis = 0;   // at rest: the next push may go either way
            return NONE;
        }
        if (stickAxis != 0) {
            // The push is over once its own axis is back and the other is not pressed (a drifting stick).
            float own = stickAxis == VERTICAL ? ay : ax, other = stickAxis == VERTICAL ? ax : ay;
            if (own < STICK_RELEASE && other < STICK_PRESS) stickAxis = 0;
        }
        boolean stillHeld;
        switch (current) {
            case LEFT: stillHeld = x < -STICK_RELEASE; break;
            case RIGHT: stillHeld = x > STICK_RELEASE; break;
            case UP: stillHeld = y < -STICK_RELEASE; break;
            case DOWN: stillHeld = y > STICK_RELEASE; break;
            default: stillHeld = false;
        }
        if (stillHeld) return current;
        int wanted = NONE;
        if (ax > STICK_PRESS && ax > STICK_SIDEWAYS * ay) wanted = x > 0 ? RIGHT : LEFT;
        else if (ay > STICK_PRESS) wanted = y > 0 ? DOWN : UP;
        if (wanted == NONE) return NONE;
        int axis = (1 << wanted & VERTICAL) != 0 ? VERTICAL : HORIZONTAL;
        return stickAxis == 0 || stickAxis == axis ? wanted : NONE;
    }

    /** Brings one axis source to the given set of held directions. Returns true when the set changed. */
    private boolean hold(int source, int now, long time) {
        int was = held[source];
        if (was == now) return false;
        for (int direction = UP; direction <= RIGHT; ++direction) {
            int bit = 1 << direction;
            if ((was & bit) != 0 && (now & bit) == 0) release(source, direction, time);
        }
        for (int direction = UP; direction <= RIGHT; ++direction) {
            int bit = 1 << direction;
            if ((was & bit) == 0 && (now & bit) != 0) press(source, direction, time);
        }
        return true;
    }

    private void press(int source, int direction, long time) {
        int bit = 1 << direction;
        held[source] |= bit;
        ended[source] &= ~bit;
        if (source != STICK) {
            Press partner = pair(source, direction, time);
            if (partner != null) {
                // The second report of a press already counted: it holds with it, and is ended if that press is.
                if (partner.to != Long.MAX_VALUE || (ended[partner.source] & bit) != 0) ended[source] |= bit;
                return;
            }
        }
        // The stick and the D-pad holding one direction together are one hold.
        int otherControl = source == STICK ? held[KEYS] & ~ended[KEYS] | held[HAT] & ~ended[HAT] : held[STICK] & ~ended[STICK];
        if ((otherControl & bit) != 0) return;
        repeating = direction;
        repeats = 0;
        // The hold is recorded and its repeat scheduled before the sink runs, so that a reset() made by the sink
        // (a command that opened another screen) ends it.
        schedule(FIRST_REPEAT_MS);
        sink.onNav(direction, false);
    }

    /**
     * Records a D-pad report and pairs it with the oldest unpaired report of the same direction from the other D-pad
     * source whose press time is within PAIR_MS of its own. Returns that partner, or null for a press of its own.
     */
    private Press pair(int source, int direction, long time) {
        Press own = new Press(source, direction, time);
        if (source == KEYS) keyHold = own;
        for (Press other : presses) {
            if (other.source == source || other.direction != direction || other.partner != null) continue;
            if (Math.abs(time - other.from) <= PAIR_MS) {
                other.partner = own;
                own.partner = other;
                break;
            }
        }
        presses.add(own);
        while (presses.size() > REMEMBERED) presses.remove(0);
        return own.partner;
    }

    private void release(int source, int direction, long time) {
        int bit = 1 << direction;
        held[source] &= ~bit;
        ended[source] &= ~bit;
        if (source != STICK) {
            for (int i = presses.size() - 1; i >= 0; --i) {
                Press p = presses.get(i);
                if (p.source != source || p.direction != direction || p.to != Long.MAX_VALUE) continue;
                p.to = time;
                // A HAT release also ends the key press it is paired with (a made-up key may never be released); a
                // newer key press is another press and keeps its hold.
                Press key = p.partner;
                if (source == HAT && key != null && key.to == Long.MAX_VALUE) {
                    key.to = time;
                    if (key == keyHold) {
                        held[KEYS] &= ~bit;
                        ended[KEYS] &= ~bit;
                        keyHold = null;
                    }
                }
                break;
            }
        }
        int stillHeld = live();
        if (repeating == direction && (stillHeld & bit) == 0) {
            stopRepeat();
            // Another source still holds UP or DOWN (the D-pad under a stick that let go): it repeats again, from its
            // first repeat; the player pressed nothing new, so no press of its own (vm-pad-hold D6). A still-held
            // LEFT / RIGHT would change a value on the row the other direction moved to: it is not taken back and
            // does nothing until it is pressed again (menu-input-fixes minor 2).
            int handOver = directionOf(Integer.lowestOneBit(stillHeld & VERTICAL));
            if (handOver != NONE) {
                repeating = handOver;
                repeats = 0;
                schedule(FIRST_REPEAT_MS);
            }
        }
    }

    private void schedule(long delayMs) {
        final int mine = ++ticket;
        timer.schedule(() -> tick(mine), delayMs);
    }

    private void tick(int mine) {
        if (mine != ticket || repeating == NONE) return;
        int direction = repeating;
        repeats++;
        schedule(repeats >= FAST_AFTER ? FAST_REPEAT_MS : REPEAT_MS);
        sink.onNav(direction, true);
    }

    private void stopRepeat() {
        repeating = NONE;
        ticket++;
        timer.cancel();
    }
}
