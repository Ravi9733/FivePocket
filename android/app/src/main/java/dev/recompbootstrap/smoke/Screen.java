package dev.recompbootstrap.smoke;

import android.content.Intent;
import android.view.View;

/** One full-screen page hosted by BootstrapActivity. All methods are called on the UI thread. */
interface Screen {
    /** Called once, before the first onShown. The returned view fills the host. */
    View createView(ScreenHost host);
    /** Called each time this screen becomes the visible one (after createView, and after a screen above it was popped). */
    void onShown();
    /** Called when another screen covers this one or it is popped. */
    void onHidden();
    /** A NavInput command. Return true if handled. For NavInput.BACK, false means "pop me". */
    boolean onNav(int command, boolean repeat);
    /** Result of ScreenHost.startForResult for this screen. */
    void onActivityResult(int requestCode, int resultCode, Intent data);

    /**
     * The whole app left the front while this screen is the visible one: Home, another app (the system's file
     * chooser among them), the screen switched off. Called from the activity's onStop.
     */
    default void onAppLeft() {
    }

    /** The app came back to the front while this screen is the visible one (every resume of the activity but the first). */
    default void onAppBack() {
    }

    /**
     * The app's window got the input focus back while this screen is the visible one: after the notification shade, a
     * system dialog or the recent-apps view that did not stop the app. Not called for the first focus of a window.
     */
    default void onWindowBack() {
    }
}
