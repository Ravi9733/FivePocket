package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.content.Intent;
import java.io.File;

interface ScreenHost {
    Activity activity();
    /** The app's files directory (Context.getFilesDir()). */
    File filesDir();
    /** Panel-pixel scale: content height / 1080f. */
    float unit();
    void push(Screen screen);
    void pop();
    /**
     * A direction that is still held does nothing more until it is let go (NavInput.endHold): another tab came up
     * without a page change.
     */
    void endHold();
    /** True while the process <package>:private_trace exists. */
    boolean gameRunning();
    void startForResult(Intent intent, int requestCode);
    /** Runs work off the UI thread, then done on the UI thread (done is skipped if the activity was destroyed). */
    void background(Runnable work, Runnable done);
}
