package dev.recompbootstrap.smoke;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * The capture combo of the game window: Select (the Xbox Back button) held, then A pressed. Plain Java (no Android
 * class is needed to run it); CaptureController feeds it the game window's key events and does what it asks.
 *
 * What the game receives (every event the combo does not name below reaches the game unchanged and at once):
 * - A is never held back. An A press that completes a combo is not given to the game, nor are its repeats and its
 *   release.
 * - A Select press is held back for at most WINDOW_MS. If A is pressed on the same controller within WINDOW_MS of
 *   the Select press (by the two presses' own times) while it is held back, both presses form the combo: the game
 *   gets neither (nor their repeats and releases), and capture() is called. While that Select stays down, each
 *   further A press is one more capture (also kept from the game).
 * - If no A came, the held-back Select press is given to the game late, and its release is given late by the same
 *   delay, so the game sees the press for its real duration:
 *     released within the window (a tap of d ms): press given at the release, release given d ms later;
 *     still down when the window ends: press given at WINDOW_MS, release given WINDOW_MS after the real release.
 * - A new Select press while a delayed release is still waiting gives that release first (the game then sees the
 *   earlier press shorter than it was; only a double tap faster than the delay can do that).
 * - Repeat events (a held key's auto-repeat, repeat count above 0) of a Select the combo is managing are not given to
 *   the game; the press itself already holds the button down there. Repeats of anything else pass unchanged.
 * - reset() (the window lost focus) and reset(device) (that controller went away); both already release the buttons
 *   in the game: a held-back press is dropped, a waiting release is given at once; see reset().
 *
 * Every controller has its own state (Android's device number; -1 is injected test input, which the game also
 * keeps apart from the real controller's buttons). A release, a repeat and a second press are matched to the
 * controller they came from: a combo held on one controller never keeps another controller's A from the game, and a
 * Select held back for one controller does not take another controller's Select press.
 *
 * Known and left as it is: a Select tap is given to the game late by its own length, and A is never delayed, so an
 * A pressed within that length after the tap ended overlaps the delayed Select in the game although the two were
 * never down together (tap 0 to 80, A at 100: the game has Select from 80 to 160 and A from 100). The other choice,
 * ending the delayed Select at once, would cut a tap to a few milliseconds, shorter than one frame of the game,
 * which can lose the press; Select and A together mean nothing in the game.
 *
 * Two clocks, both in milliseconds since the device started (SystemClock.uptimeMillis on Android): an event's own
 * time (when the button moved) and the time it is handled (`now`). They differ when the main thread was busy.
 * - Whether A came inside the window is decided by the two presses' own times, whenever they are handled.
 * - The window ends WINDOW_MS after the Select press was handled when that is later than its own time: Android hands
 *   the next key over only after the one before was handled, so an A pressed in time may not have arrived yet.
 * - What is given late is given when it is handled; its release is delayed by exactly that lateness.
 * - A release whose own time is not after its press's time carries no time of its own (Android's `input keyevent
 *   --longpress` gives the release the time of the press): the time it is handled counts instead.
 * The owner of this object must call advance(now) at the time nextDeadline() names (CaptureController posts it on the
 * main looper). Not thread-safe: one thread (the main thread) calls everything.
 */
final class CaptureCombo<E> {
    /** android.view.KeyEvent.KEYCODE_BUTTON_SELECT and KEYCODE_BUTTON_A (CaptureComboGlueCheck compares them). */
    static final int SELECT=109, A=96;
    /** How long a Select press is held back waiting for A. */
    static final long WINDOW_MS=250;

    interface Output<E> {
        /** Give this event to the game now (it was held back or delayed). */
        void forward(E event);
        /** The combo was pressed; `atMs` is the time of the A press. */
        void capture(long atMs);
    }

    /** One controller's Select and A. */
    private static final class Pad<E> {
        // A Select press held back (not given to the game yet), and when it is given if no A comes.
        E heldSelect; long heldDue;
        // The own time of the last Select press this pad took (held back, or given late).
        long pressAt;
        // The Select press was given late by `shift` ms and is still down: its release is to be delayed by `shift`.
        boolean selectLate; long shift;
        // A Select release waiting to be given, and when.
        E pendingUp; long pendingDue;
        // Keys whose rest (repeats, release) the combo keeps from the game.
        boolean swallowSelect, swallowA;
    }

    private final Output<E> out;
    private final long window;
    private final Map<Integer,Pad<E>> pads=new LinkedHashMap<>();
    private int lastDevice=Integer.MIN_VALUE;

    CaptureCombo(Output<E> out){this(out,WINDOW_MS);}
    CaptureCombo(Output<E> out,long windowMs){this.out=out;this.window=windowMs;}

    /** An event that is handled at its own time (the main thread was not late). */
    boolean key(E event,int code,boolean down,int repeat,long time,int device){
        return key(event,code,down,repeat,time,time,device);
    }

    /**
     * One key event. `time`: the event's own time; `now`: the time it is handled. Returns true when the combo took it
     * (it is held back, delayed, or kept from the game); false when the caller must give it to the game itself, now.
     * Call only with ACTION_DOWN / ACTION_UP events.
     */
    boolean key(E event,int code,boolean down,int repeat,long time,long now,int device){
        if(now<time)now=time;
        if(code!=SELECT && code!=A){advance(now);return false;}
        lastDevice=device;
        Pad<E> pad=pads.get(device);
        if(pad==null){pad=new Pad<>();pads.put(device,pad);}
        // The combo, by the two presses' own times. Asked before anything that fell due is given: when the main
        // thread is late, the window's timer may be late as well, and this A was still pressed in time.
        if(code==A && down && repeat==0 && pad.heldSelect!=null && time-pad.pressAt<window){
            pad.heldSelect=null;pad.swallowSelect=true;pad.swallowA=true;
            advance(now);
            out.capture(time);
            return true;
        }
        advance(now); // whatever fell due before this event happens first
        if(code==SELECT){
            if(down && repeat>0)return pad.heldSelect!=null || pad.selectLate || pad.swallowSelect;
            if(down){
                if(pad.swallowSelect)return true; // a second press without a release: still the combo's
                if(pad.heldSelect!=null)return true;
                if(pad.selectLate)return false;   // (a press while the game already has Select down: pass)
                if(pad.pendingUp!=null){E up=pad.pendingUp;pad.pendingUp=null;out.forward(up);}
                pad.heldSelect=event;pad.pressAt=time;pad.heldDue=now+window;
                return true;
            }
            if(pad.swallowSelect){pad.swallowSelect=false;return true;}
            // A release without a time of its own (not after its press's): it happened when it was handled.
            long released=time>pad.pressAt?time:now;
            if(pad.heldSelect!=null){ // a tap that ended inside the window: no combo is possible any more
                E press=pad.heldSelect;pad.heldSelect=null;
                out.forward(press);
                delayRelease(pad,event,released+(now-pad.pressAt),now);
                return true;
            }
            if(pad.selectLate){pad.selectLate=false;delayRelease(pad,event,released+pad.shift,now);return true;}
            return false;
        }
        if(down && repeat>0)return pad.swallowA;
        if(down){
            // (An A outside the window while Select is still held back: advance() above has given that Select first
            // when its time was up; otherwise the two simply reach the game in the order they were pressed.)
            if(pad.heldSelect!=null){E press=pad.heldSelect;pad.heldSelect=null;pad.selectLate=true;pad.shift=now-pad.pressAt;out.forward(press);}
            if(pad.swallowSelect){pad.swallowA=true;out.capture(time);return true;}
            return false;
        }
        if(pad.swallowA){pad.swallowA=false;return true;}
        return false;
    }

    private void delayRelease(Pad<E> pad,E up,long due,long now){
        if(due<=now){out.forward(up);return;}
        pad.pendingUp=up;pad.pendingDue=due;
    }

    /** Gives whatever is due at `now` (the time it is called). */
    void advance(long now){
        for(Pad<E> pad:pads.values()){
            if(pad.heldSelect!=null && now>=pad.heldDue){
                E press=pad.heldSelect;pad.heldSelect=null;
                pad.selectLate=true;pad.shift=now-pad.pressAt; // the real delay (a late timer makes it longer, the duration stays right)
                out.forward(press);
            }
            if(pad.pendingUp!=null && now>=pad.pendingDue){E up=pad.pendingUp;pad.pendingUp=null;out.forward(up);}
        }
    }

    /** When advance must next be called; Long.MIN_VALUE when nothing waits. */
    long nextDeadline(){
        long next=Long.MIN_VALUE;
        for(Pad<E> pad:pads.values()){
            if(pad.heldSelect!=null && (next==Long.MIN_VALUE || pad.heldDue<next))next=pad.heldDue;
            if(pad.pendingUp!=null && (next==Long.MIN_VALUE || pad.pendingDue<next))next=pad.pendingDue;
        }
        return next;
    }

    /**
     * The window lost focus (PrivateGamepadInput has released every button in the game): for every controller, a
     * held-back press is dropped (the game never gets it); a waiting release is given at once (harmless after the
     * release of everything, and it can never leave Select stuck down if that release did not happen); the combo
     * forgets which keys it was keeping, so their later releases pass to the game (releasing a released key does
     * nothing there).
     */
    void reset(){
        for(Pad<E> pad:pads.values())reset(pad);
        pads.clear();
    }

    /** This controller went away: the same for it alone. Another controller's presses are not touched. */
    void reset(int device){
        Pad<E> pad=pads.remove(device);
        if(pad!=null)reset(pad);
    }

    private void reset(Pad<E> pad){
        pad.heldSelect=null;pad.selectLate=false;pad.swallowSelect=false;pad.swallowA=false;
        if(pad.pendingUp!=null){E up=pad.pendingUp;pad.pendingUp=null;out.forward(up);}
    }

    /** The controller of the last Select or A event seen (-1: injected test input; MIN_VALUE: none yet). */
    int device(){return lastDevice;}

    /** For CaptureController's log: is a Select press being held back right now (on any controller). */
    boolean holding(){
        for(Pad<E> pad:pads.values())if(pad.heldSelect!=null)return true;
        return false;
    }
}
