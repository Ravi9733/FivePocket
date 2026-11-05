package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.util.Log;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.util.List;

/**
 * The first screen of the app: "Setting up". In a build that carries its game code it puts that code and the other
 * pieces the app carries in place (first start and after every update), shows how far that is, and then opens the
 * menu. It asks nothing and offers no folder. In a build without game code inside (a developer build) it opens the
 * menu at once.
 *
 * Thin: what is done and in which order is ShipStart's, what is decided and said is ShipSetupModel's (both pure
 * Java, host tested). This class draws the model with plain views and runs the work off the UI thread. Gamepad: A
 * presses "Try again" after a failure; B and Android's Back close the screen after a failure and do nothing while
 * the work runs. Touch works on the button.
 *
 * What it writes to the log, tag RecompShip, so that an install can be checked from outside although the build is
 * not debuggable (the text of each line is ShipLog's; DEVICE-STEPS-VM.md reads these lines):
 *   PIECE (place) bytes=(n) sha256=(12 digits) copied|kept     one per bundled piece, when the full work ran
 *   FIRST_RUN ok|failed changed=(n) pieces=(n) build=... record=... code=... shaders=... [error=...]
 *   CODE in place build=... record=... code=... shaders=... pieces=(n)      on every later start
 *   PIECE_CHANGED count=(n) first=(path)                       a later start found a carried piece changed; the full work follows
 *   LEFT_OVERS removed bytes=(n)                               when a killed copy had left half files
 *   GAME_DATA in place|needed: (sentence) pinned=(n) missing=(n) missing_bytes=(n) bundled=(n) from_discs=(n)
 *   MISSING (path) bytes=(n) sha256=(12 digits)                one per pinned file that is absent
 *   DIFFERENT (path) bytes=(n) sha256=(12 digits)              one per pinned file that is there but is not the pinned one
 *
 * Started with the extra "ship_from_launcher" = true (the menu's "Finish Setting Up") it checks every piece again by
 * its SHA-256, whatever was marked, and returns to the menu below it instead of starting a new one.
 */
public final class ShipStartActivity extends Activity {
    static final String EXTRA_FROM_LAUNCHER = "ship_from_launcher";
    private static final String TAG = ShipLog.TAG;
    private static final long TICK_MS = 250;

    private final ShipSetupModel model = new ShipSetupModel();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private TextView heading, text;
    private ProgressBar bar;
    private Button first;
    private boolean fromLauncher, destroyed, working;

    // written by the worker thread, read by the UI tick
    private volatile ShipFirstRun.Step step;
    private volatile long stepDone, stepTotal;

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (destroyed || !model.busy()) return;
            model.prepareProgress(step, stepDone, stepTotal);
            render();
            ui.postDelayed(this, TICK_MS);
        }
    };

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        fromLauncher = getIntent().getBooleanExtra(EXTRA_FROM_LAUNCHER, false);
        model.opened(fromLauncher);
        buildViews();
        render();
        prepare();
    }

    @Override protected void onDestroy() {
        destroyed = true;
        ui.removeCallbacksAndMessages(null);
        super.onDestroy();
    }

    // ------------------------------------------------------------------------------------------ the work

    /** Runs ShipStart off the UI thread: checks the app's own pieces and puts them in place when needed. */
    private void prepare() {
        if (working) return;
        working = true;
        final File filesDir = getFilesDir();
        final String apkPath = getApplicationInfo().sourceDir;
        final boolean gameRunning = gameRunning();
        final boolean force = fromLauncher;
        new Thread(new Runnable() {
            @Override public void run() {
                // Public app updates preserve imported code. Setup activates it only after a checked import.
                if (PublicGameCode.isPublic(apkPath)) { done(null); return; }
                ShipBundle.Apk bundle = null;
                ShipStart.Outcome outcome;
                try {
                    bundle = new ShipBundle.Apk(apkPath);
                    outcome = ShipStart.run(bundle, filesDir, force, gameRunning, new ShipStart.Watch() {
                        @Override public void working() {
                            ui.post(new Runnable() {
                                @Override public void run() {
                                    if (destroyed) return;
                                    model.prepareStarted();
                                    render();
                                    ui.postDelayed(tick, TICK_MS);
                                }
                            });
                        }

                        @Override public void at(ShipFirstRun.Step at, long done, long total) {
                            step = at;
                            stepDone = done;
                            stepTotal = total;
                        }
                    }, ShipFiles.DISK);
                    // One line per piece and per missing file: the log is the only way to see from outside what is installed.
                    for (String line : outcome.log) Log.i(TAG, line);
                    done(outcome.error);
                } catch (IOException | RuntimeException e) {
                    done("This app's own file cannot be read: " + ShipFiles.reason(e) + ". Install the app again.");
                } finally {
                    if (bundle != null) {
                        try {
                            bundle.close();
                        } catch (IOException ignored) {
                            // it was only read
                        }
                    }
                }
            }
        }, "ship-first-run").start();
    }

    /** On the UI thread: the outcome of prepare(). error null: the menu follows. */
    private void done(final String error) {
        ui.post(new Runnable() {
            @Override public void run() {
                working = false;
                ui.removeCallbacks(tick);
                if (destroyed) return;
                if (error == null) {
                    model.finished();
                    leave();
                    return;
                }
                model.prepareFailed(error);
                render();
            }
        });
    }

    // ------------------------------------------------------------------------------------------ actions

    private void perform(int action) {
        switch (action) {
            case ShipSetupModel.ACT_RETRY:
                if (working) return;
                model.prepareStarted();
                render();
                prepare();
                break;
            case ShipSetupModel.ACT_CLOSE:
                // After a failure: back to the menu below when it opened this screen, out of the app otherwise.
                finish();
                break;
            default:
                break;
        }
    }

    /** To the menu: the one below when this screen was opened from it, a new one otherwise. */
    private void leave() {
        if (isFinishing()) return;
        if (!fromLauncher) startActivity(new Intent(this, BootstrapActivity.class));
        finish();
        overridePendingTransition(0, 0);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int code = event.getKeyCode();
        boolean a = code == KeyEvent.KEYCODE_BUTTON_A, b = code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_BACK;
        if (!a && !b) return super.dispatchKeyEvent(event);
        if (event.getAction() == KeyEvent.ACTION_UP && event.getRepeatCount() == 0) {
            perform(a ? model.primaryAction() : model.backAction());
        }
        return true;   // consumed, so that Android does not also turn them into a click or a Back
    }

    private boolean gameRunning() {
        ActivityManager manager = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> processes = manager == null ? null : manager.getRunningAppProcesses();
        if (processes == null) return false;
        for (ActivityManager.RunningAppProcessInfo info : processes) {
            if (info.uid == Process.myUid() && (getPackageName() + ":private_trace").equals(info.processName)) return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------------------------ drawing

    private void buildViews() {
        float density = getResources().getDisplayMetrics().density;
        int pad = (int) (32 * density);
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER_VERTICAL);
        column.setBackgroundColor(Color.BLACK);
        column.setPadding(pad * 2, pad, pad * 2, pad);

        heading = new TextView(this);
        heading.setTextSize(28);
        heading.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        heading.setTextColor(Color.WHITE);
        column.addView(heading, wrap());

        text = new TextView(this);
        text.setTextSize(18);
        text.setPadding(0, pad / 2, 0, pad / 2);
        column.addView(text, wrap());

        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        column.addView(bar, wrap());

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setPadding(0, pad / 2, 0, 0);
        first = new Button(this);
        first.setAllCaps(false);
        first.setTextSize(18);
        first.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View view) {
                perform(model.primaryAction());
            }
        });
        buttons.addView(first, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        column.addView(buttons, wrap());
        setContentView(column);
    }

    private static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private void render() {
        if (heading == null) return;
        heading.setText(model.heading());
        text.setText(model.text());
        text.setTextColor(model.textIsProblem() ? Color.rgb(255, 170, 150) : Color.rgb(214, 220, 228));
        float fraction = model.fraction();
        bar.setVisibility(fraction < 0f ? View.INVISIBLE : View.VISIBLE);
        if (fraction >= 0f) bar.setProgress(Math.round(fraction * 1000f));
        String primary = model.primary();
        first.setVisibility(primary == null ? View.GONE : View.VISIBLE);
        if (primary != null) first.setText("A  " + primary);
        heading.setKeepScreenOn(model.busy());   // the work must not be cut short by the screen timeout
    }
}
